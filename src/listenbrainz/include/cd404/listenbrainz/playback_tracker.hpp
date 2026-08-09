#pragma once

#include <cd404/core/cd_time.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cd404::listenbrainz {

enum class SubmissionType {
    playing_now,
    single,
};

struct TrackMetadata final {
    unsigned int track_number{};
    std::wstring title;
    std::wstring artist;
    std::wstring release;
    core::SampleFrame duration_frames{};
    std::wstring recording_mbid;
    std::wstring release_mbid;
    std::wstring release_group_mbid;
    std::wstring track_mbid;
    std::vector<std::wstring> artist_mbids;
};

struct Submission final {
    SubmissionType type{SubmissionType::playing_now};
    std::int64_t listened_at{};
    std::wstring track_name;
    std::wstring artist_name;
    std::wstring release_name;
    std::uint64_t duration_milliseconds{};
    std::uint64_t duration_played_seconds{};
    unsigned int track_number{};
    std::wstring recording_mbid;
    std::wstring release_mbid;
    std::wstring release_group_mbid;
    std::wstring track_mbid;
    std::vector<std::wstring> artist_mbids;
};

struct PlaybackSubmissionProgress final {
    bool active{};
    bool playing_now_submitted{};
    bool single_submitted{};
    core::SampleFrame rendered_frames{};
    core::SampleFrame threshold_frames{};
    core::SampleFrame duration_frames{};
};

// Converts rendered-frame progress into ListenBrainz submissions. The tracker
// never performs network I/O and can therefore be driven directly by the UI.
class PlaybackTracker final {
public:
    using SubmitCallback = std::function<void(const Submission&)>;

    explicit PlaybackTracker(SubmitCallback callback);

    void begin(
        TrackMetadata track,
        core::SampleFrame position_frames,
        std::int64_t unix_time);
    void update(
        TrackMetadata track,
        core::SampleFrame position_frames,
        std::int64_t unix_time);
    void seek(core::SampleFrame position_frames) noexcept;
    void end();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] core::SampleFrame rendered_frames() const noexcept;
    [[nodiscard]] PlaybackSubmissionProgress progress() const noexcept;

private:
    void start_track(
        TrackMetadata track,
        core::SampleFrame position_frames,
        std::int64_t unix_time);
    void merge_metadata(const TrackMetadata& track);
    void submit_playing_now_if_ready();
    void submit_single_if_ready();
    void finish_track();
    [[nodiscard]] bool metadata_ready() const noexcept;

    SubmitCallback callback_;
    TrackMetadata track_;
    core::SampleFrame last_position_frames_{};
    core::SampleFrame rendered_frames_{};
    std::int64_t listened_at_{};
    bool active_{};
    bool playing_now_submitted_{};
    bool single_submitted_{};
};

} // namespace cd404::listenbrainz
