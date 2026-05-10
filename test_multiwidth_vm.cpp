#include "ternary_asm.h"
#include "ternary_device_allocators.h"
#include "ternary_gpu_kernels.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

static int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

template<typename T>
void expectLong(T value, long long want, const std::string& label) {
    const long long got = sandbox::native_ops::toLongLong(value);
    expect(got == want, label + " got " + std::to_string(got) +
                      " want " + std::to_string(want));
}

long double toLongDouble(sandbox::LongTriple value) {
    if (value.isZero()) return 0.0L;
    auto [m, e] = sandbox::long_ops::decode(value);
    return m * std::pow(3.0L, static_cast<long double>(e));
}

template<typename T>
long double toLongDoubleValue(T value) {
    return toLongDouble(sandbox::native_ops::toLongTriple(value));
}

void expectNear(long double got, long double want, long double tol,
                const std::string& label) {
    const long double diff = std::fabsl(got - want);
    const long double scale = std::max(1.0L, std::fabsl(want));
    expect(diff <= tol * scale,
           label + " got " + std::to_string(static_cast<double>(got)) +
           " want " + std::to_string(static_cast<double>(want)));
}

long long vectorLong(const sandbox::vm::VMState& vm, int vreg, int lane) {
    return sandbox::vm::ops::toLong(vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane));
}

sandbox::TernaryMode vectorMode(const sandbox::vm::VMState& vm, int vreg, int lane) {
    return vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane).mode;
}

int8_t vectorPredicateTrit(const sandbox::vm::VMState& vm, int vreg, int lane) {
    return vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane).asL1().tritAt(0);
}

#if defined(__SIZEOF_INT128__)
using Native128 = unsigned __int128;

std::string native128ToString(Native128 value) {
    if (value == 0) return "0";
    std::string out;
    while (value != 0) {
        const unsigned digit = static_cast<unsigned>(value % 10);
        out.insert(out.begin(), static_cast<char>('0' + digit));
        value /= 10;
    }
    return out;
}

void expectUInt128(sandbox::UInt128 got, Native128 want, const std::string& label) {
    expect(got.toNative() == want,
           label + " got " + got.toString() +
           " want " + native128ToString(want));
}

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

struct CountingAllocator final : sandbox::vm::VMStateAllocator {
    int dataAllocations = 0;
    int dataFrees = 0;
    int instructionAllocations = 0;
    int instructionFrees = 0;

    sandbox::vm::TernaryValue* allocateDataWords(int capacity) override {
        ++dataAllocations;
        return capacity > 0 ? new sandbox::vm::TernaryValue[capacity] : nullptr;
    }

    void deallocateDataWords(sandbox::vm::TernaryValue* words) override {
        ++dataFrees;
        delete[] words;
    }

    sandbox::isa::TritWord27* allocateInstructionWords(int capacity) override {
        ++instructionAllocations;
        return capacity > 0 ? new sandbox::isa::TritWord27[capacity] : nullptr;
    }

    void deallocateInstructionWords(sandbox::isa::TritWord27* words) override {
        ++instructionFrees;
        delete[] words;
    }
};

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

void testIsaAndAsmWidths() {
    std::cout << "[6] ISA func widths and assembler suffixes\n";
    using namespace sandbox::isa;
    using namespace sandbox::vm::assembler;

    for (uint8_t func : {FUNC_T1, FUNC_T5, FUNC_T10, FUNC_T20, FUNC_T40, FUNC_T50}) {
        TritWord27 w = InstructionWord::encodeR(Opcode::ADD, R3, R1, R2, func);
        InstructionWord iw = InstructionWord::decode(w);
        expect(!iw.malformed, "width func decodes");
        expect(iw.func == func, "width func roundtrip");
    }

    for (uint8_t func : {FUNC_L1, FUNC_L5, FUNC_L10, FUNC_L20, FUNC_L40, FUNC_L50}) {
        TritWord27 w = InstructionWord::encodeR(Opcode::TLADD, R3, R1, R2, func);
        InstructionWord iw = InstructionWord::decode(w);
        expect(!iw.malformed, "lane width func decodes");
        expect(iw.func == func, "lane width func roundtrip");
    }

    for (uint8_t op = 27; op <= OPCODE_MAX_ASSIGNED; ++op) {
        TritWord27 w = InstructionWord::encodeR(static_cast<Opcode>(op), R3, R1, R2);
        InstructionWord iw = InstructionWord::decode(w);
        expect(!iw.malformed, "Phase 4 opcode decodes");
        expect(iw.opcode == static_cast<Opcode>(op), "Phase 4 opcode roundtrip");
        expect(opcodeToString(iw.opcode) != "???", "Phase 4 opcode has disassembly name");
    }

    auto add = assembleOrThrow("add.t20 r3, r1, r2\nhalt\n");
    auto iw = InstructionWord::decode(add[0]);
    expect(iw.opcode == Opcode::ADD && iw.func == FUNC_T20, "add.t20 encodes func");

    auto mov = assembleOrThrow("mov.t20 r1, 42\nhalt\n");
    expect(mov.size() == 3, "mov.t20 lowers to MOV + CVT + HALT");
    auto cvt = InstructionWord::decode(mov[1]);
    expect(cvt.opcode == Opcode::CVT && cvt.func == FUNC_T20, "mov.t20 emits CVT.t20");

    auto bad = assemble("load.t20 r1, r2, 0\n");
    expect(!bad.success, "suffix rejected on LOAD");

    TritWord27 tselWord = InstructionWord::encodeR5(Opcode::TSEL, R6, R3, R1, R2, R4);
    auto tsel = InstructionWord::decode(tselWord);
    expect(!tsel.malformed && tsel.opcode == Opcode::TSEL && tsel.r5_layout,
           "TSEL R5 layout decodes");
    expect(tsel.rd == R6 && tsel.rcond == R3 && tsel.rneg == R1 &&
           tsel.rzero == R2 && tsel.rpos == R4,
           "TSEL R5 register fields roundtrip");

    auto tselAsm = assembleOrThrow("tsel r6, r3, r1, r2, r4\nhalt\n");
    auto tselIw = InstructionWord::decode(tselAsm[0]);
    expect(tselIw.opcode == Opcode::TSEL && tselIw.rpos == R4,
           "assembler encodes TSEL");

    auto branchAsm = assembleOrThrow("brz r1, 2\nbrp r2, -1\nhalt\n");
    auto brz = InstructionWord::decode(branchAsm[0]);
    auto brp = InstructionWord::decode(branchAsm[1]);
    expect(brz.opcode == Opcode::BRZ && brz.rs_branch == R1 && brz.offset == 2,
           "assembler encodes BRZ");
    expect(brp.opcode == Opcode::BRP && brp.rs_branch == R2 && brp.offset == -1,
           "assembler encodes BRP");

    auto swapAsm = assembleOrThrow("swap r1, r2\nhalt\n");
    auto swap = InstructionWord::decode(swapAsm[0]);
    expect(swap.opcode == Opcode::SWAP && swap.rd == R1 && swap.rs1 == R2,
           "assembler encodes SWAP");

    auto cvtPair = assembleOrThrow("cvt.t10.t20 r3, r2\nhalt\n");
    auto cvtPairIw = InstructionWord::decode(cvtPair[0]);
    expect(cvtPairIw.opcode == Opcode::CVT &&
           cvtPairIw.rs2 == FUNC_T10 && cvtPairIw.func == FUNC_T20,
           "assembler encodes cvt.src.dst");

    auto movLane = assembleOrThrow("mov.l20 r1, 7\nhalt\n");
    expect(movLane.size() == 3, "mov.l20 lowers to MOV + CVT + HALT");
    auto movLaneCvt = InstructionWord::decode(movLane[1]);
    expect(movLaneCvt.opcode == Opcode::CVT &&
           movLaneCvt.rs2 == FUNC_T20 && movLaneCvt.func == FUNC_L20,
           "mov.l20 lowers through matching numeric width");

    auto laneAdd = assembleOrThrow("tladd.l20 r3, r1, r2\ntlneg.l20 r4, r3\nhalt\n");
    auto laneAddIw = InstructionWord::decode(laneAdd[0]);
    auto laneNegIw = InstructionWord::decode(laneAdd[1]);
    expect(laneAddIw.opcode == Opcode::TLADD && laneAddIw.func == FUNC_L20,
           "assembler encodes tladd.l20");
    expect(laneNegIw.opcode == Opcode::TLNEG && laneNegIw.func == FUNC_L20,
           "assembler encodes tlneg.l20");

    expect(!assemble("tladd r1, r2, r3\n").success, "bare TLADD rejected");
    expect(!assemble("tladd.t20 r1, r2, r3\n").success, "numeric suffix rejected on TLADD");
    expect(!assemble("add.l20 r1, r2, r3\n").success, "lane suffix rejected on numeric ADD");
    expect(!assemble("cvt.t20.l10 r1, r2\n").success, "mismatched numeric-lane CVT rejected");

    auto vlen = assembleOrThrow("vlen r3\nhalt\n");
    auto vlenIw = InstructionWord::decode(vlen[0]);
    expect(vlenIw.opcode == Opcode::VLEN && vlenIw.rd == R3, "assembler encodes VLEN");
    expect(parseVectorRegister("v7") == 7, "vector register parser accepts v7");
    expect(parseVectorRegister("v8") < 0, "vector register parser rejects v8");
    expect(!assemble("vlen v0\nhalt\n").success, "vector register rejected in scalar destination");

    auto vectorOps = assembleOrThrow(R"(
        vbcast.t20 v0, r1
        vadd.t20   v1, v0, v0
        vneg.t20   v2, v1
        vcmp.t20   v3, v0, v1
        vsel.t20   v4, v3, v0, v1, v2
        vload.t20  v5, r2, 3
        vstore.t20 v5, r2, -2
        halt
    )");
    expect(InstructionWord::decode(vectorOps[0]).opcode == Opcode::VBCAST &&
           InstructionWord::decode(vectorOps[0]).func == FUNC_T20,
           "assembler encodes VBCAST.t20");
    expect(InstructionWord::decode(vectorOps[1]).opcode == Opcode::VADD &&
           InstructionWord::decode(vectorOps[1]).rd == 1,
           "assembler encodes VADD vector registers");
    auto vselIw = InstructionWord::decode(vectorOps[4]);
    expect(vselIw.opcode == Opcode::VSEL && vselIw.r5_layout &&
           vselIw.rd == 4 && vselIw.rcond == 3 && vselIw.rneg == 0 &&
           vselIw.rzero == 1 && vselIw.rpos == 2 && vselIw.func == FUNC_T20,
           "assembler encodes VSEL R5 vector fields");
    auto vloadIw = InstructionWord::decode(vectorOps[5]);
    auto vstoreIw = InstructionWord::decode(vectorOps[6]);
    expect(vloadIw.opcode == Opcode::VLOAD && vloadIw.rd == 5 &&
           vloadIw.rs1 == R2 && vloadIw.imm == 3 && vloadIw.func == FUNC_T20,
           "assembler encodes VLOAD vector-memory overlay");
    expect(vstoreIw.opcode == Opcode::VSTORE && vstoreIw.rd == 5 &&
           vstoreIw.rs1 == R2 && vstoreIw.imm == -2 && vstoreIw.func == FUNC_T20,
           "assembler encodes VSTORE vector-memory overlay");
    expect(disassemble(vectorOps[1]).find("VADD.t20 v1, v0, v0") != std::string::npos,
           "disassembler prints vector registers");
    expect(disassemble(vectorOps[4]).find("VSEL.t20 v4, v3, v0, v1, v2") != std::string::npos,
           "disassembler prints VSEL R5 shape");

    expect(!assemble("vadd v1, v0, v0\n").success, "bare VADD rejected");
    expect(!assemble("vadd.l20 v1, v0, v0\n").success, "lane suffix rejected on VADD");
    expect(!assemble("vadd.t20 r1, v0, v0\n").success, "scalar register rejected in vector dest");
    expect(!assemble("vbcast.t20 v0, v1\n").success, "vector register rejected as VBCAST scalar source");
    expect(!assemble("vload.t20 r1, r2, 0\n").success, "scalar register rejected in VLOAD vector dest");
    expect(!assemble("vload.t20 v0, v8, 0\n").success, "vector register rejected as VLOAD base");
}

void testVmWidths() {
    std::cout << "[7] VM width execution and tagged load/store\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;

    {
        CountingAllocator allocator;
        {
            VMState vm(4, 8, allocator);
            expect(vm.imem.words != nullptr, "IMEM uses flat pointer storage");
            expect(vm.dmem.words != nullptr, "DMEM uses flat pointer storage");
            expect(vm.imem.capacity == 4 && vm.dmem.capacity == 8, "flat memory capacities");
            expect(vm.dmem.store(0, TernaryValue::fromT20(native_ops::fromIntT20(7))) == MemFaultCode::OK,
                   "flat DMEM store");
            auto [loaded, fault] = vm.dmem.load(0);
            expect(fault == MemFaultCode::OK && loaded.mode == TernaryMode::T20 &&
                   native_ops::toLongLong(loaded.asT20()) == 7,
                   "flat DMEM load preserves tag and payload");
        }
        expect(allocator.dataAllocations == 1 && allocator.instructionAllocations == 1,
               "custom VM allocator allocation count");
        expect(allocator.dataFrees == 1 && allocator.instructionFrees == 1,
               "custom VM allocator free count");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t5 r1, 120
            mov.t5 r2, 1
            add.t5 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "T5 program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "T5 add program halts");
        expect(vm.regfile.read(R3).mode == TernaryMode::T5, "T5 result tag");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 121, "T5 result value");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t5 r1, 121
            mov.t5 r2, 1
            add.t5 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "T5 overflow program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "T5 overflow traps");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "T5 overflow trap code");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 42
            store r1, sp, 0
            load r2, sp, 0
            add.t20 r3, r2, r2
            halt
        )");
        expect(loadAndReset(vm, program), "T20 load/store program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "T20 load/store program halts");
        expect(vm.regfile.read(R2).mode == TernaryMode::T20, "LOAD preserves T20 tag");
        expect(vm.regfile.read(R3).mode == TernaryMode::T20, "ADD writes T20 tag");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 84, "T20 arithmetic value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t5  r1, -5
            mov.t20 r2, 20
            mov     r3, 50
            mov.t1  r4, -1
            tsel    r5, r4, r1, r2, r3
            mov.t1  r4, 0
            tsel    r6, r4, r1, r2, r3
            mov.t1  r4, 1
            tsel    r7, r4, r1, r2, r3
            halt
        )");
        expect(loadAndReset(vm, program), "TSEL program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "TSEL program halts");
        expect(vm.regfile.read(R5).mode == TernaryMode::T5 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R5)) == -5,
               "TSEL preserves negative arm tag/value");
        expect(vm.regfile.read(R6).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R6)) == 20,
               "TSEL preserves zero arm tag/value");
        expect(vm.regfile.read(R7).mode == TernaryMode::T50 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R7)) == 50,
               "TSEL preserves positive arm tag/value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t1 r1, 0
            brz    r1, zero_path
            mov    r2, 999
zero_path:
            mov.t1 r1, 1
            brp    r1, pos_path
            mov    r2, 999
pos_path:
            mov    r2, 7
            halt
        )");
        expect(loadAndReset(vm, program), "BRZ/BRP program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "BRZ/BRP program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 7,
               "BRZ and BRP both branch on T1 predicates");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t5  r1, 11
            mov.t20 r2, 22
            swap    r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "SWAP program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "SWAP program halts");
        expect(vm.regfile.read(R1).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 22,
               "SWAP moves second value into first register");
        expect(vm.regfile.read(R2).mode == TernaryMode::T5 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 11,
               "SWAP moves first value into second register");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t10 r1, 42
            cvt.t10.t20 r2, r1
            halt
        )");
        expect(loadAndReset(vm, program), "cvt.src.dst program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "cvt.src.dst program halts");
        expect(vm.regfile.read(R2).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 42,
               "CVT source/destination suffix converts value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20     r1, 42
            cvt.t20.l20 r2, r1
            cvt.l20.t20 r3, r2
            halt
        )");
        expect(loadAndReset(vm, program), "numeric/lane CVT program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "numeric/lane CVT program halts");
        expect(vm.regfile.read(R2).mode == TernaryMode::L20, "CVT writes L20 tag");
        expect(vm.regfile.read(R3).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 42,
               "CVT lane to matching numeric recovers value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.l1    r1, 1
            mov.l1    r2, 1
            tladd.l1  r3, r1, r2
            tlsub.l1  r4, r1, r3
            tlneg.l1  r5, r3
            tland.l1  r6, r1, r3
            tlor.l1   r7, r1, r3
            halt
        )");
        expect(loadAndReset(vm, program), "scalar lane program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "scalar lane program halts");
        expect(vm.regfile.read(R3).mode == TernaryMode::L1 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R3)) == -1,
               "TLADD is carryless modulo per trit");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R4)) == -1,
               "TLSUB is carryless modulo per trit");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R5)) == 1,
               "TLNEG flips lane trit");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R6)) == -1,
               "TLAND is per-trit lattice min");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R7)) == 1,
               "TLOR is per-trit lattice max");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.l20 r1, 7
            add.t20 r2, r1, r1
            halt
        )");
        expect(loadAndReset(vm, program), "numeric op with lane input program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "numeric op rejects lane input");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "numeric op lane trap code");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("tlneg.l20 r2, r1\nhalt\n");
        expect(loadAndReset(vm, program), "invalid lane payload program loads");
        vm.regfile.write(R1, TernaryValue::fromL20(TritLane20::invalid()));
        auto result = sandbox::vm::run(vm, 8);
        expect(result.trapped(), "TLNEG rejects invalid lane payload");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "invalid lane trap code");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("vlen r1\nhalt\n");
        expect(loadAndReset(vm, program), "VLEN program loads");
        auto result = sandbox::vm::run(vm, 8);
        expect(result.halted(), "VLEN program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 16, "VLEN returns default length");
        expect(vm.vregfile.reg[0].lane.size() == 16, "vector registers allocate default lanes");
        expect(vm.vector_faults.fault_valid.size() == 16 && !vm.vector_faults.any(),
               "vector fault masks reset to default length");
    }

    {
        VMState vm(64, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow(R"(
            mov.t20   r1, 3
            mov.t20   r2, 5
            vbcast.t20 v0, r1
            vbcast.t20 v1, r2
            vadd.t20   v2, v0, v1
            vsub.t20   v3, v1, v0
            vneg.t20   v4, v3
            vmul.t20   v5, v3, v1
            vcmp.t20   v6, v0, v1
            vsel.t20   v7, v6, v4, v0, v5
            halt
        )");
        expect(loadAndReset(vm, program), "vector arithmetic program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "vector arithmetic program halts");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            expect(vectorMode(vm, 2, lane) == TernaryMode::T20 && vectorLong(vm, 2, lane) == 8,
                   "VADD.t20 lane result");
            expect(vectorLong(vm, 3, lane) == 2, "VSUB.t20 lane result");
            expect(vectorLong(vm, 4, lane) == -2, "VNEG.t20 lane result");
            expect(vectorLong(vm, 5, lane) == 10, "VMUL.t20 lane result");
            expect(vectorMode(vm, 6, lane) == TernaryMode::L1 &&
                   vectorPredicateTrit(vm, 6, lane) == -1,
                   "VCMP.t20 writes L1 predicate");
            expect(vectorLong(vm, 7, lane) == -2, "VSEL.t20 chooses negative arm");
        }
        expect(!vm.vector_faults.any(), "vector arithmetic has no lane faults");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow("vsel.t20 v7, v6, v0, v1, v2\nhalt\n");
        expect(loadAndReset(vm, program), "three-arm VSEL program loads");
        TritLane1 neg; neg.setTrit(0, -1);
        TritLane1 zer; zer.setTrit(0, 0);
        TritLane1 pos; pos.setTrit(0, 1);
        vm.vregfile.reg[6].write(0, TernaryValue::fromL1(neg));
        vm.vregfile.reg[6].write(1, TernaryValue::fromL1(zer));
        vm.vregfile.reg[6].write(2, TernaryValue::fromL1(pos));
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(-10 - lane)));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(20 + lane)));
            vm.vregfile.reg[2].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(30 + lane)));
        }
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "three-arm VSEL program halts");
        expect(vectorLong(vm, 7, 0) == -10, "VSEL chooses negative lane arm");
        expect(vectorLong(vm, 7, 1) == 21, "VSEL chooses zero lane arm");
        expect(vectorLong(vm, 7, 2) == 32, "VSEL chooses positive lane arm");
        expect(!vm.vector_faults.any(), "three-arm VSEL has no lane faults");
    }

    {
        VMState vm(64, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow(R"(
            mov r1, 10
            vload.t20  v0, r1, 0
            vstore.t20 v0, r1, 20
            halt
        )");
        expect(loadAndReset(vm, program), "vector load/store program loads");
        for (int i = 0; i < vm.vector_length; ++i) {
            vm.dmem.store(10 + i, TernaryValue::fromT5(native_ops::fromIntT5(2 + i)));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "vector load/store program halts");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            expect(vectorMode(vm, 0, lane) == TernaryMode::T20 &&
                   vectorLong(vm, 0, lane) == 2 + lane,
                   "VLOAD converts contiguous memory to suffix type");
            auto [stored, fc] = vm.dmem.load(30 + lane);
            expect(fc == MemFaultCode::OK && stored.mode == TernaryMode::T20 &&
                   sandbox::vm::ops::toLong(stored) == 2 + lane,
                   "VSTORE writes contiguous converted memory");
        }
        expect(!vm.vector_faults.any(), "vector load/store has no lane faults");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 2;
        auto program = assembleOrThrow(R"(
            mov.t5 r1, 5
            mov.t5 r2, 6
            vbcast.t5 v0, r1
            vbcast.t5 v1, r2
            vadd.t5 v2, v0, v1
            halt
        )");
        expect(loadAndReset(vm, program), "T5 vector arithmetic program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "T5 vector arithmetic program halts");
        expect(vectorMode(vm, 2, 0) == TernaryMode::T5 && vectorLong(vm, 2, 0) == 11,
               "VADD.t5 executes");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow("vdiv.t20 v2, v0, v1\nhalt\n");
        expect(loadAndReset(vm, program), "vector divide fault program loads");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(9)));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(lane == 2 ? 0 : 3)));
        }
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "VDIV lane fault program still halts");
        expect(vectorLong(vm, 2, 0) == 3, "VDIV valid lane result");
        expect(vectorLong(vm, 2, 2) == 0, "VDIV fault lane typed zero");
        expect(vm.vector_faults.fault_valid[2] &&
               vm.vector_faults.fault_class[2] == TrapCode::TRAP_DIV_ZERO,
               "VDIV records lane divide-by-zero");
        expect(!vm.vector_faults.fault_valid[0], "VDIV leaves valid lane fault clear");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow("vadd.t20 v2, v0, v1\nhalt\n");
        expect(loadAndReset(vm, program), "vector wrong-tag fault program loads");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(1)));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(2)));
        }
        vm.vregfile.reg[1].write(1, TernaryValue::fromL20(toLane(native_ops::fromIntT20(2))));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "vector wrong-tag lane fault program still halts");
        expect(vectorLong(vm, 2, 0) == 3, "VADD valid lane survives wrong-tag neighbor");
        expect(vectorLong(vm, 2, 1) == 0, "VADD wrong-tag lane typed zero");
        expect(vm.vector_faults.fault_valid[1] &&
               vm.vector_faults.fault_class[1] == TrapCode::TRAP_ILLEGAL_OP,
               "VADD records wrong-tag lane fault");
    }

    {
        VMState vm(16, 12);
        vm.vector_length = 4;
        auto program = assembleOrThrow("mov r1, 10\nvload.t20 v0, r1, 0\nhalt\n");
        expect(loadAndReset(vm, program), "vector memory fault program loads");
        vm.dmem.store(10, TernaryValue::fromT20(native_ops::fromIntT20(10)));
        vm.dmem.store(11, TernaryValue::fromT20(native_ops::fromIntT20(11)));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "VLOAD memory fault program still halts");
        expect(vectorLong(vm, 0, 0) == 10 && vectorLong(vm, 0, 1) == 11,
               "VLOAD valid memory lanes load");
        expect(vectorLong(vm, 0, 2) == 0 && vectorLong(vm, 0, 3) == 0,
               "VLOAD memory fault lanes typed zero");
        expect(vm.vector_faults.fault_valid[2] &&
               vm.vector_faults.fault_class[2] == TrapCode::TRAP_MEM_FAULT,
               "VLOAD records out-of-range lane fault");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("mov.l20 r1, 7\nvbcast.t20 v0, r1\nhalt\n");
        expect(loadAndReset(vm, program), "VBCAST structural fault program loads");
        auto result = sandbox::vm::run(vm, 16);
        expect(result.trapped(), "VBCAST rejects lane-family scalar source structurally");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "VBCAST structural trap code");
    }

    {
        VMState vm;
        expect(!sandbox::vm::trapValid(vm.trap_reg), "reset trap record is invalid/no-fault");
        expect(sandbox::vm::readStoredTrit(vm.trap_reg, 0) == sandbox::isa::FAULT_VALID_NONE,
               "reset trap_valid trit is zero");
        vm.trap(TrapCode::TRAP_ILLEGAL_OP);
        expect(sandbox::vm::trapValid(vm.trap_reg), "trap record valid bit set");
        expect(sandbox::vm::readStoredTrit(vm.trap_reg, 0) == sandbox::isa::FAULT_VALID_SET,
               "trap_valid trit set");
        expect(sandbox::vm::readStoredTrit(vm.trap_reg, 1) == sandbox::isa::T_POS,
               "trap_class trit stores illegal op");
        expect(sandbox::vm::decodeTrap(vm.trap_reg) == TrapCode::TRAP_ILLEGAL_OP,
               "two-trit trap record decodes");
    }
}

void testNoBridgeInExecutionHeaders() {
    std::cout << "[8] static no-bridge scan\n";
    const std::vector<std::string> files = {
        "ternary_native_ops.h",
        "ternary_backend.h",
        "ternary_kernel.h",
        "ternary_gpu_kernels.h",
        "ternary_lanes.h",
        "ternary_simd.h",
        "ternary_device_allocators.h",
        "ternary_isa.h",
        "ternary_vm_state.h",
        "ternary_vm.h",
        "ternary_asm.h",
    };
    const std::vector<std::string> banned = {
        "long_ops::decode",
        "long_ops::encode",
        "fromDouble",
        "toDouble",
        "std::pow",
        "std::sqrt",
        "long double",
    };

    for (const auto& file : files) {
        std::ifstream in(file);
        expect(in.good(), "open " + file);
        std::string line;
        while (std::getline(in, line)) {
            const auto comment = line.find("//");
            if (comment != std::string::npos) line = line.substr(0, comment);
            for (const auto& token : banned) {
                expect(line.find(token) == std::string::npos,
                       file + " production path contains bridge token " + token);
            }
        }
    }

    {
        std::ifstream in("ternary_native_ops.h");
        expect(in.good(), "open ternary_native_ops.h for native scratch scan");
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t comment = line.find("//");
            if (comment != std::string::npos) line = line.substr(0, comment);
            expect(line.find("__int128") == std::string::npos,
                   "ternary_native_ops.h reintroduced compiler __int128 scratch arithmetic");
        }
    }
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testUInt128Core();
    testFormatTraits();
    testIntegerFormats();
    testFloatFormats();
    testFractionalAlignmentAndSqrt();
    testIsaAndAsmWidths();
    testVmWidths();
    testNoBridgeInExecutionHeaders();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " multi-width test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll multi-width tests passed\n";
    return EXIT_SUCCESS;
}
