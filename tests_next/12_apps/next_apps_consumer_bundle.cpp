#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_consumer_shell.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::os;

constexpr int kParentPid = 1;

struct AppManifestEntry {
    std::string source;
    std::string id;
    std::string title;
    std::string guest_path;
    int stack_words = 0;
    bool gui_registry = false;
};

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

std::string jsonStringValue(const std::string& object, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_pos = object.find(marker);
    if (key_pos == std::string::npos) return {};
    const std::size_t colon = object.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return {};
    const std::size_t first_quote = object.find('"', colon + 1);
    if (first_quote == std::string::npos) return {};
    const std::size_t second_quote = object.find('"', first_quote + 1);
    if (second_quote == std::string::npos) return {};
    return object.substr(first_quote + 1, second_quote - first_quote - 1);
}

int jsonIntValue(const std::string& object, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_pos = object.find(marker);
    if (key_pos == std::string::npos) return 0;
    const std::size_t colon = object.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return 0;
    std::size_t pos = colon + 1;
    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
    int sign = 1;
    if (pos < object.size() && object[pos] == '-') {
        sign = -1;
        ++pos;
    }
    int value = 0;
    while (pos < object.size() && std::isdigit(static_cast<unsigned char>(object[pos]))) {
        value = value * 10 + (object[pos] - '0');
        ++pos;
    }
    return sign * value;
}

bool jsonBoolValue(const std::string& object, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_pos = object.find(marker);
    if (key_pos == std::string::npos) return false;
    const std::size_t colon = object.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return false;
    std::size_t pos = colon + 1;
    while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
    return object.compare(pos, 4, "true") == 0;
}

std::vector<std::string> jsonObjectBlocksInArray(const std::string& text,
                                                 const std::string& array_key) {
    std::vector<std::string> objects;
    const std::string marker = "\"" + array_key + "\"";
    const std::size_t key_pos = text.find(marker);
    if (key_pos == std::string::npos) return objects;
    const std::size_t array_begin = text.find('[', key_pos + marker.size());
    if (array_begin == std::string::npos) return objects;

    int array_depth = 1;
    int object_depth = 0;
    std::size_t object_begin = std::string::npos;
    for (std::size_t pos = array_begin + 1; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (ch == '[' && object_depth == 0) {
            ++array_depth;
        } else if (ch == ']' && object_depth == 0) {
            --array_depth;
            if (array_depth == 0) break;
        } else if (ch == '{') {
            if (object_depth == 0) object_begin = pos;
            ++object_depth;
        } else if (ch == '}') {
            --object_depth;
            if (object_depth == 0 && object_begin != std::string::npos) {
                objects.push_back(text.substr(object_begin, pos - object_begin + 1));
                object_begin = std::string::npos;
            }
        }
    }
    return objects;
}

std::vector<AppManifestEntry> parseBundledApps(const std::string& manifest) {
    std::vector<AppManifestEntry> entries;
    for (const std::string& object : jsonObjectBlocksInArray(manifest, "bundled_apps")) {
        AppManifestEntry entry;
        entry.source = jsonStringValue(object, "source");
        entry.id = jsonStringValue(object, "id");
        entry.title = jsonStringValue(object, "title");
        entry.guest_path = jsonStringValue(object, "guest_path");
        entry.stack_words = jsonIntValue(object, "stack_words");
        entry.gui_registry = jsonBoolValue(object, "gui_registry");
        entries.push_back(entry);
    }
    return entries;
}

std::string sourceBaseName(const std::string& source) {
    std::filesystem::path path(source);
    return path.stem().string();
}

const AppManifestEntry* findEntry(const std::vector<AppManifestEntry>& entries,
                                  const std::string& id) {
    for (const AppManifestEntry& entry : entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

std::vector<long long> programWords(const std::vector<sandbox::isa::TritWord27>& program) {
    std::vector<long long> words;
    words.reserve(program.size());
    for (const auto& word : program) words.push_back(static_cast<long long>(word.bits));
    return words;
}

bool containsSequence(const std::vector<long long>& haystack,
                      const std::vector<long long>& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end()) != haystack.end();
}

long long wordAt(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return -999999;
    return sandbox::vm::ops::toLong(value);
}

sandbox::compiler::LinkResult compileBundledApp(TestContext& ctx,
                                                const std::string& source_name,
                                                const std::string& id,
                                                int stack_words,
                                                bool standalone_halt_on_exit) {
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
    options.standalone_halt_on_exit = standalone_halt_on_exit;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    ctx.check(linked.success, id + " links with bundled app options");
    if (!linked.success) {
        ctx.fail(id + " link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return linked;
    }
    ctx.check(!linked.assembled.program.empty(), id + " emits executable text");
    ctx.equal(linked.executable_header.stack_words, stack_words,
              id + " executable header preserves release stack hint");
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

void manifestRegistryReleaseContract(TestContext& ctx) {
    const std::string manifest = tests_next::readText("APP_MANIFEST.json");
    const std::string image_builder = tests_next::readText("build_tos_image.cpp");
    ctx.check(!manifest.empty(), "APP_MANIFEST.json is present");
    ctx.check(!image_builder.empty(), "build_tos_image.cpp is present");
    if (manifest.empty() || image_builder.empty()) return;

    ctx.contains(manifest, "\"source_of_truth\": \"build_tos_image.cpp\"",
                 "app manifest names the image builder source of truth");
    ctx.contains(manifest, "\"shell\": \"/bin/desktop\"",
                 "default user shell is the desktop");
    ctx.contains(manifest, "\"path\": \"/apps/registry\"",
                 "GUI registry path is stable");
    for (const std::string& required_dir :
         {"/bin", "/apps", "/dev", "/system/services", "/var/packages", "/home/root/docs"}) {
        ctx.contains(manifest, "\"" + required_dir + "\"",
                     "root layout includes " + required_dir);
    }

    const std::vector<AppManifestEntry> entries = parseBundledApps(manifest);
    ctx.check(entries.size() >= 50, "release bundle manifest lists the expected app surface");
    std::set<std::string> ids;
    std::set<std::string> guest_paths;
    const std::set<std::string> gui_ids = {
        "desktop", "terminal", "files", "settings", "tasks",
        "calculator", "paint", "text_editor", "about", "help",
    };
    int gui_count = 0;
    for (const AppManifestEntry& entry : entries) {
        ctx.check(!entry.id.empty(), "app entry has an id");
        ctx.check(ids.insert(entry.id).second, "app id is unique: " + entry.id);
        ctx.check(guest_paths.insert(entry.guest_path).second,
                  "guest path is unique: " + entry.guest_path);
        ctx.check(std::filesystem::exists(entry.source),
                  "source exists for app id " + entry.id);

        const std::string builder_pattern =
            "{\"" + sourceBaseName(entry.source) + "\", \"" + entry.id + "\"";
        ctx.contains(image_builder, builder_pattern,
                     "image builder contains manifest app " + entry.id);

        if (entry.gui_registry) {
            ++gui_count;
            ctx.check(gui_ids.count(entry.id) == 1,
                      "only GUI apps are recorded in the GUI registry: " + entry.id);
            ctx.equal(entry.stack_words, 1024,
                      "GUI app stack hint follows release image builder");
        } else {
            ctx.check(gui_ids.count(entry.id) == 0,
                      "GUI app is not missing from registry: " + entry.id);
        }
    }
    ctx.equal(gui_count, static_cast<int>(gui_ids.size()),
              "GUI registry covers every expected launcher app");
    ctx.contains(image_builder, "rootfs.addFile(\"/apps/registry\", registry)",
                 "image builder installs the GUI app registry");
    ctx.contains(image_builder, "addUserRecord(\"root\", 333667, \"/home/root\", \"/bin/desktop\")",
                 "image builder seeds root user with desktop shell");
}

void bundledAppFdZeroOpenGuards(TestContext& ctx) {
    const std::string manifest = tests_next::readText("APP_MANIFEST.json");
    const std::vector<AppManifestEntry> entries = parseBundledApps(manifest);
    ctx.check(!entries.empty(), "app manifest entries are available for fd guard audit");
    if (entries.empty()) return;

    std::set<std::string> audited_sources;
    for (const AppManifestEntry& entry : entries) {
        if (!audited_sources.insert(entry.source).second) continue;
        const std::string source = tests_next::readText(entry.source);
        ctx.check(!source.empty(), "app source is readable for fd guard audit: " + entry.source);
        if (source.empty()) continue;

        ctx.check(source.find("fd <= 0") == std::string::npos,
                  entry.source + " treats fd 0 as a valid open file descriptor");
    }
}

void directGuiAppMarkers(TestContext& ctx) {
    sandbox::vm::VMState desktop_vm(65536, 1000000);
    const auto desktop = compileBundledApp(ctx, "desktop", "desktop", 1024, true);
    if (runLinkedApp(ctx, "desktop", desktop, desktop_vm, "DESKTOP\n")) {
        ctx.equal(wordAt(desktop_vm, 60000), 84LL + 11LL * 256LL,
                  "desktop draws its launcher title cell");
    }

    sandbox::vm::VMState calculator_vm(65536, 1000000);
    const auto calculator = compileBundledApp(ctx, "calculator", "calculator", 1024, true);
    if (runLinkedApp(ctx, "calculator", calculator, calculator_vm, "144 + 12 = 156\n")) {
        ctx.equal(sandbox::vm::ops::toLong(calculator_vm.regfile.read(13)), 156LL,
                  "calculator returns the computed result");
    }

    sandbox::vm::VMState paint_vm(65536, 1000000);
    const auto paint = compileBundledApp(ctx, "paint", "paint", 1024, true);
    if (runLinkedApp(ctx, "paint", paint, paint_vm, "PAINT\n")) {
        ctx.equal(paint_vm.gpu_mode, 1LL, "paint switches into graphics mode");
        ctx.equal(paint_vm.sprite_attr, 43LL + 14LL * 256LL,
                  "paint publishes a cursor sprite");
        ctx.equal(wordAt(paint_vm, 55000 + 6 * 80 + 6), 4LL,
                  "paint fills the primary canvas through the GPU CSR");
        ctx.equal(wordAt(paint_vm, 60000), 80LL + 14LL * 256LL,
                  "paint labels its text overlay");
    }

    sandbox::vm::VMState files_vm(65536, 1000000);
    const auto files = compileBundledApp(ctx, "file_manager", "files", 1024, true);
    if (runLinkedApp(ctx, "files", files, files_vm, "FILES\n")) {
        ctx.equal(wordAt(files_vm, 60000), 70LL + 10LL * 256LL,
                  "file manager draws its title cell");
    }

    sandbox::vm::VMState settings_vm(65536, 1000000);
    const auto settings = compileBundledApp(ctx, "settings", "settings", 1024, true);
    if (runLinkedApp(ctx, "settings", settings, settings_vm, "SETTINGS\n")) {
        ctx.equal(wordAt(settings_vm, 60000), 83LL + 10LL * 256LL,
                  "settings draws its title cell");
    }

    sandbox::vm::VMState terminal_vm(65536, 1000000);
    const auto terminal = compileBundledApp(ctx, "terminal", "terminal", 1024, true);
    if (runLinkedApp(ctx, "terminal", terminal, terminal_vm, "TERMINAL\n")) {
        ctx.equal(wordAt(terminal_vm, 60000), 84LL + 10LL * 256LL,
                  "terminal draws its title cell");
    }
}

void cliServiceAliasLinkOptions(TestContext& ctx) {
    const std::string manifest = tests_next::readText("APP_MANIFEST.json");
    const std::vector<AppManifestEntry> entries = parseBundledApps(manifest);
    const std::map<std::string, std::string> expected_source = {
        {"sh", "shell"},
        {"top", "task_manager"},
        {"cat", "bin_core"},
        {"edit", "text_editor"},
        {"sessiond", "service_stub"},
    };
    const std::map<std::string, int> expected_stack = {
        {"sh", 256},
        {"top", 256},
        {"cat", 128},
        {"edit", 256},
        {"sessiond", 128},
    };

    for (const auto& [id, source_name] : expected_source) {
        const AppManifestEntry* entry = findEntry(entries, id);
        ctx.check(entry != nullptr, "manifest includes alias " + id);
        if (entry != nullptr) {
            ctx.equal(sourceBaseName(entry->source), source_name,
                      id + " alias points at shared source");
            ctx.equal(entry->stack_words, expected_stack.at(id),
                      id + " manifest stack matches release role");
            ctx.check(!entry->gui_registry, id + " stays out of GUI registry");
        }

        const auto linked =
            compileBundledApp(ctx, source_name, id, expected_stack.at(id), false);
        if (!linked.success) continue;
        ctx.check(linked.executable_header.text_pages * sandbox::vm::MMU_PAGE_WORDS >=
                      linked.instruction_count,
                  id + " executable header covers emitted text");
    }
}

void diskBackedCalculatorExec(TestContext& ctx) {
    const auto linked =
        compileBundledApp(ctx, "calculator", "calculator", 1024, false);
    if (!linked.success) return;
    const std::vector<long long> image = programWords(linked.assembled.program);
    ctx.check(!image.empty(), "calculator executable image has words");

    constexpr int kCalculatorTextPpn = 8100;
    NativeVfsImageBuilder rootfs(32768);
    ctx.check(rootfs.status().ok(), "native VFS image builder formats");
    ctx.check(rootfs.installBaseLayout().ok(), "release root layout installs");
    ctx.check(rootfs.addExecutableImage("/bin/calculator",
                                        linked.assembled.program,
                                        linked.executable_header,
                                        kCalculatorTextPpn).ok(),
              "compiled calculator installs into the release disk image");
    const std::vector<long long> disk = rootfs.image();
    ctx.check(!disk.empty(), "release root image serializes");

    const int first_text_word = NATIVE_VFS_REQUIRED_BLOCKS * BLOCK_WORDS;
    ctx.check(first_text_word + static_cast<int>(image.size()) <=
                  static_cast<int>(disk.size()),
              "serialized disk has room for calculator text blocks");
    if (first_text_word + static_cast<int>(image.size()) <=
        static_cast<int>(disk.size())) {
        ctx.equal(disk[static_cast<std::size_t>(first_text_word)],
                  image.front(),
                  "calculator text first word is written to the native disk image");
        ctx.equal(disk[static_cast<std::size_t>(first_text_word + image.size() - 1)],
                  image.back(),
                  "calculator text last word is written to the native disk image");
    }

    const std::vector<long long> descriptor = {
        sandbox::vm::EXEC_MAGIC,
        linked.executable_header.version,
        linked.executable_header.abi_version,
        linked.executable_header.entry_virtual_pc,
        linked.executable_header.text_pages,
        linked.executable_header.data_pages,
        linked.executable_header.stack_words,
        linked.executable_header.syscall_abi_version,
        linked.executable_header.flags,
        kCalculatorTextPpn,
        NATIVE_VFS_REQUIRED_BLOCKS,
        static_cast<long long>(image.size()),
    };
    ctx.check(containsSequence(disk, descriptor),
              "native disk image stores calculator executable metadata and text block");
}

bool directoryContains(const std::vector<DirectoryEntry>& entries, const std::string& name) {
    for (const DirectoryEntry& entry : entries) {
        if (entry.name == name) return true;
    }
    return false;
}

void installDummyConsumerApps(TestContext& ctx, OSKernel& kernel) {
    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 4;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 64;
    const std::vector<long long> image = {900, 901, 902, 903};
    for (const ConsumerAppEntry& app : ConsumerShell::defaultApps()) {
        ctx.check(kernel.installExecutable(app.path, image, header).ok(),
                  "consumer app executable installs: " + app.path);
    }
}

void consumerShellLifecyclePersistence(TestContext& ctx) {
    OSKernel kernel(256);
    ConsumerShell shell(kernel);
    ctx.check(shell.boot().ok(), "consumer shell boots");
    ctx.check(shell.snapshot().boot_splash_seen, "boot splash is recorded");
    ctx.check(shell.installBaseExperience().ok(), "base consumer layout installs");
    installDummyConsumerApps(ctx, kernel);

    std::vector<DirectoryEntry> apps_dir;
    ctx.check(shell.listDirectory("/apps", apps_dir).ok(),
              "apps directory lists after base install");
    ctx.check(directoryContains(apps_dir, "registry"),
              "app registry exists under /apps");

    ctx.check(shell.completeFirstRun("ada", 777).ok(),
              "first-run setup creates user");
    ctx.check(shell.login("ada", 777).ok(), "user logs in");
    ConsumerDesktopSnapshot desktop = shell.snapshot();
    ctx.check(desktop.phase == ConsumerShellPhase::Desktop,
              "consumer shell reaches desktop phase");
    ctx.check(desktop.launcher.size() >= 9,
              "launcher exposes essential consumer apps");

    const StatusResult calc = shell.launchApp("calculator");
    const StatusResult files = shell.launchApp("files");
    ctx.check(calc.ok(), "launcher starts calculator");
    ctx.check(files.ok(), "launcher starts file manager");
    desktop = shell.snapshot();
    ctx.equal(static_cast<int>(desktop.taskbar_titles.size()), 2,
              "taskbar tracks launched app windows");
    if (calc.ok()) {
        ctx.check(shell.switchWindow(calc.payload).ok(),
                  "window switching focuses calculator");
        ctx.equal(shell.snapshot().active_window_id, calc.payload,
                  "active window id follows focus");
    }

    ctx.check(shell.createFolder("/home/ada/docs").ok(),
              "file manager creates a user folder");
    ctx.check(shell.writeUserFile("/home/ada/docs/readme", {84, 82, 73, 84}).ok(),
              "file manager writes user file words");
    std::vector<DirectoryEntry> docs;
    ctx.check(shell.listDirectory("/home/ada/docs", docs).ok(),
              "file manager lists the user folder");
    ctx.check(directoryContains(docs, "readme"),
              "created file appears in listing");

    ConsumerSettingsInfo settings = shell.settingsInfo();
    ctx.equal(settings.display_width, 80, "settings reports display width");
    ctx.equal(settings.display_height, 60, "settings reports display height");
    ctx.equal(settings.disk_blocks, 256, "settings reports disk geometry");
    ctx.equal(settings.active_user, std::string("ada"),
              "settings reports active user");

    const StatusResult terminal = shell.launchApp("terminal");
    ctx.check(terminal.ok(), "terminal launches as an app window");
    if (terminal.ok()) {
        ctx.check(shell.reportCrash(terminal.payload, -9).ok(),
                  "terminal crash report is persisted");
        ctx.check(shell.lastDialog().kind == ConsumerDialogKind::Crash,
                  "crash dialog records the failed app");
        std::vector<long long> crash_log;
        ctx.check(kernel.fs().readFile("/var/crash/latest", crash_log).ok(),
                  "crash log is stored in the VFS");
        ctx.check(!crash_log.empty(), "crash log has payload");
    }

    ctx.check(shell.setPreference("accent", "cyan").ok(),
              "preference write succeeds");
    ctx.check(shell.shutdown().ok(), "consumer shell shuts down");
    OSKernel rebooted(kernel.diskImage());
    ConsumerShell resumed(rebooted);
    ctx.check(resumed.boot().ok(), "rebooted shell mounts persisted disk");
    ctx.check(resumed.login("ada", 777).ok(), "user logs in after reboot");
    ctx.equal(resumed.preference("accent"), std::string("cyan"),
              "preferences survive reboot");
    std::vector<DirectoryEntry> rebooted_docs;
    ctx.check(resumed.listDirectory("/home/ada/docs", rebooted_docs).ok(),
              "home docs directory survives reboot");
    ctx.check(directoryContains(rebooted_docs, "readme"),
              "managed user file survives reboot");
}

void consumerShellIndirectCliExecutableHandoff(TestContext& ctx) {
    OSKernel kernel(256);
    ConsumerShell shell(kernel);
    const std::vector<ConsumerAppEntry> cli_apps = {
        {"sync", "sync", "/bin/sync", CONSUMER_APP_CAP_SYSTEM, false},
    };
    ctx.check(shell.boot().ok(), "real CLI handoff shell boots");
    ctx.check(shell.installBaseExperience(cli_apps).ok(),
              "real CLI handoff custom registry installs");
    ctx.check(shell.completeFirstRun("ada", 777).ok(),
              "real CLI handoff first-run completes");
    ctx.check(shell.login("ada", 777).ok(),
              "real CLI handoff user logs in");

    const auto linked = compileBundledApp(ctx, "sync", "sync", 128, false);
    if (!linked.success) return;
    const std::vector<long long> image = programWords(linked.assembled.program);
    ctx.check(static_cast<int>(image.size()) > DIRECT_BLOCKS * BLOCK_WORDS,
              "compiled sync image crosses the direct-block fixture boundary");
    ctx.check(kernel.installExecutable("/bin/sync",
                                       image,
                                       linked.executable_header).ok(),
              "compiled sync executable installs through OSKernel facade");

    FileStat sync_stat;
    ctx.check(kernel.fs().stat("/bin/sync", sync_stat).ok(),
              "installed sync executable can be statted");
    ctx.equal(sync_stat.direct_blocks, DIRECT_BLOCKS,
              "compiled sync executable fills direct blocks");
    ctx.check(sync_stat.indirect_block >= 0,
              "compiled sync executable allocates an indirect extent");

    const StatusResult launched = shell.launchApp("sync");
    ctx.check(launched.ok(), "consumer shell launches real compiled sync app");
    if (!launched.ok()) return;
    ctx.equal(kernel.windowCount(), 1,
              "real executable launch creates one window");
    ConsumerDesktopSnapshot desktop = shell.snapshot();
    ctx.equal(desktop.active_window_id, launched.payload,
              "real executable launch becomes active");
    ctx.equal(static_cast<int>(desktop.taskbar_titles.size()), 1,
              "real executable launch enters the taskbar");
    ctx.equal(desktop.taskbar_titles[0], std::string("sync"),
              "taskbar uses the custom CLI registry title");

    const Process* proc = kernel.process(2);
    ctx.check(proc != nullptr, "real executable launch forks child pid 2");
    if (proc == nullptr) return;
    ctx.equal(proc->parent_pid, kParentPid,
              "real executable child remains parented by the session process");
    ctx.equal(proc->state, sandbox::vm::PROC_STATE_RUNNABLE,
              "real executable child is runnable after sysExec");
    ctx.equal(static_cast<int>(proc->memory.size()), static_cast<int>(image.size()),
              "sysExec loads the full compiled sync image into process memory");
    ctx.equal(proc->memory.front(), image.front(),
              "sysExec preserves the first executable word");
    ctx.equal(proc->memory.back(), image.back(),
              "sysExec preserves the last executable word from indirect storage");
    ctx.equal(proc->memory[static_cast<std::size_t>(DIRECT_BLOCKS * BLOCK_WORDS)],
              image[static_cast<std::size_t>(DIRECT_BLOCKS * BLOCK_WORDS)],
              "sysExec preserves words beyond the direct-block boundary");
    ctx.equal(proc->exec_header.stack_words, linked.executable_header.stack_words,
              "sysExec preserves the executable stack hint");
    ctx.equal(proc->exec_header.text_pages, linked.executable_header.text_pages,
              "sysExec preserves text page metadata");
    ctx.equal(proc->heap_start,
              linked.executable_header.data_pages * sandbox::vm::MMU_PAGE_WORDS,
              "sysExec initializes heap start from executable data pages");

    ProcessInfo info;
    ctx.check(kernel.sysGetProc(2, info).ok(),
              "process info reads the real executable child");
    ctx.equal(info.memory_words, static_cast<int>(image.size()),
              "process info reports the compiled image size");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "real executable launch keeps process/window ownership consistent");
}

void consumerShellGuiSizedExecutableHandoff(TestContext& ctx) {
    OSKernel kernel(2048);
    ConsumerShell shell(kernel);
    const std::vector<ConsumerAppEntry> gui_apps = {
        {"calculator", "Calculator", "/bin/calculator", CONSUMER_APP_CAP_LAUNCH, true},
    };
    ctx.check(shell.boot().ok(), "GUI executable handoff shell boots");
    ctx.check(shell.installBaseExperience(gui_apps).ok(),
              "GUI executable handoff custom registry installs");
    ctx.check(shell.completeFirstRun("ada", 777).ok(),
              "GUI executable handoff first-run completes");
    ctx.check(shell.login("ada", 777).ok(),
              "GUI executable handoff user logs in");

    const auto linked = compileBundledApp(ctx, "calculator", "calculator", 1024, false);
    if (!linked.success) return;
    const std::vector<long long> image = programWords(linked.assembled.program);
    const std::size_t old_one_index_capacity_words =
        static_cast<std::size_t>((DIRECT_BLOCKS + BLOCK_WORDS) * BLOCK_WORDS);
    ctx.check(image.size() > old_one_index_capacity_words,
              "compiled calculator exceeds the old one-indirect-block capacity");
    ctx.check(kernel.installExecutable("/bin/calculator",
                                       image,
                                       linked.executable_header).ok(),
              "compiled calculator executable installs through OSKernel facade");

    FileStat calc_stat;
    ctx.check(kernel.fs().stat("/bin/calculator", calc_stat).ok(),
              "installed calculator executable can be statted");
    ctx.equal(calc_stat.direct_blocks, DIRECT_BLOCKS,
              "compiled calculator executable fills direct blocks");
    ctx.check(calc_stat.indirect_block >= 0,
              "compiled calculator executable allocates chained indirect storage");
    std::vector<long long> readback;
    ctx.check(kernel.fs().readFile("/bin/calculator", readback).ok(),
              "compiled calculator executable reads back from the VFS");
    ctx.check(readback == image,
              "compiled calculator executable payload is exact before launch");
    ctx.check(kernel.checkFilesystemConsistency().ok(),
              "GUI executable filesystem is consistent before launch");

    const StatusResult launched = shell.launchApp("calculator");
    ctx.check(launched.ok(), "consumer shell launches real compiled calculator app");
    if (!launched.ok()) return;
    ctx.equal(kernel.windowCount(), 1,
              "GUI executable launch creates one window");
    ConsumerDesktopSnapshot desktop = shell.snapshot();
    ctx.equal(desktop.active_window_id, launched.payload,
              "GUI executable launch becomes active");
    ctx.equal(static_cast<int>(desktop.taskbar_titles.size()), 1,
              "GUI executable launch enters the taskbar");
    ctx.equal(desktop.taskbar_titles[0], std::string("Calculator"),
              "taskbar uses the GUI registry title");

    const Process* proc = kernel.process(2);
    ctx.check(proc != nullptr, "GUI executable launch forks child pid 2");
    if (proc == nullptr) return;
    ctx.equal(proc->parent_pid, kParentPid,
              "GUI executable child remains parented by the session process");
    ctx.equal(proc->state, sandbox::vm::PROC_STATE_RUNNABLE,
              "GUI executable child is runnable after sysExec");
    ctx.equal(static_cast<int>(proc->memory.size()), static_cast<int>(image.size()),
              "sysExec loads the full compiled calculator image into process memory");
    ctx.equal(proc->memory.front(), image.front(),
              "sysExec preserves the first calculator word");
    ctx.equal(proc->memory.back(), image.back(),
              "sysExec preserves the last calculator word from chained storage");
    ctx.equal(proc->memory[old_one_index_capacity_words],
              image[old_one_index_capacity_words],
              "sysExec preserves words beyond the old one-indirect-block capacity");
    ctx.equal(proc->exec_header.stack_words, linked.executable_header.stack_words,
              "sysExec preserves the calculator stack hint");
    ctx.equal(proc->exec_header.text_pages, linked.executable_header.text_pages,
              "sysExec preserves calculator text page metadata");
    ctx.equal(proc->heap_start,
              linked.executable_header.data_pages * sandbox::vm::MMU_PAGE_WORDS,
              "sysExec initializes calculator heap start from executable data pages");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "GUI executable launch keeps process/window ownership consistent");

    ctx.check(kernel.shutdownSync().ok(),
              "GUI executable filesystem syncs before reboot");
    OSKernel rebooted(kernel.diskImage());
    ctx.check(rebooted.boot().ok(),
              "GUI executable disk image remounts after reboot");
    ctx.check(rebooted.checkFilesystemConsistency().ok(),
              "GUI executable filesystem is consistent after reboot");
    const StatusResult rebooted_exec = rebooted.sysExec(kParentPid, "/bin/calculator");
    ctx.check(rebooted_exec.ok(),
              "rebooted kernel execs calculator from chained storage");
    const Process* rebooted_proc = rebooted.process(kParentPid);
    ctx.check(rebooted_proc != nullptr,
              "rebooted kernel retains the session process");
    if (rebooted_proc == nullptr || !rebooted_exec.ok()) return;
    ctx.equal(static_cast<int>(rebooted_proc->memory.size()), static_cast<int>(image.size()),
              "rebooted sysExec loads the full calculator image");
    ctx.equal(rebooted_proc->memory[old_one_index_capacity_words],
              image[old_one_index_capacity_words],
              "rebooted sysExec preserves words beyond the old storage ceiling");
    ctx.equal(rebooted_proc->exec_header.stack_words,
              linked.executable_header.stack_words,
              "rebooted sysExec preserves executable metadata");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"apps.manifest.registry_release_contract", "apps.consumer_bundle_contract",
         manifestRegistryReleaseContract},
        {"apps.bundle.fd_zero_open_guards", "apps.consumer_bundle_contract",
         bundledAppFdZeroOpenGuards},
        {"apps.bundle.gui_marker_smoke", "apps.consumer_bundle_contract",
         directGuiAppMarkers},
        {"apps.bundle.cli_service_alias_link_options", "apps.consumer_bundle_contract",
         cliServiceAliasLinkOptions},
        {"apps.bundle.disk_backed_calculator_exec", "apps.consumer_bundle_contract",
         diskBackedCalculatorExec},
        {"apps.shell.consumer_lifecycle_persistence", "apps.consumer_bundle_contract",
         consumerShellLifecyclePersistence},
        {"apps.shell.indirect_cli_executable_handoff", "apps.consumer_bundle_contract",
         consumerShellIndirectCliExecutableHandoff},
        {"apps.shell.gui_sized_executable_handoff", "apps.consumer_bundle_contract",
         consumerShellGuiSizedExecutableHandoff},
    };
    return tests_next::runCases("next_apps_consumer_bundle", cases);
}
