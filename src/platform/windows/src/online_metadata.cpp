#include <cd404/platform/windows/gnudb_client.hpp>
#include <cd404/platform/windows/itunes_client.hpp>
#include <cd404/platform/windows/musicbrainz_client.hpp>
#include <cd404/platform/windows/online_metadata.hpp>
#include <cd404/platform/windows/user_settings.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <utility>

namespace cd404::platform::windows {
namespace {

void merge_value(std::wstring& destination, const std::wstring& source)
{
    if (destination.empty() && !source.empty()) {
        destination = source;
    }
}

void merge_editable(
    EditableDiscMetadata& destination,
    const std::wstring& album_title,
    const std::wstring& album_artist,
    const std::vector<std::wstring>& track_titles,
    const std::vector<std::wstring>& track_artists,
    const MetadataSource source,
    const std::size_t expected_tracks)
{
    static_cast<void>(merge_metadata_value(
        destination.album_title,
        album_title,
        source));
    static_cast<void>(merge_metadata_value(
        destination.album_artist,
        album_artist,
        source));
    destination.tracks.resize(expected_tracks);
    for (std::size_t index = 0;
         index < std::min(expected_tracks, track_titles.size());
         ++index) {
        static_cast<void>(merge_metadata_value(
            destination.tracks[index].title,
            track_titles[index],
            source));
    }
    for (std::size_t index = 0;
         index < std::min(expected_tracks, track_artists.size());
         ++index) {
        static_cast<void>(merge_metadata_value(
            destination.tracks[index].artist,
            track_artists[index],
            source));
    }
}

void apply_user_revisions(
    EditableDiscMetadata& destination,
    const EditableDiscMetadata& cached)
{
    if (cached.album_title.source == MetadataSource::user) {
        revise_metadata_value(destination.album_title, cached.album_title.value);
    }
    if (cached.album_artist.source == MetadataSource::user) {
        revise_metadata_value(destination.album_artist, cached.album_artist.value);
    }
    destination.tracks.resize(std::max(
        destination.tracks.size(),
        cached.tracks.size()));
    for (std::size_t index = 0; index < cached.tracks.size(); ++index) {
        if (cached.tracks[index].title.source == MetadataSource::user) {
            revise_metadata_value(
                destination.tracks[index].title,
                cached.tracks[index].title.value);
        }
        if (cached.tracks[index].artist.source == MetadataSource::user) {
            revise_metadata_value(
                destination.tracks[index].artist,
                cached.tracks[index].artist.value);
        }
    }
}

void project_editable(OnlineMetadata& metadata)
{
    metadata.album_title = metadata.editable.album_title.value;
    metadata.album_artist = metadata.editable.album_artist.value;
    metadata.track_titles.clear();
    metadata.track_artists.clear();
    for (const auto& track : metadata.editable.tracks) {
        metadata.track_titles.push_back(track.title.value);
        metadata.track_artists.push_back(track.artist.value);
    }
}

void merge_values(
    std::vector<std::wstring>& destination,
    const std::vector<std::wstring>& source,
    const std::size_t expected_size)
{
    destination.resize(expected_size);
    const std::size_t count = std::min(expected_size, source.size());
    for (std::size_t index = 0; index < count; ++index) {
        merge_value(destination[index], source[index]);
    }
}

template <typename Metadata>
void merge_text_metadata(
    OnlineMetadata& destination,
    const Metadata& source,
    const std::size_t expected_tracks)
{
    merge_value(destination.album_title, source.album_title);
    merge_value(destination.album_artist, source.album_artist);
    merge_values(destination.track_titles, source.track_titles, expected_tracks);
    merge_values(destination.track_artists, source.track_artists, expected_tracks);
}

} // namespace

std::optional<OnlineMetadata> lookup_online_metadata(
    const disc::Toc& toc,
    const std::wstring_view seed_album_title,
    const std::wstring_view seed_album_artist,
    const std::wstring_view preferred_release_id,
    const OnlineMetadataOptions& options)
{
    const std::size_t expected_tracks = toc.tracks().size();
    if (expected_tracks == 0U) {
        return std::nullopt;
    }

    const std::wstring disc_key = make_disc_settings_key(toc);
    const auto cached = load_metadata_cache(disc_key);
    const std::wstring effective_release = !preferred_release_id.empty()
        ? std::wstring(preferred_release_id)
        : (cached ? cached->selected_release_id : std::wstring{});
    auto musicbrainz_future = std::async(std::launch::async, [&toc, effective_release] {
        return lookup_musicbrainz(toc, effective_release);
    });
    std::optional<std::future<GnudbLookupResult>> gnudb_future;
    if (options.cddb_enabled) {
        gnudb_future.emplace(std::async(
            std::launch::async,
            [&toc, &options] {
                return lookup_gnudb(
                    toc,
                    CddbClientConfiguration{
                        options.cddb_server,
                        options.cddb_email,
                    });
            }));
    }

    OnlineMetadata merged;
    const MusicBrainzLookupResult musicbrainz = musicbrainz_future.get();
    if (musicbrainz.metadata) {
        merge_text_metadata(merged, *musicbrainz.metadata, expected_tracks);
        merge_editable(
            merged.editable,
            musicbrainz.metadata->album_title,
            musicbrainz.metadata->album_artist,
            musicbrainz.metadata->track_titles,
            musicbrainz.metadata->track_artists,
            MetadataSource::musicbrainz,
            expected_tracks);
        merged.release_mbid = musicbrainz.metadata->release_id;
        merged.release_group_mbid = musicbrainz.metadata->release_group_id;
        merged.track_mbids = musicbrainz.metadata->track_ids;
        merged.recording_mbids = musicbrainz.metadata->recording_ids;
        merged.track_artist_mbids = musicbrainz.metadata->track_artist_ids;
        merged.cover_art_path = musicbrainz.metadata->cover_art_path;
        if (!merged.cover_art_path.empty()) {
            merged.cover_art_source = L"Cover Art Archive";
        }
        merged.sources.emplace_back(L"MusicBrainz");
    }
    merged.used_fuzzy_musicbrainz_fallback = musicbrainz.used_fuzzy_fallback;
    for (const auto& candidate : musicbrainz.candidates) {
        merged.release_candidates.push_back({
            candidate.release_id,
            candidate.album_title,
            candidate.album_artist,
        });
    }
    merged.selected_release_id = musicbrainz.metadata
        ? musicbrainz.metadata->release_id
        : effective_release;

    std::optional<GnudbLookupResult> gnudb;
    if (gnudb_future) {
        gnudb = gnudb_future->get();
        if (gnudb->metadata) {
            merge_text_metadata(merged, *gnudb->metadata, expected_tracks);
            merge_editable(
                merged.editable,
                gnudb->metadata->album_title,
                gnudb->metadata->album_artist,
                gnudb->metadata->track_titles,
                gnudb->metadata->track_artists,
                MetadataSource::gnudb,
                expected_tracks);
            merged.cddb_category = gnudb->metadata->category;
            merged.cddb_year = gnudb->metadata->year;
            merged.cddb_revision = gnudb->metadata->revision;
            merged.sources.emplace_back(L"CDDB/freedb");
        }
    }

    if (!musicbrainz.metadata && gnudb && gnudb->metadata &&
        gnudb->metadata->track_titles.size() == expected_tracks) {
        MusicBrainzContentQuery query;
        query.album_title = gnudb->metadata->album_title;
        query.album_artist = gnudb->metadata->album_artist;
        query.year = gnudb->metadata->year;
        query.track_titles = gnudb->metadata->track_titles;
        query.track_lengths_milliseconds.reserve(expected_tracks);
        for (const auto& track : toc.tracks()) {
            query.track_lengths_milliseconds.push_back(
                static_cast<std::uint64_t>(
                    track.frame_count * 1'000 /
                    core::kCdSampleFramesPerSecond));
        }
        const MusicBrainzLookupResult content =
            lookup_musicbrainz_by_content(query);
        if (content.metadata) {
            merged.reference_release_mbid = content.metadata->release_id;
            merged.reference_release_group_mbid =
                content.metadata->release_group_id;
            merged.recording_mbids = content.metadata->recording_ids;
            merged.track_artist_mbids = content.metadata->track_artist_ids;
            if (merged.cover_art_path.empty()) {
                merged.cover_art_path = content.metadata->cover_art_path;
                if (!merged.cover_art_path.empty()) {
                    merged.cover_art_source =
                        L"MusicBrainz → Cover Art Archive";
                }
            }
            merged.used_musicbrainz_content_match = true;
            merged.sources.emplace_back(L"MusicBrainz · 内容关联");
        }
    }
    if (merged.cover_art_path.empty() && gnudb && gnudb->metadata) {
        for (const auto& art_id : gnudb->metadata->cover_art_ids) {
            merged.cover_art_path = download_musicbrainz_cover_art(art_id);
            if (!merged.cover_art_path.empty()) {
                merged.cover_art_source = L"GnuDB → Cover Art Archive";
                break;
            }
        }
    }

    const std::wstring_view itunes_title = merged.album_title.empty()
        ? seed_album_title
        : std::wstring_view(merged.album_title);
    const std::wstring_view itunes_artist = merged.album_artist.empty()
        ? seed_album_artist
        : std::wstring_view(merged.album_artist);
    if (!itunes_title.empty()) {
        const ItunesLookupResult itunes = lookup_itunes(
            toc,
            itunes_title,
            itunes_artist);
        if (itunes.metadata) {
            merge_text_metadata(merged, *itunes.metadata, expected_tracks);
            merge_editable(
                merged.editable,
                itunes.metadata->album_title,
                itunes.metadata->album_artist,
                itunes.metadata->track_titles,
                itunes.metadata->track_artists,
                MetadataSource::itunes,
                expected_tracks);
            merged.sources.emplace_back(L"iTunes");
        }
    }
    if (cached) {
        apply_user_revisions(merged.editable, cached->metadata);
    }
    if (merged.sources.empty()) {
        if (!cached) {
            return std::nullopt;
        }
        merged.editable = cached->metadata;
        merged.selected_release_id = cached->selected_release_id;
        merged.sources.emplace_back(L"Local");
    }
    project_editable(merged);
    const auto now = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    static_cast<void>(save_metadata_cache(MetadataCacheEntry{
        disc_key,
        merged.selected_release_id,
        now,
        merged.editable,
    }));
    return merged;
}

} // namespace cd404::platform::windows
