#include "ternary_os.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

long long fileSizeBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.good()) return -1;
    return static_cast<long long>(in.tellg());
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
    const long long initialized_size = fileSizeBytes(path);
    expect(writer.pendingDiskWrites() == 1 && writer.sparseDiskRecordCount() == 0,
           "sparse disk buffers first write before a flush barrier");
    for (int i = 0; i < vm::MMU_PAGE_WORDS; ++i) {
        expect(writer.dmem.store(2048 + i, vm::ops::fromLong(9100 + i)) == vm::MemFaultCode::OK,
               "disk overwrite seed stores into sparse DMEM");
    }
    expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_CMD, 2), "last sparse disk block overwrites");
    expect(readCsrLong(writer, sandbox::isa::CSR_BLOCK_STATUS) == 1,
           "sparse file-backed disk overwrite succeeds");
    expect(writer.pendingDiskWrites() == 1,
           "sparse disk coalesces repeated writes to one pending block");
    expect(fileSizeBytes(path) == initialized_size,
           "coalesced sparse disk write does not append before flush");
    expect(writer.flushBlockBackingFile(), "sparse disk flush persists pending block records");
    expect(writer.pendingDiskWrites() == 0 && writer.sparseDiskRecordCount() == 1,
           "sparse disk flush drains pending writes into one append record");
    vm::VMBlockDeviceStats stats = writer.blockDeviceStats();
    expect(stats.writes == 2 && stats.dirty_flushes == 1,
           "sparse disk metrics count writes and dirty block flushes");
    const long long first_flush_size = fileSizeBytes(path);
    const long long compact_record_bytes =
        static_cast<long long>(sizeof(int) + sizeof(long long) * vm::MMU_PAGE_WORDS);
    const long long compact_header_bytes =
        static_cast<long long>(sizeof(long long) + sizeof(int));
    expect(first_flush_size == compact_header_bytes + compact_record_bytes,
           "sparse disk flush appends one coalesced touched-block record");
    expect(writer.allocatedDiskBlocks() == 1,
           "sparse disk overwrite keeps one live touched block");

    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < vm::MMU_PAGE_WORDS; ++i) {
            expect(writer.dmem.store(2048 + i, vm::ops::fromLong(9200 + round + i)) ==
                       vm::MemFaultCode::OK,
                   "disk compact seed stores into sparse DMEM");
        }
        expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_CMD, 2),
               "sparse disk repeated append write accepts");
        expect(writer.flushBlockBackingFile(), "sparse disk repeated append write flushes");
    }
    expect(writer.sparseDiskRecordCount() == 5,
           "sparse disk append log records flushed overwrites before compaction");
    expect(writer.compactBlockBackingFile(), "sparse disk backing file compacts live records");
    const long long compacted_size = fileSizeBytes(path);
    expect(compacted_size == compact_header_bytes + compact_record_bytes,
           "sparse disk compaction rewrites one live block record");

    vm::VMState reader(profile);
    expect(reader.attachBlockBackingFile(path), "rebooted VM attaches sparse disk image file");
    expect(writeCsrLong(reader, sandbox::isa::CSR_BLOCK_INDEX, last_block), "reboot block index writes");
    expect(writeCsrLong(reader, sandbox::isa::CSR_BLOCK_ADDR, 4096), "reboot block destination writes");
    expect(writeCsrLong(reader, sandbox::isa::CSR_BLOCK_CMD, 1), "reboot sparse disk block reads");
    auto [reloaded, reload_fault] = reader.dmem.load(4096 + 26);
    expect(reload_fault == vm::MemFaultCode::OK && vm::ops::toLong(reloaded) == 9229,
           "sparse file-backed disk preserves high block contents");

    for (int block = 7; block <= 8; ++block) {
        for (int i = 0; i < vm::MMU_PAGE_WORDS; ++i) {
            expect(writer.dmem.store(2048 + i, vm::ops::fromLong(block * 1000 + i)) ==
                       vm::MemFaultCode::OK,
                   "sequential read-ahead seed stores into sparse DMEM");
        }
        expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_INDEX, block),
               "sequential read-ahead block index writes");
        expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_ADDR, 2048),
               "sequential read-ahead source address writes");
        expect(writeCsrLong(writer, sandbox::isa::CSR_BLOCK_CMD, 2),
               "sequential read-ahead seed block writes");
    }
    expect(writer.flushBlockBackingFile(), "sequential read-ahead seed flushes");

    vm::VMState sequentialReader(profile);
    expect(sequentialReader.attachBlockBackingFile(path),
           "sequential read-ahead reader attaches sparse disk image file");
    sequentialReader.resetBlockDeviceStats();
    expect(writeCsrLong(sequentialReader, sandbox::isa::CSR_BLOCK_INDEX, 7),
           "read-ahead first block index writes");
    expect(writeCsrLong(sequentialReader, sandbox::isa::CSR_BLOCK_ADDR, 5000),
           "read-ahead first block destination writes");
    expect(writeCsrLong(sequentialReader, sandbox::isa::CSR_BLOCK_CMD, 1),
           "read-ahead first block reads");
    expect(writeCsrLong(sequentialReader, sandbox::isa::CSR_BLOCK_INDEX, 8),
           "read-ahead second block index writes");
    expect(writeCsrLong(sequentialReader, sandbox::isa::CSR_BLOCK_ADDR, 5100),
           "read-ahead second block destination writes");
    expect(writeCsrLong(sequentialReader, sandbox::isa::CSR_BLOCK_CMD, 1),
           "read-ahead second block reads");
    stats = sequentialReader.blockDeviceStats();
    expect(stats.reads == 2 && stats.misses >= 1 && stats.hits >= 1 && stats.read_ahead >= 1,
           "sparse disk block cache metrics record sequential read-ahead hit");

    {
        std::fstream crash(path, std::ios::binary | std::ios::in | std::ios::out);
        expect(crash.good(), "sparse disk crash simulator opens backing file");
        int declared_records = static_cast<int>(writer.sparseDiskRecordCount()) + 1;
        crash.seekp(static_cast<std::streamoff>(sizeof(long long)), std::ios::beg);
        crash.write(reinterpret_cast<const char*>(&declared_records), sizeof(declared_records));
        crash.seekp(0, std::ios::end);
        const int partial_index = 9;
        const long long partial_word = 123456;
        crash.write(reinterpret_cast<const char*>(&partial_index), sizeof(partial_index));
        crash.write(reinterpret_cast<const char*>(&partial_word), sizeof(partial_word));
    }
    vm::VMState recovered(profile);
    expect(recovered.attachBlockBackingFile(path),
           "sparse disk attach recovers from a truncated append record");
    expect(writeCsrLong(recovered, sandbox::isa::CSR_BLOCK_INDEX, 7),
           "recovered sparse disk block index writes");
    expect(writeCsrLong(recovered, sandbox::isa::CSR_BLOCK_ADDR, 5200),
           "recovered sparse disk destination writes");
    expect(writeCsrLong(recovered, sandbox::isa::CSR_BLOCK_CMD, 1),
           "recovered sparse disk block reads");
    auto [recoveredWord, recoveredFault] = recovered.dmem.load(5200 + 26);
    expect(recoveredFault == vm::MemFaultCode::OK &&
               vm::ops::toLong(recoveredWord) == 7026,
           "sparse disk recovery preserves last complete flushed block contents");

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

    machine.restoreCoreState(0);
    machine.vector_length = 3;
    machine.vregfile.reset(machine.vector_length);
    machine.vector_faults.reset(machine.vector_length);
    machine.vregfile.reg[0].write(0, vm::ops::fromLong(101));
    machine.vector_faults.setLane(1, vm::TrapCode::TRAP_DIV_ZERO);
    machine.accumulator = vm::ops::fromLong(1001);
    machine.mmu_enable = true;
    machine.user_imem_ptbr = 111;
    machine.user_imem_pages = 7;
    machine.user_dmem_ptbr = 222;
    machine.user_dmem_pages = 9;
    machine.syscall_id = 33;
    machine.console_char_mode = true;
    machine.gpu_page = 1;
    machine.block_index = 44;
    machine.block_addr = 55;
    machine.captureCoreState(0);

    machine.restoreCoreState(1);
    machine.vector_length = 4;
    machine.vregfile.reset(machine.vector_length);
    machine.vector_faults.reset(machine.vector_length);
    machine.vregfile.reg[0].write(0, vm::ops::fromLong(202));
    machine.accumulator = vm::ops::fromLong(2002);
    machine.mmu_enable = false;
    machine.user_imem_ptbr = 333;
    machine.user_imem_pages = 11;
    machine.user_dmem_ptbr = 444;
    machine.user_dmem_pages = 13;
    machine.syscall_id = 66;
    machine.console_char_mode = false;
    machine.gpu_page = 0;
    machine.block_index = 77;
    machine.block_addr = 88;
    machine.captureCoreState(1);

    machine.restoreCoreState(0);
    expect(machine.mmu_enable && machine.user_imem_ptbr == 111 &&
               machine.user_dmem_ptbr == 222,
           "core 0 restores independent MMU CSR state");
    expect(machine.vector_length == 3 &&
               vm::ops::toLong(machine.vregfile.reg[0].read(0)) == 101 &&
               machine.vector_faults.any(),
           "core 0 restores vector lanes and lane faults");
    expect(vm::ops::toLong(machine.accumulator) == 1001 &&
               machine.syscall_id == 33 && machine.console_char_mode &&
               machine.gpu_page == 1 && machine.block_index == 44 &&
               machine.block_addr == 55,
           "core 0 restores accumulator, syscall mode, and device CSR staging");

    machine.restoreCoreState(1);
    expect(!machine.mmu_enable && machine.user_imem_ptbr == 333 &&
               machine.user_dmem_ptbr == 444,
           "core 1 restores independent MMU CSR state");
    expect(machine.vector_length == 4 &&
               vm::ops::toLong(machine.vregfile.reg[0].read(0)) == 202 &&
               !machine.vector_faults.any(),
           "core 1 restores vector lanes independently");
    expect(vm::ops::toLong(machine.accumulator) == 2002 &&
               machine.syscall_id == 66 && !machine.console_char_mode &&
               machine.gpu_page == 0 && machine.block_index == 77 &&
               machine.block_addr == 88,
           "core 1 restores accumulator, syscall mode, and device CSR staging");

    for (int pid = 1; pid <= 100; ++pid) machine.enqueueProcess(pid);
    machine.loadBalanceRunQueues();
    const std::size_t c0 = machine.runQueueDepth(0);
    const std::size_t c1 = machine.runQueueDepth(1);
    expect(c0 + c1 == 100 && (c0 > c1 ? c0 - c1 : c1 - c0) <= 1,
           "global load balancing spreads 100 runnable processes across two cores");
}

void testNativeKernelProductionConstants() {
    std::cout << "[4] Native kernel production constants\n";
    using namespace sandbox;
    using namespace sandbox::compiler;

    const std::string kernel = readTextFile("kernel.trit");
    expect(!kernel.empty(), "native kernel source is available");
    const std::string driver = R"(
        fn main() -> t40 {
            var process_max: t40 = PROCESS_MAX;
            var window_max: t40 = WINDOW_MAX;
            var inode_max: t40 = VFS_MAX_INODES;
            var dirent_max: t40 = VFS_MAX_DIRENTS;
            var fb_words: t40 = FB_MAX_WORDS;
            var ipc_max: t40 = IPC_MAX_CHANNELS;
            var net_max: t40 = NET_MAX_SOCKETS;
            var process_dmem_base: t40 = PROCESS_DMEM_PPN_BASE * MMU_PAGE_WORDS;
            var hw_pt_end: t40 = HW_PT_BASE + PROCESS_MAX * HW_PT_WORDS;
            var window_words: t40 = WINDOW_BUFFER_WORDS;
            var required_blocks: t40 = VFS_DISK_REQUIRED_BLOCKS;
            if process_max - 100 < 0 { return -1; }
            if window_max - 100 < 0 { return -2; }
            if inode_max - 2048 != 0 { return -3; }
            if dirent_max - 1000 < 0 { return -4; }
            if fb_words - (1280 * 720 * 3) < 0 { return -5; }
            if ipc_max - 100 < 0 { return -6; }
            if net_max - 100 < 0 { return -7; }
            if process_dmem_base - hw_pt_end <= 0 { return -8; }
            if window_words - (64 * 36 * 3) < 0 { return -9; }
            if required_blocks - 8192 >= 0 { return -10; }
            return 1;
        }
    )";
    CompileResult compiled = compileSource("native_kernel_production_constants.trit",
                                           kernel + "\n" + driver);
    if (!compiled.success) {
        for (const auto& diagnostic : compiled.diagnostics) {
            std::cout << diagnostic.format() << "\n";
        }
    }
    expect(compiled.success, "native kernel production constant probe compiles");
    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "native kernel production constant probe links");
    vm::VMState machine(262144, 1000000);
    if (linked.success) {
        expect(vm::loadAndReset(machine, linked.assembled.program),
               "native kernel production constant probe loads");
        const vm::RunResult result = vm::run(machine, 1000000);
        expect(result.halted(), "native kernel production constant probe halts");
        expect(vm::ops::toLong(machine.regfile.read(13)) == 1,
               "compiled native kernel exposes production-scale limits");
    }
}

void testProductionOsStressLimits() {
    std::cout << "[5] Production OS process, file, and window stress limits\n";
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
    testNativeKernelProductionConstants();
    testProductionOsStressLimits();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " scaling profile test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nScaling profile tests passed\n";
    return EXIT_SUCCESS;
}
