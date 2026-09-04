// =============================================================================
// ternary_backend.h - small device-safe backend helpers
// =============================================================================
//
// This header intentionally avoids STL, exceptions, strings, and virtual
// dispatch. It is the place for helpers that can later be shared by CPU SIMD,
// SYCL/CUDA kernels, FPGA-oriented models, and scalar fallback code.

#pragma once
#ifndef TERNARY_BACKEND_H
#define TERNARY_BACKEND_H

#include <cstdint>

#if defined(__CUDACC__) || defined(__HIPCC__)
#define TERNARY_HOST_DEVICE __host__ __device__
#else
#define TERNARY_HOST_DEVICE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TERNARY_FORCE_INLINE inline __attribute__((always_inline))
#else
#define TERNARY_FORCE_INLINE inline
#endif

namespace sandbox {
namespace backend {

inline constexpr int8_t TRIT_COMPARE_INVALID = 2;

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint8_t encodeTritPair(int8_t trit) {
    if (trit < -1 || trit > 1) return 0x3U;
    return static_cast<uint8_t>(trit + 1);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE int8_t decodeTritPair(uint8_t raw) {
    if (raw >= 3U) return 0;
    return static_cast<int8_t>(raw) - 1;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validTritPair(uint8_t raw) {
    return raw < 3;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint8_t negateTritPair(uint8_t raw) {
    if (raw >= 3U) return 0x3U;
    return static_cast<uint8_t>(2U - raw);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE int8_t normalizeTritSum(int& sum) {
    while (sum > 1) sum -= 3;
    while (sum < -1) sum += 3;
    return static_cast<int8_t>(sum);
}

struct RawUInt128 {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validLaneWidth64(int trits) {
    return trits > 0 && trits <= 32;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validLanePosition64(int pos) {
    return pos >= 0 && pos < 32;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validLaneWidth128(int trits) {
    return trits > 0 && trits <= 64;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validLanePosition128(int pos) {
    return pos >= 0 && pos < 64;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t payloadMask64(int trits) {
    if (!validLaneWidth64(trits)) return 0;
    const int usedBits = 2 * trits;
    if (usedBits == 64) return UINT64_MAX;
    return (1ULL << usedBits) - 1ULL;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool paddingClear64(uint64_t raw, int trits) {
    if (!validLaneWidth64(trits)) return false;
    const uint64_t mask = payloadMask64(trits);
    return (raw & ~mask) == 0;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint8_t getPair64(uint64_t raw, int pos) {
    if (!validLanePosition64(pos)) return 0x3U;
    return static_cast<uint8_t>((raw >> (2 * pos)) & 0x3ULL);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t setPair64(
    uint64_t raw,
    int pos,
    uint8_t pair) {

    if (!validLanePosition64(pos)) return UINT64_MAX;
    const int bit = 2 * pos;
    const uint64_t mask = 0x3ULL << bit;
    return (raw & ~mask) | (static_cast<uint64_t>(pair & 0x3U) << bit);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validLane64(uint64_t raw, int trits) {
    if (!validLaneWidth64(trits)) return false;
    if (!paddingClear64(raw, trits)) return false;
    for (int i = 0; i < trits; ++i) {
        if (getPair64(raw, i) == 0x3U) return false;
    }
    return true;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t invalidLane64(int trits) {
    if (!validLaneWidth64(trits)) return UINT64_MAX;
    uint64_t raw = 0;
    for (int i = 0; i < trits; ++i) raw = setPair64(raw, i, 0x3U);
    return raw;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t compareResultLane1Raw(int8_t cmp) {
    return static_cast<uint64_t>(encodeTritPair(cmp));
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t negLane64(uint64_t raw, int trits) {
    if (!validLane64(raw, trits)) return invalidLane64(trits);
    uint64_t out = 0;
    for (int i = 0; i < trits; ++i) {
        out = setPair64(out, i, negateTritPair(getPair64(raw, i)));
    }
    return out;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t addSubLane64(
    uint64_t a,
    uint64_t b,
    int trits,
    bool subtractB) {

    if (!validLane64(a, trits) || !validLane64(b, trits)) return invalidLane64(trits);

    uint64_t out = 0;
    int carry = 0;
    for (int i = 0; i < trits; ++i) {
        const int av = decodeTritPair(getPair64(a, i));
        int bv = decodeTritPair(getPair64(b, i));
        if (subtractB) bv = -bv;

        int sum = av + bv + carry;
        carry = 0;
        while (sum > 1) {
            sum -= 3;
            ++carry;
        }
        while (sum < -1) {
            sum += 3;
            --carry;
        }
        out = setPair64(out, i, encodeTritPair(static_cast<int8_t>(sum)));
    }

    return carry == 0 ? out : invalidLane64(trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t addLane64(uint64_t a, uint64_t b, int trits) {
    return addSubLane64(a, b, trits, false);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t subLane64(uint64_t a, uint64_t b, int trits) {
    return addSubLane64(a, b, trits, true);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE int8_t compareLane64(uint64_t a, uint64_t b, int trits) {
    if (!validLane64(a, trits) || !validLane64(b, trits)) return TRIT_COMPARE_INVALID;
    for (int i = trits - 1; i >= 0; --i) {
        const int8_t av = decodeTritPair(getPair64(a, i));
        const int8_t bv = decodeTritPair(getPair64(b, i));
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t minLane64(uint64_t a, uint64_t b, int trits) {
    if (!validLane64(a, trits) || !validLane64(b, trits)) return invalidLane64(trits);
    return compareLane64(a, b, trits) <= 0 ? a : b;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint64_t maxLane64(uint64_t a, uint64_t b, int trits) {
    if (!validLane64(a, trits) || !validLane64(b, trits)) return invalidLane64(trits);
    return compareLane64(a, b, trits) >= 0 ? a : b;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 makeRawUInt128(uint64_t hi, uint64_t lo) {
    RawUInt128 out{};
    out.lo = lo;
    out.hi = hi;
    return out;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool equalRaw128(RawUInt128 a, RawUInt128 b) {
    return a.lo == b.lo && a.hi == b.hi;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool paddingClear128(RawUInt128 raw, int trits) {
    if (!validLaneWidth128(trits)) return false;
    const int usedBits = 2 * trits;
    if (usedBits == 128) return true;
    if (usedBits <= 64) {
        const uint64_t mask = usedBits == 64 ? UINT64_MAX : ((1ULL << usedBits) - 1ULL);
        return raw.hi == 0 && (raw.lo & ~mask) == 0;
    }

    const int hiBits = usedBits - 64;
    const uint64_t hiMask = hiBits >= 64 ? UINT64_MAX : ((1ULL << hiBits) - 1ULL);
    return (raw.hi & ~hiMask) == 0;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE uint8_t getPair128(RawUInt128 raw, int pos) {
    if (!validLanePosition128(pos)) return 0x3U;
    const int bit = 2 * pos;
    if (bit < 64) return static_cast<uint8_t>((raw.lo >> bit) & 0x3ULL);
    return static_cast<uint8_t>((raw.hi >> (bit - 64)) & 0x3ULL);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 setPair128(
    RawUInt128 raw,
    int pos,
    uint8_t pair) {

    if (!validLanePosition128(pos)) return RawUInt128{UINT64_MAX, UINT64_MAX};
    const int bit = 2 * pos;
    if (bit < 64) {
        const uint64_t mask = 0x3ULL << bit;
        raw.lo = (raw.lo & ~mask) | (static_cast<uint64_t>(pair & 0x3U) << bit);
    } else {
        const int offset = bit - 64;
        const uint64_t mask = 0x3ULL << offset;
        raw.hi = (raw.hi & ~mask) | (static_cast<uint64_t>(pair & 0x3U) << offset);
    }
    return raw;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE bool validLane128(RawUInt128 raw, int trits) {
    if (!validLaneWidth128(trits)) return false;
    if (!paddingClear128(raw, trits)) return false;
    for (int i = 0; i < trits; ++i) {
        if (getPair128(raw, i) == 0x3U) return false;
    }
    return true;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 invalidLane128(int trits) {
    if (!validLaneWidth128(trits)) return RawUInt128{UINT64_MAX, UINT64_MAX};
    RawUInt128 raw{};
    for (int i = 0; i < trits; ++i) raw = setPair128(raw, i, 0x3U);
    return raw;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 negLane128(RawUInt128 raw, int trits) {
    if (!validLane128(raw, trits)) return invalidLane128(trits);
    RawUInt128 out{};
    for (int i = 0; i < trits; ++i) {
        out = setPair128(out, i, negateTritPair(getPair128(raw, i)));
    }
    return out;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 addSubLane128(
    RawUInt128 a,
    RawUInt128 b,
    int trits,
    bool subtractB) {

    if (!validLane128(a, trits) || !validLane128(b, trits)) return invalidLane128(trits);

    RawUInt128 out{};
    int carry = 0;
    for (int i = 0; i < trits; ++i) {
        const int av = decodeTritPair(getPair128(a, i));
        int bv = decodeTritPair(getPair128(b, i));
        if (subtractB) bv = -bv;

        int sum = av + bv + carry;
        carry = 0;
        while (sum > 1) {
            sum -= 3;
            ++carry;
        }
        while (sum < -1) {
            sum += 3;
            --carry;
        }
        out = setPair128(out, i, encodeTritPair(static_cast<int8_t>(sum)));
    }

    return carry == 0 ? out : invalidLane128(trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 addLane128(
    RawUInt128 a,
    RawUInt128 b,
    int trits) {

    return addSubLane128(a, b, trits, false);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 subLane128(
    RawUInt128 a,
    RawUInt128 b,
    int trits) {

    return addSubLane128(a, b, trits, true);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE int8_t compareLane128(
    RawUInt128 a,
    RawUInt128 b,
    int trits) {

    if (!validLane128(a, trits) || !validLane128(b, trits)) return TRIT_COMPARE_INVALID;
    for (int i = trits - 1; i >= 0; --i) {
        const int8_t av = decodeTritPair(getPair128(a, i));
        const int8_t bv = decodeTritPair(getPair128(b, i));
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 minLane128(
    RawUInt128 a,
    RawUInt128 b,
    int trits) {

    if (!validLane128(a, trits) || !validLane128(b, trits)) return invalidLane128(trits);
    return compareLane128(a, b, trits) <= 0 ? a : b;
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE RawUInt128 maxLane128(
    RawUInt128 a,
    RawUInt128 b,
    int trits) {

    if (!validLane128(a, trits) || !validLane128(b, trits)) return invalidLane128(trits);
    return compareLane128(a, b, trits) >= 0 ? a : b;
}

} // namespace backend
} // namespace sandbox

#endif // TERNARY_BACKEND_H
