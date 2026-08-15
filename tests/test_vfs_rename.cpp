#include "ternary_compiler.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <fstream>
#include <iostream>
#include <limits>
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

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

void dumpDiagnostics(const sandbox::compiler::CompileResult& compiled) {
    for (const auto& diagnostic : compiled.diagnostics) {
        std::cerr << "  " << diagnostic.format() << "\n";
    }
}

const std::string kRenameDriver = R"TRIT(
fn seed_src(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 0); return addr;
}
fn seed_dst(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 100); kstore(addr + 2, 115);
    kstore(addr + 3, 116); kstore(addr + 4, 0); return addr;
}
fn seed_old(addr: t40) -> t40 {
    seed_src(addr); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_new(addr: t40) -> t40 {
    seed_dst(addr); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_existing(addr: t40) -> t40 {
    seed_dst(addr); kstore(addr + 4, 47); kstore(addr + 5, 101);
    kstore(addr + 6, 120); kstore(addr + 7, 105); kstore(addr + 8, 115);
    kstore(addr + 9, 116); kstore(addr + 10, 105); kstore(addr + 11, 110);
    kstore(addr + 12, 103); kstore(addr + 13, 0); return addr;
}
fn seed_dir(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 0); return addr;
}
fn seed_child(addr: t40) -> t40 {
    seed_dst(addr); kstore(addr + 4, 47); kstore(addr + 5, 101);
    kstore(addr + 6, 120); kstore(addr + 7, 105); kstore(addr + 8, 115);
    kstore(addr + 9, 116); kstore(addr + 10, 0); return addr;
}
fn seed_nested(addr: t40) -> t40 {
    seed_existing(addr); kstore(addr + 13, 47); kstore(addr + 14, 99);
    kstore(addr + 15, 104); kstore(addr + 16, 105); kstore(addr + 17, 108);
    kstore(addr + 18, 100); kstore(addr + 19, 0); return addr;
}
fn seed_root(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 0); return addr;
}
fn seed_retry(addr: t40) -> t40 {
    seed_src(addr); kstore(addr + 4, 47); kstore(addr + 5, 114);
    kstore(addr + 6, 101); kstore(addr + 7, 116); kstore(addr + 8, 114);
    kstore(addr + 9, 121); kstore(addr + 10, 0); return addr;
}

fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var src_dir: t40 = USER_MEM_BASE + 200;
    var dst_dir: t40 = USER_MEM_BASE + 220;
    var old_path: t40 = USER_MEM_BASE;
    var new_path: t40 = USER_MEM_BASE + 32;
    var existing_path: t40 = USER_MEM_BASE + 64;
    var dir_path: t40 = USER_MEM_BASE + 96;
    var child_path: t40 = USER_MEM_BASE + 128;
    var root_path: t40 = USER_MEM_BASE + 160;
    var retry_path: t40 = USER_MEM_BASE + 192;
    var payload: t40 = USER_MEM_BASE + 300;
    var readback: t40 = USER_MEM_BASE + 340;

    seed_src(src_dir); seed_dst(dst_dir); seed_old(old_path);
    if vfs_create(0, src_dir, KIND_DIR) - 1 != 0 { return -2; }
    if vfs_create(0, dst_dir, KIND_DIR) - 2 != 0 { return -3; }
    if vfs_create(0, old_path, KIND_FILE) - 3 != 0 { return -4; }
    var fd: t40 = vfs_open(1, old_path, 2);
    if fd < 0 { return -5; }
    kstore(payload + 0, 11); kstore(payload + 1, 22); kstore(payload + 2, -33);
    var written: t40 = vfs_write(1, fd, payload, 3);
    if written - 3 != 0 { return -600 + written; }
    kstore(fd_addr(fd) + FD_OFFSET, 0);

    var inode_before: t40 = vfs_lookup(0, old_path);
    var source_slot: t40 = vfs_find_child_slot(0, 1, old_path + 5, 5,
                                                 vfs_component_hash(old_path + 5, 5));
    if inode_before - 3 != 0 { return -7; }
    if source_slot < 0 { return -8; }
    var inode_version: t40 = kload(inode_addr(inode_before) + INODE_VERSION);
    var next_inode: t40 = kload(VFS_NEXT_INODE_ADDR);
    var next_dirent: t40 = kload(VFS_NEXT_DIRENT_ADDR);
    var next_extent: t40 = kload(VFS_NEXT_EXTENT_ADDR);
    var next_data: t40 = kload(VFS_NEXT_DATA_ADDR);

    seed_new(new_path);
    if kernel_syscall_dispatch(1, 59, old_path, new_path, 0, 0) - 1 != 0 { return -9; }
    if kload(SYS_STATUS_ADDR) - 1 != 0 { return -10; }
    if kload(SYS_PAYLOAD_ADDR) - 1 != 0 { return -11; }
    if vfs_lookup(0, old_path) - ERR_NOT_FOUND != 0 { return -12; }
    if vfs_lookup(0, new_path) - inode_before != 0 { return -13; }
    if vfs_find_child_slot(0, 2, new_path + 5, 5,
                           vfs_component_hash(new_path + 5, 5)) - source_slot != 0 {
        return -14;
    }
    if kload(VFS_NEXT_INODE_ADDR) - next_inode != 0 { return -15; }
    if kload(VFS_NEXT_DIRENT_ADDR) - next_dirent != 0 { return -16; }
    if kload(VFS_NEXT_EXTENT_ADDR) - next_extent != 0 { return -17; }
    if kload(VFS_NEXT_DATA_ADDR) - next_data != 0 { return -18; }
    if kload(inode_addr(inode_before) + INODE_VERSION) - inode_version != 0 { return -19; }
    if kload(inode_addr(inode_before) + INODE_FLAGS) - 2 != 0 { return -20; }
    if vfs_read(1, fd, readback, 3) - 3 != 0 { return -21; }
    if kload(readback + 0) - 11 != 0 { return -22; }
    if kload(readback + 1) - 22 != 0 { return -23; }
    if kload(readback + 2) + 33 != 0 { return -24; }

    var wal_head: t40 = kload(WAL_HEAD_ADDR);
    var wal_tail: t40 = kload(WAL_TAIL_ADDR);
    var wal_next_lsn: t40 = kload(WAL_NEXT_LSN_ADDR);
    if kernel_syscall_dispatch(1, 59, new_path, new_path, 0, 0) - 1 != 0 { return -25; }
    if kload(WAL_HEAD_ADDR) - wal_head != 0 { return -26; }
    if kload(WAL_TAIL_ADDR) - wal_tail != 0 { return -27; }
    if kload(WAL_NEXT_LSN_ADDR) - wal_next_lsn != 0 { return -28; }

    seed_existing(existing_path); seed_nested(child_path); seed_dir(dir_path);
    if vfs_create(0, existing_path, KIND_FILE) - 4 != 0 { return -29; }
    var replaced_inode: t40 = vfs_lookup(0, existing_path);
    var replaced_fd: t40 = vfs_open(1, existing_path, 2);
    if replaced_fd < 0 { return -30; }
    kstore(payload + 0, 71);
    if vfs_write(1, replaced_fd, payload, 1) - 1 != 0 { return -31; }
    kstore(fd_addr(replaced_fd) + FD_OFFSET, 0);
    var collision_status: t40 = kernel_syscall_dispatch(1, 59, new_path, existing_path, 0, 0);
    if collision_status - 1 != 0 { return -700 + collision_status; }
    if vfs_lookup(0, existing_path) - inode_before != 0 { return -32; }
    if vfs_lookup(0, new_path) - ERR_NOT_FOUND != 0 { return -33; }
    if vfs_read(1, replaced_fd, readback, 1) - 1 != 0 { return -34; }
    if kload(readback) - 71 != 0 { return -35; }
    if kernel_syscall_dispatch(1, 59, existing_path, child_path, 0, 0) + 1 != 0 { return -36; }
    if kload(SYS_DETAIL_ADDR) - ERR_NOT_DIR != 0 { return -37; }
    if vfs_close(1, replaced_fd) - 1 != 0 { return -38; }
    if kload(inode_addr(replaced_inode) + INODE_KIND) - KIND_FREE != 0 { return -39; }
    seed_root(root_path);
    if kernel_syscall_dispatch(1, 59, root_path, child_path, 0, 0) + 1 != 0 { return -40; }
    if kload(SYS_DETAIL_ADDR) - ERR_IS_DIR != 0 { return -41; }
    if kernel_syscall_dispatch(1, 59, -1, new_path, 0, 0) + 1 != 0 { return -42; }
    if kload(SYS_DETAIL_ADDR) - ERR_BAD_PTR != 0 { return -43; }
    if vfs_lookup(0, existing_path) - inode_before != 0 { return -44; }
    if kernel_syscall_dispatch(1, 59, existing_path, child_path, 0, 0) + 1 != 0 { return -45; }
    if kload(SYS_DETAIL_ADDR) - ERR_NOT_DIR != 0 { return -46; }

    seed_retry(retry_path);
    var before_txid: t40 = kload(WAL_NEXT_TXID_ADDR);
    var before_parent: t40 = kload(dirent_addr(source_slot) + DIRENT_PARENT);
    kstore(WAL_HEAD_ADDR, 1); kstore(WAL_TAIL_ADDR, 2);
    if kernel_syscall_dispatch(1, 59, existing_path, retry_path, 0, 0) + 1 != 0 { return -47; }
    if kload(SYS_DETAIL_ADDR) - ERR_NO_SPACE != 0 { return -48; }
    if kload(WAL_NEXT_TXID_ADDR) - before_txid != 0 { return -49; }
    if kload(dirent_addr(source_slot) + DIRENT_PARENT) - before_parent != 0 { return -50; }
    if vfs_lookup(0, existing_path) - inode_before != 0 { return -51; }
    return 1;
}
)TRIT";

const std::string kRenameDirectoryDriver = R"TRIT(
fn seed_container(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 99); kstore(addr + 2, 111);
    kstore(addr + 3, 110); kstore(addr + 4, 116); kstore(addr + 5, 97);
    kstore(addr + 6, 105); kstore(addr + 7, 110); kstore(addr + 8, 101);
    kstore(addr + 9, 114); kstore(addr + 10, 0); return addr;
}
fn seed_source(addr: t40) -> t40 {
    seed_container(addr); kstore(addr + 10, 47); kstore(addr + 11, 115);
    kstore(addr + 12, 114); kstore(addr + 13, 99); kstore(addr + 14, 0);
    return addr;
}
fn seed_source_file(addr: t40) -> t40 {
    seed_source(addr); kstore(addr + 14, 47); kstore(addr + 15, 102);
    kstore(addr + 16, 105); kstore(addr + 17, 108); kstore(addr + 18, 101);
    kstore(addr + 19, 0); return addr;
}
fn seed_target(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 116); kstore(addr + 2, 97);
    kstore(addr + 3, 114); kstore(addr + 4, 103); kstore(addr + 5, 101);
    kstore(addr + 6, 116); kstore(addr + 7, 0); return addr;
}
fn seed_busy(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 98); kstore(addr + 2, 117);
    kstore(addr + 3, 115); kstore(addr + 4, 121); kstore(addr + 5, 0);
    return addr;
}
fn seed_busy_child(addr: t40) -> t40 {
    seed_busy(addr); kstore(addr + 5, 47); kstore(addr + 6, 99);
    kstore(addr + 7, 104); kstore(addr + 8, 105); kstore(addr + 9, 108);
    kstore(addr + 10, 100); kstore(addr + 11, 0); return addr;
}
fn seed_cycle(addr: t40) -> t40 {
    seed_target(addr); kstore(addr + 7, 47); kstore(addr + 8, 115);
    kstore(addr + 9, 117); kstore(addr + 10, 98); kstore(addr + 11, 0);
    return addr;
}
fn seed_target_file(addr: t40) -> t40 {
    seed_target(addr); kstore(addr + 7, 47); kstore(addr + 8, 102);
    kstore(addr + 9, 105); kstore(addr + 10, 108); kstore(addr + 11, 101);
    kstore(addr + 12, 0); return addr;
}
fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var container: t40 = USER_MEM_BASE + 500;
    var source: t40 = USER_MEM_BASE + 540;
    var source_file: t40 = USER_MEM_BASE + 580;
    var target: t40 = USER_MEM_BASE + 620;
    var busy: t40 = USER_MEM_BASE + 660;
    var busy_child: t40 = USER_MEM_BASE + 700;
    var cycle: t40 = USER_MEM_BASE + 740;
    var target_file: t40 = USER_MEM_BASE + 780;
    seed_container(container); seed_source(source); seed_source_file(source_file);
    seed_target(target); seed_busy(busy); seed_busy_child(busy_child);
    seed_cycle(cycle); seed_target_file(target_file);
    if vfs_create(0, container, KIND_DIR) - 1 != 0 { return -2; }
    var source_inode: t40 = vfs_create(0, source, KIND_DIR);
    if source_inode - 2 != 0 { return -3; }
    if vfs_create(0, source_file, KIND_FILE) <= 0 { return -4; }
    var target_inode: t40 = vfs_create(0, target, KIND_DIR);
    if target_inode <= 0 { return -5; }
    if kernel_syscall_dispatch(1, 59, source, target, 0, 0) - 1 != 0 {
        return -6;
    }
    if vfs_lookup(0, target) - source_inode != 0 { return -7; }
    if vfs_lookup(0, source_file) - ERR_NOT_FOUND != 0 { return -8; }
    if vfs_lookup(0, target_file) <= 0 { return -9; }
    if kload(inode_addr(source_inode) + INODE_FLAGS) != 0 { return -10; }
    if kload(inode_addr(target_inode) + INODE_KIND) - KIND_FREE != 0 { return -11; }

    var busy_inode: t40 = vfs_create(0, busy, KIND_DIR);
    if busy_inode <= 0 { return -12; }
    if vfs_create(0, busy_child, KIND_FILE) <= 0 { return -13; }
    if kernel_syscall_dispatch(1, 59, target, busy, 0, 0) + 1 != 0 { return -14; }
    if kload(SYS_DETAIL_ADDR) - ERR_EXISTS != 0 { return -15; }
    if vfs_lookup(0, target) - source_inode != 0 { return -16; }
    if vfs_lookup(0, busy) - busy_inode != 0 { return -17; }

    if kernel_syscall_dispatch(1, 59, target, cycle, 0, 0) + 1 != 0 { return -18; }
    if kload(SYS_DETAIL_ADDR) - ERR_INVALID != 0 { return -19; }
    if vfs_lookup(0, target) - source_inode != 0 { return -20; }
    return 1;
}
)TRIT";

const std::string kRenameRecoveryWriter = R"TRIT(
fn seed_src(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 0); return addr;
}
fn seed_dst(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 100); kstore(addr + 2, 115);
    kstore(addr + 3, 116); kstore(addr + 4, 0); return addr;
}
fn seed_old(addr: t40) -> t40 {
    seed_src(addr); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_new(addr: t40) -> t40 {
    seed_dst(addr); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var src_dir: t40 = USER_MEM_BASE + 200;
    var dst_dir: t40 = USER_MEM_BASE + 220;
    var old_path: t40 = USER_MEM_BASE;
    var new_path: t40 = USER_MEM_BASE + 32;
    seed_src(src_dir); seed_dst(dst_dir); seed_old(old_path); seed_new(new_path);
    if vfs_create(0, src_dir, KIND_DIR) - 1 != 0 { return -2; }
    if vfs_create(0, dst_dir, KIND_DIR) - 2 != 0 { return -3; }
    if vfs_create(0, old_path, KIND_FILE) - 3 != 0 { return -4; }
    if kernel_syscall_dispatch(1, 59, old_path, new_path, 0, 0) - 1 != 0 {
        return -500 + kload(SYS_DETAIL_ADDR);
    }
    if wal_sync_to_disk() - 1 != 0 { return -6; }
    return 1;
}
)TRIT";

const std::string kRenameRecoveryReader = R"TRIT(
fn seed_old(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_new(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 100); kstore(addr + 2, 115);
    kstore(addr + 3, 116); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var old_path: t40 = USER_MEM_BASE;
    var new_path: t40 = USER_MEM_BASE + 32;
    seed_old(old_path); seed_new(new_path);
    if vfs_lookup(0, old_path) - ERR_NOT_FOUND != 0 { return -2; }
    if vfs_lookup(0, new_path) <= 0 { return -3; }
    return 1;
}
)TRIT";

// The same reader image is run against both a fully committed disk and a
// copy with one rename data record torn. The marker distinguishes the expected
// committed (new name) and torn (old name) outcomes without recompiling the
// kernel twice.
const std::string kRenameRecoveryReaderBoth = R"TRIT(
fn seed_old(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_new(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 100); kstore(addr + 2, 115);
    kstore(addr + 3, 116); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var old_path: t40 = USER_MEM_BASE;
    var new_path: t40 = USER_MEM_BASE + 32;
    seed_old(old_path); seed_new(new_path);
    var old_inode: t40 = vfs_lookup(0, old_path);
    var new_inode: t40 = vfs_lookup(0, new_path);
    if old_inode >= 0 {
        if new_inode - ERR_NOT_FOUND != 0 { return -2; }
        kstore(36000, 2);
        return 1;
    }
    if old_inode - ERR_NOT_FOUND != 0 { return -3; }
    if new_inode <= 0 { return -4; }
    kstore(36000, 1);
    return 1;
}
)TRIT";

const std::string kRenameTornWriter = R"TRIT(
fn seed_src(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 0); return addr;
}
fn seed_dst(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 100); kstore(addr + 2, 115);
    kstore(addr + 3, 116); kstore(addr + 4, 0); return addr;
}
fn seed_old(addr: t40) -> t40 {
    seed_src(addr); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_new(addr: t40) -> t40 {
    seed_dst(addr); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var src_dir: t40 = USER_MEM_BASE + 200;
    var dst_dir: t40 = USER_MEM_BASE + 220;
    var old_path: t40 = USER_MEM_BASE;
    var new_path: t40 = USER_MEM_BASE + 32;
    seed_src(src_dir); seed_dst(dst_dir); seed_old(old_path); seed_new(new_path);
    if vfs_create(0, src_dir, KIND_DIR) - 1 != 0 { return -2; }
    if vfs_create(0, dst_dir, KIND_DIR) - 2 != 0 { return -3; }
    if vfs_create(0, old_path, KIND_FILE) - 3 != 0 { return -4; }
    if log_checkpoint() < 0 { return -5; }
    if kernel_syscall_dispatch(1, 59, old_path, new_path, 0, 0) - 1 != 0 {
        return -600 + kload(SYS_DETAIL_ADDR);
    }
    if wal_sync_to_disk() - 1 != 0 { return -7; }
    return 1;
}
)TRIT";

const std::string kRenameTornReader = R"TRIT(
fn seed_old(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 115); kstore(addr + 2, 114);
    kstore(addr + 3, 99); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn seed_new(addr: t40) -> t40 {
    kstore(addr + 0, 47); kstore(addr + 1, 100); kstore(addr + 2, 115);
    kstore(addr + 3, 116); kstore(addr + 4, 47); kstore(addr + 5, 97);
    kstore(addr + 6, 108); kstore(addr + 7, 112); kstore(addr + 8, 104);
    kstore(addr + 9, 97); kstore(addr + 10, 0); return addr;
}
fn main() -> t40 {
    if kernel_init() - 1 != 0 { return -1; }
    var old_path: t40 = USER_MEM_BASE;
    var new_path: t40 = USER_MEM_BASE + 32;
    seed_old(old_path); seed_new(new_path);
    if vfs_lookup(0, old_path) <= 0 { return -2; }
    if vfs_lookup(0, new_path) - ERR_NOT_FOUND != 0 { return -3; }
    return 1;
}
)TRIT";

bool buildDriver(const std::string& kernel, const std::string& name,
                 const std::string& driver,
                 sandbox::compiler::LinkResult& linked_out) {
    using namespace sandbox::compiler;
    CompileResult compiled = compileSource(name, kernel + "\n" + driver);
    if (!compiled.success) {
        dumpDiagnostics(compiled);
        expect(false, name + " compiles");
        return false;
    }
    linked_out = linkModules({compiled.object});
    expect(linked_out.success, name + " links");
    return linked_out.success;
}

bool runLinked(const sandbox::compiler::LinkResult& linked,
               const std::string& name,
               const std::vector<long long>* disk_in = nullptr,
               std::vector<long long>* disk_out = nullptr,
               long long* marker_out = nullptr) {
    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    if (disk_in) {
        expect(vm.loadBlockImage(*disk_in), name + " loads disk image");
    } else {
        vm.resetBlockDevice(8192);
    }
    expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), name + " loads");
    const auto result = sandbox::vm::run(vm, 50000000);
    expect(result.halted(), name + " halts");
    const long long return_value = regLong(vm, 13);
    if (return_value != 1) {
        std::cout << "DEBUG " << name << " returned " << return_value
                  << " status=" << static_cast<int>(result.status) << "\n";
    }
    expect(return_value == 1, name + " assertions pass");
    if (disk_out) *disk_out = vm.blockImage();
    if (marker_out) {
        auto [marker, fault] = vm.dmem.load(36000);
        (void)fault;
        *marker_out = sandbox::vm::ops::toLong(marker);
    }
    return result.halted() && return_value == 1;
}

bool runDriver(const std::string& kernel, const std::string& name,
               const std::string& driver,
               const std::vector<long long>* disk_in = nullptr,
               std::vector<long long>* disk_out = nullptr) {
    sandbox::compiler::LinkResult linked;
    if (!buildDriver(kernel, name, driver, linked)) return false;
    return runLinked(linked, name, disk_in, disk_out);
}

void testRenameKernelContract() {
    std::cout << "[1] Atomic VFS rename kernel contract\n";
    const std::string kernel = readTextFile("kernel.trit");
    expect(!kernel.empty(), "kernel.trit is available");
    runDriver(kernel, "vfs_rename_contract.trit", kRenameDriver);
}

void testRenameDirectorySemantics() {
    std::cout << "[2] VFS directory rename and replacement semantics\n";
    const std::string kernel = readTextFile("kernel.trit");
    runDriver(kernel, "vfs_rename_directory_contract.trit",
              kRenameDirectoryDriver);
}

void testRenameRecovery() {
    std::cout << "[3] Atomic VFS rename WAL recovery\n";
    const std::string kernel = readTextFile("kernel.trit");
    std::vector<long long> committed_disk;
    if (!runDriver(kernel, "vfs_rename_recovery_writer.trit",
                   kRenameRecoveryWriter, nullptr, &committed_disk)) {
        return;
    }
    sandbox::compiler::LinkResult reader_linked;
    if (!buildDriver(kernel, "vfs_rename_recovery_reader.trit",
                     kRenameRecoveryReaderBoth, reader_linked)) {
        return;
    }
    long long marker = 0;
    expect(runLinked(reader_linked, "vfs_rename_recovery_reader_committed.trit",
                     &committed_disk, nullptr, &marker),
           "committed rename reboots and replays");
    expect(marker == 1, "committed WAL exposes the new name after reboot");

    std::vector<long long> torn_disk = committed_disk;
    constexpr int kRecordWords = 27;
    constexpr int kWalMagic = 170171;
    constexpr int kWalVersion = 2;
    constexpr int kWalData = 1;
    constexpr int kLsnWord = 3;
    constexpr int kTypeWord = 6;
    constexpr int kChecksumWord = 11;
    std::size_t torn_record = std::numeric_limits<std::size_t>::max();
    long long newest_lsn = -1;
    for (int slot = 0; slot < sandbox::os::NATIVE_WAL_DISK_RECORD_BLOCKS; ++slot) {
        const std::size_t base =
            (static_cast<std::size_t>(sandbox::os::NATIVE_WAL_DISK_RECORD_BLOCK) +
             static_cast<std::size_t>(slot)) * kRecordWords;
        if (base + kRecordWords > torn_disk.size()) continue;
        if (torn_disk[base + 0] != kWalMagic ||
            torn_disk[base + 1] != kWalVersion ||
            torn_disk[base + kTypeWord] != kWalData) {
            continue;
        }
        if (torn_disk[base + kLsnWord] > newest_lsn) {
            newest_lsn = torn_disk[base + kLsnWord];
            torn_record = base;
        }
    }
    expect(torn_record != std::numeric_limits<std::size_t>::max(),
           "torn test finds a persisted rename WAL data record");
    if (torn_record != std::numeric_limits<std::size_t>::max()) {
        torn_disk[torn_record + kChecksumWord] = 0;
        expect(runLinked(reader_linked, "vfs_rename_recovery_reader_torn.trit",
                         &torn_disk, nullptr, &marker),
               "torn rename WAL reboots without a mixed state");
        expect(marker == 2, "torn/incomplete WAL leaves the old name visible");
    }
}

void testRenameCompilerSurface() {
    std::cout << "[4] Atomic VFS rename compiler and SDK surface\n";
    using namespace sandbox::compiler;
    expect(runtime::sys_rename == 59, "compiler runtime reserves syscall 59");
    expect(sandbox::vm::SYSCALL_RENAME == 59, "VM ABI reserves syscall 59");
    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string source = R"TRIT(
fn main() -> t40 {
    return os_rename(10000, 10032);
}
)TRIT";
    CompileResult compiled = compileSource("os_rename_wrapper.trit", sdk + "\n" + source);
    if (!compiled.success) dumpDiagnostics(compiled);
    expect(compiled.success, "os_rename SDK wrapper compiles");
    if (compiled.success) {
        expect(compiled.assembly.find("syscall 59") != std::string::npos,
               "os_rename lowers to syscall 59");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testRenameKernelContract();
    testRenameDirectorySemantics();
    testRenameRecovery();
    testRenameCompilerSurface();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "VFS atomic rename tests passed\n";
    return 0;
}
