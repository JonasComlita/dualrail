#include "ternary_vm.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using sandbox::LongTriple;

static int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

std::string toStringUnsigned(unsigned __int128 value) {
    if (value == 0) return "0";
    std::string out;
    while (value != 0) {
        const unsigned digit = static_cast<unsigned>(value % 10);
        out.insert(out.begin(), static_cast<char>('0' + digit));
        value /= 10;
    }
    return out;
}

std::string toStringSigned(__int128 value) {
    if (value < 0) return "-" + toStringUnsigned(static_cast<unsigned __int128>(-value));
    return toStringUnsigned(static_cast<unsigned __int128>(value));
}

unsigned __int128 pow3(int n) {
    unsigned __int128 value = 1;
    for (int i = 0; i < n; ++i) value *= 3;
    return value;
}

int8_t balancedRem(__int128 n) {
    __int128 r = n % 3;
    if (r > 1) r -= 3;
    if (r < -1) r += 3;
    return static_cast<int8_t>(r);
}

std::array<int8_t, 41> encodeBalanced41(__int128 n) {
    std::array<int8_t, 41> trits{};
    for (int i = 0; i < 41; ++i) {
        const int8_t trit = balancedRem(n);
        trits[i] = trit;
        n = (n - trit) / 3;
    }
    expect(n == 0, "integer too large for 41-trit mantissa");
    return trits;
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
    expect(e == 0, "exponent too large for 9-trit field");
    return LongTriple::pack(trits);
}

LongTriple fromInt(__int128 n) {
    if (n == 0) return LongTriple{0};
    return packMantissaExp(encodeBalanced41(n), 40);
}

LongTriple fromLongDouble(long double value) {
    return sandbox::long_ops::encode(value, 0);
}

long double toLongDouble(LongTriple t) {
    if (t.isZero()) return 0.0L;
    auto [m, e] = sandbox::long_ops::decode(t);
    return m * std::pow(3.0L, static_cast<long double>(e));
}

__int128 mantissaToInt(const std::array<int8_t, 50>& trits) {
    __int128 value = 0;
    __int128 place = 1;
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

bool exactIntegerValue(LongTriple t, __int128& out) {
    if (t.isZero()) {
        out = 0;
        return true;
    }
    if (t.isSpecial()) return false;

    const auto trits = t.unpack();
    const __int128 mantissa = mantissaToInt(trits);
    const int exponent = exponentOf(trits);

    if (exponent >= 40) {
        out = mantissa * static_cast<__int128>(pow3(exponent - 40));
        return true;
    }

    const __int128 divisor = static_cast<__int128>(pow3(40 - exponent));
    if (mantissa % divisor != 0) return false;
    out = mantissa / divisor;
    return true;
}

bool near(long double got, long double want, long double relTol, long double absTol) {
    const long double diff = std::fabs(got - want);
    const long double scale = std::max(std::fabs(want), 1.0L);
    return diff <= absTol || diff <= relTol * scale;
}

void expectNear(LongTriple got, long double want, const std::string& label,
                long double relTol = 1e-10L, long double absTol = 1e-12L) {
    const long double actual = toLongDouble(got);
    if (near(actual, want, relTol, absTol)) return;
    ++g_failures;
    std::cout << "FAIL: " << label << "\n";
    std::cout << "  got=" << static_cast<double>(actual)
              << " want=" << static_cast<double>(want) << "\n";
}

LongTriple add(LongTriple a, LongTriple b) { return sandbox::ops::add(a, b); }
LongTriple sub(LongTriple a, LongTriple b) { return sandbox::ops::subtract(a, b); }
LongTriple mul(LongTriple a, LongTriple b) { return sandbox::ops::multiply(a, b); }
LongTriple div(LongTriple a, LongTriple b) { return sandbox::ops::divide(a, b); }
LongTriple neg(LongTriple a) { return sandbox::ops::negate(a); }

std::pair<LongTriple, LongTriple> fibonacciFastDoubling(int n) {
    if (n == 0) return {fromInt(0), fromInt(1)};

    auto [a, b] = fibonacciFastDoubling(n / 2);
    LongTriple twoBMinusA = sub(mul(fromInt(2), b), a);
    LongTriple c = mul(a, twoBMinusA);
    LongTriple d = add(mul(a, a), mul(b, b));

    if ((n % 2) == 0) return {c, d};
    return {d, add(c, d)};
}

__int128 fibonacciReference(int n) {
    __int128 a = 0;
    __int128 b = 1;
    for (int i = 0; i < n; ++i) {
        const __int128 next = a + b;
        a = b;
        b = next;
    }
    return a;
}

LongTriple factorialRecursive(int n) {
    if (n <= 1) return fromInt(1);
    return mul(fromInt(n), factorialRecursive(n - 1));
}

__int128 factorialReference(int n) {
    __int128 out = 1;
    for (int i = 2; i <= n; ++i) out *= i;
    return out;
}

struct ComplexLT {
    LongTriple re;
    LongTriple im;
};

ComplexLT cadd(ComplexLT a, ComplexLT b) {
    return {add(a.re, b.re), add(a.im, b.im)};
}

ComplexLT csub(ComplexLT a, ComplexLT b) {
    return {sub(a.re, b.re), sub(a.im, b.im)};
}

ComplexLT cmul(ComplexLT a, ComplexLT b) {
    return {
        sub(mul(a.re, b.re), mul(a.im, b.im)),
        add(mul(a.re, b.im), mul(a.im, b.re)),
    };
}

ComplexLT twiddle(int k, int n) {
    static constexpr long double PI = 3.141592653589793238462643383279502884L;
    const long double angle = -2.0L * PI * static_cast<long double>(k)
                            / static_cast<long double>(n);
    return {fromLongDouble(std::cos(angle)), fromLongDouble(std::sin(angle))};
}

std::vector<ComplexLT> dft(const std::vector<ComplexLT>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<ComplexLT> out(n, {fromInt(0), fromInt(0)});

    for (int k = 0; k < n; ++k) {
        ComplexLT sum{fromInt(0), fromInt(0)};
        for (int j = 0; j < n; ++j) {
            sum = cadd(sum, cmul(input[j], twiddle(k * j, n)));
        }
        out[k] = sum;
    }

    return out;
}

std::vector<ComplexLT> fftRecursive(const std::vector<ComplexLT>& input) {
    const int n = static_cast<int>(input.size());
    if (n == 1) return input;

    std::vector<ComplexLT> even;
    std::vector<ComplexLT> odd;
    even.reserve(n / 2);
    odd.reserve(n / 2);
    for (int i = 0; i < n; ++i) {
        ((i % 2) == 0 ? even : odd).push_back(input[i]);
    }

    std::vector<ComplexLT> e = fftRecursive(even);
    std::vector<ComplexLT> o = fftRecursive(odd);
    std::vector<ComplexLT> out(n);

    for (int k = 0; k < n / 2; ++k) {
        ComplexLT t = cmul(twiddle(k, n), o[k]);
        out[k] = cadd(e[k], t);
        out[k + n / 2] = csub(e[k], t);
    }

    return out;
}

LongTriple expMaclaurin(LongTriple x, int terms) {
    LongTriple sum = fromInt(1);
    LongTriple term = fromInt(1);
    for (int k = 1; k <= terms; ++k) {
        term = div(mul(term, x), fromInt(k));
        sum = add(sum, term);
    }
    return sum;
}

LongTriple sinMaclaurin(LongTriple x, int terms) {
    LongTriple x2 = mul(x, x);
    LongTriple term = x;
    LongTriple sum = x;

    for (int k = 1; k <= terms; ++k) {
        const int denom = (2 * k) * (2 * k + 1);
        term = neg(div(mul(term, x2), fromInt(denom)));
        sum = add(sum, term);
    }

    return sum;
}

LongTriple cosMaclaurin(LongTriple x, int terms) {
    LongTriple x2 = mul(x, x);
    LongTriple term = fromInt(1);
    LongTriple sum = fromInt(1);

    for (int k = 1; k <= terms; ++k) {
        const int denom = (2 * k - 1) * (2 * k);
        term = neg(div(mul(term, x2), fromInt(denom)));
        sum = add(sum, term);
    }

    return sum;
}

void testFibonacciAndRecursion() {
    std::cout << "[1] fibonacci fast-doubling recursion\n";

    for (int n = 0; n <= 80; ++n) {
        LongTriple got = fibonacciFastDoubling(n).first;
        expectNear(got, static_cast<long double>(fibonacciReference(n)),
                   "F(" + std::to_string(n) + ") fast-doubling value",
                   1e-12L, 1e-12L);
    }

    std::cout << "[2] recursive factorial\n";
    for (int n = 0; n <= 20; ++n) {
        LongTriple got = factorialRecursive(n);
        __int128 actual = 0;
        expect(exactIntegerValue(got, actual),
               std::to_string(n) + "! should decode as an exact integer");
        expect(actual == factorialReference(n),
               std::to_string(n) + "! expected " + toStringSigned(factorialReference(n)));
    }
}

void testDftAndFft() {
    std::cout << "[3] DFT vs recursive FFT\n";

    const std::vector<long double> realInput = {
        1.0L, -2.0L, 3.5L, -4.25L, 5.125L, -6.0L, 7.75L, -8.5L
    };
    const std::vector<long double> imagInput = {
        0.0L, 0.5L, -1.0L, 1.5L, -2.0L, 2.5L, -3.0L, 3.5L
    };

    std::vector<ComplexLT> input;
    for (std::size_t i = 0; i < realInput.size(); ++i) {
        input.push_back({fromLongDouble(realInput[i]), fromLongDouble(imagInput[i])});
    }

    const std::vector<ComplexLT> direct = dft(input);
    const std::vector<ComplexLT> fast = fftRecursive(input);

    for (std::size_t k = 0; k < direct.size(); ++k) {
        expectNear(fast[k].re, toLongDouble(direct[k].re),
                   "FFT real bin " + std::to_string(k), 1e-9L, 1e-10L);
        expectNear(fast[k].im, toLongDouble(direct[k].im),
                   "FFT imag bin " + std::to_string(k), 1e-9L, 1e-10L);
    }
}

void testTaylorMaclaurin() {
    std::cout << "[4] Taylor/Maclaurin series\n";

    const std::vector<long double> xs = {-1.0L, -0.5L, 0.0L, 0.5L, 1.0L};
    for (long double x : xs) {
        LongTriple tx = fromLongDouble(x);
        expectNear(expMaclaurin(tx, 18), std::exp(x),
                   "exp Maclaurin x=" + std::to_string(static_cast<double>(x)),
                   1e-10L, 1e-12L);
        expectNear(sinMaclaurin(tx, 14), std::sin(x),
                   "sin Maclaurin x=" + std::to_string(static_cast<double>(x)),
                   1e-10L, 1e-12L);
        expectNear(cosMaclaurin(tx, 14), std::cos(x),
                   "cos Maclaurin x=" + std::to_string(static_cast<double>(x)),
                   1e-10L, 1e-12L);
    }

    LongTriple identity = mul(expMaclaurin(fromLongDouble(0.75L), 18),
                              expMaclaurin(fromLongDouble(-0.75L), 18));
    expectNear(identity, 1.0L, "exp(x) * exp(-x)", 1e-10L, 1e-12L);
}

void testNativeWrappers() {
    std::cout << "[5] native ops wrappers\n";

    const std::vector<long double> xs = {-1.0L, -0.5L, 0.0L, 0.5L, 1.0L};
    for (long double x : xs) {
        LongTriple tx = fromLongDouble(x);
        expectNear(sandbox::ops::exp(tx), std::exp(x),
                   "ops::exp x=" + std::to_string(static_cast<double>(x)),
                   1e-9L, 1e-11L);
        expectNear(sandbox::ops::sin(tx), std::sin(x),
                   "ops::sin x=" + std::to_string(static_cast<double>(x)),
                   1e-7L, 1e-9L);
        expectNear(sandbox::ops::cos(tx), std::cos(x),
                   "ops::cos x=" + std::to_string(static_cast<double>(x)),
                   1e-7L, 1e-9L);
    }

    const std::vector<long double> positives = {0.5L, 1.0L, 1.5L, 2.0L, 3.0L};
    for (long double x : positives) {
        expectNear(sandbox::ops::ln(fromLongDouble(x)), std::log(x),
                   "ops::ln x=" + std::to_string(static_cast<double>(x)),
                   1e-8L, 1e-10L);
        expectNear(sandbox::ops::sqrt(fromLongDouble(x)), std::sqrt(x),
                   "ops::sqrt x=" + std::to_string(static_cast<double>(x)),
                   1e-10L, 1e-12L);
    }
}

} // namespace

int main() {
    LongTriple::initPowTable();

    testFibonacciAndRecursion();
    testDftAndFft();
    testTaylorMaclaurin();
    testNativeWrappers();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " numeric workload test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll numeric workload tests passed\n";
    return EXIT_SUCCESS;
}
