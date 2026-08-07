#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace cd404::platform::windows {

enum class SystemMediaCommand {
    play,
    pause,
    stop,
    previous,
    next,
    seek,
};

enum class SystemMediaPlaybackState {
    closed,
    stopped,
    playing,
    paused,
};

struct SystemMediaRequest final {
    SystemMediaCommand command{SystemMediaCommand::play};
    std::uint64_t position_milliseconds{};
};

class SystemMediaControls final {
public:
    using RequestCallback = std::function<void(SystemMediaRequest)>;

    SystemMediaControls();
    ~SystemMediaControls();

    SystemMediaControls(const SystemMediaControls&) = delete;
    SystemMediaControls& operator=(const SystemMediaControls&) = delete;

    [[nodiscard]] bool initialize(
        void* window_handle,
        RequestCallback callback);
    [[nodiscard]] bool available() const noexcept;
    void update_metadata(
        std::wstring_view title,
        std::wstring_view artist,
        std::wstring_view album);
    void update_timeline(
        std::uint64_t position_milliseconds,
        std::uint64_t duration_milliseconds);
    void set_playback_state(SystemMediaPlaybackState state);
    void clear();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace cd404::platform::windows
