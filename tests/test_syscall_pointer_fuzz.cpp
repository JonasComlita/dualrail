#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr long long kSeed = 4242;
constexpr int kIterations = 96;
constexpr int kMaxSteps = 50000000;
constexpr int kFailureCountAddr = 35000;
constexpr int kFailureCaseAddr = 35001;
constexpr int kFailurePointerAddr = 35002;
constexpr int kFailureStatusAddr = 35003;
constexpr int kFailurePayloadAddr = 35004;
constexpr int kFailureDetailAddr = 35005;
constexpr int kFailureReasonAddr = 35006;
constexpr int kFailureGuardBeforeAddr = 35007;
constexpr int kFailureGuardAfterAddr = 35008;
constexpr int kSentinelCorruptionAddr = 35009;
constexpr int kGuardCorruptionAddr = 35010;
constexpr int kSetupFailureAddr = 35011;
constexpr int kSentinelAddr = 50000;
constexpr int kSentinelValue = 1729;

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

long long wordAt(const sandbox::vm::VMState& vm, int address) {
    auto [value, fault] = vm.dmem.load(address);
    if (fault != sandbox::vm::MemFaultCode::OK) return -0x7fffffffffffffffLL;
    return sandbox::vm::ops::toLong(value);
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

void dumpDiagnostics(const sandbox::compiler::CompileResult& compiled) {
    for (const auto& diagnostic : compiled.diagnostics) {
        std::cerr << "  " << diagnostic.format() << "\n";
    }
}

const std::string& fuzzDriver() {
    static const std::string source = R"TRIT(
const FUZZ_SEED: t40 = 4242;
const FUZZ_ITERATIONS: t40 = 96;
const FUZZ_CASE_COUNT: t40 = 19;
const FUZZ_FAILURE_COUNT_ADDR: t40 = 35000;
const FUZZ_FAILURE_CASE_ADDR: t40 = 35001;
const FUZZ_FAILURE_POINTER_ADDR: t40 = 35002;
const FUZZ_FAILURE_STATUS_ADDR: t40 = 35003;
const FUZZ_FAILURE_PAYLOAD_ADDR: t40 = 35004;
const FUZZ_FAILURE_DETAIL_ADDR: t40 = 35005;
const FUZZ_FAILURE_REASON_ADDR: t40 = 35006;
const FUZZ_FAILURE_GUARD_BEFORE_ADDR: t40 = 35007;
const FUZZ_FAILURE_GUARD_AFTER_ADDR: t40 = 35008;
const FUZZ_SENTINEL_CORRUPTION_ADDR: t40 = 35009;
const FUZZ_GUARD_CORRUPTION_ADDR: t40 = 35010;
const FUZZ_SETUP_FAILURE_ADDR: t40 = 35011;
const FUZZ_SENTINEL_ADDR: t40 = 50000;
const FUZZ_SENTINEL_VALUE: t40 = 1729;

fn seed_path(addr: t40) -> t40 {
    kstore(addr + 0, 47);
    kstore(addr + 1, 102);
    kstore(addr + 2, 117);
    kstore(addr + 3, 122);
    kstore(addr + 4, 122);
    kstore(addr + 5, 0);
    return addr;
}

fn seed_root(addr: t40) -> t40 {
    kstore(addr + 0, 47);
    kstore(addr + 1, 0);
    return addr;
}

fn seed_rename_path(addr: t40) -> t40 {
    kstore(addr + 0, 47);
    kstore(addr + 1, 114);
    kstore(addr + 2, 101);
    kstore(addr + 3, 110);
    kstore(addr + 4, 97);
    kstore(addr + 5, 109);
    kstore(addr + 6, 101);
    kstore(addr + 7, 100);
    kstore(addr + 8, 0);
    return addr;
}

fn seed_payload(addr: t40) -> t40 {
    kstore(addr, 77);
    return addr;
}

fn next_random(seed: t40) -> t40 {
    // Keep the deterministic PRNG in the immediate encoding range while
    // retaining a full-period-style affine recurrence over the bounded
    // harness state space.
    var value: t40 = seed * 1661 + 101;
    unsafe { value = tmod(value, 19683); }
    return value;
}

fn bad_pointer(iteration: t40, random_value: t40) -> t40 {
    var kind: t40 = iteration;
    while kind - 8 >= 0 {
        kind = kind - 8;
    }
    if kind == 0 { return 0; }
    if kind == 1 { return USER_MEM_BASE - 1; }
    if kind == 2 { return USER_MEM_LIMIT; }
    if kind == 3 { return FUZZ_SENTINEL_ADDR; }
    if kind == 4 { return -1; }
    var offset: t40 = random_value;
    unsafe { offset = tmod(offset, 2000); }
    if kind == 5 { return 17000 + offset; }
    if kind == 6 { return 20500 + offset; }
    return 18000 + offset;
}

fn protected_checksum(fd_read: t40, fd_write: t40, window_slot: t40, socket_id: t40) -> t40 {
    var checksum: t40 = 0;
    checksum = checksum + kload(VFS_NEXT_INODE_ADDR) * 3;
    checksum = checksum + kload(VFS_NEXT_DIRENT_ADDR) * 5;
    checksum = checksum + kload(VFS_NEXT_EXTENT_ADDR) * 7;
    checksum = checksum + kload(fd_addr(fd_read) + FD_OFFSET) * 11;
    checksum = checksum + kload(fd_addr(fd_write) + FD_OFFSET) * 13;
    checksum = checksum + kload(IPC_BASE + IPC_COUNT) * 17;
    checksum = checksum + kload(IPC_BASE + IPC_READ) * 19;
    checksum = checksum + kload(window_addr(window_slot) + WIN_EVENT_COUNT) * 23;
    checksum = checksum + kload(window_addr(window_slot) + WIN_EVENT_READ) * 29;
    checksum = checksum + kload(net_socket_row(socket_id) + NET_SOCK_RX_READ) * 31;
    checksum = checksum + kload(net_socket_row(socket_id) + NET_SOCK_RX_WRITE) * 37;
    checksum = checksum + kload(process_addr(0) + PROC_STATE) * 41;
    checksum = checksum + kload(WAIT_CHANNEL_BASE) * 43;
    return checksum;
}

fn pointer_syscall(case_id: t40, pointer: t40, fd_read: t40, fd_write: t40,
                   window_id: t40, socket_id: t40, path_addr: t40,
                   root_addr: t40, rename_addr: t40) -> t40 {
    if case_id == 0 { return kernel_syscall_dispatch(1, 12, pointer, 0, 0, 0); }
    if case_id == 1 { return kernel_syscall_dispatch(1, 14, fd_read, pointer, 1, 0); }
    if case_id == 2 { return kernel_syscall_dispatch(1, 15, fd_write, pointer, 1, 0); }
    if case_id == 3 { return kernel_syscall_dispatch(1, 16, path_addr, pointer, 0, 0); }
    if case_id == 4 { return kernel_syscall_dispatch(1, 17, root_addr, pointer, 8, 0); }
    if case_id == 5 { return kernel_syscall_dispatch(1, 21, pointer, 0, 0, 0); }
    if case_id == 6 { return kernel_syscall_dispatch(1, 24, 0, pointer, 0, 0); }
    if case_id == 7 { return kernel_syscall_dispatch(1, 33, window_id, pointer, 0, 0); }
    if case_id == 8 { return kernel_syscall_dispatch(1, 40, socket_id, pointer, 0, 0); }
    if case_id == 9 { return kernel_syscall_dispatch(1, 41, pointer, 0, 0, 0); }
    if case_id == 10 { return kernel_syscall_dispatch(1, 42, pointer, 0, 0, 0); }
    if case_id == 11 { return kernel_syscall_dispatch(1, 46, pointer, 1, 0, 0); }
    if case_id == 12 { return kernel_syscall_dispatch(1, 51, 1, pointer, 0, 0); }
    if case_id == 13 { return kernel_syscall_dispatch(1, 52, pointer, 0, 0, 0); }
    if case_id == 14 { return kernel_syscall_dispatch(1, 54, 0, pointer, 1, 0); }
    if case_id == 15 { return kernel_syscall_dispatch(1, 55, window_id, pointer, 0, 0); }
    if case_id == 16 { return kernel_syscall_dispatch(1, 57, pointer, 0, 0, 0); }
    if case_id == 17 { return kernel_syscall_dispatch(1, 59, pointer, rename_addr, 0, 0); }
    return kernel_syscall_dispatch(1, 59, path_addr, pointer, 0, 0);
}

fn record_failure(case_id: t40, pointer: t40, status: t40, payload: t40,
                  detail: t40, reason: t40, guard_before: t40,
                  guard_after: t40) -> t40 {
    var count: t40 = kload(FUZZ_FAILURE_COUNT_ADDR);
    if count == 0 {
        kstore(FUZZ_FAILURE_CASE_ADDR, case_id);
        kstore(FUZZ_FAILURE_POINTER_ADDR, pointer);
        kstore(FUZZ_FAILURE_STATUS_ADDR, status);
        kstore(FUZZ_FAILURE_PAYLOAD_ADDR, payload);
        kstore(FUZZ_FAILURE_DETAIL_ADDR, detail);
        kstore(FUZZ_FAILURE_REASON_ADDR, reason);
        kstore(FUZZ_FAILURE_GUARD_BEFORE_ADDR, guard_before);
        kstore(FUZZ_FAILURE_GUARD_AFTER_ADDR, guard_after);
    }
    kstore(FUZZ_FAILURE_COUNT_ADDR, count + 1);
    return count + 1;
}

fn check_pointer_case(case_id: t40, pointer: t40, fd_read: t40,
                      fd_write: t40, window_id: t40, socket_id: t40,
                      path_addr: t40, root_addr: t40, rename_addr: t40) -> t40 {
    kstore(FUZZ_SENTINEL_ADDR, FUZZ_SENTINEL_VALUE);
    var guard_before: t40 = protected_checksum(fd_read, fd_write, 0, socket_id);
    var status: t40 = pointer_syscall(case_id, pointer, fd_read, fd_write,
                                      window_id, socket_id, path_addr,
                                      root_addr, rename_addr);
    var payload: t40 = kload(SYS_PAYLOAD_ADDR);
    var detail: t40 = kload(SYS_DETAIL_ADDR);
    var guard_after: t40 = protected_checksum(fd_read, fd_write, 0, socket_id);
    var reason: t40 = 0;
    if status + 1 != 0 { reason = reason + 1; }
    if payload != 0 { reason = reason + 2; }
    if detail + 5 != 0 { reason = reason + 4; }
    if kload(FUZZ_SENTINEL_ADDR) - FUZZ_SENTINEL_VALUE != 0 {
        reason = reason + 8;
        kstore(FUZZ_SENTINEL_CORRUPTION_ADDR,
               kload(FUZZ_SENTINEL_CORRUPTION_ADDR) + 1);
    }
    if guard_after - guard_before != 0 {
        reason = reason + 16;
        kstore(FUZZ_GUARD_CORRUPTION_ADDR,
               kload(FUZZ_GUARD_CORRUPTION_ADDR) + 1);
    }
    if reason != 0 {
        record_failure(case_id, pointer, status, payload, detail, reason,
                       guard_before, guard_after);
        return 0;
    }
    return 1;
}

fn main() -> t40 {
    kstore(FUZZ_FAILURE_COUNT_ADDR, 0);
    kstore(FUZZ_SENTINEL_CORRUPTION_ADDR, 0);
    kstore(FUZZ_GUARD_CORRUPTION_ADDR, 0);
    kstore(FUZZ_SETUP_FAILURE_ADDR, 0);
    if kernel_init() < 0 {
        kstore(FUZZ_SETUP_FAILURE_ADDR, -1);
        return -1;
    }
    var path_addr: t40 = USER_MEM_BASE;
    var root_addr: t40 = USER_MEM_BASE + 20;
    var rename_addr: t40 = USER_MEM_BASE + 40;
    var payload_addr: t40 = USER_MEM_BASE + 64;
    seed_path(path_addr);
    seed_root(root_addr);
    seed_rename_path(rename_addr);
    seed_payload(payload_addr);

    kernel_syscall_dispatch(1, 12, path_addr, 2, 0, 0);
    var fd_write: t40 = kload(SYS_PAYLOAD_ADDR);
    if fd_write < 0 {
        kstore(FUZZ_SETUP_FAILURE_ADDR, -2);
        return -1;
    }
    kernel_syscall_dispatch(1, 15, fd_write, payload_addr, 1, 0);
    kernel_syscall_dispatch(1, 12, path_addr, 0, 0, 0);
    var fd_read: t40 = kload(SYS_PAYLOAD_ADDR);
    if fd_read < 0 {
        kstore(FUZZ_SETUP_FAILURE_ADDR, -3);
        return -1;
    }

    ipc_open(0, 0, 0);
    ipc_send(0, 0, 777);
    framebuffer_init(8, 6);
    var window_id: t40 = window_create(1, 1, 1, 2, 2);
    if window_id < 0 {
        kstore(FUZZ_SETUP_FAILURE_ADDR, -4);
        return -1;
    }
    window_queue_event(0, EVENT_KIND_KEY, 65, 0, 0);
    var socket_id: t40 = net_socket_create(1, 1);
    if socket_id < 0 {
        kstore(FUZZ_SETUP_FAILURE_ADDR, -5);
        return -1;
    }
    net_send(socket_id, 1, 888);

    var seed: t40 = FUZZ_SEED;
    var iteration: t40 = 0;
    while FUZZ_ITERATIONS - iteration > 0 {
        seed = next_random(seed);
        var pointer: t40 = bad_pointer(iteration, seed);
        var case_id: t40 = iteration;
        while case_id - FUZZ_CASE_COUNT >= 0 {
            case_id = case_id - FUZZ_CASE_COUNT;
        }
        check_pointer_case(case_id, pointer, fd_read, fd_write, window_id,
                           socket_id, path_addr, root_addr, rename_addr);
        iteration = iteration + 1;
    }
    if kload(FUZZ_FAILURE_COUNT_ADDR) == 0 {
        return 1;
    }
    return -1;
}
)TRIT";
    return source;
}

struct RunSnapshot {
    bool halted = false;
    long long return_value = 0;
    long long failure_count = 0;
    long long failure_case = 0;
    long long failure_pointer = 0;
    long long failure_status = 0;
    long long failure_payload = 0;
    long long failure_detail = 0;
    long long failure_reason = 0;
    long long failure_guard_before = 0;
    long long failure_guard_after = 0;
    long long sentinel_corruptions = 0;
    long long guard_corruptions = 0;
    long long setup_failure = 0;
};

RunSnapshot runHarness(const sandbox::compiler::LinkResult& linked) {
    RunSnapshot snapshot;
    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    vm.resetBlockDevice(192);
    if (!sandbox::vm::assembler::loadAndReset(vm, linked.assembled)) {
        return snapshot;
    }
    const auto result = sandbox::vm::run(vm, kMaxSteps);
    snapshot.halted = result.halted();
    snapshot.return_value = regLong(vm, 13);
    snapshot.failure_count = wordAt(vm, kFailureCountAddr);
    snapshot.failure_case = wordAt(vm, kFailureCaseAddr);
    snapshot.failure_pointer = wordAt(vm, kFailurePointerAddr);
    snapshot.failure_status = wordAt(vm, kFailureStatusAddr);
    snapshot.failure_payload = wordAt(vm, kFailurePayloadAddr);
    snapshot.failure_detail = wordAt(vm, kFailureDetailAddr);
    snapshot.failure_reason = wordAt(vm, kFailureReasonAddr);
    snapshot.failure_guard_before = wordAt(vm, kFailureGuardBeforeAddr);
    snapshot.failure_guard_after = wordAt(vm, kFailureGuardAfterAddr);
    snapshot.sentinel_corruptions = wordAt(vm, kSentinelCorruptionAddr);
    snapshot.guard_corruptions = wordAt(vm, kGuardCorruptionAddr);
    snapshot.setup_failure = wordAt(vm, kSetupFailureAddr);
    return snapshot;
}

bool sameSnapshot(const RunSnapshot& a, const RunSnapshot& b) {
    return a.halted == b.halted &&
           a.return_value == b.return_value &&
           a.failure_count == b.failure_count &&
           a.failure_case == b.failure_case &&
           a.failure_pointer == b.failure_pointer &&
           a.failure_status == b.failure_status &&
           a.failure_payload == b.failure_payload &&
           a.failure_detail == b.failure_detail &&
           a.failure_reason == b.failure_reason &&
           a.failure_guard_before == b.failure_guard_before &&
           a.failure_guard_after == b.failure_guard_after &&
           a.sentinel_corruptions == b.sentinel_corruptions &&
           a.guard_corruptions == b.guard_corruptions &&
           a.setup_failure == b.setup_failure;
}

void printFailure(const RunSnapshot& snapshot) {
    if (snapshot.failure_count <= 0) return;
    std::cout << "syscall pointer fuzz failure seed=" << kSeed
              << " iterations=" << kIterations
              << " count=" << snapshot.failure_count
              << " case=" << snapshot.failure_case
              << " pointer=" << snapshot.failure_pointer
              << " status=" << snapshot.failure_status
              << " payload=" << snapshot.failure_payload
              << " detail=" << snapshot.failure_detail
              << " reason=" << snapshot.failure_reason
              << " guard_before=" << snapshot.failure_guard_before
              << " guard_after=" << snapshot.failure_guard_after << "\n";
}

void testRandomizedMalformedGuestPointers() {
    std::cout << "[1] Randomized malformed guest-pointer syscall harness\n";
    const std::string kernel = readTextFile("kernel.trit");
    expect(!kernel.empty(), "kernel.trit is present");
    if (kernel.empty()) return;

    const auto compiled = sandbox::compiler::compileSource(
        "syscall_pointer_fuzz.trit", kernel + "\n" + fuzzDriver());
    if (!compiled.success) {
        std::cerr << "SYSCALL POINTER FUZZ COMPILE DIAGNOSTICS:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "kernel and syscall pointer fuzz driver compile");
    if (!compiled.success) return;

    const auto linked = sandbox::compiler::linkModules({compiled.object});
    if (!linked.success) {
        for (const auto& diagnostic : linked.diagnostics) {
            std::cerr << "  " << diagnostic.format() << "\n";
        }
    }
    expect(linked.success, "syscall pointer fuzz driver links");
    if (!linked.success) return;

    const RunSnapshot first = runHarness(linked);
    const RunSnapshot second = runHarness(linked);
    expect(first.halted, "syscall pointer fuzz run halts within the step bound");
    expect(first.setup_failure == 0, "syscall pointer fuzz setup succeeds");
    expect(first.return_value == 1, "syscall pointer fuzz driver reports success");
    expect(first.failure_count == 0, "malformed pointers return safe errors");
    expect(first.sentinel_corruptions == 0,
           "malformed pointers cannot overwrite the kernel sentinel");
    expect(first.guard_corruptions == 0,
           "malformed pointers cannot mutate protected syscall state");
    expect(sameSnapshot(first, second),
           "syscall pointer fuzz diagnostics are reproducible for the fixed seed");
    printFailure(first);
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    testRandomizedMalformedGuestPointers();
    if (g_failures != 0) {
        std::cout << "\n" << g_failures
                  << " syscall pointer fuzz harness failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "\nSyscall pointer fuzz harness passed (seed=" << kSeed
              << ", iterations=" << kIterations << ")\n";
    return EXIT_SUCCESS;
}
