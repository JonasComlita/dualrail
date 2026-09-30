#include "ternary_host_runtime.h"
#include "ternary_os.h"

// The conformance executable must evaluate checks in Release builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif
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
        std::filesystem::path(TRIT_TEST_BUILD_DIR) / "release" /
        "TernaryOS";
    const std::filesystem::path boot_path = root / "ternary-os.tboot";
    const std::filesystem::path disk_path = root / "ternary-os.tdisk";
    const std::filesystem::path artifact =
        std::filesystem::path(TRIT_TEST_BUILD_DIR) /
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
        if (!require(result.status != sandbox::vm::VMStatus::TRAPPED,
                     phase + " trapped: " + result.description)) return false;
        if (!runtime.exportDiagnostics((artifact / "health").string(), &error)) return false;
        std::ifstream processes(artifact / "health" / "process_table.json");
        const std::string table((std::istreambuf_iterator<char>(processes)), {});
        return require(table.find("\"state_name\": \"crashed\"") == std::string::npos,
                       phase + ": guest process crashed (see health/process_table.json)");
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
            (std::filesystem::path(TRIT_TEST_BUILD_DIR) /
             "desktop_workflow_rgb_failure").string(),
            &rgb_diagnostic_error);
    }
    if (!require(has_visible_rgb,
                 "calculator RGB compositor frame is only the background")) {
        return false;
    }
    // Verify distinct controls at their drawn coordinates, rather than merely
    // accepting any RGB pixel. ABI argument corruption used to fill the whole
    // surface with the last control's color and still pass the RGB assertion.
    auto pixel = [&](int x, int y) { return frame.rgba[y * frame.width + x]; };
    if (!require(pixel(23, 22) != pixel(21, 22) &&
                 pixel(31, 12) != pixel(30, 12) &&
                 pixel(34, 28) != pixel(34, 26),
                 "calculator controls are not visible at their input targets")) return false;
    const std::uint64_t initial_frame_revision = frame.revision;

    // Click the calculator's local '1' button after translating it through
    // the window origin, then use Tab and text input to exercise keyboard
    // focus forwarding in the same child workflow.
    // Transition pressure cannot be coalesced like redundant pointer motion.
    for (int i = 0; i < 12; ++i) {
        runtime.updateMouseState(21, 20, 1, "e2e.calculator.blank.down");
        runtime.updateMouseState(21, 20, 0, "e2e.calculator.blank.up");
    }
    runtime.updateMouseState(25, 24, 1, "e2e.calculator.one.down");
    // More samples than the guest's eight-event queue, delivered while it
    // redraws. A captured drag across the close control must not close it.
    for (int i = 0; i < 20; ++i)
        runtime.updateMouseState(25 + i % 2, 24, 1, "e2e.calculator.held.move");
    runtime.updateMouseState(58, 2, 1, "e2e.calculator.captured.close.crossing");
    runtime.updateMouseState(75, 50, 0, "e2e.calculator.one.up");
    runtime.pushKeyboardInput(9, "e2e.calculator.tab");
    runtime.pushTextInput("29", "e2e.calculator.text");
    runtime.pushKeyboardInput(8, "e2e.calculator.backspace");
    runtime.pushTextInput("+30", "e2e.calculator.add");
    runtime.pushKeyboardInput(13, "e2e.calculator.enter");
    const auto burst_checkpoint = artifact / "input_burst_checkpoint";
    if (!require(runtime.captureCheckpoint(&error) &&
                 runtime.exportCheckpointBundle(burst_checkpoint.string(), &error) &&
                 runtime.restoreCheckpointBundle(burst_checkpoint.string(), &error),
                 "mixed input checkpoint round trip failed: " + error)) return false;
    if (!advance(30'000'000, "calculator interaction")) return false;
    frame = runtime.readFramebufferMemory(initial_frame_revision);
    if (!require(frame.mode == TosFramebufferMode::GraphicsRGB && frame.changed,
                 "calculator interaction did not change the RGB frame")) {
        return false;
    }

    // Read the expected 4 and 2 from the actual presented 3x5 display. Checking
    // the exit status alone missed corrupted has_error/result draw arguments.
    const std::vector<std::string> expected_digits = {"000101111001000", "111001111100111"};
    for (int digit = 0; digit < 2; ++digit) {
        std::string shape;
        for (int y = 5; y < 10; ++y) {
            for (int x = 31 + digit * 4; x < 34 + digit * 4; ++x) {
                const auto color = frame.rgba[y * frame.width + x];
                shape += ((color >> 24) >= 200 && ((color >> 16) & 255) >= 180 &&
                          ((color >> 8) & 255) <= 100) ? '1' : '0';
            }
        }
        if (!require(shape == expected_digits[digit], "calculator did not display result 42")) return false;
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
    if (!require(runtime.readFramebuffer().mode == TosFramebufferMode::Text80x25,
                 "desktop remains hidden behind the compositor")) return false;
    std::ifstream completed_processes(artifact / "health" / "process_table.json");
    std::string process_line;
    bool correct_result = false;
    while (std::getline(completed_processes, process_line)) {
        if (process_line.find("\"pid\": 101,") != std::string::npos &&
            process_line.find("\"exit_status\": 42,") != std::string::npos) correct_result = true;
    }
    if (!require(correct_result, "calculator did not compute mouse 1 then 2+30 = 42")) return false;

    // Launch Files through a different visible row, select an entry inside
    // its drawn row, enter the directory, and return with the parent control.
    runtime.updateMouseState(11, 7, 1, "e2e.launch.files.down");
    runtime.updateMouseState(11, 7, 0, "e2e.launch.files.up");
    if (!advance(10'000'000, "files launch")) return false;
    const auto root_frame = runtime.readFramebufferMemory();
    if (!require(root_frame.mode == TosFramebufferMode::GraphicsRGB,
                 "Files did not render its window")) return false;
    runtime.updateMouseState(28, 15, 1, "e2e.files.row.down");
    runtime.updateMouseState(28, 15, 0, "e2e.files.row.up");
    runtime.pushKeyboardInput(13, "e2e.files.open");
    if (!advance(15'000'000, "files open directory")) return false;
    const auto directory_frame = runtime.readFramebufferMemory();
    auto header = [](const auto& fb) {
        std::vector<std::uint32_t> pixels;
        if (fb.mode != TosFramebufferMode::GraphicsRGB || fb.width != 80 || fb.height != 60)
            return pixels;
        for (int y = 6; y < 11; ++y)
            for (int x = 11; x < 67; ++x) pixels.push_back(fb.rgba[y * fb.width + x]);
        return pixels;
    };
    if (!require(!header(directory_frame).empty() && header(directory_frame) != header(root_frame),
                 "Files did not show the opened directory path")) return false;
    if (!require(runtime.exportDiagnostics((artifact / "files_directory").string(), &error),
                 "Files directory diagnostics failed: " + error)) return false;
    runtime.updateMouseState(16, 37, 1, "e2e.files.parent.down");
    runtime.updateMouseState(16, 37, 0, "e2e.files.parent.up");
    if (!advance(10'000'000, "files parent directory")) return false;
    if (!require(header(runtime.readFramebufferMemory()) == header(root_frame),
                 "Files parent control did not return to root")) return false;
    runtime.pushKeyboardInput(120, "e2e.files.close");
    if (!advance(10'000'000, "files close")) return false;
    if (!require(runtime.readFramebuffer().mode == TosFramebufferMode::Text80x25,
                 "desktop did not resume after Files close")) return false;

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

    std::cout << "desktop workflow conformance: launcher -> calculator burst/drag/backspace/result -> Files open/parent -> desktop\n";
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
