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

namespace sandbox {

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

    [[nodiscard]] UInt128 divSmall(uint32_t divisor) const {
        UInt128 q{};
        uint64_t rem = 0;
        for (int i = 127; i >= 0; --i) {
            rem = (rem << 1) | (bit(i) ? 1ULL : 0ULL);
            if (rem >= divisor) {
                rem -= divisor;
                q.setBit(i);
            }
        }
        return q;
    }

    [[nodiscard]] uint32_t modSmall(uint32_t divisor) const {
        uint64_t rem = 0;
        for (int i = 127; i >= 0; --i) {
            rem = (rem << 1) | (bit(i) ? 1ULL : 0ULL);
            if (rem >= divisor) rem -= divisor;
        }
        return static_cast<uint32_t>(rem);
    }

    [[nodiscard]] std::string toString() const {
        if (isZero()) return "0";
        UInt128 tmp = *this;
        std::string out;
        while (!tmp.isZero()) {
            uint32_t digit = tmp.modSmall(10);
            out.insert(out.begin(), static_cast<char>('0' + digit));
            tmp = tmp.divSmall(10);
        }
        return out;
    }

#if defined(__SIZEOF_INT128__) && !defined(__CUDA_ARCH__)
    [[nodiscard]] static constexpr UInt128 fromNative(unsigned __int128 value) {
        return UInt128{
            static_cast<uint64_t>(value >> 64),
            static_cast<uint64_t>(value)
        };
    }

    [[nodiscard]] constexpr unsigned __int128 toNative() const {
        return (static_cast<unsigned __int128>(hi) << 64)
             | static_cast<unsigned __int128>(lo);
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

[[nodiscard]] constexpr UInt128 operator+(UInt128 a, UInt128 b) {
    UInt128 out;
    out.lo = a.lo + b.lo;
    out.hi = a.hi + b.hi + (out.lo < a.lo ? 1ULL : 0ULL);
    return out;
}

[[nodiscard]] constexpr UInt128 operator-(UInt128 a, UInt128 b) {
    UInt128 out;
    out.lo = a.lo - b.lo;
    out.hi = a.hi - b.hi - (a.lo < b.lo ? 1ULL : 0ULL);
    return out;
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
    UInt128 result{};
    while (b != 0) {
        if ((b & 1U) != 0) result += a;
        a <<= 1;
        b >>= 1;
    }
    return result;
}

[[nodiscard]] inline UInt128 operator*(uint32_t a, UInt128 b) {
    return b * a;
}

inline UInt128& operator*=(UInt128& a, uint32_t b) {
    a = a * b;
    return a;
}

[[nodiscard]] inline UInt128 operator*(UInt128 a, UInt128 b) {
    UInt128 result{};
    for (int i = 0; i < 128; ++i) {
        if (b.bit(i)) result += (a << static_cast<unsigned>(i));
    }
    return result;
}

[[nodiscard]] inline UInt128 operator/(UInt128 dividend, UInt128 divisor) {
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
}

[[nodiscard]] inline UInt128 operator%(UInt128 dividend, UInt128 divisor) {
    if (divisor.isZero()) return UInt128{};
    UInt128 remainder{};
    for (int i = 127; i >= 0; --i) {
        remainder <<= 1;
        if (dividend.bit(i)) remainder.lo |= 1ULL;
        if (remainder >= divisor) remainder -= divisor;
    }
    return remainder;
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
