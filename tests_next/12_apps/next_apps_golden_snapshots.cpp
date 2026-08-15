#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;

constexpr int kTextBase = 60000;
constexpr int kTextWidth = 80;
constexpr int kTextHeight = 25;

std::string formatDiagnostics(
    const std::vector<sandbox::compiler::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

std::string sourceWithRenamedMain(TestContext& ctx,
                                  const std::string& source,
                                  const std::string& new_name) {
    std::string renamed = source;
    const std::string main_decl = "fn main() -> t40";
    const std::size_t pos = renamed.find(main_decl);
    if (pos == std::string::npos) {
        ctx.fail("app source has no standard main declaration to rename");
        return {};
    }
    renamed.replace(pos, main_decl.size(), "fn " + new_name + "() -> t40");
    return renamed;
}

std::string sdkForSnapshot(TestContext& ctx,
                           const std::string& source_name,
                           std::string sdk) {
    // The isolated VM has no VFS service table.  Keep the file-manager draw
    // path intact, but make its public SDK directory read return the same
    // empty-directory result that a freshly formatted root would expose.
    if (source_name != "file_manager") return sdk;
    const std::string syscall_wrapper = R"(fn os_readdir(path_addr: t40, out_addr: t40, max_words: t40) -> t40 {
    return sys_readdir(path_addr, out_addr, max_words);
})";
    const std::string empty_wrapper = R"(fn os_readdir(path_addr: t40, out_addr: t40, max_words: t40) -> t40 {
    return 0;
})";
    const std::size_t pos = sdk.find(syscall_wrapper);
    if (pos == std::string::npos) {
        ctx.fail("file_manager snapshot could not locate os_readdir SDK wrapper");
        return {};
    }
    sdk.replace(pos, syscall_wrapper.size(), empty_wrapper);
    return sdk;
}

sandbox::compiler::LinkResult compileDrawApp(TestContext& ctx,
                                             const std::string& source_name,
                                             const std::string& id,
                                             const std::string& driver) {
    using namespace sandbox::compiler;
    const std::string sdk = tests_next::readText("apps/os_sdk.trit");
    const std::string widget = tests_next::readText("apps/libwidget.trit");
    const std::string source = tests_next::readText("apps/" + source_name + ".trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is present for " + id);
    ctx.check(!widget.empty(), "apps/libwidget.trit is present for " + id);
    ctx.check(!source.empty(), source_name + " app source is present for " + id);
    if (sdk.empty() || widget.empty() || source.empty()) return {};
    const std::string snapshot_sdk = sdkForSnapshot(ctx, source_name, sdk);
    if (snapshot_sdk.empty()) return {};

    const std::string renamed = sourceWithRenamedMain(ctx, source, id + "_app_main");
    if (renamed.empty()) return {};

    CompileResult compiled = compileSource(id + "_snapshot.trit",
                                           snapshot_sdk + "\n" + widget + "\n" +
                                               renamed + "\n" + driver);
    if (!compiled.success) {
        ctx.fail(id + " diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
        return {};
    }

    LinkOptions options;
    options.stack_hint_words = 1024;
    options.standalone_halt_on_exit = true;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    ctx.check(linked.success, id + " snapshot driver links");
    if (!linked.success) {
        ctx.fail(id + " link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
    }
    return linked;
}

long long wordAt(sandbox::vm::VMState& vm, int address) {
    auto [value, fault] = vm.dmem.load(address);
    if (fault != sandbox::vm::MemFaultCode::OK) return -999999;
    return sandbox::vm::ops::toLong(value);
}

std::string glyphLabel(int glyph) {
    if (glyph >= 33 && glyph <= 126) {
        return std::string(1, static_cast<char>(glyph));
    }
    return "?";
}

std::string semanticSnapshot(const std::string& app,
                            sandbox::vm::VMState& vm) {
    std::ostringstream out;
    out << "# Trit app semantic text snapshot v1\n";
    out << "# Only non-blank text cells are included; glyph is ASCII decimal.\n";
    out << "app=" << app << "\n";
    out << "width=" << kTextWidth << " height=" << kTextHeight << "\n";
    out << "cells:\n";

    for (int y = 0; y < kTextHeight; ++y) {
        for (int x = 0; x < kTextWidth; ++x) {
            const long long cell = wordAt(vm, kTextBase + y * kTextWidth + x);
            const int glyph = static_cast<int>(cell & 0xffLL);
            const int color = static_cast<int>(cell / 256LL);
            if (glyph == 0 || glyph == 32) continue;
            out << "cell x=" << x << " y=" << y
                << " glyph=" << glyph << "('" << glyphLabel(glyph)
                << "') color=" << color << "\n";
        }
    }
    return out.str();
}

std::string normalizeGolden(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.empty() || text.back() != '\n') text.push_back('\n');
    return text;
}

std::string firstDifference(const std::string& expected,
                           const std::string& actual) {
    std::istringstream expected_lines(expected);
    std::istringstream actual_lines(actual);
    std::string expected_line;
    std::string actual_line;
    int line_number = 1;
    while (true) {
        const bool have_expected = static_cast<bool>(std::getline(expected_lines, expected_line));
        const bool have_actual = static_cast<bool>(std::getline(actual_lines, actual_line));
        if (!have_expected && !have_actual) return "no differing line";
        if (!have_expected || !have_actual || expected_line != actual_line) {
            std::ostringstream out;
            out << "line " << line_number << " expected="
                << (have_expected ? expected_line : "<end>")
                << " actual=" << (have_actual ? actual_line : "<end>");
            return out.str();
        }
        ++line_number;
    }
}

bool updateGoldensEnabled() {
    const char* value = std::getenv("TRIT_UPDATE_APP_GOLDENS");
    return value != nullptr && std::string(value) == "1";
}

void compareGolden(TestContext& ctx,
                   const std::string& app,
                   const std::string& actual) {
    const std::filesystem::path path = tests_next::goldenPath(
        std::filesystem::current_path(), "apps/" + app + ".snapshot");
    const std::string expected_raw = tests_next::readText(path);
    const std::string expected = normalizeGolden(expected_raw);
    const std::string canonical_actual = normalizeGolden(actual);

    if (updateGoldensEnabled()) {
        tests_next::writeText(path, canonical_actual);
        ctx.equal(normalizeGolden(tests_next::readText(path)), canonical_actual,
                  app + " golden fixture regenerated");
        return;
    }

    if (expected_raw.empty()) {
        ctx.fail(app + " golden fixture is missing at " + path.string() +
                 "; regenerate intentionally with TRIT_UPDATE_APP_GOLDENS=1\n" +
                 "actual snapshot:\n" + canonical_actual);
        return;
    }

    if (expected != canonical_actual) {
        ctx.fail(app + " semantic snapshot mismatch at " + path.string() +
                 ": " + firstDifference(expected, canonical_actual) +
                 "\nactual snapshot:\n" + canonical_actual);
    }
}

bool runSnapshotDriver(TestContext& ctx,
                       const std::string& source_name,
                       const std::string& app,
                       const std::string& driver,
                       sandbox::vm::VMState& vm) {
    const auto linked = compileDrawApp(ctx, source_name, app, driver);
    if (!linked.success) return false;
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
              app + " snapshot image loads");
    const auto result = sandbox::vm::run(vm, 400000);
    if (!result.halted()) {
        std::ostringstream out;
        out << app << " snapshot driver did not halt; status="
            << static_cast<int>(result.status) << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
            << " buffer='" << vm.syscall_buffer << "'";
        ctx.fail(out.str());
        return false;
    }
    ctx.equal(sandbox::vm::ops::toLong(vm.regfile.read(13)), 123LL,
              app + " snapshot driver returns sentinel");
    return sandbox::vm::ops::toLong(vm.regfile.read(13)) == 123;
}

void desktopSnapshot(TestContext& ctx) {
    sandbox::vm::VMState vm(65536, 1000000);
    const std::string driver = R"(
        fn main() -> t40 {
            os_gpu_mode(0);
            os_clear_text(0);
            desktop_draw_hud();
            desktop_draw_launcher();
            return 123;
        }
    )";
    if (runSnapshotDriver(ctx, "desktop", "desktop", driver, vm)) {
        compareGolden(ctx, "desktop", semanticSnapshot("desktop", vm));
    }
}

void terminalSnapshot(TestContext& ctx) {
    sandbox::vm::VMState vm(65536, 1000000);
    const std::string driver = R"(
        fn main() -> t40 {
            os_gpu_mode(0);
            term_draw_ui();
            return 123;
        }
    )";
    if (runSnapshotDriver(ctx, "terminal", "terminal", driver, vm)) {
        compareGolden(ctx, "terminal", semanticSnapshot("terminal", vm));
    }
}

void fileManagerSnapshot(TestContext& ctx) {
    sandbox::vm::VMState vm(65536, 1000000);
    const std::string driver = R"(
        fn main() -> t40 {
            os_gpu_mode(0);
            fm_draw_ui();
            return 123;
        }
    )";
    if (runSnapshotDriver(ctx, "file_manager", "file_manager", driver, vm)) {
        compareGolden(ctx, "file_manager", semanticSnapshot("file_manager", vm));
    }
}

void settingsSnapshot(TestContext& ctx) {
    sandbox::vm::VMState vm(65536, 1000000);
    const std::string driver = R"(
        fn main() -> t40 {
            os_gpu_mode(0);
            cfg_defaults();
            os_clear_text(0);
            draw_header();
            draw_tab_bar(TAB_DISPLAY);
            draw_tab_display();
            draw_footer();
            return 123;
        }
    )";
    if (runSnapshotDriver(ctx, "settings", "settings", driver, vm)) {
        compareGolden(ctx, "settings", semanticSnapshot("settings", vm));
    }
}

}  // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"apps.snapshot.desktop_semantic_text", "desktop semantic text framebuffer golden",
         desktopSnapshot},
        {"apps.snapshot.terminal_semantic_text", "terminal semantic text framebuffer golden",
         terminalSnapshot},
        {"apps.snapshot.file_manager_semantic_text", "file manager semantic text framebuffer golden",
         fileManagerSnapshot},
        {"apps.snapshot.settings_semantic_text", "settings semantic text framebuffer golden",
         settingsSnapshot},
    };
    return tests_next::runCases("next_apps_golden_snapshots", cases);
}
