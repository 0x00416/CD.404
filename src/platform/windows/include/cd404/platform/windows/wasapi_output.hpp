#pragma once

#include <cstdint>
#include <span>

namespace cd404::platform::windows {

// Result of a possibly partial write. `frames_written` is always safe to retry
// from; `status` is S_OK only when every requested frame was accepted.
struct WasapiWriteResult final {
    std::int32_t status{};
    std::uint32_t frames_written{};
};

// Synchronous, event-driven WASAPI renderer for audio-CD PCM. The object must
// be opened, used, and destroyed on the same thread because it owns COM audio
// interfaces from that apartment.
class WasapiOutput final {
public:
    static constexpr std::uint32_t sample_rate = 44'100;
    static constexpr std::uint16_t channel_count = 2;
    static constexpr std::uint16_t bits_per_sample = 16;

    WasapiOutput() noexcept;
    ~WasapiOutput();

    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;
    WasapiOutput(WasapiOutput&&) = delete;
    WasapiOutput& operator=(WasapiOutput&&) = delete;

    // Opens the default multimedia render endpoint in shared mode. Windows may
    // convert the fixed 44.1 kHz / 16-bit / stereo PCM stream to the endpoint's
    // mix format; input to this class always remains native audio-CD PCM.
    [[nodiscard]] std::int32_t open_default_shared() noexcept;

    // Writes interleaved stereo samples. The span must contain exactly two
    // samples per frame (or more when frame_count addresses a prefix).
    [[nodiscard]] WasapiWriteResult write_interleaved(
        std::span<const std::int16_t> samples,
        std::uint32_t frame_count) noexcept;

    // Waits until all submitted frames have left the WASAPI endpoint buffer.
    [[nodiscard]] std::int32_t drain() noexcept;

    // Stops and resets the stream, discarding queued frames. It is idempotent.
    [[nodiscard]] std::int32_t stop() noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint32_t buffer_frame_count() const noexcept;

private:
    struct Implementation;
    Implementation* implementation_{};
};

} // namespace cd404::platform::windows
