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

void testLayer1HalEndToEnd() {
    std::cout << "[1] Layer 1 bootloader and HAL end-to-end\n";
    using namespace sandbox::compiler;

    const std::string hal = readTextFile("kernel/hal.trit");
    expect(!hal.empty(), "hal.trit is present");

    const std::string driver = R"(
        fn kstore(addr: t40, val: t40) -> void {
            unsafe { store(addr, val); }
        }

        fn kload(addr: t40) -> t40 {
            var val: t40 = 0;
            unsafe { val = load(addr); }
            return val;
        }

        fn kernel_init() -> t40 {
            return 1;
        }

        fn kernel_dispatch(cause: t40) -> void {
            var ctx: t40 = kload(3019);
            kstore(ctx, kload(ctx) + 1);
            kstore(ctx + 18, 1);
        }

        fn main() -> t40 {
            var ok: t40 = 1;
            if hal_verify_dtb() <= 0 {
                ok = -1;
            }
            if hal_get_memory_size() - 1000000 != 0 {
                ok = -2;
            }
            if hal_get_console_out_id() - 22 != 0 {
                ok = -3;
            }
            if hal_get_console_in_id() - 24 != 0 {
                ok = -4;
            }
            if hal_get_console_in_ctrl_id() - 25 != 0 {
                ok = -5;
            }
            if hal_get_framebuffer_base() - 50000 != 0 {
                ok = -6;
            }
            var fb: t40 = hal_fb_init(200, 120, 3100);
            if fb - 3100 != 0 {
                ok = -7;
            }
            if kload(3100 + FB_DESC_WIDTH) - 320 != 0 {
                ok = -8;
            }
            if kload(3100 + FB_DESC_HEIGHT) - 180 != 0 {
                ok = -9;
            }
            if kload(3100 + FB_DESC_STRIDE) - 960 != 0 {
                ok = -10;
            }
            if kload(3100 + FB_DESC_FORMAT) - FB_FORMAT_T5_RGB != 0 {
                ok = -11;
            }
            if kload(3100 + FB_DESC_PAGES) - hal_fb_pages(320, 180) != 0 {
                ok = -12;
            }
            if hal_fb_flip(3100) - 2 != 0 {
                ok = -13;
            }
            return ok;
        }
    )";

    CompileResult compiled = compileSource("hal_e2e.trit", hal + "\n" + driver);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR HAL:\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << "\n";
        }
    }
    expect(compiled.success, "hal.trit plus driver compiles");

    const std::string bootloader = readTextFile("bootloader.tasm");
    expect(!bootloader.empty(), "bootloader.tasm is present");
    const std::string trap_stub = readTextFile("native_kernel_trap_stub.tasm");
    expect(!trap_stub.empty(), "native_kernel_trap_stub.tasm is present");

    if (compiled.success && !bootloader.empty() && !trap_stub.empty()) {
        auto boot_image = sandbox::vm::assembler::assemble(
            ".isa 2\n"
            ".require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50\n" +
            bootloader + "\n" + trap_stub + "\n" + compiled.assembly);
        if (!boot_image.success) {
            for (const auto& error : boot_image.errors) {
                std::cerr << "BOOTLOADER ASSEMBLY ERROR line " << error.line
                          << ": " << error.message << "\n";
            }
        }
        expect(boot_image.success, "bootloader image assembles");

        if (boot_image.success) {
            sandbox::vm::VMState vm(262144, 1000000);
            expect(sandbox::vm::assembler::loadAndReset(vm, boot_image), "bootloader image loads");
            
            // Run the bootloader which sets up the DTB, calls kernel_init, configures user context, triggers user syscall, takes the trap, and halts.
            const auto result = sandbox::vm::run(vm, 10000000);
            if (!result.halted()) {
                std::cout << "DEBUG: status=" << static_cast<int>(result.status)
                          << " pc=" << vm.pc
                          << " cause=" << vm.cause
                          << " epc=" << vm.epc
                          << " scratch=" << vm.scratch
                          << " priv=" << static_cast<int>(vm.privilege)
                          << " dmem_limit=" << vm.user_dmem_limit
                          << " sp=" << sandbox::vm::ops::toLong(vm.regfile.read(26))
                          << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
                          << " insn='" << sandbox::isa::disassemble(vm.imem.words[vm.pc]) << "'\n";
            }
            expect(result.halted(), "bootloader runs and halts");
            expect(regLong(vm, 13) == 1, "r13 returned 1 (success)");

            // Also test running main() directly as a standalone linked executable module
            LinkResult linked = linkModules({compiled.object});
            expect(linked.success, "hal plus driver links");
            if (linked.success) {
                sandbox::vm::VMState vm_direct(262144, 1000000);
                expect(sandbox::vm::assembler::loadAndReset(vm_direct, linked.assembled), "linked direct image loads");
                
                // Write the DTB manually so hal_verify_dtb succeeds in direct run
                vm_direct.dmem.store(2900, sandbox::vm::ops::fromLong(272727));
                vm_direct.dmem.store(2901, sandbox::vm::ops::fromLong(1000000));
                vm_direct.dmem.store(2902, sandbox::vm::ops::fromLong(22));
                vm_direct.dmem.store(2903, sandbox::vm::ops::fromLong(24));
                vm_direct.dmem.store(2904, sandbox::vm::ops::fromLong(25));
                vm_direct.dmem.store(2910, sandbox::vm::ops::fromLong(50000));
                vm_direct.dmem.store(2911, sandbox::vm::ops::fromLong(37));
                vm_direct.dmem.store(2912, sandbox::vm::ops::fromLong(640));
                vm_direct.dmem.store(2913, sandbox::vm::ops::fromLong(360));
                vm_direct.dmem.store(2914, sandbox::vm::ops::fromLong(5));
                
                const auto res_direct = sandbox::vm::run(vm_direct, 500000);
                expect(res_direct.halted(), "direct main run halts");
                if (regLong(vm_direct, 13) != 1) {
                    std::cout << "DEBUG direct HAL r13=" << regLong(vm_direct, 13)
                              << " fb_w=" << wordAt(vm_direct, 3100 + 1)
                              << " fb_h=" << wordAt(vm_direct, 3100 + 2)
                              << " fb_stride=" << wordAt(vm_direct, 3100 + 3)
                              << " fb_pages=" << wordAt(vm_direct, 3100 + 5)
                              << " pc=" << vm_direct.pc
                              << " trap=" << sandbox::vm::ops::toLong(vm_direct.trap_reg) << "\n";
                }
                expect(regLong(vm_direct, 13) == 1, "direct main execution returned success (1)");
            }
        }
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testLayer1HalEndToEnd();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "Layer 1 HAL tests passed\n";
    return 0;
}
