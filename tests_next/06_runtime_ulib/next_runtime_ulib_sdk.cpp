#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::compiler;

std::string formatDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(
        vm.regfile.read(static_cast<uint8_t>(reg)));
}

long long wordAt(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
}

bool expectCompileOk(TestContext& ctx,
                     const CompileResult& compiled,
                     const std::string& label) {
    if (compiled.success) return true;
    ctx.fail(label + " diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
    return false;
}

bool expectLinkOk(TestContext& ctx,
                  const LinkResult& linked,
                  const std::string& label) {
    if (linked.success) return true;
    ctx.fail(label + " diagnostics:\n" + formatDiagnostics(linked.diagnostics));
    return false;
}

bool loadAndRun(TestContext& ctx,
                sandbox::vm::VMState& vm,
                const LinkResult& linked,
                int max_steps,
                const std::string& label) {
    ctx.check(sandbox::vm::loadAndReset(vm, linked.assembled.program),
              label + " image loads");
    const auto result = sandbox::vm::run(vm, max_steps);
    if (result.halted()) return true;

    std::ostringstream out;
    out << label << " did not halt; status=" << static_cast<int>(result.status)
        << " pc=" << vm.pc << " cause=" << vm.cause
        << " r13=" << regLong(vm, 13);
    ctx.fail(out.str());
    return false;
}

std::string readRepoText(const std::string& relative) {
    for (const std::string& candidate : {relative, "../" + relative, "../../" + relative}) {
        std::ifstream file(candidate);
        if (!file.is_open()) continue;
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }
    return "";
}

std::string asciiStores(const std::string& base,
                        const std::string& text,
                        const std::string& indent = "                ") {
    std::ostringstream out;
    out << indent << "unsafe {\n";
    for (std::size_t i = 0; i < text.size(); ++i) {
        out << indent << "    store(" << base << " + " << i << ", "
            << static_cast<int>(static_cast<unsigned char>(text[i])) << ");\n";
    }
    out << indent << "    store(" << base << " + " << text.size() << ", 0);\n"
        << indent << "}\n";
    return out.str();
}

CompileResult compileWithOptions(const std::string& name,
                                 const std::string& source,
                                 OptimizationLevel optimization = OptimizationLevel::None) {
    CompilerOptions options;
    options.optimization = optimization;
    return compileSource(name, source, options);
}

std::string allocatorInvariantMain() {
    return R"(
        fn check_eq(value: t40, expected: t40, code: t40) -> t40 {
            match value - expected {
                zero => { return 1; }
                neg => { return 0 - code; }
                pos => { return 0 - code; }
            }
        }

        fn check_positive(value: t40, code: t40) -> t40 {
            match value {
                pos => { return 1; }
                zero => { return 0 - code; }
                neg => { return 0 - code; }
            }
        }

        fn check_free_list(limit: t40) -> t40 {
            var curr: t40 = 0;
            unsafe { curr = load(22); }
            var count: t40 = 0;
            while curr > 0 {
                count = count + 1;
                match count - limit {
                    pos => { return 0 - 10; }
                    zero => {}
                    neg => {}
                }

                var size: t40 = 0;
                var next: t40 = 0;
                unsafe {
                    size = load(curr);
                    next = load(curr + 1);
                }
                match size {
                    pos => {}
                    zero => { return 0 - 11; }
                    neg => { return 0 - 11; }
                }
                match next - curr {
                    zero => { return 0 - 12; }
                    neg => {}
                    pos => {}
                }
                match next {
                    neg => { return 0 - 13; }
                    zero => {}
                    pos => {}
                }
                curr = next;
            }
            return 1;
        }

        fn main() -> t40 {
            var a: t40 = malloc_raw(2);
            var b: t40 = malloc_raw(3);
            var code: t40 = check_positive(a, 1);
            match code { neg => { return code; } zero => { return 0 - 20; } pos => {} }
            code = check_positive(b, 2);
            match code { neg => { return code; } zero => { return 0 - 21; } pos => {} }

            var ah: t40 = 0;
            var bh: t40 = 0;
            unsafe {
                ah = load(a - 1);
                bh = load(b - 1);
                store(a, 111);
                store(a + 1, 222);
                store(b, 333);
            }
            code = check_eq(ah, 0 - 2, 3);
            match code { neg => { return code; } zero => { return 0 - 22; } pos => {} }
            code = check_eq(bh, 0 - 3, 4);
            match code { neg => { return code; } zero => { return 0 - 23; } pos => {} }

            free_raw(b);
            code = check_free_list(16);
            match code { neg => { return code; } zero => { return 0 - 24; } pos => {} }

            var c: t40 = malloc_raw(1);
            code = check_positive(c, 5);
            match code { neg => { return code; } zero => { return 0 - 25; } pos => {} }
            code = check_eq(c, b, 6);
            match code { neg => { return code; } zero => { return 0 - 26; } pos => {} }

            var av0: t40 = 0;
            var av1: t40 = 0;
            unsafe {
                av0 = load(a);
                av1 = load(a + 1);
            }
            code = check_eq(av0, 111, 7);
            match code { neg => { return code; } zero => { return 0 - 27; } pos => {} }
            code = check_eq(av1, 222, 8);
            match code { neg => { return code; } zero => { return 0 - 28; } pos => {} }

            free_raw(a);
            free_raw(c);
            code = check_free_list(16);
            match code { neg => { return code; } zero => { return 0 - 29; } pos => {} }
            return 1;
        }
    )";
}

void standaloneSbrkContract(TestContext& ctx) {
    const std::string source = R"(
        .text
        _start:
            mov.t40 r13, 3
            syscall 19
            copy r2, r13
            mov.t40 r13, 2
            syscall 19
            copy r3, r13
            halt
    )";

    auto assembled = sandbox::vm::assembler::assemble(source);
    ctx.check(assembled.success, "standalone sbrk contract assembly parses");
    if (!assembled.success) return;

    for (int run_idx = 0; run_idx < 2; ++run_idx) {
        sandbox::vm::VMState vm(128, 4096);
        ctx.check(sandbox::vm::assembler::loadAndReset(vm, assembled),
                  "standalone sbrk contract image loads");
        const auto result = sandbox::vm::run(vm, 64);
        ctx.check(result.halted(), "standalone sbrk contract image halts");
        ctx.equal(regLong(vm, 2), 2000LL,
                  "first sbrk returns initial fresh-region base");
        ctx.equal(regLong(vm, 3), 2003LL,
                  "second sbrk returns next fresh-region base");
        ctx.equal(static_cast<long long>(vm.standalone_heap_break), 2005LL,
                  "standalone heap break advances per VM");
    }
}

void ulibAllocatorFreeListReuse(TestContext& ctx) {
    const std::string ulib = readRepoText("ulib.trit");
    ctx.check(!ulib.empty(), "ulib.trit is available");
    if (ulib.empty()) return;

    CompileResult compiled = compileWithOptions(
        "next_ulib_allocator_invariants.trit",
        ulib + "\n" + allocatorInvariantMain());
    if (!expectCompileOk(ctx, compiled,
                         "ulib allocator invariant fixture compiles")) {
        return;
    }

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "ulib allocator invariant fixture links")) {
        return;
    }

    sandbox::vm::VMState vm(65536, 65536);
    if (loadAndRun(ctx, vm, linked, 200000,
                   "ulib allocator invariant fixture")) {
        ctx.equal(regLong(vm, 13), 1LL,
                  "allocator preserves headers, free-list shape, reuse, and live data");
    }
}

void ulibFormatParseHelpers(TestContext& ctx) {
    const std::string ulib = readRepoText("ulib.trit");
    ctx.check(!ulib.empty(), "ulib.trit is available");
    if (ulib.empty()) return;

    std::string source = R"(
        fn expect_eq(actual: t40, expected: t40, code: t40) -> t40 {
            if actual - expected == 0 {
                return 1;
            }
            return 0 - code;
        }

        fn main() -> t40 {
            var spaced: t40 = malloc_raw(32);
            var tern_text: t40 = malloc_raw(32);
            if spaced <= 0 { return 0 - 2; }
            if tern_text <= 0 { return 0 - 3; }
)" + asciiStores("spaced", "  -123x") +
                 asciiStores("tern_text", "122") + R"(

            var code: t40 = 0;
            code = expect_eq(atoi(spaced), 0 - 123, 15);
            if code < 0 { return code; }
            code = expect_eq(atoi_ternary(tern_text), 17, 20);
            if code < 0 { return code; }
            code = expect_eq(strlen(spaced), 7, 21);
            if code < 0 { return code; }
            code = expect_eq(strncmp(spaced + 2, spaced + 2, 4), 0, 22);
            if code < 0 { return code; }
            return 1;
        }
    )";

    CompileResult compiled = compileWithOptions("next_ulib_format_parse.trit",
                                                ulib + "\n" + source);
    if (!expectCompileOk(ctx, compiled,
                         "ulib format/parse fixture compiles")) {
        return;
    }

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "ulib format/parse fixture links")) {
        return;
    }

    sandbox::vm::VMState vm(65536, 65536);
    if (loadAndRun(ctx, vm, linked, 1000000,
                   "ulib format/parse fixture")) {
        ctx.equal(regLong(vm, 13), 1LL,
                  "format/parse helper fixture returns success");
    }

    std::string itoa_source = R"(
        fn main() -> t40 {
            var dec: t40 = malloc_raw(32);
            var tern: t40 = malloc_raw(32);
            if dec <= 0 { return 0 - 1; }
            if tern <= 0 { return 0 - 2; }
            itoa(0 - 42, dec);
            itoa_ternary(17, tern);
            return 1;
        }
    )";
    CompileResult itoa_compiled = compileWithOptions(
        "next_ulib_itoa_link.trit", ulib + "\n" + itoa_source);
    if (!expectCompileOk(ctx, itoa_compiled,
                         "ulib itoa fixture compiles")) {
        return;
    }
    LinkResult itoa_linked = linkModules({itoa_compiled.object});
    if (!expectLinkOk(ctx, itoa_linked,
                      "ulib itoa fixture links")) {
        return;
    }
    ctx.contains(itoa_linked.assembly, "itoa:",
                 "decimal itoa helper is present in the linked image");
    ctx.contains(itoa_linked.assembly, "itoa_ternary:",
                 "ternary itoa helper is present in the linked image");

    std::string printf_source = R"(
        fn main() -> t40 {
            var fmt: t40 = malloc_raw(16);
            if fmt <= 0 { return 0 - 1; }
)" + asciiStores("fmt", "V=%t\n") + R"(
            printf(fmt, 5);
            return 1;
        }
    )";

    CompileResult printf_compiled = compileWithOptions(
        "next_ulib_printf_link.trit", ulib + "\n" + printf_source);
    if (!expectCompileOk(ctx, printf_compiled,
                         "ulib printf fixture compiles")) {
        return;
    }
    ctx.contains(printf_compiled.assembly, "call printf",
                 "printf fixture calls the formatting helper");
    LinkResult printf_linked = linkModules({printf_compiled.object});
    if (!expectLinkOk(ctx, printf_linked,
                      "ulib printf fixture links")) {
        return;
    }
    ctx.contains(printf_linked.assembly, "printf:",
                 "printf helper is present in the linked image");
    ctx.contains(printf_linked.assembly, "itoa_ternary:",
                 "ternary formatting helper is linked for percent-t");
    ctx.contains(printf_linked.assembly, "syscall 22",
                 "printf literal output path uses sys_write_char");
}

void sdkFileIpcWrappers(TestContext& ctx) {
    const std::string sdk = readRepoText("apps/os_sdk.trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is available");
    if (sdk.empty()) return;

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
            os_ipc_send(0, 77);
            os_ipc_recv_blocking(0, buf + 32, 10);
            os_futex_wait(buf + 32, 77, 10);
            os_futex_wake(buf + 32, 1);
            return os_ipc_recv(0, buf + 32);
        }
    )";

    CompileResult compiled = compileWithOptions("next_sdk_file_ipc.trit",
                                                sdk + "\n" + src);
    if (!expectCompileOk(ctx, compiled,
                         "file and IPC SDK wrapper fixture compiles")) {
        return;
    }

    const std::vector<std::pair<std::string, std::string>> expected = {
        {"syscall 12", "os_open lowers to syscall 12"},
        {"syscall 13", "os_close lowers to syscall 13"},
        {"syscall 14", "os_read lowers to syscall 14"},
        {"syscall 15", "os_write lowers to syscall 15"},
        {"syscall 16", "os_stat lowers to syscall 16"},
        {"syscall 17", "os_readdir lowers to syscall 17"},
        {"syscall 23", "os_ipc_send lowers to syscall 23"},
        {"syscall 24", "os_ipc_recv lowers to syscall 24"},
        {"syscall 47", "os_fsync lowers to syscall 47"},
        {"syscall 52", "os_futex_wait lowers to syscall 52"},
        {"syscall 53", "os_futex_wake lowers to syscall 53"},
        {"syscall 54", "os_ipc_recv_blocking lowers to syscall 54"},
    };
    for (const auto& [needle, message] : expected) {
        ctx.contains(compiled.assembly, needle, message);
    }
}

void sdkGuiProcessWrappers(TestContext& ctx) {
    const std::string sdk = readRepoText("apps/os_sdk.trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is available");
    if (sdk.empty()) return;

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

            var info: t40 = 10100;
            var path: t40 = 10200;
            os_spawn_app(path, 1, 101);
            os_getproc(1, info);
            os_suspend(2);
            os_resume(2);
            os_sleep_ms(5);
            return os_kill(2, OS_SIGNAL_KILL) + fb + buf;
        }
    )";

    CompileResult compiled = compileWithOptions("next_sdk_gui_process.trit",
                                                sdk + "\n" + src);
    if (!expectCompileOk(ctx, compiled,
                         "GUI and process SDK wrapper fixture compiles")) {
        return;
    }

    const std::vector<std::pair<std::string, std::string>> expected = {
        {"syscall 25", "os_fb_init lowers to syscall 25"},
        {"syscall 26", "os_fb_flip lowers to syscall 26"},
        {"syscall 27", "os_window_create lowers to syscall 27"},
        {"syscall 28", "os_window_get_buffer lowers to syscall 28"},
        {"syscall 29", "os_window_present lowers to syscall 29"},
        {"syscall 30", "os_window_move lowers to syscall 30"},
        {"syscall 31", "os_window_set_z lowers to syscall 31"},
        {"syscall 32", "os_window_destroy lowers to syscall 32"},
        {"syscall 33", "os_window_read_event lowers to syscall 33"},
        {"syscall 34", "os_window_resize lowers to syscall 34"},
        {"syscall 35", "os_window_request_close lowers to syscall 35"},
        {"syscall 48", "os_kill lowers to syscall 48"},
        {"syscall 49", "os_suspend lowers to syscall 49"},
        {"syscall 50", "os_resume lowers to syscall 50"},
        {"syscall 51", "os_getproc lowers to syscall 51"},
        {"syscall 55", "blocking event waits lower to syscall 55"},
        {"syscall 56", "os_sleep_ms lowers to syscall 56"},
        {"syscall 57", "os_spawn_app lowers to syscall 57"},
    };
    for (const auto& [needle, message] : expected) {
        ctx.contains(compiled.assembly, needle, message);
    }
}

void widgetToolkitDeadStripAndBehavior(TestContext& ctx) {
    const std::string sdk = readRepoText("apps/os_sdk.trit");
    const std::string widget = readRepoText("apps/libwidget.trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is available");
    ctx.check(!widget.empty(), "apps/libwidget.trit is available");
    if (sdk.empty() || widget.empty()) return;

    const std::string draw_src = R"(
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

    CompileResult compiled = compileWithOptions("next_widget_draw.trit",
                                                sdk + "\n" + widget + "\n" + draw_src);
    if (!expectCompileOk(ctx, compiled,
                         "widget draw fixture compiles")) {
        return;
    }

    LinkOptions options;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    if (!expectLinkOk(ctx, linked,
                      "widget draw fixture links with dead-strip enabled")) {
        return;
    }
    ctx.contains(linked.assembly, "widget_draw_button:",
                 "used widget draw helper survives stripping");
    ctx.check(!contains(linked.assembly, "widget_hit_window_rect:"),
              "unused widget hit-test helper strips out of draw-only image");

    const std::string hit_src = R"(
        fn main() -> t40 {
            return widget_hit_window_rect(10000, 2, 2, 1, 1, 4, 4);
        }
    )";
    CompileResult hit_compiled = compileWithOptions(
        "next_widget_hit.trit", sdk + "\n" + widget + "\n" + hit_src);
    if (!expectCompileOk(ctx, hit_compiled,
                         "widget hit-test fixture compiles")) {
        return;
    }
    LinkResult hit_linked = linkModules({hit_compiled.object}, options);
    if (!expectLinkOk(ctx, hit_linked,
                      "widget hit-test fixture links")) {
        return;
    }
    ctx.contains(hit_linked.assembly, "widget_hit_window_rect:",
                 "used widget hit-test helper survives stripping");

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

    CompileResult behavior_compiled = compileWithOptions(
        "next_widget_behavior.trit", sdk + "\n" + widget + "\n" + behavior_src);
    if (!expectCompileOk(ctx, behavior_compiled,
                         "widget behavior fixture compiles")) {
        return;
    }
    LinkResult behavior_linked = linkModules({behavior_compiled.object}, options);
    if (!expectLinkOk(ctx, behavior_linked,
                      "widget behavior fixture links")) {
        return;
    }

    sandbox::vm::VMState vm(65536, 1000000);
    if (loadAndRun(ctx, vm, behavior_linked, 200000,
                   "widget behavior fixture")) {
        ctx.equal(regLong(vm, 13), 123LL,
                  "widget hit testing, focus, dirty rects, and text editing behave");
        ctx.equal(wordAt(vm, 60000 + 1 * 80 + 1), 91LL + 5LL * 256LL,
                  "widget redraw writes highlighted button left edge");
        ctx.equal(wordAt(vm, 60000 + 1 * 80 + 2), 65LL + 5LL * 256LL,
                  "widget redraw writes highlighted button label");
        ctx.equal(wordAt(vm, 60000), 91LL + 5LL * 256LL,
                  "password field redraw writes focused field border");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();

    return tests_next::runCases("06_runtime_ulib", {
        {"runtime.sbrk.standalone_heap_contract",
         "runtime.ulib_sdk_contract",
         standaloneSbrkContract},
        {"runtime.ulib.allocator_free_list_reuse",
         "runtime.ulib_sdk_contract",
         ulibAllocatorFreeListReuse},
        {"runtime.ulib.format_parse_helpers",
         "runtime.ulib_sdk_contract",
         ulibFormatParseHelpers},
        {"runtime.sdk.file_ipc_wrappers",
         "runtime.ulib_sdk_contract",
         sdkFileIpcWrappers},
        {"runtime.sdk.gui_process_wrappers",
         "runtime.ulib_sdk_contract",
         sdkGuiProcessWrappers},
        {"runtime.widget.dead_strip_behavior",
         "runtime.ulib_sdk_contract",
         widgetToolkitDeadStripAndBehavior},
    });
}
