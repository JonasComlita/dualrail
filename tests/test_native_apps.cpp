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
}

void testIpcSdkWrappersCompile() {
    std::cout << "[2] Native OS IPC SDK wrappers\n";
    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string src = R"(
        fn main() -> t40 {
            var out_addr: t40 = 10000;
            os_ipc_send(0, 77);
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
}

void testWidgetToolkitCompiles() {
    std::cout << "[5] Native OS widget toolkit\n";
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
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testNativeApps();
    testIpcSdkWrappersCompile();
    testFileSdkWrappersCompile();
    testGuiSdkWrappersCompile();
    testWidgetToolkitCompiles();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "Native OS app tests passed\n";
    return 0;
}
