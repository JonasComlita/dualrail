#ifndef TERNARY_MATH_H
#define TERNARY_MATH_H

#include "ternary_scalar.h"
#include "ternary_uint128.h"

#include <cstdint>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>
#include <string>

#ifndef TERNARY_IGNORE_LONG_DOUBLE_ASSERT
static_assert(sizeof(long double) > sizeof(double),
    "long double is the same width as double on this platform (likely MSVC). "
    "Bridged ternary math will not gain extended precision over binary. "
    "Build with MinGW-w64 GCC or Clang to get 80-bit long double on x86.");
#endif

namespace sandbox {

// ---------------------------------------------------------------------------
// COMPILE-TIME CONSTANTS
// ---------------------------------------------------------------------------

static const double LOG3 = std::log(3.0);

// ---------------------------------------------------------------------------
// TRIT DEFINITION
// Trit enum is now defined in ternary_scalar.h.
//
// ENCODING CONTRACT — READ BEFORE ADDING NEW OPS:
//   A) BASE-3 POSITIONAL (used by TernaryScalar pack/unpack and all ops):
//      digit 0 = balanced -1, digit 1 = balanced 0, digit 2 = balanced +1
//   B) DUAL-RAIL (Trit enum, for HAL):
//      Neutral=0x0, Positive=0x1, Negative=0x2, Invalid=0x3
//   These encodings are NOT interchangeable.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// CANONICAL TYPE ALIASES
// T10, T20, Triple, LongTriple are now aliases for TernaryScalar<N>.
// All methods (pack, unpack, getTritRaw, getTrit, isZero, isSpecial, etc.)
// are provided by the TernaryScalar template in ternary_scalar.h.
// ---------------------------------------------------------------------------
using T10 = TernaryScalar<10>;
using T20 = TernaryScalar<20>;
using Triple = TernaryScalar<40>;
using LongTriple = TernaryScalar<50>;

// ---------------------------------------------------------------------------
// LONGTRIPLE OPS
// ---------------------------------------------------------------------------
namespace long_ops {
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
        if (mantissa == 0.0L) return LongTriple{UInt128{}};
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

    namespace ops {

    // -----------------------------------------------------------------------
    // BALANCED TRIT FULL-ADDER
    // Returns {sum, carry} where all values are in {-1, 0, 1}.
    // -----------------------------------------------------------------------
    inline std::pair<int8_t, int8_t> addTrit(int8_t a, int8_t b, int8_t carryIn) {
        int sum = a + b + carryIn;
        if (sum >  1) return {-1,  1};
        if (sum < -1) return { 1, -1};
        return {static_cast<int8_t>(sum), 0};
    }

    // -----------------------------------------------------------------------
    // BALANCED TERNARY REMAINDER
    // -----------------------------------------------------------------------
    inline int8_t getBalancedRem(int n) {
        int r = (n + 1) % 3;
        if (r < 0) r += 3;
        return static_cast<int8_t>(r - 1);
    }

    // -----------------------------------------------------------------------
    // DECODE Triple -> {long double mantissa, int exponent}
    // -----------------------------------------------------------------------
    inline std::pair<long double, int> decodeTriple(Triple t) {
        if (t.isZero())    return {0.0L, 0};
        if (t.isSpecial()) return {0.0L, 0};

        int exponent = 0;
        for (int i = 0; i < 7; ++i) {
            int8_t tr = static_cast<int8_t>(t.getTritRaw(33 + i)) - 1;
            exponent += tr * static_cast<int>(Triple::POW3(i));
        }

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
    // ENCODE {long double mantissa, int exponent} -> Triple
    // -----------------------------------------------------------------------
    inline Triple encodeTriple(long double mantissa, int exponent) {
        if (mantissa == 0.0L) return Triple{0};
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

    inline Triple fromDouble(double d) {
        if (d == 0.0) return Triple{0};
        int exponent     = static_cast<int>(std::floor(std::log(std::abs(d)) / LOG3));
        long double mant = static_cast<long double>(d) /
                           std::pow(3.0L, static_cast<long double>(exponent));
        return encodeTriple(mant, exponent);
    }

    inline double toDouble(Triple t) {
        if (t.isZero())      return 0.0;
        if (t.isOverflow())  return std::numeric_limits<double>::infinity();
        if (t.isUnderflow()) return 0.0;
        auto [mantissa, exponent] = decodeTriple(t);
        return static_cast<double>(
            mantissa * std::pow(3.0L, static_cast<long double>(exponent)));
    }

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

    inline Triple negate(Triple t) {
        return native_ops::negate(t);
    }

    inline Triple add(Triple a, Triple b) {
        return native_ops::add(a, b);
    }

    inline Triple subtract(Triple a, Triple b) {
        return native_ops::subtract(a, b);
    }

    inline Triple multiply(Triple a, Triple b) {
        return native_ops::multiply(a, b);
    }

    inline Triple divide(Triple a, Triple b) {
        return native_ops::divide(a, b);
    }

    inline Triple sqrt(Triple t) {
        return native_ops::sqrt(t);
    }

    inline Triple exp(Triple x) {
        return native_ops::toT40(native_ops::exp(native_ops::toLongTriple(x)));
    }

    inline Triple ln(Triple x) {
        return native_ops::toT40(native_ops::ln(native_ops::toLongTriple(x)));
    }

    struct TernaryAccumulator {
        long double partial  = 0.0L;
        int         steps    = 0;
        static constexpr int FLUSH_INTERVAL = 1000;

        void add(long double val) { partial += val; ++steps; }

        Triple flush() {
            Triple t = fromDouble(static_cast<double>(partial));
            partial  = 0.0L;
            steps    = 0;
            return t;
        }

        Triple result() const {
            return fromDouble(static_cast<double>(partial));
        }
    };

    struct LongTripleAccumulator {
        long double partial = 0.0L;
        int         steps   = 0;

        void add(long double val) { partial += val; ++steps; }

        void add(LongTriple val) {
            if (val.isSpecial() || val.isZero()) return;
            auto [m, e] = long_ops::decode(val);
            partial += m * std::pow(3.0L, e);
            ++steps;
        }

        LongTriple result() const {
            return long_ops::encode(partial, 0);
        }

        void reset() { partial = 0.0L; steps = 0; }
    };

    inline Triple computeOrbitalAmplitudeTernary(double r, int n) {
        double a0  = 2.0;
        double rho = r / a0;
        double expBase3 = -rho / (n * std::log(3.0));
        int    iExp     = static_cast<int>(std::floor(expBase3));
        long double mant = std::pow(3.0L, static_cast<long double>(expBase3 - iExp));
        return encodeTriple(mant, iExp);
    }

    inline LongTriple multiply(LongTriple a, LongTriple b) { return native_ops::multiply(a, b); }
    inline LongTriple divide(LongTriple a, LongTriple b) { return native_ops::divide(a, b); }
    inline LongTriple add(LongTriple a, LongTriple b) { return native_ops::add(a, b); }
    inline LongTriple negate(LongTriple a) { return native_ops::negate(a); }
    inline LongTriple subtract(LongTriple a, LongTriple b) { return native_ops::subtract(a, b); }
    inline LongTriple sqrt(LongTriple t) { return native_ops::sqrt(t); }
    inline LongTriple exp(LongTriple x) { return native_ops::exp(x); }
    inline LongTriple ln(LongTriple x) { return native_ops::ln(x); }
    inline LongTriple cos(LongTriple x) { return native_ops::cos(x); }
    inline LongTriple sin(LongTriple x) { return native_ops::sin(x); }

} // namespace ops

} // namespace sandbox

#endif // TERNARY_MATH_H
