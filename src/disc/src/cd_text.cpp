#include <cd404/disc/cd_text.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace cd404::disc {
namespace {

void assign_text(
    CdTextMetadata& metadata,
    const std::uint8_t type,
    const std::uint8_t track_number,
    std::u16string text)
{
    if (text.size() == 1 && text.front() == u'\t' && track_number > 0) {
        const auto& previous = metadata.tracks[track_number - 1];
        text = type == kCdTextAlbumNamePack ? previous.title : previous.performer;
    }

    if (track_number == 0) {
        if (type == kCdTextAlbumNamePack) {
            metadata.album_title = std::move(text);
        } else {
            metadata.album_performer = std::move(text);
        }
        return;
    }

    auto& track = metadata.tracks[track_number];
    if (type == kCdTextAlbumNamePack) {
        track.title = std::move(text);
    } else {
        track.performer = std::move(text);
    }
}

void parse_text_type(
    CdTextMetadata& metadata,
    const std::span<const CdTextPack> packs,
    const std::uint8_t type)
{
    std::vector<const CdTextPack*> selected;
    for (const auto& pack : packs) {
        if (pack.type == type && pack.block_number == 0 && !pack.extension) {
            selected.push_back(&pack);
        }
    }
    std::ranges::sort(selected, {}, &CdTextPack::sequence_number);

    std::u16string text;
    std::uint8_t current_track{};
    bool have_track{};
    for (const CdTextPack* pack : selected) {
        if (!have_track || pack->track_number != current_track) {
            if (have_track && !text.empty()) {
                assign_text(metadata, type, current_track, std::move(text));
                text.clear();
            }
            current_track = pack->track_number;
            have_track = true;
        }

        const std::size_t unit_size = pack->unicode ? 2U : 1U;
        for (std::size_t index = 0; index + unit_size <= pack->payload.size();
             index += unit_size) {
            char16_t character{};
            if (pack->unicode) {
                character = static_cast<char16_t>(
                    (static_cast<std::uint16_t>(pack->payload[index]) << 8U) |
                    pack->payload[index + 1]);
            } else {
                character = static_cast<char16_t>(pack->payload[index]);
            }

            if (character != u'\0') {
                text.push_back(character);
                continue;
            }

            if (!text.empty() && current_track < metadata.tracks.size()) {
                assign_text(metadata, type, current_track, std::move(text));
                text.clear();
            }
            if (current_track < 99) {
                ++current_track;
            }
        }
    }

    if (have_track && !text.empty() && current_track < metadata.tracks.size()) {
        assign_text(metadata, type, current_track, std::move(text));
    }
}

} // namespace

bool CdTextMetadata::empty() const noexcept
{
    if (!album_title.empty() || !album_performer.empty()) {
        return false;
    }
    return std::ranges::all_of(tracks, [](const CdTextTrackMetadata& track) {
        return track.title.empty() && track.performer.empty();
    });
}

CdTextMetadata parse_cd_text(const std::span<const CdTextPack> packs)
{
    CdTextMetadata metadata;
    parse_text_type(metadata, packs, kCdTextAlbumNamePack);
    parse_text_type(metadata, packs, kCdTextPerformerPack);
    return metadata;
}

} // namespace cd404::disc
