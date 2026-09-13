// =============================================================================
// ternary_scalar.h - Unified positional base-3 scalar type
// =============================================================================
//
// TernaryScalar<N> is the canonical in-memory representation for ternary
// numeric values of N trits. Ordinary storage is positional base-3: each value
// is a single integer in [0, 3^N) where trit i has value
// (data / 3^i) % 3 - 1. Raw zero is reserved as canonical numeric zero; use
// unpackPositional() only when deliberately interpreting the physical payload.
//
// This is the ONLY representation that arithmetic operates on directly.
// Lane types (TritLane<N>) are SIMD transport views computed on demand.
//
// Storage is the smallest unsigned integer that fits 3^N:
//   N <=  5 → uint8_t     (3^5  = 243     < 256)
//   N <= 10 → uint16_t    (3^10 = 59049   < 65536)
//   N <= 20 → uint32_t    (3^20 ~ 3.49e9  < 4.29e9)
//   N <= 40 → uint64_t    (3^40 ~ 1.22e19 < 1.84e19)
//   N <= 50 → UInt128     (3^50 ~ 7.18e23 < 3.40e38)

#pragma once
#ifndef TERNARY_SCALAR_H
#define TERNARY_SCALAR_H

#include "ternary_uint128.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace sandbox {

// ---------------------------------------------------------------------------
// TRIT DEFINITION (Dual-Rail Encoding)
// Moved here from ternary_math.h so TernaryScalar can provide getTrit().
// ---------------------------------------------------------------------------
enum class Trit : uint8_t {
    Neutral  = 0x0,
    Positive = 0x1,
    Negative = 0x2,
    Invalid  = 0x3
};

namespace scalar_detail {

template<int Trits>
using ScalarStorage =
    std::conditional_t<(Trits <= 5),  uint8_t,
    std::conditional_t<(Trits <= 10), uint16_t,
    std::conditional_t<(Trits <= 20), uint32_t,
    std::conditional_t<(Trits <= 40), uint64_t,
                                      UInt128>>>>;

template<typename T>
struct StorageTraits {
    static constexpr T maxValue() { return std::numeric_limits<T>::max(); }
    static constexpr T maxValueMinus1() { return std::numeric_limits<T>::max() - static_cast<T>(1); }
    static constexpr T one() { return static_cast<T>(1); }
    static constexpr T zero() { return static_cast<T>(0); }
};

template<>
struct StorageTraits<UInt128> {
    static constexpr UInt128 maxValue() { return UInt128::max(); }
    static constexpr UInt128 maxValueMinus1() { return UInt128{UINT64_MAX, UINT64_MAX - 1}; }
    static constexpr UInt128 one() { return UInt128{1}; }
    static constexpr UInt128 zero() { return UInt128{}; }
};

} // namespace scalar_detail

// ---------------------------------------------------------------------------
// TernaryScalar<N> — the canonical positional base-3 numeric type.
// ---------------------------------------------------------------------------
template<int Trits>
struct TernaryScalar {
    static_assert(Trits >= 1 && Trits <= 50, "TernaryScalar supports 1-50 trits");

    using Storage = scalar_detail::ScalarStorage<Trits>;
    using Traits = scalar_detail::StorageTraits<Storage>;

    Storage data = Traits::zero();

    static constexpr int trits = Trits;

    // Sentinel values — at the top of the storage range, unreachable by pack().
    static constexpr Storage OVERFLOW_DATA  = Traits::maxValue();
    static constexpr Storage UNDERFLOW_DATA = Traits::maxValueMinus1();

    // --- Predicates ---------------------------------------------------------

    [[nodiscard]] bool isZero() const {
        if constexpr (std::is_same_v<Storage, UInt128>) {
            return data.isZero();
        } else {
            return data == 0;
        }
    }

    [[nodiscard]] bool isOverflow() const { return data == OVERFLOW_DATA; }
    [[nodiscard]] bool isUnderflow() const { return data == UNDERFLOW_DATA; }
    [[nodiscard]] bool isSpecial() const { return isOverflow() || isUnderflow(); }

    [[nodiscard]] static Storage validStateCount() {
        const Storage highestPower = pow3Table()[static_cast<std::size_t>(Trits - 1)];
        if constexpr (std::is_same_v<Storage, UInt128>) {
            return highestPower * static_cast<uint32_t>(3);
        } else {
            return static_cast<Storage>(highestPower * 3);
        }
    }

    // A raw value in [3^N, UNDERFLOW_DATA) is neither a positional payload nor
    // one of the two defined floating-point sentinels.
    [[nodiscard]] bool isInvalid() const {
        return !isSpecial() && data >= validStateCount();
    }

    // --- Power-of-3 table ---------------------------------------------------

    static const std::array<Storage, Trits>& pow3Table() {
        static const auto table = []() {
            std::array<Storage, Trits> t{};
            if constexpr (std::is_same_v<Storage, UInt128>) {
                UInt128 p{1};
                for (int i = 0; i < Trits; ++i) {
                    t[static_cast<std::size_t>(i)] = p;
                    p = p * static_cast<uint32_t>(3);
                }
            } else {
                Storage p = 1;
                for (int i = 0; i < Trits; ++i) {
                    t[static_cast<std::size_t>(i)] = p;
                    p = static_cast<Storage>(p * 3);
                }
            }
            return t;
        }();
        return table;
    }

    // --- Trit access --------------------------------------------------------

    // Returns the semantic positional digit:
    //   0 = balanced -1, 1 = balanced 0, 2 = balanced +1.
    // The canonical zero sentinel expands to neutral digits. Invalid indices,
    // invalid raw payloads, and special values return 3.
    [[nodiscard]] uint8_t getTritRaw(int index) const {
        if (index < 0 || index >= Trits || isSpecial() || isInvalid()) return 3;
        if (isZero()) return 1;
        const auto& pow = pow3Table();
        if constexpr (std::is_same_v<Storage, UInt128>) {
            return static_cast<uint8_t>((data / pow[static_cast<std::size_t>(index)]) % static_cast<uint32_t>(3));
        } else {
            return static_cast<uint8_t>((data / pow[static_cast<std::size_t>(index)]) % 3);
        }
    }

    // HAL trit at position [index]: returns Trit enum for hardware abstraction.
    [[nodiscard]] Trit getTrit(int index) const {
        uint8_t raw = getTritRaw(index);
        if (raw == 0) return Trit::Negative;
        if (raw == 1) return Trit::Neutral;
        if (raw == 2) return Trit::Positive;
        return Trit::Invalid;
    }

    // --- Pack / Unpack ------------------------------------------------------

    // Pack: balanced trit array {-1, 0, +1} → positional base-3 integer.
    [[nodiscard]] static TernaryScalar pack(const std::array<int8_t, Trits>& trits) {
        for (int i = 0; i < Trits; ++i) {
            const int8_t trit = trits[static_cast<std::size_t>(i)];
            if (trit < -1 || trit > 1) return TernaryScalar{OVERFLOW_DATA};
        }

        const auto& pow = pow3Table();
        if constexpr (std::is_same_v<Storage, UInt128>) {
            UInt128 result{};
            for (int i = 0; i < Trits; ++i) {
                uint8_t u_trit = static_cast<uint8_t>(trits[static_cast<std::size_t>(i)] + 1);
                result = result + pow[static_cast<std::size_t>(i)] * static_cast<uint32_t>(u_trit);
            }
            // Raw zero is reserved for canonical floating zero, so the
            // all-negative positional pattern is not a representable payload.
            if (result.isZero()) return TernaryScalar{OVERFLOW_DATA};
            return TernaryScalar{result};
        } else {
            Storage result = 0;
            for (int i = 0; i < Trits; ++i) {
                uint8_t u_trit = static_cast<uint8_t>(trits[static_cast<std::size_t>(i)] + 1);
                result += static_cast<Storage>(u_trit) * pow[static_cast<std::size_t>(i)];
            }
            if (result == 0) return TernaryScalar{OVERFLOW_DATA};
            return TernaryScalar{result};
        }
    }

    // Decode the physical positional payload without applying the numeric-zero
    // sentinel. This is for storage formats (for example packed model weights)
    // that deliberately use TernaryScalar's carrier as a raw trit container.
    [[nodiscard]] std::array<int8_t, Trits> unpackPositional() const {
        std::array<int8_t, Trits> out{};
        if (isSpecial() || isInvalid()) return out;
        if constexpr (std::is_same_v<Storage, UInt128>) {
            UInt128 temp = data;
            for (int i = 0; i < Trits; ++i) {
                uint32_t raw = 0;
                temp = temp.divMod3(raw);
                out[static_cast<std::size_t>(i)] = static_cast<int8_t>(raw) - 1;
            }
        } else {
            Storage temp = data;
            for (int i = 0; i < Trits; ++i) {
                out[static_cast<std::size_t>(i)] = static_cast<int8_t>(temp % 3) - 1;
                temp /= 3;
            }
        }
        return out;
    }

    // Unpack a numeric scalar. Raw zero is a reserved canonical-zero sentinel,
    // so it expands to all neutral trits rather than the physical all-negative
    // digit pattern returned by unpackPositional().
    [[nodiscard]] std::array<int8_t, Trits> unpack() const {
        if (isZero()) return {};
        return unpackPositional();
    }

    // --- Equality -----------------------------------------------------------

    [[nodiscard]] bool operator==(const TernaryScalar& other) const {
        return data == other.data;
    }

    [[nodiscard]] bool operator!=(const TernaryScalar& other) const {
        return !(*this == other);
    }

    // --- Legacy compatibility ------------------------------------------------
    // These members preserve the API surface of the original T10/T20/Triple/
    // LongTriple types so existing code compiles without changes when those
    // types become aliases for TernaryScalar<N>.

    // Convenient sentinel instances.
    static const TernaryScalar Overflow;
    static const TernaryScalar Underflow;

    // HAL trit at position [index]: returns Trit enum for hardware abstraction.
    // Forward-declared; requires Trit enum from ternary_math.h.
    // Implemented out-of-line after Trit is available.

    // Exponent constants — computed from the float format traits.
    // For integer types (T1, T5), these are unused but present for uniformity.
    // The actual mantissa/exponent split is defined in native_ops::TernaryFloatFormat.
    // These constants match: EXP_MAX = (3^exponent_trits - 1) / 2
    // T10:  4 exp trits → EXP_MAX = 40
    // T20:  6 exp trits → EXP_MAX = 364
    // T40:  7 exp trits → EXP_MAX = 1093
    // T50:  9 exp trits → EXP_MAX = 9841
    static constexpr int EXP_MAX =
        (Trits == 10) ?   40 :
        (Trits == 20) ?  364 :
        (Trits == 40) ? 1093 :
        (Trits == 50) ? 9841 : 0;
    static constexpr int EXP_MIN = -EXP_MAX;

    // POW3 table as a C-style array accessor for backward compatibility.
    // Usage: TernaryScalar<40>::POW3(i) instead of Triple::POW3_40[i]
    [[nodiscard]] static Storage POW3(int index) {
        if (index < 0 || index >= Trits) {
            throw std::out_of_range("TernaryScalar::POW3 index out of range");
        }
        return pow3Table()[static_cast<std::size_t>(index)];
    }

    // For LongTriple compatibility — pow table is lazy-initialized regardless,
    // but callers that used to call LongTriple::initPowTable() can call this.
    static void initPowTable() { (void)pow3Table(); }
};

// Static sentinel definitions
template<int Trits>
inline const TernaryScalar<Trits> TernaryScalar<Trits>::Overflow{TernaryScalar<Trits>::OVERFLOW_DATA};
template<int Trits>
inline const TernaryScalar<Trits> TernaryScalar<Trits>::Underflow{TernaryScalar<Trits>::UNDERFLOW_DATA};

// ---------------------------------------------------------------------------
// Verification: TernaryScalar<40> must fit in uint64_t.
// This is the compactness proof — positional base-3 is the only encoding
// where T40 (the native word) fits in a single 64-bit integer.
// ---------------------------------------------------------------------------
static_assert(sizeof(TernaryScalar<40>::Storage) == sizeof(uint64_t),
    "T40 must fit in uint64_t — this is the canonical representation invariant");
static_assert(sizeof(TernaryScalar<10>::Storage) == sizeof(uint16_t),
    "T10 must fit in uint16_t");
static_assert(sizeof(TernaryScalar<20>::Storage) == sizeof(uint32_t),
    "T20 must fit in uint32_t");

} // namespace sandbox

#endif // TERNARY_SCALAR_H
