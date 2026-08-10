#pragma once

#include <cd404/listenbrainz/playback_tracker.hpp>
#include <cd404/platform/windows/http_client.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <string>
#include <string_view>

namespace cd404::platform::windows {

// Builds an API-compliant JSON body. Exposed for deterministic tests; the
// authorization token is never included in this document.
[[nodiscard]] std::wstring build_listenbrainz_payload(
    const listenbrainz::Submission& submission);

[[nodiscard]] bool is_listenbrainz_token_format_valid(
    std::wstring_view token) noexcept;

// Stores the token in Windows Credential Manager. Passing an empty token
// removes the saved credential. No token is written to an application file.
[[nodiscard]] bool save_listenbrainz_token(std::wstring_view token) noexcept;

// Returns only the number of characters in the effective credential. The
// token itself remains in Windows Credential Manager or the process environment.
[[nodiscard]] std::size_t listenbrainz_token_character_count() noexcept;

enum class ListenBrainzState {
    disabled,
    token_missing,
    validating,
    ready,
    submitting,
    retry_wait,
    unauthorized,
    error,
};

struct ListenBrainzStatus final {
    ListenBrainzState state{ListenBrainzState::token_missing};
    std::wstring user_name;
    std::size_t pending_listens{};
    std::size_t failed_listens{};
    std::uint64_t submitted_listens{};
    std::uint64_t submitted_playing_now{};
    std::uint64_t retry_after_seconds{};
    unsigned long http_status{};
    unsigned long system_error{};
};

[[nodiscard]] const wchar_t* to_string(ListenBrainzState state) noexcept;

struct ListenBrainzReporterOptions final {
    std::shared_ptr<HttpClient> http_client;
    std::filesystem::path queue_path;
    std::wstring token;
};

class ListenBrainzReporter final {
public:
    ListenBrainzReporter();
    explicit ListenBrainzReporter(ListenBrainzReporterOptions options);
    ~ListenBrainzReporter();

    ListenBrainzReporter(const ListenBrainzReporter&) = delete;
    ListenBrainzReporter& operator=(const ListenBrainzReporter&) = delete;

    [[nodiscard]] bool enabled() const noexcept;
    void set_reporting_enabled(bool enabled) noexcept;
    [[nodiscard]] bool reporting_enabled() const noexcept;
    void reload_token();
    void submit(const listenbrainz::Submission& submission);
    void clear_playing_now();
    void retry_pending();
    [[nodiscard]] bool clear_pending();
    [[nodiscard]] ListenBrainzStatus status() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    std::atomic_bool reporting_enabled_{true};
};

} // namespace cd404::platform::windows
