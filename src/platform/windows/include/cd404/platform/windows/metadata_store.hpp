#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

enum class MetadataSource {
    unknown,
    itunes,
    gnudb,
    musicbrainz,
    cd_text,
    user,
};

struct SourcedMetadataValue final {
    std::wstring value;
    MetadataSource source{MetadataSource::unknown};
};

struct EditableTrackMetadata final {
    SourcedMetadataValue title;
    SourcedMetadataValue artist;
};

struct EditableDiscMetadata final {
    SourcedMetadataValue album_title;
    SourcedMetadataValue album_artist;
    std::vector<EditableTrackMetadata> tracks;
};

struct MetadataReleaseCandidate final {
    std::wstring release_id;
    std::wstring album_title;
    std::wstring album_artist;
};

struct MetadataCacheEntry final {
    std::wstring disc_key;
    std::wstring selected_release_id;
    std::int64_t updated_unix_seconds{};
    EditableDiscMetadata metadata;
};

[[nodiscard]] const wchar_t* to_string(MetadataSource source) noexcept;
[[nodiscard]] bool merge_metadata_value(
    SourcedMetadataValue& destination,
    std::wstring_view value,
    MetadataSource source);
void revise_metadata_value(
    SourcedMetadataValue& destination,
    std::wstring_view value);
[[nodiscard]] std::size_t select_metadata_candidate(
    const std::vector<MetadataReleaseCandidate>& candidates,
    std::wstring_view preferred_release_id) noexcept;

[[nodiscard]] std::wstring encode_metadata_cache(
    const MetadataCacheEntry& entry);
[[nodiscard]] std::optional<MetadataCacheEntry> decode_metadata_cache(
    std::wstring_view json) noexcept;
[[nodiscard]] bool metadata_cache_is_fresh(
    const MetadataCacheEntry& entry,
    std::int64_t now_unix_seconds,
    std::int64_t maximum_age_seconds) noexcept;
[[nodiscard]] std::optional<MetadataCacheEntry> load_metadata_cache(
    std::wstring_view disc_key) noexcept;
[[nodiscard]] bool save_metadata_cache(
    const MetadataCacheEntry& entry) noexcept;

} // namespace cd404::platform::windows
