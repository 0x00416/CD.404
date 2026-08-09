#include <windows.h>

#include <objbase.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cd404/platform/windows/listenbrainz_reporter.hpp>
#include <cd404/platform/windows/listenbrainz_queue.hpp>

#include "http_client.hpp"
#include "listenbrainz_credentials.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace cd404::platform::windows {
namespace {

using namespace winrt::Windows::Data::Json;

[[nodiscard]] JsonValue string_value(const std::wstring& value)
{
    return JsonValue::CreateStringValue(value);
}

[[nodiscard]] std::int64_t unix_time_now() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] std::string new_session_id()
{
    GUID identifier{};
    wchar_t text[40]{};
    if (SUCCEEDED(CoCreateGuid(&identifier)) &&
        StringFromGUID2(identifier, text, static_cast<int>(std::size(text))) > 0) {
        return detail::wide_to_utf8(text);
    }
    static std::atomic_uint64_t fallback_counter{};
    return std::format(
        "{}-{}",
        unix_time_now(),
        fallback_counter.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] std::string token_owner_key(const std::wstring_view token)
{
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t hash = offset;
    for (const wchar_t character : token) {
        const auto value = static_cast<std::uint32_t>(character);
        for (unsigned int byte = 0; byte < sizeof(value); ++byte) {
            hash ^= (value >> (byte * 8U)) & 0xffU;
            hash *= prime;
        }
    }
    return std::format("{:016x}", hash);
}

[[nodiscard]] std::wstring authorization_headers(const std::wstring& token)
{
    return L"Authorization: Token " + token +
        L"\r\nContent-Type: application/json\r\n";
}

[[nodiscard]] std::span<const std::uint8_t> as_bytes(const std::string& value)
{
    return {
        reinterpret_cast<const std::uint8_t*>(value.data()),
        value.size(),
    };
}

} // namespace

std::wstring build_listenbrainz_payload(
    const listenbrainz::Submission& submission)
{
    JsonObject additional;
    additional.Insert(L"media_player", string_value(L"CD.404"));
    additional.Insert(L"submission_client", string_value(L"CD.404"));
    additional.Insert(L"submission_client_version", string_value(L"0.1.0"));
    if (submission.track_number != 0) {
        additional.Insert(
            L"tracknumber",
            string_value(std::to_wstring(submission.track_number)));
    }
    if (!submission.recording_mbid.empty()) {
        additional.Insert(L"recording_mbid", string_value(submission.recording_mbid));
    }
    if (!submission.release_mbid.empty()) {
        additional.Insert(L"release_mbid", string_value(submission.release_mbid));
    }
    if (!submission.release_group_mbid.empty()) {
        additional.Insert(
            L"release_group_mbid",
            string_value(submission.release_group_mbid));
    }
    if (!submission.track_mbid.empty()) {
        additional.Insert(L"track_mbid", string_value(submission.track_mbid));
    }
    if (!submission.artist_mbids.empty()) {
        JsonArray artist_mbids;
        for (const auto& artist_mbid : submission.artist_mbids) {
            if (!artist_mbid.empty()) {
                artist_mbids.Append(string_value(artist_mbid));
            }
        }
        if (artist_mbids.Size() != 0) {
            additional.Insert(L"artist_mbids", artist_mbids);
        }
    }
    additional.Insert(
        L"duration_ms",
        JsonValue::CreateNumberValue(
            static_cast<double>(submission.duration_milliseconds)));
    if (submission.type == listenbrainz::SubmissionType::single) {
        additional.Insert(
            L"duration_played",
            JsonValue::CreateNumberValue(
                static_cast<double>(submission.duration_played_seconds)));
    }

    JsonObject metadata;
    metadata.Insert(L"track_name", string_value(submission.track_name));
    metadata.Insert(L"artist_name", string_value(submission.artist_name));
    if (!submission.release_name.empty()) {
        metadata.Insert(L"release_name", string_value(submission.release_name));
    }
    metadata.Insert(L"additional_info", additional);

    JsonObject listen;
    if (submission.type == listenbrainz::SubmissionType::single) {
        listen.Insert(
            L"listened_at",
            JsonValue::CreateNumberValue(static_cast<double>(submission.listened_at)));
    }
    listen.Insert(L"track_metadata", metadata);

    JsonArray payload;
    payload.Append(listen);
    JsonObject root;
    root.Insert(
        L"listen_type",
        string_value(
            submission.type == listenbrainz::SubmissionType::playing_now
                ? L"playing_now"
                : L"single"));
    root.Insert(L"payload", payload);
    return root.Stringify().c_str();
}

struct ListenBrainzReporter::Implementation final {
    enum class ActionKind { none, validate, clear, playing_now, single };

    struct Action final {
        ActionKind kind{ActionKind::none};
        std::wstring token;
        std::string payload;
        std::optional<ListenBrainzPendingListen> pending;
        unsigned int attempts{};
        std::uint64_t generation{};
    };

    Implementation(
        std::wstring loaded_token,
        std::shared_ptr<HttpClient> transport,
        std::filesystem::path queue_path)
        : token(std::move(loaded_token)),
          owner_key(token_owner_key(token)),
          queue(std::move(queue_path)),
          http_client(transport ? std::move(transport) : make_win_http_client()),
          validation_needed(!token.empty())
    {
        refresh_counts_locked();
        if (!queue.available()) {
            current_status.state = ListenBrainzState::error;
            current_status.system_error = ERROR_OPEN_FAILED;
        } else if (token.empty()) {
            current_status.state = ListenBrainzState::token_missing;
        } else {
            current_status.state = ListenBrainzState::validating;
        }
        worker = std::jthread([this](const std::stop_token stop_token) {
            run(stop_token);
        });
    }

    ~Implementation()
    {
        if (worker.joinable()) {
            worker.request_stop();
            changed.notify_all();
            worker.join();
        }
    }

    void run(const std::stop_token stop_token)
    {
        while (!stop_token.stop_requested()) {
            Action action;
            {
                std::unique_lock lock(mutex);
                const std::int64_t now = unix_time_now();
                refresh_counts_locked();
                if (token.empty()) {
                    current_status.state = reporting_enabled
                        ? ListenBrainzState::token_missing
                        : ListenBrainzState::disabled;
                } else if (validation_needed && now >= validation_next_attempt) {
                    current_status.state = ListenBrainzState::validating;
                    action.kind = ActionKind::validate;
                    action.token = token;
                } else if (!token_valid) {
                    if (validation_needed && validation_next_attempt > now) {
                        current_status.state = ListenBrainzState::retry_wait;
                        current_status.retry_after_seconds =
                            static_cast<std::uint64_t>(validation_next_attempt - now);
                    } else if (!validation_needed &&
                        current_status.state != ListenBrainzState::unauthorized) {
                        current_status.state = ListenBrainzState::error;
                    }
                } else if (clear_requested && clear_next_attempt <= now) {
                    clear_requested = false;
                    current_status.state = ListenBrainzState::submitting;
                    action.kind = ActionKind::clear;
                    action.token = token;
                    action.payload = R"({"client":"CD.404"})";
                    action.attempts = clear_attempts;
                    action.generation = ephemeral_generation;
                } else if (!reporting_enabled) {
                    current_status.state = ListenBrainzState::disabled;
                } else if (playing_now && playing_now->next_attempt <= now) {
                    current_status.state = ListenBrainzState::submitting;
                    action.kind = ActionKind::playing_now;
                    action.token = token;
                    action.payload = playing_now->payload;
                    action.attempts = playing_now->attempts;
                    action.generation = ephemeral_generation;
                } else if (const auto pending = queue.next(owner_key);
                           pending && pending->next_attempt <= now) {
                    current_status.state = ListenBrainzState::submitting;
                    action.kind = ActionKind::single;
                    action.token = token;
                    action.payload = pending->payload;
                    action.pending = pending;
                    action.attempts = pending->attempts;
                } else {
                    std::int64_t next_attempt{};
                    if (playing_now && playing_now->next_attempt > now) {
                        next_attempt = playing_now->next_attempt;
                    }
                    if (const auto next_pending = queue.next(owner_key);
                        next_pending &&
                        next_pending->next_attempt > now &&
                        (next_attempt == 0 ||
                         next_pending->next_attempt < next_attempt)) {
                        next_attempt = next_pending->next_attempt;
                    }
                    if (validation_needed && validation_next_attempt > now &&
                        (next_attempt == 0 || validation_next_attempt < next_attempt)) {
                        next_attempt = validation_next_attempt;
                    }
                    if (clear_requested && clear_next_attempt > now &&
                        (next_attempt == 0 || clear_next_attempt < next_attempt)) {
                        next_attempt = clear_next_attempt;
                    }
                    if (next_attempt > now) {
                        current_status.state = ListenBrainzState::retry_wait;
                        current_status.retry_after_seconds = static_cast<std::uint64_t>(
                            next_attempt - now);
                    } else {
                        current_status.state = ListenBrainzState::ready;
                        current_status.retry_after_seconds = 0;
                    }
                }
                if (action.kind == ActionKind::none) {
                    changed.wait_for(lock, stop_token, std::chrono::seconds(1), [] {
                        return false;
                    });
                    continue;
                }
            }

            if (action.kind == ActionKind::validate) {
                process_validation(action.token);
                continue;
            }

            const std::wstring_view path = action.kind == ActionKind::clear
                ? L"/1/playing-now/delete"
                : (action.kind == ActionKind::playing_now
                    ? L"/1/submit-listens?return_msid=true"
                    : L"/1/submit-listens");
            const HttpResponse response = http_client->post(
                L"api.listenbrainz.org",
                path,
                authorization_headers(action.token),
                as_bytes(action.payload),
                64U * 1'024U);
            process_submission_result(action, response);
        }
    }

    void process_validation(const std::wstring& attempted_token)
    {
        const HttpResponse response = http_client->get(
            L"api.listenbrainz.org",
            L"/1/validate-token",
            authorization_headers(attempted_token),
            64U * 1'024U);

        bool valid{};
        bool response_parsed{};
        std::wstring user_name;
        if (response.system_error == ERROR_SUCCESS && response.status == 200 &&
            !response.body.empty()) {
            try {
                const std::string body(
                    reinterpret_cast<const char*>(response.body.data()),
                    response.body.size());
                const JsonObject root = JsonObject::Parse(detail::utf8_to_wide(body));
                valid = root.GetNamedBoolean(L"valid", false);
                user_name = root.GetNamedString(L"user_name", L"").c_str();
                response_parsed = true;
            } catch (const winrt::hresult_error&) {
            }
        }

        std::scoped_lock lock(mutex);
        if (attempted_token != token) {
            return;
        }
        current_status.http_status = response.status;
        current_status.system_error = response.system_error;
        if (valid) {
            token_valid = true;
            validation_needed = false;
            validation_attempts = 0;
            validation_next_attempt = 0;
            current_status.user_name = std::move(user_name);
            current_status.state = reporting_enabled
                ? ListenBrainzState::ready
                : ListenBrainzState::disabled;
            current_status.retry_after_seconds = 0;
            changed.notify_all();
            return;
        }
        token_valid = false;
        current_status.user_name.clear();
        if ((response.status == 200 && response_parsed) || response.status == 401) {
            validation_needed = false;
            current_status.state = ListenBrainzState::unauthorized;
            current_status.retry_after_seconds = 0;
            return;
        }
        ++validation_attempts;
        const std::uint64_t delay = retry_delay(validation_attempts, response, 0);
        validation_needed = true;
        validation_next_attempt = unix_time_now() + static_cast<std::int64_t>(delay);
        current_status.state = ListenBrainzState::retry_wait;
        current_status.retry_after_seconds = delay;
    }

    void process_submission_result(
        const Action& action,
        const detail::HttpResponse& response)
    {
        const bool clear_not_owned = action.kind == ActionKind::clear &&
            response.status == 404;
        const bool succeeded = response.system_error == ERROR_SUCCESS &&
            (response.status == 200 || clear_not_owned);

        std::scoped_lock lock(mutex);
        current_status.http_status = response.status;
        current_status.system_error = response.system_error;
        if (succeeded) {
            if (action.kind == ActionKind::clear) {
                clear_attempts = 0;
                clear_next_attempt = 0;
            }
            if (action.kind == ActionKind::single && action.pending) {
                static_cast<void>(queue.complete(action.pending->id));
                ++current_status.submitted_listens;
            } else if (action.kind == ActionKind::playing_now) {
                if (action.generation == ephemeral_generation && playing_now &&
                    playing_now->payload == action.payload) {
                    playing_now.reset();
                }
                ++current_status.submitted_playing_now;
            }
            refresh_counts_locked();
            current_status.state = reporting_enabled
                ? ListenBrainzState::ready
                : ListenBrainzState::disabled;
            current_status.retry_after_seconds = 0;
            changed.notify_all();
            return;
        }

        if (response.status == 401) {
            token_valid = false;
            validation_needed = false;
            current_status.user_name.clear();
            current_status.state = ListenBrainzState::unauthorized;
            current_status.retry_after_seconds = 0;
            return;
        }

        const bool transient = response.system_error != ERROR_SUCCESS ||
            response.status == 0 || response.status == 408 ||
            response.status == 429 || response.status >= 500;
        if (!transient) {
            if (action.kind == ActionKind::single && action.pending) {
                static_cast<void>(queue.mark_failed(
                    action.pending->id,
                    response.status,
                    response.system_error));
            } else if (action.kind == ActionKind::playing_now &&
                       action.generation == ephemeral_generation && playing_now &&
                       playing_now->payload == action.payload) {
                playing_now.reset();
            }
            refresh_counts_locked();
            current_status.state = ListenBrainzState::error;
            current_status.retry_after_seconds = 0;
            return;
        }

        const unsigned int attempts = action.attempts + 1;
        const std::uint64_t salt = action.pending
            ? static_cast<std::uint64_t>(action.pending->id)
            : 0;
        const std::uint64_t delay = retry_delay(attempts, response, salt);
        const std::int64_t next_attempt = unix_time_now() +
            static_cast<std::int64_t>(delay);
        if (action.kind == ActionKind::single && action.pending) {
            static_cast<void>(queue.schedule_retry(
                action.pending->id,
                attempts,
                next_attempt,
                response.status,
                response.system_error));
        } else if (action.kind == ActionKind::playing_now &&
                   action.generation == ephemeral_generation && playing_now &&
                   playing_now->payload == action.payload) {
            playing_now->attempts = attempts;
            playing_now->next_attempt = next_attempt;
        } else if (action.kind == ActionKind::clear) {
            if (action.generation == ephemeral_generation) {
                clear_requested = true;
                clear_attempts = attempts;
                clear_next_attempt = next_attempt;
            }
        }
        refresh_counts_locked();
        current_status.state = ListenBrainzState::retry_wait;
        current_status.retry_after_seconds = delay;
    }

    [[nodiscard]] static std::uint64_t retry_delay(
        const unsigned int attempts,
        const detail::HttpResponse& response,
        const std::uint64_t salt) noexcept
    {
        if (response.status == 429 && response.has_rate_limit_reset) {
            return std::clamp<std::uint64_t>(
                response.rate_limit_reset_seconds,
                1,
                3'600);
        }
        const unsigned int shift = std::min(
            attempts == 0 ? 0U : attempts - 1U,
            9U);
        const std::uint64_t base = std::min<std::uint64_t>(
            5ULL << shift,
            3'600ULL);
        return std::min<std::uint64_t>(base + salt % 5U, 3'600U);
    }

    void reload(std::wstring loaded_token)
    {
        std::scoped_lock lock(mutex);
        ++ephemeral_generation;
        if (loaded_token.empty()) {
            playing_now.reset();
        } else if (!active_playing_now_payload.empty()) {
            playing_now = EphemeralSubmission{
                active_playing_now_payload,
                0,
                0,
            };
        }
        clear_requested = false;
        clear_attempts = 0;
        clear_next_attempt = 0;
        token = std::move(loaded_token);
        owner_key = token_owner_key(token);
        token_valid = false;
        validation_needed = !token.empty();
        validation_attempts = 0;
        validation_next_attempt = 0;
        current_status.user_name.clear();
        current_status.http_status = 0;
        current_status.system_error = 0;
        current_status.retry_after_seconds = 0;
        current_status.state = token.empty()
            ? ListenBrainzState::token_missing
            : ListenBrainzState::validating;
        changed.notify_all();
    }

    void set_enabled(const bool enabled)
    {
        std::scoped_lock lock(mutex);
        reporting_enabled = enabled;
        if (!enabled) {
            ++ephemeral_generation;
            playing_now.reset();
            active_playing_now_payload.clear();
            clear_requested = token_valid;
            clear_attempts = 0;
            clear_next_attempt = 0;
            current_status.state = ListenBrainzState::disabled;
        } else if (token.empty()) {
            current_status.state = ListenBrainzState::token_missing;
        } else if (!token_valid) {
            validation_needed = true;
            validation_next_attempt = 0;
            current_status.state = ListenBrainzState::validating;
        } else {
            current_status.state = ListenBrainzState::ready;
        }
        changed.notify_all();
    }

    [[nodiscard]] bool has_token() const
    {
        std::scoped_lock lock(mutex);
        return !token.empty();
    }

    void enqueue(const listenbrainz::Submission& submission)
    {
        const std::wstring payload_wide = build_listenbrainz_payload(submission);
        const std::string payload = detail::wide_to_utf8(payload_wide);
        if (payload.empty()) {
            return;
        }
        std::scoped_lock lock(mutex);
        if (submission.type == listenbrainz::SubmissionType::playing_now) {
            ++ephemeral_generation;
            clear_requested = false;
            clear_attempts = 0;
            clear_next_attempt = 0;
            active_playing_now_payload = payload;
            playing_now = EphemeralSubmission{payload, 0, 0};
        } else if (!queue.enqueue(owner_key, new_session_id(), payload)) {
            current_status.state = ListenBrainzState::error;
            current_status.system_error = ERROR_WRITE_FAULT;
        }
        refresh_counts_locked();
        changed.notify_all();
    }

    void clear_now()
    {
        std::scoped_lock lock(mutex);
        ++ephemeral_generation;
        playing_now.reset();
        active_playing_now_payload.clear();
        if (!token.empty()) {
            clear_requested = true;
            clear_attempts = 0;
            clear_next_attempt = 0;
        }
        changed.notify_all();
    }

    void retry()
    {
        std::scoped_lock lock(mutex);
        static_cast<void>(queue.retry_all(owner_key));
        validation_needed = !token.empty() && !token_valid;
        validation_next_attempt = 0;
        refresh_counts_locked();
        changed.notify_all();
    }

    [[nodiscard]] bool clear_pending()
    {
        std::scoped_lock lock(mutex);
        const bool cleared = queue.clear_owner(owner_key);
        refresh_counts_locked();
        changed.notify_all();
        return cleared;
    }

    [[nodiscard]] ListenBrainzStatus status() const
    {
        std::scoped_lock lock(mutex);
        return current_status;
    }

    void refresh_counts_locked()
    {
        current_status.pending_listens = queue.pending_count(owner_key);
        current_status.failed_listens = queue.failed_count(owner_key);
    }

    struct EphemeralSubmission final {
        std::string payload;
        unsigned int attempts{};
        std::int64_t next_attempt{};
    };

    std::wstring token;
    std::string owner_key;
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    ListenBrainzQueue queue;
    std::shared_ptr<HttpClient> http_client;
    ListenBrainzStatus current_status;
    std::optional<EphemeralSubmission> playing_now;
    std::string active_playing_now_payload;
    bool reporting_enabled{true};
    bool token_valid{};
    bool validation_needed{};
    unsigned int validation_attempts{};
    std::int64_t validation_next_attempt{};
    bool clear_requested{};
    unsigned int clear_attempts{};
    std::int64_t clear_next_attempt{};
    std::uint64_t ephemeral_generation{};
    std::jthread worker;
};

ListenBrainzReporter::ListenBrainzReporter()
    : implementation_(std::make_unique<Implementation>(
          detail::load_listenbrainz_token(),
          make_win_http_client(),
          default_listenbrainz_queue_path()))
{
}

ListenBrainzReporter::ListenBrainzReporter(ListenBrainzReporterOptions options)
    : implementation_(std::make_unique<Implementation>(
          std::move(options.token),
          std::move(options.http_client),
          options.queue_path.empty()
              ? default_listenbrainz_queue_path()
              : std::move(options.queue_path)))
{
}

ListenBrainzReporter::~ListenBrainzReporter() = default;

bool ListenBrainzReporter::enabled() const noexcept
{
    return reporting_enabled_.load(std::memory_order_acquire) &&
           implementation_ != nullptr && implementation_->has_token();
}

void ListenBrainzReporter::set_reporting_enabled(const bool enabled) noexcept
{
    reporting_enabled_.store(enabled, std::memory_order_release);
    if (implementation_ != nullptr) {
        implementation_->set_enabled(enabled);
    }
}

bool ListenBrainzReporter::reporting_enabled() const noexcept
{
    return reporting_enabled_.load(std::memory_order_acquire);
}

void ListenBrainzReporter::reload_token()
{
    if (implementation_ != nullptr) {
        implementation_->reload(detail::load_listenbrainz_token());
    }
}

void ListenBrainzReporter::submit(const listenbrainz::Submission& submission)
{
    if (!enabled() || submission.track_name.empty() || submission.artist_name.empty()) {
        return;
    }
    implementation_->enqueue(submission);
}

void ListenBrainzReporter::clear_playing_now()
{
    if (implementation_ != nullptr) {
        implementation_->clear_now();
    }
}

void ListenBrainzReporter::retry_pending()
{
    if (implementation_ != nullptr) {
        implementation_->retry();
    }
}

bool ListenBrainzReporter::clear_pending()
{
    return implementation_ != nullptr && implementation_->clear_pending();
}

ListenBrainzStatus ListenBrainzReporter::status() const
{
    if (implementation_ == nullptr) {
        return {};
    }
    ListenBrainzStatus result = implementation_->status();
    if (!reporting_enabled()) {
        result.state = ListenBrainzState::disabled;
    }
    return result;
}

const wchar_t* to_string(const ListenBrainzState state) noexcept
{
    switch (state) {
    case ListenBrainzState::disabled: return L"已关闭";
    case ListenBrainzState::token_missing: return L"未配置";
    case ListenBrainzState::validating: return L"正在验证";
    case ListenBrainzState::ready: return L"已连接";
    case ListenBrainzState::submitting: return L"正在同步";
    case ListenBrainzState::retry_wait: return L"等待重试";
    case ListenBrainzState::unauthorized: return L"凭据无效";
    case ListenBrainzState::error: return L"同步异常";
    }
    return L"未知";
}

} // namespace cd404::platform::windows
