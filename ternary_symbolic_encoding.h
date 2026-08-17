// =============================================================================
// ternary_symbolic_encoding.h - host-side symbolic encoding contract
// =============================================================================
//
// The VM's value model is balanced ternary, while source files, manifests and
// host tools still need ASCII, UTF-8 and hexadecimal at their boundaries.  This
// header keeps those projections explicit and provides the small, deterministic
// ternary-native notation used by assemblers and dump tools.
//
// Canonical notation:
//   0t<balanced-trits>  exact trits, most-significant first, using -, 0, +
//   0z27:<digits>       groups of three trits, base-27 dump alphabet
//   0z81:<digits>       groups of four trits, base-81 dump alphabet
//
// `0y` is accepted as a compatibility spelling for an older proposal of the
// exact-trit prefix; formatters always emit the canonical `0t` spelling.
// Neither native spelling changes the meaning of existing decimal or 0x hex
// literals.  A literal which starts like a number but is malformed is rejected
// by the caller rather than being silently treated as a label.
//
// This file is deliberately independent of the VM headers so host tools and
// focused contract tests can include it without pulling in the full runtime.

#pragma once
#ifndef TERNARY_SYMBOLIC_ENCODING_H
#define TERNARY_SYMBOLIC_ENCODING_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sandbox::symbolic {

using Trit = std::int8_t;
using Trits = std::vector<Trit>; // textual order: most-significant trit first

// TASCII-81 has 4 trits per symbol.  The first 19 entries cover the control
// and punctuation needed by source, shell and path text; all decimal digits
// and Latin letters then have stable contiguous ranges.
inline constexpr std::string_view kTascii81Table =
    " \t\n\"',.:;!-_+=/[]{}"
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz";
static_assert(kTascii81Table.size() == 81,
              "TASCII-81 must contain exactly 81 symbols");

// Digits are deliberately printable and whitespace-free for log/dump output.
// Their value is the ordinary 0..26/0..80 value of a balanced-trit group.
inline constexpr std::string_view kBase27Alphabet =
    "0123456789abcdefghijklmnopq";
inline constexpr std::string_view kBase81Alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?";
static_assert(kBase27Alphabet.size() == 27,
              "base-27 alphabet must contain exactly 27 symbols");
static_assert(kBase81Alphabet.size() == 81,
              "base-81 alphabet must contain exactly 81 symbols");

inline constexpr std::string_view tascii81Table() noexcept {
    return kTascii81Table;
}

inline std::optional<std::uint8_t> tascii81EncodeChar(unsigned char c) {
    if (c > 0x7f) return std::nullopt;
    const auto pos = kTascii81Table.find(static_cast<char>(c));
    if (pos == std::string_view::npos) return std::nullopt;
    return static_cast<std::uint8_t>(pos);
}

inline std::optional<char> tascii81DecodeChar(std::uint8_t value) {
    if (value >= kTascii81Table.size()) return std::nullopt;
    return kTascii81Table[value];
}

inline std::vector<std::uint8_t> tascii81Encode(std::string_view ascii) {
    std::vector<std::uint8_t> out;
    out.reserve(ascii.size());
    for (unsigned char c : ascii) {
        const auto encoded = tascii81EncodeChar(c);
        if (!encoded) {
            throw std::invalid_argument(
                "TASCII-81 only accepts ASCII characters present in its fixed table");
        }
        out.push_back(*encoded);
    }
    return out;
}

inline std::string tascii81Decode(const std::vector<std::uint8_t>& values) {
    std::string out;
    out.reserve(values.size());
    for (const auto value : values) {
        const auto decoded = tascii81DecodeChar(value);
        if (!decoded) throw std::invalid_argument("invalid TASCII-81 symbol index");
        out.push_back(*decoded);
    }
    return out;
}

// Compatibility aliases used by app/host callers that spell the acronym with
// a capital A or expand the name.
inline std::vector<std::uint8_t> tASCII81Encode(std::string_view ascii) {
    return tascii81Encode(ascii);
}
inline std::string tASCII81Decode(const std::vector<std::uint8_t>& values) {
    return tascii81Decode(values);
}
inline std::vector<std::uint8_t> asciiToTascii81(std::string_view ascii) {
    return tascii81Encode(ascii);
}
inline std::string tascii81ToAscii(const std::vector<std::uint8_t>& values) {
    return tascii81Decode(values);
}

inline std::vector<std::uint8_t> asciiToBytes(std::string_view ascii) {
    std::vector<std::uint8_t> out;
    out.reserve(ascii.size());
    for (unsigned char c : ascii) {
        if (c > 0x7f) throw std::invalid_argument("input is not ASCII");
        out.push_back(c);
    }
    return out;
}

inline std::string bytesToAscii(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const auto byte : bytes) {
        if (byte > 0x7f) throw std::invalid_argument("byte is not ASCII");
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

inline bool decodeUtf8CodePoint(std::string_view text,
                                std::size_t& cursor,
                                std::uint32_t& code_point) {
    if (cursor >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[cursor++]);
    if (first <= 0x7f) {
        code_point = first;
        return true;
    }
    int continuation_count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        continuation_count = 1;
        value = first & 0x1f;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        continuation_count = 2;
        value = first & 0x0f;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        continuation_count = 3;
        value = first & 0x07;
        minimum = 0x10000;
    } else {
        return false;
    }
    if (cursor + static_cast<std::size_t>(continuation_count) > text.size()) {
        return false;
    }
    for (int i = 0; i < continuation_count; ++i) {
        const auto next = static_cast<unsigned char>(text[cursor++]);
        if ((next & 0xc0) != 0x80) return false;
        value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        return false;
    }
    code_point = value;
    return true;
}

inline bool validateUtf8(std::string_view text) noexcept {
    std::size_t cursor = 0;
    std::uint32_t code_point = 0;
    while (cursor < text.size()) {
        if (!decodeUtf8CodePoint(text, cursor, code_point)) return false;
    }
    return true;
}

inline std::u32string utf8ToCodePoints(std::string_view text) {
    std::u32string out;
    std::size_t cursor = 0;
    std::uint32_t code_point = 0;
    while (cursor < text.size()) {
        if (!decodeUtf8CodePoint(text, cursor, code_point)) {
            throw std::invalid_argument("invalid UTF-8 sequence");
        }
        out.push_back(static_cast<char32_t>(code_point));
    }
    return out;
}

inline std::string codePointsToUtf8(const std::u32string& code_points) {
    std::string out;
    for (const char32_t raw : code_points) {
        const auto cp = static_cast<std::uint32_t>(raw);
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            throw std::invalid_argument("invalid Unicode scalar value");
        }
        if (cp <= 0x7f) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

inline std::string utf8ToBytes(std::string_view text) {
    if (!validateUtf8(text)) throw std::invalid_argument("invalid UTF-8 sequence");
    return std::string(text);
}
inline std::string bytesToUtf8(std::string_view text) {
    if (!validateUtf8(text)) throw std::invalid_argument("invalid UTF-8 sequence");
    return std::string(text);
}

inline char hexDigit(unsigned value) noexcept {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + (value - 10));
}

inline std::string hexEncode(const std::vector<std::uint8_t>& bytes,
                             bool prefix = false) {
    std::string out;
    out.reserve((prefix ? 2 : 0) + bytes.size() * 2);
    if (prefix) out += "0x";
    for (const auto byte : bytes) {
        out.push_back(hexDigit(byte >> 4));
        out.push_back(hexDigit(byte & 0x0f));
    }
    return out;
}

inline std::vector<std::uint8_t> hexDecode(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.size() % 2 != 0) throw std::invalid_argument("hex string has odd length");
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]);
        const int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) throw std::invalid_argument("invalid hex digit");
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

inline Trits balancedTrits(long long value, std::size_t minimum_width = 1) {
    Trits out;
    while (value != 0) {
        long long remainder = value % 3;
        value /= 3;
        if (remainder == 2) {
            remainder = -1;
            ++value;
        } else if (remainder == -2) {
            remainder = 1;
            --value;
        }
        out.push_back(static_cast<Trit>(remainder));
    }
    while (out.size() < minimum_width) out.push_back(0);
    if (out.empty()) out.push_back(0);
    std::reverse(out.begin(), out.end());
    return out;
}

inline std::optional<long long> tritsToLong(const Trits& trits) noexcept {
    __int128 value = 0;
    for (const Trit trit : trits) {
        if (trit < -1 || trit > 1) return std::nullopt;
        value = value * 3 + trit;
        if (value < std::numeric_limits<long long>::min() ||
            value > std::numeric_limits<long long>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<long long>(value);
}

inline bool isTritLiteral(std::string_view token) noexcept {
    if (token.size() < 3 || token[0] != '0' ||
        (token[1] != 't' && token[1] != 'T' &&
         token[1] != 'y' && token[1] != 'Y')) {
        return false;
    }
    for (std::size_t i = 2; i < token.size(); ++i) {
        if (token[i] != '-' && token[i] != '0' && token[i] != '+') return false;
    }
    return true;
}

inline std::optional<Trits> parseTritLiteral(std::string_view token) {
    if (!isTritLiteral(token)) return std::nullopt;
    Trits out;
    out.reserve(token.size() - 2);
    for (std::size_t i = 2; i < token.size(); ++i) {
        out.push_back(token[i] == '-' ? -1 : token[i] == '+' ? 1 : 0);
    }
    return out;
}

inline std::string formatTritLiteral(const Trits& trits) {
    if (trits.empty()) return "0t0";
    std::string out = "0t";
    out.reserve(2 + trits.size());
    for (const Trit trit : trits) {
        if (trit < -1 || trit > 1) throw std::invalid_argument("invalid balanced trit");
        out.push_back(trit < 0 ? '-' : trit > 0 ? '+' : '0');
    }
    return out;
}

inline int baseDigit(std::string_view alphabet, char c) noexcept {
    const auto pos = alphabet.find(c);
    if (pos != std::string_view::npos) return static_cast<int>(pos);
    if (alphabet == kBase27Alphabet || alphabet == kBase81Alphabet) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'a' && uc <= 'z') {
            const char upper = static_cast<char>(uc - 'a' + 'A');
            const auto upper_pos = alphabet.find(upper);
            if (upper_pos != std::string_view::npos) {
                return static_cast<int>(upper_pos);
            }
        }
    }
    return -1;
}

inline Trits decodeGroupedDigits(std::string_view digits,
                                 std::string_view alphabet,
                                 std::size_t trits_per_digit) {
    if (digits.empty()) throw std::invalid_argument("compact ternary dump is empty");
    if (trits_per_digit == 0 ||
        digits.size() > std::numeric_limits<std::size_t>::max() / trits_per_digit) {
        throw std::invalid_argument("compact ternary dump is too large");
    }
    Trits out;
    out.reserve(digits.size() * trits_per_digit);
    for (const char c : digits) {
        const int raw = baseDigit(alphabet, c);
        if (raw < 0) throw std::invalid_argument("invalid compact ternary digit");
        int value = raw;
        int weight = 1;
        for (std::size_t i = 0; i < trits_per_digit; ++i) weight *= 3;
        for (std::size_t i = 0; i < trits_per_digit; ++i) {
            weight /= 3;
            const int ordinary = value / weight;
            value -= ordinary * weight;
            out.push_back(static_cast<Trit>(ordinary - 1));
        }
    }
    return out;
}

inline std::string encodeGroupedDigits(const Trits& input,
                                       std::string_view alphabet,
                                       std::size_t trits_per_digit,
                                       std::string_view prefix) {
    Trits trits = input;
    if (trits.empty()) trits.push_back(0);
    for (const Trit trit : trits) {
        if (trit < -1 || trit > 1) throw std::invalid_argument("invalid balanced trit");
    }
    const std::size_t remainder = trits.size() % trits_per_digit;
    if (remainder != 0) {
        trits.insert(trits.begin(), trits_per_digit - remainder, 0);
    }
    std::string out(prefix);
    out.reserve(prefix.size() + trits.size() / trits_per_digit);
    for (std::size_t i = 0; i < trits.size(); i += trits_per_digit) {
        int digit = 0;
        for (std::size_t j = 0; j < trits_per_digit; ++j) {
            digit = digit * 3 + (trits[i + j] + 1);
        }
        out.push_back(alphabet[static_cast<std::size_t>(digit)]);
    }
    return out;
}

inline std::optional<Trits> parseBase27Dump(std::string_view token) {
    if (token.size() < 6 || token.substr(0, 5) != "0z27:") return std::nullopt;
    try {
        return decodeGroupedDigits(token.substr(5), kBase27Alphabet, 3);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

inline std::optional<Trits> parseBase81Dump(std::string_view token) {
    if (token.size() < 6 || token.substr(0, 5) != "0z81:") return std::nullopt;
    try {
        return decodeGroupedDigits(token.substr(5), kBase81Alphabet, 4);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

inline std::string formatBase27(const Trits& trits) {
    return encodeGroupedDigits(trits, kBase27Alphabet, 3, "0z27:");
}
inline std::string formatBase81(const Trits& trits) {
    return encodeGroupedDigits(trits, kBase81Alphabet, 4, "0z81:");
}
inline std::string formatBase27(long long value) {
    return formatBase27(balancedTrits(value));
}
inline std::string formatBase81(long long value) {
    return formatBase81(balancedTrits(value));
}

inline std::optional<long long> parseNumericLiteral(std::string_view token) {
    if (token.empty()) return std::nullopt;
    if (const auto trits = parseTritLiteral(token)) return tritsToLong(*trits);
    if (const auto base27 = parseBase27Dump(token)) return tritsToLong(*base27);
    if (const auto base81 = parseBase81Dump(token)) return tritsToLong(*base81);

    bool negative = false;
    std::size_t offset = 0;
    if (token[offset] == '+' || token[offset] == '-') {
        negative = token[offset] == '-';
        if (++offset == token.size()) return std::nullopt;
    }
    int base = 10;
    if (offset + 2 <= token.size() && token[offset] == '0' &&
        (token[offset + 1] == 'x' || token[offset + 1] == 'X')) {
        base = 16;
        offset += 2;
        if (offset == token.size()) return std::nullopt;
    }
    __int128 value = 0;
    const __int128 max_magnitude =
        static_cast<__int128>(std::numeric_limits<long long>::max()) +
        (negative ? 1 : 0);
    for (std::size_t i = offset; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0 || digit >= base) return std::nullopt;
        if (value > (max_magnitude - digit) / base) return std::nullopt;
        value = value * base + digit;
    }
    if (negative) value = -value;
    if (value < std::numeric_limits<long long>::min() ||
        value > std::numeric_limits<long long>::max()) return std::nullopt;
    return static_cast<long long>(value);
}

inline bool looksLikeNumericLiteral(std::string_view token) noexcept {
    if (token.empty()) return false;
    std::size_t i = (token[0] == '+' || token[0] == '-') ? 1 : 0;
    if (i == token.size()) return false;
    if (std::isdigit(static_cast<unsigned char>(token[i]))) return true;
    return token.size() >= i + 2 && token[i] == '0' &&
           (token[i + 1] == 't' || token[i + 1] == 'T' ||
            token[i + 1] == 'y' || token[i + 1] == 'Y' ||
            token[i + 1] == 'z' || token[i + 1] == 'Z');
}

inline std::string formatTritDump(const Trits& trits) {
    return formatTritLiteral(trits) + " " + formatBase27(trits) + " " +
           formatBase81(trits);
}

template <typename TritWord>
inline std::string formatTritWordDump(const TritWord& word,
                                      std::size_t width) {
    Trits trits;
    trits.reserve(width);
    for (std::size_t i = width; i-- > 0;) {
        trits.push_back(static_cast<Trit>(word.getTrit(static_cast<int>(i))));
    }
    return formatTritDump(trits);
}

inline std::string formatIntegerDump(long long value) {
    const auto trits = balancedTrits(value);
    const auto magnitude = static_cast<unsigned long long>(value);
    std::string hex = "0x";
    static constexpr char digits[] = "0123456789ABCDEF";
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((magnitude >> shift) & 0xfULL);
        if (nibble != 0 || started || shift == 0) {
            started = true;
            hex.push_back(digits[nibble]);
        }
    }
    return "hex=" + hex + " trits=" + formatTritLiteral(trits) +
           " base27=" + formatBase27(trits) +
           " base81=" + formatBase81(trits);
}

} // namespace sandbox::symbolic

namespace sandbox {
namespace vm {
namespace symbolic = ::sandbox::symbolic;
namespace encoding = ::sandbox::symbolic;
} // namespace vm
namespace encoding = ::sandbox::symbolic;
} // namespace sandbox

#endif // TERNARY_SYMBOLIC_ENCODING_H
