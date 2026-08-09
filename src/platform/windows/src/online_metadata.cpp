#include <cd404/platform/windows/gnudb_client.hpp>
#include <cd404/platform/windows/itunes_client.hpp>
#include <cd404/platform/windows/musicbrainz_client.hpp>
#include <cd404/platform/windows/online_metadata.hpp>

#include <algorithm>
#include <future>
#include <utility>

namespace cd404::platform::windows {
namespace {

void merge_value(std::wstring& destination, const std::wstring& source)
{
    if (destination.empty() && !source.empty()) {
        destination = source;
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
    const std::wstring_view seed_album_artist)
{
    const std::size_t expected_tracks = toc.tracks().size();
    if (expected_tracks == 0U) {
        return std::nullopt;
    }

    auto musicbrainz_future = std::async(std::launch::async, [&toc] {
        return lookup_musicbrainz(toc);
    });
    auto gnudb_future = std::async(std::launch::async, [&toc] {
        return lookup_gnudb(toc);
    });

    OnlineMetadata merged;
    const MusicBrainzLookupResult musicbrainz = musicbrainz_future.get();
    if (musicbrainz.metadata) {
        merge_text_metadata(merged, *musicbrainz.metadata, expected_tracks);
        merged.release_mbid = musicbrainz.metadata->release_id;
        merged.release_group_mbid = musicbrainz.metadata->release_group_id;
        merged.track_mbids = musicbrainz.metadata->track_ids;
        merged.recording_mbids = musicbrainz.metadata->recording_ids;
        merged.track_artist_mbids = musicbrainz.metadata->track_artist_ids;
        merged.cover_art_path = musicbrainz.metadata->cover_art_path;
        merged.sources.emplace_back(L"MusicBrainz");
    }

    const GnudbLookupResult gnudb = gnudb_future.get();
    if (gnudb.metadata) {
        merge_text_metadata(merged, *gnudb.metadata, expected_tracks);
        merged.sources.emplace_back(L"GnuDB");
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
            merged.sources.emplace_back(L"iTunes");
        }
    }

    return merged.sources.empty()
        ? std::nullopt
        : std::optional(std::move(merged));
}

} // namespace cd404::platform::windows
