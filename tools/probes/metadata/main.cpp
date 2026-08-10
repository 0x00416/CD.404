#include <windows.h>

#include <winrt/base.h>

#include <cd404/platform/windows/gnudb_client.hpp>
#include <cd404/platform/windows/itunes_client.hpp>
#include <cd404/platform/windows/musicbrainz_client.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <iostream>
#include <cstdint>
#include <optional>

namespace {

[[nodiscard]] std::wstring to_wstring(const std::u16string_view value)
{
    return std::wstring(value.begin(), value.end());
}

} // namespace

int wmain()
{
    using namespace cd404;

    try {

    for (const auto& drive : platform::windows::enumerate_optical_drives()) {
        const auto toc_result = platform::windows::read_toc(drive);
        if (!toc_result.toc) {
            continue;
        }

        std::wcout << L"Looking up " << drive.root_path << L" ("
                   << toc_result.toc->tracks().size() << L" tracks)...\n";
        const auto cd_text = platform::windows::read_cd_text(drive);
        if (cd_text.metadata) {
            std::wcout << L"CD-TEXT: album=\""
                       << to_wstring(cd_text.metadata->album_title)
                       << L"\", artist=\""
                       << to_wstring(cd_text.metadata->album_performer)
                       << L"\"\n";
        } else {
            std::wcout << L"CD-TEXT: unavailable, system="
                       << cd_text.system_error << L"\n";
        }
        const auto musicbrainz = platform::windows::lookup_musicbrainz(*toc_result.toc);
        std::wcout << L"MusicBrainz: "
                   << (musicbrainz.metadata ? L"match" : L"no match")
                   << L", HTTP=" << musicbrainz.http_status
                   << L", system=" << musicbrainz.system_error << L"\n";

        const auto gnudb = platform::windows::lookup_gnudb(*toc_result.toc);
        std::wcout << L"GnuDB: " << (gnudb.metadata ? L"match" : L"no match")
                   << L", HTTP=" << gnudb.http_status
                   << L", protocol=" << gnudb.protocol_status
                   << L", system=" << gnudb.system_error;
        if (!gnudb.message.empty()) {
            std::wcout << L", message=\"" << gnudb.message << L"\"";
        }
        std::wcout << L"\n";

        std::optional<platform::windows::MusicBrainzMetadata> content_metadata;
        if (!musicbrainz.metadata && gnudb.metadata) {
            platform::windows::MusicBrainzContentQuery query;
            query.album_title = gnudb.metadata->album_title;
            query.album_artist = gnudb.metadata->album_artist;
            query.year = gnudb.metadata->year;
            query.track_titles = gnudb.metadata->track_titles;
            for (const auto& track : toc_result.toc->tracks()) {
                query.track_lengths_milliseconds.push_back(
                    static_cast<std::uint64_t>(
                        track.frame_count * 1'000 /
                        core::kCdSampleFramesPerSecond));
            }
            const auto content =
                platform::windows::lookup_musicbrainz_by_content(query);
            content_metadata = content.metadata;
            std::wcout << L"MusicBrainz content: "
                       << (content.metadata ? L"match" : L"no match")
                       << L", HTTP=" << content.http_status
                       << L", system=" << content.system_error << L"\n";
            if (content.metadata) {
                std::wcout << L"Reference release: "
                           << content.metadata->release_id << L"\n"
                           << L"Reference release group: "
                           << content.metadata->release_group_id << L"\n"
                           << L"Cover: " << content.metadata->cover_art_path.wstring()
                           << L"\n";
                for (std::size_t index = 0U;
                     index < content.metadata->track_titles.size();
                     ++index) {
                    std::wcout << index + 1U << L". "
                               << content.metadata->track_titles[index] << L" -> "
                               << content.metadata->recording_ids[index] << L"\n";
                }
            }
        }

        std::wstring album_title;
        std::wstring album_artist;
        if (musicbrainz.metadata) {
            album_title = musicbrainz.metadata->album_title;
            album_artist = musicbrainz.metadata->album_artist;
        } else if (gnudb.metadata) {
            album_title = gnudb.metadata->album_title;
            album_artist = gnudb.metadata->album_artist;
        }
        if (!album_title.empty()) {
            const auto itunes = platform::windows::lookup_itunes(
                *toc_result.toc,
                album_title,
                album_artist);
            std::wcout << L"iTunes: " << (itunes.metadata ? L"match" : L"no match")
                       << L", HTTP=" << itunes.http_status
                       << L", system=" << itunes.system_error << L"\n";
        } else {
            std::wcout << L"iTunes: skipped (no trusted album seed)\n";
        }

        const auto* metadata = musicbrainz.metadata
            ? &*musicbrainz.metadata
            : nullptr;
        if (metadata != nullptr) {
            std::wcout << L"Album: " << metadata->album_title
                       << L"\nArtist: " << metadata->album_artist << L"\n";
        } else if (gnudb.metadata) {
            std::wcout << L"Album: " << gnudb.metadata->album_title
                       << L"\nArtist: " << gnudb.metadata->album_artist << L"\n";
        }
        return musicbrainz.metadata || content_metadata || gnudb.metadata ? 0 : 1;
    }

    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"WinRT error: 0x" << std::hex
                   << static_cast<unsigned long>(error.code().value) << L" "
                   << error.message().c_str() << L"\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Exception: " << error.what() << "\n";
        return 2;
    }

    std::wcout << L"No ready audio CD was found.\n";
    return 1;
}
