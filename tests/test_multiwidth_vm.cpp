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

std::string readTextFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.good()) return {};
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        out += line;
        out += '\n';
    }
    return out;
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

long long loadPhysLong(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
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

    auto phase4Rest = assembleOrThrow(R"(
        aclr.t50
        aload.t20 r1
        aadd.t20 r2
        asub.t20 r3
        amul.t20 r4
        astore.t20 r5
        vdot.t1 r6, v0, v1
        vmac.t1 v0, v1
        vact.t1 v2, v3
        vpack.t20.t10 v4, v5
        vunpack.t10.t20 v5, v4
        vpermute.t20 v6, v5, v0
        vblend.t20 v7, v2, v3, v4
        vswap v0, v1
        vgather.t20 v2, r1, v0
        vscatter.t20 v2, r1, v0
        halt
    )");
    expect(InstructionWord::decode(phase4Rest[0]).opcode == Opcode::ACLR,
           "assembler encodes ACLR");
    expect(InstructionWord::decode(phase4Rest[6]).opcode == Opcode::VDOT &&
           InstructionWord::decode(phase4Rest[6]).func == FUNC_T1,
           "assembler encodes VDOT.t1");
    auto vpackIw = InstructionWord::decode(phase4Rest[9]);
    expect(vpackIw.opcode == Opcode::VPACK &&
           vpackIw.rs2 == FUNC_T20 && vpackIw.func == FUNC_T10,
           "assembler encodes VPACK source/dest suffix pair");
    auto vblendIw = InstructionWord::decode(phase4Rest[12]);
    expect(vblendIw.opcode == Opcode::VBLEND && vblendIw.r5_layout &&
           vblendIw.rcond == 2 && vblendIw.rneg == 3 &&
           vblendIw.rzero == 3 && vblendIw.rpos == 4,
           "assembler encodes VBLEND as R5 false/true select");
    expect(disassemble(phase4Rest[9]).find("VPACK.t20.t10 v4, v5") != std::string::npos,
           "disassembler prints VPACK suffix pair");
    expect(disassemble(phase4Rest[12]).find("VBLEND.t20 v7, v2, v3, v4") != std::string::npos,
           "disassembler prints VBLEND shape");
    expect(!assemble("vdot.t20 r1, v0, v1\n").success, "VDOT only accepts .t1");
    expect(!assemble("vpack.t20 v1, v0\n").success, "VPACK requires source and destination suffixes");
    expect(!assemble("vswap.t20 v0, v1\n").success, "VSWAP rejects width suffix");

    TritWord27 r4Word = InstructionWord::encodeR4(Opcode::TWCMP, R5, R1, R2, R3, FUNC_T20);
    auto r4Iw = InstructionWord::decode(r4Word);
    expect(!r4Iw.malformed && r4Iw.r4_layout && r4Iw.opcode == Opcode::TWCMP &&
           r4Iw.rd == R5 && r4Iw.rs1 == R1 && r4Iw.rs2 == R2 &&
           r4Iw.rs3 == R3 && r4Iw.func == FUNC_T20,
           "R4 layout roundtrips TWCMP fields");

    auto phase2 = assembleOrThrow(R"(
        twcmp.t20   r4,  r1, r2, r3
        tclamp.t20  r5,  r1, r2, r3
        tmod.t20    r6,  r1, r2
        tlshift.t20 r7,  r1, r2
        trshift.t20 r8,  r1, r2
        tmac.t20    r1,  r2
        tcount.t20  r9,  r1
        tscan.t20   r10, r1
        callr       r11
        jmpr        r12
        syscall     1
        fence
        vsum.t20    r13, v0
        vhmin.t20   r14, v1
        vhmax.t20   r15, v2
        halt
    )");
    expect(InstructionWord::decode(phase2[0]).opcode == Opcode::TWCMP &&
           InstructionWord::decode(phase2[0]).r4_layout &&
           InstructionWord::decode(phase2[0]).func == FUNC_T20,
           "assembler encodes TWCMP.t20 R4");
    expect(InstructionWord::decode(phase2[1]).opcode == Opcode::TCLAMP &&
           InstructionWord::decode(phase2[1]).r4_layout,
           "assembler encodes TCLAMP.t20 R4");
    expect(InstructionWord::decode(phase2[2]).opcode == Opcode::TMOD, "assembler encodes TMOD");
    expect(InstructionWord::decode(phase2[5]).opcode == Opcode::TMAC &&
           InstructionWord::decode(phase2[5]).rs1 == R1,
           "assembler encodes TMAC source-only shape");
    expect(InstructionWord::decode(phase2[8]).opcode == Opcode::CALLR &&
           InstructionWord::decode(phase2[8]).rs1 == R11,
           "assembler encodes CALLR register target");
    expect(InstructionWord::decode(phase2[10]).opcode == Opcode::SYSCALL &&
           InstructionWord::decode(phase2[10]).imm == 1,
           "assembler encodes SYSCALL service id");
    expect(InstructionWord::decode(phase2[12]).opcode == Opcode::VSUM &&
           InstructionWord::decode(phase2[12]).rd == 13 &&
           InstructionWord::decode(phase2[12]).rs1 == 0,
           "assembler encodes VSUM scalar/vector operands");
    expect(disassemble(phase2[0]).find("TWCMP.t20 r4, r1, r2, r3") != std::string::npos,
           "disassembler prints TWCMP R4 shape");
    expect(disassemble(phase2[10]).find("SYSCALL 1") != std::string::npos,
           "disassembler prints SYSCALL service id");
    expect(disassemble(phase2[12]).find("VSUM.t20 r13, v0") != std::string::npos,
           "disassembler prints vector reduction shape");

    expect(!assemble("tmod r1, r2, r3\n").success, "TMOD requires suffix");
    expect(!assemble("twcmp.l20 r1, r2, r3, r4\n").success, "TWCMP rejects lane suffix");
    expect(!assemble("vsum r1, v0\n").success, "VSUM requires suffix");
    expect(!assemble("vsum.t20 v1, v0\n").success, "VSUM rejects vector destination");
    expect(!assemble("callr.t20 r1\n").success, "CALLR rejects suffix");
    expect(!assemble("syscall.t20 1\n").success, "SYSCALL rejects suffix");

    auto phase3 = assembleOrThrow(R"(
        csrr r1, cause
        csrw tvec, r1
        csrrw r2, scratch, r3
        eret
        halt
    )");
    expect(InstructionWord::decode(phase3[0]).opcode == Opcode::CSRR &&
           InstructionWord::decode(phase3[0]).rd == R1 &&
           InstructionWord::decode(phase3[0]).imm == CSR_CAUSE,
           "assembler encodes CSRR rd, csr");
    expect(InstructionWord::decode(phase3[1]).opcode == Opcode::CSRW &&
           InstructionWord::decode(phase3[1]).rd == R1 &&
           InstructionWord::decode(phase3[1]).imm == CSR_TVEC,
           "assembler encodes CSRW csr, rs");
    expect(InstructionWord::decode(phase3[2]).opcode == Opcode::CSRRW &&
           InstructionWord::decode(phase3[2]).rd == R2 &&
           InstructionWord::decode(phase3[2]).rs1 == R3 &&
           InstructionWord::decode(phase3[2]).rs2 == CSR_SCRATCH,
           "assembler encodes CSRRW rd, csr, rs");
    expect(InstructionWord::decode(phase3[3]).opcode == Opcode::ERET,
           "assembler encodes ERET");
    expect(disassemble(phase3[0]).find("CSRR r1, cause") != std::string::npos,
           "disassembler prints CSRR csr name");
    expect(disassemble(phase3[1]).find("CSRW tvec, r1") != std::string::npos,
           "disassembler prints CSRW csr name");
    expect(disassemble(phase3[2]).find("CSRRW r2, scratch, r3") != std::string::npos,
           "disassembler prints CSRRW csr name");
    expect(disassemble(phase3[3]) == "ERET", "disassembler prints ERET");
    expect(!assemble("csrr r1, bogus\n").success, "CSRR rejects invalid CSR name");
    expect(!assemble("csrw 99, r1\n").success, "CSRW rejects invalid CSR id");
    expect(!assemble("csrrw r1, 99, r2\n").success, "CSRRW rejects invalid CSR id");
    expect(!assemble("eret.t20\n").success, "ERET rejects width suffix");
    expect(assemble("csrr r1, mmu_enable\ncsrr r2, page_fault_addr\ncsrr r3, console_ctrl\ncsrr r4, console_in\ncsrr r5, console_in_ctrl\nhalt\n").success,
           "assembler accepts MMU and console CSR names");

    auto atomics = assembleOrThrow(R"(
        tldr.+1 r1, r2
        tstr.-1 r3, r4, r5, r6
        fence.+1
        fence -1
        halt
    )");
    auto tldr = InstructionWord::decode(atomics[0]);
    auto tstr = InstructionWord::decode(atomics[1]);
    auto fenceSeq = InstructionWord::decode(atomics[2]);
    auto fenceRelaxed = InstructionWord::decode(atomics[3]);
    expect(tldr.opcode == Opcode::TLDR && tldr.rd == R1 && tldr.rs1 == R2 &&
           tldr.func == FUNC_ORDER_SEQ_CST,
           "assembler encodes TLDR with ternary memory order");
    expect(tstr.opcode == Opcode::TSTR && tstr.r4_layout &&
           tstr.rd == R3 && tstr.rs1 == R4 && tstr.rs2 == R5 &&
           tstr.rs3 == R6 && tstr.func == FUNC_ORDER_RELAXED,
           "assembler encodes TSTR R4 with ternary memory order");
    expect(fenceSeq.opcode == Opcode::FENCE && fenceSeq.func == FUNC_ORDER_SEQ_CST,
           "assembler encodes FENCE.+1 memory order");
    expect(fenceRelaxed.opcode == Opcode::FENCE && fenceRelaxed.func == FUNC_ORDER_RELAXED,
           "assembler encodes FENCE operand memory order");
    expect(disassemble(atomics[0]).find("TLDR.+1 r1, r2") != std::string::npos,
           "disassembler prints TLDR memory order");
    expect(disassemble(atomics[1]).find("TSTR.-1 r3, r4, r5, r6") != std::string::npos,
           "disassembler prints TSTR memory order");
    expect(disassemble(atomics[2]).find("FENCE.+1") != std::string::npos,
           "disassembler prints non-default FENCE memory order");
    expect(!assemble("tldr.t20 r1, r2\n").success, "TLDR rejects width suffix");
    expect(!assemble("tstr r1, r2, r3\n").success, "TSTR rejects missing expected operand");
    expect(!assemble("fence 2\n").success, "FENCE rejects invalid memory order");
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
        expect(vm.regfile.read(R7).mode == TernaryMode::T40 &&
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
        VMState vm(64, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 2
            mov.t20 r2, 3
            mov.t20 r3, 7
            twcmp.t20  r4, r1, r2, r3
            mov.t20 r1, 5
            twcmp.t20  r5, r1, r2, r3
            mov.t20 r1, 9
            twcmp.t20  r6, r1, r2, r3
            tclamp.t20 r7, r1, r2, r3
            mov.t20 r1, 1
            tclamp.t20 r8, r1, r2, r3
            halt
        )");
        expect(loadAndReset(vm, program), "window compare/clamp program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "window compare/clamp program halts");
        expect(readTrit0(vm.regfile.read(R4)) == T_NEG, "TWCMP reports below window");
        expect(readTrit0(vm.regfile.read(R5)) == T_ZER, "TWCMP reports inside window");
        expect(readTrit0(vm.regfile.read(R6)) == T_POS, "TWCMP reports above window");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R7)) == 7, "TCLAMP clamps high");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R8)) == 3, "TCLAMP clamps low");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 5
            mov.t20 r2, 9
            mov.t20 r3, 3
            twcmp.t20 r4, r1, r2, r3
            halt
        )");
        expect(loadAndReset(vm, program), "invalid window program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "TWCMP traps on inverted bounds");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "TWCMP inverted bounds trap code");
    }

    {
        VMState vm(64, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, -8
            mov.t20 r2, 3
            tmod.t20 r3, r1, r2
            mov.t20 r4, 2
            tlshift.t20 r5, r4, r2
            trshift.t20 r6, r5, r2
            mov.t5 r7, 9
            tcount.t5 r8, r7
            tscan.t5  r9, r7
            mov.t5 r10, 0
            tscan.t5 r11, r10
            halt
        )");
        expect(loadAndReset(vm, program), "Phase 2 scalar numeric program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "Phase 2 scalar numeric program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == -2,
               "TMOD remainder follows dividend sign");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R5)) == 54,
               "TLSHIFT scales by powers of three");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R6)) == 2,
               "TRSHIFT inverse scales by powers of three");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R8)) == 1,
               "TCOUNT counts non-zero trits");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R9)) == 2,
               "TSCAN returns first non-zero trit position");
        expect(vm.regfile.read(R11).mode == TernaryMode::T1 &&
               readTrit0(vm.regfile.read(R11)) == T_NEG,
               "TSCAN all-zero returns T1 -1 sentinel");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 5
            mov.t20 r2, 0
            tmod.t20 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "TMOD divisor zero program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "TMOD traps on zero divisor");
        expect(result.trap_code == TrapCode::TRAP_DIV_ZERO, "TMOD zero divisor trap code");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 6
            mov.t20 r2, 7
            aclr.t40
            tmac.t20 r1, r2
            astore.t40 r3
            halt
        )");
        expect(loadAndReset(vm, program), "TMAC program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TMAC program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 42,
               "TMAC multiplies into accumulator");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            syscall 3
            mov.t20 r1, 42
            syscall 1
            syscall 2
            fence
            halt
        )");
        expect(loadAndReset(vm, program), "SYSCALL/FENCE program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "SYSCALL/FENCE program halts");
        expect(vm.syscall_buffer == "42\n", "SYSCALL writes sandbox output buffer");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 4
            callr r1
            mov r2, 999
            halt
            mov r2, 7
            mov r3, 3
            jmpr r3
        )");
        expect(loadAndReset(vm, program), "CALLR/JMPR program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "CALLR/JMPR program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 7,
               "CALLR jumps to absolute PC target");
        expect(sandbox::vm::ops::toLong(vm.regfile.readLR()) == 2,
               "CALLR writes link register with return PC");
    }

    {
        VMState vm(32, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow(R"(
            vsum.t20  r1, v0
            vhmin.t20 r2, v0
            vhmax.t20 r3, v0
            halt
        )");
        expect(loadAndReset(vm, program), "vector reduction program loads");
        vm.vector_length = 3;
        vm.vregfile.reset(3);
        vm.vector_faults.reset(3);
        const long long values[] = {3, -2, 5};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "vector reduction program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 6, "VSUM reduces lanes");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == -2, "VHMIN reduces lanes");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 5, "VHMAX reduces lanes");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("vlen r1\nhalt\n");
        expect(loadAndReset(vm, program), "VLEN program loads");
        auto result = sandbox::vm::run(vm, 8);
        expect(result.halted(), "VLEN program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == DEFAULT_VECTOR_LENGTH, "VLEN returns default length");
        expect(vm.vregfile.reg[0].lane.size() == DEFAULT_VECTOR_LENGTH, "vector registers allocate default lanes");
        expect(vm.vector_faults.fault_valid.size() == DEFAULT_VECTOR_LENGTH && !vm.vector_faults.any(),
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
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 3
            mov.t20 r2, 4
            aclr.t50
            aload.t20 r1
            aadd.t20  r2
            amul.t20  r2
            asub.t20  r1
            astore.t20 r3
            halt
        )");
        expect(loadAndReset(vm, program), "accumulator program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "accumulator program halts");
        expect(vm.regfile.read(R3).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 25,
               "accumulator keeps T50 internal precision and stores selected width");
    }

    {
        VMState vm(32, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow(R"(
            vdot.t1 r1, v0, v1
            vmac.t1 v0, v1
            astore.t50 r2
            vact.t1 v2, v3
            halt
        )");
        expect(loadAndReset(vm, program), "T1 AI program loads");
        auto l1 = [](int8_t trit) {
            TritLane1 lane;
            lane.setTrit(0, trit);
            return TernaryValue::fromL1(lane);
        };
        const int8_t a[] = {1, 1, 0, -1};
        const int8_t b[] = {1, -1, 1, -1};
        const long long signs[] = {-5, 0, 7, -1};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, l1(a[lane]));
            vm.vregfile.reg[1].write(lane, l1(b[lane]));
            vm.vregfile.reg[3].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(signs[lane])));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "T1 AI program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 1,
               "VDOT.t1 writes T50 dot product to scalar register");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 1,
               "VMAC.t1 accumulates T1 dot product into accumulator");
        expect(vectorPredicateTrit(vm, 2, 0) == -1 &&
               vectorPredicateTrit(vm, 2, 1) == 0 &&
               vectorPredicateTrit(vm, 2, 2) == 1 &&
               vectorPredicateTrit(vm, 2, 3) == -1,
               "VACT.t1 writes sign predicates");
        expect(!vm.vector_faults.any(), "T1 AI program has no lane faults");
    }

    {
        VMState vm(32, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow(R"(
            vpack.t20.t10   v1, v0
            vunpack.t10.t20 v2, v1
            vpermute.t20    v3, v2, v4
            vblend.t20      v6, v5, v2, v3
            vswap           v1, v2
            halt
        )");
        expect(loadAndReset(vm, program), "vector plumbing program loads");
        const long long values[] = {10, 20, 30};
        const long long indices[] = {2, 0, 1};
        const int8_t cond[] = {-1, 0, 1};
        auto l1 = [](int8_t trit) {
            TritLane1 lane;
            lane.setTrit(0, trit);
            return TernaryValue::fromL1(lane);
        };
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
            vm.vregfile.reg[4].write(lane, TernaryValue::fromT5(native_ops::fromIntT5(indices[lane])));
            vm.vregfile.reg[5].write(lane, l1(cond[lane]));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "vector plumbing program halts");
        expect(vectorMode(vm, 1, 0) == TernaryMode::T20 &&
               vectorMode(vm, 2, 0) == TernaryMode::T10,
               "VSWAP exchanges whole vector register payloads");
        expect(vectorLong(vm, 3, 0) == 30 &&
               vectorLong(vm, 3, 1) == 10 &&
               vectorLong(vm, 3, 2) == 20,
               "VPERMUTE reorders lanes by index vector");
        expect(vectorLong(vm, 6, 0) == 10 &&
               vectorLong(vm, 6, 1) == 20 &&
               vectorLong(vm, 6, 2) == 20,
               "VBLEND selects false for non-positive and true for positive");
        expect(!vm.vector_faults.any(), "vector plumbing program has no lane faults");
    }

    {
        VMState vm(32, 32);
        vm.vector_length = 3;
        auto program = assembleOrThrow(R"(
            mov r1, 10
            vgather.t20  v2, r1, v0
            vscatter.t20 v2, r1, v1
            halt
        )");
        expect(loadAndReset(vm, program), "gather/scatter program loads");
        const long long gatherIdx[] = {0, 2, 4};
        const long long scatterIdx[] = {6, 7, 40};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT5(native_ops::fromIntT5(gatherIdx[lane])));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT5(native_ops::fromIntT5(scatterIdx[lane])));
        }
        vm.dmem.store(10, TernaryValue::fromT5(native_ops::fromIntT5(11)));
        vm.dmem.store(12, TernaryValue::fromT5(native_ops::fromIntT5(22)));
        vm.dmem.store(14, TernaryValue::fromT5(native_ops::fromIntT5(33)));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "gather/scatter program halts with lane-local fault");
        auto [stored0, fc0] = vm.dmem.load(16);
        auto [stored1, fc1] = vm.dmem.load(17);
        expect(fc0 == MemFaultCode::OK && sandbox::vm::ops::toLong(stored0) == 11,
               "VGATHER/VSCATTER stores first indexed lane");
        expect(fc1 == MemFaultCode::OK && sandbox::vm::ops::toLong(stored1) == 22,
               "VGATHER/VSCATTER stores second indexed lane");
        expect(vm.vector_faults.fault_valid[2] &&
               vm.vector_faults.fault_class[2] == TrapCode::TRAP_MEM_FAULT,
               "VSCATTER records out-of-range indexed lane");
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

void testPhase35Infrastructure() {
    std::cout << "[8] Phase 3.5 VM hooks, data sections, and run limits\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;

    {
        auto assembled = assemble(R"(
            .data
        value: .word 42
        next:  .word -7, value
            .text
        start:
            mov  r1, value
            load r2, r1
            load r3, zero, next
            mov  r4, next
            load r5, zero, 2
            halt
        )");
        expect(assembled.success, "assembler accepts .data/.text with .word");
        if (assembled.success) {
            expect(assembled.labels.count("start") && assembled.labels.at("start") == 0,
                   "text label maps to IMEM address");
            expect(assembled.data_labels.count("value") && assembled.data_labels.at("value") == 0,
                   "data label maps to first DMEM address");
            expect(assembled.data_labels.count("next") && assembled.data_labels.at("next") == 1,
                   "data label maps to second DMEM address");
            expect(assembled.data.size() == 3, ".word emits each data word");
            expect(sandbox::vm::ops::toLong(assembled.data[0]) == 42, ".word stores numeric literal");
            expect(sandbox::vm::ops::toLong(assembled.data[1]) == -7, ".word stores signed literal");
            expect(sandbox::vm::ops::toLong(assembled.data[2]) == 0, ".word resolves data label");

            VMState vm(32, 16);
            expect(loadAndReset(vm, assembled), "assembled text/data image loads");
            auto result = sandbox::vm::run(vm, 16);
            expect(result.halted(), "text/data program halts");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 42,
                   "LOAD reads value through data label address");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == -7,
                   "LOAD immediate resolves data label absolutely");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R4)) == 1,
                   "MOV resolves data label absolutely");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R5)) == 0,
                   ".word label operand stores absolute data address");
        }
    }

    {
        auto mixed = assemble(R"(
            .text
        entry: mov r1, payload
               jmp done
            .data
        payload: .word 17
            .text
        done:  halt
        )");
        expect(mixed.success, "assembler accepts mixed text/data/text sections");
        if (mixed.success) {
            expect(mixed.labels.count("entry") && mixed.labels.at("entry") == 0,
                   "entry text label survives section switch");
            expect(mixed.labels.count("done") && mixed.labels.at("done") == 2,
                   "done text label address ignores data words");
            expect(mixed.data_labels.count("payload") && mixed.data_labels.at("payload") == 0,
                   "payload data label address ignores text words");
            auto jmp = InstructionWord::decode(mixed.program[1]);
            expect(jmp.opcode == Opcode::JMP && jmp.offset == 1,
                   "branch labels remain PC-relative text offsets");
        }
    }

    expect(!assemble("foo: halt\n.data\nfoo: .word 1\n").success,
           "duplicate labels across text and data are rejected");
    expect(!assemble(".data\nx: .word 1\n.text\njmp x\n").success,
           "data labels cannot be branch targets");
    expect(!assemble(".word 1\nhalt\n").success,
           ".word outside .data is rejected");

    {
        auto placed = assemble(R"(
            .org 3
        start:
            halt
            .data
        first: .word 11
            .org 4
        pte:   .pte 8, 1, 0, 0, 1
        )");
        expect(placed.success, "assembler accepts .org and .pte directives");
        if (placed.success) {
            expect(placed.labels.count("start") && placed.labels.at("start") == 3,
                   ".org advances text addresses");
            expect(placed.program.size() == 4,
                   ".org pads text image with NOPs");
            expect(InstructionWord::decode(placed.program[0]).opcode == Opcode::NOP &&
                   InstructionWord::decode(placed.program[3]).opcode == Opcode::HALT,
                   ".org text padding executes as NOPs before placed code");
            expect(placed.data_labels.count("pte") && placed.data_labels.at("pte") == 4,
                   ".org advances data addresses");
            expect(placed.data.size() == 5,
                   ".org pads data image with zero words");
            PageTableEntry decoded;
            expect(decodePageTableEntry(placed.data[4], decoded),
                   ".pte emits a decodable raw PTE");
            expect(decoded.ppn == 8 && decoded.present && decoded.user &&
                   !decoded.read && !decoded.write && decoded.execute,
                   ".pte stores ppn and permission flags");
        }
    }

    expect(!assemble(".org -1\nhalt\n").success,
           ".org rejects negative addresses");
    expect(!assemble(".data\n.pte 1, 2, 0, 0, 1\n").success,
           ".pte rejects non-boolean flags");
    expect(!assemble(".pte 1, 1, 0, 0, 1\n").success,
           ".pte outside .data is rejected");

    {
        auto image = assemble(R"(
            .text
        entry:
            halt
            .data
        app: .execheader 0, 1, 1, 24, 1, 0
        )");
        expect(image.success, "assembler accepts executable header directive");
        if (image.success) {
            expect(image.data_labels.count("app") && image.data_labels.at("app") == 0,
                   ".execheader defines a data label");
            expect(image.data.size() == EXEC_HEADER_WORDS,
                   ".execheader emits fixed-size header words");
            expect(image.executable_headers.count("app"),
                   ".execheader records executable metadata");
            const ExecutableImageHeader header = image.executable_headers.at("app");
            expect(header.entry_virtual_pc == 0 &&
                   header.text_pages == 1 &&
                   header.data_pages == 1 &&
                   header.stack_words == 24 &&
                   header.syscall_abi_version == EXEC_SYSCALL_ABI_VERSION_V1,
                   "executable metadata decodes header fields");
            VMState vm(64, 64);
            expect(initializeTaskContext(vm.dmem, 8, header, 1, 2),
                   "loader helper initializes a task context from executable metadata");
            expect(loadPhysLong(vm, 8 + TASK_CONTEXT_EPC) == 0 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_IMEM_PTBR) == 1 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_DMEM_PTBR) == 2 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "loader helper writes context PC, page tables, and SP");
        }

        expect(!assemble(".data\nbad: .execheader 0, 0, 1, 24, 1, 0\n").success,
               ".execheader rejects invalid text page count");
        expect(!assemble(".execheader 0, 1, 1, 24, 1, 0\n").success,
               ".execheader outside .data is rejected");
        expect(!assemble(".data\n.execheader 0, 1, 1, 24, 1, 0\n").success,
               ".execheader requires a label");
    }

    {
        VMState vm(16, 16);
        auto program = assembleOrThrow(R"(
            nop
            nop
            nop
            halt
        )");
        expect(loadAndReset(vm, program), "hooked halt program loads");
        std::vector<std::string> events;
        VMHooks hooks;
        hooks.onStep = [&](const VMState&, int pc) {
            events.push_back("step:" + std::to_string(pc));
        };
        hooks.onTrap = [&](const VMState&, int pc) {
            events.push_back("trap:" + std::to_string(pc));
        };
        hooks.onHalt = [&](const VMState&, int pc) {
            events.push_back("halt:" + std::to_string(pc));
        };
        auto result = sandbox::vm::run(vm, 16, &hooks);
        expect(result.halted() && result.steps == 4, "hooked halt program runs four steps");
        const std::vector<std::string> want = {
            "step:0", "step:1", "step:2", "step:3", "halt:3"
        };
        expect(events == want, "hooks fire onStep per instruction then onHalt");
    }

    {
        VMState vm(16, 16);
        auto program = assembleOrThrow(R"(
            mov r1, 1
            mov r2, 0
            div.t20 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "hooked trap program loads");
        std::vector<std::string> events;
        VMHooks hooks;
        hooks.onStep = [&](const VMState&, int pc) {
            events.push_back("step:" + std::to_string(pc));
        };
        hooks.onTrap = [&](const VMState&, int pc) {
            events.push_back("trap:" + std::to_string(pc));
        };
        auto result = sandbox::vm::run(vm, 16, &hooks);
        expect(result.trapped() && result.trap_code == TrapCode::TRAP_DIV_ZERO,
               "hooked trap program reports divide by zero");
        const std::vector<std::string> want = {
            "step:0", "step:1", "step:2", "trap:2"
        };
        expect(events == want, "hooks fire onStep per instruction then onTrap");
    }

    {
        VMState vm(16, 16);
        auto program = assembleOrThrow("nop\nhalt\n");
        expect(loadAndReset(vm, program), "run-limit program loads");
        std::vector<std::string> events;
        VMHooks hooks;
        hooks.onStep = [&](const VMState&, int pc) {
            events.push_back("step:" + std::to_string(pc));
        };
        hooks.onHalt = [&](const VMState&, int pc) {
            events.push_back("halt:" + std::to_string(pc));
        };
        auto first = sandbox::vm::run(vm, 1, &hooks);
        expect(first.timeout() && first.steps == 1 && vm.isRunning() && vm.pc == 1,
               "step limit stops after exact instruction count without trap/halt");
        expect(events.size() == 1 && events[0] == "step:0",
               "timeout fires only onStep for executed instruction");
        auto second = sandbox::vm::run(vm, 1, &hooks);
        expect(second.halted() && second.steps == 1,
               "continuing after timeout can reach HALT");
        expect(events.size() == 3 && events[1] == "step:1" && events[2] == "halt:1",
               "HALT hook fires when second run executes terminal instruction");
    }

    {
        const LongTriple* lnA = &native_ops::cachedLn3();
        const LongTriple* lnB = &native_ops::cachedLn3();
        const LongTriple* piA = &native_ops::cachedPi();
        const LongTriple* piB = &native_ops::cachedPi();
        expect(lnA == lnB, "cachedLn3 returns stable cached object");
        expect(piA == piB, "cachedPi returns stable cached object");
        expect(native_ops::compare(native_ops::ln3(), *lnA) == 0,
               "ln3 preserves public value through cache");
        expect(native_ops::compare(native_ops::pi(), *piA) == 0,
               "pi preserves public value through cache");
    }
}

void testOsSubstrate() {
    std::cout << "[9] Phase 3 OS substrate VM machine contract\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;

    auto asLong = [](const VMState& vm, int reg) {
        return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
    };
    auto loadPhysLong = [](VMState& vm, int addr) {
        auto [value, fault] = vm.dmem.load(addr);
        expect(fault == MemFaultCode::OK, "physical DMEM load succeeds in test helper");
        return sandbox::vm::ops::toLong(value);
    };

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r1, 1
            mov r2, 0
        fault_div:
            div.t20 r3, r1, r2
        after_fault:
            halt
        handler:
            csrr r4, cause
            csrr r5, epc
            mov r6, after_fault
            csrw epc, r6
            eret
        )");
        expect(assembled.success, "routed div-zero program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "routed div-zero program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "routed div-zero handler returns to halt");
            expect(asLong(vm, R4) == OS_CAUSE_DIV_ZERO, "routed div-zero stores cause");
            expect(asLong(vm, R5) == assembled.labels.at("fault_div"),
                   "routed div-zero stores faulting PC in EPC");
            expect(vm.privilege == PrivilegeMode::User,
                   "ERET restores user mode before resumed HALT");
        }
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov sp, 11
            mov r1, 22
            csrw scratch, r1
            csrrw sp, scratch, sp
            csrr r2, scratch
            halt
        )");
        expect(loadAndReset(vm, program), "CSRRW scratch swap program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "CSRRW scratch swap program halts");
        expect(asLong(vm, R26_SP) == 22, "CSRRW writes old scratch to rd");
        expect(asLong(vm, R2) == 11, "CSRRW writes source value into CSR");
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r1, 99
            csrrw r2, scratch, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "user CSRRW trap program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "user CSRRW trap program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "user CSRRW routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT,
                   "user CSRRW routes protection fault");
        }
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r1, 99
            csrw scratch, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "user CSRW trap program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "user CSRW trap program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "user CSRW routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT,
                   "user CSRW routes protection fault");
        }
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            eret
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "user ERET trap program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "user ERET trap program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "user ERET routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT,
                   "user ERET routes protection fault");
        }
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r13, 123
            syscall 9
        after_syscall:
            halt
        handler:
            csrr r4, cause
            csrr r5, syscall_id
            mov r13, 777
            mov r1, after_syscall
            csrw epc, r1
            eret
        )");
        expect(assembled.success, "routed syscall program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "routed syscall program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "routed syscall handler returns");
            expect(asLong(vm, R4) == OS_CAUSE_SYSCALL, "SYSCALL routes ECALL cause");
            expect(asLong(vm, R5) == 9, "SYSCALL stores immediate in syscall_id CSR");
            expect(asLong(vm, 13) == 777, "syscall return value uses r13");
        }
    }

    {
        VMState vm(96, 96);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r13, 123
            syscall 1
            syscall 2
            halt
        handler:
            csrr r4, cause
            csrr r5, syscall_id
            mov r1, 1
            tcmp r2, r5, r1
            brz r2, write_value
            mov r1, 2
            tcmp r2, r5, r1
            brz r2, write_newline
            mov r13, -1
            jmp syscall_return
        write_value:
            csrw console_out, r13
            mov r13, 0
            jmp syscall_return
        write_newline:
            mov r1, 1
            csrw console_ctrl, r1
            mov r13, 0
        syscall_return:
            csrr r1, epc
            mov r2, 1
            add r1, r1, r2
            csrw epc, r1
            eret
        )");
        expect(assembled.success, "routed syscall console program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "routed syscall console program loads");
            auto result = sandbox::vm::run(vm, 96);
            expect(result.halted(), "routed syscall console program returns");
            expect(vm.syscall_buffer == "123\n",
                   "kernel console CSR writes routed syscall output");
            expect(asLong(vm, R4) == OS_CAUSE_SYSCALL, "console syscall routes ECALL cause");
            expect(asLong(vm, R5) == 2, "console syscall records final syscall id");
            expect(asLong(vm, 13) == 0, "console syscall returns success in r13");
        }
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, -1
            csrw console_ctrl, r1
            mov r1, 55
            csrw console_out, r1
            mov r1, 1
            csrw console_ctrl, r1
            csrr r2, console_ctrl
            halt
        )");
        expect(loadAndReset(vm, program), "kernel console CSR program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "kernel console CSR program halts");
        expect(vm.syscall_buffer == "55\n", "console CSRs update output buffer");
        expect(asLong(vm, R2) == 3, "console_ctrl reads output buffer length");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            csrr r1, console_in_ctrl
            csrr r2, console_in
            mov r3, 1
            csrw console_in_ctrl, r3
            csrr r4, console_in_ctrl
            csrr r5, console_in
            mov r3, -1
            csrw console_in_ctrl, r3
            csrr r6, console_in_ctrl
            halt
        )");
        expect(loadAndReset(vm, program), "kernel console input CSR program loads");
        vm.enqueueConsoleAscii("az");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "kernel console input CSR program halts");
        expect(asLong(vm, R1) == 2 &&
               asLong(vm, R2) == 97 &&
               asLong(vm, R4) == 1 &&
               asLong(vm, R5) == 122 &&
               asLong(vm, R6) == 0,
               "console input CSRs peek, consume, and clear host-fed input");
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            nop
            nop
            nop
        timer_after:
            mov r8, 99
            halt
        handler:
            csrr r6, cause
            csrr r7, epc
            mov r1, 0
            csrw timer_enable, r1
            eret
        )");
        expect(assembled.success, "timer IRQ program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "timer IRQ program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.interrupt_enable = true;
            vm.timer_counter = 3;
            vm.timer_reload = 0;
            vm.timer_enable = true;
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "timer IRQ handler resumes program");
            expect(asLong(vm, R6) == OS_CAUSE_TIMER_IRQ, "timer IRQ routes interrupt cause");
            expect(asLong(vm, R7) == assembled.labels.at("timer_after"),
                   "timer IRQ EPC is next PC after exact instruction count");
            expect(asLong(vm, R8) == 99, "program resumes after timer ERET");
        }
    }

    {
        VMState vm(96, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, 1
            csrw timer_counter, r1
            csrw timer_enable, r1
            nop
            csrr r2, timer_pending
            mov r1, -7
            csrw status, r1
        after_enable:
            mov r8, 44
            halt
        handler:
            csrr r4, cause
            csrr r5, epc
            mov r1, 0
            csrw timer_enable, r1
            eret
        )");
        expect(assembled.success, "critical section timer program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "critical section timer program loads");
            auto result = sandbox::vm::run(vm, 96);
            expect(result.halted(), "critical section timer program halts");
            expect(asLong(vm, R2) == 1,
                   "timer interrupt remains pending while interrupts are disabled");
            expect(asLong(vm, R4) == OS_CAUSE_TIMER_IRQ,
                   "pending timer routes after interrupts are re-enabled");
            expect(asLong(vm, R5) == assembled.labels.at("after_enable"),
                   "pending timer EPC is the first instruction after critical section");
            expect(asLong(vm, R8) == 44,
                   "program resumes after deferred timer interrupt");
        }
    }

    {
        VMState vm(16, 16);
        auto assembled = assemble(R"(
            nop
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "fetch protection program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "fetch protection program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.user_imem_base = 1;
            vm.user_imem_limit = 2;
            auto result = sandbox::vm::run(vm, 16);
            expect(result.halted(), "fetch protection routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_FETCH_FAULT,
                   "user fetch outside IMEM range routes fetch fault");
        }
    }

    {
        VMState vm(32, 64);
        auto assembled = assemble(R"(
            mov r1, 9
            load r2, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "load protection program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "load protection program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.user_dmem_base = 10;
            vm.user_dmem_limit = 12;
            auto result = sandbox::vm::run(vm, 32);
            expect(result.halted(), "load protection routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_LOAD_FAULT,
                   "user load outside DMEM range routes load fault");
        }
    }

    {
        VMState vm(32, 64);
        auto assembled = assemble(R"(
            mov r1, 9
            mov r2, 77
            store r2, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "store protection program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "store protection program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.user_dmem_base = 10;
            vm.user_dmem_limit = 12;
            auto result = sandbox::vm::run(vm, 32);
            expect(result.halted(), "store protection routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_STORE_FAULT,
                   "user store outside DMEM range routes store fault");
        }
    }

    {
        VMState vm(96, 96);
        auto user = assembleOrThrow(R"(
            mov r1, 42
            halt
        )");
        expect(vm.imem.loadProgram(user, MMU_PAGE_WORDS), "MMU fetch user program loads at physical page");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "MMU translates user fetches");
        expect(asLong(vm, R1) == 42, "MMU fetch executes mapped physical IMEM page");
    }

    {
        VMState vm(96, 128);
        auto user = assembleOrThrow(R"(
            load r1, zero, 0
            mov r2, 1
            add.t20 r1, r1, r2
            store r1, zero, 0
            halt
        )");
        expect(vm.imem.loadProgram(user, MMU_PAGE_WORDS), "MMU data user program loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(4, encodePageTableEntry(2, true, true, true, false));
        vm.dmem.store(2 * MMU_PAGE_WORDS, sandbox::vm::ops::fromLong(5));
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "MMU translates user load/store");
        expect(loadPhysLong(vm, 2 * MMU_PAGE_WORDS) == 6,
               "MMU store updates mapped physical data page");
    }

    {
        VMState vm(96, 96);
        auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_addr
            csrr r6, page_fault_access
            halt
        )");
        expect(vm.imem.loadProgram(handler, 2 * MMU_PAGE_WORDS), "fetch page fault handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true, false));
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "absent IMEM PTE routes page fault");
        expect(asLong(vm, R4) == OS_CAUSE_FETCH_PAGE_FAULT, "absent fetch PTE cause");
        expect(asLong(vm, R5) == 0, "fetch page fault records virtual address");
        expect(asLong(vm, R6) == OS_PAGE_ACCESS_FETCH, "fetch page fault records access type");
    }

    {
        VMState vm(96, 128);
        auto user = assembleOrThrow(R"(
            load r1, zero, 0
            store r1, zero, 0
            halt
        )");
        auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_addr
            csrr r6, page_fault_access
            halt
        )");
        expect(vm.imem.loadProgram(user, MMU_PAGE_WORDS), "read-only data user program loads");
        expect(vm.imem.loadProgram(handler, 2 * MMU_PAGE_WORDS), "read-only data handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(4, encodePageTableEntry(2, true, true, false, false));
        vm.dmem.store(2 * MMU_PAGE_WORDS, sandbox::vm::ops::fromLong(33));
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "read-only page rejects user store");
        expect(asLong(vm, R1) == 33, "read-only page allows user load");
        expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT, "read-only store protection cause");
        expect(asLong(vm, R5) == 0, "store protection records virtual address");
        expect(asLong(vm, R6) == OS_PAGE_ACCESS_STORE, "store protection records access type");
    }

    {
        VMState vm(96, 96);
        auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_access
            halt
        )");
        expect(vm.imem.loadProgram(handler, 2 * MMU_PAGE_WORDS), "NX handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, true, false, false));
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "non-executable PTE routes protection fault");
        expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT, "NX fetch protection cause");
        expect(asLong(vm, R5) == OS_PAGE_ACCESS_FETCH, "NX fetch records access type");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 9
            load r2, r1
            halt
        )");
        expect(loadAndReset(vm, program), "kernel MMU bypass program loads");
        vm.mmu_enable = true;
        vm.user_dmem_ptbr = 0;
        vm.user_dmem_pages = 0;
        vm.dmem.store(9, sandbox::vm::ops::fromLong(66));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "kernel bypasses MMU translation");
        expect(asLong(vm, R2) == 66, "kernel load succeeds with MMU enabled");
    }

    {
        VMState vm(320, 224);
        auto user = assembleOrThrow(R"(
        loop:
            load r1, zero, 0
            mov r2, 1
            add r1, r1, r2
            store r1, zero, 0
            jmp loop
        )");
        auto handler = assembleOrThrow(R"(
            csrrw sp, scratch, sp
            store r1, sp, 6
            store r2, sp, 7
            store r3, sp, 8
            store r4, sp, 9
            store r5, sp, 10
            store r6, sp, 11
            store r7, sp, 12
            store r8, sp, 13
            store r9, sp, 14
            store r10, sp, 15
            store r11, sp, 16
            store r12, sp, 17
            store r13, sp, 18
            store r14, sp, 19
            store r15, sp, 20
            store r16, sp, 21
            store r17, sp, 22
            store r18, sp, 23
            store r19, sp, 24
            store r20, sp, 25
            store r21, sp, 26
            store r22, sp, 27
            store r23, sp, 28
            store r24, sp, 29
            store r25, sp, 30
            csrr r1, scratch
            store r1, sp, 31
            csrr r1, epc
            store r1, sp, 0
            csrr r1, status
            store r1, sp, 1
            csrr r1, user_imem_ptbr
            store r1, sp, 2
            csrr r1, user_imem_pages
            store r1, sp, 3
            csrr r1, user_dmem_ptbr
            store r1, sp, 4
            csrr r1, user_dmem_pages
            store r1, sp, 5
            mov r1, 0
            csrw timer_pending, r1
            csrw timer_enable, r1
            mov r1, 120
            tcmp r2, sp, r1
            brz r2, use_ctx1
            mov r3, 120
            jmp restore_next
        use_ctx1:
            mov r3, 152
        restore_next:
            copy sp, r3
            load r1, sp, 0
            csrw epc, r1
            load r1, sp, 1
            csrw status, r1
            load r1, sp, 2
            csrw user_imem_ptbr, r1
            load r1, sp, 3
            csrw user_imem_pages, r1
            load r1, sp, 4
            csrw user_dmem_ptbr, r1
            load r1, sp, 5
            csrw user_dmem_pages, r1
            load r1, sp, 31
            csrw scratch, r1
            mov r1, 180
            csrw timer_counter, r1
            mov r1, 1
            csrw timer_enable, r1
            load r1, sp, 6
            load r2, sp, 7
            load r3, sp, 8
            load r4, sp, 9
            load r5, sp, 10
            load r6, sp, 11
            load r7, sp, 12
            load r8, sp, 13
            load r9, sp, 14
            load r10, sp, 15
            load r11, sp, 16
            load r12, sp, 17
            load r13, sp, 18
            load r14, sp, 19
            load r15, sp, 20
            load r16, sp, 21
            load r17, sp, 22
            load r18, sp, 23
            load r19, sp, 24
            load r20, sp, 25
            load r21, sp, 26
            load r22, sp, 27
            load r23, sp, 28
            load r24, sp, 29
            load r25, sp, 30
            csrrw sp, scratch, sp
            eret
        )");
        constexpr int kUserPhys = MMU_PAGE_WORDS;
        constexpr int kHandlerPhys = 4 * MMU_PAGE_WORDS;
        constexpr int kTask0Context = 120;
        constexpr int kTask1Context = 152;
        constexpr int kTaskStatus = -1 + 9 + 27; // kernel current, user previous, previous IE set.

        expect(vm.imem.loadProgram(user, kUserPhys), "two-task user program loads");
        expect(vm.imem.loadProgram(handler, kHandlerPhys), "two-task timer handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(4, encodePageTableEntry(2, true, true, true, false));
        vm.dmem.store(8, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(12, encodePageTableEntry(3, true, true, true, false));

        vm.dmem.store(kTask1Context + TASK_CONTEXT_EPC, sandbox::vm::ops::fromLong(0));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_STATUS, sandbox::vm::ops::fromLong(kTaskStatus));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_IMEM_PTBR, sandbox::vm::ops::fromLong(8));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_IMEM_PAGES, sandbox::vm::ops::fromLong(1));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_DMEM_PTBR, sandbox::vm::ops::fromLong(12));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_DMEM_PAGES, sandbox::vm::ops::fromLong(1));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_REG_BASE + R26_SP - 1,
                      sandbox::vm::ops::fromLong(24));

        vm.trap_routing_enabled = true;
        vm.tvec = kHandlerPhys;
        vm.privilege = PrivilegeMode::User;
        vm.interrupt_enable = true;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;
        vm.scratch = kTask0Context;
        vm.regfile.write(R26_SP, sandbox::vm::ops::fromLong(24));
        vm.timer_counter = 40;
        vm.timer_enable = true;

        auto result = sandbox::vm::run(vm, 900);
        expect(result.timeout() && vm.isRunning(), "two-task timer proof keeps VM running");
        expect(loadPhysLong(vm, 2 * MMU_PAGE_WORDS) > 0,
               "task 0 physical counter advances");
        expect(loadPhysLong(vm, 3 * MMU_PAGE_WORDS) > 0,
               "task 1 physical counter advances");
        expect(loadPhysLong(vm, 2 * MMU_PAGE_WORDS) !=
                   loadPhysLong(vm, 3 * MMU_PAGE_WORDS),
               "tasks retain independent physical counters");
        expect(loadPhysLong(vm, kTask0Context + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
               "task 0 saved user stack pointer");
        expect(loadPhysLong(vm, kTask1Context + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
               "task 1 saved user stack pointer");
    }

    {
        const std::string source = readTextFile("OS3/minimal_kernel_bringup.tasm");
        expect(!source.empty(), "minimal kernel bring-up artifact is readable");
        auto assembled = assemble(source);
            expect(assembled.success, "minimal kernel bring-up artifact assembles");
        if (assembled.success) {
            expect(assembled.labels.count("boot") && assembled.labels.at("boot") == 0,
                   "minimal kernel boots at PC zero");
            expect(assembled.labels.count("shell_loop") && assembled.labels.at("shell_loop") == 50 * MMU_PAGE_WORDS,
                   "minimal kernel places shell code on mapped physical page");
            expect(assembled.labels.count("prog_a") && assembled.labels.at("prog_a") == 53 * MMU_PAGE_WORDS,
                   "minimal kernel places static program A on mapped physical page");
            expect(assembled.labels.count("prog_b") && assembled.labels.at("prog_b") == 54 * MMU_PAGE_WORDS,
                   "minimal kernel places static program B on mapped physical page");
            expect(assembled.labels.count("idle_loop") && assembled.labels.at("idle_loop") == 55 * MMU_PAGE_WORDS,
                   "minimal kernel places idle task code on mapped physical page");
            expect(assembled.data_labels.count("shell_data") &&
                   assembled.data_labels.at("shell_data") == 16 * MMU_PAGE_WORDS,
                   "minimal kernel maps shell data page");
            expect(assembled.data_labels.count("prog_a_counter") &&
                   assembled.data_labels.at("prog_a_counter") == 17 * MMU_PAGE_WORDS,
                   "minimal kernel maps program A data page");
            expect(assembled.data_labels.count("prog_b_counter") &&
                   assembled.data_labels.at("prog_b_counter") == 18 * MMU_PAGE_WORDS,
                   "minimal kernel maps program B data page");
            expect(assembled.data_labels.count("idle_counter") &&
                   assembled.data_labels.at("idle_counter") == 19 * MMU_PAGE_WORDS,
                   "minimal kernel maps idle counter page");
            expect(assembled.executable_headers.count("exec_shell") &&
                   assembled.executable_headers.count("exec_prog_a") &&
                   assembled.executable_headers.count("exec_prog_b"),
                   "minimal kernel defines executable image metadata");
            expect(assembled.data_labels.count("proc_count") &&
                   assembled.data_labels.count("user_proc_count") &&
                   assembled.data_labels.count("idle_proc") &&
                   assembled.data_labels.count("current_proc") &&
                   assembled.data_labels.count("ready_head") &&
                   assembled.data_labels.count("ready_tail") &&
                   assembled.data_labels.count("proc_table"),
                   "minimal kernel defines process table metadata");
            expect(assembled.data_labels.count("proc_state") &&
                   assembled.data_labels.count("proc_parent_pid") &&
                   assembled.data_labels.count("proc_exit_status") &&
                   assembled.data_labels.count("proc_ticks") &&
                   assembled.data_labels.count("proc_quantum_remaining") &&
                   assembled.data_labels.count("proc_preemptions") &&
                   assembled.data_labels.count("proc_wakeup_tick") &&
                   assembled.data_labels.count("proc_wait_channel") &&
                   assembled.data_labels.count("proc_wait_target") &&
                   assembled.data_labels.count("proc_ready_next") &&
                   assembled.data_labels.count("proc_wait_next") &&
                   assembled.data_labels.count("proc_yields") &&
                   assembled.data_labels.count("proc_sleeps") &&
                   assembled.data_labels.count("proc_exits") &&
                   assembled.data_labels.count("proc_spawns") &&
                   assembled.data_labels.count("proc_waits") &&
                   assembled.data_labels.count("proc_read_blocks") &&
                   assembled.data_labels.count("proc_input_reads"),
                   "minimal kernel defines scheduler lifecycle metadata");

            VMState vm(2048, 768);
            expect(loadAndReset(vm, assembled), "minimal kernel image loads");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_count")) == 5,
                   "minimal kernel process table declares four user slots plus idle");
            expect(loadPhysLong(vm, assembled.data_labels.at("user_proc_count")) == 4,
                   "minimal kernel process table declares four user slots");
            expect(loadPhysLong(vm, assembled.data_labels.at("idle_proc")) == 4,
                   "minimal kernel records idle process index");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_table")) ==
                       assembled.data_labels.at("ctx_shell") &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_table") + 1) ==
                       assembled.data_labels.at("ctx_a") &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_table") + 2) ==
                       assembled.data_labels.at("ctx_b") &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_table") + 4) ==
                       assembled.data_labels.at("idle_ctx"),
                   "minimal kernel process table points at task and idle contexts");
            auto result = sandbox::vm::run(vm, 1000);
            expect(result.timeout() && vm.isRunning(),
                   "minimal kernel idles while shell blocks for input");
            expect(vm.trap_routing_enabled && vm.mmu_enable,
                   "minimal kernel boot enabled routed traps and MMU");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_state")) == PROC_STATE_BLOCKED &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_wait_channel")) == PROC_WAIT_CONSOLE_INPUT,
                   "shell blocks on console input without polling");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_read_blocks")) > 0,
                   "console input wait is accounted");
            expect(loadPhysLong(vm, assembled.data_labels.at("idle_counter")) > 0,
                   "idle task runs while shell waits for input");

            vm.enqueueConsoleAscii("awbu x");
            result = sandbox::vm::run(vm, 20000);
            expect(result.timeout() && vm.isRunning(),
                   "minimal kernel services image keeps running under timer preemption");
            expect(loadPhysLong(vm, assembled.data_labels.at("current_proc")) >= 0 &&
                   loadPhysLong(vm, assembled.data_labels.at("current_proc")) <
                       loadPhysLong(vm, assembled.data_labels.at("proc_count")),
                   "minimal kernel scheduler keeps current process index in range");
            expect(loadPhysLong(vm, assembled.data_labels.at("prog_a_counter")) == 1,
                   "spawned program A runs once and exits");
            expect(loadPhysLong(vm, assembled.data_labels.at("prog_b_counter")) == 1,
                   "spawned program B runs once and exits");
            expect(loadPhysLong(vm, assembled.data_labels.at("shell_data")) == 3,
                   "shell records last spawned child PID");
            expect(loadPhysLong(vm, assembled.data_labels.at("shell_data") + 1) == 11,
                   "waitpid returns program A exit status to shell memory");
            expect(!vm.syscall_buffer.empty() &&
                   vm.syscall_buffer.find("2\n") != std::string::npos &&
                   vm.syscall_buffer.find("11\n") != std::string::npos &&
                   vm.syscall_buffer.find("3\n") != std::string::npos,
                   "shell prints spawn and wait results through console CSR");
            expect(loadPhysLong(vm, assembled.data_labels.at("ctx_shell") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved shell user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("ctx_a") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved program A user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("ctx_b") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved program B user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("idle_ctx") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved idle user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_spawns")) == 2,
                   "spawn syscall accounts shell-created children");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_waits")) == 1,
                   "waitpid blocking path is accounted");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_input_reads")) >= 5,
                   "console input reads are accounted");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_exits")) == 1 &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_exits") + 1) == 1 &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_exits") + 2) == 1,
                   "exit syscall accounts shell and children");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_state")) == PROC_STATE_EXITED &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_state") + 1) == PROC_STATE_FREE &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_state") + 2) == PROC_STATE_EXITED,
                   "waited child is freed and un-waited child remains exited");
            expect(loadPhysLong(vm, assembled.data_labels.at("ready_head")) == -1 &&
                   loadPhysLong(vm, assembled.data_labels.at("ready_tail")) == -1,
                   "ready queue drains when only idle remains runnable");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_quantum_remaining")) <= PROC_DEFAULT_QUANTUM &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_quantum_remaining") + 1) <= PROC_DEFAULT_QUANTUM &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_quantum_remaining") + 4) <= PROC_DEFAULT_QUANTUM,
                   "minimal kernel tracks per-process quantum remaining");

            VMState exhaustedVm(2048, 768);
            expect(loadAndReset(exhaustedVm, assembled), "spawn exhaustion image loads");
            exhaustedVm.enqueueConsoleAscii("aa x");
            auto exhaustedResult = sandbox::vm::run(exhaustedVm, 20000);
            expect(exhaustedResult.timeout() && exhaustedVm.isRunning(),
                   "kernel keeps running through spawn exhaustion");
            expect(exhaustedVm.syscall_buffer.find("-1\n") != std::string::npos,
                   "spawn returns -1 when the static slot is not free");
            expect(loadPhysLong(exhaustedVm, assembled.data_labels.at("proc_spawns")) == 1,
                   "failed spawn is not counted as a created process");
        }
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 9
            load r2, r1
            halt
        )");
        expect(loadAndReset(vm, program), "kernel bypass program loads");
        vm.user_dmem_base = 10;
        vm.user_dmem_limit = 12;
        vm.dmem.store(9, sandbox::vm::ops::fromLong(55));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "kernel bypasses user DMEM bounds");
        expect(asLong(vm, R2) == 55, "kernel load succeeds outside user window");
    }
}

void testTernaryAtomicsAndLockAbi() {
    std::cout << "[10] ternary atomics and lock ABI\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;

    auto asLong = [](const VMState& vm, int reg) {
        return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
    };
    auto loadPhysLong = [](VMState& vm, int addr) {
        auto [value, fault] = vm.dmem.load(addr);
        expect(fault == MemFaultCode::OK, "physical DMEM load succeeds in atomics helper");
        return sandbox::vm::ops::toLong(value);
    };

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 42
            tldr.+1 r3, r1
            tstr.+1 r4, r1, r2, r3
            load r5, r1
            halt
        )");
        expect(loadAndReset(vm, program), "TLDR/TSTR success program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(7));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TLDR/TSTR success program halts");
        expect(asLong(vm, R3) == 7, "TLDR reads reserved word");
        expect(asLong(vm, R4) == 1, "TSTR returns +1 on success");
        expect(asLong(vm, R5) == 42, "TSTR writes desired value");
        expect(loadPhysLong(vm, 5) == 42, "successful TSTR updates memory");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 42
            mov r6, 99
            tldr r3, r1
            tstr r4, r1, r2, r6
            load r5, r1
            halt
        )");
        expect(loadAndReset(vm, program), "TSTR mismatch program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(7));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TSTR mismatch program halts");
        expect(asLong(vm, R4) == 0, "TSTR returns 0 on value mismatch");
        expect(asLong(vm, R5) == 7, "TSTR mismatch leaves memory unchanged");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 8
            mov r5, 42
            tldr r3, r1
            store r2, r1
            tstr r4, r1, r5, r3
            load r6, r1
            halt
        )");
        expect(loadAndReset(vm, program), "TSTR collision program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(7));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TSTR collision program halts");
        expect(asLong(vm, R4) == -1, "TSTR returns -1 when reservation is lost");
        expect(asLong(vm, R6) == 8, "ordinary store invalidates reservation before TSTR");
    }

    {
        VMState vm(64, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 1
            mov r3, 0
            tldr.0 r4, r1
            tstr.+1 r5, r1, r2, r3
            brp r5, acquired
            halt
        acquired:
            mov r6, 6
            load r7, r6
            mov r8, 1
            add.t40 r9, r7, r8
            store r9, r6
            store r3, r1
            fence.+1
            halt
        )");
        expect(loadAndReset(vm, program), "lock ABI acquire/release program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(0));
        vm.dmem.store(6, sandbox::vm::ops::fromLong(10));
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "lock ABI acquire/release program halts");
        expect(asLong(vm, R5) == 1, "lock acquire TSTR succeeds");
        expect(loadPhysLong(vm, 5) == 0, "lock release stores zero");
        expect(loadPhysLong(vm, 6) == 11, "critical section updates protected counter");
    }
}

void testNoBridgeInExecutionHeaders() {
    std::cout << "[11] static no-bridge scan\n";
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
        "ternary_transformer_runtime.h",
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
    testPhase35Infrastructure();
    testOsSubstrate();
    testTernaryAtomicsAndLockAbi();
    testNoBridgeInExecutionHeaders();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " multi-width test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll multi-width tests passed\n";
    return EXIT_SUCCESS;
}
