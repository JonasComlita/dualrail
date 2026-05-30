#include "test_multiwidth_vm_common.h"

#if defined(__SIZEOF_INT128__) && !defined(_WIN32)
void testUInt128Core() {
    std::cout << "[1] UInt128 portable storage arithmetic\n";
    using sandbox::UInt128;
    using sandbox::UInt256;
    using sandbox::multiplyFull;

    constexpr uint64_t max64 = std::numeric_limits<uint64_t>::max();
    const Native128 one = 1;
    const Native128 max128 = ~static_cast<Native128>(0);
    const std::vector<Native128> values = {
        0,
        1,
        2,
        3,
        9,
        10,
        static_cast<Native128>(max64 - 1),
        static_cast<Native128>(max64),
        one << 64,
        (one << 64) + 1,
        (one << 64) + static_cast<Native128>(max64),
        one << 80,
        one << 100,
        max128 - 1,
        max128
    };

    for (Native128 value : values) {
        const UInt128 u = UInt128::fromNative(value);
        expectUInt128(u, value, "UInt128 native roundtrip");
        expect(u.toString() == native128ToString(value), "UInt128 decimal string");
    }

    for (Native128 a : values) {
        const UInt128 ua = UInt128::fromNative(a);
        for (Native128 b : values) {
            const UInt128 ub = UInt128::fromNative(b);
            expect((ua < ub) == (a < b), "UInt128 less-than");
            expect((ua > ub) == (a > b), "UInt128 greater-than");
            expectUInt128(ua + ub, a + b, "UInt128 add wraps like unsigned 128");
            expectUInt128(ua - ub, a - b, "UInt128 subtract wraps like unsigned 128");
            expectUInt128(ua * ub, a * b, "UInt128 low multiply wraps like unsigned 128");
            if (b != 0) {
                expectUInt128(ua / ub, a / b, "UInt128 divide");
                expectUInt128(ua % ub, a % b, "UInt128 modulo");
            }
        }

        for (unsigned shift : {0U, 1U, 2U, 31U, 63U, 64U, 65U, 100U, 127U, 128U, 129U}) {
            expectUInt128(ua << shift, shift >= 128 ? 0 : (a << shift), "UInt128 left shift");
            expectUInt128(ua >> shift, shift >= 128 ? 0 : (a >> shift), "UInt128 right shift");
        }

        for (uint32_t divisor : {1U, 2U, 3U, 5U, 7U, 10U, 17U, 65535U, 2147483649U, 4294967295U}) {
            expectUInt128(ua / divisor, a / divisor, "UInt128 small divide");
            expect((ua % divisor) == static_cast<uint32_t>(a % divisor), "UInt128 small modulo");
        }
    }

    const std::vector<Native128> productValues = {
        0,
        1,
        3,
        10,
        static_cast<Native128>(max64),
        static_cast<Native128>(max64 - 7)
    };
    const std::vector<Native128> productDivisors = {
        0,
        1,
        5,
        static_cast<Native128>(max64),
        static_cast<Native128>(max64 - 11)
    };
    for (Native128 a : productValues) {
        for (Native128 b : productDivisors) {
            const UInt256 product = multiplyFull(UInt128::fromNative(a), UInt128::fromNative(b));
            const Native128 nativeProduct = a * b;
            expect(product.limb[0] == static_cast<uint64_t>(nativeProduct), "UInt256 product low limb");
            expect(product.limb[1] == static_cast<uint64_t>(nativeProduct >> 64), "UInt256 product high limb");
            expect(product.limb[2] == 0 && product.limb[3] == 0, "UInt256 product upper zero for 64x64");
        }
    }

    {
        const UInt256 product = multiplyFull(UInt128::fromParts(1, 0), UInt128::fromParts(1, 0));
        expect(product.limb[0] == 0 && product.limb[1] == 0 &&
               product.limb[2] == 1 && product.limb[3] == 0,
               "UInt256 2^64 * 2^64");
    }
    {
        const UInt256 product = multiplyFull(UInt128::max(), UInt128::max());
        expect(product.limb[0] == 1 && product.limb[1] == 0 &&
               product.limb[2] == max64 - 1 && product.limb[3] == max64,
               "UInt256 max128 squared limbs");
    }
}
#else
void testUInt128Core() {
    std::cout << "[1] UInt128 portable storage arithmetic skipped: no host __int128 oracle\n";
}
#endif
