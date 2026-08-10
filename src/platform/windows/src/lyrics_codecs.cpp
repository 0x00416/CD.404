#include <windows.h>
#include <wincrypt.h>

#include <cd404/platform/windows/lyrics_codecs.hpp>

#include "http_client.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace cd404::platform::windows::detail {
namespace {

class BitReader final {
public:
    explicit BitReader(const std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] bool read(const unsigned int count, std::uint32_t& value) noexcept
    {
        if (count > 24U) {
            return false;
        }
        while (bits_ < count) {
            if (position_ >= data_.size()) {
                return false;
            }
            buffer_ |= static_cast<std::uint64_t>(data_[position_++]) << bits_;
            bits_ += 8U;
        }
        value = static_cast<std::uint32_t>(
            buffer_ & ((std::uint64_t{1} << count) - 1U));
        buffer_ >>= count;
        bits_ -= count;
        return true;
    }

    void align_byte() noexcept
    {
        const unsigned int discard = bits_ % 8U;
        buffer_ >>= discard;
        bits_ -= discard;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t position_{};
    std::uint64_t buffer_{};
    unsigned int bits_{};
};

[[nodiscard]] std::uint32_t reverse_bits(
    std::uint32_t value,
    const unsigned int count) noexcept
{
    std::uint32_t result{};
    for (unsigned int index{}; index < count; ++index) {
        result = (result << 1U) | (value & 1U);
        value >>= 1U;
    }
    return result;
}

class HuffmanTree final {
public:
    [[nodiscard]] bool build(const std::span<const std::uint8_t> lengths)
    {
        constexpr unsigned int maximum_bits = 15U;
        std::array<std::uint32_t, maximum_bits + 1U> counts{};
        for (const std::uint8_t length : lengths) {
            if (length > maximum_bits) {
                return false;
            }
            if (length != 0U) {
                ++counts[length];
                maximum_length_ = std::max(maximum_length_,
                    static_cast<unsigned int>(length));
            }
        }
        if (maximum_length_ == 0U) {
            return false;
        }
        std::array<std::uint32_t, maximum_bits + 1U> next{};
        std::uint32_t code{};
        for (unsigned int bits = 1U; bits <= maximum_bits; ++bits) {
            code = (code + counts[bits - 1U]) << 1U;
            next[bits] = code;
        }
        codes_.resize(lengths.size());
        lengths_.assign(lengths.begin(), lengths.end());
        for (std::size_t symbol{}; symbol < lengths.size(); ++symbol) {
            const unsigned int length = lengths[symbol];
            if (length != 0U) {
                codes_[symbol] = reverse_bits(next[length]++, length);
            }
        }
        return true;
    }

    [[nodiscard]] bool decode(BitReader& reader, std::uint32_t& symbol) const noexcept
    {
        std::uint32_t code{};
        for (unsigned int length = 1U; length <= maximum_length_; ++length) {
            std::uint32_t bit{};
            if (!reader.read(1U, bit)) {
                return false;
            }
            code |= bit << (length - 1U);
            for (std::size_t index{}; index < lengths_.size(); ++index) {
                if (lengths_[index] == length && codes_[index] == code) {
                    symbol = static_cast<std::uint32_t>(index);
                    return true;
                }
            }
        }
        return false;
    }

private:
    std::vector<std::uint32_t> codes_;
    std::vector<std::uint8_t> lengths_;
    unsigned int maximum_length_{};
};

[[nodiscard]] bool fixed_trees(HuffmanTree& literal, HuffmanTree& distance)
{
    std::vector<std::uint8_t> literal_lengths(288U, 8U);
    std::fill(literal_lengths.begin() + 144, literal_lengths.begin() + 256,
        static_cast<std::uint8_t>(9U));
    std::fill(literal_lengths.begin() + 256, literal_lengths.begin() + 280,
        static_cast<std::uint8_t>(7U));
    std::fill(literal_lengths.begin() + 280, literal_lengths.end(),
        static_cast<std::uint8_t>(8U));
    std::vector<std::uint8_t> distance_lengths(32U, 5U);
    return literal.build(literal_lengths) && distance.build(distance_lengths);
}

[[nodiscard]] bool dynamic_trees(
    BitReader& reader,
    HuffmanTree& literal,
    HuffmanTree& distance)
{
    std::uint32_t hlit{};
    std::uint32_t hdist{};
    std::uint32_t hclen{};
    if (!reader.read(5U, hlit) || !reader.read(5U, hdist) ||
        !reader.read(4U, hclen)) {
        return false;
    }
    hlit += 257U;
    hdist += 1U;
    hclen += 4U;
    constexpr std::array<std::uint8_t, 19> order{
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    };
    std::array<std::uint8_t, 19> code_lengths{};
    for (std::uint32_t index{}; index < hclen; ++index) {
        std::uint32_t length{};
        if (!reader.read(3U, length)) {
            return false;
        }
        code_lengths[order[index]] = static_cast<std::uint8_t>(length);
    }
    HuffmanTree code_tree;
    if (!code_tree.build(code_lengths)) {
        return false;
    }
    std::vector<std::uint8_t> lengths;
    lengths.reserve(hlit + hdist);
    while (lengths.size() < hlit + hdist) {
        std::uint32_t symbol{};
        if (!code_tree.decode(reader, symbol)) {
            return false;
        }
        if (symbol <= 15U) {
            lengths.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        std::uint32_t extra{};
        std::size_t repeat{};
        std::uint8_t value{};
        if (symbol == 16U) {
            if (lengths.empty() || !reader.read(2U, extra)) {
                return false;
            }
            repeat = 3U + extra;
            value = lengths.back();
        } else if (symbol == 17U) {
            if (!reader.read(3U, extra)) {
                return false;
            }
            repeat = 3U + extra;
        } else if (symbol == 18U) {
            if (!reader.read(7U, extra)) {
                return false;
            }
            repeat = 11U + extra;
        } else {
            return false;
        }
        if (repeat > hlit + hdist - lengths.size()) {
            return false;
        }
        lengths.insert(lengths.end(), repeat, value);
    }
    return literal.build(std::span(lengths).first(hlit)) &&
           distance.build(std::span(lengths).subspan(hlit, hdist));
}

[[nodiscard]] bool inflate_block(
    BitReader& reader,
    const HuffmanTree& literal,
    const HuffmanTree& distance,
    std::vector<std::uint8_t>& output,
    const std::size_t maximum_output)
{
    constexpr std::array<std::uint16_t, 29> length_base{
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
    };
    constexpr std::array<std::uint8_t, 29> length_extra{
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    };
    constexpr std::array<std::uint16_t, 30> distance_base{
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1'025, 1'537, 2'049, 3'073,
        4'097, 6'145, 8'193, 12'289, 16'385, 24'577,
    };
    constexpr std::array<std::uint8_t, 30> distance_extra{
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
    };
    for (;;) {
        std::uint32_t symbol{};
        if (!literal.decode(reader, symbol)) {
            return false;
        }
        if (symbol < 256U) {
            if (output.size() >= maximum_output) {
                return false;
            }
            output.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256U) {
            return true;
        }
        if (symbol < 257U || symbol > 285U) {
            return false;
        }
        const std::size_t length_index = symbol - 257U;
        std::uint32_t extra{};
        if (!reader.read(length_extra[length_index], extra)) {
            return false;
        }
        const std::size_t length = length_base[length_index] + extra;
        std::uint32_t distance_symbol{};
        if (!distance.decode(reader, distance_symbol) || distance_symbol >= 30U ||
            !reader.read(distance_extra[distance_symbol], extra)) {
            return false;
        }
        const std::size_t copy_distance = distance_base[distance_symbol] + extra;
        if (copy_distance == 0U || copy_distance > output.size() ||
            length > maximum_output - output.size()) {
            return false;
        }
        for (std::size_t index{}; index < length; ++index) {
            output.push_back(output[output.size() - copy_distance]);
        }
    }
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> inflate_zlib(
    const std::span<const std::uint8_t> input,
    const std::size_t maximum_output = 8U * 1'024U * 1'024U)
{
    if (input.size() < 6U || (input[0] & 0x0fU) != 8U ||
        ((static_cast<unsigned int>(input[0]) << 8U) | input[1]) % 31U != 0U ||
        (input[1] & 0x20U) != 0U) {
        return std::nullopt;
    }
    BitReader reader(input.subspan(2U, input.size() - 6U));
    std::vector<std::uint8_t> output;
    bool final{};
    while (!final) {
        std::uint32_t final_bit{};
        std::uint32_t type{};
        if (!reader.read(1U, final_bit) || !reader.read(2U, type)) {
            return std::nullopt;
        }
        final = final_bit != 0U;
        if (type == 0U) {
            reader.align_byte();
            std::uint32_t length{};
            std::uint32_t inverse{};
            if (!reader.read(16U, length) || !reader.read(16U, inverse) ||
                (length ^ 0xffffU) != inverse ||
                length > maximum_output - output.size()) {
                return std::nullopt;
            }
            for (std::uint32_t index{}; index < length; ++index) {
                std::uint32_t byte{};
                if (!reader.read(8U, byte)) {
                    return std::nullopt;
                }
                output.push_back(static_cast<std::uint8_t>(byte));
            }
            continue;
        }
        HuffmanTree literal;
        HuffmanTree distance;
        const bool valid_tree = type == 1U
            ? fixed_trees(literal, distance)
            : type == 2U && dynamic_trees(reader, literal, distance);
        if (!valid_tree || !inflate_block(
                reader, literal, distance, output, maximum_output)) {
            return std::nullopt;
        }
    }
    std::uint32_t adler = 1U;
    std::uint32_t second{};
    for (const std::uint8_t byte : output) {
        adler = (adler + byte) % 65'521U;
        second = (second + adler) % 65'521U;
    }
    const std::uint32_t actual = (second << 16U) | adler;
    bool checksum_found{};
    for (std::size_t offset = 2U; offset + 4U <= input.size(); ++offset) {
        const std::uint32_t expected =
            (static_cast<std::uint32_t>(input[offset]) << 24U) |
            (static_cast<std::uint32_t>(input[offset + 1U]) << 16U) |
            (static_cast<std::uint32_t>(input[offset + 2U]) << 8U) |
            input[offset + 3U];
        if (expected == actual) {
            checksum_found = true;
            break;
        }
    }
    if (!checksum_found) {
        return std::nullopt;
    }
    return output;
}

using DesRoundKey = std::array<std::uint8_t, 6>;
using DesSchedule = std::array<DesRoundKey, 16>;

constexpr std::array<unsigned int, 64> kInitialPermutation{
    57,49,41,33,25,17,9,1, 59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7,
    56,48,40,32,24,16,8,0, 58,50,42,34,26,18,10,2,
    60,52,44,36,28,20,12,4, 62,54,46,38,30,22,14,6,
};
constexpr std::array<unsigned int, 48> kExpansion{
    31,0,1,2,3,4, 3,4,5,6,7,8, 7,8,9,10,11,12,
    11,12,13,14,15,16, 15,16,17,18,19,20,
    19,20,21,22,23,24, 23,24,25,26,27,28, 27,28,29,30,31,0,
};
constexpr std::array<unsigned int, 32> kRoundPermutation{
    15,6,19,20,28,11,27,16, 0,14,22,25,4,17,30,9,
    1,7,23,13,31,26,2,8, 18,12,29,5,21,10,3,24,
};
constexpr std::array<unsigned int, 28> kKeyLeft{
    56,48,40,32,24,16,8,0, 57,49,41,33,25,17,
    9,1,58,50,42,34,26,18,10,2,59,51,43,35,
};
constexpr std::array<unsigned int, 28> kKeyRight{
    62,54,46,38,30,22,14,6, 61,53,45,37,29,21,
    13,5,60,52,44,36,28,20,12,4,27,19,11,3,
};
constexpr std::array<unsigned int, 48> kKeyCompression{
    13,16,10,23,0,4,2,27,14,5,20,9,22,18,11,3,
    25,7,15,6,26,19,12,1, 40,51,30,36,46,54,29,39,
    50,44,32,47,43,48,38,55,33,52,45,41,49,35,28,31,
};
constexpr std::array<unsigned int, 16> kKeyRotations{
    1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1,
};
constexpr std::array<std::string_view, 8> kSBoxes{
    "e4d12fb83a6c59070f74e2d1a6cb953841e8d62bfc973a50fc8249175b3ea06d",
    "f18e6b34972dc05a3d47f28fc01a69b50e7ba4d158c6932fd8a13f42b67c05e9",
    "a09e63f51dc7b428d709346a285ecbf1d6498f30b12c5ae71ad069874fe3b52c",
    "7de3069a1285bc4fd8b56f03472c1ae9a690cb7df13e52843f06aad8945bc72e",
    "2c417ab6853fd0e9eb2c47d150fa3986421bad78f9c5630eb8c71e2d6f09a453",
    "c1af92680d34e75baf427c9561de0b389ef528c3704a1db6432c95fabe17608d",
    "4b2ef08d3c975a61d0b7491ae35c2f8614bdc37eaf6805926bd814a7950fe23c",
    "d2846fb1a93e50c71fd8a374c56b0e927b419ce206adf35821e74a8dfc90356b",
};

[[nodiscard]] constexpr std::uint32_t byte_bit(
    const std::span<const std::uint8_t> data,
    const unsigned int position) noexcept
{
    const std::size_t byte = (position / 32U) * 4U + 3U - (position % 32U) / 8U;
    return (data[byte] >> (7U - position % 8U)) & 1U;
}

[[nodiscard]] constexpr std::uint32_t word_bit(
    const std::uint32_t value,
    const unsigned int position) noexcept
{
    return (value >> (31U - position)) & 1U;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> permute_input(
    const std::span<const std::uint8_t, 8> block) noexcept
{
    std::uint32_t left{};
    std::uint32_t right{};
    for (std::size_t index{}; index < kInitialPermutation.size(); ++index) {
        const std::uint32_t bit = byte_bit(block, kInitialPermutation[index]);
        if (index < 32U) {
            left |= bit << (31U - static_cast<unsigned int>(index));
        } else {
            right |= bit << (63U - static_cast<unsigned int>(index));
        }
    }
    return {left, right};
}

[[nodiscard]] std::array<std::uint8_t, 8> permute_output(
    const std::uint32_t left,
    const std::uint32_t right) noexcept
{
    std::array<std::uint8_t, 8> output{};
    for (std::size_t index{}; index < kInitialPermutation.size(); ++index) {
        const std::uint32_t bit = index < 32U
            ? word_bit(left, static_cast<unsigned int>(index))
            : word_bit(right, static_cast<unsigned int>(index - 32U));
        const unsigned int position = kInitialPermutation[index];
        const std::size_t byte = (position / 32U) * 4U + 3U - (position % 32U) / 8U;
        output[byte] |= static_cast<std::uint8_t>(bit << (7U - position % 8U));
    }
    return output;
}

[[nodiscard]] constexpr std::uint8_t sbox_value(
    const std::size_t box,
    const std::uint8_t six_bits) noexcept
{
    const std::size_t index = (six_bits & 0x20U) |
        ((six_bits & 0x1fU) >> 1U) | ((six_bits & 1U) << 4U);
    const char value = kSBoxes[box][index];
    return static_cast<std::uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10);
}

[[nodiscard]] std::uint32_t des_round(
    const std::uint32_t state,
    const DesRoundKey& key) noexcept
{
    std::array<std::uint8_t, 6> expanded{};
    for (std::size_t index{}; index < kExpansion.size(); ++index) {
        expanded[index / 8U] |= static_cast<std::uint8_t>(
            word_bit(state, kExpansion[index]) << (7U - index % 8U));
    }
    for (std::size_t index{}; index < expanded.size(); ++index) {
        expanded[index] ^= key[index];
    }
    std::uint32_t substituted{};
    for (std::size_t box{}; box < 8U; ++box) {
        const std::size_t bit = box * 6U;
        const std::uint16_t pair = static_cast<std::uint16_t>(expanded[bit / 8U]) << 8U |
            (bit / 8U + 1U < expanded.size() ? expanded[bit / 8U + 1U] : 0U);
        const std::uint8_t six = static_cast<std::uint8_t>(
            (pair >> (10U - bit % 8U)) & 0x3fU);
        substituted |= static_cast<std::uint32_t>(sbox_value(box, six)) <<
            (28U - static_cast<unsigned int>(box * 4U));
    }
    std::uint32_t result{};
    for (std::size_t index{}; index < kRoundPermutation.size(); ++index) {
        result |= word_bit(substituted, kRoundPermutation[index]) <<
            (31U - static_cast<unsigned int>(index));
    }
    return result;
}

[[nodiscard]] DesSchedule make_des_schedule(
    const std::span<const std::uint8_t, 8> key,
    const bool decrypt) noexcept
{
    std::uint32_t left{};
    std::uint32_t right{};
    for (std::size_t index{}; index < 28U; ++index) {
        left |= byte_bit(key, kKeyLeft[index]) << (31U - static_cast<unsigned int>(index));
        right |= byte_bit(key, kKeyRight[index]) << (31U - static_cast<unsigned int>(index));
    }
    DesSchedule result{};
    for (std::size_t round{}; round < result.size(); ++round) {
        const unsigned int rotation = kKeyRotations[round];
        left = ((left << rotation) | (left >> (28U - rotation))) & 0xfffffff0U;
        right = ((right << rotation) | (right >> (28U - rotation))) & 0xfffffff0U;
        auto& round_key = result[decrypt ? result.size() - 1U - round : round];
        for (std::size_t index{}; index < kKeyCompression.size(); ++index) {
            const unsigned int source = kKeyCompression[index];
            const std::uint32_t bit = index < 24U
                ? word_bit(left, source)
                : word_bit(right, source - 27U);
            round_key[index / 8U] |= static_cast<std::uint8_t>(
                bit << (7U - index % 8U));
        }
    }
    return result;
}

[[nodiscard]] std::array<std::uint8_t, 8> custom_des(
    const std::span<const std::uint8_t, 8> block,
    const DesSchedule& schedule) noexcept
{
    auto [left, right] = permute_input(block);
    for (std::size_t round{}; round < 15U; ++round) {
        const std::uint32_t previous_right = right;
        right = des_round(right, schedule[round]) ^ left;
        left = previous_right;
    }
    left ^= des_round(right, schedule.back());
    return permute_output(left, right);
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> triple_des_decrypt(
    const std::span<const std::uint8_t> encrypted)
{
    constexpr std::array<std::uint8_t, 24> key{
        '!', '@', '#', ')', '(', '*', '$', '%', '1', '2', '3', 'Z',
        'X', 'C', '!', '@', '!', '@', '#', ')', '(', 'N', 'H', 'L',
    };
    if (encrypted.empty() || encrypted.size() % 8U != 0U) {
        return std::nullopt;
    }
    const auto first = make_des_schedule(std::span(key).subspan<16U, 8U>(), true);
    const auto middle = make_des_schedule(std::span(key).subspan<8U, 8U>(), false);
    const auto last = make_des_schedule(std::span(key).first<8U>(), true);
    std::vector<std::uint8_t> output(encrypted.size());
    for (std::size_t offset{}; offset < encrypted.size(); offset += 8U) {
        const auto stage1 = custom_des(
            std::span(encrypted).subspan(offset, 8U).first<8U>(), first);
        const auto stage2 = custom_des(std::span(stage1), middle);
        const auto stage3 = custom_des(std::span(stage2), last);
        std::ranges::copy(stage3, output.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return output;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> hex_bytes(
    const std::string_view value)
{
    if (value.empty() || value.size() % 2U != 0U) {
        return std::nullopt;
    }
    const auto nibble = [](const char character) noexcept -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> result(value.size() / 2U);
    for (std::size_t index{}; index < result.size(); ++index) {
        const int high = nibble(value[index * 2U]);
        const int low = nibble(value[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

} // namespace

std::optional<std::vector<std::uint8_t>> decode_base64(
    const std::wstring_view value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>(
            std::numeric_limits<DWORD>::max())) {
        return std::nullopt;
    }
    DWORD size{};
    const std::wstring storage(value);
    if (CryptStringToBinaryW(
            storage.c_str(),
            static_cast<DWORD>(storage.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &size,
            nullptr,
            nullptr) == FALSE) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(size);
    if (CryptStringToBinaryW(
            storage.c_str(),
            static_cast<DWORD>(storage.size()),
            CRYPT_STRING_BASE64,
            result.data(),
            &size,
            nullptr,
            nullptr) == FALSE) {
        return std::nullopt;
    }
    result.resize(size);
    return result;
}

std::optional<std::wstring> decode_krc(
    const std::span<const std::uint8_t> encrypted)
{
    constexpr std::array<std::uint8_t, 16> key{
        '@', 'G', 'a', 'w', '^', '2', 't', 'G', 'Q', '6', '1', '-',
        0xce, 0xd2, 'n', 'i',
    };
    if (encrypted.size() <= 4U ||
        !std::ranges::equal(encrypted.first(4U), std::array<std::uint8_t, 4>{'k','r','c','1'})) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> compressed(encrypted.size() - 4U);
    for (std::size_t index{}; index < compressed.size(); ++index) {
        compressed[index] = encrypted[index + 4U] ^ key[index % key.size()];
    }
    const auto plain = inflate_zlib(compressed);
    if (!plain) {
        return std::nullopt;
    }
    return utf8_to_wide(std::string_view(
        reinterpret_cast<const char*>(plain->data()), plain->size()));
}

std::optional<std::wstring> decode_qrc(const std::string_view encrypted_hex)
{
    const auto encrypted = hex_bytes(encrypted_hex);
    if (!encrypted) {
        return std::nullopt;
    }
    const auto compressed = triple_des_decrypt(*encrypted);
    if (!compressed) {
        return std::nullopt;
    }
    const auto plain = inflate_zlib(*compressed);
    if (!plain) return std::nullopt;
    return utf8_to_wide(std::string_view(
        reinterpret_cast<const char*>(plain->data()), plain->size()));
}

} // namespace cd404::platform::windows::detail
