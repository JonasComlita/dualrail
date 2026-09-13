#include "ternary_asm.h"
#include "ternary_vm.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

using namespace sandbox::isa;
namespace assembler = sandbox::vm::assembler;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireRegisterFields(const InstructionWord& decoded,
                           Opcode opcode,
                           int rd,
                           int rs1,
                           int rs2) {
    require(!decoded.malformed, "decoded instruction is malformed");
    require(decoded.opcode == opcode, "decoded opcode mismatch");
    require(decoded.rd == rd, "decoded destination mismatch");
    require(decoded.rs1 == rs1, "decoded source 1 mismatch");
    require(decoded.rs2 == rs2, "decoded source 2 mismatch");
}

void testDirectMap() {
    const auto word = VersionedInstructionCodec::encodeR(
        Opcode::CALLR, 13, R1, R2, FUNC_DEFAULT,
        IsaEncodingVersion::V2);
    require(decodeUnsignedField(word, FIELD_OP_LSB, FIELD_OP_W) == 27,
            "CALLR must use direct opcode 27");
    requireRegisterFields(
        VersionedInstructionCodec::decode(word, IsaEncodingVersion::V2),
        Opcode::CALLR, 13, R1, R2);

}

void testTinvLowering() {
    const auto word = VersionedInstructionCodec::encodeR(
        Opcode::TINV, R3, R4, R0_ZERO, FUNC_DEFAULT,
        IsaEncodingVersion::V2);
    require(decodeUnsignedField(word, FIELD_OP_LSB, FIELD_OP_W) == 10,
            "TINV must lower to the NEG direct encoding");
    require(VersionedInstructionCodec::decode(
                word, IsaEncodingVersion::V2).opcode == Opcode::NEG,
            "TINV wire encoding must decode as NEG");
}

void testStandardExtension() {
    const auto word = VersionedInstructionCodec::encodeR(
        Opcode::TLADD, R3, R4, R5, FUNC_DEFAULT,
        IsaEncodingVersion::V2);
    require(decodeUnsignedField(word, FIELD_OP_LSB, FIELD_OP_W) == 80,
            "extension must use opcode 80");
    require(decodeUnsignedField(word, 0, 10) == 27,
            "TLADD must retain v1 opcode 27 as its selector");
    requireRegisterFields(
        VersionedInstructionCodec::decode(word, IsaEncodingVersion::V2),
        Opcode::TLADD, R3, R4, R5);
}

void testR4Extension() {
    const auto word = VersionedInstructionCodec::encodeR4(
        Opcode::TWCMP, R3, R4, R5, R6, FUNC_T20,
        IsaEncodingVersion::V2);
    require(decodeUnsignedField(word, 0, 4) == 59,
            "TWCMP must retain selector 59");
    const auto decoded =
        VersionedInstructionCodec::decode(word, IsaEncodingVersion::V2);
    require(decoded.opcode == Opcode::TWCMP && decoded.r4_layout,
            "TWCMP must decode with the R4 layout");
    require(decoded.rd == R3 && decoded.rs1 == R4 &&
                decoded.rs2 == R5 && decoded.rs3 == R6,
            "TWCMP operands did not round trip");
    require(decoded.func == FUNC_T20, "TWCMP width did not round trip");
}

void testR5Extension() {
    const auto word = VersionedInstructionCodec::encodeR5(
        Opcode::VSEL, R6, R3, R1, R2, R4, FUNC_T20,
        IsaEncodingVersion::V2);
    require(decodeUnsignedField(word, 0, 4) == 38,
            "VSEL must retain selector 38");
    require(decodeUnsignedField(word, 4, 3) == FUNC_T20,
            "VSEL function field must occupy trits [6:4]");
    const auto decoded =
        VersionedInstructionCodec::decode(word, IsaEncodingVersion::V2);
    require(decoded.opcode == Opcode::VSEL && decoded.r5_layout,
            "VSEL must decode with the R5 layout");
    require(decoded.rd == R6 && decoded.rcond == R3 &&
                decoded.rneg == R1 && decoded.rzero == R2 &&
                decoded.rpos == R4 && decoded.func == FUNC_T20,
            "VSEL operands did not round trip");
}

void testVectorMemoryExtension() {
    const auto word = VersionedInstructionCodec::encodeVectorMemory(
        Opcode::VLOAD, R7, R8, -121, FUNC_T10,
        IsaEncodingVersion::V2);
    require(decodeUnsignedField(word, 9, 4) == 39,
            "VLOAD must retain selector 39");
    const auto decoded =
        VersionedInstructionCodec::decode(word, IsaEncodingVersion::V2);
    require(decoded.opcode == Opcode::VLOAD &&
                decoded.rd == R7 && decoded.rs1 == R8 &&
                decoded.imm == -121 && decoded.func == FUNC_T10,
            "VLOAD fields did not round trip");
}

void testReservedEncodingsTrapAtDecodeBoundary() {
    auto reserved_gap =
        InstructionWord::encodeSemanticR(Opcode::ADD, R1, R2, R3);
    encodeUnsignedField(
        reserved_gap, FIELD_OP_LSB, FIELD_OP_W, 15);
    require(VersionedInstructionCodec::decode(
                reserved_gap, IsaEncodingVersion::V2).opcode ==
                Opcode::RESERVED,
            "unassigned direct opcode 15 must decode as illegal");

    auto reserved_direct =
        InstructionWord::encodeSemanticR(Opcode::ADD, R1, R2, R3);
    encodeUnsignedField(
        reserved_direct, FIELD_OP_LSB, FIELD_OP_W, 38);
    require(VersionedInstructionCodec::decode(
                reserved_direct, IsaEncodingVersion::V2).opcode ==
                Opcode::RESERVED,
            "reserved direct opcode must decode as illegal");

    auto reserved_selector =
        InstructionWord::encodeSemanticR(Opcode::ADD, R1, R2, R3);
    encodeUnsignedField(
        reserved_selector, FIELD_OP_LSB, FIELD_OP_W, 80);
    encodeUnsignedField(reserved_selector, 0, 10, 61);
    require(VersionedInstructionCodec::decode(
                reserved_selector, IsaEncodingVersion::V2).opcode ==
                Opcode::RESERVED,
            "unassigned extension selector must decode as illegal");
}

void testAssemblerDirectivesAndFeatures() {
    const auto no_directive = assembler::assembleV2("add r1, r2, r3\n");
    require(!no_directive.success,
            "strict assembly must require an explicit ISA directive");

    const auto base = assembler::assembleV2(
        ".isa 2\nadd r1, r2, r3\n");
    require(base.success && base.isa_version == IsaEncodingVersion::V2,
            "base ISA v2 source must assemble");
    require(VersionedInstructionCodec::decode(
                base.program.front(), IsaEncodingVersion::V2).opcode ==
                Opcode::ADD,
            "assembler must emit the v2 direct map");

    const auto missing_feature = assembler::assembleV2(
        ".isa 2\ntladd.l20 r1, r2, r3\n");
    require(!missing_feature.success,
            "optional instruction must require a feature declaration");

    const auto lane = assembler::assembleV2(
        ".isa 2\n.require lane\ntladd.l20 r1, r2, r3\n");
    require(lane.success, "declared lane instruction must assemble");
    require(decodeUnsignedField(
                lane.program.front(), FIELD_OP_LSB, FIELD_OP_W) == 80 &&
                decodeUnsignedField(lane.program.front(), 0, 10) == 27,
            "assembler must emit TLADD through EXT selector 27");
    require((lane.required_features &
             featureBit(sandbox::architecture::v2::FEATURE_LANE)) != 0,
            "assembler must report its required feature word");

    const auto waiting = assembler::assembleV2(
        ".isa 2\n.require wait\nwait\n");
    require(waiting.success,
            "WAIT feature source must assemble: " +
                (waiting.errors.empty() ? std::string{} : waiting.errors.front().format()));
    require(decodeUnsignedField(
                waiting.program.front(), FIELD_OP_LSB, FIELD_OP_W) == 37,
            "WAIT must use v2 direct opcode 37");
    require(disassemble(
                waiting.program.front(), IsaEncodingVersion::V2) == "WAIT",
            "version-aware disassembly must decode the v2 direct map");
    require(assembler::listing(waiting).find("WAIT") != std::string::npos,
            "assembly listings must retain their ISA version");

    const auto legacy = assembler::assemble(
        ".isa 1\nadd r1, r2, r3\n",
        assembler::AssemblyOptions{IsaEncodingVersion::V2, true});
    require(!legacy.success,
            "the production assembler must reject explicit ISA v1");
    require(!legacy.errors.empty() &&
                legacy.errors.front().message.find("Only .isa 2") !=
                    std::string::npos,
            "ISA v1 rejection must direct users to offline migration");
}

void testVmVersionDiscoveryAndWaiting() {
    const auto assembled = assembler::assembleV2(
        ".isa 2\n.require wait\nwait\nhalt\n");
    require(assembled.success, "v2 WAIT program must assemble");

    sandbox::vm::VMState vm(64, 64);
    require(assembler::loadAndReset(vm, assembled),
            "VM must accept supported v2 feature requirements");
    sandbox::vm::TernaryValue csr;
    require(vm.readCSR(CSR_ISA_VERSION, csr) &&
                sandbox::vm::ops::toLong(csr) == 2,
            "ISA version CSR mismatch");
    require(vm.readCSR(CSR_MMU_BASE_PAGE_WORDS, csr) &&
                sandbox::vm::ops::toLong(csr) == 729,
            "base-page CSR mismatch");
    require(vm.readCSR(CSR_MMU_SUPERPAGE_WORDS, csr) &&
                sandbox::vm::ops::toLong(csr) == 19683,
            "superpage CSR mismatch");
    require(!vm.writeCSR(CSR_ISA_VERSION,
                         sandbox::vm::ops::fromLong(1)),
            "architecture discovery CSRs must be read-only");

    require(sandbox::vm::step(vm) == sandbox::vm::VMStatus::WAITING &&
                vm.pc == 1,
            "WAIT must retire once and enter architectural idle");
    require(sandbox::vm::step(vm) == sandbox::vm::VMStatus::WAITING &&
                vm.pc == 1,
            "WAITING VM must not busy-execute instructions");
    vm.resumeFromEvent();
    require(sandbox::vm::step(vm) == sandbox::vm::VMStatus::HALTED,
            "event resume must continue at the instruction after WAIT");

    sandbox::vm::VMState unsupported(64, 64);
    unsupported.supported_features &=
        ~featureBit(sandbox::architecture::v2::FEATURE_WAIT);
    require(!assembler::loadAndReset(unsupported, assembled),
            "loader must reject an unavailable required feature");
}

void testPairedT50RegisterAndMemoryContract() {
    const auto assembled = assembler::assembleV2(
        ".isa 2\n"
        ".require wide_t50\n"
        "store.t50 r3, r1, 0\n"
        "load.t50 r5, r1, 0\n"
        "add.t50 r7, r5, r5\n"
        "halt\n");
    require(assembled.success, "paired T50 memory source must assemble");
    require(assembled.program.size() == 7,
            "T50 store/load pseudos must lower to two/three base instructions");

    sandbox::vm::VMState vm(64, 64);
    require(assembler::loadAndReset(vm, assembled),
            "paired T50 memory program must load");
    vm.regfile.write(R1, sandbox::vm::ops::fromLong(10));

    std::array<int8_t, 50> wide_trits{};
    wide_trits[0] = 1;
    wide_trits[40] = 1;
    const sandbox::LongTriple wide =
        sandbox::LongTriple::pack(wide_trits);
    vm.regfile.write(R3, sandbox::vm::TernaryValue::fromLongTriple(wide));

    const auto result = sandbox::vm::run(vm, 32);
    require(result.halted(), "paired T50 memory program must halt");
    const auto [low_word, low_fault] = vm.dmem.load(10);
    const auto [high_word, high_fault] = vm.dmem.load(11);
    require(low_fault == sandbox::vm::MemFaultCode::OK &&
                high_fault == sandbox::vm::MemFaultCode::OK &&
                low_word.mode == sandbox::TernaryMode::T40 &&
                high_word.mode == sandbox::TernaryMode::T40,
            "each architectural memory location must contain one physical T40 word");
    require(high_word.asTriple().unpack()[0] == 1,
            "T50 high ten trits must be stored in the second word");

    const auto wide_value =
        sandbox::vm::TernaryValue::fromLongTriple(wide);
    const auto expected = sandbox::vm::exec::addValue(
        wide_value, wide_value, sandbox::TernaryMode::T50);
    require(vm.regfile.readView(R7, sandbox::TernaryMode::T50)
                .asLongTripleRaw() ==
                expected.asLongTripleRaw(),
            "T50 arithmetic must reconstruct both physical register words");
}

void testNumericPteTlbsAndShootdown() {
    using namespace sandbox::vm;
    PageTableEntry entry;
    entry.ppn = 8;
    entry.present = true;
    entry.user = true;
    entry.read = true;
    entry.write = true;
    const TernaryValue encoded = encodePageTableEntry(entry);
    PageTableEntry decoded;
    require(decodePageTableEntryV2(encoded, decoded) &&
                decoded.ppn == 8 && decoded.present && decoded.user &&
                decoded.read && decoded.write,
            "numeric PTE v2 fields must round trip");
    require(!decodePageTableEntryV2(ops::fromLong(2), decoded),
            "negative/dual flag digit encodings must be invalid");

    const std::uint64_t mmu_features =
        featureBit(sandbox::architecture::v2::FEATURE_BASE_V2) |
        featureBit(sandbox::architecture::v2::FEATURE_MMU);
    VMState vm(64, 400000);
    require(vm.configureArchitecture(
                IsaEncodingVersion::V2, mmu_features),
            "v2 MMU feature profile must configure");
    vm.privilege = PrivilegeMode::User;
    vm.mmu_enable = true;
    vm.user_dmem_ptbr = 300000;
    vm.user_dmem_pages = 100;
    for (int vpn = 0; vpn < 100; ++vpn) {
        PageTableEntry page;
        page.ppn = vpn;
        page.present = true;
        page.user = true;
        page.read = true;
        page.write = true;
        require(vm.dmem.store(
                    vm.user_dmem_ptbr + vpn,
                    encodePageTableEntry(page)) == MemFaultCode::OK,
                "test page table must fit in memory");
    }

    int physical = -1;
    int cause = 0;
    for (int vpn = 0; vpn < 100; ++vpn) {
        require(vm.translateLoadAddress(
                    vpn * MMU_PAGE_WORDS, physical, cause),
                "v2 base-page warmup translation must succeed");
    }
    vm.tlb_stats.reset();
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const int vpn = iteration % 100;
        require(vm.translateLoadAddress(
                    vpn * MMU_PAGE_WORDS + 3, physical, cause),
                "sequential warm translation must succeed");
    }
    require(vm.tlb_stats.data_l1_hits + vm.tlb_stats.l2_hits >= 9900,
            "sequential warm workload must achieve at least 99% TLB hits");

    vm.tlb_stats.reset();
    std::uint32_t random = 1;
    for (int iteration = 0; iteration < 1000; ++iteration) {
        random = random * 1664525u + 1013904223u;
        const int vpn = static_cast<int>(random % 100u);
        require(vm.translateLoadAddress(
                    vpn * MMU_PAGE_WORDS + 7, physical, cause) &&
                    physical == vpn * MMU_PAGE_WORDS + 7,
                "random in-range translation must preserve page offset");
    }
    const std::uint64_t combined_hits =
        vm.tlb_stats.data_l1_hits + vm.tlb_stats.l2_hits;
    require(combined_hits >= 950,
            "warm random L2 workload must achieve at least 95% hits");

    PageTableEntry updated;
    auto [updated_word, updated_fault] = vm.dmem.load(vm.user_dmem_ptbr);
    require(updated_fault == MemFaultCode::OK &&
                decodePageTableEntryV2(updated_word, updated) &&
                updated.accessed && !updated.dirty,
            "load walk must set accessed without dirty");
    vm.invalidateAllTlbs(true);
    const bool store_translated =
        vm.translateStoreAddress(0, physical, cause);
    require(store_translated,
            "write-enabled page must translate for store");
    std::tie(updated_word, updated_fault) =
        vm.dmem.load(vm.user_dmem_ptbr);
    require(updated_fault == MemFaultCode::OK &&
                decodePageTableEntryV2(updated_word, updated) &&
                updated.accessed && updated.dirty,
            "store walk must set accessed and dirty");

    PageTableEntry denied = updated;
    denied.read = false;
    denied.write = false;
    require(vm.dmem.store(vm.user_dmem_ptbr,
                          encodePageTableEntry(denied)) == MemFaultCode::OK,
            "permission test PTE must store");
    vm.invalidateAllTlbs(true);
    require(!vm.translateLoadAddress(0, physical, cause) &&
                cause == OS_CAUSE_PROTECTION_FAULT,
            "PTE permission failure must route protection before access");

    PageTableEntry asid_page = updated;
    asid_page.ppn = 0;
    asid_page.read = true;
    asid_page.write = true;
    require(vm.dmem.store(vm.user_dmem_ptbr,
                          encodePageTableEntry(asid_page)) == MemFaultCode::OK,
            "ASID test PTE must store");
    vm.setCurrentAsid(1);
    vm.invalidateAllTlbs(true);
    require(vm.translateLoadAddress(5, physical, cause) && physical == 5,
            "first ASID mapping must populate");
    asid_page.ppn = 1;
    require(vm.dmem.store(vm.user_dmem_ptbr,
                          encodePageTableEntry(asid_page)) == MemFaultCode::OK,
            "ASID reuse replacement PTE must store");
    vm.setCurrentAsid(2);
    require(vm.translateLoadAddress(5, physical, cause) &&
                physical == MMU_PAGE_WORDS + 5,
            "unrelated ASID must not reuse another ASID's translation");
    vm.setCurrentAsid(1);
    require(vm.translateLoadAddress(5, physical, cause) && physical == 5,
            "ASID switch must retain the old ASID entry until shootdown");
    vm.invalidateTlb(-1, 1, 1);
    require(vm.translateLoadAddress(5, physical, cause) &&
                physical == MMU_PAGE_WORDS + 5,
            "ASID reuse shootdown must expose the replacement PTE");

    vm.invalidateAllTlbs(true);
    PageTableEntry superpage;
    superpage.ppn = 0;
    superpage.present = true;
    superpage.user = true;
    superpage.read = true;
    superpage.superpage = true;
    vm.user_dmem_ptbr = 350000;
    vm.user_dmem_pages = 27;
    require(vm.dmem.store(
                vm.user_dmem_ptbr,
                encodePageTableEntry(superpage)) == MemFaultCode::OK,
            "aligned superpage PTE must store");
    require(vm.translateLoadAddress(1000, physical, cause) &&
                physical == 1000,
            "superpage walk must translate through aligned base PPN");
    require(vm.translateLoadAddress(1001, physical, cause) &&
                vm.tlb_stats.superpage_hits > 0,
            "superpage translation must populate the TLB");
    PageTableEntry unaligned_super = superpage;
    unaligned_super.ppn = 1;
    require(encodePageTableEntry(unaligned_super).isInvalid(),
            "unaligned superpage PPN must be rejected");

    const auto tlbinv = assembler::assembleV2(
        ".isa 2\n.require mmu\ntlbinv r1, r2, r3\nhalt\n");
    require(tlbinv.success,
            "extended TLBINV address/ASID/scope form must assemble");
    require(decodeUnsignedField(
                tlbinv.program.front(), FIELD_OP_LSB, FIELD_OP_W) == 80 &&
                decodeUnsignedField(tlbinv.program.front(), 0, 10) == 80,
            "TLBINV must use EXT selector 80");
    VMState shootdown(64, 64);
    require(assembler::loadAndReset(shootdown, tlbinv),
            "TLBINV test program must load");
    TlbEntry cached;
    cached.valid = true;
    cached.asid = 0;
    cached.vpn = 0;
    shootdown.data_tlb[0] = cached;
    shootdown.regfile.write(R1, ops::fromLong(0));
    shootdown.regfile.write(R2, ops::fromLong(0));
    shootdown.regfile.write(R3, ops::fromLong(3));
    require(sandbox::vm::step(shootdown) == VMStatus::RUNNING &&
                !shootdown.data_tlb[0].valid &&
                shootdown.tlb_stats.shootdowns == 1,
            "TLBINV must invalidate the selected scope precisely");
}


void testExecutableHeaderV2() {
    sandbox::vm::ExecutableImageHeaderV2 header;
    header.required_features =
        featureBit(sandbox::architecture::v2::FEATURE_BASE_V2) |
        featureBit(sandbox::architecture::v2::FEATURE_WAIT);
    header.entry_pc = 9;
    header.text_words = 81;
    header.data_words = 27;
    header.stack_words = 18;
    header.flags = 5;

    const auto encoded = sandbox::vm::encodeExecutableHeaderV2(header);
    require(encoded.size() ==
                sandbox::architecture::v2::EXECUTABLE_HEADER_WORDS,
            "executable header v2 must contain exactly 15 words");

    sandbox::vm::ExecutableImageHeaderV2 decoded;
    require(sandbox::vm::decodeExecutableHeaderV2(encoded, 0, decoded),
            "valid executable header v2 must decode");
    require(decoded.entry_pc == 9 && decoded.text_words == 81 &&
                decoded.data_words == 27 && decoded.stack_words == 18 &&
                decoded.required_features == header.required_features,
            "executable header v2 fields did not round trip");

    auto torn = encoded;
    torn[sandbox::vm::EXEC_V2_FLAGS] = sandbox::vm::ops::fromLong(6);
    require(!sandbox::vm::decodeExecutableHeaderV2(torn, 0, decoded),
            "header checksum must reject modified metadata");

    const long long feature_word =
        featureWordNumeric(header.required_features);
    const std::string source =
        ".isa 2\n.require wait\n.text\nwait\nhalt\n.data\n" 
        "app: .execheader2 0, 2, 0, 18, " +
        std::to_string(feature_word) + ", 2, 0\n";
    const auto assembled = assembler::assembleV2(source);
    require(assembled.success &&
                assembled.executable_headers_v2.count("app") == 1,
            "assembler must emit and collect executable header v2");
    require(assembled.executable_headers_v2.at("app").text_words == 2,
            "assembler header v2 text count mismatch");

    const auto mismatched = assembler::assembleV2(
        ".isa 2\n.require wait\n.text\nwait\nhalt\n.data\n"
        "app: .execheader2 0, 2, 0, 18, 1, 2, 0\n");
    require(!mismatched.success,
            "executable feature metadata must match .require exactly");
}

void testGeneratedArchitectureConstants() {
    namespace contract = sandbox::architecture::v2;
    require(contract::INSTRUCTION_TRITS == 27, "instruction width drift");
    require(contract::SCALAR_WORD_TRITS == 40, "scalar width drift");
    require(contract::WIDE_SCALAR_TRITS == 50, "wide width drift");
    require(contract::BASE_PAGE_WORDS == 729, "base page drift");
    require(contract::SUPERPAGE_WORDS == 19683, "superpage drift");
    require(contract::FUNCTION_ARG_FIRST == 13 &&
                contract::FUNCTION_ARG_LAST == 18,
            "function argument ABI drift");
    require(contract::STACK_ALIGNMENT_WORDS == 9,
            "stack alignment drift");
    require(contract::CSR_DESCRIPTORS.size() == 53 &&
                contract::CSR_MAX_ID == 52,
            "complete CSR contract drift");
}

void testCsrAccessPolicy() {
    using sandbox::isa::PrivilegeMode;
    require(canReadCSR(CSR_STATUS, PrivilegeMode::User) &&
                canReadCSR(CSR_TVEC, PrivilegeMode::User) &&
                canReadCSR(CSR_SCRATCH, PrivilegeMode::User),
            "user-readable compatibility CSRs must remain readable");
    require(!canReadCSR(CSR_EPC, PrivilegeMode::User) &&
                !canReadCSR(CSR_USER_DMEM_PTBR, PrivilegeMode::User),
            "trap frames and page-table roots must be kernel-readable only");
    require(canWriteCSR(CSR_CONSOLE_OUT, PrivilegeMode::User) &&
                canWriteCSR(CSR_GPU_CMD, PrivilegeMode::User),
            "user console and graphics controls must remain writable");
    require(!canWriteCSR(CSR_BLOCK_CMD, PrivilegeMode::User) &&
                !canWriteCSR(CSR_POWER_CONTROL, PrivilegeMode::User) &&
                !canWriteCSR(CSR_MOUSE_X, PrivilegeMode::User),
            "user mode must not control block, power, or host input state");

    sandbox::vm::VMState vm(4, 64);
    vm.reset();
    vm.trap_routing_enabled = true;
    vm.tvec = 1;
    vm.privilege = PrivilegeMode::User;
    vm.regfile.write(R1, sandbox::vm::ops::fromLong(1));
    require(vm.imem.write(
                0, VersionedInstructionCodec::encodeI(
                       Opcode::CSRW, R1, R0_ZERO, CSR_POWER_CONTROL,
                       IsaEncodingVersion::V2)) == sandbox::vm::MemFaultCode::OK,
            "power-control denial fixture must encode");
    require(sandbox::vm::step(vm) == sandbox::vm::VMStatus::RUNNING &&
                vm.pc == 1 && vm.cause == OS_CAUSE_PROTECTION_FAULT &&
                vm.power_control == 0,
            "user power-control write must route without a side effect");
}

void testNestedTrapAndLegacyTrapRecord() {
    using namespace sandbox::vm;
    VMState nested(4, 64);
    nested.reset();
    nested.trap_routing_enabled = true;
    nested.tvec = 1;
    nested.privilege = PrivilegeMode::User;
    nested.regfile.write(R1, ops::fromLong(1));
    nested.regfile.write(R2, ops::fromLong(1));
    nested.regfile.write(R3, ops::fromLong(0));
    require(nested.imem.write(
                0, VersionedInstructionCodec::encodeI(
                       Opcode::CSRW, R1, R0_ZERO, CSR_BLOCK_CMD,
                       IsaEncodingVersion::V2)) == MemFaultCode::OK &&
                nested.imem.write(
                    1, VersionedInstructionCodec::encodeR(
                           Opcode::DIV, R4, R2, R3, FUNC_T40,
                           IsaEncodingVersion::V2)) == MemFaultCode::OK,
            "nested-trap fixture must encode");
    require(step(nested) == VMStatus::RUNNING && nested.trap_active &&
                nested.epc == 0 &&
                nested.cause == OS_CAUSE_PROTECTION_FAULT &&
                nested.previous_privilege == PrivilegeMode::User,
            "first routed trap must capture the user frame");
    require(step(nested) == VMStatus::TRAPPED && nested.pc == 1 &&
                nested.epc == 0 &&
                nested.cause == OS_CAUSE_PROTECTION_FAULT &&
                nested.previous_privilege == PrivilegeMode::User &&
                decodeTrap(nested.trap_reg) == TrapCode::TRAP_DIV_ZERO,
            "nested handler fault must stop without overwriting the first frame");

    VMState event(4, 64);
    event.reset();
    event.trap_routing_enabled = true;
    event.tvec = 2;
    event.privilege = PrivilegeMode::User;
    event.interrupt_enable = true;
    event.trapWithCause(
        TrapCode::TRAP_ILLEGAL_OP, OS_CAUSE_SYSCALL, 0);
    require(event.trap_active && !trapValid(event.trap_reg),
            "routed syscall must not masquerade as an illegal instruction");
    event.epc = 1;
    require(event.returnFromTrap() && !event.trap_active &&
                !trapValid(event.trap_reg) &&
                event.privilege == PrivilegeMode::User && event.pc == 1,
            "ERET must clear the routed trap record and active frame");
    event.trapWithCause(
        TrapCode::TRAP_DIV_ZERO, OS_CAUSE_DIV_ZERO, 1);
    require(event.trap_active && trapValid(event.trap_reg) &&
                decodeTrap(event.trap_reg) == TrapCode::TRAP_DIV_ZERO,
            "routed synchronous fault must expose its legacy fault class");
    event.epc = 3;
    require(event.returnFromTrap() && !trapValid(event.trap_reg) &&
                event.pc == 3,
            "ERET must clear a handled synchronous fault record");
}

}  // namespace

int main() {
    try {
        testDirectMap();
        testTinvLowering();
        testStandardExtension();
        testR4Extension();
        testR5Extension();
        testVectorMemoryExtension();
        testReservedEncodingsTrapAtDecodeBoundary();
        testAssemblerDirectivesAndFeatures();
        testVmVersionDiscoveryAndWaiting();
        testPairedT50RegisterAndMemoryContract();
        testNumericPteTlbsAndShootdown();
        testExecutableHeaderV2();
        testGeneratedArchitectureConstants();
        testCsrAccessPolicy();
        testNestedTrapAndLegacyTrapRecord();
        std::cout << "ISA v2 architecture contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ISA v2 architecture contract test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
