#include "ternary_asm.h"
#include "ternary_symbolic_encoding.h"

#include <iostream>
#include <string>

int main() {
    using namespace sandbox::symbolic;
    const std::string sample = "Trit /bin";
    const auto encoded = asciiToTascii81(sample);
    if (tascii81ToAscii(encoded) != sample) return 1;
    try {
        (void)asciiToTascii81("@");
        return 2;
    } catch (const std::invalid_argument&) {
    }

    for (long long value : {-9841LL, -42LL, -1LL, 0LL, 1LL, 42LL, 9841LL}) {
        const auto exact = parseNumericLiteral(formatTritLiteral(balancedTrits(value)));
        const auto b27 = parseNumericLiteral(formatBase27(value));
        const auto b81 = parseNumericLiteral(formatBase81(value));
        if (!exact || !b27 || !b81 || *exact != value || *b27 != value || *b81 != value)
            return 3;
    }
    if (parseNumericLiteral("0x2a") != 42 || parseNumericLiteral("-17") != -17)
        return 4;
    if (sandbox::vm::assembler::parseImmOrLabel("0t-0+").imm != -8 ||
        sandbox::vm::assembler::parseImmOrLabel("0z81:d>").isLabel)
        return 5;
    if (validateUtf8(std::string("\xC3", 1))) return 6;
    if (!validateUtf8("ASCII")) return 7;
    std::cout << "Symbolic encoding tests passed\n";
    return 0;
}
