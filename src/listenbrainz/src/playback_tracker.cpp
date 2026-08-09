#include <cd404/listenbrainz/playback_tracker.hpp>

#include <algorithm>
#include <utility>

namespace cd404::listenbrainz {
namespace {

constexpr core::SampleFrame kFourMinutesFrames =
    4 * 60 * core::kCdSampleFramesPerSecond;

[[nodiscard]] core::SampleFrame submission_threshold(
    const core::SampleFrame duration_frames) noexcept
{
    if (duration_frames <= 0) {
        return 0;
    }
    return std::min(duration_frames / 2, kFourMinutesFrames);
}

[[nodiscard]] bool has_musicbrainz_identity(const TrackMetadata& track) noexcept
{
    return !track.recording_mbid.empty() || !track.release_mbid.empty() ||
        !track.release_group_mbid.empty() || !track.track_mbid.empty() ||
        !track.artist_mbids.empty();
}

} // namespace

PlaybackTracker::PlaybackTracker(SubmitCallback callback)
    : callback_(std::move(callback))
{
}

void PlaybackTracker::begin(
    TrackMetadata track,
    const core::SampleFrame position_frames,
    const std::int64_t unix_time)
{
    finish_track();
    start_track(std::move(track), position_frames, unix_time);
}

void PlaybackTracker::update(
    TrackMetadata track,
    const core::SampleFrame position_frames,
    const std::int64_t unix_time)
{
    if (!active_) {
        start_track(std::move(track), position_frames, unix_time);
        return;
    }
    if (track.track_number != track_.track_number) {
        finish_track();
        start_track(std::move(track), 0, unix_time);
    }

    if (position_frames >= last_position_frames_) {
        rendered_frames_ += position_frames - last_position_frames_;
    }
    last_position_frames_ = position_frames;
    const bool identity_was_available = has_musicbrainz_identity(track_);
    merge_metadata(track);
    submit_playing_now_if_ready();
    submit_playing_now_identity_update_if_ready(identity_was_available);
    submit_single_if_ready();
}

void PlaybackTracker::end()
{
    finish_track();
}

void PlaybackTracker::seek(const core::SampleFrame position_frames) noexcept
{
    if (active_) {
        last_position_frames_ = std::max<core::SampleFrame>(position_frames, 0);
    }
}

bool PlaybackTracker::active() const noexcept
{
    return active_;
}

core::SampleFrame PlaybackTracker::rendered_frames() const noexcept
{
    return rendered_frames_;
}

PlaybackSubmissionProgress PlaybackTracker::progress() const noexcept
{
    return PlaybackSubmissionProgress{
        active_,
        playing_now_submitted_,
        single_submitted_,
        rendered_frames_,
        submission_threshold(track_.duration_frames),
        track_.duration_frames,
    };
}

void PlaybackTracker::start_track(
    TrackMetadata track,
    const core::SampleFrame position_frames,
    const std::int64_t unix_time)
{
    track_ = std::move(track);
    last_position_frames_ = std::max<core::SampleFrame>(position_frames, 0);
    rendered_frames_ = 0;
    listened_at_ = unix_time;
    active_ = track_.duration_frames > 0;
    playing_now_submitted_ = false;
    playing_now_identity_update_submitted_ = false;
    single_submitted_ = false;
    submit_playing_now_if_ready();
}

void PlaybackTracker::merge_metadata(const TrackMetadata& track)
{
    if (!track.title.empty()) {
        track_.title = track.title;
    }
    if (!track.artist.empty()) {
        track_.artist = track.artist;
    }
    if (!track.release.empty()) {
        track_.release = track.release;
    }
    if (track.duration_frames > 0) {
        track_.duration_frames = track.duration_frames;
    }
    if (!track.recording_mbid.empty()) {
        track_.recording_mbid = track.recording_mbid;
    }
    if (!track.release_mbid.empty()) {
        track_.release_mbid = track.release_mbid;
    }
    if (!track.release_group_mbid.empty()) {
        track_.release_group_mbid = track.release_group_mbid;
    }
    if (!track.track_mbid.empty()) {
        track_.track_mbid = track.track_mbid;
    }
    if (!track.artist_mbids.empty()) {
        track_.artist_mbids = track.artist_mbids;
    }
}

void PlaybackTracker::submit_playing_now_if_ready()
{
    if (!active_ || playing_now_submitted_ || !metadata_ready() || !callback_) {
        return;
    }

    callback_(Submission{
        SubmissionType::playing_now,
        0,
        track_.title,
        track_.artist,
        track_.release,
        static_cast<std::uint64_t>(
            track_.duration_frames * 1'000 / core::kCdSampleFramesPerSecond),
        0,
        track_.track_number,
        track_.recording_mbid,
        track_.release_mbid,
        track_.release_group_mbid,
        track_.track_mbid,
        track_.artist_mbids,
    });
    playing_now_submitted_ = true;
    playing_now_identity_update_submitted_ = has_musicbrainz_identity(track_);
}

void PlaybackTracker::submit_playing_now_identity_update_if_ready(
    const bool identity_was_available)
{
    if (!active_ || !playing_now_submitted_ ||
        playing_now_identity_update_submitted_ || identity_was_available ||
        !has_musicbrainz_identity(track_) || !callback_) {
        return;
    }
    callback_(Submission{
        SubmissionType::playing_now,
        0,
        track_.title,
        track_.artist,
        track_.release,
        static_cast<std::uint64_t>(
            track_.duration_frames * 1'000 / core::kCdSampleFramesPerSecond),
        0,
        track_.track_number,
        track_.recording_mbid,
        track_.release_mbid,
        track_.release_group_mbid,
        track_.track_mbid,
        track_.artist_mbids,
    });
    playing_now_identity_update_submitted_ = true;
}

void PlaybackTracker::finish_track()
{
    if (!active_) {
        return;
    }

    submit_single_if_ready();

    track_ = {};
    last_position_frames_ = 0;
    rendered_frames_ = 0;
    listened_at_ = 0;
    active_ = false;
    playing_now_submitted_ = false;
    playing_now_identity_update_submitted_ = false;
    single_submitted_ = false;
}

void PlaybackTracker::submit_single_if_ready()
{
    if (!active_ || single_submitted_) {
        return;
    }

    const core::SampleFrame threshold = submission_threshold(track_.duration_frames);
    if (threshold > 0 && rendered_frames_ >= threshold && metadata_ready() && callback_) {
        callback_(Submission{
            SubmissionType::single,
            listened_at_,
            track_.title,
            track_.artist,
            track_.release,
            static_cast<std::uint64_t>(
                track_.duration_frames * 1'000 / core::kCdSampleFramesPerSecond),
            static_cast<std::uint64_t>(
                rendered_frames_ / core::kCdSampleFramesPerSecond),
            track_.track_number,
            track_.recording_mbid,
            track_.release_mbid,
            track_.release_group_mbid,
            track_.track_mbid,
            track_.artist_mbids,
        });
        single_submitted_ = true;
    }
}

bool PlaybackTracker::metadata_ready() const noexcept
{
    return !track_.title.empty() && !track_.artist.empty();
}

} // namespace cd404::listenbrainz
