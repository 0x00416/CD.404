#pragma once

#include <cd404/core/cd_time.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cd404::disc {

struct RawTocEntry final {
    std::uint8_t number{};
    core::Sector start_lba{};
    bool is_audio{};
};

struct Track final {
    std::uint8_t number{};
    core::Sector start_lba{};
    core::Sector end_lba{};
    core::SampleFrame start_disc_frame{};
    core::SampleFrame frame_count{};
    bool is_audio{};
};

enum class TocError {
    none,
    no_tracks,
    invalid_track_number,
    non_consecutive_track_number,
    non_increasing_track_start,
    invalid_lead_out,
    arithmetic_overflow,
};

class Toc final {
public:
    [[nodiscard]] static std::optional<Toc> create(
        std::span<const RawTocEntry> entries,
        core::Sector lead_out_lba,
        TocError& error);

    [[nodiscard]] const std::vector<Track>& tracks() const noexcept;
    [[nodiscard]] core::Sector origin_lba() const noexcept;
    [[nodiscard]] core::Sector lead_out_lba() const noexcept;
    [[nodiscard]] const Track* find_track(std::uint8_t number) const noexcept;

private:
    Toc(
        std::vector<Track> tracks,
        core::Sector origin_lba,
        core::Sector lead_out_lba);

    std::vector<Track> tracks_;
    core::Sector origin_lba_{};
    core::Sector lead_out_lba_{};
};

[[nodiscard]] const char* to_string(TocError error) noexcept;

} // namespace cd404::disc
