#include "ternary_compiler.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct AppCase {
    const char* source_name;
    const char* id;
    int stack_words;
};

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

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

std::vector<AppCase> bundledApps() {
    constexpr int kGuiStackWords = 1024;
    constexpr int kServiceStackWords = 256;
    constexpr int kCliStackWords = 128;

    return {
        {"init", "init", kServiceStackWords},
        {"desktop", "desktop", kGuiStackWords},
        {"shell", "shell", kServiceStackWords},
        {"shell", "sh", kServiceStackWords},
        {"terminal", "terminal", kGuiStackWords},
        {"file_manager", "files", kGuiStackWords},
        {"settings", "settings", kGuiStackWords},
        {"task_manager", "tasks", kGuiStackWords},
        {"task_manager", "top", kServiceStackWords},
        {"task_manager", "tasks_cmd", kServiceStackWords},
        {"calculator", "calculator", kGuiStackWords},
        {"paint", "paint", kGuiStackWords},
        {"text_editor", "text_editor", kGuiStackWords},
        {"text_editor", "edit", kServiceStackWords},
        {"about", "about", kGuiStackWords},
        {"help", "help", kGuiStackWords},
        {"ls", "ls", kCliStackWords},
        {"bin_core", "cat", kCliStackWords},
        {"bin_core", "echo", kCliStackWords},
        {"bin_core", "pwd", kCliStackWords},
        {"bin_core", "cp", kCliStackWords},
        {"bin_core", "mv", kCliStackWords},
        {"bin_core", "rm", kCliStackWords},
        {"bin_core", "mkdir", kCliStackWords},
        {"bin_core", "rmdir", kCliStackWords},
        {"bin_core", "touch", kCliStackWords},
        {"bin_core", "stat", kCliStackWords},
        {"bin_core", "find", kCliStackWords},
        {"bin_core", "grep", kCliStackWords},
        {"clear", "clear", kCliStackWords},
        {"date", "date", kCliStackWords},
        {"sleep", "sleep", kCliStackWords},
        {"ps", "ps", kCliStackWords},
        {"kill", "kill", kCliStackWords},
        {"mount", "mount", kCliStackWords},
        {"fsck", "fsck", kCliStackWords},
        {"sync", "sync", kCliStackWords},
        {"reboot", "reboot", kCliStackWords},
        {"shutdown", "shutdown", kCliStackWords},
        {"login", "login", kCliStackWords},
        {"passwd", "passwd", kCliStackWords},
        {"service_stub", "sessiond", kCliStackWords},
        {"service_stub", "window_server", kCliStackWords},
        {"service_stub", "compositor", kCliStackWords},
        {"service_stub", "inputd", kCliStackWords},
        {"service_stub", "mountd", kCliStackWords},
        {"service_stub", "logd", kCliStackWords},
        {"service_stub", "crashd", kCliStackWords},
        {"service_stub", "updated", kCliStackWords},
        {"service_stub", "packaged", kCliStackWords},
        {"service_stub", "devd", kCliStackWords},
        {"service_stub", "timed", kCliStackWords},
        {"service_stub", "authd", kCliStackWords},
        {"service_stub", "powerd", kCliStackWords},
    };
}

void testBundledAppsCompileAndLink() {
    std::cout << "[1] All release-bundled apps compile and link\n";

    using namespace sandbox::compiler;

    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string widget = readTextFile("apps/libwidget.trit");
    expect(!sdk.empty(), "app SDK source is present");
    expect(!widget.empty(), "widget library source is present");

    for (const AppCase& app : bundledApps()) {
        const std::string app_source_path = std::string("apps/") + app.source_name + ".trit";
        const std::string source = readTextFile(app_source_path);
        expect(!source.empty(), std::string(app.id) + " source is present");
        if (sdk.empty() || widget.empty() || source.empty()) continue;

        CompileResult compiled =
            compileSource(std::string(app.id) + ".trit", sdk + "\n" + widget + "\n" + source);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR " << app.id << ":\n";
            dumpDiagnostics(compiled);
        }
        expect(compiled.success, std::string(app.id) + " compiles against the OS SDK");
        if (!compiled.success) continue;

        LinkOptions options;
        options.stack_hint_words = app.stack_words;
        options.standalone_halt_on_exit = false;
        options.dead_strip_functions = true;
        LinkResult linked = linkModules({compiled.object}, options);
        expect(linked.success, std::string(app.id) + " links with release options");
        if (!linked.success) continue;
        expect(!linked.assembled.program.empty(), std::string(app.id) + " emits text words");
        expect(linked.executable_header.stack_words == app.stack_words,
               std::string(app.id) + " preserves its release stack hint");
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();
    testBundledAppsCompileAndLink();
    if (g_failures != 0) {
        std::cout << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "All bundled app tests passed\n";
    return 0;
}