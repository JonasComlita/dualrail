#include "ternary_asm.h"
#include "ternary_vm.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using namespace sandbox;
using namespace sandbox::isa;
namespace assembler = sandbox::vm::assembler;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

long long regLong(const vm::VMState& machine, int reg) {
    return vm::ops::toLong(machine.regfile.read(static_cast<uint8_t>(reg)));
}

void testArtifactContract(const assembler::AssemblyResult& image,
                          const std::string& source) {
    expect(source.find(".isa 2") != std::string::npos,
           "bring-up artifact declares ISA v2");
    expect(source.find(".isa 1") == std::string::npos,
           "bring-up artifact contains no v1 declaration");
    expect(source.find("syscall 1") != std::string::npos &&
               source.find("syscall 47") != std::string::npos,
           "bring-up artifact exercises v2 syscall ids");
    expect(source.find("r13") != std::string::npos &&
               source.find("r14") != std::string::npos &&
               source.find("r15") != std::string::npos,
           "bring-up artifact names v2 status/payload/detail registers");
    expect(source.find("eret") != std::string::npos &&
               source.find("timer_enable") != std::string::npos &&
               source.find("wait") != std::string::npos,
           "bring-up artifact contains ERET, timer, and WAIT paths");

    const auto mmu = featureBit(architecture::v2::FEATURE_MMU);
    const auto wait = featureBit(architecture::v2::FEATURE_WAIT);
    expect(image.isa_version == IsaEncodingVersion::V2,
           "assembled bring-up image records ISA v2");
    expect((image.required_features & (mmu | wait)) == (mmu | wait),
           "assembled bring-up image requires v2 MMU and WAIT");
    expect(image.labels.count("bringup_entry") &&
               image.labels.at("bringup_entry") == 0,
           "bring-up entry is the reset PC");
    expect(image.labels.count("trap_entry") &&
               image.labels.count("timer_entry") &&
               image.labels.count("timer_after"),
           "bring-up image exposes trap and timer entry points");
    expect(image.data_labels.count("page_geometry") &&
               image.data_labels.at("page_geometry") == 0 &&
               image.data.size() >= 2,
           "bring-up image carries numeric page geometry");
    if (image.data.size() >= 2) {
        expect(vm::ops::toLong(image.data[0]) == 729 &&
                   vm::ops::toLong(image.data[1]) == 19683,
               "bring-up image stores 729/19683 page geometry");
    }

    vm::PageTableEntry text_pte;
    vm::PageTableEntry data_pte;
    expect(image.data_labels.count("text_pte") &&
               image.data_labels.count("data_pte"),
           "bring-up image carries text and data PTEs");
    if (image.data_labels.count("text_pte") &&
        image.data_labels.count("data_pte")) {
        expect(vm::decodePageTableEntryV2(
                   image.data[image.data_labels.at("text_pte")], text_pte) &&
                   text_pte.ppn == 1 && text_pte.present && text_pte.user &&
                   text_pte.read && !text_pte.write && text_pte.execute,
               "text PTE uses numeric v2 permissions");
        expect(vm::decodePageTableEntryV2(
                   image.data[image.data_labels.at("data_pte")], data_pte) &&
                   data_pte.ppn == 2 && data_pte.present && data_pte.user &&
                   data_pte.read && data_pte.write && !data_pte.execute,
               "data PTE uses numeric v2 permissions");
    }
}

void testSyscallAndWait(const assembler::AssemblyResult& image) {
    vm::VMState machine(256, 30000);
    const bool loaded = assembler::loadAndReset(machine, image);
    expect(loaded,
           "v2 bring-up image loads into a monitor-sized VM");
    if (!loaded) return;
    machine.trap_routing_enabled = true;

    const auto result = vm::run(machine, 512);
    expect(result.status == vm::VMStatus::WAITING,
           "v2 bring-up reaches architectural WAIT after syscall traps");
    expect(machine.syscall_buffer == "42",
           "v2 r13 syscall argument reaches the kernel console path");
    expect(machine.syscall_id == 47,
           "v2 fsync syscall id survives the trap path");
    expect(regLong(machine, 13) == 1 && regLong(machine, 14) == 0 &&
               regLong(machine, 15) == 0,
           "v2 syscall returns status/payload/detail in r13-r15");
    expect(machine.pc == image.labels.at("after_wait"),
           "WAIT advances to the precise post-WAIT PC");

    machine.resumeFromEvent();
    const auto resumed = vm::run(machine, 32);
    expect(resumed.halted(), "v2 bring-up resumes from WAIT and halts");
    expect(machine.privilege == PrivilegeMode::User,
           "ERET returns the resumed task to user privilege");
}

void testTimerTrap(const assembler::AssemblyResult& image) {
    vm::VMState machine(256, 30000);
    const bool loaded = assembler::loadAndReset(machine, image);
    expect(loaded,
           "v2 bring-up image reloads for timer validation");
    if (!loaded) return;
    machine.trap_routing_enabled = true;
    machine.pc = image.labels.at("timer_entry");

    const auto result = vm::run(machine, 256);
    expect(result.halted(), "v2 timer bring-up takes the trap and halts");
    expect(machine.cause == OS_CAUSE_TIMER_IRQ,
           "timer bring-up records the v2 timer interrupt cause");
    expect(regLong(machine, 10) == 1,
           "timer trap returns through the observable delivery marker");
    expect(!machine.timer_enable && machine.pc == image.labels.at("timer_after"),
           "timer trap disables the timer and returns to its precise EPC");
    expect(machine.privilege == PrivilegeMode::User,
           "timer ERET restores user privilege");
}

}  // namespace

int main() {
    const std::string source = readTextFile("v2_first_silicon_bringup.tasm");
    expect(!source.empty(), "v2 first-silicon bring-up source is present");
    if (source.empty()) return 1;

    const auto image = assembler::assemble(source);
    if (!image.success) {
        for (const auto& error : image.errors) {
            std::cerr << error.format() << '\n';
        }
    }
    expect(image.success, "v2 first-silicon bring-up source assembles");
    if (image.success) {
        testArtifactContract(image, source);
        testSyscallAndWait(image);
        testTimerTrap(image);
    }

    if (failures != 0) {
        std::cerr << failures << " first-silicon bring-up assertion(s) failed\n";
        return 1;
    }
    std::cout << "v2 first-silicon bring-up tests passed\n";
    return 0;
}
