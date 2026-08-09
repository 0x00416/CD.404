#pragma once

#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/metadata_store.hpp>
#include <cd404/platform/windows/online_metadata.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cd404::ui::detail {

struct UiTrack final {
    std::uint8_t number{};
    core::SampleFrame frame_count{};
    bool is_audio{};
    bool has_metadata_title{};
    std::wstring title;
    std::wstring artist;
    platform::windows::MetadataSource title_source{
        platform::windows::MetadataSource::unknown};
    platform::windows::MetadataSource artist_source{
        platform::windows::MetadataSource::unknown};
    std::wstring track_mbid;
    std::wstring recording_mbid;
    std::vector<std::wstring> artist_mbids;
};

struct DiscSnapshot final {
    std::optional<platform::windows::OpticalDrive> drive;
    std::optional<disc::Toc> toc;
    std::vector<UiTrack> tracks;
    core::SampleFrame total_audio_frames{};
    std::wstring album_title;
    std::wstring album_artist;
    platform::windows::MetadataSource album_title_source{
        platform::windows::MetadataSource::unknown};
    platform::windows::MetadataSource album_artist_source{
        platform::windows::MetadataSource::unknown};
    std::wstring release_mbid;
    std::wstring release_group_mbid;
    std::vector<std::wstring> metadata_sources;
    std::vector<platform::windows::MetadataReleaseCandidate> release_candidates;
    std::wstring selected_release_id;
    std::filesystem::path cover_art_path;
    std::wstring status;
    bool has_cd_text{};
    bool has_optical_drive{};
};

struct OnlineMetadataSnapshot final {
    std::optional<platform::windows::OnlineMetadata> metadata;
    std::uint64_t disc_generation{};
};

[[nodiscard]] DiscSnapshot load_disc_snapshot();

} // namespace cd404::ui::detail
