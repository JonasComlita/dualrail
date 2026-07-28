#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_os.h"

#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::os;

long long readCsrLong(sandbox::vm::VMState& vm, int csr) {
    sandbox::vm::TernaryValue value;
    if (!vm.readCSR(csr, value)) return -999999;
    return sandbox::vm::ops::toLong(value);
}

bool writeCsrLong(sandbox::vm::VMState& vm, int csr, long long value) {
    return vm.writeCSR(csr, sandbox::vm::ops::fromLong(value));
}

void blockDeviceCsrPersistence(TestContext& ctx) {
    BlockDevice device(6);
    std::vector<long long> block(BLOCK_WORDS, 0);
    block[0] = 17;
    block[BLOCK_WORDS - 1] = -5;
    ctx.check(device.writeBlock(2, block).ok(), "direct block write succeeds");
    ctx.check(device.dirty(2), "direct block write marks block dirty");
    std::vector<long long> readback;
    ctx.check(device.readBlock(2, readback).ok(), "direct block read succeeds");
    ctx.check(readback == block, "direct block read returns exact payload");
    ctx.equal(device.readBlock(99, readback).detail, ERR_INVALID,
              "out-of-range block read is rejected");
    ctx.check(device.serialize() == device.serialize(),
              "block device serialization is deterministic");

    sandbox::vm::VMState vm(64, 512);
    ctx.check(readCsrLong(vm, sandbox::isa::CSR_BLOCK_COUNT) >= 141,
              "VM exposes default persistent block count");
    ctx.equal(readCsrLong(vm, sandbox::isa::CSR_BLOCK_WORDS),
              static_cast<long long>(sandbox::vm::STORAGE_BLOCK_WORDS),
              "VM block words match ternary page size");
    for (int i = 0; i < sandbox::vm::STORAGE_BLOCK_WORDS; ++i) {
        ctx.check(vm.dmem.store(200 + i, sandbox::vm::ops::fromLong(900 + i)) ==
                      sandbox::vm::MemFaultCode::OK,
                  "VM block write seed stores");
    }
    ctx.check(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_INDEX, 5),
              "block index CSR writes");
    ctx.check(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_ADDR, 200),
              "block address CSR writes");
    ctx.check(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_CMD, 2),
              "block write command accepts");
    ctx.equal(readCsrLong(vm, sandbox::isa::CSR_BLOCK_STATUS), 1LL,
              "block write command succeeds");
    ctx.check(vm.block_dirty[5], "VM block write marks dirty state");

    for (int i = 0; i < sandbox::vm::STORAGE_BLOCK_WORDS; ++i) {
        (void)vm.dmem.store(260 + i, sandbox::vm::ops::fromLong(0));
    }
    ctx.check(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_ADDR, 260),
              "block read address CSR writes");
    ctx.check(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_CMD, 1),
              "block read command accepts");
    const auto [loaded, fault] = vm.dmem.load(260 + 8);
    ctx.check(fault == sandbox::vm::MemFaultCode::OK &&
                  sandbox::vm::ops::toLong(loaded) == 908,
              "block read command transfers words into DMEM");

    const std::vector<long long> image = vm.blockImage();
    sandbox::vm::VMState rebooted(64, 512);
    ctx.check(rebooted.loadBlockImage(image), "VM block image reloads");
    ctx.check(writeCsrLong(rebooted, sandbox::isa::CSR_BLOCK_INDEX, 5),
              "reboot block index CSR writes");
    ctx.check(writeCsrLong(rebooted, sandbox::isa::CSR_BLOCK_ADDR, 300),
              "reboot block address CSR writes");
    ctx.check(writeCsrLong(rebooted, sandbox::isa::CSR_BLOCK_CMD, 1),
              "reboot block read command accepts");
    const auto [reloaded, reload_fault] = rebooted.dmem.load(300);
    ctx.check(reload_fault == sandbox::vm::MemFaultCode::OK &&
                  sandbox::vm::ops::toLong(reloaded) == 900,
              "VM block image survives reboot");
}

void tinyFsFormatMountReaddirStat(TestContext& ctx) {
    BlockDevice device(96);
    TinyFileSystem fs;
    ctx.check(fs.format(device).ok(), "filesystem formats disk");
    ctx.check(fs.mount(device).ok(), "filesystem mounts formatted disk");
    ctx.check(fs.createFile("/bin", InodeKind::Directory).ok(),
              "directory creates");
    ctx.check(fs.createFile("/bin/app", InodeKind::Executable, true).ok(),
              "executable inode creates");

    const std::vector<long long> app = {1, 2, 3, 4, 5};
    ctx.check(fs.writeFile("/bin/app", app).ok(), "file write succeeds");
    std::vector<long long> out;
    ctx.check(fs.readFile("/bin/app", out).ok(), "file read succeeds");
    ctx.check(out == app, "file read returns stored words");

    std::vector<DirectoryEntry> entries;
    ctx.check(fs.readdir("/bin", entries).ok(), "readdir succeeds");
    bool saw_app = false;
    for (const DirectoryEntry& entry : entries) {
        saw_app = saw_app || entry.name == "app";
    }
    ctx.check(saw_app, "directory lists child executable");

    FileStat stat;
    ctx.check(fs.stat("/bin/app", stat).ok(), "stat succeeds");
    ctx.check(stat.kind == InodeKind::Executable && stat.size_words == 5,
              "stat reports executable metadata");
    ctx.equal(fs.createFile("/bin/app").detail, ERR_EXISTS,
              "duplicate path creation fails");
    ctx.equal(fs.lookup("/missing").detail, ERR_NOT_FOUND,
              "missing path lookup fails");
}

void tinyFsLargeExtentReboot(TestContext& ctx) {
    BlockDevice device(128);
    TinyFileSystem fs;
    ctx.check(fs.format(device).ok(), "large extent filesystem formats");
    ctx.check(fs.mount(device).ok(), "large extent filesystem mounts");
    ctx.check(fs.createFile("/large", InodeKind::File).ok(),
              "large file inode creates");

    std::vector<long long> large(
        static_cast<std::size_t>(DIRECT_BLOCKS * BLOCK_WORDS + 11), 0);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<long long>(1000 + i);
    }
    ctx.check(fs.writeFile("/large", large).ok(),
              "large file crossing direct blocks writes");
    FileStat stat;
    ctx.check(fs.stat("/large", stat).ok(), "large file stat succeeds");
    ctx.check(stat.direct_blocks == DIRECT_BLOCKS && stat.indirect_block >= 0,
              "large file allocates direct blocks and indirect extent");
    ctx.check(fs.sync().ok(), "large file fsync succeeds");

    const std::vector<long long> image = device.serialize();
    BlockDevice rebooted_device(128);
    ctx.check(rebooted_device.loadSerialized(image).ok(),
              "serialized disk image loads into rebooted device");
    TinyFileSystem rebooted_fs;
    ctx.check(rebooted_fs.mount(rebooted_device).ok(),
              "filesystem remounts after reboot");
    std::vector<long long> out;
    ctx.check(rebooted_fs.readFile("/large", out).ok(),
              "large file reads after reboot");
    ctx.check(out == large, "large file words persist across reboot");
}

void tinyFsChainedIndirectExtentReboot(TestContext& ctx) {
    BlockDevice device(192);
    TinyFileSystem fs;
    ctx.check(fs.format(device).ok(), "chained extent filesystem formats");
    ctx.check(fs.mount(device).ok(), "chained extent filesystem mounts");
    ctx.check(fs.createFile("/huge", InodeKind::File).ok(),
              "huge file inode creates");

    const int old_one_index_capacity_words =
        (DIRECT_BLOCKS + BLOCK_WORDS) * BLOCK_WORDS;
    std::vector<long long> huge(
        static_cast<std::size_t>(old_one_index_capacity_words + 5 * BLOCK_WORDS + 3),
        0);
    for (std::size_t i = 0; i < huge.size(); ++i) {
        huge[i] = static_cast<long long>((i * 37) % 19683 - 9841);
    }
    ctx.check(static_cast<int>(huge.size()) > old_one_index_capacity_words,
              "huge fixture exceeds the old one-indirect-block capacity");
    ctx.check(fs.writeFile("/huge", huge).ok(),
              "huge file crossing chained indirect blocks writes");
    FileStat stat;
    ctx.check(fs.stat("/huge", stat).ok(), "huge file stat succeeds");
    ctx.check(stat.direct_blocks == DIRECT_BLOCKS && stat.indirect_block >= 0,
              "huge file allocates direct blocks and an indirect chain");
    ctx.check(fs.checkConsistency().ok(),
              "huge file filesystem is consistent before reboot");
    ctx.check(fs.sync().ok(), "huge file fsync succeeds");

    const std::vector<long long> image = device.serialize();
    BlockDevice rebooted_device(192);
    ctx.check(rebooted_device.loadSerialized(image).ok(),
              "chained extent disk image loads into rebooted device");
    TinyFileSystem rebooted_fs;
    ctx.check(rebooted_fs.mount(rebooted_device).ok(),
              "chained extent filesystem remounts after reboot");
    ctx.check(rebooted_fs.checkConsistency().ok(),
              "chained extent filesystem is consistent after reboot");
    std::vector<long long> out;
    ctx.check(rebooted_fs.readFile("/huge", out).ok(),
              "huge file reads after reboot");
    ctx.check(out == huge, "huge file words persist across chained indirect reboot");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"vfs.block_device.csr_persistence", "vfs.persistence_contract",
         blockDeviceCsrPersistence},
        {"vfs.tinyfs.format_mount_readdir_stat", "vfs.persistence_contract",
         tinyFsFormatMountReaddirStat},
        {"vfs.tinyfs.large_extent_reboot", "vfs.persistence_contract",
         tinyFsLargeExtentReboot},
        {"vfs.tinyfs.chained_indirect_extent_reboot", "vfs.persistence_contract",
         tinyFsChainedIndirectExtentReboot},
    };
    return tests_next::runCases("next_vfs_persistence", cases);
}
