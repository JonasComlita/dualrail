#include "ternary_asm.h"
#include "ternary_symbolic_encoding.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(const char* message, int code) {
    std::cerr << "FAIL: " << message << "\n";
    return code;
}

} // namespace

int main() {
    using namespace sandbox::symbolic;

    const std::string expected_table =
        " \t\n\"',.:;!-_+=/[]{}"
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    if (tascii81Table() != expected_table || tascii81Table().size() != 81)
        return fail("TASCII-81 table has drifted", 1);

    const auto every_symbol = asciiToTascii81(expected_table);
    if (every_symbol.size() != 81)
        return fail("TASCII-81 encode length", 2);
    for (std::size_t i = 0; i < every_symbol.size(); ++i) {
        if (every_symbol[i] != i)
            return fail("TASCII-81 table index parity", 3);
    }
    std::vector<std::uint8_t> every_index(81);
    for (std::size_t i = 0; i < every_index.size(); ++i)
        every_index[i] = static_cast<std::uint8_t>(i);
    if (tascii81ToAscii(every_index) != expected_table)
        return fail("TASCII-81 decode parity", 4);

    const std::string sample = "Trit /bin";
    if (tascii81ToAscii(asciiToTascii81(sample)) != sample)
        return fail("TASCII-81 round trip", 5);
    try {
        (void)asciiToTascii81("@");
        return fail("unsupported TASCII-81 character accepted", 6);
    } catch (const std::invalid_argument&) {
    }
    try {
        (void)asciiToTascii81(std::string("\x80", 1));
        return fail("non-ASCII TASCII-81 character accepted", 7);
    } catch (const std::invalid_argument&) {
    }

    for (long long value : {std::numeric_limits<long long>::min(), -9841LL,
                            -42LL, -1LL, 0LL, 1LL, 42LL, 9841LL,
                            std::numeric_limits<long long>::max()}) {
        const auto exact = parseNumericLiteral(formatTritLiteral(balancedTrits(value)));
        const auto b27 = parseNumericLiteral(formatBase27(value));
        const auto b81 = parseNumericLiteral(formatBase81(value));
        if (!exact || !b27 || !b81 || *exact != value || *b27 != value ||
            *b81 != value)
            return fail("canonical numeric round trip", 8);
    }
    if (parseNumericLiteral("0y-+++0") != -42)
        return fail("0y parse-only compatibility", 9);
    if (!isTritLiteral("0y-+++0") || isTritLiteral("0t"))
        return fail("exact-trit prefix validation", 10);
    if (formatTritLiteral(balancedTrits(-42)).rfind("0t", 0) != 0)
        return fail("formatters must emit canonical 0t", 11);

    if (parseNumericLiteral("0x2a") != 42 ||
        parseNumericLiteral("-17") != -17 ||
        parseNumericLiteral("+17") != 17 ||
        parseNumericLiteral("-0x8000000000000000") !=
            std::numeric_limits<long long>::min())
        return fail("decimal/hex compatibility", 12);

    const std::array<std::string_view, 10> malformed = {
        "0t", "0y", "0t+2", "0z27:", "0z27:!", "0z81:",
        "0z81:~", "0x", "12x", "--1"};
    for (const auto token : malformed) {
        if (parseNumericLiteral(token))
            return fail("malformed numeric literal accepted", 13);
    }
    if (parseNumericLiteral("9223372036854775808") ||
        parseNumericLiteral("-9223372036854775809") ||
        parseNumericLiteral(std::string(400, '9')))
        return fail("numeric overflow accepted", 14);

    try {
        (void)hexDecode("41 42");
        return fail("hex whitespace accepted", 15);
    } catch (const std::invalid_argument&) {
    }
    try {
        (void)hexDecode("ABC");
        return fail("odd-length hex accepted", 16);
    } catch (const std::invalid_argument&) {
    }
    try {
        (void)hexDecode("GG");
        return fail("invalid hex accepted", 17);
    } catch (const std::invalid_argument&) {
    }

    if (formatIntegerDump(-42) !=
        "hex=0xFFFFFFFFFFFFFFD6 trits=0t-+++0 base27=0z27:bp base81=0z81:d>")
        return fail("default human dump compatibility", 18);
    if (sandbox::vm::assembler::parseImmOrLabel("0t-0+").imm != -8 ||
        sandbox::vm::assembler::parseImmOrLabel("0y-0+").imm != -8 ||
        sandbox::vm::assembler::parseImmOrLabel("0z81:d>").imm != -42 ||
        sandbox::vm::assembler::parseImmOrLabel("0z81:~").isLabel)
        return fail("assembler symbolic numeric parity", 19);
    if (validateUtf8(std::string("\xC3", 1))) return fail("invalid UTF-8 accepted", 20);
    if (!validateUtf8("ASCII")) return fail("valid UTF-8 rejected", 21);

    std::cout << "Symbolic encoding tests passed\n";
    return 0;
}
