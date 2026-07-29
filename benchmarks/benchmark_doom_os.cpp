#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) return {};
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

long long wordAt(sandbox::vm::VMState& vm, int address) {
    const auto [value, fault] = vm.dmem.load(address);
    return fault == sandbox::vm::MemFaultCode::OK
        ? sandbox::vm::ops::toLong(value)
        : 0;
}

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

bool writeReport(const std::filesystem::path& path,
                 bool passed,
                 const std::string& detail,
                 double compile_ms,
                 double run_ms,
                 long long steps,
                 long long asset_checksum,
                 long long frame_hash,
                 long long frames,
                 long long ticks,
                 long long input_events,
                 long long asset_words,
                 long long memory_high_water) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    out << "{\n"
        << "  \"schema\": \"trit.benchmark_result.v1\",\n"
        << "  \"source\": {\"identity\": \"synthetic-doom-v1\"},\n"
        << "  \"host\": {\"profile\": \"portable\"},\n"
        << "  \"build\": {\"profile\": \"current\"},\n"
        << "  \"workload\": {\"name\": \"doom-class-os\", "
           "\"frames\": 27, \"asset_words\": 81},\n"
        << "  \"correctness\": {\"passed\": "
        << (passed ? "true" : "false")
        << ", \"detail\": \"" << detail << "\", "
           "\"asset_checksum\": " << asset_checksum
        << ", \"frame_hash\": " << frame_hash << "},\n"
        << "  \"timing\": {\"unit\": \"milliseconds\", "
           "\"compile\": " << compile_ms << ", \"run\": " << run_ms
        << ", \"warmups\": 0, \"iterations\": 1, "
           "\"coefficient_of_variation\": 0.0},\n"
        << "  \"instruction_mix\": {\"dynamic_total\": " << steps << "},\n"
        << "  \"memory\": {\"high_water_words\": " << memory_high_water << "},\n"
        << "  \"tlb\": {},\n"
        << "  \"scheduler\": {\"timer_ticks\": " << ticks
        << ", \"input_events\": " << input_events << "},\n"
        << "  \"wal\": {},\n"
        << "  \"disk\": {\"read_words\": " << asset_words << "},\n"
        << "  \"graphics\": {\"frames_presented\": " << frames
        << ", \"late_frames\": 0}\n"
        << "}\n";
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();
    const std::filesystem::path report =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("build/benchmarks/doom-os.json");

    const std::string kernel = readText("kernel.trit");
    if (kernel.empty()) {
        std::cerr << "benchmark_doom_os: kernel.trit is unavailable\n";
        return 1;
    }

    const std::string driver = R"(
        const BENCH_BASE: t40 = 35600;

        fn main() -> t40 {
            if kernel_init() <= 0 { return 0; }
            kstore(BENCH_BASE + 7, 1);
            var path: t40 = USER_MEM_BASE + 600;
            kstore(path + 0, 47);
            kstore(path + 1, 100);
            kstore(path + 2, 111);
            kstore(path + 3, 111);
            kstore(path + 4, 109);
            kstore(path + 5, 46);
            kstore(path + 6, 119);
            kstore(path + 7, 97);
            kstore(path + 8, 100);
            kstore(path + 9, 0);

            var asset: t40 = USER_MEM_BASE + 700;
            var loaded: t40 = USER_MEM_BASE + 900;
            var i: t40 = 0;
            var trit: t40 = -1;
            var source_hash: t40 = 0;
            while 81 - i > 0 {
                kstore(asset + i, trit);
                source_hash = source_hash + (i + 1) * trit;
                trit = trit + 1;
                if trit - 1 > 0 { trit = -1; }
                i = i + 1;
            }
            var fd: t40 = vfs_open(1, path, 3);
            if fd < 0 { return 0; }
            kstore(BENCH_BASE + 7, 2);
            if vfs_write(1, fd, asset, 81) - 81 != 0 { return 0; }
            kstore(BENCH_BASE + 7, 3);
            vfs_close(1, fd);
            fd = vfs_open(1, path, 0);
            if fd < 0 { return 0; }
            if vfs_read(1, fd, loaded, 81) - 81 != 0 { return 0; }
            kstore(BENCH_BASE + 7, 4);
            vfs_close(1, fd);

            i = 0;
            var loaded_hash: t40 = 0;
            while 81 - i > 0 {
                loaded_hash = loaded_hash + (i + 1) * kload(loaded + i);
                i = i + 1;
            }
            if loaded_hash - source_hash != 0 { return 0; }
            if loaded_hash - 54 != 0 { return 0; }
            kstore(BENCH_BASE + 7, 5);
            if framebuffer_init(27, 18) <= 0 { return 0; }
            kstore(BENCH_BASE + 7, 6);
            var window: t40 = window_create(1, 0, 0, 9, 9);
            if window < 0 { return 0; }
            kstore(BENCH_BASE + 7, 7);
            var mapped_pixels: t40 = window_get_buffer(1, window);
            if mapped_pixels <= 0 { return 0; }
            var pixels: t40 = kload(
                window_addr(window_find(window)) + WIN_BUFFER_ADDR);
            if pixels <= 0 { return 0; }
            kstore(BENCH_BASE + 7, 8);

            var frame: t40 = 0;
            var input_events: t40 = 0;
            var asset_index: t40 = 0;
            while 27 - frame > 0 {
                i = 0;
                while 27 - i > 0 {
                    var value: t40 = kload(loaded + asset_index) + 1;
                    kstore(pixels + i * 3 + 0, value + frame);
                    kstore(pixels + i * 3 + 1, value + i);
                    kstore(pixels + i * 3 + 2, frame + i);
                    asset_index = asset_index + 1;
                    if asset_index - 81 >= 0 { asset_index = 0; }
                    i = i + 1;
                }
                if frame - 9 == 0 {
                    if window_route_input(EVENT_KIND_KEY, 75, 0, 0) - window == 0 {
                        input_events = input_events + 1;
                    }
                }
                if window_present(1, window) <= 0 { return 0; }
                kernel_timer_tick();
                frame = frame + 1;
            }
            kstore(BENCH_BASE + 7, 9);

            var visible: t40 = framebuffer_visible_base();
            var frame_hash: t40 = 0;
            i = 0;
            while 81 - i > 0 {
                frame_hash = frame_hash + (i + 1) * kload(visible + i);
                i = i + 1;
            }
            kstore(BENCH_BASE + 0, loaded_hash);
            kstore(BENCH_BASE + 1, frame_hash);
            kstore(BENCH_BASE + 2, kload(COMPOSITOR_FRAME_COUNT_ADDR));
            kstore(BENCH_BASE + 3, input_events);
            kstore(BENCH_BASE + 4, kload(KERNEL_TICK_ADDR));
            kstore(BENCH_BASE + 5, 81);
            kstore(BENCH_BASE + 6, pixels + 243);
            if kload(COMPOSITOR_FRAME_COUNT_ADDR) - 27 != 0 { return 0; }
            if kload(KERNEL_TICK_ADDR) - 27 != 0 { return 0; }
            if input_events - 1 != 0 { return 0; }
            return 1;
        }
    )";

    const auto compile_begin = Clock::now();
    const auto compiled = sandbox::compiler::compileSource(
        "benchmark_doom_os.trit", kernel + "\n" + driver);
    sandbox::compiler::LinkResult linked;
    if (compiled.success) {
        linked = sandbox::compiler::linkModules({compiled.object});
    }
    const auto compile_end = Clock::now();

    bool passed = compiled.success && linked.success;
    std::string detail = passed ? "ok" : "compile-or-link-failed";
    long long steps = 0;
    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    sandbox::vm::RunResult result;
    const auto run_begin = Clock::now();
    if (passed) {
        vm.resetBlockDevice(192);
        passed = sandbox::vm::assembler::loadAndReset(vm, linked.assembled);
        if (passed) {
            result = sandbox::vm::run(vm, 100000000);
            steps = result.steps;
            passed = result.halted() &&
                     sandbox::vm::ops::toLong(vm.regfile.read(13)) == 1;
            if (!passed) detail = "runtime-correctness-failed";
        } else {
            detail = "image-load-failed";
        }
    }
    const auto run_end = Clock::now();

    const long long asset_checksum = wordAt(vm, 35600);
    const long long frame_hash = wordAt(vm, 35601);
    const long long frames = wordAt(vm, 35602);
    const long long input_events = wordAt(vm, 35603);
    const long long ticks = wordAt(vm, 35604);
    const long long asset_words = wordAt(vm, 35605);
    const long long high_water = wordAt(vm, 35606);
    passed = passed && asset_checksum == 54 && frame_hash == 8235 &&
             frames == 27 && ticks == 27 &&
             input_events == 1 && asset_words == 81;
    if (!writeReport(report, passed, detail,
                     milliseconds(compile_begin, compile_end),
                     milliseconds(run_begin, run_end),
                     steps, asset_checksum, frame_hash, frames, ticks,
                     input_events, asset_words, high_water)) {
        std::cerr << "benchmark_doom_os: failed to write " << report << "\n";
        return 1;
    }
    std::cout << "benchmark_doom_os: " << (passed ? "PASS" : "FAIL")
              << " report=" << report.string()
              << " frames=" << frames
              << " frame_hash=" << frame_hash
              << " stage=" << wordAt(vm, 35607) << "\n";
    return passed ? 0 : 1;
}
