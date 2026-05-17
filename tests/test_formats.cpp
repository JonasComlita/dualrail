#include "test_multiwidth_vm_common.h"

void testFormatTraits() {
    std::cout << "[2] format trait constants\n";
    using namespace sandbox::native_ops::detail;

    expect(FmtT10::mantissa_trits == 6, "T10 mantissa split");
    expect(FmtT10::exponent_trits == 4, "T10 exponent split");
    expect(FmtT10::product_trits == 16, "T10 product guard");

    expect(FmtT20::mantissa_trits == 14, "T20 mantissa split");
    expect(FmtT20::exponent_trits == 6, "T20 exponent split");
    expect(FmtT20::product_trits == 33, "T20 product guard");

    expect(FmtT40::mantissa_trits == 33, "T40 mantissa split");
    expect(FmtT40::exponent_trits == 7, "T40 exponent split");
    expect(FmtT40::product_trits == 72, "T40 product guard");

    expect(FmtT50::mantissa_trits == 41, "T50 mantissa split");
    expect(FmtT50::exponent_trits == 9, "T50 exponent split");
    expect(FmtT50::product_trits == 96, "T50 product guard");
}

void testIntegerFormats() {
    std::cout << "[3] T1/T5 exhaustive integer ops\n";
    using namespace sandbox;

    for (int n = -1; n <= 1; ++n) {
        T1 v = native_ops::fromIntT1(n);
        expect(!native_ops::isInvalid(v), "T1 valid encode " + std::to_string(n));
        expectLong(v, n, "T1 roundtrip " + std::to_string(n));
    }
    expect(native_ops::isInvalid(native_ops::add(native_ops::fromIntT1(1),
                                                native_ops::fromIntT1(1))),
           "T1 overflow traps through invalid state");

    for (int a = -121; a <= 121; ++a) {
        T5 av = native_ops::fromIntT5(a);
        expect(!native_ops::isInvalid(av), "T5 valid encode " + std::to_string(a));
        expectLong(av, a, "T5 roundtrip " + std::to_string(a));
        for (int b = -121; b <= 121; ++b) {
            T5 bv = native_ops::fromIntT5(b);
            const int sum = a + b;
            T5 add = native_ops::add(av, bv);
            if (sum < -121 || sum > 121) {
                expect(native_ops::isInvalid(add), "T5 add overflow");
            } else {
                expectLong(add, sum, "T5 add");
            }

            const int diff = a - b;
            T5 sub = native_ops::subtract(av, bv);
            if (diff < -121 || diff > 121) {
                expect(native_ops::isInvalid(sub), "T5 sub overflow");
            } else {
                expectLong(sub, diff, "T5 sub");
            }

            if (b != 0) {
                expectLong(native_ops::divide(av, bv), a / b, "T5 div trunc");
            }
        }
    }
}

void testFloatFormats() {
    std::cout << "[4] T10/T20/T40/T50 small exact arithmetic\n";
    using namespace sandbox;

    for (int a = -12; a <= 12; ++a) {
        for (int b = -12; b <= 12; ++b) {
            T10 a10 = native_ops::fromIntT10(a);
            T10 b10 = native_ops::fromIntT10(b);
            T20 a20 = native_ops::fromIntT20(a);
            T20 b20 = native_ops::fromIntT20(b);
            Triple a40 = native_ops::fromIntT40(a);
            Triple b40 = native_ops::fromIntT40(b);
            LongTriple a50 = native_ops::fromInt(a);
            LongTriple b50 = native_ops::fromInt(b);

            expectLong(native_ops::add(a10, b10), a + b, "T10 add");
            expectLong(native_ops::add(a20, b20), a + b, "T20 add");
            expectLong(native_ops::add(a40, b40), a + b, "T40 add");
            expectLong(native_ops::add(a50, b50), a + b, "T50 add");

            expectLong(native_ops::multiply(a10, b10), a * b, "T10 mul");
            expectLong(native_ops::multiply(a20, b20), a * b, "T20 mul");
            expectLong(native_ops::multiply(a40, b40), a * b, "T40 mul");
            expectLong(native_ops::multiply(a50, b50), a * b, "T50 mul");

            if (b != 0 && a % b == 0) {
                expectLong(native_ops::divide(a10, b10), a / b, "T10 exact div");
                expectLong(native_ops::divide(a20, b20), a / b, "T20 exact div");
                expectLong(native_ops::divide(a40, b40), a / b, "T40 exact div");
                expectLong(native_ops::divide(a50, b50), a / b, "T50 exact div");
            }
        }
    }
}

void testFractionalAlignmentAndSqrt() {
    std::cout << "[5] fractional exponent alignment and sqrt regression\n";
    using namespace sandbox;

    const long double sqrt2 = std::sqrt(2.0L);
    const long double fourThirds = 4.0L / 3.0L;

    {
        T10 one = native_ops::fromIntT10(1);
        T10 half = native_ops::divide(native_ops::fromIntT10(1), native_ops::fromIntT10(2));
        T10 third = native_ops::divide(native_ops::fromIntT10(1), native_ops::fromIntT10(3));
        expectNear(toLongDoubleValue(native_ops::add(one, half)), 1.5L, 2e-2L, "T10 1+1/2");
        expectNear(toLongDoubleValue(native_ops::add(one, third)), fourThirds, 2e-2L, "T10 1+1/3");
        expectNear(toLongDoubleValue(native_ops::sqrt(native_ops::fromIntT10(2))), sqrt2, 2e-2L, "T10 sqrt2");
    }
    {
        T20 one = native_ops::fromIntT20(1);
        T20 half = native_ops::divide(native_ops::fromIntT20(1), native_ops::fromIntT20(2));
        T20 third = native_ops::divide(native_ops::fromIntT20(1), native_ops::fromIntT20(3));
        expectNear(toLongDoubleValue(native_ops::add(one, half)), 1.5L, 1e-5L, "T20 1+1/2");
        expectNear(toLongDoubleValue(native_ops::add(one, third)), fourThirds, 1e-5L, "T20 1+1/3");
        expectNear(toLongDoubleValue(native_ops::sqrt(native_ops::fromIntT20(2))), sqrt2, 1e-5L, "T20 sqrt2");
    }
    {
        Triple one = native_ops::fromIntT40(1);
        Triple half = native_ops::divide(native_ops::fromIntT40(1), native_ops::fromIntT40(2));
        Triple third = native_ops::divide(native_ops::fromIntT40(1), native_ops::fromIntT40(3));
        expectNear(toLongDoubleValue(native_ops::add(one, half)), 1.5L, 1e-11L, "T40 1+1/2");
        expectNear(toLongDoubleValue(native_ops::add(one, third)), fourThirds, 1e-11L, "T40 1+1/3");
        expectNear(toLongDoubleValue(native_ops::sqrt(native_ops::fromIntT40(2))), sqrt2, 1e-11L, "T40 sqrt2");
    }
    {
        LongTriple one = native_ops::fromInt(1);
        LongTriple two = native_ops::fromInt(2);
        LongTriple half = native_ops::divide(one, two);
        LongTriple third = native_ops::divide(one, native_ops::fromInt(3));
        expectNear(toLongDoubleValue(native_ops::add(one, half)), 1.5L, 1e-12L, "T50 1+1/2");
        expectNear(toLongDoubleValue(native_ops::add(one, third)), fourThirds, 1e-12L, "T50 1+1/3");
        expectNear(toLongDoubleValue(native_ops::sqrt(two)), sqrt2, 1e-12L, "T50 sqrt2");

        LongTriple x1 = native_ops::divide(native_ops::add(one, two), two);
        LongTriple x2 = native_ops::divide(native_ops::add(x1, native_ops::divide(two, x1)), two);
        expectNear(toLongDoubleValue(x1), 1.5L, 1e-12L, "T50 Newton sqrt2 x1");
        expectNear(toLongDoubleValue(x2), 17.0L / 12.0L, 1e-12L, "T50 Newton sqrt2 x2");
    }
}
