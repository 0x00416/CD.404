#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cd404::audio {

inline constexpr std::size_t kPcm16StereoChannelCount = 2;

enum class Pcm16BufferStatus {
    ok,
    partial,
    full,
    empty,
    closed,
    invalid_frame_alignment,
};

struct Pcm16BufferResult final {
    Pcm16BufferStatus status{Pcm16BufferStatus::ok};
    std::size_t frames_transferred{};
};

// A bounded single-producer/single-consumer queue of interleaved stereo PCM16
// frames. Storage is allocated once during construction; push() and pop() do
// not allocate or block.
//
// Thread ownership:
// - The producer thread exclusively calls push() and close().
// - The consumer thread exclusively calls pop().
// - Observers may call the const query functions from either thread.
// - reset() requires both producer and consumer to be stopped.
//
// Samples are published by a release store to write_position_. The consumer's
// acquire load of that position makes those sample writes visible. Reclaimed
// slots are published by a release store to read_position_, paired with the
// producer's acquire load before it writes those slots again.
class Pcm16SpscRingBuffer final {
public:
    explicit Pcm16SpscRingBuffer(std::size_t capacity_frames);

    Pcm16SpscRingBuffer(const Pcm16SpscRingBuffer&) = delete;
    Pcm16SpscRingBuffer& operator=(const Pcm16SpscRingBuffer&) = delete;
    Pcm16SpscRingBuffer(Pcm16SpscRingBuffer&&) = delete;
    Pcm16SpscRingBuffer& operator=(Pcm16SpscRingBuffer&&) = delete;

    [[nodiscard]] std::size_t capacity_frames() const noexcept;
    [[nodiscard]] std::size_t readable_frames() const noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] bool drained() const noexcept;

    // The source contains interleaved L, R samples and must contain an even
    // number of samples. A partial result means the queue accepted all space
    // currently available without waiting.
    [[nodiscard]] Pcm16BufferResult push(
        std::span<const std::int16_t> interleaved_samples) noexcept;

    // The destination must have room for whole interleaved stereo frames. A
    // closed result is returned only after all previously published frames
    // have been drained.
    [[nodiscard]] Pcm16BufferResult pop(
        std::span<std::int16_t> interleaved_destination) noexcept;

    // Called by the producer after its final push(). Existing frames remain
    // available to the consumer.
    void close() noexcept;

    // Clears buffered frames and reopens the queue. No other operation may run
    // concurrently with reset(). The sample storage is retained.
    void reset() noexcept;

private:
    [[nodiscard]] static std::size_t checked_sample_capacity(
        std::size_t capacity_frames);

    std::size_t capacity_frames_{};
    std::vector<std::int16_t> samples_;
    std::atomic<std::uint64_t> write_position_{};
    std::atomic<std::uint64_t> read_position_{};
    std::atomic<bool> closed_{};
};

} // namespace cd404::audio
