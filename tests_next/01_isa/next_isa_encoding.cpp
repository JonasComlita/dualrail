#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_asm.h"

#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::isa;
using namespace sandbox::vm::assembler;

void expectWordBits(TestContext& ctx,
                    const TritWord27& got,
                    const TritWord27& want,
                    const std::string& message) {
    ctx.equal(got.bits, want.bits, message);
}

void rtypeWidthFields(TestContext& ctx) {
    const TritWord27 word =
        InstructionWord::encodeSemanticR(Opcode::ADD, R3, R1, R2, FUNC_T20);
    const InstructionWord decoded = InstructionWord::decodeSemantic(word);

    ctx.check(!decoded.malformed, "ADD.t20 decodes cleanly");
    ctx.check(decoded.fmt == InstructionFormat::R_TYPE, "ADD.t20 is R-type");
    ctx.check(decoded.opcode == Opcode::ADD, "ADD opcode roundtrips");
    ctx.equal(static_cast<int>(decoded.rd), static_cast<int>(R3), "rd field");
    ctx.equal(static_cast<int>(decoded.rs1), static_cast<int>(R1), "rs1 field");
    ctx.equal(static_cast<int>(decoded.rs2), static_cast<int>(R2), "rs2 field");
    ctx.equal(static_cast<int>(decoded.func), static_cast<int>(FUNC_T20), "func field");
    ctx.equal(static_cast<int>(word.getTrit(FIELD_FMT_LSB)), static_cast<int>(T_POS),
              "raw R-type format trit");
    ctx.equal(word.getField(FIELD_RD_LSB, FIELD_RD_W),
              static_cast<int>(R3) - REG_FIELD_OFFSET,
              "raw rd ternary field");
}

void immediateSignedFields(TestContext& ctx) {
    const TritWord27 mov =
        InstructionWord::encodeSemanticI(Opcode::MOV, R5, R0_ZERO, -42);
    const InstructionWord decoded = InstructionWord::decodeSemantic(mov);

    ctx.check(!decoded.malformed, "MOV immediate decodes cleanly");
    ctx.check(decoded.fmt == InstructionFormat::I_TYPE, "MOV is I-type");
    ctx.check(decoded.opcode == Opcode::MOV, "MOV opcode roundtrips");
    ctx.equal(static_cast<int>(decoded.rd), static_cast<int>(R5), "MOV rd field");
    ctx.equal(decoded.imm, -42, "signed immediate roundtrip");
    ctx.equal(decodeSigned(mov, FIELD_IMM16_LSB, FIELD_IMM16_W), -42,
              "raw imm16 signed field");
}

void branchOffsets(TestContext& ctx) {
    const TritWord27 brp = InstructionWord::encodeSemanticB(Opcode::BRP, R2, -3);
    const TritWord27 brn = InstructionWord::encodeSemanticB(Opcode::BRN, R4, 5);
    const TritWord27 brz = InstructionWord::encodeSemanticB(Opcode::BRZ, R1, 0);

    const InstructionWord brp_decoded = InstructionWord::decodeSemantic(brp);
    const InstructionWord brn_decoded = InstructionWord::decodeSemantic(brn);
    const InstructionWord brz_decoded = InstructionWord::decodeSemantic(brz);

    ctx.check(brp_decoded.opcode == Opcode::BRP, "BRP opcode");
    ctx.equal(static_cast<int>(brp_decoded.rs_branch), static_cast<int>(R2),
              "BRP condition register");
    ctx.equal(brp_decoded.offset, -3, "BRP signed offset");
    ctx.check(brn_decoded.opcode == Opcode::BRN, "BRN opcode");
    ctx.equal(static_cast<int>(brn_decoded.rs_branch), static_cast<int>(R4),
              "BRN condition register");
    ctx.equal(brn_decoded.offset, 5, "BRN signed offset");
    ctx.check(brz_decoded.opcode == Opcode::BRZ, "BRZ opcode");
    ctx.equal(static_cast<int>(brz_decoded.rs_branch), static_cast<int>(R1),
              "BRZ condition register");
    ctx.equal(brz_decoded.offset, 0, "BRZ zero offset");
}

void csrNameResolution(TestContext& ctx) {
    ctx.equal(parseCSR("cause"), CSR_CAUSE, "cause CSR parser");
    ctx.equal(parseCSR("tvec"), CSR_TVEC, "tvec CSR parser");
    ctx.equal(parseCSR("mmu_enable"), CSR_MMU_ENABLE, "MMU CSR parser");
    ctx.equal(parseCSR("page_fault_addr"), CSR_PAGE_FAULT_ADDR,
              "page fault CSR parser");
    ctx.equal(parseCSR("console_in_ctrl"), CSR_CONSOLE_IN_CTRL,
              "console CSR parser");
    ctx.equal(parseCSR("bogus"), -1, "unknown CSR rejected");
    ctx.equal(parseCSR("99"), -1, "out-of-range numeric CSR rejected");
    ctx.equal(std::string(csrToString(CSR_PAGE_FAULT_ACCESS)),
              std::string("page_fault_access"),
              "CSR name printer");

    const auto program = assembleOrThrow(R"(
        csrr r1, cause
        csrw tvec, r1
        csrrw r2, scratch, r3
        eret
        halt
    )");
    expectWordBits(ctx, program[0],
                   VersionedInstructionCodec::encodeI(
                       Opcode::CSRR, R1, R0_ZERO, CSR_CAUSE,
                       IsaEncodingVersion::V2),
                   "CSRR exact word");
    expectWordBits(ctx, program[1],
                   VersionedInstructionCodec::encodeI(
                       Opcode::CSRW, R1, R0_ZERO, CSR_TVEC,
                       IsaEncodingVersion::V2),
                   "CSRW exact word");
    expectWordBits(ctx, program[2],
                   VersionedInstructionCodec::encodeR(
                       Opcode::CSRRW, R2, R3, CSR_SCRATCH, FUNC_DEFAULT,
                       IsaEncodingVersion::V2),
                   "CSRRW exact word");
    expectWordBits(ctx, program[3],
                   VersionedInstructionCodec::encodeR(
                       Opcode::ERET, R0_ZERO, R0_ZERO, R0_ZERO, FUNC_DEFAULT,
                       IsaEncodingVersion::V2),
                   "ERET exact word");
}

void r4R5Layouts(TestContext& ctx) {
    const TritWord27 r4 =
        InstructionWord::encodeSemanticR4(Opcode::TWCMP, R5, R1, R2, R3, FUNC_T20);
    const InstructionWord r4_decoded = InstructionWord::decodeSemantic(r4);
    ctx.check(r4_decoded.r4_layout, "TWCMP uses R4 layout");
    ctx.check(r4_decoded.opcode == Opcode::TWCMP, "TWCMP opcode");
    ctx.equal(static_cast<int>(r4_decoded.rs3), static_cast<int>(R3), "R4 third source");
    ctx.equal(static_cast<int>(r4_decoded.func), static_cast<int>(FUNC_T20), "R4 func");

    const TritWord27 tsel =
        InstructionWord::encodeSemanticR5(Opcode::TSEL, R6, R3, R1, R2, R4);
    const InstructionWord tsel_decoded = InstructionWord::decodeSemantic(tsel);
    ctx.check(tsel_decoded.r5_layout, "TSEL uses R5 layout");
    ctx.equal(static_cast<int>(tsel_decoded.rcond), static_cast<int>(R3),
              "TSEL condition register");
    ctx.equal(static_cast<int>(tsel_decoded.rneg), static_cast<int>(R1),
              "TSEL negative arm");
    ctx.equal(static_cast<int>(tsel_decoded.rzero), static_cast<int>(R2),
              "TSEL zero arm");
    ctx.equal(static_cast<int>(tsel_decoded.rpos), static_cast<int>(R4),
              "TSEL positive arm");

    const TritWord27 vsel =
        InstructionWord::encodeSemanticR5(Opcode::VSEL, 6, 3, 1, 2, 4, FUNC_T20);
    const InstructionWord vsel_decoded = InstructionWord::decodeSemantic(vsel);
    ctx.check(vsel_decoded.opcode == Opcode::VSEL, "VSEL opcode");
    ctx.equal(static_cast<int>(vsel_decoded.func), static_cast<int>(FUNC_T20),
              "VSEL stores width suffix in R5 overlay");
}

void vectorMemoryOverlay(TestContext& ctx) {
    const TritWord27 vload =
        InstructionWord::encodeSemanticVectorMemory(Opcode::VLOAD, 5, R2, 3, FUNC_T20);
    const InstructionWord vload_decoded = InstructionWord::decodeSemantic(vload);
    ctx.check(!vload_decoded.malformed, "VLOAD decodes cleanly");
    ctx.check(vload_decoded.fmt == InstructionFormat::I_TYPE, "VLOAD uses I-type overlay");
    ctx.check(vload_decoded.opcode == Opcode::VLOAD, "VLOAD opcode");
    ctx.equal(static_cast<int>(vload_decoded.rd), 5, "VLOAD vector destination field");
    ctx.equal(static_cast<int>(vload_decoded.rs1), static_cast<int>(R2), "VLOAD scalar base field");
    ctx.equal(static_cast<int>(vload_decoded.func), static_cast<int>(FUNC_T20), "VLOAD width field");
    ctx.equal(vload_decoded.imm, 3, "VLOAD signed immediate field");

    const TritWord27 vstore =
        InstructionWord::encodeSemanticVectorMemory(Opcode::VSTORE, 6, R4, -2, FUNC_T10);
    const InstructionWord vstore_decoded = InstructionWord::decodeSemantic(vstore);
    ctx.check(vstore_decoded.opcode == Opcode::VSTORE, "VSTORE opcode");
    ctx.equal(static_cast<int>(vstore_decoded.rd), 6, "VSTORE vector source field");
    ctx.equal(static_cast<int>(vstore_decoded.rs1), static_cast<int>(R4), "VSTORE scalar base field");
    ctx.equal(static_cast<int>(vstore_decoded.func), static_cast<int>(FUNC_T10), "VSTORE width field");
    ctx.equal(vstore_decoded.imm, -2, "VSTORE signed immediate field");
}

void atomicMemoryOrderFields(TestContext& ctx) {
    const TritWord27 tldr =
        InstructionWord::encodeSemanticR(Opcode::TLDR, R1, R2, R0_ZERO,
                                 FUNC_ORDER_SEQ_CST);
    const InstructionWord tldr_decoded = InstructionWord::decodeSemantic(tldr);
    ctx.check(tldr_decoded.opcode == Opcode::TLDR, "TLDR opcode");
    ctx.equal(static_cast<int>(tldr_decoded.rd), static_cast<int>(R1), "TLDR destination");
    ctx.equal(static_cast<int>(tldr_decoded.rs1), static_cast<int>(R2), "TLDR address register");
    ctx.equal(static_cast<int>(tldr_decoded.func), static_cast<int>(FUNC_ORDER_SEQ_CST),
              "TLDR memory order func");
    ctx.equal(atomicOrderFromFunc(tldr_decoded.func), ATOMIC_ORDER_SEQ_CST,
              "TLDR order decodes to seq-cst");

    const TritWord27 tstr =
        InstructionWord::encodeSemanticR4(Opcode::TSTR, R3, R4, R5, R6,
                                  FUNC_ORDER_RELAXED);
    const InstructionWord tstr_decoded = InstructionWord::decodeSemantic(tstr);
    ctx.check(tstr_decoded.opcode == Opcode::TSTR, "TSTR opcode");
    ctx.check(tstr_decoded.r4_layout, "TSTR uses R4 layout");
    ctx.equal(static_cast<int>(tstr_decoded.rd), static_cast<int>(R3), "TSTR status register");
    ctx.equal(static_cast<int>(tstr_decoded.rs1), static_cast<int>(R4), "TSTR address register");
    ctx.equal(static_cast<int>(tstr_decoded.rs2), static_cast<int>(R5), "TSTR desired register");
    ctx.equal(static_cast<int>(tstr_decoded.rs3), static_cast<int>(R6), "TSTR expected register");
    ctx.equal(atomicOrderFromFunc(tstr_decoded.func), ATOMIC_ORDER_RELAXED,
              "TSTR order decodes to relaxed");

    const TritWord27 fence =
        InstructionWord::encodeSemanticR(Opcode::FENCE, R0_ZERO, R0_ZERO, R0_ZERO,
                                 FUNC_ORDER_ACQ_REL);
    const InstructionWord fence_decoded = InstructionWord::decodeSemantic(fence);
    ctx.check(fence_decoded.opcode == Opcode::FENCE, "FENCE opcode");
    ctx.equal(atomicOrderFromFunc(fence_decoded.func), ATOMIC_ORDER_ACQ_REL,
              "FENCE default order decodes to acq-rel");
}

void invalidInstructionRejection(TestContext& ctx) {
    TritWord27 reserved_gap = InstructionWord::encodeSemanticR(
        Opcode::ADD, R1, R2, R3);
    encodeUnsignedField(reserved_gap, FIELD_OP_LSB, FIELD_OP_W, 15);
    const InstructionWord reserved_gap_decoded =
        VersionedInstructionCodec::decode(
            reserved_gap, IsaEncodingVersion::V2);
    ctx.check(reserved_gap_decoded.opcode == Opcode::RESERVED,
              "unassigned direct opcode 15 decodes as reserved");

    TritWord27 reserved = InstructionWord::encodeSemanticR(
        Opcode::ADD, R1, R2, R3);
    encodeUnsignedField(reserved, FIELD_OP_LSB, FIELD_OP_W, 38);
    const InstructionWord reserved_decoded = VersionedInstructionCodec::decode(
        reserved, IsaEncodingVersion::V2);
    ctx.check(!reserved_decoded.malformed, "reserved opcode word is structurally well formed");
    ctx.check(reserved_decoded.opcode == Opcode::RESERVED,
              "reserved direct opcode 38 decodes as reserved");

    TritWord27 malformed{};
    malformed.bits = 0x3ULL;
    const InstructionWord malformed_decoded = VersionedInstructionCodec::decode(
        malformed, IsaEncodingVersion::V2);
    ctx.check(malformed_decoded.malformed, "raw 0b11 trit pattern is rejected as malformed");
}

void phase4VectorPlumbingFields(TestContext& ctx) {
    const TritWord27 vpack =
        InstructionWord::encodeSemanticR(Opcode::VPACK, 4, 5, FUNC_T20, FUNC_T10);
    const InstructionWord vpack_decoded = InstructionWord::decodeSemantic(vpack);
    ctx.check(vpack_decoded.opcode == Opcode::VPACK, "VPACK opcode");
    ctx.equal(static_cast<int>(vpack_decoded.rd), 4, "VPACK destination vector");
    ctx.equal(static_cast<int>(vpack_decoded.rs1), 5, "VPACK source vector");
    ctx.equal(static_cast<int>(vpack_decoded.rs2), static_cast<int>(FUNC_T20),
              "VPACK source width field");
    ctx.equal(static_cast<int>(vpack_decoded.func), static_cast<int>(FUNC_T10),
              "VPACK destination width field");

    const TritWord27 vunpack =
        InstructionWord::encodeSemanticR(Opcode::VUNPACK, 5, 4, FUNC_T10, FUNC_T20);
    const InstructionWord vunpack_decoded = InstructionWord::decodeSemantic(vunpack);
    ctx.check(vunpack_decoded.opcode == Opcode::VUNPACK, "VUNPACK opcode");
    ctx.equal(static_cast<int>(vunpack_decoded.rs2), static_cast<int>(FUNC_T10),
              "VUNPACK source width field");
    ctx.equal(static_cast<int>(vunpack_decoded.func), static_cast<int>(FUNC_T20),
              "VUNPACK destination width field");

    const TritWord27 vpermute =
        InstructionWord::encodeSemanticR(Opcode::VPERMUTE, 6, 5, 0, FUNC_T20);
    const InstructionWord vpermute_decoded = InstructionWord::decodeSemantic(vpermute);
    ctx.check(vpermute_decoded.opcode == Opcode::VPERMUTE, "VPERMUTE opcode");
    ctx.equal(static_cast<int>(vpermute_decoded.rd), 6, "VPERMUTE destination");
    ctx.equal(static_cast<int>(vpermute_decoded.rs1), 5, "VPERMUTE source vector");
    ctx.equal(static_cast<int>(vpermute_decoded.rs2), 0, "VPERMUTE index vector");
    ctx.equal(static_cast<int>(vpermute_decoded.func), static_cast<int>(FUNC_T20),
              "VPERMUTE width field");

    const TritWord27 vblend =
        InstructionWord::encodeSemanticR5(Opcode::VBLEND, 7, 2, 3, 3, 4, FUNC_T20);
    const InstructionWord vblend_decoded = InstructionWord::decodeSemantic(vblend);
    ctx.check(vblend_decoded.opcode == Opcode::VBLEND, "VBLEND opcode");
    ctx.check(vblend_decoded.r5_layout, "VBLEND uses R5 layout");
    ctx.equal(static_cast<int>(vblend_decoded.rcond), 2, "VBLEND condition vector");
    ctx.equal(static_cast<int>(vblend_decoded.rneg), 3, "VBLEND negative false arm");
    ctx.equal(static_cast<int>(vblend_decoded.rzero), 3, "VBLEND zero false arm");
    ctx.equal(static_cast<int>(vblend_decoded.rpos), 4, "VBLEND positive true arm");
    ctx.equal(static_cast<int>(vblend_decoded.func), static_cast<int>(FUNC_T20),
              "VBLEND width field");

    const TritWord27 vgather =
        InstructionWord::encodeSemanticR(Opcode::VGATHER, 2, R1, 0, FUNC_T20);
    const InstructionWord vgather_decoded = InstructionWord::decodeSemantic(vgather);
    ctx.check(vgather_decoded.opcode == Opcode::VGATHER, "VGATHER opcode");
    ctx.equal(static_cast<int>(vgather_decoded.rd), 2, "VGATHER vector field");
    ctx.equal(static_cast<int>(vgather_decoded.rs1), static_cast<int>(R1),
              "VGATHER scalar base field");
    ctx.equal(static_cast<int>(vgather_decoded.rs2), 0, "VGATHER index vector field");

    const TritWord27 vscatter =
        InstructionWord::encodeSemanticR(Opcode::VSCATTER, 2, R1, 0, FUNC_T20);
    const InstructionWord vscatter_decoded = InstructionWord::decodeSemantic(vscatter);
    ctx.check(vscatter_decoded.opcode == Opcode::VSCATTER, "VSCATTER opcode");
    ctx.equal(static_cast<int>(vscatter_decoded.rs1), static_cast<int>(R1),
              "VSCATTER scalar base field");
}

void vectorReductionFields(TestContext& ctx) {
    const TritWord27 vsum =
        InstructionWord::encodeSemanticR(Opcode::VSUM, 13, 0, R0_ZERO, FUNC_T20);
    const InstructionWord vsum_decoded = InstructionWord::decodeSemantic(vsum);
    ctx.check(vsum_decoded.opcode == Opcode::VSUM, "VSUM opcode");
    ctx.equal(static_cast<int>(vsum_decoded.rd), 13, "VSUM scalar destination");
    ctx.equal(static_cast<int>(vsum_decoded.rs1), 0, "VSUM vector source");
    ctx.equal(static_cast<int>(vsum_decoded.func), static_cast<int>(FUNC_T20),
              "VSUM width field");

    const TritWord27 vhmin =
        InstructionWord::encodeSemanticR(Opcode::VHMIN, 14, 1, R0_ZERO, FUNC_T10);
    const InstructionWord vhmin_decoded = InstructionWord::decodeSemantic(vhmin);
    ctx.check(vhmin_decoded.opcode == Opcode::VHMIN, "VHMIN opcode");
    ctx.equal(static_cast<int>(vhmin_decoded.rd), 14, "VHMIN scalar destination");
    ctx.equal(static_cast<int>(vhmin_decoded.rs1), 1, "VHMIN vector source");
    ctx.equal(static_cast<int>(vhmin_decoded.func), static_cast<int>(FUNC_T10),
              "VHMIN width field");

    const TritWord27 vhmax =
        InstructionWord::encodeSemanticR(Opcode::VHMAX, 15, 2, R0_ZERO, FUNC_T5);
    const InstructionWord vhmax_decoded = InstructionWord::decodeSemantic(vhmax);
    ctx.check(vhmax_decoded.opcode == Opcode::VHMAX, "VHMAX opcode");
    ctx.equal(static_cast<int>(vhmax_decoded.rd), 15, "VHMAX scalar destination");
    ctx.equal(static_cast<int>(vhmax_decoded.rs1), 2, "VHMAX vector source");
    ctx.equal(static_cast<int>(vhmax_decoded.func), static_cast<int>(FUNC_T5),
              "VHMAX width field");
}

} // namespace

int main() {
    const std::vector<TestCase> cases = {
        {"isa.encoding.rtype_width_fields", "isa.encoding_contract", rtypeWidthFields},
        {"isa.encoding.immediate_signed_fields", "isa.encoding_contract", immediateSignedFields},
        {"isa.branch.brp_brn_brz_offsets", "isa.encoding_contract", branchOffsets},
        {"isa.csr.name_resolution", "isa.encoding_contract", csrNameResolution},
        {"isa.encoding.r4_r5_layouts", "isa.encoding_contract", r4R5Layouts},
        {"isa.vector.vector_memory_overlay", "isa.encoding_contract", vectorMemoryOverlay},
        {"isa.atomic.memory_order_fields", "isa.encoding_contract", atomicMemoryOrderFields},
        {"isa.encoding.invalid_instruction_rejection", "isa.encoding_contract", invalidInstructionRejection},
        {"isa.vector.phase4_plumbing_fields", "isa.encoding_contract", phase4VectorPlumbingFields},
        {"isa.vector.reduction_fields", "isa.encoding_contract", vectorReductionFields},
    };
    return tests_next::runCases("next_isa_encoding", cases);
}
