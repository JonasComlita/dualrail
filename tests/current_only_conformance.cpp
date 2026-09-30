#include "ternary_host_runtime.h"
#include "ternary_os.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "current platform conformance: " << message << "\n";
        return false;
    }
    return true;
}

bool containsInOrder(const std::string& text,
                    const std::vector<std::string>& needles) {
    std::size_t offset = 0;
    for (const std::string& needle : needles) {
        const std::size_t found = text.find(needle, offset);
        if (found == std::string::npos) return false;
        offset = found + needle.size();
    }
    return true;
}

bool runDesktopWorkflow() {
    using sandbox::host::TosFramebufferMode;
    using sandbox::host::TosRuntime;
    using sandbox::host::TosRuntimeConfig;

    const std::filesystem::path root =
        std::filesystem::current_path() / "build_current_cleanup" / "release" /
        "TernaryOS";
    const std::filesystem::path boot_path = root / "ternary-os.tboot";
    const std::filesystem::path disk_path = root / "ternary-os.tdisk";
    const std::filesystem::path artifact =
        std::filesystem::current_path() / "build_current_cleanup" /
        "desktop_workflow_artifact";
    if (!require(std::filesystem::exists(boot_path) &&
                     std::filesystem::exists(disk_path),
                 "staged release image is missing for desktop workflow")) {
        return false;
    }

    std::error_code artifact_reset_error;
    std::filesystem::remove_all(artifact, artifact_reset_error);
    if (!require(!artifact_reset_error,
                 "cannot reset desktop workflow artifact: " +
                     artifact_reset_error.message())) {
        return false;
    }

    TosRuntimeConfig config;
    config.boot_image_path = boot_path.string();
    config.disk_path = disk_path.string();
    config.profile_name = "minimum";
    config.execution_backend = sandbox::vm::VMExecutionBackend::NativeX64Jit;
    config.record_syscall_trace = true;
    TosRuntime runtime(config);
    std::string error;
    if (!require(runtime.loadImage(&error), "runtime image load failed: " + error)) {
        return false;
    }

    auto advance = [&](int steps, const std::string& phase) {
        const sandbox::vm::RunResult result = runtime.runForSteps(steps);
        return require(result.status != sandbox::vm::VMStatus::TRAPPED,
                       phase + " trapped: " + result.description);
    };

    // Boot through the login screen, then use the guest keyboard path to
    // unlock the desktop. The passcode is intentionally sent as text so this
    // covers the SDL_TEXTINPUT-compatible path rather than only raw keys.
    if (!advance(10'000'000, "desktop boot")) return false;
    runtime.pushTextInput("777", "e2e.login.text");
    runtime.pushKeyboardInput(13, "e2e.login.enter");
    if (!advance(1'000'000, "desktop login")) return false;

    // The launcher is rendered as rows beginning at y=4. Click the displayed
    // Calculator row, exercising a complete down/up transition rather than a
    // final button-state sample.
    runtime.updateMouseState(11, 4, 1, "e2e.launch.calculator.down");
    runtime.updateMouseState(11, 4, 0, "e2e.launch.calculator.up");
    auto frame = runtime.readFramebufferMemory();
    bool has_visible_rgb = false;
    for (int attempt = 0; attempt < 12 && !has_visible_rgb; ++attempt) {
        if (!advance(1'000'000, "calculator launch")) return false;
        frame = runtime.readFramebufferMemory();
        if (frame.mode != TosFramebufferMode::GraphicsRGB) continue;
        for (const std::uint32_t pixel : frame.rgba) {
            if (pixel != 0x000000ffU) {
                has_visible_rgb = true;
                break;
            }
        }
    }
    if (!require(frame.mode == TosFramebufferMode::GraphicsRGB,
                 "calculator did not publish an RGB compositor frame")) {
        return false;
    }
    if (!require(frame.width >= 40 && frame.height >= 32 &&
                     frame.rgba.size() ==
                         static_cast<std::size_t>(frame.width * frame.height),
                 "calculator RGB frame has invalid dimensions")) {
        return false;
    }
    if (!has_visible_rgb) {
        std::string rgb_diagnostic_error;
        (void)runtime.exportDiagnostics(
            (std::filesystem::current_path() / "build_current_cleanup" /
             "desktop_workflow_rgb_failure").string(),
            &rgb_diagnostic_error);
    }
    if (!require(has_visible_rgb,
                 "calculator RGB compositor frame is only the background")) {
        return false;
    }
    const std::uint64_t initial_frame_revision = frame.revision;

    // Click the calculator's local '1' button after translating it through
    // the window origin, then use Tab and text input to exercise keyboard
    // focus forwarding in the same child workflow.
    runtime.updateMouseState(23, 22, 1, "e2e.calculator.one.down");
    runtime.updateMouseState(23, 22, 0, "e2e.calculator.one.up");
    runtime.pushKeyboardInput(9, "e2e.calculator.tab");
    runtime.pushTextInput("2", "e2e.calculator.text");
    runtime.pushKeyboardInput(13, "e2e.calculator.enter");
    if (!advance(10'000'000, "calculator interaction")) return false;
    frame = runtime.readFramebufferMemory(initial_frame_revision);
    if (!require(frame.mode == TosFramebufferMode::GraphicsRGB && frame.changed,
                 "calculator interaction did not change the RGB frame")) {
        return false;
    }

    std::string artifact_error;
    if (!require(runtime.exportDiagnostics(
                     (artifact / "calculator_rgb").string(), &artifact_error),
                 "calculator RGB diagnostics failed: " + artifact_error)) {
        return false;
    }
    if (!require(std::filesystem::exists(
                     artifact / "calculator_rgb" / "framebuffer_snapshot.png"),
                 "calculator RGB artifact is incomplete")) {
        return false;
    }

    // Let the child consume its close key and verify the desktop resumes with
    // its text presentation after the child exits.
    runtime.pushKeyboardInput(120, "e2e.calculator.close");
    if (!advance(10'000'000, "calculator close")) return false;
    if (runtime.snapshot().gpu_mode != 0) {
        std::string close_diagnostic_error;
        (void)runtime.exportDiagnostics(
            (artifact / "calculator_close_failure").string(),
            &close_diagnostic_error);
    }
    if (!require(runtime.snapshot().gpu_mode == 0,
                 "desktop did not resume after calculator close")) {
        return false;
    }

    if (!require(runtime.exportDiagnostics(artifact.string(), &error),
                 "desktop workflow diagnostics failed: " + error)) {
        return false;
    }
    const std::filesystem::path journal_path = artifact / "input_journal.jsonl";
    std::ifstream journal_file(journal_path);
    if (!require(journal_file.is_open(), "desktop workflow journal is unreadable")) {
        return false;
    }
    std::stringstream journal_stream;
    journal_stream << journal_file.rdbuf();
    const std::string journal = journal_stream.str();
    if (!require(containsInOrder(
                     journal,
                     {"e2e.login.text", "e2e.login.enter",
                      "e2e.launch.calculator.down", "e2e.launch.calculator.up",
                      "e2e.calculator.one.down", "e2e.calculator.one.up",
                      "e2e.calculator.tab", "e2e.calculator.text",
                      "e2e.calculator.close"}),
                 "desktop workflow input journal lost event ordering")) {
        return false;
    }
    if (!require(std::filesystem::exists(artifact / "framebuffer_snapshot.png") &&
                     std::filesystem::exists(
                         artifact / "calculator_rgb" / "framebuffer_snapshot.png") &&
                     std::filesystem::exists(artifact / "checkpoint" /
                                             "input_journal.jsonl"),
                 "desktop workflow artifact is incomplete")) {
        return false;
    }

    std::cout << "desktop workflow conformance: launcher -> calculator -> RGB frame -> close\n";
    std::cout << "desktop workflow artifact: " << artifact.string() << "\n";
    return true;
}

}  // namespace

int main() {
    using namespace sandbox;
    using namespace sandbox::vm;

    static_assert(architecture::v2::ISA_VERSION == 2);
    static_assert(architecture::v3::EXECUTABLE_VERSION == 3);
    static_assert(architecture::v3::FUNCTION_ABI_VERSION == 3);
    static_assert(architecture::v3::SYSCALL_ABI_VERSION == 2);
    static_assert(architecture::v3::VECTOR_ABI_VERSION == 1);
    static_assert(architecture::v2::TBOOT_WRITE_VERSION == 3);
    static_assert(architecture::v2::TDISK_WRITE_VERSION == 2);

    ExecutableImageHeaderV3 header = makeExecutableHeaderV3(7, 19, 3, 27);
    assert(validateExecutableHeaderV3(header));
    const std::vector<TernaryValue> encoded = encodeExecutableHeaderV3(header);
    assert(encoded.size() == static_cast<std::size_t>(EXEC_V3_HEADER_WORDS));

    ExecutableImageHeaderV3 decoded;
    assert(decodeExecutableHeaderV3(encoded, 0, decoded));
    assert(decoded.executable_version == architecture::v3::EXECUTABLE_VERSION);
    assert(decoded.function_abi_version == architecture::v3::FUNCTION_ABI_VERSION);
    assert(decoded.isa_version == architecture::v2::ISA_VERSION);
    assert(decoded.vector_lane_count == architecture::v3::VECTOR_LANE_COUNT);

    auto retired_header = encoded;
    retired_header[EXEC_V3_EXECUTABLE_VERSION] = sandbox::vm::ops::fromLong(2);
    ExecutableHeaderVariant retired_variant;
    assert(!decodeExecutableHeaderVersioned(retired_header, 0, retired_variant));

    const std::filesystem::path disk_path =
        std::filesystem::temp_directory_path() /
        "trit-current-platform-conformance.tdisk";
    std::error_code cleanup_error;
    std::filesystem::remove(disk_path, cleanup_error);
    {
        os::BlockDevice disk(8, disk_path.string());
        std::vector<long long> block(STORAGE_BLOCK_WORDS, 0);
        block[0] = 42;
        assert(disk.writeBlock(2, block).ok());
    }
    {
        std::ifstream image(disk_path, std::ios::binary);
        std::uint64_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t block_words = 0;
        image.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        image.read(reinterpret_cast<char*>(&version), sizeof(version));
        image.read(reinterpret_cast<char*>(&block_words), sizeof(block_words));
        assert(image.good());
        assert(magic == 0x54524954535032ULL);
        assert(version == 2);
        assert(block_words == STORAGE_BLOCK_WORDS);

        os::BlockDevice restored(8, disk_path.string());
        std::vector<long long> restored_block;
        assert(restored.readBlock(2, restored_block).ok());
        assert(restored_block.size() == static_cast<std::size_t>(STORAGE_BLOCK_WORDS));
        assert(restored_block[0] == 42);
    }
    std::filesystem::remove(disk_path, cleanup_error);

    {
        std::ofstream retired(disk_path, std::ios::binary | std::ios::trunc);
        const std::uint64_t retired_magic = 0x54524954535031ULL;
        retired.write(reinterpret_cast<const char*>(&retired_magic),
                      sizeof(retired_magic));
        const std::int32_t retired_count = 0;
        retired.write(reinterpret_cast<const char*>(&retired_count),
                      sizeof(retired_count));
    }
    os::BlockDevice retired_disk(8);
    assert(!retired_disk.attachBackingFile(disk_path.string()).ok());
    std::filesystem::remove(disk_path, cleanup_error);

    if (!runDesktopWorkflow()) return 1;

    std::cout << "current platform conformance: ISA v2, executable ABI v3\n";
    return 0;
}
