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
    // samples per frame (or more when frame_count addresses a prefix). Before
    // start(), this only primes the currently available endpoint space and
    // returns S_FALSE with a partial frame count rather than waiting when full.
    [[nodiscard]] WasapiWriteResult write_interleaved(
        std::span<const std::int16_t> samples,
        std::uint32_t frame_count) noexcept;

    // Starts rendering frames previously primed through write_interleaved().
    [[nodiscard]] std::int32_t start() noexcept;

    // Pauses the audio clock without resetting or discarding queued frames.
    // start() resumes from the exact next frame in the same WASAPI session.
    [[nodiscard]] std::int32_t pause() noexcept;

    // Waits until all submitted frames have left the WASAPI endpoint buffer.
    [[nodiscard]] std::int32_t drain() noexcept;

    // Returns the number of frames still queued in the endpoint buffer.
    [[nodiscard]] std::int32_t get_current_padding(
        std::uint32_t& frame_count) noexcept;

    // Stops and resets the stream, discarding queued frames. It is idempotent
    // and also resets a stream that is currently paused.
    [[nodiscard]] std::int32_t stop() noexcept;

    // May be called from another thread to interrupt a blocked write or drain.
    // The object must remain alive until that operation has returned.
    void request_cancel() noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint32_t buffer_frame_count() const noexcept;

private:
    struct Implementation;
    Implementation* implementation_{};
};

} // namespace cd404::platform::windows
