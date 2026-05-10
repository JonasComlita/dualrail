// =============================================================================
// ternary_uint128.h - portable 128-bit unsigned integer storage
// =============================================================================
//
// Device-safe 128-bit unsigned integer represented as two 64-bit limbs.
// This is intentionally small and header-only so it can become the common
// storage primitive for CPU, CUDA, SYCL, and other accelerator backends.

#pragma once
#ifndef TERNARY_UINT128_H
#define TERNARY_UINT128_H

#include <cstdint>
#include <limits>
#include <string>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace sandbox {

#if defined(__SIZEOF_INT128__) && !defined(__CUDA_ARCH__) && !defined(__SYCL_DEVICE_ONLY__)
#define SANDBOX_TERNARY_HAS_NATIVE_UINT128 1
#if defined(__GNUC__) || defined(__clang__)
__extension__ typedef unsigned __int128 NativeUInt128;
#else
using NativeUInt128 = unsigned __int128;
#endif
#endif

struct UInt256;

struct UInt128 {
    uint64_t lo = 0;
    uint64_t hi = 0;

    constexpr UInt128() = default;
    constexpr UInt128(uint64_t low) : lo(low), hi(0) {}
    constexpr UInt128(uint64_t high, uint64_t low) : lo(low), hi(high) {}

    [[nodiscard]] static constexpr UInt128 fromParts(uint64_t high, uint64_t low) {
        return UInt128{high, low};
    }

    [[nodiscard]] static constexpr UInt128 max() {
        return UInt128{UINT64_MAX, UINT64_MAX};
    }

    [[nodiscard]] constexpr bool isZero() const {
        return lo == 0 && hi == 0;
    }

    [[nodiscard]] constexpr uint64_t toUint64() const {
        return lo;
    }

    [[nodiscard]] constexpr bool bit(int index) const {
        return index < 64
            ? ((lo >> index) & 1ULL) != 0
            : ((hi >> (index - 64)) & 1ULL) != 0;
    }

    constexpr void setBit(int index) {
        if (index < 64) lo |= (1ULL << index);
        else hi |= (1ULL << (index - 64));
    }

    [[nodiscard]] UInt128 divModSmall(uint32_t divisor, uint32_t& rem_out) const {
        if (divisor == 0) {
            rem_out = 0;
            return UInt128{};
        }
        if (divisor == 1) {
            rem_out = 0;
            return *this;
        }

        uint64_t parts[4] = {
            hi >> 32,
            hi & 0xFFFFFFFFULL,
            lo >> 32,
            lo & 0xFFFFFFFFULL
        };
        uint64_t q_parts[4] = {0, 0, 0, 0};
        uint64_t rem = 0;
        for (int i = 0; i < 4; ++i) {
            uint64_t num = (rem << 32) | parts[i];
            q_parts[i] = num / divisor;
            rem = num % divisor;
        }
        rem_out = static_cast<uint32_t>(rem);
        return UInt128{(q_parts[0] << 32) | q_parts[1], (q_parts[2] << 32) | q_parts[3]};
    }

    [[nodiscard]] UInt128 divSmall(uint32_t divisor) const {
        uint32_t rem;
        return divModSmall(divisor, rem);
    }

    [[nodiscard]] uint32_t modSmall(uint32_t divisor) const {
        uint32_t rem;
        (void)divModSmall(divisor, rem);
        return rem;
    }

    [[nodiscard]] UInt128 divMod3(uint32_t& rem_out) const {
        return divModSmall(3, rem_out);
    }

    [[nodiscard]] std::string toString() const {
        if (isZero()) return "0";
        UInt128 tmp = *this;
        std::string out;
        while (!tmp.isZero()) {
            uint32_t digit;
            tmp = tmp.divModSmall(10, digit);
            out.insert(out.begin(), static_cast<char>('0' + digit));
        }
        return out;
    }

#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    [[nodiscard]] static constexpr UInt128 fromNative(NativeUInt128 value) {
        return UInt128{
            static_cast<uint64_t>(value >> 64),
            static_cast<uint64_t>(value)
        };
    }

    [[nodiscard]] constexpr NativeUInt128 toNative() const {
        return (static_cast<NativeUInt128>(hi) << 64)
             | static_cast<NativeUInt128>(lo);
    }
#endif
};

[[nodiscard]] constexpr bool operator==(UInt128 a, UInt128 b) {
    return a.lo == b.lo && a.hi == b.hi;
}

[[nodiscard]] constexpr bool operator!=(UInt128 a, UInt128 b) {
    return !(a == b);
}

[[nodiscard]] constexpr bool operator<(UInt128 a, UInt128 b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

[[nodiscard]] constexpr bool operator>(UInt128 a, UInt128 b) {
    return b < a;
}

[[nodiscard]] constexpr bool operator<=(UInt128 a, UInt128 b) {
    return !(b < a);
}

[[nodiscard]] constexpr bool operator>=(UInt128 a, UInt128 b) {
    return !(a < b);
}

[[nodiscard]] inline UInt128 operator+(UInt128 a, UInt128 b) {
#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    return UInt128::fromNative(a.toNative() + b.toNative());
#elif defined(_MSC_VER) && defined(_M_X64)
    UInt128 out;
    unsigned char carry = _addcarry_u64(0, a.lo, b.lo, &out.lo);
    _addcarry_u64(carry, a.hi, b.hi, &out.hi);
    return out;
#else
    UInt128 out;
    out.lo = a.lo + b.lo;
    out.hi = a.hi + b.hi + (out.lo < a.lo ? 1ULL : 0ULL);
    return out;
#endif
}

[[nodiscard]] inline UInt128 operator-(UInt128 a, UInt128 b) {
#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    return UInt128::fromNative(a.toNative() - b.toNative());
#elif defined(_MSC_VER) && defined(_M_X64)
    UInt128 out;
    unsigned char borrow = _subborrow_u64(0, a.lo, b.lo, &out.lo);
    _subborrow_u64(borrow, a.hi, b.hi, &out.hi);
    return out;
#else
    UInt128 out;
    out.lo = a.lo - b.lo;
    out.hi = a.hi - b.hi - (a.lo < b.lo ? 1ULL : 0ULL);
    return out;
#endif
}

inline UInt128& operator+=(UInt128& a, UInt128 b) {
    a = a + b;
    return a;
}

inline UInt128& operator-=(UInt128& a, UInt128 b) {
    a = a - b;
    return a;
}

[[nodiscard]] constexpr UInt128 operator<<(UInt128 a, unsigned shift) {
    if (shift >= 128) return UInt128{};
    if (shift == 0) return a;
    if (shift >= 64) return UInt128{a.lo << (shift - 64), 0};
    return UInt128{(a.hi << shift) | (a.lo >> (64 - shift)), a.lo << shift};
}

[[nodiscard]] constexpr UInt128 operator>>(UInt128 a, unsigned shift) {
    if (shift >= 128) return UInt128{};
    if (shift == 0) return a;
    if (shift >= 64) return UInt128{0, a.hi >> (shift - 64)};
    return UInt128{a.hi >> shift, (a.lo >> shift) | (a.hi << (64 - shift))};
}

inline UInt128& operator<<=(UInt128& a, unsigned shift) {
    a = a << shift;
    return a;
}

inline UInt128& operator>>=(UInt128& a, unsigned shift) {
    a = a >> shift;
    return a;
}

[[nodiscard]] inline UInt128 operator*(UInt128 a, uint32_t b) {
#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    return UInt128::fromNative(a.toNative() * static_cast<NativeUInt128>(b));
#else
    UInt128 result{};
    const uint64_t lo0 = (a.lo & 0xFFFFFFFFULL) * b;
    const uint64_t lo1 = (a.lo >> 32) * b + (lo0 >> 32);
    result.lo = (lo1 << 32) | (lo0 & 0xFFFFFFFFULL);

    const uint64_t hi0 = (a.hi & 0xFFFFFFFFULL) * b + (lo1 >> 32);
    const uint64_t hi1 = (a.hi >> 32) * b + (hi0 >> 32);
    result.hi = (hi1 << 32) | (hi0 & 0xFFFFFFFFULL);
    return result;
#endif
}

[[nodiscard]] inline UInt128 operator*(uint32_t a, UInt128 b) {
    return b * a;
}

inline UInt128& operator*=(UInt128& a, uint32_t b) {
    a = a * b;
    return a;
}

[[nodiscard]] inline UInt128 operator*(UInt128 a, UInt128 b) {
#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    return UInt128::fromNative(a.toNative() * b.toNative());
#else
    UInt128 result{};
    for (int i = 0; i < 128; ++i) {
        if (b.bit(i)) result += (a << static_cast<unsigned>(i));
    }
    return result;
#endif
}

[[nodiscard]] inline UInt128 operator/(UInt128 dividend, UInt128 divisor) {
#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    if (divisor.isZero()) return UInt128{};
    return UInt128::fromNative(dividend.toNative() / divisor.toNative());
#else
    if (divisor.isZero()) return UInt128{};
    UInt128 quotient{};
    UInt128 remainder{};
    for (int i = 127; i >= 0; --i) {
        remainder <<= 1;
        if (dividend.bit(i)) remainder.lo |= 1ULL;
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient.setBit(i);
        }
    }
    return quotient;
#endif
}

[[nodiscard]] inline UInt128 operator%(UInt128 dividend, UInt128 divisor) {
#if defined(SANDBOX_TERNARY_HAS_NATIVE_UINT128)
    if (divisor.isZero()) return UInt128{};
    return UInt128::fromNative(dividend.toNative() % divisor.toNative());
#else
    if (divisor.isZero()) return UInt128{};
    UInt128 remainder{};
    for (int i = 127; i >= 0; --i) {
        remainder <<= 1;
        if (dividend.bit(i)) remainder.lo |= 1ULL;
        if (remainder >= divisor) remainder -= divisor;
    }
    return remainder;
#endif
}

[[nodiscard]] inline UInt128 operator/(UInt128 dividend, uint32_t divisor) {
    return dividend.divSmall(divisor);
}

[[nodiscard]] inline uint32_t operator%(UInt128 dividend, uint32_t divisor) {
    return dividend.modSmall(divisor);
}

struct UInt256 {
    uint64_t limb[4] = {0, 0, 0, 0};

    [[nodiscard]] bool bit(int index) const {
        return ((limb[index / 64] >> (index % 64)) & 1ULL) != 0;
    }

    void addShifted(UInt128 value, int shift) {
        const int word = shift / 64;
        const int bits = shift % 64;
        uint64_t parts[4] = {0, 0, 0, 0};
        if (word < 4) parts[word] |= value.lo << bits;
        if (bits == 0) {
            if (word + 1 < 4) parts[word + 1] |= value.hi;
        } else {
            if (word + 1 < 4) {
                parts[word + 1] |= (value.lo >> (64 - bits));
                parts[word + 1] |= (value.hi << bits);
            }
            if (word + 2 < 4) parts[word + 2] |= (value.hi >> (64 - bits));
        }

        uint64_t carry = 0;
        for (int i = 0; i < 4; ++i) {
            uint64_t before = limb[i];
            limb[i] += parts[i];
            uint64_t carry1 = limb[i] < before ? 1ULL : 0ULL;
            before = limb[i];
            limb[i] += carry;
            uint64_t carry2 = limb[i] < before ? 1ULL : 0ULL;
            carry = (carry1 | carry2);
        }
    }

    [[nodiscard]] std::string toString() const {
        if (limb[0] == 0 && limb[1] == 0 && limb[2] == 0 && limb[3] == 0) return "0";
        UInt256 tmp = *this;
        std::string out;
        while (!(tmp.limb[0] == 0 && tmp.limb[1] == 0 && tmp.limb[2] == 0 && tmp.limb[3] == 0)) {
            uint32_t rem = 0;
            for (int i = 255; i >= 0; --i) {
                rem = static_cast<uint32_t>((rem << 1) | (tmp.bit(i) ? 1U : 0U));
                if (rem >= 10) rem = static_cast<uint32_t>(rem - 10);
            }
            out.insert(out.begin(), static_cast<char>('0' + rem));

            UInt256 q{};
            rem = 0;
            for (int i = 255; i >= 0; --i) {
                rem = static_cast<uint32_t>((rem << 1) | (tmp.bit(i) ? 1U : 0U));
                if (rem >= 10) {
                    rem = static_cast<uint32_t>(rem - 10);
                    q.limb[i / 64] |= 1ULL << (i % 64);
                }
            }
            tmp = q;
        }
        return out;
    }
};

[[nodiscard]] inline UInt256 multiplyFull(UInt128 a, UInt128 b) {
    UInt256 out{};
    for (int i = 0; i < 128; ++i) {
        if (b.bit(i)) out.addShifted(a, i);
    }
    return out;
}

struct Int128 {
    UInt128 magnitude{};
    int8_t sign = 0;

    constexpr Int128() = default;

    [[nodiscard]] static constexpr Int128 fromMagnitude(int s, UInt128 mag) {
        Int128 out{};
        out.magnitude = mag;
        out.sign = mag.isZero() ? 0 : static_cast<int8_t>(s < 0 ? -1 : 1);
        return out;
    }

    [[nodiscard]] static constexpr Int128 fromLongLong(long long value) {
        if (value == 0) return Int128{};
        if (value > 0) return fromMagnitude(1, UInt128{static_cast<uint64_t>(value)});

        const uint64_t mag = value == std::numeric_limits<long long>::min()
            ? (1ULL << 63)
            : static_cast<uint64_t>(-value);
        return fromMagnitude(-1, UInt128{mag});
    }

    [[nodiscard]] constexpr bool isZero() const {
        return sign == 0 || magnitude.isZero();
    }
};

[[nodiscard]] constexpr Int128 makeInt128(int sign, UInt128 magnitude) {
    return Int128::fromMagnitude(sign, magnitude);
}

[[nodiscard]] constexpr Int128 operator-(Int128 value) {
    return Int128::fromMagnitude(-value.sign, value.magnitude);
}

[[nodiscard]] constexpr int compareMagnitude(UInt128 a, UInt128 b) {
    return a > b ? 1 : (a < b ? -1 : 0);
}

[[nodiscard]] constexpr int compare(Int128 a, Int128 b) {
    if (a.isZero() && b.isZero()) return 0;
    if (a.sign != b.sign) return a.sign < b.sign ? -1 : 1;
    const int magCmp = compareMagnitude(a.magnitude, b.magnitude);
    return a.sign >= 0 ? magCmp : -magCmp;
}

[[nodiscard]] constexpr bool operator==(Int128 a, Int128 b) {
    return compare(a, b) == 0;
}

[[nodiscard]] constexpr bool operator!=(Int128 a, Int128 b) {
    return !(a == b);
}

[[nodiscard]] constexpr bool operator<(Int128 a, Int128 b) {
    return compare(a, b) < 0;
}

[[nodiscard]] constexpr bool operator>(Int128 a, Int128 b) {
    return compare(a, b) > 0;
}

[[nodiscard]] constexpr bool operator<=(Int128 a, Int128 b) {
    return compare(a, b) <= 0;
}

[[nodiscard]] constexpr bool operator>=(Int128 a, Int128 b) {
    return compare(a, b) >= 0;
}

[[nodiscard]] constexpr Int128 operator+(Int128 a, Int128 b) {
    if (a.isZero()) return b;
    if (b.isZero()) return a;
    if (a.sign == b.sign) return Int128::fromMagnitude(a.sign, a.magnitude + b.magnitude);

    const int cmp = compareMagnitude(a.magnitude, b.magnitude);
    if (cmp == 0) return Int128{};
    if (cmp > 0) return Int128::fromMagnitude(a.sign, a.magnitude - b.magnitude);
    return Int128::fromMagnitude(b.sign, b.magnitude - a.magnitude);
}

[[nodiscard]] constexpr Int128 operator-(Int128 a, Int128 b) {
    return a + (-b);
}

[[nodiscard]] inline Int128 operator*(Int128 a, Int128 b) {
    if (a.isZero() || b.isZero()) return Int128{};
    return Int128::fromMagnitude(a.sign * b.sign, a.magnitude * b.magnitude);
}

[[nodiscard]] inline Int128 operator*(Int128 a, UInt128 b) {
    if (a.isZero() || b.isZero()) return Int128{};
    return Int128::fromMagnitude(a.sign, a.magnitude * b);
}

[[nodiscard]] inline Int128 operator*(UInt128 a, Int128 b) {
    return b * a;
}

[[nodiscard]] inline Int128 operator/(Int128 a, UInt128 b) {
    if (a.isZero() || b.isZero()) return Int128{};
    return Int128::fromMagnitude(a.sign, a.magnitude / b);
}

[[nodiscard]] inline Int128 operator/(Int128 a, Int128 b) {
    if (a.isZero() || b.isZero()) return Int128{};
    return Int128::fromMagnitude(a.sign * b.sign, a.magnitude / b.magnitude);
}

[[nodiscard]] inline int signOf(Int128 value) {
    return value.isZero() ? 0 : value.sign;
}

[[nodiscard]] inline UInt128 absUnsigned(Int128 value) {
    return value.magnitude;
}

[[nodiscard]] inline long long toLongLongSaturated(Int128 value) {
    if (value.isZero()) return 0LL;

    const UInt128 maxPositive{static_cast<uint64_t>(std::numeric_limits<long long>::max())};
    const UInt128 maxNegativeMagnitude{1ULL << 63};

    if (value.sign > 0) {
        if (value.magnitude > maxPositive) return std::numeric_limits<long long>::max();
        return static_cast<long long>(value.magnitude.lo);
    }

    if (value.magnitude > maxNegativeMagnitude) return std::numeric_limits<long long>::min();
    if (value.magnitude == maxNegativeMagnitude) return std::numeric_limits<long long>::min();
    return -static_cast<long long>(value.magnitude.lo);
}

} // namespace sandbox

#endif // TERNARY_UINT128_H
