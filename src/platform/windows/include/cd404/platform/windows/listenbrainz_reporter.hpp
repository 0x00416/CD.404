#pragma once

#include <cd404/listenbrainz/playback_tracker.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace cd404::platform::windows {

// Builds an API-compliant JSON body. Exposed for deterministic tests; the
// authorization token is never included in this document.
[[nodiscard]] std::wstring build_listenbrainz_payload(
    const listenbrainz::Submission& submission);

// Stores the token in Windows Credential Manager. Passing an empty token
// removes the saved credential. No token is written to an application file.
[[nodiscard]] bool save_listenbrainz_token(std::wstring_view token) noexcept;

class ListenBrainzReporter final {
public:
    ListenBrainzReporter();
    ~ListenBrainzReporter();

    ListenBrainzReporter(const ListenBrainzReporter&) = delete;
    ListenBrainzReporter& operator=(const ListenBrainzReporter&) = delete;

    [[nodiscard]] bool enabled() const noexcept;
    void reload_token();
    void submit(const listenbrainz::Submission& submission);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace cd404::platform::windows
