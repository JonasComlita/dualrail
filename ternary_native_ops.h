// =============================================================================
// ternary_native_ops.h  -  Native balanced-ternary arithmetic
// =============================================================================
//
// Included from ternary_math.h after the concrete ternary storage types are
// defined. All production algorithms here are bridge-free: no binary FPU
// decode/encode path is used for arithmetic.

#pragma once
#ifndef TERNARY_NATIVE_OPS_H
#define TERNARY_NATIVE_OPS_H

#ifndef TERNARY_MATH_H
#error "ternary_native_ops.h is included by ternary_math.h after numeric types are defined."
#endif

#include <array>
#include <cstdint>
#include <limits>

namespace sandbox {
namespace native_ops {
namespace detail {

constexpr int pow3Int(int n) {
    int value = 1;
    for (int i = 0; i < n; ++i) value *= 3;
    return value;
}

[[nodiscard]] inline UInt128 pow3UInt128(int n) {
    UInt128 value = 1;
    for (int i = 0; i < n; ++i) value *= 3;
    return value;
}

[[nodiscard]] inline UInt128 pow3(int n) {
    return pow3UInt128(n);
}

[[nodiscard]] inline int8_t balancedRem(long long n) {
    int r = static_cast<int>(n % 3);
    if (r > 1) r -= 3;
    if (r < -1) r += 3;
    return static_cast<int8_t>(r);
}

[[nodiscard]] inline int8_t balancedRem(UInt128 n) {
    const unsigned rem = static_cast<unsigned>(n % 3);
    if (rem == 0) return 0;
    if (rem == 1) return 1;
    return -1;
}

[[nodiscard]] inline int8_t balancedRem(Int128 n) {
    if (n.isZero()) return 0;
    int r = static_cast<int>(n.magnitude % 3);
    if (n.sign < 0) r = -r;
    if (r > 1) r -= 3;
    if (r < -1) r += 3;
    return static_cast<int8_t>(r);
}

[[nodiscard]] inline UInt128 roundedDiv3(UInt128 n) {
    uint32_t rem = 0;
    UInt128 quotient = n.divMod3(rem);
    if (rem == 2) quotient += UInt128{1};
    return quotient;
}

[[nodiscard]] inline Int128 roundedDiv3Signed(Int128 n) {
    const int sign = signOf(n);
    if (sign == 0) return Int128{};
    return Int128::fromMagnitude(sign, roundedDiv3(absUnsigned(n)));
}

[[nodiscard]] inline Int128 roundedDivPow3Signed(Int128 n, int power) {
    for (int i = 0; i < power && !n.isZero(); ++i) {
        n = roundedDiv3Signed(n);
    }
    return n;
}

[[nodiscard]] inline UInt128 roundedDivide(
    UInt128 numerator,
    UInt128 denominator) {

    if (denominator == UInt128{0}) return UInt128{0};
    UInt128 quotient = numerator / denominator;
    const UInt128 remainder = numerator % denominator;
    if (remainder * 2 >= denominator) quotient += UInt128{1};
    return quotient;
}

[[nodiscard]] inline UInt128 isqrt(UInt128 n) {
    if (n == UInt128{0}) return UInt128{0};
    UInt128 x = n;
    UInt128 y = (x + UInt128{1}) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

template<int Trits, typename StorageT, typename ValueT>
struct TernaryIntFormat {
    using storage_type = StorageT;
    using value_type = ValueT;
    static constexpr int trits = Trits;
    static constexpr int states = pow3Int(Trits);
    static constexpr int max_value = (states - 1) / 2;
    static constexpr int min_value = -max_value;
};

struct FmtT1 : TernaryIntFormat<1, uint8_t, T1> {};
struct FmtT5 : TernaryIntFormat<5, uint8_t, T5> {};

template<class ValueT,
         int TotalTrits,
         int MantissaTrits,
         int ExponentTrits,
         int GuardTrits,
         typename StorageT,
         typename SignedAccumT,
         typename UnsignedAccumT>
struct TernaryFloatFormat {
    using value_type = ValueT;
    using storage_type = StorageT;
    using signed_accumulator_type = SignedAccumT;
    using unsigned_accumulator_type = UnsignedAccumT;
    static constexpr int total_trits = TotalTrits;
    static constexpr int mantissa_trits = MantissaTrits;
    static constexpr int exponent_trits = ExponentTrits;
    static constexpr int exponent_lsb = MantissaTrits;
    static constexpr int guard_trits = GuardTrits;
    static constexpr int product_trits = 2 * MantissaTrits + GuardTrits;
    static constexpr int division_scale_trits = MantissaTrits - 1;
    static constexpr int exponent_max = (pow3Int(ExponentTrits) - 1) / 2;
    static constexpr int exponent_min = -exponent_max;
};

struct FmtT10 : TernaryFloatFormat<T10, 10, 6, 4, 4,
                                  uint16_t, int32_t, uint32_t> {};
struct FmtT20 : TernaryFloatFormat<T20, 20, 14, 6, 5,
                                  uint32_t, int64_t, uint64_t> {};
struct FmtT40 : TernaryFloatFormat<Triple, 40, 33, 7, 6,
                                  uint64_t, Int128, UInt128> {};
struct FmtT50 : TernaryFloatFormat<LongTriple, 50, 41, 9, 14,
                                  UInt128, Int128, UInt128> {};

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type makeIntInvalid() {
    return typename Fmt::value_type{
        static_cast<typename Fmt::storage_type>(Fmt::value_type::INVALID_DATA)
    };
}

template<class Fmt>
[[nodiscard]] inline bool intInvalid(typename Fmt::value_type value) {
    return value.isInvalid();
}

template<class Fmt>
[[nodiscard]] inline long long intToSigned(typename Fmt::value_type value) {
    if (intInvalid<Fmt>(value)) return 0;
    long long out = 0;
    long long place = 1;
    typename Fmt::storage_type temp = value.data;
    for (int i = 0; i < Fmt::trits; ++i) {
        out += (static_cast<int>(temp % 3) - 1) * place;
        temp /= 3;
        place *= 3;
    }
    return out;
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intFromSigned(long long value) {
    if (value < Fmt::min_value || value > Fmt::max_value) {
        return makeIntInvalid<Fmt>();
    }

    typename Fmt::storage_type raw = 0;
    typename Fmt::storage_type place = 1;
    long long n = value;
    for (int i = 0; i < Fmt::trits; ++i) {
        const int8_t trit = balancedRem(n);
        raw += static_cast<typename Fmt::storage_type>(trit + 1) * place;
        n = (n - trit) / 3;
        if (i + 1 < Fmt::trits) place *= 3;
    }
    if (n != 0) return makeIntInvalid<Fmt>();
    return typename Fmt::value_type{raw};
}

template<class Fmt>
[[nodiscard]] inline int8_t intSign(typename Fmt::value_type value) {
    const long long signedValue = intToSigned<Fmt>(value);
    return static_cast<int8_t>((signedValue > 0) - (signedValue < 0));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intAdd(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (intInvalid<Fmt>(a) || intInvalid<Fmt>(b)) return makeIntInvalid<Fmt>();
    return intFromSigned<Fmt>(intToSigned<Fmt>(a) + intToSigned<Fmt>(b));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intSubtract(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (intInvalid<Fmt>(a) || intInvalid<Fmt>(b)) return makeIntInvalid<Fmt>();
    return intFromSigned<Fmt>(intToSigned<Fmt>(a) - intToSigned<Fmt>(b));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intMultiply(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (intInvalid<Fmt>(a) || intInvalid<Fmt>(b)) return makeIntInvalid<Fmt>();
    return intFromSigned<Fmt>(intToSigned<Fmt>(a) * intToSigned<Fmt>(b));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intDivide(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (intInvalid<Fmt>(a) || intInvalid<Fmt>(b)) return makeIntInvalid<Fmt>();
    const long long bv = intToSigned<Fmt>(b);
    if (bv == 0) return makeIntInvalid<Fmt>();
    return intFromSigned<Fmt>(intToSigned<Fmt>(a) / bv);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intSqrt(typename Fmt::value_type a) {
    if (intInvalid<Fmt>(a)) return makeIntInvalid<Fmt>();
    const long long av = intToSigned<Fmt>(a);
    if (av < 0) return makeIntInvalid<Fmt>();
    return intFromSigned<Fmt>(toLongLongSaturated(
        Int128::fromMagnitude(1, isqrt(UInt128{static_cast<uint64_t>(av)}))));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intNegate(typename Fmt::value_type a) {
    if (intInvalid<Fmt>(a)) return makeIntInvalid<Fmt>();
    return intFromSigned<Fmt>(-intToSigned<Fmt>(a));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type intAbs(typename Fmt::value_type a) {
    if (intInvalid<Fmt>(a)) return makeIntInvalid<Fmt>();
    const long long av = intToSigned<Fmt>(a);
    return intFromSigned<Fmt>(av < 0 ? -av : av);
}

template<class Fmt>
[[nodiscard]] inline int decodeExponent(
    const std::array<int8_t, Fmt::total_trits>& trits) {
    int exponent = 0;
    int place = 1;
    for (int i = 0; i < Fmt::exponent_trits; ++i) {
        exponent += trits[Fmt::exponent_lsb + i] * place;
        place *= 3;
    }
    return exponent;
}

template<class Fmt>
inline void encodeExponent(int exponent, std::array<int8_t, Fmt::total_trits>& out) {
    for (int i = 0; i < Fmt::exponent_trits; ++i) {
        const int8_t trit = balancedRem(static_cast<long long>(exponent));
        out[Fmt::exponent_lsb + i] = trit;
        exponent = (exponent - trit) / 3;
    }
}

template<class Fmt>
[[nodiscard]] inline Int128 mantissaToInt(
    const std::array<int8_t, Fmt::total_trits>& trits) {

    Int128 value{};
    UInt128 place = 1;
    for (int i = 0; i < Fmt::mantissa_trits; ++i) {
        if (trits[i] != 0) {
            value = value + Int128::fromMagnitude(trits[i], place);
        }
        place *= 3;
    }
    return value;
}

template<class Fmt>
[[nodiscard]] inline UInt128 mantissaMax() {
    return (pow3(Fmt::mantissa_trits) - UInt128{1}) / 2;
}

template<class Fmt>
[[nodiscard]] inline UInt128 mantissaMin() {
    return (pow3(Fmt::mantissa_trits - 1) - UInt128{1}) / 2;
}

template<class Fmt>
[[nodiscard]] inline std::array<int8_t, Fmt::product_trits> multiplyMantissas(
    const std::array<int8_t, Fmt::total_trits>& a,
    const std::array<int8_t, Fmt::total_trits>& b) {

    std::array<int, Fmt::product_trits> accum{};
    for (int i = 0; i < Fmt::mantissa_trits; ++i) {
        for (int j = 0; j < Fmt::mantissa_trits; ++j) {
            accum[i + j] += a[i] * b[j];
        }
    }

    std::array<int8_t, Fmt::product_trits> product{};
    long long carry = 0;
    for (int i = 0; i < Fmt::product_trits; ++i) {
        const long long total = static_cast<long long>(accum[i]) + carry;
        const int8_t trit = balancedRem(total);
        product[i] = trit;
        carry = (total - trit) / 3;
    }
    return product;
}

template<class Fmt>
[[nodiscard]] inline int signOfProduct(
    const std::array<int8_t, Fmt::product_trits>& trits) {
    for (int i = Fmt::product_trits - 1; i >= 0; --i) {
        if (trits[i] < 0) return -1;
        if (trits[i] > 0) return 1;
    }
    return 0;
}

template<class Fmt>
[[nodiscard]] inline UInt128 magnitudeOfProduct(
    const std::array<int8_t, Fmt::product_trits>& trits,
    int sign) {

    UInt128 magnitude = 0;
    bool seenNonZero = false;
    for (int i = Fmt::product_trits - 1; i >= 0; --i) {
        const int digit = sign * trits[i];
        if (!seenNonZero) {
            if (digit == 0) continue;
            seenNonZero = true;
        }
        magnitude *= 3;
        if (digit > 0) magnitude += UInt128{static_cast<uint64_t>(digit)};
        else if (digit < 0) magnitude -= UInt128{static_cast<uint64_t>(-digit)};
    }
    return magnitude;
}

template<class Fmt>
inline void encodeMantissa(
    UInt128 magnitude,
    int sign,
    std::array<int8_t, Fmt::total_trits>& out) {

    for (int i = 0; i < Fmt::mantissa_trits; ++i) {
        uint32_t rem = 0;
        UInt128 quotient = magnitude.divMod3(rem);
        const int8_t trit = rem == 2
            ? static_cast<int8_t>(-1)
            : static_cast<int8_t>(rem);
        out[i] = static_cast<int8_t>(sign * trit);
        magnitude = trit < 0 ? quotient + UInt128{1} : quotient;
    }
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type overflowValue() {
    return typename Fmt::value_type{Fmt::value_type::OVERFLOW_DATA};
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type underflowValue() {
    return typename Fmt::value_type{Fmt::value_type::UNDERFLOW_DATA};
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type packNormalized(
    int sign,
    UInt128 magnitude,
    int exponent) {

    if (sign == 0 || magnitude == UInt128{0}) return typename Fmt::value_type{0};

    while (magnitude > mantissaMax<Fmt>()) {
        magnitude = roundedDiv3(magnitude);
        ++exponent;
    }
    while (magnitude != UInt128{0} && magnitude < mantissaMin<Fmt>()) {
        magnitude *= 3;
        --exponent;
    }

    if (magnitude == UInt128{0}) return typename Fmt::value_type{0};
    if (exponent > Fmt::exponent_max) return overflowValue<Fmt>();
    if (exponent < Fmt::exponent_min) return underflowValue<Fmt>();

    std::array<int8_t, Fmt::total_trits> result{};
    encodeMantissa<Fmt>(magnitude, sign, result);
    encodeExponent<Fmt>(exponent, result);
    return Fmt::value_type::pack(result);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type packSigned(Int128 mantissa, int exponent) {
    return packNormalized<Fmt>(signOf(mantissa), absUnsigned(mantissa), exponent);
}

template<class Fmt>
inline void normalizeParts(Int128& mantissa, int& exponent) {
    const int sign = signOf(mantissa);
    if (sign == 0) return;

    UInt128 magnitude = absUnsigned(mantissa);
    while (magnitude > mantissaMax<Fmt>()) {
        magnitude = roundedDiv3(magnitude);
        ++exponent;
    }
    while (magnitude != UInt128{0} && magnitude < mantissaMin<Fmt>()) {
        magnitude *= 3;
        --exponent;
    }

    mantissa = Int128::fromMagnitude(sign, magnitude);
}

template<class SrcFmt, class DstFmt>
[[nodiscard]] inline typename DstFmt::value_type convertFloat(
    typename SrcFmt::value_type value) {

    if (value.isZero()) return typename DstFmt::value_type{0};
    if (value.isOverflow()) return overflowValue<DstFmt>();
    if (value.isUnderflow()) return underflowValue<DstFmt>();

    const auto trits = value.unpack();
    const Int128 mantissa = mantissaToInt<SrcFmt>(trits);
    const int exponent = decodeExponent<SrcFmt>(trits)
        - (SrcFmt::mantissa_trits - 1)
        + (DstFmt::mantissa_trits - 1);
    return packSigned<DstFmt>(mantissa, exponent);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatFromInt(long long value) {
    return packSigned<Fmt>(Int128::fromLongLong(value), Fmt::mantissa_trits - 1);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatFromWide(Int128 value) {
    return packSigned<Fmt>(value, Fmt::mantissa_trits - 1);
}

template<class Fmt>
[[nodiscard]] inline int8_t floatSign(typename Fmt::value_type t) {
    if (t.isZero()) return 0;
    if (t.isSpecial()) return 1;
    const Int128 mantissa = mantissaToInt<Fmt>(t.unpack());
    return static_cast<int8_t>(signOf(mantissa));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatAdd(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (a.isZero()) return b;
    if (b.isZero()) return a;
    if (a.isSpecial() || b.isSpecial()) return overflowValue<Fmt>();

    const auto aTrits = a.unpack();
    const auto bTrits = b.unpack();
    Int128 ma = mantissaToInt<Fmt>(aTrits);
    Int128 mb = mantissaToInt<Fmt>(bTrits);
    int ea = decodeExponent<Fmt>(aTrits);
    int eb = decodeExponent<Fmt>(bTrits);

    if (ma.isZero()) return b;
    if (mb.isZero()) return a;

    normalizeParts<Fmt>(ma, ea);
    normalizeParts<Fmt>(mb, eb);

    if (ea >= eb) {
        return packSigned<Fmt>(ma + roundedDivPow3Signed(mb, ea - eb), ea);
    }
    return packSigned<Fmt>(roundedDivPow3Signed(ma, eb - ea) + mb, eb);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatNegate(typename Fmt::value_type a) {
    if (a.isZero() || a.isSpecial()) return a;
    auto trits = a.unpack();
    for (int i = 0; i < Fmt::mantissa_trits; ++i) trits[i] = -trits[i];
    return Fmt::value_type::pack(trits);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatSubtract(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    return floatAdd<Fmt>(a, floatNegate<Fmt>(b));
}

template<class Fmt>
[[nodiscard]] inline int8_t floatCompare(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    return floatSign<Fmt>(floatSubtract<Fmt>(a, b));
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatMultiply(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (a.isZero() || b.isZero()) return typename Fmt::value_type{0};
    if (a.isSpecial() || b.isSpecial()) return overflowValue<Fmt>();

    const auto aTrits = a.unpack();
    const auto bTrits = b.unpack();
    const Int128 ma = mantissaToInt<Fmt>(aTrits);
    const Int128 mb = mantissaToInt<Fmt>(bTrits);
    if (ma.isZero() || mb.isZero()) return typename Fmt::value_type{0};

    const auto productTrits = multiplyMantissas<Fmt>(aTrits, bTrits);
    const int sign = signOfProduct<Fmt>(productTrits);
    if (sign == 0) return typename Fmt::value_type{0};

    const UInt128 magnitude = magnitudeOfProduct<Fmt>(productTrits, sign);
    const int exponent = decodeExponent<Fmt>(aTrits) + decodeExponent<Fmt>(bTrits)
        - (Fmt::mantissa_trits - 1);
    return packNormalized<Fmt>(sign, magnitude, exponent);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatDivide(
    typename Fmt::value_type a, typename Fmt::value_type b) {
    if (b.isZero()) return overflowValue<Fmt>();
    if (a.isZero()) return typename Fmt::value_type{0};
    if (a.isSpecial() || b.isSpecial()) return overflowValue<Fmt>();

    const auto aTrits = a.unpack();
    const auto bTrits = b.unpack();
    const Int128 ma = mantissaToInt<Fmt>(aTrits);
    const Int128 mb = mantissaToInt<Fmt>(bTrits);
    if (mb.isZero()) return overflowValue<Fmt>();
    if (ma.isZero()) return typename Fmt::value_type{0};

    const int sign = signOf(ma) * signOf(mb);
    const UInt128 numerator = absUnsigned(ma) * pow3(Fmt::division_scale_trits);
    const UInt128 quotient = roundedDivide(numerator, absUnsigned(mb));
    const int exponent = decodeExponent<Fmt>(aTrits) - decodeExponent<Fmt>(bTrits);
    return packNormalized<Fmt>(sign, quotient, exponent);
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatSqrt(typename Fmt::value_type t) {
    if (t.isZero()) return typename Fmt::value_type{0};
    if (t.isSpecial()) return overflowValue<Fmt>();

    const auto trits = t.unpack();
    const Int128 mantissa = mantissaToInt<Fmt>(trits);
    if (signOf(mantissa) < 0) return overflowValue<Fmt>();
    if (mantissa.isZero()) return typename Fmt::value_type{0};

    UInt128 magnitude = absUnsigned(mantissa);
    int scaleExponent = decodeExponent<Fmt>(trits) - (Fmt::mantissa_trits - 1);
    if ((scaleExponent % 2) != 0) {
        magnitude *= 3;
        --scaleExponent;
    }

    UInt128 seed = isqrt(magnitude);
    if (seed == UInt128{0}) seed = UInt128{1};

    typename Fmt::value_type x =
        packNormalized<Fmt>(1, seed, scaleExponent / 2 + (Fmt::mantissa_trits - 1));
    const auto two = floatFromInt<Fmt>(2);

    for (int i = 0; i < 8; ++i) {
        auto next = floatDivide<Fmt>(floatAdd<Fmt>(x, floatDivide<Fmt>(t, x)), two);
        if (next.data == x.data) break;
        x = next;
    }
    return x;
}

template<class Fmt>
[[nodiscard]] inline typename Fmt::value_type floatAbs(typename Fmt::value_type t) {
    return floatSign<Fmt>(t) < 0 ? floatNegate<Fmt>(t) : t;
}

template<class Fmt>
[[nodiscard]] inline long long floatToLongLong(typename Fmt::value_type t) {
    if (t.isZero() || t.isSpecial()) return 0LL;

    const auto trits = t.unpack();
    Int128 mantissa = mantissaToInt<Fmt>(trits);
    const int exponent = decodeExponent<Fmt>(trits);
    Int128 value{};
    const int radix = Fmt::mantissa_trits - 1;

    if (exponent >= radix) {
        const int shift = exponent - radix;
        if (shift >= Fmt::mantissa_trits) {
            return signOf(mantissa) >= 0
                ? std::numeric_limits<long long>::max()
                : std::numeric_limits<long long>::min();
        }
        value = mantissa * pow3(shift);
    } else {
        const int shift = radix - exponent;
        if (shift >= Fmt::mantissa_trits + 8) return 0LL;
        value = mantissa / pow3(shift);
    }

    return toLongLongSaturated(value);
}

} // namespace detail

[[nodiscard]] inline T1 fromIntT1(long long value) {
    return detail::intFromSigned<detail::FmtT1>(value);
}

[[nodiscard]] inline T5 fromIntT5(long long value) {
    return detail::intFromSigned<detail::FmtT5>(value);
}

[[nodiscard]] inline T10 fromIntT10(long long value) {
    return detail::floatFromInt<detail::FmtT10>(value);
}

[[nodiscard]] inline T20 fromIntT20(long long value) {
    return detail::floatFromInt<detail::FmtT20>(value);
}

[[nodiscard]] inline Triple fromIntT40(long long value) {
    return detail::floatFromInt<detail::FmtT40>(value);
}

[[nodiscard]] inline LongTriple fromInt(long long value) {
    return detail::floatFromInt<detail::FmtT50>(value);
}

[[nodiscard]] inline LongTriple fromInt128(Int128 value) {
    return detail::floatFromWide<detail::FmtT50>(value);
}

[[nodiscard]] inline bool isInvalid(T1 value) { return detail::intInvalid<detail::FmtT1>(value); }
[[nodiscard]] inline bool isInvalid(T5 value) { return detail::intInvalid<detail::FmtT5>(value); }
[[nodiscard]] inline bool isInvalid(T10 value) { return value.data >= 59049u && !value.isSpecial(); }
[[nodiscard]] inline bool isInvalid(T20 value) { return value.data >= 3486784401u && !value.isSpecial(); }
[[nodiscard]] inline bool isInvalid(Triple value) { return value.data >= detail::pow3(40) && !value.isSpecial(); }
[[nodiscard]] inline bool isInvalid(LongTriple value) { return value.data >= detail::pow3UInt128(50) && !value.isSpecial(); }

[[nodiscard]] inline int8_t sign(T1 t) { return detail::intSign<detail::FmtT1>(t); }
[[nodiscard]] inline int8_t sign(T5 t) { return detail::intSign<detail::FmtT5>(t); }
[[nodiscard]] inline int8_t sign(T10 t) { return detail::floatSign<detail::FmtT10>(t); }
[[nodiscard]] inline int8_t sign(T20 t) { return detail::floatSign<detail::FmtT20>(t); }
[[nodiscard]] inline int8_t sign(Triple t) { return detail::floatSign<detail::FmtT40>(t); }
[[nodiscard]] inline int8_t sign(LongTriple t) { return detail::floatSign<detail::FmtT50>(t); }

[[nodiscard]] inline T1 add(T1 a, T1 b) { return detail::intAdd<detail::FmtT1>(a, b); }
[[nodiscard]] inline T5 add(T5 a, T5 b) { return detail::intAdd<detail::FmtT5>(a, b); }
[[nodiscard]] inline T10 add(T10 a, T10 b) { return detail::floatAdd<detail::FmtT10>(a, b); }
[[nodiscard]] inline T20 add(T20 a, T20 b) { return detail::floatAdd<detail::FmtT20>(a, b); }
[[nodiscard]] inline Triple add(Triple a, Triple b) { return detail::floatAdd<detail::FmtT40>(a, b); }
[[nodiscard]] inline LongTriple add(LongTriple a, LongTriple b) { return detail::floatAdd<detail::FmtT50>(a, b); }

[[nodiscard]] inline T1 negate(T1 a) { return detail::intNegate<detail::FmtT1>(a); }
[[nodiscard]] inline T5 negate(T5 a) { return detail::intNegate<detail::FmtT5>(a); }
[[nodiscard]] inline T10 negate(T10 a) { return detail::floatNegate<detail::FmtT10>(a); }
[[nodiscard]] inline T20 negate(T20 a) { return detail::floatNegate<detail::FmtT20>(a); }
[[nodiscard]] inline Triple negate(Triple a) { return detail::floatNegate<detail::FmtT40>(a); }
[[nodiscard]] inline LongTriple negate(LongTriple a) { return detail::floatNegate<detail::FmtT50>(a); }

[[nodiscard]] inline T1 subtract(T1 a, T1 b) { return detail::intSubtract<detail::FmtT1>(a, b); }
[[nodiscard]] inline T5 subtract(T5 a, T5 b) { return detail::intSubtract<detail::FmtT5>(a, b); }
[[nodiscard]] inline T10 subtract(T10 a, T10 b) { return detail::floatSubtract<detail::FmtT10>(a, b); }
[[nodiscard]] inline T20 subtract(T20 a, T20 b) { return detail::floatSubtract<detail::FmtT20>(a, b); }
[[nodiscard]] inline Triple subtract(Triple a, Triple b) { return detail::floatSubtract<detail::FmtT40>(a, b); }
[[nodiscard]] inline LongTriple subtract(LongTriple a, LongTriple b) { return detail::floatSubtract<detail::FmtT50>(a, b); }

[[nodiscard]] inline T1 multiply(T1 a, T1 b) { return detail::intMultiply<detail::FmtT1>(a, b); }
[[nodiscard]] inline T5 multiply(T5 a, T5 b) { return detail::intMultiply<detail::FmtT5>(a, b); }
[[nodiscard]] inline T10 multiply(T10 a, T10 b) { return detail::floatMultiply<detail::FmtT10>(a, b); }
[[nodiscard]] inline T20 multiply(T20 a, T20 b) { return detail::floatMultiply<detail::FmtT20>(a, b); }
[[nodiscard]] inline Triple multiply(Triple a, Triple b) { return detail::floatMultiply<detail::FmtT40>(a, b); }
[[nodiscard]] inline LongTriple multiply(LongTriple a, LongTriple b) { return detail::floatMultiply<detail::FmtT50>(a, b); }

[[nodiscard]] inline T1 divide(T1 a, T1 b) { return detail::intDivide<detail::FmtT1>(a, b); }
[[nodiscard]] inline T5 divide(T5 a, T5 b) { return detail::intDivide<detail::FmtT5>(a, b); }
[[nodiscard]] inline T10 divide(T10 a, T10 b) { return detail::floatDivide<detail::FmtT10>(a, b); }
[[nodiscard]] inline T20 divide(T20 a, T20 b) { return detail::floatDivide<detail::FmtT20>(a, b); }
[[nodiscard]] inline Triple divide(Triple a, Triple b) { return detail::floatDivide<detail::FmtT40>(a, b); }
[[nodiscard]] inline LongTriple divide(LongTriple a, LongTriple b) { return detail::floatDivide<detail::FmtT50>(a, b); }

[[nodiscard]] inline T1 sqrt(T1 a) { return detail::intSqrt<detail::FmtT1>(a); }
[[nodiscard]] inline T5 sqrt(T5 a) { return detail::intSqrt<detail::FmtT5>(a); }
[[nodiscard]] inline T10 sqrt(T10 a) { return detail::floatSqrt<detail::FmtT10>(a); }
[[nodiscard]] inline T20 sqrt(T20 a) { return detail::floatSqrt<detail::FmtT20>(a); }
[[nodiscard]] inline Triple sqrt(Triple a) { return detail::floatSqrt<detail::FmtT40>(a); }
[[nodiscard]] inline LongTriple sqrt(LongTriple a) { return detail::floatSqrt<detail::FmtT50>(a); }

[[nodiscard]] inline T1 abs(T1 a) { return detail::intAbs<detail::FmtT1>(a); }
[[nodiscard]] inline T5 abs(T5 a) { return detail::intAbs<detail::FmtT5>(a); }
[[nodiscard]] inline T10 abs(T10 a) { return detail::floatAbs<detail::FmtT10>(a); }
[[nodiscard]] inline T20 abs(T20 a) { return detail::floatAbs<detail::FmtT20>(a); }
[[nodiscard]] inline Triple abs(Triple a) { return detail::floatAbs<detail::FmtT40>(a); }
[[nodiscard]] inline LongTriple abs(LongTriple a) { return detail::floatAbs<detail::FmtT50>(a); }

[[nodiscard]] inline int8_t compare(T1 a, T1 b) {
    const long long av = detail::intToSigned<detail::FmtT1>(a);
    const long long bv = detail::intToSigned<detail::FmtT1>(b);
    return static_cast<int8_t>((av > bv) - (av < bv));
}
[[nodiscard]] inline int8_t compare(T5 a, T5 b) {
    const long long av = detail::intToSigned<detail::FmtT5>(a);
    const long long bv = detail::intToSigned<detail::FmtT5>(b);
    return static_cast<int8_t>((av > bv) - (av < bv));
}
[[nodiscard]] inline int8_t compare(T10 a, T10 b) { return detail::floatCompare<detail::FmtT10>(a, b); }
[[nodiscard]] inline int8_t compare(T20 a, T20 b) { return detail::floatCompare<detail::FmtT20>(a, b); }
[[nodiscard]] inline int8_t compare(Triple a, Triple b) { return detail::floatCompare<detail::FmtT40>(a, b); }
[[nodiscard]] inline int8_t compare(LongTriple a, LongTriple b) { return detail::floatCompare<detail::FmtT50>(a, b); }

[[nodiscard]] inline long long toLongLong(T1 t) {
    return static_cast<long long>(detail::intToSigned<detail::FmtT1>(t));
}
[[nodiscard]] inline long long toLongLong(T5 t) {
    return static_cast<long long>(detail::intToSigned<detail::FmtT5>(t));
}
[[nodiscard]] inline long long toLongLong(T10 t) { return detail::floatToLongLong<detail::FmtT10>(t); }
[[nodiscard]] inline long long toLongLong(T20 t) { return detail::floatToLongLong<detail::FmtT20>(t); }
[[nodiscard]] inline long long toLongLong(Triple t) { return detail::floatToLongLong<detail::FmtT40>(t); }
[[nodiscard]] inline long long toLongLong(LongTriple t) { return detail::floatToLongLong<detail::FmtT50>(t); }

[[nodiscard]] inline LongTriple toLongTriple(T1 value) {
    return fromInt(toLongLong(value));
}
[[nodiscard]] inline LongTriple toLongTriple(T5 value) {
    return fromInt(toLongLong(value));
}
[[nodiscard]] inline LongTriple toLongTriple(T10 value) {
    return detail::convertFloat<detail::FmtT10, detail::FmtT50>(value);
}
[[nodiscard]] inline LongTriple toLongTriple(T20 value) {
    return detail::convertFloat<detail::FmtT20, detail::FmtT50>(value);
}
[[nodiscard]] inline LongTriple toLongTriple(Triple value) {
    return detail::convertFloat<detail::FmtT40, detail::FmtT50>(value);
}
[[nodiscard]] inline LongTriple toLongTriple(LongTriple value) {
    return value;
}

[[nodiscard]] inline T10 toT10(LongTriple value) {
    return detail::convertFloat<detail::FmtT50, detail::FmtT10>(value);
}
[[nodiscard]] inline T20 toT20(LongTriple value) {
    return detail::convertFloat<detail::FmtT50, detail::FmtT20>(value);
}
[[nodiscard]] inline Triple toT40(LongTriple value) {
    return detail::convertFloat<detail::FmtT50, detail::FmtT40>(value);
}

[[nodiscard]] inline LongTriple fromRatio(long long numerator, long long denominator) {
    return divide(fromInt(numerator), fromInt(denominator));
}

[[nodiscard]] inline LongTriple exp(LongTriple x) {
    if (x.isZero()) return fromInt(1);
    if (sign(x) < 0) return divide(fromInt(1), exp(negate(x)));

    const LongTriple three = fromInt(3);
    int powerOfThree = 0;
    while (compare(x, three) == 1) {
        x = divide(x, three);
        ++powerOfThree;
    }

    LongTriple sum = fromInt(1);
    LongTriple term = fromInt(1);
    for (int k = 1; k <= 22; ++k) {
        term = divide(multiply(term, x), fromInt(k));
        sum = add(sum, term);
    }
    for (int i = 0; i < powerOfThree; ++i) {
        sum = multiply(multiply(sum, sum), sum);
    }
    return sum;
}

[[nodiscard]] inline LongTriple ln3() {
    const LongTriple three = fromInt(3);
    const LongTriple one = fromInt(1);
    const LongTriple y = divide(subtract(three, one), add(three, one));
    const LongTriple y2 = multiply(y, y);
    LongTriple term = y;
    LongTriple sum = y;
    for (int k = 1; k <= 30; ++k) {
        term = multiply(term, y2);
        sum = add(sum, divide(term, fromInt(2 * k + 1)));
    }
    return multiply(fromInt(2), sum);
}

[[nodiscard]] inline LongTriple ln(LongTriple x) {
    if (x.isZero() || sign(x) < 0) return LongTriple{LongTriple::OVERFLOW_DATA};

    const LongTriple one = fromInt(1);
    const LongTriple two = fromInt(2);
    const LongTriple three = fromInt(3);
    const LongTriple low = divide(two, three);
    const LongTriple high = divide(three, two);

    int powerOfThree = 0;
    while (compare(x, high) == 1) {
        x = divide(x, three);
        ++powerOfThree;
    }
    while (compare(x, low) == -1) {
        x = multiply(x, three);
        --powerOfThree;
    }

    const LongTriple y = divide(subtract(x, one), add(x, one));
    const LongTriple y2 = multiply(y, y);
    LongTriple term = y;
    LongTriple sum = y;
    for (int k = 1; k <= 28; ++k) {
        term = multiply(term, y2);
        sum = add(sum, divide(term, fromInt(2 * k + 1)));
    }
    LongTriple result = multiply(two, sum);
    if (powerOfThree != 0) {
        result = add(result, multiply(fromInt(powerOfThree), ln3()));
    }
    return result;
}

[[nodiscard]] inline LongTriple arctan_series(LongTriple x, int terms) {
    LongTriple x2  = multiply(x, x);
    LongTriple term = x;
    LongTriple sum  = x;
    for (int k = 1; k <= terms; ++k) {
        term = negate(divide(multiply(term, x2), fromInt(2 * k + 1)));
        sum  = add(sum, term);
    }
    return sum;
}

[[nodiscard]] inline LongTriple pi() {
    LongTriple a = arctan_series(fromRatio(1, 5),   28);
    LongTriple b = arctan_series(fromRatio(1, 239),  12);
    return multiply(fromInt(4), subtract(multiply(fromInt(4), a), b));
}

[[nodiscard]] inline LongTriple sin(LongTriple x) {
    const LongTriple twoPi = multiply(fromInt(2), pi());
    LongTriple cycles = divide(x, twoPi);
    long long n = toLongLong(cycles);
    x = subtract(x, multiply(fromInt(n), twoPi));
    if (compare(x, pi()) == 1)  x = subtract(x, twoPi);
    if (compare(x, negate(pi())) == -1) x = add(x, twoPi);

    const LongTriple x2 = multiply(x, x);
    LongTriple term = x;
    LongTriple sum = x;
    for (int k = 1; k <= 14; ++k) {
        const int denominator = (2 * k) * (2 * k + 1);
        term = negate(divide(multiply(term, x2), fromInt(denominator)));
        sum = add(sum, term);
    }
    return sum;
}

[[nodiscard]] inline LongTriple cos(LongTriple x) {
    const LongTriple twoPi = multiply(fromInt(2), pi());
    LongTriple cycles = divide(x, twoPi);
    long long n = toLongLong(cycles);
    x = subtract(x, multiply(fromInt(n), twoPi));
    if (compare(x, pi()) == 1)  x = subtract(x, twoPi);
    if (compare(x, negate(pi())) == -1) x = add(x, twoPi);

    const LongTriple x2 = multiply(x, x);
    LongTriple term = fromInt(1);
    LongTriple sum = fromInt(1);
    for (int k = 1; k <= 14; ++k) {
        const int denominator = (2 * k - 1) * (2 * k);
        term = negate(divide(multiply(term, x2), fromInt(denominator)));
        sum = add(sum, term);
    }
    return sum;
}

} // namespace native_ops
} // namespace sandbox

#endif // TERNARY_NATIVE_OPS_H
