#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace cd404::disc {

constexpr std::uint8_t kCdTextAlbumNamePack = 0x80;
constexpr std::uint8_t kCdTextPerformerPack = 0x81;

struct CdTextPack final {
    std::uint8_t type{};
    std::uint8_t track_number{};
    std::uint8_t sequence_number{};
    std::uint8_t character_position{};
    std::uint8_t block_number{};
    bool unicode{};
    bool extension{};
    std::array<std::uint8_t, 12> payload{};
};

struct CdTextTrackMetadata final {
    std::u16string title;
    std::u16string performer;
};

struct CdTextMetadata final {
    std::u16string album_title;
    std::u16string album_performer;
    std::array<CdTextTrackMetadata, 100> tracks{};

    [[nodiscard]] bool empty() const noexcept;
};

[[nodiscard]] CdTextMetadata parse_cd_text(std::span<const CdTextPack> packs);

} // namespace cd404::disc
