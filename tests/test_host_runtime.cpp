#include "ternary_host_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

sandbox::host::TosBootImage assembleImage(
    const std::string& source,
    sandbox::host::TosImageManifest manifest = {}) {

    auto assembled = sandbox::vm::assembler::assemble(source);
    expect(assembled.success, "test boot assembly assembles");
    auto boot = assembled.labels.find("boot");
    if (boot != assembled.labels.end()) manifest.boot_entry = boot->second;
    if (manifest.image_version.empty() || manifest.image_version == "dev") {
        manifest.image_version = "test";
    }
    if (manifest.profile_name.empty() || manifest.profile_name == "minimum") {
        manifest.profile_name = "compact";
    }
    return sandbox::host::bootImageFromAssembly(assembled, std::move(manifest));
}

std::string buildPath(const std::string& name) {
    std::filesystem::create_directories("build");
    return "build/" + name;
}

bool fileExists(const std::string& path) {
    return std::filesystem::exists(path);
}

void testBootImageValidation() {
    std::cout << "[1] Host boot image round-trip and validation\n";

    const std::string path = buildPath("host_runtime_roundtrip.tboot");
    const sandbox::host::TosBootImage image = assembleImage(R"(
        .text
        boot:
            mov r1, 2900
            mov r2, 60000
            store r1, r2, 0
            halt
    )");

    std::string error;
    expect(sandbox::host::writeBootImageFile(path, image, &error),
           "boot image writer accepts valid image");
    sandbox::host::TosBootImage readback;
    expect(sandbox::host::readBootImageFile(path, readback, &error),
           "boot image reader accepts valid image");
    expect(readback.program.size() == image.program.size(),
           "boot image round-trip preserves text segment");
    expect(readback.manifest.format_version == sandbox::host::TOS_BOOT_FORMAT_VERSION,
           "boot image writer emits current format version");
    expect(readback.manifest.profile_name == "compact",
           "boot image round-trip preserves profile");
    expect(!readback.manifest.sections.empty() &&
               readback.manifest.sections[0].kind == "kernel",
           "boot image round-trip preserves section metadata");
    expect(readback.rootfs_words.empty(),
           "current boot image format does not embed mutable rootfs seed");

    {
        std::fstream tamper(path, std::ios::binary | std::ios::in | std::ios::out);
        tamper.seekg(-1, std::ios::end);
        char value = 0;
        tamper.read(&value, 1);
        tamper.seekp(-1, std::ios::end);
        value = static_cast<char>(value ^ 0x1);
        tamper.write(&value, 1);
    }
    expect(!sandbox::host::readBootImageFile(path, readback, &error) &&
               error.find("checksum") != std::string::npos,
           "boot image reader rejects checksum mismatch");
}

void testRuntimeTextFramebufferAndInput() {
    std::cout << "[2] Runtime text framebuffer and host input routing\n";

    const sandbox::host::TosBootImage image = assembleImage(R"(
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
    expect(runtime.loadImage(image, &error), "runtime loads in-memory boot image");
    runtime.pushTextInput("A");
    runtime.updateMouseState('B', 'C', 'D');
    const auto result = runtime.runForSteps(32);
    expect(result.halted(), "runtime executes boot image to halt");

    const sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
    expect(framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25,
           "runtime decodes text framebuffer mode");
    expect(framebuffer.width == 80 && framebuffer.height == 25,
           "runtime reports 80x25 text geometry");
    expect(framebuffer.glyphs.size() >= 4 &&
               framebuffer.glyphs[0] == 'A' &&
               framebuffer.glyphs[1] == 'B' &&
               framebuffer.glyphs[2] == 'C' &&
               framebuffer.glyphs[3] == 'D',
           "runtime routes keyboard and mouse CSR values into guest-visible state");
}

void testRuntimeGraphicsResetAndDiagnostics() {
    std::cout << "[3] Runtime graphics decode, reset, disk seed, and diagnostics\n";

    sandbox::host::TosBootImage image = assembleImage(R"(
        .text
        boot:
            mov r1, 1
            csrw gpu_mode, r1
            mov r1, 14
            mov r2, 50000
            store r1, r2, 0
            mov r1, 80
            csrw console_out, r1
            mov r1, 10
            csrw console_out, r1
            halt
    )");
    image.rootfs_words.assign(sandbox::vm::MMU_PAGE_WORDS * 2, 0);
    image.rootfs_words[0] = 123;
    image.rootfs_words[sandbox::vm::MMU_PAGE_WORDS] = 456;

    const std::string disk_path = buildPath("host_runtime_seed.tdisk");
    const std::string diag_path = buildPath("host_runtime_diagnostics");
    std::filesystem::remove(disk_path);
    std::filesystem::remove_all(diag_path);

    sandbox::host::TosRuntimeConfig config;
    config.disk_path = disk_path;
    config.profile_name = "compact";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    expect(runtime.loadImage(image, &error), "runtime loads image with sparse disk seed");
    expect(fileExists(disk_path), "runtime initializes missing .tdisk from image rootfs seed");
    const auto result = runtime.runForSteps(32);
    expect(result.halted(), "graphics boot image halts");

    sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
    expect(framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60,
           "runtime decodes graphics framebuffer mode");
    expect(framebuffer.width == 80 && framebuffer.height == 60,
           "runtime reports 80x60 graphics geometry");
    expect(!framebuffer.rgba.empty() && framebuffer.rgba[0] == 0xffff00ff,
           "runtime maps graphics color index through host palette");

    expect(runtime.exportDiagnostics(diag_path, &error), "runtime exports diagnostics bundle");
    expect(fileExists(diag_path + "/vm_state.txt"), "diagnostics include VM state");
    expect(fileExists(diag_path + "/guest.log"), "diagnostics include guest log");
    expect(fileExists(diag_path + "/kernel_log.txt"), "diagnostics include kernel log alias");
    expect(fileExists(diag_path + "/manifest.txt"), "diagnostics include image manifest");
    expect(fileExists(diag_path + "/manifest.json"), "diagnostics include machine-readable manifest");
    expect(fileExists(diag_path + "/process_table.json"), "diagnostics include process snapshot");
    expect(fileExists(diag_path + "/syscall_trace.jsonl"), "diagnostics include syscall trace placeholder");
    expect(fileExists(diag_path + "/crash_report.txt"), "diagnostics include crash report");
    expect(fileExists(diag_path + "/framebuffer_snapshot.txt"),
           "diagnostics include framebuffer snapshot");

    expect(runtime.reset(&error), "runtime resets from loaded boot image");
    framebuffer = runtime.readFramebuffer();
    expect(framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25,
           "reset returns VM to cold text mode before guest runs");
}

void testRuntimeSeparateDiskRequiredAndPreserved() {
    std::cout << "[4] Runtime boots from separate mutable sparse disk\n";

    sandbox::host::TosBootImage image = assembleImage(R"(
        .text
        boot:
            mov r1, 0
            csrw block_index, r1
            mov r1, 300
            csrw block_addr, r1
            mov r1, 1
            csrw block_cmd, r1
            mov r2, 300
            load r3, r2, 0
            mov r4, 60000
            store r3, r4, 0
            mov r3, 89
            store r3, r2, 0
            mov r1, 0
            csrw block_index, r1
            mov r1, 300
            csrw block_addr, r1
            mov r1, 2
            csrw block_cmd, r1
            halt
    )");
    image.rootfs_words.clear();

    const std::string disk_path = buildPath("host_runtime_separate_disk.tdisk");
    std::filesystem::remove(disk_path);

    sandbox::host::TosRuntimeConfig config;
    config.disk_path = disk_path;
    config.profile_name = "compact";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    expect(!runtime.loadImage(image, &error) &&
               error.find("disk image is required") != std::string::npos,
           "runtime rejects missing separate disk for rootfs-less boot image");

    std::vector<long long> seed(sandbox::vm::MMU_PAGE_WORDS * 2, 0);
    seed[0] = 90;
    expect(sandbox::host::writeSparseDiskFile(disk_path, seed, true, &error),
           "test writes initialized sparse disk artifact");
    expect(runtime.loadImage(image, &error),
           "runtime loads rootfs-less boot image with companion .tdisk");

    auto result = runtime.runForSteps(128);
    expect(result.halted(), "first separate-disk boot halts");
    sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
    expect(!framebuffer.glyphs.empty() && framebuffer.glyphs[0] == 'Z',
           "first boot reads initialized .tdisk state");

    expect(runtime.reset(&error), "runtime reboots against the same mutable .tdisk");
    result = runtime.runForSteps(128);
    expect(result.halted(), "second separate-disk boot halts");
    framebuffer = runtime.readFramebuffer();
    expect(!framebuffer.glyphs.empty() && framebuffer.glyphs[0] == 'Y',
           "reboot preserves modified .tdisk state");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testBootImageValidation();
    testRuntimeTextFramebufferAndInput();
    testRuntimeGraphicsResetAndDiagnostics();
    testRuntimeSeparateDiskRequiredAndPreserved();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " host runtime failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nHost runtime tests passed\n";
    return EXIT_SUCCESS;
}
