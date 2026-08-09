#include <cd404/disc/musicbrainz_disc_id.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace cd404::disc {
namespace {

[[nodiscard]] std::array<std::uint8_t, 20> sha1(const std::string& input)
{
    std::vector<std::uint8_t> message(input.begin(), input.end());
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while (message.size() % 64U != 56U) {
        message.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    }

    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    for (std::size_t block = 0; block < message.size(); block += 64U) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = block + index * 4U;
            words[index] =
                (static_cast<std::uint32_t>(message[offset]) << 24U) |
                (static_cast<std::uint32_t>(message[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(message[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(message[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            words[index] = std::rotl(
                words[index - 3U] ^ words[index - 8U] ^
                    words[index - 14U] ^ words[index - 16U],
                1);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;
        for (std::size_t index = 0; index < words.size(); ++index) {
            std::uint32_t function{};
            std::uint32_t constant{};
            if (index < 20U) {
                function = (b & c) | ((~b) & d);
                constant = 0x5A827999U;
            } else if (index < 40U) {
                function = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            } else if (index < 60U) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCU;
            } else {
                function = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }
            const std::uint32_t temporary =
                std::rotl(a, 5) + function + e + constant + words[index];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temporary;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    const std::array values{h0, h1, h2, h3, h4};
    for (std::size_t word = 0; word < values.size(); ++word) {
        for (std::size_t byte = 0; byte < 4U; ++byte) {
            digest[word * 4U + byte] = static_cast<std::uint8_t>(
                values[word] >> (24U - static_cast<unsigned int>(byte * 8U)));
        }
    }
    return digest;
}

[[nodiscard]] std::string musicbrainz_base64(
    const std::array<std::uint8_t, 20>& bytes)
{
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    std::string result;
    result.reserve(28U);
    for (std::size_t index = 0; index < bytes.size(); index += 3U) {
        const std::size_t remaining = bytes.size() - index;
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (remaining > 1U
                 ? static_cast<std::uint32_t>(bytes[index + 1U]) << 8U
                 : 0U) |
            (remaining > 2U
                 ? static_cast<std::uint32_t>(bytes[index + 2U])
                 : 0U);
        result.push_back(alphabet[(value >> 18U) & 0x3fU]);
        result.push_back(alphabet[(value >> 12U) & 0x3fU]);
        result.push_back(remaining > 1U ? alphabet[(value >> 6U) & 0x3fU] : '-');
        result.push_back(remaining > 2U ? alphabet[value & 0x3fU] : '-');
    }
    return result;
}

} // namespace

std::optional<MusicBrainzDiscIdentity> make_musicbrainz_disc_identity(
    const Toc& toc)
{
    const auto& tracks = toc.tracks();
    if (tracks.empty() || tracks.size() > 99U) {
        return std::nullopt;
    }
    for (const auto& track : tracks) {
        if (!track.is_audio) {
            return std::nullopt;
        }
    }

    constexpr std::int64_t lead_in_sectors = 150;
    std::string hash_input = std::format(
        "{:02X}{:02X}{:08X}",
        tracks.front().number,
        tracks.back().number,
        static_cast<std::uint64_t>(toc.lead_out_lba() + lead_in_sectors));
    std::string toc_parameter = std::format(
        "{} {} {}",
        tracks.front().number,
        tracks.back().number,
        toc.lead_out_lba() + lead_in_sectors);
    for (std::size_t slot = 0; slot < 99U; ++slot) {
        const std::int64_t offset = slot < tracks.size()
            ? tracks[slot].start_lba + lead_in_sectors
            : 0;
        hash_input += std::format("{:08X}", static_cast<std::uint64_t>(offset));
        if (slot < tracks.size()) {
            toc_parameter += std::format(" {}", offset);
        }
    }

    return MusicBrainzDiscIdentity{
        musicbrainz_base64(sha1(hash_input)),
        std::move(toc_parameter),
    };
}

} // namespace cd404::disc
