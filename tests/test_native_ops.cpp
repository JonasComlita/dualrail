#include "ternary_vm.h"
#include "int128_compat.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

void testMathCompatibilityContracts() {
    std::cout << "[8] math compatibility and checked bridge contracts\n";

    for (int a = -1; a <= 1; ++a) {
        for (int b = -1; b <= 1; ++b) {
            for (int carry = -1; carry <= 1; ++carry) {
                const auto [sumTrit, carryOut] = sandbox::ops::addTrit(
                    static_cast<int8_t>(a), static_cast<int8_t>(b),
                    static_cast<int8_t>(carry));
                expect(sumTrit >= -1 && sumTrit <= 1,
                       "full-adder sum trit stays balanced");
                expect(carryOut >= -1 && carryOut <= 1,
                       "full-adder carry stays balanced");
                expect(sumTrit + 3 * carryOut == a + b + carry,
                       "full-adder preserves value for " + std::to_string(a) + "," +
                       std::to_string(b) + "," + std::to_string(carry));
            }
        }
    }

    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    expect(sandbox::ops::fromDouble(infinity).isOverflow(),
           "positive infinity converts to overflow without integer conversion");
    expect(sandbox::ops::fromDouble(-infinity).isOverflow(),
           "negative infinity converts to overflow without integer conversion");
    expect(sandbox::ops::fromDouble(nan).isOverflow(),
           "NaN converts to overflow without integer conversion");

    const auto longValue = sandbox::long_ops::decodeChecked(fromInt(7));
    expect(longValue.status == sandbox::HostDecodeStatus::Value,
           "checked T50 decode identifies ordinary values");
    const auto longZero = sandbox::long_ops::decodeChecked(LongTriple{});
    expect(longZero.status == sandbox::HostDecodeStatus::Zero,
           "checked T50 decode distinguishes zero");
    const auto longOverflow = sandbox::long_ops::decodeChecked(LongTriple::Overflow);
    expect(longOverflow.status == sandbox::HostDecodeStatus::Overflow,
           "checked T50 decode distinguishes overflow");
    const auto longUnderflow = sandbox::long_ops::decodeChecked(LongTriple::Underflow);
    expect(longUnderflow.status == sandbox::HostDecodeStatus::Underflow,
           "checked T50 decode distinguishes underflow");
    const LongTriple invalidLong{LongTriple::validStateCount()};
    const auto longInvalid = sandbox::long_ops::decodeChecked(invalidLong);
    expect(longInvalid.status == sandbox::HostDecodeStatus::Invalid,
           "checked T50 decode distinguishes invalid raw storage");
    expect(std::isnan(sandbox::long_ops::decode(invalidLong).first),
           "legacy T50 decode wrapper does not erase invalid as zero");
    expect(std::isnan(sandbox::long_ops::toDouble(invalidLong)),
           "invalid T50 converts to host NaN");

    const auto tripleZero = sandbox::ops::decodeTripleChecked(sandbox::Triple{});
    expect(tripleZero.status == sandbox::HostDecodeStatus::Zero,
           "checked T40 decode distinguishes zero");
    const auto tripleOverflow = sandbox::ops::decodeTripleChecked(sandbox::Triple::Overflow);
    expect(tripleOverflow.status == sandbox::HostDecodeStatus::Overflow,
           "checked T40 decode distinguishes overflow");
    const auto tripleUnderflow = sandbox::ops::decodeTripleChecked(sandbox::Triple::Underflow);
    expect(tripleUnderflow.status == sandbox::HostDecodeStatus::Underflow,
           "checked T40 decode distinguishes underflow");
    const sandbox::Triple invalidTriple{sandbox::Triple::validStateCount()};
    const auto tripleInvalid = sandbox::ops::decodeTripleChecked(invalidTriple);
    expect(tripleInvalid.status == sandbox::HostDecodeStatus::Invalid,
           "checked T40 decode distinguishes invalid raw storage");
    expect(std::isnan(sandbox::ops::decodeTriple(invalidTriple).first),
           "legacy T40 decode wrapper does not fabricate balanced digits");
    expect(std::isnan(sandbox::ops::toDouble(invalidTriple)),
           "invalid T40 converts to host NaN");
    expect(sandbox::ops::toString(invalidTriple) == "[Invalid ternary encoding]",
           "invalid T40 has an explicit diagnostic string");

    sandbox::ops::TernaryAccumulator shortAccumulator;
    shortAccumulator.add(sandbox::native_ops::fromIntT40(4));
    shortAccumulator.add(sandbox::native_ops::fromIntT40(-1));
    expect(sandbox::native_ops::toLongLong(shortAccumulator.result()) == 3,
           "T40 accumulator uses native ternary addition");
    const auto shortFlushed = shortAccumulator.flush();
    expect(sandbox::native_ops::toLongLong(shortFlushed) == 3,
           "T40 accumulator flush returns its ternary partial");
    expect(shortAccumulator.result().isZero() && shortAccumulator.steps == 0,
           "T40 accumulator flush restores canonical zero");

    sandbox::ops::LongTripleAccumulator accumulator;
    accumulator.add(fromInt(2));
    accumulator.add(fromInt(3));
    expect(sandbox::native_ops::toLongLong(accumulator.result()) == 5,
           "T50 accumulator uses native ternary addition");
    expect(accumulator.steps == 2, "T50 accumulator counts ternary additions");
    accumulator.reset();
    expect(accumulator.result().isZero() && accumulator.steps == 0,
           "T50 accumulator reset restores canonical zero");
    accumulator.add(LongTriple::Underflow);
    expect(accumulator.result().isUnderflow(),
           "T50 accumulator preserves an initial underflow sentinel");
    accumulator.add(fromInt(1));
    expect(accumulator.result().isOverflow(),
           "T50 accumulator propagates special arithmetic instead of dropping it");
    accumulator.reset();
    accumulator.add(invalidLong);
    expect(accumulator.result().isOverflow(),
           "T50 accumulator turns invalid raw input into explicit overflow");

    expect(sandbox::native_ops::compare(LongTriple::Overflow, LongTriple::Overflow) ==
               sandbox::native_ops::RELATION_INVALID,
           "T50 overflow has no fabricated numeric ordering");
    expect(sandbox::native_ops::compare(LongTriple::Underflow, fromInt(1)) ==
               sandbox::native_ops::RELATION_INVALID,
           "T50 underflow comparison reports an invalid relation");
    expect(sandbox::native_ops::compare(invalidLong, fromInt(1)) ==
               sandbox::native_ops::RELATION_INVALID,
           "invalid T50 comparison reports an invalid relation");
    expect(sandbox::native_ops::compare(fromInt(-8), fromInt(-7)) == -1 &&
               sandbox::native_ops::compare(fromInt(7), fromInt(-7)) == 1 &&
               sandbox::native_ops::compare(fromInt(7), fromInt(7)) == 0,
           "finite T50 comparison remains an ordered relation");
    const LongTriple adjacentHigherExponent =
        sandbox::native_ops::detail::packNormalized<sandbox::native_ops::detail::FmtT50>(
            1,
            sandbox::native_ops::detail::mantissaMin<sandbox::native_ops::detail::FmtT50>(),
            1);
    const LongTriple adjacentLowerExponent =
        sandbox::native_ops::detail::packNormalized<sandbox::native_ops::detail::FmtT50>(
            1,
            sandbox::native_ops::detail::mantissaMax<sandbox::native_ops::detail::FmtT50>(),
            0);
    expect(sandbox::native_ops::compare(adjacentHigherExponent, adjacentLowerExponent) == -1 &&
               sandbox::native_ops::compare(adjacentLowerExponent, adjacentHigherExponent) == 1,
           "finite T50 comparison handles overlapping magnitudes at adjacent exponents");

    expect(sandbox::native_ops::exp(LongTriple::Overflow).isOverflow(),
           "exp rejects overflow without entering range-reduction loops");
    expect(sandbox::native_ops::exp(LongTriple::Underflow).isOverflow(),
           "exp rejects underflow without entering range-reduction loops");
    expect(sandbox::native_ops::ln(LongTriple::Overflow).isOverflow(),
           "ln rejects overflow without entering normalization loops");
    expect(sandbox::native_ops::ln(LongTriple::Underflow).isOverflow(),
           "ln rejects underflow without entering normalization loops");
    expect(sandbox::native_ops::ln(invalidLong).isOverflow(),
           "ln rejects invalid raw storage without entering normalization loops");

    expect(sandbox::native_ops::add(invalidLong, fromInt(1)).isOverflow(),
           "invalid T50 arithmetic becomes explicit overflow");
    expect(sandbox::native_ops::toLongTriple(invalidTriple).isOverflow(),
           "invalid T40 promotion becomes explicit T50 overflow");
    expect(sandbox::native_ops::toT40(invalidLong).isOverflow(),
           "invalid T50 narrowing becomes explicit T40 overflow");
    long long checkedInteger = 99;
    expect(!sandbox::native_ops::tryToLongLong(invalidLong, checkedInteger) &&
               checkedInteger == 0,
           "checked integer conversion rejects invalid T50 storage");
    expect(sandbox::native_ops::toLongLong(invalidLong) ==
               std::numeric_limits<long long>::max(),
           "legacy integer conversion no longer erases invalid T50 as zero");
    const sandbox::T5 invalidT5{sandbox::T5::INVALID_DATA};
    expect(sandbox::native_ops::compare(invalidT5, sandbox::native_ops::fromIntT5(0)) ==
               sandbox::native_ops::RELATION_INVALID,
           "invalid T5 comparison reports an invalid relation");

    const LongTriple moderateAngle = fromInt(1000000);
    const LongTriple nativeSin = sandbox::native_ops::sin(moderateAngle);
    const LongTriple nativeCos = sandbox::native_ops::cos(moderateAngle);
    const double nativeSinValue = sandbox::long_ops::toDouble(nativeSin);
    const double nativeCosValue = sandbox::long_ops::toDouble(nativeCos);
    const LongTriple twoPiProbe = sandbox::native_ops::multiply(
        fromInt(2), sandbox::native_ops::pi());
    const LongTriple cyclesProbe = sandbox::native_ops::divide(moderateAngle, twoPiProbe);
    long long wholeCyclesProbe = 0;
    const bool cyclesProbeOk = sandbox::native_ops::tryToLongLong(cyclesProbe, wholeCyclesProbe);
    LongTriple reducedProbe{};
    const bool reducedProbeOk = sandbox::native_ops::reduceAngle(moderateAngle, reducedProbe);
    const std::string reductionProbe =
        " pi=" + std::to_string(sandbox::long_ops::toDouble(sandbox::native_ops::pi())) +
        " cycles=" + std::to_string(wholeCyclesProbe) +
        " reduced=" + std::to_string(sandbox::long_ops::toDouble(reducedProbe)) +
        " checked=" + std::to_string(cyclesProbeOk && reducedProbeOk);
    expect(!nativeSin.isSpecial() && !nativeSin.isInvalid() &&
               std::abs(nativeSinValue - std::sin(1000000.0)) < 1e-8,
           "sin range-reduces a large supported finite argument got=" +
               std::to_string(nativeSinValue) + reductionProbe);
    expect(!nativeCos.isSpecial() && !nativeCos.isInvalid() &&
               std::abs(nativeCosValue - std::cos(1000000.0)) < 1e-8,
           "cos range-reduces a large supported finite argument got=" +
               std::to_string(nativeCosValue) + reductionProbe);
    const LongTriple unreducibleAngle =
        sandbox::native_ops::detail::packNormalized<sandbox::native_ops::detail::FmtT50>(
            1,
            sandbox::native_ops::detail::mantissaMin<sandbox::native_ops::detail::FmtT50>(),
            sandbox::native_ops::detail::FmtT50::exponent_max);
    expect(sandbox::native_ops::sin(unreducibleAngle).isOverflow(),
           "sin explicitly rejects a finite argument beyond reliable range reduction");
    expect(sandbox::native_ops::cos(unreducibleAngle).isOverflow(),
           "cos explicitly rejects a finite argument beyond reliable range reduction");

    sandbox::vm::VMState comparisonVm(4, 16);
    const std::vector<sandbox::isa::TritWord27> comparisonProgram = {
        sandbox::isa::InstructionWord::encodeSemanticR(
            sandbox::isa::Opcode::TCMP,
            sandbox::isa::R3,
            sandbox::isa::R1,
            sandbox::isa::R2,
            sandbox::isa::FUNC_T40),
        sandbox::isa::InstructionWord::encodeSemanticB(
            sandbox::isa::Opcode::HALT,
            sandbox::isa::R0_ZERO,
            0),
    };
    expect(sandbox::vm::loadAndReset(comparisonVm, comparisonProgram),
           "exceptional TCMP regression program loads");
    comparisonVm.regfile.write(
        sandbox::isa::R1,
        sandbox::vm::TernaryValue::fromTriple(sandbox::Triple::Overflow));
    comparisonVm.regfile.write(
        sandbox::isa::R2,
        sandbox::vm::TernaryValue::fromTriple(sandbox::native_ops::fromIntT40(1)));
    const auto comparisonRun = sandbox::vm::run(comparisonVm, 4);
    expect(comparisonRun.trapped() &&
               comparisonRun.trap_code == sandbox::vm::TrapCode::TRAP_ILLEGAL_OP,
           "architectural TCMP traps instead of ordering an exceptional value");

    sandbox::vm::VMState branchVm(4, 16);
    const std::vector<sandbox::isa::TritWord27> branchProgram = {
        sandbox::isa::InstructionWord::encodeSemanticB(
            sandbox::isa::Opcode::BRZ,
            sandbox::isa::R1,
            1),
        sandbox::isa::InstructionWord::encodeSemanticB(
            sandbox::isa::Opcode::HALT,
            sandbox::isa::R0_ZERO,
            0),
    };
    expect(sandbox::vm::loadAndReset(branchVm, branchProgram),
           "exceptional branch regression program loads");
    branchVm.regfile.write(
        sandbox::isa::R1,
        sandbox::vm::TernaryValue::fromTriple(sandbox::Triple::Overflow));
    const auto branchRun = sandbox::vm::run(branchVm, 4);
    expect(branchRun.trapped() &&
               branchRun.trap_code == sandbox::vm::TrapCode::TRAP_ILLEGAL_OP,
           "architectural branch traps instead of treating an exceptional value as zero");
}

void testVmMulPath() {
    std::cout << "[9] VM arithmetic path\n";

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
    testMathCompatibilityContracts();
    testVmMulPath();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " native-op test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll native-op tests passed\n";
    return EXIT_SUCCESS;
}
