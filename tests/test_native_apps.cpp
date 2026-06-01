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

sandbox::vm::VMState compileAndRunApp(
    const std::string& app_name,
    const std::string& expected_marker) {

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
    expect(compiled.success, app_name + " compiles against the OS SDK");

    LinkOptions options;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    expect(linked.success, app_name + " links as a standalone app image");

    sandbox::vm::VMState vm(65536, 1000000);
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program),
               app_name + " loads into the VM");
        const auto result = sandbox::vm::run(vm, 200000);
        if (!result.halted()) {
            std::cout << "DEBUG " << app_name << ": status="
                      << static_cast<int>(result.status)
                      << " pc=" << vm.pc
                      << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
                      << " buffer='" << vm.syscall_buffer << "'\n";
        }
        expect(result.halted(), app_name + " halts cleanly");
        expect(contains(vm.syscall_buffer, expected_marker),
               app_name + " writes its console marker");
    }
    return vm;
}

void testNativeApps() {
    std::cout << "[1] Native OS app SDK and built-in apps\n";

    sandbox::vm::VMState desktop = compileAndRunApp("desktop", "DESKTOP\n");
    expect(wordAt(desktop, 60000) == 84 + 11 * 256,
           "desktop draws its launcher title");

    sandbox::vm::VMState calc = compileAndRunApp("calculator", "144 + 12 = 156\n");
    expect(sandbox::vm::ops::toLong(calc.regfile.read(13)) == 156,
           "calculator returns the computed result");

    sandbox::vm::VMState task = compileAndRunApp("task_manager", "TASKS\n");
    expect(wordAt(task, 60000) == 84 + 10 * 256,
           "task manager draws its text-mode title");

    sandbox::vm::VMState paint = compileAndRunApp("paint", "PAINT\n");
    expect(paint.gpu_mode == 1, "paint switches the display into graphics mode");
    expect(paint.sprite_attr == 43 + 14 * 256, "paint publishes a cursor sprite");
    expect(wordAt(paint, 55000 + 6 * 80 + 6) == 4,
           "paint fills the primary canvas through the GPU CSR");
    expect(wordAt(paint, 60000) == 80 + 14 * 256,
           "paint labels its text overlay");

    sandbox::vm::VMState files = compileAndRunApp("file_manager", "FILES\n");
    expect(wordAt(files, 60000) == 70 + 10 * 256,
           "file manager draws its text-mode title");

    sandbox::vm::VMState settings = compileAndRunApp("settings", "SETTINGS\n");
    expect(wordAt(settings, 60000) == 83 + 10 * 256,
           "settings app draws its text-mode title");

    sandbox::vm::VMState terminal = compileAndRunApp("terminal", "TERMINAL\n");
    expect(wordAt(terminal, 60000) == 84 + 10 * 256,
           "terminal app draws its text-mode title");
}

void testIpcSdkWrappersCompile() {
    std::cout << "[2] Native OS IPC SDK wrappers\n";
    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string src = R"(
        fn main() -> t40 {
            var out_addr: t40 = 10000;
            os_ipc_send(0, 77);
            os_ipc_recv_blocking(0, out_addr, 10);
            os_futex_wait(out_addr, 77, 10);
            os_futex_wake(out_addr, 1);
            return os_ipc_recv(0, out_addr);
        }
    )";

    CompileResult compiled = compileSource("native_app_ipc.trit", sdk + "\n" + src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR IPC SDK:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "IPC SDK wrappers compile");
    expect(contains(compiled.assembly, "syscall 23"), "os_ipc_send lowers to syscall 23");
    expect(contains(compiled.assembly, "syscall 24"), "os_ipc_recv lowers to syscall 24");
    expect(contains(compiled.assembly, "syscall 52"), "os_futex_wait lowers to syscall 52");
    expect(contains(compiled.assembly, "syscall 53"), "os_futex_wake lowers to syscall 53");
    expect(contains(compiled.assembly, "syscall 54"), "os_ipc_recv_blocking lowers to syscall 54");
}

void testFileSdkWrappersCompile() {
    std::cout << "[3] Native OS file SDK wrappers\n";
    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string src = R"(
        fn main() -> t40 {
            var path: t40 = 10000;
            var buf: t40 = 10040;
            var fd: t40 = os_open(path, 0);
            os_stat(path, buf);
            os_readdir(path, buf + 8, 16);
            os_read(fd, buf + 24, 4);
            os_write(fd, buf + 24, 4);
            os_fsync(fd);
            os_close(fd);
            return fd;
        }
    )";

    CompileResult compiled = compileSource("native_app_files.trit", sdk + "\n" + src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR FILE SDK:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "file SDK wrappers compile");
    expect(contains(compiled.assembly, "syscall 12"), "os_open lowers to syscall 12");
    expect(contains(compiled.assembly, "syscall 13"), "os_close lowers to syscall 13");
    expect(contains(compiled.assembly, "syscall 14"), "os_read lowers to syscall 14");
    expect(contains(compiled.assembly, "syscall 15"), "os_write lowers to syscall 15");
    expect(contains(compiled.assembly, "syscall 16"), "os_stat lowers to syscall 16");
    expect(contains(compiled.assembly, "syscall 17"), "os_readdir lowers to syscall 17");
    expect(contains(compiled.assembly, "syscall 47"), "os_fsync lowers to syscall 47");
}

void testGuiSdkWrappersCompile() {
    std::cout << "[4] Native OS framebuffer and window SDK wrappers\n";
    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string src = R"(
        fn main() -> t40 {
            var fb: t40 = os_fb_init(8, 6);
            os_fb_flip();
            var win: t40 = os_window_create(1, 2, 3, 2);
            var buf: t40 = os_window_get_buffer(win);
            os_window_fill_rgb(buf, 3, 2, 70, 71, 72);
            os_window_plot_rgb(buf, 3, 2, 1, 1, 80, 81, 82);
            os_window_fill_rect_rgb(buf, 3, 2, 0, 0, 2, 1, 83, 84, 85);
            os_window_draw_rect_rgb(buf, 3, 2, 0, 0, 3, 2, 86, 87, 88);
            os_window_present(win);
            os_window_move(win, 2, 3);
            os_window_set_z(win, 7);
            os_window_resize(win, 4, 3);
            os_window_request_close(win);
            os_window_read_event(win, 10000);
            os_wait_event(win, 10000, 10);
            os_window_wait_event(win, 10000, 10);
            os_event_kind(10000);
            os_event_x_or_key(10000);
            os_event_y(10000);
            os_event_modifiers(10000);
            os_window_destroy(win);
            return fb + buf;
        }
    )";

    CompileResult compiled = compileSource("native_app_gui.trit", sdk + "\n" + src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR GUI SDK:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "GUI SDK wrappers compile");
    expect(contains(compiled.assembly, "syscall 25"), "os_fb_init lowers to syscall 25");
    expect(contains(compiled.assembly, "syscall 26"), "os_fb_flip lowers to syscall 26");
    expect(contains(compiled.assembly, "syscall 27"), "os_window_create lowers to syscall 27");
    expect(contains(compiled.assembly, "syscall 28"), "os_window_get_buffer lowers to syscall 28");
    expect(contains(compiled.assembly, "syscall 29"), "os_window_present lowers to syscall 29");
    expect(contains(compiled.assembly, "syscall 30"), "os_window_move lowers to syscall 30");
    expect(contains(compiled.assembly, "syscall 31"), "os_window_set_z lowers to syscall 31");
    expect(contains(compiled.assembly, "syscall 32"), "os_window_destroy lowers to syscall 32");
    expect(contains(compiled.assembly, "syscall 33"), "os_window_read_event lowers to syscall 33");
    expect(contains(compiled.assembly, "syscall 34"), "os_window_resize lowers to syscall 34");
    expect(contains(compiled.assembly, "syscall 35"), "os_window_request_close lowers to syscall 35");
    expect(contains(compiled.assembly, "syscall 55"), "blocking event waits lower to syscall 55");
}

void testProcessControlSdkWrappersCompile() {
    std::cout << "[5] Native OS process-control SDK wrappers\n";
    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string src = R"(
        fn main() -> t40 {
            var info: t40 = 10000;
            os_getproc(1, info);
            os_suspend(2);
            os_resume(2);
            os_sleep_ms(5);
            return os_kill(2, OS_SIGNAL_KILL);
        }
    )";

    CompileResult compiled = compileSource("native_app_process_control.trit", sdk + "\n" + src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR PROCESS SDK:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "process-control SDK wrappers compile");
    expect(contains(compiled.assembly, "syscall 48"), "os_kill lowers to syscall 48");
    expect(contains(compiled.assembly, "syscall 49"), "os_suspend lowers to syscall 49");
    expect(contains(compiled.assembly, "syscall 50"), "os_resume lowers to syscall 50");
    expect(contains(compiled.assembly, "syscall 51"), "os_getproc lowers to syscall 51");
    expect(contains(compiled.assembly, "syscall 56"), "os_sleep_ms lowers to syscall 56");
}

void testWidgetToolkitCompiles() {
    std::cout << "[6] Native OS widget toolkit\n";
    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string widget = readTextFile("apps/libwidget.trit");
    const std::string src = R"(
        fn main() -> t40 {
            var fb: t40 = os_fb_init(18, 9);
            var win: t40 = os_window_create(0, 0, 18, 9);
            var buf: t40 = os_window_get_buffer(win);
            widget_draw_panel(buf, 18, 9, 0, 0, 18, 9, 0);
            widget_draw_button(buf, 18, 9, 2, 2, 6, 6, 55, 1);
            os_window_present(win);
            return fb + buf;
        }
    )";

    CompileResult compiled = compileSource("native_widget_toolkit.trit", sdk + "\n" + widget + "\n" + src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR WIDGET TOOLKIT:\n";
        dumpDiagnostics(compiled);
    }
    expect(compiled.success, "widget toolkit compiles");
    LinkOptions options;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    expect(linked.success, "widget toolkit links with dead-strip enabled");
    expect(contains(linked.assembly, "widget_draw_button:"), "used widget button survives stripping");
    expect(!contains(linked.assembly, "widget_hit_window_rect:"), "unused widget hit-test strips out of this image");

    const std::string hit_src = R"(
        fn main() -> t40 {
            return widget_hit_window_rect(10000, 2, 2, 1, 1, 4, 4);
        }
    )";
    CompileResult hit_compiled = compileSource("native_widget_hit.trit", sdk + "\n" + widget + "\n" + hit_src);
    if (!hit_compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR WIDGET HIT TEST:\n";
        dumpDiagnostics(hit_compiled);
    }
    expect(hit_compiled.success, "widget hit-test helper compiles");
    LinkResult hit_linked = linkModules({hit_compiled.object}, options);
    expect(hit_linked.success, "widget hit-test helper links");
    expect(contains(hit_linked.assembly, "widget_hit_window_rect:"), "used widget hit-test survives stripping");

    const std::string behavior_src = R"(
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

    CompileResult behavior_compiled = compileSource("native_widget_behavior.trit", sdk + "\n" + widget + "\n" + behavior_src);
    if (!behavior_compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR WIDGET BEHAVIOR:\n";
        dumpDiagnostics(behavior_compiled);
    }
    expect(behavior_compiled.success, "widget behavior test compiles");
    LinkResult behavior_linked = linkModules({behavior_compiled.object}, options);
    expect(behavior_linked.success, "widget behavior test links");
    sandbox::vm::VMState vm(65536, 1000000);
    if (behavior_linked.success) {
        expect(sandbox::vm::loadAndReset(vm, behavior_linked.assembled.program),
               "widget behavior test loads");
        const auto result = sandbox::vm::run(vm, 200000);
        if (!result.halted()) {
            std::cout << "DEBUG widget behavior: status="
                      << static_cast<int>(result.status)
                      << " pc=" << vm.pc
                      << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
                      << "\n";
        }
        expect(result.halted(), "widget behavior test halts cleanly");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(13)) == 123,
               "widget hit testing, focus, dirty rects, and text editing behave");
        expect(wordAt(vm, 60000 + 1 * 80 + 1) == 91 + 5 * 256,
               "widget redraw writes highlighted button left edge");
        expect(wordAt(vm, 60000 + 1 * 80 + 2) == 65 + 5 * 256,
               "widget redraw writes highlighted button label");
        expect(wordAt(vm, 60000 + 0 * 80 + 0) == 91 + 5 * 256,
               "password field redraw writes focused field border");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testNativeApps();
    testIpcSdkWrappersCompile();
    testFileSdkWrappersCompile();
    testGuiSdkWrappersCompile();
    testProcessControlSdkWrappersCompile();
    testWidgetToolkitCompiles();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "Native OS app tests passed\n";
    return 0;
}
