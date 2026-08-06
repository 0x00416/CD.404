#include <cd404/audio/continuous_cdda_stream.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/platform/windows/raw_cdda_sector_source.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

[[nodiscard]] std::uint64_t fnv1a64(const std::span<const std::byte> bytes) noexcept
{
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const std::byte value : bytes) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

} // namespace

int wmain()
{
    using namespace cd404;

    const auto drives = platform::windows::enumerate_optical_drives();
    if (drives.empty()) {
        std::wcout << L"No optical drives were detected.\n";
        return 0;
    }

    for (const auto& drive : drives) {
        std::wcout << L"Inspecting " << drive.root_path << L"\n";
        const auto toc_result = platform::windows::read_toc(drive);
        if (!toc_result.toc) {
            std::wcout << L"  TOC unavailable: "
                       << platform::windows::format_system_error(toc_result.system_error)
                       << L"\n";
            continue;
        }

        const auto audio_track = std::find_if(
            toc_result.toc->tracks().begin(),
            toc_result.toc->tracks().end(),
            [](const disc::Track& track) { return track.is_audio; });
        if (audio_track == toc_result.toc->tracks().end()) {
            std::wcout << L"  No audio tracks found.\n";
            continue;
        }

        auto audio_run_end = audio_track;
        for (auto next_track = std::next(audio_track);
             next_track != toc_result.toc->tracks().end() && next_track->is_audio &&
             next_track->start_lba == audio_run_end->end_lba;
             ++next_track) {
            audio_run_end = next_track;
        }

        auto open_result = platform::windows::open_raw_cdda_source(
            drive,
            audio_track->start_lba,
            audio_run_end->end_lba);
        if (!open_result.source) {
            std::wcout << L"  Unable to open raw CDDA source: "
                       << platform::windows::format_system_error(
                              open_result.system_error)
                       << L"\n";
            continue;
        }

        constexpr std::size_t kProbeSectors = 16;
        const auto track_sectors = static_cast<std::size_t>(
            audio_track->end_lba - audio_track->start_lba);
        const std::size_t sectors_to_read = std::min(kProbeSectors, track_sectors);
        std::vector<std::byte> buffer(
            sectors_to_read *
            static_cast<std::size_t>(core::kCdBytesPerSector));
        const auto read_result = open_result.source->read_sectors(
            audio_track->start_lba,
            buffer);
        if (read_result.status != audio::ReadStatus::ok) {
            std::wcout << L"  Raw CDDA read failed: "
                       << platform::windows::format_system_error(
                              read_result.native_error)
                       << L"\n";
            continue;
        }

        std::wcout << L"  Track " << static_cast<unsigned int>(audio_track->number)
                   << L": read " << read_result.sectors_read << L" sector(s), "
                   << buffer.size() << L" bytes, FNV-1a-64=0x" << std::hex
                   << std::setw(16) << std::setfill(L'0') << fnv1a64(buffer)
                   << std::dec << std::setfill(L' ') << L"\n";

        const auto next_audio_track = std::next(audio_track);
        if (next_audio_track != toc_result.toc->tracks().end() &&
            next_audio_track->is_audio &&
            next_audio_track->start_lba == audio_track->end_lba) {
            audio::ContinuousCddaStream stream(
                *open_result.source,
                audio_track->start_lba,
                audio_run_end->end_lba);
            const core::SampleFrame boundary_frame =
                (next_audio_track->start_lba - audio_track->start_lba) *
                core::kCdSampleFramesPerSector;
            std::array<std::byte, 3 * 4> boundary_buffer{};
            if (stream.seek(boundary_frame - 1)) {
                const auto boundary_result = stream.read_frames(boundary_buffer);
                if (boundary_result.status == audio::ReadStatus::ok &&
                    boundary_result.frames_read == 3) {
                    std::wcout << L"  Continuous stream read three frames across tracks "
                               << static_cast<unsigned int>(audio_track->number) << L" -> "
                               << static_cast<unsigned int>(next_audio_track->number)
                               << L" without restarting the source.\n";
                } else {
                    std::wcout << L"  Cross-track continuous read failed: "
                               << platform::windows::format_system_error(
                                      boundary_result.native_error)
                               << L"\n";
                }
            }
        }
    }

    return 0;
}
