#include <cd404/core/cd_time.hpp>
#include <cd404/disc/toc.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures{};

void expect(const bool condition, const std::string_view description)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

void test_cd_time_conversions()
{
    using namespace cd404::core;

    expect(is_valid_msf(Msf{0, 2, 0}), "00:02:00 is a valid MSF value");
    expect(!is_valid_msf(Msf{0, 60, 0}), "second 60 is rejected");
    expect(!is_valid_msf(Msf{0, 0, 75}), "frame 75 is rejected");

    const auto absolute_sector = msf_to_absolute_sector(Msf{0, 2, 0});
    expect(absolute_sector == 150, "00:02:00 converts to absolute sector 150");

    const auto lba = absolute_sector_to_lba(150);
    expect(lba == 0, "absolute sector 150 converts to LBA 0");
    expect(lba_to_absolute_sector(0) == 150, "LBA 0 converts to absolute sector 150");
    expect(lba_to_absolute_sector(-151) == std::nullopt, "LBA before lead-in is rejected");
    expect(
        lba_to_absolute_sector(std::numeric_limits<Sector>::max()) == std::nullopt,
        "LBA conversion overflow is rejected");

    const auto round_trip = absolute_sector_to_msf(332'999);
    expect(round_trip == Msf{73, 59, 74}, "absolute sector round-trips to MSF");

    expect(
        sector_count_to_sample_frames(75) == kCdSampleFramesPerSecond,
        "75 sectors equal one second of CD sample frames");
    expect(
        sector_count_to_sample_frames(-1) == std::nullopt,
        "negative sector count is rejected");
    expect(
        sector_count_to_sample_frames(std::numeric_limits<Sector>::max()) ==
            std::nullopt,
        "sample-frame multiplication overflow is rejected");
    expect(lba_to_disc_frame(75, 0) == 44'100, "disc frame is relative to origin LBA");
    expect(lba_to_disc_frame(-1, 0) == std::nullopt, "LBA before origin is rejected");
    expect(
        lba_to_disc_frame(std::numeric_limits<Sector>::max(), -1) == std::nullopt,
        "disc-frame subtraction overflow is rejected");
}

void test_valid_toc()
{
    using namespace cd404::disc;

    constexpr std::array entries{
        RawTocEntry{1, 0, true},
        RawTocEntry{2, 10'000, true},
        RawTocEntry{3, 20'000, false},
    };

    TocError error{};
    const auto toc = Toc::create(entries, 25'000, error);
    expect(toc.has_value(), "valid TOC is accepted");
    expect(error == TocError::none, "valid TOC has no validation error");
    if (!toc) {
        return;
    }

    expect(toc->tracks().size() == 3, "valid TOC creates three tracks");
    expect(toc->origin_lba() == 0, "TOC origin is first track LBA");
    expect(toc->lead_out_lba() == 25'000, "TOC preserves lead-out LBA");

    const Track* second = toc->find_track(2);
    expect(second != nullptr, "track lookup finds existing track");
    if (second != nullptr) {
        expect(second->start_lba == 10'000, "track start LBA is preserved");
        expect(second->end_lba == 20'000, "track end is next track start");
        expect(second->start_disc_frame == 5'880'000, "track absolute frame is derived");
        expect(second->frame_count == 5'880'000, "track frame count is derived");
        expect(second->is_audio, "audio flag is preserved");
    }

    const Track* third = toc->find_track(3);
    expect(third != nullptr && !third->is_audio, "data track flag is preserved");
    expect(toc->find_track(99) == nullptr, "missing track lookup returns null");
}

void test_invalid_toc()
{
    using namespace cd404::disc;

    TocError error{};
    expect(
        !Toc::create(std::span<const RawTocEntry>{}, 1, error) &&
            error == TocError::no_tracks,
        "empty TOC is rejected");

    constexpr std::array non_consecutive{
        RawTocEntry{1, 0, true},
        RawTocEntry{3, 100, true},
    };
    expect(
        !Toc::create(non_consecutive, 200, error) &&
            error == TocError::non_consecutive_track_number,
        "non-consecutive track numbers are rejected");

    constexpr std::array non_increasing{
        RawTocEntry{1, 100, true},
        RawTocEntry{2, 100, true},
    };
    expect(
        !Toc::create(non_increasing, 200, error) &&
            error == TocError::non_increasing_track_start,
        "non-increasing track starts are rejected");

    constexpr std::array invalid_lead_out{
        RawTocEntry{1, 100, true},
    };
    expect(
        !Toc::create(invalid_lead_out, 100, error) &&
            error == TocError::invalid_lead_out,
        "lead-out at track start is rejected");
}

} // namespace

int main()
{
    test_cd_time_conversions();
    test_valid_toc();
    test_invalid_toc();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All CD.404 foundation tests passed.\n";
    return 0;
}
