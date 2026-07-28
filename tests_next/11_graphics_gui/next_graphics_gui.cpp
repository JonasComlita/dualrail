#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_host_runtime.h"
#include "ternary_vm.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;

std::string formatDiagnostics(const std::vector<sandbox::compiler::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

sandbox::host::TosBootImage assembleImage(
    TestContext& ctx,
    const std::string& source,
    sandbox::host::TosImageManifest manifest = {}) {

    auto assembled = sandbox::vm::assembler::assemble(source);
    if (!assembled.success) {
        std::ostringstream out;
        for (const auto& error : assembled.errors) {
            out << "line " << error.line << ": " << error.message << "\n";
        }
        ctx.fail("boot image assembly failed:\n" + out.str());
        return {};
    }
    auto boot = assembled.labels.find("boot");
    if (boot != assembled.labels.end()) manifest.boot_entry = boot->second;
    if (manifest.image_version.empty() || manifest.image_version == "dev") {
        manifest.image_version = "tests_next_gui";
    }
    if (manifest.profile_name.empty() || manifest.profile_name == "minimum") {
        manifest.profile_name = "compact";
    }
    return sandbox::host::bootImageFromAssembly(assembled, std::move(manifest));
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

long long wordAt(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return -999999;
    return sandbox::vm::ops::toLong(value);
}

std::string expectHelpers(int base) {
    return R"(
        fn expect_eq(actual: t40, expected: t40, ok: t40) -> t40 {
            kstore()" + std::to_string(base + 3) + R"(, kload()" + std::to_string(base + 3) + R"() + 1);
            if actual - expected == 0 {
                return ok;
            }
            if kload()" + std::to_string(base + 0) + R"() == 0 {
                kstore()" + std::to_string(base + 0) + R"(, 1);
                kstore()" + std::to_string(base + 1) + R"(, actual);
                kstore()" + std::to_string(base + 2) + R"(, expected);
                kstore()" + std::to_string(base + 4) + R"(, kload()" + std::to_string(base + 3) + R"());
            }
            return 0;
        }
    )";
}

std::string compositorDriver() {
    return expectHelpers(35300) + R"(
        fn seed_rgb(addr: t40, r: t40, g: t40, b: t40) -> t40 {
            kstore(addr + 0, r);
            kstore(addr + 1, g);
            kstore(addr + 2, b);
            return addr;
        }

        fn main() -> t40 {
            var ok: t40 = 1;
            ok = expect_eq(kernel_init(), 1, ok);

            kernel_syscall_dispatch(1, 25, 8, 6, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(FB_STATE_BASE + FB_WIDTH), 8, ok);
            ok = expect_eq(kload(FB_STATE_BASE + FB_HEIGHT), 6, ok);

            kernel_syscall_dispatch(1, 27, 1, 1, 3, 2);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            var low: t40 = kload(SYS_PAYLOAD_ADDR);
            kernel_syscall_dispatch(1, 28, low, 0, 0, 0);
            var low_buf: t40 = kload(window_addr(window_find(low)) + WIN_BUFFER_ADDR);
            seed_rgb(low_buf, 11, 12, 13);
            kernel_syscall_dispatch(1, 29, low, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            var visible: t40 = framebuffer_visible_base();
            var pixel: t40 = visible + (1 * 8 + 1) * 3;
            ok = expect_eq(kload(pixel), 11, ok);
            ok = expect_eq(kload(pixel + 1), 12, ok);
            ok = expect_eq(kload(pixel + 2), 13, ok);
            ok = expect_eq(kload(window_addr(window_find(low)) + WIN_DIRTY), 0, ok);

            kernel_syscall_dispatch(1, 27, 1, 1, 3, 2);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            var high: t40 = kload(SYS_PAYLOAD_ADDR);
            kernel_syscall_dispatch(1, 28, high, 0, 0, 0);
            var high_buf: t40 = kload(window_addr(window_find(high)) + WIN_BUFFER_ADDR);
            seed_rgb(high_buf, 44, 45, 46);
            kernel_syscall_dispatch(1, 29, high, 0, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 2, ok);
            visible = framebuffer_visible_base();
            pixel = visible + (1 * 8 + 1) * 3;
            ok = expect_eq(kload(pixel), 44, ok);
            ok = expect_eq(kload(pixel + 1), 45, ok);
            ok = expect_eq(kload(pixel + 2), 46, ok);

            kernel_syscall_dispatch(1, 31, low, 7, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            kernel_syscall_dispatch(1, 29, low, 0, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 2, ok);
            visible = framebuffer_visible_base();
            pixel = visible + (1 * 8 + 1) * 3;
            ok = expect_eq(kload(pixel), 11, ok);
            ok = expect_eq(kload(pixel + 1), 12, ok);
            ok = expect_eq(kload(pixel + 2), 13, ok);

            kernel_syscall_dispatch(1, 34, low, 4, 3, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(window_find(low)) + WIN_WIDTH), 4, ok);
            ok = expect_eq(kload(window_addr(window_find(low)) + WIN_HEIGHT), 3, ok);
            ok = expect_eq(kload(low_buf), 0, ok);
            seed_rgb(low_buf + (2 * 4 + 3) * 3, 70, 71, 72);
            kernel_syscall_dispatch(1, 29, low, 0, 0, 0);
            visible = framebuffer_visible_base();
            pixel = visible + (3 * 8 + 4) * 3;
            ok = expect_eq(kload(pixel), 70, ok);
            ok = expect_eq(kload(pixel + 1), 71, ok);
            ok = expect_eq(kload(pixel + 2), 72, ok);
            ok = expect_eq(kload(COMPOSITOR_FRAME_COUNT_ADDR), 4, ok);

            kernel_syscall_dispatch(1, 34, low, 100, 100, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_NO_SPACE, ok);
            return ok;
        }
    )";
}

std::string inputRoutingDriver() {
    return expectHelpers(35400) + R"(
        fn main() -> t40 {
            var ok: t40 = 1;
            var out: t40 = USER_MEM_BASE + 900;
            ok = expect_eq(kernel_init(), 1, ok);
            kernel_syscall_dispatch(1, 25, 8, 6, 0, 0);

            kernel_syscall_dispatch(1, 27, 0, 0, 3, 2);
            var low: t40 = kload(SYS_PAYLOAD_ADDR);
            kernel_syscall_dispatch(1, 27, 1, 1, 3, 2);
            var high: t40 = kload(SYS_PAYLOAD_ADDR);
            kernel_syscall_dispatch(1, 31, high, 7, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);

            ok = expect_eq(window_route_input(EVENT_KIND_MOUSE, 3, 1, 1), high, ok);
            ok = expect_eq(kload(WINDOW_FOCUS_ID_ADDR), high, ok);
            ok = expect_eq(kload(window_addr(window_find(high)) + WIN_EVENT_COUNT), 1, ok);
            kernel_syscall_dispatch(1, 33, high, out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(out + EVENT_KIND), EVENT_KIND_CLOSE, ok);
            ok = expect_eq(kload(out + EVENT_X_OR_KEY), high, ok);
            ok = expect_eq(signal_has(kload(PROC_SIGNAL_PENDING_BASE), SIGNAL_CLOSE_REQUEST), 1, ok);

            ok = expect_eq(window_route_input(EVENT_KIND_KEY, 75, 0, 0), high, ok);
            kernel_syscall_dispatch(1, 33, high, out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(out + EVENT_KIND), EVENT_KIND_KEY, ok);
            ok = expect_eq(kload(out + EVENT_X_OR_KEY), 75, ok);
            ok = expect_eq(kload(window_addr(window_find(high)) + WIN_EVENT_COUNT), 0, ok);

            ok = expect_eq(window_route_input(EVENT_KIND_MOUSE, 0, 0, 0), low, ok);
            kernel_syscall_dispatch(1, 33, low, out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(out + EVENT_KIND), EVENT_KIND_MOUSE, ok);
            ok = expect_eq(kload(out + EVENT_X_OR_KEY), 0, ok);
            ok = expect_eq(kload(out + EVENT_Y), 0, ok);

            ok = expect_eq(window_route_input(EVENT_KIND_MOUSE, 7, 5, 0), 0, ok);
            kernel_syscall_dispatch(1, 33, high, out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 0, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_EOF, ok);
            kernel_syscall_dispatch(3, 33, high, out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            return ok;
        }
    )";
}

bool runKernelDriver(TestContext& ctx,
                     const std::string& name,
                     const std::string& driver,
                     int fail_base) {
    const std::string kernel = tests_next::readText("kernel.trit");
    ctx.check(!kernel.empty(), "kernel.trit is present");
    sandbox::compiler::CompileResult compiled =
        sandbox::compiler::compileSource(name + ".trit", kernel + "\n" + driver);
    if (!compiled.success) {
        ctx.fail(name + " diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
        return false;
    }
    sandbox::compiler::LinkResult linked =
        sandbox::compiler::linkModules({compiled.object});
    ctx.check(linked.success, name + " links");
    if (!linked.success) {
        ctx.fail(name + " link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return false;
    }

    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    vm.resetBlockDevice(192);
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
              name + " image loads");
    const auto result = sandbox::vm::run(vm, 50000000);
    if (!result.halted()) {
        std::ostringstream out;
        out << name << " did not halt; status=" << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return false;
    }
    ctx.equal(regLong(vm, 13), 1LL, name + " returns ok");
    if (regLong(vm, 13) != 1) {
        std::ostringstream out;
        out << "first assertion flag=" << wordAt(vm, fail_base + 0)
            << " actual=" << wordAt(vm, fail_base + 1)
            << " expected=" << wordAt(vm, fail_base + 2)
            << " assertion_index=" << wordAt(vm, fail_base + 4);
        ctx.fail(out.str());
    }
    return regLong(vm, 13) == 1;
}

void framebufferTextInputSnapshot(TestContext& ctx) {
    const sandbox::host::TosBootImage image = assembleImage(ctx, R"(
        .text
        boot:
            csrr r1, console_in
            mov r2, 60000
            store r1, r2, 0
            csrr r1, mouse_x
            store r1, r2, 1
            csrr r1, mouse_y
            store r1, r2, 2
            csrr r1, mouse_btn
            store r1, r2, 3
            halt
    )");

    sandbox::host::TosRuntime runtime;
    std::string error;
    ctx.check(runtime.loadImage(image, &error), "runtime loads text snapshot image");
    runtime.pushTextInput("A");
    runtime.updateMouseState('B', 'C', 'D');
    const auto result = runtime.runForSteps(32);
    ctx.check(result.halted(), "text snapshot image halts");

    const sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
    ctx.check(framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25,
              "runtime decodes text framebuffer mode");
    ctx.equal(framebuffer.width, 80, "text framebuffer width");
    ctx.equal(framebuffer.height, 25, "text framebuffer height");
    ctx.check(framebuffer.glyphs.size() >= 4 &&
                  framebuffer.glyphs[0] == 'A' &&
                  framebuffer.glyphs[1] == 'B' &&
                  framebuffer.glyphs[2] == 'C' &&
                  framebuffer.glyphs[3] == 'D',
              "text snapshot exposes keyboard and mouse routed glyphs");

    const sandbox::host::TosFramebufferMemorySnapshot raw =
        runtime.readFramebufferMemory();
    ctx.check(raw.changed && raw.mode == sandbox::host::TosFramebufferMode::Text80x25,
              "raw text framebuffer reports initial change");
    ctx.check(raw.words.size() >= 4 &&
                  (raw.words[0] & 0xff) == 'A' &&
                  (raw.words[1] & 0xff) == 'B',
              "raw text framebuffer preserves cell words");
    const sandbox::host::TosFramebufferMemorySnapshot unchanged =
        runtime.readFramebufferMemory(raw.revision);
    ctx.check(!unchanged.changed && unchanged.words.empty(),
              "raw text framebuffer skips unchanged revisions");
}

void framebufferGraphicsPaletteSnapshot(TestContext& ctx) {
    const sandbox::host::TosBootImage image = assembleImage(ctx, R"(
        .text
        boot:
            mov r1, 1
            csrw gpu_mode, r1
            mov r1, 14
            mov r2, 50000
            store r1, r2, 0
            mov r1, 6
            csrw sprite_x, r1
            mov r1, 7
            csrw sprite_y, r1
            mov r1, 99
            csrw sprite_attr, r1
            halt
    )");

    sandbox::host::TosRuntime runtime;
    std::string error;
    ctx.check(runtime.loadImage(image, &error),
              "runtime loads graphics snapshot image");
    const auto result = runtime.runForSteps(32);
    ctx.check(result.halted(), "graphics snapshot image halts");

    const sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
    ctx.check(framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60,
              "runtime decodes graphics framebuffer mode");
    ctx.equal(framebuffer.width, 80, "graphics framebuffer width");
    ctx.equal(framebuffer.height, 60, "graphics framebuffer height");
    ctx.check(!framebuffer.rgba.empty() && framebuffer.rgba[0] == 0xffff00ff,
              "graphics snapshot maps color index through the host palette");
    ctx.equal(framebuffer.sprite_x, 6LL, "graphics snapshot reports sprite x");
    ctx.equal(framebuffer.sprite_y, 7LL, "graphics snapshot reports sprite y");
    ctx.equal(framebuffer.sprite_attr, 99LL, "graphics snapshot reports sprite attr");

    const sandbox::host::TosFramebufferMemorySnapshot raw =
        runtime.readFramebufferMemory();
    ctx.check(raw.changed && raw.mode == sandbox::host::TosFramebufferMode::Graphics80x60,
              "raw graphics framebuffer reports initial change");
    ctx.check(!raw.words.empty() && raw.words[0] == 14,
              "raw graphics framebuffer preserves color indices");
    const sandbox::host::TosFramebufferMemorySnapshot unchanged =
        runtime.readFramebufferMemory(raw.revision);
    ctx.check(!unchanged.changed && unchanged.words.empty(),
              "raw graphics framebuffer skips unchanged revisions");
}

void compositorZOrderDirtyResize(TestContext& ctx) {
    (void)runKernelDriver(ctx, "next_gui_compositor", compositorDriver(), 35300);
}

void inputFocusCloseKeyEvents(TestContext& ctx) {
    (void)runKernelDriver(ctx, "next_gui_input", inputRoutingDriver(), 35400);
}

void widgetDirtyTextRedrawRuntime(TestContext& ctx) {
    const std::string sdk = tests_next::readText("apps/os_sdk.trit");
    const std::string widget = tests_next::readText("apps/libwidget.trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is present");
    ctx.check(!widget.empty(), "apps/libwidget.trit is present");
    const std::string source = R"(
        fn main() -> t40 {
            var button_a: t40 = 11000;
            var button_b: t40 = 11040;
            var children: t40 = 11100;
            var event_addr: t40 = 11200;
            var text_buf: t40 = 11300;
            var routed: t40 = 0;
            var focused: t40 = 0;

            widget_init(button_a, WIDGET_KIND_BUTTON, 2, 2, 5, 1, 0, 7);
            widget_init(button_b, WIDGET_KIND_BUTTON, 8, 2, 5, 1, 0, 7);
            widget_store(children, button_a);
            widget_store(children + 1, button_b);

            unsafe {
                store(event_addr + 0, OS_EVENT_KIND_MOUSE);
                store(event_addr + 1, 9);
                store(event_addr + 2, 2);
                store(event_addr + 3, 1);
            }
            routed = widget_panel_route_event(children, 2, event_addr, 0, 0);
            if routed - button_b != 0 { return 10; }

            focused = widget_focus_first(children, 2);
            if focused - button_a != 0 { return 20; }
            focused = widget_focus_next(children, 2, focused);
            if focused - button_b != 0 { return 30; }

            widget_clear_dirty(button_b);
            widget_invalidate_rect(button_b, 1, 1, 2, 2);
            widget_invalidate_rect(button_b, 0, 0, 1, 1);
            if widget_load(button_b + WIDGET_DIRTY_X) != 0 { return 40; }
            if widget_load(button_b + WIDGET_DIRTY_Y) != 0 { return 41; }
            if widget_load(button_b + WIDGET_DIRTY_W) - 3 != 0 { return 42; }
            if widget_load(button_b + WIDGET_DIRTY_H) - 3 != 0 { return 43; }

            widget_passwordfield_init(button_a, text_buf, 8, 0, 0, 10, 1, 0, -1);
            widget_set_focus(button_a, 1);
            widget_textfield_handle_key(button_a, 65);
            widget_textfield_handle_key(button_a, 66);
            widget_textfield_handle_key(button_a, 8);
            if widget_load(button_a + WIDGET_TEXT_LEN) - 1 != 0 { return 50; }
            if widget_load(text_buf) - 65 != 0 { return 51; }
            widget_textfield_select_all(button_a);
            widget_textfield_handle_key(button_a, 67);
            if widget_load(button_a + WIDGET_TEXT_LEN) - 1 != 0 { return 52; }
            if widget_load(text_buf) - 67 != 0 { return 53; }

            os_clear_text(0);
            widget_draw_text_button_char(1, 1, 3, 65, 1, 0, 0, 1);
            widget_draw_passwordfield(button_a);
            return 123;
        }
    )";

    sandbox::compiler::CompileResult compiled =
        sandbox::compiler::compileSource("next_gui_widget.trit",
                                         sdk + "\n" + widget + "\n" + source);
    if (!compiled.success) {
        ctx.fail("widget diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
        return;
    }
    sandbox::compiler::LinkOptions options;
    options.dead_strip_functions = true;
    sandbox::compiler::LinkResult linked =
        sandbox::compiler::linkModules({compiled.object}, options);
    ctx.check(linked.success, "widget GUI driver links with dead-strip");
    if (!linked.success) {
        ctx.fail("widget link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return;
    }

    sandbox::vm::VMState vm(65536, 1000000);
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
              "widget GUI image loads");
    const auto result = sandbox::vm::run(vm, 200000);
    if (!result.halted()) {
        std::ostringstream out;
        out << "widget GUI driver did not halt; status="
            << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return;
    }
    ctx.equal(regLong(vm, 13), 123LL,
              "widget hit testing, focus, dirty rects, and text editing behave");
    ctx.equal(wordAt(vm, 60000 + 1 * 80 + 1), 91LL + 5LL * 256LL,
              "widget redraw writes highlighted button left edge");
    ctx.equal(wordAt(vm, 60000 + 1 * 80 + 2), 65LL + 5LL * 256LL,
              "widget redraw writes highlighted button label");
    ctx.equal(wordAt(vm, 60000 + 0 * 80 + 0), 91LL + 5LL * 256LL,
              "password field redraw writes focused field border");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"gui.framebuffer.text_input_snapshot", "gui.six_layer_contract",
         framebufferTextInputSnapshot},
        {"gui.framebuffer.graphics_palette_snapshot", "gui.six_layer_contract",
         framebufferGraphicsPaletteSnapshot},
        {"gui.compositor.z_order_dirty_resize", "gui.six_layer_contract",
         compositorZOrderDirtyResize},
        {"gui.input.focus_close_key_events", "gui.six_layer_contract",
         inputFocusCloseKeyEvents},
        {"gui.widget.dirty_text_redraw_runtime", "gui.six_layer_contract",
         widgetDirtyTextRedrawRuntime},
    };
    return tests_next::runCases("next_graphics_gui", cases);
}
