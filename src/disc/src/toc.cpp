#include <cd404/disc/toc.hpp>

#include <algorithm>
#include <utility>

namespace cd404::disc {

std::optional<Toc> Toc::create(
    const std::span<const RawTocEntry> entries,
    const core::Sector lead_out_lba,
    TocError& error)
{
    error = TocError::none;

    if (entries.empty()) {
        error = TocError::no_tracks;
        return std::nullopt;
    }

    if (entries.front().number < 1 || entries.front().number > 99) {
        error = TocError::invalid_track_number;
        return std::nullopt;
    }

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto expected_number =
            static_cast<unsigned int>(entries.front().number) + index;

        if (expected_number > 99 || entry.number != expected_number) {
            error = TocError::non_consecutive_track_number;
            return std::nullopt;
        }

        if (index > 0 && entry.start_lba <= entries[index - 1].start_lba) {
            error = TocError::non_increasing_track_start;
            return std::nullopt;
        }
    }

    if (lead_out_lba <= entries.back().start_lba) {
        error = TocError::invalid_lead_out;
        return std::nullopt;
    }

    const core::Sector origin_lba = entries.front().start_lba;
    std::vector<Track> tracks;
    tracks.reserve(entries.size());

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const core::Sector end_lba =
            index + 1 < entries.size() ? entries[index + 1].start_lba : lead_out_lba;
        const auto start_disc_frame =
            core::lba_to_disc_frame(entry.start_lba, origin_lba);
        const auto frame_count =
            core::sector_count_to_sample_frames(end_lba - entry.start_lba);

        if (!start_disc_frame || !frame_count) {
            error = TocError::arithmetic_overflow;
            return std::nullopt;
        }

        tracks.push_back(Track{
            entry.number,
            entry.start_lba,
            end_lba,
            *start_disc_frame,
            *frame_count,
            entry.is_audio,
        });
    }

    return Toc{std::move(tracks), origin_lba, lead_out_lba};
}

Toc::Toc(
    std::vector<Track> tracks,
    const core::Sector origin_lba,
    const core::Sector lead_out_lba)
    : tracks_(std::move(tracks)),
      origin_lba_(origin_lba),
      lead_out_lba_(lead_out_lba)
{
}

const std::vector<Track>& Toc::tracks() const noexcept
{
    return tracks_;
}

core::Sector Toc::origin_lba() const noexcept
{
    return origin_lba_;
}

core::Sector Toc::lead_out_lba() const noexcept
{
    return lead_out_lba_;
}

const Track* Toc::find_track(const std::uint8_t number) const noexcept
{
    const auto iterator = std::find_if(
        tracks_.begin(),
        tracks_.end(),
        [number](const Track& track) { return track.number == number; });
    return iterator == tracks_.end() ? nullptr : &*iterator;
}

const char* to_string(const TocError error) noexcept
{
    switch (error) {
    case TocError::none:
        return "none";
    case TocError::no_tracks:
        return "no tracks";
    case TocError::invalid_track_number:
        return "invalid track number";
    case TocError::non_consecutive_track_number:
        return "non-consecutive track number";
    case TocError::non_increasing_track_start:
        return "non-increasing track start";
    case TocError::invalid_lead_out:
        return "invalid lead-out";
    case TocError::arithmetic_overflow:
        return "arithmetic overflow";
    }

    return "unknown";
}

} // namespace cd404::disc
