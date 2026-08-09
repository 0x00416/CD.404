#include <windows.h>

#include <wincred.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cd404/platform/windows/listenbrainz_reporter.hpp>

#include "http_client.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace cd404::platform::windows {
namespace {

using namespace winrt::Windows::Data::Json;

constexpr wchar_t kCredentialTarget[] = L"CD.404/ListenBrainz";
constexpr wchar_t kTokenEnvironment[] = L"CD404_LISTENBRAINZ_TOKEN";

[[nodiscard]] std::wstring token_from_environment()
{
    const DWORD required = GetEnvironmentVariableW(kTokenEnvironment, nullptr, 0);
    if (required <= 1U) {
        return {};
    }
    std::wstring token(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        kTokenEnvironment,
        token.data(),
        required);
    if (written == 0U || written >= required) {
        return {};
    }
    token.resize(written);
    return token;
}

[[nodiscard]] std::wstring load_token()
{
    PCREDENTIALW credential{};
    if (CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) != FALSE) {
        std::wstring token;
        if (credential->CredentialBlob != nullptr &&
            credential->CredentialBlobSize >= sizeof(wchar_t) &&
            credential->CredentialBlobSize % sizeof(wchar_t) == 0) {
            const auto* characters = reinterpret_cast<const wchar_t*>(
                credential->CredentialBlob);
            token.assign(
                characters,
                characters + credential->CredentialBlobSize / sizeof(wchar_t));
            while (!token.empty() && token.back() == L'\0') {
                token.pop_back();
            }
        }
        CredFree(credential);
        if (!token.empty()) {
            return token;
        }
    }
    return token_from_environment();
}

[[nodiscard]] JsonValue string_value(const std::wstring& value)
{
    return JsonValue::CreateStringValue(value);
}

} // namespace

bool save_listenbrainz_token(const std::wstring_view token) noexcept
{
    if (token.empty()) {
        if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) != FALSE) {
            return true;
        }
        return GetLastError() == ERROR_NOT_FOUND;
    }

    if (token.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) /
            sizeof(wchar_t)) {
        return false;
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(
        token.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<wchar_t*>(token.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"ListenBrainz");
    return CredWriteW(&credential, 0) != FALSE;
}

std::wstring build_listenbrainz_payload(
    const listenbrainz::Submission& submission)
{
    JsonObject additional;
    additional.Insert(L"media_player", string_value(L"CD.404"));
    additional.Insert(L"submission_client", string_value(L"CD.404"));
    additional.Insert(L"submission_client_version", string_value(L"0.1.0"));
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
    explicit Implementation(std::wstring loaded_token)
        : token(std::move(loaded_token))
    {
        if (!token.empty()) {
            worker = std::jthread([this](const std::stop_token stop_token) {
                run(stop_token);
            });
        }
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
        for (;;) {
            listenbrainz::Submission submission;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, stop_token, [this] { return !queue.empty(); });
                if (stop_token.stop_requested()) {
                    return;
                }
                submission = std::move(queue.front());
                queue.pop_front();
            }

            const std::wstring payload = build_listenbrainz_payload(submission);
            const std::string utf8 = detail::wide_to_utf8(payload);
            if (utf8.empty()) {
                continue;
            }
            const std::wstring headers =
                L"Authorization: Token " + token +
                L"\r\nContent-Type: application/json\r\n";
            static_cast<void>(detail::https_post(
                L"api.listenbrainz.org",
                L"/1/submit-listens",
                headers,
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(utf8.data()),
                    utf8.size()),
                64U * 1'024U));
        }
    }

    std::wstring token;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<listenbrainz::Submission> queue;
    std::jthread worker;
};

ListenBrainzReporter::ListenBrainzReporter()
    : implementation_(std::make_unique<Implementation>(load_token()))
{
}

ListenBrainzReporter::~ListenBrainzReporter() = default;

bool ListenBrainzReporter::enabled() const noexcept
{
    return reporting_enabled_.load(std::memory_order_acquire) &&
           implementation_ != nullptr && !implementation_->token.empty();
}

void ListenBrainzReporter::set_reporting_enabled(const bool enabled) noexcept
{
    reporting_enabled_.store(enabled, std::memory_order_release);
}

bool ListenBrainzReporter::reporting_enabled() const noexcept
{
    return reporting_enabled_.load(std::memory_order_acquire);
}

void ListenBrainzReporter::reload_token()
{
    implementation_ = std::make_unique<Implementation>(load_token());
}

void ListenBrainzReporter::submit(const listenbrainz::Submission& submission)
{
    if (!enabled() || submission.track_name.empty() || submission.artist_name.empty()) {
        return;
    }
    {
        std::scoped_lock lock(implementation_->mutex);
        constexpr std::size_t kMaximumQueuedSubmissions = 64;
        if (implementation_->queue.size() >= kMaximumQueuedSubmissions) {
            implementation_->queue.pop_front();
        }
        implementation_->queue.push_back(submission);
    }
    implementation_->changed.notify_one();
}

} // namespace cd404::platform::windows
