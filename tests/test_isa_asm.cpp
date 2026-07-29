#include "test_multiwidth_vm_common.h"

void testIsaAndAsmWidths() {
    std::cout << "[6] ISA func widths and assembler suffixes\n";
    using namespace sandbox::isa;
    using namespace sandbox::vm::assembler;
    const auto assembleOrThrow = [](const std::string& source) {
        return assembleV2TestOrThrow(source);
    };
    const auto decodeV2 = [](const TritWord27& word) {
        return VersionedInstructionCodec::decode(
            word, IsaEncodingVersion::V2);
    };

    for (uint8_t func : {FUNC_T1, FUNC_T5, FUNC_T10, FUNC_T20, FUNC_T40, FUNC_T50}) {
        TritWord27 w = VersionedInstructionCodec::encodeR(
            Opcode::ADD, R3, R1, R2, func, IsaEncodingVersion::V2);
        InstructionWord iw = decodeV2(w);
        expect(!iw.malformed, "width func decodes");
        expect(iw.func == func, "width func roundtrip");
    }

    for (uint8_t func : {FUNC_L1, FUNC_L5, FUNC_L10, FUNC_L20, FUNC_L40, FUNC_L50}) {
        TritWord27 w = VersionedInstructionCodec::encodeR(
            Opcode::TLADD, R3, R1, R2, func, IsaEncodingVersion::V2);
        InstructionWord iw = decodeV2(w);
        expect(!iw.malformed, "lane width func decodes");
        expect(iw.func == func, "lane width func roundtrip");
    }

    for (uint8_t op = 27; op <= OPCODE_MAX_ASSIGNED; ++op) {
        TritWord27 w = VersionedInstructionCodec::encodeR(
            static_cast<Opcode>(op), R3, R1, R2, FUNC_DEFAULT,
            IsaEncodingVersion::V2);
        InstructionWord iw = decodeV2(w);
        expect(!iw.malformed, "Phase 4 opcode decodes");
        expect(iw.opcode == static_cast<Opcode>(op), "Phase 4 opcode roundtrip");
        expect(opcodeToString(iw.opcode) != "???", "Phase 4 opcode has disassembly name");
    }

    auto add = assembleV2TestOrThrow("add.t20 r3, r1, r2\nhalt\n");
    auto iw = decodeV2(add[0]);
    expect(iw.opcode == Opcode::ADD && iw.func == FUNC_T20, "add.t20 encodes func");

    auto mov = assembleV2TestOrThrow("mov.t20 r1, 42\nhalt\n");
    expect(mov.size() == 3, "mov.t20 lowers to MOV + CVT + HALT");
    auto cvt = decodeV2(mov[1]);
    expect(cvt.opcode == Opcode::CVT && cvt.func == FUNC_T20, "mov.t20 emits CVT.t20");

    auto bad = assemble("load.t20 r1, r2, 0\n");
    expect(!bad.success, "suffix rejected on LOAD");

    TritWord27 tselWord = VersionedInstructionCodec::encodeR5(
        Opcode::TSEL, R6, R3, R1, R2, R4, FUNC_DEFAULT,
        IsaEncodingVersion::V2);
    auto tsel = decodeV2(tselWord);
    expect(!tsel.malformed && tsel.opcode == Opcode::TSEL && tsel.r5_layout,
           "TSEL R5 layout decodes");
    expect(tsel.rd == R6 && tsel.rcond == R3 && tsel.rneg == R1 &&
           tsel.rzero == R2 && tsel.rpos == R4,
           "TSEL R5 register fields roundtrip");

    auto tselAsm = assembleV2TestOrThrow("tsel r6, r3, r1, r2, r4\nhalt\n");
    auto tselIw = decodeV2(tselAsm[0]);
    expect(tselIw.opcode == Opcode::TSEL && tselIw.rpos == R4,
           "assembler encodes TSEL");

    auto branchAsm = assembleV2TestOrThrow("brz r1, 2\nbrp r2, -1\nhalt\n");
    auto brz = decodeV2(branchAsm[0]);
    auto brp = decodeV2(branchAsm[1]);
    expect(brz.opcode == Opcode::BRZ && brz.rs_branch == R1 && brz.offset == 2,
           "assembler encodes BRZ");
    expect(brp.opcode == Opcode::BRP && brp.rs_branch == R2 && brp.offset == -1,
           "assembler encodes BRP");

    auto swapAsm = assembleV2TestOrThrow("swap r1, r2\nhalt\n");
    auto swap = decodeV2(swapAsm[0]);
    expect(swap.opcode == Opcode::SWAP && swap.rd == R1 && swap.rs1 == R2,
           "assembler encodes SWAP");

    auto cvtPair = assembleV2TestOrThrow("cvt.t10.t20 r3, r2\nhalt\n");
    auto cvtPairIw = decodeV2(cvtPair[0]);
    expect(cvtPairIw.opcode == Opcode::CVT &&
           cvtPairIw.rs2 == FUNC_T10 && cvtPairIw.func == FUNC_T20,
           "assembler encodes cvt.src.dst");

    auto movLane = assembleV2TestOrThrow("mov.l20 r1, 7\nhalt\n");
    expect(movLane.size() == 3, "mov.l20 lowers to MOV + CVT + HALT");
    auto movLaneCvt = decodeV2(movLane[1]);
    expect(movLaneCvt.opcode == Opcode::CVT &&
           movLaneCvt.rs2 == FUNC_T20 && movLaneCvt.func == FUNC_L20,
           "mov.l20 lowers through matching numeric width");

    auto laneAdd = assembleV2TestOrThrow("tladd.l20 r3, r1, r2\ntlneg.l20 r4, r3\nhalt\n");
    auto laneAddIw = decodeV2(laneAdd[0]);
    auto laneNegIw = decodeV2(laneAdd[1]);
    expect(laneAddIw.opcode == Opcode::TLADD && laneAddIw.func == FUNC_L20,
           "assembler encodes tladd.l20");
    expect(laneNegIw.opcode == Opcode::TLNEG && laneNegIw.func == FUNC_L20,
           "assembler encodes tlneg.l20");

    expect(!assemble("tladd r1, r2, r3\n").success, "bare TLADD rejected");
    expect(!assemble("tladd.t20 r1, r2, r3\n").success, "numeric suffix rejected on TLADD");
    expect(!assemble("add.l20 r1, r2, r3\n").success, "lane suffix rejected on numeric ADD");
    expect(!assemble("cvt.t20.l10 r1, r2\n").success, "mismatched numeric-lane CVT rejected");

    auto vlen = assembleV2TestOrThrow("vlen r3\nhalt\n");
    auto vlenIw = decodeV2(vlen[0]);
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
    expect(decodeV2(vectorOps[0]).opcode == Opcode::VBCAST &&
           decodeV2(vectorOps[0]).func == FUNC_T20,
           "assembler encodes VBCAST.t20");
    expect(decodeV2(vectorOps[1]).opcode == Opcode::VADD &&
           decodeV2(vectorOps[1]).rd == 1,
           "assembler encodes VADD vector registers");
    auto vselIw = decodeV2(vectorOps[4]);
    expect(vselIw.opcode == Opcode::VSEL && vselIw.r5_layout &&
           vselIw.rd == 4 && vselIw.rcond == 3 && vselIw.rneg == 0 &&
           vselIw.rzero == 1 && vselIw.rpos == 2 && vselIw.func == FUNC_T20,
           "assembler encodes VSEL R5 vector fields");
    auto vloadIw = decodeV2(vectorOps[5]);
    auto vstoreIw = decodeV2(vectorOps[6]);
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
    expect(decodeV2(phase4Rest[0]).opcode == Opcode::ACLR,
           "assembler encodes ACLR");
    expect(decodeV2(phase4Rest[6]).opcode == Opcode::VDOT &&
           decodeV2(phase4Rest[6]).func == FUNC_T1,
           "assembler encodes VDOT.t1");
    auto vpackIw = decodeV2(phase4Rest[9]);
    expect(vpackIw.opcode == Opcode::VPACK &&
           vpackIw.rs2 == FUNC_T20 && vpackIw.func == FUNC_T10,
           "assembler encodes VPACK source/dest suffix pair");
    auto vblendIw = decodeV2(phase4Rest[12]);
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

    TritWord27 r4Word = VersionedInstructionCodec::encodeR4(
        Opcode::TWCMP, R5, R1, R2, R3, FUNC_T20,
        IsaEncodingVersion::V2);
    auto r4Iw = decodeV2(r4Word);
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
    expect(decodeV2(phase2[0]).opcode == Opcode::TWCMP &&
           decodeV2(phase2[0]).r4_layout &&
           decodeV2(phase2[0]).func == FUNC_T20,
           "assembler encodes TWCMP.t20 R4");
    expect(decodeV2(phase2[1]).opcode == Opcode::TCLAMP &&
           decodeV2(phase2[1]).r4_layout,
           "assembler encodes TCLAMP.t20 R4");
    expect(decodeV2(phase2[2]).opcode == Opcode::TMOD, "assembler encodes TMOD");
    expect(decodeV2(phase2[5]).opcode == Opcode::TMAC &&
           decodeV2(phase2[5]).rs1 == R1,
           "assembler encodes TMAC source-only shape");
    expect(decodeV2(phase2[8]).opcode == Opcode::CALLR &&
           decodeV2(phase2[8]).rs1 == R11,
           "assembler encodes CALLR register target");
    expect(decodeV2(phase2[10]).opcode == Opcode::SYSCALL &&
           decodeV2(phase2[10]).imm == 1,
           "assembler encodes SYSCALL service id");
    expect(decodeV2(phase2[12]).opcode == Opcode::VSUM &&
           decodeV2(phase2[12]).rd == 13 &&
           decodeV2(phase2[12]).rs1 == 0,
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
    expect(decodeV2(phase3[0]).opcode == Opcode::CSRR &&
           decodeV2(phase3[0]).rd == R1 &&
           decodeV2(phase3[0]).imm == CSR_CAUSE,
           "assembler encodes CSRR rd, csr");
    expect(decodeV2(phase3[1]).opcode == Opcode::CSRW &&
           decodeV2(phase3[1]).rd == R1 &&
           decodeV2(phase3[1]).imm == CSR_TVEC,
           "assembler encodes CSRW csr, rs");
    expect(decodeV2(phase3[2]).opcode == Opcode::CSRRW &&
           decodeV2(phase3[2]).rd == R2 &&
           decodeV2(phase3[2]).rs1 == R3 &&
           decodeV2(phase3[2]).rs2 == CSR_SCRATCH,
           "assembler encodes CSRRW rd, csr, rs");
    expect(decodeV2(phase3[3]).opcode == Opcode::ERET,
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
    auto tldr = decodeV2(atomics[0]);
    auto tstr = decodeV2(atomics[1]);
    auto fenceSeq = decodeV2(atomics[2]);
    auto fenceRelaxed = decodeV2(atomics[3]);
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
