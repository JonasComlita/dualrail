#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr int kDesktopPhys = 140000;
constexpr int kLauncherPhys = 150000;
constexpr int kCalcTextPpn = 8200;
constexpr int kCalcTextPhys = kCalcTextPpn * sandbox::vm::MMU_PAGE_WORDS;
constexpr int kWindowProbeTextPpn = 8600;
constexpr int kWindowProbeTextPhys = kWindowProbeTextPpn * sandbox::vm::MMU_PAGE_WORDS;
constexpr int kHwPtBase = 19300;
constexpr int kHwPtMaxPages = 512;
constexpr int kProcessDmemPpnBase = 900;
constexpr int kFbStateBase = 23650;
constexpr int kFbFrontIndex = 5;
constexpr int kFbBackBufferBase = 43000;
constexpr int kCalcFbWidth = 20;
constexpr int kWindowUserVpnBase = 64;
constexpr int kWindowBufferPpn = 1690;
constexpr int kWindowUserMappedPages = 82;

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

void appendStoreWord(std::ostringstream& out, int addr, int offset, int value) {
    out << "    mov r1, " << value << "\n";
    out << "    mov r2, " << addr << "\n";
    out << "    store r1, r2, " << offset << "\n";
}

void appendStoreCString(std::ostringstream& out, int addr, const std::string& text) {
    for (int i = 0; i < static_cast<int>(text.size()); ++i) {
        appendStoreWord(out, addr, i, static_cast<unsigned char>(text[static_cast<std::size_t>(i)]));
    }
    appendStoreWord(out, addr, static_cast<int>(text.size()), 0);
}

sandbox::compiler::LinkResult compileApp(
    const std::string& app_name,
    int stack_words = 256) {

    using namespace sandbox::compiler;
    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string widget = readTextFile("apps/libwidget.trit");
    const std::string app = readTextFile("apps/" + app_name + ".trit");
    expect(!sdk.empty(), "app SDK source is present");
    expect(!widget.empty(), "widget library source is present");
    expect(!app.empty(), app_name + " source is present");

    CompileResult compiled = compileSource(app_name + ".trit", sdk + "\n" + widget + "\n" + app);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR " << app_name << ":\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, app_name + " compiles");

    LinkOptions options;
    options.stack_hint_words = stack_words;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    expect(linked.success, app_name + " links");
    if (linked.success) {
        expect(linked.executable_header.text_pages * sandbox::vm::MMU_PAGE_WORDS >=
                   linked.instruction_count,
               app_name + " executable header covers text image");
        expect(linked.executable_header.stack_words == stack_words,
               app_name + " executable header carries stack hint");
    }
    return linked;
}

sandbox::compiler::LinkResult compileInlineApp(
    const std::string& app_name,
    const std::string& source,
    int stack_words = 256) {

    using namespace sandbox::compiler;
    CompileResult compiled = compileSource(app_name + ".trit", source);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR " << app_name << ":\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, app_name + " compiles");

    LinkOptions options;
    options.stack_hint_words = stack_words;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    expect(linked.success, app_name + " links");
    return linked;
}

std::string buildBootAssembly(const sandbox::compiler::LinkResult& calc) {
    std::ostringstream boot;
    boot << ".text\n";
    boot << "boot:\n";
    boot << "    mov sp, 16383\n";
    boot << "    call kernel_init\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 0\n";
    boot << "    mov r15, 0\n";
    boot << "    call ipc_open\n";
    boot << "    mov r1, native_trap_entry\n";
    boot << "    csrw tvec, r1\n";

    appendStoreCString(boot, 10000, "/bin");
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10000\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_create\n";

    appendStoreCString(boot, 10020, "/bin/calculator");
    appendStoreWord(boot, 10100, 0, sandbox::vm::EXEC_MAGIC);
    appendStoreWord(boot, 10100, 1, sandbox::vm::EXEC_VERSION_V1);
    appendStoreWord(boot, 10100, 2, sandbox::vm::EXEC_ABI_VERSION_V1);
    appendStoreWord(boot, 10100, 3, calc.executable_header.entry_virtual_pc);
    appendStoreWord(boot, 10100, 4, calc.executable_header.text_pages);
    appendStoreWord(boot, 10100, 5, calc.executable_header.data_pages);
    appendStoreWord(boot, 10100, 6, calc.executable_header.stack_words);
    appendStoreWord(boot, 10100, 7, calc.executable_header.syscall_abi_version);
    appendStoreWord(boot, 10100, 8, calc.executable_header.flags);
    appendStoreWord(boot, 10100, 9, kCalcTextPpn);

    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 3\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10250\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10100\n";
    boot << "    mov r16, 10\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10250\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 1\n";
    boot << "    mov r16, 0\n";
    boot << "    mov r17, 1\n";
    boot << "    mov r18, 128\n";
    boot << "    call app_register\n";

    boot << "    mov r1, 12345\n";
    boot << "    mov r2, 62000\n";
    boot << "    store r1, r2, 0\n";

    boot << "    mov r1, ctx_desktop\n";
    boot << "    csrw scratch, r1\n";
    boot << "    mov sp, 12000\n";
    boot << "    mov r1, " << kDesktopPhys << "\n";
    boot << "    csrw epc, r1\n";
    boot << "    mov r1, 35\n";
    boot << "    csrw status, r1\n";
    boot << "    eret\n";
    boot << ".data\n";
    boot << ".org 64\n";
    boot << "ctx_desktop: .word 0, 35, 0, 0, 0, 0\n";
    boot << ".org 95\n";
    boot << ".word 12000\n";
    return boot.str();
}

std::string buildWindowProbeBootAssembly(const sandbox::compiler::LinkResult& probe) {
    std::ostringstream boot;
    boot << ".text\n";
    boot << "boot:\n";
    boot << "    mov sp, 16383\n";
    boot << "    call kernel_init\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 0\n";
    boot << "    mov r15, 0\n";
    boot << "    call ipc_open\n";
    boot << "    mov r1, native_trap_entry\n";
    boot << "    csrw tvec, r1\n";

    appendStoreCString(boot, 10000, "/bin");
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10000\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_create\n";

    appendStoreCString(boot, 10030, "/bin/window_probe");
    appendStoreWord(boot, 10100, 0, sandbox::vm::EXEC_MAGIC);
    appendStoreWord(boot, 10100, 1, sandbox::vm::EXEC_VERSION_V1);
    appendStoreWord(boot, 10100, 2, sandbox::vm::EXEC_ABI_VERSION_V1);
    appendStoreWord(boot, 10100, 3, probe.executable_header.entry_virtual_pc);
    appendStoreWord(boot, 10100, 4, probe.executable_header.text_pages);
    appendStoreWord(boot, 10100, 5, probe.executable_header.data_pages);
    appendStoreWord(boot, 10100, 6, probe.executable_header.stack_words);
    appendStoreWord(boot, 10100, 7, probe.executable_header.syscall_abi_version);
    appendStoreWord(boot, 10100, 8, probe.executable_header.flags);
    appendStoreWord(boot, 10100, 9, kWindowProbeTextPpn);

    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10030\n";
    boot << "    mov r15, 3\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10030\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10250\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10100\n";
    boot << "    mov r16, 10\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10250\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10030\n";
    boot << "    mov r15, 9\n";
    boot << "    mov r16, 0\n";
    boot << "    mov r17, 1\n";
    boot << "    mov r18, 128\n";
    boot << "    call app_register\n";

    appendStoreCString(boot, 10300, "/probe");
    appendStoreWord(boot, 10320, 0, 80);
    appendStoreWord(boot, 10320, 1, 82);
    appendStoreWord(boot, 10320, 2, 79);
    appendStoreWord(boot, 10320, 3, 66);
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10300\n";
    boot << "    mov r15, 1\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10300\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10330\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10320\n";
    boot << "    mov r16, 4\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10330\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";

    boot << "    mov r1, ctx_launcher\n";
    boot << "    csrw scratch, r1\n";
    boot << "    mov sp, 12000\n";
    boot << "    mov r1, " << kLauncherPhys << "\n";
    boot << "    csrw epc, r1\n";
    boot << "    mov r1, 35\n";
    boot << "    csrw status, r1\n";
    boot << "    eret\n";
    boot << ".data\n";
    boot << ".org 64\n";
    boot << "ctx_launcher: .word 0, 35, 0, 0, 0, 0\n";
    boot << ".org 95\n";
    boot << ".word 12000\n";
    return boot.str();
}

void testDesktopLaunchesMappedCalculator() {
    std::cout << "[1] Desktop-triggered process image handoff\n";
    using namespace sandbox::compiler;

    const std::string kernel = readTextFile("kernel.trit");
    const std::string trap = readTextFile("OS3/native_kernel_trap_stub.tasm");
    expect(!kernel.empty(), "kernel source is present");
    expect(!trap.empty(), "native trap stub is present");

    CompileResult compiled_kernel = compileSource("kernel.trit", kernel);
    if (!compiled_kernel.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR KERNEL:\n";
        dumpDiagnostics(compiled_kernel);
    }
    expect(compiled_kernel.success, "kernel compiles");

    LinkResult desktop = compileApp("desktop", 512);
    LinkResult calc = compileApp("calculator", 256);
    if (!compiled_kernel.success || !desktop.success || !calc.success) return;
    expect(calc.executable_header.text_pages <= kHwPtMaxPages,
           "dead-stripped calculator image fits the current IMEM page-table contract");

    const std::string image =
        buildBootAssembly(calc) + "\n" +
        trap + "\n" +
        compiled_kernel.assembly + "\n" +
        ".text\n.org " + std::to_string(kDesktopPhys) + "\n" +
        desktop.assembly + "\n";

    auto assembled = sandbox::vm::assembler::assemble(image);
    if (!assembled.success) {
        std::cerr << "NATIVE DESKTOP IMAGE ASSEMBLY FAILED:\n";
        for (const auto& error : assembled.errors) {
            std::cerr << "  line " << error.line << ": " << error.message << "\n";
        }
    }
    expect(assembled.success, "native desktop boot image assembles");

    sandbox::vm::VMState vm(262144, 1000000);
    if (!assembled.success) return;
    expect(sandbox::vm::assembler::loadAndReset(vm, assembled),
           "native desktop boot image loads");
    expect(vm.imem.loadProgram(calc.assembled.program, kCalcTextPhys),
           "calculator text image is installed at its physical IMEM pages");
    vm.enqueueConsoleAscii("1a");

    const auto result = sandbox::vm::run(vm, 8000000);
    const bool mapped_handoff =
        vm.mmu_enable &&
        vm.user_imem_ptbr == kHwPtBase &&
        vm.user_dmem_ptbr == kHwPtBase + kHwPtMaxPages;
    if (!result.halted() && !mapped_handoff) {
        std::string nearest = "<none>";
        int nearest_pc = -1;
        for (const auto& [label, pc] : assembled.labels) {
            if (pc <= vm.pc && pc > nearest_pc) {
                nearest = label;
                nearest_pc = pc;
            }
        }
        std::cout << "DEBUG handoff: status=" << static_cast<int>(result.status)
                  << " pc=" << vm.pc
                  << " nearest=" << nearest << "@" << nearest_pc
                  << " cause=" << vm.cause
                  << " epc=" << vm.epc
                  << " pf_addr=" << vm.page_fault_addr
                  << " pf_access=" << vm.page_fault_access
                  << " syscall=" << vm.syscall_id
                  << " priv=" << static_cast<int>(vm.privilege)
                  << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
                  << " insn='" << sandbox::isa::disassemble(vm.imem.words[vm.pc]) << "'"
                  << " epc_insn='" << sandbox::isa::disassemble(vm.imem.words[vm.epc]) << "'"
                  << " buffer='" << vm.syscall_buffer << "'\n";
    }
    expect(result.halted() || mapped_handoff,
           "desktop launch reaches mapped calculator handoff");
    if (result.halted() && !contains(vm.syscall_buffer, "144 + 12 = 156\n")) {
        std::cout << "DEBUG handoff halted=" << result.halted()
                  << " pc=" << vm.pc
                  << " r13=" << sandbox::vm::ops::toLong(vm.regfile.read(13))
                  << " r14=" << sandbox::vm::ops::toLong(vm.regfile.read(14))
                  << " r15=" << sandbox::vm::ops::toLong(vm.regfile.read(15))
                  << " mmu=" << vm.mmu_enable
                  << " imem_ptbr=" << vm.user_imem_ptbr
                  << " dmem_ptbr=" << vm.user_dmem_ptbr
                  << " buffer='" << vm.syscall_buffer << "'\n";
    }
    expect(contains(vm.syscall_buffer, "DESKTOP\n"), "desktop ran before launch");
    if (result.halted()) {
        expect(contains(vm.syscall_buffer, "144 + 12 = 156\n"),
               "mapped calculator ran after sys_exec");
    }
    expect(vm.mmu_enable, "exec handoff enables the MMU for the launched image");
    expect(vm.user_imem_ptbr == kHwPtBase,
           "calculator context installs per-process IMEM page table");
    expect(vm.user_dmem_ptbr == kHwPtBase + kHwPtMaxPages,
           "calculator context installs per-process DMEM page table");

    expect(wordAt(vm, kFbStateBase + kFbFrontIndex) == 1,
           "desktop-launched calculator presents a compositor frame");
    const int calc_pixel = kFbBackBufferBase + (3 * kCalcFbWidth + 10) * 3;
    expect(wordAt(vm, calc_pixel) == 100,
           "desktop-launched calculator status red channel reaches visible framebuffer");
    expect(wordAt(vm, calc_pixel + 1) == 88,
           "desktop-launched calculator status green channel reaches visible framebuffer");
    expect(wordAt(vm, calc_pixel + 2) == 24,
           "desktop-launched calculator status blue channel reaches visible framebuffer");
    expect(vm.user_dmem_pages >= kWindowUserMappedPages,
           "desktop-launched calculator maps its window buffer into user DMEM");

    auto [calc_window_pte_value, calc_window_pte_fault] =
        vm.dmem.load(kHwPtBase + kHwPtMaxPages + kWindowUserVpnBase);
    expect(calc_window_pte_fault == sandbox::vm::MemFaultCode::OK,
           "calculator window buffer PTE can be read");
    sandbox::vm::PageTableEntry calc_window_pte;
    expect(sandbox::vm::decodePageTableEntry(calc_window_pte_value, calc_window_pte) &&
               calc_window_pte.present && calc_window_pte.user && calc_window_pte.read &&
               calc_window_pte.write && calc_window_pte.ppn == kWindowBufferPpn,
           "calculator window buffer user VPN maps to the compositor backing buffer");

    auto [imem_pte_value, imem_pte_fault] = vm.dmem.load(kHwPtBase);
    if (sandbox::vm::ops::toLong(imem_pte_value) == 0 ||
        (result.halted() && !contains(vm.syscall_buffer, "144 + 12 = 156\n"))) {
        std::cout << "DEBUG pte imem_word=" << sandbox::vm::ops::toLong(imem_pte_value)
                  << " dmem_word=" << sandbox::vm::ops::toLong(vm.dmem.load(kHwPtBase + kHwPtMaxPages).first)
                  << " calc_text_pages=" << calc.executable_header.text_pages
                  << " calc_data_pages=" << calc.executable_header.data_pages
                  << " calc_program_words=" << calc.instruction_count << "\n";
    }
    expect(imem_pte_fault == sandbox::vm::MemFaultCode::OK,
           "IMEM PTE can be read");
    sandbox::vm::PageTableEntry imem_pte;
    expect(sandbox::vm::decodePageTableEntry(imem_pte_value, imem_pte) &&
               imem_pte.present && imem_pte.user && imem_pte.execute &&
               imem_pte.ppn == kCalcTextPpn,
           "IMEM PTE maps virtual page zero to the calculator text page");

    auto [dmem_pte_value, dmem_pte_fault] = vm.dmem.load(kHwPtBase + kHwPtMaxPages);
    expect(dmem_pte_fault == sandbox::vm::MemFaultCode::OK,
           "DMEM PTE can be read");
    sandbox::vm::PageTableEntry dmem_pte;
    expect(sandbox::vm::decodePageTableEntry(dmem_pte_value, dmem_pte) &&
               dmem_pte.present && dmem_pte.user && dmem_pte.read &&
               dmem_pte.write && dmem_pte.ppn == kProcessDmemPpnBase,
           "DMEM PTE maps virtual data page zero to the process data page");
}

void testWindowProbeRunsThroughMappedWindowBuffer() {
    std::cout << "[2] Windowed app maps compositor buffer through process handoff\n";
    using namespace sandbox::compiler;

    const std::string kernel = readTextFile("kernel.trit");
    const std::string trap = readTextFile("OS3/native_kernel_trap_stub.tasm");
    expect(!kernel.empty(), "kernel source is present for window probe");
    expect(!trap.empty(), "native trap stub is present for window probe");

    CompileResult compiled_kernel = compileSource("kernel.trit", kernel);
    if (!compiled_kernel.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR WINDOW PROBE KERNEL:\n";
        dumpDiagnostics(compiled_kernel);
    }
    expect(compiled_kernel.success, "kernel compiles for window probe");

    LinkResult probe = compileApp("window_probe", 256);
    const std::string launcher_source = R"(
        fn seed_window_probe_path(addr: t40) -> t40 {
            unsafe {
                store(addr + 0, 47);
                store(addr + 1, 98);
                store(addr + 2, 105);
                store(addr + 3, 110);
                store(addr + 4, 47);
                store(addr + 5, 119);
                store(addr + 6, 105);
                store(addr + 7, 110);
                store(addr + 8, 100);
                store(addr + 9, 111);
                store(addr + 10, 119);
                store(addr + 11, 95);
                store(addr + 12, 112);
                store(addr + 13, 114);
                store(addr + 14, 111);
                store(addr + 15, 98);
                store(addr + 16, 101);
                store(addr + 17, 0);
            }
            return addr;
        }

        fn main() -> t40 {
            var path: t40 = seed_window_probe_path(10000);
            sys_exec(path);
            return -1;
        }
    )";
    LinkResult launcher = compileInlineApp("window_probe_launcher", launcher_source, 128);
    if (!compiled_kernel.success || !probe.success || !launcher.success) return;

    const std::string image =
        buildWindowProbeBootAssembly(probe) + "\n" +
        trap + "\n" +
        compiled_kernel.assembly + "\n" +
        ".text\n.org " + std::to_string(kLauncherPhys) + "\n" +
        launcher.assembly + "\n";

    auto assembled = sandbox::vm::assembler::assemble(image);
    if (!assembled.success) {
        std::cerr << "WINDOW PROBE IMAGE ASSEMBLY FAILED:\n";
        for (const auto& error : assembled.errors) {
            std::cerr << "  line " << error.line << ": " << error.message << "\n";
        }
    }
    expect(assembled.success, "window probe boot image assembles");

    sandbox::vm::VMState vm(262144, 1000000);
    if (!assembled.success) return;
    expect(sandbox::vm::assembler::loadAndReset(vm, assembled),
           "window probe boot image loads");
    expect(vm.imem.loadProgram(probe.assembled.program, kWindowProbeTextPhys),
           "window probe text image is installed at its physical IMEM pages");

    const auto result = sandbox::vm::run(vm, 12000000);
    if (!result.halted()) {
        std::cout << "DEBUG window probe: status=" << static_cast<int>(result.status)
                  << " pc=" << vm.pc
                  << " cause=" << vm.cause
                  << " epc=" << vm.epc
                  << " pf_addr=" << vm.page_fault_addr
                  << " pf_access=" << vm.page_fault_access
                  << " syscall=" << vm.syscall_id
                  << " mmu=" << vm.mmu_enable
                  << " dmem_pages=" << vm.user_dmem_pages
                  << " buffer='" << vm.syscall_buffer << "'\n";
    }
    expect(result.halted(), "window probe app halts after drawing");
    expect(contains(vm.syscall_buffer, "WINDOW\n"),
           "window probe app ran after sys_exec");
    expect(contains(vm.syscall_buffer, "FS\n"),
           "window probe read a VFS file through translated process DMEM");
    expect(contains(vm.syscall_buffer, "DIR\n"),
           "window probe read a VFS directory through translated process DMEM");
    expect(contains(vm.syscall_buffer, "IPC\n"),
           "window probe received IPC through translated process DMEM");
    expect(contains(vm.syscall_buffer, "WR\n"),
           "window probe wrote a VFS file from translated process DMEM");
    expect(contains(vm.syscall_buffer, "CLOSE\n"),
           "window probe received a cooperative close event");
    expect(wordAt(vm, kFbStateBase + kFbFrontIndex) == 1,
           "window probe present flips the framebuffer");
    const int pixel = kFbBackBufferBase + (1 * 8 + 1) * 3;
    expect(wordAt(vm, pixel) == 70, "window probe red channel reached visible framebuffer");
    expect(wordAt(vm, pixel + 1) == 71, "window probe green channel reached visible framebuffer");
    expect(wordAt(vm, pixel + 2) == 72, "window probe blue channel reached visible framebuffer");
    const int resized_pixel = kFbBackBufferBase + (3 * 8 + 4) * 3;
    expect(wordAt(vm, resized_pixel) == 70,
           "window probe resized red channel reached visible framebuffer");
    expect(wordAt(vm, resized_pixel + 1) == 71,
           "window probe resized green channel reached visible framebuffer");
    expect(wordAt(vm, resized_pixel + 2) == 72,
           "window probe resized blue channel reached visible framebuffer");
    expect(vm.user_dmem_pages >= kWindowUserMappedPages,
           "window buffer mapping expands user DMEM page span");

    auto [window_pte_value, window_pte_fault] =
        vm.dmem.load(kHwPtBase + kHwPtMaxPages + kWindowUserVpnBase);
    expect(window_pte_fault == sandbox::vm::MemFaultCode::OK,
           "window buffer PTE can be read");
    sandbox::vm::PageTableEntry window_pte;
    expect(sandbox::vm::decodePageTableEntry(window_pte_value, window_pte) &&
               window_pte.present && window_pte.user && window_pte.read &&
               window_pte.write && window_pte.ppn == kWindowBufferPpn,
           "window buffer user VPN maps to the compositor backing buffer");
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testDesktopLaunchesMappedCalculator();
    testWindowProbeRunsThroughMappedWindowBuffer();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "Process handoff tests passed\n";
    return 0;
}
