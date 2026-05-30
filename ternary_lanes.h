// =============================================================================
// ternary_lanes.h - 2-bit-per-trit lane transport types
// =============================================================================
//
// Lane types are wire/SIMD/GPU transport containers. They intentionally do not
// expose arithmetic on their backing integer because binary integer arithmetic
// has no balanced-ternary meaning in this encoding.

#pragma once
#ifndef TERNARY_LANES_H
#define TERNARY_LANES_H

#include "ternary_backend.h"
#include "ternary_math.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sandbox {
namespace lane_detail {

[[nodiscard]] inline uint8_t encodeTrit(int8_t trit) {
    assert(trit >= -1 && trit <= 1);
    return backend::encodeTritPair(trit);
}

[[nodiscard]] inline int8_t decodeTrit(uint8_t raw) {
    assert(raw < 3);
    return backend::decodeTritPair(raw);
}

template<typename Storage>
[[nodiscard]] inline uint8_t getPair(Storage bits, int pos) {
    static_assert(std::is_integral<Storage>::value, "integral lane storage expected");
    return static_cast<uint8_t>((bits >> (2 * pos)) & static_cast<Storage>(0x3));
}

[[nodiscard]] inline uint8_t getPair(UInt128 bits, int pos) {
    const int bit = 2 * pos;
    return static_cast<uint8_t>((bits.bit(bit) ? 1U : 0U) |
                                (bits.bit(bit + 1) ? 2U : 0U));
}

template<typename Storage>
inline void setPair(Storage& bits, int pos, uint8_t raw) {
    static_assert(std::is_integral<Storage>::value, "integral lane storage expected");
    const Storage shift = static_cast<Storage>(2 * pos);
    const Storage mask = static_cast<Storage>(0x3) << shift;
    bits = static_cast<Storage>((bits & ~mask) |
                                (static_cast<Storage>(raw & 0x3U) << shift));
}

inline void setPair(UInt128& bits, int pos, uint8_t raw) {
    const int bit = 2 * pos;
    uint64_t& limb = bit < 64 ? bits.lo : bits.hi;
    const int offset = bit < 64 ? bit : bit - 64;
    const uint64_t mask = 0x3ULL << offset;
    limb = (limb & ~mask) | (static_cast<uint64_t>(raw & 0x3U) << offset);
}

template<typename Storage>
[[nodiscard]] inline bool paddingClear(Storage bits, int usedBits) {
    static_assert(std::is_integral<Storage>::value, "integral lane storage expected");
    const int totalBits = static_cast<int>(sizeof(Storage) * 8);
    if (usedBits >= totalBits) return true;
    const Storage usedMask = static_cast<Storage>((static_cast<Storage>(1) << usedBits) - 1);
    return (bits & ~usedMask) == 0;
}

[[nodiscard]] inline bool paddingClear(UInt128 bits, int usedBits) {
    for (int i = usedBits; i < 128; ++i) {
        if (bits.bit(i)) return false;
    }
    return true;
}


} // namespace lane_detail

template<int Trits, typename StorageT>
class TritLane {
public:
    using storage_type = StorageT;
    static constexpr int trits = Trits;
    static constexpr int used_bits = 2 * Trits;

    TritLane() {
        fill(0);
    }

    [[nodiscard]] static TritLane invalid() {
        TritLane lane;
        lane.bits_ = storage_type{};
        for (int i = 0; i < trits; ++i) {
            lane_detail::setPair(lane.bits_, i, 0x3U);
        }
        return lane;
    }

    [[nodiscard]] static TritLane fromRawForKernel(storage_type raw) {
        TritLane lane;
        lane.bits_ = raw;
        return lane;
    }

    [[nodiscard]] storage_type rawForKernel() const {
        return bits_;
    }

    [[nodiscard]] bool isValid() const {
        if (!lane_detail::paddingClear(bits_, used_bits)) return false;
        for (int i = 0; i < trits; ++i) {
            if (lane_detail::getPair(bits_, i) == 0x3U) return false;
        }
        return true;
    }

    [[nodiscard]] int8_t tritAt(int pos) const {
        assert(pos >= 0 && pos < trits);
        return lane_detail::decodeTrit(lane_detail::getPair(bits_, pos));
    }

    void setTrit(int pos, int8_t trit) {
        assert(pos >= 0 && pos < trits);
        lane_detail::setPair(bits_, pos, lane_detail::encodeTrit(trit));
    }

    void fill(int8_t trit) {
        bits_ = storage_type{};
        for (int i = 0; i < trits; ++i) setTrit(i, trit);
    }

    [[nodiscard]] bool operator==(TritLane other) const {
        return bits_ == other.bits_;
    }

    [[nodiscard]] bool operator!=(TritLane other) const {
        return !(*this == other);
    }

private:
    storage_type bits_{};
};

using TritLane1  = TritLane<1,  uint8_t>;
using TritLane5  = TritLane<5,  uint16_t>;
using TritLane10 = TritLane<10, uint32_t>;
using TritLane20 = TritLane<20, uint64_t>;
using TritLane40 = TritLane<40, UInt128>;
using TritLane50 = TritLane<50, UInt128>;

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseNeg(TritLane<Trits, Storage> lane) {
    if (!lane.isValid()) return TritLane<Trits, Storage>::invalid();
    if constexpr (sizeof(Storage) <= 8) {
        uint64_t raw = static_cast<uint64_t>(lane.rawForKernel());
        uint64_t res = backend::negLane64(raw, Trits);
        return TritLane<Trits, Storage>::fromRawForKernel(static_cast<Storage>(res));
    } else {
        backend::RawUInt128 raw{lane.rawForKernel().lo, lane.rawForKernel().hi};
        backend::RawUInt128 res = backend::negLane128(raw, Trits);
        return TritLane<Trits, Storage>::fromRawForKernel(UInt128{res.hi, res.lo});
    }
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseAdd(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    if constexpr (sizeof(Storage) <= 8) {
        uint64_t ra = static_cast<uint64_t>(a.rawForKernel());
        uint64_t rb = static_cast<uint64_t>(b.rawForKernel());
        uint64_t res = backend::addLane64(ra, rb, Trits);
        return TritLane<Trits, Storage>::fromRawForKernel(static_cast<Storage>(res));
    } else {
        backend::RawUInt128 ra{a.rawForKernel().lo, a.rawForKernel().hi};
        backend::RawUInt128 rb{b.rawForKernel().lo, b.rawForKernel().hi};
        backend::RawUInt128 res = backend::addLane128(ra, rb, Trits);
        return TritLane<Trits, Storage>::fromRawForKernel(UInt128{res.hi, res.lo});
    }
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseSubtract(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    if constexpr (sizeof(Storage) <= 8) {
        uint64_t ra = static_cast<uint64_t>(a.rawForKernel());
        uint64_t rb = static_cast<uint64_t>(b.rawForKernel());
        uint64_t res = backend::subLane64(ra, rb, Trits);
        return TritLane<Trits, Storage>::fromRawForKernel(static_cast<Storage>(res));
    } else {
        backend::RawUInt128 ra{a.rawForKernel().lo, a.rawForKernel().hi};
        backend::RawUInt128 rb{b.rawForKernel().lo, b.rawForKernel().hi};
        backend::RawUInt128 res = backend::subLane128(ra, rb, Trits);
        return TritLane<Trits, Storage>::fromRawForKernel(UInt128{res.hi, res.lo});
    }
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseAddCarryless(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    TritLane<Trits, Storage> out;
    for (int i = 0; i < Trits; ++i) {
        int sum = a.tritAt(i) + b.tritAt(i);
        while (sum > 1) sum -= 3;
        while (sum < -1) sum += 3;
        out.setTrit(i, static_cast<int8_t>(sum));
    }
    return out;
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseSubtractCarryless(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    TritLane<Trits, Storage> out;
    for (int i = 0; i < Trits; ++i) {
        int diff = a.tritAt(i) - b.tritAt(i);
        while (diff > 1) diff -= 3;
        while (diff < -1) diff += 3;
        out.setTrit(i, static_cast<int8_t>(diff));
    }
    return out;
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseAnd(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    TritLane<Trits, Storage> out;
    for (int i = 0; i < Trits; ++i) out.setTrit(i, std::min(a.tritAt(i), b.tritAt(i)));
    return out;
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseOr(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    TritLane<Trits, Storage> out;
    for (int i = 0; i < Trits; ++i) out.setTrit(i, std::max(a.tritAt(i), b.tritAt(i)));
    return out;
}

template<int Trits, typename Storage>
[[nodiscard]] inline int8_t tritwiseCompare(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return 0;

    if constexpr (sizeof(Storage) <= 8) {
        uint64_t ra = static_cast<uint64_t>(a.rawForKernel());
        uint64_t rb = static_cast<uint64_t>(b.rawForKernel());
        return backend::compareLane64(ra, rb, Trits);
    } else {
        backend::RawUInt128 ra{a.rawForKernel().lo, a.rawForKernel().hi};
        backend::RawUInt128 rb{b.rawForKernel().lo, b.rawForKernel().hi};
        return backend::compareLane128(ra, rb, Trits);
    }
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> laneFromTritArray(
    const std::array<int8_t, Trits>& trits) {

    TritLane<Trits, Storage> lane;
    for (int i = 0; i < Trits; ++i) lane.setTrit(i, trits[i]);
    return lane;
}

template<int Trits, typename Storage>
[[nodiscard]] inline std::array<int8_t, Trits> laneToTritArray(TritLane<Trits, Storage> lane) {
    std::array<int8_t, Trits> trits{};
    for (int i = 0; i < Trits; ++i) trits[i] = lane.tritAt(i);
    return trits;
}



// ---------------------------------------------------------------------------
// CANONICAL CROSSING POINTS: Positional Base-3 <-> 2-bit Packed Lane
//
// These two functions are the ONLY legal conversion path between the
// canonical positional representation (TernaryScalar<N>) and the SIMD
// transport format (TritLane<N>). Every other conversion must go through
// these. This eliminates the class of bugs where different conversion
// sites make different assumptions about field layouts.
//
// Invalid lane sentinel: an invalid lane (containing 0b11 bit patterns)
// maps to OVERFLOW_DATA in positional representation. This is consistent
// with the existing convention where invalid numeric values use OVERFLOW.
// ---------------------------------------------------------------------------

// Positional -> Lane: unpack positional to trit array, pack into lane.
template<int Trits, typename LaneStorage>
[[nodiscard]] inline TritLane<Trits, LaneStorage> laneFromPositional(
    TernaryScalar<Trits> scalar) {
    if (scalar.isSpecial()) return TritLane<Trits, LaneStorage>::invalid();
    if (scalar.isZero()) {
        TritLane<Trits, LaneStorage> lane;
        lane.fill(0);
        return lane;
    }
    return laneFromTritArray<Trits, LaneStorage>(scalar.unpack());
}

// Lane -> Positional: extract all trits from lane, pack into positional.
// Invalid lanes map to OVERFLOW_DATA sentinel — no selective field checks.
template<int Trits, typename LaneStorage>
[[nodiscard]] inline TernaryScalar<Trits> laneToPositional(
    TritLane<Trits, LaneStorage> lane) {
    if (!lane.isValid()) return TernaryScalar<Trits>{TernaryScalar<Trits>::OVERFLOW_DATA};
    const auto trits = laneToTritArray(lane);

    bool allZero = true;
    for (int i = 0; i < Trits; ++i) {
        if (trits[i] != 0) {
            allZero = false;
            break;
        }
    }
    if (allZero) return TernaryScalar<Trits>{};

    constexpr int mantissaTrits =
        Trits == 10 ? 6 :
        Trits == 20 ? 14 :
        Trits == 40 ? 33 :
        Trits == 50 ? 41 : Trits;
    if constexpr (mantissaTrits < Trits) {
        bool mantissaZero = true;
        for (int i = 0; i < mantissaTrits; ++i) {
            if (trits[i] != 0) {
                mantissaZero = false;
                break;
            }
        }
        if (mantissaZero) return TernaryScalar<Trits>{};
    }

    return TernaryScalar<Trits>::pack(trits);
}

// ---------------------------------------------------------------------------
// Legacy toLane/fromLane wrappers (delegate to canonical crossing points)
// ---------------------------------------------------------------------------

[[nodiscard]] inline TritLane1 toLane(T1 value) {
    if (value.isInvalid()) return TritLane1::invalid();
    TritLane1 lane;
    lane.setTrit(0, static_cast<int8_t>(value.data % 3) - 1);
    return lane;
}

[[nodiscard]] inline T1 fromLane(TritLane1 lane) {
    if (!lane.isValid()) return T1{T1::INVALID_DATA};
    return T1{static_cast<uint8_t>(lane.tritAt(0) + 1)};
}

[[nodiscard]] inline TritLane5 toLane(T5 value) {
    if (value.isInvalid()) return TritLane5::invalid();
    TritLane5 lane;
    uint16_t temp = value.data;
    for (int i = 0; i < TritLane5::trits; ++i) {
        lane.setTrit(i, static_cast<int8_t>(temp % 3) - 1);
        temp = static_cast<uint16_t>(temp / 3);
    }
    return lane;
}

[[nodiscard]] inline T5 fromLane(TritLane5 lane) {
    if (!lane.isValid()) return T5{T5::INVALID_DATA};
    uint16_t raw = 0;
    uint16_t place = 1;
    for (int i = 0; i < TritLane5::trits; ++i) {
        raw = static_cast<uint16_t>(raw + (lane.tritAt(i) + 1) * place);
        place = static_cast<uint16_t>(place * 3);
    }
    return T5{static_cast<uint8_t>(raw)};
}

// T10+ toLane/fromLane now delegate to the canonical crossing points.
[[nodiscard]] inline TritLane10 toLane(T10 value) {
    return laneFromPositional<10, uint32_t>(value);
}

[[nodiscard]] inline T10 fromLane(TritLane10 lane) {
    return laneToPositional<10, uint32_t>(lane);
}

[[nodiscard]] inline TritLane20 toLane(T20 value) {
    return laneFromPositional<20, uint64_t>(value);
}

[[nodiscard]] inline T20 fromLane(TritLane20 lane) {
    return laneToPositional<20, uint64_t>(lane);
}

[[nodiscard]] inline TritLane40 toLane(Triple value) {
    return laneFromPositional<40, UInt128>(value);
}

[[nodiscard]] inline Triple fromLane(TritLane40 lane) {
    return laneToPositional<40, UInt128>(lane);
}

[[nodiscard]] inline TritLane50 toLane(LongTriple value) {
    return laneFromPositional<50, UInt128>(value);
}

[[nodiscard]] inline LongTriple fromLane(TritLane50 lane) {
    return laneToPositional<50, UInt128>(lane);
}

} // namespace sandbox

#endif // TERNARY_LANES_H
