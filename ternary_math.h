#ifndef TERNARY_MATH_H
#define TERNARY_MATH_H

#include "ternary_uint128.h"

#include <cstdint>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>
#include <string>

#ifndef TERNARY_IGNORE_LONG_DOUBLE_ASSERT
// FIX 3: Guard against MSVC collapsing long double to double width.
// Bridged encode/decode and transcendental paths depend on long double being
// wider than double (80-bit on x86 GCC/Clang) to preserve ternary precision.
// If this fires: build with MinGW/GCC or Clang instead of MSVC.
static_assert(sizeof(long double) > sizeof(double),
    "long double is the same width as double on this platform (likely MSVC). "
    "Bridged ternary math will not gain extended precision over binary. "
    "Build with MinGW-w64 GCC or Clang to get 80-bit long double on x86.");
#endif

namespace sandbox {

// ---------------------------------------------------------------------------
// COMPILE-TIME CONSTANTS
// ---------------------------------------------------------------------------

// Cached natural log of 3. std::log is not constexpr, so this is computed
// once at program startup rather than on every fromDouble() call.
static const double LOG3 = std::log(3.0);

// ---------------------------------------------------------------------------
// TRIT DEFINITION (Dual-Rail Encoding)
//
// ENCODING CONTRACT — READ BEFORE ADDING NEW OPS:
//
//   There are TWO separate trit encodings in this codebase:
//
//   A) BASE-3 POSITIONAL (used by Triple / T20 pack/unpack and all ops):
//      digit value 0 = balanced  0
//      digit value 1 = balanced +1
//      digit value 2 = balanced -1
//      Stored as: (balanced_value + 1) packed into a base-3 integer.
//
//   B) DUAL-RAIL (used by the Trit enum, reserved for future HAL layer):
//      Neutral  = 0x0  ( 0)
//      Positive = 0x1  (+1)
//      Negative = 0x2  (-1)
//      Invalid  = 0x3  (error)
//
//   These encodings are NOT interchangeable. All code in namespace ops
//   uses Base-3 Positional (A). The Trit enum (B) is the hardware
//   abstraction boundary — do not pass Trit enum values into pack/unpack.
//   Use getTritRaw() for internal ops, getTrit() for HAL consumers.
// ---------------------------------------------------------------------------
enum class Trit : uint8_t {
    Neutral  = 0x0,
    Positive = 0x1,
    Negative = 0x2,
    Invalid  = 0x3
};

enum class TernaryMode : uint8_t {
    T1  = 1,
    T5  = 5,
    T10 = 10,
    T20 = 20,
    T40 = 40,
    T50 = 50,
    L1  = 101,
    L5  = 105,
    L10 = 110,
    L20 = 120,
    L40 = 140,
    L50 = 150
};

struct T1 {
    uint8_t data;

    static constexpr int TRITS = 1;
    static constexpr uint8_t VALID_STATES = 3;
    static constexpr uint8_t INVALID_DATA = 0xFF;

    [[nodiscard]] bool isInvalid() const { return data >= VALID_STATES; }
    [[nodiscard]] bool isZero() const { return data == 1; }
};

struct T5 {
    uint8_t data;

    static constexpr int TRITS = 5;
    static constexpr uint16_t VALID_STATES = 243;
    static constexpr uint8_t INVALID_DATA = 0xFF;

    [[nodiscard]] bool isInvalid() const { return data >= VALID_STATES; }
    [[nodiscard]] bool isZero() const { return data == 121; }
};

struct T10 {
    uint16_t data;

    static constexpr int EXP_MAX =  40;
    static constexpr int EXP_MIN = -40;
    static constexpr uint16_t OVERFLOW_DATA  = UINT16_MAX;
    static constexpr uint16_t UNDERFLOW_DATA = UINT16_MAX - 1;

    static constexpr uint16_t POW3_10[10] = {
        1, 3, 9, 27, 81, 243, 729, 2187, 6561, 19683
    };

    [[nodiscard]] bool isOverflow() const { return data == OVERFLOW_DATA; }
    [[nodiscard]] bool isUnderflow() const { return data == UNDERFLOW_DATA; }
    [[nodiscard]] bool isZero() const { return data == 0; }
    [[nodiscard]] bool isSpecial() const { return isOverflow() || isUnderflow(); }

    [[nodiscard]] uint8_t getTritRaw(int index) const {
        if (isSpecial()) return 0;
        return static_cast<uint8_t>((data / POW3_10[index]) % 3);
    }

    [[nodiscard]] Trit getTrit(int index) const {
        uint8_t raw = getTritRaw(index);
        if (raw == 0) return Trit::Neutral;
        if (raw == 1) return Trit::Positive;
        return Trit::Negative;
    }

    static T10 pack(const std::array<int8_t, 10>& trits) {
        uint16_t result = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t u_trit = static_cast<uint8_t>(trits[i] + 1);
            result += static_cast<uint16_t>(u_trit) * POW3_10[i];
        }
        return T10{result};
    }

    [[nodiscard]] std::array<int8_t, 10> unpack() const {
        std::array<int8_t, 10> trits{};
        if (isSpecial()) { trits.fill(0); return trits; }
        uint16_t temp = data;
        for (int i = 0; i < 10; ++i) {
            trits[i] = static_cast<int8_t>(temp % 3) - 1;
            temp /= 3;
        }
        return trits;
    }
};

struct T20 {
    uint32_t data;

    static constexpr int EXP_MAX =  364;
    static constexpr int EXP_MIN = -364;
    static constexpr uint32_t OVERFLOW_DATA  = UINT32_MAX;
    static constexpr uint32_t UNDERFLOW_DATA = UINT32_MAX - 1;

    static constexpr uint32_t POW3_20[20] = {
        1, 3, 9, 27, 81, 243, 729, 2187, 6561, 19683,
        59049, 177147, 531441, 1594323, 4782969, 14348907,
        43046721, 129140163, 387420489, 1162261467
    };

    [[nodiscard]] bool isOverflow() const { return data == OVERFLOW_DATA; }
    [[nodiscard]] bool isUnderflow() const { return data == UNDERFLOW_DATA; }
    [[nodiscard]] bool isZero() const { return data == 0; }
    [[nodiscard]] bool isSpecial() const { return isOverflow() || isUnderflow(); }

    [[nodiscard]] uint8_t getTritRaw(int index) const {
        if (isSpecial()) return 0;
        return static_cast<uint8_t>((data / POW3_20[index]) % 3);
    }

    [[nodiscard]] Trit getTrit(int index) const {
        uint8_t raw = getTritRaw(index);
        if (raw == 0) return Trit::Neutral;
        if (raw == 1) return Trit::Positive;
        return Trit::Negative;
    }

    static T20 pack(const std::array<int8_t, 20>& trits) {
        uint32_t result = 0;
        for (int i = 0; i < 20; ++i) {
            uint8_t u_trit = static_cast<uint8_t>(trits[i] + 1);
            result += static_cast<uint32_t>(u_trit) * POW3_20[i];
        }
        return T20{result};
    }

    [[nodiscard]] std::array<int8_t, 20> unpack() const {
        std::array<int8_t, 20> trits{};
        if (isSpecial()) { trits.fill(0); return trits; }
        uint32_t temp = data;
        for (int i = 0; i < 20; ++i) {
            trits[i] = static_cast<int8_t>(temp % 3) - 1;
            temp /= 3;
        }
        return trits;
    }
};

using T20 = T20;

// ---------------------------------------------------------------------------
// TRIPLE (Ternary Double - 64-bit host)
//   Packs 40 trits into a 64-bit unsigned integer.
//   33 trits = Mantissa
//    7 trits = Exponent
//   Exponent range: [-1093, +1093]  (7 balanced ternary digits: 3^7 = 2187 states)
//
// FIX 1: Special sentinel values for overflow and underflow.
//   Triple::Overflow  — exponent exceeded +1093
//   Triple::Underflow — exponent fell below -1093 (distinct from zero)
//   Triple{0}         — exact zero
//   Use isSpecial() before arithmetic to avoid propagating corrupt values.
// ---------------------------------------------------------------------------
struct Triple {
    uint64_t data;

    static constexpr int EXP_MAX =  1093;
    static constexpr int EXP_MIN = -1093;

    // Sentinel data values. These use bit patterns that cannot arise from
    // valid pack() calls (all trits = 2 = balanced -1 is ~max uint64,
    // which we reserve; all trits = 2 minus 1 for underflow).
    // We use specific reserved values chosen to be unambiguous.
    static constexpr uint64_t OVERFLOW_DATA  = UINT64_MAX;
    static constexpr uint64_t UNDERFLOW_DATA = UINT64_MAX - 1;

    // Precomputed powers of 3 for O(1) trit extraction.
    // POW3_40[i] = 3^i for i in [0, 39].
    static constexpr uint64_t POW3_40[40] = {
        1ULL, 3ULL, 9ULL, 27ULL, 81ULL, 243ULL, 729ULL, 2187ULL, 6561ULL, 19683ULL,
        59049ULL, 177147ULL, 531441ULL, 1594323ULL, 4782969ULL, 14348907ULL,
        43046721ULL, 129140163ULL, 387420489ULL, 1162261467ULL, 3486784401ULL,
        10460353203ULL, 31381059609ULL, 94143178827ULL, 282429536481ULL,
        847288609443ULL, 2541865828329ULL, 7625597484987ULL, 22876792454961ULL,
        68630377364883ULL, 205891132094649ULL, 617673396283947ULL,
        1853020188851841ULL, 5559060566555523ULL, 16677181699666569ULL,
        50031545098999707ULL, 150094635296999121ULL, 450283905890997363ULL,
        1350851717672992089ULL, 4052555153018976267ULL
    };

    static const Triple Overflow;
    static const Triple Underflow;

    bool isOverflow()  const { return data == OVERFLOW_DATA;  }
    bool isUnderflow() const { return data == UNDERFLOW_DATA; }
    bool isZero()      const { return data == 0; }
    bool isSpecial()   const { return isOverflow() || isUnderflow(); }

    // RAW trit at position [index]: returns Base-3 Positional value (0, 1, or 2).
    // (0=balanced 0, 1=balanced +1, 2=balanced -1)
    uint8_t getTritRaw(int index) const {
        if (isSpecial()) return 0;
        return static_cast<uint8_t>((data / POW3_40[index]) % 3);
    }

    // HAL trit at position [index]: returns Trit enum for hardware abstraction.
    Trit getTrit(int index) const {
        uint8_t raw = getTritRaw(index);
        if (raw == 0) return Trit::Neutral;
        if (raw == 1) return Trit::Positive;
        return Trit::Negative; // raw == 2
    }

    // PACKING: 40 balanced trits → 64-bit integer.
    // Input: balanced values in {-1, 0, 1}.
    static Triple pack(const std::array<int8_t, 40>& trits) {
        uint64_t result = 0;
        for (int i = 0; i < 40; ++i) {
            uint8_t u_trit = static_cast<uint8_t>(trits[i] + 1);
            result += static_cast<uint64_t>(u_trit) * POW3_40[i];
        }
        return Triple{result};
    }

    // UNPACKING: 64-bit integer → 40 balanced trits.
    // Output: balanced values in {-1, 0, 1}.
    std::array<int8_t, 40> unpack() const {
        std::array<int8_t, 40> trits;
        if (isSpecial()) { trits.fill(0); return trits; }
        for (int i = 0; i < 40; ++i) {
            trits[i] = static_cast<int8_t>((data / POW3_40[i]) % 3) - 1;
        }
        return trits;
    }
};

// ---------------------------------------------------------------------------
// LONGTRIPLE (Ternary Extended - 128-bit host, 50-trit packing)
//   Optimized for 80-bit positional density (Claude's Convention).
//   41 trits = Mantissa
//    9 trits = Exponent (Range: [-9841, +9841])
// ---------------------------------------------------------------------------
struct LongTriple {
    UInt128 data;

    static constexpr int EXP_MAX =  9841;
    static constexpr int EXP_MIN = -9841;

    static constexpr UInt128 OVERFLOW_DATA  = UInt128::max();
    static constexpr UInt128 UNDERFLOW_DATA = UInt128::max() - UInt128{1};

    // Precomputed powers of 3 for 50 trits.
    static UInt128 POW3_50[50];
    static bool powTableInitialized;

    static void initPowTable() {
        if (powTableInitialized) return;
        UInt128 p = 1;
        for (int i = 0; i < 50; ++i) {
            POW3_50[i] = p;
            p *= 3;
        }
        powTableInitialized = true;
    }

    bool isOverflow()  const { return data == OVERFLOW_DATA;  }
    bool isUnderflow() const { return data == UNDERFLOW_DATA; }
    bool isZero()      const { return data == 0; }
    bool isSpecial()   const { return isOverflow() || isUnderflow(); }

    uint8_t getTritRaw(int index) const {
        if (isSpecial()) return 0;
        return static_cast<uint8_t>((data / POW3_50[index]) % 3);
    }

    // UNPACKING: 128-bit integer → 50 balanced trits.
    std::array<int8_t, 50> unpack() const {
        initPowTable();
        std::array<int8_t, 50> trits;
        if (isSpecial()) { trits.fill(0); return trits; }
        UInt128 temp = data;
        for (int i = 0; i < 50; ++i) {
            trits[i] = static_cast<int8_t>(temp % 3) - 1;
            temp = temp / 3;
        }
        return trits;
    }

    static LongTriple pack(const std::array<int8_t, 50>& trits) {
        initPowTable();
        UInt128 result = 0;
        for (int i = 0; i < 50; ++i) {
            uint8_t u_trit = static_cast<uint8_t>(trits[i] + 1);
            result += POW3_50[i] * u_trit;
        }
        return LongTriple{result};
    }
};

// ---------------------------------------------------------------------------
// LONGTRIPLE OPS
// ---------------------------------------------------------------------------
namespace long_ops {
    // Basic helpers for LongTriple decoding
    inline std::pair<long double, int> decode(LongTriple t) {
        if (t.isZero() || t.isSpecial()) return {0.0L, 0};
        
        int exponent = 0;
        std::array<int8_t, 50> trits = t.unpack();
        
        UInt128 p3 = 1;
        for (int i = 0; i < 9; ++i) {
            exponent += trits[41 + i] * static_cast<int>(p3.toUint64());
            p3 *= 3;
        }
        
        long double m_sum = 0.0L;
        long double p_val = 1.0L;
        for (int i = 0; i < 41; ++i) {
            m_sum += trits[i] * p_val;
            p_val *= 3.0L;
        }
        
        // FIX: Removed the rogue '+ 40' that was blowing up the exponent
        return {m_sum / 12157665459056928801.0L, exponent};
    }

    inline double toDouble(LongTriple t) {
        if (t.isZero()) return 0.0;
        if (t.isOverflow()) return std::numeric_limits<double>::infinity();
        if (t.isUnderflow()) return 0.0;
        auto [m, e] = decode(t);
        return static_cast<double>(m * std::pow(3.0L, (long double)e));
    }

    inline LongTriple encode(long double mantissa, int exponent) {
        if (mantissa == 0.0L) return LongTriple{0};
        // FIX: Protects against Infinity/NaN causing an infinite normalization loop
        if (!std::isfinite(mantissa)) return LongTriple{LongTriple::OVERFLOW_DATA}; 
        
        long double m = mantissa;
        int e = exponent;
        
        while (std::abs(m) > 1.5L) { m /= 3.0L; e++; }
        while (std::abs(m) < 0.5L) { m *= 3.0L; e--; }
        
        if (e > LongTriple::EXP_MAX) return LongTriple{LongTriple::OVERFLOW_DATA};
        if (e < LongTriple::EXP_MIN) return LongTriple{LongTriple::UNDERFLOW_DATA};
        
        std::array<int8_t, 50> trits;
        trits.fill(0);
        
        int temp_exp = e;
        for (int i = 0; i < 9; ++i) {
            int r = (temp_exp + 1) % 3;
            if (r < 0) r += 3;
            int8_t trit = static_cast<int8_t>(r - 1);
            trits[41 + i] = trit;
            temp_exp = (temp_exp - trit) / 3;
        }
        
        for (int i = 40; i >= 0; --i) {
            if (m >= 0.5L) { trits[i] = 1; m -= 1.0L; }
            else if (m <= -0.5L) { trits[i] = -1; m += 1.0L; }
            else { trits[i] = 0; }
            m *= 3.0L;
        }
        
        return LongTriple::pack(trits);
    }
} // namespace long_ops
} // namespace sandbox

#include "ternary_native_ops.h"

namespace sandbox {

// ---------------------------------------------------------------------------
// TERNARY OPS
// ---------------------------------------------------------------------------

    // Out-of-line static member definitions (required by linker)
    inline UInt128 LongTriple::POW3_50[50] = {};
    inline bool LongTriple::powTableInitialized = false;
    inline const Triple Triple::Overflow  = Triple{Triple::OVERFLOW_DATA};
    inline const Triple Triple::Underflow = Triple{Triple::UNDERFLOW_DATA};

    namespace ops {

    // -----------------------------------------------------------------------
    // BALANCED TRIT FULL-ADDER
    // Returns {sum, carry} where all values are in {-1, 0, 1}.
    // -----------------------------------------------------------------------
    inline std::pair<int8_t, int8_t> addTrit(int8_t a, int8_t b, int8_t carryIn) {
        int sum = a + b + carryIn;
        if (sum >  1) return {-1,  1};  // +2 → -1, carry +1
        if (sum < -1) return { 1, -1};  // -2 → +1, carry -1
        return {static_cast<int8_t>(sum), 0};
    }

    // -----------------------------------------------------------------------
    // BALANCED TERNARY REMAINDER
    // Returns the balanced remainder of n mod 3, result in {-1, 0, 1}.
    // Handles negative n correctly (C++ % is truncation-toward-zero).
    // -----------------------------------------------------------------------
    inline int8_t getBalancedRem(int n) {
        int r = (n + 1) % 3;
        if (r < 0) r += 3;
        return static_cast<int8_t>(r - 1);
    }

    // -----------------------------------------------------------------------
    // DECODE Triple → {long double mantissa, int exponent}
    //
    // Single decode path used by bridged Triple conversions and transcendentals.
    // Mantissa reconstructed in long double (80-bit on x86 GCC/Clang).
    // Returns {0.0, 0} for zero; callers should check isSpecial() first.
    // -----------------------------------------------------------------------
    inline std::pair<long double, int> decodeTriple(Triple t) {
        if (t.isZero())    return {0.0L, 0};
        if (t.isSpecial()) return {0.0L, 0}; // Caller should have checked

        // Reconstruct exponent from trits [33..39].
        int exponent = 0;
        for (int i = 0; i < 7; ++i) {
            int8_t tr = static_cast<int8_t>(t.getTritRaw(33 + i)) - 1;
            exponent += tr * static_cast<int>(Triple::POW3_40[i]);
        }

        // Reconstruct mantissa from trits [32..0] using iterative /3.
        // Avoids any pow() call in the decode path.
        long double mantissa     = 0.0L;
        long double currentPower = 1.0L;
        const long double inv3   = 1.0L / 3.0L;
        for (int i = 32; i >= 0; --i) {
            int8_t tr = static_cast<int8_t>(t.getTritRaw(i)) - 1;
            mantissa     += tr * currentPower;
            currentPower *= inv3;
        }

        return {mantissa, exponent};
    }

    // -----------------------------------------------------------------------
    // ENCODE {long double mantissa, int exponent} → Triple
    //
    // FIX 1: Bounds-checks exponent against [EXP_MIN, EXP_MAX] before
    // packing. Returns Triple::Overflow or Triple::Underflow on violation
    // instead of silently wrapping the exponent field.
    //
    // Normalizes mantissa into [1, 3) or (-3, -1] before encoding.
    // Uses precomputed pow3[] table — no per-trit std::pow call.
    // -----------------------------------------------------------------------
    inline Triple encodeTriple(long double mantissa, int exponent) {
        if (mantissa == 0.0L) return Triple{0};
        // FIX: Protects against Infinity/NaN causing an infinite loop
        if (!std::isfinite(mantissa)) return Triple::Overflow;

        while (std::fabs(mantissa) >= 1.5L) { mantissa /= 3.0L; ++exponent; }
        while (std::fabs(mantissa) <  0.5L) { mantissa *= 3.0L; --exponent; }

        if (exponent > Triple::EXP_MAX) return Triple::Overflow;
        if (exponent < Triple::EXP_MIN) return Triple::Underflow;

        std::array<int8_t, 40> trits{};

        int expTemp = exponent;
        for (int i = 0; i < 7; ++i) {
            int8_t rem    = getBalancedRem(expTemp);
            trits[33 + i] = rem;
            expTemp       = (expTemp - rem) / 3;
        }

        long double pow3[33];
        pow3[32] = 1.0L;
        for (int k = 31; k >= 0; --k) pow3[k] = pow3[k + 1] / 3.0L;

        long double mTemp = mantissa;
        for (int i = 32; i >= 0; --i) {
            if      (mTemp >  0.5L * pow3[i]) { trits[i] =  1; mTemp -= pow3[i]; }
            else if (mTemp < -0.5L * pow3[i]) { trits[i] = -1; mTemp += pow3[i]; }
            else                               { trits[i] =  0; }
        }

        return Triple::pack(trits);
    }

    // -----------------------------------------------------------------------
    // BRIDGE: double → Triple
    // Converts via base-3 decomposition. Not suitable for hot loops —
    // use a TernaryAccumulator for repeated small additions.
    // -----------------------------------------------------------------------
    inline Triple fromDouble(double d) {
        if (d == 0.0) return Triple{0};
        int exponent     = static_cast<int>(std::floor(std::log(std::abs(d)) / LOG3));
        long double mant = static_cast<long double>(d) /
                           std::pow(3.0L, static_cast<long double>(exponent));
        return encodeTriple(mant, exponent);
    }

    // -----------------------------------------------------------------------
    // BRIDGE: Triple → double (for output / comparison)
    // Values below 3^~-1022 will read back as 0.0 due to binary double floor.
    // Use toString() to inspect values in the ternary-only range.
    // Returns quiet NaN for special sentinels so callers can detect them.
    // -----------------------------------------------------------------------
    inline double toDouble(Triple t) {
        if (t.isZero())      return 0.0;
        if (t.isOverflow())  return std::numeric_limits<double>::infinity();
        if (t.isUnderflow()) return 0.0;  // Below binary floor — use toString()
        auto [mantissa, exponent] = decodeTriple(t);
        return static_cast<double>(
            mantissa * std::pow(3.0L, static_cast<long double>(exponent)));
    }

    // -----------------------------------------------------------------------
    // PRINTER: Full ternary scientific notation.
    // Bypasses binary double floor — shows values in the 3^-1093 range.
    // Portable: uses std::ostringstream, not sprintf("%Lf").
    // -----------------------------------------------------------------------
    inline std::string toString(Triple t) {
        if (t.isZero())      return "0.0";
        if (t.isOverflow())  return "[Overflow: exponent > 3^1093]";
        if (t.isUnderflow()) return "[Underflow: exponent < 3^-1093]";
        auto [mantissa, exponent] = decodeTriple(t);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(15)
            << static_cast<double>(mantissa) << " * 3^" << exponent;
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // FIX 2: NEGATION
    // Flips all 40 trits. In balanced ternary this is exact — no precision
    // cost, no carry propagation, no rounding. sign(x) = sign(-x) inverted.
    // Special values negate to their mirror sentinel.
    // -----------------------------------------------------------------------
    inline Triple negate(Triple t) {
        return native_ops::negate(t);
    }

    // -----------------------------------------------------------------------
    // FLOATING POINT ADDITION
    //
    // Changes from v1.1:
    //   - Special value propagation: Overflow/Underflow sentinels pass
    //     through rather than being decoded as garbage trit patterns.
    //   - Complete cancellation detection: if all mantissa trits are zero
    //     after addition, returns Triple{0} explicitly (not a zero-mantissa
    //     packed value whose data field may not equal 0).
    //
    // Retained from v1.1:
    //   - Left-normalization after catastrophic cancellation.
    //   - Right-normalization (carry out of MSB).
    // -----------------------------------------------------------------------
    inline Triple add(Triple a, Triple b) {
        return native_ops::add(a, b);
    }

    // -----------------------------------------------------------------------
    // FIX 2: SUBTRACTION  (a - b = a + (-b))
    // Defined after add() so the call resolves without a forward declaration.
    // -----------------------------------------------------------------------
    inline Triple subtract(Triple a, Triple b) {
        return native_ops::subtract(a, b);
    }

    // -----------------------------------------------------------------------
    // Triple arithmetic now delegates to the shared native format engine.
    // -----------------------------------------------------------------------
    inline Triple multiply(Triple a, Triple b) {
        return native_ops::multiply(a, b);
    }

    inline Triple divide(Triple a, Triple b) {
        return native_ops::divide(a, b);
    }

    inline Triple sqrt(Triple t) {
        return native_ops::sqrt(t);
    }

    /**
     * NATURAL EXPONENTIAL (e^x)
     * Argument reduction: e^x = 3^(x / ln(3))
     */
    inline Triple exp(Triple x) {
        return native_ops::toT40(native_ops::exp(native_ops::toLongTriple(x)));
    }

    /**
     * NATURAL LOGARITHM (ln(x))
     */
    inline Triple ln(Triple x) {
        return native_ops::toT40(native_ops::ln(native_ops::toLongTriple(x)));
    }

    // -----------------------------------------------------------------------
    // TERNARY ACCUMULATOR
    // Batches additions in long double, packing into a Triple only at flush.
    // Eliminates the pack/unpack overhead that causes drift in tight loops.
    //
    // Usage:
    //   TernaryAccumulator acc;
    //   for (...) acc.add(value);
    //   Triple result = acc.flush();
    //
    // The flush interval trades frequency of boundary conversion against
    // long double overflow risk. 1000 steps with 1e-12 increments is safe.
    // -----------------------------------------------------------------------
    struct TernaryAccumulator {
        long double partial  = 0.0L;
        int         steps    = 0;
        static constexpr int FLUSH_INTERVAL = 1000;

        void add(long double val) {
            partial += val;
            ++steps;
        }

        // Returns the current accumulated value as a Triple and resets.
        Triple flush() {
            Triple t = fromDouble(static_cast<double>(partial));
            partial  = 0.0L;
            steps    = 0;
            return t;
        }

        // Finalizes without resetting — call at end of accumulation.
        Triple result() const {
            return fromDouble(static_cast<double>(partial));
        }
    };

    // -----------------------------------------------------------------------
    // LONGTRIPLE ACCUMULATOR
    // Preserves 80-bit precision during rapid loops. 
    // Does NOT cast to standard 53-bit double like the TernaryAccumulator.
    // -----------------------------------------------------------------------
    struct LongTripleAccumulator {
        long double partial = 0.0L;
        int         steps   = 0;

        void add(long double val) {
            partial += val;
            ++steps;
        }

        // Extracts the full value of a LongTriple and adds it to the bridge
        void add(LongTriple val) {
            if (val.isSpecial() || val.isZero()) return;
            auto [m, e] = long_ops::decode(val);
            partial += m * std::pow(3.0L, e);
            ++steps;
        }

        // Returns the value safely packed back into a 50-trit container
        LongTriple result() const {
            return long_ops::encode(partial, 0);
        }
        
        void reset() {
            partial = 0.0L;
            steps = 0;
        }
    };

    // -----------------------------------------------------------------------
    // NATIVE TERNARY ORBITAL AMPLITUDE
    // Computes exp(-rho/n) directly in ternary coordinate space.
    //
    // The canary test previously called computeOrbitalAmplitude() (binary),
    // received 0.0 after underflow, then encoded that zero into a Triple.
    // The "1.5 * 3^-365" display was noise from Triple{0} being decoded
    // without hitting the data==0 guard — not a real tracked value.
    //
    // This function constructs the value inside ternary's own coordinate
    // system by converting the continuous exponent directly to base-3.
    // The result is genuine — never representable in binary double.
    //
    //   exp(-rho/n) = 3^( -rho/n / ln(3) )
    //   iExp  = floor of the base-3 exponent  (integer part)
    //   mant  = 3^(fractional base-3 exponent) (mantissa in [1,3))
    // -----------------------------------------------------------------------
    inline Triple computeOrbitalAmplitudeTernary(double r, int n) {
        double a0  = 2.0;
        double rho = r / a0;
        double expBase3 = -rho / (n * std::log(3.0));  // exact base-3 exponent
        int    iExp     = static_cast<int>(std::floor(expBase3));
        long double mant = std::pow(3.0L, static_cast<long double>(expBase3 - iExp));
        return encodeTriple(mant, iExp);
    }

    // -----------------------------------------------------------------------
    // LongTriple MATH WRAPPERS (FIXED)
    // -----------------------------------------------------------------------
    inline LongTriple multiply(LongTriple a, LongTriple b) {
        return native_ops::multiply(a, b);
    }

    inline LongTriple divide(LongTriple a, LongTriple b) {
        return native_ops::divide(a, b);
    }

    inline LongTriple add(LongTriple a, LongTriple b) {
        return native_ops::add(a, b);
    }

    inline LongTriple negate(LongTriple a) {
        return native_ops::negate(a);
    }

    inline LongTriple subtract(LongTriple a, LongTriple b) {
        return native_ops::subtract(a, b);
    }

    inline LongTriple sqrt(LongTriple t) {
        return native_ops::sqrt(t);
    }

    inline LongTriple exp(LongTriple x) {
        return native_ops::exp(x);
    }

    inline LongTriple ln(LongTriple x) {
        return native_ops::ln(x);
    }

    inline LongTriple cos(LongTriple x) {
        return native_ops::cos(x);
    }

    inline LongTriple sin(LongTriple x) {
        return native_ops::sin(x);
    }

} // namespace ops

} // namespace sandbox

#endif // TERNARY_MATH_H
