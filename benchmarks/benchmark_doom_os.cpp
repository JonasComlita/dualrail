#include "ternary_compiler.h"
#include "ternary_vm.h"
#include "system_benchmark_support.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

static constexpr int kAssetChunkWords = 81;
// The bounded profile is deliberately small enough that a complete guest pass
// stays below the 60-second host budget.  The sustained profile below keeps
// the heavier 81-frame/729-word trace opt-in.
static constexpr int kGateFrames = 7;
static constexpr int kGateAssetWords = 81;
static constexpr int kGateInputEvents = 2;
static constexpr int kLargeFrames = 81;
static constexpr int kLargeAssetWords = 729;
static constexpr int kLargeInputEvents = 3;
static constexpr long long kMaximumSampleSeconds = 60;

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

struct DoomPassResult {
    bool passed = false;
    double seconds = 0.0;
    int returncode = 1;
    long long steps = 0;
    long long asset_checksum = 0;
    long long frame_hash = 0;
    long long frames = 0;
    long long input_events = 0;
    long long input_trace_hash = 0;
    long long ticks = 0;
    long long asset_words = 0;
    long long asset_read_operations = 0;
    long long asset_write_operations = 0;
    long long io_operations = 0;
    long long draw_calls = 0;
    long long present_calls = 0;
    long long pixel_end = 0;
    long long stage = 0;
    long long final_reg13 = 0;
    long long final_pc = 0;
    long long scheduler_epoch = 0;
    long long tier1_count = 0;
    long long macro_count = 0;
    long long tlb_hits = 0;
    long long tlb_misses = 0;
    long long span_cache_hits = 0;
    long long slow_path_faults = 0;
    long long vfs_lookups = 0;
    long long wal_next_lsn = 0;
    long long wal_durable_lsn = 0;
    long long wal_pending_tx = 0;
    long long wal_pending_blocks = 0;
    long long wal_last_error = 0;
    long long wal_durable_head = 0;
    long long wal_checkpoint = 0;
    long long buffer_dirty = 0;
    long long buffer_flushes = 0;
    long long buffer_evictions = 0;
    long long memory_high_water = 0;
    sandbox::vm::VMTlbStats vm_tlb;
    sandbox::vm::VMBlockDeviceStats disk;
    long long disk_allocated_blocks = 0;
    long long branch_instructions = 0;
    long long decoded_instructions = 0;
    long long cache_instructions = 0;
    long long decoded_trace_instructions = 0;
};

DoomPassResult runDoomPass(
    const sandbox::vm::VMState& baseline,
    const std::vector<sandbox::vm::TritWord27>& program,
    int expected_frames,
    int expected_asset_words,
    long long expected_asset_checksum,
    int expected_input_events,
    long long expected_frame_hash,
    long long expected_input_trace_hash,
    int max_steps) {
    sandbox::vm::VMState vm = baseline;
    vm.reset();
    // The system portfolio measures the portable decoded-trace executor's
    // steady state rather than paying the interpreter decode cost for every
    // 27-frame pass.  This is still the same v2 VM semantics and keeps the
    // workload useful on non-x86 hosts where native JIT emission is absent.
    vm.setExecutionBackend(sandbox::vm::VMExecutionBackend::DecodedTraceExecutor);
    vm.setTraceJitHotThreshold(4);
    vm.resetBlockDeviceStats();
    const auto begin = std::chrono::steady_clock::now();
    const sandbox::vm::RunResult result = sandbox::vm::run(vm, max_steps);
    const auto end = std::chrono::steady_clock::now();

    DoomPassResult sample;
    sample.seconds = std::chrono::duration<double>(end - begin).count();
    sample.steps = result.steps;
    sample.asset_checksum = wordAt(vm, 35600);
    sample.frame_hash = wordAt(vm, 35601);
    sample.frames = wordAt(vm, 35602);
    sample.input_events = wordAt(vm, 35603);
    sample.input_trace_hash = wordAt(vm, 35608);
    sample.ticks = wordAt(vm, 35604);
    sample.asset_words = wordAt(vm, 35605);
    sample.asset_read_operations = wordAt(vm, 35609);
    sample.asset_write_operations = wordAt(vm, 35610);
    sample.io_operations = wordAt(vm, 35611);
    sample.draw_calls = wordAt(vm, 35612);
    sample.present_calls = wordAt(vm, 35613);
    sample.pixel_end = wordAt(vm, 35606);
    sample.stage = wordAt(vm, 35607);
    sample.final_reg13 = sandbox::vm::ops::toLong(vm.regfile.read(13));
    sample.final_pc = vm.pc;
    sample.scheduler_epoch = wordAt(vm, 3013);
    sample.tier1_count = wordAt(vm, 3011);
    sample.macro_count = wordAt(vm, 3012);
    sample.tlb_hits = wordAt(vm, 3036);
    sample.tlb_misses = wordAt(vm, 3037);
    sample.span_cache_hits = wordAt(vm, 3038);
    sample.slow_path_faults = wordAt(vm, 3039);
    sample.vfs_lookups = wordAt(vm, 3040);
    sample.wal_next_lsn = wordAt(vm, 3041);
    sample.wal_durable_lsn = wordAt(vm, 3042);
    sample.wal_pending_tx = wordAt(vm, 3045);
    sample.wal_pending_blocks = wordAt(vm, 3046);
    sample.wal_last_error = wordAt(vm, 3049);
    sample.wal_durable_head = wordAt(vm, 3044);
    sample.wal_checkpoint = wordAt(vm, 3008);
    sample.buffer_dirty = wordAt(vm, 3025);
    sample.buffer_flushes = wordAt(vm, 3026);
    sample.buffer_evictions = wordAt(vm, 3027);
    sample.memory_high_water = std::max(
        sample.pixel_end,
        static_cast<long long>(vm.dmem.allocatedPages()) *
            sandbox::vm::SPARSE_VM_PAGE_WORDS);
    sample.vm_tlb = vm.tlb_stats;
    sample.disk = vm.blockDeviceStats();
    sample.disk_allocated_blocks = static_cast<long long>(vm.allocatedDiskBlocks());
    sample.branch_instructions = vm.branch_instructions_count;
    sample.decoded_instructions = vm.decode_instructions_count;
    sample.cache_instructions = vm.block_cache_stats.instructions_executed;
    sample.decoded_trace_instructions = vm.trace_jit_stats.instructions_executed;
    sample.passed = result.halted() &&
                    sample.final_reg13 == 1 &&
                    sample.asset_checksum == expected_asset_checksum &&
                    (expected_frame_hash == 0 || sample.frame_hash == expected_frame_hash) &&
                    sample.frames == expected_frames &&
                    sample.ticks == expected_frames &&
                    sample.input_events == expected_input_events &&
                    sample.input_trace_hash == expected_input_trace_hash &&
                    sample.asset_words == expected_asset_words &&
                    sample.asset_read_operations == expected_asset_words / kAssetChunkWords &&
                    sample.asset_write_operations == 1 &&
                    sample.io_operations == expected_asset_words / kAssetChunkWords + 1 &&
                    sample.draw_calls == expected_frames &&
                    sample.present_calls == expected_frames;
    sample.returncode = sample.passed ? 0 : 1;
    (void)program;
    return sample;
}

std::string doomCorrectnessHash(long long asset_checksum,
                                long long frame_hash,
                                long long frames,
                                long long ticks,
                                long long input_events,
                                long long input_trace_hash,
                                long long asset_words,
                                long long io_operations) {
    return trit::system_benchmark::hex64(trit::system_benchmark::fnv1a64(
        {asset_checksum, frame_hash, frames, ticks, input_events,
         input_trace_hash, asset_words, io_operations}));
}

void writeBenchmarkTiming(std::ostringstream& out,
                          const std::string& scope,
                          double bootstrap_seconds,
                          const std::vector<double>& samples,
                          int warmups,
                          int iterations) {
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    out << "\"timing\": {\n"
        << "    \"unit\": \"seconds\",\n"
        << "    \"scope\": " << trit::system_benchmark::jsonString(scope) << ",\n"
        << "    \"bootstrap_seconds\": " << std::setprecision(12)
        << bootstrap_seconds << ",\n"
        << "    \"warmups\": " << warmups << ",\n"
        << "    \"iterations\": " << iterations << ",\n"
        << "    \"samples\": ";
    trit::system_benchmark::writeSamples(out, samples);
    out << ",\n"
        << "    \"median\": " << timing.median << ",\n"
        << "    \"mean\": " << timing.mean << ",\n"
        << "    \"standard_deviation\": " << timing.standard_deviation << ",\n"
        << "    \"coefficient_of_variation\": " << timing.coefficient_of_variation << ",\n"
        << "    \"maximum_accepted_cv\": " << trit::system_benchmark::kMaximumAcceptedCv << ",\n"
        << "    \"stable\": " << (timing.stable ? "true" : "false") << "\n"
        << "  }";
}

bool writeDoomReport(
    const std::filesystem::path& path,
    bool passed,
    const std::string& detail,
    double compile_seconds,
    double boot_seconds,
    int frames,
    int asset_words,
    int asset_shards,
    long long expected_asset_checksum,
    int input_event_count,
    long long expected_frame_hash,
    long long expected_input_trace_hash,
    const std::vector<int>& warmup_returncodes,
    const std::vector<int>& measured_returncodes,
    const std::vector<double>& samples,
    const DoomPassResult& metrics,
    int warmups,
    int iterations,
    bool probe_profile) {
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    const std::string observed_hash = doomCorrectnessHash(
        metrics.asset_checksum, metrics.frame_hash, metrics.frames,
        metrics.ticks, metrics.input_events, metrics.input_trace_hash,
        metrics.asset_words, metrics.io_operations);
    const std::string expected_hash = doomCorrectnessHash(
        expected_asset_checksum,
        expected_frame_hash, frames, frames, input_event_count,
        expected_input_trace_hash, asset_words, asset_shards + 1);
    const auto disk = metrics.disk;
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"trit.benchmark_result.v1\",\n"
        << "  \"captured_at_utc\": "
        << trit::system_benchmark::jsonString(trit::system_benchmark::utcNow()) << ",\n"
        << "  \"source\": {\n"
        << "    \"repository\": \"TernaryStack\",\n"
        << "    \"commit\": " << trit::system_benchmark::jsonString(
            trit::system_benchmark::environmentValue("TRIT_BENCH_COMMIT", "unknown")) << ",\n"
        << "    \"dirty\": " << (trit::system_benchmark::environmentBool("TRIT_BENCH_DIRTY") ? "true" : "false") << ",\n"
        << "    \"generator\": \"synthetic-doom-v2\"\n"
        << "  },\n"
        << "  \"host\": {\n"
        << "    \"system\": " << trit::system_benchmark::jsonString(trit::system_benchmark::hostSystem()) << ",\n"
        << "    \"release\": " << trit::system_benchmark::jsonString(trit::system_benchmark::environmentValue("OS_VERSION", "unknown")) << ",\n"
        << "    \"machine\": " << trit::system_benchmark::jsonString(trit::system_benchmark::environmentValue("PROCESSOR_ARCHITECTURE", "unknown")) << ",\n"
        << "    \"processor\": " << trit::system_benchmark::jsonString(trit::system_benchmark::hostProcessor()) << ",\n"
        << "    \"python\": \"not-used-by-native-target\"\n"
        << "  },\n"
        << "  \"build\": {\n"
        << "    \"directory\": " << trit::system_benchmark::jsonString(trit::system_benchmark::environmentValue("TRIT_BUILD_DIR", "build")) << ",\n"
        << "    \"profile\": \"current-compiler\",\n"
        << "    \"compiler_seconds\": " << compile_seconds << ",\n"
        << "    \"backend\": \"decoded_trace_executor\"\n"
        << "  },\n"
        << "  \"workload\": {\n"
        << "    \"name\": \"doom-class-os\",\n"
        << "    \"suite\": \"system_benchmarks\",\n"
        << "    \"version\": \"synthetic-doom-v2\",\n"
        << "    \"profile\": " << trit::system_benchmark::jsonString(
            probe_profile ? "probe" : (asset_shards > 3 ? "large-sustained" : "bounded-gate")) << ",\n"
        << "    \"frames\": " << frames << ",\n"
        << "    \"asset_words\": " << asset_words << ",\n"
        << "    \"asset_shards\": " << asset_shards << ",\n"
        << "    \"input_events\": " << input_event_count << ",\n"
        << "    \"asset_generator\": \"guest_trit_cycle_neg_one_zero_one\"\n"
        << "  },\n"
        << "  \"correctness\": {\n"
        << "    \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "    \"detail\": " << trit::system_benchmark::jsonString(detail) << ",\n"
        << "    \"warmup_returncodes\": ";
    trit::system_benchmark::writeIntArray(out, warmup_returncodes);
    out << ",\n    \"measured_returncodes\": ";
    trit::system_benchmark::writeIntArray(out, measured_returncodes);
    out << ",\n"
        << "    \"expected_hash\": \"fnv1a64:" << expected_hash << "\",\n"
        << "    \"observed_hash\": \"fnv1a64:" << observed_hash << "\",\n"
        << "    \"hashes\": {\"asset\": " << metrics.asset_checksum
        << ", \"frame\": " << metrics.frame_hash
        << ", \"input_trace\": " << metrics.input_trace_hash << "},\n"
        << "    \"asset_words\": " << metrics.asset_words << ",\n"
        << "    \"io_operations\": " << metrics.io_operations << "\n"
        << "  },\n";
    writeBenchmarkTiming(out, "guest_workload_pass_after_boot",
                         boot_seconds, samples, warmups, iterations);
    out << ",\n"
        << "  \"instruction_mix\": {\n"
        << "    \"dynamic_total\": " << metrics.steps << ",\n"
        << "    \"branches\": " << metrics.branch_instructions << ",\n"
        << "    \"decoded_instructions\": " << metrics.decoded_instructions << ",\n"
        << "    \"cached_block_instructions\": " << metrics.cache_instructions << ",\n"
        << "    \"decoded_trace_instructions\": " << metrics.decoded_trace_instructions << "\n"
        << "  },\n"
        << "  \"memory\": {\n"
        << "    \"high_water_words\": " << metrics.memory_high_water << ",\n"
        << "    \"allocated_pages\": " << metrics.memory_high_water / sandbox::vm::SPARSE_VM_PAGE_WORDS << "\n"
        << "  },\n"
        << "  \"tlb\": {\n"
        << "    \"guest_hits\": " << metrics.tlb_hits << ",\n"
        << "    \"guest_misses\": " << metrics.tlb_misses << ",\n"
        << "    \"span_cache_hits\": " << metrics.span_cache_hits << ",\n"
        << "    \"slow_path_faults\": " << metrics.slow_path_faults << ",\n"
        << "    \"vm_l1_instruction_hits\": " << metrics.vm_tlb.instruction_l1_hits << ",\n"
        << "    \"vm_l1_data_hits\": " << metrics.vm_tlb.data_l1_hits << ",\n"
        << "    \"vm_l2_hits\": " << metrics.vm_tlb.l2_hits << ",\n"
        << "    \"vm_misses\": " << metrics.vm_tlb.misses << ",\n"
        << "    \"vm_walks\": " << metrics.vm_tlb.walks << ",\n"
        << "    \"vm_evictions\": " << metrics.vm_tlb.evictions << "\n"
        << "  },\n"
        << "  \"scheduler\": {\n"
        << "    \"timer_ticks\": " << metrics.ticks << ",\n"
        << "    \"input_events\": " << metrics.input_events << ",\n"
        << "    \"epoch\": " << metrics.scheduler_epoch << ",\n"
        << "    \"tier1_queue_depth\": " << metrics.tier1_count << ",\n"
        << "    \"macro_queue_depth\": " << metrics.macro_count << "\n"
        << "  },\n"
        << "  \"wal\": {\n"
        << "    \"next_lsn\": " << metrics.wal_next_lsn << ",\n"
        << "    \"durable_lsn\": " << metrics.wal_durable_lsn << ",\n"
        << "    \"pending_transactions\": " << metrics.wal_pending_tx << ",\n"
        << "    \"pending_blocks\": " << metrics.wal_pending_blocks << ",\n"
        << "    \"last_error\": " << metrics.wal_last_error << ",\n"
        << "    \"durable_head\": " << metrics.wal_durable_head << ",\n"
        << "    \"checkpoint\": " << metrics.wal_checkpoint << ",\n"
        << "    \"buffer_dirty\": " << metrics.buffer_dirty << ",\n"
        << "    \"buffer_flushes\": " << metrics.buffer_flushes << ",\n"
        << "    \"buffer_evictions\": " << metrics.buffer_evictions << "\n"
        << "  },\n"
        << "  \"disk\": {\n"
        << "    \"read_words\": " << metrics.asset_words << ",\n"
        << "    \"reads\": " << disk.reads << ",\n"
        << "    \"writes\": " << disk.writes << ",\n"
        << "    \"cache_hits\": " << disk.hits << ",\n"
        << "    \"cache_misses\": " << disk.misses << ",\n"
        << "    \"flushes\": " << disk.flushes << ",\n"
        << "    \"allocated_blocks\": " << metrics.disk_allocated_blocks << "\n"
        << "  },\n"
        << "  \"graphics\": {\n"
        << "    \"frames_presented\": " << metrics.frames << ",\n"
        << "    \"frame_hash\": " << metrics.frame_hash << ",\n"
        << "    \"stage\": " << metrics.stage << ",\n"
        << "    \"final_reg13\": " << metrics.final_reg13 << ",\n"
        << "    \"final_pc\": " << metrics.final_pc << ",\n"
        << "    \"input_events\": " << metrics.input_events << ",\n"
        << "    \"input_trace_hash\": " << metrics.input_trace_hash << ",\n"
        << "    \"draw_calls\": " << metrics.draw_calls << ",\n"
        << "    \"present_calls\": " << metrics.present_calls << ",\n"
        << "    \"late_frames\": 0\n"
        << "  },\n"
        << "  \"compute\": {\"kernel\": \"none\", \"operations\": 0},\n"
        << "  \"probe\": " << (probe_profile ? "true" : "false") << "\n"
        << "}\n";
    return trit::system_benchmark::writeText(path, out.str());
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

    const bool probe_profile =
        trit::system_benchmark::environmentBool("TRIT_BENCH_PROBE");
    const bool large_profile =
        trit::system_benchmark::environmentValue(
            "TRIT_BENCH_PROFILE", "bounded-gate") == "large-sustained" && !probe_profile;
    const int frames = probe_profile ? 1 : (large_profile ? kLargeFrames : kGateFrames);
    const int asset_words = probe_profile ? kAssetChunkWords
        : (large_profile ? kLargeAssetWords : kGateAssetWords);
    const int asset_shards = asset_words / kAssetChunkWords;
    const int input_event_count = probe_profile ? 0
        : (large_profile ? kLargeInputEvents : kGateInputEvents);
    const long long expected_asset_checksum = 2LL * (asset_words / 3);
    const long long expected_frame_hash = probe_profile ? 1683
        : (large_profile ? 21843 : 3195);
    const long long expected_input_trace_hash = probe_profile ? 0
        : (large_profile ? 8719 : 2213);

    std::string driver = R"(
        const BENCH_BASE: t40 = 35600;

        fn main() -> t40 {
            if kload(BENCH_BASE + 7) == 0 {
                if kernel_init() <= 0 { return 0; }
                kstore(BENCH_BASE + 7, 1);
                return 1;
            }
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
            while @ASSET_WORDS@ - i > 0 {
                kstore(asset + i, trit);
                source_hash = source_hash + (i + 1) * trit;
                trit = trit + 1;
                if trit - 1 > 0 { trit = -1; }
                i = i + 1;
            }
            var fd: t40 = vfs_open(1, path, 3);
            if fd < 0 { return 0; }
            kstore(BENCH_BASE + 7, 2);
            if vfs_write(1, fd, asset, @ASSET_WORDS@) - @ASSET_WORDS@ != 0 { return 0; }
            kstore(BENCH_BASE + 7, 3);
            var asset_write_operations: t40 = 1;
            var asset_read_operations: t40 = 0;
            var io_operations: t40 = 1;
            vfs_close(1, fd);
            fd = vfs_open(1, path, 0);
            if fd < 0 { return 0; }
            var read_offset: t40 = 0;
            while @ASSET_WORDS@ - read_offset > 0 {
                if vfs_read(1, fd, loaded + read_offset, @ASSET_CHUNK_WORDS@) - @ASSET_CHUNK_WORDS@ != 0 { return 0; }
                read_offset = read_offset + @ASSET_CHUNK_WORDS@;
                asset_read_operations = asset_read_operations + 1;
                io_operations = io_operations + 1;
            }
            kstore(BENCH_BASE + 7, 4);
            vfs_close(1, fd);

            i = 0;
            var loaded_hash: t40 = 0;
            while @ASSET_WORDS@ - i > 0 {
                loaded_hash = loaded_hash + (i + 1) * kload(loaded + i);
                i = i + 1;
            }
            if loaded_hash - source_hash != 0 { return 0; }
            if loaded_hash - @ASSET_CHECKSUM@ != 0 { return 0; }
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
            var input_trace_hash: t40 = 0;
            var draw_calls: t40 = 0;
            var present_calls: t40 = 0;
            var asset_index: t40 = 0;
            while @DOOM_FRAMES@ - frame > 0 {
                i = 0;
                while 27 - i > 0 {
                    var value: t40 = kload(loaded + asset_index) + 1;
                    kstore(pixels + i * 3 + 0, value + frame);
                    kstore(pixels + i * 3 + 1, value + i);
                    kstore(pixels + i * 3 + 2, frame + i);
                    asset_index = asset_index + 1;
                    if asset_index - @ASSET_WORDS@ >= 0 { asset_index = 0; }
                    i = i + 1;
                }
                @FIRST_INPUT_EVENT@
                @SECOND_INPUT_EVENT@
                @EXTRA_INPUT_EVENT@
                draw_calls = draw_calls + 1;
                if window_present(1, window) <= 0 { return 0; }
                present_calls = present_calls + 1;
                kernel_timer_tick();
                frame = frame + 1;
            }
            kstore(BENCH_BASE + 7, 9);

            var visible: t40 = framebuffer_visible_base();
            var frame_hash: t40 = 0;
            i = 0;
            while 27 - i > 0 {
                frame_hash = frame_hash + (i + 1) * kload(visible + i);
                i = i + 1;
            }
            kstore(BENCH_BASE + 0, loaded_hash);
            kstore(BENCH_BASE + 1, frame_hash);
            kstore(BENCH_BASE + 2, kload(COMPOSITOR_FRAME_COUNT_ADDR));
            kstore(BENCH_BASE + 3, input_events);
            kstore(BENCH_BASE + 4, kload(KERNEL_TICK_ADDR));
            kstore(BENCH_BASE + 5, @ASSET_WORDS@);
            kstore(BENCH_BASE + 6, pixels + 243);
            kstore(BENCH_BASE + 8, input_trace_hash);
            kstore(BENCH_BASE + 9, asset_read_operations);
            kstore(BENCH_BASE + 10, asset_write_operations);
            kstore(BENCH_BASE + 11, io_operations);
            kstore(BENCH_BASE + 12, draw_calls);
            kstore(BENCH_BASE + 13, present_calls);
            if kload(COMPOSITOR_FRAME_COUNT_ADDR) - @DOOM_FRAMES@ != 0 { return 0; }
            if kload(KERNEL_TICK_ADDR) - @DOOM_FRAMES@ != 0 { return 0; }
            if input_events - @INPUT_EVENTS@ != 0 { return 0; }
            return 1;
        }
    )";
    const std::string extra_input_event = large_profile ? R"(
                if frame - 63 == 0 {
                    if window_route_input(EVENT_KIND_KEY, 80, 0, 0) - window == 0 {
                        input_events = input_events + 1;
                        input_trace_hash = input_trace_hash + 5120;
                    }
                }
)" : "";
    const std::string first_input_event = probe_profile ? "" :
        (large_profile ? R"(
                if frame - 9 == 0 {
                    if window_route_input(EVENT_KIND_KEY, 75, 0, 0) - window == 0 {
                        input_events = input_events + 1;
                        input_trace_hash = input_trace_hash + 750;
                    }
                }
)" : R"(
                if frame - 2 == 0 {
                    if window_route_input(EVENT_KIND_KEY, 75, 0, 0) - window == 0 {
                        input_events = input_events + 1;
                        input_trace_hash = input_trace_hash + 750;
                    }
                }
)");
    const std::string second_input_event = large_profile ? R"(
                if frame - 36 == 0 {
                    if window_route_input(EVENT_KIND_KEY, 77, 0, 0) - window == 0 {
                        input_events = input_events + 1;
                        input_trace_hash = input_trace_hash + 2849;
                    }
                }
)" : R"(
                if frame - 6 == 0 {
                    if window_route_input(EVENT_KIND_KEY, 77, 0, 0) - window == 0 {
                        input_events = input_events + 1;
                        input_trace_hash = input_trace_hash + 1463;
                    }
                }
)";
    const auto replace = [](std::string& text,
                            const std::string& token,
                            const std::string& value) {
        std::size_t offset = 0;
        while ((offset = text.find(token, offset)) != std::string::npos) {
            text.replace(offset, token.size(), value);
            offset += value.size();
        }
    };
    replace(driver, "@DOOM_FRAMES@", std::to_string(frames));
    replace(driver, "@ASSET_WORDS@", std::to_string(asset_words));
    replace(driver, "@ASSET_CHUNK_WORDS@", std::to_string(kAssetChunkWords));
    replace(driver, "@ASSET_CHECKSUM@", std::to_string(expected_asset_checksum));
    replace(driver, "@INPUT_EVENTS@", std::to_string(input_event_count));
    replace(driver, "@FIRST_INPUT_EVENT@", first_input_event);
    replace(driver, "@SECOND_INPUT_EVENT@", second_input_event);
    replace(driver, "@EXTRA_INPUT_EVENT@", extra_input_event);

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
    std::vector<int> warmup_returncodes;
    std::vector<int> measured_returncodes;
    std::vector<double> samples;
    DoomPassResult metrics;
    double boot_seconds = 0.0;
    const int warmups = trit::system_benchmark::environmentBool("TRIT_BENCH_PROBE")
        ? 0 : trit::system_benchmark::kWarmups;
    const int iterations = trit::system_benchmark::environmentBool("TRIT_BENCH_PROBE")
        ? 1 : trit::system_benchmark::kIterations;
    const int max_steps = probe_profile ? 30000000
        : (large_profile ? 100000000 : 50000000);
    bool sample_budget_exceeded = false;

    if (passed) {
        sandbox::vm::VMState boot_vm(sandbox::vm::ProductionProfile::minimum());
        // v2 VFS persistence reserves the inode/extent/data regions plus the
        // 729-record WAL ring; 192 blocks was the legacy bring-up fixture and
        // now correctly surfaces ERR_NO_SPACE during the first large write.
        boot_vm.resetBlockDevice(8192);
        if (!sandbox::vm::assembler::loadAndReset(boot_vm, linked.assembled)) {
            passed = false;
            detail = "image-load-failed";
        } else {
            const auto boot_begin = Clock::now();
            const sandbox::vm::RunResult boot_result =
                sandbox::vm::run(boot_vm, 100000000);
            const auto boot_end = Clock::now();
            boot_seconds = std::chrono::duration<double>(boot_end - boot_begin).count();
            const bool boot_ok = boot_result.halted() &&
                sandbox::vm::ops::toLong(boot_vm.regfile.read(13)) == 1 &&
                wordAt(boot_vm, 35607) == 1;
            if (!boot_ok) {
                passed = false;
                detail = "guest-boot-failed";
            } else {
                const sandbox::vm::VMState baseline = boot_vm;
                for (int iteration = 0;
                     iteration < warmups + iterations;
                     ++iteration) {
                    DoomPassResult sample = runDoomPass(
                        baseline, linked.assembled.program,
                        frames, asset_words, expected_asset_checksum,
                        input_event_count, expected_frame_hash,
                        expected_input_trace_hash, max_steps);
                    metrics = sample;
                    if (iteration < warmups) {
                        warmup_returncodes.push_back(sample.returncode);
                    } else {
                        measured_returncodes.push_back(sample.returncode);
                        samples.push_back(sample.seconds);
                    }
                    if (sample.seconds > static_cast<double>(kMaximumSampleSeconds)) {
                        sample_budget_exceeded = true;
                        if (detail == "ok") detail = "sample-time-budget-exceeded";
                        break;
                    }
                    if (!sample.passed && detail == "ok") {
                        detail = "runtime-correctness-failed";
                    }
                }
                const auto timing = trit::system_benchmark::summarizeTiming(samples);
                passed = passed && (probe_profile || timing.stable) &&
                         warmup_returncodes.size() ==
                             static_cast<std::size_t>(warmups) &&
                         measured_returncodes.size() ==
                             static_cast<std::size_t>(iterations);
                if (!probe_profile && !timing.stable && detail == "ok") {
                    detail = "host-timing-unstable";
                }
                for (int code : warmup_returncodes) passed = passed && code == 0;
                for (int code : measured_returncodes) passed = passed && code == 0;
            }
        }
    } else {
        warmup_returncodes.assign(warmups, 1);
        measured_returncodes.assign(iterations, 1);
    }

    if (sample_budget_exceeded) passed = false;
    if (!writeDoomReport(
            report, passed, detail,
            std::chrono::duration<double>(compile_end - compile_begin).count(),
            boot_seconds, frames, asset_words, asset_shards,
            expected_asset_checksum, input_event_count, expected_frame_hash,
            expected_input_trace_hash, warmup_returncodes,
            measured_returncodes, samples, metrics, warmups, iterations,
            probe_profile)) {
        std::cerr << "benchmark_doom_os: failed to write " << report << "\n";
        return 1;
    }
    std::cout << "benchmark_doom_os: " << (passed ? "PASS" : "FAIL")
              << " report=" << report.string()
              << " frames=" << metrics.frames
              << " frame_hash=" << metrics.frame_hash
              << " cv=" << trit::system_benchmark::summarizeTiming(samples).coefficient_of_variation
              << "\n";    return passed ? 0 : 1;
}
