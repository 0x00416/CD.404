#include <cd404/audio/pcm16_spsc_ring_buffer.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cd404::audio {
namespace {

constexpr std::size_t samples_for_frames(const std::size_t frame_count) noexcept
{
    return frame_count * kPcm16StereoChannelCount;
}

} // namespace

Pcm16SpscRingBuffer::Pcm16SpscRingBuffer(const std::size_t capacity_frames)
    : capacity_frames_(capacity_frames),
      samples_(checked_sample_capacity(capacity_frames))
{
}

std::size_t Pcm16SpscRingBuffer::capacity_frames() const noexcept
{
    return capacity_frames_;
}

std::size_t Pcm16SpscRingBuffer::readable_frames() const noexcept
{
    const std::uint64_t read = read_position_.load(std::memory_order_acquire);
    const std::uint64_t write = write_position_.load(std::memory_order_acquire);
    if (write < read) {
        return 0;
    }

    const std::uint64_t available = write - read;
    return static_cast<std::size_t>(std::min(
        available,
        static_cast<std::uint64_t>(capacity_frames_)));
}

bool Pcm16SpscRingBuffer::closed() const noexcept
{
    return closed_.load(std::memory_order_acquire);
}

bool Pcm16SpscRingBuffer::drained() const noexcept
{
    return closed() && readable_frames() == 0;
}

Pcm16BufferResult Pcm16SpscRingBuffer::push(
    const std::span<const std::int16_t> interleaved_samples) noexcept
{
    if (interleaved_samples.size() % kPcm16StereoChannelCount != 0) {
        return {Pcm16BufferStatus::invalid_frame_alignment, 0};
    }
    if (closed_.load(std::memory_order_acquire)) {
        return {Pcm16BufferStatus::closed, 0};
    }

    const std::size_t requested_frames =
        interleaved_samples.size() / kPcm16StereoChannelCount;
    if (requested_frames == 0) {
        return {Pcm16BufferStatus::ok, 0};
    }

    const std::uint64_t write = write_position_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_position_.load(std::memory_order_acquire);
    const auto occupied = static_cast<std::size_t>(write - read);
    const std::size_t writable_frames = capacity_frames_ - occupied;
    const std::size_t frames_to_write =
        std::min(requested_frames, writable_frames);
    if (frames_to_write == 0) {
        return {Pcm16BufferStatus::full, 0};
    }

    const std::size_t write_index =
        static_cast<std::size_t>(write % capacity_frames_);
    const std::size_t first_frames =
        std::min(frames_to_write, capacity_frames_ - write_index);
    const std::size_t first_samples = samples_for_frames(first_frames);
    std::memcpy(
        samples_.data() + samples_for_frames(write_index),
        interleaved_samples.data(),
        first_samples * sizeof(std::int16_t));

    const std::size_t second_frames = frames_to_write - first_frames;
    if (second_frames != 0) {
        std::memcpy(
            samples_.data(),
            interleaved_samples.data() + first_samples,
            samples_for_frames(second_frames) * sizeof(std::int16_t));
    }

    write_position_.store(write + frames_to_write, std::memory_order_release);
    return {
        frames_to_write == requested_frames ? Pcm16BufferStatus::ok
                                            : Pcm16BufferStatus::partial,
        frames_to_write,
    };
}

Pcm16BufferResult Pcm16SpscRingBuffer::pop(
    const std::span<std::int16_t> interleaved_destination) noexcept
{
    if (interleaved_destination.size() % kPcm16StereoChannelCount != 0) {
        return {Pcm16BufferStatus::invalid_frame_alignment, 0};
    }

    const std::size_t requested_frames =
        interleaved_destination.size() / kPcm16StereoChannelCount;
    if (requested_frames == 0) {
        return {Pcm16BufferStatus::ok, 0};
    }

    const std::uint64_t read = read_position_.load(std::memory_order_relaxed);
    std::uint64_t write = write_position_.load(std::memory_order_acquire);
    if (write == read && closed_.load(std::memory_order_acquire)) {
        // The close acquire synchronizes with the producer's final release.
        // Reloading afterwards prevents an earlier stale write-position
        // observation from being mistaken for a drained queue.
        write = write_position_.load(std::memory_order_acquire);
        if (write == read) {
            return {Pcm16BufferStatus::closed, 0};
        }
    }

    const auto available_frames = static_cast<std::size_t>(write - read);
    const std::size_t frames_to_read =
        std::min(requested_frames, available_frames);
    if (frames_to_read == 0) {
        return {Pcm16BufferStatus::empty, 0};
    }

    const std::size_t read_index =
        static_cast<std::size_t>(read % capacity_frames_);
    const std::size_t first_frames =
        std::min(frames_to_read, capacity_frames_ - read_index);
    const std::size_t first_samples = samples_for_frames(first_frames);
    std::memcpy(
        interleaved_destination.data(),
        samples_.data() + samples_for_frames(read_index),
        first_samples * sizeof(std::int16_t));

    const std::size_t second_frames = frames_to_read - first_frames;
    if (second_frames != 0) {
        std::memcpy(
            interleaved_destination.data() + first_samples,
            samples_.data(),
            samples_for_frames(second_frames) * sizeof(std::int16_t));
    }

    read_position_.store(read + frames_to_read, std::memory_order_release);
    return {
        frames_to_read == requested_frames ? Pcm16BufferStatus::ok
                                           : Pcm16BufferStatus::partial,
        frames_to_read,
    };
}

void Pcm16SpscRingBuffer::close() noexcept
{
    closed_.store(true, std::memory_order_release);
}

void Pcm16SpscRingBuffer::reset() noexcept
{
    read_position_.store(0, std::memory_order_relaxed);
    write_position_.store(0, std::memory_order_relaxed);
    closed_.store(false, std::memory_order_release);
}

std::size_t Pcm16SpscRingBuffer::checked_sample_capacity(
    const std::size_t capacity_frames)
{
    if (capacity_frames == 0) {
        throw std::invalid_argument("PCM ring-buffer capacity must be nonzero");
    }
    if (capacity_frames >
            std::numeric_limits<std::size_t>::max() /
                kPcm16StereoChannelCount ||
        capacity_frames > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("PCM ring-buffer capacity is too large");
    }

    return samples_for_frames(capacity_frames);
}

} // namespace cd404::audio
