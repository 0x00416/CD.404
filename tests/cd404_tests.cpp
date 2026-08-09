#include <cd404/audio/cdda_pcm.hpp>
#include <cd404/audio/cdda_sector_source.hpp>
#include <cd404/audio/continuous_cdda_stream.hpp>
#include <cd404/audio/pcm16_spsc_ring_buffer.hpp>
#include <cd404/audio/pcm16_volume.hpp>
#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/audio/reliable_cdda_sector_source.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/disc/cd_text.hpp>
#include <cd404/disc/gnudb.hpp>
#include <cd404/disc/toc.hpp>
#include <cd404/listenbrainz/playback_tracker.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
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

class FaultInjectingSectorSource final : public cd404::audio::CddaSectorSource {
public:
    FaultInjectingSectorSource(
        const cd404::core::Sector first_lba,
        const cd404::core::Sector end_lba)
        : pattern_(first_lba, end_lba)
    {
    }

    [[nodiscard]] cd404::core::Sector first_lba() const noexcept override
    {
        return pattern_.first_lba();
    }

    [[nodiscard]] cd404::core::Sector end_lba() const noexcept override
    {
        return pattern_.end_lba();
    }

    [[nodiscard]] cd404::audio::SectorReadResult read_sectors(
        const cd404::core::Sector start_lba,
        const std::span<std::byte> destination) override
    {
        using namespace cd404::audio;

        if (forced_status_ != ReadStatus::ok) {
            const ReadStatus status = std::exchange(forced_status_, ReadStatus::ok);
            return {status, 0, 995};
        }

        if (failures_remaining_ != 0) {
            --failures_remaining_;
            return {ReadStatus::io_error, 0, 123};
        }

        auto result = pattern_.read_sectors(start_lba, destination);
        if (result.status == ReadStatus::ok && short_reads_remaining_ != 0) {
            --short_reads_remaining_;
            result.sectors_read = result.sectors_read == 0
                ? 0
                : result.sectors_read - 1;
        }
        if (result.status == ReadStatus::ok && corruption_remaining_ != 0 &&
            start_lba == corruption_start_lba_ && !destination.empty()) {
            destination.front() ^= std::byte{0xff};
            --corruption_remaining_;
        }
        return result;
    }

    void fail_next_reads(const std::size_t count) noexcept
    {
        failures_remaining_ = count;
    }

    void corrupt_next_read_starting_at(
        const cd404::core::Sector start_lba) noexcept
    {
        corruption_start_lba_ = start_lba;
        corruption_remaining_ = 1;
    }

    void return_next_status(const cd404::audio::ReadStatus status) noexcept
    {
        forced_status_ = status;
    }

    void shorten_next_reads(const std::size_t count) noexcept
    {
        short_reads_remaining_ = count;
    }

private:
    PatternSectorSource pattern_;
    std::size_t failures_remaining_{};
    cd404::core::Sector corruption_start_lba_{};
    std::size_t corruption_remaining_{};
    std::size_t short_reads_remaining_{};
    cd404::audio::ReadStatus forced_status_{cd404::audio::ReadStatus::ok};
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

void test_cd_text_parsing()
{
    using namespace cd404::disc;

    CdTextPack title_first;
    title_first.type = kCdTextAlbumNamePack;
    title_first.track_number = 0;
    title_first.sequence_number = 0;
    title_first.payload = {
        'A', 'l', 'b', 'u', 'm', 0, 'F', 'i', 'r', 's', 't', ' ',
    };

    CdTextPack title_second;
    title_second.type = kCdTextAlbumNamePack;
    title_second.track_number = 1;
    title_second.sequence_number = 1;
    title_second.payload = {
        'S', 'o', 'n', 'g', 0, 'S', 'e', 'c', 'o', 'n', 'd', ' ',
    };

    CdTextPack title_third;
    title_third.type = kCdTextAlbumNamePack;
    title_third.track_number = 2;
    title_third.sequence_number = 2;
    title_third.payload = {'T', 'r', 'a', 'c', 'k', 0};

    CdTextPack performer;
    performer.type = kCdTextPerformerPack;
    performer.track_number = 0;
    performer.sequence_number = 3;
    performer.payload = {
        'A', 'r', 't', 'i', 's', 't', 0, 'B', 'a', 'n', 'd', 0,
    };

    CdTextPack repeated_performer;
    repeated_performer.type = kCdTextPerformerPack;
    repeated_performer.track_number = 2;
    repeated_performer.sequence_number = 4;
    repeated_performer.payload = {'\t', 0};

    const std::array packs{
        title_second,
        performer,
        title_third,
        title_first,
        repeated_performer,
    };
    const auto metadata = parse_cd_text(packs);
    expect(metadata.album_title == u"Album", "CD-TEXT parses the album title");
    expect(metadata.album_performer == u"Artist", "CD-TEXT parses the album performer");
    expect(metadata.tracks[1].title == u"First Song", "CD-TEXT joins text across packs");
    expect(metadata.tracks[2].title == u"Second Track", "CD-TEXT advances at terminators");
    expect(metadata.tracks[1].performer == u"Band", "CD-TEXT parses track performer");
    expect(metadata.tracks[2].performer == u"Band", "CD-TEXT expands tab repetition");
    expect(!metadata.empty(), "parsed CD-TEXT reports non-empty metadata");
}

void test_gnudb_identity_and_entry()
{
    using namespace cd404;
    using namespace cd404::disc;

    constexpr std::array<std::uint32_t, 13> absolute_offsets{
        150, 15'105, 26'335, 40'545, 48'890, 66'822, 92'035,
        104'685, 114'340, 130'040, 146'350, 165'575, 171'530,
    };
    std::vector<RawTocEntry> entries;
    entries.reserve(absolute_offsets.size());
    for (std::size_t index = 0; index < absolute_offsets.size(); ++index) {
        entries.push_back(RawTocEntry{
            static_cast<std::uint8_t>(index + 1U),
            static_cast<core::Sector>(absolute_offsets[index]) -
                core::kCdProgramAreaOffsetSectors,
            true,
        });
    }
    TocError error{};
    const auto toc = Toc::create(
        entries,
        2'358 * core::kCdSectorsPerSecond - core::kCdProgramAreaOffsetSectors,
        error);
    expect(toc.has_value(), "GnuDB reference TOC is valid");
    if (toc) {
        const auto identity = make_gnudb_disc_identity(*toc);
        expect(
            identity && identity->disc_id == 0x9a09340dU &&
                identity->disc_length_seconds == 2'358U &&
                identity->track_offsets ==
                    std::vector<std::uint32_t>(absolute_offsets.begin(), absolute_offsets.end()),
            "GnuDB Disc ID matches the published protocol example");
    }

    constexpr std::string_view response =
        "210 data 12345603 entry follows\r\n"
        "# xmcd database file\r\n"
        "DTITLE=Various Artists / Example Album\r\n"
        "TTITLE0=Artist One / First Track\r\n"
        "TTITLE1=Artist Two / Second\r\n"
        "TTITLE1= Track\r\n"
        "TTITLE2=Artist Three / Third Track\r\n"
        ".\r\n";
    const auto metadata = parse_gnudb_entry(response, 3);
    expect(
        metadata && metadata->album_title == "Example Album" &&
            metadata->album_artist == "Various Artists" &&
            metadata->track_titles ==
                std::vector<std::string>{"First Track", "Second Track", "Third Track"} &&
            metadata->track_artists ==
                std::vector<std::string>{"Artist One", "Artist Two", "Artist Three"},
        "GnuDB UTF-8 entry joins repeated fields and separates compilation artists");
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

void test_reliable_cdda_sector_source()
{
    using namespace cd404;

    constexpr std::size_t kSectorBytes =
        static_cast<std::size_t>(core::kCdBytesPerSector);
    FaultInjectingSectorSource source(0, 8);
    source.fail_next_reads(2);
    audio::ReliableCddaSectorSource reliable(
        source,
        audio::ReliableReadPolicy{3, true});

    std::array<std::byte, 2 * kSectorBytes> first_block{};
    const auto first_result = reliable.read_sectors(0, first_block);
    expect(
        first_result.status == audio::ReadStatus::ok &&
            first_result.sectors_read == 2,
        "reliable source recovers after two bounded I/O retries");
    expect(
        read_pattern_frame(first_block, 0) == 0 &&
            read_pattern_frame(
                first_block,
                2 * static_cast<std::size_t>(core::kCdSampleFramesPerSector) - 1) ==
                2 * static_cast<std::size_t>(core::kCdSampleFramesPerSector) - 1,
        "retried block preserves the exact source frames");

    source.corrupt_next_read_starting_at(1);
    std::array<std::byte, 2 * kSectorBytes> second_block{};
    const auto second_result = reliable.read_sectors(2, second_block);
    const auto& statistics = reliable.statistics();
    expect(
        second_result.status == audio::ReadStatus::ok &&
            second_result.sectors_read == 2,
        "overlap mismatch is retried and a matching block is delivered");
    expect(
        read_pattern_frame(second_block, 0) ==
            2 * static_cast<std::size_t>(core::kCdSampleFramesPerSector),
        "overlap verification does not duplicate the verification sector");
    expect(
        statistics.logical_reads == 2 && statistics.device_reads == 5 &&
            statistics.retries == 3 && statistics.overlap_checks == 2 &&
            statistics.overlap_mismatches == 1 &&
            statistics.sectors_delivered == 4,
        "reliable source exposes retry and overlap statistics");

    FaultInjectingSectorSource failing_source(0, 4);
    failing_source.fail_next_reads(3);
    audio::ReliableCddaSectorSource bounded_source(
        failing_source,
        audio::ReliableReadPolicy{2, true});
    std::array<std::byte, kSectorBytes> failed_block{};
    const auto failed_result = bounded_source.read_sectors(0, failed_block);
    expect(
        failed_result.status == audio::ReadStatus::io_error &&
            failed_result.sectors_read == 0 &&
            bounded_source.statistics().device_reads == 2 &&
            bounded_source.statistics().retries == 1,
        "reliable source stops after the configured attempt limit");

    FaultInjectingSectorSource short_source(0, 4);
    short_source.shorten_next_reads(1);
    audio::ReliableCddaSectorSource short_reliable(short_source);
    std::array<std::byte, 2 * kSectorBytes> short_block{};
    const auto short_result = short_reliable.read_sectors(0, short_block);
    expect(
        short_result.status == audio::ReadStatus::ok &&
            short_result.sectors_read == 2 &&
            short_reliable.statistics().device_reads == 2 &&
            short_reliable.statistics().retries == 1,
        "short successful device reads are rejected and retried");

    FaultInjectingSectorSource cancelled_source(0, 4);
    cancelled_source.return_next_status(audio::ReadStatus::cancelled);
    audio::ReliableCddaSectorSource cancelled_reliable(cancelled_source);
    std::array<std::byte, kSectorBytes> cancelled_block{};
    const auto cancelled_result = cancelled_reliable.read_sectors(0, cancelled_block);
    expect(
        cancelled_result.status == audio::ReadStatus::cancelled &&
            cancelled_reliable.statistics().device_reads == 1 &&
            cancelled_reliable.statistics().retries == 0,
        "cancellation is propagated immediately without retry");

    FaultInjectingSectorSource unordered_source(0, 8);
    audio::ReliableCddaSectorSource unordered_reliable(unordered_source);
    std::array<std::byte, kSectorBytes> unordered_block{};
    static_cast<void>(unordered_reliable.read_sectors(0, unordered_block));
    static_cast<void>(unordered_reliable.read_sectors(4, unordered_block));
    expect(
        unordered_reliable.statistics().overlap_checks == 0 &&
            unordered_reliable.statistics().device_reads == 2,
        "non-sequential seeks do not compare unrelated overlap sectors");

    FaultInjectingSectorSource unchecked_source(0, 4);
    audio::ReliableCddaSectorSource unchecked_reliable(
        unchecked_source,
        audio::ReliableReadPolicy{3, false});
    std::array<std::byte, kSectorBytes> unchecked_block{};
    static_cast<void>(unchecked_reliable.read_sectors(0, unchecked_block));
    static_cast<void>(unchecked_reliable.read_sectors(1, unchecked_block));
    expect(
        unchecked_reliable.statistics().overlap_checks == 0 &&
            unchecked_reliable.statistics().device_reads == 2,
        "overlap verification can be disabled explicitly");

    FaultInjectingSectorSource invalid_source(0, 4);
    audio::ReliableCddaSectorSource invalid_reliable(invalid_source);
    const auto invalid_result = invalid_reliable.read_sectors(0, {});
    expect(
        invalid_result.status == audio::ReadStatus::invalid_request &&
            invalid_reliable.statistics().logical_reads == 1 &&
            invalid_reliable.statistics().device_reads == 0,
        "invalid logical reads never reach the device source");
}

void test_playback_state_machine()
{
    using namespace cd404::audio;

    PlaybackStateMachine completed;
    expect(completed.state() == PlaybackState::idle, "playback starts idle");
    expect(
        !completed.apply(PlaybackEvent::prebuffer_ready) &&
            completed.state() == PlaybackState::idle,
        "invalid playback event leaves state unchanged");
    expect(
        completed.apply(PlaybackEvent::open) &&
            completed.apply(PlaybackEvent::source_ready) &&
            completed.apply(PlaybackEvent::prebuffer_ready) &&
            completed.apply(PlaybackEvent::stream_ended) &&
            completed.apply(PlaybackEvent::drain_completed) &&
            completed.state() == PlaybackState::completed,
        "normal playback reaches completed through buffering and draining");
    expect(
        completed.apply(PlaybackEvent::reset) &&
            completed.state() == PlaybackState::idle,
        "completed playback can reset for reuse");

    PlaybackStateMachine paused;
    expect(
        paused.apply(PlaybackEvent::open) &&
            paused.apply(PlaybackEvent::source_ready) &&
            paused.apply(PlaybackEvent::prebuffer_ready) &&
            paused.apply(PlaybackEvent::pause_requested) &&
            paused.state() == PlaybackState::paused &&
            paused.apply(PlaybackEvent::resume_requested) &&
            paused.state() == PlaybackState::playing,
        "playback pauses and resumes without terminating the session");

    PlaybackStateMachine cancelled;
    expect(
        cancelled.apply(PlaybackEvent::open) &&
            cancelled.apply(PlaybackEvent::source_ready) &&
            cancelled.apply(PlaybackEvent::stop_requested) &&
            cancelled.apply(PlaybackEvent::cancellation_completed) &&
            cancelled.state() == PlaybackState::cancelled,
        "buffering playback stops through the explicit cancellation path");

    PlaybackStateMachine playing_cancelled;
    expect(
        playing_cancelled.apply(PlaybackEvent::open) &&
            playing_cancelled.apply(PlaybackEvent::source_ready) &&
            playing_cancelled.apply(PlaybackEvent::prebuffer_ready) &&
            playing_cancelled.apply(PlaybackEvent::stop_requested) &&
            playing_cancelled.apply(PlaybackEvent::cancellation_completed) &&
            playing_cancelled.state() == PlaybackState::cancelled,
        "active playback can be cancelled without entering drain");

    PlaybackStateMachine draining_cancelled;
    expect(
        draining_cancelled.apply(PlaybackEvent::open) &&
            draining_cancelled.apply(PlaybackEvent::source_ready) &&
            draining_cancelled.apply(PlaybackEvent::prebuffer_ready) &&
            draining_cancelled.apply(PlaybackEvent::stream_ended) &&
            draining_cancelled.apply(PlaybackEvent::stop_requested) &&
            draining_cancelled.apply(PlaybackEvent::cancellation_completed) &&
            draining_cancelled.state() == PlaybackState::cancelled,
        "endpoint drain remains cancellable");

    PlaybackStateMachine failed;
    expect(
        failed.apply(PlaybackEvent::open) &&
            failed.apply(PlaybackEvent::failure) &&
            failed.state() == PlaybackState::failed &&
            !failed.apply(PlaybackEvent::drain_completed),
        "opening failure is terminal until reset");
    expect(
        failed.apply(PlaybackEvent::reset) &&
            failed.state() == PlaybackState::idle,
        "failed playback can reset for a later session");
}

void test_cdda_pcm_conversion()
{
    using namespace cd404::audio;

    constexpr std::array<std::byte, 8> source{
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0xff},
        std::byte{0x7f},
        std::byte{0x00},
        std::byte{0x80},
        std::byte{0xff},
        std::byte{0xff},
    };
    std::array<std::byte, 8> destination{};
    const auto copy_result = copy_cdda_to_pcm16le(source, destination);
    expect(
        copy_result.status == CddaPcmConversionStatus::ok &&
            copy_result.frames_written == 2,
        "two aligned CDDA frames convert to PCM16LE");
    expect(destination == source, "CDDA byte order is already PCM16LE");

    std::array<std::int16_t, 4> samples{};
    std::memcpy(samples.data(), destination.data(), destination.size());
    expect(
        samples[0] == 0 && samples[1] == 32'767 && samples[2] == -32'768 &&
            samples[3] == -1,
        "PCM16LE test vector decodes expected signed samples");

    std::array<std::byte, 3> unaligned{};
    expect(
        convert_cdda_to_pcm16le_in_place(unaligned) ==
            CddaPcmConversionStatus::source_not_frame_aligned,
        "unaligned CDDA input is rejected");

    std::array<std::byte, 3> small_destination{};
    expect(
        copy_cdda_to_pcm16le(
            std::span<const std::byte>(source.data(), 4),
            small_destination)
                .status == CddaPcmConversionStatus::destination_too_small,
        "undersized PCM destination is rejected");

    std::array<std::byte, 0> empty{};
    const auto empty_result = copy_cdda_to_pcm16le(empty, empty);
    expect(
        empty_result.status == CddaPcmConversionStatus::ok &&
            empty_result.frames_written == 0,
        "empty CDDA input is valid");
}

void test_pcm16_spsc_ring_buffer()
{
    using namespace cd404::audio;

    Pcm16SpscRingBuffer ring(3);
    constexpr std::array<std::int16_t, 8> source{1, 2, 3, 4, 5, 6, 7, 8};

    const auto first_push = ring.push(std::span(source).first(4));
    expect(
        first_push.status == Pcm16BufferStatus::ok &&
            first_push.frames_transferred == 2,
        "ring accepts two complete stereo frames");

    std::array<std::int16_t, 2> first_frame{};
    const auto first_pop = ring.pop(first_frame);
    expect(
        first_pop.frames_transferred == 1 &&
            first_frame == std::array<std::int16_t, 2>{1, 2},
        "ring pops the oldest stereo frame");

    const auto wrapped_push = ring.push(std::span(source).subspan(4));
    expect(
        wrapped_push.status == Pcm16BufferStatus::ok &&
            wrapped_push.frames_transferred == 2,
        "ring writes across its storage boundary");
    expect(
        ring.push(std::span(source).first(2)).status == Pcm16BufferStatus::full,
        "full ring rejects another frame without blocking");

    ring.close();
    std::array<std::int16_t, 8> remaining{};
    const auto final_pop = ring.pop(remaining);
    expect(
        final_pop.status == Pcm16BufferStatus::partial &&
            final_pop.frames_transferred == 3,
        "closed ring remains readable until drained");
    expect(
        std::equal(
            remaining.begin(),
            remaining.begin() + 6,
            std::array<std::int16_t, 6>{3, 4, 5, 6, 7, 8}.begin()),
        "wrapped frames preserve sample order");
    expect(
        ring.pop(first_frame).status == Pcm16BufferStatus::closed && ring.drained(),
        "drained closed ring reports end of stream");

    ring.reset();
    expect(!ring.closed() && ring.readable_frames() == 0, "reset reopens the ring");
    expect(
        ring.push(std::span(source).first(3)).status ==
            Pcm16BufferStatus::invalid_frame_alignment,
        "ring rejects an odd number of stereo samples");

    bool rejected_zero_capacity{};
    try {
        Pcm16SpscRingBuffer invalid_ring(0);
    } catch (const std::invalid_argument&) {
        rejected_zero_capacity = true;
    }
    expect(rejected_zero_capacity, "ring rejects zero frame capacity");

    constexpr std::uint32_t kConcurrentFrameCount = 50'000;
    Pcm16SpscRingBuffer concurrent_ring(97);
    std::thread producer([&concurrent_ring] {
        std::array<std::int16_t, 26> block{};
        std::uint32_t produced{};
        while (produced < kConcurrentFrameCount) {
            const std::size_t block_frames = std::min<std::size_t>(
                block.size() / kPcm16StereoChannelCount,
                kConcurrentFrameCount - produced);
            for (std::size_t frame = 0; frame < block_frames; ++frame) {
                const auto value = static_cast<std::uint16_t>(produced + frame);
                block[frame * 2] = static_cast<std::int16_t>(value);
                block[frame * 2 + 1] =
                    static_cast<std::int16_t>(static_cast<std::uint16_t>(~value));
            }

            std::size_t block_offset{};
            while (block_offset < block_frames) {
                const auto result = concurrent_ring.push(
                    std::span(block).subspan(
                        block_offset * kPcm16StereoChannelCount,
                        (block_frames - block_offset) *
                            kPcm16StereoChannelCount));
                block_offset += result.frames_transferred;
                if (result.frames_transferred == 0) {
                    std::this_thread::yield();
                }
            }
            produced += static_cast<std::uint32_t>(block_frames);
        }
        concurrent_ring.close();
    });

    std::array<std::int16_t, 34> consumed_samples{};
    std::uint32_t consumed{};
    bool sequence_is_intact{true};
    while (!concurrent_ring.drained()) {
        const auto result = concurrent_ring.pop(consumed_samples);
        if (result.frames_transferred == 0) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t frame = 0; frame < result.frames_transferred; ++frame) {
            const auto value = static_cast<std::uint16_t>(consumed + frame);
            if (consumed_samples[frame * 2] != static_cast<std::int16_t>(value) ||
                consumed_samples[frame * 2 + 1] !=
                    static_cast<std::int16_t>(static_cast<std::uint16_t>(~value))) {
                sequence_is_intact = false;
                break;
            }
        }
        consumed += static_cast<std::uint32_t>(result.frames_transferred);
    }
    producer.join();
    expect(
        sequence_is_intact && consumed == kConcurrentFrameCount,
        "concurrent ring transfer preserves every stereo frame in order");
}

void test_pcm16_volume()
{
    using cd404::audio::apply_pcm16_volume;

    std::array<std::int16_t, 5> unity{-32'768, -1, 0, 1, 32'767};
    const auto original = unity;
    apply_pcm16_volume(unity, 1.0F);
    expect(unity == original, "unity volume preserves every PCM16 sample exactly");

    std::array<std::int16_t, 5> half{-32'768, -10'001, 0, 10'001, 32'767};
    apply_pcm16_volume(half, 0.5F);
    expect(
        half == std::array<std::int16_t, 5>{-16'384, -5'000, 0, 5'000, 16'383},
        "half volume scales signed PCM16 values deterministically");

    std::array<std::int16_t, 3> muted{-32'768, 123, 32'767};
    apply_pcm16_volume(muted, -1.0F);
    expect(
        muted == std::array<std::int16_t, 3>{0, 0, 0},
        "volume below zero clamps to mute");

    std::array<std::int16_t, 2> over{123, -456};
    apply_pcm16_volume(over, 2.0F);
    expect(
        over == std::array<std::int16_t, 2>{123, -456},
        "volume above one clamps to exact unity");
}

void test_listenbrainz_playback_tracker()
{
    using namespace cd404;
    using listenbrainz::SubmissionType;

    std::vector<listenbrainz::Submission> submissions;
    listenbrainz::PlaybackTracker tracker(
        [&](const auto& submission) { submissions.push_back(submission); });
    const listenbrainz::TrackMetadata short_track{
        1,
        L"Track One",
        L"Artist",
        L"Album",
        120 * core::kCdSampleFramesPerSecond,
    };

    tracker.begin(short_track, 0, 1'700'000'000);
    expect(
        submissions.size() == 1 &&
            submissions.front().type == SubmissionType::playing_now &&
            submissions.front().listened_at == 0,
        "ListenBrainz playing_now is emitted without a timestamp");
    tracker.update(
        short_track,
        59 * core::kCdSampleFramesPerSecond,
        1'700'000'059);
    tracker.end();
    expect(
        submissions.size() == 1,
        "less than half of a short track is not submitted as a listen");

    tracker.begin(short_track, 0, 1'700'001'000);
    tracker.update(
        short_track,
        60 * core::kCdSampleFramesPerSecond,
        1'700'001'060);
    expect(
        submissions.size() == 3 &&
            submissions.back().type == SubmissionType::single,
        "ListenBrainz single is emitted immediately at the playback threshold");
    tracker.end();
    expect(
        submissions.size() == 3 &&
            submissions.back().type == SubmissionType::single &&
            submissions.back().listened_at == 1'700'001'000 &&
            submissions.back().duration_played_seconds == 60,
        "half of a short track produces one timestamped single listen");

    const listenbrainz::TrackMetadata long_track{
        2,
        L"Long Track",
        L"Artist",
        L"Album",
        600 * core::kCdSampleFramesPerSecond,
    };
    tracker.begin(long_track, 0, 1'700'002'000);
    tracker.update(
        long_track,
        240 * core::kCdSampleFramesPerSecond,
        1'700'002'240);
    tracker.update(short_track, 0, 1'700'002'241);
    expect(
        submissions.size() == 6 &&
            submissions[submissions.size() - 2].type == SubmissionType::single &&
            submissions.back().type == SubmissionType::playing_now,
        "four minutes completes a long track before a track transition");
    tracker.end();

    listenbrainz::PlaybackTracker late_metadata_tracker(
        [&](const auto& submission) { submissions.push_back(submission); });
    auto unknown_track = short_track;
    unknown_track.artist.clear();
    const std::size_t before_late_metadata = submissions.size();
    late_metadata_tracker.begin(unknown_track, 0, 1'700'003'000);
    late_metadata_tracker.update(short_track, core::kCdSampleFramesPerSecond, 1'700'003'001);
    expect(
        submissions.size() == before_late_metadata + 1 &&
            submissions.back().type == SubmissionType::playing_now,
        "playing_now waits until required metadata becomes available");
    late_metadata_tracker.end();

    std::vector<listenbrainz::Submission> seek_submissions;
    listenbrainz::PlaybackTracker seek_tracker(
        [&](const auto& submission) { seek_submissions.push_back(submission); });
    seek_tracker.begin(short_track, 0, 1'700'004'000);
    seek_tracker.update(
        short_track,
        30 * core::kCdSampleFramesPerSecond,
        1'700'004'030);
    seek_tracker.seek(100 * core::kCdSampleFramesPerSecond);
    seek_tracker.update(
        short_track,
        110 * core::kCdSampleFramesPerSecond,
        1'700'004'040);
    seek_tracker.end();
    expect(
        seek_submissions.size() == 1,
        "ListenBrainz seeking forward does not count skipped frames as playback");
}

} // namespace

int main()
{
    test_cd_time_conversions();
    test_valid_toc();
    test_invalid_toc();
    test_cd_text_parsing();
    test_gnudb_identity_and_entry();
    test_continuous_cdda_stream();
    test_reliable_cdda_sector_source();
    test_playback_state_machine();
    test_cdda_pcm_conversion();
    test_pcm16_spsc_ring_buffer();
    test_pcm16_volume();
    test_listenbrainz_playback_tracker();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All CD.404 foundation tests passed.\n";
    return 0;
}
