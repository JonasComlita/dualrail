#include "ternary_vm.h"
#include "int128_compat.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sandbox::LongTriple;

static int g_failures = 0;

std::string toStringUnsigned(uint128_t value) {
    if (value == 0) return "0";

    std::string out;
    while (value != 0) {
        const unsigned digit = static_cast<unsigned>(value % 10);
        out.insert(out.begin(), static_cast<char>('0' + digit));
        value /= 10;
    }
    return out;
}

std::string toStringSigned(int128_t value) {
    if (value < 0) {
        return "-" + toStringUnsigned(static_cast<uint128_t>(-value));
    }
    return toStringUnsigned(static_cast<uint128_t>(value));
}

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

uint128_t pow3(int n) {
    uint128_t value = 1;
    for (int i = 0; i < n; ++i) value *= 3;
    return value;
}

int8_t balancedRem(int128_t n) {
    int128_t r = n % 3;
    if (r > 1) r -= 3;
    if (r < -1) r += 3;
    return static_cast<int8_t>(r);
}

std::array<int8_t, 41> encodeBalanced41(int128_t n) {
    std::array<int8_t, 41> trits{};
    for (int i = 0; i < 41; ++i) {
        const int8_t trit = balancedRem(n);
        trits[i] = trit;
        n = (n - trit) / 3;
    }
    expect(n == 0, "encodeBalanced41 overflow while building test value");
    return trits;
}

int128_t mantissaToInt(const std::array<int8_t, 50>& trits) {
    int128_t value = 0;
    int128_t place = 1;
    for (int i = 0; i < 41; ++i) {
        value += static_cast<int>(trits[i]) * place;
        place *= 3;
    }
    return value;
}

int exponentOf(const std::array<int8_t, 50>& trits) {
    int exponent = 0;
    int place = 1;
    for (int i = 0; i < 9; ++i) {
        exponent += trits[41 + i] * place;
        place *= 3;
    }
    return exponent;
}

LongTriple packMantissaExp(const std::array<int8_t, 41>& mantissa, int exponent) {
    std::array<int8_t, 50> trits{};
    for (int i = 0; i < 41; ++i) trits[i] = mantissa[i];

    int e = exponent;
    for (int i = 0; i < 9; ++i) {
        const int8_t trit = balancedRem(e);
        trits[41 + i] = trit;
        e = (e - trit) / 3;
    }
    expect(e == 0, "packMantissaExp exponent overflow while building test value");
    return LongTriple::pack(trits);
}

LongTriple fromInt(int128_t value) {
    if (value == 0) return LongTriple{0};
    return packMantissaExp(encodeBalanced41(value), 40);
}

bool exactIntegerValue(LongTriple t, int128_t& out) {
    if (t.isZero()) {
        out = 0;
        return true;
    }
    if (t.isSpecial()) return false;

    const auto trits = t.unpack();
    int128_t mantissa = mantissaToInt(trits);
    const int exponent = exponentOf(trits);

    if (mantissa == 0) {
        out = 0;
        return true;
    }

    if (exponent >= 40) {
        out = mantissa * static_cast<int128_t>(pow3(exponent - 40));
        return true;
    }

    const int128_t divisor = static_cast<int128_t>(pow3(40 - exponent));
    if (mantissa % divisor != 0) return false;
    out = mantissa / divisor;
    return true;
}

int signOf(int128_t value) {
    if (value < 0) return -1;
    if (value > 0) return 1;
    return 0;
}

uint128_t abs128(int128_t value) {
    return value < 0
        ? static_cast<uint128_t>(-value)
        : static_cast<uint128_t>(value);
}

uint128_t roundedDiv3(uint128_t n) {
    switch (static_cast<unsigned>(n % 3)) {
        case 0:  return n / 3;
        case 1:  return (n - 1) / 3;
        default: return (n + 1) / 3;
    }
}

int8_t balancedRemUnsigned(uint128_t n) {
    const unsigned rem = static_cast<unsigned>(n % 3);
    if (rem == 0) return 0;
    if (rem == 1) return 1;
    return -1;
}

LongTriple referenceMultiply(LongTriple a, LongTriple b) {
    if (a.isZero() || b.isZero()) return LongTriple{0};
    if (a.isSpecial() || b.isSpecial()) return LongTriple{LongTriple::OVERFLOW_DATA};

    const auto aTrits = a.unpack();
    const auto bTrits = b.unpack();
    const int128_t ma = mantissaToInt(aTrits);
    const int128_t mb = mantissaToInt(bTrits);
    const int ea = exponentOf(aTrits);
    const int eb = exponentOf(bTrits);

    if (ma == 0 || mb == 0) return LongTriple{0};

    const int sign = signOf(ma) * signOf(mb);
    uint128_t magnitude = abs128(ma) * abs128(mb);
    int exponent = ea + eb - 40;

    const uint128_t mantissaMax = (pow3(41) - 1) / 2;
    const uint128_t mantissaMin = (pow3(40) - 1) / 2;

    while (magnitude > mantissaMax) {
        magnitude = roundedDiv3(magnitude);
        ++exponent;
    }
    while (magnitude != 0 && magnitude < mantissaMin) {
        magnitude *= 3;
        --exponent;
    }

    if (magnitude == 0) return LongTriple{0};
    if (exponent > LongTriple::EXP_MAX) return LongTriple{LongTriple::OVERFLOW_DATA};
    if (exponent < LongTriple::EXP_MIN) return LongTriple{LongTriple::UNDERFLOW_DATA};

    std::array<int8_t, 41> mantissa{};
    for (int i = 0; i < 41; ++i) {
        const int8_t trit = balancedRemUnsigned(magnitude);
        mantissa[i] = static_cast<int8_t>(sign * trit);
        if (trit < 0) {
            magnitude = (magnitude + 1) / 3;
        } else {
            magnitude = (magnitude - static_cast<unsigned>(trit)) / 3;
        }
    }

    return packMantissaExp(mantissa, exponent);
}

void expectSameLongTriple(LongTriple got, LongTriple want, const std::string& label) {
    if (got.data == want.data) return;

    ++g_failures;
    std::cout << "FAIL: " << label << "\n";
    std::cout << "  got.data != want.data\n";

    if (!got.isSpecial() && !want.isSpecial()) {
        const auto g = got.unpack();
        const auto w = want.unpack();
        std::cout << "  got mantissa=" << toStringSigned(mantissaToInt(g))
                  << " exponent=" << exponentOf(g) << "\n";
        std::cout << "  want mantissa=" << toStringSigned(mantissaToInt(w))
                  << " exponent=" << exponentOf(w) << "\n";
    }
}

void testBalancedEncoding() {
    std::cout << "[1] balanced integer encoding\n";

    for (int128_t n = -200000; n <= 200000; ++n) {
        const auto mantissa = encodeBalanced41(n);
        for (int i = 0; i < 41; ++i) {
            expect(mantissa[i] >= -1 && mantissa[i] <= 1,
                   "invalid trit from encodeBalanced41(" + toStringSigned(n) + ")");
        }

        LongTriple t = packMantissaExp(mantissa, 40);
        int128_t decoded = 0;
        expect(exactIntegerValue(t, decoded), "encoded integer should decode exactly");
        expect(decoded == n, "integer round-trip " + toStringSigned(n));
    }
}

void testBalancedRemainder() {
    std::cout << "[2] native balanced remainder helper\n";

    for (long long n = -200000; n <= 200000; ++n) {
        const int8_t rem = sandbox::native_ops::detail::balancedRem(n);
        expect(rem >= -1 && rem <= 1, "native balancedRem out of range");
        expect((n - rem) % 3 == 0, "native balancedRem divisibility");
    }

    for (int i = 0; i <= 80; ++i) {
        const uint128_t n = pow3(i);
        const int8_t rem = sandbox::native_ops::detail::balancedRem(n);
        expect(rem >= -1 && rem <= 1, "native unsigned balancedRem out of range");
    }
}

void testExactSmallIntegerMultiply() {
    std::cout << "[3] exhaustive small integer multiply\n";

    for (int a = -121; a <= 121; ++a) {
        for (int b = -121; b <= 121; ++b) {
            LongTriple got = sandbox::ops::multiply(fromInt(a), fromInt(b));
            int128_t decoded = 0;
            expect(exactIntegerValue(got, decoded),
                   "product should be exact integer for small inputs");
            expect(decoded == static_cast<int128_t>(a) * b,
                   "small product " + std::to_string(a) + "*" + std::to_string(b));
        }
    }
}

void testExactSmallIntegerAddSubtract() {
    std::cout << "[4] exhaustive small integer add/subtract\n";

    for (int a = -121; a <= 121; ++a) {
        for (int b = -121; b <= 121; ++b) {
            int128_t decodedAdd = 0;
            LongTriple add = sandbox::ops::add(fromInt(a), fromInt(b));
            expect(exactIntegerValue(add, decodedAdd),
                   "sum should be exact integer for small inputs");
            expect(decodedAdd == static_cast<int128_t>(a) + b,
                   "small sum " + std::to_string(a) + "+" + std::to_string(b));

            int128_t decodedSub = 0;
            LongTriple sub = sandbox::ops::subtract(fromInt(a), fromInt(b));
            expect(exactIntegerValue(sub, decodedSub),
                   "difference should be exact integer for small inputs");
            expect(decodedSub == static_cast<int128_t>(a) - b,
                   "small difference " + std::to_string(a) + "-" + std::to_string(b));
        }
    }
}

void testExactIntegerDivideAndSqrt() {
    std::cout << "[5] exact integer divide/sqrt\n";

    for (int a = -2000; a <= 2000; ++a) {
        for (int b = -80; b <= 80; ++b) {
            if (b == 0 || (a % b) != 0) continue;

            int128_t decoded = 0;
            LongTriple quotient = sandbox::ops::divide(fromInt(a), fromInt(b));
            expect(exactIntegerValue(quotient, decoded),
                   "integer quotient should decode exactly");
            expect(decoded == a / b,
                   "integer quotient " + std::to_string(a) + "/" + std::to_string(b));
        }
    }

    for (int n = 0; n <= 2000; ++n) {
        const int128_t square = static_cast<int128_t>(n) * n;
        int128_t decoded = 0;
        LongTriple root = sandbox::ops::sqrt(fromInt(square));
        expect(exactIntegerValue(root, decoded),
               "perfect-square sqrt should decode exactly");
        expect(decoded == n, "sqrt perfect square " + std::to_string(n));
    }
}

std::vector<int128_t> interestingMantissas() {
    const int128_t maxMantissa = static_cast<int128_t>((pow3(41) - 1) / 2);
    const int128_t minNormalized = static_cast<int128_t>((pow3(40) - 1) / 2);

    std::vector<int128_t> values = {
        -maxMantissa, -maxMantissa + 1,
        -minNormalized, -minNormalized + 1,
        -1000000000000LL, -59049, -243, -122, -121, -120,
        -42, -10, -5, -3, -2, -1,
        0,
        1, 2, 3, 5, 10, 42,
        120, 121, 122, 243, 59049, 1000000000000LL,
        minNormalized - 1, minNormalized,
        maxMantissa - 1, maxMantissa,
    };

    for (int k = 0; k <= 40; ++k) {
        const int128_t p = static_cast<int128_t>(pow3(k));
        values.push_back(p);
        values.push_back(-p);
        if (p > 1) {
            values.push_back(p - 1);
            values.push_back(-(p - 1));
        }
        if (p < maxMantissa) {
            values.push_back(p + 1);
            values.push_back(-(p + 1));
        }
    }

    return values;
}

void testReferenceMultiplyCorpus() {
    std::cout << "[6] exact reference corpus multiply\n";

    const std::vector<int128_t> mantissas = interestingMantissas();
    const std::vector<int> exponents = {
        LongTriple::EXP_MIN, LongTriple::EXP_MIN + 1,
        -121, -40, -1, 0, 1, 40, 121,
        LongTriple::EXP_MAX - 1, LongTriple::EXP_MAX
    };

    std::vector<LongTriple> values;
    for (int128_t mantissa : mantissas) {
        if (mantissa == 0) {
            values.push_back(LongTriple{0});
            continue;
        }
        values.push_back(packMantissaExp(encodeBalanced41(mantissa), 40));
    }

    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t j = 0; j < values.size(); ++j) {
            LongTriple got = sandbox::ops::multiply(values[i], values[j]);
            LongTriple want = referenceMultiply(values[i], values[j]);
            expectSameLongTriple(got, want,
                "reference product case " + std::to_string(i) + "," + std::to_string(j));
        }
    }

    const int128_t maxMantissa = static_cast<int128_t>((pow3(41) - 1) / 2);
    const int128_t minNormalized = static_cast<int128_t>((pow3(40) - 1) / 2);
    const std::vector<int128_t> edgeMantissas = {
        -maxMantissa, -minNormalized, -243, -2, -1,
        1, 2, 243, minNormalized, maxMantissa
    };

    std::vector<LongTriple> exponentStress;
    for (int128_t mantissa : edgeMantissas) {
        const auto trits = encodeBalanced41(mantissa);
        for (int exponent : exponents) {
            exponentStress.push_back(packMantissaExp(trits, exponent));
        }
    }

    for (std::size_t i = 0; i < exponentStress.size(); ++i) {
        for (std::size_t j = 0; j < exponentStress.size(); ++j) {
            LongTriple got = sandbox::ops::multiply(exponentStress[i], exponentStress[j]);
            LongTriple want = referenceMultiply(exponentStress[i], exponentStress[j]);
            expectSameLongTriple(got, want,
                "exponent-stress product case " + std::to_string(i) + "," + std::to_string(j));
        }
    }
}

void testZeroAndSpecialCases() {
    std::cout << "[7] zero and special cases\n";

    std::array<int8_t, 41> zeroMantissa{};
    LongTriple packedZero = packMantissaExp(zeroMantissa, 123);
    expect(sandbox::ops::multiply(packedZero, fromInt(999)).isZero(),
           "packed zero mantissa should multiply to canonical zero");

    LongTriple overflow{LongTriple::OVERFLOW_DATA};
    LongTriple underflow{LongTriple::UNDERFLOW_DATA};
    expect(sandbox::ops::multiply(overflow, fromInt(1)).isOverflow(),
           "overflow multiply should propagate overflow sentinel");
    expect(sandbox::ops::multiply(underflow, fromInt(1)).isOverflow(),
           "underflow multiply keeps legacy special-to-overflow contract");
    expect(sandbox::ops::multiply(fromInt(0), overflow).isZero(),
           "canonical zero times special should stay zero");
}

void testVmMulPath() {
    std::cout << "[8] VM arithmetic path\n";

    sandbox::vm::VMState vm(8, 32);
    std::vector<sandbox::isa::TritWord27> program = {
        sandbox::isa::InstructionWord::encodeSemanticI(sandbox::isa::Opcode::MOV,
            sandbox::isa::R1, sandbox::isa::R0_ZERO, 7),
        sandbox::isa::InstructionWord::encodeSemanticI(sandbox::isa::Opcode::MOV,
            sandbox::isa::R2, sandbox::isa::R0_ZERO, -6),
        sandbox::isa::InstructionWord::encodeSemanticR(sandbox::isa::Opcode::MUL,
            sandbox::isa::R3, sandbox::isa::R1, sandbox::isa::R2),
        sandbox::isa::InstructionWord::encodeSemanticB(sandbox::isa::Opcode::HALT,
            sandbox::isa::R0_ZERO, 0),
    };

    expect(sandbox::vm::loadAndReset(vm, program), "program should load");
    const auto run = sandbox::vm::run(vm, 16);
    expect(run.halted(), "VM program should halt");
    expect(sandbox::vm::ops::toLong(vm.regfile.read(sandbox::isa::R3)) == -42,
           "VM MUL should produce -42");
}

} // namespace

int main() {
    LongTriple::initPowTable();

    testBalancedEncoding();
    testBalancedRemainder();
    testExactSmallIntegerMultiply();
    testExactSmallIntegerAddSubtract();
    testExactIntegerDivideAndSqrt();
    testReferenceMultiplyCorpus();
    testZeroAndSpecialCases();
    testVmMulPath();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " native-op test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll native-op tests passed\n";
    return EXIT_SUCCESS;
}
