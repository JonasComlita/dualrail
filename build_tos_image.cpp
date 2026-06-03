#include "ternary_compiler.h"
#include "ternary_host_runtime.h"
#include "ternary_os.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BundledApp {
    std::string name;
    std::string guest_path;
    int text_ppn = 0;
    int stack_words = 256;
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

void appendStringWords(std::vector<long long>& out, const std::string& text) {
    out.push_back(static_cast<long long>(text.size()));
    for (unsigned char c : text) {
        out.push_back(static_cast<long long>(c));
    }
}

std::string buildBootExecAssembly(const std::string& path) {
    std::ostringstream boot;
    boot << ".text\n";
    boot << "boot:\n";
    boot << "    mov sp, 16383\n";
    boot << "    call kernel_init\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 0\n";
    boot << "    mov r15, 0\n";
    boot << "    call ipc_open\n";
    boot << "    mov r1, native_trap_entry\n";
    boot << "    csrw tvec, r1\n";

    appendStoreCString(boot, 10020, path);
    boot << "    mov r1, 12345\n";
    boot << "    mov r2, 62000\n";
    boot << "    store r1, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 0\n";
    boot << "    call app_launch\n";
    boot << "    mov r1, 12345\n";
    boot << "    mov r2, 62000\n";
    boot << "    store r1, r2, 0\n";

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

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string widget = readTextFile("apps/libwidget.trit");
    const std::string source = readTextFile("apps/" + app.name + ".trit");
    if (sdk.empty() || widget.empty() || source.empty()) {
        std::cerr << "missing source for app " << app.name << "\n";
        ok = false;
        return {};
    }

    CompileResult compiled =
        compileSource(app.name + ".trit", sdk + "\n" + widget + "\n" + source);
    if (!compiled.success) {
        std::cerr << "compile failed for " << app.name << ":\n";
        dumpDiagnostics(compiled);
        ok = false;
        return {};
    }

    LinkOptions options;
    options.stack_hint_words = app.stack_words;
    options.standalone_halt_on_exit = false;
    options.dead_strip_functions = true;
    LinkResult linked = linkModules({compiled.object}, options);
    if (!linked.success) {
        std::cerr << "link failed for " << app.name << "\n";
        ok = false;
    }
    return linked;
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
    const std::string disk_path = argc >= 3 ? argv[2] : "";
    const std::string image_version = argc >= 4 ? argv[3] : "dev";

    const std::vector<BundledApp> apps = {
        {"desktop", "/bin/desktop", 8100, 512},
        {"calculator", "/bin/calculator", 8200, 256},
        {"task_manager", "/bin/task_manager", 8300, 256},
        {"paint", "/bin/paint", 8400, 256},
        {"file_manager", "/bin/file_manager", 8500, 256},
        {"settings", "/bin/settings", 8600, 256},
        {"terminal", "/bin/terminal", 8700, 256},
    };

    const std::string kernel = readTextFile("kernel.trit");
    const std::string trap = readTextFile("OS3/native_kernel_trap_stub.tasm");
    if (kernel.empty() || trap.empty()) {
        std::cerr << "failed to read kernel.trit or native trap stub\n";
        return EXIT_FAILURE;
    }

    using namespace sandbox::compiler;
    CompileResult compiled_kernel = compileSource("kernel.trit", kernel);
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

    sandbox::os::NativeVfsImageBuilder rootfs(32768);
    if (!rootfs.status().ok() || !rootfs.installBaseLayout().ok()) {
        std::cerr << "failed to initialize native VFS root image\n";
        return EXIT_FAILURE;
    }

    std::vector<long long> registry;
    for (std::size_t i = 0; i < apps.size(); ++i) {
        const BundledApp& app = apps[i];
        const LinkResult& linked = linked_apps[i];
        if (!rootfs.addExecutableImage(app.guest_path,
                                       linked.assembled.program,
                                       linked.executable_header,
                                       app.text_ppn).ok()) {
            std::cerr << "failed to install " << app.name << " into root image\n";
            return EXIT_FAILURE;
        }
        appendStringWords(registry, app.name);
        appendStringWords(registry, app.guest_path);
    }
    (void)rootfs.addFile("/apps/registry", registry);
    (void)rootfs.addFile("/etc/release", {84, 101, 114, 110, 97, 114, 121});
    (void)rootfs.addUserRecord("root", 333667, "/home/root", "/bin/desktop");

    const std::string boot_source =
        buildBootExecAssembly("/bin/desktop") + "\n" +
        trap + "\n" +
        compiled_kernel.assembly + "\n";
    auto assembled = sandbox::vm::assembler::assemble(boot_source);
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
    for (std::size_t i = 0; i < apps.size(); ++i) {
        const BundledApp& app = apps[i];
        const auto& header = linked_apps[i].executable_header;
        manifest.apps.push_back({
            app.name,
            app.guest_path,
            app.text_ppn,
            header.entry_virtual_pc,
            header.text_pages,
            header.data_pages,
            header.stack_words,
        });
    }

    sandbox::host::TosBootImage image =
        sandbox::host::bootImageFromAssembly(assembled, manifest, rootfs.image());
    std::string error;
    if (!sandbox::host::writeBootImageFile(boot_path, image, &error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }
    if (!disk_path.empty() &&
        !sandbox::host::writeSparseDiskFile(disk_path, image.rootfs_words, true, &error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "wrote " << boot_path << "\n";
    if (!disk_path.empty()) std::cout << "wrote " << disk_path << "\n";
    return EXIT_SUCCESS;
}
