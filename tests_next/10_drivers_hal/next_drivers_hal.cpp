#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::os;

std::string formatDiagnostics(const std::vector<sandbox::compiler::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

long long csrLong(sandbox::vm::VMState& vm, int csr) {
    sandbox::vm::TernaryValue value;
    if (!vm.readCSR(csr, value)) return -999999;
    return sandbox::vm::ops::toLong(value);
}

bool writeCsr(sandbox::vm::VMState& vm, int csr, long long value) {
    return vm.writeCSR(csr, sandbox::vm::ops::fromLong(value));
}

long long wordAt(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return -999999;
    return sandbox::vm::ops::toLong(value);
}

std::string halRuntimeDriver() {
    return R"(
        fn kstore(addr: t40, val: t40) -> void {
            unsafe { store(addr, val); }
        }

        fn kload(addr: t40) -> t40 {
            var val: t40 = 0;
            unsafe { val = load(addr); }
            return val;
        }

        fn seed_dtb() -> void {
            kstore(DTB_BASE, DTB_MAGIC);
            kstore(DTB_BASE + DTB_MEMORY_SIZE, 1000000);
            kstore(DTB_BASE + DTB_CONSOLE_OUT, 22);
            kstore(DTB_BASE + DTB_CONSOLE_IN, 24);
            kstore(DTB_BASE + DTB_CONSOLE_IN_CTRL, 25);
            kstore(DTB_BASE + DTB_FRAMEBUFFER_BASE, 50000);
            kstore(DTB_BASE + DTB_FRAMEBUFFER_CTRL, 37);
            kstore(DTB_BASE + DTB_FRAMEBUFFER_MAX_W, 640);
            kstore(DTB_BASE + DTB_FRAMEBUFFER_MAX_H, 360);
            kstore(DTB_BASE + DTB_FRAMEBUFFER_FORMAT, FB_FORMAT_T5_RGB);
            kstore(DTB_BASE + DTB_BLOCK_INDEX, 41);
            kstore(DTB_BASE + DTB_BLOCK_ADDR, 42);
            kstore(DTB_BASE + DTB_BLOCK_CMD, 43);
            kstore(DTB_BASE + DTB_BLOCK_STATUS, 44);
        }

        fn main() -> t40 {
            var ok: t40 = 1;
            if hal_verify_dtb() + 1 != 0 {
                ok = -1;
            }
            if hal_get_memory_size() - 65536 != 0 {
                ok = -2;
            }
            if hal_get_console_out_id() - 22 != 0 {
                ok = -3;
            }
            if hal_get_framebuffer_max_width() - 640 != 0 {
                ok = -4;
            }

            seed_dtb();
            if hal_verify_dtb() - 1 != 0 {
                ok = -5;
            }
            if hal_get_memory_size() - 1000000 != 0 {
                ok = -6;
            }
            if hal_get_block_index_id() - 41 != 0 {
                ok = -7;
            }
            if hal_get_block_addr_id() - 42 != 0 {
                ok = -8;
            }
            if hal_get_block_cmd_id() - 43 != 0 {
                ok = -9;
            }
            if hal_get_block_status_id() - 44 != 0 {
                ok = -10;
            }

            var fb: t40 = hal_fb_init(200, 120, 3100);
            if fb - 3100 != 0 {
                ok = -11;
            }
            if kload(3100 + FB_DESC_PHYS_BASE) - 50000 != 0 {
                ok = -12;
            }
            if kload(3100 + FB_DESC_WIDTH) - 320 != 0 {
                ok = -13;
            }
            if kload(3100 + FB_DESC_HEIGHT) - 180 != 0 {
                ok = -14;
            }
            if kload(3100 + FB_DESC_STRIDE) - 960 != 0 {
                ok = -15;
            }
            if kload(3100 + FB_DESC_FORMAT) - FB_FORMAT_T5_RGB != 0 {
                ok = -16;
            }
            if kload(3100 + FB_DESC_PAGES) - hal_fb_pages(320, 180) != 0 {
                ok = -17;
            }
            if hal_fb_flip(3100) - 2 != 0 {
                ok = -18;
            }
            if kload(3100 + FB_DESC_EPOCH) - 2 != 0 {
                ok = -19;
            }
            if hal_get_block_words() - 27 != 0 {
                ok = -20;
            }
            if hal_get_block_count() <= 0 {
                ok = -21;
            }

            hal_write_console_char(65);
            hal_write_console_int(99);
            if hal_read_console_char() - 90 != 0 {
                ok = -22;
            }
            if hal_read_console_char() + 1 != 0 {
                ok = -23;
            }
            hal_enable_timer(7);
            hal_disable_timer();
            hal_clear_timer_pending();
            return ok;
        }
    )";
}

void deviceTreeValidation(TestContext& ctx) {
    DeviceTree tree = defaultDeviceTree(32);
    std::vector<std::string> errors;
    ctx.check(tree.validate(&errors).ok(), "default device tree validates");
    const DeviceNode* console = tree.find("console");
    const DeviceNode* timer = tree.find("timer");
    const DeviceNode* block = tree.find("block0");
    ctx.check(console != nullptr && console->compatible == "trit,console-v1",
              "console node is discoverable");
    ctx.check(timer != nullptr && timer->compatible == "trit,timer-v1",
              "timer node is discoverable");
    ctx.check(block != nullptr && block->properties.at("block_words") == BLOCK_WORDS,
              "block node records ternary page-sized blocks");
    ctx.check(block != nullptr && block->properties.at("block_count") == 32,
              "block node records block count");

    tree.add(DeviceNode{"console", "trit,console-v1", {}});
    errors.clear();
    ctx.equal(tree.validate(&errors).detail, ERR_INVALID,
              "duplicate device node fails validation");
    ctx.check(!errors.empty() && errors[0].find("duplicate") != std::string::npos,
              "duplicate diagnostic names the duplicate");

    DeviceTree missing;
    missing.add(DeviceNode{"console", "trit,console-v1", {}});
    errors.clear();
    ctx.equal(missing.validate(&errors).detail, ERR_INVALID,
              "missing required devices fail validation");
    ctx.check(errors.size() >= 2, "missing-device validation reports each gap");

    DeviceTree bad_geometry;
    bad_geometry.add(DeviceNode{"console", "trit,console-v1", {}});
    bad_geometry.add(DeviceNode{"timer", "trit,timer-v1", {}});
    bad_geometry.add(DeviceNode{"block0", "trit,block-v1",
                                {{"block_words", 9}, {"block_count", 0}}});
    errors.clear();
    ctx.equal(bad_geometry.validate(&errors).detail, ERR_INVALID,
              "bad block geometry fails validation");
    ctx.check(errors.size() >= 2,
              "block geometry reports bad word size and count");
}

void vmDeviceCsrIo(TestContext& ctx) {
    sandbox::vm::VMState vm(128, 512);
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_CTRL, -1),
              "console control clears buffer");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_CTRL, 2),
              "console char mode enables");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_OUT, 72),
              "console writes char");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_CTRL, 3),
              "console char mode disables");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_OUT, 17),
              "console writes integer text");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_CTRL, 1),
              "console newline writes");
    ctx.equal(vm.syscall_buffer, std::string("H17\n"),
              "console CSR preserves char, integer, and newline modes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_CONSOLE_CTRL), 4LL,
              "console control reads buffered output length");

    vm.enqueueConsoleAscii("K");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_CONSOLE_IN_CTRL), 1LL,
              "console input control reports queued input");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_CONSOLE_IN), 75LL,
              "console input peeks first queued character");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_CONSOLE_IN_CTRL, 1),
              "console input control consumes a word");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_CONSOLE_IN_CTRL), 0LL,
              "console input control clears after consume");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_CONSOLE_IN), -1LL,
              "empty console input reads sentinel");

    ctx.check(writeCsr(vm, sandbox::isa::CSR_TIMER_RELOAD, 5),
              "timer reload writes");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_TIMER_COUNTER, 0),
              "timer counter writes zero");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_TIMER_ENABLE, 1),
              "timer enable writes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_TIMER_COUNTER), 5LL,
              "enabling timer reloads zero counter");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_TIMER_PENDING, 1),
              "timer pending writes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_TIMER_PENDING), 1LL,
              "timer pending reads true");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_TIMER_PENDING, 0),
              "timer pending clears");

    ctx.check(writeCsr(vm, sandbox::isa::CSR_MOUSE_X, 12),
              "mouse x writes");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_MOUSE_Y, 34),
              "mouse y writes");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_MOUSE_BTN, 1),
              "mouse button writes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_MOUSE_X), 12LL,
              "mouse x reads");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_MOUSE_Y), 34LL,
              "mouse y reads");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_MOUSE_BTN), 1LL,
              "mouse button reads");

    ctx.equal(csrLong(vm, sandbox::isa::CSR_GPU_DRAW_BASE), 55000LL,
              "front GPU page exposes front draw base");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_GPU_PAGE, 1),
              "GPU page writes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_GPU_DRAW_BASE), 50000LL,
              "back GPU page exposes alternate draw base");
    ctx.check(!writeCsr(vm, sandbox::isa::CSR_GPU_DRAW_BASE, 123),
              "GPU draw base is read-only");
}

void vmBlockCsrBoundaries(TestContext& ctx) {
    sandbox::vm::VMState vm(64, 512);
    ctx.equal(csrLong(vm, sandbox::isa::CSR_BLOCK_WORDS),
              static_cast<long long>(sandbox::vm::MMU_PAGE_WORDS),
              "block words CSR reports ternary page size");
    ctx.check(csrLong(vm, sandbox::isa::CSR_BLOCK_COUNT) >= 141,
              "block count CSR reports persistent native VFS capacity");
    ctx.check(!writeCsr(vm, sandbox::isa::CSR_BLOCK_COUNT, 1),
              "block count CSR is read-only");
    ctx.check(!writeCsr(vm, sandbox::isa::CSR_BLOCK_WORDS, 1),
              "block words CSR is read-only");

    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_INDEX, 3),
              "block index writes");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_ADDR, 200),
              "block address writes");
    for (int i = 0; i < sandbox::vm::MMU_PAGE_WORDS; ++i) {
        ctx.check(vm.dmem.store(200 + i, sandbox::vm::ops::fromLong(700 + i)) ==
                      sandbox::vm::MemFaultCode::OK,
                  "block payload stores into DMEM");
    }
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_CMD, 2),
              "block write command accepts");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_BLOCK_STATUS), 1LL,
              "block write command succeeds");
    ctx.check(vm.block_dirty[3], "block write marks dirty state");

    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_INDEX, -1),
              "invalid block index writes into CSR");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_CMD, 1),
              "invalid block read command executes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_BLOCK_STATUS), -1LL,
              "invalid block index fails closed");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_INDEX, 3),
              "block index restores");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_ADDR, 512),
              "out-of-range block transfer address writes");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_CMD, 1),
              "out-of-range block read command executes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_BLOCK_STATUS), -1LL,
              "out-of-range block transfer fails closed");

    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_STATUS, 77),
              "block status can be staged by kernel code");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_BLOCK_STATUS), 77LL,
              "block status staging is readable");
    ctx.check(writeCsr(vm, sandbox::isa::CSR_BLOCK_CMD, 4),
              "block flush command executes");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_BLOCK_STATUS), 1LL,
              "block flush command succeeds");
    ctx.check(!vm.block_dirty[3], "block flush clears dirty state");
}

void tritHalRuntime(TestContext& ctx) {
    const std::string hal = tests_next::readText("kernel/hal.trit");
    ctx.check(!hal.empty(), "kernel/hal.trit is present");
    sandbox::compiler::CompileResult compiled =
        sandbox::compiler::compileSource("next_drivers_hal.trit",
                                         hal + "\n" + halRuntimeDriver());
    if (!compiled.success) {
        ctx.fail("HAL runtime diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
        return;
    }
    ctx.contains(compiled.assembly, "csrw console_out",
                 "HAL console helper writes console_out CSR");
    ctx.contains(compiled.assembly, "csrr",
                 "HAL console input helper reads console_in CSR");
    ctx.contains(compiled.assembly, "console_in",
                 "HAL console input helper names console_in CSR");
    ctx.contains(compiled.assembly, "csrw timer_reload",
                 "HAL timer helper writes timer_reload CSR");
    ctx.contains(compiled.assembly, "csrr",
                 "HAL block helper reads block_words CSR");
    ctx.contains(compiled.assembly, "block_words",
                 "HAL block helper names block_words CSR");

    sandbox::compiler::LinkResult linked =
        sandbox::compiler::linkModules({compiled.object});
    ctx.check(linked.success, "HAL runtime driver links");
    if (!linked.success) {
        ctx.fail("HAL link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return;
    }

    sandbox::vm::VMState vm(262144, 1000000);
    ctx.check(sandbox::vm::loadAndReset(vm, linked.assembled.program),
              "HAL runtime image loads");
    vm.enqueueConsoleAscii("Z");
    const auto result = sandbox::vm::run(vm, 1000000);
    if (!result.halted()) {
        std::ostringstream out;
        out << "HAL runtime driver did not halt; status="
            << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return;
    }

    ctx.equal(sandbox::vm::ops::toLong(vm.regfile.read(13)), 1LL,
              "HAL runtime driver returns ok");
    ctx.equal(vm.syscall_buffer, std::string("A99"),
              "HAL console helpers preserve char and integer output");
    ctx.equal(wordAt(vm, 3100 + 0), 50000LL,
              "HAL framebuffer descriptor records physical base");
    ctx.equal(wordAt(vm, 3100 + 1), 320LL,
              "HAL framebuffer descriptor selects mode width");
    ctx.equal(wordAt(vm, 3100 + 2), 180LL,
              "HAL framebuffer descriptor selects mode height");
    ctx.equal(wordAt(vm, 3100 + 6), 2LL,
              "HAL framebuffer flip increments epoch");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_TIMER_ENABLE), 0LL,
              "HAL timer helpers disable timer at end");
    ctx.equal(csrLong(vm, sandbox::isa::CSR_TIMER_PENDING), 0LL,
              "HAL timer helpers clear pending state");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"drivers.hal.device_tree_validation", "drivers.hal_contract",
         deviceTreeValidation},
        {"drivers.hal.vm_device_csr_io", "drivers.hal_contract",
         vmDeviceCsrIo},
        {"drivers.hal.block_csr_boundaries", "drivers.hal_contract",
         vmBlockCsrBoundaries},
        {"drivers.hal.trit_dtb_framebuffer_runtime", "drivers.hal_contract",
         tritHalRuntime},
    };
    return tests_next::runCases("next_drivers_hal", cases);
}
