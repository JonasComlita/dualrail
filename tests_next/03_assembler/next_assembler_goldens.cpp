#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_asm.h"

#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox;
using namespace sandbox::isa;
using namespace sandbox::vm;
using namespace sandbox::vm::assembler;

long long valueLong(const TernaryValue& value) {
    return sandbox::vm::ops::toLong(value);
}

void expectWord(TestContext& ctx,
                const TritWord27& got,
                const TritWord27& want,
                const std::string& message) {
    ctx.equal(got.bits, want.bits, message);
}

void forwardBackwardBranches(TestContext& ctx) {
    const AssemblyResult assembled = assemble(R"(
        .text
    start:
        brp r1, done
    loop:
        brn r2, start
        jmp loop
    done:
        halt
    )");

    ctx.check(assembled.success, "branch fixture assembles");
    if (!assembled.success) return;

    ctx.equal(assembled.labels.at("start"), 0, "start label PC");
    ctx.equal(assembled.labels.at("loop"), 1, "loop label PC");
    ctx.equal(assembled.labels.at("done"), 3, "done label PC");
    ctx.equal(static_cast<int>(assembled.program.size()), 4, "branch fixture word count");
    expectWord(ctx, assembled.program[0],
               InstructionWord::encodeB(Opcode::BRP, R1, 3),
               "forward BRP exact word");
    expectWord(ctx, assembled.program[1],
               InstructionWord::encodeB(Opcode::BRN, R2, -1),
               "backward BRN exact word");
    expectWord(ctx, assembled.program[2],
               InstructionWord::encodeB(Opcode::JMP, R0_ZERO, -1),
               "backward JMP exact word");
    expectWord(ctx, assembled.program[3],
               InstructionWord::encodeB(Opcode::HALT, R0_ZERO, 0),
               "HALT exact word");
}

void orgWordLayout(TestContext& ctx) {
    const AssemblyResult assembled = assemble(R"(
        .org 2
    entry:
        halt
        .data
    value:
        .word 42, -7
        .org 5
    label_ref:
        .word value
    )");

    ctx.check(assembled.success, ".org/.word fixture assembles");
    if (!assembled.success) return;

    ctx.equal(assembled.labels.at("entry"), 2, "text .org label PC");
    ctx.equal(static_cast<int>(assembled.program.size()), 3, "text .org pads to entry");
    ctx.check(InstructionWord::decode(assembled.program[0]).opcode == Opcode::NOP,
              "text .org pads with NOP");
    ctx.check(InstructionWord::decode(assembled.program[2]).opcode == Opcode::HALT,
              "entry contains HALT");
    ctx.equal(assembled.data_labels.at("value"), 0, "first data label");
    ctx.equal(assembled.data_labels.at("label_ref"), 5, "data .org label");
    ctx.equal(static_cast<int>(assembled.data.size()), 6, "data .org pads to label_ref");
    ctx.equal(valueLong(assembled.data[0]), 42LL, ".word positive literal");
    ctx.equal(valueLong(assembled.data[1]), -7LL, ".word negative literal");
    ctx.equal(valueLong(assembled.data[5]), 0LL, ".word data label resolves absolute address");
}

void csrExactEncoding(TestContext& ctx) {
    const auto program = assembleOrThrow(R"(
        csrr r1, cause
        csrw tvec, r1
        csrrw r2, scratch, r3
        eret
        halt
    )");

    ctx.equal(static_cast<int>(program.size()), 5, "CSR fixture word count");
    expectWord(ctx, program[0],
               InstructionWord::encodeI(Opcode::CSRR, R1, R0_ZERO, CSR_CAUSE),
               "CSRR exact emitted word");
    expectWord(ctx, program[1],
               InstructionWord::encodeI(Opcode::CSRW, R1, R0_ZERO, CSR_TVEC),
               "CSRW exact emitted word");
    expectWord(ctx, program[2],
               InstructionWord::encodeR(Opcode::CSRRW, R2, R3, CSR_SCRATCH),
               "CSRRW exact emitted word");
    expectWord(ctx, program[3],
               InstructionWord::encodeR(Opcode::ERET, R0_ZERO, R0_ZERO, R0_ZERO),
               "ERET exact emitted word");
}

void malformedDiagnostics(TestContext& ctx) {
    const AssemblyResult duplicate = assemble("foo: halt\n.data\nfoo: .word 1\n");
    ctx.check(!duplicate.success, "duplicate labels are rejected");
    ctx.check(!duplicate.errors.empty(), "duplicate label has diagnostic");
    if (!duplicate.errors.empty()) {
        ctx.contains(duplicate.errors.front().format(), "Duplicate label",
                     "duplicate label diagnostic text");
    }

    const AssemblyResult badOrg = assemble(".org -1\nhalt\n");
    ctx.check(!badOrg.success, "negative .org is rejected");
    if (!badOrg.errors.empty()) {
        ctx.contains(badOrg.errors.front().format(), ".org",
                     "negative .org diagnostic mentions directive");
    }

    const AssemblyResult badCsr = assemble("csrr r1, bogus\n");
    ctx.check(!badCsr.success, "unknown CSR is rejected");
    if (!badCsr.errors.empty()) {
        ctx.contains(badCsr.errors.front().format(), "CSR",
                     "unknown CSR diagnostic mentions CSR");
    }

    const AssemblyResult dataBranch = assemble(".data\nx: .word 1\n.text\njmp x\n");
    ctx.check(!dataBranch.success, "data label cannot be branch target");
}

void vectorExactEncoding(TestContext& ctx) {
    const auto program = assembleOrThrow(R"(
        vbcast.t20 v0, r1
        vadd.t20   v1, v0, v0
        vsel.t20   v4, v3, v0, v1, v2
        vload.t20  v5, r2, 3
        vstore.t20 v5, r2, -2
        halt
    )");

    ctx.equal(static_cast<int>(program.size()), 6, "vector fixture word count");
    expectWord(ctx, program[0],
               InstructionWord::encodeR(Opcode::VBCAST, 0, R1, R0_ZERO, FUNC_T20),
               "VBCAST exact emitted word");
    expectWord(ctx, program[1],
               InstructionWord::encodeR(Opcode::VADD, 1, 0, 0, FUNC_T20),
               "VADD exact emitted word");
    expectWord(ctx, program[2],
               InstructionWord::encodeR5(Opcode::VSEL, 4, 3, 0, 1, 2, FUNC_T20),
               "VSEL exact emitted word");
    expectWord(ctx, program[3],
               InstructionWord::encodeVectorMemory(Opcode::VLOAD, 5, R2, 3, FUNC_T20),
               "VLOAD exact emitted word");
    expectWord(ctx, program[4],
               InstructionWord::encodeVectorMemory(Opcode::VSTORE, 5, R2, -2, FUNC_T20),
               "VSTORE exact emitted word");
}

void atomicExactEncoding(TestContext& ctx) {
    const auto program = assembleOrThrow(R"(
        tldr.+1 r1, r2
        tstr.-1 r3, r4, r5, r6
        fence.+1
        fence -1
        halt
    )");

    ctx.equal(static_cast<int>(program.size()), 5, "atomic fixture word count");
    expectWord(ctx, program[0],
               InstructionWord::encodeR(Opcode::TLDR, R1, R2, R0_ZERO,
                                        FUNC_ORDER_SEQ_CST),
               "TLDR.+1 exact emitted word");
    expectWord(ctx, program[1],
               InstructionWord::encodeR4(Opcode::TSTR, R3, R4, R5, R6,
                                         FUNC_ORDER_RELAXED),
               "TSTR.-1 exact emitted word");
    expectWord(ctx, program[2],
               InstructionWord::encodeR(Opcode::FENCE, R0_ZERO, R0_ZERO, R0_ZERO,
                                        FUNC_ORDER_SEQ_CST),
               "FENCE.+1 exact emitted word");
    expectWord(ctx, program[3],
               InstructionWord::encodeR(Opcode::FENCE, R0_ZERO, R0_ZERO, R0_ZERO,
                                        FUNC_ORDER_RELAXED),
               "FENCE operand order exact emitted word");
}

void suffixAndRegisterDiagnostics(TestContext& ctx) {
    ctx.check(!assemble("vadd v1, v0, v0\n").success,
              "bare vector arithmetic is rejected");
    ctx.check(!assemble("vadd.l20 v1, v0, v0\n").success,
              "lane suffix is rejected for vector arithmetic");
    ctx.check(!assemble("vadd.t20 r1, v0, v0\n").success,
              "scalar destination is rejected for vector arithmetic");
    ctx.check(!assemble("vbcast.t20 v0, v1\n").success,
              "vector source is rejected for VBCAST scalar operand");
    ctx.check(!assemble("vload.t20 r1, r2, 0\n").success,
              "scalar destination is rejected for VLOAD");
    ctx.check(!assemble("vload.t20 v0, v8, 0\n").success,
              "vector register is rejected as VLOAD base");
    ctx.check(!assemble("tldr.t20 r1, r2\n").success,
              "width suffix is rejected for TLDR");
    ctx.check(!assemble("tstr r1, r2, r3\n").success,
              "short TSTR operand list is rejected");
    ctx.check(!assemble("fence 2\n").success,
              "invalid FENCE memory order is rejected");
}

void phase4VectorPlumbingExactEncoding(TestContext& ctx) {
    const auto program = assembleOrThrow(R"(
        vpack.t20.t10   v4, v5
        vunpack.t10.t20 v5, v4
        vpermute.t20    v6, v5, v0
        vblend.t20      v7, v2, v3, v4
        vswap           v0, v1
        vgather.t20     v2, r1, v0
        vscatter.t20    v2, r1, v0
        halt
    )");

    ctx.equal(static_cast<int>(program.size()), 8, "phase4 vector fixture word count");
    expectWord(ctx, program[0],
               InstructionWord::encodeR(Opcode::VPACK, 4, 5, FUNC_T20, FUNC_T10),
               "VPACK exact emitted word");
    expectWord(ctx, program[1],
               InstructionWord::encodeR(Opcode::VUNPACK, 5, 4, FUNC_T10, FUNC_T20),
               "VUNPACK exact emitted word");
    expectWord(ctx, program[2],
               InstructionWord::encodeR(Opcode::VPERMUTE, 6, 5, 0, FUNC_T20),
               "VPERMUTE exact emitted word");
    expectWord(ctx, program[3],
               InstructionWord::encodeR5(Opcode::VBLEND, 7, 2, 3, 3, 4, FUNC_T20),
               "VBLEND exact emitted word");
    expectWord(ctx, program[4],
               InstructionWord::encodeR(Opcode::VSWAP, 0, 1, R0_ZERO, FUNC_DEFAULT),
               "VSWAP exact emitted word");
    expectWord(ctx, program[5],
               InstructionWord::encodeR(Opcode::VGATHER, 2, R1, 0, FUNC_T20),
               "VGATHER exact emitted word");
    expectWord(ctx, program[6],
               InstructionWord::encodeR(Opcode::VSCATTER, 2, R1, 0, FUNC_T20),
               "VSCATTER exact emitted word");
    ctx.contains(disassemble(program[0]), "VPACK.t20.t10 v4, v5",
                 "disassembler prints VPACK suffix pair");
    ctx.contains(disassemble(program[3]), "VBLEND.t20 v7, v2, v3, v4",
                 "disassembler prints VBLEND shape");
}

void vectorReductionExactEncoding(TestContext& ctx) {
    const auto program = assembleOrThrow(R"(
        vsum.t20  r13, v0
        vhmin.t20 r14, v1
        vhmax.t20 r15, v2
        halt
    )");

    ctx.equal(static_cast<int>(program.size()), 4, "vector reduction fixture word count");
    expectWord(ctx, program[0],
               InstructionWord::encodeR(Opcode::VSUM, 13, 0, R0_ZERO, FUNC_T20),
               "VSUM exact emitted word");
    expectWord(ctx, program[1],
               InstructionWord::encodeR(Opcode::VHMIN, 14, 1, R0_ZERO, FUNC_T20),
               "VHMIN exact emitted word");
    expectWord(ctx, program[2],
               InstructionWord::encodeR(Opcode::VHMAX, 15, 2, R0_ZERO, FUNC_T20),
               "VHMAX exact emitted word");
    ctx.contains(disassemble(program[0]), "VSUM.t20 r13, v0",
                 "disassembler prints VSUM shape");
}

void phase4VectorDiagnostics(TestContext& ctx) {
    ctx.check(!assemble("vpack.t20 v1, v0\n").success,
              "VPACK requires source and destination suffixes");
    ctx.check(!assemble("vpack.t20.l10 v1, v0\n").success,
              "VPACK rejects mixed numeric/lane suffix pair");
    ctx.check(!assemble("vunpack.t10 v1, v0\n").success,
              "VUNPACK requires source and destination suffixes");
    ctx.check(!assemble("vpermute.l20 v1, v0, v2\n").success,
              "VPERMUTE rejects lane suffix");
    ctx.check(!assemble("vblend.t20 v7, v2, v3\n").success,
              "VBLEND rejects short operand list");
    ctx.check(!assemble("vswap.t20 v0, v1\n").success,
              "VSWAP rejects width suffix");
    ctx.check(!assemble("vgather.t20 v2, v0, v1\n").success,
              "VGATHER rejects vector base register");
    ctx.check(!assemble("vscatter.t20 r2, r1, v0\n").success,
              "VSCATTER rejects scalar vector operand");
    ctx.check(!assemble("vsum r1, v0\n").success,
              "VSUM requires suffix");
    ctx.check(!assemble("vsum.t20 v1, v0\n").success,
              "VSUM rejects vector destination");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"assembler.labels.forward_backward_branches", "assembler.golden_contract", forwardBackwardBranches},
        {"assembler.directives.org_word_layout", "assembler.golden_contract", orgWordLayout},
        {"assembler.csr.exact_encoding", "assembler.golden_contract", csrExactEncoding},
        {"assembler.diagnostics.malformed_input", "assembler.golden_contract", malformedDiagnostics},
        {"assembler.vector.exact_encoding", "assembler.golden_contract", vectorExactEncoding},
        {"assembler.atomic.memory_order_encoding", "assembler.golden_contract", atomicExactEncoding},
        {"assembler.diagnostics.suffix_register_rejection", "assembler.golden_contract", suffixAndRegisterDiagnostics},
        {"assembler.vector.phase4_plumbing_exact_encoding", "assembler.golden_contract", phase4VectorPlumbingExactEncoding},
        {"assembler.vector.reduction_exact_encoding", "assembler.golden_contract", vectorReductionExactEncoding},
        {"assembler.diagnostics.phase4_vector_rejection", "assembler.golden_contract", phase4VectorDiagnostics},
    };
    return tests_next::runCases("next_assembler_goldens", cases);
}
