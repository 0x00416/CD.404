#include "disc_snapshot.hpp"

#include <cd404/platform/windows/user_settings.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <string_view>

namespace cd404::ui::detail {
namespace {

[[nodiscard]] std::wstring to_wstring(const std::u16string_view text)
{
    std::wstring result;
    result.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(result), [](const char16_t character) {
        return static_cast<wchar_t>(character);
    });
    return result;
}

} // namespace

DiscSnapshot load_disc_snapshot()
{
    DiscSnapshot snapshot;
    const auto drives = platform::windows::enumerate_optical_drives();
    snapshot.has_optical_drive = !drives.empty();
    if (drives.empty()) {
        snapshot.status = L"未检测到光驱";
        return snapshot;
    }

    unsigned long last_error{};
    for (const auto& drive : drives) {
        auto toc_result = platform::windows::read_toc(drive);
        if (!toc_result.toc) {
            last_error = toc_result.system_error;
            continue;
        }

        snapshot.drive = drive;
        snapshot.toc = *toc_result.toc;
        for (const auto& track : toc_result.toc->tracks()) {
            UiTrack view;
            view.number = track.number;
            view.frame_count = track.frame_count;
            view.is_audio = track.is_audio;
            view.title = track.is_audio
                ? std::format(L"音轨 {:02}", track.number)
                : std::format(L"数据轨 {:02}", track.number);
            if (track.is_audio) {
                snapshot.total_audio_frames += track.frame_count;
            }
            snapshot.tracks.push_back(std::move(view));
        }

        const auto cd_text_result = platform::windows::read_cd_text(drive);
        if (cd_text_result.metadata) {
            snapshot.has_cd_text = true;
            snapshot.metadata_source = L"CD-TEXT";
            snapshot.album_title = to_wstring(cd_text_result.metadata->album_title);
            snapshot.album_artist = to_wstring(cd_text_result.metadata->album_performer);
            if (!snapshot.album_title.empty()) {
                snapshot.album_title_source = platform::windows::MetadataSource::cd_text;
            }
            if (!snapshot.album_artist.empty()) {
                snapshot.album_artist_source = platform::windows::MetadataSource::cd_text;
            }
            for (auto& track : snapshot.tracks) {
                const auto& metadata = cd_text_result.metadata->tracks[track.number];
                if (!metadata.title.empty()) {
                    track.title = to_wstring(metadata.title);
                    track.has_metadata_title = true;
                    track.title_source = platform::windows::MetadataSource::cd_text;
                }
                if (!metadata.performer.empty()) {
                    track.artist = to_wstring(metadata.performer);
                    track.artist_source = platform::windows::MetadataSource::cd_text;
                }
            }
        }
        if (const auto cached = platform::windows::load_metadata_cache(
                platform::windows::make_disc_settings_key(*snapshot.toc))) {
            const auto merge_cached = [](
                                          std::wstring& value,
                                          platform::windows::MetadataSource& source,
                                          const platform::windows::SourcedMetadataValue& incoming) {
                platform::windows::SourcedMetadataValue destination{value, source};
                if (platform::windows::merge_metadata_value(
                        destination,
                        incoming.value,
                        incoming.source)) {
                    value = std::move(destination.value);
                    source = destination.source;
                }
            };
            merge_cached(snapshot.album_title, snapshot.album_title_source,
                cached->metadata.album_title);
            merge_cached(snapshot.album_artist, snapshot.album_artist_source,
                cached->metadata.album_artist);
            for (std::size_t index = 0;
                 index < std::min(snapshot.tracks.size(), cached->metadata.tracks.size());
                 ++index) {
                merge_cached(snapshot.tracks[index].title,
                    snapshot.tracks[index].title_source,
                    cached->metadata.tracks[index].title);
                merge_cached(snapshot.tracks[index].artist,
                    snapshot.tracks[index].artist_source,
                    cached->metadata.tracks[index].artist);
                snapshot.tracks[index].has_metadata_title =
                    snapshot.tracks[index].title_source !=
                    platform::windows::MetadataSource::unknown;
            }
            snapshot.selected_release_id = cached->selected_release_id;
            snapshot.metadata_source = L"本地元数据缓存";
        }
        snapshot.status = std::format(
            L"已就绪 · {} 首音频轨",
            std::count_if(snapshot.tracks.begin(), snapshot.tracks.end(),
                [](const UiTrack& track) { return track.is_audio; }));
        return snapshot;
    }

    snapshot.drive = drives.front();
    snapshot.status = last_error == 0
        ? L"请插入一张音频 CD"
        : L"光驱已连接 · 等待音频 CD";
    return snapshot;
}

} // namespace cd404::ui::detail
