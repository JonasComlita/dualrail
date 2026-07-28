#include "ternary_compiler.h"
#include "ternary_host_runtime.h"
#include "ternary_os.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BundledApp {
    std::string source_name;
    std::string id;
    std::string title;
    std::string guest_path;
    int text_ppn = 0;
    int stack_words = 256;
    bool gui_registry = false;
};

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void dumpDiagnostics(const sandbox::compiler::CompileResult& compiled) {
    for (const auto& diagnostic : compiled.diagnostics) {
        std::cerr << "  " << diagnostic.format() << "\n";
    }
}

void appendStoreWord(std::ostringstream& out, int addr, int offset, int value) {
    out << "    mov r1, " << value << "\n";
    out << "    mov r2, " << addr << "\n";
    out << "    store r1, r2, " << offset << "\n";
}

void appendStoreCString(std::ostringstream& out, int addr, const std::string& text) {
    for (int i = 0; i < static_cast<int>(text.size()); ++i) {
        appendStoreWord(out, addr, i,
                        static_cast<unsigned char>(text[static_cast<std::size_t>(i)]));
    }
    appendStoreWord(out, addr, static_cast<int>(text.size()), 0);
}

void appendStoreTextCells(std::ostringstream& out,
                          int x,
                          int y,
                          const std::string& text,
                          int color) {
    constexpr int kTextBase = 60000;
    constexpr int kTextWidth = 80;
    const int addr = kTextBase + y * kTextWidth + x;
    for (int i = 0; i < static_cast<int>(text.size()); ++i) {
        const int ch = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        appendStoreWord(out, addr, i, ch + color * 256);
    }
}

void appendStringWords(std::vector<long long>& out, const std::string& text) {
    out.push_back(static_cast<long long>(text.size()));
    for (unsigned char c : text) {
        out.push_back(static_cast<long long>(c));
    }
}

std::vector<long long> asciiWords(const std::string& text) {
    std::vector<long long> out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        out.push_back(static_cast<long long>(c));
    }
    return out;
}

int alignUp(int value, int alignment) {
    if (alignment <= 1) return value;
    const int remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

int pagesForWords(int words) {
    return words <= 0 ? 0 : (words + sandbox::vm::MMU_PAGE_WORDS - 1) /
                             sandbox::vm::MMU_PAGE_WORDS;
}

std::string defaultDiskPathForBoot(const std::string& boot_path) {
    const std::size_t slash = boot_path.find_last_of("/\\");
    const std::size_t dot = boot_path.find_last_of('.');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        return boot_path.substr(0, dot) + ".tdisk";
    }
    return boot_path + ".tdisk";
}

std::string buildBootExecAssembly(const std::string& path) {
    std::ostringstream boot;
    boot << ".text\n";
    boot << "boot:\n";
    boot << "    mov sp, 16383\n";
    appendStoreTextCells(boot, 1, 0, "OS 3 - BOOTING", 7);
    boot << "    call kernel_init\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 0\n";
    boot << "    mov r15, 0\n";
    boot << "    call ipc_open\n";
    boot << "    mov r1, native_trap_entry\n";
    boot << "    csrw tvec, r1\n";

    appendStoreCString(boot, 10020, path);
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 0\n";
    boot << "    call app_launch\n";

    boot << "    mov r2, 3019\n";
    boot << "    load r1, r2, 0\n";
    boot << "    csrw scratch, r1\n";
    boot << "    load r3, r1, 0\n";
    boot << "    csrw epc, r3\n";
    boot << "    load r3, r1, 1\n";
    boot << "    csrw status, r3\n";
    boot << "    load r3, r1, 2\n";
    boot << "    csrw user_imem_ptbr, r3\n";
    boot << "    load r3, r1, 3\n";
    boot << "    csrw user_imem_pages, r3\n";
    boot << "    load r3, r1, 4\n";
    boot << "    csrw user_dmem_ptbr, r3\n";
    boot << "    load r3, r1, 5\n";
    boot << "    csrw user_dmem_pages, r3\n";
    boot << "    load r3, r1, 31\n";
    boot << "    copy sp, r3\n";
    boot << "    eret\n";
    return boot.str();
}

sandbox::compiler::LinkResult compileApp(const BundledApp& app, bool& ok) {
    using namespace sandbox::compiler;

    const std::string architecture =
        readTextFile("generated/architecture_contract.trit");
    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string widget = readTextFile("apps/libwidget.trit");
    const std::string source = readTextFile("apps/" + app.source_name + ".trit");
    if (architecture.empty() || sdk.empty() || widget.empty() ||
        source.empty()) {
        std::cerr << "missing source for app " << app.id << "\n";
        ok = false;
        return {};
    }

    CompileResult compiled = compileSource(
        app.source_name + ".trit",
        architecture + "\n" + sdk + "\n" + widget + "\n" + source);
    if (!compiled.success) {
        std::cerr << "compile failed for " << app.id << ":\n";
        dumpDiagnostics(compiled);
        ok = false;
        return {};
    }

    LinkOptions options;
    options.stack_hint_words = alignUp(app.stack_words, 9);
    options.standalone_halt_on_exit = false;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    if (!linked.success) {
        std::cerr << "link failed for " << app.id << "\n";
        for (const auto& diagnostic : linked.diagnostics) {
            std::cerr << "  " << diagnostic.format() << "\n";
        }
        for (const auto& assembly_error : linked.assembled.errors) {
            std::cerr << "  " << assembly_error.format() << "\n";
        }
        ok = false;
    }
    const char* dump_dir = std::getenv("TRIT_DUMP_APP_ASM_DIR");
    if (dump_dir && *dump_dir && linked.success) {
        std::filesystem::create_directories(dump_dir);
        std::ofstream out(std::filesystem::path(dump_dir) /
                          (app.id + ".linked.tasm"));
        out << linked.assembly;
    }
    return linked;
}

void appendRegistryApp(std::vector<long long>& registry, const BundledApp& app) {
    appendStringWords(registry, app.id);
    appendStringWords(registry, app.title);
    appendStringWords(registry, app.guest_path);
    registry.push_back(1);
    registry.push_back(app.gui_registry ? 1 : 0);
}

bool addTextFile(sandbox::os::NativeVfsImageBuilder& rootfs,
                 const std::string& path,
                 const std::string& text) {
    return rootfs.addFile(path, asciiWords(text)).ok();
}

bool installEssentialRootFiles(sandbox::os::NativeVfsImageBuilder& rootfs,
                               const std::vector<BundledApp>& apps,
                               const std::string& image_version) {
    using sandbox::os::StatusResult;

    for (const std::string& dir : {"/dev", "/system", "/system/services",
                                   "/lib", "/var/crash", "/var/packages",
                                   "/home/root/docs"}) {
        StatusResult made = rootfs.mkdir(dir);
        if (!made.ok()) return false;
    }

    std::vector<long long> registry;
    int gui_count = 0;
    for (const BundledApp& app : apps) {
        if (app.gui_registry) ++gui_count;
    }
    registry.push_back(gui_count);
    for (const BundledApp& app : apps) {
        if (app.gui_registry) appendRegistryApp(registry, app);
    }
    if (!rootfs.addFile("/apps/registry", registry).ok()) return false;

    for (const BundledApp& app : apps) {
        if (!app.gui_registry) continue;
        const std::string metadata =
            "id=" + app.id + "\n"
            "title=" + app.title + "\n"
            "path=" + app.guest_path + "\n"
            "windowed=1\n";
        if (!addTextFile(rootfs, "/apps/" + app.id + ".app", metadata)) {
            return false;
        }
    }

    std::string bin_manifest;
    for (const BundledApp& app : apps) {
        bin_manifest += app.guest_path + " " + app.id + " " + app.source_name + "\n";
    }
    if (!addTextFile(rootfs, "/system/bin.manifest", bin_manifest)) return false;

    const std::string service_manifest =
        "init first user process\n"
        "sessiond starts login and desktop sessions\n"
        "window_server owns windows and input routing\n"
        "inputd normalizes keyboard and mouse events\n"
        "mountd mounts root and user disks\n"
        "logd records system events\n"
        "crashd records app faults\n"
        "updated stages system updates\n"
        "packaged installs and removes apps\n"
        "devd coordinates devices and drivers\n"
        "timed owns the system clock\n"
        "authd owns users and sessions\n"
        "powerd coordinates shutdown and reboot\n";
    if (!addTextFile(rootfs, "/system/services/manifest", service_manifest)) return false;

    for (const std::string& service : {"sessiond", "window_server", "inputd",
                                       "mountd", "logd", "crashd", "updated",
                                       "packaged", "devd", "timed", "authd",
                                       "powerd"}) {
        if (!addTextFile(rootfs, "/system/services/" + service, "enabled\n")) {
            return false;
        }
    }

    for (const std::string& dev : {"null", "zero", "console", "keyboard",
                                   "mouse", "fb0", "random", "disk0"}) {
        if (!rootfs.addFile("/dev/" + dev, {0}).ok()) return false;
    }

    if (!addTextFile(rootfs, "/etc/os-release",
                     "NAME=Ternary OS\n"
                     "ID=ternary\n"
                     "VERSION=" + image_version + "\n"
                     "PROFILE=minimum\n")) {
        return false;
    }
    if (!addTextFile(rootfs, "/etc/issue", "Ternary OS 3\n")) return false;
    if (!addTextFile(rootfs, "/etc/motd", "Welcome to Ternary OS.\n")) return false;
    if (!addTextFile(rootfs, "/etc/fstab", "disk0 / vfs rw\n")) return false;
    if (!addTextFile(rootfs, "/etc/profile", "PATH=/bin\nHOME=/home/root\n")) return false;
    if (!rootfs.addFile("/etc/os3.cfg", {7, 1, 8, 0}).ok()) return false;
    if (!rootfs.addFile("/etc/first_run", {0}).ok()) return false;
    if (!addTextFile(rootfs, "/etc/shell_prefs", "accent=green\nscale=1\n")) return false;
    if (!addTextFile(rootfs, "/system/build",
                     "version=" + image_version + "\n"
                     "profile=minimum\n"
                     "kernel=trit-native\n")) {
        return false;
    }
    if (!addTextFile(rootfs, "/var/log/boot.log",
                     "init: root filesystem mounted\n"
                     "sessiond: desktop target ready\n")) {
        return false;
    }
    if (!addTextFile(rootfs, "/var/log/system.log", "logd: ready\n")) return false;
    if (!addTextFile(rootfs, "/var/crash/README", "crash reports land here\n")) return false;
    if (!addTextFile(rootfs, "/var/packages/status", "base-system installed\n")) return false;
    if (!addTextFile(rootfs, "/home/root/README",
                     "This is the root home directory for the Ternary OS image.\n")) {
        return false;
    }
    return true;
}

int usage(const char* exe) {
    std::cerr << "usage: " << exe
              << " [output.tboot] [output.tdisk] [image-version]\n";
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();

    if (argc > 4) return usage(argv[0]);
    const std::string boot_path = argc >= 2 ? argv[1] : "build/ternary-os.tboot";
    const std::string disk_path = argc >= 3 ? argv[2] : defaultDiskPathForBoot(boot_path);
    const std::string image_version = argc >= 4 ? argv[3] : "dev";

    constexpr int kGuiStackWords = 1024;
    constexpr int kServiceStackWords = 256;
    constexpr int kCliStackWords = 128;

    std::vector<BundledApp> apps = {
        {"init", "init", "Init", "/bin/init", 0, kServiceStackWords, false},
        {"desktop", "desktop", "Desktop", "/bin/desktop", 0, kGuiStackWords, true},
        {"shell", "shell", "Shell", "/bin/shell", 0, kServiceStackWords, false},
        {"shell", "sh", "sh", "/bin/sh", 0, kServiceStackWords, false},
        {"terminal", "terminal", "Terminal", "/bin/terminal", 0, kGuiStackWords, true},
        {"file_manager", "files", "Files", "/bin/file_manager", 0, kGuiStackWords, true},
        {"settings", "settings", "Settings", "/bin/settings", 0, kGuiStackWords, true},
        {"task_manager", "tasks", "Tasks", "/bin/task_manager", 0, kGuiStackWords, true},
        {"task_manager", "top", "top", "/bin/top", 0, kServiceStackWords, false},
        {"task_manager", "tasks_cmd", "tasks", "/bin/tasks", 0, kServiceStackWords, false},
        {"calculator", "calculator", "Calculator", "/bin/calculator", 0, kGuiStackWords, true},
        {"paint", "paint", "Paint", "/bin/paint", 0, kGuiStackWords, true},
        {"text_editor", "text_editor", "Text Editor", "/bin/text_editor", 0, kGuiStackWords, true},
        {"text_editor", "edit", "edit", "/bin/edit", 0, kServiceStackWords, false},
        {"about", "about", "About", "/bin/about", 0, kGuiStackWords, true},
        {"help", "help", "Help", "/bin/help", 0, kGuiStackWords, true},
        {"ls", "ls", "ls", "/bin/ls", 0, kCliStackWords, false},
        {"bin_core", "cat", "cat", "/bin/cat", 0, kCliStackWords, false},
        {"bin_core", "echo", "echo", "/bin/echo", 0, kCliStackWords, false},
        {"bin_core", "pwd", "pwd", "/bin/pwd", 0, kCliStackWords, false},
        {"bin_core", "cp", "cp", "/bin/cp", 0, kCliStackWords, false},
        {"bin_core", "mv", "mv", "/bin/mv", 0, kCliStackWords, false},
        {"bin_core", "rm", "rm", "/bin/rm", 0, kCliStackWords, false},
        {"bin_core", "mkdir", "mkdir", "/bin/mkdir", 0, kCliStackWords, false},
        {"bin_core", "rmdir", "rmdir", "/bin/rmdir", 0, kCliStackWords, false},
        {"bin_core", "touch", "touch", "/bin/touch", 0, kCliStackWords, false},
        {"bin_core", "stat", "stat", "/bin/stat", 0, kCliStackWords, false},
        {"bin_core", "find", "find", "/bin/find", 0, kCliStackWords, false},
        {"bin_core", "grep", "grep", "/bin/grep", 0, kCliStackWords, false},
        {"clear", "clear", "clear", "/bin/clear", 0, kCliStackWords, false},
        {"date", "date", "date", "/bin/date", 0, kCliStackWords, false},
        {"sleep", "sleep", "sleep", "/bin/sleep", 0, kCliStackWords, false},
        {"ps", "ps", "ps", "/bin/ps", 0, kCliStackWords, false},
        {"kill", "kill", "kill", "/bin/kill", 0, kCliStackWords, false},
        {"mount", "mount", "mount", "/bin/mount", 0, kCliStackWords, false},
        {"fsck", "fsck", "fsck", "/bin/fsck", 0, kCliStackWords, false},
        {"sync", "sync", "sync", "/bin/sync", 0, kCliStackWords, false},
        {"reboot", "reboot", "reboot", "/bin/reboot", 0, kCliStackWords, false},
        {"shutdown", "shutdown", "shutdown", "/bin/shutdown", 0, kCliStackWords, false},
        {"crash", "crash", "crash", "/bin/crash", 0, kCliStackWords, false},
        {"login", "login", "login", "/bin/login", 0, kCliStackWords, false},
        {"passwd", "passwd", "passwd", "/bin/passwd", 0, kCliStackWords, false},
        {"service_stub", "sessiond", "sessiond", "/bin/sessiond", 0, kCliStackWords, false},
        {"service_stub", "window_server", "window_server", "/bin/window_server", 0, kCliStackWords, false},
        {"service_stub", "compositor", "compositor", "/bin/compositor", 0, kCliStackWords, false},
        {"service_stub", "inputd", "inputd", "/bin/inputd", 0, kCliStackWords, false},
        {"service_stub", "mountd", "mountd", "/bin/mountd", 0, kCliStackWords, false},
        {"service_stub", "logd", "logd", "/bin/logd", 0, kCliStackWords, false},
        {"service_stub", "crashd", "crashd", "/bin/crashd", 0, kCliStackWords, false},
        {"service_stub", "updated", "updated", "/bin/updated", 0, kCliStackWords, false},
        {"service_stub", "packaged", "packaged", "/bin/packaged", 0, kCliStackWords, false},
        {"service_stub", "devd", "devd", "/bin/devd", 0, kCliStackWords, false},
        {"service_stub", "timed", "timed", "/bin/timed", 0, kCliStackWords, false},
        {"service_stub", "authd", "authd", "/bin/authd", 0, kCliStackWords, false},
        {"service_stub", "powerd", "powerd", "/bin/powerd", 0, kCliStackWords, false},
    };

    const std::string architecture =
        readTextFile("generated/architecture_contract.trit");
    const std::string kernel = readTextFile("kernel.trit");
    const std::string trap = readTextFile("native_kernel_trap_stub.tasm");
    if (architecture.empty() || kernel.empty() || trap.empty()) {
        std::cerr
            << "failed to read generated architecture contract, kernel, "
               "or native trap stub\n";
        return EXIT_FAILURE;
    }

    using namespace sandbox::compiler;
    CompileResult compiled_kernel =
        compileSource("kernel.trit", architecture + "\n" + kernel);
    if (!compiled_kernel.success) {
        std::cerr << "kernel compile failed:\n";
        dumpDiagnostics(compiled_kernel);
        return EXIT_FAILURE;
    }

    bool ok = true;
    std::vector<LinkResult> linked_apps;
    linked_apps.reserve(apps.size());
    for (const BundledApp& app : apps) {
        linked_apps.push_back(compileApp(app, ok));
    }
    if (!ok) return EXIT_FAILURE;

    // Base-page PPNs: keep the boot kernel below this range while leaving the
    // first interactive bundles inside the compact profile's IMEM.
    int next_text_ppn = 250;
    constexpr int kAppTextPpnAlignment = 16;
    constexpr int kAppTextPpnGuardPages = 8;
    for (std::size_t i = 0; i < apps.size(); ++i) {
        next_text_ppn = alignUp(next_text_ppn, kAppTextPpnAlignment);
        apps[i].text_ppn = next_text_ppn;
        next_text_ppn += linked_apps[i].executable_header.text_pages + kAppTextPpnGuardPages;
    }

    sandbox::os::NativeVfsImageBuilder rootfs(32768);
    if (!rootfs.status().ok() || !rootfs.installBaseLayout().ok()) {
        std::cerr << "failed to initialize native VFS root image\n";
        return EXIT_FAILURE;
    }

    for (std::size_t i = 0; i < apps.size(); ++i) {
        const BundledApp& app = apps[i];
        const LinkResult& linked = linked_apps[i];
        if (!rootfs.addExecutableImage(app.guest_path,
                                       linked.assembled.program,
                                       linked.executable_header,
                                       linked.executable_header_v2,
                                       app.text_ppn).ok()) {
            std::cerr << "failed to install " << app.id << " into root image\n";
            return EXIT_FAILURE;
        }
    }
    (void)rootfs.addUserRecord("root", 333667, "/home/root", "/bin/desktop");
    if (!installEssentialRootFiles(rootfs, apps, image_version)) {
        std::cerr << "failed to install essential root filesystem files\n";
        return EXIT_FAILURE;
    }
    (void)rootfs.addFile("/etc/release", asciiWords("Ternary OS " + image_version + "\n"));

    const std::string boot_source =
        ".isa 2\n"
        ".require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50\n" +
        buildBootExecAssembly("/bin/desktop") + "\n" +
        trap + "\n" +
        compiled_kernel.assembly + "\n";
    auto assembled = sandbox::vm::assembler::assembleV2(boot_source);
    if (!assembled.success) {
        std::cerr << "boot image assembly failed:\n";
        for (const auto& error : assembled.errors) {
            std::cerr << "  " << error.format() << "\n";
        }
        return EXIT_FAILURE;
    }

    sandbox::host::TosImageManifest manifest;
    manifest.image_version = image_version;
    manifest.profile_name = "minimum";
    manifest.boot_entry = 0;
    auto boot_label = assembled.labels.find("boot");
    if (boot_label != assembled.labels.end()) manifest.boot_entry = boot_label->second;
    manifest.sections.push_back({
        "kernel",
        "/kernel",
        "kernel",
        0,
        manifest.boot_entry,
        static_cast<int>(assembled.program.size()),
        pagesForWords(static_cast<int>(assembled.program.size())),
        sandbox::host::TOS_IMAGE_SECTION_EXECUTABLE |
            sandbox::host::TOS_IMAGE_SECTION_KERNEL,
    });
    for (std::size_t i = 0; i < apps.size(); ++i) {
        const BundledApp& app = apps[i];
        const auto& header = linked_apps[i].executable_header;
        manifest.sections.push_back({
            app.id,
            app.guest_path,
            "app",
            app.text_ppn * sandbox::vm::MMU_PAGE_WORDS,
            header.entry_virtual_pc,
            static_cast<int>(linked_apps[i].assembled.program.size()),
            header.text_pages,
            sandbox::host::TOS_IMAGE_SECTION_EXECUTABLE |
                sandbox::host::TOS_IMAGE_SECTION_APP,
        });
        manifest.apps.push_back({
            app.id,
            app.guest_path,
            app.text_ppn,
            header.entry_virtual_pc,
            header.text_pages,
            header.data_pages,
            header.stack_words,
            sandbox::architecture::v2::ISA_VERSION,
            linked_apps[i].executable_header_v2.required_features,
            sandbox::architecture::v2::FUNCTION_ABI_VERSION,
            sandbox::architecture::v2::SYSCALL_ABI_VERSION,
        });
    }

    std::vector<long long> rootfs_image = rootfs.image();
    sandbox::host::TosBootImage image =
        sandbox::host::bootImageFromAssembly(assembled, manifest);
    std::string error;
    if (!sandbox::host::writeBootImageFile(boot_path, image, &error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }
    if (!disk_path.empty() &&
        !sandbox::host::writeSparseDiskFile(disk_path, rootfs_image, true, &error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "wrote " << boot_path << "\n";
    if (!disk_path.empty()) std::cout << "wrote " << disk_path << "\n";
    return EXIT_SUCCESS;
}
