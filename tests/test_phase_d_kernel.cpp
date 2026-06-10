#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
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

long long wordAt(const sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    (void)fault;
    return sandbox::vm::ops::toLong(value);
}

void dumpDiagnostics(const sandbox::compiler::CompileResult& compiled) {
    for (const auto& diag : compiled.diagnostics) {
        std::cerr << "  " << diag.format() << "\n";
    }
}

void testPhaseDKernelEndToEnd() {
    std::cout << "[1] Phase D native kernel end-to-end\n";
    using namespace sandbox::compiler;

    const std::string kernel = readTextFile("kernel.trit");
    expect(!kernel.empty(), "kernel.trit is present");

    const std::string driver = R"(
        fn expect_eq(actual: t40, expected: t40, ok: t40) -> t40 {
            kstore(35003, kload(35003) + 1);
            if actual - expected == 0 {
                return ok;
            }
            if kload(35000) == 0 {
                kstore(35000, 1);
                kstore(35001, actual);
                kstore(35002, expected);
                kstore(35004, kload(35003));
            }
            return 0;
        }

        fn expect_pos(actual: t40, ok: t40) -> t40 {
            kstore(35003, kload(35003) + 1);
            if actual > 0 {
                return ok;
            }
            if kload(35000) == 0 {
                kstore(35000, 1);
                kstore(35001, actual);
                kstore(35002, 1);
                kstore(35004, kload(35003));
            }
            return 0;
        }

        fn seed_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 97);
            kstore(addr + 2, 108);
            kstore(addr + 3, 112);
            kstore(addr + 4, 104);
            kstore(addr + 5, 97);
            kstore(addr + 6, 0);
            return addr;
        }

        fn seed_root(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 0);
            return addr;
        }

        fn seed_dot_alpha(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 46);
            kstore(addr + 2, 47);
            kstore(addr + 3, 97);
            kstore(addr + 4, 108);
            kstore(addr + 5, 112);
            kstore(addr + 6, 104);
            kstore(addr + 7, 97);
            kstore(addr + 8, 0);
            return addr;
        }

        fn seed_dir(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 100);
            kstore(addr + 2, 105);
            kstore(addr + 3, 114);
            kstore(addr + 4, 0);
            return addr;
        }

        fn seed_dir_parent_alpha(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 100);
            kstore(addr + 2, 105);
            kstore(addr + 3, 114);
            kstore(addr + 4, 47);
            kstore(addr + 5, 46);
            kstore(addr + 6, 46);
            kstore(addr + 7, 47);
            kstore(addr + 8, 97);
            kstore(addr + 9, 108);
            kstore(addr + 10, 112);
            kstore(addr + 11, 104);
            kstore(addr + 12, 97);
            kstore(addr + 13, 0);
            return addr;
        }

        fn seed_huge(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 104);
            kstore(addr + 2, 117);
            kstore(addr + 3, 103);
            kstore(addr + 4, 101);
            kstore(addr + 5, 0);
            return addr;
        }

        fn seed_app(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 97);
            kstore(addr + 2, 112);
            kstore(addr + 3, 112);
            kstore(addr + 4, 0);
            return addr;
        }

        fn seed_payload(addr: t40, count: t40) -> t40 {
            var i: t40 = 0;
            while count - i > 0 {
                var value: t40 = i * 3 - 40;
                if i - 10 == 0 {
                    value = 0;
                }
                if i - 11 == 0 {
                    value = -7;
                }
                kstore(addr + i, value);
                i = i + 1;
            }
            return count;
        }

        fn seed_exec_desc(addr: t40, text_ppn: t40, text_pages: t40, data_pages: t40, stack_words: t40) -> t40 {
            kstore(addr + EXEC_HEADER_MAGIC, EXEC_MAGIC);
            kstore(addr + EXEC_HEADER_VERSION, EXEC_VERSION_V1);
            kstore(addr + EXEC_HEADER_ABI_VERSION, EXEC_ABI_VERSION_V1);
            kstore(addr + EXEC_HEADER_ENTRY_PC, 0);
            kstore(addr + EXEC_HEADER_TEXT_PAGES, text_pages);
            kstore(addr + EXEC_HEADER_DATA_PAGES, data_pages);
            kstore(addr + EXEC_HEADER_STACK_WORDS, stack_words);
            kstore(addr + EXEC_HEADER_SYSCALL_ABI_VERSION, EXEC_SYSCALL_ABI_VERSION_V1);
            kstore(addr + EXEC_HEADER_FLAGS, 0);
            kstore(addr + EXEC_DESC_TEXT_PPN, text_ppn);
            return EXEC_DESC_WORDS;
        }

        fn verify_payload(src: t40, dst: t40, count: t40, ok: t40) -> t40 {
            var i: t40 = 0;
            while count - i > 0 {
                ok = expect_eq(kload(dst + i), kload(src + i), ok);
                i = i + 1;
            }
            if ok > 0 {
                return 1;
            }
            return -1;
        }

        fn main() -> t40 {
            var ok: t40 = 1;
            ok = expect_eq(kernel_init(), 1, ok);
            ok = expect_eq(bootstrap_bitmap_is_sized(), 1, ok);

            var before_allocs: t40 = bootstrap_allocated_count();
            var ppn: t40 = alloc_bootstrap_page();
            ok = expect_eq(bootstrap_allocated_count(), before_allocs + 1, ok);
            ok = expect_eq(bootstrap_is_allocated(ppn), 1, ok);

            ok = expect_eq(page_is_evictable(ppn), 1, ok);
            page_pin(ppn);
            ok = expect_eq(page_is_evictable(ppn), 0, ok);
            ok = expect_eq(cow_can_remap(ppn), 0, ok);
            page_unpin(ppn);
            ok = expect_eq(cow_can_remap(ppn), 1, ok);

            page_set_version_state(ppn, PTR_STATE_NULL, 7);
            ok = expect_eq(page_version_state(kload(page_header_field(ppn, PH_VERSION))), PTR_STATE_NULL, ok);
            page_set_version_state(ppn, PTR_STATE_UNKNOWN, 7);
            ok = expect_eq(page_version_state(kload(page_header_field(ppn, PH_VERSION))), PTR_STATE_UNKNOWN, ok);
            page_set_version_state(ppn, PTR_STATE_VALID, 7);
            ok = expect_eq(page_version_state(kload(page_header_field(ppn, PH_VERSION))), PTR_STATE_VALID, ok);

            kstore(1200, 41);
            var tx_abort: t40 = log_begin();
            log_write(tx_abort, 1200, 41, 55);
            log_abort(tx_abort);
            ok = expect_eq(kload(1200), 41, ok);

            var tx_commit: t40 = log_begin();
            log_write(tx_commit, 1200, 41, 77);
            log_commit(tx_commit);
            ok = expect_eq(kload(1200), 77, ok);
            log_checkpoint();

            migrate_bootstrap_allocations_step(4);
            migrate_bootstrap_allocations();
            ok = expect_eq(rel_allocation_state(ppn), 1, ok);
            ok = expect_eq(kload(REL_MIGRATION_DONE_ADDR), 1, ok);
            ok = expect_eq(alloc_bootstrap_page(), ERR_INVALID, ok);

            macro_stage_thread(9);
            macro_stage_thread(10);
            var epoch: t40 = scheduler_epoch();
            ok = expect_eq(macro_publish(), 2, ok);
            ok = expect_eq(scheduler_epoch(), epoch + 1, ok);
            ok = expect_eq(tier1_dequeue(), 9, ok);
            ok = expect_eq(tier1_dequeue(), 10, ok);

            ok = expect_eq(process_state(0), PROC_RUNNABLE, ok);
            quota_set_limit(0, 3);
            ok = expect_eq(quota_remaining(0), 3, ok);
            process_create(1, 2, 0, 1, 0, 120);
            process_wait(1, 44);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            ok = expect_eq(process_wake_channel(44), 1, ok);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(macro_reconcile_processes(), 2, ok);
            ok = expect_eq(macro_publish(), 2, ok);
            var staged_a: t40 = tier1_dequeue();
            var staged_b: t40 = tier1_dequeue();
            ok = expect_eq(staged_a + staged_b, 3, ok);
            ok = expect_eq(staged_a * staged_b, 2, ok);
            ok = expect_eq(quota_remaining(0), 1, ok);

            var futex_addr: t40 = USER_MEM_BASE + 10;
            kstore(futex_addr, 7);
            kstore(120 + TASK_CONTEXT_EPC, 200);
            kernel_syscall_dispatch(2, 52, futex_addr, 7, 5, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            ok = expect_eq(wait_wake_futex(2, futex_addr, 1), 1, ok);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 201, ok);
            ok = expect_eq(kload(120 + 19), 1, ok);

            process_create(6, 7, 0, 1, 0, 180);
            process_create(7, 8, 0, 1, 0, 220);
            var futex_va: t40 = 10;
            var futex_ppn_a: t40 = exec_process_dmem_ppn(6);
            var futex_ppn_b: t40 = exec_process_dmem_ppn(7);
            kstore(exec_hw_dmem_ptbr(6), exec_encode_pte(futex_ppn_a, 1, 1, 1, 0));
            kstore(exec_hw_dmem_ptbr(7), exec_encode_pte(futex_ppn_b, 1, 1, 1, 0));
            kstore(180 + TASK_CONTEXT_DMEM_PTBR, exec_hw_dmem_ptbr(6));
            kstore(180 + TASK_CONTEXT_DMEM_PAGES, 1);
            kstore(220 + TASK_CONTEXT_DMEM_PTBR, exec_hw_dmem_ptbr(7));
            kstore(220 + TASK_CONTEXT_DMEM_PAGES, 1);
            kstore(futex_ppn_a * MMU_PAGE_WORDS + futex_va, 42);
            kstore(futex_ppn_b * MMU_PAGE_WORDS + futex_va, 42);
            ok = expect_eq(hw_pte_flags(kload(exec_hw_dmem_ptbr(6))), 40, ok);
            ok = expect_eq(hw_pte_allows(hw_pte_flags(kload(exec_hw_dmem_ptbr(6))), 0), 1, ok);
            ok = expect_eq(user_validate_span(7, futex_va, 1, 0), 1, ok);
            ok = expect_pos(kload(METRIC_TLB_MISSES_ADDR), ok);
            kstore(180 + TASK_CONTEXT_EPC, 600);
            kernel_syscall_dispatch(7, 52, futex_va, 42, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(6), PROC_BLOCKED, ok);
            ok = expect_pos(kload(METRIC_TLB_HITS_ADDR), ok);
            ok = expect_pos(kload(METRIC_SPAN_CACHE_HITS_ADDR), ok);
            var saved_futex_pte: t40 = kload(exec_hw_dmem_ptbr(6));
            var slow_faults_before: t40 = kload(METRIC_SLOW_PATH_FAULTS_ADDR);
            kstore(exec_hw_dmem_ptbr(6), 0);
            ok = expect_eq(user_validate_span(7, futex_va, 1, 0), 0, ok);
            ok = expect_pos(kload(METRIC_SLOW_PATH_FAULTS_ADDR) - slow_faults_before, ok);
            kstore(exec_hw_dmem_ptbr(6), saved_futex_pte);
            translation_cache_invalidate(6);
            kernel_syscall_dispatch(8, 53, futex_va, 1, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 0, ok);
            ok = expect_eq(process_state(6), PROC_BLOCKED, ok);
            kernel_syscall_dispatch(7, 53, futex_va, 1, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            ok = expect_eq(process_state(6), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(180 + TASK_CONTEXT_EPC), 601, ok);
            process_reap_slot(6);
            process_reap_slot(7);

            kstore(120 + TASK_CONTEXT_EPC, 300);
            kernel_syscall_dispatch(2, 56, 2, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_SLEEPING, ok);
            kstore(KERNEL_TICK_ADDR, kload(KERNEL_TICK_ADDR) + 1);
            wait_expire_timeouts();
            ok = expect_eq(process_state(1), PROC_SLEEPING, ok);
            kstore(KERNEL_TICK_ADDR, kload(KERNEL_TICK_ADDR) + 1);
            wait_expire_timeouts();
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 301, ok);

            ipc_open(2, 0, 0);
            kstore(120 + TASK_CONTEXT_EPC, 400);
            kernel_syscall_dispatch(2, 54, 2, USER_MEM_BASE + 820, 10, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            kernel_syscall_dispatch(1, 23, 2, 999, 0, 0);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(USER_MEM_BASE + 820), 999, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 401, ok);

            var path: t40 = USER_MEM_BASE;
            var root_path: t40 = USER_MEM_BASE + 20;
            var dot_path: t40 = USER_MEM_BASE + 40;
            var dir_path: t40 = USER_MEM_BASE + 60;
            var parent_path: t40 = USER_MEM_BASE + 75;
            var huge_path: t40 = USER_MEM_BASE + 92;
            var src: t40 = USER_MEM_BASE + 100;
            var dst: t40 = USER_MEM_BASE + 300;
            var stat_out: t40 = USER_MEM_BASE + 500;
            var dir_out: t40 = USER_MEM_BASE + 600;
            var ipc_out: t40 = USER_MEM_BASE + 800;
            var app_path: t40 = USER_MEM_BASE + 850;
            var exec_desc: t40 = USER_MEM_BASE + 900;
            seed_path(path);
            seed_root(root_path);
            seed_dot_alpha(dot_path);
            seed_dir(dir_path);
            seed_dir_parent_alpha(parent_path);
            seed_huge(huge_path);
            seed_app(app_path);
            seed_payload(src, 90);
            seed_exec_desc(exec_desc, 700, 2, 1, 24);

            ok = expect_eq(vfs_create(0, dir_path, KIND_DIR), 1, ok);
            var fd: t40 = vfs_open(1, path, 2);
            var alpha_inode: t40 = vfs_lookup(0, path);
            ok = expect_eq(vfs_lookup(0, dot_path), alpha_inode, ok);
            ok = expect_eq(vfs_lookup(0, parent_path), alpha_inode, ok);
            ok = expect_eq(vfs_write(1, fd, src, 90), 90, ok);
            ok = expect_eq(vfs_extent_count(alpha_inode), 4, ok);
            ok = expect_eq(kload(extent_addr(vfs_find_extent_covering(alpha_inode, 54)) + EXTENT_LOGICAL_START), 54, ok);
            ok = expect_eq(buffer_dirty_count(), 4, ok);
            ok = expect_eq(buffer_pinned_count(), 0, ok);
            ok = expect_eq(buffer_find(0, alpha_inode, 0), 0, ok);
            ok = expect_eq(buffer_find(0, alpha_inode, 3), 3, ok);
            ok = expect_eq(vfs_fsync(1, fd), 1, ok);
            ok = expect_eq(buffer_dirty_count(), 0, ok);
            ok = expect_eq(buffer_flush_count(), 4, ok);
            vfs_close(1, fd);
            var read_fd: t40 = vfs_open(1, path, 0);
            ok = expect_eq(vfs_read(1, read_fd, dst, 90), 90, ok);
            ok = verify_payload(src, dst, 90, ok);
            ok = expect_eq(vfs_stat(0, path, stat_out), 90, ok);
            ok = expect_eq(kload(stat_out + 2), 90, ok);
            var dir_words: t40 = vfs_readdir(0, root_path, dir_out, 32);
            if dir_words > 0 {
                ok = expect_eq(kload(dir_out + 1), KIND_DIR, ok);
                ok = expect_eq(kload(dir_out + 2), 3, ok);
                ok = expect_eq(kload(dir_out + 7), KIND_FILE, ok);
                ok = expect_eq(kload(dir_out + 8), 5, ok);
                ok = expect_eq(kload(dir_out + 9), 97, ok);
            } else {
                ok = 0;
            }

            kernel_syscall_dispatch(1, 16, path, stat_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 90, ok);

            var payload_addr: t40 = kload(extent_addr(vfs_find_extent_covering(alpha_inode, 0)) + EXTENT_DATA_ADDR);
            var payload_old: t40 = kload(payload_addr);
            var payload_tx: t40 = log_begin();
            log_write(payload_tx, payload_addr, payload_old, 999);
            log_recover();
            ok = expect_eq(kload(payload_addr), payload_old, ok);
            payload_tx = log_begin();
            log_write(payload_tx, payload_addr, payload_old, 888);
            log_commit(payload_tx);
            kstore(payload_addr, payload_old);
            log_recover();
            ok = expect_eq(kload(payload_addr), 888, ok);
            kstore(payload_addr, payload_old);

            var inode_size_addr: t40 = inode_addr(alpha_inode) + INODE_SIZE;
            var old_size: t40 = kload(inode_size_addr);
            var crash_tx: t40 = log_begin();
            log_write(crash_tx, inode_size_addr, old_size, 123);
            log_recover();
            ok = expect_eq(kload(inode_size_addr), old_size, ok);
            var committed_tx: t40 = log_begin();
            log_write(committed_tx, inode_size_addr, old_size, 91);
            log_commit(committed_tx);
            kstore(inode_size_addr, old_size);
            log_recover();
            ok = expect_eq(kload(inode_size_addr), 91, ok);
            kstore(inode_size_addr, old_size);

            var huge_fd: t40 = vfs_open(1, huge_path, 2);
            var huge_inode: t40 = vfs_lookup(0, huge_path);
            ok = expect_eq(user_has_dmem_mmu(1), 0, ok);
            var saved_next_data: t40 = kload(VFS_NEXT_DATA_ADDR);
            kstore(VFS_NEXT_DATA_ADDR, VFS_DATA_BASE + VFS_PAYLOAD_WORDS - 10);
            ok = expect_eq(user_validate_span(1, src, 20, 0), 1, ok);
            ok = expect_eq(vfs_write(1, huge_fd, src, 20), ERR_NO_SPACE, ok);
            kstore(VFS_NEXT_DATA_ADDR, saved_next_data);
            ok = expect_eq(vfs_stat(0, huge_path, 0), 0, ok);
            ok = expect_eq(vfs_extent_count(huge_inode), 0, ok);
            vfs_close(1, huge_fd);

            var pte_addr: t40 = USER_MEM_LIMIT - 8;
            kstore(pte_addr, 0);
            ok = expect_eq(pte_faults(kload(pte_addr)), 1, ok);
            ok = expect_eq(mmu_page_walk_valid(pte_addr), 0, ok);
            kstore(pte_addr, 5);
            ok = expect_eq(mmu_page_walk_valid(pte_addr), 1, ok);
            page_pin(ppn);
            ok = expect_eq(cow_fault_can_remap(pte_addr, ppn), 0, ok);
            page_unpin(ppn);

            namespace_create(1, 0);
            process_create(2, 3, 1, 1, 1, 130);
            var ns_fd: t40 = vfs_open(3, path, 2);
            ok = expect_eq(vfs_write(3, ns_fd, src, 3), 3, ok);
            vfs_close(3, ns_fd);
            process_cache_observe(2, 77, 88);
            ok = expect_eq(process_cache_read(2, 77), 88, ok);
            ok = expect_eq(exec_process(2, path), ERR_INVALID, ok);
            ok = expect_eq(process_cache_read(2, 77), 88, ok);
            quota_set_limit(1, 1);
            kernel_syscall_dispatch(3, 21, app_path, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_NOT_FOUND, ok);
            ok = expect_pos(vfs_create(1, app_path, KIND_EXEC), ok);
            var app_fd: t40 = vfs_open(3, app_path, 2);
            ok = expect_eq(vfs_write(3, app_fd, exec_desc, EXEC_DESC_WORDS), EXEC_DESC_WORDS, ok);
            vfs_close(3, app_fd);
            var app_id: t40 = app_register(1, app_path, APP_CAP_CONSOLE + APP_CAP_PROCESS, 1, 1, EXEC_IMAGE_WORDS);
            ok = expect_eq(app_id, 0, ok);
            ok = expect_eq(app_caps_allow(APP_CAP_CONSOLE + APP_CAP_PROCESS, APP_CAP_PROCESS), 1, ok);
            ok = expect_eq(app_caps_allow(APP_CAP_CONSOLE, APP_CAP_PROCESS), 0, ok);
            kernel_syscall_dispatch(3, 21, app_path, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), EXEC_DESC_WORDS, ok);
            ok = expect_eq(app_launch_count(app_id), 1, ok);
            ok = expect_eq(kload(exec_image_addr(2)), EXEC_MAGIC, ok);
            ok = expect_eq(kload(exec_hw_imem_ptbr(2)), exec_encode_pte(700, 1, 0, 0, 1), ok);
            ok = expect_eq(kload(exec_hw_imem_ptbr(2) + 1), exec_encode_pte(701, 1, 0, 0, 1), ok);
            ok = expect_eq(kload(exec_hw_dmem_ptbr(2)), exec_encode_pte(exec_process_dmem_ppn(2), 1, 1, 1, 0), ok);
            ok = expect_eq(kload(kload(process_addr(2) + PROC_CONTEXT) + TASK_CONTEXT_EPC), 0, ok);
            ok = expect_eq(kload(kload(process_addr(2) + PROC_CONTEXT) + TASK_CONTEXT_IMEM_PAGES), 2, ok);
            ok = expect_eq(kload(kload(process_addr(2) + PROC_CONTEXT) + TASK_CONTEXT_DMEM_PAGES), USER_SCRATCH_VPN_BASE + USER_SCRATCH_PAGES, ok);
            ok = expect_eq(kload(kload(process_addr(2) + PROC_CONTEXT) + TASK_CONTEXT_SP), 24, ok);
            ok = expect_eq(process_cache_read(2, 77), ERR_NOT_FOUND, ok);
            ok = expect_eq(kload(PROC_CAPS_BASE + 2), APP_CAP_CONSOLE + APP_CAP_PROCESS, ok);
            kernel_syscall_dispatch(3, 19, 30, 0, 0, 0);
            kstore(36000, kload(SYS_STATUS_ADDR));
            kstore(36001, kload(SYS_PAYLOAD_ADDR));
            kstore(36002, kload(process_heap_addr(2) + PROC_HEAP_BREAK));
            kstore(36003, kload(kload(process_addr(2) + PROC_CONTEXT) + TASK_CONTEXT_DMEM_PAGES));
            kstore(36004, kload(exec_hw_dmem_ptbr(2) + 2));
            ok = expect_eq(user_store_word(3, USER_MEM_BASE + 0, 47), 1, ok);
            ok = expect_eq(user_store_word(3, USER_MEM_BASE + 1, 97), 1, ok);
            ok = expect_eq(user_store_word(3, USER_MEM_BASE + 2, 112), 1, ok);
            ok = expect_eq(user_store_word(3, USER_MEM_BASE + 3, 112), 1, ok);
            ok = expect_eq(user_store_word(3, USER_MEM_BASE + 4, 0), 1, ok);
            ok = expect_eq(user_load_word(3, USER_MEM_BASE + 1), 97, ok);
            ok = expect_eq(user_copy_cstring_to_kernel(3, USER_MEM_BASE, SYSCALL_PATH_COPY_BASE, VFS_MAX_PATH_WORDS), 5, ok);
            ok = expect_eq(kload(SYSCALL_PATH_COPY_BASE + 1), 97, ok);
            ok = expect_eq(app_find(1, SYSCALL_PATH_COPY_BASE), app_id, ok);
            ok = expect_pos(vfs_lookup(1, SYSCALL_PATH_COPY_BASE), ok);
            ok = expect_eq(vfs_stat(1, SYSCALL_PATH_COPY_BASE, 0), EXEC_DESC_WORDS, ok);
            ok = expect_eq(quota_remaining(1), 0, ok);
            ok = expect_eq(kload(app_addr(app_id) + APP_QUOTA), 1, ok);
            ok = expect_eq(quota_charge(kload(app_addr(app_id) + APP_QUOTA), 1), ERR_NO_SPACE, ok);
            ok = expect_eq(app_launch(3, SYSCALL_PATH_COPY_BASE, 0), ERR_NO_SPACE, ok);
            kernel_syscall_dispatch(3, 21, USER_MEM_BASE, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_NO_SPACE, ok);
            ok = expect_eq(app_launch_count(app_id), 1, ok);

            pte_map(0, 0, ppn, 0);
            pte_map(0, 1, ppn, PTE_FLAG_MMIO);
            page_pin(ppn);
            ok = expect_eq(fork_process(0, 3, 4), ERR_INVALID, ok);
            page_unpin(ppn);
            ok = expect_eq(fork_process(0, 3, 4), 4, ok);
            ok = expect_eq(pte_entry_faults(3, 0), 0, ok);
            ok = expect_eq(pte_entry_faults(3, 1), 1, ok);
            ok = expect_eq(kload(pte_entry_addr(3, 0) + PTE_FLAGS), PTE_FLAG_COW, ok);
            ok = expect_eq(cow_fault_resolve(3, 0, ppn + 1), 1, ok);
            ok = expect_eq(kload(pte_entry_addr(3, 0) + PTE_FLAGS), 0, ok);

            ipc_open(0, 0, 1);
            ok = expect_eq(ipc_send(0, 0, 444), 1, ok);
            ok = expect_eq(ipc_recv(0, 1, ipc_out), 1, ok);
            ok = expect_eq(kload(ipc_out), 444, ok);
            ok = expect_eq(ipc_recv(0, 1, ipc_out), 0, ok);
            kernel_syscall_dispatch(3, 23, 0, 555, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            kernel_syscall_dispatch(1, 24, 0, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out), 555, ok);
            kernel_syscall_dispatch(1, 24, 0, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 0, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_EOF, ok);

            kernel_syscall_dispatch(1, 25, 8, 6, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), FB_FRONT_BUFFER_BASE, ok);
            ok = expect_eq(kload(FB_STATE_BASE + FB_WIDTH), 8, ok);
            ok = expect_eq(kload(FB_STATE_BASE + FB_HEIGHT), 6, ok);
            ok = expect_eq(kload(FB_STATE_BASE + FB_PINNED_PAGES), framebuffer_pages(framebuffer_words(8, 6)), ok);
            ok = expect_eq(kload(page_header_field(FB_PPN_BASE, PH_PIN_COUNT)), 1, ok);

            kernel_syscall_dispatch(1, 27, 1, 2, 3, 2);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 0, ok);
            kernel_syscall_dispatch(1, 28, 0, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), window_user_addr(0), ok);
            var win0_buf: t40 = kload(window_addr(0) + WIN_BUFFER_ADDR);
            kstore(win0_buf, 11);
            kstore(win0_buf + 1, 12);
            kstore(win0_buf + 2, 13);
            kernel_syscall_dispatch(1, 29, 0, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            var fb_visible: t40 = framebuffer_visible_base();
            var pixel0: t40 = fb_visible + (2 * 8 + 1) * 3;
            ok = expect_eq(kload(pixel0), 11, ok);
            ok = expect_eq(kload(pixel0 + 1), 12, ok);
            ok = expect_eq(kload(pixel0 + 2), 13, ok);
            ok = expect_eq(kload(window_addr(0) + WIN_DIRTY), 0, ok);

            kernel_syscall_dispatch(1, 27, 1, 2, 3, 2);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            kernel_syscall_dispatch(1, 28, 1, 0, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), window_user_addr(1), ok);
            var win1_buf: t40 = kload(window_addr(1) + WIN_BUFFER_ADDR);
            kstore(win1_buf, 44);
            kstore(win1_buf + 1, 45);
            kstore(win1_buf + 2, 46);
            kernel_syscall_dispatch(1, 29, 1, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 2, ok);
            fb_visible = framebuffer_visible_base();
            pixel0 = fb_visible + (2 * 8 + 1) * 3;
            ok = expect_eq(kload(pixel0), 44, ok);
            ok = expect_eq(kload(pixel0 + 1), 45, ok);
            ok = expect_eq(kload(pixel0 + 2), 46, ok);
            ok = expect_eq(kload(COMPOSITOR_FRAME_COUNT_ADDR), 2, ok);
            kernel_syscall_dispatch(1, 30, 1, 2, 3, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_X), 2, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_Y), 3, ok);
            kernel_syscall_dispatch(1, 31, 1, 7, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_Z), 7, ok);
            kernel_syscall_dispatch(1, 34, 1, 4, 3, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_WIDTH), 4, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_HEIGHT), 3, ok);
            ok = expect_eq(kload(win1_buf), 0, ok);
            kstore(win1_buf + (2 * 4 + 3) * 3, 77);
            kstore(win1_buf + (2 * 4 + 3) * 3 + 1, 78);
            kstore(win1_buf + (2 * 4 + 3) * 3 + 2, 79);
            kernel_syscall_dispatch(1, 29, 1, 0, 0, 0);
            fb_visible = framebuffer_visible_base();
            var resized_pixel: t40 = fb_visible + (5 * 8 + 5) * 3;
            ok = expect_eq(kload(resized_pixel), 77, ok);
            ok = expect_eq(kload(resized_pixel + 1), 78, ok);
            ok = expect_eq(kload(resized_pixel + 2), 79, ok);
            kernel_syscall_dispatch(1, 34, 1, 100, 100, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_NO_SPACE, ok);
            ok = expect_eq(window_route_input(2, 2, 3, 5), 1, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_EVENT_COUNT), 1, ok);
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + EVENT_KIND), 2, ok);
            ok = expect_eq(kload(ipc_out + EVENT_X_OR_KEY), 2, ok);
            ok = expect_eq(kload(ipc_out + EVENT_Y), 3, ok);
            ok = expect_eq(kload(ipc_out + EVENT_MODIFIERS), 5, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_EVENT_COUNT), 0, ok);
            ok = expect_eq(window_route_input(EVENT_KIND_MOUSE, 5, 3, 1), 1, ok);
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + EVENT_KIND), EVENT_KIND_CLOSE, ok);
            ok = expect_eq(kload(ipc_out + EVENT_X_OR_KEY), 1, ok);
            ok = expect_eq(signal_has(kload(PROC_SIGNAL_PENDING_BASE), SIGNAL_CLOSE_REQUEST), 1, ok);
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + EVENT_KIND), EVENT_KIND_KEY, ok);
            ok = expect_eq(kload(ipc_out + EVENT_X_OR_KEY), 75, ok);
            ok = expect_eq(kload(ipc_out + EVENT_Y), 0, ok);
            ok = expect_eq(kload(ipc_out + EVENT_MODIFIERS), 0, ok);
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 0, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_EOF, ok);
            kernel_syscall_dispatch(3, 33, 0, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            ok = expect_eq(window_route_input(2, 7, 5, 0), 0, ok);
            unsafe { csr_write(mouse_x, 2); }
            unsafe { csr_write(mouse_y, 3); }
            unsafe { csr_write(mouse_btn, 1); }
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            ok = expect_eq(kload(WINDOW_FOCUS_ID_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + EVENT_KIND), EVENT_KIND_MOUSE, ok);
            ok = expect_eq(kload(ipc_out + EVENT_X_OR_KEY), 2, ok);
            ok = expect_eq(kload(ipc_out + EVENT_Y), 3, ok);
            ok = expect_eq(kload(ipc_out + EVENT_MODIFIERS), 1, ok);
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 0, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_EOF, ok);
            ok = expect_eq(window_poll_input_driver(), 0, ok);
            unsafe { csr_write(mouse_btn, 0); }
            ok = expect_eq(window_poll_input_driver(), 0, ok);
            kernel_syscall_dispatch(2, 35, 1, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            kernel_syscall_dispatch(1, 33, 1, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + EVENT_KIND), EVENT_KIND_CLOSE, ok);
            ok = expect_eq(kload(ipc_out + EVENT_X_OR_KEY), 1, ok);
            kernel_syscall_dispatch(3, 35, 1, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            kernel_syscall_dispatch(1, 32, 1, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(1) + WIN_ACTIVE), 0, ok);

            kernel_syscall_dispatch(2, 27, 0, 0, 2, 2);
            var wait_win2: t40 = kload(SYS_PAYLOAD_ADDR);
            kstore(120 + TASK_CONTEXT_EPC, 500);
            kernel_syscall_dispatch(2, 55, wait_win2, ipc_out, 10, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            window_queue_event(window_find(wait_win2), EVENT_KIND_KEY, 65, 0, 0);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(ipc_out + EVENT_KIND), EVENT_KIND_KEY, ok);
            ok = expect_eq(kload(ipc_out + EVENT_X_OR_KEY), 65, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 501, ok);
            kernel_syscall_dispatch(2, 32, wait_win2, 0, 0, 0);

            process_create(4, 5, 0, 1, 0, 160);
            process_set_parent(4, 1);
            process_create(5, 6, 0, 1, 0, 170);
            process_set_parent(5, 5);
            kernel_syscall_dispatch(6, 48, 5, SIGNAL_KILL, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            ok = expect_eq(process_state(4), PROC_RUNNABLE, ok);
            kernel_syscall_dispatch(6, 49, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            kernel_syscall_dispatch(6, 50, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            process_set_caps(5, APP_CAP_PROCESS);
            kernel_syscall_dispatch(6, 49, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_STOPPED, ok);
            kernel_syscall_dispatch(6, 50, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_RUNNABLE, ok);
            process_set_caps(5, 0);
            kernel_syscall_dispatch(5, 49, 6, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(5), PROC_STOPPED, ok);
            kernel_syscall_dispatch(5, 50, 6, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(5), PROC_RUNNABLE, ok);
            var kill_fd: t40 = vfs_open(5, path, 0);
            ok = expect_pos(kill_fd, ok);
            ok = expect_eq(ipc_open_owned(1, 5, 0, 1), 1, ok);
            pte_map(4, 0, ppn, 0);
            process_wait(4, 777);
            kernel_syscall_dispatch(1, 49, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_STOPPED, ok);
            ok = expect_eq(kload(WAIT_CHANNEL_BASE + 4), 0, ok);
            kernel_syscall_dispatch(1, 50, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_RUNNABLE, ok);
            kernel_syscall_dispatch(5, 27, 0, 0, 2, 2);
            var killed_window: t40 = kload(SYS_PAYLOAD_ADDR);
            var killed_window_slot: t40 = window_find(killed_window);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(killed_window_slot) + WIN_OWNER_PID), 5, ok);
            process_wait(4, 888);
            kernel_syscall_dispatch(1, 48, 5, SIGNAL_KILL, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_ZOMBIE, ok);
            ok = expect_eq(kload(fd_addr(kill_fd) + FD_REFCOUNT), 0, ok);
            ok = expect_eq(kload(window_addr(killed_window_slot) + WIN_ACTIVE), 0, ok);
            ok = expect_eq(kload(ipc_channel_addr(1) + IPC_VERSION), 0, ok);
            ok = expect_eq(kload(IPC_OWNER_BASE + 1), -1, ok);
            ok = expect_eq(kload(WAIT_CHANNEL_BASE + 4), 0, ok);
            ok = expect_eq(pte_entry_faults(4, 0), 1, ok);
            ok = expect_eq(kload(PROC_PARENT_PID_BASE + 5), -1, ok);
            kernel_syscall_dispatch(1, 51, 5, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + PROCINFO_STATE), PROC_ZOMBIE, ok);
            ok = expect_eq(kload(ipc_out + PROCINFO_PARENT_PID), 1, ok);
            ok = expect_eq(signal_has(kload(ipc_out + PROCINFO_PENDING_SIGNALS), SIGNAL_KILL), 1, ok);
            kernel_syscall_dispatch(1, 48, 6, SIGNAL_KILL, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_find_pid(6), ERR_NOT_FOUND, ok);
            kernel_syscall_dispatch(1, 43, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), SIGNAL_KILL, ok);
            ok = expect_eq(process_find_pid(5), ERR_NOT_FOUND, ok);

            return ok;
        }
    )";

    CompileResult compiled = compileSource("phase_d_kernel_e2e.trit", kernel + "\n" + driver);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR PHASE D KERNEL:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "kernel.trit plus driver compiles");
    expect(contains(compiled.assembly, "tldr.+1"), "scheduler/VFS paths use acquire loads");
    expect(contains(compiled.assembly, "tstr.+1"), "scheduler/VFS paths use release CAS stores");
    const std::string trap_stub = readTextFile("OS3/native_kernel_trap_stub.tasm");
    expect(contains(trap_stub, "call kernel_dispatch"), "D1 trap stub enters compiled kernel_dispatch");
    if (compiled.success && !trap_stub.empty()) {
        auto assembled_stub = sandbox::vm::assembler::assemble(trap_stub + "\n" + compiled.assembly);
        if (!assembled_stub.success) {
            for (const auto& error : assembled_stub.errors) {
                std::cerr << "D1 STUB ASSEMBLY ERROR line " << error.line
                          << ": " << error.message << "\n";
            }
        }
        expect(assembled_stub.success, "D1 trap stub assembles with compiled kernel.trit output");
    }

    const std::string boot = readTextFile("OS3/native_kernel_boot.tasm");
    expect(contains(boot, "call kernel_init"), "native boot prelude initializes compiled kernel");
    expect(contains(boot, "syscall 16"), "native boot prelude exercises D8 syscall dispatch");
    if (compiled.success && !trap_stub.empty() && !boot.empty()) {
        auto boot_image = sandbox::vm::assembler::assemble(boot + "\n" + trap_stub + "\n" + compiled.assembly);
        if (!boot_image.success) {
            for (const auto& error : boot_image.errors) {
                std::cerr << "NATIVE BOOT ASSEMBLY ERROR line " << error.line
                          << ": " << error.message << "\n";
            }
        }
        expect(boot_image.success, "native Phase D boot image assembles");
        sandbox::vm::VMState boot_vm(sandbox::vm::ProductionProfile::minimum());
        boot_vm.resetBlockDevice(192);
        if (boot_image.success) {
            expect(sandbox::vm::assembler::loadAndReset(boot_vm, boot_image),
                   "native Phase D boot image loads");
            const auto boot_result = sandbox::vm::run(boot_vm, 50000000);
            if (!boot_result.halted()) {
                std::cout << "DEBUG BOOT: status=" << static_cast<int>(boot_result.status)
                          << " pc=" << boot_vm.pc
                          << " trap=" << sandbox::vm::ops::toLong(boot_vm.trap_reg) << "\n";
            }
            expect(boot_result.halted(), "native Phase D boot image returns from user syscall");
            expect(regLong(boot_vm, 13) == 1, "booted native kernel returns syscall success status");
            expect(regLong(boot_vm, 14) == 0, "booted native kernel stats root with zero size");
            expect(regLong(boot_vm, 15) == 0, "booted native kernel returns zero syscall detail");
            auto after_it = boot_image.labels.find("after_user_stat");
            if (after_it != boot_image.labels.end()) {
                expect(static_cast<int>(boot_vm.pc) == after_it->second,
                       "ERET resumed at the instruction after syscall");
            }
        }
    }

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "kernel.trit plus driver links");
    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    vm.resetBlockDevice(192);
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "kernel image loads");
        vm.enqueueConsoleAscii("K");
        const auto result = sandbox::vm::run(vm, 50000000);
        if (!result.halted()) {
            std::cout << "DEBUG: status=" << static_cast<int>(result.status)
                      << " pc=" << vm.pc
                      << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg) << "\n";
        }
        expect(result.halted(), "kernel driver halts");
        if (regLong(vm, 13) != 1) {
            auto [flag, flag_fault] = vm.dmem.load(35000);
            auto [actual, actual_fault] = vm.dmem.load(35001);
            auto [expected, expected_fault] = vm.dmem.load(35002);
            auto [index, index_fault] = vm.dmem.load(35004);
            (void)flag_fault;
            (void)actual_fault;
            (void)expected_fault;
            (void)index_fault;
            std::cout << "DEBUG: first kernel assertion flag="
                      << sandbox::vm::ops::toLong(flag)
                      << " actual=" << sandbox::vm::ops::toLong(actual)
                      << " expected=" << sandbox::vm::ops::toLong(expected)
                      << " index=" << sandbox::vm::ops::toLong(index) << "\n";
        }
        expect(regLong(vm, 13) == 1, "Phase D kernel primitives pass VM assertions");
        expect(wordAt(vm, 36000) == 1, "sys_sbrk returns success in the native kernel");
        expect(wordAt(vm, 36001) == 27, "sys_sbrk returns the old process break");
        expect(wordAt(vm, 36002) == 57, "sys_sbrk advances the process heap break");
        expect(wordAt(vm, 36003) == 488, "sys_sbrk preserves sparse user scratch DMEM span");
        const long long expected_sbrk_pte = (30000 + 2 * 192 + 2) * 243 + 40;
        expect(wordAt(vm, 36004) == expected_sbrk_pte, "sys_sbrk installs the new DMEM PTE");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testPhaseDKernelEndToEnd();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "Phase D kernel tests passed\n";
    return 0;
}
