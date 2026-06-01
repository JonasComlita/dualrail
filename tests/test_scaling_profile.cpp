#include "ternary_os.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

long long readCsrLong(sandbox::vm::VMState& vm, int csr) {
    sandbox::vm::TernaryValue value;
    if (!vm.readCSR(csr, value)) return -999999;
    return sandbox::vm::ops::toLong(value);
}

bool writeCsrLong(sandbox::vm::VMState& vm, int csr, long long value) {
    return vm.writeCSR(csr, sandbox::vm::ops::fromLong(value));
}

void testProductionProfileGeometryAndSparseMemory() {
    std::cout << "[1] Production profile geometry and sparse VM memory\n";
    using namespace sandbox;

    vm::ProductionProfile profile = vm::ProductionProfile::minimum();
    expect(profile.cores == 2, "production profile exposes two cores");
    expect(profile.ram_bytes == 4LL * 1024LL * 1024LL * 1024LL,
           "production profile targets 4 GiB RAM");
    expect(profile.disk_bytes == 64LL * 1024LL * 1024LL * 1024LL,
           "production profile targets 64 GiB disk");
    expect(profile.hardware_page_words == vm::MMU_PAGE_WORDS,
           "production profile keeps 27-word hardware pages");
    expect(profile.cluster_words == 4096 && vm::clustersForWords(4097) == 2,
           "production profile exposes 4 Kiword OS clusters");
    expect(profile.max_processes >= 100 && profile.max_files >= 1000 &&
               profile.max_windows >= 100,
           "production profile raises process, file, and window limits");

    vm::VMState machine(profile);
    expect(machine.coreCount() == 2, "production VM configures two cores");
    expect(machine.dmem.isSparse() && machine.imem.isSparse(),
           "production VM uses sparse IMEM and DMEM backing");
    expect(machine.dmem.size() == profile.ram_words, "production DMEM reports full RAM words");
    expect(readCsrLong(machine, sandbox::isa::CSR_BLOCK_COUNT) == profile.disk_blocks,
           "production VM exposes the full sparse disk geometry");

    const int high_dmem = profile.ram_words - 17;
    expect(machine.dmem.store(high_dmem, vm::ops::fromLong(123456)) == vm::MemFaultCode::OK,
           "sparse DMEM stores near the production RAM limit");
    auto [loaded, load_fault] = machine.dmem.load(high_dmem);
    expect(load_fault == vm::MemFaultCode::OK && vm::ops::toLong(loaded) == 123456,
           "sparse DMEM loads high-address payloads");
    expect(machine.dmem.allocatedPages() <= 2,
           "sparse DMEM does not allocate untouched production RAM pages");

    sandbox::isa::TritWord27 marker{};
    marker.bits = 777;
    const int high_imem = profile.instruction_words - 9;
    expect(machine.imem.write(high_imem, marker) == vm::MemFaultCode::OK,
           "sparse IMEM stores near the production instruction limit");
    auto [fetched, fetch_fault] = machine.imem.fetch(high_imem);
    expect(fetch_fault == vm::MemFaultCode::OK && fetched.bits == marker.bits,
           "sparse IMEM fetches high-address instructions");
}

void testSparseFileBackedDisk() {
    std::cout << "[2] 64 GiB sparse file-backed VM disk\n";
    using namespace sandbox;

    const std::string path = "build\\scaling_sparse_disk_compact.img";
    std::remove(path.c_str());

    vm::ProductionProfile profile = vm::ProductionProfile::minimum();
    vm::VMState writer(profile);
    expect(writer.attachBlockBackingFile(path), "production VM attaches sparse disk image file");
    for (int i = 0; i < vm::MMU_PAGE_WORDS; ++i) {
        expect(writer.dmem.store(2048 + i, vm::ops::fromLong(9000 + i)) == vm::MemFaultCode::OK,
               "disk write seed stores into sparse DMEM");
    }
    const int last_block = profile.disk_blocks - 1;
    expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_INDEX, last_block), "last disk block CSR writes");
    expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_ADDR, 2048), "disk source address CSR writes");
    expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_CMD, 2), "last sparse disk block writes");
    expect(readCsrLong(writer, sandbox::isa::CSR_BLOCK_STATUS) == 1,
           "sparse file-backed disk write succeeds");
    expect(writer.allocatedDiskBlocks() == 1 && writer.block_dirty[last_block],
           "sparse disk tracks only the touched high block");

    vm::VMState reader(profile);
    expect(reader.attachBlockBackingFile(path), "rebooted VM attaches sparse disk image file");
    expect(writeCsrLong(reader, sandbox::isa::CSR_BLOCK_INDEX, last_block), "reboot block index writes");
    expect(writeCsrLong(reader, sandbox::isa::CSR_BLOCK_ADDR, 4096), "reboot block destination writes");
    expect(writeCsrLong(reader, sandbox::isa::CSR_BLOCK_CMD, 1), "reboot sparse disk block reads");
    auto [reloaded, reload_fault] = reader.dmem.load(4096 + 26);
    expect(reload_fault == vm::MemFaultCode::OK && vm::ops::toLong(reloaded) == 9026,
           "sparse file-backed disk preserves high block contents");

    std::remove(path.c_str());
}

void testTwoCoreExecutionAndRunQueues() {
    std::cout << "[3] Two-core execution and load-balanced run queues\n";
    using namespace sandbox;
    using namespace sandbox::vm::assembler;

    auto assembled = assemble(R"(
        .text
        .org 0
        core0:
            mov r13, 11
            halt
        .org 8
        core1:
            mov r13, 22
            halt
    )");
    expect(assembled.success, "two-core test program assembles");
    vm::VMState machine(64, 512);
    expect(machine.imem.loadProgram(assembled.program), "two-core test program loads");
    machine.configureCores(2);
    machine.coreState(0).pc = 0;
    machine.coreState(1).pc = 8;
    const vm::RunResult result = vm::runMultiCore(machine, 16);
    expect(result.halted(), "two-core VM halts both cores");
    expect(vm::ops::toLong(machine.coreState(0).regfile.read(13)) == 11,
           "core 0 keeps an independent register file");
    expect(vm::ops::toLong(machine.coreState(1).regfile.read(13)) == 22,
           "core 1 keeps an independent register file");

    for (int pid = 1; pid <= 100; ++pid) machine.enqueueProcess(pid);
    machine.loadBalanceRunQueues();
    const std::size_t c0 = machine.runQueueDepth(0);
    const std::size_t c1 = machine.runQueueDepth(1);
    expect(c0 + c1 == 100 && (c0 > c1 ? c0 - c1 : c1 - c0) <= 1,
           "global load balancing spreads 100 runnable processes across two cores");
}

void testProductionOsStressLimits() {
    std::cout << "[4] Production OS process, file, and window stress limits\n";
    using namespace sandbox::os;

    ProductionProfile profile = ProductionProfile::minimum();
    OSKernel kernel(profile);
    expect(kernel.boot().ok(), "production-profile OS boots from sparse 64 GiB disk");

    constexpr int kPid = 1;
    for (int i = 0; i < 99; ++i) {
        StatusResult forked = kernel.sysFork(kPid);
        expect(forked.ok(), "production process table admits 100 processes");
    }
    expect(kernel.processCount() == 100, "production process table reaches 100 processes");

    expect(kernel.fs().createFile("/stress", InodeKind::Directory).ok(),
           "stress directory creates");
    for (int i = 0; i < 1000; ++i) {
        std::ostringstream path;
        path << "/stress/file" << i;
        expect(kernel.fs().createFile(path.str()).ok(), "production VFS creates 1000 files");
    }
    std::vector<DirectoryEntry> entries;
    expect(kernel.fs().readdir("/stress", entries).ok(), "stress directory reads");
    expect(static_cast<int>(entries.size()) >= 1002,
           "production VFS tracks 1000 file entries plus dot entries");

    for (int i = 0; i < 100; ++i) {
        expect(kernel.createWindow(kPid, 64, 36).ok(), "production window table admits 100 windows");
    }
    expect(kernel.windowCount() == 100, "production window table reaches 100 windows");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testProductionProfileGeometryAndSparseMemory();
    testSparseFileBackedDisk();
    testTwoCoreExecutionAndRunQueues();
    testProductionOsStressLimits();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " scaling profile test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nScaling profile tests passed\n";
    return EXIT_SUCCESS;
}
