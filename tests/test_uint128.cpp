#include "test_multiwidth_vm_common.h"

#if defined(__SIZEOF_INT128__)
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

    {
        UInt128 bits{};
        bits.setBit(0);
        bits.setBit(127);
        expect(bits.bit(0) && bits.bit(127),
               "UInt128 boundary bit access");
        const UInt128 before = bits;
        bits.setBit(-1);
        bits.setBit(128);
        expect(bits == before && !bits.bit(-1) && !bits.bit(128),
               "UInt128 rejects out-of-range bit indices without mutation");

        UInt256 wide{};
        wide.limb[3] = 1ULL << 63;
        expect(wide.bit(255) && !wide.bit(-1) && !wide.bit(256),
               "UInt256 bounds-checks bit indices");
        const UInt256 wide_before = wide;
        wide.addShifted(UInt128{1}, -1);
        wide.addShifted(UInt128{1}, 256);
        expect(wide.limb[0] == wide_before.limb[0] &&
                   wide.limb[1] == wide_before.limb[1] &&
                   wide.limb[2] == wide_before.limb[2] &&
                   wide.limb[3] == wide_before.limb[3],
               "UInt256 rejects out-of-range shifted additions");
    }

    {
        auto expectDomainError = [](auto&& operation, const std::string& label) {
            bool threw = false;
            try {
                operation();
            } catch (const std::domain_error&) {
                threw = true;
            }
            expect(threw, label);
        };
        expectDomainError(
            [] { (void)(UInt128{1} / UInt128{}); },
            "UInt128 division by zero is explicit");
        expectDomainError(
            [] { (void)(UInt128{1} % UInt128{}); },
            "UInt128 modulo by zero is explicit");
        expectDomainError(
            [] { (void)(UInt128{1} / uint32_t{0}); },
            "UInt128 small division by zero is explicit");
        expectDomainError(
            [] { (void)(UInt128{1} % uint32_t{0}); },
            "UInt128 small modulo by zero is explicit");
        expectDomainError(
            [] {
                (void)sandbox::native_ops::detail::roundedDivide(
                    UInt128{1}, UInt128{});
            },
            "rounded UInt128 division by zero is explicit");
        expectDomainError(
            [] {
                (void)(sandbox::Int128::fromLongLong(1) / UInt128{});
            },
            "Int128/UInt128 division by zero is explicit");
        expectDomainError(
            [] {
                (void)(sandbox::Int128::fromLongLong(1) /
                       sandbox::Int128{});
            },
            "Int128/Int128 division by zero is explicit");
    }
}
#else
void testUInt128Core() {
    std::cout << "[1] UInt128 portable storage arithmetic skipped: compiler has no __int128 oracle\n";
}
#endif
