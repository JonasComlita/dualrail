#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_consumer_shell.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <algorithm>
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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
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

long long wordAt(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return -999999;
    return sandbox::vm::ops::toLong(value);
}

long long textCell(char glyph, int color) {
    return static_cast<long long>(static_cast<unsigned char>(glyph)) +
           static_cast<long long>(color) * 256LL;
}

sandbox::compiler::LinkResult compileBundledApp(TestContext& ctx,
                                                const std::string& source_name,
                                                const std::string& id,
                                                int stack_words) {
    using namespace sandbox::compiler;
    const std::string sdk = tests_next::readText("apps/os_sdk.trit");
    const std::string widget = tests_next::readText("apps/libwidget.trit");
    const std::string source = tests_next::readText("apps/" + source_name + ".trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is present");
    ctx.check(!widget.empty(), "apps/libwidget.trit is present");
    ctx.check(!source.empty(), source_name + " app source is present");
    if (sdk.empty() || widget.empty() || source.empty()) return {};

    CompileResult compiled =
        compileSource(id + ".trit", sdk + "\n" + widget + "\n" + source);
    if (!compiled.success) {
        ctx.fail(id + " diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
        return {};
    }

    LinkOptions options;
    options.stack_hint_words = stack_words;
    options.standalone_halt_on_exit = true;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    ctx.check(linked.success, id + " links with standalone app options");
    if (!linked.success) {
        ctx.fail(id + " link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return linked;
    }
    ctx.equal(linked.executable_header.stack_words, stack_words,
              id + " executable header preserves stack hint");
    return linked;
}

bool runLinkedApp(TestContext& ctx,
                  const std::string& id,
                  const sandbox::compiler::LinkResult& linked,
                  sandbox::vm::VMState& vm,
                  const std::string& marker) {
    if (!linked.success) return false;
    ctx.check(sandbox::vm::loadAndReset(vm, linked.assembled.program),
              id + " image loads");
    const auto result = sandbox::vm::run(vm, 200000);
    if (!result.halted()) {
        std::ostringstream out;
        out << id << " did not halt; status=" << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg)
            << " buffer='" << vm.syscall_buffer << "'";
        ctx.fail(out.str());
        return false;
    }
    ctx.contains(vm.syscall_buffer, marker, id + " writes console marker");
    return contains(vm.syscall_buffer, marker);
}

bool runAppDrawDriver(TestContext& ctx,
                      const std::string& source_name,
                      const std::string& id,
                      const std::string& driver,
                      sandbox::vm::VMState& vm) {
    using namespace sandbox::compiler;
    const std::string sdk = tests_next::readText("apps/os_sdk.trit");
    const std::string widget = tests_next::readText("apps/libwidget.trit");
    const std::string source = tests_next::readText("apps/" + source_name + ".trit");
    ctx.check(!sdk.empty(), "apps/os_sdk.trit is present for " + id);
    ctx.check(!widget.empty(), "apps/libwidget.trit is present for " + id);
    ctx.check(!source.empty(), source_name + " app source is present for draw driver");
    if (sdk.empty() || widget.empty() || source.empty()) return false;

    const std::string renamed = sourceWithRenamedMain(ctx, source, id + "_app_main");
    if (renamed.empty()) return false;
    CompileResult compiled = compileSource(id + "_draw.trit",
                                           sdk + "\n" + widget + "\n" +
                                               renamed + "\n" + driver);
    if (!compiled.success) {
        ctx.fail(id + " draw diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
        return false;
    }

    LinkOptions options;
    options.standalone_halt_on_exit = true;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    ctx.check(linked.success, id + " draw driver links");
    if (!linked.success) {
        ctx.fail(id + " draw link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return false;
    }

    ctx.check(sandbox::vm::loadAndReset(vm, linked.assembled.program),
              id + " draw image loads");
    const auto result = sandbox::vm::run(vm, 400000);
    if (!result.halted()) {
        std::ostringstream out;
        out << id << " draw driver did not halt; status=" << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return false;
    }
    ctx.equal(sandbox::vm::ops::toLong(vm.regfile.read(13)), 123LL,
              id + " draw driver returns sentinel");
    return sandbox::vm::ops::toLong(vm.regfile.read(13)) == 123;
}

sandbox::vm::ExecutableImageHeader dummyHeader() {
    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 4;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 64;
    return header;
}

bool installAppExecutable(TestContext& ctx,
                          OSKernel& kernel,
                          const ConsumerAppEntry& app) {
    const std::vector<long long> image = {900, 901, 902, 903};
    StatusResult installed = kernel.installExecutable(app.path, image, dummyHeader());
    ctx.check(installed.ok(), "consumer app executable installs: " + app.path);
    return installed.ok();
}

bool installAppById(TestContext& ctx, OSKernel& kernel, const std::string& id) {
    for (const ConsumerAppEntry& app : ConsumerShell::defaultApps()) {
        if (app.id == id) return installAppExecutable(ctx, kernel, app);
    }
    ctx.fail("default app id not found: " + id);
    return false;
}

void installAllDefaultExecutables(TestContext& ctx, OSKernel& kernel) {
    for (const ConsumerAppEntry& app : ConsumerShell::defaultApps()) {
        (void)installAppExecutable(ctx, kernel, app);
    }
}

void bootInstallLogin(TestContext& ctx, OSKernel& kernel, ConsumerShell& shell) {
    ctx.check(shell.boot().ok(), "consumer shell boots");
    ctx.check(shell.installBaseExperience().ok(), "consumer base experience installs");
    installAllDefaultExecutables(ctx, kernel);
    ctx.check(shell.completeFirstRun("ada", 777).ok(), "first-run user creates");
    ctx.check(shell.login("ada", 777).ok(), "user logs in");
}

bool hasLauncherApp(const ConsumerDesktopSnapshot& snapshot,
                    const std::string& id,
                    int capabilities) {
    for (const ConsumerAppEntry& app : snapshot.launcher) {
        if (app.id == id && app.capabilities == capabilities) return true;
    }
    return false;
}

void remainingGuiAppMarkers(TestContext& ctx) {
    struct AppCase {
        const char* source;
        const char* id;
        const char* marker;
        int cell_addr;
        long long cell_value;
    };
    const std::vector<AppCase> cases = {
        {"task_manager", "tasks", "TASKS\n", 60000, 84 + 10 * 256},
        {"text_editor", "text_editor", "TEXT\n", -1, 0},
        {"about", "about", "ABOUT\n", -1, 0},
        {"help", "help", "HELP\n", -1, 0},
    };

    for (const AppCase& app : cases) {
        sandbox::vm::VMState vm(65536, 1000000);
        const auto linked = compileBundledApp(ctx, app.source, app.id, 1024);
        if (!runLinkedApp(ctx, app.id, linked, vm, app.marker)) continue;
        if (app.cell_addr >= 0) {
            ctx.equal(wordAt(vm, app.cell_addr), app.cell_value,
                      std::string(app.id) + " draws its standalone title cell");
        }
    }
}

void remainingGuiDrawContracts(TestContext& ctx) {
    {
        sandbox::vm::VMState vm(65536, 1000000);
        const std::string driver = R"(
            fn main() -> t40 {
                os_gpu_mode(0);
                about_draw();
                return 123;
            }
        )";
        if (runAppDrawDriver(ctx, "about", "about", driver, vm)) {
            ctx.equal(wordAt(vm, 60000 + 4 * 80 + 16), textCell('-', 10),
                      "about draws the panel frame");
            ctx.equal(wordAt(vm, 60000 + 6 * 80 + 33), textCell('A', 3),
                      "about draws the title word");
            ctx.equal(wordAt(vm, 60000 + 9 * 80 + 25), textCell('O', 14),
                      "about draws the OS version row");
            ctx.equal(wordAt(vm, 60000 + 15 * 80 + 35), textCell('K', 1),
                      "about draws status OK");
        }
    }

    {
        sandbox::vm::VMState vm(65536, 1000000);
        const std::string driver = R"(
            fn main() -> t40 {
                os_gpu_mode(0);
                help_draw();
                return 123;
            }
        )";
        if (runAppDrawDriver(ctx, "help", "help", driver, vm)) {
            ctx.equal(wordAt(vm, 60000 + 0 * 80 + 8), textCell('H', 8),
                      "help draws the header");
            ctx.equal(wordAt(vm, 60000 + 4 * 80 + 8), textCell('1', 14),
                      "help draws launcher shortcut guidance");
            ctx.equal(wordAt(vm, 60000 + 8 * 80 + 16), textCell('N', 10),
                      "help draws terminal open guidance");
            ctx.equal(wordAt(vm, 60000 + 14 * 80 + 8), textCell('E', 8),
                      "help draws close guidance");
        }
    }

    {
        sandbox::vm::VMState vm(65536, 1000000);
        const std::string driver = R"(
            fn main() -> t40 {
                os_gpu_mode(0);
                unsafe {
                    store(TE_BUF + 0, 84);
                    store(TE_BUF + 1, 82);
                    store(TE_BUF + 2, 73);
                    store(TE_BUF + 3, 84);
                    store(TE_BUF + 4, 10);
                    store(TE_BUF + 5, 79);
                    store(TE_BUF + 6, 83);
                }
                te_draw_frame(7);
                return 123;
            }
        )";
        if (runAppDrawDriver(ctx, "text_editor", "text_editor", driver, vm)) {
            ctx.equal(wordAt(vm, 60000 + 0 * 80 + 1), textCell('O', 5),
                      "text editor draws the OS header");
            ctx.equal(wordAt(vm, 60000 + 3 * 80 + 5), textCell('-', 10),
                      "text editor draws the document frame");
            ctx.equal(wordAt(vm, 60000 + 5 * 80 + 8), textCell('T', 14),
                      "text editor draws the first note line");
            ctx.equal(wordAt(vm, 60000 + 6 * 80 + 8), textCell('O', 14),
                      "text editor handles newline layout");
            ctx.equal(wordAt(vm, 60000 + 6 * 80 + 10), textCell('_', 5),
                      "text editor draws the cursor after note text");
        }
    }

    {
        sandbox::vm::VMState vm(65536, 1000000);
        const std::string driver = R"(
            fn main() -> t40 {
                os_gpu_mode(0);
                os_clear_text(0);
                task_draw_state(5, 2, 1);
                task_draw_state(5, 3, 2);
                task_draw_state(5, 4, 3);
                task_draw_state(5, 5, 9);
                task_draw_selected_info(0);
                return 123;
            }
        )";
        if (runAppDrawDriver(ctx, "task_manager", "tasks", driver, vm)) {
            ctx.equal(wordAt(vm, 60000 + 2 * 80 + 5), textCell('R', 2),
                      "task manager draws runnable state");
            ctx.equal(wordAt(vm, 60000 + 3 * 80 + 6), textCell('U', 5),
                      "task manager draws running state");
            ctx.equal(wordAt(vm, 60000 + 4 * 80 + 7), textCell('K', 9),
                      "task manager draws blocked state");
            ctx.equal(wordAt(vm, 60000 + 5 * 80 + 5), textCell('C', 12),
                      "task manager draws crashed state");
            ctx.equal(wordAt(vm, 60000 + 19 * 80 + 18), textCell('P', 8),
                      "task manager draws selected pid label");
            ctx.equal(wordAt(vm, 60000 + 19 * 80 + 22), textCell('-', 8),
                      "task manager draws empty selected pid");
        }
    }
}

void launchFailureCleanup(TestContext& ctx) {
    OSKernel pre_login_kernel(128);
    ConsumerShell pre_login_shell(pre_login_kernel);
    ctx.check(pre_login_shell.boot().ok(), "pre-login shell boots");
    ctx.check(pre_login_shell.installBaseExperience().ok(),
              "pre-login base experience installs");
    ctx.equal(pre_login_shell.launchApp("calculator").detail, ERR_INVALID,
              "launching before login is rejected");
    ctx.equal(pre_login_shell.setPreference("accent", "red").detail, ERR_INVALID,
              "preference writes before login are rejected");

    OSKernel kernel(256);
    ConsumerShell shell(kernel);
    ctx.check(shell.boot().ok(), "launch-failure shell boots");
    ctx.check(shell.installBaseExperience().ok(), "base experience installs");
    ctx.check(shell.completeFirstRun("ada", 777).ok(), "first-run completes");
    ctx.check(shell.login("ada", 777).ok(), "user logs in");

    const StatusResult missing_exec = shell.launchApp("calculator");
    ctx.equal(missing_exec.detail, ERR_NOT_FOUND,
              "registry app with missing executable fails closed");
    ctx.equal(kernel.windowCount(), 0, "failed launch does not leak a window");
    ctx.check(shell.snapshot().taskbar_titles.empty(),
              "failed launch does not add a taskbar entry");
    ctx.check(kernel.process(2) == nullptr,
              "failed launch child process is killed and reaped");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "process isolation remains clean after failed launch");

    ctx.equal(shell.launchApp("not_installed").detail, ERR_NOT_FOUND,
              "unknown launcher id is rejected before forking");
    ctx.equal(shell.switchWindow(404).detail, ERR_NOT_FOUND,
              "switching an unknown window is rejected");
    ctx.equal(shell.closeWindow(404).detail, ERR_NOT_FOUND,
              "closing an unknown window is rejected");

    (void)installAppById(ctx, kernel, "calculator");
    const StatusResult calculator = shell.launchApp("calculator");
    ctx.check(calculator.ok(), "launcher recovers after missing executable is installed");
    ctx.equal(kernel.windowCount(), 1, "successful launch creates one window");
    ctx.equal(static_cast<int>(shell.snapshot().taskbar_titles.size()), 1,
              "successful launch creates one taskbar entry");
}

void taskbarDialogControls(TestContext& ctx) {
    OSKernel kernel(256);
    ConsumerShell shell(kernel);
    bootInstallLogin(ctx, kernel, shell);

    const StatusResult text = shell.launchApp("text_editor");
    const StatusResult tasks = shell.launchApp("tasks");
    const StatusResult settings = shell.launchApp("settings");
    ctx.check(text.ok() && tasks.ok() && settings.ok(),
              "text editor, task manager, and settings launch");
    if (!text.ok() || !tasks.ok() || !settings.ok()) return;

    ConsumerDesktopSnapshot desktop = shell.snapshot();
    ctx.equal(static_cast<int>(desktop.taskbar_titles.size()), 3,
              "taskbar tracks three open apps");
    ctx.equal(desktop.taskbar_titles[0], std::string("Text Editor"),
              "taskbar preserves first launched title");
    ctx.equal(desktop.taskbar_titles[1], std::string("Task Manager"),
              "taskbar preserves second launched title");
    ctx.equal(desktop.taskbar_titles[2], std::string("Settings"),
              "taskbar preserves third launched title");
    ctx.equal(desktop.active_window_id, settings.payload,
              "last launched app is active");

    ctx.check(shell.switchWindow(text.payload).ok(),
              "switching to text editor succeeds");
    ctx.equal(shell.snapshot().active_window_id, text.payload,
              "active window follows switch");
    ctx.check(shell.closeWindow(text.payload).ok(),
              "closing the active text editor succeeds");
    desktop = shell.snapshot();
    ctx.equal(static_cast<int>(desktop.taskbar_titles.size()), 2,
              "taskbar removes closed text editor");
    ctx.equal(desktop.active_window_id, tasks.payload,
              "closing active window focuses first remaining window");

    ctx.check(shell.markNotResponding(settings.payload).ok(),
              "settings can be marked not responding");
    ctx.check(shell.lastDialog().kind == ConsumerDialogKind::NotResponding,
              "not-responding dialog records settings");
    const int settings_pid = shell.lastDialog().pid;
    const Process* settings_proc = kernel.process(settings_pid);
    ctx.check(settings_proc != nullptr &&
                  settings_proc->state == sandbox::vm::PROC_STATE_STOPPED,
              "not-responding app process is suspended");
    ctx.check(shell.forceCloseDialogApp().ok(),
              "force-closing dialog app succeeds");
    ctx.equal(kernel.windowCount(), 1,
              "force close releases the settings window");
    ctx.equal(shell.forceCloseDialogApp().detail, ERR_NOT_FOUND,
              "forcing an already closed dialog app fails closed");

    const StatusResult terminal = shell.launchApp("terminal");
    ctx.check(terminal.ok(), "terminal launches for crash dialog");
    if (terminal.ok()) {
        ctx.check(shell.reportCrash(terminal.payload, -42).ok(),
                  "terminal crash closes the window");
        ctx.check(shell.lastDialog().kind == ConsumerDialogKind::Crash,
                  "crash dialog replaces the not-responding dialog");
        std::vector<long long> crash_log;
        ctx.check(kernel.fs().readFile("/var/crash/latest", crash_log).ok(),
                  "crash log is persisted");
        ctx.check(!crash_log.empty(), "crash log records payload");
    }
}

void customRegistryCapabilities(TestContext& ctx) {
    OSKernel kernel(256);
    ConsumerShell shell(kernel);
    ctx.check(shell.boot().ok(), "custom registry shell boots");
    const std::vector<ConsumerAppEntry> custom_apps = {
        {"help", "Help", "/bin/help", CONSUMER_APP_CAP_LAUNCH, true},
        {"settings", "Settings", "/bin/settings", CONSUMER_APP_CAP_SETTINGS, true},
    };
    ctx.check(shell.installBaseExperience(custom_apps).ok(),
              "custom app registry installs");
    for (const ConsumerAppEntry& app : custom_apps) {
        (void)installAppExecutable(ctx, kernel, app);
    }
    ctx.check(shell.completeFirstRun("ada", 777).ok(), "custom first-run completes");
    ctx.check(shell.login("ada", 777).ok(), "custom user logs in");

    ConsumerDesktopSnapshot desktop = shell.snapshot();
    ctx.equal(static_cast<int>(desktop.launcher.size()), 2,
              "custom registry limits launcher entries");
    ctx.check(hasLauncherApp(desktop, "help", CONSUMER_APP_CAP_LAUNCH),
              "custom registry preserves help launch capability");
    ctx.check(hasLauncherApp(desktop, "settings", CONSUMER_APP_CAP_SETTINGS),
              "custom registry preserves settings capability");
    ctx.equal(shell.launchApp("calculator").detail, ERR_NOT_FOUND,
              "apps outside the custom registry cannot launch");

    const StatusResult settings = shell.launchApp("settings");
    ctx.check(settings.ok(), "settings launches from custom registry");
    ctx.equal(shell.settingsInfo().open_windows, 1,
              "settings info counts custom-registry open windows");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"apps.bundle.remaining_gui_marker_smoke", "apps.consumer_bundle_contract",
         remainingGuiAppMarkers},
        {"apps.bundle.remaining_gui_draw_contracts", "apps.consumer_bundle_contract",
         remainingGuiDrawContracts},
        {"apps.shell.launch_failure_cleanup", "apps.consumer_bundle_contract",
         launchFailureCleanup},
        {"apps.shell.taskbar_dialog_controls", "apps.consumer_bundle_contract",
         taskbarDialogControls},
        {"apps.shell.custom_registry_capabilities", "apps.consumer_bundle_contract",
         customRegistryCapabilities},
    };
    return tests_next::runCases("next_apps_desktop_shell", cases);
}
