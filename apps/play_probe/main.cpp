#include <cd404/audio/cdda_pcm.hpp>
#include <cd404/audio/continuous_cdda_stream.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/platform/windows/raw_cdda_sector_source.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options final {
    unsigned int track_number{};
    unsigned int seconds{15};
    bool show_help{};
    bool valid{true};
};

[[nodiscard]] bool parse_unsigned(
    const wchar_t* text,
    const unsigned long maximum,
    unsigned int& value) noexcept
{
    wchar_t* end{};
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (text == end || *end != L'\0' || parsed == 0 || parsed > maximum) {
        return false;
    }
    value = static_cast<unsigned int>(parsed);
    return true;
}

[[nodiscard]] Options parse_options(const int argument_count, wchar_t** arguments)
{
    Options options;
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
            continue;
        }

        if ((argument == L"--track" || argument == L"--seconds") &&
            index + 1 < argument_count) {
            unsigned int parsed{};
            const unsigned long maximum = argument == L"--track" ? 99UL : 600UL;
            if (!parse_unsigned(arguments[++index], maximum, parsed)) {
                options.valid = false;
                return options;
            }
            if (argument == L"--track") {
                options.track_number = parsed;
            } else {
                options.seconds = parsed;
            }
            continue;
        }

        options.valid = false;
        return options;
    }
    return options;
}

void print_usage()
{
    std::wcout << L"Usage: cd404_play_probe [--track N] [--seconds N]\n"
                  L"  --track N    Play this audio track; defaults to the first audio track.\n"
                  L"  --seconds N  Play at most N seconds (1-600); default is 15.\n";
}

void print_hresult(const wchar_t* operation, const std::int32_t status)
{
    std::wcout << operation << L" failed with HRESULT 0x" << std::hex
               << std::setw(8) << std::setfill(L'0')
               << static_cast<std::uint32_t>(status) << std::dec << std::setfill(L' ')
               << L".\n";
}

} // namespace

int wmain(const int argument_count, wchar_t** arguments)
{
    using namespace cd404;

    const Options options = parse_options(argument_count, arguments);
    if (!options.valid || options.show_help) {
        print_usage();
        return options.valid ? 0 : 2;
    }

    const auto drives = platform::windows::enumerate_optical_drives();
    const platform::windows::OpticalDrive* selected_drive{};
    std::optional<disc::Toc> selected_toc;
    for (const auto& drive : drives) {
        auto toc_result = platform::windows::read_toc(drive);
        if (toc_result.toc) {
            selected_drive = &drive;
            selected_toc = std::move(toc_result.toc);
            break;
        }
    }

    if (selected_drive == nullptr || !selected_toc) {
        std::wcout << L"No ready audio CD was found.\n";
        return 1;
    }

    const disc::Track* selected_track{};
    for (const auto& track : selected_toc->tracks()) {
        if (!track.is_audio) {
            continue;
        }
        if (options.track_number == 0 || track.number == options.track_number) {
            selected_track = &track;
            break;
        }
    }

    if (selected_track == nullptr) {
        std::wcout << L"The requested audio track was not found.\n";
        return 1;
    }

    auto source_result = platform::windows::open_raw_cdda_source(
        *selected_drive,
        selected_track->start_lba,
        selected_track->end_lba);
    if (!source_result.source) {
        std::wcout << L"Unable to open the CDDA source: "
                   << platform::windows::format_system_error(
                          source_result.system_error)
                   << L"\n";
        return 1;
    }

    audio::ContinuousCddaStream stream(
        *source_result.source,
        selected_track->start_lba,
        selected_track->end_lba);
    if (!stream.valid()) {
        std::wcout << L"Unable to create a continuous stream for the track.\n";
        return 1;
    }

    const auto requested_frames = static_cast<core::SampleFrame>(options.seconds) *
                                  core::kCdSampleFramesPerSecond;
    const core::SampleFrame frames_to_play =
        std::min(stream.total_frames(), requested_frames);
    if (frames_to_play <= 0 ||
        frames_to_play > static_cast<core::SampleFrame>(
                             std::numeric_limits<std::size_t>::max() / 2)) {
        std::wcout << L"The requested playback range is invalid.\n";
        return 1;
    }

    std::vector<std::int16_t> pcm_samples(
        static_cast<std::size_t>(frames_to_play) * 2);
    const auto pcm_bytes = std::as_writable_bytes(std::span(pcm_samples));

    std::wcout << L"Reading track "
               << static_cast<unsigned int>(selected_track->number) << L" ("
               << frames_to_play << L" frames) into the playback buffer...\n";
    const auto read_result = stream.read_frames(pcm_bytes);
    if (read_result.status != audio::ReadStatus::ok ||
        read_result.frames_read != static_cast<std::size_t>(frames_to_play)) {
        std::wcout << L"CDDA read failed after " << read_result.frames_read
                   << L" frame(s): "
                   << platform::windows::format_system_error(read_result.native_error)
                   << L"\n";
        return 1;
    }

    if (audio::convert_cdda_to_pcm16le_in_place(pcm_bytes) !=
        audio::CddaPcmConversionStatus::ok) {
        std::wcout << L"The CDDA buffer is not valid PCM16LE.\n";
        return 1;
    }

    platform::windows::WasapiOutput output;
    const std::int32_t open_status = output.open_default_shared();
    if (open_status < 0) {
        print_hresult(L"Opening the default audio endpoint", open_status);
        return 1;
    }

    std::wcout << L"Playing " << static_cast<double>(frames_to_play) /
                                      static_cast<double>(core::kCdSampleFramesPerSecond)
               << L" second(s) through the default shared-mode endpoint...\n";
    const auto write_result = output.write_interleaved(
        pcm_samples,
        static_cast<std::uint32_t>(frames_to_play));
    if (write_result.status < 0 ||
        write_result.frames_written != static_cast<std::uint32_t>(frames_to_play)) {
        print_hresult(L"Submitting PCM frames", write_result.status);
        static_cast<void>(output.stop());
        return 1;
    }

    const std::int32_t drain_status = output.drain();
    if (drain_status < 0) {
        print_hresult(L"Draining the audio endpoint", drain_status);
        static_cast<void>(output.stop());
        return 1;
    }

    static_cast<void>(output.stop());
    std::wcout << L"Playback completed.\n";
    return 0;
}
