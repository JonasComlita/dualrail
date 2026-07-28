#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdlib>
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

sandbox::vm::VMState compileRun(
    const std::string& name,
    const std::string& source,
    long long expected_r13,
    const std::string& expected_output = std::string(),
    const std::string& console_input = std::string()) {

    using namespace sandbox::compiler;
    CompileResult compiled = compileSource(name, source);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR " << name << ":\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, name + " compiles");
    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, name + " links");
    sandbox::vm::VMState vm(65536, 1000000);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
               name + " image loads");
        if (!console_input.empty()) {
            vm.enqueueConsoleAscii(console_input);
        }
        const auto result = sandbox::vm::run(vm, 1000000);
        if (!result.halted()) {
            std::cout << "DEBUG " << name << ": status=" << static_cast<int>(result.status)
                      << " pc=" << vm.pc
                      << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
                      << " buffer='" << vm.syscall_buffer << "'\n";
        }
        expect(result.halted(), name + " halts");
        const long long actual_r13 = regLong(vm, 13);
        if (actual_r13 != expected_r13) {
            std::cout << "DEBUG " << name << ": r13=" << actual_r13
                      << " expected=" << expected_r13
                      << " dbg0=" << wordAt(vm, 9000)
                      << " dbg1=" << wordAt(vm, 9001)
                      << " dbg2=" << wordAt(vm, 9002)
                      << "\n";
        }
        expect(actual_r13 == expected_r13, name + " returns expected r13");
        if (!expected_output.empty()) {
            expect(contains(vm.syscall_buffer, expected_output),
                   name + " writes expected output");
        }
    }
    return vm;
}

void testLayer2ProcessModule() {
    std::cout << "[1] Layer 2 process/MMU module\n";
    const std::string process = readTextFile("kernel/process.trit");
    expect(!process.empty(), "process.trit is present");
    const std::string driver = R"(
        fn main() -> t40 {
            var root: t40 = 1000;
            var child_leaf: t40 = 51 * 27 + 2;
            proc2_bootstrap_table(root, 0, 50);
            proc2_map(root, 2, 700, PROC2_FLAG_USER + PROC2_FLAG_READ + PROC2_FLAG_WRITE);
            var phys: t40 = proc2_translate(root, 2 * 27 + 3, 1);
            var expected_phys: t40 = 18903;
            unsafe { store(9000, phys); store(9001, expected_phys); }
            if phys - expected_phys != 0 {
                return -1;
            }
            proc2_fork_leaf_as_cow(50 * 27 + 2, child_leaf);
            if proc2_translate(root, 2 * 27 + 3, 1) - PROC2_FAULT_COW != 0 {
                return -2;
            }
            if proc2_ptr_state_from_version(-1) + 1 != 0 {
                return -3;
            }
            if proc2_ptr_state_from_version(0) != 0 {
                return -4;
            }
            if proc2_ptr_state_from_version(9) - 1 != 0 {
                return -5;
            }
            return 1;
        }
    )";
    compileRun("production_process.trit", process + "\n" + driver, 1);
}

void testLayers3And4Modules() {
    std::cout << "[2] Layer 3 storage/VFS and Layer 4 sockets\n";
    const std::string bio = readTextFile("kernel/bio.trit");
    const std::string vfs = readTextFile("kernel/vfs.trit");
    const std::string net = readTextFile("kernel/net.trit");
    expect(!bio.empty(), "bio.trit is present");
    expect(!vfs.empty(), "vfs.trit is present");
    expect(!net.empty(), "net.trit is present");
    const std::string driver = R"(
        fn seed_path(addr: t40) -> t40 {
            unsafe {
                store(addr + 0, 47);
                store(addr + 1, 116);
                store(addr + 2, 109);
                store(addr + 3, 112);
                store(addr + 4, 0);
            }
            return addr;
        }

        fn main() -> t40 {
            bio_init(1000, 4);
            var row: t40 = bio_get(1000, 4, 7, 2000);
            var expected_row: t40 = 1015;
            unsafe { store(9000, row); store(9001, expected_row); }
            if row - expected_row != 0 {
                return -1;
            }
            bio_mark_dirty(row);
            if bio_load(row + BIO_DIRTY) - 1 != 0 {
                return -2;
            }
            var wal: t40 = bio_wal_append(1200, 4, 5, 7, 2100, 2200);
            if bio_wal_replay_needed(wal) - 1 != 0 {
                return -3;
            }
            bio_wal_commit(wal);
            if bio_wal_replay_needed(wal) != 0 {
                return -4;
            }

            seed_path(1400);
            if vfs2_strlen(1400, 16) - 4 != 0 {
                return -5;
            }
            vfs2_neutral_router(1400, 1420, 5);
            if vfs2_load(1420 + 1) - 116 != 0 {
                return -6;
            }
            vfs2_stat_pack(1440, VFS2_KIND_DIR, 3, 8);
            if vfs2_load(1441) - 3 != 0 {
                return -7;
            }

            net_init(1500, 2);
            var sock: t40 = net_socket_create(1500, 2, 1, 1, 1600);
            if sock != 0 {
                return -8;
            }
            net_bind(1500, sock, 8080);
            net_connect(1500, sock, 9090);
            if net_send_word(1500, sock, 4, 77) - 1 != 0 {
                return -9;
            }
            if net_recv_word(1500, sock, 4, 1700) - 1 != 0 {
                return -10;
            }
            if net_load(1700) - 77 != 0 {
                return -11;
            }
            return 1;
        }
    )";
    compileRun("production_storage_net.trit", bio + "\n" + vfs + "\n" + net + "\n" + driver, 1);
}

void testLayer5Libc() {
    std::cout << "[3] Layer 5 libc bridge\n";
    const std::string libc = readTextFile("ulib_c.trit");
    expect(!libc.empty(), "ulib_c.trit is present");
    const std::string driver = R"(
        fn main() -> t40 {
            var p: t40 = libc_malloc_words(8);
            if p <= 0 {
                return -1;
            }
            libc_store(p + 0, 111);
            libc_store(p + 1, 115);
            libc_store(p + 2, 0);
            if libc_strlen(p) - 2 != 0 {
                return -2;
            }
            libc_memcpy(p + 4, p, 3);
            if libc_strcmp(p, p + 4) != 0 {
                return -3;
            }
            return 1;
        }
    )";
    compileRun("production_libc.trit", libc + "\n" + driver, 1);
}

void testLayer6Apps() {
    std::cout << "[4] Layer 6 shell and compiler apps\n";
    const std::string libc = readTextFile("ulib_c.trit");
    const std::string ulibMini = readTextFile("ulib_mini.trit");
    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string token = readTextFile("tcl_token.trit");
    const std::string lexer = readTextFile("tcl_lexer.trit");
    const std::string frontend = readTextFile("tcl_frontend.trit");
    const std::string shell = readTextFile("apps/shell.trit");
    const std::string tcc = readTextFile("apps/tcc.trit");
    expect(!sdk.empty(), "os_sdk.trit is present");
    expect(!ulibMini.empty(), "ulib_mini.trit is present");
    expect(!token.empty(), "tcl_token.trit is present");
    expect(!lexer.empty(), "tcl_lexer.trit is present");
    expect(!frontend.empty(), "tcl_frontend.trit is present");
    expect(!shell.empty(), "shell.trit is present");
    expect(!tcc.empty(), "tcc.trit is present");
    compileRun("shell_app.trit", libc + "\n" + sdk + "\n" + shell,
               0, "TRIT SHELL\n", "exit\r");
    compileRun("tcc_app.trit",
               libc + "\n" + ulibMini + "\n" + sdk + "\n" + token + "\n" +
                   lexer + "\n" + frontend + "\n" + tcc,
               0, "TCC v0.1\n", "\r");
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testLayer2ProcessModule();
    testLayers3And4Modules();
    testLayer5Libc();
    testLayer6Apps();
    if (g_failures != 0) {
        std::cout << g_failures << " production layer test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Production layer module tests passed\n";
    return EXIT_SUCCESS;
}
