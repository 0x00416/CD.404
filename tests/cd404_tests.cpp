#include <cd404/audio/cdda_sector_source.hpp>
#include <cd404/audio/continuous_cdda_stream.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/disc/toc.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

int failures{};

class PatternSectorSource final : public cd404::audio::CddaSectorSource {
public:
    PatternSectorSource(
        const cd404::core::Sector first_lba,
        const cd404::core::Sector end_lba)
        : first_lba_(first_lba), end_lba_(end_lba)
    {
    }

    [[nodiscard]] cd404::core::Sector first_lba() const noexcept override
    {
        return first_lba_;
    }

    [[nodiscard]] cd404::core::Sector end_lba() const noexcept override
    {
        return end_lba_;
    }

    [[nodiscard]] cd404::audio::SectorReadResult read_sectors(
        const cd404::core::Sector start_lba,
        const std::span<std::byte> destination) override
    {
        using namespace cd404;

        const auto bytes_per_sector =
            static_cast<std::size_t>(core::kCdBytesPerSector);
        if (start_lba < first_lba_ || destination.size() % bytes_per_sector != 0) {
            return {audio::ReadStatus::invalid_request, 0, 0};
        }
        if (start_lba >= end_lba_) {
            return {audio::ReadStatus::end_of_stream, 0, 0};
        }

        const std::size_t requested_sectors = destination.size() / bytes_per_sector;
        const std::size_t available_sectors =
            static_cast<std::size_t>(end_lba_ - start_lba);
        const std::size_t sectors_to_write =
            std::min(requested_sectors, available_sectors);

        for (std::size_t sector = 0; sector < sectors_to_write; ++sector) {
            const auto current_lba =
                start_lba + static_cast<core::Sector>(sector);
            const auto first_frame = static_cast<std::uint32_t>(
                (current_lba - first_lba_) * core::kCdSampleFramesPerSector);

            for (std::uint32_t frame = 0;
                 frame < static_cast<std::uint32_t>(core::kCdSampleFramesPerSector);
                 ++frame) {
                const std::uint32_t value = first_frame + frame;
                const std::size_t offset = sector * bytes_per_sector +
                                           static_cast<std::size_t>(frame) * 4;
                destination[offset] = static_cast<std::byte>(value & 0xffU);
                destination[offset + 1] =
                    static_cast<std::byte>((value >> 8U) & 0xffU);
                destination[offset + 2] =
                    static_cast<std::byte>((value >> 16U) & 0xffU);
                destination[offset + 3] =
                    static_cast<std::byte>((value >> 24U) & 0xffU);
            }
        }

        return {audio::ReadStatus::ok, sectors_to_write, 0};
    }

private:
    cd404::core::Sector first_lba_{};
    cd404::core::Sector end_lba_{};
};

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

[[nodiscard]] std::uint32_t read_pattern_frame(
    const std::span<const std::byte> bytes,
    const std::size_t frame)
{
    const std::size_t offset = frame * 4;
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void test_continuous_cdda_stream()
{
    using namespace cd404;

    constexpr core::Sector kFirstLba = 100;
    constexpr core::Sector kTrackBoundaryLba = 102;
    constexpr core::Sector kEndLba = 104;
    PatternSectorSource source(kFirstLba, kEndLba);
    audio::ContinuousCddaStream stream(source, kFirstLba, kEndLba);

    expect(stream.valid(), "continuous stream accepts a valid source range");
    expect(stream.position() == 0, "continuous stream starts at frame zero");
    expect(stream.total_frames() == 2'352, "four sectors contain 2352 frames");

    const auto total_bytes = static_cast<std::size_t>(
        stream.total_frames() * core::kCdBytesPerSampleFrame);
    std::vector<std::byte> output(total_bytes);
    constexpr std::array<std::size_t, 7> chunk_frames{
        1,
        586,
        2,
        137,
        700,
        17,
        909,
    };

    std::size_t output_offset{};
    std::size_t chunk_index{};
    while (output_offset < output.size()) {
        const std::size_t remaining_frames =
            (output.size() - output_offset) /
            static_cast<std::size_t>(core::kCdBytesPerSampleFrame);
        const std::size_t frames_to_read =
            std::min(chunk_frames[chunk_index % chunk_frames.size()], remaining_frames);
        const std::size_t bytes_to_read =
            frames_to_read * static_cast<std::size_t>(core::kCdBytesPerSampleFrame);
        const auto result = stream.read_frames(
            std::span<std::byte>(output.data() + output_offset, bytes_to_read));
        expect(result.status == audio::ReadStatus::ok, "chunked stream read succeeds");
        expect(result.frames_read == frames_to_read, "chunked stream returns requested frames");
        output_offset += bytes_to_read;
        ++chunk_index;
    }

    for (std::size_t frame = 0; frame < static_cast<std::size_t>(stream.total_frames());
         ++frame) {
        if (read_pattern_frame(output, frame) != frame) {
            expect(false, "continuous output contains no dropped or duplicated frames");
            break;
        }
    }

    const std::size_t boundary_frame = static_cast<std::size_t>(
        (kTrackBoundaryLba - kFirstLba) * core::kCdSampleFramesPerSector);
    expect(
        read_pattern_frame(output, boundary_frame - 1) == boundary_frame - 1,
        "last frame before track boundary is preserved");
    expect(
        read_pattern_frame(output, boundary_frame) == boundary_frame,
        "first frame after track boundary immediately follows");

    std::array<std::byte, 4> end_buffer{};
    const auto end_result = stream.read_frames(end_buffer);
    expect(
        end_result.status == audio::ReadStatus::end_of_stream &&
            end_result.frames_read == 0,
        "read after final frame reports end of stream");

    expect(
        stream.seek(static_cast<core::SampleFrame>(boundary_frame - 1)),
        "stream can seek to frame before track boundary");
    std::array<std::byte, 12> seek_buffer{};
    const auto seek_result = stream.read_frames(seek_buffer);
    expect(
        seek_result.status == audio::ReadStatus::ok && seek_result.frames_read == 3,
        "unaligned seek reads across track boundary");
    expect(
        read_pattern_frame(seek_buffer, 0) == boundary_frame - 1 &&
            read_pattern_frame(seek_buffer, 1) == boundary_frame &&
            read_pattern_frame(seek_buffer, 2) == boundary_frame + 1,
        "seek preserves exact boundary frame order");

    std::array<std::byte, 3> invalid_buffer{};
    expect(
        stream.read_frames(invalid_buffer).status == audio::ReadStatus::invalid_request,
        "non-frame-aligned destination is rejected");

    audio::ContinuousCddaStream invalid_stream(source, kFirstLba - 1, kEndLba);
    expect(!invalid_stream.valid(), "stream rejects a range outside its source");

    PatternSectorSource extreme_source(
        std::numeric_limits<core::Sector>::min(),
        std::numeric_limits<core::Sector>::max());
    audio::ContinuousCddaStream overflowing_stream(
        extreme_source,
        extreme_source.first_lba(),
        extreme_source.end_lba());
    expect(!overflowing_stream.valid(), "stream rejects an overflowing LBA range");
}

} // namespace

int main()
{
    test_cd_time_conversions();
    test_valid_toc();
    test_invalid_toc();
    test_continuous_cdda_stream();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All CD.404 foundation tests passed.\n";
    return 0;
}
