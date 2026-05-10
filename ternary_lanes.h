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

template<std::size_t Trits>
[[nodiscard]] inline bool mantissaZero(const std::array<int8_t, Trits>& trits, int mantissaTrits) {
    for (int i = 0; i < mantissaTrits; ++i) {
        if (trits[i] != 0) return false;
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
    TritLane<Trits, Storage> out;
    for (int i = 0; i < Trits; ++i) out.setTrit(i, static_cast<int8_t>(-lane.tritAt(i)));
    return out;
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseAdd(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    if (!a.isValid() || !b.isValid()) return TritLane<Trits, Storage>::invalid();

    TritLane<Trits, Storage> out;
    int carry = 0;
    for (int i = 0; i < Trits; ++i) {
        int sum = a.tritAt(i) + b.tritAt(i) + carry;
        carry = 0;
        while (sum > 1) {
            sum -= 3;
            ++carry;
        }
        while (sum < -1) {
            sum += 3;
            --carry;
        }
        out.setTrit(i, static_cast<int8_t>(sum));
    }

    if (carry != 0) return TritLane<Trits, Storage>::invalid();
    return out;
}

template<int Trits, typename Storage>
[[nodiscard]] inline TritLane<Trits, Storage> tritwiseSubtract(
    TritLane<Trits, Storage> a,
    TritLane<Trits, Storage> b) {

    return tritwiseAdd(a, tritwiseNeg(b));
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
    for (int i = Trits - 1; i >= 0; --i) {
        const int8_t av = a.tritAt(i);
        const int8_t bv = b.tritAt(i);
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
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

[[nodiscard]] inline TritLane10 toLane(T10 value) {
    if (value.isSpecial() || native_ops::isInvalid(value)) return TritLane10::invalid();
    if (value.isZero()) {
        TritLane10 lane;
        lane.fill(0);
        return lane;
    }
    return laneFromTritArray<10, uint32_t>(value.unpack());
}

[[nodiscard]] inline T10 fromLane(TritLane10 lane) {
    if (!lane.isValid()) return T10{T10::OVERFLOW_DATA};
    const auto trits = laneToTritArray(lane);
    if (lane_detail::mantissaZero(trits, 6)) return T10{0};
    return T10::pack(trits);
}

[[nodiscard]] inline TritLane20 toLane(T20 value) {
    if (value.isSpecial() || native_ops::isInvalid(value)) return TritLane20::invalid();
    if (value.isZero()) {
        TritLane20 lane;
        lane.fill(0);
        return lane;
    }
    return laneFromTritArray<20, uint64_t>(value.unpack());
}

[[nodiscard]] inline T20 fromLane(TritLane20 lane) {
    if (!lane.isValid()) return T20{T20::OVERFLOW_DATA};
    const auto trits = laneToTritArray(lane);
    if (lane_detail::mantissaZero(trits, 14)) return T20{0};
    return T20::pack(trits);
}

[[nodiscard]] inline TritLane40 toLane(Triple value) {
    if (value.isSpecial() || native_ops::isInvalid(value)) return TritLane40::invalid();
    if (value.isZero()) {
        TritLane40 lane;
        lane.fill(0);
        return lane;
    }
    return laneFromTritArray<40, UInt128>(value.unpack());
}

[[nodiscard]] inline Triple fromLane(TritLane40 lane) {
    if (!lane.isValid()) return Triple{Triple::OVERFLOW_DATA};
    const auto trits = laneToTritArray(lane);
    if (lane_detail::mantissaZero(trits, 33)) return Triple{0};
    return Triple::pack(trits);
}

[[nodiscard]] inline TritLane50 toLane(LongTriple value) {
    if (value.isSpecial() || native_ops::isInvalid(value)) return TritLane50::invalid();
    if (value.isZero()) {
        TritLane50 lane;
        lane.fill(0);
        return lane;
    }
    return laneFromTritArray<50, UInt128>(value.unpack());
}

[[nodiscard]] inline LongTriple fromLane(TritLane50 lane) {
    if (!lane.isValid()) return LongTriple{LongTriple::OVERFLOW_DATA};
    const auto trits = laneToTritArray(lane);
    if (lane_detail::mantissaZero(trits, 41)) return LongTriple{0};
    return LongTriple::pack(trits);
}

} // namespace sandbox

#endif // TERNARY_LANES_H
