#include "ternary_host_runtime.h"
#include "ternary_os.h"
#include "ternary_compiler.h"

// The conformance executable must evaluate checks in Release builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <regex>
#include <map>
#include <vector>
#include <chrono>
#include <set>
#include <array>
#include <tuple>
#include <algorithm>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string stableHash(const std::string& text) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string jsonEscape(const std::string& text) {
    std::ostringstream out;
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << std::hex << std::setw(2)
                        << std::setfill('0') << static_cast<int>(ch)
                        << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

bool runNumericWorkflow() {
    using namespace sandbox;
    const auto artifact = std::filesystem::path(TRIT_TEST_BUILD_DIR) / "numeric_workflow_artifact";
    std::filesystem::create_directories(artifact);
    const std::string guest = R"TRIT(
const QUANTUM: t40 = radix3(1, -5);
const FRACTION: t40 = 1 / 243;
const DERIVED_FRACTION: t40 = FRACTION / 3;
const SHORT_FRACTION: t20 = 3 / 2;
const SHORT_ALIAS: t40 = SHORT_FRACTION;
fn impulse(x: t40) -> t40 { return x * FRACTION; }
fn main() -> t40 {
    const LOCAL_FRACTION: t40 = 1 / 9;
    var torque: t40 = 1;
    var i: t40 = 0;
    while i < 243 { torque = torque - impulse(1); i = i + 1; }
    let tiny: t40 = radix3(1, -1000);
    let huge: t40 = radix3(1, 1000);
    let global_fraction: t40 = FRACTION;
    var direct_torque: t40 = 1;
    i = 0;
    while i < 243 { direct_torque = direct_torque - radix3(1, -5); i = i + 1; }
    const FRACTION: t40 = 1 / 9;
    unsafe {
        store(10000, QUANTUM);
        store(10001, global_fraction);
        store(10002, 1 / 243);
        store(10003, 3 / 2);
        store(10004, torque);
        store(10005, LOCAL_FRACTION);
        store(10006, radix3(-7, 2));
        store(10007, tiny * huge);
        store(10008, DERIVED_FRACTION);
        store(10009, FRACTION);
        store(10010, direct_torque);
        store(10011, SHORT_ALIAS);
    }
    return 0;
}
)TRIT";
    std::ofstream(artifact / "guest.trit") << guest;
    const auto exact = [](long long coefficient, int exponent) {
        return native_ops::detail::packSigned<native_ops::detail::FmtT40>(
            Int128::fromLongLong(coefficient), exponent + 32);
    };
    bool ok = true;
    std::ostringstream guest_results;
    bool first_result = true;
    const auto check = [&](bool pass, const std::string& message) {
        ok &= require(pass, "numeric workflow: " + message);
        return pass;
    };
    for (const auto level : {compiler::OptimizationLevel::None,
                             compiler::OptimizationLevel::Basic,
                             compiler::OptimizationLevel::Aggressive}) {
        compiler::CompilerOptions options;
        options.optimization = level;
        const auto compiled = compiler::compileSource("numeric_guest.trit", guest, options);
        if (!check(compiled.success, "guest compilation")) {
            for (const auto& diagnostic : compiled.diagnostics) std::cerr << diagnostic.format() << '\n';
            break;
        }
        compiler::LinkOptions link;
        link.standalone_halt_on_exit = true;
        const auto linked = compiler::linkModules({compiled.object}, link);
        if (!check(linked.success, "guest linking")) {
            for (const auto& diagnostic : linked.diagnostics) std::cerr << diagnostic.format() << '\n';
            std::ofstream(artifact / "failed_guest.tasm") << linked.assembly;
            break;
        }
        const std::string suffix = std::to_string(static_cast<int>(level));
        std::ofstream(artifact / ("guest_O" + suffix + ".tasm")) << linked.assembly;
        vm::VMState machine(32768, 32768);
        if (!check(vm::assembler::loadAndReset(machine, linked.assembled), "guest load")) break;
        vm::run(machine, 300000);
        if (!check(machine.status == vm::VMStatus::HALTED, "guest halted")) break;
        const std::array<Triple, 12> expected = {
            exact(1, -5), exact(1, -5), exact(1, -5),
            native_ops::divide(native_ops::fromIntT40(3), native_ops::fromIntT40(2)),
            Triple{}, exact(1, -2), exact(-7, 2), exact(1, 0), exact(1, -6), exact(1, -2), Triple{},
            native_ops::detail::convertFloat<native_ops::detail::FmtT20, native_ops::detail::FmtT40>(
                native_ops::divide(native_ops::fromIntT20(3), native_ops::fromIntT20(2)))};
        for (int word = 0; word < 12; ++word) {
            const auto value = machine.dmem.load(10000 + word);
            if (!first_result) guest_results << ',';
            first_result = false;
            guest_results << "{\"level\":" << suffix << ",\"word\":" << 10000 + word
                          << ",\"actual\":" << value.first.asTriple().data
                          << ",\"expected\":" << expected[word].data << '}';
            check(value.second == vm::MemFaultCode::OK && value.first.mode == TernaryMode::T40 &&
                  value.first.asTriple().data == expected[word].data,
                  "compiled fraction/torque result O" + suffix + " word " + std::to_string(word));
        }
    }
    // A reduction survives actual ISA context store/load and a disk checkpoint
    // while the accumulator is live. AST constants are independently checked above.
    const std::string assembly = R"ASM(.isa 2
.require vector accumulator_ai vector_context vector_geometry executable_v3
.text
main:
 mov r1, 1
 mov r2, 243
 div.t40 r3, r1, r2
 mov r4, 18000
 mov r5, 0
 aload.t40 r1
loop:
 asub.t40 r3
 add.t40 r5, r5, r1
 vctxstore r4
 aclr.t40
 vctxload r4
 sub.t40 r6, r5, r2
 brn r6, loop
 astore.t40 r7
 halt
)ASM";
    const auto features = vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES |
        isa::featureBit(architecture::v2::FEATURE_ACCUMULATOR_AI) |
        isa::featureBit(architecture::v3::FEATURE_VECTOR_ABI_V3) |
        isa::featureBit(architecture::v3::FEATURE_VECTOR_GEOMETRY),
                                                          architecture::v3::FEATURE_V3_LAST);
    const std::string executable = assembly + "\n.data\nexec: .execheader3 0, 15, 0, 2043, " +
        std::to_string(features) + ", 2, 0\n";
    std::ofstream(artifact / "accumulator.tasm") << executable;
    const auto assembled = vm::assembler::assemble(executable);
    if (check(assembled.success, "accumulator assembly")) {
        vm::ProductionProfile profile;
        profile.cores = 1;
        profile.ram_words = 32768;
        profile.instruction_words = 32768;
        profile.disk_blocks = 16;
        profile.framebuffer_width = 8;
        profile.framebuffer_height = 4;
        profile.framebuffer_words = 32;
        vm::VMState machine(profile);
        check(vm::assembler::loadAndReset(machine, assembled), "accumulator load");
        bool saved = false;
        std::unique_ptr<vm::VMState> restored;
        std::string error;
        for (int step = 0; step < 10000 && machine.status == vm::VMStatus::RUNNING; ++step) {
            vm::step(machine);
            if (!saved && vm::ops::toLong(machine.regfile.read(5)) == 117) {
                check(host::detail::writeCheckpointState(artifact / "mid_reduction.bin", machine, &error), "checkpoint save " + error);
                const bool loaded = host::detail::readCheckpointState(artifact / "mid_reduction.bin", restored, &error);
                check(loaded, "checkpoint load " + error);
                saved = true;
            }
        }
        check(saved && restored != nullptr, "mid-reduction checkpoint exists");
        if (restored) {
            vm::run(*restored, 10000);
            check(restored->status == vm::VMStatus::HALTED &&
                  restored->regfile.read(7).isZero(), "resumed reduction reaches exact zero");
        }
        check(machine.status == vm::VMStatus::HALTED && machine.regfile.read(7).isZero(), "uninterrupted reduction reaches exact zero");
    } else {
        for (const auto& error : assembled.errors) std::cerr << error.message << '\n';
    }
    // The native helper exercises rounding and exceptional states against the
    // existing packed T40 arithmetic, including cancellation after each boundary.
    ops::TernaryAccumulator accumulator;
    Triple reference{};
    const std::array<Triple, 12> values = {exact(1, 0), exact(1, -33), exact(2, -33),
        exact(-1, 0), exact(7, -5), exact(-7, -5), Triple::Underflow,
        Triple{}, Triple::Overflow, exact(1, 0), Triple{Triple::validStateCount()},
        Triple::pack(std::array<int8_t, 40>{})};
    for (int cycle = 0; cycle < 30; ++cycle) {
        for (const auto value : values) {
            accumulator.add(value);
            reference = native_ops::add(reference, value);
            check(accumulator.result().data == reference.data, "strict T40 rounding/sentinel parity");
        }
        check(accumulator.flush().data == reference.data && accumulator.result().isZero(), "flush and reset");
        reference = Triple{};
    }
    std::array<int8_t, 40> noncanonical_zero{};
    noncanonical_zero[33] = 1;
    for (const auto initial : {Triple::pack(noncanonical_zero), Triple::Underflow,
                              Triple::Overflow, Triple{Triple::validStateCount()}}) {
        ops::TernaryAccumulator state;
        state.add(initial);
        state.add(exact(1, -5));
        check(state.result().data == native_ops::add(initial, exact(1, -5)).data,
              "noncanonical/exceptional input boundary parity");
    }
    // Same-host, same-precision representation comparison. Timing is evidence,
    // never a pass/fail threshold or a claim about physical ternary hardware.
    const std::array<Triple, 6> reduction = {exact(1, 0), exact(1, -5), exact(2, -33),
                                           exact(-1, 0), exact(-1, -5), exact(-2, -33)};
    const int reduction_steps = 30000;
    auto begin = std::chrono::steady_clock::now();
    Triple packed_result{};
    for (int i = 0; i < reduction_steps; ++i)
        packed_result = native_ops::add(packed_result, reduction[i % reduction.size()]);
    const auto packed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    begin = std::chrono::steady_clock::now();
    ops::TernaryAccumulator retained;
    for (int i = 0; i < reduction_steps; ++i) retained.add(reduction[i % reduction.size()]);
    const Triple retained_result = retained.result();
    const auto retained_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    check(retained_result.data == packed_result.data, "retained representation matches packed T40 reduction");
    double binary64 = 1, compensated = 1, correction = 0;
    long double extended = 1;
    for (int i = 0; i < 243; ++i) {
        binary64 -= 1.0 / 243.0;
        extended -= 1.0L / 243.0L;
        const double y = -1.0 / 243.0 - correction;
        const double next = compensated + y;
        correction = (next - compensated) - y;
        compensated = next;
    }
    for (const auto source : {
             "fn main() -> t40 { return radix3(1, 2000); }",
             "fn main() -> t40 { return radix3(1, -2000); }",
             "const BAD: t40 = radix3(1, 2000); fn main() -> t40 { return 0; }",
             "const BAD: t40 = radix3(1 / 2, -5); fn main() -> t40 { return 0; }",
             "fn main() -> t40 { var e: t40 = 5; return radix3(1, e); }",
             "fn main() -> t40 { return radix3(1); }"}) {
        check(!compiler::compileSource("invalid_numeric.trit", source).success,
              "invalid radix-3 constant rejected");
    }
    std::ofstream report(artifact / "report.json");
    report << "{\"status\":\"" << (ok ? "pass" : "fail")
           << "\",\"standard\":\"T40\",\"guest_hash\":\"" << stableHash(guest)
           << "\",\"optimization_levels\":3,\"torque_steps\":243,\"checkpoint_step\":117,"
              "\"rounding\":\"strict T40 per operation\",\"widening\":false,\"guest_results\":["
           << guest_results.str() << "],\"same_host_T40\":{\"steps\":" << reduction_steps
           << ",\"packed_ns\":" << packed_ns << ",\"retained_ns\":" << retained_ns
           << ",\"packed_result\":" << packed_result.data << ",\"retained_result\":" << retained_result.data
           << "},\"binary_controls\":{\"double_digits\":" << std::numeric_limits<double>::digits
           << ",\"long_double_digits\":" << std::numeric_limits<long double>::digits
           << std::setprecision(21) << ",\"double_residual\":" << binary64
           << ",\"long_double_residual\":" << extended << ",\"kahan_double_residual\":" << compensated
           << "},\"sources\":{\"native_ops\":\"" << stableHash(readTextFile("ternary_native_ops.h"))
           << "\",\"compiler_ast\":\"" << stableHash(readTextFile("ternary_compiler_ast.h"))
           << "\",\"compiler_codegen\":\"" << stableHash(readTextFile("ternary_compiler_codegen.h"))
           << "\"}}\n";
    std::cout << "numeric workflow artifact: " << artifact.string() << '\n';
    return ok;
}

bool runStreamWorkflow() {
    using namespace sandbox;

    const std::filesystem::path build_root = TRIT_TEST_BUILD_DIR;
    const std::filesystem::path artifact =
        build_root / "service_workflow_artifact";
    const std::filesystem::path stream_artifact = artifact / "stream";
    std::error_code reset_error;
    std::filesystem::remove_all(stream_artifact, reset_error);
    if (!require(!reset_error,
                 "cannot reset stream workflow artifact: " +
                     reset_error.message())) {
        return false;
    }
    std::filesystem::create_directories(stream_artifact);

    // This is compiled as one guest translation unit using the current
    // architecture contract, SDK, service adapter, dataflow, and stream code.
    // The guest writes its accepted events and scheduler observations into
    // guest DMEM; the host below independently reconciles those records.
    const std::string guest = R"TRIT(
const DF_TEST_KIND_SOURCE: t40 = 11;
const DF_TEST_KIND_SINK: t40 = 12;
const DF_TEST_KIND_PROBE: t40 = 13;
const DF_TEST_KIND_START_BLOCKED: t40 = 14;
const DF_TEST_KIND_FATAL_SINK: t40 = 15;
const DF_TEST_KIND_START_DONE: t40 = 16;
const DF_TEST_KIND_START_PROGRESS: t40 = 17;
const DF_TEST_KIND_CONTROL_SOURCE: t40 = 18;
const DF_TEST_KIND_START_GATE: t40 = 19;
const DF_TEST_KIND_COUNT_ERROR: t40 = 20;
const DF_TEST_REPORT: t40 = 2500;
const DF_TEST_OUT_CAP1: t40 = 5000;
const DF_TEST_OUT_CAP2: t40 = 5600;
const DF_TEST_OUT_FAIR: t40 = 6200;
const DF_TEST_OUT_CANCEL: t40 = 6800;
const DF_TEST_OUT_ANOMALY: t40 = 7400;
const DF_TEST_OUT_WINDOW: t40 = 8000;
const DF_TEST_ARENA_BASE: t40 = 12000;
const DF_TEST_ARENA_WORDS: t40 = 40000;

// Source context: [0]=next sequence, [1]=total, [2]=EOS accepted.
fn df_test_source_execute(node: t40) -> t40 {
    var custom: t40 = 0;
    var outputs: t40 = 0;
    unsafe {
        custom = load(node + 7);
        outputs = load(node + 3);
    }
    if df_node_is_cancel_requested(node) > 0 {
        unsafe { store(custom + 1, 0); }
    }
    var eos_sent: t40 = 0;
    unsafe { eos_sent = load(custom + 2); }
    if eos_sent > 0 { return DF_CALLBACK_DONE; }

    var chan: t40 = vec_get(outputs, 0);
    var next: t40 = 0;
    var total: t40 = 0;
    unsafe {
        next = load(custom);
        total = load(custom + 1);
    }
    var tag: t40 = DF_EVENT_DATA;
    var value: t40 = next + 1;
    if df_node_is_cancel_requested(node) > 0 {
        tag = DF_EVENT_EOS;
        value = 0;
    } else if next - total >= 0 {
        tag = DF_EVENT_EOS;
        value = 0;
    }
    if df_channel_write_event(chan, tag, value) > 0 {
        if tag - DF_EVENT_EOS == 0 {
            unsafe { store(custom + 2, 1); }
        } else {
            unsafe { store(custom, next + 1); }
        }
        return DF_CALLBACK_PROGRESS;
    }
    return DF_CALLBACK_BLOCKED_OUTPUT;
}

// Sink context: [0]=record base, [1]=accepted DATA count,
// [2]=stall countdown, [3]=stalls per event, [4]=stall count,
// [5]=EOS count, [6]=permanent stall flag.
fn df_test_sink_execute(node: t40) -> t40 {
    var custom: t40 = 0;
    var inputs: t40 = 0;
    unsafe {
        custom = load(node + 7);
        inputs = load(node + 2);
    }
    var permanent: t40 = 0;
    var remaining: t40 = 0;
    unsafe {
        permanent = load(custom + 6);
        remaining = load(custom + 2);
    }
    if permanent > 0 { return DF_CALLBACK_BLOCKED_OUTPUT; }
    if remaining > 0 {
        unsafe {
            store(custom + 2, remaining - 1);
            store(custom + 4, load(custom + 4) + 1);
        }
        return DF_CALLBACK_BLOCKED_OUTPUT;
    }
    var chan: t40 = vec_get(inputs, 0);
    var tag: t40 = df_channel_read(chan, node + DF_NODE_SCRATCH_VALUE);
    if (tag == DF_EVENT_NONE) { return DF_CALLBACK_BLOCKED_INPUT; }
    if (tag == DF_EVENT_EOS) {
        unsafe { store(custom + 5, load(custom + 5) + 1); }
        return DF_CALLBACK_DONE;
    }
    var count: t40 = 0;
    var record_base: t40 = 0;
    var value: t40 = 0;
    unsafe {
        count = load(custom + 1);
        record_base = load(custom);
        value = load(node + DF_NODE_SCRATCH_VALUE);
        store(record_base + count * 2, tag);
        store(record_base + count * 2 + 1, value);
        store(custom + 1, count + 1);
        store(custom + 2, load(custom + 3));
    }
    return DF_CALLBACK_PROGRESS;
}

fn df_test_probe_execute(node: t40) -> t40 {
    var counter: t40 = 0;
    unsafe { counter = load(node + 7); }
    unsafe { store(counter, load(counter) + 1); }
    return DF_CALLBACK_PROGRESS;
}

fn df_test_start_blocked_execute(node: t40) -> t40 {
    return DF_CALLBACK_BLOCKED_INPUT;
}

fn df_test_fatal_sink_execute(node: t40) -> t40 {
    return DF_CALLBACK_ERROR;
}

fn df_test_start_done_execute(node: t40) -> t40 {
    var counter: t40 = 0;
    unsafe { counter = load(node + 7); store(counter, load(counter) + 1); }
    return DF_CALLBACK_DONE;
}

// Context: [0]=gate open, [1]=callback attempts, [2]=completed calls.
fn df_test_start_gate_execute(node: t40) -> t40 {
    var context: t40 = 0;
    unsafe { context = load(node + 7); }
    var open: t40 = 0;
    var attempts: t40 = 0;
    unsafe { open = load(context); attempts = load(context + 1) + 1; }
    unsafe { store(context + 1, attempts); }
    if open <= 0 { return DF_CALLBACK_BLOCKED_INPUT; }
    unsafe { store(context + 2, load(context + 2) + 1); }
    return DF_CALLBACK_DONE;
}

fn df_test_start_progress_execute(node: t40) -> t40 {
    return DF_CALLBACK_PROGRESS;
}

fn df_test_count_error_execute(node: t40) -> t40 {
    var context: t40 = 0;
    unsafe { context = load(node + 7); store(context, load(context) + 1); }
    return DF_CALLBACK_ERROR;
}

fn df_test_control_source_execute(node: t40) -> t40 {
    var outputs: t40 = 0;
    var context: t40 = 0;
    unsafe { outputs = load(node + 3); context = load(node + 7); }
    var step: t40 = 0;
    unsafe { step = load(context); }
    var tag: t40 = DF_EVENT_EOS;
    var value: t40 = 0;
    if step == 0 { tag = DF_EVENT_DATA; value = 0 - 42; }
    if step == 1 { tag = DF_EVENT_ANOMALY; }
    if step == 2 { tag = DF_EVENT_UNCERTAIN; }
    if step == 3 { tag = DF_EVENT_ERROR; }
    if step == 4 { tag = DF_EVENT_CANCEL; }
    var channel: t40 = vec_get(outputs, 0);
    if df_channel_write_event(channel, tag, value) > 0 {
        unsafe { store(context, step + 1); }
        if (tag == DF_EVENT_EOS) { return DF_CALLBACK_DONE; }
        return DF_CALLBACK_PROGRESS;
    }
    return DF_CALLBACK_BLOCKED_OUTPUT;
}

fn df_service_kind_valid(kind: t40) -> t40 {
    if (kind == DF_TEST_KIND_SOURCE) { return 1; }
    if (kind == DF_TEST_KIND_SINK) { return 1; }
    if (kind == DF_TEST_KIND_PROBE) { return 1; }
    if (kind == DF_TEST_KIND_START_BLOCKED) { return 1; }
    if (kind == DF_TEST_KIND_FATAL_SINK) { return 1; }
    if (kind == DF_TEST_KIND_START_DONE) { return 1; }
    if (kind == DF_TEST_KIND_START_PROGRESS) { return 1; }
    if (kind == DF_TEST_KIND_CONTROL_SOURCE) { return 1; }
    if (kind == DF_TEST_KIND_START_GATE) { return 1; }
    if (kind == DF_TEST_KIND_COUNT_ERROR) { return 1; }
    return 0;
}

fn df_service_node_execute(node: t40) -> t40 {
    var kind: t40 = 0;
    unsafe { kind = load(node + 1); }
    if (kind == DF_TEST_KIND_SOURCE) { return df_test_source_execute(node); }
    if (kind == DF_TEST_KIND_SINK) { return df_test_sink_execute(node); }
    if (kind == DF_TEST_KIND_PROBE) { return df_test_probe_execute(node); }
    if (kind == DF_TEST_KIND_START_BLOCKED) { return df_test_start_blocked_execute(node); }
    if (kind == DF_TEST_KIND_FATAL_SINK) { return df_test_fatal_sink_execute(node); }
    if (kind == DF_TEST_KIND_START_DONE) { return df_test_start_done_execute(node); }
    if (kind == DF_TEST_KIND_START_PROGRESS) { return df_test_start_progress_execute(node); }
    if (kind == DF_TEST_KIND_CONTROL_SOURCE) { return df_test_control_source_execute(node); }
    if (kind == DF_TEST_KIND_START_GATE) { return df_test_start_gate_execute(node); }
    if (kind == DF_TEST_KIND_COUNT_ERROR) { return df_test_count_error_execute(node); }
    return df_service_unknown_kind(node);
}

fn df_test_new_source(sched: t40, total: t40) -> t40 {
    var context: t40 = malloc_raw(3);
    if context <= 0 { return 0; }
    unsafe {
        store(context, 0);
        store(context + 1, total);
        store(context + 2, 0);
    }
    return df_node_new(sched, 0, DF_TEST_KIND_SOURCE, context);
}

fn df_test_new_sink(sched: t40, record_base: t40,
                    initial_stall: t40, stall_each: t40,
                    permanent: t40) -> t40 {
    var context: t40 = malloc_raw(7);
    if context <= 0 { return 0; }
    unsafe {
        store(context, record_base);
        store(context + 1, 0);
        store(context + 2, initial_stall);
        store(context + 3, stall_each);
        store(context + 4, 0);
        store(context + 5, 0);
        store(context + 6, permanent);
    }
    return df_node_new(sched, 0, DF_TEST_KIND_SINK, context);
}

fn df_test_run_copy(capacity: t40, record_base: t40,
                    report_slot: t40) -> t40 {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return 0; }
    var sched: t40 = df_sched_new(8);
    if sched <= 0 { return 0; }
    var chan: t40 = df_channel_new(capacity);
    var source: t40 = df_test_new_source(sched, 256);
    var sink: t40 = df_test_new_sink(sched, record_base, 2, 2, 0);
    if df_bool_or(df_bool_or(chan <= 0, source <= 0), sink <= 0) { return 0; }
    df_node_add_output(source, chan);
    df_node_add_input(sink, chan);
    df_sched_register_channel(sched, chan);
    df_sched_register_stream(sched, source);
    df_sched_register_stream(sched, sink);

    var arena_at_construction: t40 = df_arena_used();

    var rounds: t40 = 0;
    var max_ops: t40 = 0;
    var ops: t40 = 0;
    while df_bool_and(df_sched_lifecycle(sched) == DF_SCHED_LIFECYCLE_RUNNING,
                      rounds < 5000) {
        ops = df_sched_run_budget(sched, 100);
        if ops - max_ops > 0 { max_ops = ops; }
        rounds = rounds + 1;
    }
    var custom: t40 = 0;
    var source_next: t40 = 0;
    var sink_count: t40 = 0;
    var sink_stalls: t40 = 0;
    var sink_eos: t40 = 0;
    unsafe {
        custom = load(source + 7);
        source_next = load(custom);
        custom = load(sink + 7);
        sink_count = load(custom + 1);
        sink_stalls = load(custom + 4);
        sink_eos = load(custom + 5);
        store(DF_TEST_REPORT + report_slot, sink_count);
        store(DF_TEST_REPORT + report_slot + 1, sink_eos);
        store(DF_TEST_REPORT + report_slot + 2, sink_stalls);
        store(DF_TEST_REPORT + report_slot + 3, max_ops);
        store(DF_TEST_REPORT + report_slot + 4, source_next);
        store(DF_TEST_REPORT + report_slot + 5, df_sched_lifecycle(sched));
        store(DF_TEST_REPORT + report_slot + 6, df_arena_highwater());
        store(DF_TEST_REPORT + report_slot + 7, arena_at_construction);
        store(DF_TEST_REPORT + report_slot + 8, df_arena_used());
    }
    df_sched_free(sched);
    unsafe { store(DF_TEST_REPORT + report_slot + 9, df_arena_used()); }
    return rounds;
}

// Forty registered callbacks make the requested budget of 100 observable:
// one dispatch must invoke exactly 32 distinct callback operations.
fn df_test_run_budget_probe() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var sched: t40 = df_sched_new(40);
    var counter: t40 = malloc_raw(1);
    unsafe { store(counter, 0); }
    var i: t40 = 0;
    while 40 - i > 0 {
        var probe: t40 = df_node_new(sched, 0, DF_TEST_KIND_PROBE, counter);
        df_sched_register_stream(sched, probe);
        i = i + 1;
    }
    var ops: t40 = df_sched_run_budget(sched, 100);
    var calls: t40 = 0;
    unsafe {
        calls = load(counter);
        store(DF_TEST_REPORT + 30, ops);
        store(DF_TEST_REPORT + 31, calls);
    }
    df_sched_free(sched);
}

fn df_test_run_fairness() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var sched: t40 = df_sched_new(8);
    var blocked_chan: t40 = df_channel_new(1);
    var free_chan: t40 = df_channel_new(2);
    var blocked_source: t40 = df_test_new_source(sched, 20);
    var blocked_sink: t40 = df_test_new_sink(sched, DF_TEST_OUT_FAIR, 0, 0, 1);
    var free_source: t40 = df_test_new_source(sched, 1);
    var free_sink: t40 = df_test_new_sink(sched, DF_TEST_OUT_FAIR + 100, 0, 0, 0);
    df_node_add_output(blocked_source, blocked_chan);
    df_node_add_input(blocked_sink, blocked_chan);
    df_node_add_output(free_source, free_chan);
    df_node_add_input(free_sink, free_chan);
    df_sched_register_channel(sched, blocked_chan);
    df_sched_register_channel(sched, free_chan);
    df_sched_register_stream(sched, blocked_source);
    df_sched_register_stream(sched, blocked_sink);
    df_sched_register_stream(sched, free_source);
    df_sched_register_stream(sched, free_sink);
    var ops: t40 = df_sched_run_budget(sched, 100);
    var free_ctx: t40 = 0;
    var free_count: t40 = 0;
    var blocked_ctx: t40 = 0;
    var blocked_count: t40 = 0;
    unsafe {
        free_ctx = load(free_sink + 7);
        blocked_ctx = load(blocked_sink + 7);
        free_count = load(free_ctx + 1);
        blocked_count = load(blocked_ctx + 1);
        store(DF_TEST_REPORT + 32, ops);
        store(DF_TEST_REPORT + 33, free_count);
        store(DF_TEST_REPORT + 34, blocked_count);
    }
    df_sched_free(sched);

    // Repeated one-operation budgets must alternate between streams and
    // ready startup work. A local turn variable would always pick the stream.
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    sched = df_sched_new(4);
    var stream_counter: t40 = malloc_raw(1);
    var startup_counter: t40 = malloc_raw(1);
    unsafe { store(stream_counter, 0); store(startup_counter, 0); }
    var live_stream: t40 = df_node_new(sched, 0, DF_TEST_KIND_PROBE, stream_counter);
    var startup: t40 = df_node_new(sched, 0, DF_TEST_KIND_START_DONE, startup_counter);
    df_sched_register_stream(sched, live_stream);
    df_sched_submit(sched, startup);
    df_sched_run_budget(sched, 1);
    df_sched_run_budget(sched, 1);
    unsafe {
        store(DF_TEST_REPORT + 35, load(stream_counter));
        store(DF_TEST_REPORT + 36, load(startup_counter));
        store(DF_TEST_REPORT + 37, df_sched_last_attempts(sched));
    }
    df_sched_free(sched);
}

fn df_test_run_cancel() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var sched: t40 = df_sched_new(8);
    var chan: t40 = df_channel_new(1);
    var source: t40 = df_test_new_source(sched, 20);
    var sink: t40 = df_test_new_sink(sched, DF_TEST_OUT_CANCEL, 2, 0, 0);
    df_node_add_output(source, chan);
    df_node_add_input(sink, chan);
    df_sched_register_channel(sched, chan);
    df_sched_register_stream(sched, source);
    df_sched_register_stream(sched, sink);
    df_sched_run_budget(sched, 32);
    var source_ctx_at_cancel: t40 = 0;
    var accepted_at_cancel: t40 = 0;
    unsafe {
        source_ctx_at_cancel = load(source + 7);
        accepted_at_cancel = load(source_ctx_at_cancel);
    }
    df_sched_cancel(sched);
    var rounds: t40 = 0;
    while df_bool_and(df_sched_lifecycle(sched) == DF_SCHED_LIFECYCLE_RUNNING,
                      rounds < 100) {
        df_sched_run_budget(sched, 32);
        rounds = rounds + 1;
    }
    var source_ctx: t40 = 0;
    var sink_ctx: t40 = 0;
    unsafe {
        source_ctx = load(source + 7);
        sink_ctx = load(sink + 7);
        store(DF_TEST_REPORT + 40, load(sink_ctx + 1));
        store(DF_TEST_REPORT + 41, load(sink_ctx + 5));
        store(DF_TEST_REPORT + 42, df_sched_lifecycle(sched));
        store(DF_TEST_REPORT + 43, load(source_ctx));
        store(DF_TEST_REPORT + 44, load(sink_ctx + 4));
        store(DF_TEST_REPORT + 45, accepted_at_cancel);
    }
    df_sched_free(sched);
}

fn df_test_run_validation() -> void {
    var counter: t40 = 0;
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var duplicate_sched: t40 = df_sched_new(4);
    var left: t40 = df_node_new(duplicate_sched, 0, DF_TEST_KIND_PROBE, counter);
    var right: t40 = df_node_new(duplicate_sched, 0, DF_TEST_KIND_PROBE, counter);
    var graph: t40 = df_vec_new_sized(2);
    df_vec_push_safe(graph, left); df_vec_push_safe(graph, right);
    df_node_add_dependent(left, right); df_node_add_dependent(left, right);
    df_sched_register_stream(duplicate_sched, left);
    df_sched_register_stream(duplicate_sched, right);
    var primitive_duplicate: t40 = df_graph_validate(graph);
    var scheduler_duplicate: t40 = df_sched_validate(duplicate_sched);
    var duplicate_ops: t40 = df_sched_run_budget(duplicate_sched, 32);
    unsafe {
        store(DF_TEST_REPORT + 70, primitive_duplicate);
        store(DF_TEST_REPORT + 71, scheduler_duplicate);
        store(DF_TEST_REPORT + 72, duplicate_ops);
        store(DF_TEST_REPORT + 73, load(counter));
    }
    df_sched_free(duplicate_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var cycle_sched: t40 = df_sched_new(4);
    left = df_node_new(cycle_sched, 0, DF_TEST_KIND_PROBE, counter);
    right = df_node_new(cycle_sched, 0, DF_TEST_KIND_PROBE, counter);
    df_node_add_dependent(left, right); df_node_add_dependent(right, left);
    var cycle_result: t40 = df_sched_validate(cycle_sched);
    unsafe { store(DF_TEST_REPORT + 74, cycle_result); }
    df_sched_free(cycle_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var invalid_sched: t40 = df_sched_new(3);
    var invalid_node: t40 = df_node_new(invalid_sched, 0, 99, counter);
    df_sched_register_stream(invalid_sched, invalid_node);
    var invalid_result: t40 = df_sched_validate(invalid_sched);
    var invalid_ops: t40 = df_sched_run_budget(invalid_sched, 32);
    unsafe {
        store(DF_TEST_REPORT + 75, invalid_result);
        store(DF_TEST_REPORT + 76, invalid_ops);
        store(DF_TEST_REPORT + 77, load(counter));
    }
    df_sched_free(invalid_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var exhausted_sched: t40 = df_sched_new(2);
    left = df_node_new(exhausted_sched, 0, DF_TEST_KIND_PROBE, counter);
    right = df_node_new(exhausted_sched, 0, DF_TEST_KIND_PROBE, counter);
    var overflow_node: t40 = df_node_new(exhausted_sched, 0, DF_TEST_KIND_PROBE, counter);
    df_sched_register_stream(exhausted_sched, left);
    df_sched_register_stream(exhausted_sched, right);
    var exhausted_result: t40 = df_sched_validate(exhausted_sched);
    var exhausted_ops: t40 = df_sched_run_budget(exhausted_sched, 32);
    unsafe {
        store(DF_TEST_REPORT + 78, overflow_node);
        store(DF_TEST_REPORT + 79, exhausted_result);
        store(DF_TEST_REPORT + 80, exhausted_ops);
        store(DF_TEST_REPORT + 81, load(counter));
    }
    df_sched_free(exhausted_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var startup_sched: t40 = df_sched_new(4);
    var blocker: t40 = df_node_new(startup_sched, 0, DF_TEST_KIND_START_BLOCKED, 0);
    var dependent: t40 = df_node_new(startup_sched, 0, DF_TEST_KIND_PROBE, counter);
    df_node_add_dependent(blocker, dependent);
    df_sched_submit(startup_sched, blocker);
    df_sched_run_loop(startup_sched);
    unsafe {
        store(DF_TEST_REPORT + 82, load(blocker));
        store(DF_TEST_REPORT + 83, load(dependent));
        store(DF_TEST_REPORT + 84, load(counter));
    }
    df_sched_free(startup_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var duplicate_submit_sched: t40 = df_sched_new(4);
    var duplicate_submit_node: t40 = df_node_new(
        duplicate_submit_sched, 0, DF_TEST_KIND_START_DONE, counter);
    var first_admission: t40 = df_sched_submit(duplicate_submit_sched, duplicate_submit_node);
    var second_admission: t40 = df_sched_submit(duplicate_submit_sched, duplicate_submit_node);
    var duplicate_submit_validation: t40 = df_sched_validate(duplicate_submit_sched);
    var duplicate_submit_ops: t40 = df_sched_run_budget(duplicate_submit_sched, 32);
    unsafe {
        store(DF_TEST_REPORT + 101, first_admission);
        store(DF_TEST_REPORT + 102, second_admission);
        store(DF_TEST_REPORT + 103, duplicate_submit_validation);
        store(DF_TEST_REPORT + 104, duplicate_submit_ops);
        store(DF_TEST_REPORT + 105, load(counter));
        store(DF_TEST_REPORT + 106, df_sched_lifecycle(duplicate_submit_sched));
    }
    df_sched_free(duplicate_submit_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var numerator_ptr: t40 = malloc_raw(1);
    var denominator_ptr: t40 = malloc_raw(1);
    unsafe { store(numerator_ptr, 3); store(denominator_ptr, 2); }
    var numerator: t40 = 0;
    var denominator: t40 = 0;
    unsafe { numerator = load(numerator_ptr); denominator = load(denominator_ptr); }
    var fractional_kind: t40 = numerator / denominator;
    var fractional_sched: t40 = df_sched_new(2);
    var fractional_node: t40 = df_node_new(fractional_sched, 0, fractional_kind, 0);
    unsafe {
        store(DF_TEST_REPORT + 107, fractional_node);
        store(DF_TEST_REPORT + 108, df_sched_validate(fractional_sched));
        store(DF_TEST_REPORT + 112, fractional_kind);
    }
    df_sched_free(fractional_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var priority_sched: t40 = df_sched_new(2);
    var priority_node: t40 = df_node_new(
        priority_sched, 0, DF_TEST_KIND_START_DONE, counter);
    var first_priority_admission: t40 = df_sched_submit_priority(priority_sched, priority_node);
    var second_priority_admission: t40 = df_sched_submit_priority(priority_sched, priority_node);
    var priority_validation: t40 = df_sched_validate(priority_sched);
    var priority_ops: t40 = df_sched_run_budget(priority_sched, 32);
    unsafe {
        store(DF_TEST_REPORT + 113, first_priority_admission);
        store(DF_TEST_REPORT + 114, second_priority_admission);
        store(DF_TEST_REPORT + 115, priority_validation);
        store(DF_TEST_REPORT + 116, priority_ops);
        store(DF_TEST_REPORT + 117, load(counter));
        store(DF_TEST_REPORT + 118, df_sched_lifecycle(priority_sched));
    }
    df_sched_free(priority_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    counter = malloc_raw(1);
    unsafe { store(counter, 0); }
    var priority_fair_sched: t40 = df_sched_new(4);
    var permanently_blocked_priority: t40 = df_node_new(
        priority_fair_sched, 0, DF_TEST_KIND_START_BLOCKED, 0);
    var ready_normal: t40 = df_node_new(
        priority_fair_sched, 0, DF_TEST_KIND_START_DONE, counter);
    df_sched_submit_priority(priority_fair_sched, permanently_blocked_priority);
    df_sched_submit(priority_fair_sched, ready_normal);
    var priority_fair_attempts: t40 = df_sched_run_budget(priority_fair_sched, 4);
    unsafe {
        store(DF_TEST_REPORT + 119, priority_fair_attempts);
        store(DF_TEST_REPORT + 120, load(counter));
        store(DF_TEST_REPORT + 121, load(permanently_blocked_priority));
        store(DF_TEST_REPORT + 122, load(ready_normal));
        store(DF_TEST_REPORT + 123, df_sched_lifecycle(priority_fair_sched));
    }
    df_sched_free(priority_fair_sched);
}

fn df_test_run_fatal_sink() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var sched: t40 = df_sched_new(4);
    var chan: t40 = df_channel_new(1);
    var source: t40 = df_test_new_source(sched, 20);
    var sink: t40 = df_node_new(sched, 0, DF_TEST_KIND_FATAL_SINK, 0);
    df_node_add_output(source, chan);
    df_node_add_input(sink, chan);
    df_sched_register_channel(sched, chan);
    df_sched_register_stream(sched, source);
    df_sched_register_stream(sched, sink);
    var attempted: t40 = df_sched_run_budget(sched, 32);
    var source_ctx: t40 = 0;
    unsafe { source_ctx = load(source + 7); }
    unsafe {
        store(DF_TEST_REPORT + 90, attempted);
        store(DF_TEST_REPORT + 91, df_sched_lifecycle(sched));
        store(DF_TEST_REPORT + 92, df_sched_uncommitted_count(sched));
        store(DF_TEST_REPORT + 93, load(source_ctx));
    }
    df_sched_free(sched);
}

fn df_test_run_startup_dag() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var parent_a_calls: t40 = malloc_raw(1);
    var parent_b_state: t40 = malloc_raw(3);
    var join_calls: t40 = malloc_raw(1);
    unsafe {
        store(parent_a_calls, 0); store(parent_b_state, 0);
        store(parent_b_state + 1, 0); store(parent_b_state + 2, 0);
        store(join_calls, 0);
    }
    var sched: t40 = df_sched_new(6);
    var parent_a: t40 = df_node_new(sched, 0, DF_TEST_KIND_START_DONE, parent_a_calls);
    var parent_b: t40 = df_node_new(sched, 0, DF_TEST_KIND_START_GATE, parent_b_state);
    var join: t40 = df_node_new(sched, 0, DF_TEST_KIND_START_DONE, join_calls);
    df_node_add_dependent(parent_a, join);
    df_node_add_dependent(parent_b, join);
    df_sched_submit(sched, parent_a);
    df_sched_submit(sched, parent_b);
    df_sched_run_loop(sched);
    unsafe { store(DF_TEST_REPORT + 109, load(join_calls)); }
    unsafe { store(parent_b_state, 1); }
    var dag_status: t40 = df_sched_run_loop(sched);
    unsafe {
        store(DF_TEST_REPORT + 85, load(parent_a_calls));
        store(DF_TEST_REPORT + 86, load(parent_b_state + 2));
        store(DF_TEST_REPORT + 87, load(join_calls));
        store(DF_TEST_REPORT + 88, load(join));
        store(DF_TEST_REPORT + 89, dag_status);
        store(DF_TEST_REPORT + 110, load(parent_b_state + 1));
        store(DF_TEST_REPORT + 111, load(parent_b_state + 2));
    }
    df_sched_free(sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var counter: t40 = malloc_raw(1);
    unsafe { store(counter, 0); }
    sched = df_sched_new(4);
    var progress: t40 = df_node_new(sched, 0, DF_TEST_KIND_START_PROGRESS, 0);
    var blocked_dep: t40 = df_node_new(sched, 0, DF_TEST_KIND_PROBE, counter);
    df_node_add_dependent(progress, blocked_dep);
    df_sched_submit(sched, progress);
    df_sched_run_loop(sched);
    unsafe {
        store(DF_TEST_REPORT + 94, load(progress));
        store(DF_TEST_REPORT + 95, load(blocked_dep));
        store(DF_TEST_REPORT + 96, load(counter));
    }
    df_sched_free(sched);
}

fn df_test_run_builtin_anomaly() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var sched: t40 = df_sched_new(8);
    var input: t40 = df_channel_new(1);
    var output: t40 = df_channel_new(1);
    var source_ctx: t40 = malloc_raw(3);
    var history: t40 = malloc_raw(10);
    if df_bool_or(df_bool_or(df_bool_or(sched <= 0, input <= 0),
                             df_bool_or(output <= 0, source_ctx <= 0)),
                  history <= 0) { return; }
    unsafe {
        store(source_ctx, 1); store(source_ctx + 1, 0); store(source_ctx + 2, 0);
        store(history, 12); store(history + 1, 0); store(history + 2, 0);
        store(history + 3, 1); store(history + 4, 3);
        store(history + 5, 0); store(history + 6, 0); store(history + 7, 0);
        store(history + 8, 0); store(history + 9, 3);
    }
    var source: t40 = df_node_new(sched, 0, DF_STREAM_KIND_SOURCE, source_ctx);
    var anomaly: t40 = df_node_new(sched, 0, DF_STREAM_KIND_ANOMALY, history);
    var sink: t40 = df_test_new_sink(sched, DF_TEST_OUT_ANOMALY, 2, 2, 0);
    if df_bool_or(df_bool_or(source <= 0, anomaly <= 0), sink <= 0) { return; }
    df_node_add_output(source, input);
    df_node_add_input(anomaly, input);
    df_node_add_output(anomaly, output);
    df_node_add_input(sink, output);
    df_sched_register_channel(sched, input);
    df_sched_register_channel(sched, output);
    df_sched_register_stream(sched, source);
    df_sched_register_stream(sched, anomaly);
    df_sched_register_stream(sched, sink);
    var rounds: t40 = 0;
    while df_bool_and(df_sched_lifecycle(sched) == DF_SCHED_LIFECYCLE_RUNNING,
                      rounds < 2000) {
        df_sched_run_budget(sched, 32);
        rounds = rounds + 1;
    }
    var sink_ctx: t40 = 0;
    unsafe { sink_ctx = load(sink + 7); }
    unsafe {
        store(DF_TEST_REPORT + 50, load(sink_ctx + 1));
        store(DF_TEST_REPORT + 51, load(sink_ctx + 5));
        store(DF_TEST_REPORT + 52, load(sink_ctx + 4));
        store(DF_TEST_REPORT + 53, df_sched_lifecycle(sched));
        store(DF_TEST_REPORT + 54, load(history + 3));
        store(DF_TEST_REPORT + 55, load(history + 8));
        store(DF_TEST_REPORT + 56, load(source_ctx + 1));
    }
    df_sched_free(sched);
}

fn df_test_run_builtin_window() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var sched: t40 = df_sched_new(8);
    var c0: t40 = df_channel_new(1);
    var c1: t40 = df_channel_new(1);
    var c2: t40 = df_channel_new(1);
    var c3: t40 = df_channel_new(1);
    var source_ctx: t40 = malloc_raw(3);
    var history: t40 = malloc_raw(10);
    var window_ctx: t40 = malloc_raw(4);
    var filter_threshold: t40 = 17;
    if df_bool_or(df_bool_or(df_bool_or(sched <= 0, c0 <= 0),
                             df_bool_or(c1 <= 0, c2 <= 0)),
                  df_bool_or(df_bool_or(c3 <= 0, source_ctx <= 0),
                             df_bool_or(df_bool_or(history <= 0, window_ctx <= 0),
                                        filter_threshold <= 0))) { return; }
    unsafe {
        store(source_ctx, 0); store(source_ctx + 1, 0); store(source_ctx + 2, 0);
        store(history, 0); store(history + 1, 0); store(history + 2, 0);
        store(history + 3, 0); store(history + 4, 100);
        store(history + 5, 0); store(history + 6, 0); store(history + 7, 0);
        store(history + 8, 0); store(history + 9, 3);
        store(window_ctx, 0); store(window_ctx + 1, 0);
        store(window_ctx + 2, 4); store(window_ctx + 3, 0);
    }
    var source: t40 = df_node_new(sched, 0, DF_STREAM_KIND_SOURCE, source_ctx);
    var anomaly: t40 = df_node_new(sched, 0, DF_STREAM_KIND_ANOMALY, history);
    var window: t40 = df_node_new(sched, 0, DF_STREAM_KIND_WINDOW, window_ctx);
    var filter: t40 = df_node_new(sched, 0, DF_STREAM_KIND_FILTER, filter_threshold);
    var sink: t40 = df_test_new_sink(sched, DF_TEST_OUT_WINDOW, 2, 2, 0);
    if df_bool_or(df_bool_or(df_bool_or(source <= 0, anomaly <= 0),
                             df_bool_or(window <= 0, filter <= 0)),
                  sink <= 0) { return; }
    df_node_add_output(source, c0);
    df_node_add_input(anomaly, c0); df_node_add_output(anomaly, c1);
    df_node_add_input(window, c1); df_node_add_output(window, c2);
    df_node_add_input(filter, c2); df_node_add_output(filter, c3);
    df_node_add_input(sink, c3);
    df_sched_register_channel(sched, c0); df_sched_register_channel(sched, c1);
    df_sched_register_channel(sched, c2); df_sched_register_channel(sched, c3);
    df_sched_register_stream(sched, source); df_sched_register_stream(sched, anomaly);
    df_sched_register_stream(sched, window); df_sched_register_stream(sched, filter);
    df_sched_register_stream(sched, sink);
    var rounds: t40 = 0;
    while df_bool_and(df_sched_lifecycle(sched) == DF_SCHED_LIFECYCLE_RUNNING,
                      rounds < 3000) {
        df_sched_run_budget(sched, 32);
        rounds = rounds + 1;
    }
    var sink_ctx: t40 = 0;
    unsafe { sink_ctx = load(sink + 7); }
    unsafe {
        store(DF_TEST_REPORT + 60, load(sink_ctx + 1));
        store(DF_TEST_REPORT + 61, load(sink_ctx + 5));
        store(DF_TEST_REPORT + 62, load(sink_ctx + 4));
        store(DF_TEST_REPORT + 63, df_sched_lifecycle(sched));
        store(DF_TEST_REPORT + 64, load(history + 3));
        store(DF_TEST_REPORT + 65, load(window_ctx + 1));
        store(DF_TEST_REPORT + 66, load(window_ctx));
        store(DF_TEST_REPORT + 67, load(source_ctx + 1));
    }
    df_sched_free(sched);
}

fn df_test_run_builtin_sink_controls() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    unsafe { store(31, 0); store(32, 0); store(34, 0); }
    var sched: t40 = df_sched_new(4);
    var channel: t40 = df_channel_new(1);
    var context: t40 = malloc_raw(1);
    unsafe { store(context, 0); }
    var source: t40 = df_node_new(sched, 0, DF_TEST_KIND_CONTROL_SOURCE, context);
    var sink: t40 = df_node_new(sched, 0, DF_STREAM_KIND_SINK, 0);
    df_node_add_output(source, channel);
    df_node_add_input(sink, channel);
    df_sched_register_channel(sched, channel);
    df_sched_register_stream(sched, source);
    df_sched_register_stream(sched, sink);
    var rounds: t40 = 0;
    while df_bool_and(df_sched_lifecycle(sched) == DF_SCHED_LIFECYCLE_RUNNING,
                      rounds < 100) {
        df_sched_run_budget(sched, 32);
        rounds = rounds + 1;
    }
    unsafe {
        store(DF_TEST_REPORT + 97, df_sched_lifecycle(sched));
        store(DF_TEST_REPORT + 98, load(context));
        store(DF_TEST_REPORT + 99, load(sink));
        store(DF_TEST_REPORT + 100, rounds);
    }
    df_sched_free(sched);
}

fn df_test_run_static() -> void {
    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var gate_context: t40 = malloc_raw(3);
    unsafe { store(gate_context, 0); store(gate_context + 1, 0); store(gate_context + 2, 0); }
    var sched: t40 = df_sched_new(2);
    var gate: t40 = df_node_new(sched, 0, DF_TEST_KIND_START_GATE, gate_context);
    var topo: t40 = df_vec_new_sized(1);
    df_vec_push_safe(topo, gate);
    var first_status: t40 = df_sched_run_static(sched, topo);
    unsafe {
        store(DF_TEST_REPORT + 124, first_status);
        store(DF_TEST_REPORT + 125, load(gate));
        store(DF_TEST_REPORT + 126, load(gate_context + 1));
        store(DF_TEST_REPORT + 127, load(gate_context + 2));
        store(gate_context, 1);
    }
    var second_status: t40 = df_sched_run_static(sched, topo);
    unsafe {
        store(DF_TEST_REPORT + 128, second_status);
        store(DF_TEST_REPORT + 129, load(gate));
        store(DF_TEST_REPORT + 130, load(gate_context + 1));
        store(DF_TEST_REPORT + 131, load(gate_context + 2));
        store(DF_TEST_REPORT + 150, df_sched_lifecycle(sched));
    }
    df_sched_free(sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var duplicate_count: t40 = malloc_raw(1);
    unsafe { store(duplicate_count, 0); }
    var duplicate_sched: t40 = df_sched_new(2);
    var duplicate_node: t40 = df_node_new(duplicate_sched, 0, DF_TEST_KIND_PROBE, duplicate_count);
    var duplicate_topo: t40 = df_vec_new_sized(2);
    df_vec_push_safe(duplicate_topo, duplicate_node);
    df_vec_push_safe(duplicate_topo, duplicate_node);
    var duplicate_status: t40 = df_sched_run_static(duplicate_sched, duplicate_topo);
    unsafe {
        store(DF_TEST_REPORT + 132, duplicate_status);
        store(DF_TEST_REPORT + 133, load(duplicate_count));
    }
    df_sched_free(duplicate_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var owner_count: t40 = malloc_raw(1);
    var foreign_count: t40 = malloc_raw(1);
    unsafe { store(owner_count, 0); store(foreign_count, 0); }
    var owner_sched: t40 = df_sched_new(2);
    var foreign_sched: t40 = df_sched_new(2);
    var owner_node: t40 = df_node_new(owner_sched, 0, DF_TEST_KIND_PROBE, owner_count);
    var foreign_node: t40 = df_node_new(foreign_sched, 0, DF_TEST_KIND_PROBE, foreign_count);
    var foreign_topo: t40 = df_vec_new_sized(1);
    df_vec_push_safe(foreign_topo, foreign_node);
    var foreign_status: t40 = df_sched_run_static(owner_sched, foreign_topo);
    unsafe {
        store(DF_TEST_REPORT + 134, foreign_status);
        store(DF_TEST_REPORT + 135, load(owner_count));
        store(DF_TEST_REPORT + 136, load(foreign_count));
        store(DF_TEST_REPORT + 137, load(owner_node));
    }
    df_sched_free(owner_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var loop_count: t40 = malloc_raw(1);
    unsafe { store(loop_count, 0); }
    var loop_sched: t40 = df_sched_new(4);
    var loop_blocker: t40 = df_node_new(loop_sched, 0, DF_TEST_KIND_START_BLOCKED, 0);
    var loop_normal: t40 = df_node_new(loop_sched, 0, DF_TEST_KIND_START_DONE, loop_count);
    df_sched_submit_priority(loop_sched, loop_blocker);
    df_sched_submit(loop_sched, loop_normal);
    var loop_status: t40 = df_sched_run_loop(loop_sched);
    unsafe {
        store(DF_TEST_REPORT + 138, loop_status);
        store(DF_TEST_REPORT + 139, load(loop_count));
        store(DF_TEST_REPORT + 140, load(loop_blocker));
        store(DF_TEST_REPORT + 141, load(loop_normal));
        store(DF_TEST_REPORT + 142, df_sched_lifecycle(loop_sched));
    }
    df_sched_free(loop_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var first_parent_count: t40 = malloc_raw(1);
    var second_parent_context: t40 = malloc_raw(3);
    var or_join_count: t40 = malloc_raw(1);
    unsafe {
        store(first_parent_count, 0);
        store(second_parent_context, 0); store(second_parent_context + 1, 0);
        store(second_parent_context + 2, 0); store(or_join_count, 0);
    }
    var or_sched: t40 = df_sched_new(4);
    var first_parent: t40 = df_node_new(or_sched, 0, DF_TEST_KIND_START_DONE, first_parent_count);
    var second_parent: t40 = df_node_new(or_sched, 0, DF_TEST_KIND_START_GATE, second_parent_context);
    var or_join: t40 = df_node_new(or_sched, 0, DF_TEST_KIND_START_DONE, or_join_count);
    df_node_add_dependent(first_parent, or_join);
    df_node_add_dependent(second_parent, or_join);
    df_node_set_required_threshold(or_join, 1);
    df_sched_submit(or_sched, first_parent);
    df_sched_submit(or_sched, second_parent);
    df_sched_run_budget(or_sched, 2);
    df_sched_run_budget(or_sched, 1);
    unsafe {
        store(DF_TEST_REPORT + 143, load(or_join_count));
        store(DF_TEST_REPORT + 144, load(second_parent_context + 1));
        store(DF_TEST_REPORT + 145, load(second_parent_context + 2));
        store(second_parent_context, 1);
    }
    var or_settle_ops: t40 = df_sched_run_budget(or_sched, 8);
    unsafe {
        store(DF_TEST_REPORT + 146, or_settle_ops);
        store(DF_TEST_REPORT + 147, load(or_join_count));
        store(DF_TEST_REPORT + 148, load(second_parent_context + 2));
        store(DF_TEST_REPORT + 149, df_sched_lifecycle(or_sched));
    }
    df_sched_free(or_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var mixed_count: t40 = malloc_raw(1);
    unsafe { store(mixed_count, 0); }
    var mixed_sched: t40 = df_sched_new(2);
    var mixed_node: t40 = df_node_new(mixed_sched, 0, DF_TEST_KIND_START_DONE, mixed_count);
    var mixed_topo: t40 = df_vec_new_sized(1);
    df_vec_push_safe(mixed_topo, mixed_node);
    df_sched_submit(mixed_sched, mixed_node);
    var mixed_static_status: t40 = df_sched_run_static(mixed_sched, mixed_topo);
    unsafe {
        store(DF_TEST_REPORT + 151, mixed_static_status);
        store(DF_TEST_REPORT + 152, load(mixed_count));
    }
    df_sched_free(mixed_sched);

    if df_arena_init(DF_TEST_ARENA_BASE, DF_TEST_ARENA_WORDS) == 0 { return; }
    var error_count: t40 = malloc_raw(1);
    unsafe { store(error_count, 0); }
    var error_sched: t40 = df_sched_new(2);
    var error_node: t40 = df_node_new(error_sched, 0, DF_TEST_KIND_COUNT_ERROR, error_count);
    var error_topo: t40 = df_vec_new_sized(1);
    df_vec_push_safe(error_topo, error_node);
    df_sched_submit(error_sched, error_node);
    var first_error_status: t40 = df_sched_run_loop(error_sched);
    var repeated_loop_error: t40 = df_sched_run_loop(error_sched);
    var static_after_error: t40 = df_sched_run_static(error_sched, error_topo);
    unsafe {
        store(DF_TEST_REPORT + 153, first_error_status);
        store(DF_TEST_REPORT + 154, repeated_loop_error);
        store(DF_TEST_REPORT + 155, static_after_error);
        store(DF_TEST_REPORT + 156, load(error_count));
        store(DF_TEST_REPORT + 157, df_sched_lifecycle(error_sched));
    }
    df_sched_free(error_sched);
}

fn main() -> t40 {
    unsafe { store(DF_TEST_REPORT, 516); }
    df_test_run_copy(1, DF_TEST_OUT_CAP1, 1);
    df_test_run_copy(2, DF_TEST_OUT_CAP2, 16);
    df_test_run_budget_probe();
    df_test_run_fairness();
    df_test_run_cancel();
    df_test_run_validation();
    df_test_run_fatal_sink();
    df_test_run_startup_dag();
    df_test_run_builtin_anomaly();
    df_test_run_builtin_window();
    df_test_run_builtin_sink_controls();
    df_test_run_static();
    return 0;
}
)TRIT";

    const std::vector<std::pair<std::string, std::filesystem::path>> inputs = {
        {"architecture_contract.trit", "architecture_contract.trit"},
        {"os_sdk.trit", "apps/os_sdk.trit"},
        {"service_runtime.trit", "apps/service_runtime.trit"},
        {"ternary_dataflow.trit", "ternary_dataflow.trit"},
        {"ternary_stream.trit", "ternary_stream.trit"},
    };
    std::string guest_source;
    std::ostringstream hashes;
    hashes << "{\n  \"algorithm\": \"FNV-1a-64\",\n  \"sources\": {\n";
    bool first_hash = true;
    for (const auto& [name, relative] : inputs) {
        const std::string contents = readTextFile(relative);
        if (contents.empty()) {
            std::ofstream(stream_artifact / "phase1_report.json")
                << "{\"status\":\"fail\",\"stage\":\"source-load\",\"missing\":\""
                << relative.generic_string() << "\"}\n";
            return require(false, "stream workflow cannot read " +
                                      relative.generic_string());
        }
        if (!first_hash) hashes << ",\n";
        first_hash = false;
        hashes << "  \"" << name << "\": \"" << stableHash(contents) << "\"";
        guest_source += "\n// source: " + name + "\n" + contents + "\n";
    }
    hashes << ",\n    \"embedded_guest.trit\": \"" << stableHash(guest)
           << "\"\n  }\n}\n";
    guest_source += "\n// embedded guest scenario\n" + guest;
    {
        std::ofstream guest_file(stream_artifact / "embedded_guest.trit",
                                 std::ios::binary);
        guest_file << guest;
        std::ofstream hash_file(stream_artifact / "source_hashes.json",
                                std::ios::binary);
        hash_file << hashes.str();
    }

    compiler::CompilerOptions compiler_options;
    compiler_options.target_abi_version = architecture::v3::FUNCTION_ABI_VERSION;
    compiler_options.target_executable_version =
        architecture::v3::EXECUTABLE_VERSION;
    compiler_options.enable_vector_abi = true;
    compiler_options.enable_vector_spilling = true;
    compiler::CompileResult compiled = compiler::compileSource(
        "service_workflow_guest.trit", guest_source, compiler_options);
    if (!compiled.success) {
        std::ofstream report(stream_artifact / "phase1_report.json");
        report << "{\"status\":\"fail\",\"stage\":\"compile\",\"diagnostics\":[";
        for (std::size_t i = 0; i < compiled.diagnostics.size(); ++i) {
            if (i) report << ',';
            report << '"' << jsonEscape(compiled.diagnostics[i].format()) << '"';
        }
        report << "]}\n";
        return require(false, "stream guest did not compile; see " +
                                  (stream_artifact / "phase1_report.json").string());
    }
    compiler::LinkOptions link_options;
    link_options.standalone_halt_on_exit = true;
    link_options.dead_strip_functions = true;
    link_options.function_abi_version = architecture::v3::FUNCTION_ABI_VERSION;
    link_options.executable_version = architecture::v3::EXECUTABLE_VERSION;
    link_options.enable_vector_abi = true;
    link_options.enable_vector_spilling = true;
    link_options.vector_abi_version = architecture::v3::VECTOR_ABI_VERSION;
    link_options.stack_hint_words = 2048;
    compiler::LinkResult linked = compiler::linkModules(
        {compiled.object}, link_options);
    if (!linked.success) {
        std::ofstream report(stream_artifact / "phase1_report.json");
        report << "{\"status\":\"fail\",\"stage\":\"link\",\"diagnostics\":[";
        for (std::size_t i = 0; i < linked.diagnostics.size(); ++i) {
            if (i) report << ',';
            report << '"' << jsonEscape(linked.diagnostics[i].format()) << '"';
        }
        report << "]}\n";
        return require(false, "stream guest did not link; see " +
                                  (stream_artifact / "phase1_report.json").string());
    }
    {
        std::ofstream assembly(stream_artifact / "guest.linked.tasm",
                               std::ios::binary);
        assembly << linked.assembly;
    }

    vm::VMState machine(32768, 80000);
    if (!vm::assembler::loadAndReset(machine, linked.assembled)) {
        return require(false, "stream guest executable did not load into VM");
    }
    const vm::RunResult run = vm::run(machine, 100'000'000);
    bool ok = require(run.status == vm::VMStatus::HALTED,
                      "stream guest VM did not halt: " + run.description);
    auto read_word = [&](int address, long long& value) {
        const auto loaded = machine.dmem.load(address);
        if (loaded.second != vm::MemFaultCode::OK) return false;
        value = vm::ops::toLong(loaded.first);
        return true;
    };
    auto read_double = [&](int address, double& value) {
        const auto loaded = machine.dmem.load(address);
        if (loaded.second != vm::MemFaultCode::OK) return false;
        value = sandbox::ops::toDouble(loaded.first.asTriple());
        return std::isfinite(value);
    };
    auto read_report = [&](int offset, long long& value) {
        return read_word(2500 + offset, value);
    };
    const long long maximum_event_records = 256;
    bool event_read_bounds_ok = true;
    auto read_events = [&](int base, long long count, long long max_count) {
        std::vector<std::pair<long long, long long>> events;
        if (count < 0 || count > max_count || count > maximum_event_records) {
            event_read_bounds_ok = false;
            return events;
        }
        for (long long i = 0; i < count; ++i) {
            long long tag = 0;
            long long value = 0;
            if (!read_word(base + static_cast<int>(i * 2), tag) ||
                !read_word(base + static_cast<int>(i * 2 + 1), value)) {
                break;
            }
            events.push_back({tag, value});
        }
        return events;
    };

    long long magic = 0;
    ok &= require(read_report(0, magic) && magic == 516,
                  "guest did not publish stream workflow report marker");
    std::ostringstream event_json;
    event_json << "{\n  \"guest_vm\": \""
               << vm::vmStatusToString(run.status) << "\",\n  \"steps\": "
               << run.steps << ",\n  \"capacities\": [";
    const int report_offsets[] = {1, 16};
    const int output_bases[] = {5000, 5600};
    const long long expected_copy_count = 256;
    for (int scenario = 0; scenario < 2; ++scenario) {
        long long count = 0, eos = 0, stalls = 0, max_ops = 0;
        long long source_next = 0, lifecycle = 0, highwater = 0;
        long long reset_used = 0, used_before_callbacks = 0, used_after_callbacks = 0;
        const int offset = report_offsets[scenario];
        ok &= require(read_report(offset, count) && count == 256,
                      "guest sequence count differs from 256 for channel capacity " +
                          std::to_string(scenario + 1));
        read_report(offset + 1, eos);
        read_report(offset + 2, stalls);
        read_report(offset + 3, max_ops);
        read_report(offset + 4, source_next);
        read_report(offset + 5, lifecycle);
        read_report(offset + 6, highwater);
        read_report(offset + 7, used_before_callbacks);
        read_report(offset + 8, used_after_callbacks);
        read_report(offset + 9, reset_used);
        ok &= require(eos == 1, "guest sequence did not deliver one EOS");
        ok &= require(lifecycle == 2,
                      "completed copy graph lifecycle is not DONE");
        ok &= require(stalls >= 512,
                      "sink did not apply repeated stalls for each event");
        ok &= require(max_ops <= 32,
                      "scheduler exceeded the 32-operation dispatch limit");
        ok &= require(source_next == 256,
                      "source did not commit exactly 256 accepted events");
        ok &= require(highwater > 0 && highwater <= 40000 && reset_used == 0,
                      "bounded arena high-water/reset evidence is invalid");
        ok &= require(used_before_callbacks == used_after_callbacks,
                      "arena used-word count changed during callback processing");
        const auto events = read_events(output_bases[scenario], count,
                                        expected_copy_count);
        ok &= require(event_read_bounds_ok,
                      "guest event count exceeded its bounded output record area");
        ok &= require(events.size() == static_cast<std::size_t>(expected_copy_count),
                      "external guest record read lost event records");
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].first != 1 ||
                events[i].second != static_cast<long long>(i + 1)) {
                ok &= require(false,
                              "sequence order/tag mismatch at index " +
                                  std::to_string(i));
                break;
            }
        }
        if (scenario) event_json << ',';
        event_json << "\n    {\"channel_capacity\": " << scenario + 1
                   << ", \"accepted\": " << count
                   << ", \"eos\": " << eos
                   << ", \"sink_stalls\": " << stalls
                   << ", \"max_callback_ops\": " << max_ops
                   << ", \"arena_used_before_callbacks\": " << used_before_callbacks
                   << ", \"arena_used_after_callbacks\": " << used_after_callbacks
                   << ", \"arena_highwater_words\": " << highwater
                   << ", \"arena_used_after_reset\": " << reset_used
                   << ", \"events\": [";
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (i) event_json << ',';
            event_json << '[' << events[i].first << ',' << events[i].second << ']';
        }
        event_json << "]}";
    }
    long long fairness_ops = 0, unrelated_count = 0, blocked_count = 0;
    long long budget_ops = 0, probe_calls = 0;
    read_report(30, budget_ops);
    read_report(31, probe_calls);
    read_report(32, fairness_ops);
    read_report(33, unrelated_count);
    read_report(34, blocked_count);
    long long live_stream_calls = 0, startup_calls_after_budget = 0;
    long long fairness_last_attempts = 0;
    read_report(35, live_stream_calls);
    read_report(36, startup_calls_after_budget);
    read_report(37, fairness_last_attempts);
    ok &= require(fairness_ops <= 32 && unrelated_count == 1 && blocked_count == 0,
                  "unrelated registered stream did not progress beside blocked pipeline");
    ok &= require(live_stream_calls == 1 && startup_calls_after_budget == 1 &&
                      fairness_last_attempts == 1,
                  "repeated one-operation budgets starved one-shot startup work");
    ok &= require(budget_ops == 32 && probe_calls == 32,
                  "scheduler budget did not clamp 100 requests to 32 operations");
    long long cancel_count = 0, cancel_eos = 0, cancel_lifecycle = 0;
    long long cancel_source_next = 0, cancel_stalls = 0, cancel_accepted = 0;
    read_report(40, cancel_count);
    read_report(41, cancel_eos);
    read_report(42, cancel_lifecycle);
    read_report(43, cancel_source_next);
    read_report(44, cancel_stalls);
    read_report(45, cancel_accepted);
    ok &= require(cancel_count == cancel_accepted && cancel_eos == 1 &&
                      cancel_source_next == cancel_accepted && cancel_lifecycle == 2,
                  "graceful cancellation did not drain accepted data then EOS");

    long long duplicate_primitive = 0, duplicate_scheduler = 0;
    long long duplicate_ops = 0, duplicate_calls = 0, cycle_result = 0;
    long long invalid_result = 0, invalid_ops = 0, invalid_calls = 0;
    long long overflow_node = 0, exhaustion_result = 0, exhaustion_ops = 0;
    long long exhaustion_calls = 0, blocker_state = 0, dependent_state = 0;
    long long startup_calls = 0;
    read_report(70, duplicate_primitive); read_report(71, duplicate_scheduler);
    read_report(72, duplicate_ops); read_report(73, duplicate_calls);
    read_report(74, cycle_result); read_report(75, invalid_result);
    read_report(76, invalid_ops); read_report(77, invalid_calls);
    read_report(78, overflow_node); read_report(79, exhaustion_result);
    read_report(80, exhaustion_ops); read_report(81, exhaustion_calls);
    read_report(82, blocker_state); read_report(83, dependent_state);
    read_report(84, startup_calls);
    ok &= require(duplicate_primitive < 0 && duplicate_scheduler < 0 &&
                      duplicate_calls == 0,
                  "duplicate dependency edge was not rejected before callbacks");
    ok &= require(cycle_result < 0,
                  "cycle was not rejected by the graph validator");
    ok &= require(invalid_result < 0 && invalid_calls == 0,
                  "unknown service kind was not rejected before callbacks");
    ok &= require(overflow_node == 0 && exhaustion_result < 0 &&
                      exhaustion_calls == 0,
                  "node-pool exhaustion did not invalidate the graph before callbacks");
    ok &= require(blocker_state != 1 && dependent_state < 0 && startup_calls == 0,
                  "blocked startup node incorrectly completed or woke its dependent");
    long long dag_parent_a = 0, dag_parent_b = 0, dag_join = 0;
    long long dag_join_state = 0, dag_status = 0;
    long long progress_state = 0, progress_dependent_state = 0;
    long long progress_dependent_calls = 0;
    long long dag_join_while_parent_blocked = 0;
    long long dag_parent_b_attempts = 0, dag_parent_b_completions = 0;
    read_report(85, dag_parent_a); read_report(86, dag_parent_b);
    read_report(87, dag_join); read_report(88, dag_join_state);
    read_report(89, dag_status); read_report(94, progress_state);
    read_report(95, progress_dependent_state);
    read_report(96, progress_dependent_calls);
    read_report(109, dag_join_while_parent_blocked);
    read_report(110, dag_parent_b_attempts);
    read_report(111, dag_parent_b_completions);
    ok &= require(dag_parent_a == 1 && dag_parent_b == 1 && dag_join == 1 &&
                      dag_join_state == 1 && dag_status == 0 &&
                      dag_join_while_parent_blocked == 0 &&
                      dag_parent_b_attempts == 2 && dag_parent_b_completions == 1,
                  "startup AND-join ran before both predecessors completed or resumed more than once");
    ok &= require(progress_state != 1 && progress_dependent_state < 0 &&
                      progress_dependent_calls == 0,
                  "startup PROGRESS incorrectly released a dependency");
    long long first_admission = 0, second_admission = 0;
    long long duplicate_submit_validation = 0, duplicate_submit_ops = 0;
    long long duplicate_submit_callbacks = 0, duplicate_submit_lifecycle = 0;
    long long fractional_node = 0, fractional_validation = 0;
    double fractional_kind_value = 0.0;
    read_report(101, first_admission); read_report(102, second_admission);
    read_report(103, duplicate_submit_validation); read_report(104, duplicate_submit_ops);
    read_report(105, duplicate_submit_callbacks); read_report(106, duplicate_submit_lifecycle);
    read_report(107, fractional_node); read_report(108, fractional_validation);
    const bool fractional_value_read = read_double(2500 + 112, fractional_kind_value);
    ok &= require(first_admission == 1 && second_admission == 0 &&
                      duplicate_submit_validation < 0 && duplicate_submit_ops == 0 &&
                      duplicate_submit_callbacks == 0 && duplicate_submit_lifecycle == -1,
                  "duplicate one-shot admission was not rejected before callbacks");
    ok &= require(fractional_value_read && std::abs(fractional_kind_value - 1.5) < 1e-8 &&
                      fractional_node == 0 && fractional_validation < 0,
                  "fractional builtin kind passed graph construction or validation");

    long long first_priority_admission = 0, second_priority_admission = 0;
    long long priority_validation = 0, priority_ops = 0, priority_callbacks = 0;
    long long priority_lifecycle = 0;
    read_report(113, first_priority_admission); read_report(114, second_priority_admission);
    read_report(115, priority_validation); read_report(116, priority_ops);
    read_report(117, priority_callbacks); read_report(118, priority_lifecycle);
    ok &= require(first_priority_admission == 1 && second_priority_admission == 0 &&
                      priority_validation < 0 && priority_ops == 0 &&
                      priority_callbacks == 0 && priority_lifecycle == -1,
                  "duplicate priority admission was not rejected before callbacks");
    long long priority_fair_attempts = 0, priority_normal_calls = 0;
    long long priority_blocker_state = 0, priority_normal_state = 0;
    long long priority_fair_lifecycle = 0;
    read_report(119, priority_fair_attempts); read_report(120, priority_normal_calls);
    read_report(121, priority_blocker_state); read_report(122, priority_normal_state);
    read_report(123, priority_fair_lifecycle);
    ok &= require(priority_fair_attempts > 0 && priority_normal_calls == 1 &&
                      priority_blocker_state == 0 && priority_normal_state == 1 &&
                      priority_fair_lifecycle == 1,
                  "permanently blocked priority node starved ready normal startup work");

    long long static_first_status = 0, static_first_state = 0;
    long long static_first_attempts = 0, static_first_completions = 0;
    long long static_second_status = 0, static_second_state = 0;
    long long static_second_attempts = 0, static_second_completions = 0;
    read_report(124, static_first_status); read_report(125, static_first_state);
    read_report(126, static_first_attempts); read_report(127, static_first_completions);
    read_report(128, static_second_status); read_report(129, static_second_state);
    read_report(130, static_second_attempts); read_report(131, static_second_completions);
    long long static_lifecycle = 0;
    read_report(150, static_lifecycle);
    ok &= require(static_first_status >= 0 && static_first_state == 0 &&
                      static_first_attempts == 1 && static_first_completions == 0 &&
                      static_second_status >= 0 && static_second_state == 1 &&
                      static_second_attempts == 2 && static_second_completions == 1 &&
                      static_lifecycle == 2,
                  "static runner marked a blocked node DONE or failed to resume it once");
    long long static_duplicate_status = 0, static_duplicate_calls = 0;
    long long static_foreign_status = 0, static_owner_calls = 0, static_foreign_calls = 0;
    read_report(132, static_duplicate_status); read_report(133, static_duplicate_calls);
    read_report(134, static_foreign_status); read_report(135, static_owner_calls);
    read_report(136, static_foreign_calls);
    ok &= require(static_duplicate_status < 0 && static_duplicate_calls == 0 &&
                      static_foreign_status < 0 && static_owner_calls == 0 &&
                      static_foreign_calls == 0,
                  "static runner executed an invalid duplicate or foreign topo node before rejecting it");
    long long loop_status = 0, loop_normal_calls = 0, loop_blocker_state = 0;
    long long loop_normal_state = 0, loop_lifecycle = 0;
    read_report(138, loop_status); read_report(139, loop_normal_calls);
    read_report(140, loop_blocker_state); read_report(141, loop_normal_state);
    read_report(142, loop_lifecycle);
    ok &= require(loop_status >= 0 && loop_normal_calls == 1 &&
                      loop_blocker_state == 0 && loop_normal_state == 1 &&
                      loop_lifecycle == 1,
                  "legacy run loop let a blocked priority node starve normal work");
    long long or_join_calls_before_late_parent = 0;
    long long late_parent_attempts_before_open = 0, late_parent_completions_before_open = 0;
    long long or_settle_ops = 0, or_join_calls_after_late_parent = 0;
    long long late_parent_completions = 0, or_lifecycle = 0;
    read_report(143, or_join_calls_before_late_parent);
    read_report(144, late_parent_attempts_before_open);
    read_report(145, late_parent_completions_before_open);
    read_report(146, or_settle_ops); read_report(147, or_join_calls_after_late_parent);
    read_report(148, late_parent_completions); read_report(149, or_lifecycle);
    ok &= require(or_join_calls_before_late_parent == 1 &&
                      late_parent_attempts_before_open == 1 &&
                      late_parent_completions_before_open == 0 && or_settle_ops > 0 &&
                      or_join_calls_after_late_parent == 1 && late_parent_completions == 1 &&
                      or_lifecycle == 2,
                  "OR threshold join was re-enqueued or errored when a later parent completed");
    long long mixed_static_status = 0, mixed_static_calls = 0;
    read_report(151, mixed_static_status); read_report(152, mixed_static_calls);
    ok &= require(mixed_static_status < 0 && mixed_static_calls == 0,
                  "static runner executed a node already admitted to the scheduler queue");
    long long first_error_status = 0, repeated_loop_error = 0, static_after_error = 0;
    long long terminal_error_calls = 0, terminal_error_lifecycle = 0;
    read_report(153, first_error_status); read_report(154, repeated_loop_error);
    read_report(155, static_after_error); read_report(156, terminal_error_calls);
    read_report(157, terminal_error_lifecycle);
    ok &= require(first_error_status < 0 && repeated_loop_error < 0 &&
                      static_after_error < 0 && terminal_error_calls == 1 &&
                      terminal_error_lifecycle == -1,
                  "terminal scheduler ERROR allowed a later runner callback or lost its error result");

    long long fatal_attempts = 0, fatal_lifecycle = 0;
    long long fatal_uncommitted = 0, fatal_source_next = 0;
    read_report(90, fatal_attempts); read_report(91, fatal_lifecycle);
    read_report(92, fatal_uncommitted); read_report(93, fatal_source_next);
    ok &= require(fatal_lifecycle == -1 && fatal_uncommitted == 1 &&
                      fatal_source_next == 1,
                  "fatal sink did not preserve exact one-event uncommitted count");

    long long anomaly_count = 0, anomaly_eos = 0, anomaly_stalls = 0;
    long long anomaly_lifecycle = 0, history_count = 0, candidate_count = 0;
    read_report(50, anomaly_count); read_report(51, anomaly_eos);
    read_report(52, anomaly_stalls); read_report(53, anomaly_lifecycle);
    read_report(54, history_count); read_report(55, candidate_count);
    const auto anomaly_events = read_events(7400, anomaly_count, 8);
    const std::vector<std::pair<long long, long long>> expected_anomaly_events = {
        {1, 12}, {-2, 0}, {1, 15}, {-1, 0}, {-1, 0}};
    ok &= require(anomaly_events == expected_anomaly_events && anomaly_eos == 1 &&
                      anomaly_lifecycle == 2 && anomaly_stalls >= 10,
                  "built-in anomaly path lost DATA/UNCERTAIN/ANOMALY/EOS events under pressure");
    ok &= require(history_count == 3 && candidate_count == 1,
                  "anomaly threshold equality or candidate history committed incorrectly");

    long long window_count = 0, window_eos = 0, window_stalls = 0;
    long long window_lifecycle = 0, window_history = 0, partial_count = 0;
    read_report(60, window_count); read_report(61, window_eos);
    read_report(62, window_stalls); read_report(63, window_lifecycle);
    read_report(64, window_history); read_report(65, partial_count);
    const auto window_events = read_events(8000, window_count, 8);
    double full_average = 0.0, partial_average = 0.0;
    const bool full_value_read = read_double(8001, full_average);
    const bool partial_value_read = read_double(8003, partial_average);
    ok &= require(window_count == 2 && window_events.size() == 2 &&
                      window_events[0].first == 1 && window_events[1].first == 1 &&
                      full_value_read && std::abs(full_average - 17.5) < 1e-8 &&
                      partial_value_read && std::abs(partial_average - 32.5) < 1e-8,
                  "full or partial built-in window aggregate was lost or changed");
    ok &= require(window_eos == 1 && window_lifecycle == 2 &&
                      window_stalls >= 6 && window_history == 3 && partial_count == 0,
                  "window/filter path did not deliver aggregates before terminal EOS");
    long long window_source_samples = 0;
    read_report(67, window_source_samples);
    ok &= require(window_source_samples == 6,
                  "window guest did not accept all six source samples");

    long long controls_lifecycle = 0, controls_source_step = 0;
    long long controls_sink_state = 0, controls_rounds = 0;
    read_report(97, controls_lifecycle); read_report(98, controls_source_step);
    read_report(99, controls_sink_state); read_report(100, controls_rounds);
    ok &= require(controls_lifecycle == 2 && controls_source_step == 6 &&
                      controls_sink_state == 1 && controls_rounds > 0,
                  "built-in sink treated a negative control event or negative DATA payload as terminal");

    event_json << "\n  ],\n  \"fairness\": {\"attempts\": " << fairness_ops
               << ", \"unrelated_sink_records\": " << unrelated_count
               << ", \"permanently_blocked_sink_records\": " << blocked_count
               << ", \"one_op_stream_calls\": " << live_stream_calls
               << ", \"one_op_startup_calls\": " << startup_calls_after_budget
               << "},\n  \"budget\": {\"requested\": 100, \"attempted\": "
               << budget_ops << ", \"probe_callbacks\": " << probe_calls
               << "},\n  \"cancellation\": {\"records\": " << cancel_count
               << ", \"eos\": " << cancel_eos
               << ", \"lifecycle\": " << cancel_lifecycle
               << ", \"source_sequence_committed\": " << cancel_source_next
               << ", \"accepted_at_cancel\": " << cancel_accepted
               << ", \"stall_count\": " << cancel_stalls
               << "},\n  \"validation\": {\"duplicate_primitive\": "
               << duplicate_primitive << ", \"duplicate_scheduler\": "
               << duplicate_scheduler << ", \"duplicate_callback_count\": "
               << duplicate_calls << ", \"cycle\": " << cycle_result
               << ", \"invalid_kind\": " << invalid_result
               << ", \"pool_overflow_node\": " << overflow_node
               << ", \"pool_overflow_validation\": " << exhaustion_result
               << ", \"startup_blocker_state\": " << blocker_state
               << ", \"startup_dependent_state\": " << dependent_state
               << ", \"duplicate_submit_validation\": " << duplicate_submit_validation
               << ", \"duplicate_submit_callbacks\": " << duplicate_submit_callbacks
               << ", \"fractional_node\": " << fractional_node
               << ", \"fractional_kind_validation\": " << fractional_validation
               << ", \"fractional_kind_value\": " << fractional_kind_value
               << ", \"duplicate_priority_first\": " << first_priority_admission
               << ", \"duplicate_priority_second\": " << second_priority_admission
               << ", \"priority_validation\": " << priority_validation
               << ", \"priority_callbacks\": " << priority_callbacks
               << ", \"priority_fair_attempts\": " << priority_fair_attempts
               << ", \"priority_normal_calls\": " << priority_normal_calls
               << ", \"priority_blocker_state\": " << priority_blocker_state
               << ", \"static_first_state\": " << static_first_state
               << ", \"static_first_attempts\": " << static_first_attempts
               << ", \"static_first_completions\": " << static_first_completions
               << ", \"static_second_state\": " << static_second_state
               << ", \"static_second_attempts\": " << static_second_attempts
               << ", \"static_second_completions\": " << static_second_completions
               << ", \"static_duplicate_calls\": " << static_duplicate_calls
               << ", \"static_foreign_calls\": " << static_foreign_calls
               << ", \"static_lifecycle\": " << static_lifecycle
               << ", \"run_loop_normal_calls\": " << loop_normal_calls
               << ", \"run_loop_blocker_state\": " << loop_blocker_state
               << ", \"or_join_calls_before_late_parent\": " << or_join_calls_before_late_parent
               << ", \"or_join_calls_after_late_parent\": " << or_join_calls_after_late_parent
               << ", \"late_parent_completions\": " << late_parent_completions
               << ", \"or_lifecycle\": " << or_lifecycle
               << ", \"mixed_static_status\": " << mixed_static_status
               << ", \"mixed_static_calls\": " << mixed_static_calls
               << ", \"terminal_first_status\": " << first_error_status
               << ", \"terminal_repeat_loop_status\": " << repeated_loop_error
               << ", \"terminal_static_status\": " << static_after_error
               << ", \"terminal_error_calls\": " << terminal_error_calls
               << "},\n  \"fatal_sink\": {\"attempts\": " << fatal_attempts
               << ", \"lifecycle\": " << fatal_lifecycle
               << ", \"uncommitted_events\": " << fatal_uncommitted
               << ", \"source_accepted\": " << fatal_source_next
               << "},\n  \"builtin_anomaly\": {\"events\": [";
    for (std::size_t i = 0; i < anomaly_events.size(); ++i) {
        if (i) event_json << ',';
        event_json << '[' << anomaly_events[i].first << ',' << anomaly_events[i].second << ']';
    }
    event_json << "], \"eos\": " << anomaly_eos << ", \"history_count\": "
               << history_count << ", \"candidate_count\": " << candidate_count
               << "},\n  \"builtin_window_filter\": {\"events\": ["
               << "[1," << full_average << "],[1," << partial_average
               << "]], \"eos\": " << window_eos << ", \"history_count\": "
               << window_history << ", \"source_samples\": " << window_source_samples
               << ", \"partial_count_after_eos\": " << partial_count
               << "},\n  \"startup_dag\": {\"parent_a_calls\": " << dag_parent_a
               << ", \"parent_b_calls\": " << dag_parent_b
               << ", \"join_calls\": " << dag_join
               << ", \"join_state\": " << dag_join_state
               << ", \"join_calls_while_blocked\": " << dag_join_while_parent_blocked
               << ", \"blocked_parent_attempts\": " << dag_parent_b_attempts
               << ", \"progress_dependent_calls\": " << progress_dependent_calls
               << "},\n  \"builtin_sink_controls\": {\"source_step\": "
               << controls_source_step << ", \"sink_state\": " << controls_sink_state
               << ", \"lifecycle\": " << controls_lifecycle
               << "},\n  \"source_hash_algorithm\": \"FNV-1a-64\",\n"
               << "  \"source_hashes\": " << hashes.str()
               << ",\n  \"status\": \"" << (ok ? "pass" : "fail")
               << "\"\n}\n";
    {
        std::ofstream events(stream_artifact / "stream-events.json",
                             std::ios::binary);
        events << event_json.str();
        std::ofstream report(stream_artifact / "phase1_report.json",
                             std::ios::binary);
        report << "{\"status\":\"" << (ok ? "pass" : "fail")
               << "\",\"gate\":\"phase1-test-first\",\"guest_vm\":\""
               << vm::vmStatusToString(run.status) << "\",\"steps\":"
               << run.steps << "}\n";
    }
    if (!ok) return false;
    std::cout << "stream workflow guest conformance: capacities 1/2, 256 ordered events, budget, fairness, cancellation\n";
    std::cout << "stream workflow artifact: " << stream_artifact.string() << "\n";
    return true;
}


// SDK failure modes, specified before codegen repair: hidden negative-detail
// errors, positive-detail fallback, fd0/EOF collisions, and live-value clobber.
// WAL failure modes frozen before repair: reserve exhaustion, live/empty collisions,
// duplicate closure, snapshot/pin/newer frames, uncovered homes, corrupt chains,
// torn commits, premature tail publication, failed retry, and lost fatal errno.
bool runWalWorkflow(bool admission_only=false, bool lifecycle_only=false, bool shape_only=false) {
    using namespace sandbox;
    const auto wal_base=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact"/"wal";
    const auto base=shape_only?wal_base/"checkpoint_shapes":lifecycle_only?wal_base/"extent_lifecycle":admission_only?wal_base/"admission_low":wal_base;
    std::filesystem::create_directories(base);
    std::vector<std::string> failures;
    auto check=[&](bool pass,const std::string& label) { if(!pass) { failures.push_back(label); require(false,"WAL: "+label); } return pass; };
    auto word=[](const vm::VMState& m,int a){return host::detail::dmemWord(m,a);};
    auto userword=[&](const vm::VMState& m,int a) {
        const auto ctx=word(m,390000+9+5),pt=word(m,static_cast<int>(ctx)+4);
        const auto pte=word(m,static_cast<int>(pt)+a/vm::MMU_PAGE_WORDS);
        return word(m,static_cast<int>(pte/19683*vm::MMU_PAGE_WORDS)+a%vm::MMU_PAGE_WORDS);
    };
    const std::string contract=readTextFile("architecture_contract.trit"), production=readTextFile("kernel.trit");
    std::ofstream hashes(base/"source_hashes.json"); hashes<<"{\"algorithm\":\"FNV-1a-64\",\"sources\":{";
    const std::vector<std::string> inputs={"kernel.trit","apps/service_host.trit","apps/os_sdk.trit","architecture_contract.trit","native_kernel_trap_stub.tasm","tests/current_only_conformance.cpp","ternary_vm.h","ternary_vm_state.h"};
    for(size_t i=0;i<inputs.size();++i) { if(i) hashes<<','; hashes<<'"'<<inputs[i]<<"\":\""<<stableHash(readTextFile(inputs[i]))<<'"'; } hashes<<"}}\n"; hashes.close();
    auto diagnostic=[&](const auto& result,const auto& path) { std::ofstream out(path); for(const auto& d:result.diagnostics) out<<d.format()<<'\n'; };
    struct Run { std::unique_ptr<vm::VMState> machine; host::TosBootImage image; std::filesystem::path dir,backing; int checkpoint_pc=-1; };
    // Supervisor fixtures call the actual compiled kernel. Primary persistence
    // and fresh recovery run installed SDK programs through normal app_spawn/ERET.
    const std::string readerGuest=R"TRIT(fn main() -> t40 {
        unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,119); store(10306,97); store(10307,108); store(10308,45); store(10309,97); store(10310,0); }
        var a: t40 = os_open(10300,0); unsafe { store(10309,98); } var b: t40 = os_open(10300,0);
        var n: t40 = 0; var got: t40 = 27;
        while got > 0 { got=os_read(a,11000+n,27); if got > 0 { n=n+got; } if n > 1620 { got=0; } }
        unsafe { store(10094,n); } n=0; got=27;
        while got > 0 { got=os_read(b,13000+n,27); if got > 0 { n=n+got; } if n > 1620 { got=0; } }
        unsafe { store(10095,n); store(10091,777); }
        while n < 999999 { n=n+1; } return 0;
    })TRIT";
    auto makeRun=[&](const std::string& name,const std::string& fixture,const std::string& guest,const std::filesystem::path& recovered,const std::string& recoveryReader=std::string())->Run {
        Run run; run.dir=base/name; run.backing=run.dir/"live.tdisk"; std::filesystem::create_directories(run.dir);
        std::ofstream(run.dir/"fixture.trit")<<fixture; std::ofstream(run.dir/"guest.trit")<<guest;
        const auto kernel=compiler::compileSource(name+"_kernel.trit",contract+"\n"+production+"\n"+fixture); diagnostic(kernel,run.dir/"kernel_compile.txt");
        if(!check(kernel.success,name+" actual kernel/fixture compiles")) return run;
        os::NativeVfsImageBuilder fs(32768); check(fs.installBaseLayout().ok(),name+" current native rootfs");
        if(!guest.empty()) {
            compiler::CompilerOptions opts; opts.enable_vector_abi=true; opts.enable_vector_spilling=true;
            const auto app=compiler::compileSource(name+"_guest.trit",contract+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+guest,opts); diagnostic(app,run.dir/"guest_compile.txt");
            if(!check(app.success,name+" current SDK guest compiles")) return run;
            compiler::LinkOptions lo; lo.stack_hint_words=2048; lo.dead_strip_functions=true;lo.standalone_halt_on_exit=false;lo.enable_vector_abi=true;lo.enable_vector_spilling=true;
            const auto linked=compiler::linkModules({app.object},lo);diagnostic(linked,run.dir/"guest_link.txt");
            if(!check(linked.success,name+" current guest links")) return run;
            std::ofstream(run.dir/"guest.linked.tasm")<<linked.assembly;
            if(!check(fs.addExecutableImage("/bin/wal",linked.assembled.program,linked.executable_header_v3,256).ok(),name+" installed guest")) return run;
            const auto readerApp=compiler::compileSource("wal_reader.trit",contract+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+(recoveryReader.empty()?readerGuest:recoveryReader),opts);
            diagnostic(readerApp,run.dir/"reader_compile.txt");if(!check(readerApp.success,name+" recovery reader compile")) return run;
            const auto readerLinked=compiler::linkModules({readerApp.object},lo);diagnostic(readerLinked,run.dir/"reader_link.txt");
            if(!check(readerLinked.success,name+" reader link") || !check(fs.addExecutableImage("/bin/wal-recovery",readerLinked.assembled.program,readerLinked.executable_header_v3,256).ok(),name+" original image recovery reader installed")) return run;
        }
        std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n mov r2, 10011\n store r13, r2, 0\n brn r13, wal_test_boot_failed\n brz r13, wal_test_boot_failed\n";
        if(!fixture.empty()) boot+=" call wal_test_fixture\n";
        if(guest.empty()) boot+=" halt\n";
        else {
            boot+=" mov r13, 0\n mov r14, 3\n call process_set_state\n mov r1, native_trap_entry\n csrw tvec, r1\n";
            const std::string path=recovered.empty()?"/bin/wal":"/bin/wal-recovery";
            for(size_t i=0;i<=path.size();++i) boot+=" mov r1, "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"\n mov r2, "+std::to_string(10020+i)+"\n store r1, r2, 0\n";
            boot+=" mov r13, 1\n mov r14, 10020\n mov r15, 1\n mov r16, 2\n call app_spawn\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
        }
        boot+="\nwal_test_boot_failed:\n halt\n";
        boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
        auto feature=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
        auto assemble=[&](int count){return vm::assembler::assembleV2(boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(count)+", 0, 27, "+std::to_string(feature)+", 2, 0\n");};
        auto assembled=assemble(1); feature=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);assembled=assemble(1);if(assembled.success) assembled=assemble(static_cast<int>(assembled.program.size()));
        std::ofstream(run.dir/"boot.tasm")<<boot; if(!check(assembled.success,name+" actual current trap/kernel assembly")) return run;
        run.checkpoint_pc=assembled.labels.at("log_checkpoint");
        host::TosImageManifest manifest;run.image=host::bootImageFromAssembly(assembled,manifest,fs.image()); std::string error;
        check(host::writeBootImageFile((run.dir/"boot.tboot").string(),run.image,&error),name+" current tboot "+error);
        run.machine=std::make_unique<vm::VMState>(host::profileForManifest(run.image.manifest));
        if(!check(host::loadBootImageIntoVm(*run.machine,run.image,"",&error),name+" real VM load "+error)) {run.machine.reset();return run;}
        if(recovered.empty()) check(run.machine->block_device.writeSnapshotFile(run.backing.string()),name+" initial durable fixture image");
        else std::filesystem::copy_file(recovered,run.backing,std::filesystem::copy_options::overwrite_existing);
        check(run.machine->block_device.attachBackingFile(run.backing.string()),name+" actual backing attach"); return run;
    };
    auto recordBoot=[&](Run& run) {
        if(!run.machine) return;
        const auto actual=word(*run.machine,10011);
        check(actual==1,run.dir.filename().string()+" actual kernel_init return1");
        std::ofstream(run.dir/"boot_result.json")<<"{\"actual_kernel_init_return\":"<<actual<<",\"pc\":"<<run.machine->pc<<",\"status\":"<<static_cast<int>(run.machine->status)<<"}\n";
    };
    const std::string helpers=R"TRIT(
fn wal_test_check(actual: t40, expected: t40, code: t40) -> t40 {
    kstore(10001, kload(10001) + 1);
    if actual != expected { if kload(10000) == 0 { kstore(10000, code); } }
    return 1;
}
fn wal_test_seed() -> t40 {
    namespace_create(1, 0); quota_set_limit(1, 9876);
    var i: t40 = 0; while i < 52 { namespace_create(1, 0); i = i + 1; }
    return 1;
}
fn wal_test_ring_stamp() -> t40 {
    var s: t40 = kload(WAL_TAIL_ADDR); var sum: t40 = 0;
    while s != kload(WAL_HEAD_ADDR) { sum=sum+wal_record_checksum(s)+kload(wal_record_addr(s)+WAL_TXID); s=kwrap_inc(s,WAL_CAPACITY); }
    return sum;
}
fn wal_test_unchanged(head: t40, tail: t40, next: t40, code: t40) -> t40 {
    wal_test_check(kload(WAL_HEAD_ADDR), head, code);
    wal_test_check(kload(WAL_TAIL_ADDR), tail, code + 1);
    wal_test_check(kload(WAL_NEXT_TXID_ADDR), next, code + 2);
    wal_test_check(wal_test_ring_stamp(),kload(10007),code+3);
    wal_test_check(kload(namespace_addr(1)+NS_VERSION),kload(10008),code+4); return 1;
}
)TRIT";
    // Extent lookup failure modes frozen before the allocated-bound repair:
    // last-row loss, tombstone reuse/shrink, stale capacity rows, malformed
    // cursor narrowing, and acknowledged bytes lost through normal recovery.
    auto runExtentLifecycle=[&]() {
        const auto startFailures=failures.size();
        const std::string common=R"TRIT(
fn extent_check(value: t40, expected: t40, code: t40) -> t40 {
    if value != expected { unsafe { if load(10000) == 0 { store(10000,code); } } }
    return 1;
}
fn extent_path(letter: t40) -> t40 {
    unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47);
        store(10305,101); store(10306,120); store(10307,116); store(10308,45); store(10309,letter); store(10310,0); }
    return 10300;
}
fn extent_fill(words: t40, value: t40) -> t40 {
    var i: t40 = 0; while i < words { unsafe { store(10400+i,value+i); } i=i+1; } return 1;
}
fn extent_write(fd: t40, words: t40, value: t40, code: t40) -> t40 {
    extent_fill(words,value); extent_check(os_write(fd,10400,words),words,code); return 1;
}
fn extent_read(letter: t40, words: t40, value: t40, code: t40) -> t40 {
    var fd: t40 = os_open(extent_path(letter),0); extent_check(os_read(fd,10500,words+1),words,code);
    var i: t40 = 0; while i < words { var got: t40 = 0; unsafe { got=load(10500+i); }
        extent_check(got,value+i,code+1); i=i+1; }
    extent_check(os_read(fd,10500,1),0,code+2); os_close(fd); return 1;
}
)TRIT";
        const std::string reader=common+R"TRIT(
fn main() -> t40 {
    extent_read(97,13,501,1301); extent_read(99,19,601,1304); extent_read(100,23,701,1307);
    extent_check(os_open(extent_path(98),0),-1,1310);
    unsafe { store(10091,777); } var idle: t40 = 0; while idle < 999999 { idle=idle+1; } return 0;
})TRIT";
        const std::string guest=common+R"TRIT(
fn main() -> t40 {
    var a: t40 = os_open(extent_path(97),3); var b: t40 = os_open(extent_path(98),3);
    extent_write(a,37,101,1201); extent_write(b,41,201,1202);
    extent_write(a,29,301,1203); extent_write(b,31,401,1204);
    extent_check(os_fsync(a),1,1205); extent_check(os_fsync(b),1,1206);
    unsafe { store(10096,1); } os_close(a);
    a=os_open(extent_path(97),3); extent_check(os_fsync(a),1,1207);
    extent_write(a,13,501,1208); extent_check(os_fsync(a),1,1209); os_close(a);
    unsafe { store(10096,2); } var c: t40 = os_open(extent_path(99),3);
    extent_write(c,19,601,1210); extent_check(os_fsync(c),1,1211); os_close(c);
    unsafe { store(10096,3); } extent_check(os_unlink(extent_path(98)),1,1212); os_close(b);
    unsafe { store(10096,4); } var d: t40 = os_open(extent_path(100),3);
    extent_write(d,23,701,1213); extent_check(os_fsync(d),1,1214); os_close(d);
    unsafe { store(10096,5); }
    var stage: t40 = 10;
    while stage < 14 {
        d=os_open(extent_path(100),0); var i: t40 = 0;
        while i < 31 { unsafe { store(10500+i,777); } i=i+1; }
        unsafe { store(10096,stage); } var got: t40 = os_read(d,10500,23);
        unsafe { store(10020+stage,got); }
        if stage < 13 { extent_check(got,23,1220+stage); i=0;
            while i < 23 { var v: t40 = 0; unsafe { v=load(10500+i); } extent_check(v,701+i,1240+stage); i=i+1; } }
        os_close(d); stage=stage+1;
    }
    unsafe { store(10096,20); }
    extent_read(97,13,501,1261); extent_read(99,19,601,1264); extent_read(100,23,701,1267);
    extent_check(os_open(extent_path(98),0),-1,1270);
    d=os_open(extent_path(100),0); extent_check(os_fsync(d),1,1271); os_close(d);
    unsafe { store(10091,777); } var idle: t40 = 0; while idle < 999999 { idle=idle+1; } return 0;
})TRIT";
        auto run=makeRun("extent_lifecycle","",guest,{},reader);
        if(run.machine) {
            auto& m=*run.machine; long long steps=0, lastMarker=0, beforeShrink=-1, afterShrink=-1;
            bool awaiting=false; int stage=0, originalSlot=-1, staleSlot=-1;
            std::set<int> injected; std::vector<std::pair<int,vm::TernaryValue>> saved;
            std::ofstream injections(run.dir/"cursor_injections.jsonl"), lifecycle(run.dir/"lifecycle.jsonl");
            auto save=[&](int addr) { saved.push_back({addr,m.dmem.load(addr).first}); };
            auto sameRegisters=[&](const auto& registers) { bool same=true; for(int r=0;r<isa::REG_COUNT;++r)
                same=same && registers[r].mode==m.regfile.reg[r].mode && registers[r].bits.lo==m.regfile.reg[r].bits.lo && registers[r].bits.hi==m.regfile.reg[r].bits.hi; return same; };
            while(m.isRunning() && steps++<120'000'000) {
                vm::VMExecutionRecord rec; vm::step(m,&rec);
                const auto marker=userword(m,10096);
                if(!m.trap_active && marker!=lastMarker && marker>=1 && marker<=5) {
                    lastMarker=marker; const auto cursor=word(m,3017);
                    lifecycle<<"{\"marker\":"<<marker<<",\"cursor\":"<<cursor<<",\"rows\":[";
                    for(int e=0;e<cursor;++e) {if(e)lifecycle<<','; lifecycle<<'[';for(int j=0;j<6;++j){if(j)lifecycle<<',';lifecycle<<word(m,265000+e*6+j);}lifecycle<<']';}lifecycle<<"]}\n";
                    if(marker==3) beforeShrink=cursor;
                    if(marker==4) afterShrink=cursor;
                    if(marker==5) {
                        check(beforeShrink>afterShrink && afterShrink==cursor,"extent lifecycle actual reclaim shrinks allocated cursor and replacement reuses lower slot");
                        check(cursor>0 && word(m,265000+static_cast<int>(cursor-1)*6+5)>0,"extent lifecycle last allocated row remains live");
                    }
                }
                if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL &&
                   rec.before_privilege==vm::PrivilegeMode::User && rec.syscall_id==14 && marker>=10 && marker<=13 && !injected.count(static_cast<int>(marker))) {
                    stage=static_cast<int>(marker); injected.insert(stage); awaiting=true; saved.clear(); save(3017);
                    const auto registers=m.regfile.reg;const auto pc=m.pc;const auto trap=m.trap_active;
                    std::array<long long,8> descriptor{};for(int j=0;j<8;++j)descriptor[j]=word(m,290000+static_cast<int>(rec.syscall_arg0)*8+j);
                    const auto cursor=word(m,3017); originalSlot=staleSlot=-1;
                    if(stage==10)m.dmem.store(3017,vm::ops::fromLong(-1));
                    if(stage==11)m.dmem.store(3017,vm::exec::divideValue(vm::ops::fromLong(3),vm::ops::fromLong(2),TernaryMode::T40));
                    if(stage==12)m.dmem.store(3017,vm::ops::fromLong(4097));
                    if(stage==13) {
                        const auto inode=descriptor[2];
                        for(int e=0;e<cursor;++e)if(word(m,265000+e*6+1)==inode && word(m,265000+e*6+2)==0 && word(m,265000+e*6+5)>0)originalSlot=e;
                        staleSlot=static_cast<int>(cursor);
                        check(originalSlot>=0 && staleSlot<4096,"extent lifecycle stale-row adapter found actual descriptor extent and unused row");
                        if(originalSlot>=0 && staleSlot<4096) {
                            save(265000+originalSlot*6+5);
                            for(int j=0;j<6;++j)save(265000+staleSlot*6+j);
                            for(int j=0;j<6;++j)m.dmem.store(265000+staleSlot*6+j,m.dmem.load(265000+originalSlot*6+j).first);
                            m.dmem.store(265000+originalSlot*6+5,vm::ops::fromLong(0));
                        }
                    }
                    bool sameDescriptor=true;for(int j=0;j<8;++j)sameDescriptor=sameDescriptor && descriptor[j]==word(m,290000+static_cast<int>(rec.syscall_arg0)*8+j);
                    const double observedCursor=sandbox::ops::toDouble(m.dmem.load(3017).first.asTriple());
                    check(stage!=11 || std::abs(observedCursor-1.5)<1e-8,"extent lifecycle actual fractional cursor is1.5");
                    check(sameRegisters(registers) && m.pc==pc && m.trap_active==trap && sameDescriptor,"extent cursor adapter preserves actual PC/registers/descriptor/trap state");
                    injections<<"{\"event\":\"inject_at_actual_user_read\",\"stage\":"<<stage<<",\"cursor\":"<<observedCursor<<",\"original_cursor\":"<<cursor<<",\"original_slot\":"<<originalSlot<<",\"stale_slot\":"<<staleSlot<<",\"pc\":"<<pc<<",\"fd\":"<<rec.syscall_arg0<<",\"descriptor_offset\":"<<descriptor[3]<<",\"label\":\"only saved cursor/extent words deliberately changed; no fabricated return\"}\n";
                }
                if(awaiting && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET && rec.after_privilege==vm::PrivilegeMode::User) {
                    const auto status=vm::ops::toLong(m.regfile.read(13)),payload=vm::ops::toLong(m.regfile.read(14)),detail=vm::ops::toLong(m.regfile.read(15));
                    if(stage<13)check(status==1 && payload==23 && detail==0,"extent lifecycle malformed/fractional cursor preserves actual read");
                    else {
                        check(status==-1 && detail==-1,"extent lifecycle valid cursor rejects stale unallocated row via actual read");
                        bool untouched=true;for(int j=0;j<31;++j)untouched=untouched && userword(m,10500+j)==777;
                        check(untouched,"extent lifecycle allocation-bound rejection has no partial output");
                    }
                    for(const auto& value:saved)m.dmem.store(value.first,value.second);
                    bool restored=true;for(const auto& value:saved){const auto actual=m.dmem.load(value.first).first;restored=restored && actual.mode==value.second.mode && actual.bits.lo==value.second.bits.lo && actual.bits.hi==value.second.bits.hi;}
                    check(restored,"extent cursor adapter restores every exact saved metadata word");
                    injections<<"{\"event\":\"actual_read_return_and_restore\",\"stage\":"<<stage<<",\"status\":"<<status<<",\"payload\":"<<payload<<",\"detail\":"<<detail<<",\"restored\":"<<(restored?"true":"false")<<"}\n";
                    awaiting=false;
                }
                if(!m.trap_active && userword(m,10091)==777)break;
            }
            injections.close();lifecycle.close();recordBoot(run);
            check(!awaiting && injected.size()==4,"extent lifecycle all four actual cursor read adapters complete and restore");
            check(userword(m,10091)==777 && userword(m,10000)==0,"extent lifecycle actual guest create/grow/truncate/unlink/reuse and exact surviving bytes");
            std::ofstream(run.dir/"result.json")<<"{\"steps\":"<<steps<<",\"first_failure\":"<<userword(m,10000)<<",\"cursor_before_reclaim\":"<<beforeShrink<<",\"cursor_after_reclaim\":"<<afterShrink<<",\"injected_reads\":"<<injected.size()<<"}\n";
            const auto copy=run.dir/"acknowledged_lifecycle.tdisk";std::filesystem::copy_file(run.backing,copy,std::filesystem::copy_options::overwrite_existing);
            auto fresh=makeRun("extent_lifecycle_recovery","",reader,copy,reader);
            if(fresh.machine) {
                long long freshSteps=0;while(fresh.machine->isRunning() && freshSteps++<120'000'000) {vm::step(*fresh.machine);if(!fresh.machine->trap_active && userword(*fresh.machine,10091)==777)break;}
                recordBoot(fresh);check(userword(*fresh.machine,10091)==777 && userword(*fresh.machine,10000)==0,"extent lifecycle fresh normal kernel recovers exact A13/C19/D23 and deleted B");
                std::ofstream(fresh.dir/"result.json")<<"{\"steps\":"<<freshSteps<<",\"first_failure\":"<<userword(*fresh.machine,10000)<<",\"cursor\":"<<word(*fresh.machine,3017)<<"}\n";
            }
        }
        std::ofstream result(base/"extent_lifecycle_report.json");result<<"{\"status\":\""<<(failures.size()==startFailures?"PASS":"FAIL")<<"\",\"scope\":\""<<(lifecycle_only?"focused":"additional-to-original41")<<"\",\"selected\":[\"extent_lifecycle\",\"extent_lifecycle_recovery\"],\"required\":[\"extent_lifecycle\",\"extent_lifecycle_recovery\"],\"failures\":[";
        for(size_t i=startFailures;i<failures.size();++i){if(i>startFailures)result<<',';result<<'"'<<jsonEscape(failures[i])<<'"';}result<<"]}\n";
        return failures.size()==startFailures;
    };
    // Whole-ring proof gaps: an apparently closed prefix can be invalidated by
    // duplicate COMMIT or late DATA; valid interleaving/reverse closure and
    // aborted-zero DATA require compatibility fallback, including durable recovery.
    auto runCheckpointShapes=[&]() {
        const auto firstFailure=failures.size();
        const std::vector<std::string> names={"sequential_closed","interleaved","reverse_closure","duplicate_commit","late_data","aborted_zero"};
        std::vector<std::string> selected;
        const std::string exact=R"TRIT(
    wal_test_check(kload(namespace_addr(1)+NS_ID),1,1301);
    wal_test_check(kload(namespace_addr(1)+NS_ROOT_INODE),0,1302);
    wal_test_check(kload(namespace_addr(1)+NS_ACTIVE),1,1303);
    wal_test_check(kload(namespace_addr(1)+NS_VERSION),81,1304);
    wal_test_check(kload(quota_addr(1)+QUOTA_ID),1,1305);
    wal_test_check(kload(quota_addr(1)+QUOTA_LIMIT),7654,1306);
    wal_test_check(kload(quota_addr(1)+QUOTA_VERSION),9,1307);
)TRIT";
        for(const auto& shape:names) {
            const bool invalid=shape=="duplicate_commit" || shape=="late_data";
            const std::string name="shape_"+shape; selected.push_back(name);
            std::string fixture=helpers+R"TRIT(fn wal_test_fixture() -> t40 {
    wal_test_seed(); wal_shape_bool(log_checkpoint() >= 0,1201);
    wal_test_check(wal_used_blocks(),0,1202);
    var ns: t40 = namespace_addr(1)+NS_VERSION;
    var limit: t40 = quota_addr(1)+QUOTA_LIMIT;
    var version: t40 = quota_addr(1)+QUOTA_VERSION;
)TRIT";
            if(shape=="sequential_closed") fixture+=R"TRIT(
    var i: t40 = 0;
    while i < 12 {
        var tx: t40 = log_begin(); wal_shape_bool(log_write(tx,ns,0,70+i) >= 0,1203);
        wal_shape_bool(log_write(tx,limit,0,7643+i) >= 0,1204);
        wal_shape_bool(log_write(tx,version,0,9) >= 0,1205);
        wal_shape_bool(log_commit(tx) > 0,1206);
        var empty: t40 = log_begin(); wal_shape_bool(log_commit(empty) > 0,1207); i=i+1;
    }
)TRIT";
            else if(shape=="interleaved") {
                fixture+="var a: t40 = log_begin(); var b: t40 = log_begin(); log_write(a,ns,53,81); log_write(b,limit,9876,7654); log_write(a,version,2,9); log_commit(a); log_commit(b);\n";
            } else if(shape=="reverse_closure") {
                fixture+="var a: t40 = log_begin(); var b: t40 = log_begin(); log_write(b,limit,9876,7654); log_write(b,version,2,9); log_commit(b); log_write(a,ns,53,81); log_commit(a);\n";
            } else if(shape=="aborted_zero") fixture+="var abort: t40 = log_begin(); log_write(abort,ns,53,999); log_write(abort,limit,9876,999); log_abort(abort); var a: t40 = log_begin(); log_write(a,ns,53,81); log_write(a,limit,9876,7654); log_write(a,version,2,9); log_commit(a);\n";
            else {
                fixture+="var a: t40 = log_begin(); log_write(a,ns,53,81); log_write(a,limit,9876,7654); log_write(a,version,2,9); log_commit(a); var b: t40 = log_begin(); ";
                if(shape=="duplicate_commit") fixture+="var bad: t40 = kload(WAL_HEAD_ADDR); log_commit(b); kstore(wal_record_addr(bad)+WAL_TXID,a); wal_record_finalize(bad);\n";
                else fixture+="var bad: t40 = log_write(b,ns,81,82); log_commit(b); kstore(wal_record_addr(bad)+WAL_TXID,a); wal_record_finalize(bad);\n";
            }
            fixture+="kstore(10016,1); var guard: t40 = wal_checkpoint_guard(); kstore(10017,guard); ";
            if(invalid) fixture+=R"TRIT(
    kstore(10016,2); wal_test_check(guard,ERR_INVALID,1210);
    var fitting: t40 = wal_prepare_admission(3); kstore(10018,fitting); kstore(10016,3); wal_test_check(fitting,ERR_INVALID,1211);
    var maintenance: t40 = wal_prepare_admission(727); kstore(10019,maintenance); kstore(10016,4); wal_test_check(maintenance,ERR_INVALID,1212);
)TRIT";
            else fixture+="wal_test_check(guard,1,1210); var oldtail: t40 = kload(WAL_TAIL_ADDR); var admission: t40 = wal_prepare_admission(727); kstore(10019,admission); wal_test_check(admission,1,1211); wal_shape_bool(kload(WAL_TAIL_ADDR) != oldtail,1212); wal_test_check(wal_used_blocks(),0,1213);"+exact;
            fixture+="kstore(10002,777); return 1; }";
            fixture.insert(0,"fn wal_shape_bool(value: trit, code: t40) -> t40 { match value { pos => { return wal_test_check(1,1,code); } _ => { return wal_test_check(0,1,code); } } }\n");
            auto run=makeRun(name,fixture,"",{}); if(!run.machine)continue;
            auto& m=*run.machine; long long steps=0; bool captured=false; int observedStage=0;
            std::vector<long long> before; std::string diskBefore;
            auto state=[&]() {
                std::vector<long long> values;
                for(int a=3000;a<3020;++a)values.push_back(word(m,a));
                for(int a=680000;a<680000+729*27;++a)values.push_back(word(m,a));
                for(int a=391000;a<392000+64*4;++a)values.push_back(word(m,a));
                return values;
            };
            auto saveState=[&](const std::vector<long long>& values,const std::string& label) {
                std::ofstream out(run.dir/(label+".bin"),std::ios::binary);
                out.write(reinterpret_cast<const char*>(values.data()),static_cast<std::streamsize>(values.size()*sizeof(long long)));
            };
            std::ofstream boundaries(run.dir/"invalid_boundaries.jsonl");
            while(m.isRunning() && steps++<90'000'000) {
                const auto stage=word(m,10016);
                if(invalid && captured && stage>observedStage) {
                    const auto current=state();const auto durable=readTextFile(run.backing);
                    const bool same=current==before && durable==diskBefore;
                    check(same,name+" exact unchanged after boundary"+std::to_string(stage));
                    saveState(current,"state_after_"+std::to_string(stage));
                    boundaries<<"{\"stage\":"<<stage<<",\"unchanged\":"<<(same?"true":"false")<<",\"durable_before_hash\":\""<<stableHash(diskBefore)<<"\",\"durable_after_hash\":\""<<stableHash(durable)<<"\"}\n";
                    observedStage=static_cast<int>(stage);
                }
                if(!captured && word(m,10016)==1) {
                    captured=true; observedStage=1; if(invalid) {before=state();diskBefore=readTextFile(run.backing);saveState(before,"state_before");std::filesystem::copy_file(run.backing,run.dir/"durable_before.tdisk",std::filesystem::copy_options::overwrite_existing);}
                    std::ofstream(run.dir/"shape_before.json")<<"{\"used\":"<<word(m,3005)-word(m,3006)<<",\"steps\":"<<steps<<"}\n";
                }
                vm::step(m);
            }
            boundaries.close();recordBoot(run);check(captured,name+" observed actual guard entry");
            check(!m.isRunning() && word(m,10002)==777,name+" terminal fixture completion");
            check(word(m,10000)==0,name+" exact semantics code="+std::to_string(word(m,10000)));
            bool unchanged=true;
            if(invalid) {check(observedStage==4,name+" all guard/fitting/maintenance boundaries observed");std::filesystem::copy_file(run.backing,run.dir/"durable_after.tdisk",std::filesystem::copy_options::overwrite_existing);unchanged=captured && state()==before && readTextFile(run.backing)==diskBefore;check(unchanged,name+" exact ring/control/namespace/quota/durable unchanged");}
            std::ofstream(run.dir/"result.json")<<"{\"steps\":"<<steps<<",\"guard\":"<<word(m,10017)<<",\"fitting3\":"<<word(m,10018)<<",\"maintenance727\":"<<word(m,10019)<<",\"first_failure\":"<<word(m,10000)<<",\"checks\":"<<word(m,10001)<<",\"unchanged\":"<<(unchanged?"true":"false")<<"}\n";
            if(!invalid) {
                const auto copy=run.dir/"acknowledged_shape.tdisk";std::filesystem::copy_file(run.backing,copy,std::filesystem::copy_options::overwrite_existing);
                const auto freshName=name+"_recovery";selected.push_back(freshName);
                auto fresh=makeRun(freshName,helpers+"fn wal_test_fixture() -> t40 {"+exact+"kstore(10002,777); return 1; }","",copy);
                if(fresh.machine) {
                    long long freshSteps=0;while(fresh.machine->isRunning() && freshSteps++<90'000'000)vm::step(*fresh.machine);
                    recordBoot(fresh);check(!fresh.machine->isRunning() && word(*fresh.machine,10002)==777 && word(*fresh.machine,10000)==0,freshName+" fresh normal recovery exact namespace/quota");
                    std::ofstream(fresh.dir/"result.json")<<"{\"steps\":"<<freshSteps<<",\"first_failure\":"<<word(*fresh.machine,10000)<<",\"checks\":"<<word(*fresh.machine,10001)<<"}\n";
                }
            }
        }
        std::ofstream report(base/"checkpoint_shapes_report.json");report<<"{\"status\":\""<<(failures.size()==firstFailure?"PASS":"FAIL")<<"\",\"scope\":\""<<(shape_only?"focused":"additional-to-original43")<<"\",\"selected\":[";
        for(size_t i=0;i<selected.size();++i){if(i)report<<',';report<<'"'<<selected[i]<<'"';}report<<"],\"failures\":[";
        for(size_t i=firstFailure;i<failures.size();++i){if(i>firstFailure)report<<',';report<<'"'<<jsonEscape(failures[i])<<'"';}report<<"]}\n";
        return failures.size()==firstFailure;
    };
    if(shape_only)return runCheckpointShapes();
    if(!admission_only && !lifecycle_only)runCheckpointShapes();
    if(lifecycle_only)return runExtentLifecycle();
    if(!admission_only)runExtentLifecycle();
    const std::string guards=helpers+R"TRIT(
fn wal_test_fixture() -> t40 {
    wal_test_seed(); var before: t40 = kload(WAL_TAIL_ADDR);
    var snap: t40 = log_snapshot_acquire(1);
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 101);
    wal_test_check(kload(WAL_TAIL_ADDR), before, 102);
    wal_test_check(wal_prepare_admission(3), 1, 103);
    var oldversion: t40 = kload(namespace_addr(1)+NS_VERSION); wal_test_check(namespace_create(1,0),1,146); wal_test_check(kload(namespace_addr(1)+NS_VERSION),oldversion+1,147); wal_test_check(kload(WAL_TAIL_ADDR),before,148);
    log_snapshot_release(snap);
    var tx: t40 = log_begin();
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 104);
    log_abort(tx); wal_test_check(wal_prepare_admission(727), 1, 105);
    wal_test_check(kload(ATTR_STORE_BASE + ATTR_OBJECT_ID), 1, 106);
    wal_test_check(kload(ATTR_STORE_BASE + ATTR_OBJECT_TYPE), ATTR_TYPE_PROCESS, 107);
    wal_test_check(kload(ATTR_STORE_BASE + ATTR_KEY), ATTR_KEY_CREATED_TXID, 108);
    wal_test_check(kload(ATTR_STORE_BASE + ATTR_VALUE), 0, 109);
    wal_test_check(kload(ATTR_STORE_BASE + ATTR_TXID),1,139); wal_test_check(kload(ATTR_STORE_BASE+ATTR_VERSION),1,140);
    wal_test_check(kload(ATTR_STORE_BASE+ATTR_STORE_WORDS+ATTR_OBJECT_ID),1,141); wal_test_check(kload(ATTR_STORE_BASE+ATTR_STORE_WORDS+ATTR_OBJECT_TYPE),ATTR_TYPE_PROCESS,142);
    wal_test_check(kload(ATTR_STORE_BASE+ATTR_STORE_WORDS+ATTR_KEY),ATTR_KEY_DESIRED_STATE,143); wal_test_check(kload(ATTR_STORE_BASE+ATTR_STORE_WORDS+ATTR_TXID),2,144); wal_test_check(kload(ATTR_STORE_BASE+ATTR_STORE_WORDS+ATTR_VERSION),1,145);
    wal_test_check(kload(ATTR_STORE_BASE + ATTR_STORE_WORDS + ATTR_VALUE), PROC_RUNNABLE, 110);
    wal_test_seed(); before = kload(WAL_TAIL_ADDR);
    var pin: t40 = buffer_pin(1, 0, 0, VFS_DATA_BASE);
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 111);
    wal_test_check(kload(WAL_TAIL_ADDR), before, 112); buffer_unpin(pin);
    buffer_mark_dirty(pin, kload(WAL_NEXT_LSN_ADDR) + 100);
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 113);
    wal_test_check(kload(WAL_TAIL_ADDR), before, 114);
    kstore(buffer_frame_addr(pin) + BF_DIRTY, 0); kstore(BUFFER_DIRTY_COUNT_ADDR, 0);
    wal_test_check(wal_prepare_admission(727), 1, 115);
    var head: t40 = kload(WAL_HEAD_ADDR); var tail: t40 = kload(WAL_TAIL_ADDR); var next: t40 = kload(WAL_NEXT_TXID_ADDR); kstore(10007,wal_test_ring_stamp()); kstore(10008,kload(namespace_addr(1)+NS_VERSION));
    wal_test_check(wal_prepare_admission(728), ERR_INVALID, 116);
    wal_test_check(wal_prepare_admission(-1), ERR_INVALID, 117);
    wal_test_check(wal_prepare_admission(3/2),ERR_INVALID,149); wal_test_unchanged(head,tail,next,118);
    // Empty colliding begin remains owned after original owner closes.
    tx = log_begin(); var i: t40 = 0;
    while i < 242 { var empty: t40 = log_begin(); log_abort(empty); i = i + 1; }
    var collision: t40 = log_begin(); log_abort(tx);
    wal_test_seed(); before = kload(WAL_TAIL_ADDR);
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 121);
    log_abort(collision); wal_test_check(wal_prepare_admission(727), 1, 122);
    // Replacement owner and duplicate closure must preserve exact identities.
    tx = log_begin(); log_write(tx, namespace_addr(1) + NS_VERSION, 0, 55); log_commit(tx);
    var owner: t40 = log_begin(); head = kload(WAL_HEAD_ADDR); tail = kload(WAL_TAIL_ADDR); next = kload(WAL_NEXT_TXID_ADDR); kstore(10007,wal_test_ring_stamp()); kstore(10008,kload(namespace_addr(1)+NS_VERSION));
    wal_test_check(log_abort(tx), ERR_INVALID, 123); wal_test_check(log_commit(tx), ERR_INVALID, 124);
    wal_test_check(log_write(tx, namespace_addr(1) + NS_VERSION, 55, 99), ERR_INVALID, 125);
    wal_test_unchanged(head,tail,next,126); wal_test_check(kload(namespace_addr(1) + NS_VERSION), 55, 129);
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 130); log_abort(owner);
    wal_test_check(wal_prepare_admission(727), 1, 131);
    // All 243 normal slots plus all 243 fallback owners, no DATA evidence.
    var start: t40 = kload(WAL_NEXT_TXID_ADDR); i = 0;
    while i < 486 { wal_test_check(log_begin(), start + i, 132); i = i + 1; }
    head = kload(WAL_HEAD_ADDR); tail = kload(WAL_TAIL_ADDR); next = kload(WAL_NEXT_TXID_ADDR); kstore(10007,wal_test_ring_stamp()); kstore(10008,kload(namespace_addr(1)+NS_VERSION));
    wal_test_check(log_begin(), ERR_AGAIN, 133); wal_test_unchanged(head,tail,next,134);
    wal_test_check(wal_prepare_admission(727), ERR_AGAIN, 137);
    i = 0; while i < 486 { log_abort(start + i); i = i + 1; }
    wal_test_check(wal_prepare_admission(727), 1, 138);
    kstore(10002, 777); return 1;
}
)TRIT";
    // Each blocker is a separate kernel boot; no resetting a live WAL to pass.
    std::vector<std::pair<std::string,std::string>> guard_cases={{"live_ownership",guards}};
    for(const auto& kind:std::vector<std::string>{"untracked_data","unknown","outside_used","boot_identity","boot_value","boot_txid","boot_version","corrupt","broken_chain","invalid_highwater","invalid_domain"}) {
        std::string body="fn wal_test_fixture() -> t40 { wal_test_seed(); var tx: t40 = log_begin(); ";
        if(kind=="untracked_data") body+="var i: t40 = 0; while i < 242 { var e: t40 = log_begin(); log_abort(e); i = i + 1; } var c: t40 = log_begin(); log_write(c, namespace_addr(1) + NS_VERSION, 0, 7); log_abort(tx);";
        else if(kind=="unknown") body+="log_write(tx, 139220, 0, 7); log_commit(tx);";
        else if(kind=="outside_used") body+="wal_test_check(wal_addr_is_persistent(kload(VFS_NEXT_DATA_ADDR)), 1, 200); log_write(tx, kload(VFS_NEXT_DATA_ADDR), 0, 7); log_commit(tx);";
        else if(kind.rfind("boot_",0)==0) { body+="log_abort(tx); ";const std::string field=kind=="boot_identity"?"ATTR_OBJECT_ID":kind=="boot_value"?"ATTR_VALUE":kind=="boot_txid"?"ATTR_TXID":"ATTR_VERSION"; body+="kstore(ATTR_STORE_BASE + "+field+", 99);"; }
        else if(kind=="invalid_highwater") body+="log_abort(tx); kstore(VFS_NEXT_DATA_ADDR,VFS_DATA_BASE-1);";
        else if(kind=="invalid_domain") body+="var slot: t40 = log_write(tx,namespace_addr(1)+NS_VERSION,0,7); log_commit(tx); kstore(wal_record_addr(slot)+WAL_TARGET_KIND,99); wal_record_finalize(slot);";
        else if(kind=="corrupt") body+="log_abort(tx); kstore(wal_record_addr(kload(WAL_TAIL_ADDR)) + WAL_CHECKSUM, 1);";
        else body+="log_write(tx, namespace_addr(1) + NS_VERSION, 0, 7); var slot: t40 = kload(WAL_HEAD_ADDR) - 1; if slot < 0 { slot = WAL_CAPACITY - 1; } kstore(wal_record_addr(slot) + WAL_PREV_LSN, 999); wal_record_finalize(slot); log_commit(tx);";
        body+=" var head: t40 = kload(WAL_HEAD_ADDR); var tail: t40 = kload(WAL_TAIL_ADDR); var next: t40 = kload(WAL_NEXT_TXID_ADDR); kstore(10007,wal_test_ring_stamp()); kstore(10008,kload(namespace_addr(1)+NS_VERSION)); wal_test_check(wal_prepare_admission(727), "+std::string(kind=="corrupt"||kind=="broken_chain"||kind=="invalid_highwater"||kind=="invalid_domain"?"ERR_INVALID":"ERR_AGAIN")+", 201); wal_test_unchanged(head,tail,next,202);";
        if(kind=="corrupt" || kind=="invalid_domain" || kind=="invalid_highwater" || kind=="broken_chain") {
            const auto point=body.find("wal_test_check(wal_prepare_admission(727)");
            body.insert(point,"wal_test_check(wal_prepare_admission(3),ERR_INVALID,211); wal_test_unchanged(head,tail,next,212);");
        }
        if(kind=="untracked_data") body+="log_abort(c); wal_test_check(wal_prepare_admission(727),1,210);";
        body+=" kstore(10002,777); return 1; }";
        guard_cases.push_back({kind,helpers+body});
    }
    for(const auto& kind:std::vector<std::string>{"corrupt","broken_chain","invalid_highwater","invalid_domain"}) {
        std::string body=R"TRIT(fn wal_test_fixture() -> t40 {
            wal_test_seed(); var seeded: t40 = log_checkpoint();
            if seeded < 0 { wal_test_check(seeded,1,1101); return seeded; }
            wal_test_check(kload(WAL_TAIL_ADDR),kload(WAL_HEAD_ADDR),1102);
            wal_test_check(wal_used_blocks(),0,1103);
            wal_test_check(kload(namespace_addr(1)+NS_VERSION),53,1104);
            wal_test_check(kload(quota_addr(1)+QUOTA_LIMIT),9876,1105);
            wal_test_check(kload(quota_addr(1)+QUOTA_VERSION),2,1106);
            var record: t40 = kload(WAL_HEAD_ADDR);
        )TRIT";
        int occupancy=0;
        if(kind=="invalid_highwater") body+="kstore(VFS_NEXT_DATA_ADDR,VFS_DATA_BASE-1);";
        else {
            occupancy=kind=="corrupt"?1:2;
            body+="var tx: t40 = log_begin(); if tx <= 0 { wal_test_check(tx,1,1107); return tx; } var wrote: t40 = log_write(tx,namespace_addr(1)+NS_VERSION,53,53); wal_test_check(wrote,record,1108); if wrote < 0 { return wrote; }";
            if(kind=="corrupt") body+="wal_test_check(log_abort(tx),1,1109); kstore(wal_record_addr(record)+WAL_CHECKSUM,kload(wal_record_addr(record)+WAL_CHECKSUM)+1);";
            else {
                if(kind=="broken_chain") body+="kstore(wal_record_addr(record)+WAL_PREV_LSN,kload(WAL_NEXT_LSN_ADDR)+100); wal_record_finalize(record);";
                body+="var committed: t40 = log_commit(tx); if committed <= 0 { wal_test_check(committed,1,1110); return committed; }";
                if(kind=="invalid_domain") body+="kstore(wal_record_addr(record)+WAL_TARGET_KIND,99); wal_record_finalize(record);";
            }
        }
        body+="wal_test_check(wal_used_blocks(),"+std::to_string(occupancy)+",1111); if wal_used_blocks() >= 243 { wal_test_check(wal_used_blocks(),0,1112); } wal_test_check(log_capacity_for(3),1,1113); kstore(10013,wal_used_blocks()); kstore(10014,log_capacity_for(3)); ";
        body+=R"TRIT(var head: t40 = kload(WAL_HEAD_ADDR); var tail: t40 = kload(WAL_TAIL_ADDR); var next: t40 = kload(WAL_NEXT_TXID_ADDR);
            kstore(10007,wal_test_ring_stamp()); kstore(10008,kload(namespace_addr(1)+NS_VERSION));
            var checkpoint: t40 = kload(WAL_CHECKPOINT_ADDR); var lsn: t40 = kload(WAL_NEXT_LSN_ADDR);
            var highwater: t40 = kload(VFS_NEXT_DATA_ADDR);
            var checksum: t40 = kload(wal_record_addr(record)+WAL_CHECKSUM);
            var actual: t40 = wal_prepare_admission(3); kstore(10015,actual);
            wal_test_check(actual,ERR_INVALID,1114); wal_test_unchanged(head,tail,next,1115);
            wal_test_check(kload(WAL_CHECKPOINT_ADDR),checkpoint,1120); wal_test_check(kload(WAL_NEXT_LSN_ADDR),lsn,1121);
            wal_test_check(kload(VFS_NEXT_DATA_ADDR),highwater,1122);
            wal_test_check(kload(wal_record_addr(record)+WAL_CHECKSUM),checksum,1123);
            wal_test_check(kload(namespace_addr(1)+NS_VERSION),53,1124);
            wal_test_check(kload(quota_addr(1)+QUOTA_LIMIT),9876,1125);
            wal_test_check(kload(quota_addr(1)+QUOTA_VERSION),2,1126);
            kstore(10002,777); return 1;
        })TRIT";
        guard_cases.push_back({"low_"+kind,helpers+body});
    }
    if(admission_only) {
        guard_cases.erase(std::remove_if(guard_cases.begin(),guard_cases.end(),[](const auto& entry){return entry.first.rfind("low_",0)!=0;}),guard_cases.end());
    }
    std::ofstream mapping(base/"acceptance_mapping.json");mapping<<"{\"test_first\":true,\"criteria\":[\"AC1 frozen test assertions and actual existing-path Red\",\"AC2 normal/prefix guest writes multiple epochs wrap exact bytes FD reserve\",\"AC3 live/empty untracked ownership duplicate close exhaustion snapshot pin newer dirty corrupt unknown used-boundary boot mismatch\",\"AC4 observed WAL home checkpoint tail cutpoint durable copies fresh normal kernel recovery failed barrier retry unchanged tail\",\"AC5 targeted fsync two dirty inodes multi-namespace quota no-device boot reconstruction\",\"AC6 original errno4 exact pending initialized-only reverse fd0 cleanup generic graph failure\",\"AC7 unchanged full33/service SDK metrics stream default required final gates\",\"AC8 205-path scope protected bodies current formats parseable closed hashes\"],\"fixtures\":[";
    for(size_t i=0;i<guard_cases.size();++i) {if(i) mapping<<',';mapping<<'"'<<guard_cases[i].first<<'"';}mapping<<"]}\n";mapping.close();
    // Compile diagnostics are legitimate missing-feature Red, but the pressure
    // and publication fixtures below compile against pre-repair production.
    for(const auto& [name,fixture]:guard_cases) {
        auto run=makeRun(name,fixture,"",{});if(!run.machine) continue;
        long long steps=0;while(run.machine->isRunning() && steps++<50'000'000) vm::step(*run.machine);
        recordBoot(run);check(word(*run.machine,10002)==777,name+" fixture completed");check(word(*run.machine,10000)==0,name+" semantic assertion code="+std::to_string(word(*run.machine,10000)));
        check(word(*run.machine,10001)>0,name+" actual assertions executed");
        std::ofstream(run.dir/"result.json")<<"{\"steps\":"<<steps<<",\"first_failure\":"<<word(*run.machine,10000)<<",\"checks\":"<<word(*run.machine,10001)<<",\"used\":"<<(name.rfind("low_",0)==0?std::to_string(word(*run.machine,10013)):"null")<<",\"capacity3\":"<<(name.rfind("low_",0)==0?std::to_string(word(*run.machine,10014)):"null")<<",\"actual_admission\":"<<(name.rfind("low_",0)==0?std::to_string(word(*run.machine,10015)):"null")<<"}\n";
    }
    if(admission_only) {
        std::ofstream report(base/"report.json"); report<<"{\"status\":\""<<(failures.empty()?"PASS":"FAIL")<<"\",\"workflow\":\"wal-e2e\",\"validation\":\"current-source\",\"scope\":\"focused\",\"scenario_count\":"<<guard_cases.size()<<",\"selected\":[";
        for(size_t i=0;i<guard_cases.size();++i) {if(i)report<<',';report<<'"'<<guard_cases[i].first<<'"';} report<<"],\"failures\":[";
        for(size_t i=0;i<failures.size();++i) {if(i)report<<',';report<<'"'<<jsonEscape(failures[i])<<'"';} report<<"]}\n"; return failures.empty();
    }
    std::string paths;
    for(const auto& [addr,path]:std::vector<std::pair<int,std::string>>{{10300,"/tmp/wal-a"},{10350,"/tmp/wal-b"}})
        for(size_t i=0;i<=path.size();++i) paths+="unsafe { store("+std::to_string(addr+i)+", "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"); }\n";
    auto guestCheck=[](std::string actual,std::string expected,int code){return "if "+actual+" != "+expected+" { unsafe { if load(10000) == 0 { store(10000, "+std::to_string(code)+"); } } }\n";};
    std::string pressure="fn main() -> t40 { "+paths+"var a: t40 = os_open(10300,3); var b: t40 = os_open(10350,3);"+guestCheck("a","0",301)+guestCheck("b","1",302);
    pressure+="var n: t40 = 0; var selected: t40 = a; while n < 120 { var i: t40 = 0; while i < 27 { unsafe { store(10400+i,n*27+i+31); } i=i+1; } var fd: t40 = selected; if selected == a { selected=b; } else { selected=a; } unsafe { store(10090,n); } var wrote: t40 = os_write(fd,10400,27);"+guestCheck("wrote","27",303)+"var synced: t40 = os_fsync(fd);"+guestCheck("synced","1",304)+"unsafe { store(10092,n+1); } n=n+1; } os_close(a); os_close(b); var rd: t40 = os_open(10300,0); n=0; while n < 60 { var got: t40 = os_read(rd,10500,27);"+guestCheck("got","27",305)+"var i: t40 = 0; while i < 27 { var v: t40 = 0; unsafe { v=load(10500+i); } "+guestCheck("v","n*54+i+31",306)+"i=i+1; } n=n+1; } os_close(rd); rd=os_open(10350,0); n=0; while n < 60 { var got: t40 = os_read(rd,10500,27);"+guestCheck("got","27",307)+"var i: t40 = 0; while i < 27 { var v: t40 = 0; unsafe { v=load(10500+i); } "+guestCheck("v","n*54+i+58",308)+"i=i+1; } n=n+1; } os_close(rd); unsafe { store(10091,777); } while n < 999999 { n=n+1; } return 0; }";
    auto pressureRun=makeRun("pressure", "fn wal_test_fixture() -> t40 { namespace_create(1,0); quota_set_limit(1,9876); return 1; }",pressure,{});
    long long epochs=0,wraps=0,barriers=0,writes=0,steps=0,acknowledged=0,lasthead=0,lasttail=0;
    std::vector<std::pair<std::filesystem::path,long long>> cuts;
    if(pressureRun.machine) {
        auto& m=*pressureRun.machine;std::ofstream trace(pressureRun.dir/"storage.jsonl"),calls(pressureRun.dir/"syscalls.jsonl");bool awaiting=false,checkpointActive=false,captureEpoch=false;int call=0,checkpointPhase=0;long long request=0;
        while(steps++<400'000'000 && m.isRunning()) {
            if(m.privilege==vm::PrivilegeMode::Kernel && m.pc==pressureRun.checkpoint_pc) {
                checkpointActive=true;checkpointPhase=0;captureEpoch=cuts.empty() && userword(m,10092)>=10;
            }
            if(checkpointActive && m.privilege==vm::PrivilegeMode::Kernel) {
                auto [raw,fault]=m.imem.fetch(m.pc);
                if(fault==vm::MemFaultCode::OK) {
                    const auto iw=vm::VersionedInstructionCodec::decode(raw,vm::IsaEncodingVersion::V2);
                    if(iw.opcode==isa::Opcode::CSRW && iw.imm==architecture::v2::CSR_BLOCK_CMD && vm::ops::toLong(m.regfile.read(iw.rd))==4 && captureEpoch) {
                        const auto copy=pressureRun.dir/("before_checkpoint_stage"+std::to_string(checkpointPhase+1)+".tdisk");
                        std::filesystem::copy_file(pressureRun.backing,copy,std::filesystem::copy_options::overwrite_existing);cuts.push_back({copy,userword(m,10092)});
                        check(!readTextFile(copy).empty(),"nonempty actual durable cut before checkpoint stage"+std::to_string(checkpointPhase+1));
                    }
                }
            }
            vm::VMExecutionRecord rec;vm::step(m,&rec);if(!rec.has_instruction) continue;
            if(rec.instruction.opcode==isa::Opcode::SYSCALL && rec.before_privilege==vm::PrivilegeMode::User) {awaiting=true;call=rec.syscall_id;request=rec.syscall_arg2;}
            if(awaiting && rec.instruction.opcode==isa::Opcode::ERET && rec.after_privilege==vm::PrivilegeMode::User) {
                const auto status=vm::ops::toLong(m.regfile.read(13)),payload=vm::ops::toLong(m.regfile.read(14)),detail=vm::ops::toLong(m.regfile.read(15));
                calls<<"{\"id\":"<<call<<",\"status\":"<<status<<",\"payload\":"<<payload<<",\"detail\":"<<detail<<",\"request\":"<<request<<"}\n";awaiting=false;
            }
            if(rec.instruction.opcode==isa::Opcode::CSRW && rec.instruction.imm==architecture::v2::CSR_BLOCK_CMD) {
                const auto cmd=vm::ops::toLong(m.regfile.read(rec.instruction.rd));const auto head=word(m,3005),tail=word(m,3006),durable=word(m,3042);
                if(cmd==2) ++writes;if(cmd==4) ++barriers;
                if(head<lasthead) ++wraps;if(tail!=lasttail) ++epochs;lasthead=head;lasttail=tail;
                trace<<"{\"step\":"<<steps<<",\"cmd\":"<<cmd<<",\"block\":"<<m.block_index<<",\"status\":"<<m.block_status<<",\"head\":"<<head<<",\"tail\":"<<tail<<",\"durable_lsn\":"<<durable<<",\"checkpoint\":"<<word(m,3008)<<"}\n";
                if(word(m,3000)==1 && !checkpointActive) check((729+tail-head)%729>=2 || head==tail,"persistent transaction retains publication slot plus separator");
                if(checkpointActive) {
                    if(cmd==2) {
                        const auto block=m.block_index;
                        if(block>=7289 && block<8018) {
                            std::vector<long long> payload;check(m.block_device.readBlock(static_cast<int>(block),payload),"observed actual WAL payload");
                            if(payload.size()==27 && payload[6]==3) check(checkpointPhase==2 && payload[5]==durable && payload[10]==0,"checkpoint record follows home barrier and names covered durable predecessor");
                            else check(checkpointPhase==0,"DATA/COMMIT persistence precedes home writes");
                        } else if(block==7287 || block==7288) {
                            std::vector<long long> payload;check(m.block_device.readBlock(static_cast<int>(block),payload),"observed actual superblock payload");
                            if(payload.size()==27) {long long sum=17;for(int j=0;j<11;++j)sum+=payload[j]*(j+1);check(sum==payload[11] && payload[7]==729 && payload[8]==27,"checksummed unchanged super layout");
                                if(checkpointPhase<3)check(payload[4]==tail,"prepublication supers retain old tail");else check(payload[4]==head,"final candidate super names reclaimed head");}
                        } else check(checkpointPhase==1,"global committed homes only after WAL barrier and before home barrier");
                    }
                    if(cmd==4) {check(m.block_status==1,"actual successful checkpoint stage barrier");++checkpointPhase;if(checkpointPhase==4)checkpointActive=false;}
                }
            }
            if(!m.trap_active && userword(m,10091)==777) break;
            if(!m.trap_active && userword(m,10000)!=0) break;
        }
        trace.close();calls.close();acknowledged=userword(m,10092);
        recordBoot(pressureRun);check(userword(m,10091)==777,"pressure guest completes all120 writes and exact reads");check(userword(m,10000)==0,"pressure guest semantic assertions code="+std::to_string(userword(m,10000)));
        check(cuts.size()==4,"all four exact checkpoint-stage crash cuts captured");
        check(epochs>=10 && wraps>=3,"multiple actual checkpoint epochs and unchanged ring wrap");check(acknowledged==120,"all120 real writes/fsync acknowledged");check(barriers>120 && writes>729,"independent actual disk/barrier work");
        std::ofstream(pressureRun.dir/"result.json")<<"{\"steps\":"<<steps<<",\"acknowledged\":"<<acknowledged<<",\"epochs\":"<<epochs<<",\"wraps\":"<<wraps<<",\"barriers\":"<<barriers<<",\"writes\":"<<writes<<"}\n";
    }
    // Recovery uses normal kernel_init/vfs_mount and a fresh installed reader.
    // The executable text already on each captured disk is the same SDK guest;
    // a recovered fixture reader is installed into the disk before boot below.
    for(size_t i=0;i<cuts.size();++i) {
        const std::string recovery=readerGuest;
        auto run=makeRun("recovery_"+std::to_string(i),"",recovery,cuts[i].first);
        if(!run.machine) continue;
        auto& m=*run.machine;long long count=0;while(m.isRunning() && count++<120'000'000) {
            vm::step(m);if(!m.trap_active && userword(m,10091)==777) break;
        }
        recordBoot(run);check(userword(m,10091)==777,"fresh kernel mount and normal reader completes");
        check(userword(m,10094)>=(cuts[i].second+1)/2*27 && userword(m,10095)>=cuts[i].second/2*27,"fresh file metadata lengths contain every acknowledged record");
        for(long long n=0;n<cuts[i].second;++n) for(int j=0;j<27;++j)
            check(userword(m,static_cast<int>((n%2==0?11000:13000)+(n/2)*27+j))==n*27+j+31,"fresh guest exact acknowledged prefix "+std::to_string(n)+" word"+std::to_string(j));
        check(word(m,139200)==1 && word(m,139201)==1 && word(m,139202)==3 && word(m,139203)==0,"fresh boot CREATED_TXID exact relation");
        check(word(m,392004)==1 && word(m,392005)==0 && word(m,392006)==1 && word(m,392007)==1,"fresh namespace1 exact active root version");
        check(word(m,391004)==1 && word(m,391005)==9876 && word(m,391007)==2,"fresh quota1 exact ID limit version");
        check(word(m,139206)==1 && word(m,139207)==1 && word(m,139208)==6 && word(m,139209)==1,"fresh boot DESIRED_STATE exact relation");
        std::ofstream(run.dir/"result.json")<<"{\"steps\":"<<count<<",\"acknowledged_prefix_records\":"<<cuts[i].second<<",\"guest_a_words\":"<<userword(m,10094)<<",\"guest_b_words\":"<<userword(m,10095)<<"}\n";

    }

    // Honest no-device semantics are exercised before kernel initialization.
    const std::string noDevice=helpers+R"TRIT(fn wal_test_fixture() -> t40 {
        wal_test_check(vfs_disk_available(),0,601); var tx: t40 = log_begin();
        var wrote: t40 = log_write(tx,namespace_addr(0)+NS_VERSION,0,9); if wrote < 0 { wal_test_check(wrote,1,602); }
        var committed: t40 = log_commit(tx); if committed <= 0 { wal_test_check(committed,1,603); } wal_test_check(wal_sync_to_disk(),ERR_NO_SPACE,604);
        var cleaned: t40 = log_checkpoint(); if cleaned < 0 { wal_test_check(cleaned,0,605); }
        wal_test_check(kload(WAL_TAIL_ADDR),kload(WAL_HEAD_ADDR),606);
        wal_test_check(kload(WAL_DURABLE_LSN_ADDR),0,607); kstore(10002,777); return 1;
    })TRIT";
    auto noDeviceRun=makeRun("no_device",noDevice,"",{});
    if(noDeviceRun.machine) {
        noDeviceRun.machine->resetBlockDevice(192); // labelled absent-device configuration, before any execution
        long long count=0;while(noDeviceRun.machine->isRunning() && count++<30'000'000)vm::step(*noDeviceRun.machine);
        recordBoot(noDeviceRun);check(word(*noDeviceRun.machine,10002)==777 && word(*noDeviceRun.machine,10000)==0,"actual no-device checkpoint never claims durability");
        std::ofstream(noDeviceRun.dir/"result.json")<<"{\"steps\":"<<count<<",\"first_failure\":"<<word(*noDeviceRun.machine,10000)<<"}\n";
    }
    const std::string targeted="fn main() -> t40 { "+paths+
        "var a: t40 = os_open(10300,3); var b: t40 = os_open(10350,3); var i: t40 = 0; while i < 27 { unsafe { store(10400+i,41+i); } i=i+1; } var x: t40 = os_write(a,10400,27); var y: t40 = os_write(b,10400,27);"+
        guestCheck("x","27",701)+guestCheck("y","27",702)+"unsafe { store(10093,1); } var done: t40 = os_fsync(a);"+guestCheck("done","1",703)+"unsafe { store(10093,2); } done=os_fsync(b);"+guestCheck("done","1",704)+"unsafe { store(10091,777); } while i < 999999 { i=i+1; } return 0; }";
    auto targetedRun=makeRun("targeted_fsync","",targeted,{});
    if(targetedRun.machine) {
        auto& m=*targetedRun.machine;long long count=0;bool captured=false;long long aDirty=-1,bDirty=-1;
        while(m.isRunning() && count++<90'000'000) {
            vm::step(m);
            if(!m.trap_active && userword(m,10093)==1 && !captured) {
                aDirty=bDirty=0;const auto a=word(m,290002),b=word(m,290010);
                for(int f=0;f<64;++f) {const int row=130700+f*11;if(word(m,row)>0 && word(m,row+6)>0) {if(word(m,row+2)==a)++aDirty;if(word(m,row+2)==b)++bDirty;}}
                captured=true;check(aDirty>0 && bDirty>0,"two actual inode frames simultaneously dirty before targeted fsync");
            }
            if(!m.trap_active && userword(m,10093)==2) {
                long long afterA=0,afterB=0;const auto a=word(m,290002),b=word(m,290010);
                for(int f=0;f<64;++f) {const int row=130700+f*11;if(word(m,row)>0 && word(m,row+6)>0) {if(word(m,row+2)==a)++afterA;if(word(m,row+2)==b)++afterB;}}
                check(afterA==0 && afterB==bDirty,"targeted fsync clears only requested inode dirty frames");
                std::ofstream(targetedRun.dir/"frames.json")<<"{\"before_a\":"<<aDirty<<",\"before_b\":"<<bDirty<<",\"after_a\":"<<afterA<<",\"after_b\":"<<afterB<<"}\n";break;
            }
        }
        recordBoot(targetedRun);check(captured && userword(m,10093)==2 && userword(m,10000)==0,"targeted fsync actual guest evidence complete");
    }
    const std::string prefix="fn main() -> t40 { "+paths+
        "var fd: t40 = os_open(10300,3); var i: t40 = 0; while i < 900 { unsafe { store(11000+i,73+i); } i=i+1; } var n: t40 = os_write(fd,11000,900); unsafe { store(10096,n); } if n <= 0 { unsafe { store(10000,801); } } if n >= 900 { unsafe { store(10000,802); } } var next: t40 = os_write(fd,11000,27);"+
        guestCheck("next","-11",803)+"var synced: t40 = os_fsync(fd);"+guestCheck("synced","1",804)+
        "unsafe { store(10097,1); } var resumed: t40 = os_write(fd,11000+n,900-n);"+guestCheck("resumed","900-n",805)+
        "unsafe { store(10098,resumed); } synced=os_fsync(fd);"+guestCheck("synced","1",806)+
        "os_close(fd); fd=os_open(10300,0); var got: t40 = os_read(fd,12000,900);"+guestCheck("got","900",807)+
        "i=0; while i < 900 { var v: t40 = 0; unsafe { v=load(12000+i); } "+guestCheck("v","73+i",808)+"i=i+1; } unsafe { store(10091,777); } while i < 999999 { i=i+1; } return 0; }";
    auto prefixRun=makeRun("blocked_prefix","fn wal_test_fixture() -> t40 { var snap: t40 = log_snapshot_acquire(1); kstore(10009,snap); kstore(10010,kload(log_snapshot_row(snap)+SNAP_READ_TXID)); return 1; }",prefix,{});
    if(prefixRun.machine) {
        auto& m=*prefixRun.machine;long long count=0;bool released=false;long long originalOffset=-1;std::ofstream release(prefixRun.dir/"snapshot_release.jsonl");
        while(m.isRunning() && count++<150'000'000) {
            vm::VMExecutionRecord rec;vm::step(m,&rec);
            if(!released && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL && rec.before_privilege==vm::PrivilegeMode::User && rec.syscall_id==15 && userword(m,10097)==1) {
                const auto prefixCount=userword(m,10096),slot=word(m,10009);const int row=28800+static_cast<int>(slot)*3;
                check(slot>=0 && slot<8 && word(m,row)==1 && word(m,row+1)==word(m,10010) && word(m,row+2)==1,"saved real snapshot slot domain PID/readTXID/active identity");
                check(rec.syscall_arg0==0 && rec.syscall_arg1==11000+prefixCount && rec.syscall_arg2==900-prefixCount,"same FD exact remaining source/count real syscall");
                originalOffset=word(m,290003);check(originalOffset==prefixCount,"same original FD offset equals committed prefix before release");
                const auto registers=m.regfile.reg;const auto pc=m.pc;const bool dispatch=m.trap_active;const auto head=word(m,3005),tail=word(m,3006),pid=word(m,3020),fd=word(m,290003);
                const auto diskHash=stableHash(readTextFile(prefixRun.backing));
                release<<"{\"label\":\"approved test-only release at actual resume SYSCALL; no fabricated result\",\"slot\":"<<slot<<",\"before\":["<<word(m,row)<<','<<word(m,row+1)<<','<<word(m,row+2)<<"],\"pc\":"<<pc<<",\"head\":"<<head<<",\"tail\":"<<tail<<",\"fd_offset\":"<<fd<<"}\n";
                if(slot>=0 && slot<8 && word(m,row)==1 && word(m,row+1)==word(m,10010) && word(m,row+2)==1)for(int j=0;j<3;++j)m.dmem.store(row+j,vm::ops::fromLong(0));
                bool sameRegisters=true;for(int r=0;r<isa::REG_COUNT;++r)sameRegisters=sameRegisters && registers[r].mode==m.regfile.reg[r].mode && registers[r].bits.lo==m.regfile.reg[r].bits.lo && registers[r].bits.hi==m.regfile.reg[r].bits.hi;
                check(sameRegisters && m.pc==pc && m.trap_active==dispatch && word(m,3005)==head && word(m,3006)==tail && word(m,3020)==pid && word(m,290003)==fd && stableHash(readTextFile(prefixRun.backing))==diskHash,"release changes only snapshot words; PC/registers/returns/WAL/FD/process/native dispatch/backing unchanged");
                release<<"{\"after\":["<<word(m,row)<<','<<word(m,row+1)<<','<<word(m,row+2)<<"],\"unchanged_registers\":"<<(sameRegisters?"true":"false")<<"}\n";released=true;
            }
            if(!m.trap_active && userword(m,10091)==777)break;
        }
        release.close();recordBoot(prefixRun);const auto committed=userword(m,10096);check(userword(m,10091)==777 && userword(m,10000)==0,"real blocked admission produces short prefix and explicit busy");
        check(committed>0 && committed<900 && originalOffset==committed && released,"exact original FD prefix then one labelled release");
        check(userword(m,10098)==900-committed && word(m,290003)==900,"actual same-FD resumed write and final reader offset900");
        const auto inode=word(m,290002);check(word(m,150000+static_cast<int>(inode)*8+2)==900,"inode size equals complete resumed900");
        long long found=0;for(int e=0;e<word(m,3017);++e) {const int row=265000+e*6;if(word(m,row+1)==inode && word(m,row+5)>0)for(int j=0;j<word(m,row+3);++j) {const auto offset=word(m,row+2)+j;if(offset<900) {check(word(m,static_cast<int>(word(m,row+4)+j))==73+offset,"exact prefix home word");++found;}}}
        check(found==900,"complete900 unique resumed words/extent coverage observed");check((729+word(m,3006)-word(m,3005))%729>=2,"blocked prefix preserves publication reserve");
        std::ofstream(prefixRun.dir/"result.json")<<"{\"steps\":"<<count<<",\"committed_prefix\":"<<committed<<",\"fd_offset\":"<<word(m,290003)<<",\"found_words\":"<<found<<"}\n";
        const auto prefixCopy=prefixRun.dir/"acknowledged_resumed.tdisk";std::filesystem::copy_file(prefixRun.backing,prefixCopy,std::filesystem::copy_options::overwrite_existing);
        auto fresh=makeRun("resumed_prefix_recovery","",readerGuest,prefixCopy);
        if(fresh.machine) {
            long long freshSteps=0;while(fresh.machine->isRunning() && freshSteps++<120'000'000) {vm::step(*fresh.machine);if(!fresh.machine->trap_active && userword(*fresh.machine,10091)==777)break;}
            recordBoot(fresh);check(userword(*fresh.machine,10091)==777 && userword(*fresh.machine,10094)==900,"fresh normal kernel reads complete acknowledged resumed900");
            for(int j=0;j<900;++j)check(userword(*fresh.machine,11000+j)==73+j,"fresh recovered resumed unique word"+std::to_string(j));
            std::ofstream(fresh.dir/"result.json")<<"{\"steps\":"<<freshSteps<<",\"guest_words\":"<<userword(*fresh.machine,10094)<<"}\n";
        }

    }
    // Actual durable orphan/broken-chain images recover via normal kernel_init,
    // not an oracle replay or volatile snapshot restore.
    for(const auto& mode:std::vector<std::string>{"orphan","torn_chain"}) {
        std::string fixture=helpers+"fn wal_test_fixture() -> t40 { namespace_create(1,0); quota_set_limit(1,9876); log_checkpoint(); var tx: t40 = log_begin(); var slot: t40 = log_write(tx,namespace_addr(1)+NS_VERSION,1,77); ";
        if(mode=="torn_chain") fixture+="log_commit(tx); kstore(wal_record_addr(slot)+WAL_PREV_LSN,999); wal_record_finalize(slot);";
        fixture+="wal_sync_to_disk(); kstore(10002,777); return 1; }";
        auto run=makeRun("replay_"+mode,fixture,"",{});if(!run.machine)continue;
        long long count=0;while(run.machine->isRunning() && count++<60'000'000)vm::step(*run.machine);
        recordBoot(run);check(word(*run.machine,10002)==777,mode+" original real kernel disk generation completed");
        const auto copy=run.dir/"durable_crash.tdisk";std::filesystem::copy_file(run.backing,copy,std::filesystem::copy_options::overwrite_existing);
        const std::string recoveryFixture=helpers+"fn wal_test_fixture() -> t40 { wal_test_check(kload(namespace_addr(1)+NS_VERSION),1,901); wal_test_check(kload(quota_addr(1)+QUOTA_LIMIT),9876,902); kstore(10002,777); return 1; }";
        auto fresh=makeRun("recovered_"+mode,recoveryFixture,"",copy);if(!fresh.machine)continue;
        count=0;while(fresh.machine->isRunning() && count++<60'000'000)vm::step(*fresh.machine);
        recordBoot(fresh);check(word(*fresh.machine,10002)==777 && word(*fresh.machine,10000)==0,"fresh actual kernel rejects "+mode+" replay while preserving acknowledged ns/quota");
        std::ofstream(fresh.dir/"result.json")<<"{\"steps\":"<<count<<",\"first_failure\":"<<word(*fresh.machine,10000)<<",\"namespace_version\":"<<word(*fresh.machine,392006)<<"}\n";
    }
    // Existing-path final publication Red and real sharing-denial proof.
    const std::string publication=helpers+R"TRIT(
fn wal_test_fixture() -> t40 {
    wal_test_seed(); var seeded: t40 = log_checkpoint();
    if seeded < 0 { wal_test_check(seeded,1,508); return seeded; }
    wal_test_check(kload(WAL_TAIL_ADDR),kload(WAL_HEAD_ADDR),509);
    var fill: t40 = log_begin(); if fill < 0 { wal_test_check(fill,1,510); return fill; }
    var i: t40 = 0;
    while i < 726 {
        var expected: t40 = kload(WAL_HEAD_ADDR);
        var wrote: t40 = log_write(fill,namespace_addr(1)+NS_VERSION,53,53);
        wal_test_check(wrote,expected,511);
        if wrote < 0 { return wrote; } i=i+1;
    }
    var committed: t40 = log_commit(fill);
    if committed <= 0 { wal_test_check(committed,1,512); return committed; }
    wal_test_check(kload(wal_tx_tracker_key_addr(wal_tx_tracker_slot(fill))),0,513);
    var slot: t40 = kload(WAL_TAIL_ADDR); var previous: t40 = 0; i=0;
    while i < 727 {
        wal_test_check(wal_record_valid(slot),1,514);
        var rec: t40 = wal_record_addr(slot);
        wal_test_check(kload(rec+WAL_TXID),fill,515);
        wal_test_check(kload(rec+WAL_PREV_LSN),previous,516);
        if i < 726 {
            wal_test_check(kload(rec+WAL_TYPE),WAL_DATA,517);
            wal_test_check(kload(rec+WAL_COUNT),1,518);
            wal_test_check(kload(rec+WAL_OFFSET),namespace_addr(1)+NS_VERSION,519);
            wal_test_check(kload(rec+WAL_HEADER_WORDS),53,520);
        } else {
            wal_test_check(kload(rec+WAL_TYPE),WAL_COMMIT,521);
            wal_test_check(kload(rec+WAL_LSN),committed,522);
        }
        previous=kload(rec+WAL_LSN);slot=kwrap_inc(slot,WAL_CAPACITY);i=i+1;
    }
    wal_test_check(slot,kload(WAL_HEAD_ADDR),523);
    wal_test_check(wal_used_blocks(), 727, 500); kstore(10003, 1); var oldtail: t40 = kload(WAL_TAIL_ADDR);
    var oldcheckpoint: t40 = kload(WAL_CHECKPOINT_ADDR);
    var result: t40 = log_checkpoint(); kstore(10004,result);
    wal_test_check(result, ERR_INVALID, 501);
    wal_test_check(kload(WAL_TAIL_ADDR), oldtail, 502);
    wal_test_check(kload(WAL_CHECKPOINT_ADDR), oldcheckpoint, 503);
    var retryhead: t40 = kload(WAL_HEAD_ADDR); var retrylsn: t40 = kload(WAL_NEXT_LSN_ADDR); kstore(10003,2); var retry: t40 = log_checkpoint();
    if kload(10005) >= 3 { wal_test_check(kload(WAL_HEAD_ADDR), retryhead, 506); wal_test_check(kload(WAL_NEXT_LSN_ADDR), retrylsn, 507); }
    if retry < 0 { wal_test_check(retry, 1, 504); }
    wal_test_check(kload(WAL_TAIL_ADDR), kload(WAL_HEAD_ADDR), 505);
    kstore(10002,777); return 1;
}
)TRIT";
    for(int phase=1;phase<=6;++phase) {
        const int faultPhase=phase>=5?1:phase;
        auto phaseFixture=publication;const auto marker=phaseFixture.find("wal_test_seed();");phaseFixture.insert(marker,"kstore(10005,"+std::to_string(faultPhase)+"); ");
        if(phase>=5) {
            const auto resultAt=phaseFixture.find("var result: t40 = log_checkpoint();");
            phaseFixture.replace(resultAt,std::string("var result: t40 = log_checkpoint();").size(),"var admissionhead: t40 = kload(WAL_HEAD_ADDR); var admissionnext: t40 = kload(WAL_NEXT_TXID_ADDR); var admissionstamp: t40 = wal_test_ring_stamp(); var result: t40 = wal_prepare_admission(3);");
            const auto retryAt=phaseFixture.find("kstore(10003,2); var retry: t40 = log_checkpoint();");
            phaseFixture.replace(retryAt,std::string("kstore(10003,2); var retry: t40 = log_checkpoint();").size(),
                "wal_test_check(kload(WAL_HEAD_ADDR),admissionhead,531); wal_test_check(kload(WAL_NEXT_TXID_ADDR),admissionnext,532); wal_test_check(wal_test_ring_stamp(),admissionstamp,539); wal_test_check(kload(namespace_addr(1)+NS_VERSION),53,540); var owner: t40 = 0; while owner < 243 { wal_test_check(kload(WAL_TX_TRACKER_KEY_BASE+owner),0,533); wal_test_check(kload(679486+owner),0,534); owner=owner+1; } kstore(10003,2); var retry: t40 = wal_prepare_admission(3); wal_test_check(retry,1,535); wal_test_check(kload(WAL_NEXT_TXID_ADDR),admissionnext,536);");
        }
        if(phase==6) {
            auto replaceAll=[&](const std::string& from,const std::string& to){size_t at=0;while((at=phaseFixture.find(from,at))!=std::string::npos){phaseFixture.replace(at,from.size(),to);at+=to.size();}};
            replaceAll("i < 726","i < 242");replaceAll("i < 727","i < 243");
            replaceAll("wal_test_check(wal_used_blocks(), 727, 500);","wal_test_check(wal_used_blocks(), 243, 500); wal_test_check(log_capacity_for(3),1,537); wal_test_check(wal_free_blocks(),485,538);");
        }
        auto run=makeRun(phase==6?"admission_fitting_io_failure":phase==5?"admission_io_failure":"publication_failure_"+std::to_string(phase),phaseFixture,"",{});if(!run.machine) continue;
        auto& m=*run.machine;int phasebarriers=0;bool injected=false,rejected=false,unchanged=false; std::filesystem::path failedCopy;std::string before;long long count=0;
#ifdef _WIN32
        HANDLE held=INVALID_HANDLE_VALUE;
#endif
        while(m.isRunning() && count++<60'000'000) {
            bool target=false;
            if(m.privilege==vm::PrivilegeMode::Kernel) {
                auto [raw,fault]=m.imem.fetch(m.pc);
                if(fault==vm::MemFaultCode::OK) {
                    const auto iw=vm::VersionedInstructionCodec::decode(raw,vm::IsaEncodingVersion::V2);
                    if(iw.opcode==isa::Opcode::CSRW && iw.imm==architecture::v2::CSR_BLOCK_CMD && vm::ops::toLong(m.regfile.read(iw.rd))==4 && word(m,10003)==1) target=++phasebarriers==faultPhase;
                }
            }
            if(target) {
                before=readTextFile(run.backing);
#ifdef _WIN32
                held=CreateFileW(run.backing.wstring().c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                injected=held!=INVALID_HANDLE_VALUE;check(injected,"actual Windows sharing denial acquired phase"+std::to_string(phase));
#else
                check(false,"actual sharing-denial fixture requires current Windows host");
#endif
            }
            vm::VMExecutionRecord rec;vm::step(m,&rec);
            if(target) {
                rejected=m.block_status==-1;
#ifdef _WIN32
                if(held!=INVALID_HANDLE_VALUE) {CloseHandle(held);held=INVALID_HANDLE_VALUE;}
#endif
                unchanged=readTextFile(run.backing)==before;
                failedCopy=run.dir/"failed_durable.tdisk";std::filesystem::copy_file(run.backing,failedCopy,std::filesystem::copy_options::overwrite_existing);
                check(rejected && unchanged,"phase"+std::to_string(phase)+" actual flush rejects with byte-identical durable live backing");
                std::ofstream(run.dir/"fault.json")<<"{\"injected\":"<<(injected?"true":"false")<<",\"actual_status\":"<<m.block_status<<",\"live_unchanged\":"<<(unchanged?"true":"false")<<",\"before_hash\":\""<<stableHash(before)<<"\",\"after_hash\":\""<<stableHash(readTextFile(run.backing))<<"\",\"temporary_compact_work\":\"separate from unchanged live backing\"}\n";
            }
        }
        recordBoot(run);check(injected && rejected && unchanged,"real phase"+std::to_string(phase)+" fault viability");check(word(m,10002)==777,"publication retry fixture completion");check(word(m,10000)==0,"publication failure semantic code="+std::to_string(word(m,10000)));
        std::ofstream(run.dir/"result.json")<<"{\"steps\":"<<count<<",\"first_failure\":"<<word(m,10000)<<",\"checks\":"<<word(m,10001)<<"}\n";
        if(!failedCopy.empty()) {
            const auto recoveredFixture=helpers+R"TRIT(fn wal_test_fixture() -> t40 {
                wal_test_check(kload(namespace_addr(1)+NS_ID),1,951);
                wal_test_check(kload(namespace_addr(1)+NS_ROOT_INODE),0,952);
                wal_test_check(kload(namespace_addr(1)+NS_VERSION),53,953);
                wal_test_check(kload(namespace_addr(1)+NS_ACTIVE),1,954);
                wal_test_check(kload(quota_addr(1)+QUOTA_ID),1,955);
                wal_test_check(kload(quota_addr(1)+QUOTA_LIMIT),9876,956);
                wal_test_check(kload(quota_addr(1)+QUOTA_VERSION),2,957);
                kstore(10002,777); return 1;
            })TRIT";
            auto fresh=makeRun("failed_publication_recovery_"+std::to_string(phase),recoveredFixture,"",failedCopy);
            if(fresh.machine) {
                long long freshSteps=0;while(fresh.machine->isRunning() && freshSteps++<90'000'000)vm::step(*fresh.machine);
                recordBoot(fresh);check(word(*fresh.machine,10002)==777 && word(*fresh.machine,10000)==0,"fresh normal kernel recovers exact acknowledged namespace/quota from failed phase"+std::to_string(phase));
                std::ofstream(fresh.dir/"result.json")<<"{\"steps\":"<<freshSteps<<",\"first_failure\":"<<word(*fresh.machine,10000)<<",\"namespace_version\":"<<word(*fresh.machine,392006)<<"}\n";
            }
        }

    }
    std::ofstream report(base/"report.json");report<<"{\"status\":\""<<(failures.empty()?"PASS":"FAIL")<<"\",\"workflow\":\"wal-e2e\",\"validation\":\"current-source\",\"scope\":\"full\",\"guard_scenario_count\":"<<guard_cases.size()<<",\"selected_guard_cases\":[";
    for(size_t i=0;i<guard_cases.size();++i) {if(i)report<<',';report<<'"'<<guard_cases[i].first<<'"';} report<<"],\"failures\":[";
    for(size_t i=0;i<failures.size();++i) {if(i)report<<',';report<<'"'<<jsonEscape(failures[i])<<'"';}report<<"]}\n";report.close();return failures.empty();
}

bool runSdkWorkflow() {
    using namespace sandbox;
    const auto artifact = std::filesystem::path(TRIT_TEST_BUILD_DIR) / "service_workflow_artifact" / "sdk";
    std::filesystem::create_directories(artifact);
    std::vector<std::string> failures;
    auto check = [&](bool value, const std::string& label) {
        if (!value) { failures.push_back(label); require(false, "SDK: " + label); }
        return value;
    };
    auto finish = [&]() {
        std::ofstream out(artifact / "sdk_report.json");
        out << "{\"status\":\"" << (failures.empty() ? "PASS" : "FAIL") << "\",\"failures\":[";
        for (size_t i=0;i<failures.size();++i) { if(i) out << ','; out << '"' << jsonEscape(failures[i]) << '"'; }
        out << "]}\n";
        return failures.empty();
    };
    std::string guest = R"TRIT(
fn sdk_heap_check(actual: t40, expected: t40) -> t40 {
    unsafe {store(10094,load(10094)+1);if actual!=expected {store(10093,load(10093)+1);}}return 1;
}
fn sdk_heap_fill(base: t40, words: t40, left: t40, right: t40) -> t40 {
    unsafe {store(base,left);store(base+words-1,right);}
    var j: t40=1;while j<words-1 {var value: t40=73+j;
        unsafe {if tmod(j,2)==1 {value=j+1/3;}}unsafe {store(base+j,value);}j=j+1;}return 1;
}
fn sdk_heap_verify(base: t40, words: t40, left: t40, right: t40) -> t40 {
    unsafe {sdk_heap_check(load(base),left);sdk_heap_check(load(base+words-1),right);}
    var j: t40=1;while j<words-1 {var expected: t40=73+j;
        unsafe {if tmod(j,2)==1 {expected=j+1/3;}}unsafe {sdk_heap_check(load(base+j),expected);}j=j+1;}return 1;
}
fn main() -> t40 {
    unsafe {store(10092,0);store(10093,0);store(10094,0);}
)TRIT";
    // Eighteen independent runtime-derived values remain live through every VFS call.
    for (int i=0;i<18;++i) {
        guest += "unsafe { store(10090, " + std::to_string(100+i) + "); }\nlet live" + std::to_string(i) +
            ": t40 = sys_getpid() + " + std::to_string(i) + ";\n";
    }
    const std::string file_path="/tmp/sdk-roundtrip", missing_path="/tmp/sdk-absent";
    for (const auto& entry : std::vector<std::pair<int,std::string>>{{10300,file_path},{10350,missing_path}}) {
        for(size_t i=0;i<=entry.second.size();++i)
            guest += "unsafe { store(" + std::to_string(entry.first+i) + ", " +
                std::to_string(i==entry.second.size()?0:static_cast<unsigned char>(entry.second[i])) + "); }\n";
    }
    guest += "var i: t40 = 0; while i < 37 { unsafe { store(10400 + i, 31 + i * 7); store(10500 + i, 999); } i = i + 1; } i = 37; while i < 64 { unsafe { store(10500 + i, 999); } i = i + 1; }\n";
    const std::vector<std::string> names={"missing_open","invalid_write","invalid_fsync","first_fd0","write37",
        "fsync","close_success","close_invalid","reopen_fd0","bad_output_span","short_read37_of64","eof0","read_close","spawn_missing_executable","heap_query_initial","heap_grow1495","heap_query_first","heap_grow748","heap_exhaust60000","heap_query_final"};
    const std::vector<int> services={12,15,47,12,15,47,13,13,12,14,14,14,13,57,19,19,19,19,19,19};
    const std::vector<std::string> calls={"os_open(10350, 0)","os_write(-1, 10400, 37)","os_fsync(-1)",
        "os_open(10300, 3)","os_write(fd, 10400, 37)","os_fsync(fd)","os_close(fd)","os_close(fd)",
        "os_open(10300, 0)","os_read(rd, -1, 64)","os_read(rd, 10500, 64)","os_read(rd, 10600, 64)","os_close(rd)","os_spawn_app(10350, 2, 3)","sys_sbrk(0)","sys_sbrk(1495)","sys_sbrk(0)","sys_sbrk(748)","sys_sbrk(60000)","sys_sbrk(0)"};
    for(size_t i=0;i<calls.size();++i) {
        guest += "unsafe { store(10090, " + std::to_string(i) + "); }\n";
        const std::string variable=i==3?"fd":i==8?"rd":"result"+std::to_string(i);
        guest += "let " + variable + ": t40 = " + calls[i] + "; unsafe { store(" + std::to_string(10000+i) + ", " + variable + "); }\n";
        if(i==16) guest += R"TRIT(
    // Guard every use: result-selection failure must finish calls without touching pointer1.
    if result14>2048 {if result15==result14 {if result16==result14+1495 {
        if result16<9477 {sdk_heap_fill(result15,1495,-91,-92);unsafe {store(10092,1);}}
    }}}
)TRIT";
        if(i==17) guest += R"TRIT(
    if result14>2048 {if result15==result14 {if result16==result14+1495 {
        if result17==result16 {if result17+748<9477 {
            sdk_heap_verify(result15,1495,-91,-92);
            sdk_heap_fill(result17,748,-93,-94);
            sdk_heap_verify(result17,748,-93,-94);
        }}
    }}}
)TRIT";
        if(i==19) guest += R"TRIT(
    if result14>2048 {if result15==result14 {if result16==result14+1495 {
        if result17==result16 {if result19==result17+748 {if result19<9477 {
            if result18==-3 {
                // Both spans were already written before exhaustion; now verify preservation.
                sdk_heap_verify(result15,1495,-91,-92);
                sdk_heap_verify(result17,748,-93,-94);
                unsafe {store(10092,2);}
            }
        }}}
    }}}
)TRIT";
    }
    guest += "unsafe {\n";
    for(int i=0;i<18;++i) guest += "store(" + std::to_string(10100+i) + ", live" + std::to_string(i) + ");\n";
    guest += "store(10130, ";
    for(int i=0;i<18;++i) { if(i) guest += " + "; guest += "live"+std::to_string(i); }
    guest += "); store(10091, 777); } while i < 999999 { i = i + 1; } return 0; }\n";
    std::ofstream(artifact / "embedded_guest.trit") << guest;
    std::ofstream hashes(artifact / "source_hashes.json");
    hashes << "{\"algorithm\":\"FNV-1a-64\",\"sources\":{";
    const std::vector<std::string> sources={"kernel.trit","native_kernel_trap_stub.tasm","ternary_vm.h","ternary_vm_state.h",
        "ternary_compiler_codegen.h","ternary_compiler_ir.h","apps/os_sdk.trit","architecture_contract.trit","tests/current_only_conformance.cpp"};
    for(size_t i=0;i<sources.size();++i) {
        const auto content=readTextFile(sources[i]); check(!content.empty(),"readable hash source "+sources[i]);
        if(i) hashes << ','; hashes << '"' << sources[i] << "\":\"" << stableHash(content) << '"';
    }
    hashes << "}}\n"; hashes.close();
    auto diagnostics = [&](const auto& result, const std::string& stage) {
        std::ofstream out(artifact / (stage + ".txt"));
        for(const auto& d:result.diagnostics) out << d.format() << '\n';
    };
    compiler::CompilerOptions opts;
    opts.enable_vector_abi = true; opts.enable_vector_spilling = true;
    const auto app = compiler::compileSource("sdk_guest.trit",
        readTextFile("architecture_contract.trit") + "\n" + readTextFile("apps/os_sdk.trit") + "\n" + guest, opts);
    diagnostics(app, "guest_compile");
    if(!check(app.success, "AC1 compiled SDK sdk guest")) return finish();
    compiler::LinkOptions lo;
    lo.stack_hint_words = 2048; lo.dead_strip_functions = true;
    lo.standalone_halt_on_exit = false; lo.enable_vector_abi = true; lo.enable_vector_spilling = true;
    const auto linked = compiler::linkModules({app.object}, lo);
    diagnostics(linked, "guest_link");
    if(!check(linked.success, "AC1 guest links")) return finish();
    std::ofstream(artifact / "guest.linked.tasm") << linked.assembly;
    const auto kernel = compiler::compileSource("sdk_kernel.trit",
        readTextFile("architecture_contract.trit") + "\n" + readTextFile("kernel.trit"));
    diagnostics(kernel, "kernel_compile");
    if(!check(kernel.success, "AC4 real kernel compiles")) return finish();
    os::NativeVfsImageBuilder fs(32768);
    check(fs.installBaseLayout().ok(), "sdk rootfs layout");
    check(fs.addExecutableImage("/bin/sdk", linked.assembled.program,
        linked.executable_header_v3, 256).ok(), "sdk rootfs executable");
    std::string boot = ".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n mov r13, 0\n mov r14, 3\n call process_set_state\n mov r1, native_trap_entry\n csrw tvec, r1\n";
    const std::string path = "/bin/sdk";
    for(size_t i=0;i<=path.size();++i) boot += " mov r1, " + std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i])) + "\n mov r2, " + std::to_string(10020+i) + "\n store r1, r2, 0\n";
    boot += " mov r13, 1\n mov r14, 10020\n mov r15, 1\n mov r16, 2\n call app_spawn\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
    boot += readTextFile("native_kernel_trap_stub.tasm") + "\n" + kernel.assembly;
    std::ofstream(artifact / "boot.tasm") << boot;
    auto feature_word=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
    auto boot_source=[&](int words) { return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(words)+", 0, 27, "+std::to_string(feature_word)+", 2, 0\n"; };
    auto assembled = vm::assembler::assembleV2(boot_source(1));
    feature_word=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
    assembled=vm::assembler::assembleV2(boot_source(1));
    if(assembled.success) assembled=vm::assembler::assembleV2(boot_source(static_cast<int>(assembled.program.size())));
    std::ofstream(artifact/"boot.tasm") << boot_source(static_cast<int>(assembled.program.size()));
    if(!assembled.success) {
        std::ofstream out(artifact / "boot_assembly.txt");
        for(const auto& e:assembled.errors) out << e.format() << '\n';
    }
    if(!check(assembled.success, "real kernel/trap assembly")) return finish();
    host::TosImageManifest manifest;
    auto image = host::bootImageFromAssembly(assembled, manifest, fs.image());
    std::string error;
    check(host::writeBootImageFile((artifact / "sdk.tboot").string(), image, &error), "sdk tboot: " + error);
    auto machine = std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    if(!check(host::loadBootImageIntoVm(*machine, image, "", &error), "sdk VM load: " + error)) return finish();
    auto word = [](const vm::VMState& m, int addr) { return host::detail::dmemWord(m,addr); };
    auto userword = [&](const vm::VMState& m, int addr) {
        const auto ctx = word(m,390000+9+5);
        const auto pt = word(m,static_cast<int>(ctx)+4);
        const auto pte = word(m,static_cast<int>(pt)+addr/vm::MMU_PAGE_WORDS);
        return word(m,static_cast<int>(pte/19683*vm::MMU_PAGE_WORDS)+addr%vm::MMU_PAGE_WORDS);
    };
    // Read exact physical words through actual SDK slot1 HW mappings.
    auto uservalue = [&](const vm::VMState& m,int addr) {
        const auto ctx=word(m,390000+9+5),pt=word(m,static_cast<int>(ctx)+4);
        const auto pte=word(m,static_cast<int>(pt)+addr/vm::MMU_PAGE_WORDS);
        return m.dmem.load(static_cast<int>(pte/19683*vm::MMU_PAGE_WORDS)+addr%vm::MMU_PAGE_WORDS).first;
    };
    auto exact = [](std::ostream& out,const vm::TernaryValue& value){out<<"{\"mode\":"<<static_cast<int>(value.mode)<<",\"lo\":\""<<value.bits.lo<<"\",\"hi\":\""<<value.bits.hi<<"\"}";};
    struct HeapSnapshot {std::array<long long,4> heap{};std::array<vm::TernaryValue,512> ptes{};long long pt=0,ctx=0;};
    auto heap_snapshot = [&](const vm::VMState& m){HeapSnapshot s;
        for(int i=0;i<4;++i)s.heap[i]=word(m,668004+i);
        s.ctx=word(m,390000+9+5);s.pt=word(m,static_cast<int>(s.ctx)+4);
        for(int i=0;i<512;++i)s.ptes[i]=m.dmem.load(static_cast<int>(s.pt)+i).first;return s;};
    auto same_word=[](const vm::TernaryValue&a,const vm::TernaryValue&b){return a.mode==b.mode&&a.bits.lo==b.bits.lo&&a.bits.hi==b.bits.hi;};
    auto same_pte=[&](const HeapSnapshot&a,const HeapSnapshot&b,int lo,int hi){bool equal=true;
        for(int i=lo;i<hi;++i)equal &= same_word(a.ptes[i],b.ptes[i]);return equal;};
    const auto initial_break=((std::max(linked.executable_header_v3.data_words,linked.executable_header_v3.stack_words)+vm::MMU_PAGE_WORDS-1)/vm::MMU_PAGE_WORDS)*vm::MMU_PAGE_WORDS;
    std::map<int,HeapSnapshot> heap_entries,heap_returns;
    std::ofstream heap_receipts(artifact/"heap_metadata.jsonl");
    check(heap_receipts.good(),"heap receipt opened");
    auto heap_write=[&](int caller,const char*phase,const HeapSnapshot&s){heap_receipts<<"{\"caller\":"<<caller<<",\"phase\":\""<<phase<<"\",\"linked_data_words\":"<<linked.executable_header_v3.data_words<<",\"linked_stack_words\":"<<linked.executable_header_v3.stack_words<<",\"derived_initial_break\":"<<initial_break<<",\"context\":"<<s.ctx<<",\"ptbr\":"<<s.pt<<",\"heap\":[";
        for(int i=0;i<4;++i){if(i)heap_receipts<<',';heap_receipts<<s.heap[i];}heap_receipts<<"],\"ptes_exact\":[";
        for(int i=0;i<512;++i){if(i)heap_receipts<<',';exact(heap_receipts,s.ptes[i]);}heap_receipts<<"]}\n";};
    struct Raw { int service=-1, caller=-1, process_pid=-1, return_pid=-1, pc=-1, return_pc=-1; std::array<long long,4> args{}; long long status=0,payload=0,detail=0; };
    std::map<int,Raw> records;
    Raw pending;
    bool awaiting=false, reached=false;
    std::ofstream raw(artifact/"raw_return_records.jsonl");
    for(int n=0;n<30'000'000;++n) {
        vm::VMExecutionRecord rec;
        vm::step(*machine,&rec);
        if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL && rec.before_privilege==vm::PrivilegeMode::User) {
            check(!awaiting,"previous syscall reached ERET before next call");
            pending={}; pending.service=rec.syscall_id; pending.pc=rec.pc;
            pending.caller=static_cast<int>(userword(*machine,10090));
            pending.process_pid=static_cast<int>(word(*machine,3020));
            check(pending.process_pid==2,"syscall entry actual caller PID2");
            pending.args={rec.syscall_arg0,rec.syscall_arg1,rec.syscall_arg2,rec.syscall_arg3}; awaiting=true;
            if(pending.service==19){heap_entries[pending.caller]=heap_snapshot(*machine);heap_write(pending.caller,"actual_User_entry",heap_entries.at(pending.caller));}
        }
        // ERET has executed, and no generated user result-selection instruction has run yet.
        if(awaiting && rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET && rec.after_privilege==vm::PrivilegeMode::User) {
            pending.return_pid=static_cast<int>(word(*machine,3020));
            check(pending.return_pid==2,"syscall ERET actual caller PID2");
            pending.status=vm::ops::toLong(machine->regfile.read(13));
            pending.payload=vm::ops::toLong(machine->regfile.read(14));
            pending.detail=vm::ops::toLong(machine->regfile.read(15)); pending.return_pc=machine->pc;
            check(pending.return_pc==pending.pc+1,"ERET resumes immediately after originating syscall");
            check(!records.count(pending.caller),"unique caller mapping"); records[pending.caller]=pending;
            raw << "{\"caller\":\"" << (pending.caller>=100?"live"+std::to_string(pending.caller-100):names.at(pending.caller))
                << "\",\"process_pid\":" << pending.process_pid << ",\"return_pid\":" << pending.return_pid << ",\"caller_index\":" << pending.caller << ",\"syscall_id\":" << pending.service << ",\"syscall_pc\":" << pending.pc
                << ",\"eret_pc\":" << rec.pc << ",\"resume_pc\":" << pending.return_pc << ",\"args\":[";
            for(int i=0;i<4;++i) { if(i) raw << ','; raw << pending.args[i]; }
            raw << "],\"r13_status\":" << pending.status << ",\"r14_payload\":" << pending.payload << ",\"r15_detail\":" << pending.detail << "}\n";
            if(pending.service==19){heap_returns[pending.caller]=heap_snapshot(*machine);heap_write(pending.caller,"actual_ERET",heap_returns.at(pending.caller));}
            awaiting=false;
        }
        if(!machine->trap_active && userword(*machine,10091)==777) { reached=true; break; }
        if(machine->status!=vm::VMStatus::RUNNING && machine->status!=vm::VMStatus::WAITING) break;
    }
    raw.close();
    check(reached,"guest completed all calls and live-value stores pc="+std::to_string(machine->pc)+" status="+std::to_string(static_cast<int>(machine->status))+" cause="+std::to_string(machine->cause));
    if(!reached) (void)host::detail::writeCheckpointState(artifact/"failed_vm_state.bin",*machine,&error);
    check(!awaiting && records.size()==38,"all 38 actual syscall ERET returns captured");
    std::ofstream results(artifact/"sdk_results.json"); results << "{\"results\":[";
    for(size_t i=0;i<names.size();++i) {
        if(i) results << ',';
        const auto actual=userword(*machine,10000+static_cast<int>(i));
        results << "{\"caller\":\"" << names[i] << "\",\"caller_index\":" << i << ",\"sdk_result\":" << actual;
        if(records.count(static_cast<int>(i))) {
            const auto& r=records.at(static_cast<int>(i));
            check(r.service==services[i],names[i]+" actual service ID");
            const bool payload=i!=6 && i!=7 && i!=12;
            const auto expected=payload?(r.status<0?(r.detail<0?r.detail:r.status):r.payload):r.status;
            results << ",\"expected\":" << expected;
            check(actual==expected,names[i]+" SDK result matches raw status/detail/payload selection");
            if(i==0 || i==1 || i==2 || i==9) check(r.status==-1 && r.payload==0 && r.detail<0,names[i]+" genuine negative-detail error");
            if(i==13) check(r.status==-1 && r.payload==0 && r.detail==-1 && actual==-1,"real spawn missing executable canonical negative-detail tuple");
        } else check(false,names[i]+" raw record present");
        results << '}';
    }
    results << "],\"live_values\":[";
    for(int i=0;i<18;++i) {
        const auto value=userword(*machine,10100+i); if(i) results << ','; results << value;
        check(value==2+i,"runtime live value "+std::to_string(i)+" preserved");
    }
    const auto checksum=userword(*machine,10130);
    results << "],\"live_checksum\":" << checksum << "}\n"; results.close();
    heap_receipts.flush();check(heap_receipts.good(),"heap metadata exact records written");heap_receipts.close();
    const std::array<long long,6> deltas={0,1495,0,748,60000,0};
    const std::array<long long,6> expected_old={initial_break,initial_break,initial_break+1495,initial_break+1495,0,initial_break+2243};
    const std::array<long long,6> expected_breaks={initial_break,initial_break+1495,initial_break+1495,initial_break+2243,initial_break+2243,initial_break+2243};
    check(initial_break>=2048 && initial_break+2243<13*vm::MMU_PAGE_WORDS,"linked heap spans disjoint from stack and scratch");
    for(int j=0;j<6;++j){const int caller=14+j;
        if(!records.count(caller)||!heap_entries.count(caller)||!heap_returns.count(caller)){check(false,"heap actual entry/return present");continue;}
        const auto&r=records.at(caller);const auto&a=heap_entries.at(caller);const auto&b=heap_returns.at(caller);
        check(r.service==19&&r.args[0]==deltas[j],"normal compiled unique heap service19 request");
        check(a.heap[0]==initial_break&&b.heap[0]==initial_break&&a.heap[2]==74*vm::MMU_PAGE_WORDS&&b.heap[2]==a.heap[2],"heap actual initial/limit from header and kernel");
        check(r.status==(j==4?-1:1)&&r.payload==expected_old[j]&&r.detail==(j==4?-3:0),"actual canonical heap tuple");
        check(b.heap[1]==expected_breaks[j],"actual break after query/growth/exhaustion");
        check(b.heap[3]==(expected_breaks[j]+vm::MMU_PAGE_WORDS-1)/vm::MMU_PAGE_WORDS,"mapped heap page count");
        check(same_pte(a,b,13,22),"ordinary scratch mappings preserved");
        if(j==0||j==2||j==4||j==5)check(a.heap==b.heap&&same_pte(a,b,0,512),"query/exhaust preserves all heap words and512PTEs");
        for(int vpn=0;vpn<b.heap[3];++vpn){vm::PageTableEntry pte;
            const bool decoded=vm::decodePageTableEntry(b.ptes[vpn],pte);
            check(decoded&&pte.present&&pte.user&&pte.read&&pte.write&&!pte.execute&&!pte.global&&!pte.superpage&&pte.ppn==1000+192+vpn,"actual canonical slot1 private backing and data permissions; dynamic accessed/dirty preserved");}
    }
    const auto memory_status=userword(*machine,10092),memory_failures=userword(*machine,10093),memory_checks=userword(*machine,10094);
    std::ofstream memory(artifact/"heap_memory.json");memory<<"{\"executed\":"<<(memory_status==2?"true":"false")<<",\"guest_phase\":"<<memory_status<<",\"guest_failures\":"<<memory_failures<<",\"guest_checks\":"<<memory_checks<<",\"coverage\":\"real guest guarded spans, integer/fraction across mapped boundaries; skip invalid compiled pointer\",\"words\":[";
    bool comma=false;
    if(memory_status==2){
        check(memory_failures==0&&memory_checks==2*(1495+748),"real guest fractional/guard verification after secondgrow and exhaust");
        for(const auto span:std::vector<std::tuple<int,int,int,int>>{{static_cast<int>(initial_break),1495,-91,-92},{static_cast<int>(initial_break+1495),748,-93,-94}}){
            const auto[base,count,left,right]=span;
            for(int i=0;i<count;++i){const auto value=uservalue(*machine,base+i);const auto expected=i==0?native_ops::fromIntT40(left):i==count-1?native_ops::fromIntT40(right):i%2?native_ops::add(native_ops::fromIntT40(i),native_ops::divide(native_ops::fromIntT40(1),native_ops::fromIntT40(3))):native_ops::fromIntT40(73+i);
                check(value.mode==TernaryMode::T40&&!value.isInvalid()&&!value.asTriple().isSpecial()&&native_ops::compare(value.asTriple(),expected)==0,"actual mapped finite typed fraction/integer/guard");
                if(comma)memory<<',';comma=true;memory<<"{\"virtual_word\":"<<base+i<<",\"actual\":";exact(memory,value);memory<<"}";
            }
        }
    } else {
        check(memory_status==0&&memory_checks==0&&memory_failures==0,"wrong pointer safely skips all memory checks");
        check(false,"heap memory behavior unexecuted due compiled result-selection prerequisite");
    }
    memory<<"]}\n";memory.flush();check(memory.good(),"heap memory receipt written");memory.close();
    check(host::detail::writeCheckpointState(artifact/"terminal_state.bin",*machine,&error),"SDK/heap actual terminal checkpoint: "+error);
    check(checksum==189,"live checksum189");
    check(userword(*machine,10003)==0 && userword(*machine,10008)==0,"first and reopened successful fd0");
    check(userword(*machine,10004)==37 && userword(*machine,10010)==37,"write37 and short read37 from requested64");
    check(userword(*machine,10011)==0,"next read EOF0");
    check(userword(*machine,10006)==1 && userword(*machine,10007)==-1 && userword(*machine,10012)==1,"nonpayload close success/error status");
    std::ofstream content(artifact/"vfs_content.json"); content << "{\"path\":\"/tmp/sdk-roundtrip\",\"read_request_words\":64,\"actual_words\":[";
    for(int i=0;i<37;++i) {
        const auto actual=userword(*machine,10500+i); if(i) content << ','; content << actual;
        check(actual==31+i*7,"actual VFS roundtrip content word"+std::to_string(i));
    }
    content << "]}\n"; content.close();
    check(userword(*machine,10537)==999,"short read trailing sentinel untouched");
    check(machine->block_device.writeSnapshotFile((artifact/"disk.tdisk").string()),"actual current VFS disk snapshot");
    check(!readTextFile(artifact/"raw_return_records.jsonl").empty(),"nonempty closed raw records");
    for(const char* name:{"heap_metadata.jsonl","heap_memory.json","terminal_state.bin"})check(std::filesystem::exists(artifact/name)&&std::filesystem::file_size(artifact/name)>0,std::string("SDK required heap evidence ")+name);
    return finish();
}

// Failure modes are specified before the timer/metrics repair: complete-span
// validation, namespace leaks, trap timer starvation, idle wake, replay ordering.
bool runMetricsWorkflow() {
    using namespace sandbox;
    const auto artifact = std::filesystem::path(TRIT_TEST_BUILD_DIR) /
        "service_workflow_artifact" / "metrics";
    std::filesystem::create_directories(artifact);
    std::vector<std::string> failures;
    auto check = [&](bool value, const std::string& label) {
        if (!value) { failures.push_back(label); require(false, label); }
        return value;
    };
    auto finish = [&]() {
        std::ofstream out(artifact / "phase2_report.json");
        out << "{\"status\":\"" << (failures.empty() ? "pass" : "fail")
            << "\",\"sleep_units\":\"250 milliseconds = 250000000 monotonic virtual cycles at1ns/cycle; metrics kernel_tick remains delivered IRQ count; masked expiries coalesce in one pending latch\",\"failures\":[";
        for (size_t i=0;i<failures.size();++i) {
            if(i) out << ','; out << '"' << jsonEscape(failures[i]) << '"';
        }
        out << "]}\n";
        return failures.empty();
    };
    // Compare registries to the actual app rows and runtime constants, including aliases.
    const auto app_manifest=readTextFile("APP_MANIFEST.json");
    const auto syscall_manifest=readTextFile("SYSCALL_MANIFEST.json");
    const auto builder=readTextFile("build_tos_image.cpp");
    const std::regex app_row(R"RX(\{"([^"]+)", "([^"]+)", "([^"]+)", "([^"]+)", 0,)RX");
    size_t app_count=0;
    for(auto it=std::sregex_iterator(builder.begin(),builder.end(),app_row);it!=std::sregex_iterator();++it) {
        const auto& m=*it;
        bool found=false;
        const std::regex object(R"RX(\{([^{}]*)\})RX");
        const std::regex field(R"RX("([^"]+)"\s*:\s*"([^"]*)")RX");
        for(auto entry=std::sregex_iterator(app_manifest.begin(),app_manifest.end(),object);entry!=std::sregex_iterator();++entry) {
            const auto text=(*entry)[1].str(); std::map<std::string,std::string> fields;
            for(auto f=std::sregex_iterator(text.begin(),text.end(),field);f!=std::sregex_iterator();++f) fields[(*f)[1].str()]=(*f)[2].str();
            if(fields["source"]==m[1].str()+".trit" && fields["id"]==m[2].str() && fields["title"]==m[3].str() && fields["guest_path"]==m[4].str()) found=true;
        }
        check(found,"AC1 semantic manifest app row "+m[2].str());
        ++app_count;
    }
    check(app_count==60,"AC1 original59 plus service_host app registrations");
    const std::regex path_key("\\\"guest_path\\\"");
    check(static_cast<size_t>(std::distance(std::sregex_iterator(app_manifest.begin(),app_manifest.end(),path_key),std::sregex_iterator()))==60,"AC1 manifest preserves original59 plus service_host");
    const auto ir=readTextFile("ternary_compiler_ir.h");
    const auto runtime_begin=ir.find("namespace runtime {");
    const auto runtime_end=ir.find("} // namespace runtime",runtime_begin);
    const auto runtime_source=ir.substr(runtime_begin,runtime_end-runtime_begin);
    const std::regex constant(R"RX(static constexpr int ([a-zA-Z0-9_]+) = ([a-zA-Z0-9_]+);)RX");
    std::map<std::string,int> mappings;
    for(auto it=std::sregex_iterator(runtime_source.begin(),runtime_source.end(),constant);it!=std::sregex_iterator();++it) {
        const auto name=(*it)[1].str(), value=(*it)[2].str();
        const int id=std::isdigit(value[0])?std::stoi(value):mappings.at(value);
        mappings[name]=id;
        check(std::regex_search(syscall_manifest,std::regex("\\\""+name+"\\\"\\s*:\\s*"+std::to_string(id)+"\\b")),"AC1 manifest mapping "+name);
    }
    check(mappings.count("sys_service_metrics") && mappings["sys_service_metrics"]==60,"AC1 runtime metrics60");
    const std::string guest = R"TRIT(
const REPORT: t40 = 10000;
const BUFFER: t40 = 10200;
fn main() -> t40 {
    var i: t40 = 0;
    while i < 10 { unsafe { store(BUFFER + i, 777); } i = i + 1; }
    var short_result: t40 = os_service_metrics(BUFFER, 6);
    unsafe { store(REPORT, short_result); store(REPORT + 1, load(BUFFER)); }
    var good: t40 = os_service_metrics(BUFFER, 10);
    unsafe {
        store(REPORT + 2, good);
        store(REPORT + 3, load(BUFFER));
        store(REPORT + 4, load(BUFFER + 1));
        store(REPORT + 5, load(BUFFER + 3));
        store(REPORT + 6, load(BUFFER + 4));
        store(REPORT + 7, load(BUFFER + 5));
        store(REPORT + 8, load(BUFFER + 6));
        store(REPORT + 9, load(BUFFER + 7));
    }
    // Syscall-heavy execution must still deliver ticks through the real shim.
    os_service_metrics(BUFFER, 7);
    unsafe { store(REPORT + 16, load(BUFFER + 2)); }
    var busy_start: t40 = 0; var busy_now: t40 = 0;
    var busy_quantum: t40 = ARCH_CLOCK_TIMER_QUANTUM_CYCLES;
    unsafe { busy_start = csr_read(cycle); store(REPORT + 21, busy_start); store(REPORT + 24, busy_quantum); }
    busy_now = busy_start; i = 0; var busy_active: t40 = 1;
    while busy_active == 1 {
        sys_getpid(); i = i + 1;
        unsafe { busy_now = csr_read(cycle); }
        if busy_now - busy_start >= busy_quantum * 2 { busy_active = 0; }
        if i >= 16384 { busy_active = 0; }
    }
    unsafe { store(REPORT + 22, busy_now); store(REPORT + 23, i); }
    os_service_metrics(BUFFER, 7);
    unsafe { store(REPORT + 10, load(BUFFER + 2)); store(REPORT + 11, 101); }
    unsafe { store(REPORT + 17, csr_read(cycle)); }
    os_sleep_ms(250);
    unsafe { store(REPORT + 18, csr_read(cycle)); }
    os_service_metrics(BUFFER, 7);
    unsafe { store(REPORT + 12, load(BUFFER + 2)); store(REPORT + 11, 102); }
    unsafe { store(REPORT + 19, csr_read(cycle)); }
    os_sleep_ms(250);
    unsafe { store(REPORT + 20, csr_read(cycle)); }
    os_service_metrics(BUFFER, 7);
    unsafe { store(REPORT + 13, load(BUFFER + 2)); store(REPORT + 11, 103); }
    // Service55 is a poll at timeout0. Journaled input can already be pending
    // after timed sleep; consume it through the authoritative guest input API.
    var first: t40 = sys_wait_event(-1, BUFFER, 0);
    if first > 0 {
        unsafe { store(REPORT + 14, load(BUFFER + 1)); store(REPORT + 11, 104); }
        var second: t40 = sys_wait_event(-1, BUFFER, 0);
        if second > 0 {
            unsafe { store(REPORT + 15, load(BUFFER + 1)); store(REPORT + 11, 105); }
        }
    }
    // A negative futex timeout is the existing no-deadline contract. Keyboard
    // input must retain FIFO ownership and must not complete an unrelated futex.
    unsafe { store(BUFFER + 20, 0); }
    sys_futex_wait(BUFFER + 20, 0, -1);
    return 0;
}
)TRIT";
    const std::string setup = R"TRIT(
fn metrics_test_check(value: t40, expected: t40) -> t40 {
    if value != expected {
        var count: t40 = kload(14500);
        kstore(12000 + count * 2, value);
        kstore(12001 + count * 2, expected);
        kstore(14500, count + 1);
    }
    return 1;
}
fn metrics_test_sentinel(addr: t40, expected: t40, words: t40) -> t40 {
    var i: t40 = 0;
    while i < words { metrics_test_check(kload(addr + i), expected); i = i + 1; }
    return 1;
}
fn metrics_test_setup() -> t40 {
    kstore(14500, 0);
    namespace_create(1, 0);
    var slot: t40 = 10;
    while slot < 16 {
        var ns: t40 = 0;
        if slot >= 13 { ns = 1; }
        process_create(slot, 100 + slot, ns, 1, 0, 0);
        kstore(process_addr(slot) + PROC_CONTEXT, 0);
        process_set_state(slot, PROC_BLOCKED);
        slot = slot + 1;
    }
    process_set_state(10, PROC_RUNNABLE);
    process_set_state(12, PROC_EXITED);
    process_set_state(13, PROC_RUNNABLE);
    process_set_state(15, PROC_CRASHED);
    var w: t40 = 0;
    while w < 4 {
        var row: t40 = window_addr(w);
        kstore(row + WIN_ACTIVE, 1);
        kstore(row + WIN_NAMESPACE, 0);
        kstore(row + WIN_EVENT_COUNT, 3 + w * 3);
        w = w + 1;
    }
    kstore(window_addr(2) + WIN_NAMESPACE, 1);
    kstore(window_addr(3) + WIN_ACTIVE, 0);
    var j: t40 = 0;
    while j < 12 { kstore(15000 + j, 777); j = j + 1; }
    kernel_syscall_dispatch(110, 60, 15000, 12, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), 1);
    metrics_test_check(kload(SYS_PAYLOAD_ADDR), 7);
    metrics_test_check(kload(SYS_DETAIL_ADDR), 0);
    metrics_test_check(kload(15000), 1);
    metrics_test_check(kload(15001), 110);
    metrics_test_check(kload(15003), 4);
    metrics_test_check(kload(15004), 2);
    metrics_test_check(kload(15005), 9);
    metrics_test_check(kload(15006), 16);
    metrics_test_sentinel(15007, 777, 5);
    j = 0;
    while j < 7 { kstore(14520 + j, kload(15000 + j)); j = j + 1; }
    process_set_state(13, PROC_EXITED);
    kstore(window_addr(2) + WIN_EVENT_COUNT, 1);
    kernel_syscall_dispatch(110, 60, 15000, 7, 0, 0);
    metrics_test_check(kload(15003), 4);
    metrics_test_check(kload(15004), 2);
    metrics_test_check(kload(15005), 9);
    j = 0;
    while j < 7 { kstore(14530 + j, kload(15000 + j)); j = j + 1; }
    kernel_syscall_dispatch(113, 60, 15100, 7, 0, 0);
    metrics_test_check(kload(15103), 3);
    metrics_test_check(kload(15105), 1);
    metrics_test_check(kload(15106), 8);
    j = 0;
    while j < 7 { kstore(14540 + j, kload(15100 + j)); j = j + 1; }
    j = 0;
    while j < 7 { kstore(15000 + j, 888); j = j + 1; }
    kernel_syscall_dispatch(9999, 60, 15000, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kernel_syscall_dispatch(110, 60, 15000, 6, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kernel_syscall_dispatch(110, 60, -1, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    kernel_syscall_dispatch(110, 60, 15000, 0, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kernel_syscall_dispatch(110, 60, 15000, -1, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    kernel_syscall_dispatch(110, 60, 15000, 1000000000000, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kernel_syscall_dispatch(110, 60, 1000000000000, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    kernel_syscall_dispatch(110, 60, 15000, 1000000000000000000, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kernel_syscall_dispatch(110, 60, 1000000000000000000, 1000000000000000000, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kstore(15200, 3);
    kstore(15201, 2);
    var fraction: t40 = kload(15200) / kload(15201);
    kernel_syscall_dispatch(110, 60, 15000 + fraction, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    kernel_syscall_dispatch(110, 60, 15000, 6 + fraction, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    // Entire advertised span crosses the direct user window after valid prefix.
    kernel_syscall_dispatch(110, 60, 15000, 1001, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(15000, 888, 7);
    // A mapped writable first page, then unmapped/read-only second page.
    var ctx: t40 = process_default_context_addr(10);
    var pt: t40 = exec_hw_dmem_ptbr(10);
    kstore(process_addr(10) + PROC_CONTEXT, ctx);
    kstore(ctx + TASK_CONTEXT_DMEM_PTBR, pt);
    kstore(ctx + TASK_CONTEXT_DMEM_PAGES, 3);
    kstore(pt + 1, 20 * 19683 + 40);
    kstore(pt + 2, 0);
    translation_cache_invalidate(10);
    kernel_syscall_dispatch(110, 60, MMU_PAGE_WORDS + 700, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), 1);
    metrics_test_check(kload(20 * MMU_PAGE_WORDS + 700), 1);
    // Remove write permission after a successful cached syscall span. The
    // actual dispatcher must recheck the changed PTE without test invalidation.
    j = 0;
    while j < 7 { kstore(20 * MMU_PAGE_WORDS + 700 + j, 999); j = j + 1; }
    kstore(pt + 1, 20 * 19683 + 13);
    kernel_syscall_dispatch(110, 60, MMU_PAGE_WORDS + 700, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(20 * MMU_PAGE_WORDS + 700, 999, 7);
    kstore(pt + 1, 20 * 19683 + 40);
    kernel_syscall_dispatch(110, 60, MMU_PAGE_WORDS + 700, 7, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), 1);
    metrics_test_check(kload(20 * MMU_PAGE_WORDS + 700), 1);
    j = 0;
    while j < 7 { kstore(20 * MMU_PAGE_WORDS + 700 + j, 999); j = j + 1; }
    kernel_syscall_dispatch(110, 60, MMU_PAGE_WORDS + 700, 40, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(20 * MMU_PAGE_WORDS + 700, 999, 7);
    kstore(pt + 2, 21 * 19683 + 13);
    translation_cache_invalidate(10);
    kernel_syscall_dispatch(110, 60, MMU_PAGE_WORDS + 700, 40, 0, 0);
    metrics_test_check(kload(SYS_STATUS_ADDR), -1);
    metrics_test_sentinel(20 * MMU_PAGE_WORDS + 700, 999, 7);
    kstore(process_addr(10) + PROC_CONTEXT, 0);
    process_set_state(0, PROC_BLOCKED);
    process_set_state(10, PROC_BLOCKED);
    process_set_state(13, PROC_BLOCKED);
    return 1;
}
)TRIT";
    std::ofstream(artifact / "embedded_guest.trit") << guest;
    std::ofstream(artifact / "embedded_kernel_setup.trit") << setup;
    std::ofstream hashes(artifact / "source_hashes.json");
    hashes << "{\"algorithm\":\"FNV-1a-64\",\"sources\":{";
    const std::vector<std::string> sources = {"kernel.trit", "native_kernel_trap_stub.tasm",
        "ternary_vm_state.h", "ternary_vm.h", "ternary_host_runtime.h",
        "ternary_compiler_ir.h", "ternary_compiler_codegen.h", "apps/os_sdk.trit",
        "APP_MANIFEST.json", "SYSCALL_MANIFEST.json", "tests/current_only_conformance.cpp"};
    for(size_t i=0;i<sources.size();++i) {
        check(!readTextFile(sources[i]).empty(),"AC10 readable source hash input "+sources[i]);
        if(i) hashes << ',';
        hashes << '"' << sources[i] << "\":\"" << stableHash(readTextFile(sources[i])) << '"';
    }
    hashes << "}}\n"; hashes.close();
    auto diagnostics = [&](const auto& result, const std::string& stage) {
        std::ofstream out(artifact / (stage + ".txt"));
        for(const auto& d:result.diagnostics) out << d.format() << '\n';
    };
    compiler::CompilerOptions opts;
    opts.enable_vector_abi = true; opts.enable_vector_spilling = true;
    const auto app = compiler::compileSource("metrics_guest.trit",
        readTextFile("architecture_contract.trit") + "\n" + readTextFile("apps/os_sdk.trit") + "\n" + guest, opts);
    diagnostics(app, "guest_compile");
    if(!check(app.success, "AC1 compiled SDK metrics guest")) return finish();
    compiler::LinkOptions lo;
    lo.stack_hint_words = 2048; lo.dead_strip_functions = true;
    lo.standalone_halt_on_exit = false; lo.enable_vector_abi = true; lo.enable_vector_spilling = true;
    const auto linked = compiler::linkModules({app.object}, lo);
    diagnostics(linked, "guest_link");
    if(!check(linked.success, "AC1 guest links")) return finish();
    std::ofstream(artifact / "guest.linked.tasm") << linked.assembly;
    const auto kernel = compiler::compileSource("metrics_kernel.trit",
        readTextFile("architecture_contract.trit") + "\n" + readTextFile("kernel.trit") + "\n" + setup);
    diagnostics(kernel, "kernel_compile");
    if(!check(kernel.success, "AC4 real kernel compiles")) return finish();
    os::NativeVfsImageBuilder fs(32768);
    check(fs.installBaseLayout().ok(), "metrics rootfs layout");
    check(fs.addExecutableImage("/bin/metrics", linked.assembled.program,
        linked.executable_header_v3, 256).ok(), "metrics rootfs executable");
    std::string boot = ".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n call metrics_test_setup\n mov r1, native_trap_entry\n csrw tvec, r1\n";
    const std::string path = "/bin/metrics";
    for(size_t i=0;i<=path.size();++i) boot += " mov r1, " + std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i])) + "\n mov r2, " + std::to_string(10020+i) + "\n store r1, r2, 0\n";
    boot += " mov r13, 1\n mov r14, 10020\n mov r15, 1\n mov r16, 2\n call app_spawn\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
    boot += readTextFile("native_kernel_trap_stub.tasm") + "\n" + kernel.assembly;
    std::ofstream(artifact / "boot.tasm") << boot;
    auto feature_word=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
    auto boot_source=[&](int words) { return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(words)+", 0, 27, "+std::to_string(feature_word)+", 2, 0\n"; };
    auto assembled = vm::assembler::assembleV2(boot_source(1));
    feature_word=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
    assembled=vm::assembler::assembleV2(boot_source(1));
    if(assembled.success) assembled=vm::assembler::assembleV2(boot_source(static_cast<int>(assembled.program.size())));
    std::ofstream(artifact/"boot.tasm") << boot_source(static_cast<int>(assembled.program.size()));
    if(!assembled.success) {
        std::ofstream out(artifact / "boot_assembly.txt");
        for(const auto& e:assembled.errors) out << e.format() << '\n';
    }
    if(!check(assembled.success, "real kernel/trap assembly")) return finish();
    host::TosImageManifest manifest;
    auto image = host::bootImageFromAssembly(assembled, manifest, fs.image());
    std::string error;
    check(host::writeBootImageFile((artifact / "metrics.tboot").string(), image, &error), "metrics tboot: " + error);
    auto machine = std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    if(!check(host::loadBootImageIntoVm(*machine, image, "", &error), "metrics VM load: " + error)) return finish();
    auto word = [](const vm::VMState& m, int addr) { return host::detail::dmemWord(m,addr); };
    auto userword = [&](const vm::VMState& m, int addr) {
        const auto ctx = word(m,390000+9+5);
        const auto pt = word(m,static_cast<int>(ctx)+4);
        const auto pte = word(m,static_cast<int>(pt)+addr/vm::MMU_PAGE_WORDS);
        return word(m,static_cast<int>(pte/19683*vm::MMU_PAGE_WORDS)+addr%vm::MMU_PAGE_WORDS);
    };
    long long executed = 0, idle_cycles = 0, idle_events = 0;
    std::ofstream transitions(artifact / "timer_transitions.jsonl");
    bool reached = false;
    bool saved_timed_idle = false;
    bool ordinary_trap=false;
    long long trap_counter=0,trap_cycle=0,trap_reload=0;
    int ordinary_returns=0,ordinary_entries=0;
    std::vector<long long> actual_user_cycle_reads;
    bool timer_preserved=true;
    long long irq_entries=0,idle_acks=0;
    for(int n=0;n<200'000'000;++n) {
        const auto before = machine->cycle_count;
        const auto status = machine->status;
        const bool pending_before=machine->timer_pending, active_before=machine->trap_active;
        const auto cause_before=machine->cause;
        vm::VMExecutionRecord rec;
        vm::step(*machine,&rec);
        const bool irq_entry=machine->trap_active && machine->cause==vm::OS_CAUSE_TIMER_IRQ &&
            (!active_before || cause_before!=vm::OS_CAUSE_TIMER_IRQ ||
             (rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET));
        if(irq_entry) ++irq_entries;
        if(pending_before && !machine->timer_pending && !irq_entry && active_before) ++idle_acks;
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User && rec.instruction.opcode==isa::Opcode::CSRR && rec.instruction.imm==isa::CSR_CYCLE)
            actual_user_cycle_reads.push_back(vm::ops::toLong(machine->regfile.read(rec.instruction.rd)));
        if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL && rec.syscall_id==7) {
            ++ordinary_entries;
            ordinary_trap=true; trap_counter=machine->timer_counter; trap_cycle=machine->cycle_count; trap_reload=machine->timer_reload;
        } else if(ordinary_trap && !machine->trap_active) {
            const auto elapsed=machine->cycle_count-trap_cycle;
            const auto expected=trap_reload>0 ? (trap_counter-elapsed-1)%trap_reload+1 : trap_counter-elapsed;
            const auto normalized=expected<=0 ? expected+trap_reload : expected;
            timer_preserved &= machine->timer_reload==trap_reload && machine->timer_counter==normalized && machine->timer_enable;
            ++ordinary_returns; ordinary_trap=false;
        }
        if(rec.attempted) ++executed;
        else if(machine->cycle_count>before) { idle_cycles+=machine->cycle_count-before; ++idle_events; }
        if(status==vm::VMStatus::WAITING || machine->status==vm::VMStatus::WAITING) {
            if(n<100 || machine->cycle_count!=before) transitions << "{\"cycle\":" << machine->cycle_count << ",\"counter\":" << machine->timer_counter << ",\"kernel_tick\":" << word(*machine,3033) << ",\"instruction\":" << rec.attempted << ",\"pending\":" << machine->timer_pending << ",\"active\":" << machine->trap_active << ",\"epc\":" << machine->epc << "}\n";
        }
        if(!saved_timed_idle && machine->status==vm::VMStatus::WAITING && userword(*machine,10011)==101) {
            check(host::detail::writeCheckpointState(artifact/"timed_idle_state.bin",*machine,&error),"timed idle state saved");
            saved_timed_idle=true;
        }
        if(machine->status==vm::VMStatus::TRAPPED) break;
        if(userword(*machine,10011)==103 && machine->status==vm::VMStatus::WAITING) { reached=true; break; }
        if(status==vm::VMStatus::WAITING && machine->status==status && before==machine->cycle_count) break;
    }
    std::ofstream kernel_checks(artifact/"kernel_assertions.json");
    kernel_checks << "{\"failure_count\":" << word(*machine,14500) << ",\"actual_expected\":[";
    for(int i=0;i<word(*machine,14500);++i) { if(i) kernel_checks << ','; kernel_checks << '[' << word(*machine,12000+i*2) << ',' << word(*machine,12001+i*2) << ']'; }
    kernel_checks << "]}\n"; kernel_checks.close();
    check(word(*machine,14500)==0, "AC2/3 kernel namespace/span/sentinel assertions: failures=" + std::to_string(word(*machine,14500)));
    check(reached, "AC4/7 VM step repeated250 timed waits bounded without input: pc=" + std::to_string(machine->pc));
    if(!reached) {
        (void)host::detail::writeCheckpointState(artifact/"failed_vm_state.bin",*machine,&error);
        std::ofstream(artifact/"failed_vm_control.json") << "{\"pc\":" << machine->pc << ",\"cause\":" << machine->cause << ",\"status\":" << static_cast<int>(machine->status) << ",\"cycle\":" << machine->cycle_count << ",\"guest_marker\":" << userword(*machine,10011) << ",\"timer_enable\":" << machine->timer_enable << ",\"timer_counter\":" << machine->timer_counter << "}\n";
        return finish();
    }
    check(word(*machine,3033)==irq_entries+idle_acks,"AC5 delivered ticks equal single IRQ/idle pending acknowledgments");
    check(ordinary_returns>0 && timer_preserved,"AC5 real ordinary trap preserves timer reload/counter through return");
    check(userword(*machine,10000)<0 && userword(*machine,10001)==777, "AC3 SDK short span untouched");
    check(userword(*machine,10002)==7 && userword(*machine,10003)==1 && userword(*machine,10004)==2, "AC1 actual SDK seven-word return and caller");
    check(userword(*machine,10005)==5 && userword(*machine,10006)==1 && userword(*machine,10007)==9 && userword(*machine,10008)==16, "AC2 SDK caller namespace counts");
    check(userword(*machine,10009)==777, "AC3 SDK trailing word untouched");
    check(userword(*machine,10208)==777 && userword(*machine,10209)==777,"AC3 all SDK trailing words untouched");
    const auto busy_start=userword(*machine,10021),busy_end=userword(*machine,10022),busy_calls=userword(*machine,10023),busy_quantum=userword(*machine,10024);
    const bool busy_duration_met=busy_quantum==1000000 && busy_quantum==trap_reload && busy_end-busy_start>=2*busy_quantum;
    const bool busy_calls_match=busy_calls>0 && busy_calls<=16384 && busy_calls==ordinary_entries && busy_calls==ordinary_returns;
    const bool busy_csr_match=busy_calls>0 && busy_calls<static_cast<long long>(actual_user_cycle_reads.size()) && actual_user_cycle_reads.front()==busy_start && actual_user_cycle_reads[static_cast<size_t>(busy_calls)]==busy_end;
    check(busy_duration_met,"AC5 real syscall-heavy CYCLE duration >=two current timer quanta; cap exhaustion fails");
    check(busy_calls_match,"AC5 bounded busy call counter equals actual sys7 entries and ordinary returns");
    check(busy_csr_match,"AC5 busy start/end CYCLE fields equal independently observed real CSR reads");
    std::ofstream(artifact/"busy_syscall_accounting.json")<<"{\"start_cycle\":"<<busy_start<<",\"end_cycle\":"<<busy_end<<",\"elapsed_cycles\":"<<busy_end-busy_start<<",\"timer_quantum_cycles\":"<<busy_quantum<<",\"actual_trap_reload\":"<<trap_reload<<",\"required_quanta\":2,\"reported_calls\":"<<busy_calls<<",\"actual_sys7_entries\":"<<ordinary_entries<<",\"ordinary_returns\":"<<ordinary_returns<<",\"actual_user_csr_reads\":"<<actual_user_cycle_reads.size()<<",\"observed_start_cycle\":"<<(actual_user_cycle_reads.empty()?0:actual_user_cycle_reads.front())<<",\"observed_end_cycle\":"<<(busy_calls>=0 && busy_calls<static_cast<long long>(actual_user_cycle_reads.size())?actual_user_cycle_reads[static_cast<size_t>(busy_calls)]:0)<<",\"pre_loop_tick\":"<<userword(*machine,10016)<<",\"post_loop_tick\":"<<userword(*machine,10010)<<",\"call_cap\":16384,\"duration_bound_met\":"<<(busy_duration_met?"true":"false")<<",\"calls_match\":"<<(busy_calls_match?"true":"false")<<",\"csr_match\":"<<(busy_csr_match?"true":"false")<<",\"cap_exhausted_before_duration\":"<<(busy_calls>=16384 && !busy_duration_met?"true":"false")<<"}\n";
    check(userword(*machine,10010)>userword(*machine,10016), "AC5 syscall-heavy guest delivers ticks beyond pre-loop tick");
    check(userword(*machine,10018)-userword(*machine,10017)>=250000000 && userword(*machine,10020)-userword(*machine,10019)>=250000000, "AC4 independently observed CYCLE250ms repeated; delivered IRQ count is not milliseconds");
    transitions.close();
    const auto parked_cycle = machine->cycle_count;
    const auto parked_epc = machine->epc;
    const auto parked_tick = word(*machine,3033);
    const auto parked_run = vm::run(*machine,1000);
    check(machine->status==vm::VMStatus::WAITING && machine->cycle_count==parked_cycle && parked_run.steps==0 && machine->epc==parked_epc, "AC6 no-deadline park without fabricated cycles/instructions");
    auto batch_vm=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    check(host::loadBootImageIntoVm(*batch_vm,image,"",&error),"AC7 fresh vm run image");
    const auto batch_result=vm::run(*batch_vm,200'000'000);
    check(batch_result.status==vm::VMStatus::WAITING && userword(*batch_vm,10011)==103,"AC7 vm run timed waits on fresh real kernel");
    check(batch_vm->cycle_count==machine->cycle_count && batch_vm->epc==machine->epc && batch_vm->timer_counter==machine->timer_counter,"AC7 VM run/step architectural equivalence");
    std::ofstream(artifact/"vm_run_accounting.json") << "{\"budget_units\":" << batch_result.steps << ",\"cycles\":" << batch_vm->cycle_count << "}\n";
    // Exact WAIT completion and preexisting pending in an existing sole frame.
    for(int pending=0;pending<2;++pending) {
        vm::VMState probe(100,100);
        std::string probe_source=".isa 2\n.require wait executable_v3 vector vector_context\n.text\n wait\n halt\n";
        long long probe_features=0;
        auto envelope=[&]() { return probe_source+".data\nprobe_exec: .execheader3 0, 2, 0, 27, "+std::to_string(probe_features)+", 2, 0\n"; };
        auto code=vm::assembler::assembleV2(envelope());
        probe_features=vm::executableFeatureWordNumeric(code.required_features,architecture::v3::FEATURE_V3_LAST);
        code=vm::assembler::assembleV2(envelope());
        check(vm::assembler::loadAndReset(probe,code), "WAIT probe load");
        probe.trap_routing_enabled=true; probe.trap_active=true; probe.epc=55;
        probe.interrupt_enable=false; probe.timer_enable=true; probe.timer_reload=37;
        probe.timer_counter=pending?15:1; probe.timer_pending=pending!=0;
        vm::step(probe);
        check(probe.status==vm::VMStatus::RUNNING && probe.pc==1 && probe.epc==55 && probe.trap_active && probe.timer_pending, "AC5 WAIT expiry/pending immediately resumes sole frame");
        probe.timer_pending=false; probe.timer_enable=false; probe.enterWaiting();
        const auto cycles=probe.cycle_count; vm::run(probe,100);
        check(probe.status==vm::VMStatus::WAITING && probe.cycle_count==cycles, "AC6 disabled timer parked");
    }
    std::ofstream accounting(artifact / "idle_accounting.json");
    accounting << "{\"executed_instructions\":" << executed << ",\"virtual_idle_cycles\":" << idle_cycles << ",\"idle_budget_events\":" << idle_events << ",\"parked_cycle\":" << parked_cycle << ",\"parked_tick\":" << parked_tick << ",\"irq_entries\":" << irq_entries << ",\"idle_pending_acks\":" << idle_acks << ",\"ordinary_trap_returns\":" << ordinary_returns << "}\n";
    accounting.close();
    // Journal two keys strictly inside the first timer idle interval. Compare
    // replay to an independently advanced boundary checkpoint and normal host FIFO.
    if(check(saved_timed_idle,"AC8 timed idle checkpoint exists")) {
        std::unique_ptr<vm::VMState> idle;
        check(host::detail::readCheckpointState(artifact/"timed_idle_state.bin",idle,&error),"timed idle checkpoint read");
        if(idle) {
            const auto boundary=idle->cycle_count+std::max<long long>(1,idle->timer_counter/2);
            check(boundary>idle->cycle_count && boundary<idle->cycle_count+idle->timer_counter,"AC8 journal strictly before expiry");
            auto bundle=[&](const std::filesystem::path& dir,const vm::VMState& state,const std::vector<host::TosInputJournalEvent>& journal) {
                std::filesystem::create_directories(dir);
                check(host::writeBootImageFile((dir/"boot.tboot").string(),image,&error),"timed bundle boot");
                check(host::detail::writeCheckpointState(dir/"vm_state.bin",state,&error),"timed bundle state");
                check(state.block_device.writeSnapshotFile((dir/"disk.tdisk").string()),"timed bundle disk");
                check(host::detail::writeCheckpointJournal(dir/"input_journal.bin",journal,&error),"timed bundle journal");
                check(host::detail::writeInputJournalJson(dir/"input_journal.jsonl",journal,&error),"timed bundle journal JSON");
                check(host::detail::writePendingInputs(dir/"pending_input.bin",{},&error),"timed bundle pending");
                host::detail::CheckpointWriter meta(dir/"checkpoint.bin");
                meta.pod(host::detail::kCheckpointStateMagic); meta.pod(host::detail::kCheckpointStateVersion);
                meta.pod(std::uint64_t(0)); meta.pod(std::uint64_t(0));
                meta.pod(static_cast<std::uint64_t>(state.cycle_count)); meta.pod(static_cast<std::int32_t>(state.pc));
                meta.pod(std::uint64_t(1)); meta.pod(std::uint64_t(0));
                check(meta.ok,"timed bundle metadata");
            };
            std::vector<host::TosInputJournalEvent> journal{{0,static_cast<std::uint64_t>(boundary),host::TosInputEventKind::Text,0,0,0,"AB","metrics.idle_boundary","text"}};
            bundle(artifact/"timed_replay_bundle",*idle,journal);
            // Reference clock progression deliberately uses single cycles, with
            // no instructions, to validate the optimized event advance result.
            while(idle->cycle_count<boundary) idle->recordCycle(false);
            bundle(artifact/"timed_boundary_bundle",*idle,{});
            host::TosRuntimeConfig cfg; cfg.execution_backend=vm::VMExecutionBackend::Interpreter; cfg.record_syscall_trace=true;
            host::TosRuntime reference(cfg), replay(cfg);
            check(reference.restoreCheckpointBundle((artifact/"timed_boundary_bundle").string(),&error),"AC8 boundary reference restore");
            reference.pushTextInput("AB","metrics.idle_boundary");
            (void)reference.runForSteps(200'000'000);
            check(replay.restoreCheckpointBundle((artifact/"timed_replay_bundle").string(),&error),"AC8 idle replay restore");
            (void)replay.replayFromCheckpoint(200'000'000,&error);
            check(reference.captureCheckpoint(&error),"AC8 final reference capture");
            check(replay.captureCheckpoint(&error),"AC8 final replay capture");
            check(reference.exportDiagnostics((artifact/"timed_reference").string(),&error),"AC8 reference diagnostics");
            check(replay.exportDiagnostics((artifact/"timed_replayed").string(),&error),"AC8 idle replay diagnostics");
            std::unique_ptr<vm::VMState> expected,actual;
            check(host::detail::readCheckpointState(artifact/"timed_reference"/"checkpoint"/"vm_state.bin",expected,&error),"AC8 reference state read");
            check(host::detail::readCheckpointState(artifact/"timed_replayed"/"checkpoint"/"vm_state.bin",actual,&error),"AC8 replay state read");
            if(expected && actual) {
                check(expected->cycle_count==actual->cycle_count && expected->pc==actual->pc && expected->epc==actual->epc && expected->trap_active==actual->trap_active && expected->timer_counter==actual->timer_counter && expected->timer_pending==actual->timer_pending && expected->timer_enable==actual->timer_enable,"AC8 idle journal timer/cycle/EPC/frame equivalence");
                check(userword(*actual,10011)==105 && userword(*actual,10014)==65 && userword(*actual,10015)==66,"AC8 input inside idle interval preserved FIFO and process wake");
                for(int address: {3033,3019,3020,3021,3022,3023,390011,3050,3051,3052,3053,3054,3055}) check(word(*expected,address)==word(*actual,address),"AC8 process/input ACK/syscall state address"+std::to_string(address));
                check(!readTextFile(artifact/"timed_reference"/"framebuffer_snapshot.txt").empty() && !readTextFile(artifact/"timed_replayed"/"framebuffer_snapshot.txt").empty() && readTextFile(artifact/"timed_reference"/"framebuffer_snapshot.txt")==readTextFile(artifact/"timed_replayed"/"framebuffer_snapshot.txt"),"AC8 framebuffer outcome");
                check(!readTextFile(artifact/"timed_reference"/"checkpoint"/"pending_input.bin").empty() && !readTextFile(artifact/"timed_replayed"/"checkpoint"/"pending_input.bin").empty() && readTextFile(artifact/"timed_reference"/"checkpoint"/"pending_input.bin")==readTextFile(artifact/"timed_replayed"/"checkpoint"/"pending_input.bin"),"AC8 host pending FIFO outcome");
                const auto reference_trace=readTextFile(artifact/"timed_reference"/"checkpoint"/"syscall_trace.jsonl");
                const auto replay_trace=readTextFile(artifact/"timed_replayed"/"checkpoint"/"syscall_trace.jsonl");
                check(!reference_trace.empty() && !replay_trace.empty() && reference_trace==replay_trace,"AC8 readable syscall replay trace");
                check(expected->console_input==actual->console_input && actual->consoleInputAvailable()==0,"AC8 console FIFO consumed identically");
            }
        }
    }
    std::ofstream guest_report(artifact/"guest_report.json");
    guest_report << "{\"sdk_report\":[";
    for(int i=0;i<25;++i) { if(i) guest_report << ','; guest_report << userword(*machine,10000+i); }
    guest_report << "],\"namespace_records\":[";
    for(int r=0;r<3;++r) { if(r) guest_report << ','; guest_report << '['; for(int i=0;i<7;++i) { if(i) guest_report << ','; guest_report << word(*machine,14520+r*10+i); } guest_report << ']'; }
    guest_report << "],\"namespace_rows\":[";
    for(int n=0;n<2;++n) { if(n) guest_report << ','; guest_report << '['; for(int i=0;i<4;++i) { if(i) guest_report << ','; guest_report << word(*machine,392000+n*4+i); } guest_report << ']'; }
    guest_report << "]}\n"; guest_report.close();
    std::ofstream api_accounting(artifact/"api_accounting.json");
    api_accounting << "{\"budget_semantics\":\"one instruction or one bounded idle event; disabled park consumes zero\",\"apis\":[";
    // The same boot image runs through batching, single stepping and live worker.
    for(int api=0;api<3;++api) {
        host::TosRuntimeConfig cfg; cfg.execution_backend=vm::VMExecutionBackend::Interpreter; cfg.record_syscall_trace=true;
        host::TosRuntime runtime(cfg);
        if(!check(runtime.loadImage(image,&error), "host metrics load: " + error)) continue;
        long long previous_cycles=-1;
        if(api==2) {
            check(runtime.start(), "actual live worker starts: " + error);
            for(int retry=0;retry<10000;++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if(runtime.snapshot().status==vm::VMStatus::WAITING) {
                    // A completed no-deadline park is identifiable by its clock.
                    if(runtime.snapshot().cycles==previous_cycles) break;
                }
                previous_cycles=runtime.snapshot().cycles;
                if(runtime.snapshot().status==vm::VMStatus::TRAPPED) break;
            }
            runtime.pause();
        } else {
            for(int n=0;n<200'000'000;++n) {
                const auto result = api==0 ? runtime.runForSteps(8192).status : runtime.stepOnce();
                if(result==vm::VMStatus::TRAPPED) break;
                if(result==vm::VMStatus::WAITING && runtime.snapshot().cycles==previous_cycles) break;
                previous_cycles=runtime.snapshot().cycles;
                if(api==0 && n+1>=(200'000'000+8191)/8192) break;
            }
        }
        const auto dir=artifact/ ("host_api_"+std::to_string(api));
        check(runtime.exportDiagnostics(dir.string(),&error), "host diagnostics: " + error);
        std::unique_ptr<vm::VMState> state;
        if(!check(host::detail::readCheckpointState(dir/"checkpoint"/"vm_state.bin",state,&error), "host checkpoint read: " + error)) continue;
        if(api) api_accounting << ',';
        api_accounting << "{\"api\":" << api << ",\"cycles\":" << state->cycle_count << ",\"timer_counter\":" << state->timer_counter << ",\"epc\":" << state->epc << "}";
        check(state->cycle_count==machine->cycle_count && state->epc==machine->epc && state->timer_counter==machine->timer_counter,"AC7 host API architectural result equals VMstep api="+std::to_string(api));
        check(userword(*state,10011)==103 && state->status==vm::VMStatus::WAITING, "AC7 host API timed wakes api="+std::to_string(api));
        if(api!=0) continue;
        check(runtime.captureCheckpoint(&error), "replay capture: "+error);
        const auto tick=word(*state,3033);
        runtime.pushTextInput("AB","metrics.fifo");
        (void)runtime.runForSteps(200000);
        check(runtime.exportCheckpointBundle((artifact/"replay_bundle").string(),&error), "original replay bundle export");
        check(runtime.captureCheckpoint(&error),"AC6 final input ownership capture");
        check(runtime.exportDiagnostics((artifact/"input_outcome").string(),&error), "input outcome export");
        std::unique_ptr<vm::VMState> outcome;
        check(host::detail::readCheckpointState(artifact/"input_outcome"/"checkpoint"/"vm_state.bin",outcome,&error), "input outcome read");
        std::deque<host::TosPendingInput> held;
        check(host::detail::readPendingInputs(artifact/"input_outcome"/"checkpoint"/"pending_input.bin",held,&error),"AC6 retained input FIFO read");
        check(outcome && outcome->consoleInputAvailable()==1 && outcome->peekConsoleInput()==65 && held.size()==2 && held[0].word==65 && held[0].applied && held[1].word==66 && !held[1].applied && word(*outcome,3033)==tick && userword(*outcome,10011)==103 && word(*outcome,138701)==-1,"AC6 input-only wake retains FIFO/ownership without tick or futex completion");
        check(runtime.restoreCheckpointBundle((artifact/"replay_bundle").string(),&error), "AC8 original replay checkpoint restore");
        auto replay=runtime.replayFromCheckpoint(200000,&error);
        check(replay.status!=vm::VMStatus::TRAPPED, "AC8 checkpoint replay not trapped");
        check(runtime.captureCheckpoint(&error),"AC8 final postpark replay capture");
        check(runtime.exportDiagnostics((artifact/"replayed").string(),&error), "replayed export");
        std::unique_ptr<vm::VMState> replayed;
        check(host::detail::readCheckpointState(artifact/"replayed"/"checkpoint"/"vm_state.bin",replayed,&error), "replayed read");
        check(outcome && replayed && outcome->cycle_count==replayed->cycle_count && outcome->epc==replayed->epc && outcome->timer_counter==replayed->timer_counter && outcome->timer_pending==replayed->timer_pending && outcome->pc==replayed->pc && outcome->console_input==replayed->console_input && readTextFile(artifact/"input_outcome"/"checkpoint"/"pending_input.bin")==readTextFile(artifact/"replayed"/"checkpoint"/"pending_input.bin"), "AC8 replay timer/cycle/EPC/FIFO outcome equivalent");
        host::TosRuntime restored(cfg);
        check(restored.restoreCheckpointBundle((artifact/"replay_bundle").string(),&error), "AC8 fresh bundle restore: "+error);
        (void)restored.replayFromCheckpoint(200000,&error);
        check(restored.snapshot().cycles==runtime.snapshot().cycles && restored.snapshot().pc==runtime.snapshot().pc, "AC8 serialized restore/replay equivalent");
    }
    api_accounting << "]}\n"; api_accounting.close();
    for(const auto& file: {"embedded_guest.trit","source_hashes.json","idle_accounting.json","timer_transitions.jsonl","vm_run_accounting.json","guest_report.json"}) check(!readTextFile(artifact/file).empty(),"AC10 nonempty artifact "+std::string(file));
    return finish();
}


// Clock failure modes/oracles were frozen before changing any production source.
// All guest returns below traverse the existing SDK, current kernel and ERET.
bool runClockKernelWorkflow(bool boundaries = false) {
    using namespace sandbox;
    const auto base=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact"/"clock"/(boundaries?"kernel_boundaries":"kernel");
    std::filesystem::create_directories(base);
    std::vector<std::string> failures;
    auto check=[&](bool pass,const std::string& label) { if(!pass) { failures.push_back(label); require(false,"clock: "+label); } return pass; };
    auto finish=[&]() { std::ofstream out(base/"report.json"); out<<"{\"status\":\""<<(failures.empty()?"PASS":"FAIL")<<"\",\"virtual_ns_per_cycle\":1,\"cycles_per_ms\":1000000,\"timer_quantum_cycles\":1000000,\"host_wall_time_guaranteed\":false,\"failures\":["; for(size_t i=0;i<failures.size();++i) { if(i) out<<','; out<<'"'<<jsonEscape(failures[i])<<'"'; } out<<"]}\n"; return failures.empty(); };
    const std::string guest=R"TRIT(
fn main() -> t40 {
    var i: t40 = 0; var entered: t40 = 0; var returned: t40 = 0; var result: t40 = 0;
    unsafe { store(10220,0); }
    while i < 6 {
        unsafe { entered=csr_read(cycle); store(10020+i*4,entered); }
        if i < 2 { result=os_sleep_ms(250); }
        if i == 2 { result=os_futex_wait(10220,0,250); }
        if i == 3 { result=os_ipc_recv_blocking(3,10200,250); }
        if i == 4 { result=os_wait_event(-1,10200,250); }
        if i == 5 { result=os_window_wait_event(0,10200,250); }
        unsafe { returned=csr_read(cycle); store(10021+i*4,returned); store(10022+i*4,result); store(10023+i*4,returned-entered); }
        i=i+1;
    }
    unsafe { store(10011,777); }
    os_futex_wait(10220,0,-1); return 0;
}
)TRIT";
    const std::string setup="const CLOCK_BOUNDARY_CASE: t40 = "+std::to_string(boundaries?1:0)+";\n"+R"TRIT(
fn clock_snapshot(stage: t40) -> t40 {
    var at: t40 = 14000+stage*100; var i: t40=0; var now: t40=0;
    unsafe { now=csr_read(cycle); }
    kstore(at,now); kstore(at+1,kload(KERNEL_TICK_ADDR));
    unsafe { kstore(at+2,csr_read(timer_pending)); }
    while i < 6 {
        var slot: t40=10+i; var ctx: t40=kload(process_addr(slot)+PROC_CONTEXT);
        kstore(at+10+i*10,process_state(slot)); kstore(at+11+i*10,kload(WAIT_KIND_BASE+slot));
        kstore(at+12+i*10,kload(WAIT_DEADLINE_BASE+slot)); kstore(at+13+i*10,kload(WAIT_PROC_NEXT_BASE+slot));
        kstore(at+14+i*10,kload(TIMER_WHEEL_NEXT_BASE+slot)); kstore(at+15+i*10,kload(process_addr(slot)+PROC_WAIT_CHANNEL));
        kstore(at+16+i*10,kload(ctx+TASK_CONTEXT_EPC)); i=i+1;
    }
    return 1;
}
fn clock_test_setup() -> t40 {
    var slot: t40=10; var ctx: t40=0; var j: t40=0; var before: t40=0; var fraction: t40=0;
    var boundary_case: t40=0; var id: t40=0; var duration: t40=0; var a: t40=0; var b: t40=0; var c: t40=0;
    unsafe { csr_write(timer_enable,0); }
    namespace_create(1,0); ipc_open_owned(3,2,0,0);
    kstore(window_addr(0)+WIN_ACTIVE,1); kstore(window_addr(0)+WIN_NAMESPACE,0);
    kstore(window_addr(0)+WIN_OWNER_PID,2); kstore(window_addr(0)+WIN_EVENT_COUNT,0);
    while slot < 16 {
        process_create(slot,100+slot,0,1,0,0); ctx=process_default_context_addr(slot);
        kstore(process_addr(slot)+PROC_CONTEXT,ctx); kstore(ctx+TASK_CONTEXT_EPC,700+slot);
        process_set_state(slot,PROC_BLOCKED); slot=slot+1;
    }
    // Runtime-loaded division produces actual1.5; literal3/2 can constant-fold.
    kstore(15300,3); kstore(15301,2); fraction=kload(15300)/kload(15301);
    kstore(14600,fraction); kstore(15000,0);
    // Validate only real blocking attempts; ready/mismatch/pointer priority precedes duration.
    kstore(15303,2147483); kstore(15304,1000); kstore(15305,250); kstore(15306,1000000);
    kstore(15302,CLOCK_BOUNDARY_CASE); boundary_case=kload(15302);
    if boundary_case == 1 {
    j=0;
    while j < 4 {
        id=52; a=15000; b=0; if j == 1 { id=54; a=3; b=15010; }
        if j == 2 { id=55; a=-1; b=15010; } if j == 3 { id=56; a=0; b=0; }
        var n: t40=0;
        while n < 4 {
            duration=0; if n == 1 { duration=-1; } if n == 2 { duration=fraction; } if n == 3 { duration=kload(15303)*kload(15304)+648; }
            if id == 56 { a=duration; c=0; } else { c=duration; }
            kernel_syscall_dispatch(110,id,a,b,c,0);
            var row: t40=14610+(j*4+n)*6;
            kstore(row,kload(SYS_STATUS_ADDR)); kstore(row+1,kload(SYS_PAYLOAD_ADDR)); kstore(row+2,kload(SYS_DETAIL_ADDR));
            kstore(row+3,kload(WAIT_KIND_BASE+10)); kstore(row+4,kload(WAIT_DEADLINE_BASE+10));
            wait_clear_slot(10); process_set_state(10,PROC_BLOCKED); kstore(row+5,kload(TIMER_WHEEL_NEXT_BASE+10)); n=n+1;
        }
        j=j+1;
    }
    kernel_syscall_dispatch(110,52,15000,1,fraction,0); kstore(14710,kload(SYS_DETAIL_ADDR));
    kernel_syscall_dispatch(110,52,-1,0,fraction,0); kstore(14711,kload(SYS_DETAIL_ADDR));
    // Maxvalid is enqueued then cancelled through the normal queue path, never waited to expiry.
    kernel_syscall_dispatch(110,56,kload(15303)*kload(15304)+647,0,0,0);
    kstore(14712,kload(SYS_STATUS_ADDR)); kstore(14713,kload(WAIT_DEADLINE_BASE+10));
    wait_clear_slot(10); kstore(14714,kload(TIMER_WHEEL_NEXT_BASE+10)); process_set_state(10,PROC_BLOCKED);
    }
    // Actual kernel queues and completion paths; no CYCLE/deadline/tick assignment.
    process_block_on(10,wait_channel_sleep(110),WAIT_KIND_SLEEP,10,0,0,0,PROC_SLEEPING);
    process_block_on(11,wait_channel_sleep(111),WAIT_KIND_SLEEP,496,0,0,0,PROC_SLEEPING);
    process_block_on(12,wait_channel_futex_key(15000),WAIT_KIND_FUTEX,20,0,0,0,PROC_BLOCKED);
    process_block_on(13,wait_channel_sleep(113),WAIT_KIND_SLEEP,330,0,0,0,PROC_SLEEPING);
    process_block_on(14,wait_channel_futex_key(15001),WAIT_KIND_FUTEX,15,0,0,0,PROC_BLOCKED);
    process_block_on(15,wait_channel_sleep(115),WAIT_KIND_SLEEP,8,0,0,0,PROC_SLEEPING);
    clock_snapshot(0); wait_wake_futex_key(15001,1); wait_clear_slot(12);
    process_create(12,212,1,1,0,0); ctx=process_default_context_addr(12); kstore(process_addr(12)+PROC_CONTEXT,ctx);
    kstore(ctx+TASK_CONTEXT_EPC,912); process_block_on(12,wait_channel_sleep(212),WAIT_KIND_SLEEP,0,0,0,0,PROC_BLOCKED);
    clock_snapshot(1);
    // Hardware-config adapter: a real WAIT advances250M cycles, >243 ordinary1M quanta.
    // A subsequent small-period instruction loop proves several expiries coalesce.
    unsafe { csr_write(timer_reload,1000000); csr_write(timer_counter,kload(15305)*kload(15306)); csr_write(timer_pending,0); csr_write(timer_enable,1); arch_wait(); }
    clock_snapshot(2); unsafe { csr_write(timer_pending,0); } kernel_timer_tick(); clock_snapshot(3);
    unsafe { csr_write(timer_reload,37); csr_write(timer_counter,37); }
    j=0; while j < 200 { j=j+1; } clock_snapshot(4);
    unsafe { csr_write(timer_pending,0); csr_write(timer_reload,1000000); csr_write(timer_counter,1000000); }
    kernel_timer_tick(); clock_snapshot(5);
    // Cleanup uses existing queue paths, never host-fabricated completion.
    slot=10; while slot < 16 { wait_clear_slot(slot); process_set_state(slot,PROC_BLOCKED); kstore(process_addr(slot)+PROC_CONTEXT,0); slot=slot+1; }
    process_set_state(0,PROC_BLOCKED); return 1;
}
)TRIT";
    const auto artifact=base;
    std::ofstream(artifact/"embedded_guest.trit")<<guest;
    std::ofstream(artifact/"embedded_kernel_setup.trit")<<setup;
    auto diagnostics = [&](const auto& result, const std::string& stage) {
        std::ofstream out(artifact / (stage + ".txt"));
        for(const auto& d:result.diagnostics) out << d.format() << '\n';
    };
    compiler::CompilerOptions opts;
    opts.enable_vector_abi = true; opts.enable_vector_spilling = true;
    const auto app = compiler::compileSource("clock_guest.trit",
        readTextFile("architecture_contract.trit") + "\n" + readTextFile("apps/os_sdk.trit") + "\n" + guest, opts);
    diagnostics(app, "guest_compile");
    if(!check(app.success, "AC1 compiled SDK clock guest")) return finish();
    compiler::LinkOptions lo;
    lo.stack_hint_words = 2048; lo.dead_strip_functions = true;
    lo.standalone_halt_on_exit = false; lo.enable_vector_abi = true; lo.enable_vector_spilling = true;
    const auto linked = compiler::linkModules({app.object}, lo);
    diagnostics(linked, "guest_link");
    if(!check(linked.success, "AC1 guest links")) return finish();
    std::ofstream(artifact / "guest.linked.tasm") << linked.assembly;
    const auto kernel = compiler::compileSource("clock_kernel.trit",
        readTextFile("architecture_contract.trit") + "\n" + readTextFile("kernel.trit") + "\n" + setup);
    diagnostics(kernel, "kernel_compile");
    if(!check(kernel.success, "AC4 real kernel compiles")) return finish();
    os::NativeVfsImageBuilder fs(32768);
    check(fs.installBaseLayout().ok(), "clock rootfs layout");
    check(fs.addExecutableImage("/bin/clock", linked.assembled.program,
        linked.executable_header_v3, 256).ok(), "clock rootfs executable");
    std::string boot = ".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n call clock_test_setup\n mov r1, native_trap_entry\n csrw tvec, r1\n";
    const std::string path = "/bin/clock";
    for(size_t i=0;i<=path.size();++i) boot += " mov r1, " + std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i])) + "\n mov r2, " + std::to_string(10020+i) + "\n store r1, r2, 0\n";
    boot += " mov r13, 1\n mov r14, 10020\n mov r15, 1\n mov r16, 2\n call app_spawn\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
    boot += readTextFile("native_kernel_trap_stub.tasm") + "\n" + kernel.assembly;
    std::ofstream(artifact / "boot.tasm") << boot;
    auto feature_word=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
    auto boot_source=[&](int words) { return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(words)+", 0, 27, "+std::to_string(feature_word)+", 2, 0\n"; };
    auto assembly_input=boot_source(1);
    std::ofstream(artifact/"assembly_input_1.tasm",std::ios::binary)<<assembly_input;
    auto assembled = vm::assembler::assembleV2(assembly_input);
    feature_word=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
    assembly_input=boot_source(1);
    std::ofstream(artifact/"assembly_input_2.tasm",std::ios::binary)<<assembly_input;
    assembled=vm::assembler::assembleV2(assembly_input);
    if(assembled.success) { assembly_input=boot_source(static_cast<int>(assembled.program.size())); std::ofstream(artifact/"assembly_input_3.tasm",std::ios::binary)<<assembly_input; assembled=vm::assembler::assembleV2(assembly_input); }
    std::ofstream(artifact/"boot.tasm") << boot_source(static_cast<int>(assembled.program.size()));
    if(!assembled.success) {
        std::ofstream out(artifact / "boot_assembly.txt");
        for(const auto& e:assembled.errors) out << e.format() << '\n';
    }
    if(!check(assembled.success, "real kernel/trap assembly")) return finish();
    host::TosImageManifest manifest;
    auto image = host::bootImageFromAssembly(assembled, manifest, fs.image());
    std::string error;
    check(host::writeBootImageFile((artifact / "clock.tboot").string(), image, &error), "clock tboot: " + error);
    auto machine = std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    if(!check(host::loadBootImageIntoVm(*machine, image, "", &error), "clock VM load: " + error)) return finish();
    auto word = [](const vm::VMState& m, int addr) { return host::detail::dmemWord(m,addr); };
    auto userword = [&](const vm::VMState& m, int addr) {
        const auto ctx = word(m,390000+9+5);
        const auto pt = word(m,static_cast<int>(ctx)+4);
        const auto pte = word(m,static_cast<int>(pt)+addr/vm::MMU_PAGE_WORDS);
        return word(m,static_cast<int>(pte/19683*vm::MMU_PAGE_WORDS)+addr%vm::MMU_PAGE_WORDS);
    };

    std::ofstream rate(artifact/"actual_boot_rate.jsonl");
    const auto setup_label=assembled.labels.find("clock_test_setup");
    if(!check(setup_label!=assembled.labels.end(),"rate observer resolves actual clock setup entry")) return finish();
    const int setup_pc=setup_label->second;
    bool boot_rate_observed=false,boot_rate_red=false;
    std::ofstream calls(artifact/"actual_waits.jsonl");
    std::ofstream queues(artifact/"queue_transitions.jsonl");
    long long entry_cycle=0; int pending_id=0; long long instructions=0; int completions=0;
    auto entry_wall=std::chrono::steady_clock::now(); bool reached=false;
    std::array<int,6> ids{56,56,52,54,55,55};
    const auto start=std::chrono::steady_clock::now();
    vm::VMHooks hooks;
    hooks.onInstruction=[&](const vm::VMState& observed,const vm::VMExecutionRecord& rec) {
        if(!rec.attempted || !rec.has_instruction) return;
        if(!boot_rate_observed && rec.pc==setup_pc && rec.before_privilege==vm::PrivilegeMode::Kernel) {
            boot_rate_observed=true; boot_rate_red=observed.timer_reload!=1000000;
            rate<<"{\"event\":\"normal_kernel_boot_before_test_adapters\",\"pc\":"<<rec.pc<<",\"cycle\":"<<observed.cycle_count<<",\"timer_reload\":"<<observed.timer_reload<<",\"timer_counter\":"<<observed.timer_counter<<",\"timer_enable\":"<<(observed.timer_enable?"true":"false")<<",\"required_timer_quantum_cycles\":1000000}\n";
            check(!boot_rate_red,"AC-rate real boot timer_reload="+std::to_string(observed.timer_reload)+" required=1000000 before adapters");
        }
        if(rec.before_privilege==vm::PrivilegeMode::User && rec.instruction.opcode==isa::Opcode::SYSCALL && completions<6 && rec.syscall_id==ids[completions]) {
            pending_id=rec.syscall_id; entry_cycle=observed.cycle_count; entry_wall=std::chrono::steady_clock::now();
            bool blocked=true; for(int slot=0;slot<100;++slot) if(slot!=1 && (word(observed,390000+slot*9+2)==1 || word(observed,390000+slot*9+2)==2)) blocked=false;
            check(blocked,"AC1 every other process blocked at finite syscall entry");
        }
        if(pending_id && rec.instruction.opcode==isa::Opcode::ERET && rec.after_privilege==vm::PrivilegeMode::User) {
            const auto interval=observed.cycle_count-entry_cycle; const auto wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-entry_wall).count();
            const auto status=vm::ops::toLong(observed.regfile.read(13)), payload=vm::ops::toLong(observed.regfile.read(14)),detail=vm::ops::toLong(observed.regfile.read(15));
            calls<<"{\"ordinal\":"<<completions<<",\"id\":"<<pending_id<<",\"entry_cycle\":"<<entry_cycle<<",\"return_cycle\":"<<observed.cycle_count<<",\"interval_cycles\":"<<interval<<",\"required_cycles\":250000000,\"virtual_lateness_cycles\":"<<interval-250000000<<",\"wall_ms\":"<<wall<<",\"wall_jitter_from_250ms\":"<<wall-250<<",\"slow_emulation_factor\":"<<wall/(interval/1000000.0)<<",\"status\":"<<status<<",\"payload\":"<<payload<<",\"detail\":"<<detail<<",\"delivered_irq\":"<<word(observed,3033)<<"}\n";
            check(interval>=250000000,"AC1 SDK"+std::to_string(pending_id)+" actual250ms minimum CYCLE interval="+std::to_string(interval));
            check(pending_id==56?status==1:(status==0 && payload==0 && detail==-10),"AC1 actual timed completion status/payload/detail family"+std::to_string(pending_id));
            ++completions; pending_id=0;
        }
    };
    while(instructions<200000000 && machine->status!=vm::VMStatus::TRAPPED && machine->status!=vm::VMStatus::HALTED) {
        auto result=vm::run(*machine,8192,&hooks); instructions+=result.steps;
        if(boot_rate_red) break;
        if(userword(*machine,10011)==777 && machine->status==vm::VMStatus::WAITING) { reached=true; break; }
        if(result.steps==0 && machine->status==vm::VMStatus::WAITING) break;
    }
    check(boot_rate_observed,"AC-rate actual normal-kernel boot rate observed");
    if(boot_rate_red) {
        check(host::detail::writeCheckpointState(artifact/"terminal_state.bin",*machine,&error),"clock rate Red terminal state");
        std::ofstream(artifact/"execution.json")<<"{\"steps\":"<<instructions<<",\"cycles\":"<<machine->cycle_count<<",\"wall_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<",\"actual_wait_returns\":"<<completions<<",\"remaining_cases\":\"incomplete: prompt actual boot rate Red; SDK/wheel/boundaries not asserted\"}\n";
        return finish();
    }
    check(reached && completions==6,"AC1 repeated sleep56 + finite52/54/input55/window55 actually return without input");
    check(machine->consoleInputAvailable()==0,"AC1 no external input supplied");
    for(int stage=0;stage<6;++stage) {
        queues<<"{\"stage\":"<<stage<<",\"cycle\":"<<word(*machine,14000+stage*100)<<",\"delivered_irq\":"<<word(*machine,14001+stage*100)<<",\"pending\":"<<word(*machine,14002+stage*100)<<",\"processes\":[";
        for(int i=0;i<6;++i) { if(i) queues<<','; queues<<'['; for(int k=0;k<7;++k) { if(k) queues<<','; queues<<word(*machine,14010+stage*100+i*10+k); } queues<<']'; } queues<<"]}\n";
    }
    check(word(*machine,14200)-word(*machine,14100)>=243000000,"AC2 actual WAIT elapsed >=243 hardware quanta");
    for(int slotIndex:{0,5}) { check(word(*machine,14310+slotIndex*10)==1 && word(*machine,14311+slotIndex*10)==0,"AC2 due mixed waits completed once index"+std::to_string(slotIndex)); check(word(*machine,14516+slotIndex*10)==word(*machine,14016+slotIndex*10)+1,"AC2 completion advances EPC exactly once"); }
    for(int slotIndex:{1,3}) check(word(*machine,14310+slotIndex*10)==4 && word(*machine,14312+slotIndex*10)==word(*machine,14012+slotIndex*10),"AC2 future/same-bucket owner never early");
    check(word(*machine,14330)==3 && word(*machine,14332)==-1 && word(*machine,14536)==912,"AC2 cancelled/reused slot never resurrected");
    check(word(*machine,14556)==word(*machine,14056)+1,"AC2 explicit wake not duplicated by expiry");
    check(word(*machine,14402)==1 && word(*machine,14400)-word(*machine,14300)>74 && word(*machine,14401)==word(*machine,14301),"AC2 actual multiple masked expiries coalesce without delivered tick synthesis");
    if(boundaries) {
    for(int family=0;family<4;++family) for(int n=0;n<4;++n) {
        const int row=14610+(family*4+n)*6;
        if(n>=2) check(word(*machine,row)==-1 && word(*machine,row+2)==-8 && word(*machine,row+3)==0 && word(*machine,row+4)==-1,"AC3 invalid positive duration rejects before queue family"+std::to_string(family)+" input"+std::to_string(n));
        else if(family<2) check(word(*machine,row)==2 && word(*machine,row+4)==-1,"AC3 nonpositive futex/IPC indefinite preserved");
        else check(word(*machine,row)==(family==3?1:0) && word(*machine,row+3)==0,"AC3 event poll/sleep immediate nonpositive preserved");
        check(word(*machine,row+5)==-1,"AC3 normal cancellation removes wheel link");
    }
    check(word(*machine,14710)==-11 && word(*machine,14711)==-5,"AC3 mismatch/pointer error priority precedes fractional duration");
    check(word(*machine,14712)==2 && word(*machine,14713)>=2147483647LL && word(*machine,14714)==-1,"AC3 maxvalid duration enqueue then normalcancel cleanup");
    }
    check(host::detail::writeCheckpointState(artifact/"terminal_state.bin",*machine,&error),"clock immutable terminal state");
    std::ofstream(artifact/"execution.json")<<"{\"steps\":"<<instructions<<",\"cycles\":"<<machine->cycle_count<<",\"wall_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<",\"actual_wait_returns\":"<<completions<<",\"fixture\":\"real kernel queues; timer-counter hardware configuration adapters only; no fabricated CYCLE or syscall return\"}\n";
    return finish();
}

// Service E2E contract, written before the service implementation. These are
// normal executable guests using the actual kernel, VFS and private mappings.
// Controlled queues and syscall fault adapters are explicitly labelled below.
// START establishes a successful generation. STOP closes admissions and is
// nonterminal; its fsync commit returns the source credit for EOS. Aggregate
// flushes its final partial WINDOW after STOP and before terminal EOS.
using ServiceRecord = std::array<long long, 20>;

long long serviceKernelWord(const sandbox::vm::VMState& m, int a) {
    return sandbox::host::detail::dmemWord(m, a);
}

long long serviceUserWord(const sandbox::vm::VMState& m, int a, int slot = 7) {
    const auto context = serviceKernelWord(m, 390000 + slot * 9 + 5);
    const auto pt = serviceKernelWord(m, static_cast<int>(context) + 4);
    const auto pte = serviceKernelWord(m, static_cast<int>(pt) + a / sandbox::vm::MMU_PAGE_WORDS);
    if (context <= 0 || pt <= 0 || pte <= 0) return -999999;
    return serviceKernelWord(m, static_cast<int>(pte / 19683 * sandbox::vm::MMU_PAGE_WORDS) + a % sandbox::vm::MMU_PAGE_WORDS);
}

std::vector<long long> serviceInodeWords(const sandbox::vm::VMState& m, long long inode) {
    if (inode < 0 || inode >= 2048) return {};
    const auto count = serviceKernelWord(m, 150000 + static_cast<int>(inode) * 8 + 2);
    if (count < 0 || count > 10000) return {};
    std::vector<long long> out(static_cast<size_t>(count));
    std::vector<bool> covered(out.size());
    const auto extents = serviceKernelWord(m, 3017);
    for (int e = 0; e < extents; ++e) {
        const int row = 265000 + e * 6;
        if (serviceKernelWord(m,row+1) != inode || serviceKernelWord(m,row+5) <= 0) continue;
        const auto start=serviceKernelWord(m,row+2), len=serviceKernelWord(m,row+3), data=serviceKernelWord(m,row+4);
        for(long long i=0;i<len && start+i<count;++i) if(start+i>=0) {
            out[static_cast<size_t>(start+i)]=serviceKernelWord(m,static_cast<int>(data+i)); covered[static_cast<size_t>(start+i)]=true;
        }
    }
    if(std::find(covered.begin(),covered.end(),false)!=covered.end()) return {};
    return out;
}

long long serviceFindInode(const sandbox::vm::VMState& m, const std::string& path, int ns=0) {
    long long parent=0;
    std::istringstream parts(path); std::string part;
    while(std::getline(parts,part,'/')) {
        if(part.empty()) continue;
        bool found=false;
        for(int d=0;d<serviceKernelWord(m,3016);++d) {
            const int row=170000+d*6;
            if(serviceKernelWord(m,row)!=ns || serviceKernelWord(m,row+1)!=parent || serviceKernelWord(m,row+3)!=static_cast<long long>(part.size()) || serviceKernelWord(m,row+5)<=0) continue;
            bool equal=true;
            for(size_t i=0;i<part.size();++i) if(serviceKernelWord(m,195000+d*16+static_cast<int>(i))!=static_cast<unsigned char>(part[i])) equal=false;
            if(equal) { parent=serviceKernelWord(m,row+4); found=true; break; }
        }
        if(!found) return -1;
    }
    return parent;
}

bool runDesktopWorkflow(const std::filesystem::path& image_root = {},
                        const std::filesystem::path& artifact_root = {},
                        int service_mode = 0, bool automatic_service = false);

struct ServiceScenario {
    std::string name, body;
    int capacity=2, pool=32;
    bool long_history=false, fatal=false, startup_failure=false, worst_case=false, job_case=false, restart=false;
};

std::string serviceTestSupport() {
    return R"TRIT(
const ST: t40 = 15476;
const TEST: t40 = 15600;
fn service_test_check(value: t40, expected: t40, code: t40) -> t40 {
    if value != expected { unsafe { store(TEST, load(TEST) + 1); if load(TEST + 1) == 0 { store(TEST + 1, code); } } }
    return 1;
}
fn service_test_true(value: trit, code: t40) -> t40 {
    match value { pos => { return 1; } _ => { return service_test_check(0, 1, code); } }
}
fn service_test_stats() -> t40 {
    var result: t40 = svc_stats(ST, 64);
    service_test_check(result, 64, 100);
    return result;
}
fn service_test_dispatch() -> t40 {
    var used: t40 = df_arena_used();
    var attempts: t40 = svc_dispatch(99);
    service_test_true(attempts <= 32, 101);
    service_test_check(df_arena_used(), used, 102);
    service_test_stats();
    unsafe {
        service_test_true(load(ST + 12) <= 3421, 103);
        service_test_check(load(ST + 13), 0, 104);
        service_test_check(load(ST + 17), load(ST + 18) + load(ST + 19) + load(ST + 20), 105);
    }
    return attempts;
}
fn service_test_ready() -> t40 {
    var i: t40 = 0;
    service_test_stats();
    unsafe {
        while load(ST + 54) == 0 {
            if i >= 4096 { service_test_check(0, 1, 106); return 0; }
            if load(ST + 2) < 0 { return 0; }
            service_test_dispatch(); i = i + 1;
        }
        service_test_check(load(ST + 51), 31, 107);
    }
    return 1;
}
fn service_test_run(raw_limit: t40) -> t40 {
    var i: t40 = 0;
    service_test_stats();
    unsafe {
        while load(ST + 21) < raw_limit {
            if i >= 20000 { service_test_check(0, 1, 108); return 0; }
            if load(ST + 2) < 0 { return 0; }
            service_test_dispatch();
            if svc_has_immediate_work() == 0 { os_sleep_ms(250); }
            i = i + 1;
        }
    }
    return 1;
}
fn service_test_cancel() -> t40 {
    svc_request_cancel();
    var i: t40 = 0;
    service_test_stats();
    unsafe {
        while load(ST + 1) == 1 {
            if i >= 20000 { service_test_check(0, 1, 109); return 0; }
            service_test_dispatch(); i = i + 1;
        }
        service_test_check(load(ST + 20), 0, 110);
        service_test_check(load(ST + 35), 0, 111);
        service_test_check(load(ST + 44), 0, 112);
    }
    svc_destroy(); service_test_stats();
    unsafe {
        service_test_check(load(ST + 41), 0, 113);
        service_test_check(load(ST + 49), 0, 114);
        service_test_check(load(ST + 11), 0, 115);
        service_test_check(load(ST + 63), 0, 116);
    }
    return 1;
}
fn svc_vfs_open(path: t40, mode: t40) -> t40 {
    unsafe {
        if load(TEST + 10) == 1 { return os_open(-1, mode); }
        if load(TEST + 10) == 5 { if mode == 3 { return os_open(-1, mode); } }
    }
    return svc_vfs_open_production(path, mode);
}
fn svc_metrics_read(out: t40, words: t40) -> t40 {
    unsafe { if load(TEST + 10) == 2 { return os_service_metrics(-1, words); } }
    return svc_metrics_read_production(out, words);
}
fn svc_vfs_write(fd: t40, ptr: t40, words: t40) -> t40 {
    var call: t40 = 0;
    unsafe { call = load(TEST + 11) + 1; store(TEST + 11, call); }
    unsafe {
        if load(TEST + 10) == 3 { os_close(fd); return os_write(fd, ptr, words); }
        if load(TEST + 10) == 4 {
            var wrote: t40 = os_write(fd, ptr, words);
            if wrote > 0 { os_close(fd); }
            return wrote;
        }
        if load(TEST + 10) == 6 {
            var remainder: t40 = call;
            while remainder >= 11 { remainder = remainder - 11; }
            if df_bool_or(df_bool_or(remainder == 1, remainder == 2), df_bool_or(remainder == 5, remainder == 9)) {
                store(TEST + 12, load(TEST + 12) + 1);
                return 0;
            }
            if words > 3 { return os_write(fd, ptr, 3); }
        }
        if load(TEST + 10) == 7 {
            if ptr >= 15436 {
                if ptr < 15468 {
                    if load(TEST + 13) == 1 { store(TEST + 12, load(TEST + 12) + 1); return 0; }
                    store(TEST + 13, 1);
                    if words > 3 { return os_write(fd, ptr, 3); }
                }
            }
        }
        if load(TEST + 10) == 8 {
            if ptr >= 15468 {
                if ptr < 15476 {
                    if load(TEST + 13) == 1 { store(TEST + 12, load(TEST + 12) + 1); return 0; }
                    store(TEST + 13, 1);
                    if words > 3 { return os_write(fd, ptr, 3); }
                }
            }
        }
    }
    return svc_vfs_write_production(fd, ptr, words);
}
)TRIT";
}

std::vector<ServiceScenario> serviceScenarios() {
    std::vector<ServiceScenario> cases;
    for(int cap : {1,2}) cases.push_back({"history_cap"+std::to_string(cap),R"TRIT(
        unsafe { store(TEST + 10, 6); }
        service_test_run(259);
        var i: t40 = 0;
        while i < 5 { svc_job_submit(1); i = i + 1; }
        service_test_cancel();
    )TRIT",cap,32,true});
    cases.push_back({"reserved_head_full_pool",R"TRIT(
        service_test_run(6);
        unsafe { service_test_check(load(ST + 21), 6, 200); }
        var sched: t40 = svc_graph(1);
        var source: t40 = 0;
        unsafe { source = load(sched + 4); }
        var i: t40 = 0;
        while i < 1000 {
            df_node_execute(source + 38); df_node_execute(source + 76);
            df_node_execute(source + 114); df_node_execute(source + 152);
            service_test_stats();
            unsafe { if load(ST + 20) == 0 { i = 1000; } else { i = i + 1; } }
        }
        os_sleep_ms(250); df_node_execute(source);
        os_sleep_ms(250); df_node_execute(source);
        service_test_stats();
        unsafe {
            service_test_check(load(ST + 21), 8, 201);
            service_test_check(load(ST + 29), 8, 202);
            // BEGIN RESERVED POOL ROLLBACK COVERAGE
            // RAW7 pressure releases one credit and opens the source input channel.
            df_node_execute(source + 38);
            service_test_stats();
            service_test_check(load(ST + 29), 7, 203);
            service_test_check(load(ST + 21), 8, 204);
            var accepted_before: t40 = load(ST + 17);
            var sequence_before: t40 = load(ST + 6);
            var deadline_before: t40 = load(ST + 7);
            // Only the due source runs: one free slot cannot supply its three credits.
            os_sleep_ms(250);
            service_test_check(df_node_execute(source), DF_CALLBACK_BLOCKED_OUTPUT, 205);
            service_test_stats();
            service_test_true(load(ST + 31) > 0, 206);
            service_test_check(load(ST + 29), 7, 207);
            service_test_check(load(ST + 21), 8, 208);
            service_test_check(load(ST + 17), accepted_before, 209);
            service_test_check(load(ST + 6), sequence_before, 210);
            service_test_check(load(ST + 7), deadline_before, 211);
            // END RESERVED POOL ROLLBACK COVERAGE
            store(TEST + 10, 6);
        }
        svc_job_submit(1); svc_job_submit(2);
        service_test_run(12); service_test_cancel();
    )TRIT",2,8,false,false,false,true});
    cases.push_back({"minimum_pool_exhaust_cancel",R"TRIT(
        unsafe { store(TEST + 10, 6); }
        service_test_run(11); service_test_cancel();
    )TRIT",1,5});
    cases.push_back({"eight_jobs_namespace_cancel",R"TRIT(
        var j: t40 = 0;
        while j < 8 {
            var id: t40 = svc_job_submit(1);
            service_test_true(id > 0, 300);
            unsafe { store(TEST + 30 + j, id); }
            j = j + 1;
        }
        var before_reject: t40 = 0;
        unsafe { before_reject = load(TEST + 11); }
        service_test_check(svc_job_submit(2), 0, 301);
        unsafe { service_test_check(load(TEST + 11), before_reject, 302); }
        unsafe { store(TEST + 10, 6); }
        service_test_run(20);
        var complete_id: t40 = 0;
        unsafe { complete_id = load(TEST + 30); }
        service_test_check(svc_job_snapshot(complete_id, 15540, 16), 16, 303);
        unsafe { service_test_check(load(15540 + 2), 3, 304); service_test_check(load(15540 + 3), 0, 305); }
        j = 0;
        while j < 8 {
            var id: t40 = 0;
            unsafe { id = load(TEST + 30 + j); }
            svc_job_cancel(id); j = j + 1;
        }
        service_test_cancel();
    )TRIT",2,32,false,false,false,false,true});
    cases.push_back({"tokens_reuse_restart",R"TRIT(
        service_test_run(3);
        os_sleep_ms(250);
        var source_graph: t40 = svc_graph(1);
        var source_node: t40 = 0;
        unsafe { source_node = load(source_graph + 4); }
        df_node_execute(source_node);
        var saved: t40 = 0;
        var i: t40 = 0;
        while i < 32 {
            unsafe { if load(10745 + i * 24 + 2) == 1 { saved = load(10745 + i * 24 + 1); } }
            i = i + 1;
        }
        service_test_true(saved > 0, 400);
        // BEGIN SERVICE FIXTURE STRIDE
        var owner_stride: t40 = radix3(6061,11) + radix3(53857,0);
        unsafe { store(TEST + 41, owner_stride); owner_stride = load(TEST + 41); }
        service_test_check(svc_record_snapshot(saved + owner_stride, 15556, 20), -1, 401);
        // END SERVICE FIXTURE STRIDE
        service_test_check(svc_record_snapshot(saved + 1 / 3, 15556, 20), -1, 402);
        service_test_run(7);
        service_test_check(svc_record_snapshot(saved, 15556, 20), -1, 408);
        service_test_cancel();
        service_test_check(svc_record_snapshot(saved, 15556, 20), -1, 403);
        var generation: t40 = 0;
        unsafe { generation = load(ST + 5); }
        service_test_true(svc_construct(2, 32) >= 0, 404);
        svc_start(); service_test_ready(); service_test_stats();
        unsafe { service_test_check(load(ST + 5), generation + 1, 405); }
        service_test_check(svc_record_snapshot(saved, 15556, 20), -1, 406);
        service_test_run(9); service_test_cancel();
    )TRIT",2,32,false,false,false,false,false,true});
    for(const auto& fault : std::vector<std::pair<std::string,int>>{{"permanent_write",3},{"fsync_failure",4}}) {
        cases.push_back({fault.first,"service_test_run(3); unsafe { store(TEST + 10, "+std::to_string(fault.second)+"); } service_test_run(6); service_test_stats(); unsafe { service_test_true(load(ST + 2) < 0, 500); service_test_check(load(ST + 2), -4, 505); service_test_true(load(ST + 60) > 0, 501); service_test_check(load(ST + 60), load(ST + 20), 502); } svc_destroy(); service_test_stats(); unsafe { service_test_check(load(ST + 41), 0, 503); service_test_check(load(ST + 11), 0, 504); }",2,32,false,true});
    }
    cases.push_back({"failed_rotation_no_retirement",R"TRIT(
        service_test_run(8);
        unsafe { store(TEST + 10, 5); }
        var rotation_job: t40 = svc_job_submit(2);
        var i: t40 = 0; while i < 512 { service_test_dispatch(); i = i + 1; }
        service_test_stats();
        unsafe { service_test_check(load(ST + 19), 0, 510); service_test_true(load(ST + 46) > 0, 511); store(TEST + 10, 0); }
        service_test_check(svc_job_snapshot(rotation_job, 15540, 16), 16, 512);
        unsafe { service_test_check(load(15540 + 2), -1, 513); service_test_true(load(15540 + 3) < 0, 514); }
        service_test_cancel();
    )TRIT"});
    for(const auto& fault : std::vector<std::pair<std::string,int>>{{"startup_logger_failure",1},{"startup_metrics_failure",2}})
        cases.push_back({fault.first,"unsafe { store(TEST + 10, "+std::to_string(fault.second)+"); }",2,32,false,false,true});
    for(const auto& name : {"invalid_kind","duplicate_edge","cycle","zero_channel","pool1","pool2","pool3","pool4"})
        cases.push_back({name,"",2,32,false,false,true});
    for(const auto& name : {"persistent_invalid_kind","persistent_duplicate_edge","persistent_cycle"})
        cases.push_back({name,"",2,32,false,false,true});
    cases.push_back({"report_active_prefix_cancel",R"TRIT(
        unsafe { store(TEST + 10, 7); }
        var job: t40 = svc_job_submit(1);
        service_test_true(job > 0, 900);
        var n: t40 = 0;
        service_test_stats();
        unsafe {
            while load(ST + 49) == 0 {
                service_test_dispatch(); n = n + 1;
                if n >= 1024 { service_test_check(0, 1, 901); return 0; }
            }
        }
        n = 0;
        while n < 64 { service_test_dispatch(); n = n + 1; }
        service_test_check(svc_job_snapshot(job, 15540, 16), 16, 902);
        unsafe { service_test_true(load(15540 + 12) >= 0, 903); service_test_true(load(15540 + 13) > 0, 904); }
        svc_job_cancel(job);
        unsafe { store(TEST + 10, 0); }
        service_test_cancel();
        service_test_check(svc_job_snapshot(job, 15540, 16), 16, 905);
        unsafe { service_test_check(load(15540 + 2), 4, 906); }
        service_test_check(svc_job_snapshot(-1, 15540, 16), -1, 907);
        service_test_check(svc_job_snapshot(job, -1, 16), -1, 908);
        service_test_check(svc_job_snapshot(job, 15540, 15), -1, 909);
    )TRIT"});
    cases.push_back({"destroy_running_retains_pending",R"TRIT(
        os_sleep_ms(250);
        var graph: t40 = svc_graph(1); var source: t40 = 0;
        unsafe { source = load(graph + 4); }
        df_node_execute(source); service_test_stats();
        var pending: t40 = 0; var used: t40 = df_arena_used();
        unsafe { pending = load(ST + 20); service_test_true(pending > 0, 920); }
        service_test_true(svc_destroy() < 0, 921); service_test_stats();
        unsafe { service_test_check(load(ST + 20), pending, 922); }
        service_test_check(df_arena_used(), used, 923); service_test_cancel();
    )TRIT"});
    cases.push_back({"generation_prefix_zero_startup",R"TRIT(
        unsafe { store(TEST + 10, 8); }
        svc_start();
        var n: t40 = 0; while n < 64 { service_test_dispatch(); n = n + 1; }
        service_test_stats();
        unsafe { service_test_check(load(ST + 54), 0, 930); service_test_check(load(ST + 51), 1, 931); service_test_check(load(ST + 21), 0, 932); store(TEST + 10, 0); }
        service_test_ready(); service_test_run(11); service_test_cancel();
    )TRIT"});
    cases.push_back({"control_full_stall_cancel",R"TRIT(
        service_test_run(7);
        unsafe { store(TEST + 10, 6); }
        service_test_check(svc_control_submit(0, 10), -1, 700);
        service_test_check(svc_control_submit(1, 10), -1, 701);
        service_test_check(svc_control_submit(-3, 10), -1, 702);
        service_test_check(svc_control_submit(-1, 1 / 3), -1, 703);
        // BEGIN SERVICE FIXTURE UPPER
        var invalid_sequence: t40 = SVC_WIRE_MAX + 2;
        unsafe {
            store(TEST + 40, invalid_sequence); invalid_sequence = load(TEST + 40);
            service_test_true(invalid_sequence > SVC_WIRE_MAX, 708);
            service_test_check(tmod(invalid_sequence,1), 0, 709);
        }
        service_test_check(svc_control_submit(-1, invalid_sequence), -1, 704);
        // END SERVICE FIXTURE UPPER
        var tag: t40 = -1;
        while tag >= -5 {
            if tag != -3 {
                var accepted: t40 = svc_control_submit(tag, 40 + tag);
                var loops: t40 = 0;
                while accepted == 0 {
                    service_test_dispatch(); accepted = svc_control_submit(tag, 40 + tag);
                    loops = loops + 1;
                    if loops >= 1024 { service_test_check(0, 1, 705); return 0; }
                }
                service_test_check(accepted, 1, 706);
            }
            tag = tag - 1;
        }
        service_test_cancel();
        service_test_true(svc_control_submit(-1, 10) < 0, 707);
    )TRIT",1,5});
    cases.push_back({"sequence_domain_drain",R"TRIT(
        service_test_run(8);
        var raw_before: t40 = 0;
        unsafe { raw_before = load(ST + 21); store(10006, 1048571); }
        var n: t40 = 0;
        while n < 1024 {
            service_test_dispatch();
            unsafe { if load(ST + 1) != 1 { n = 1024; } else { n = n + 1; } }
        }
        service_test_stats();
        unsafe { service_test_check(load(ST + 21), raw_before, 800); service_test_check(load(ST + 1), 2, 801); service_test_true(load(ST + 6) <= 1048576, 802); }
        svc_destroy(); service_test_stats();
    )TRIT"});
    for(int marker_case:{0,1,2,3,4,5}) {
        std::string body=R"TRIT(
            service_test_run(3); service_test_cancel();
            var text: t40 = TEST + 60;
            unsafe { store(text,47); store(text+1,118); store(text+2,97); store(text+3,114); store(text+4,47); store(text+5,108); store(text+6,111); store(text+7,103); store(text+8,47); store(text+9,115); store(text+10,118); store(text+11,99); store(text+12,46); store(text+13,103); store(text+14,101); store(text+15,110); store(text+16,0); }
            var fd: t40 = os_open(text, 0);
            service_test_true(fd >= 0, 810);
            service_test_check(os_read(fd, 15468, 8), 8, 811); os_close(fd);
            unsafe { store(15470, 99); }
        )TRIT";
        if(marker_case==0) body+="unsafe { store(15471, 1023); }\n";
        if(marker_case==1) body+="unsafe { store(15472, 1); }\n";
        if(marker_case==2) body+="unsafe { store(15469, 2); }\n";
        const int words=marker_case==3?7:marker_case==4?9:8;
        if(marker_case==4) body+="unsafe { store(15476, 0); }\n";
        body+="fd = os_open(text, 3); service_test_true(fd >= 0, 812); service_test_check(os_write(fd, 15468, "+std::to_string(words)+"), "+std::to_string(words)+", 815); service_test_true(os_fsync(fd) > 0, 816); os_close(fd); svc_construct(2, 32); svc_start();";
        if(marker_case==5) body+="service_test_ready(); service_test_run(9); service_test_cancel();\n";
        else body+="var n: t40 = 0; while n < 128 { svc_dispatch(32); n = n + 1; } service_test_stats(); unsafe { service_test_check(load(ST + 54), 0, 813); service_test_true(load(ST + 2) < 0, 814); } svc_destroy(); service_test_stats();\n";
        cases.push_back({"generation_marker_"+std::to_string(marker_case),body,2,32,false,false,false,false,false,true});
    }
    return cases;
}

// Definition-only substitution keeps all production service behavior intact.
std::string serviceRenameDefinition(std::string source, const std::string& name,
                                    const std::string& replacement) {
    const std::string needle="fn "+name+"(";
    const auto first=source.find(needle);
    if(first!=std::string::npos && source.find(needle,first+needle.size())==std::string::npos)
        source.replace(first,needle.size(),"fn "+replacement+"(");
    return source;
}

struct ServiceHistory {
    struct Call { int id=0; std::array<long long,4> args{}; std::string path; std::vector<long long> words, old_file; };
    std::map<long long,ServiceRecord> accepted;
    std::vector<ServiceRecord> commits, retired;
    std::map<int,Call> pending;
    std::map<long long,std::string> paths;
    std::map<long long,long long> offsets;
    std::map<long long,std::vector<long long>> written;
    std::vector<std::array<long long,7>> metrics;
    std::vector<std::array<long long,8>> details;
    std::vector<std::array<long long,5>> snapshots;
    std::set<long long> synced_tokens;
    std::array<long long,64> stats{};
    std::map<long long,std::array<long long,16>> jobs;
    std::vector<int> startup_order;
    int initialized_mask=0,cleanup_mask=0,jobs_during_partial=0;
    bool marker_blocked=false;
    int zero_calls=0,prefix_calls=0,failed_syncs=0,failed_truncates=0,blocked_idle=0,job_steps=0,source_blocks=0;
    bool rechecked=false, namespace_changed=false;
};

void serviceWriteWords(const std::filesystem::path& path, const std::vector<long long>& words) {
    std::ofstream out(path); out << '[';
    for(size_t i=0;i<words.size();++i) { if(i) out << ','; out << words[i]; }
    out << "]\n";
}

// Checked artifact writes shared by the automatic fixtures and focused driver.
bool automaticWrite(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    if (!require(out.is_open(), "automatic artifact opens " + path.string())) return false;
    out << text;
    out.close();
    return require(!out.fail(), "automatic artifact writes/closes " + path.string());
}

bool serviceDesktopImage(const std::filesystem::path& dir, int mode, bool automatic) {
    using namespace sandbox;
    std::filesystem::create_directories(dir);
    const auto release=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"release"/"TernaryOS";
    host::TosBootImage original; std::string error;
    if(!require(host::readBootImageFile((release/"ternary-os.tboot").string(),original,&error),"service desktop staged image "+error)) return false;
    const std::string fixture=mode==2?R"TRIT(
fn service_desktop_fault() -> t40 {
    var text: t40 = 10050;
    unsafe { store(text,47); store(text+1,98); store(text+2,105); store(text+3,110); store(text+4,47); store(text+5,115); store(text+6,101); store(text+7,114); store(text+8,118); store(text+9,105); store(text+10,99); store(text+11,101); store(text+12,95); store(text+13,104); store(text+14,111); store(text+15,115); store(text+16,116); store(text+17,0); }
    var id: t40 = app_find(0,text);
    if id >= 0 { kstore(app_addr(id)+APP_TRUSTED,0); }
    return 1;
}
)TRIT":mode==3?R"TRIT(
fn service_desktop_fault() -> t40 {
    var text: t40 = 10050;
    unsafe { store(text,47); store(text+1,118); store(text+2,97); store(text+3,114); store(text+4,47); store(text+5,108); store(text+6,111); store(text+7,103); store(text+8,47); store(text+9,115); store(text+10,118); store(text+11,99); store(text+12,46); store(text+13,103); store(text+14,101); store(text+15,110); store(text+16,0); }
    vfs_create(0,text,KIND_DIR);
    return 1;
}
)TRIT":"fn service_desktop_fault() -> t40 { return 1; }\n";
    if(!automaticWrite(dir/"kernel_fixture.trit",fixture)) return false;
    const auto kernel=compiler::compileSource("service_desktop_kernel.trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("kernel.trit")+"\n"+fixture);
    { std::ostringstream out; for(const auto& d:kernel.diagnostics) out<<d.format()<<'\n'; if(!automaticWrite(dir/"kernel_compile.txt",out.str())) return false; }
    if(!require(kernel.success,"service desktop real kernel compile")) return false;
    std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n call service_desktop_fault\n mov r1, native_trap_entry\n csrw tvec, r1\n";
    const auto seed=[&](int address,const std::string& path) { for(size_t i=0;i<=path.size();++i) boot+=" mov r1, "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"\n mov r2, "+std::to_string(address+i)+"\n store r1, r2, 0\n"; };
    if(!automatic) { seed(10020,"/bin/service_host"); boot+=" mov r13, 1\n mov r14, 10020\n mov r15, 7\n mov r16, 107\n call app_spawn\n"; }
    seed(10020,automatic?"/bin/init":"/bin/desktop");
    boot+=" mov r13, 1\n mov r14, 10020\n mov r15, 0\n call app_launch\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
    boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
    auto feature=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
    const auto source=[&](int words) { return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(words)+", 0, 27, "+std::to_string(feature)+", 2, 0\n"; };
    auto assembly=vm::assembler::assembleV2(source(1)); feature=vm::executableFeatureWordNumeric(assembly.required_features,architecture::v3::FEATURE_V3_LAST);
    assembly=vm::assembler::assembleV2(source(1)); if(assembly.success) assembly=vm::assembler::assembleV2(source(static_cast<int>(assembly.program.size())));
    if(!automaticWrite(dir/"boot.tasm",source(static_cast<int>(assembly.program.size())))) return false;
    if(!require(assembly.success,"service desktop boot assembles")) return false;
    auto manifest=original.manifest; manifest.boot_entry=assembly.labels.at("boot");
    auto image=host::bootImageFromAssembly(assembly,manifest);
    if(!require(host::writeBootImageFile((dir/"ternary-os.tboot").string(),image,&error),"service desktop image write "+error)) return false;
    std::error_code ec; std::filesystem::copy_file(release/"ternary-os.tdisk",dir/"ternary-os.tdisk",std::filesystem::copy_options::overwrite_existing,ec);
    return require(!ec,"service desktop actual staged VFS copy "+ec.message());
}

bool runServiceWorkflow(bool automatic = false, const std::set<std::string>& selected = {}, bool link_only = false) {
    using namespace sandbox;
    const bool clock_focus=selected.count("clock_cap1") || selected.count("clock_cap2");
    const auto base=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact"/(link_only?"service_link_preflight":(clock_focus?"clock/service":"service"));
    std::filesystem::create_directories(base);
    std::vector<std::string> failures;
    std::vector<std::pair<std::string,bool>> automatic_modes;
    auto check=[&](bool pass,const std::string& label) {
        if(!pass) { failures.push_back(label); require(false,"service: "+label); }
        return pass;
    };
    // BEGIN SERVICE LINK PREFLIGHT
    std::vector<std::string> link_attempted, link_compiled, link_succeeded;
    auto link_write=[&](const std::filesystem::path& path,const std::string& text) {
        std::ofstream out(path);
        if(!check(out.is_open(),"link preflight opens "+path.string())) return false;
        out << text; out.close();
        return check(!out.fail(),"link preflight writes/closes "+path.string());
    };
// END SERVICE LINK PREFLIGHT
    auto cases=serviceScenarios();
    if(clock_focus) {
        cases.clear();
        for(int cap:{1,2}) cases.push_back({"clock_cap"+std::to_string(cap),R"TRIT(
            unsafe { store(TEST+10,6); }
            service_test_run(8); service_clock_stall();
            service_test_check(svc_control_submit(-1,39),1,901);
            service_test_run(16); service_clock_stall(); service_test_run(24); service_test_cancel();
            service_test_true(svc_construct(CLOCK_CAPACITY,32)>=0,902);
            svc_start(); service_test_ready(); unsafe { store(TEST+10,6); }
            service_test_run(8); service_clock_stall();
            service_test_run(16); service_clock_stall(); service_test_run(24); service_test_cancel();
        )TRIT",cap,32,false,false,false,false,false,true});
        for(auto& row:cases) { auto at=row.body.find("CLOCK_CAPACITY"); row.body.replace(at,14,std::to_string(row.capacity)); }
    }
    if(!selected.empty()) cases.erase(std::remove_if(cases.begin(),cases.end(),[&](const auto& row){return !selected.count(row.name);}),cases.end());
    // Every scenario and assertion is saved even when the absent host stops compilation.
    std::ofstream mapping(base/"acceptance_mapping.json");
    mapping << "{\"testing\":\"test-first real guest E2E\",\"record_kinds\":{\"START\":1,\"RAW\":2,\"WINDOW\":3,\"ALERT\":4,\"RECOVERY\":5,\"CONTROL\":6,\"EOS\":7,\"STOP\":8},\"criteria\":[";
    const std::vector<std::string> assertions={
        "AC1 original59+one host/current versions/service-only libraries/private spans/highwater3421/dispatch32",
        "AC2 cap1+cap2 each >=256 real accepted samples/CYCLE gaps>=250M/no loss duplicate/blocked idle/no input",
        "AC3 independent7 sums/count8+partial/EOS/three highs+lows/zero capacity/RAW8 WINDOW8 ALERT8",
        "AC4 three-credit admission/full pool empty sink/min5/1..4 early rejection/tokens foreign fractional stale reuse restart/callback allocations0",
        "AC5 real VFS/fd0/labelled prefix+zero/write+fsync commit/two64files/retirement/fatal exact pending/failed truncate retires0",
        "AC6 eight jobs/ninth no effects/fair bounded report+rotation/complete error cancel/ps5+getproc8/ns recheck/actual report16word rows",
        "AC7 both graphs validate before effects/all five ready/order/invalid kind duplicate cycle capacity/logger metrics failures/reverse fd then reset",
        "AC8 accepted+partial+EOS+prefix drain/jobs/fds reset/persistent generation restart/old token reject/no forced-stop replay",
        "AC9 current kernel/VFS/privateMMU/PID107 slot7/exact Calculator+Files inputs ownership results health/host progress before after",
        "AC10 full accepted+commit histories/VFS snapshots/reconciliation/guest assembly images/hashes/closed files/post-assertion PASS/initialRed separate",
        "AC11 on-demand first/separate automatic selector/actual init once before desktop/spawn+post-launch failures preserve desktop",
        "AC12 worker build/service SDK metrics stream default/root final production gates outstanding"};
    for(size_t i=0;i<assertions.size();++i) { if(i) mapping << ','; mapping << '"' << jsonEscape(assertions[i]) << '"'; }
    mapping << "],\"failure_modes\":[\"upstream full-pool head expansion deadlock\",\"double state update across stalls\",\"partial record truncated or released before fsync\",\"token resurrection or fractional index\",\"namespace changes after ps\",\"jobs starved behind sink\",\"READY before generation persistence\",\"cleanup evidence erased by reset\",\"truncated history masks loss\"],\"fault_adapters\":\"definition-only static replacement; prefix requests actual existing os_write; zero withheld call; negative existing OS errors\"}\n";
    mapping.close();
    std::vector<std::string> guests;
    for(const auto& scenario:cases) {
        const auto dir=base/scenario.name; std::filesystem::create_directories(dir);
        std::string body="fn main() -> t40 { var i: t40 = 0; while i < 100 { unsafe { store(TEST + i, 0); } i = i + 1; }\n";
        if(scenario.name=="zero_channel" || scenario.name.rfind("pool",0)==0) {
            const int pool=scenario.name.rfind("pool",0)==0?std::stoi(scenario.name.substr(4)):32;
            body+="service_test_true(svc_construct("+std::to_string(scenario.name=="zero_channel"?0:2)+", "+std::to_string(pool)+") < 0, 600); service_test_stats(); unsafe { service_test_check(load(ST + 50), 0, 601); service_test_check(load(ST + 41), 0, 602); }\n";
        } else {
            body+="service_test_true(svc_construct("+std::to_string(scenario.capacity)+", "+std::to_string(scenario.pool)+") >= 0, 603);\n";
            const bool persistent=scenario.name.rfind("persistent_",0)==0;
            if(scenario.name=="invalid_kind" || scenario.name=="duplicate_edge" || scenario.name=="cycle" || persistent) {
                body+="var graph: t40 = svc_graph("+std::to_string(persistent?1:0)+"); var nodes: t40 = 0; unsafe { nodes = load(graph + 4); }\n";
                if(scenario.name.find("invalid_kind")!=std::string::npos) body+="unsafe { store(nodes + 1, 99); }\n";
                if(scenario.name.find("duplicate_edge")!=std::string::npos) { if(persistent) body+="df_node_add_dependent(nodes, nodes + 38);\n"; body+="df_node_add_dependent(nodes, nodes + 38);\n"; }
                if(scenario.name.find("cycle")!=std::string::npos) { if(persistent) body+="df_node_add_dependent(nodes, nodes + 152);\n"; body+="df_node_add_dependent(nodes + 152, nodes);\n"; }
            }
            if(scenario.startup_failure) {
                body+=scenario.body+"svc_start(); var n: t40 = 0; while n < 128 { svc_dispatch(32); n = n + 1; } service_test_stats(); unsafe { service_test_check(load(ST + 54), 0, 604); service_test_true(load(ST + 2) < 0, 605); } svc_destroy(); service_test_stats(); unsafe { service_test_check(load(ST + 41), 0, 606); service_test_check(load(ST + 11), 0, 607); }\n";
            } else if(scenario.name=="generation_prefix_zero_startup") body+=scenario.body;
            else body+="svc_start(); service_test_ready();\n"+scenario.body;
        }
        body+="unsafe { store(TEST + 2, 777); } os_futex_wait(TEST + 99, 0, -1); return 0; }\n";
        auto support=serviceTestSupport();
        // Baseline uses the existing SDK without hiding behavior behind a missing new symbol.
        // Green executes the production idle helper in this actual guest.
        if(readTextFile("apps/service_host.trit").find("fn svc_idle_wait(")!=std::string::npos) {
            const std::string old="os_sleep_ms(250);"; size_t at=0;
            while((at=support.find(old,at))!=std::string::npos) { support.replace(at,old.size(),"svc_idle_wait();"); at+=16; }
        }
        if(clock_focus) {
            const auto insert=support.find("        if load(TEST + 10) == 6 {");
            support.insert(insert,"        if load(TEST + 10) == 9 { store(TEST + 12, load(TEST + 12) + 1); return 0; }\n");
            support+=R"TRIT(
fn service_clock_stall() -> t40 {
    var channel: t40=svc_get(64); var loops: t40=0; var count: t40=0; var capacity: t40=0;
    unsafe { store(TEST+10,9); capacity=load(channel+1); count=load(channel+4); }
    while count < capacity {
        service_test_dispatch(); os_sleep_ms(250);
        unsafe { count=load(channel+4); }
        loops=loops+1; if loops >= 16 { service_test_check(0,1,910); return 0; }
    }
    // Two real periods while c0 is full: source must retain capacity and not query/reserve.
    service_test_dispatch(); os_sleep_ms(250); service_test_dispatch();
    os_sleep_ms(250); service_test_dispatch();
    unsafe { store(TEST+10,6); }
    return 1;
}
)TRIT";
        }
        guests.push_back(support+body);
        std::ofstream(dir/"embedded_guest.trit") << guests.back();
    }
    const std::string setup=R"TRIT(
fn service_test_kernel_setup() -> t40 {
    namespace_create(1, 0);
    process_create(10, 110, 0, 1, 0, 0); process_set_state(10, PROC_BLOCKED);
    process_create(11, 111, 1, 1, 0, 0); process_set_state(11, PROC_BLOCKED);
    process_create(12, 112, 0, 1, 0, 0); process_set_state(12, PROC_BLOCKED);
    process_set_state(0, PROC_BLOCKED);
    return 1;
}
)TRIT";
    std::ofstream(base/"embedded_kernel_setup.trit") << setup;
    std::ofstream hashes(base/"source_hashes.json");
    hashes << "{\"algorithm\":\"FNV-1a-64\",\"sources\":{";
    const std::vector<std::string> inputs={"apps/service_host.trit","apps/service_runtime.trit","ternary_dataflow.trit","ternary_stream.trit","apps/os_sdk.trit","architecture_contract.trit","kernel.trit","native_kernel_trap_stub.tasm","build_tos_image.cpp","APP_MANIFEST.json","CMakeLists.txt","tests/current_only_conformance.cpp"};
    for(size_t i=0;i<inputs.size();++i) { if(i) hashes<<','; const auto value=readTextFile(inputs[i]); hashes << '"' << inputs[i] << "\":{\"present\":" << (!value.empty()?"true":"false") << ",\"hash\":\"" << stableHash(value) << "\"}"; }
    hashes << "}}\n"; hashes.close();
    auto production=readTextFile("apps/service_host.trit");
    production=serviceRenameDefinition(production,"main","svc_production_main");
    production=serviceRenameDefinition(production,"svc_vfs_open","svc_vfs_open_production");
    production=serviceRenameDefinition(production,"svc_vfs_write","svc_vfs_write_production");
    production=serviceRenameDefinition(production,"svc_metrics_read","svc_metrics_read_production");
    bool ran=false;
    for(size_t index=0;index<cases.size();++index) {
        const auto& scenario=cases[index]; const auto dir=base/scenario.name;
        const auto scenario_failure_start=failures.size();
        const std::string source=readTextFile("architecture_contract.trit")+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+readTextFile("apps/service_runtime.trit")+"\n"+readTextFile("ternary_dataflow.trit")+"\n"+readTextFile("ternary_stream.trit")+"\n"+production+"\n"+guests[index];
        std::ofstream(dir/"linked_guest_source.trit") << source;
        compiler::CompilerOptions opts; opts.enable_vector_abi=true; opts.enable_vector_spilling=true;
        const auto app=compiler::compileSource(scenario.name+".trit",source,opts);
        // BEGIN SERVICE LINK PREFLIGHT
        if(link_only) { link_attempted.push_back(scenario.name); link_write(dir/"preflight.compiler.tasm",app.assembly); }
// END SERVICE LINK PREFLIGHT

        { std::ofstream out(dir/"compile.txt"); for(const auto& d:app.diagnostics) out<<d.format()<<'\n'; }
        if(!check(app.success,"AC1/AC10 "+scenario.name+" production-linked guest compiles")) continue;
        // BEGIN SERVICE LINK PREFLIGHT
        if(link_only) link_compiled.push_back(scenario.name);
// END SERVICE LINK PREFLIGHT
        compiler::LinkOptions lo; lo.stack_hint_words=2052; lo.dead_strip_functions=true; lo.standalone_halt_on_exit=false; lo.enable_vector_abi=true; lo.enable_vector_spilling=true;
        const auto linked=compiler::linkModules({app.object},lo);
        // BEGIN SERVICE LINK PREFLIGHT
        if(link_only) link_write(dir/"preflight.linked.tasm",linked.assembly);
// END SERVICE LINK PREFLIGHT

        { std::ofstream out(dir/"link.txt"); for(const auto& d:linked.diagnostics) out<<d.format()<<'\n'; }
        if(!check(linked.success,"AC1 "+scenario.name+" links current executable")) continue;
        std::ofstream(dir/"guest.linked.tasm") << linked.assembly;
        check(linked.executable_header_v3.function_abi_version==3 && linked.executable_header_v3.executable_version==3 && linked.executable_header_v3.isa_version==2,"AC1 current executable versions");
        // BEGIN SERVICE LINK PREFLIGHT
        if(link_only) { link_succeeded.push_back(scenario.name); continue; }
// END SERVICE LINK PREFLIGHT

        const auto kernel=compiler::compileSource("service_kernel.trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("kernel.trit")+"\n"+setup);
        { std::ofstream out(dir/"kernel_compile.txt"); for(const auto& d:kernel.diagnostics) out<<d.format()<<'\n'; }
        if(!check(kernel.success,"AC9 real kernel compiles")) continue;
        os::NativeVfsImageBuilder fs(32768); check(fs.installBaseLayout().ok(),"AC5 real rootfs");
        check(fs.addExecutableImage("/bin/service_host",linked.assembled.program,linked.executable_header_v3,256).ok(),"AC1 real executable installed");
        std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n call service_test_kernel_setup\n mov r1, native_trap_entry\n csrw tvec, r1\n";
        const std::string path="/bin/service_host";
        for(size_t i=0;i<=path.size();++i) boot+=" mov r1, "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"\n mov r2, "+std::to_string(10020+i)+"\n store r1, r2, 0\n";
        boot+=" mov r13, 1\n mov r14, 10020\n mov r15, 7\n mov r16, 107\n call app_spawn\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
        boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
        auto feature=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
        auto boot_source=[&](int words) { return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(words)+", 0, 27, "+std::to_string(feature)+", 2, 0\n"; };
        auto assembled=vm::assembler::assembleV2(boot_source(1)); feature=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
        assembled=vm::assembler::assembleV2(boot_source(1)); if(assembled.success) assembled=vm::assembler::assembleV2(boot_source(static_cast<int>(assembled.program.size())));
        std::ofstream(dir/"boot.tasm") << boot_source(static_cast<int>(assembled.program.size()));
        if(!check(assembled.success,"AC9 real trap boot assembles")) continue;
        host::TosImageManifest manifest; const auto image=host::bootImageFromAssembly(assembled,manifest,fs.image());
        std::string error; check(host::writeBootImageFile((dir/"guest.tboot").string(),image,&error),"AC10 current boot image "+error);
        auto machine=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
        if(!check(host::loadBootImageIntoVm(*machine,image,"",&error),"AC9 normal process load "+error)) continue;
        ServiceHistory history;
        bool clock_red=false; long long clock_source=0,clock_query=0,clock_generation=0,clock_due=0,clock_eligible=0;
        long long premature_queries=0,premature_reservations=0,clock_exhausted_queries=0;
        std::vector<std::array<long long,6>> clock_samples;
        std::map<std::pair<long long,long long>,long long> actual_raw_source_cycles;
        const auto labelPc=[&](const std::string& name) { const auto it=linked.assembled.labels.find(name); return it==linked.assembled.labels.end()?-1:it->second; };
        const int source_pc=labelPc("svc_source"),source_return_pc=labelPc("svc_source_return"),idle_pc=labelPc("svc_idle_wait"),idle_return_pc=labelPc("svc_idle_wait_return");
        bool source_active=false,idle_active=false,source_capacity_at_entry=false; long long idle_csr=0,source_csr=0;
        long long source_full_observations=0; int idle_calls=0,idle_timed_calls=0,idle_duration_matches=0;
        std::set<int> source_reservations;
        auto clock_available=[&](const vm::VMState& m,int required_free) {
            if(serviceUserWord(m,10054)!=1 || serviceUserWord(m,10055)!=0) return false;
            const auto graph=serviceUserWord(m,10010),node=serviceUserWord(m,static_cast<int>(graph)+4),channel=serviceUserWord(m,10064);
            if(node<=0 || channel<=0 || serviceUserWord(m,static_cast<int>(node)+31)!=0 || serviceUserWord(m,static_cast<int>(node)+30)>=2) return false;
            if(serviceUserWord(m,static_cast<int>(channel)+4)>=serviceUserWord(m,static_cast<int>(channel)+1)) return false;
            int free=0; for(int p=2;p<scenario.pool;++p) if(serviceUserWord(m,10745+p*24)==0) ++free;
            return free>=required_free;
        };
        auto clock_query_wall=std::chrono::steady_clock::now();
        auto clock_last_wall=clock_query_wall;
        std::ofstream clock_trace(dir/"clock_observer.jsonl");
        std::ofstream clock_timing(dir/"clock_timing.jsonl");
        std::ofstream events(dir/"accepted_records.jsonl"), commits(dir/"committed_records.jsonl"), io(dir/"syscalls.jsonl"), state(dir/"state.jsonl"), retirement(dir/"retirement.jsonl"), jobs(dir/"jobs.jsonl"), cleanup(dir/"cleanup.jsonl");
        auto emitRecord=[](std::ostream& out,long long token,const ServiceRecord& r) { out<<"{\"token\":"<<token<<",\"words\":["; for(size_t i=0;i<r.size();++i) { if(i) out<<','; out<<r[i]; } out<<"]}\n"; };
        vm::VMHooks hooks;
        hooks.onInstruction=[&](const vm::VMState& observed,const vm::VMExecutionRecord& rec) {
            if(!rec.attempted || !rec.has_instruction) return;
            const int pid=static_cast<int>(serviceKernelWord(observed,3020));
            if(rec.before_privilege==vm::PrivilegeMode::User && pid==107) {
                if(rec.pc==source_pc) {
                    source_active=true; source_csr=0; source_reservations.clear(); source_capacity_at_entry=clock_available(observed,3);
                    const auto channel=serviceUserWord(observed,10064);
                    if(serviceUserWord(observed,10055)==0 && channel>0 && serviceUserWord(observed,static_cast<int>(channel)+4)>=serviceUserWord(observed,static_cast<int>(channel)+1)) {
                        ++source_full_observations;
                        clock_trace<<"{\"event\":\"source_full_downstream_observed\",\"cycle\":"<<observed.cycle_count<<",\"count\":"<<serviceUserWord(observed,static_cast<int>(channel)+4)<<",\"capacity\":"<<serviceUserWord(observed,static_cast<int>(channel)+1)<<"}\n";
                    }
                }
                if(rec.pc==source_return_pc) source_active=false;
                if(rec.pc==idle_pc) { idle_active=true; idle_csr=0; ++idle_calls; }
                if(rec.pc==idle_return_pc) idle_active=false;
                if(source_active && rec.instruction.opcode==isa::Opcode::CSRR && rec.instruction.imm==isa::CSR_CYCLE) source_csr=vm::ops::toLong(observed.regfile.read(rec.instruction.rd));
                if(idle_active && rec.instruction.opcode==isa::Opcode::CSRR && rec.instruction.imm==isa::CSR_CYCLE) idle_csr=vm::ops::toLong(observed.regfile.read(rec.instruction.rd));
            }
            if(rec.before_privilege==vm::PrivilegeMode::User && pid==107 && rec.instruction.opcode==isa::Opcode::STORE) {
                const long long address=vm::ops::toLong(observed.regfile.read(rec.instruction.rs1))+rec.instruction.imm;
                if(address==15612 && serviceUserWord(observed,15612)>0) io<<"{\"kind\":\"adapter_withheld_call\",\"count\":"<<serviceUserWord(observed,15612)<<",\"label\":\"explicit zero-progress fault adapter; no kernel write was requested\"}\n";
                if(address==10051) {
                    const int value=static_cast<int>(serviceUserWord(observed,10051));
                    if(value==0 && history.cleanup_mask==history.initialized_mask) { history.initialized_mask=0; history.cleanup_mask=0; }
                    else history.initialized_mask=value;
                }
                if(address==10052) {
                    const int value=static_cast<int>(serviceUserWord(observed,10052));
                    if(value!=0 && value!=history.cleanup_mask) {
                        const int delta=value&~history.cleanup_mask;
                        int expected=0; for(int bit=4;bit>=0;--bit) if((history.initialized_mask&(1<<bit)) && !(history.cleanup_mask&(1<<bit))) { expected=1<<bit; break; }
                        check(delta==expected && (value&~history.initialized_mask)==0,"AC7 actual initialized-only reverse cleanup transition");
                        cleanup<<"{\"initialized_mask\":"<<history.initialized_mask<<",\"cleanup_mask_before\":"<<history.cleanup_mask<<",\"cleanup_mask_after\":"<<value<<"}\n";
                    }
                    history.cleanup_mask=value;
                }
                if(address>=10180 && address<10370 && (address-10180)%38==0 && serviceUserWord(observed,static_cast<int>(address))==1) {
                    const int node=static_cast<int>((address-10180)/38);
                    history.startup_order.push_back(node);
                }
                if(address>=11513 && address<11641 && (address-11513)%16==6) {
                    const int at=11513+static_cast<int>((address-11513)/16)*16; std::array<long long,16> row{};
                    for(int k=0;k<16;++k) row[k]=serviceUserWord(observed,at+k);
                    if(row[0]>0 && (row[2]==1 || row[2]==2 || row[2]==3 || row[2]==4 || row[2]==-1)) {
                    const auto old=history.jobs.find(row[0]);
                    if(old!=history.jobs.end()) check(row[6]-old->second[6]<=1,"AC6 one bounded job step per callback");
                    history.jobs[row[0]]=row; jobs<<"{\"tick\":"<<serviceKernelWord(observed,3033)<<",\"words\":[";
                    for(int k=0;k<16;++k) { if(k) jobs<<','; jobs<<row[k]; } jobs<<"]}\n"; ++history.job_steps;
                    if(serviceUserWord(observed,10035)>0 && serviceUserWord(observed,10036)>0) ++history.jobs_during_partial;
                    }
                }
                if(address==9998 && serviceUserWord(observed,9998)==0) {
                    int pending=0; for(int p=0;p<32;++p) if(serviceUserWord(observed,10745+p*24+2)==1) ++pending;
                    int open=0; for(int f=0;f<2048;++f) if(serviceKernelWord(observed,290000+f*8)==107 && serviceKernelWord(observed,290005+f*8)>0) ++open;
                    cleanup<<"{\"actual_pending_at_reset\":"<<pending<<",\"actual_fds_at_reset\":"<<open<<",\"captured_highwater\":"<<history.stats[12]<<"}\n";
                    check(open==0,"AC7/AC8 arena reset follows actual descriptor closure");
                    check(history.cleanup_mask==history.initialized_mask,"AC7 all initialized services cleaned before arena reset");
                }
                if(source_active && address>=10793 && address<11513 && (address-10745)%24==0 && serviceUserWord(observed,static_cast<int>(address))==1) source_reservations.insert(static_cast<int>(address));
                if(clock_focus && source_active && ((clock_due>0 && observed.cycle_count<clock_due) || !source_capacity_at_entry) && address>=10793 && address<11513 && (address-10745)%24==0 && serviceUserWord(observed,static_cast<int>(address))==1 && serviceUserWord(observed,10055)==0) {
                    ++premature_reservations; clock_red=true;
                    clock_trace<<"{\"event\":\"premature_pool_reservation\",\"cycle\":"<<observed.cycle_count<<",\"required_due\":"<<clock_due<<",\"slot\":"<<address<<"}\n";
                }
                if(address>=10745 && address<11513 && (address-10745)%24==2) {
                    const int slot=static_cast<int>((address-10745)/24); const int at=10745+slot*24;
                    if(serviceUserWord(observed,at+2)==1) {
                        const auto token=serviceUserWord(observed,at+1); ServiceRecord record{};
                        for(int i=0;i<20;++i) record[i]=serviceUserWord(observed,at+4+i);
                        if(!history.accepted.count(token)) {
                            history.accepted[token]=record; emitRecord(events,token,record);
                            if(record[5]==2) {
                                const auto source=serviceUserWord(observed,10097);
                                actual_raw_source_cycles[{record[3],record[4]}]=source_csr;
                                const auto now=std::chrono::steady_clock::now();
                                const auto wall_ms=std::chrono::duration<double,std::milli>(now-clock_last_wall).count();
                                const bool interval_available=clock_source>0 && source>0;
                                const auto delta=interval_available?source-clock_source:0;
                                clock_samples.push_back({record[3],record[4],source,clock_query,observed.cycle_count,clock_eligible});
                                clock_timing<<"{\"generation\":"<<record[3]<<",\"sequence\":"<<record[4]<<",\"source_cycle\":"<<source<<",\"actual_metrics_entry_cycle\":"<<clock_query<<",\"accepted_cycle\":"<<observed.cycle_count<<",\"interval_cycles\":"<<delta<<",\"virtual_lateness_cycles\":"<<(clock_due>0?source-clock_due:0)<<",\"eligible_cycle_observed\":"<<clock_eligible<<",\"readiness_lag_cycles\":"<<(clock_eligible>0?source-clock_eligible:0)<<",\"interval_available\":"<<(interval_available?"true":"false")<<",\"wall_interval_ms\":"<<(interval_available?wall_ms:0)<<",\"wall_jitter_from_250ms\":"<<(interval_available?wall_ms-250:0)<<",\"slow_emulation_factor\":"<<(delta>0?wall_ms/(delta/1000000.0):0)<<"}\n";
                                // The planned sidecar is labelled separately from independent syscall timing.
                                check(source>0 && source==source_csr && source<=clock_query && clock_query-source<100000,"clock AC4 source sidecar equals actual source CSR read and aligns with metrics entry");
                                if(clock_source>0) { check(delta>=250000000,"clock AC4 >=250M between accepted samples"); if(delta<250000000) clock_red=true; }
                                clock_source=source>0?source:clock_query; clock_due=clock_source+250000000; clock_eligible=0; clock_last_wall=now;
                            }
                        }
                        else check(history.accepted.at(token)==record,"AC4 immutable accepted token");
                    }
                }
                if(address>=10745 && address<11513 && (address-10745)%24==0 && serviceUserWord(observed,static_cast<int>(address))==0) {
                    const auto token=serviceUserWord(observed,static_cast<int>(address)+1);
                    if(history.accepted.count(token)) check(history.synced_tokens.count(token)>0 || scenario.fatal || scenario.startup_failure,"AC5 accepted release follows fsync commit");
                }
                if(address==15539) {
                    for(int i=0;i<64;++i) history.stats[i]=serviceUserWord(observed,15476+i);
                    state<<"{\"tick\":"<<serviceKernelWord(observed,3033)<<",\"stats\":[";
                    for(int i=0;i<64;++i) { if(i) state<<','; state<<history.stats[i]; } state<<"],\"actual_open_fds\":";
                    int open=0; for(int f=0;f<2048;++f) if(serviceKernelWord(observed,290000+f*8)==107 && serviceKernelWord(observed,290005+f*8)>0) ++open;
                    state<<open<<"}\n";
                    check(history.stats[15]<=32 && history.stats[13]==0 && history.stats[12]<=3421,"AC1/AC4 bounded dispatch/allocation/highwater");
                    check(history.stats[17]==history.stats[18]+history.stats[19]+history.stats[20],"AC5 live record reconciliation");
                    check(history.stats[41]==open,"AC7/AC8 actual descriptors agree");
                    if(history.stats[31]>0) ++history.source_blocks;
                    if(scenario.name=="generation_prefix_zero_startup" && history.stats[54]==0 && history.stats[51]==1 && history.metrics.empty()) {
                        const auto marker=serviceInodeWords(observed,serviceFindInode(observed,"/var/log/svc.gen"));
                        if(marker.size()==3) history.marker_blocked=true;
                    }
                }
            }
            if(rec.before_privilege==vm::PrivilegeMode::User && pid==107 && rec.instruction.opcode==isa::Opcode::SYSCALL) {
                ServiceHistory::Call call; call.id=rec.syscall_id; call.args={rec.syscall_arg0,rec.syscall_arg1,rec.syscall_arg2,rec.syscall_arg3};
                if(call.id==12) {
                    for(int i=0;i<64;++i) { const auto c=serviceUserWord(observed,static_cast<int>(call.args[0])+i); if(c==0) break; if(c<0 || c>127) break; call.path+=static_cast<char>(c); }
                    if(call.args[1]==3) call.old_file=serviceInodeWords(observed,serviceFindInode(observed,call.path));
                }
                if(call.id==15 && call.args[2]>=0 && call.args[2]<=500) for(int i=0;i<call.args[2];++i) call.words.push_back(serviceUserWord(observed,static_cast<int>(call.args[1])+i));
                if(clock_focus && call.id==56 && idle_active) {
                    ++idle_timed_calls;
                    const auto deadline=serviceUserWord(observed,10007),remaining=deadline-idle_csr;
                    const auto expected_ms=remaining>0?(remaining+999999)/1000000:0;
                    const bool matched=idle_csr>0 && deadline==clock_due && call.args[0]==expected_ms && observed.cycle_count>=idle_csr;
                    if(matched) ++idle_duration_matches; else clock_red=true;
                    check(matched,"clock AC5 actual idle helper duration is ceil remaining at independently observed CYCLE");
                    clock_trace<<"{\"event\":\"actual_idle_helper_sleep\",\"csr_cycle\":"<<idle_csr<<",\"entry_cycle\":"<<observed.cycle_count<<",\"deadline\":"<<deadline<<",\"requested_ms\":"<<call.args[0]<<",\"expected_remaining_ms\":"<<expected_ms<<",\"measured_instruction_gap_cycles\":"<<observed.cycle_count-idle_csr<<"}\n";
                }
                if(call.id==60) {
                    const auto generation=serviceUserWord(observed,10005);
                    if(serviceUserWord(observed,10054)==0 || generation!=clock_generation) { clock_generation=generation; clock_source=0; clock_due=0; clock_eligible=0; }
                    else {
                        clock_query=observed.cycle_count; clock_query_wall=std::chrono::steady_clock::now();
                        if(clock_due>0 && clock_query<clock_due) { ++premature_queries; clock_red=true; }
                        int available=0; for(int p=2;p<scenario.pool;++p) if(serviceUserWord(observed,10745+p*24)==0) ++available;
                        bool owns_three=source_active && source_reservations.size()==3;
                        for(int at:source_reservations) owns_three=owns_three && serviceUserWord(observed,at)==1 && serviceUserWord(observed,at+1)==0 && serviceUserWord(observed,at+2)==0;
                        // Reservations are observed from this actual source invocation, not inferred from a generic arena total.
                        if(clock_focus && (!owns_three || !clock_available(observed,0))) { ++clock_exhausted_queries; clock_red=true; }
                        if(clock_focus) check(owns_three && clock_available(observed,0),"clock AC4 actual source owns exactly three credits with downstream capacity at metrics admission");
                        if(clock_eligible>0) check(clock_query-clock_eligible<=250000000,"clock AC4 eligible idle ERET to admission within one sampling period");
                        clock_trace<<"{\"event\":\"actual_metrics_query\",\"cycle\":"<<clock_query<<",\"required_due\":"<<clock_due<<",\"reserved_credits\":"<<serviceUserWord(observed,10029)<<",\"available_credits\":"<<available<<",\"source_sidecar\":"<<serviceUserWord(observed,10097)<<",\"actual_source_csr\":"<<source_csr<<"}\n";
                    }
                }
                if(call.id==60) {
                    size_t raw_count=0; for(const auto& pair:history.accepted) if(pair.second[5]==2) ++raw_count;
                    const size_t ordinal=raw_count%16+1;
                    const bool high=ordinal>=6 && ordinal<=8, zero=ordinal==12;
                    // The SYSCALL has entered its real trap; kernel metrics has not read the queue yet.
                    machine->dmem.store(132000,vm::ops::fromLong(zero?0:1));
                    machine->dmem.store(132011,vm::ops::fromLong(0));
                    machine->dmem.store(132015,vm::ops::fromLong(high?8:1));
                }
                history.pending[107]=call;
            }
            if(rec.instruction.opcode==isa::Opcode::ERET && rec.after_privilege==vm::PrivilegeMode::User && pid==107 && history.pending.count(107)) {
                const auto call=history.pending.at(107); history.pending.erase(107);
                const auto status=vm::ops::toLong(observed.regfile.read(13)), payload=vm::ops::toLong(observed.regfile.read(14)), detail=vm::ops::toLong(observed.regfile.read(15));
                io<<"{\"id\":"<<call.id<<",\"status\":"<<status<<",\"payload\":"<<payload<<",\"detail\":"<<detail<<",\"path\":\""<<jsonEscape(call.path)<<"\",\"args\":["<<call.args[0]<<','<<call.args[1]<<','<<call.args[2]<<','<<call.args[3]<<"],\"label\":\"actual kernel return; prefix requests are labelled adapter requests\"}\n";
                if(call.id==12) {
                    if(status>=0 && payload>=0) {
                        history.paths[payload]=call.path; history.offsets[payload]=0; history.written[payload].clear();
                        if(call.args[1]==3 && !call.old_file.empty() && (call.path=="/var/log/svc.0" || call.path=="/var/log/svc.1")) {
                            check(call.old_file.size()%20==0 && call.old_file.size()<=1280,"AC5 truncate only complete <=64 records");
                            for(size_t i=0;i+20<=call.old_file.size();i+=20) { ServiceRecord r{}; std::copy_n(call.old_file.begin()+i,20,r.begin()); history.retired.push_back(r); emitRecord(retirement,(r[2]*1024+r[3])*1048576+r[4],r); }
                        }
                    } else if(call.args[1]==3) ++history.failed_truncates;
                }
                if(call.id==15 && status>=0 && payload>=0) {
                    if(payload==0) ++history.zero_calls;
                    if(payload>0 && call.args[2]<20) ++history.prefix_calls;
                    check(payload<=static_cast<long long>(call.words.size()),"AC5 write result bounded by actual request");
                    if(payload>0 && payload<=static_cast<long long>(call.words.size())) {
                        history.written[call.args[0]].insert(history.written[call.args[0]].end(),call.words.begin(),call.words.begin()+payload);
                        history.offsets[call.args[0]]+=payload;
                    }
                }
                if(call.id==47) {
                    if(status<0) ++history.failed_syncs;
                    else if(history.paths[call.args[0]]=="/var/log/svc.0" || history.paths[call.args[0]]=="/var/log/svc.1") {
                        auto& words=history.written[call.args[0]];
                        check(words.size()%20==0,"AC5 logger fsync at complete record boundary");
                        const auto actual=serviceInodeWords(observed,serviceFindInode(observed,history.paths[call.args[0]]));
                        check(actual==words,"AC5 actual VFS prefix words equal observed writes at fsync");
                        for(size_t i=0;i+20<=words.size();i+=20) {
                            ServiceRecord r{}; std::copy_n(words.begin()+i,20,r.begin()); const auto token=(r[2]*1024+r[3])*1048576+r[4];
                            if(!history.synced_tokens.count(token)) { history.synced_tokens.insert(token); history.commits.push_back(r); emitRecord(commits,token,r); check(history.accepted.count(token)>0 && history.accepted.at(token)==r,"AC5 commit matches immutable accepted record"); }
                        }
                    }
                }
                if(clock_focus && call.id==56 && clock_due>0 && observed.cycle_count>=clock_due && clock_available(observed,3)) {
                    clock_eligible=observed.cycle_count;
                    clock_trace<<"{\"event\":\"eligible_after_actual_idle_eret\",\"cycle\":"<<clock_eligible<<",\"deadline\":"<<clock_due<<",\"dispatch_attempts\":"<<serviceUserWord(observed,10015)<<",\"source_pending\":0,\"downstream_available\":true,\"free_credits_at_least\":3}\n";
                }
                if(call.id==60 && status>=0 && payload==7) { std::array<long long,7> r{}; for(int i=0;i<7;++i) r[i]=serviceUserWord(observed,static_cast<int>(call.args[0])+i); history.metrics.push_back(r); }
                if(call.id==46 && status>=0) {
                    for(int n=0;n<payload;++n) { std::array<long long,5> row{}; for(int i=0;i<5;++i) row[i]=serviceUserWord(observed,static_cast<int>(call.args[0])+n*5+i); history.snapshots.push_back(row); }
                    if(scenario.job_case && !history.namespace_changed) { machine->dmem.store(390000+12*9+1,vm::ops::fromLong(1)); history.namespace_changed=true; }
                }
                if(call.id==51 && status>=0 && payload==8) { std::array<long long,8> row{}; for(int i=0;i<8;++i) row[i]=serviceUserWord(observed,static_cast<int>(call.args[1])+i); history.details.push_back(row); if(row[0]==112 && row[7]==1) history.rechecked=true; }
            }
        };
        bool reached=false; long long total_steps=0;
        std::ofstream progress(dir/"progress.jsonl");
        long long next_progress=0; bool saved_zero_wait=false;
        auto progress_time=std::chrono::steady_clock::now();
        while(total_steps<3000000000LL && machine->status!=vm::VMStatus::TRAPPED && machine->status!=vm::VMStatus::HALTED) {
            // Bounded chunks let fixtures change only between actual guest operations.
            const auto result=vm::run(*machine,1000,&hooks); total_steps+=result.steps;
            const auto progress_now=std::chrono::steady_clock::now();
            if(total_steps>=next_progress || (result.steps==0 && std::chrono::duration<double>(progress_now-progress_time).count()>=5)) {
                progress<<"{\"pc\":"<<machine->pc<<",\"pid\":"<<serviceKernelWord(*machine,3020)<<",\"cycle\":"<<machine->cycle_count
                        <<",\"status\":"<<static_cast<int>(machine->status)<<",\"privilege\":"<<static_cast<int>(machine->privilege)
                        <<",\"result_steps\":"<<result.steps<<",\"total_steps\":"<<total_steps<<",\"kernel_tick\":"<<serviceKernelWord(*machine,3033)
                        <<",\"timer_enable\":"<<(machine->timer_enable?"true":"false")<<",\"timer_counter\":"<<machine->timer_counter
                        <<",\"timer_pending\":"<<(machine->timer_pending?"true":"false")<<",\"epc\":"<<machine->epc<<",\"scratch_context\":"<<machine->scratch
                        <<",\"kernel_context\":"<<serviceKernelWord(*machine,3019)<<",\"host_context\":"<<serviceKernelWord(*machine,390000+7*9+5)
                        <<",\"host_lifecycle\":"<<serviceUserWord(*machine,10001)<<",\"host_error\":"<<serviceUserWord(*machine,10002)
                        <<",\"host_ready\":"<<serviceUserWord(*machine,10054)
                        <<",\"startup_graph\":"<<serviceUserWord(*machine,10009)<<",\"persistent_graph\":"<<serviceUserWord(*machine,10010)
                        <<",\"arena_used_stat\":"<<serviceUserWord(*machine,10011)<<",\"captured_highwater\":"<<serviceUserWord(*machine,10012)
                        <<",\"arena_metadata\":["<<serviceUserWord(*machine,9996)<<','<<serviceUserWord(*machine,9997)<<','<<serviceUserWord(*machine,9998)<<','<<serviceUserWord(*machine,9999)
                        <<"],\"guest_failure_count\":"<<serviceUserWord(*machine,15600)<<",\"guest_first_failure\":"<<serviceUserWord(*machine,15601)<<"}\n";
                progress.flush(); next_progress=total_steps+1000000; progress_time=progress_now;
                if(!saved_zero_wait && result.steps==0 && machine->status==vm::VMStatus::WAITING) {
                    (void)host::detail::writeCheckpointState(dir/"diagnostic_zero_wait_state.bin",*machine,&error); saved_zero_wait=true;
                }
            }
            if(clock_focus && clock_red) break; // Concrete initial Red: retain incomplete scenario, no repeated failing history.
            if(clock_focus && clock_due>0 && machine->cycle_count>=clock_due && clock_eligible==0 && serviceUserWord(*machine,10055)==0) {
                if(clock_available(*machine,3)) clock_eligible=machine->cycle_count;
            }
            if(clock_focus && clock_eligible>0) {
                if(!clock_available(*machine,3)) clock_eligible=0;
                else if(machine->cycle_count-clock_eligible>250000000) { check(false,"clock AC4 eligible source admission exceeds one250M sampling period"); clock_red=true; break; }
            }
            if(serviceUserWord(*machine,15602)==777) { reached=true; break; }
            // Labelled controlled guest window queue. Sample6 is made high before its metric read.
            size_t raw=0; for(const auto& pair:history.accepted) if(pair.second[5]==2) ++raw;
            const size_t ordinal=raw+1, cycle=(ordinal-1)%16+1;
            const bool high=cycle>=6 && cycle<=8, zero=cycle==12;
            machine->dmem.store(132000,vm::ops::fromLong(zero?0:1));
            machine->dmem.store(132011,vm::ops::fromLong(0));
            machine->dmem.store(132015,vm::ops::fromLong(high?8:1));
            if(scenario.job_case && !history.snapshots.empty() && !history.namespace_changed) { machine->dmem.store(390000+12*9+1,vm::ops::fromLong(1)); history.namespace_changed=true; }
            bool all_other_blocked=true; for(int p=0;p<100;++p) if(p!=7 && serviceKernelWord(*machine,390000+p*9+2)==1) all_other_blocked=false;
            if(machine->status==vm::VMStatus::WAITING && all_other_blocked) ++history.blocked_idle;
            if(result.steps==0 && machine->status!=vm::VMStatus::WAITING) break;
        }
        if(clock_focus) {
            check(premature_queries==0 && premature_reservations==0 && clock_exhausted_queries==0,"clock AC4 no metrics/pool work before due or when exhausted");
            check(reached,"clock AC4/5 focused >=24/stalls/control/drain/EOS/restart complete (initialRed is explicitly incomplete)");
            std::map<long long,int> per_generation; for(const auto& r:clock_samples) ++per_generation[r[0]];
            check(per_generation.size()>=2,"clock AC4 actual restart observed"); for(const auto& row:per_generation) check(row.second>=24,"clock AC4 >=24RAW per generation each capacity");
            check(idle_calls>0 && idle_timed_calls>0 && idle_duration_matches==idle_timed_calls,"clock AC5 actual production idle helper invoked and every timed sleep matches remaining deadline");

        }
        clock_trace.close(); clock_timing.close();
        progress.close(); events.close(); commits.close(); io.close(); state.close(); retirement.close(); jobs.close(); cleanup.close();
        check(reached,"AC10 "+scenario.name+" bounded scenario completed actual guest");
        check(serviceUserWord(*machine,15600)==0,"guest semantic assertions "+scenario.name+" first_code="+std::to_string(serviceUserWord(*machine,15601)));
        check(serviceKernelWord(*machine,390000+7*9)==107,"AC9 actual PID107 slot7");
        check(machine->status!=vm::VMStatus::TRAPPED,"AC9 no VM trap");
        const auto context=serviceKernelWord(*machine,390000+7*9+5), page_table=serviceKernelWord(*machine,static_cast<int>(context)+4);
        // BEGIN SERVICE FIXTURE VALUES
        if(scenario.name=="tokens_reuse_restart" || scenario.name=="control_full_stall_cancel") {
            auto numeric_input=[&](int address) {
                const auto pte=serviceKernelWord(*machine,static_cast<int>(page_table)+address/vm::MMU_PAGE_WORDS);
                const auto loaded=machine->dmem.load(static_cast<int>(pte/19683*vm::MMU_PAGE_WORDS)+address%vm::MMU_PAGE_WORDS);
                check(pte>0 && loaded.second==vm::MemFaultCode::OK,"fixture numeric actual mapped word");
                return loaded.first;
            };
            const auto upper=numeric_input(15640),stride=numeric_input(15641);
            const auto actual=scenario.name=="tokens_reuse_restart"?stride:upper;
            const long long converted=vm::ops::toLong(actual);
            const long long expected=scenario.name=="tokens_reuse_restart"?1073741824LL:2779530283277763LL;
            const bool typed=actual.mode==TernaryMode::T40 && !actual.isInvalid();
            const bool positive=typed && native_ops::compare(actual.asTriple(),native_ops::fromIntT40(0))>0;
            const bool integral=typed && native_ops::compare(actual.asTriple(),native_ops::fromIntT40(converted))==0;
            check(typed && positive && integral && converted==expected,"fixture numeric actual positive integral exact target "+scenario.name);
            std::ofstream numeric(dir/"fixture_numeric_inputs.json");
            if(check(numeric.is_open(),"fixture numeric opens actual input capture")) {
                numeric << "{\"scenario\":\""<<scenario.name<<"\",\"actual_15640\":"<<vm::ops::toLong(upper)<<",\"actual_15641\":"<<vm::ops::toLong(stride)
                        <<",\"actual_mode\":"<<static_cast<int>(actual.mode)<<",\"actual_t40\":"<<(typed?"true":"false")<<",\"actual_positive\":"<<(positive?"true":"false")
                        <<",\"actual_integral\":"<<(integral?"true":"false")<<",\"actual_value\":"<<converted<<",\"expected_value\":"<<expected<<",\"exact_target\":"<<(converted==expected?"true":"false")<<"}\n";
                numeric.close();check(!numeric.fail(),"fixture numeric checked capture write/close");
            }
        }
// END SERVICE FIXTURE VALUES
        for(int vpn=13;vpn<=21;++vpn) check(serviceKernelWord(*machine,static_cast<int>(page_table)+vpn)>0,"AC1/AC9 existing private scratch PTE present");
        check(serviceKernelWord(*machine,static_cast<int>(page_table)+22)==0,"AC1 no expansion past current scratch VPN21");
        if(!scenario.startup_failure) {
            check(history.startup_order.size()>=5,"AC7 five actual successful readiness transitions");
            if(history.startup_order.size()>=5) check(history.startup_order[0]==0 && history.startup_order[1]==1 && history.startup_order[2]==2 && std::set<int>{history.startup_order[3],history.startup_order[4]}==std::set<int>{3,4},"AC7 dependency startup order");
        }
        if(scenario.name=="invalid_kind" || scenario.name=="duplicate_edge" || scenario.name=="cycle" || scenario.name=="zero_channel" || scenario.name.rfind("pool",0)==0 || scenario.name.rfind("persistent_",0)==0)
            check(history.paths.empty() && history.metrics.empty() && history.startup_order.empty(),"AC7 rejected construction/graph has zero VFS/source startup effects");
        check(machine->block_device.writeSnapshotFile((dir/"disk.tdisk").string()),"AC10 actual guest disk snapshot");
        check(host::detail::writeCheckpointState(dir/"vm_state.bin",*machine,&error),"AC10 full process/kernel snapshot");
        std::vector<ServiceRecord> raw,windows,alerts,recoveries;
        for(const auto& r:history.commits) { check(r[18]==0,"AC9 every committed record carries caller namespace aux0"); if(r[5]==2) raw.push_back(r); if(r[5]==3) windows.push_back(r); if(r[5]==4) alerts.push_back(r); if(r[5]==5) recoveries.push_back(r); }
        for(size_t i=0;i<raw.size();++i) {
            const auto current_source=actual_raw_source_cycles.find({raw[i][3],raw[i][4]});
            check(current_source!=actual_raw_source_cycles.end(),"AC2 committed RAW has independently observed actual source CYCLE");
            if(i && raw[i][3]==raw[i-1][3]) {
                const auto previous_source=actual_raw_source_cycles.find({raw[i-1][3],raw[i-1][4]});
                check(current_source!=actual_raw_source_cycles.end() && previous_source!=actual_raw_source_cycles.end() && current_source->second-previous_source->second>=250000000,"AC2 actual CYCLE spacing >=250M");
            }
            const std::array<long long,7> metric={raw[i][11],raw[i][12],raw[i][13],raw[i][14],raw[i][15],raw[i][16],raw[i][17]};
            check(std::find(history.metrics.begin(),history.metrics.end(),metric)!=history.metrics.end(),"AC2 raw equals an actual kernel metrics response");
            check(raw[i][2]==107 && raw[i][3]>0 && raw[i][4]>0,"AC4 record identity");
            check(raw[i][18]==0,"AC2/AC9 original metrics attributed to actual caller namespace");
        }
        std::vector<ServiceRecord> expected_windows; std::vector<long long> expected_alerts,expected_recoveries;
        std::array<long long,7> sums{}; int count=0,high_streak=0,low_streak=0; bool pressure=false; long long first=0;
        for(size_t i=0;i<raw.size();++i) {
            if(i && raw[i][3]!=raw[i-1][3]) { high_streak=0; low_streak=0; pressure=false; }
            const auto& r=raw[i]; if(count==0) first=r[4]; for(int k=0;k<7;++k) sums[k]+=r[11+k]; ++count;
            const bool high=r[17]>0 && r[16]*5>=r[17]*4;
            if(high) { ++high_streak; low_streak=0; if(high_streak==3 && !pressure) { pressure=true; expected_alerts.push_back(r[4]); } }
            else { ++low_streak; high_streak=0; if(low_streak==3 && pressure) { pressure=false; expected_recoveries.push_back(r[4]); } }
            if(count==8 || i+1==raw.size() || raw[i+1][3]!=r[3]) { ServiceRecord window{}; window[8]=count; window[9]=first; window[10]=r[4]; for(int k=0;k<7;++k) window[11+k]=sums[k]; expected_windows.push_back(window); sums={}; count=0; }
        }
        if(!scenario.fatal && !scenario.startup_failure) {
            check(windows.size()==expected_windows.size(),"AC3 every full/partial window present");
            for(size_t i=0;i<std::min(windows.size(),expected_windows.size());++i) for(int k=8;k<=17;++k) check(windows[i][k]==expected_windows[i][k],"AC3 independent integer sums/count/range");
            check(alerts.size()==expected_alerts.size() && recoveries.size()==expected_recoveries.size(),"AC3 independent high/low transition counts");
            for(size_t i=0;i<std::min(alerts.size(),expected_alerts.size());++i) check(alerts[i][9]==expected_alerts[i],"AC3 three-high triggering sample");
            for(size_t i=0;i<std::min(recoveries.size(),expected_recoveries.size());++i) check(recoveries[i][9]==expected_recoveries[i],"AC3 three-low triggering sample");
            std::set<long long> identities; for(const auto& r:history.commits) check(identities.insert((r[2]*1024+r[3])*1048576+r[4]).second,"AC2 no duplicate committed identities");
            check(history.accepted.size()==history.commits.size(),"AC2/AC8 every accepted record commits on graceful drain");
            for(const auto& pair:history.accepted) check(history.synced_tokens.count(pair.first)>0,"AC2 no missing accepted token");
            bool eos=false; long long generation=0; for(const auto& r:history.commits) { if(r[3]!=generation) { eos=false; generation=r[3]; } if(r[5]==7) eos=true; if(r[5]==3) check(!eos,"AC3 final partial window precedes EOS"); }
            std::map<long long,std::vector<ServiceRecord>> generations;
            for(const auto& r:history.commits) generations[r[3]].push_back(r);
            for(const auto& pair:generations) {
                int starts=0,stops=0,ends=0; bool stopped=false,ended=false;
                for(size_t n=0;n<pair.second.size();++n) {
                    const auto& r=pair.second[n];
                    if(r[5]==1) { ++starts; check(n==0,"AC8 START first once per successful generation"); }
                    if(r[5]==8) { ++stops; stopped=true; check(!ended,"AC8 nonterminal STOP precedes EOS"); }
                    if(r[5]==3 && stopped) check(r[8]<8 && !ended,"AC8 final partial WINDOW after STOP before EOS");
                    if(r[5]==7) { ++ends; ended=true; check(stopped && n+1==pair.second.size(),"AC8 EOS final after committed STOP and partial"); }
                }
                check(starts==1 && stops==1 && ends==1,"AC8 exactly one START STOP EOS per successful generation");
            }
        }
        if(scenario.long_history) {
            check(raw.size()>=256,"AC2 >=256 independently archived real raw samples each capacity");
            check(!alerts.empty() && !recoveries.empty(),"AC3 pressure lifecycle exercised");
            bool capacity_zero=false; for(const auto& r:raw) if(r[17]==0) capacity_zero=true; check(capacity_zero,"AC3 capacity0 actually observed");
            check(history.prefix_calls>0 && history.stats[33]>0,"AC5 labelled positive-prefix and zero retries exercised");
            check(serviceUserWord(*machine,15612)>=100,"AC2/AC5 withheld stalls recur across the full long history");
            check(history.retired.size()>0 && history.stats[40]>=2,"AC5 two-file rotation and explicit retirement exercised");
            check(history.blocked_idle>0,"AC2 all other processes blocked/no-input idle exercised");
            check(history.stats[47]>0,"AC6/AC8 queued jobs cooperatively cancelled during drain");
        }
        if(scenario.worst_case) {
            bool ordered=false; for(size_t i=0;i+2<history.commits.size();++i) if(history.commits[i][5]==2 && history.commits[i+1][5]==3 && history.commits[i+2][5]==4 && history.commits[i+1][10]==history.commits[i][4] && history.commits[i+2][9]==history.commits[i][4]) ordered=true;
            check(ordered && raw.size()>=12,"AC3/AC4 RAW8 WINDOW8 ALERT8/full-pool progress");
            check(history.source_blocks>0,"AC4 exhausted source blocks before acceptance");
        }
        if(scenario.job_case) { check(history.stats[42]>=8 && history.stats[43]>=1,"AC6 eight admissions/ninth rejection"); check(history.rechecked,"AC6 actual PID namespace changed and getproc rechecked"); check(history.stats[45]>0,"AC6 report completions"); }
        if(scenario.job_case) { check(history.jobs.size()>=8 && history.job_steps>=8,"AC6 independent steps for all admitted jobs"); for(const auto& pair:history.jobs) check(pair.second[6]>0,"AC6 unrelated job not starved by stalled logger"); }
        if(scenario.job_case) check(history.jobs_during_partial>0,"AC6 actual unrelated job step while logger retains positive-prefix remainder");
        if(scenario.name=="generation_prefix_zero_startup") check(history.marker_blocked,"AC7 partial real generation marker blocks READY and metrics, then resumes");
        if(scenario.name=="control_full_stall_cancel") {
            std::vector<std::pair<long long,long long>> actual;
            for(const auto& r:history.commits) if(r[5]==6) actual.emplace_back(r[7],r[11]);
            check(actual==std::vector<std::pair<long long,long long>>{{-1,39},{-2,38},{-4,36},{-5,35}},"AC2/AC3 all non-EOS control tags/values forward once in acceptance order across stalls/cancel");
        }
        if(scenario.fatal) { check(history.stats[2]<0 && history.stats[60]>0,"AC5 fatal exact nonzero pending"); check(history.accepted.size()-history.commits.size()==static_cast<size_t>(history.stats[60]),"AC5 independently counted fatal uncommitted"); }
        if(scenario.name=="fsync_failure") check(history.failed_syncs>0,"AC5 actual negative fsync observed");
        if(scenario.name=="failed_rotation_no_retirement") check(history.failed_truncates>0 && history.retired.empty(),"AC5 failed truncation retires nothing");
        size_t retained_records=0;
        std::set<long long> retained_tokens;
        for(const auto& path:std::vector<std::string>{"/var/log/svc.0","/var/log/svc.1","/var/log/svc.gen","/var/log/svc.report"}) {
            const auto content=serviceInodeWords(*machine,serviceFindInode(*machine,path)); serviceWriteWords(dir/(path.substr(path.find_last_of('/')+1)+".json"),content);
            if(path=="/var/log/svc.0" || path=="/var/log/svc.1") {
                check(content.size()<=1280,"AC5 retained files bounded64 records");
                for(size_t n=0;n+20<=content.size();n+=20) {
                    const long long token=(content[n+2]*1024+content[n+3])*1048576+content[n+4];
                    if(history.synced_tokens.count(token)) { ++retained_records; check(retained_tokens.insert(token).second,"AC5 no retained duplicate"); }
                }
            }
            if(path=="/var/log/svc.report" && scenario.job_case) {
                check(!content.empty() && content.size()%16==0,"AC6 actual complete report rows");
                bool ps=false,detail_row=false;
                for(size_t n=0;n+16<=content.size();n+=16) {
                    check(content[n+10]==0 && content[n+6]!=111,"AC6 report caller namespace");
                    check(content[n+1]==1 && (content[n+5]==1 || content[n+5]==2),"AC6 report version and process row kind");
                    if(content[n+5]==1) {
                        ps=true; check(content[n+11]==0 && content[n+12]==0 && content[n+13]==0,"AC6 ps reserved fields zero");
                        const std::array<long long,5> row={content[n+6],content[n+7],content[n+8],content[n+9],content[n+10]};
                        check(std::find(history.snapshots.begin(),history.snapshots.end(),row)!=history.snapshots.end(),"AC6 report ps fields equal actual five-word syscall snapshot");
                    }
                    if(content[n+5]==2) {
                        detail_row=true; check(content[n+6]!=112,"AC6 detail excludes PID whose namespace changed after ps");
                        const std::array<long long,8> row={content[n+6],content[n+7],content[n+11],content[n+12],content[n+13],content[n+8],content[n+9],content[n+10]};
                        check(std::find(history.details.begin(),history.details.end(),row)!=history.details.end(),"AC6 report detail fields equal actual rechecked eight-word syscall result");
                    }
                    check(content[n+14]==0 && content[n+15]==0,"AC6 report no extra payload/address fields");
                }
                check(ps && detail_row,"AC6 both actual process row formats reported");
            }
        }
        if(!scenario.startup_failure) {
            check(history.commits.size()==retained_records+history.retired.size(),"AC5 independent committed=actual retained+explicit retired");
            check(history.accepted.size()==retained_records+history.retired.size()+static_cast<size_t>(scenario.fatal?history.stats[60]:0),"AC5 independent accepted=retained+retired+pending");
        }
        std::ofstream reconciliation(dir/"reconciliation.json"); reconciliation<<"{\"accepted\":"<<history.accepted.size()<<",\"committed\":"<<history.commits.size()<<",\"retired\":"<<history.retired.size()<<",\"raw\":"<<raw.size()<<",\"windows\":"<<windows.size()<<",\"alerts\":"<<alerts.size()<<",\"recoveries\":"<<recoveries.size()<<",\"adapter_withheld_calls\":"<<serviceUserWord(*machine,15612)<<",\"actual_kernel_zero_write_returns\":"<<history.zero_calls<<",\"actual_prefix_requests\":"<<history.prefix_calls<<",\"forced_stop_replay_supported\":false,\"observer\":\"instruction hooks; full real immutable records before release; kernel VFS checked after fsync\",\"fixture\":\"controlled kernel window queues; no external input\",\"steps\":"<<total_steps<<"}\n";
        if(clock_focus) {
            check(source_full_observations>0,"clock AC4 explicitly stalled full downstream source observed");
            std::ofstream(dir/"clock_report.json")<<"{\"status\":\""<<(reached && !clock_red && failures.size()==scenario_failure_start?"PASS":"FAIL")<<"\",\"scenario_complete\":"<<(reached?"true":"false")<<",\"early_metrics_queries\":"<<premature_queries<<",\"early_pool_reservations\":"<<premature_reservations<<",\"exhausted_metrics_queries\":"<<clock_exhausted_queries<<",\"source_full_observations\":"<<source_full_observations<<",\"raw_observed\":"<<clock_samples.size()<<",\"required_raw_per_generation\":24,\"required_generations\":2,\"source_counter_field\":97,\"scenario_failure_count\":"<<failures.size()-scenario_failure_start<<",\"idle_calls\":"<<idle_calls<<",\"idle_timed_calls\":"<<idle_timed_calls<<",\"idle_duration_matches\":"<<idle_duration_matches<<",\"readiness_bound_cycles\":250000000,\"idle_helper_exercised\":"<<(idle_calls>0?"true":"false")<<"}\n";
        }
        ran=true;
    }
    // BEGIN SERVICE LINK PREFLIGHT
    if(link_only) {
        check(cases.size()==33 && link_attempted.size()==cases.size(),"link preflight exact original33 coverage");
        std::ostringstream report;
        report << "{\"status\":\"" << (failures.empty()?"PASS":"FAIL") << "\",\"scope\":\"compile-link-only\",\"runtime_executed\":false,\"requested\":" << cases.size()
               << ",\"attempted_count\":" << link_attempted.size() << ",\"compiled_count\":" << link_compiled.size() << ",\"linked_count\":" << link_succeeded.size()
               << ",\"compiler_options\":{\"enable_vector_abi\":true,\"enable_vector_spilling\":true},\"link_options\":{\"stack_hint_words\":2052,\"dead_strip_functions\":true,\"standalone_halt_on_exit\":false,\"enable_vector_abi\":true,\"enable_vector_spilling\":true}";
        auto names=[&](const char* key,const std::vector<std::string>& rows) {
            report << ",\"" << key << "\":[";
            for(size_t i=0;i<rows.size();++i) { if(i) report<<','; report<<'"'<<jsonEscape(rows[i])<<'"'; }
            report << ']';
        };
        names("attempted",link_attempted); names("compiled",link_compiled); names("linked",link_succeeded); names("failures",failures);
        report << ",\"source_hash_algorithm\":\"FNV-1a-64\",\"source_hashes\":{";
        for(size_t i=0;i<inputs.size();++i) { if(i) report<<','; report<<'"'<<jsonEscape(inputs[i])<<"\":\""<<stableHash(readTextFile(inputs[i]))<<'"'; }
        report << "}}\n";
        link_write(base/"link_preflight_report.json",report.str());
        return failures.empty();
    }
// END SERVICE LINK PREFLIGHT
    const auto builder=readTextFile("build_tos_image.cpp"), manifest=readTextFile("APP_MANIFEST.json");
    check(std::regex_search(builder,std::regex("\\{\\\"service_host\\\", \\\"service_host\\\",.*\\\"/bin/service_host\\\"")),"AC1 builder host registration");
    const std::regex host_path("\\\"guest_path\\\"\\s*:\\s*\\\"/bin/service_host\\\"");
    check(std::distance(std::sregex_iterator(manifest.begin(),manifest.end(),host_path),std::sregex_iterator())==1,"AC1 manifest one host");
    check(!production.empty(),"AC1 production service source exists");
    check(ran,"AC10 at least one actual production-linked service guest ran");
    // Use the same exact GUI workflow; image/observations are the only variations.
    if(ran && !production.empty()) {
        if(automatic) {
            check(builder.find("buildBootExecAssembly(\"/bin/init\")")!=std::string::npos,"AC11 production builder boots actual init");
            for(int mode:{1,2,3}) {
                const auto root=base/"automatic_start"/(mode==1?"normal":mode==2?"spawn_failure":"startup_failure");
                const bool image_ok=check(serviceDesktopImage(root/"image",mode,true),"AC11 automatic fixture image");
                const bool desktop_ok=image_ok && check(runDesktopWorkflow(root/"image",root/"desktop",mode,true),"AC11 exact desktop with automatic host/failure");
                automatic_modes.emplace_back(root.filename().string(),image_ok && desktop_ok);
                std::ofstream mode_report(root/"mode_report.json");
                mode_report<<"{\"status\":\""<<(image_ok && desktop_ok?"PASS":"FAIL")<<"\",\"mode\":\""<<root.filename().string()<<"\",\"image_generated\":"<<(image_ok?"true":"false")<<",\"exact_desktop_passed\":"<<(desktop_ok?"true":"false")<<",\"required_launch_count\":1,\"required_launch_before_exec\":true,\"evidence\":\"desktop/host_before; desktop/host_after; desktop/health; desktop/input_journal.jsonl\",\"source_hash_algorithm\":\"FNV-1a-64\",\"conformance_hash\":\""<<stableHash(readTextFile("tests/current_only_conformance.cpp"))<<"\"}\n";
            }
        } else if(selected.empty()) {
            const auto root=base/"coexistence";
            check(serviceDesktopImage(root/"image",1,false),"AC9 on-demand coexistence image");
            check(runDesktopWorkflow(root/"image",root/"desktop",1,false),"AC9 exact desktop with host progress");
        }
    }
    const auto report_path=automatic?base/"automatic_start"/"automatic_report.json":base/"report.json";
    std::filesystem::create_directories(report_path.parent_path());
    { std::ofstream report(report_path); report<<"{\"status\":\""<<(failures.empty()?"PASS":"FAIL")<<"\",\"stage\":\""<<(production.empty()?"missing-feature-red":"service-e2e")<<"\",\"automatic_gate\":\""<<(automatic?"requested":"separately gated")<<"\",\"source_hash_algorithm\":\"FNV-1a-64\",\"conformance_hash\":\""<<stableHash(readTextFile("tests/current_only_conformance.cpp"))<<"\",\"automatic_modes\":[";
      for(size_t i=0;i<automatic_modes.size();++i) { if(i) report<<','; report<<"{\"mode\":\""<<automatic_modes[i].first<<"\",\"status\":\""<<(automatic_modes[i].second?"PASS":"FAIL")<<"\",\"report\":\""<<automatic_modes[i].first<<"/mode_report.json\"}"; }
      report<<"],\"scope\":\""<<(selected.empty()?"full":"focused")<<"\",\"scenario_count\":"<<cases.size()<<",\"coexistence_requested\":"<<(!automatic && selected.empty()?"true":"false")<<",\"required_scenarios\":[";
      for(size_t i=0;i<cases.size();++i) {if(i)report<<',';report<<'"'<<jsonEscape(cases[i].name)<<'"';}
      report<<"],\"selected_scenarios\":[";for(size_t i=0;i<cases.size();++i) {if(i)report<<',';report<<'"'<<jsonEscape(cases[i].name)<<'"';}
      report<<"],\"failures\":["; for(size_t i=0;i<failures.size();++i) { if(i) report<<','; report<<'"'<<jsonEscape(failures[i])<<'"'; } report<<"]}\n"; }
    return failures.empty();
}

// BEGIN FOCUSED AUTOMATIC START WORKFLOW
bool runAutomaticStartWorkflow() {
    const auto root = std::filesystem::path(TRIT_TEST_BUILD_DIR) /
        "service_workflow_artifact" / "review" / "automatic_start" / "focused";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if(!require(!ec,"automatic focused artifact reset " + ec.message())) return false;
    std::filesystem::create_directories(root, ec);
    if(!require(!ec,"automatic focused artifact directory " + ec.message())) return false;
    std::vector<std::string> failures;
    std::ostringstream modes;
    const std::vector<std::string> names={"normal","spawn_failure","startup_failure"};
    for(int mode=1;mode<=3;++mode) {
        const auto dir=root/names[mode-1];
        const bool image_ok=serviceDesktopImage(dir/"image",mode,true);
        const bool desktop_ok=image_ok && runDesktopWorkflow(dir/"image",dir/"desktop",mode,true);
        if(!image_ok) failures.push_back(names[mode-1]+": fixture image failed");
        if(image_ok && !desktop_ok) failures.push_back(names[mode-1]+": automatic observation or exact desktop failed (desktop/automatic_boot and host evidence)");
        std::ostringstream report;
        report<<"{\"mode\":\""<<names[mode-1]<<"\",\"status\":\""<<(image_ok && desktop_ok?"PASS":"FAIL")
              <<"\",\"image_generated\":"<<(image_ok?"true":"false")<<",\"exact_desktop_passed\":"<<(desktop_ok?"true":"false")
              <<",\"required_launch_count\":1,\"required_launch_before_exec\":true,\"failures\":[";
        if(!image_ok || !desktop_ok) report<<"\""<<jsonEscape(failures.back())<<"\"";
        report<<"],\"evidence\":\"desktop/automatic_boot; desktop/host_before; desktop/host_after; desktop/health; desktop/input_journal.jsonl\"}\n";
        if(!automaticWrite(dir/"mode_report.json",report.str())) failures.push_back(names[mode-1]+": mode report closure failed");
        if(mode>1) modes<<',';
        modes<<"{\"mode\":\""<<names[mode-1]<<"\",\"status\":\""<<(image_ok && desktop_ok?"PASS":"FAIL")<<"\",\"report\":\""<<names[mode-1]<<"/mode_report.json\"}";
    }
    std::ostringstream report;
    report<<"{\"status\":\""<<(failures.empty()?"PASS":"FAIL")<<"\",\"selector\":\"--service-auto-modes-only\",\"scope\":\"focused-automatic-three\",\"scenario_count\":3,\"service_case_count\":0,\"full33_pass_claimed\":false,\"default_pass_claimed\":false,\"automatic_modes\":["<<modes.str()<<"],\"source_hash_algorithm\":\"FNV-1a-64\",\"conformance_hash\":\""<<stableHash(readTextFile("tests/current_only_conformance.cpp"))<<"\",\"failures\":[";
    for(size_t i=0;i<failures.size();++i) {if(i)report<<',';report<<'"'<<jsonEscape(failures[i])<<'"';}
    report<<"]}\n";
    const bool report_ok=automaticWrite(root/"automatic_report.json",report.str());
    return failures.empty() && report_ok;
}
// END FOCUSED AUTOMATIC START WORKFLOW

// BEGIN ADDITIVE SERVICE COEXISTENCE SELECTOR DRIVER
bool runServiceCoexistenceWorkflow() {
    const auto root = std::filesystem::path(TRIT_TEST_BUILD_DIR) /
        "service_workflow_artifact" / "service" / "coexistence";
    std::filesystem::create_directories(root);
    const bool image_ok = serviceDesktopImage(root / "image", 1, false);
    const bool desktop_ok = image_ok &&
        runDesktopWorkflow(root / "image", root / "desktop", 1, false);
    std::ofstream report(root / "coexistence_report.json");
    report << "{\"status\":\"" << (image_ok && desktop_ok ? "PASS" : "FAIL")
           << "\",\"selector\":\"--service-coexistence-only\",\"scope\":\"focused-coexistence\","
           << "\"automatic\":false,\"scenario_count\":1,\"image_generated\":"
           << (image_ok ? "true" : "false") << ",\"desktop_passed\":"
           << (desktop_ok ? "true" : "false")
           << ",\"source_hash_algorithm\":\"FNV-1a-64\",\"conformance_hash\":\""
           << stableHash(readTextFile("tests/current_only_conformance.cpp")) << "\"}\n";
    report.close();
    const bool report_ok = require(report.good(), "service coexistence report closure");
    return image_ok && desktop_ok && report_ok;
}
// END ADDITIVE SERVICE COEXISTENCE SELECTOR DRIVER

bool runDesktopWorkflow(const std::filesystem::path& image_root,
                        const std::filesystem::path& artifact_root,
                        int service_mode, bool automatic_service) {
    using sandbox::host::TosFramebufferMode;
    using sandbox::host::TosRuntime;
    using sandbox::host::TosRuntimeConfig;

    const std::filesystem::path root = image_root.empty() ?
        std::filesystem::path(TRIT_TEST_BUILD_DIR) / "release" /
        "TernaryOS" : image_root;
    const std::filesystem::path boot_path = root / "ternary-os.tboot";
    const std::filesystem::path disk_path = root / "ternary-os.tdisk";
    const std::filesystem::path artifact = artifact_root.empty() ?
        std::filesystem::path(TRIT_TEST_BUILD_DIR) /
        "desktop_workflow_artifact" : artifact_root;
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

    // Automatic boot is observed at actual user exec and desktop readiness.
    // Boundary trace results are deliberately not treated as syscall returns.
    struct AutomaticTrace {
        struct Segment {
            std::string name, path;
            long long rows=0, first_sequence=-1, last_sequence=-1, first_cycle=-1, last_cycle=-1;
        };
        bool valid=true;
        std::string failure;
        std::vector<Segment> segments;
        int launches=0;
        long long launch=-1, desktop=-1, console=-1, console_value=0, exit=-1, exit_value=0;
        // These ordinals explicitly concatenate validated disjoint segments;
        // they are not native cumulative sequence IDs.
        long long launch_order=-1, desktop_order=-1, console_order=-1, exit_order=-1;
        std::string launch_segment, desktop_segment, console_segment, exit_segment;
    };
    auto automaticTrace=[&](const std::filesystem::path& checkpoint, bool after_restore=false) {
        AutomaticTrace evidence;
        auto fail=[&](const std::string& label) {evidence.valid=false;if(evidence.failure.empty())evidence.failure=label;};
        const std::regex row(R"TRACE(\{"schema":"trit\.syscall_trace\.v1","sequence":([0-9]+),"pc":-?[0-9]+,"physical_pc":-?[0-9]+,"syscall_id":([0-9]+),"process_id":-?[0-9]+,"before_privilege":-?[0-9]+,"after_privilege":-?[0-9]+,"args":\[(-?[0-9]+),(-?[0-9]+),(-?[0-9]+),(-?[0-9]+)\],"results":\[-?[0-9]+,-?[0-9]+,-?[0-9]+\],"before_status":-?[0-9]+,"after_status":-?[0-9]+,"trap":-?[0-9]+,"trap_code":-?[0-9]+,"trap_cause":-?[0-9]+,"cycle_before":([0-9]+),"cycle_after":([0-9]+)\})TRACE");
        const std::regex empty(R"TRACE(\{"schema":"trit\.syscall_trace\.v1","event":"trace_empty","cycles":[0-9]+\})TRACE");
        auto parse=[&](const std::filesystem::path& path,const std::string& name,bool allow_empty) {
            AutomaticTrace::Segment segment;segment.name=name;segment.path=path.string();
            std::ifstream input(path);
            if(!input.is_open()) {fail("missing actual trace segment "+name);evidence.segments.push_back(segment);return;}
            long long offset=0;for(const auto& prior:evidence.segments)offset+=prior.rows;
            std::string line;bool empty_seen=false;
            while(std::getline(input,line)) {
                std::smatch m;
                if(allow_empty && segment.rows==0 && !empty_seen && std::regex_match(line,empty)) {empty_seen=true;continue;}
                if(empty_seen || !std::regex_match(line,m,row)) {fail("parse-invalid actual trace segment "+name);break;}
                try {
                    const auto seq=std::stoll(m[1]),id=std::stoll(m[2]);
                    const auto arg0=std::stoll(m[3]),arg1=std::stoll(m[4]),arg2=std::stoll(m[5]);
                    const auto cycle_before=std::stoll(m[7]),cycle_after=std::stoll(m[8]);
                    if(seq!=segment.rows || cycle_after<cycle_before || (segment.rows>0 && cycle_before<segment.last_cycle)) {fail("nonmonotonic actual trace segment "+name);break;}
                    if(segment.rows==0) {segment.first_sequence=seq;segment.first_cycle=cycle_before;}
                    segment.last_sequence=seq;segment.last_cycle=cycle_after;++segment.rows;
                    const auto order=offset+seq;
                    if(id==57 && arg1==7 && arg2==107) {++evidence.launches;evidence.launch=seq;evidence.launch_order=order;evidence.launch_segment=name;}
                    if(id==21 && evidence.desktop_order<0) {evidence.desktop=seq;evidence.desktop_order=order;evidence.desktop_segment=name;}
                    if(id==1 && arg0<0 && evidence.launch_order>=0 && order>evidence.launch_order) {evidence.console=seq;evidence.console_order=order;evidence.console_value=arg0;evidence.console_segment=name;}
                    if(id==44 && arg0<0 && evidence.launch_order>=0 && order>evidence.launch_order) {evidence.exit=seq;evidence.exit_order=order;evidence.exit_value=arg0;evidence.exit_segment=name;}
                } catch(const std::exception&) {fail("out-of-range actual trace field "+name);break;}
            }
            if(input.bad())fail("actual trace read failure "+name);
            if(segment.rows==0 && !(allow_empty && empty_seen))fail("empty actual trace segment "+name);
            evidence.segments.push_back(segment);
        };
        if(after_restore)parse(artifact/"input_burst_checkpoint"/"syscall_trace.jsonl","pre_restore",false);
        parse(checkpoint/"syscall_trace.jsonl",after_restore?"post_restore":"current",!after_restore);
        if(after_restore && evidence.segments.size()==2 && evidence.segments[0].rows>0 && evidence.segments[1].rows>0 && evidence.segments[0].last_cycle>evidence.segments[1].first_cycle)fail("overlapping pre/post restore trace segments");
        return evidence;
    };
    auto automaticEvidence=[&](const std::filesystem::path& checkpoint, const sandbox::vm::VMState& machine,
                                const AutomaticTrace& trace, const std::string& status) {
        std::ostringstream report;
        report<<"{\"status\":\""<<status<<"\",\"mode\":"<<service_mode<<",\"cycles\":"<<machine.cycle_count
              <<",\"launch_count\":"<<trace.launches<<",\"launch_sequence\":"<<trace.launch<<",\"desktop_exec_sequence\":"<<trace.desktop
              <<",\"negative_console_sequence\":"<<trace.console<<",\"negative_console_value\":"<<trace.console_value
              <<",\"negative_exit_sequence\":"<<trace.exit<<",\"negative_exit_value\":"<<trace.exit_value
              <<",\"pid\":"<<serviceKernelWord(machine,390000+7*9)<<",\"state\":"<<serviceKernelWord(machine,390000+7*9+2)
              <<",\"namespace\":"<<serviceKernelWord(machine,390000+7*9+1)<<",\"failures\":[";
        if(status!="PASS" && status!="WAIT") report<<'"'<<jsonEscape(status)<<'"';
        report<<"],\"trace_valid\":"<<(trace.valid?"true":"false")<<",\"trace_failure\":\""<<jsonEscape(trace.failure)
              <<"\",\"ordering_basis\":\"explicit logical ordinal = prior disjoint segment row count + native sequence; not native cumulative sequence\""
              <<",\"logical_ordinals\":{\"launch\":"<<trace.launch_order<<",\"desktop_exec\":"<<trace.desktop_order<<",\"negative_console\":"<<trace.console_order<<",\"negative_exit\":"<<trace.exit_order<<"}"
              <<",\"native_event_segments\":{\"launch\":\""<<trace.launch_segment<<"\",\"desktop_exec\":\""<<trace.desktop_segment<<"\",\"negative_console\":\""<<trace.console_segment<<"\",\"negative_exit\":\""<<trace.exit_segment<<"\"},\"trace_segments\":[";
        for(size_t i=0;i<trace.segments.size();++i) {
            if(i)report<<',';const auto& segment=trace.segments[i];
            report<<"{\"name\":\""<<segment.name<<"\",\"path\":\""<<jsonEscape(segment.path)<<"\",\"rows\":"<<segment.rows
                  <<",\"first_native_sequence\":"<<segment.first_sequence<<",\"last_native_sequence\":"<<segment.last_sequence
                  <<",\"first_cycle\":"<<segment.first_cycle<<",\"last_cycle\":"<<segment.last_cycle<<"}";
        }
        report<<"]}\n";
        return automaticWrite(checkpoint/"automatic_observation.json",report.str());
    };
    auto awaitAutomaticBoot=[&]() {
        const auto wait_root=artifact/"automatic_boot";
        std::error_code ec; std::filesystem::create_directories(wait_root,ec);
        if(!require(!ec,"automatic boot artifact directory "+ec.message())) return false;
        std::ofstream attempts(wait_root/"attempts.jsonl");
        if(!require(attempts.is_open(),"automatic boot attempts open")) return false;
        auto close=[&](bool passed) {attempts.close();return require(!attempts.fail(),"automatic boot attempts closure") && passed;};
        long long steps=0, batch_steps=0;
        const auto entry=runtime.snapshot().cycles;
        for(int attempt=0;;++attempt) {
            const auto checkpoint=wait_root/"checkpoint";
            if(!require(runtime.captureCheckpoint(&error) && runtime.exportCheckpointBundle(checkpoint.string(),&error),"automatic boot checkpoint "+error)) return close(false);
            std::unique_ptr<sandbox::vm::VMState> machine;
            if(!require(sandbox::host::detail::readCheckpointState(checkpoint/"vm_state.bin",machine,&error) && machine,"automatic boot state "+error)) return close(false);
            const auto trace=automaticTrace(checkpoint);
            const auto pid=serviceKernelWord(*machine,390000+7*9), state=serviceKernelWord(*machine,390000+7*9+2);
            const bool live=state>=1 && state<=4;
            const auto frame=runtime.readFramebuffer();
            auto textAt=[&](int x,int y,const std::string& text) {
                if(frame.mode!=TosFramebufferMode::Text80x25 || frame.glyphs.size()<2000) return false;
                for(size_t i=0;i<text.size();++i) if(frame.glyphs[y*80+x+i]!=text[i]) return false;
                return true;
            };
            const bool login_ready=textAt(22,10,"USER:") && textAt(28,10,"ADMIN") && textAt(22,12,"PASS:") && textAt(23,14,"LOGIN");
            const bool mode_ready=service_mode==1 ? pid==107 && live : service_mode==2 ? (pid!=107 || !live) && trace.console_order>trace.launch_order && trace.console_order<trace.desktop_order : pid==107 && state==7 && trace.console_order>trace.launch_order && trace.exit_order>trace.console_order;
            attempts<<"{\"attempt\":"<<attempt<<",\"cycles\":"<<machine->cycle_count<<",\"returned_steps\":"<<steps<<",\"batch_steps\":"<<batch_steps<<",\"launch_count\":"<<trace.launches<<",\"desktop_exec_sequence\":"<<trace.desktop<<",\"login_ready\":"<<(login_ready?"true":"false")<<",\"mode_ready\":"<<(mode_ready?"true":"false")<<"}\n";
            attempts.flush();
            if(!require(attempts.good(),"automatic boot attempt flush")) return close(false);
            if(!trace.valid) {
                require(automaticEvidence(checkpoint,*machine,trace,trace.failure),"automatic invalid trace evidence closure");
                require(false,"automatic actual trace validation "+trace.failure);return close(false);
            }
            if(trace.desktop>=0 && !(trace.launches==1 && trace.launch_order>=0 && trace.launch_order<trace.desktop_order)) {
                const bool saved=automaticEvidence(checkpoint,*machine,trace,"actual desktop21 without exactly one prior57");
                const bool health=runtime.exportDiagnostics((wait_root/"terminal").string(),&error);
                require(saved && health,"automatic missing launch evidence closure "+error);
                require(false,"AC11 actual desktop21 without exactly one prior57 (bounded old-init Red)");
                return close(false);
            }
            if(trace.desktop>=0 && login_ready && mode_ready) {
                if(!automaticEvidence(checkpoint,*machine,trace,"PASS")) return close(false);
                return close(true);
            }
            if(!require(steps<1'000'000'000 && machine->cycle_count-entry<1'000'000'000,"automatic boot/mode completion budget exhausted")) {
                require(automaticEvidence(checkpoint,*machine,trace,"automatic boot/mode completion budget exhausted"),"automatic exhausted evidence closure");
                return close(false);
            }
            if(attempt>0 && !require(batch_steps>0,"automatic boot no returned progress")) return close(false);
            const auto batch=runtime.runForSteps(1'000'000);batch_steps=batch.steps;steps+=batch.steps;
            if(!require(batch.status!=sandbox::vm::VMStatus::TRAPPED && batch.status!=sandbox::vm::VMStatus::HALTED,"automatic boot trapped/halted")) return close(false);
        }
    };

    long long service_tick_before=-1, service_tick_after=-1;
    auto observeHost=[&](const std::string& phase) {
        if(service_mode==0) return true;
        // BEGIN ADDITIVE SERVICE OBSERVATION WAIT
        if(service_mode==1) {
            const auto wait_root=artifact/("host_wait_"+phase);
            std::error_code wait_error;
            std::filesystem::create_directories(wait_root,wait_error);
            if(!require(!wait_error,"AC9 host wait directory "+wait_error.message())) return false;
            std::ofstream attempts(wait_root/"attempts.jsonl");
            if(!require(attempts.good(),"AC9 host wait attempts open")) return false;
            const auto entry_cycles=runtime.snapshot().cycles;
            long long wait_steps=0, batch_steps=0;
            auto wait_status=runtime.snapshot().status;
            auto close_wait=[&](bool passed) {
                attempts.close();
                return require(attempts.good(),"AC9 host wait attempts closure") && passed;
            };
            for(int attempt=0;;++attempt) {
                const auto probe=wait_root/"probe";
                if(!require(runtime.captureCheckpoint(&error) && runtime.exportCheckpointBundle(probe.string(),&error),"AC9 host wait checkpoint "+error)) return close_wait(false);
                std::unique_ptr<sandbox::vm::VMState> waiting;
                if(!require(sandbox::host::detail::readCheckpointState(probe/"vm_state.bin",waiting,&error) && waiting,"AC9 host wait state read "+error)) return close_wait(false);
                std::ifstream trace(probe/"syscall_trace.jsonl");
                const std::string trace_text((std::istreambuf_iterator<char>(trace)),{});
                if(!require(trace.good() && !trace_text.empty(),"AC9 host wait trace read")) return close_wait(false);
                const auto wait_pid=serviceKernelWord(*waiting,390000+7*9);
                const auto wait_state=serviceKernelWord(*waiting,390000+7*9+2);
                const auto wait_namespace=serviceKernelWord(*waiting,390000+7*9+1);
                long long latest_raw=-1;
                for(const auto& path:{"/var/log/svc.0","/var/log/svc.1"}) {
                    const auto words=serviceInodeWords(*waiting,serviceFindInode(*waiting,path));
                    for(size_t i=0;i+20<=words.size();i+=20) if(words[i+5]==2) latest_raw=std::max(latest_raw,words[i+6]);
                    std::ofstream log(probe/(std::string(path).substr(9)+".json"));
                    log<<'['; for(size_t i=0;i<words.size();++i) {if(i) log<<',';log<<words[i];} log<<"]\n";
                    log.close();
                    if(!require(log.good(),"AC9 host wait real log closure")) return close_wait(false);
                }
                const auto elapsed_cycles=waiting->cycle_count-entry_cycles;
                const bool ready=phase=="before" ? latest_raw>=0 : latest_raw>service_tick_before;
                attempts<<"{\"attempt\":"<<attempt<<",\"phase\":\""<<phase<<"\",\"entry_cycles\":"<<entry_cycles<<",\"cycles\":"<<waiting->cycle_count<<",\"elapsed_cycles\":"<<elapsed_cycles<<",\"returned_steps\":"<<wait_steps<<",\"batch_steps\":"<<batch_steps<<",\"status\":"<<static_cast<int>(wait_status)<<",\"pid\":"<<wait_pid<<",\"state\":"<<wait_state<<",\"namespace\":"<<wait_namespace<<",\"latest_raw_tick\":"<<latest_raw<<",\"required_after_tick\":"<<service_tick_before<<",\"ready\":"<<(ready?"true":"false")<<",\"step_limit\":1000000000,\"cycle_limit\":1000000000}\n";
                attempts.flush();
                if(!require(attempts.good(),"AC9 host wait attempt flush")) return close_wait(false);
                if(!require(wait_pid==107 && (wait_state==1 || wait_state==2 || wait_state==3 || wait_state==4) && wait_namespace==0,"AC9 host wait healthy PID107 namespace0")) return close_wait(false);
                if(!require(wait_status!=sandbox::vm::VMStatus::TRAPPED && wait_status!=sandbox::vm::VMStatus::HALTED,"AC9 host wait trapped/halted")) return close_wait(false);
                if(attempt>0 && !require(batch_steps>0,"AC9 host wait no returned progress")) return close_wait(false);
                if(ready) {if(!close_wait(true)) return false;break;}
                if(!require(wait_steps<1'000'000'000 && elapsed_cycles<1'000'000'000,"AC9 host wait readiness budget exhausted")) return close_wait(false);
                const auto batch=runtime.runForSteps(1'000'000);
                batch_steps=batch.steps; wait_steps+=batch.steps; wait_status=batch.status;
            }
        }
        // END ADDITIVE SERVICE OBSERVATION WAIT
        const auto checkpoint=artifact/("host_"+phase);
        if(!require(runtime.captureCheckpoint(&error) && runtime.exportCheckpointBundle(checkpoint.string(),&error),"AC9 host checkpoint "+error)) return false;
        std::unique_ptr<sandbox::vm::VMState> observed;
        if(!require(sandbox::host::detail::readCheckpointState(checkpoint/"vm_state.bin",observed,&error) && observed,"AC9 actual host private mapping snapshot "+error)) return false;
        const auto pid=serviceKernelWord(*observed,390000+7*9), state=serviceKernelWord(*observed,390000+7*9+2);
        if(service_mode==1) {
            if(!require(pid==107 && (state==1 || state==2 || state==3 || state==4),"AC9 host PID107 healthy at slot7")) return false;
            if(!require(serviceKernelWord(*observed,390000+7*9+1)==0,"AC9 host namespace preserved")) return false;
            long long latest=-1;
            for(const auto& path:{"/var/log/svc.0","/var/log/svc.1"}) {
                const auto words=serviceInodeWords(*observed,serviceFindInode(*observed,path));
                for(size_t i=0;i+20<=words.size();i+=20) if(words[i+5]==2) latest=std::max(latest,words[i+6]);
                if(automatic_service) {
                    std::ostringstream log; log<<'[';
                    for(size_t i=0;i<words.size();++i) {if(i)log<<',';log<<words[i];}
                    log<<"]\n";
                    if(!automaticWrite(checkpoint/(std::string(path).substr(9)+".json"),log.str())) return false;
                } else serviceWriteWords(checkpoint/(std::string(path).substr(9)+".json"),words);
            }
            if(!require(latest>=0,"AC9 actual host metric log progress")) return false;
            if(phase=="before") service_tick_before=latest; else service_tick_after=latest;
            if(phase=="after" && !require(service_tick_after>service_tick_before,"AC9 host sampling advances alongside exact Calculator/Files")) return false;
        }
        if(service_mode==2 && !require(pid!=107 || state<1 || state>4,"AC11 failed spawn did not leave a running host")) return false;
        if(service_mode==3 && !require(pid==107 && state==7,"AC11 post-launch startup failure terminates host")) return false;
        if(automatic_service) {
            const auto trace=automaticTrace(checkpoint,phase=="after");
            const bool order=trace.valid && trace.launches==1 && trace.launch_order>=0 && trace.desktop_order>trace.launch_order;
            const bool console=service_mode==1 || (trace.console_order>trace.launch_order && (service_mode!=2 || trace.console_order<trace.desktop_order));
            const bool exited=service_mode!=3 || (trace.exit_order>trace.console_order && trace.exit_value<0 && pid==107 && state==7);
            if(!automaticEvidence(checkpoint,*observed,trace,order && console && exited?"PASS":"automatic launch/console/exit invariant failed")) return false;
            if(!require(order,"AC11 actual init launches exactly once before desktop exec")) return false;
            if(!require(console,"AC11 actual negative console after attempt (spawn before desktop exec)")) return false;
            if(!require(exited,"AC11 actual negative44 and terminal107")) return false;
        }
        return true;
    };

    sandbox::vm::RunResult last_desktop_advance;
    auto advance = [&](int steps, const std::string& phase) {
        const sandbox::vm::RunResult result = runtime.runForSteps(steps);
        last_desktop_advance = result;
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
    if (automatic_service) { if(!awaitAutomaticBoot()) return false; }
    else if (!advance(10'000'000, "desktop boot")) return false;
    if (!observeHost("before")) return false;
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
    const auto wait_artifact = artifact / "calculator_interaction_wait";
    std::filesystem::create_directories(wait_artifact);
    std::ofstream attempts(wait_artifact / "attempts.jsonl");
    if (!require(attempts.good(), "calculator interaction wait writer failed")) return false;
    if (!advance(30'000'000, "calculator interaction")) return false;
    frame = runtime.readFramebufferMemory(initial_frame_revision);
    const std::vector<std::string> expected_digits = {"000101111001000", "111001111100111"};
    auto presented_shape = [&](int digit) {
        std::string shape;
        if (frame.width < 38 || frame.height < 10 || frame.rgba.size() <
            static_cast<size_t>(frame.width * frame.height)) return shape;
        for (int y = 5; y < 10; ++y) {
            for (int x = 31 + digit * 4; x < 34 + digit * 4; ++x) {
                const auto color = frame.rgba[y * frame.width + x];
                shape += ((color >> 24) >= 200 && ((color >> 16) & 255) >= 180 &&
                          ((color >> 8) & 255) <= 100) ? '1' : '0';
            }
        }
        return shape;
    };
    auto presented_42 = [&]() {
        return presented_shape(0) == expected_digits[0] && presented_shape(1) == expected_digits[1];
    };
    auto record_attempt = [&](int batch, int requested_steps) {
        const auto current = runtime.snapshot();
        attempts << "{\"batch\":" << batch << ",\"requested_steps\":" << requested_steps
                 << ",\"returned_steps\":" << last_desktop_advance.steps
                 << ",\"status\":\"" << sandbox::vm::vmStatusToString(last_desktop_advance.status)
                 << "\",\"cycles\":" << current.cycles << ",\"frame_revision\":" << frame.revision
                 << ",\"shapes\":[\"" << presented_shape(0) << "\",\"" << presented_shape(1)
                 << "\"],\"presented_42\":" << (presented_42() ? "true" : "false") << "}\n";
        attempts.flush();
        return require(last_desktop_advance.steps > 0 && attempts.good(),
                       "calculator interaction wait made no progress or receipt write failed");
    };
    if (!record_attempt(0, 30'000'000)) return false;
    // Actual presentation is the completion condition; at most94M interaction steps.
    for (int batch = 1; batch <= 64 && !presented_42(); ++batch) {
        if (!advance(1'000'000, "calculator presentation wait")) return false;
        frame = runtime.readFramebufferMemory(initial_frame_revision);
        if (!record_attempt(batch, 1'000'000)) return false;
    }
    attempts.close();
    if (!require(!attempts.fail(), "calculator interaction wait receipt closure failed")) return false;
    if (presented_42()) {
        const auto completed = wait_artifact / "completed";
        if (!require(runtime.captureCheckpoint(&error) &&
                     runtime.exportCheckpointBundle((completed / "checkpoint").string(), &error) &&
                     runtime.exportDiagnostics(completed.string(), &error),
                     "calculator completed presentation checkpoint failed: " + error)) return false;
    }
    if (!require(frame.mode == TosFramebufferMode::GraphicsRGB && frame.changed,
                 "calculator interaction did not change the RGB frame")) {
        return false;
    }

    // Preserve exact directed glyph assertions on the same sampled front frame.
    for (int digit = 0; digit < 2; ++digit) {
        const std::string shape = presented_shape(digit);
        if (shape != expected_digits[digit]) {
            const auto failure = artifact / "calculator_result_failure";
            std::error_code directory_error;
            std::filesystem::create_directories(failure, directory_error);
            const auto current = runtime.snapshot();
            std::string capture_error, checkpoint_error, diagnostic_error;
            const bool captured = runtime.captureCheckpoint(&capture_error);
            const bool checkpoint_written = captured && runtime.exportCheckpointBundle(
                (failure / "checkpoint").string(), &checkpoint_error);
            const bool diagnostics_written = runtime.exportDiagnostics(failure.string(), &diagnostic_error);
            std::ofstream receipt(failure / "failure_capture.json");
            receipt << "{\"digit\":" << digit << ",\"observed_shape\":\"" << shape
                    << "\",\"expected_shape\":\"" << expected_digits[digit]
                    << "\",\"frame_revision\":" << frame.revision << ",\"width\":" << frame.width
                    << ",\"height\":" << frame.height << ",\"cycles\":" << current.cycles
                    << ",\"captured\":" << (captured ? "true" : "false")
                    << ",\"checkpoint_written\":" << (checkpoint_written ? "true" : "false")
                    << ",\"diagnostics_written\":" << (diagnostics_written ? "true" : "false")
                    << ",\"capture_error\":\"" << jsonEscape(capture_error)
                    << "\",\"checkpoint_error\":\"" << jsonEscape(checkpoint_error)
                    << "\",\"diagnostic_error\":\"" << jsonEscape(diagnostic_error)
                    << "\",\"directory_error\":\"" << jsonEscape(directory_error.message()) << "\"}\n";
            receipt.flush();
            require(!directory_error && receipt.good(), "calculator failure receipt write failed");
            require(captured && checkpoint_written && diagnostics_written,
                    "calculator live failure capture failed: " + capture_error + checkpoint_error + diagnostic_error);
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
    if (!observeHost("after")) return false;

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

// BEGIN CLOCK IDLE INPUT WORKFLOW (additive test-first scope)
bool runClockIdleInputWorkflow() {
    using namespace sandbox;
    const auto artifact=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact/clock/idle_input";
    std::filesystem::create_directories(artifact);
    std::vector<std::string> failures;
    auto check=[&](bool ok,const std::string& text) { if(!ok) { failures.push_back(text); std::cerr<<"idle input: "<<text<<'\n'; } return ok; };
    const auto start=std::chrono::steady_clock::now();
    long long steps=0;
    { std::ofstream out(artifact/"source_hashes.json");out<<"{\"algorithm\":\"FNV-1a-64\",\"sources\":{";bool comma=false;for(const auto& path:{"kernel.trit","native_kernel_trap_stub.tasm","architecture_contract.trit","apps/os_sdk.trit","tests/current_only_conformance.cpp"}) {if(comma)out<<',';comma=true;out<<'"'<<path<<"\":\""<<stableHash(readTextFile(path))<<'"';}out<<"}}\n"; }
    auto finish=[&]() { std::ofstream out(artifact/"report.json"); out<<"{\"pass\":"<<(failures.empty()?"true":"false")<<",\"steps\":"<<steps<<",\"wall_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<",\"failures\":["; for(size_t i=0;i<failures.size();++i) { if(i)out<<','; out<<'"'<<failures[i]<<'"'; } out<<"]}\n"; return failures.empty(); };
    const std::vector<std::string> guests={R"TRIT(
fn main() -> t40 {
    unsafe { store(10090,201); store(10220,0); }
    var window: t40=os_window_create(0,0,40,30);
    unsafe { store(10001,window); store(10000,1); }
    if (window < 0) { unsafe { store(10000,-1); } os_futex_wait(10220,0,-1); return 1; }
    var r: t40=os_window_wait_event(window,10200,250);
    unsafe { store(10010,r); store(10011,load(10200)); store(10012,load(10201)); store(10000,2); }
    r=os_window_wait_event(window,10200,250);
    unsafe { store(10013,r); store(10014,load(10200)); store(10015,load(10201)); store(10016,load(10202)); store(10000,3); }
    r=os_window_wait_event(window,10200,250);
    unsafe { store(10021,r); store(10022,load(10200)); store(10023,load(10201)); store(10024,load(10202)); store(10002,1); }
    r=os_window_wait_event(window,10200,250);
    unsafe { store(10017,r); store(10018,load(10200)); store(10019,load(10201)); store(10000,4); }
    r=os_window_destroy(window);
    unsafe { store(10020,r); store(10000,5); }
    os_futex_wait(10220,0,-1); return 0;
}
)TRIT",R"TRIT(
fn main() -> t40 {
    unsafe { store(10090,202); store(10220,0); store(10000,1); store(10030,csr_read(cycle)); }
    os_sleep_ms(100);
    unsafe { store(10031,csr_read(cycle)); store(10000,2); }
    var r: t40=os_wait_event(-1,10200,250);
    unsafe { store(10010,r); store(10011,load(10200)); store(10012,load(10201)); store(10000,3); }
    r=os_wait_event(-1,10200,250);
    unsafe { store(10013,r); store(10014,load(10200)); store(10015,load(10201)); store(10000,4); store(10032,csr_read(cycle)); }
    r=os_wait_event(-1,10200,30);
    unsafe { store(10016,r); store(10033,csr_read(cycle)); store(10000,5); }
    r=os_wait_event(-1,10200,250);
    unsafe { store(10017,r); store(10018,load(10200)); store(10019,load(10201)); store(10000,6); }
    os_futex_wait(10220,0,-1); return 0;
}
)TRIT",R"TRIT(
fn main() -> t40 {
    unsafe { store(10090,203); store(10220,0); store(10000,1); }
    os_futex_wait(10220,0,-1);
    unsafe { store(10000,99); } return 0;
}
)TRIT"};
    compiler::CompilerOptions opts; opts.enable_vector_abi=true; opts.enable_vector_spilling=true;
    os::NativeVfsImageBuilder fs(32768);
    check(fs.installBaseLayout().ok(),"rootfs");
    for(int i=0;i<3;++i) {
        std::ofstream(artifact/("guest"+std::to_string(i)+".trit"))<<guests[i];
        const auto app=compiler::compileSource("idle_guest"+std::to_string(i)+".trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+guests[i],opts);
        { std::ofstream out(artifact/("compile"+std::to_string(i)+".txt")); for(const auto& d:app.diagnostics)out<<d.format()<<'\n'; }
        if(!check(app.success,"guest compile "+std::to_string(i)))return finish();
        compiler::LinkOptions lo; lo.stack_hint_words=2048; lo.dead_strip_functions=true; lo.standalone_halt_on_exit=false; lo.enable_vector_abi=true; lo.enable_vector_spilling=true;
        const auto linked=compiler::linkModules({app.object},lo);
        { std::ofstream out(artifact/("link"+std::to_string(i)+".txt")); for(const auto& d:linked.diagnostics)out<<d.format()<<'\n'; }
        if(!check(linked.success,"guest link"))return finish();
        std::ofstream(artifact/("guest"+std::to_string(i)+".tasm"))<<linked.assembly;
        check(fs.addExecutableImage("/bin/idle"+std::to_string(i),linked.assembled.program,linked.executable_header_v3,256+i*64).ok(),"private executable");
    }
    const std::string setup="fn idle_input_setup() -> t40 { process_set_state(0,PROC_BLOCKED); return 1; }\n";
    std::ofstream(artifact/"kernel_setup.trit")<<setup;
    const auto kernel=compiler::compileSource("idle_kernel.trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("kernel.trit")+"\n"+setup);
    { std::ofstream out(artifact/"kernel_compile.txt");for(const auto& d:kernel.diagnostics)out<<d.format()<<'\n'; }
    if(!check(kernel.success,"kernel compile"))return finish();
    std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n mov r2, 14003\n store r13, r2, 0\n call idle_input_setup\n mov r1, native_trap_entry\n csrw tvec, r1\n";
    for(int i=0;i<3;++i) {
        const auto path="/bin/idle"+std::to_string(i);
        for(size_t j=0;j<=path.size();++j)boot+=" mov r1, "+std::to_string(j==path.size()?0:static_cast<unsigned char>(path[j]))+"\n mov r2, "+std::to_string(10020+j)+"\n store r1, r2, 0\n";
        boot+=" mov r13, 1\n mov r14, 10020\n mov r15, "+std::to_string(i+1)+"\n mov r16, "+std::to_string(i+201)+"\n call app_spawn\n mov r2, "+std::to_string(14000+i)+"\n store r13, r2, 0\n";
    }
    boot+=" mov r13, 201\n call scheduler_switch_to_pid\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
    boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
    auto fw=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
    auto source=[&](int words) {return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(words)+", 0, 27, "+std::to_string(fw)+", 2, 0\n";};
    auto assembled=vm::assembler::assembleV2(source(1)); fw=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
    assembled=vm::assembler::assembleV2(source(1)); if(assembled.success)assembled=vm::assembler::assembleV2(source(static_cast<int>(assembled.program.size())));
    std::ofstream(artifact/"boot.tasm")<<source(static_cast<int>(assembled.program.size()));
    { std::ofstream out(artifact/"assembly.txt");for(const auto& e:assembled.errors)out<<e.format()<<'\n'; }
    if(!check(assembled.success,"boot assembly"))return finish();
    host::TosImageManifest manifest; auto image=host::bootImageFromAssembly(assembled,manifest,fs.image());
    std::string error;
    if(!check(host::writeBootImageFile((artifact/"boot.tboot").string(),image,&error),"boot image"))return finish();
    auto kw=[](const vm::VMState& m,int a) {return host::detail::dmemWord(m,a);};
    auto state=[&](const vm::VMState& m,int slot) {return kw(m,390000+slot*9+2);};
    auto uw=[&](const vm::VMState& m,int slot,int a) {return serviceUserWord(m,a,slot);};
    auto queue=[&](const vm::VMState& m) {for(int i=0;i<128;++i)if(kw(m,138000+i*3+2)>0 && kw(m,138000+i*3)==103000)return i;return -1;};
    // Independent INCOMPLETE no-input prefix: diagnostics only, same compiled image.
    auto prefix=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    if(!check(host::loadBootImageIntoVm(*prefix,image,"",&error),"prefix load"))return finish();
    std::map<std::string,long long> calls; long long prefix_steps=0,idle_advances=0,idle_cycles=0,executed=0,charged=0,irq_entries=0,idle_acks=0;
    std::ofstream observations(artifact/"prefix_observations.jsonl");
    { std::ofstream out(artifact/"observer_labels.json");out<<"{";bool comma=false;for(const auto& name:{"kernel_schedule_next","kernel_schedule_next_return","kernel_timer_tick","wait_queue_find","wait_wake_input"}) {const auto it=assembled.labels.find(name);if(it!=assembled.labels.end()){if(comma)out<<',';comma=true;out<<'"'<<name<<"\":"<<it->second;}}out<<"}\n"; }
    vm::VMHooks hooks; hooks.onInstruction=[&](const vm::VMState& m,const vm::VMExecutionRecord& r) {
        if(!r.attempted || !r.has_instruction)return; ++executed; if(r.cycle_after>r.cycle_before)++charged;
        if(r.before_privilege!=vm::PrivilegeMode::Kernel)return;
        if(r.instruction.opcode==isa::Opcode::WAIT)observations<<"{\"event\":\"actual_idle_wait_instruction\",\"cycle\":"<<m.cycle_count<<",\"input_queue\":"<<queue(m)<<",\"states\":["<<state(m,1)<<','<<state(m,2)<<','<<state(m,3)<<"]}\n";
        for(const auto& name:{"kernel_schedule_next","kernel_schedule_next_return","kernel_timer_tick","wait_queue_find","wait_wake_input"}) {
            const auto it=assembled.labels.find(name); if(it!=assembled.labels.end() && it->second==r.pc) {
                ++calls[name]; if(std::string(name)=="kernel_schedule_next" || std::string(name)=="kernel_schedule_next_return")observations<<"{\"event\":\""<<name<<"\",\"cycle\":"<<m.cycle_count<<",\"input_queue\":"<<queue(m)<<",\"states\":["<<state(m,1)<<','<<state(m,2)<<','<<state(m,3)<<"]}\n";
                if(std::string(name)=="wait_queue_find" && vm::ops::toLong(m.regfile.read(13))==103000) {++calls["wait_queue_find_input"];observations<<"{\"event\":\"actual_input_queue_lookup\",\"cycle\":"<<m.cycle_count<<",\"input_queue\":"<<queue(m)<<",\"states\":["<<state(m,1)<<','<<state(m,2)<<','<<state(m,3)<<"]}\n";}
            }
        }
    };
    while(prefix_steps<20000000 && prefix->cycle_count<40000000 && prefix->status!=vm::VMStatus::TRAPPED) {
        const auto before=prefix->cycle_count; const bool idle=prefix->status==vm::VMStatus::WAITING;
        const bool pending=prefix->timer_pending,active=prefix->trap_active; const auto cause=prefix->cause;
        vm::VMExecutionRecord record;vm::step(*prefix,&record);hooks.onInstruction(*prefix,record);
        if(idle && prefix->cycle_count==before && prefix->status==vm::VMStatus::WAITING)break;
        ++prefix_steps;if(idle) {++idle_advances;idle_cycles+=prefix->cycle_count-before;}
        const bool irq=prefix->trap_active && prefix->cause==vm::OS_CAUSE_TIMER_IRQ && (!active || cause!=vm::OS_CAUSE_TIMER_IRQ || (record.attempted && record.has_instruction && record.instruction.opcode==isa::Opcode::ERET));
        if(irq)++irq_entries;if(pending && !prefix->timer_pending && !irq && active)++idle_acks;
    }
    { std::ofstream out(artifact/"prefix_cost.json"); out<<"{\"status\":\"INCOMPLETE diagnostic no-input prefix; not behavioral PASS\",\"steps\":"<<prefix_steps<<",\"cycles\":"<<prefix->cycle_count<<",\"attempted_instructions\":"<<executed<<",\"cycle_charged_instructions\":"<<charged<<",\"idle_advances\":"<<idle_advances<<",\"idle_cycles\":"<<idle_cycles<<",\"input_queue\":"<<queue(*prefix)<<",\"calls\":{";bool comma=false;for(const auto& p:calls){if(comma)out<<',';comma=true;out<<'"'<<p.first<<"\":"<<p.second;}out<<"}}\n"; }
    { std::ofstream out(artifact/"prefix_setup.json");out<<"{\"spawn_returns\":["<<kw(*prefix,14000)<<','<<kw(*prefix,14001)<<','<<kw(*prefix,14002)<<"],\"states\":["<<state(*prefix,1)<<','<<state(*prefix,2)<<','<<state(*prefix,3)<<"],\"current_context\":"<<kw(*prefix,3019)<<",\"booted\":"<<kw(*prefix,3000)<<",\"init_return\":"<<kw(*prefix,14003)<<",\"irq_entries\":"<<irq_entries<<",\"idle_pending_acks\":"<<idle_acks<<",\"delivered_ticks\":"<<kw(*prefix,3033)<<"}\n"; }
    check(prefix->status!=vm::VMStatus::TRAPPED && prefix->timer_reload==1000000,"prefix actual boot timer and no trap");
    check(host::detail::writeCheckpointState(artifact/"prefix_state.bin",*prefix,&error),"prefix closure");
    host::TosRuntimeConfig cfg;cfg.boot_image_path=(artifact/"boot.tboot").string();cfg.execution_backend=vm::VMExecutionBackend::Interpreter;cfg.record_syscall_trace=true;
    host::TosRuntime runtime(cfg);if(!check(runtime.loadImage(image,&error),"continuous runtime load with real rootfs seed"))return finish();
    std::unique_ptr<vm::VMState> observed;
    std::ofstream phases(artifact/"phases.jsonl"), erets(artifact/"eret_observations.jsonl");
    auto capture=[&](const std::string& phase,bool immutable) {
        auto dir=artifact/(immutable?phase:"probe");
        if(!check(runtime.captureCheckpoint(&error) && runtime.exportCheckpointBundle(dir.string(),&error),"checkpoint "+phase))return false;
        if(!check(host::detail::readCheckpointState(dir/"vm_state.bin",observed,&error),"read "+phase))return false;
        if(immutable) {
            phases<<"{\"phase\":\""<<phase<<"\",\"steps\":"<<steps<<",\"cycle\":"<<observed->cycle_count<<",\"kernel_tick\":"<<kw(*observed,3033)<<",\"timer_pending\":"<<(observed->timer_pending?"true":"false")<<",\"timer_counter\":"<<observed->timer_counter<<",\"timer_enable\":"<<(observed->timer_enable?"true":"false")<<",\"input_queue\":"<<queue(*observed)<<",\"processes\":[";
            for(int slot=1;slot<=3;++slot) {if(slot>1)phases<<',';const auto ctx=kw(*observed,390000+slot*9+5);phases<<"{\"slot\":"<<slot<<",\"pid\":"<<kw(*observed,390000+slot*9)<<",\"namespace\":"<<kw(*observed,390000+slot*9+1)<<",\"context\":"<<ctx<<",\"private_pt\":"<<kw(*observed,static_cast<int>(ctx)+4)<<",\"state\":"<<state(*observed,slot)<<",\"kind\":"<<kw(*observed,138600+slot)<<",\"deadline\":"<<kw(*observed,138700+slot)<<",\"channel\":"<<kw(*observed,390000+slot*9+6)<<",\"next\":"<<kw(*observed,138500+slot)<<",\"epc\":"<<kw(*observed,static_cast<int>(ctx))<<",\"marker\":"<<uw(*observed,slot,10000)<<",\"sentinel\":"<<uw(*observed,slot,10090)<<"}";}phases<<"]}\n";
        } return true;
    };
    // Fine stepping around input transitions records actual ERET boundaries; coarse
    // chunks are used only while waiting for finite clock deadlines.
    auto advance=[&](bool fine) {
        if(steps>=200000000)return false;
        auto before=runtime.snapshot(); bool eret=false;
        if(fine && before.privilege==vm::PrivilegeMode::Kernel && before.pc>=0 && static_cast<size_t>(before.pc)<assembled.program.size())eret=isa::VersionedInstructionCodec::decode(assembled.program[before.pc],isa::IsaEncodingVersion::V2).opcode==isa::Opcode::ERET;
        auto r=runtime.runForSteps(fine?1:10000);steps+=r.steps;
        if(eret) {if(!capture("eret",false))return false;erets<<"{\"steps\":"<<steps<<",\"cycle\":"<<observed->cycle_count<<",\"pid\":"<<kw(*observed,3020)<<",\"epc\":"<<observed->epc<<",\"privilege\":"<<static_cast<int>(observed->privilege)<<",\"r1\":"<<vm::ops::toLong(observed->regfile.read(1))<<",\"r2\":"<<vm::ops::toLong(observed->regfile.read(2))<<"}\n";}
        return check(r.status!=vm::VMStatus::TRAPPED && r.status!=vm::VMStatus::HALTED,"runtime not trapped/halted") && r.steps>0;
    };
    auto await=[&](const std::string& name,auto predicate,bool fine=false) {
        const auto wall_start=std::chrono::steady_clock::now();
        for(int n=0;n<2000000 && steps<200000000 && std::chrono::steady_clock::now()-wall_start<std::chrono::minutes(5);++n) {
            if(runtime.snapshot().status==vm::VMStatus::WAITING) {if(!capture(name,false))return false;if(uw(*observed,1,10000)==-1){check(false,"negative window ID before first wait");break;}if(kw(*observed,14000)!=201 || kw(*observed,14001)!=202 || kw(*observed,14002)!=203){check(false,"actual spawn returns invalid");break;}if(predicate(*observed))return capture(name,true);}
            if(!advance(fine || runtime.snapshot().cycles>=50000000)){check(false,"stationary execution or terminal VM before "+name);break;}
        }capture("failed_"+name,true);check(runtime.exportDiagnostics((artifact/"failure_diagnostics").string(),&error),"failure diagnostics closure");return check(false,"phase not reached "+name);
    };
    if(!await("absent_idle",[&](const auto& m){return m.cycle_count>=50000000 && state(m,1)==3 && state(m,2)==4 && state(m,3)==3 && uw(m,1,10000)==1;}))return finish();
    check(queue(*observed)<0,"initial global input queue absent");
    check(uw(*observed,1,10001)>=0,"actual live window ID accepted");
    check(kw(*observed,138600+1)==4 && kw(*observed,138600+3)==2 && kw(*observed,138700+3)==-1,"window and indefinite futex actual blocked kinds");
    const auto ownerctx=kw(*observed,390000+9+5), owner_epc=kw(*observed,static_cast<int>(ownerctx));
    const auto sleeper_deadline=kw(*observed,138700+2), indefinite_epc=kw(*observed,static_cast<int>(kw(*observed,390000+27+5)));
    runtime.pushTextInput("A","idle_input.first_window_key");
    if(!await("first_window_wake",[&](const auto& m){return uw(m,1,10000)==2 && state(m,1)==3;},true))return finish();
    check(uw(*observed,1,10010)>0 && uw(*observed,1,10011)==1 && uw(*observed,1,10012)==65,"first key only delivered to real window owner");
    check(state(*observed,2)==4 && kw(*observed,138700+2)==sleeper_deadline && state(*observed,3)==3,"input does not wake sleeper/indefinite waiter");
    runtime.updateMouseState(10,12,0,"idle_input.window_mouse");
    if(!await("window_mouse",[&](const auto& m){return uw(m,1,10000)==3 && state(m,1)==3;},true))return finish();
    check(uw(*observed,1,10013)>0 && uw(*observed,1,10014)==2 && uw(*observed,1,10015)==10 && uw(*observed,1,10016)==12,"real mouse owner coordinates");
    runtime.updateMouseState(0,0,0,"idle_input.window_mouse_return");
    if(!await("window_mouse_return",[&](const auto& m){return uw(m,1,10002)==1 && state(m,1)==3;},true))return finish();
    check(uw(*observed,1,10021)>0 && uw(*observed,1,10022)==2 && uw(*observed,1,10023)==0 && uw(*observed,1,10024)==0,"normal owner consumes physical mouse return before global registration");
    if(!await("global_registered",[&](const auto& m){return uw(m,2,10000)==2 && state(m,2)==3 && kw(m,138600+2)==5;}))return finish();
    check(queue(*observed)>=0 && kw(*observed,138000+queue(*observed)*3+1)==2,"later global queue actual membership");
    check(uw(*observed,2,10031)-uw(*observed,2,10030)>=100000000,"finite sleeper never early");
    runtime.pushTextInput("B","idle_input.focused_competition");
    if(!await("window_destroyed",[&](const auto& m){return uw(m,1,10000)==5 && state(m,1)==3;},true))return finish();
    check(uw(*observed,1,10017)>0 && uw(*observed,1,10018)==1 && uw(*observed,1,10019)==66 && uw(*observed,1,10020)>0,"focused window gets second key and normal destroy");
    check(state(*observed,2)==3 && uw(*observed,2,10000)==2 && kw(*observed,138600+2)==5,"global waiter remains blocked while focused owner consumes B");
    runtime.pushTextInput("CD","idle_input.global_fifo");
    if(!await("global_fifo_and_timeout",[&](const auto& m){return uw(m,2,10000)==5 && state(m,2)==3 && kw(m,138600+2)==5;},true))return finish();
    check(uw(*observed,2,10010)>0 && uw(*observed,2,10011)==1 && uw(*observed,2,10012)==67 && uw(*observed,2,10013)>0 && uw(*observed,2,10014)==1 && uw(*observed,2,10015)==68,"global FIFO C then D exactly once");
    check(uw(*observed,2,10016)==0 && uw(*observed,2,10033)-uw(*observed,2,10032)>=30000000,"subsequent finite global timeout actual elapsed");
    runtime.pushTextInput("E","idle_input.global_reuse");
    if(!await("terminal",[&](const auto& m){return uw(m,2,10000)==6 && state(m,1)==3 && state(m,2)==3 && state(m,3)==3 && !m.timer_enable;},true))return finish();
    check(uw(*observed,2,10017)>0 && uw(*observed,2,10018)==1 && uw(*observed,2,10019)==69,"global reuse delivers E once");
    check(state(*observed,3)==3 && uw(*observed,3,10000)==1 && kw(*observed,138700+3)==-1 && kw(*observed,static_cast<int>(kw(*observed,390000+27+5)))==indefinite_epc,"unrelated indefinite owner never completed");
    std::set<long long> pts;
    for(int slot=1;slot<=3;++slot) {check(uw(*observed,slot,10090)==200+slot,"private sentinel integrity");pts.insert(kw(*observed,static_cast<int>(kw(*observed,390000+slot*9+5))+4));}
    check(pts.size()==3,"distinct normal private mappings");
    check(observed->timer_reload==1000000,"boot 1ms timer preserved");
    const auto parked_cycle=observed->cycle_count,parked_tick=kw(*observed,3033),parked_counter=observed->timer_counter;
    const auto parked=runtime.runForSteps(10000);steps+=parked.steps;
    if(!capture("parked",true))return finish();
    check(parked.steps==0 && observed->cycle_count==parked_cycle && kw(*observed,3033)==parked_tick && observed->timer_counter==parked_counter,"indefinite park spends no steps/cycles/ticks and retains countdown");
    erets.close();phases.close();observations.close();
    const auto eret_text=readTextFile(artifact/"eret_observations.jsonl");
    check(eret_text.find("\"pid\":201,\"epc\":"+std::to_string(owner_epc+1)+",")!=std::string::npos,"first window wait actual ERET advances blocked EPC once");
    check(runtime.exportDiagnostics((artifact/"diagnostics").string(),&error),"closed raw syscall/input diagnostics");
    std::cout<<"idle input workflow artifacts: "<<artifact.string()<<'\n';
    return finish();
}
// END CLOCK IDLE INPUT WORKFLOW

}  // namespace

// BEGIN FINAL CLI E2E (artifact-local copies, actual supported CLI)
bool runCliWorkflow() {
    const auto base=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact"/"final_gate"/"cli_e2e";
    std::filesystem::create_directories(base);
    const std::string script=R"CLI_PY(import copy, hashlib, json, pathlib, shutil, subprocess, sys, time
ROOT = pathlib.Path(sys.argv[1]).resolve()
BASE = pathlib.Path(sys.argv[2]).resolve()
INPUTS = json.loads('["APP_MANIFEST.json", "ARCHITECTURE_MANIFEST.json", "CMakeLists.txt", "IMAGE_FORMAT_MANIFEST.json", "SYSCALL_MANIFEST.json", "TEST_MANIFEST.json", "apps/about.trit", "apps/bin_core.trit", "apps/calculator.trit", "apps/clear.trit", "apps/crash.trit", "apps/date.trit", "apps/desktop.trit", "apps/doctor.trit", "apps/file_manager.trit", "apps/fsck.trit", "apps/help.trit", "apps/init.trit", "apps/kill.trit", "apps/libwidget.trit", "apps/log.trit", "apps/login.trit", "apps/ls.trit", "apps/mount.trit", "apps/os_sdk.trit", "apps/paint.trit", "apps/passwd.trit", "apps/ps.trit", "apps/reboot.trit", "apps/service_host.trit", "apps/service_runtime.trit", "apps/service_stub.trit", "apps/settings.trit", "apps/shell.trit", "apps/shutdown.trit", "apps/sleep.trit", "apps/span_draw.trit", "apps/sync.trit", "apps/sysinfo.trit", "apps/task_manager.trit", "apps/terminal.trit", "apps/test.trit", "apps/text_editor.trit", "architecture_contract.h", "architecture_contract.trit", "architecture_instruction_codec.h", "architecture_isa_v2.h", "architecture_v2_assembler_directives.h", "architecture_v2_assembler_types.h", "architecture_v2_support.h", "build_tos_image.cpp", "executable_header_common.h", "executable_header_v3.h", "kernel.trit", "native_kernel_trap_stub.tasm", "ternary_asm.h", "ternary_backend.h", "ternary_binary_io.h", "ternary_compiler.h", "ternary_compiler_ast.h", "ternary_compiler_cfg.h", "ternary_compiler_codegen.h", "ternary_compiler_ir.h", "ternary_compiler_lexer.h", "ternary_compiler_parser.h", "ternary_compiler_types.h", "ternary_dataflow.trit", "ternary_host_runtime.h", "ternary_ir.h", "ternary_isa.h", "ternary_lanes.h", "ternary_math.h", "ternary_native_ops.h", "ternary_os.h", "ternary_redo_wal.h", "ternary_scalar.h", "ternary_simd.h", "ternary_stream.trit", "ternary_symbolic_encoding.h", "ternary_uint128.h", "ternary_vm.h", "ternary_vm_state.h", "tests/current_only_conformance.cpp", "tools/trit_tool.py", "tritc.cpp"]')
CANDIDATE = json.loads('{"schema_version": 1, "platform": {"isa_version": 2, "executable_abi_version": 3, "function_abi_version": 3, "syscall_abi_version": 2, "vector_abi_version": 1, "tboot_version": 3, "tdisk_version": 2}, "conformance": {"source": "tests/current_only_conformance.cpp", "target": "trit_current_conformance", "ctest_name": "trit_current_conformance", "selectors": ["--cli-only", "--clock-idle-input-only", "--clock-kernel-only", "--clock-only", "--compositor-drawing-lifecycle-only", "--compositor-drawing-only", "--compositor-fork-fatal-user-only", "--compositor-fork-malformed-v3-only", "--compositor-fork-memory-only", "--compositor-fork-prerequisite-only", "--compositor-fork-window-lifetime-only", "--compositor-prerequisites-only", "--compositor-transport-only", "--desktop-only", "--extent-lifecycle-only", "--kernel-word-classification-only", "--metrics-only", "--numeric-only", "--sdk-only", "--service-auto-modes-only", "--service-auto-only", "--service-coexistence-only", "--service-edges-only", "--service-errors-only", "--service-fixtures-only", "--service-history-cap1-only", "--service-link-only", "--service-only", "--service-reservation-only", "--stream-only", "--wal-admission-only", "--wal-only", "--wal-shape-only"], "default_workflows": ["wal_full", "sdk", "numeric", "metrics", "clock_kernel", "clock_boundaries", "clock_idle_input", "clock_service", "desktop", "stream", "service_full", "cli"], "separate_required_workflows": ["service_auto", "compositor_prerequisites", "compositor_transport", "compositor_drawing"], "timeout_seconds": 0}, "build": {"directory": "build_current_cleanup", "configure": ["cmake", "-S", ".", "-B", "build_current_cleanup"], "targets": {"current_images": {"direct_dependencies": ["release/TernaryOS/ternary-os.tboot", "release/TernaryOS/ternary-os.tdisk"], "producer": "build_tos_image"}, "current_validate": {"direct_dependencies": ["trit_current_conformance"], "runs": "trit_current_conformance"}, "ci_production": {"direct_dependencies_if_BUILD_TESTING": ["tritc", "build_tos_image", "ctest"], "direct_dependencies_if_not_BUILD_TESTING": ["tritc", "build_tos_image", "current_validate"]}, "stage_tos_release": {"available_if": "TRIT_ENABLE_SDL_RELEASE AND SDL2_FOUND AND run_tos_sdl.cpp exists", "direct_dependencies": ["current_images", "run_tos_sdl"]}, "smoke_tos_release": {"available_if": "stage_tos_release available", "direct_dependencies": ["stage_tos_release"], "frames": 10}}}, "cli": {"source": "tools/trit_tool.py", "commands": ["doctor", "contract-check", "test", "export-diagnostics"]}}')
CONTRACT = CANDIDATE['platform']
SERVICE = json.loads(pathlib.Path(sys.argv[3]).read_text())
WAL = ['live_ownership','untracked_data','unknown','outside_used','boot_identity','boot_value','boot_txid','boot_version','corrupt','broken_chain','invalid_highwater','invalid_domain','low_corrupt','low_broken_chain','low_invalid_highwater','low_invalid_domain']
ALGORITHM = 'FNV-1a-64'
rows=[]
def fnv(data):
    value=14695981039346656037
    for byte in data: value=((value^byte)*1099511628211)&((1<<64)-1)
    return f'{value:016x}'
def write(path,obj):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(obj,indent=2)+'\n',encoding='utf-8')
def workspace(name):
    dest=BASE/name
    dest.mkdir(parents=True,exist_ok=True)
    for relative in INPUTS:
        out=dest/relative
        out.parent.mkdir(parents=True,exist_ok=True)
        if relative=='TEST_MANIFEST.json': write(out,CANDIDATE)
        else: shutil.copyfile(ROOT/relative,out)
    write(dest/'fixture_identity.json',{'validation_only':True,'purpose':'CLI E2E generated evidence; never production runtime PASS','source_inputs':INPUTS,'test_manifest_candidate':CANDIDATE})
    return dest

def invoke(dest,command,expect_error=False,needle=None):
    start=time.monotonic()
    proc=subprocess.run([sys.executable,str(dest/'tools/trit_tool.py'),command],cwd=dest,capture_output=True,text=True)
    output=proc.stdout+proc.stderr
    log=dest/(command+'.log')
    log.write_text(output,encoding='utf-8')
    passed=(proc.returncode!=0 if expect_error else proc.returncode==0)
    if needle: passed=passed and needle.lower() in output.lower()
    passed=passed and 'Traceback' not in output
    row={'scenario':dest.name,'command':[sys.executable,str(dest/'tools/trit_tool.py'),command],'exit_code':proc.returncode,'seconds':time.monotonic()-start,'expected_error':expect_error,'diagnostic_needle':needle,'log':str(log.relative_to(BASE)),'pass':passed}
    rows.append(row)
    return row

def seed(dest,group='service_full'):
    build=dest/'build_current_cleanup'
    paths={'service_full':'service_workflow_artifact/service/report.json','wal_full':'service_workflow_artifact/wal/report.json','service_auto_focused':'service_workflow_artifact/review/automatic_start/focused/automatic_report.json'}
    modes=['normal','spawn_failure','startup_failure']
    names=SERVICE if group=='service_full' else WAL if group=='wal_full' else modes
    scope='full' if group!='service_auto_focused' else 'focused-automatic-three'
    report={'status':'PASS','scope':scope,'failures':[]}
    if group=='service_full': report.update(scenario_count=33,required_scenarios=SERVICE,selected_scenarios=SERVICE,coexistence_requested=True,automatic_modes=[])
    elif group=='wal_full': report.update(guard_scenario_count=16,selected_guard_cases=WAL)
    else: report.update(selector='--service-auto-modes-only',scenario_count=3,service_case_count=0,full33_pass_claimed=False,default_pass_claimed=False,automatic_modes=[{'mode':m,'status':'PASS','report':m+'/mode_report.json'} for m in modes])
    relative=paths[group]
    write(build/relative,report)
    evidence={relative:{'algorithm':ALGORITHM,'hash':fnv((build/relative).read_bytes()),'size_bytes':(build/relative).stat().st_size}}
    if group=='service_auto_focused':
        for mode in modes:
            extra=str(pathlib.PurePosixPath(relative).parent/mode/'mode_report.json')
            write(build/extra,{'status':'PASS','mode':mode,'image_generated':True,'exact_desktop_passed':True,'required_launch_count':1,'required_launch_before_exec':True})
            evidence[extra]={'algorithm':ALGORITHM,'hash':fnv((build/extra).read_bytes()),'size_bytes':(build/extra).stat().st_size}
    if group=='service_full':
        for case in SERVICE:
            for leaf in ['reconciliation.json','state.jsonl','progress.jsonl','cleanup.jsonl','embedded_guest.trit','guest.linked.tasm','guest.tboot','disk.tdisk','vm_state.bin','svc.0.json','svc.1.json','svc.gen.json','svc.report.json']:
                extra='service_workflow_artifact/service/'+case+'/'+leaf
                path=build/extra; path.parent.mkdir(parents=True,exist_ok=True)
                write(path,{'validation_only':True,'accepted':0,'committed':0,'retired':0,'steps':1})
                evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(path.read_bytes()),'size_bytes':path.stat().st_size}
        for case in SERVICE:
            leaves=['accepted_records.jsonl','committed_records.jsonl','jobs.jsonl','retirement.jsonl','syscalls.jsonl']
            if case in ['zero_channel','pool1','pool2','pool3','pool4']: leaves.append('cleanup.jsonl')
            for leaf in leaves:
                extra='service_workflow_artifact/service/'+case+'/'+leaf
                path=build/extra; path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'')
                evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(b''),'size_bytes':0}
    if group=='wal_full':
        shapes=['shape_'+name for name in ['sequential_closed','interleaved','reverse_closure','duplicate_commit','late_data','aborted_zero']]
        shapes += [name+'_recovery' for name in shapes if not any(bad in name for bad in ['duplicate_commit','late_data'])]
        extra_cases=['pressure','no_device','targeted_fsync','blocked_prefix','resumed_prefix_recovery','replay_orphan','replay_torn_chain','recovered_orphan','recovered_torn_chain','admission_io_failure','admission_fitting_io_failure','extent_lifecycle','extent_lifecycle_recovery']
        extra_cases += ['recovery_'+str(i) for i in range(4)]+['publication_failure_'+str(i) for i in range(1,5)]+['failed_publication_recovery_'+str(i) for i in range(1,7)]
        for case in WAL+shapes+extra_cases:
            for leaf in ['result.json','boot_result.json','fixture.trit','boot.tasm','boot.tboot','live.tdisk']:
                extra='service_workflow_artifact/wal/'+case+'/'+leaf
                path=build/extra; write(path,{'validation_only':True,'first_failure':0,'steps':1})
                evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(path.read_bytes()),'size_bytes':path.stat().st_size}
        for leaf,obj in [('checkpoint_shapes_report.json',{'status':'PASS','scope':'additional-to-original43','selected':shapes,'failures':[]}),('extent_lifecycle_report.json',{'status':'PASS','scope':'additional-to-original41','selected':['extent_lifecycle','extent_lifecycle_recovery'],'required':['extent_lifecycle','extent_lifecycle_recovery'],'failures':[]})]:
            extra='service_workflow_artifact/wal/'+leaf
            path=build/extra;write(path,obj);evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(path.read_bytes()),'size_bytes':path.stat().st_size}
        for leaf in ['pressure/storage.jsonl','pressure/syscalls.jsonl','pressure/before_checkpoint_stage1.tdisk','pressure/before_checkpoint_stage2.tdisk','pressure/before_checkpoint_stage3.tdisk','pressure/before_checkpoint_stage4.tdisk','targeted_fsync/frames.json','blocked_prefix/snapshot_release.jsonl','extent_lifecycle/lifecycle.jsonl','extent_lifecycle/cursor_injections.jsonl','replay_orphan/durable_crash.tdisk','replay_torn_chain/durable_crash.tdisk']:
            extra='service_workflow_artifact/wal/'+leaf
            path=build/extra;write(path,{'validation_only':True});evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(path.read_bytes()),'size_bytes':path.stat().st_size}
    # GUI-shaped evidence is synthetic CLI validation data, never actual runtime proof.
    gui_roots=['service_workflow_artifact/service/coexistence/desktop'] if group=='service_full' else [str(pathlib.PurePosixPath(relative).parent/mode/'desktop') for mode in modes] if group=='service_auto_focused' else []
    for gui in gui_roots:
        for leaf in ['manifest.json','process_table.json','window_table.json','input_journal.jsonl','framebuffer_snapshot.png','checkpoint/checkpoint.json','checkpoint/input_journal.jsonl','checkpoint/vm_state.bin','host_before/checkpoint.json','host_before/vm_state.bin','host_before/syscall_trace.jsonl','host_after/checkpoint.json','host_after/vm_state.bin','host_after/syscall_trace.jsonl','health/process_table.json']:
            extra=gui+'/'+leaf
            path=build/extra; path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(b'{"validation_only":true,"synthetic_cli_fixture":true}\n')
            evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(path.read_bytes()),'size_bytes':path.stat().st_size}
    if group=='service_auto_focused':
        for gui in gui_roots:
            for leaf in ['automatic_boot/attempts.jsonl','host_before/automatic_observation.json','host_after/automatic_observation.json']:
                extra=gui+'/'+leaf
                path=build/extra;write(path,{'validation_only':True});evidence[extra]={'algorithm':ALGORITHM,'hash':fnv(path.read_bytes()),'size_bytes':path.stat().st_size}
    sources={name:{'algorithm':ALGORITHM,'hash':fnv((dest/name).read_bytes()),'present':True} for name in INPUTS}
    side={'schema_version':1,'group':group,'run_id':'cli-validation-fixture-'+dest.name,'selector':'default' if group!='service_auto_focused' else '--service-auto-modes-only','scope':scope,'state':'complete','outcome':'PASS','started_utc':'2026-10-11T00:00:00Z','finished_utc':'2026-10-11T00:00:01Z','contract':CONTRACT,'source_inputs':sources,'evidence_files':evidence,'required_scenarios':names}
    sp=build/'service_workflow_artifact/completion'/ (group+'.json')
    write(sp,side)
    return build/relative,sp,side

def export_case(name,status,mutation=None,group='service_full'):
    dest=workspace(name)
    report,sp,side=seed(dest,group)
    if mutation: mutation(dest,report,sp,side)
    row=invoke(dest,'export-diagnostics')
    try:
        data=json.loads((dest/'build_current_cleanup/current-platform-diagnostics.json').read_text())
        actual=data.get('canonical_evidence',{}).get(group,{})
        row['expected_status']=status; row['observed_evidence']=actual
        row['pass']=row['pass'] and actual.get('status')==status
        if status=='verified': row['pass']=row['pass'] and actual.get('current_source_match') is True
        if status=='stale': row['pass']=row['pass'] and actual.get('status')!='verified'
        row['pass']=row['pass'] and all(data.get(k)==v for k,v in CONTRACT.items())
        provenance=data.get('source_provenance',{})
        row['pass']=row['pass'] and provenance.get('algorithm')=='SHA256'
    except Exception as error: row['pass']=False; row['inspection_error']=str(error)

BASE.mkdir(parents=True,exist_ok=True)
# Deliberate absent TEST catches the pre-repair actual doctor/contract false positive.
dest=workspace('missing_TEST_initial'); (dest/'TEST_MANIFEST.json').unlink()
for command in ['doctor','contract-check']: invoke(dest,command,True,'TEST_MANIFEST')
dest=workspace('valid_copy')
for command in ['doctor','contract-check']: invoke(dest,command)
for manifest in ['ARCHITECTURE_MANIFEST.json','IMAGE_FORMAT_MANIFEST.json','APP_MANIFEST.json','SYSCALL_MANIFEST.json','TEST_MANIFEST.json']:
    for kind in ['missing','malformed','shape','schema']:
        dest=workspace(manifest.split('.')[0]+'_'+kind); path=dest/manifest
        if kind=='missing': path.unlink()
        elif kind=='malformed': path.write_text('{broken',encoding='utf-8')
        elif kind=='shape': write(path,[])
        else:
            obj=json.loads(path.read_text()); obj['schema_version' if manifest=='TEST_MANIFEST.json' else 'schema']='wrong'; write(path,obj)
        for command in ['doctor','contract-check']: invoke(dest,command,True,manifest)
for kind in ['source','duplicate','builder','syscall','selector','graph']:
    dest=workspace('mapping_'+kind)
    path=dest/('APP_MANIFEST.json' if kind in ['source','duplicate','builder'] else 'SYSCALL_MANIFEST.json' if kind=='syscall' else 'TEST_MANIFEST.json')
    obj=json.loads(path.read_text())
    if kind=='source': obj['apps'][0]['source']='missing-cli-fixture.trit'
    elif kind=='duplicate': obj['apps'][1]['id']=obj['apps'][0]['id']
    elif kind=='builder': obj['apps'][0]['guest_path']='/bin/wrong-cli-fixture'
    elif kind=='syscall': obj['runtime_mappings']['sys_service_metrics']=59
    elif kind=='selector': obj['conformance']['selectors'].append('--fictional-only')
    else: obj['conformance']['target']='fictional_conformance'
    write(path,obj); invoke(dest,'contract-check',True,path.name)
# Actual compiler returns must be bound to the matching intrinsic name.
dest=workspace('compiler_swapped_open_close')
path=dest/'ternary_compiler_codegen.h'
text=path.read_text()
open_mapping='if (name == "sys_open") return runtime::sys_open;'
close_mapping='if (name == "sys_close") return runtime::sys_close;'
assert text.count(open_mapping)==1 and text.count(close_mapping)==1
text=text.replace(open_mapping,'if (name == "sys_open") return runtime::sys_close;').replace(close_mapping,'if (name == "sys_close") return runtime::sys_open;')
path.write_text(text,encoding='utf-8')
invoke(dest,'contract-check',True,'compiler mapping')
# Each existing header/guest current-version constant is checked independently.
for snapshot,constants in [('architecture_contract.h',{'ISA_VERSION':2,'CURRENT_EXECUTABLE_VERSION':3,'CURRENT_FUNCTION_ABI_VERSION':3,'SYSCALL_ABI_VERSION':2,'TBOOT_WRITE_VERSION':3,'TDISK_WRITE_VERSION':2}),('architecture_contract.trit',{'ARCH_ISA_VERSION':2,'ARCH_EXECUTABLE_VERSION':3,'ARCH_FUNCTION_ABI_VERSION':3,'ARCH_SYSCALL_ABI_VERSION':2})]:
    for name,value in constants.items():
        dest=workspace('snapshot_'+snapshot.split('.')[-1]+'_'+name)
        path=dest/snapshot; text=path.read_text()
        old=('inline constexpr int '+name+' = '+str(value)+';') if snapshot.endswith('.h') else ('const '+name+': t40 = '+str(value)+';')
        assert text.count(old)==1
        path.write_text(text.replace(old,old.replace('= '+str(value)+';','= 99;')),encoding='utf-8')
        invoke(dest,'contract-check',True,snapshot)
export_case('completion_valid','verified')
export_case('completion_missing_fresh_hash_stale_PASS','missing',lambda d,r,p,s:(p.unlink(),write(r.parent/'source_hashes.json',s['source_inputs'])))
export_case('completion_pending','pending',lambda d,r,p,s:write(p,dict(s,state='pending',outcome=None,finished_utc=None,evidence_files={})))
export_case('completion_invalid','invalid',lambda d,r,p,s:p.write_text('{bad',encoding='utf-8'))
export_case('completion_failed','failed',lambda d,r,p,s:write(p,dict(s,outcome='FAIL')))
export_case('completion_report_digest','stale',lambda d,r,p,s:write(r,dict(json.loads(r.read_text()),extra='report changed')))
export_case('completion_wrong_scope','stale',lambda d,r,p,s:write(p,dict(s,scope='focused')))
export_case('completion_missing_compiler_input','stale',lambda d,r,p,s:(d/'ternary_compiler_codegen.h').unlink())
export_case('completion_changed_compiler_input','stale',lambda d,r,p,s:(d/'ternary_compiler_codegen.h').write_bytes((d/'ternary_compiler_codegen.h').read_bytes()+b'\n// CLI fixture changed required input\n'))
export_case('completion_missing_required_field','invalid',lambda d,r,p,s:write(p,{k:v for k,v in s.items() if k!='run_id'}))
export_case('completion_full_service_coverage','stale',lambda d,r,p,s:write(p,dict(s,required_scenarios=SERVICE[:-1])))
export_case('completion_missing_GUI','stale',lambda d,r,p,s:(d/'build_current_cleanup/service_workflow_artifact/service/coexistence/desktop/process_table.json').unlink())
export_case('completion_missing_case_reconciliation','stale',lambda d,r,p,s:(d/('build_current_cleanup/service_workflow_artifact/service/'+SERVICE[0]+'/reconciliation.json')).unlink())
export_case('completion_changed_zero_record_log','stale',lambda d,r,p,s:(d/('build_current_cleanup/service_workflow_artifact/service/'+SERVICE[0]+'/accepted_records.jsonl')).write_bytes(b'{"unexpected_append":true}\n'))
export_case('completion_missing_zero_record_log','stale',lambda d,r,p,s:(d/('build_current_cleanup/service_workflow_artifact/service/'+SERVICE[0]+'/committed_records.jsonl')).unlink())
export_case('completion_changed_empty_constructor_cleanup','stale',lambda d,r,p,s:(d/'build_current_cleanup/service_workflow_artifact/service/zero_channel/cleanup.jsonl').write_bytes(b'{"unexpected_cleanup":true}\n'))
export_case('completion_wrong_selector','stale',lambda d,r,p,s:write(p,dict(s,selector='--service-errors-only')))
export_case('completion_bool_schema','invalid',lambda d,r,p,s:write(p,dict(s,schema_version=True)))
export_case('completion_bad_time','invalid',lambda d,r,p,s:write(p,dict(s,finished_utc='not-a-time')))
export_case('completion_reversed_time','invalid',lambda d,r,p,s:write(p,dict(s,finished_utc='2026-10-10T00:00:00Z')))
export_case('completion_drive_path','invalid',lambda d,r,p,s:write(p,dict(s,evidence_files=dict(s['evidence_files'],**{'C:/cli_fixture_outside':{'algorithm':ALGORITHM,'hash':fnv(b'x'),'size_bytes':1}}))))
for kind in ['missing_sdk','missing_include']:
    dest=workspace('mapping_'+kind)
    if kind=='missing_sdk':
        path=dest/'SYSCALL_MANIFEST.json';obj=json.loads(path.read_text());obj['sdk_wrappers'].pop('os_service_metrics');write(path,obj)
    else:
        path=dest/'ternary_compiler.h';path.write_bytes(b'#include "cli_fixture_missing.h"\n'+path.read_bytes())
    invoke(dest,'contract-check',True,path.name)
export_case('completion_bad_source_entry','invalid',lambda d,r,p,s:write(p,dict(s,source_inputs=dict(s['source_inputs'],**{'ternary_compiler_codegen.h':{'algorithm':ALGORITHM,'present':True}}))))
export_case('completion_bad_contract_type','invalid',lambda d,r,p,s:write(p,dict(s,contract=dict(CONTRACT,vector_abi_version=True))))
for manifest in ['ARCHITECTURE_MANIFEST.json','TEST_MANIFEST.json']:
    dest=workspace(manifest.split('.')[0]+'_nested_type');path=dest/manifest;obj=json.loads(path.read_text())
    if manifest=='ARCHITECTURE_MANIFEST.json':obj['abi']['vector_abi_version']=True
    else:obj['platform']['vector_abi_version']=True
    write(path,obj)
    for command in ['doctor','contract-check']:invoke(dest,command,True,manifest)
export_case('completion_full_wal_valid','verified',group='wal_full')
export_case('completion_full_wal_coverage','stale',lambda d,r,p,s:write(p,dict(s,required_scenarios=WAL[-4:])),group='wal_full')
export_case('completion_auto3_valid','verified',group='service_auto_focused')
export_case('completion_auto3_missing_mode','stale',lambda d,r,p,s:(r.parent/'startup_failure/mode_report.json').unlink(),group='service_auto_focused')
write(BASE/'scenario_report.json',{'status':'PASS' if all(row['pass'] for row in rows) else 'FAIL','validation_only':True,'runtime_PASS_claimed':False,'recursive_test_invoked':False,'source_inputs':INPUTS,'candidate':CANDIDATE,'scenarios':rows})
print(f'CLI E2E: {sum(row["pass"] for row in rows)}/{len(rows)} checks passed; {BASE / "scenario_report.json"}')
sys.exit(0 if all(row['pass'] for row in rows) else 1)
)CLI_PY";
    auto quote=[](const std::string& value) {
#ifdef _WIN32
        // cmd.exe requires escaped double quotes; the embedded script uses none.
        return "\""+value+"\"";
#else
        std::string out="'"; for(char c:value) out += c=='\'' ? "'\"'\"'" : std::string(1,c); return out+"'";
#endif
    };
    std::string names="["; bool comma=false;
    for(const auto& scenario:serviceScenarios()) { if(comma)names+=",";comma=true;names+="\""+jsonEscape(scenario.name)+"\""; } names+="]";
    // Store script and scenario inputs as evidence, but invoke -c: no new entrypoint.
    std::ofstream(base/"fixture_program.txt",std::ios::binary)<<script;
    std::ofstream(base/"service_scenarios.json")<<names;
    const std::string command="python - "+quote(std::filesystem::current_path().string())+" "+quote(base.string())+" "+quote((base/"service_scenarios.json").string())+" < "+quote((base/"fixture_program.txt").string());
    const int result=std::system(command.c_str());
    return require(result==0,"CLI E2E failed; retained "+(base/"scenario_report.json").string());
}
// END FINAL CLI E2E

// BEGIN FINAL COUPLED COMPLETION DRIVER
#include <ctime>
#include <functional>
struct CompletionNeed { std::string path; bool allow_empty=false; };
const std::vector<std::string> completionSourcePaths = {"APP_MANIFEST.json","ARCHITECTURE_MANIFEST.json","CMakeLists.txt","IMAGE_FORMAT_MANIFEST.json","SYSCALL_MANIFEST.json","TEST_MANIFEST.json","apps/about.trit","apps/bin_core.trit","apps/calculator.trit","apps/clear.trit","apps/crash.trit","apps/date.trit","apps/desktop.trit","apps/doctor.trit","apps/file_manager.trit","apps/fsck.trit","apps/help.trit","apps/init.trit","apps/kill.trit","apps/libwidget.trit","apps/log.trit","apps/login.trit","apps/ls.trit","apps/mount.trit","apps/os_sdk.trit","apps/paint.trit","apps/passwd.trit","apps/ps.trit","apps/reboot.trit","apps/service_host.trit","apps/service_runtime.trit","apps/service_stub.trit","apps/settings.trit","apps/shell.trit","apps/shutdown.trit","apps/sleep.trit","apps/span_draw.trit","apps/sync.trit","apps/sysinfo.trit","apps/task_manager.trit","apps/terminal.trit","apps/test.trit","apps/text_editor.trit","architecture_contract.h","architecture_contract.trit","architecture_instruction_codec.h","architecture_isa_v2.h","architecture_v2_assembler_directives.h","architecture_v2_assembler_types.h","architecture_v2_support.h","build_tos_image.cpp","executable_header_common.h","executable_header_v3.h","kernel.trit","native_kernel_trap_stub.tasm","ternary_asm.h","ternary_backend.h","ternary_binary_io.h","ternary_compiler.h","ternary_compiler_ast.h","ternary_compiler_cfg.h","ternary_compiler_codegen.h","ternary_compiler_ir.h","ternary_compiler_lexer.h","ternary_compiler_parser.h","ternary_compiler_types.h","ternary_dataflow.trit","ternary_host_runtime.h","ternary_ir.h","ternary_isa.h","ternary_lanes.h","ternary_math.h","ternary_native_ops.h","ternary_os.h","ternary_redo_wal.h","ternary_scalar.h","ternary_simd.h","ternary_stream.trit","ternary_symbolic_encoding.h","ternary_uint128.h","ternary_vm.h","ternary_vm_state.h","tests/current_only_conformance.cpp","tools/trit_tool.py","tritc.cpp"}; // explicit85 drawing closure
const std::map<std::string,std::string> completionReports = {{"kernel_word_classification","compositor_workflow_artifact/stage2_transport/word_classification/report.json"},{"compositor_drawing_lifecycle","compositor_workflow_artifact/stage4/lifecycle/report.json"},{"compositor_drawing","compositor_workflow_artifact/stage4/report.json"},{"compositor_transport","compositor_workflow_artifact/stage2_transport/report.json"},{"compositor_prerequisites","compositor_workflow_artifact/stage1/report.json"},{"wal_full","service_workflow_artifact/wal/report.json"},{"wal_admission","service_workflow_artifact/wal/admission_low/report.json"},{"wal_shape","service_workflow_artifact/wal/checkpoint_shapes/checkpoint_shapes_report.json"},{"wal_extent","service_workflow_artifact/wal/extent_lifecycle/extent_lifecycle_report.json"},{"service_full","service_workflow_artifact/service/report.json"},{"service_errors","service_workflow_artifact/service/report.json"},{"service_fixtures","service_workflow_artifact/service/report.json"},{"service_history_cap1","service_workflow_artifact/service/report.json"},{"service_reservation","service_workflow_artifact/service/report.json"},{"service_edges","service_workflow_artifact/service/report.json"},{"service_link","service_workflow_artifact/service_link_preflight/link_preflight_report.json"},{"service_auto","service_workflow_artifact/service/automatic_start/automatic_report.json"},{"service_auto_focused","service_workflow_artifact/review/automatic_start/focused/automatic_report.json"},{"service_coexistence","service_workflow_artifact/service/coexistence/coexistence_report.json"},{"sdk","service_workflow_artifact/sdk/sdk_report.json"},{"metrics","service_workflow_artifact/metrics/phase2_report.json"},{"stream","service_workflow_artifact/stream/phase1_report.json"},{"numeric","numeric_workflow_artifact/report.json"},{"desktop",""},{"clock_kernel","service_workflow_artifact/clock/kernel/report.json"},{"clock_boundaries","service_workflow_artifact/clock/kernel_boundaries/report.json"},{"clock_idle_input","service_workflow_artifact/clock/idle_input/report.json"},{"clock_service","service_workflow_artifact/clock/service/report.json"},{"cli","service_workflow_artifact/final_gate/cli_e2e/scenario_report.json"}}; // injected actual groups
const std::vector<std::string> completionDrawingCases = {"span_left","span_right","span_top","span_bottom","span_empty","span_offscreen","span_full","copy_both_left_top","copy_both_right_bottom","copy_empty","copy_offscreen","overlap_right","overlap_left","overlap_down","overlap_up","overlap_diagonal","overlap_same","line_0_forward_reference","line_0_forward_hybrid","line_0_reverse_reference","line_0_reverse_hybrid","line_1_forward_reference","line_1_forward_hybrid","line_1_reverse_reference","line_1_reverse_hybrid","line_2_forward_reference","line_2_forward_hybrid","line_2_reverse_reference","line_2_reverse_hybrid","line_3_forward_reference","line_3_forward_hybrid","line_3_reverse_reference","line_3_reverse_hybrid","line_4_forward_reference","line_4_forward_hybrid","line_4_reverse_reference","line_4_reverse_hybrid","line_5_forward_reference","line_5_forward_hybrid","line_5_reverse_reference","line_5_reverse_hybrid","line_6_forward_reference","line_6_forward_hybrid","line_6_reverse_reference","line_6_reverse_hybrid","line_7_forward_reference","line_7_forward_hybrid","line_7_reverse_reference","line_7_reverse_hybrid","line_8_forward_reference","line_8_forward_hybrid","line_8_reverse_reference","line_8_reverse_hybrid","line_9_forward_reference","line_9_forward_hybrid","line_9_reverse_reference","line_9_reverse_hybrid","line_10_forward_reference","line_10_forward_hybrid","line_10_reverse_reference","line_10_reverse_hybrid","line_11_forward_reference","line_11_forward_hybrid","line_11_reverse_reference","line_11_reverse_hybrid","line_12_forward_reference","line_12_forward_hybrid","line_12_reverse_reference","line_12_reverse_hybrid","line_13_forward_reference","line_13_forward_hybrid","line_13_reverse_reference","line_13_reverse_hybrid","line_14_forward_reference","line_14_forward_hybrid","line_14_reverse_reference","line_14_reverse_hybrid","line_15_forward_reference","line_15_forward_hybrid","line_15_reverse_reference","line_15_reverse_hybrid","line_16_forward_reference","line_16_forward_hybrid","line_16_reverse_reference","line_16_reverse_hybrid","line_17_forward_reference","line_17_forward_hybrid","line_17_reverse_reference","line_17_reverse_hybrid","line_18_forward_reference","line_18_forward_hybrid","line_18_reverse_reference","line_18_reverse_hybrid","layer_span","layer_copy","layer_overlap","layer_line","span_reverse","fraction_x","fraction_width","negative_width","oversized_width","oversized_height","zero_pointer","fraction_pointer","negative_pointer","declared_end","rgb_fraction","rgb_negative","rgb_256","coord_4097","extent_8193","extent_fraction","extent_negative","partial_intervals","warm_horizontal_reference","warm_horizontal_hybrid","bench_horizontal_0_reference","bench_horizontal_0_hybrid","bench_horizontal_1_hybrid","bench_horizontal_1_reference","bench_horizontal_2_reference","bench_horizontal_2_hybrid","bench_horizontal_3_hybrid","bench_horizontal_3_reference","bench_horizontal_4_reference","bench_horizontal_4_hybrid","bench_horizontal_5_hybrid","bench_horizontal_5_reference","warm_shallow_reference","warm_shallow_hybrid","bench_shallow_0_reference","bench_shallow_0_hybrid","bench_shallow_1_hybrid","bench_shallow_1_reference","bench_shallow_2_reference","bench_shallow_2_hybrid","bench_shallow_3_hybrid","bench_shallow_3_reference","bench_shallow_4_reference","bench_shallow_4_hybrid","bench_shallow_5_hybrid","bench_shallow_5_reference","warm_steep_reference","warm_steep_hybrid","bench_steep_0_reference","bench_steep_0_hybrid","bench_steep_1_hybrid","bench_steep_1_reference","bench_steep_2_reference","bench_steep_2_hybrid","bench_steep_3_hybrid","bench_steep_3_reference","bench_steep_4_reference","bench_steep_4_hybrid","bench_steep_5_hybrid","bench_steep_5_reference","warm_clipped_reference","warm_clipped_hybrid","bench_clipped_0_reference","bench_clipped_0_hybrid","bench_clipped_1_hybrid","bench_clipped_1_reference","bench_clipped_2_reference","bench_clipped_2_hybrid","bench_clipped_3_hybrid","bench_clipped_3_reference","bench_clipped_4_reference","bench_clipped_4_hybrid","bench_clipped_5_hybrid","bench_clipped_5_reference","span_min_surface","line_min_surface_reference","line_min_surface_hybrid","copy_max_extents","height_fraction","height_zero","source_dimension_fraction","same_base_different_dimensions","fraction_x_hybrid","rgb_negative_hybrid","rgb_256_hybrid","coord_4097_hybrid","span_min_surface_outside","line_min_surface_outside_reference","line_min_surface_outside_hybrid","span_min_surface_crossing","line_min_surface_crossing_reference","line_min_surface_crossing_hybrid"};
const std::vector<std::string> completionDrawingLifecycleCases = {"span_left","overlap_diagonal","line_0_forward_reference","line_0_forward_hybrid"};
const std::vector<std::string> completionWalGuards = {"live_ownership","untracked_data","unknown","outside_used","boot_identity","boot_value","boot_txid","boot_version","corrupt","broken_chain","invalid_highwater","invalid_domain","low_corrupt","low_broken_chain","low_invalid_highwater","low_invalid_domain"}; // injected exact16
const std::vector<std::string> completionShapes = {"shape_sequential_closed","shape_sequential_closed_recovery","shape_interleaved","shape_interleaved_recovery","shape_reverse_closure","shape_reverse_closure_recovery","shape_duplicate_commit","shape_late_data","shape_aborted_zero","shape_aborted_zero_recovery"}; // injected exact10
const std::vector<std::string> completionWalWork = {"pressure","no_device","targeted_fsync","blocked_prefix","resumed_prefix_recovery","replay_orphan","replay_torn_chain","recovered_orphan","recovered_torn_chain","admission_io_failure","admission_fitting_io_failure","recovery_0","recovery_1","recovery_2","recovery_3","publication_failure_1","publication_failure_2","publication_failure_3","publication_failure_4","failed_publication_recovery_1","failed_publication_recovery_2","failed_publication_recovery_3","failed_publication_recovery_4","failed_publication_recovery_5","failed_publication_recovery_6"}; // injected exact original workloads
const std::vector<std::string> completionGuiFiles = {"manifest.json","process_table.json","window_table.json","input_journal.jsonl","framebuffer_snapshot.png","checkpoint/checkpoint.json","checkpoint/input_journal.jsonl","checkpoint/vm_state.bin"}; // injected exact GUI files
const std::vector<std::string> completionHostFiles = {"host_before/checkpoint.json","host_before/vm_state.bin","host_before/syscall_trace.jsonl","host_after/checkpoint.json","host_after/vm_state.bin","host_after/syscall_trace.jsonl","health/process_table.json"}; // injected service observation files
const std::vector<std::string> completionAutoFiles = {"automatic_boot/attempts.jsonl","host_before/automatic_observation.json","host_after/automatic_observation.json"}; // injected automatic observation files
const std::vector<std::string> completionServiceFiles = {"reconciliation.json","state.jsonl","progress.jsonl","cleanup.jsonl","embedded_guest.trit","guest.linked.tasm","guest.tboot","disk.tdisk","vm_state.bin","svc.0.json","svc.1.json","svc.gen.json","svc.report.json"}; // injected actual case files
const std::vector<std::string> completionZeroLogs = {"accepted_records.jsonl","committed_records.jsonl","jobs.jsonl","retirement.jsonl","syscalls.jsonl"}; // injected legitimately empty logs
const std::vector<std::string> completionModes={"normal","spawn_failure","startup_failure"};
std::vector<std::string> completionServiceNames() {
    std::vector<std::string> names; for(const auto& row:serviceScenarios())names.push_back(row.name);return names;
}
std::string completionUtc() {
    const std::time_t now=std::time(nullptr);std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc,&now);
#else
    gmtime_r(&now,&utc);
#endif
    std::ostringstream out;out<<std::put_time(&utc,"%Y-%m-%dT%H:%M:%SZ");return out.str();
}
const std::string completionRunId=[]() {
    std::ostringstream out;out<<std::chrono::system_clock::now().time_since_epoch().count();
#ifdef _WIN32
    out<<'-'<<GetCurrentProcessId();
#endif
    return out.str();
}();
std::vector<CompletionNeed> completionRequirements(const std::string& group,const std::vector<std::string>& names) {
    std::map<std::string,bool> files;
    auto add=[&](const std::string& root,const std::vector<std::string>& leaves,bool empty=false) {for(const auto& leaf:leaves)files[root+"/"+leaf]=empty;};
    const auto report=completionReports.at(group);if(!report.empty())files[report]=false;
    if(group=="kernel_word_classification") {
        const auto summary=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/report);std::smatch match;
        if(std::regex_search(summary,match,std::regex("run_[0-9]+"))) {
            const std::string root="compositor_workflow_artifact/stage2_transport/word_classification/"+match[0].str();
            add(root,{"report.json","golden_catalog.json","failure_modes.json","source_sha256_before.json","source_sha256_after.json","scenario_inventory.json","assertions.jsonl","denial_events.jsonl","query_events.jsonl","typed_inputs.jsonl","checkpoint_replay.jsonl","before_native_csr53.bin","commands.json","artifact_inventory.json"});
            for(const auto& entry:std::filesystem::recursive_directory_iterator(std::filesystem::path(TRIT_TEST_BUILD_DIR)/root))if(entry.is_regular_file())files[std::filesystem::relative(entry.path(),TRIT_TEST_BUILD_DIR).generic_string()]=entry.path().extension()==".txt";
        }
    }
    if(group=="compositor_drawing"||group=="compositor_drawing_lifecycle") {
        const bool lifecycle_only=group=="compositor_drawing_lifecycle";
        const auto summary=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/report);std::smatch run;
        if(require(std::regex_search(summary,run,std::regex("run_[0-9]+")),"drawing closed run directory")) {
            const auto root=std::string("compositor_workflow_artifact/stage4/")+(lifecycle_only?"lifecycle/":"")+run[0].str();
            add(root,{"report.json","source_hashes_before.json","source_hashes_after.json","drawing_actor.trit","drawing_actor.linked.tasm","drawing_actor.final.tasm","drawing_actor.catalog.json","drawing_survivor.trit","drawing_survivor.linked.tasm","drawing_survivor.final.tasm","drawing_survivor.catalog.json","boot.tasm","boot.tboot","disk.tdisk","terminal_state.bin","execution.json","registration.jsonl","lifecycle.jsonl","raw_returns.jsonl","ordered_cases.jsonl","drawing_raw_returns.jsonl","benchmark_raw.jsonl","counter_steps.jsonl","placement.jsonl","pixels.jsonl","guards.jsonl","special.jsonl","assertions.jsonl","case_inventory.json","artifact_inventory.json","host_commands.jsonl","benchmark_settings.json","selection.json","commands.json","criteria_map.json","bootstrap_policy.trit"});
            if(lifecycle_only)files[root+"/benchmark_raw.jsonl"]=true;
            for(size_t n=0;n<(lifecycle_only?completionDrawingLifecycleCases:completionDrawingCases).size();++n)add(root,{"rgb_"+std::to_string(n)+".png"});
            for(const auto& entry:std::filesystem::recursive_directory_iterator(std::filesystem::path(TRIT_TEST_BUILD_DIR)/root))
                if(entry.is_regular_file())files[std::filesystem::relative(entry.path(),TRIT_TEST_BUILD_DIR).generic_string()]=entry.path().extension()==".txt"||entry.path().filename()=="records.jsonl"||(lifecycle_only&&entry.path().filename()=="benchmark_raw.jsonl");
        }
    }
    if(group=="compositor_transport") {
        const auto summary=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/report);std::smatch run;
        if(require(std::regex_search(summary,run,std::regex("\\\"run_directory\\\":\\\"(run_[0-9]+)\\\"")),"transport closed run directory")) {
            const auto root="compositor_workflow_artifact/stage2_transport/"+run[1].str();
            add(root,{"report.json","source_hashes.json","scenario_plan.json","execution.json","boot.tasm","boot.tboot","disk.tdisk","terminal_state.bin","raw_returns.jsonl","interface_caller.trit","interface_caller.linked.tasm","behavior/behavior_assertions.jsonl"});
            for(const auto& name:names)add(root+"/behavior/"+name,{"boot.tasm","boot.tboot","disk.tdisk","terminal_state.bin","execution.json","registration.jsonl","lifecycle.jsonl","raw_returns.jsonl","records.jsonl","guest_assertions.json","bootstrap_policy.trit"});
            for(const auto& entry:std::filesystem::recursive_directory_iterator(std::filesystem::path(TRIT_TEST_BUILD_DIR)/root))if(entry.is_regular_file())files[std::filesystem::relative(entry.path(),TRIT_TEST_BUILD_DIR).generic_string()]=entry.path().extension()==".txt";
        }
    }
    if(group=="compositor_prerequisites") {
        const auto summary=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/report);std::smatch run;
        if(require(std::regex_search(summary,run,std::regex("\\\"run_directory\\\":\\\"(run_[0-9]+)\\\"")),"prerequisite closed run directory")) {
            const auto root="compositor_workflow_artifact/stage1/"+run[1].str();
            add(root,{"report.json","source_hashes.json","assertions.jsonl","capability_records.jsonl","pressure_geometry.json","reservation_audit.json","contract_snapshot.md"});
            for(const auto& name:names) {
                add(root+"/"+name,{"boot.tasm","boot.tboot","registration.jsonl","execution.json","lifecycle.jsonl","raw_returns.jsonl","terminal_state.bin","disk.tdisk","bootstrap_policy.trit"});
                add(root+"/"+name,{"kernel.compile.txt","assembly.txt"},true);
                const std::vector<std::string> guests=name=="capability_pressure"?std::vector<std::string>{"pressure","denied"}:std::vector<std::string>{"supervisor","owner","receiver","reuse"};
                for(const auto& guest:guests){add(root+"/"+name,{guest+".trit",guest+".linked.tasm"});add(root+"/"+name,{guest+".compile.txt",guest+".link.txt"},true);}
                if(name!="capability_pressure")add(root,{name+"_resources.jsonl"});
            }
        }
    }
    auto gui=[&](const std::string& root,bool host=false,bool automatic=false) {
        add(root,completionGuiFiles);if(host)add(root,completionHostFiles);if(automatic)add(root,completionAutoFiles);
    };
    if((group.rfind("service_",0)==0 && group!="service_auto_focused" && group!="service_link" && group!="service_coexistence") || group=="clock_service") {
        const std::string root=group=="clock_service"?"service_workflow_artifact/clock/service":"service_workflow_artifact/service";
        for(const auto& name:names) {
            if(std::find(completionModes.begin(),completionModes.end(),name)!=completionModes.end())continue;
            add(root+"/"+name,completionServiceFiles);add(root+"/"+name,completionZeroLogs,true);
            if(name=="zero_channel" || name=="pool1" || name=="pool2" || name=="pool3" || name=="pool4")files[root+"/"+name+"/cleanup.jsonl"]=true;
            if(group=="clock_service")add(root+"/"+name,{"clock_report.json"});
        }
        if(group=="service_full")gui(root+"/coexistence/desktop",true);
    }
    if(group=="service_auto" || group=="service_auto_focused") {
        const auto root=std::filesystem::path(report).parent_path().generic_string();
        for(const auto& mode:completionModes) {add(root+"/"+mode,{"mode_report.json"});gui(root+"/"+mode+"/desktop",true,true);}
    }
    if(group=="service_coexistence")gui("service_workflow_artifact/service/coexistence/desktop",true);
    if(group=="desktop") {gui("desktop_workflow_artifact");add("service_workflow_artifact/desktop_input",{"initial_images.json","initial.tboot","initial.tdisk"});}
    if(group.rfind("wal_",0)==0) {
        const auto root=std::filesystem::path(report).parent_path().generic_string();
        auto cases=names;
        if(group=="wal_full") {
            cases=completionWalGuards;cases.insert(cases.end(),completionShapes.begin(),completionShapes.end());
            cases.insert(cases.end(),{"extent_lifecycle","extent_lifecycle_recovery"});cases.insert(cases.end(),completionWalWork.begin(),completionWalWork.end());
        }
        for(const auto& name:cases) {
            add(root+"/"+name,{"boot_result.json","boot.tboot","boot.tasm","live.tdisk"});
            if(name=="targeted_fsync")add(root+"/"+name,{"frames.json"});
            else if(name.rfind("replay_",0)==0)add(root+"/"+name,{"durable_crash.tdisk"});
            else add(root+"/"+name,{"result.json"});
        }
        if(group=="wal_full")add(root,{"checkpoint_shapes_report.json","extent_lifecycle_report.json","pressure/storage.jsonl","pressure/syscalls.jsonl","pressure/before_checkpoint_stage1.tdisk","pressure/before_checkpoint_stage2.tdisk","pressure/before_checkpoint_stage3.tdisk","pressure/before_checkpoint_stage4.tdisk","blocked_prefix/snapshot_release.jsonl","extent_lifecycle/lifecycle.jsonl","extent_lifecycle/cursor_injections.jsonl"});
    }
    const std::map<std::string,std::vector<std::string>> extra={{"sdk",{"embedded_guest.trit","guest.linked.tasm","sdk.tboot","raw_return_records.jsonl","sdk_results.json","vfs_content.json","disk.tdisk","heap_metadata.jsonl","heap_memory.json","terminal_state.bin"}},{"metrics",{"embedded_guest.trit","guest.linked.tasm","metrics.tboot","timer_transitions.jsonl","kernel_assertions.json","idle_accounting.json","guest_report.json","busy_syscall_accounting.json","vm_run_accounting.json"}},{"stream",{"embedded_guest.trit","guest.linked.tasm","source_hashes.json","stream-events.json"}},{"numeric",{"guest.trit","guest_O0.tasm","guest_O1.tasm","guest_O2.tasm","accumulator.tasm","mid_reduction.bin"}},{"clock_kernel",{"embedded_guest.trit","guest.linked.tasm","clock.tboot","execution.json","actual_boot_rate.jsonl","actual_waits.jsonl","queue_transitions.jsonl","terminal_state.bin"}},{"clock_boundaries",{"embedded_guest.trit","guest.linked.tasm","clock.tboot","execution.json","actual_boot_rate.jsonl","actual_waits.jsonl","queue_transitions.jsonl","terminal_state.bin"}},{"clock_idle_input",{"boot.tboot","phases.jsonl","eret_observations.jsonl","prefix_observations.jsonl","prefix_state.bin","guest0.trit","guest1.trit","guest2.trit"}}}; // injected actual smaller group evidence
    if(extra.count(group))add(std::filesystem::path(report).parent_path().generic_string(),extra.at(group));
    if(group=="service_link") {
        for(const auto& name:names)add("service_workflow_artifact/service_link_preflight/"+name,{"preflight.compiler.tasm"});
    }
    if(group=="cli") {
        const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact/final_gate/cli_e2e";
        add("service_workflow_artifact/final_gate/cli_e2e",{"fixture_program.txt","service_scenarios.json"});
        if(std::filesystem::exists(root))for(const auto& scenario:std::filesystem::directory_iterator(root))if(scenario.is_directory())
            for(const auto& file:std::filesystem::directory_iterator(scenario.path()))if(file.is_regular_file() && file.path().extension()==".log")files[file.path().lexically_relative(TRIT_TEST_BUILD_DIR).generic_string()]=false;
    }
    std::vector<CompletionNeed> result;for(const auto& file:files)result.push_back({file.first,file.second});return result;
}
bool completionPublish(const std::filesystem::path& path,const std::string& contents) {
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);
    if(!require(!error,"completion mkdir: "+error.message()))return false;
    const auto temporary=path.string()+".pending-write";
    {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!require(out.is_open(),"completion temporary open"))return false;out<<contents;out.flush();out.close();if(!require(!out.fail(),"completion write/flush/close"))return false;}
#ifdef _WIN32
    if(!require(MoveFileExW(std::filesystem::path(temporary).c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"atomic completion replacement"))return false;
#else
    std::filesystem::rename(temporary,path,error);if(!require(!error,"atomic completion replacement: "+error.message()))return false;
#endif
    return true;
}
bool completionWorkflow(const std::string& group,const std::string& selector,const std::string& scope,const std::vector<std::string>& names,const std::function<bool()>& workflow) {
    std::map<std::string,std::string> sources;
    bool inputs_ok=true;
    for(const auto& source:completionSourcePaths) {
        const auto bytes=readTextFile(source);inputs_ok=require(!bytes.empty(),"completion required current source "+source)&&inputs_ok;sources[source]=stableHash(bytes);
    }
    const auto started=completionUtc();const auto path=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact/completion"/(group+".json");
    auto record=[&](bool complete,bool passed,const std::map<std::string,std::pair<std::string,size_t>>& files) {
        std::ostringstream out;out<<"{\"schema_version\":1,\"group\":\""<<jsonEscape(group)<<"\",\"run_id\":\""<<completionRunId<<"\",\"selector\":\""<<jsonEscape(selector)<<"\",\"scope\":\""<<jsonEscape(scope)<<"\",\"state\":\""<<(complete?"complete":"pending")<<"\",\"outcome\":";
        if(complete)out<<'"'<<(passed?"PASS":"FAIL")<<'"';else out<<"null";
        out<<",\"started_utc\":\""<<started<<"\",\"finished_utc\":";if(complete)out<<'"'<<completionUtc()<<'"';else out<<"null";
        out<<",\"contract\":{\"isa_version\":2,\"executable_abi_version\":3,\"function_abi_version\":3,\"syscall_abi_version\":2,\"vector_abi_version\":1,\"tboot_version\":3,\"tdisk_version\":2},\"source_inputs\":{";
        bool comma=false;for(const auto& source:sources) {if(comma)out<<',';comma=true;out<<'"'<<jsonEscape(source.first)<<"\":{\"algorithm\":\"FNV-1a-64\",\"present\":true,\"hash\":\""<<source.second<<"\"}";}
        out<<"},\"evidence_files\":{";comma=false;for(const auto& file:files) {if(comma)out<<',';comma=true;out<<'"'<<jsonEscape(file.first)<<"\":{\"algorithm\":\"FNV-1a-64\",\"hash\":\""<<file.second.first<<"\",\"size_bytes\":"<<file.second.second<<'}';}
        out<<"},\"required_scenarios\":[";for(size_t i=0;i<names.size();++i) {if(i)out<<',';out<<'"'<<jsonEscape(names[i])<<'"';}out<<"]}\n";return out.str();
    };
    if(!completionPublish(path,record(false,false,{})) || !inputs_ok)return false;
    const bool actual=workflow();bool closed=actual;
    for(const auto& source:sources)closed=require(stableHash(readTextFile(source.first))==source.second,"completion source changed during workflow: "+source.first)&&closed;
    std::map<std::string,std::pair<std::string,size_t>> files;
    for(const auto& need:completionRequirements(group,names)) {
        const auto file=std::filesystem::path(TRIT_TEST_BUILD_DIR)/need.path;std::ifstream input(file,std::ios::binary);
        if(!require(input.is_open(),"completion missing/unreadable evidence "+need.path)) {closed=false;continue;}
        const std::string bytes(std::istreambuf_iterator<char>(input),{});input.close();
        if(!require(need.allow_empty || !bytes.empty(),"completion empty required evidence "+need.path))closed=false;
        files[need.path]={stableHash(bytes),bytes.size()};
    }
    // Fingerprint legitimate zero-record logs too; positive counters require real records.
    if(group=="service_full" || group=="service_errors" || group=="service_fixtures" || group=="service_history_cap1" || group=="service_reservation" || group=="service_edges" || group=="service_auto" || group=="clock_service") {
        const std::string root=group=="clock_service"?"service_workflow_artifact/clock/service":"service_workflow_artifact/service";
        for(const auto& name:names) {
            const auto base=std::filesystem::path(TRIT_TEST_BUILD_DIR)/root/name;
            const auto reconciliation=readTextFile(base/"reconciliation.json");
            for(const auto& count:std::vector<std::pair<std::string,std::string>>{{"accepted","accepted_records.jsonl"},{"committed","committed_records.jsonl"},{"retired","retirement.jsonl"}}) {
                std::smatch value;const std::regex pattern("\\\""+count.first+"\\\"\\s*:\\s*([0-9]+)");
                if(std::regex_search(reconciliation,value,pattern) && std::stoull(value[1].str())>0)closed=require(!readTextFile(base/count.second).empty(),"completion positive "+count.first+" requires nonempty "+count.second)&&closed;
            }
            for(const auto& scenario:serviceScenarios())if(scenario.name==name && scenario.job_case)closed=require(!readTextFile(base/"jobs.jsonl").empty(),"completion job case requires actual jobs")&&closed;
        }
    }
    if(!completionPublish(path,record(true,closed,files)))return false;
    return require(closed,"coupled completion failed for "+group);
}
bool runDesktopIsolated() {
    const auto release=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"release/TernaryOS";
    const auto input=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"service_workflow_artifact/desktop_input";
    std::error_code error;std::filesystem::create_directories(input,error);
    if(!require(!error,"desktop input directory"))return false;
    std::ostringstream provenance;provenance<<"{\"algorithm\":\"FNV-1a-64\",\"initial_images\":{";
    bool comma=false;
    for(const auto& image:std::vector<std::pair<std::string,std::string>>{{"ternary-os.tboot","initial.tboot"},{"ternary-os.tdisk","initial.tdisk"}}) {
        const auto bytes=readTextFile(release/image.first);if(!require(!bytes.empty(),"desktop release input "+image.first))return false;
        std::filesystem::copy_file(release/image.first,input/image.first,std::filesystem::copy_options::overwrite_existing,error);
        if(!require(!error,"desktop writable image copy: "+error.message()))return false;
        std::filesystem::copy_file(release/image.first,input/image.second,std::filesystem::copy_options::overwrite_existing,error);
        if(!require(!error && stableHash(readTextFile(input/image.first))==stableHash(bytes) && stableHash(readTextFile(input/image.second))==stableHash(bytes),"desktop initial image copy/digest"))return false;
        if(comma)provenance<<',';comma=true;provenance<<'"'<<image.first<<"\":{\"hash\":\""<<stableHash(bytes)<<"\",\"size_bytes\":"<<bytes.size()<<'}';
    }
    provenance<<"},\"writable_disk\":\"ternary-os.tdisk\",\"immutable_disk_snapshot\":\"initial.tdisk\"}\n";
    if(!completionPublish(input/"initial_images.json",provenance.str()))return false;
    return runDesktopWorkflow(input);
}
// END FINAL COUPLED COMPLETION DRIVER

// Stage 1 uses real VFS-installed guests and the normal native trap/scheduler.
// The only bootstrap policy is application registration and existing IPC setup.
bool runCompositorPrerequisites() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage1";
    std::filesystem::create_directories(root);
    int run_number=1;
    while(std::filesystem::exists(root/("run_"+std::to_string(run_number))))++run_number;
    const auto base=root/("run_"+std::to_string(run_number));
    std::filesystem::create_directories(base);
    std::vector<std::string> failures;
    auto check=[&](bool ok,const std::string& label) {if(!ok){failures.push_back(label);require(false,"compositor prerequisite: "+label);}return ok;};
    const auto kernel_text=readTextFile("kernel.trit");
    std::smatch cap_match;
    if(!check(std::regex_search(kernel_text,cap_match,std::regex("const PROC_CAPS_BASE: t40 = ([0-9]+);")),"capability reservation is observable"))return false;
    const int caps_base=std::stoi(cap_match[1]);
    std::ofstream(base/"source_hashes.json")<<"{\"algorithm\":\"FNV-1a-64\",\"input\":\"file bytes\",\"kernel.trit\":\""<<stableHash(kernel_text)<<"\",\"apps/os_sdk.trit\":\""<<stableHash(readTextFile("apps/os_sdk.trit"))<<"\",\"tests/current_only_conformance.cpp\":\""<<stableHash(readTextFile("tests/current_only_conformance.cpp"))<<"\"}\n";
    std::ofstream assertions(base/"assertions.jsonl");
    auto assertion=[&](bool ok,const std::string& label){assertions<<"{\"pass\":"<<(ok?"true":"false")<<",\"assertion\":\""<<jsonEscape(label)<<"\"}\n";return check(ok,label);};
    std::map<std::string,std::string> constants;
    const std::regex declaration("const ([A-Z][A-Z0-9_]*): t40 = ([^;]+);");
    for(auto it=std::sregex_iterator(kernel_text.begin(),kernel_text.end(),declaration);it!=std::sregex_iterator();++it)constants[(*it)[1]]=(*it)[2];
    std::function<long long(const std::string&)> value;
    value=[&](const std::string& expression)->long long {
        const auto plus=expression.find('+');if(plus!=std::string::npos)return value(expression.substr(0,plus))+value(expression.substr(plus+1));
        const auto times=expression.find('*');if(times!=std::string::npos)return value(expression.substr(0,times))*value(expression.substr(times+1));
        auto token=expression;token.erase(0,token.find_first_not_of(" \r\n\t"));token.erase(token.find_last_not_of(" \r\n\t")+1);
        if(constants.count(token))return value(constants.at(token));return std::stoll(token);
    };
    const std::vector<std::pair<std::string,std::string>> reservations={
        {"KERNEL_STATE_BASE","56"},{"BOOTSTRAP_BITMAP_BASE","BOOTSTRAP_MAX_PAGES"},{"PAGE_HEADERS_BASE","BOOTSTRAP_MAX_PAGES*PAGE_HEADER_WORDS"},
        {"PROC_PARENT_PID_BASE","PROCESS_MAX"},{"PROC_EXIT_STATUS_BASE","PROCESS_MAX"},{"PROC_SIGNAL_PENDING_BASE","PROCESS_MAX"},{"PROC_CLEANED_BASE","PROCESS_MAX"},{"IPC_OWNER_BASE","IPC_MAX_CHANNELS"},{"PROC_CAPS_BASE","PROCESS_MAX"},
        {"BUFFER_POOL_BASE","BUFFER_MAX_FRAMES*BUFFER_FRAME_WORDS"},{"NS_DIRTY_VERSION_BASE","NS_DIRTY_VERSION_WORDS"},{"FB_STATE_BASE","FB_STATE_WORDS"},{"FB_FRONT_BUFFER_BASE","FB_MAX_WORDS"},{"FB_BACK_BUFFER_BASE","FB_MAX_WORDS"},
        {"WINDOW_BASE","WINDOW_MAX*WINDOW_ROW_WORDS"},{"WINDOW_EVENT_BASE","WINDOW_MAX*WINDOW_EVENT_CAPACITY*WINDOW_EVENT_WORDS"},{"WINDOW_BUFFER_BASE","WINDOW_MAX*WINDOW_BUFFER_WORDS"},
        {"WAIT_QUEUE_BASE","WAIT_QUEUE_MAX*WAIT_QUEUE_WORDS"},{"WAIT_PROC_NEXT_BASE","PROCESS_MAX"},{"WAIT_KIND_BASE","PROCESS_MAX"},{"WAIT_DEADLINE_BASE","PROCESS_MAX"},{"WAIT_ARG0_BASE","PROCESS_MAX"},{"WAIT_ARG1_BASE","PROCESS_MAX"},{"WAIT_ARG2_BASE","PROCESS_MAX"},{"WAIT_QUEUE_TAIL_BASE","WAIT_QUEUE_MAX"},
        {"WAL_BASE","WAL_CAPACITY*WAL_RECORD_WORDS"},{"WAL_TX_TRACKER_KEY_BASE","WAL_TX_TRACKER_CAPACITY"},{"WAL_TX_TRACKER_LSN_BASE","WAL_TX_TRACKER_CAPACITY"},{"WAL_TX_FALLBACK_BASE","WAL_TX_TRACKER_CAPACITY"},{"WAL_SNAPSHOT_BASE","WAL_SNAPSHOT_MAX*WAL_SNAPSHOT_WORDS"},{"ATTR_STORE_BASE","ATTR_STORE_MAX*ATTR_STORE_WORDS"},
        {"TIER1_QUEUE_BASE","TIER1_CAPACITY"},{"MACRO_QUEUE_BASE","MACRO_CAPACITY"},{"SCHED_SLOT_GENERATION_BASE","PROCESS_MAX"},{"SCHED_SLOT_MEMBER_BASE","PROCESS_MAX"},{"TIER1_QUEUE_GENERATION_BASE","TIER1_CAPACITY"},{"PID_INDEX_KEY_BASE","PID_INDEX_CAPACITY"},{"PID_INDEX_SLOT_BASE","PID_INDEX_CAPACITY"},{"TIMER_WHEEL_HEAD_BASE","TIMER_WHEEL_BUCKETS"},{"TIMER_WHEEL_NEXT_BASE","PROCESS_MAX"},
        {"VFS_INODE_BASE","VFS_MAX_INODES*VFS_INODE_WORDS"},{"INODE_REQUIRED_LSN_BASE","INODE_REQUIRED_LSN_WORDS"},{"VFS_DIRENT_BASE","VFS_MAX_DIRENTS*VFS_DIRENT_WORDS"},{"VFS_DIRENT_NAME_BASE","VFS_MAX_DIRENTS*VFS_MAX_NAME_WORDS"},{"VFS_EXTENT_BASE","VFS_MAX_EXTENTS*VFS_EXTENT_WORDS"},{"VFS_FD_BASE","VFS_MAX_FDS*VFS_FD_WORDS"},{"VFS_DATA_BASE","VFS_PAYLOAD_WORDS"},
        {"IPC_BASE","IPC_MAX_CHANNELS*IPC_CHANNEL_WORDS"},{"IPC_BUFFER_BASE","IPC_MAX_CHANNELS*IPC_CAPACITY"},{"NET_BASE","NET_MAX_SOCKETS*NET_SOCKET_WORDS"},{"NET_RX_BASE","NET_MAX_SOCKETS*NET_RX_CAPACITY"},
        {"PROCESS_BASE","PROCESS_MAX*PROCESS_ROW_WORDS"},{"QUOTA_BASE","QUOTA_MAX*QUOTA_ROW_WORDS"},{"DESIRED_PROCESS_BASE","PROCESS_MAX"},{"WAIT_CHANNEL_BASE","PROCESS_MAX"},{"NAMESPACE_BASE","NAMESPACE_MAX*NAMESPACE_ROW_WORDS"},{"PTE_BASE","PROCESS_MAX*PTE_MAX_VPN*PTE_ROW_WORDS"},{"PROCESS_CACHE_BASE","PROCESS_MAX*PROCESS_CACHE_WORDS"},{"TRANSLATION_CACHE_BASE","PROCESS_MAX*TRANSLATION_CACHE_WORDS"},{"EXEC_IMAGE_BASE","EXEC_IMAGE_WORDS"},{"PROCESS_ARCH_BASE","PROCESS_MAX*PROCESS_ARCH_WORDS"},{"HW_PT_BASE","PROCESS_MAX*HW_PT_WORDS"},{"PROCESS_HEAP_BASE","PROCESS_MAX*PROCESS_HEAP_WORDS"},{"PROCESS_CONTEXT_BASE","PROCESS_MAX*TASK_CONTEXT_WORDS"},{"PROCESS_CONTEXT_V3_BASE","PROCESS_MAX*TASK_CONTEXT_V3_WORDS"},{"APP_REGISTRY_BASE","APP_MAX*APP_ROW_WORDS"}
    };
    std::ofstream audit(base/"reservation_audit.json");audit<<"{\"algorithm\":\"FNV-1a-64 file bytes\",\"kernel_hash\":\""<<stableHash(kernel_text)<<"\",\"capability_start\":"<<caps_base<<",\"ranges\":[";bool comma=false,disjoint=true;
    for(const auto& row:reservations){const auto start=value(row.first),end=start+value(row.second);if(comma)audit<<',';comma=true;audit<<"{\"name\":\""<<row.first<<"\",\"start\":"<<start<<",\"end_exclusive\":"<<end<<'}';if(row.first!="PROC_CAPS_BASE" && start<caps_base+value("PROCESS_MAX") && caps_base<end)disjoint=false;}
    const auto backing_begin=value("PROCESS_DMEM_PPN_BASE")*value("MMU_PAGE_WORDS"),backing_end=(value("PROCESS_DMEM_PPN_BASE")+value("PROCESS_MAX")*value("PROCESS_DMEM_PAGES_MAX"))*value("MMU_PAGE_WORDS");
    disjoint &= caps_base+value("PROCESS_MAX")<=backing_begin || backing_end<=caps_base;
    audit<<",{\"name\":\"process physical backing\",\"start\":"<<backing_begin<<",\"end_exclusive\":"<<backing_end<<"}],\"capability_disjoint\":"<<(disjoint?"true":"false")<<",\"scope\":\"capability isolation; unrelated preexisting overlaps remain outside Stage1\"}\n";audit.close();
    assertion(disjoint,"AC1 source-audited capability range disjoint from all static reservations");
    const auto contract=readTextFile("docs/07_OS_Substrate/native_compositor_contract.md");std::ofstream(base/"contract_snapshot.md")<<contract;
    assertion(!contract.empty() && contract.find("Pending Stage 2")!=std::string::npos,"AC4 foundation contract with pending later-stage boundary recorded");
    auto path_source=[](int address,const std::string& path){std::string s;for(size_t i=0;i<=path.size();++i)s+="unsafe { store("+std::to_string(address+i)+", "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"); }\n";return s;};
    struct Guest {std::string name,source;int slot,pid,caps,parent;};
    int release_label=-1,release_return_label=-1,wake_signal_label=-1;
    std::map<int,std::array<long long,3>> ipc_returns;
    std::map<int,long long> ipc_return_steps;
    std::map<int,int> ipc_return_pcs,ipc_completion_counts;
    auto execute=[&](const std::string& name,const std::vector<Guest>& guests,const std::string& setup,const std::function<bool(vm::VMState&,const vm::VMExecutionRecord&,long long)>& observe) {
        const auto dir=base/name;std::filesystem::create_directories(dir);
        compiler::CompilerOptions opts;opts.enable_vector_abi=true;opts.enable_vector_spilling=true;
        os::NativeVfsImageBuilder fs(32768);if(!check(fs.installBaseLayout().ok(),name+" rootfs"))return;
        for(size_t i=0;i<guests.size();++i){const auto& g=guests[i];std::ofstream(dir/(g.name+".trit"))<<g.source;
            const auto app=compiler::compileSource(g.name+".trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+g.source,opts);
            {std::ofstream out(dir/(g.name+".compile.txt"));for(const auto& d:app.diagnostics)out<<d.format()<<'\n';}
            if(!check(app.success,name+" compile "+g.name))return;
            compiler::LinkOptions lo;lo.stack_hint_words=2048;lo.dead_strip_functions=true;lo.standalone_halt_on_exit=false;lo.enable_vector_abi=true;lo.enable_vector_spilling=true;
            const auto linked=compiler::linkModules({app.object},lo);
            {std::ofstream out(dir/(g.name+".link.txt"));for(const auto& d:linked.diagnostics)out<<d.format()<<'\n';}
            if(!check(linked.success,name+" link "+g.name))return;
            std::ofstream(dir/(g.name+".linked.tasm"))<<linked.assembly;
            if(!check(fs.addExecutableImage("/tmp/"+g.name,linked.assembled.program,linked.executable_header_v3,256+static_cast<int>(i)*64).ok(),name+" install "+g.name))return;
        }
        std::ofstream(dir/"bootstrap_policy.trit")<<setup;
        const auto kernel=compiler::compileSource("prerequisites_kernel.trit",readTextFile("architecture_contract.trit")+"\n"+kernel_text+"\n"+setup);
        {std::ofstream out(dir/"kernel.compile.txt");for(const auto& d:kernel.diagnostics)out<<d.format()<<'\n';}
        if(!check(kernel.success,name+" kernel compile"))return;
        std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n mov r13, 0\n mov r14, 3\n call process_set_state\n mov r1, native_trap_entry\n csrw tvec, r1\n";
        int registration_index=0;
        for(const auto& g:guests){const auto path="/tmp/"+g.name;for(size_t i=0;i<=path.size();++i)boot+=" mov r1, "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"\n mov r2, "+std::to_string(10020+i)+"\n store r1, r2, 0\n";
            boot+=" mov r13, 0\n mov r14, 10020\n mov r15, "+std::to_string(g.caps)+"\n mov r16, 0\n mov r17, 1\n mov r18, 12000\n call app_register\n";
            boot+=" mov r2, "+std::to_string(14200+registration_index)+"\n store r13, r2, 0\n";
            if(g.slot>0)boot+=" mov r13, "+std::to_string(g.parent)+"\n mov r14, 10020\n mov r15, "+std::to_string(g.slot)+"\n mov r16, "+std::to_string(g.pid)+"\n call app_spawn\n";
            boot+=" mov r2, "+std::to_string(14300+registration_index)+"\n store r13, r2, 0\n";++registration_index;
        }
        boot+=" call prerequisite_setup\n mov r13, "+std::to_string(guests.front().pid)+"\n call scheduler_switch_to_pid\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
        boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
        auto fw=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
        auto source=[&](int n){return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(n)+", 0, 27, "+std::to_string(fw)+", 2, 0\n";};
        auto assembled=vm::assembler::assembleV2(source(1));fw=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
        assembled=vm::assembler::assembleV2(source(1));if(assembled.success)assembled=vm::assembler::assembleV2(source(static_cast<int>(assembled.program.size())));
        std::ofstream(dir/"boot.tasm")<<source(static_cast<int>(assembled.program.size()));
        {std::ofstream out(dir/"assembly.txt");for(const auto& e:assembled.errors)out<<e.format()<<'\n';}
        if(!check(assembled.success,name+" boot assembly"))return;
        const auto release_it=assembled.labels.find("process_release_resources");release_label=release_it==assembled.labels.end()?-1:release_it->second;
        const auto release_return_it=assembled.labels.find("process_release_resources_return");release_return_label=release_return_it==assembled.labels.end()?-1:release_return_it->second;
        const auto wake_it=assembled.labels.find("process_wake_for_signal");wake_signal_label=wake_it==assembled.labels.end()?-1:wake_it->second;
        host::TosImageManifest manifest;auto image=host::bootImageFromAssembly(assembled,manifest,fs.image());std::string error;
        if(!check(host::writeBootImageFile((dir/"boot.tboot").string(),image,&error),name+" current boot image "+error))return;
        auto m=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));if(!check(host::loadBootImageIntoVm(*m,image,"",&error),name+" load "+error))return;
        std::ofstream trace(dir/"lifecycle.jsonl"),raw(dir/"raw_returns.jsonl");
        std::array<long long,4> last_state{{-1,-1,-1,-1}};bool completed=false,setup_checked=false;long long steps=0;
        std::map<int,std::pair<int,int>> pending;
        ipc_returns.clear();
        ipc_return_steps.clear();ipc_return_pcs.clear();ipc_completion_counts.clear();
        for(;steps<120000000 && m->cycle_count<2000000000;++steps){vm::VMExecutionRecord rec;vm::step(*m,&rec);
            if(!setup_checked && rec.before_privilege==vm::PrivilegeMode::User){setup_checked=true;bool policy_ok=true;std::ofstream policy(dir/"registration.jsonl");
                for(size_t i=0;i<guests.size();++i){const auto& g=guests[i];const auto registration=serviceKernelWord(*m,14200+static_cast<int>(i)),launch=serviceKernelWord(*m,14300+static_cast<int>(i));
                    policy<<"{\"path\":\"/tmp/"<<g.name<<"\",\"registration_return\":"<<registration<<",\"launch_return\":"<<launch<<",\"registered_caps\":"<<serviceKernelWord(*m,665000+static_cast<int>(registration)*10+3)<<",\"process_caps\":"<<(g.slot>0?serviceKernelWord(*m,caps_base+g.slot):-1)<<"}\n";
                    policy_ok &= assertion(registration>=0,name+" successful explicit registration "+g.name);
                    if(g.slot>0)policy_ok &= assertion(launch==g.pid && serviceKernelWord(*m,caps_base+g.slot)==g.caps,name+" real launch and initial mask "+g.name);
                }if(!policy_ok)break;
            }
            if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL && rec.before_privilege==vm::PrivilegeMode::User){const int pid=static_cast<int>(serviceKernelWord(*m,3020));pending[pid]={rec.syscall_id,rec.pc};raw<<"{\"classification\":\"syscall_entry\",\"step\":"<<steps<<",\"pid\":"<<pid<<",\"syscall_id\":"<<rec.syscall_id<<",\"pc\":"<<rec.pc<<",\"args\":["<<rec.syscall_arg0<<','<<rec.syscall_arg1<<','<<rec.syscall_arg2<<','<<rec.syscall_arg3<<"]}\n";}
            bool boundary=steps%10000==0 || (rec.attempted && (rec.pc==release_label || rec.pc==release_return_label || rec.pc==wake_signal_label));
            boundary |= rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL;
            if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::STORE){const auto a=vm::ops::toLong(m->regfile.read(rec.instruction.rs1))+rec.instruction.imm;
                boundary |= rec.before_privilege==vm::PrivilegeMode::User && (a==10090 || a==10091);
                boundary |= rec.before_privilege==vm::PrivilegeMode::Kernel && ((a>=130300 && a<131700) || (a>=390000 && a<390900) || (a>=138500 && a<139100) || a==3053);
            }
            if(boundary){
            for(int slot=1;slot<=3;++slot){const auto state=serviceKernelWord(*m,390000+slot*9+2);if(state!=last_state[slot]){trace<<"{\"step\":"<<steps<<",\"cycle\":"<<m->cycle_count<<",\"slot\":"<<slot<<",\"pid\":"<<serviceKernelWord(*m,390000+slot*9)<<",\"before\":"<<last_state[slot]<<",\"after\":"<<state<<",\"channel\":"<<serviceKernelWord(*m,391700+slot)<<",\"kind\":"<<serviceKernelWord(*m,138600+slot)<<",\"deadline\":"<<serviceKernelWord(*m,138700+slot)<<",\"caps\":"<<serviceKernelWord(*m,caps_base+slot)<<"}\n";last_state[slot]=state;}}
            if(observe(*m,rec,steps)){completed=true;break;}
            }
            if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET){const int pid=static_cast<int>(serviceKernelWord(*m,3020));const auto it=pending.find(pid);const bool matched=it!=pending.end() && m->pc==it->second.second+1;
                const std::array<long long,3> values{{vm::ops::toLong(m->regfile.read(13)),vm::ops::toLong(m->regfile.read(14)),vm::ops::toLong(m->regfile.read(15))}};
                raw<<"{\"step\":"<<steps<<",\"pid\":"<<pid<<",\"classification\":\""<<(matched?"completed_syscall":"scheduler_or_timer_resume")<<"\",\"syscall_id\":"<<(matched?it->second.first:-1)<<",\"cause\":"<<m->cause<<",\"resume_pc\":"<<m->pc<<",\"r13\":"<<values[0]<<",\"r14\":"<<values[1]<<",\"r15\":"<<values[2]<<"}\n";
                if(matched){if(it->second.first==54){ipc_returns[pid]=values;ipc_return_steps[pid]=steps;ipc_return_pcs[pid]=m->pc;++ipc_completion_counts[pid];}pending.erase(it);}
            }
            if(m->status==vm::VMStatus::WAITING && serviceKernelWord(*m,390027)==203 && serviceKernelWord(*m,391703)==101000 && serviceKernelWord(*m,380007)==0 && serviceKernelWord(*m,391701)==100203){
                assertion(false,"AC2 "+name+" peer remains blocked on released owner channel; bounded RED checkpoint");break;
            }
            if(m->status==vm::VMStatus::TRAPPED || m->status==vm::VMStatus::HALTED)break;
        }
        assertion(completed,name+" completed through real guest execution");
        std::ofstream(dir/"execution.json")<<"{\"completed\":"<<(completed?"true":"false")<<",\"steps\":"<<steps<<",\"cycles\":"<<m->cycle_count<<",\"status\":"<<static_cast<int>(m->status)<<",\"pc\":"<<m->pc<<",\"cause\":"<<m->cause<<"}\n";
        check(host::detail::writeCheckpointState(dir/"terminal_state.bin",*m,&error),name+" checkpoint "+error);
        check(m->block_device.writeSnapshotFile((dir/"disk.tdisk").string()),name+" current disk snapshot");
    };
    // Cache pages are STORAGE_BLOCK_WORDS27, not729-word MMU pages.
    // 80 distinct current cache pages force all64 frames, flush and eviction.
    std::string pressure="fn main() -> t40 {\n"+path_source(10300,"/tmp/cap-pressure")+R"TRIT(
    let before: t40 = os_kill(203,8); unsafe { store(10000,before); }
    let fd: t40 = os_open(10300,3); unsafe { store(10002,fd); }
    var i: t40 = 0; while i < 27 { unsafe { store(11000+i, i+17); } i=i+1; }
    i=0; var total: t40=0; while i < 80 { let n: t40=os_write(fd,11000,27); total=total+n; i=i+1; }
    let synced: t40=os_fsync(fd); let closed: t40=os_close(fd);
    let after: t40=os_kill(203,8);
    unsafe { store(10001,after); store(10003,total); store(10004,synced); store(10005,closed); }
    os_ipc_send(3,91); unsafe { store(10090,777); }
    os_futex_wait(10400,0,-1); return 0;
}
)TRIT";
    std::string denied=R"TRIT(fn main() -> t40 {
    let before: t40=os_kill(202,8); unsafe { store(10000,before); }
    os_ipc_recv_blocking(3,10400,-1);
    let after: t40=os_kill(202,8); unsafe { store(10001,after); store(10090,777); }
    os_futex_wait(10401,0,-1); return 0;
})TRIT";
    bool caps_stable=true, pressure_started=false;int frames=0;long long flushes=0,evictions=0;
    std::ofstream captrace(base/"capability_records.jsonl");
    execute("capability_pressure",{{"pressure",pressure,1,202,23,1},{"denied",denied,2,203,7,1}},"fn prerequisite_setup() -> t40 { ipc_open_owned(3,1,0,0); return 1; }\n",[&](vm::VMState& m,const vm::VMExecutionRecord& rec,long long steps){
        if(!pressure_started && rec.before_privilege==vm::PrivilegeMode::User && serviceKernelWord(m,3020)==202){pressure_started=true;captrace<<"{\"event\":\"first_pressure_guest_syscall\",\"step\":"<<steps<<",\"caps\":["<<serviceKernelWord(m,caps_base+1)<<','<<serviceKernelWord(m,caps_base+2)<<"]}\n";}
        if(pressure_started && serviceKernelWord(m,390009)==202 && serviceKernelWord(m,390018)==203){
            const auto a=serviceKernelWord(m,caps_base+1),b=serviceKernelWord(m,caps_base+2);caps_stable &= a==23 && b==7;
            if(steps%100000==0){captrace<<"{\"step\":"<<steps<<",\"caps\":["<<a<<','<<b<<"],\"frames\":"<<frames<<",\"flushes\":"<<serviceKernelWord(m,3026)<<",\"evictions\":"<<serviceKernelWord(m,3027)<<"}\n";}
        }
        frames=0;for(int i=0;i<64;++i)if(serviceKernelWord(m,130700+i*11)>0)++frames;
        flushes=serviceKernelWord(m,3026);evictions=serviceKernelWord(m,3027);
        if(serviceUserWord(m,10090,1)!=777 || serviceUserWord(m,10090,2)!=777)return false;
        assertion(caps_stable,"AC1 registered masks23/7 stable under actual buffer activity");
        assertion(frames==64 && flushes>0 && evictions>0,"AC1 all64 buffer frames plus flush and eviction observed");
        assertion(serviceUserWord(m,10000,1)>=0 && serviceUserWord(m,10001,1)>=0,"AC1 privileged sibling operations remain allowed");
        assertion(serviceUserWord(m,10000,2)==-8 && serviceUserWord(m,10001,2)==-8,"AC1 unprivileged sibling operations remain denied");
        assertion(serviceUserWord(m,10003,1)==80*27 && serviceUserWord(m,10004,1)>=0 && serviceUserWord(m,10005,1)>0,"AC1 actual80 cache pages/2160 words VFS write/flush/close complete");
        std::ofstream(base/"pressure_geometry.json")<<"{\"cache_page_words\":27,\"requested_pages\":80,\"expected_words\":2160,\"observed_words\":"<<serviceUserWord(m,10003,1)<<",\"active_frames\":"<<frames<<",\"flushes\":"<<flushes<<",\"evictions\":"<<evictions<<",\"results\":["<<serviceUserWord(m,10000,1)<<','<<serviceUserWord(m,10001,1)<<','<<serviceUserWord(m,10000,2)<<','<<serviceUserWord(m,10001,2)<<"]}\n";
        return true;
    });
    for(const std::string mode:{"ordinary_exit","forced_exit","explicit_cancel","signal_interrupt","ordinary_wakeup","timeout"}) {
        const bool forced=mode=="forced_exit";
        std::string supervisor="fn main() -> t40 {\n"+path_source(10300,"/tmp/reuse")+R"TRIT(
            os_ipc_recv_blocking(2,10400,-1); os_ipc_recv_blocking(2,10401,-1);
            os_sleep_ms(20);
        )TRIT";
        if(mode=="explicit_cancel")supervisor+="os_resume(203); os_sleep_ms(2);\n";
        if(mode=="signal_interrupt")supervisor+="os_kill(203,1); os_sleep_ms(2);\n";
        if(mode=="ordinary_wakeup")supervisor+="os_ipc_send(0,-73); os_sleep_ms(2);\n";
        supervisor+=forced?"os_kill(202,2);\n":"os_ipc_send(1,9);\n";
        supervisor+=R"TRIT(
            let owner: t40=os_waitpid(202); unsafe { store(10000,owner); }
            let receiver: t40=os_waitpid(203); unsafe { store(10001,receiver); }
            let reap_owner: t40=os_waitpid(202); let reap_receiver: t40=os_waitpid(203);
            unsafe { store(10002,reap_owner); store(10003,reap_receiver); store(10091,1); }
            let reused: t40=os_spawn_app(10300,2,204); unsafe { store(10004,reused); }
            let reused_status: t40=os_waitpid(204); unsafe { store(10005,reused_status); }
            os_waitpid(204);
            unsafe { store(10090,777); store(10405,0); }
            os_futex_wait(10405,0,-1); return 0;
        })TRIT";
        std::string owner="fn main() -> t40 {\n"+path_source(10300,"/tmp/owner-lifetime")+R"TRIT(
            let fd: t40=os_open(10300,3);
            unsafe { store(11000,61); store(11001,-62); store(11002,63); }
            os_write(fd,11000,3);
            let win: t40=os_window_create(10,10,20,30);
            let buf: t40=os_window_get_buffer(win);
            os_window_fill_rgb(buf,20,30,31,71,113); os_window_present(win);
            unsafe { store(10000,win); store(10001,fd); store(10002,buf); store(10090,1); }
            os_ipc_send(2,1);
        )TRIT";
        owner+=forced?"while win >= 0 { os_window_wait_event(win,10410,1000); }\n":"os_ipc_recv_blocking(1,10400,-1);\n";
        owner+="os_exit(41); return 0; }\n";
        const std::string receiver="fn main() -> t40 { os_ipc_send(2,2); let result: t40=os_ipc_recv_blocking(0,10400,"+std::string(mode=="timeout"?"1":"-1")+"); unsafe { store(10000,result); store(10001,load(10400)); store(10090,1); } os_ipc_send(2,3); os_exit(33); return 0; }\n";
        const std::string reuse=R"TRIT(fn main() -> t40 {
            let stale_fd: t40=os_close(0); let stale_win: t40=os_window_present(0);
            let stale_ipc: t40=os_ipc_recv(0,10400); let denied: t40=os_kill(201,8);
            unsafe { store(10000,stale_fd); store(10001,stale_win); store(10002,stale_ipc); store(10003,denied); store(10004,load(11000)); store(10090,1); }
            os_exit(44); return 0;
        })TRIT";
        bool input_sent=false,capture_seen=false,receiver_blocked=false,owner_wait_seen=false,cleanup_seen=false,reuse_seen=false,idempotent_seen=false,guarded_cleanup_pending=false;
        long long receiver_result=-999,receiver_payload=-999,win_id=-1;
        long long receiver_marker_step=-1,owner_cleanup_step=-1,signal_wait_epc=-1;
        std::array<long long,4> reuse_results{{-999,-999,-999,-999}};
        std::ofstream resources(base/(mode+"_resources.jsonl"));
        auto no_slot_links=[&](const vm::VMState& m,int slot){
            for(int i=0;i<128;++i)if(serviceKernelWord(m,138000+i*3+1)==slot || serviceKernelWord(m,678000+i)==slot)return false;
            for(int i=0;i<243;++i)if(serviceKernelWord(m,677000+i)==slot)return false;
            for(int i=0;i<100;++i)if(serviceKernelWord(m,138500+i)==slot || serviceKernelWord(m,677243+i)==slot)return false;
            return true;
        };
        const std::string setup=R"TRIT(fn prerequisite_setup() -> t40 {
            ipc_open_owned(0,202,0,0); ipc_open_owned(1,201,0,0);
            ipc_open_owned(2,201,0,0); return 1;
        })TRIT";
        execute(mode,{{"supervisor",supervisor,1,201,63,1},{"owner",owner,2,202,63,201},{"receiver",receiver,3,203,34,201},{"reuse",reuse,0,204,1,201}},setup,[&](vm::VMState& m,const vm::VMExecutionRecord& rec,long long steps) {
            if(rec.attempted && rec.pc==wake_signal_label){const auto slot=vm::ops::toLong(m.regfile.read(13)),ctx=serviceKernelWord(m,390000+static_cast<int>(slot)*9+5);resources<<"{\"event\":\"signal_wakeup_entry\",\"step\":"<<steps<<",\"slot\":"<<slot<<",\"kind\":"<<serviceKernelWord(m,138600+static_cast<int>(slot))<<",\"channel\":"<<serviceKernelWord(m,391700+static_cast<int>(slot))<<",\"saved_epc\":"<<serviceKernelWord(m,static_cast<int>(ctx))<<",\"cause\":"<<m.cause<<"}\n";if(slot==3 && serviceKernelWord(m,138603)==3)signal_wait_epc=serviceKernelWord(m,static_cast<int>(ctx));}
            const auto pid2=serviceKernelWord(m,390018),state3=serviceKernelWord(m,390027+2);
            if(serviceKernelWord(m,390027)==203 && state3==3 && serviceKernelWord(m,391703)==101000)receiver_blocked=true;
            if(pid2==202 && serviceKernelWord(m,138602)==4 && serviceKernelWord(m,138702)>0)owner_wait_seen=true;
            if(pid2==202 && serviceUserWord(m,10090,2)==1){win_id=serviceUserWord(m,10000,2);if(!input_sent){assertion(win_id==0 && serviceUserWord(m,10001,2)==0 && serviceUserWord(m,10002,2)>0,"AC3 "+mode+" actual first owner window0/descriptor0/surface created");resources<<"{\"event\":\"owner_created\",\"window\":"<<win_id<<",\"fd\":"<<serviceUserWord(m,10001,2)<<",\"surface_pointer\":"<<serviceUserWord(m,10002,2)<<"}\n";m.mouse_x=15;m.mouse_y=30;m.mouse_btn=1;input_sent=true;resources<<"{\"event\":\"replay_pointer_press\",\"step\":"<<steps<<",\"x\":15,\"y\":30,\"button\":1}\n";}}
            if(win_id>=0 && serviceKernelWord(m,3053)==win_id)capture_seen=true;
            if(serviceKernelWord(m,390027)==203 && serviceUserWord(m,10090,3)==1){receiver_result=serviceUserWord(m,10000,3);receiver_payload=serviceUserWord(m,10001,3);if(receiver_marker_step<0){receiver_marker_step=steps;resources<<"{\"event\":\"receiver_return_marker\",\"step\":"<<steps<<",\"result\":"<<receiver_result<<",\"payload\":"<<receiver_payload<<"}\n";}}
            if(pid2==202 && serviceKernelWord(m,130302)==1 && !cleanup_seen){cleanup_seen=true;owner_cleanup_step=steps;
                bool clean=true;for(int i=0;i<2048;++i)if(serviceKernelWord(m,290000+i*8)==202 && serviceKernelWord(m,290000+i*8+5)>0)clean=false;
                for(int i=0;i<100;++i)if(serviceKernelWord(m,132000+i*16)>0 && serviceKernelWord(m,132000+i*16+2)==202)clean=false;
                for(int i=0;i<128;++i)if(serviceKernelWord(m,130400+i)==202)clean=false;
                for(int i=0;i<512*5;++i)if(serviceKernelWord(m,393000+2*512*5+i)!=0)clean=false;
                for(int i=0;i<1024;++i)if(serviceKernelWord(m,19300+2*1024+i)!=0)clean=false;
                clean &= serviceKernelWord(m,391702)==0 && serviceKernelWord(m,138602)==0 && serviceKernelWord(m,138702)==-1 && serviceKernelWord(m,138502)==-1;
                clean &= serviceKernelWord(m,138802)==0 && serviceKernelWord(m,138902)==0 && serviceKernelWord(m,139002)==0 && serviceKernelWord(m,677245)==-1;
                assertion(clean,"AC3 "+mode+" owner descriptors/windows/IPC/waits/software and hardware mappings released");
                assertion(no_slot_links(m,2),"AC3 "+mode+" no old slot in queue heads/tails/links or timer buckets");
                assertion(serviceKernelWord(m,3030)!=win_id && serviceKernelWord(m,3053)!=win_id,"AC3 "+mode+" focus/capture released immediately on cleanup");
                resources<<"{\"event\":\"owner_cleanup\",\"step\":"<<steps<<",\"clean\":"<<(clean?"true":"false")<<",\"focus\":"<<serviceKernelWord(m,3030)<<",\"capture\":"<<serviceKernelWord(m,3053)<<",\"old_window\":"<<win_id<<"}\n";
            }
            // Reaping naturally calls guarded cleanup a second time after exit cleanup.
            if(pid2==202 && serviceKernelWord(m,130302)==1 && rec.attempted && rec.pc==release_label && vm::ops::toLong(m.regfile.read(13))==2){
                resources<<"{\"event\":\"guarded_second_cleanup_entry\",\"step\":"<<steps<<",\"slot\":2,\"already_cleaned\":1}\n";
                guarded_cleanup_pending=true;
            }
            if(guarded_cleanup_pending && rec.attempted && rec.pc==release_return_label){const auto result=vm::ops::toLong(m.regfile.read(13));assertion(result==0 && no_slot_links(m,2),"AC3 "+mode+" guarded second cleanup actually returns0 with no queue/timer links");resources<<"{\"event\":\"guarded_second_cleanup_return\",\"step\":"<<steps<<",\"result\":"<<result<<"}\n";idempotent_seen=result==0;guarded_cleanup_pending=false;}
            if(pid2==204 && serviceUserWord(m,10090,2)==1 && !reuse_seen){reuse_seen=true;for(int i=0;i<4;++i)reuse_results[i]=serviceUserWord(m,10000+i,2);
                assertion(serviceKernelWord(m,caps_base+2)==1,"AC3 "+mode+" reused slot receives registered new mask1");
                assertion(serviceKernelWord(m,391702)==0 && serviceKernelWord(m,138602)==0 && serviceKernelWord(m,138702)==-1 && serviceKernelWord(m,138502)==-1,"AC3 "+mode+" reused slot has empty wait/deadline links");
                assertion(no_slot_links(m,2),"AC3 "+mode+" reused slot has no inherited queue/timer membership");
                assertion(serviceUserWord(m,10004,2)==0 && serviceKernelWord(m,19300+2*1024+512+64)==0,"AC3 "+mode+" new lifetime has cleared old scratch and no old window hardware mapping");
                assertion(serviceKernelWord(m,130202)==0 && serviceKernelWord(m,130102)==0 && serviceKernelWord(m,130302)==0,"AC3 "+mode+" new lifetime has no old signals/exit status/cleanup flag");
                resources<<"{\"event\":\"new_lifetime\",\"pid\":204,\"slot\":2,\"caps\":"<<serviceKernelWord(m,caps_base+2)<<",\"results\":["<<reuse_results[0]<<','<<reuse_results[1]<<','<<reuse_results[2]<<','<<reuse_results[3]<<"]}\n";
            }
            if(serviceUserWord(m,10090,1)!=777)return false;
            assertion(receiver_blocked,"AC2 "+mode+" concrete blocked receiver recorded");
            const long long expected=mode=="timeout"?0:mode=="ordinary_wakeup"?1:-12;
            assertion(receiver_result==expected,"AC2 "+mode+" SDK result expected"+std::to_string(expected)+" observed"+std::to_string(receiver_result));
            const std::array<long long,3> expected_abi=mode=="timeout"?std::array<long long,3>{{0,0,-10}}:mode=="ordinary_wakeup"?std::array<long long,3>{{1,1,0}}:std::array<long long,3>{{-1,0,-12}};
            assertion(ipc_returns.count(203)>0 && ipc_returns[203]==expected_abi,"AC2 "+mode+" matched syscall54 raw ABI tuple");
            assertion(ipc_completion_counts[203]==1,"AC2 "+mode+" one completed receiver syscall54");
            if(mode!="ordinary_exit" && mode!="forced_exit")assertion(receiver_marker_step>=0 && receiver_marker_step<owner_cleanup_step && ipc_return_steps[203]<owner_cleanup_step,"AC2 "+mode+" actual syscall and SDK return precede owner cleanup");
            if(mode=="explicit_cancel" || mode=="signal_interrupt")assertion(signal_wait_epc>=0 && ipc_return_pcs[203]==signal_wait_epc+1,"AC2 "+mode+" real registered wait saved EPC advances exactly once");
            if(mode=="ordinary_wakeup")assertion(receiver_payload==-73,"AC2 negative IPC payload preserved");
            assertion(serviceUserWord(m,10000,1)==(forced?2:41) && serviceUserWord(m,10001,1)==33 && serviceUserWord(m,10005,1)==44,"AC2 "+mode+" ordinary/forced and reused waitpid statuses");
            assertion(capture_seen && cleanup_seen && reuse_seen,"AC3 "+mode+" real pointer capture/cleanup/reuse exercised");
            if(forced)assertion(owner_wait_seen,"AC3 forced owner window wait with deadline exercised");
            assertion(reuse_results[0]==-1 && reuse_results[1]==-1 && reuse_results[2]==-8 && reuse_results[3]==-8,"AC3 "+mode+" stale descriptors/windows/IPC/capability rejected (close SDK returns status)");
            assertion(idempotent_seen && serviceKernelWord(m,caps_base+2)==0 && serviceKernelWord(m,390018)==0,"AC3 "+mode+" repeated cleanup/reap leaves free slot and zero capability");
            resources<<"{\"event\":\"complete\",\"receiver_result\":"<<receiver_result<<",\"receiver_payload\":"<<receiver_payload<<",\"waitpid\":["<<serviceUserWord(m,10000,1)<<','<<serviceUserWord(m,10001,1)<<','<<serviceUserWord(m,10005,1)<<"]}\n";
            return true;
        });
    }
    assertions.close();
    std::ofstream report(base/"report.json");report<<"{\"status\":\""<<(failures.empty()?"PASS":"FAIL")<<"\",\"scope\":\"focused\",\"run_directory\":\"run_"<<run_number<<"\",\"capability_base\":"<<caps_base<<",\"failures\":[";for(size_t i=0;i<failures.size();++i){if(i)report<<',';report<<'"'<<jsonEscape(failures[i])<<'"';}report<<"],\"later_stages\":\"pending\",\"external_checks\":\"doctor/contract-check/SDK/clock/desktop reported separately\"}\n";report.close();
    std::ofstream(root/"report.json")<<readTextFile(base/"report.json");
    std::cout<<"compositor prerequisites artifact: "<<base.string()<<'\n';
    return failures.empty();
}


struct TransportActor {
    std::string name, source;
    int slot=0, pid=0, role=0, ns=0;
    bool deferred=false;
    bool late_role=false;
    std::string install_path,launch_path;
    bool numeric_native_fixture=false;
    std::vector<std::pair<std::string,std::string>> memory_native_extensions;
};

bool transportWriteArtifact(const std::filesystem::path& path,const std::string& contents) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<contents;out.flush();out.close();
    return require(!out.fail(),"transport artifact written "+path.string());
}

long long transportSourceConstant(const std::string& name) {
    const auto source=readTextFile("kernel.trit");
    std::smatch match;
    if(!std::regex_search(source,match,std::regex("const "+name+"\\s*:\\s*t40\\s*=\\s*([0-9]+)\\s*;")))return -1;
    return std::stoll(match[1].str());
}

long long transportProcessGeneration(const sandbox::vm::VMState& machine,int slot) {
    static const auto offset=transportSourceConstant("GFX_PROC_GENERATION_BASE");
    return offset<0?0:serviceKernelWord(machine,14725800+static_cast<int>(offset)+slot);
}

int transportRuntimeSlot(const sandbox::vm::VMState& machine,int pid) {
    for(int slot=0;slot<100;++slot)if(serviceKernelWord(machine,390000+slot*9)==pid && serviceKernelWord(machine,390000+slot*9+2)>0)return slot;
    return -1;
}

void transportExactWord(std::ostream& out,const sandbox::vm::TernaryValue& word) {
    out<<"{\"mode\":"<<static_cast<int>(word.mode)<<",\"bits_lo\":\""<<word.bits.lo<<"\",\"bits_hi\":\""<<word.bits.hi<<"\"}";
}

sandbox::vm::TernaryValue transportUserValue(const sandbox::vm::VMState& machine,int address,int slot) {
    const auto context=serviceKernelWord(machine,390000+slot*9+5);
    const auto pt=serviceKernelWord(machine,static_cast<int>(context)+4);
    const auto pte=serviceKernelWord(machine,static_cast<int>(pt)+address/sandbox::vm::MMU_PAGE_WORDS);
    const int physical=static_cast<int>(pte/19683*sandbox::vm::MMU_PAGE_WORDS)+address%sandbox::vm::MMU_PAGE_WORDS;
    return machine.dmem.load(physical).first;
}

// Real executable/VFS/trap actor runner. Trusted role setup is deliberately
// separate from each compiled caller and never supplies observed return data.
bool runTransportActors(const std::filesystem::path& dir,
                        const std::vector<TransportActor>& actors,
                        const std::string& kernel_extension,
                        const std::function<bool(sandbox::vm::VMState&,const sandbox::vm::VMExecutionRecord&,long long)>& observe,
                        const std::string& boot_vector_seed="",
                        bool observe_user_loads=false,
                        const std::function<void(const sandbox::vm::VMState&)>& before_step={},
                        bool observe_native_numeric=false,
                        bool observe_user_memory=false,
                        const std::function<void(sandbox::vm::VMState&,const std::map<std::string,int>&)>& metadata_hook={},
                        const std::function<bool(const sandbox::vm::VMState&,const sandbox::vm::VMExecutionRecord&)>& observation_boundary={},
                        const std::function<bool(const std::string&,int,const sandbox::vm::ExecutableImageHeaderV3&,const std::map<std::string,int>&)>& actor_catalog={},
                        long long max_steps=160000000,
                        const std::string& fixture_dispatch_wrapper="") {
    using namespace sandbox;
    std::filesystem::create_directories(dir);
    compiler::CompilerOptions opts;opts.enable_vector_abi=true;opts.enable_vector_spilling=true;
    os::NativeVfsImageBuilder fs(32768);
    if(!require(fs.installBaseLayout().ok(),"transport rootfs"))return false;
    auto production=readTextFile("kernel.trit");
    if(!fixture_dispatch_wrapper.empty())production=serviceRenameDefinition(production,"kernel_syscall_dispatch","word_class_base_dispatch");
    const bool binding_present=production.find("fn gfx_policy_bind_fixture(")!=std::string::npos;
    if(kernel_extension.find("fn transport_tick_fault(")!=std::string::npos)production=serviceRenameDefinition(production,"kernel_timer_tick","transport_base_timer_tick");
    for(size_t i=0;i<actors.size();++i) {
        const auto& actor=actors[i];if(!transportWriteArtifact(dir/(actor.name+".trit"),actor.source))return false;
        const auto app=compiler::compileSource(actor.name+".trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+actor.source,opts);
        {std::ostringstream out;for(const auto& d:app.diagnostics)out<<d.format()<<'\n';if(!transportWriteArtifact(dir/(actor.name+".compile.txt"),out.str()))return false;}
        if(!require(app.success,"transport compile "+actor.name))return false;
        compiler::LinkOptions lo;lo.stack_hint_words=2048;lo.dead_strip_functions=true;lo.standalone_halt_on_exit=false;lo.enable_vector_abi=true;lo.enable_vector_spilling=true;
        const auto linked=compiler::linkModules({app.object},lo);
        {std::ostringstream out;for(const auto& d:linked.diagnostics)out<<d.format()<<'\n';if(!transportWriteArtifact(dir/(actor.name+".link.txt"),out.str()))return false;}
        if(!transportWriteArtifact(dir/(actor.name+".linked.tasm"),linked.assembly))return false;
        if(!require(linked.success,"transport link "+actor.name))return false;
        const auto installed_path=actor.install_path.empty()?"/tmp/"+actor.name:actor.install_path;
        auto installed_program=linked.assembled.program;
        auto installed_header=linked.executable_header_v3;
        if(actor.numeric_native_fixture || !actor.memory_native_extensions.empty()) {
            // Local native E2E extension only; the production compiler and ISA
            // remain unchanged. Keep the compiler's assembly above verbatim.
            std::string fixture=linked.assembly;
            fixture.insert(fixture.find('\n')+1,actor.numeric_native_fixture?".require accumulator_ai\n":".require vector wide_t50\n");
            auto inject=[&](const std::string& label,const std::string& code){const auto at=fixture.find(label+":");if(at==std::string::npos)return false;fixture.insert(at+label.size()+1,"\n"+code);return true;};
            if(!actor.memory_native_extensions.empty()) {
                for(const auto& extension:actor.memory_native_extensions)if(!require(inject(extension.first,extension.second),"native memory fixture label "+extension.first))return false;
                std::ostringstream provenance;provenance<<"{\"scope\":\"test-only compiled guest native numeric memory extension\",\"state_injected\":false,\"labels\":[";bool first=true;for(const auto& extension:actor.memory_native_extensions){if(!first)provenance<<',';first=false;provenance<<'"'<<extension.first<<'"';}provenance<<"]}\n";if(!transportWriteArtifact(dir/(actor.name+".extension.json"),provenance.str()))return false;
            } else if(actor.pid==202) {
                if(!require(inject("numeric_overflow_entry","    ; FIXTURE native finite accumulator sequence, input1 and multiplier3.\n    aload.t40 r19\n    mov r20, 3\n    mov r21, 1200\n    mov r22, 1\nnumeric_overflow_loop:\n    amul.t40 r20\n    sub.t40 r21, r21, r22\n    brp r21, numeric_overflow_loop\n    astore.t40 r19\n") && inject("numeric_illegal_entry","numeric_illegal_instruction:\n    ; FIXTURE wire patch below replaces source vector0 with index8.\n    vmac.t1 v0, v1\n"),"native numeric fixture labels"))return false;
            } else if(!require(inject("numeric_seed_entry","    ; FIXTURE real User vector seed7, not host state injection.\n    vbcast.t5 v1, r19\n"),"native numeric survivor seed label"))return false;
            const std::regex envelope(R"(\.execheader3[^\r\n]*)");
            auto final_source=[&](int words,std::uint64_t features){const auto numeric=vm::executableFeatureWordNumeric(features,architecture::v3::FEATURE_V3_LAST);return std::regex_replace(fixture,envelope,".execheader3 "+std::to_string(installed_header.entry_pc)+", "+std::to_string(words)+", "+std::to_string(installed_header.data_words)+", "+std::to_string(installed_header.stack_words)+", "+std::to_string(numeric)+", 2, "+std::to_string(installed_header.flags));};
            auto final_assembly=final_source(1,installed_header.required_features);
            auto native=vm::assembler::assembleV2(final_assembly);
            final_assembly=final_source(static_cast<int>(native.program.size()),native.required_features);
            native=vm::assembler::assembleV2(final_assembly);
            std::ostringstream diagnostic;for(const auto& e:native.errors)diagnostic<<e.format()<<'\n';
            if(!transportWriteArtifact(dir/(actor.name+".fixture.tasm"),final_assembly) || !transportWriteArtifact(dir/(actor.name+".fixture.assembly.txt"),diagnostic.str()) || !require(native.success && native.executable_headers_v3.size()==1,"native numeric fixture assembly"))return false;
            installed_program=native.program;installed_header=native.executable_headers_v3.begin()->second;
            if(actor.numeric_native_fixture && actor.pid==202) {
                const int pc=native.labels.at("numeric_illegal_instruction");
                const auto original=installed_program.at(static_cast<size_t>(pc));
                const auto patched=isa::VersionedInstructionCodec::encodeR(isa::Opcode::VMAC,0,8,1,isa::FUNC_T1,isa::IsaEncodingVersion::V2);
                installed_program.at(static_cast<size_t>(pc))=patched;
                std::ostringstream patch;patch<<"{\"scope\":\"test-only local guest instruction operand\",\"isa_version\":2,\"pc\":"<<pc<<",\"opcode\":\"VMAC\",\"width\":\"T1\",\"source_vector_before\":0,\"source_vector_after\":8,\"valid_vector_range\":[0,7],\"original_word\":"<<original.bits<<",\"installed_word\":"<<patched.bits<<",\"fault_or_state_injected\":false}\n";
                if(!transportWriteArtifact(dir/(actor.name+".instruction_patch.json"),patch.str()))return false;
            }
        }
        if(actor_catalog) {
            if(!require(!actor.numeric_native_fixture&&actor.memory_native_extensions.empty(),"catalog drawing actors use unchanged linked assembly"))return false;
            if(!transportWriteArtifact(dir/(actor.name+".final.tasm"),linked.assembly))return false;
            if(!actor_catalog(actor.name,actor.pid,installed_header,linked.assembled.labels))return false;
        }
        if(!require(fs.addExecutableImage(installed_path,installed_program,installed_header,256+static_cast<int>(i)*64).ok(),"transport VFS install "+actor.name))return false;
    }
    const std::string policy=R"TRIT(
// TRUSTED FIXTURE POLICY: only bootstrap registration binds fixture roles.
// No caller return, identity, generation or admission result is injected.
fn transport_register_fixture(namespace_id: t40, path_addr: t40) -> t40 {
    var found: t40=app_find(namespace_id,path_addr);
    if found >= 0 { return found; }
    return app_register(namespace_id,path_addr,63,0,1,12000);
}
fn transport_fixture_setup() -> t40 {
    var p: t40=1; while p < 100 { if process_state(p)>0 { if kload(process_addr(p)+PROC_PID)!=201 { process_set_parent(p,201); } } p=p+1; }
    ipc_open_owned(0,201,0,0); ipc_open_owned(1,201,0,0);
    ipc_open_owned(19,412,0,0);
    var i: t40=0;
    while i < 9 { ipc_open_owned(10+i,201,0,0); ipc_open_owned(30+i,201,0,0); i=i+1; }
    return 1;
}
)TRIT"+kernel_extension+fixture_dispatch_wrapper;
    if(!transportWriteArtifact(dir/"bootstrap_policy.trit",policy))return false;
    const auto kernel=compiler::compileSource("transport_kernel.trit",readTextFile("architecture_contract.trit")+"\n"+production+"\n"+policy);
    {std::ostringstream out;for(const auto& d:kernel.diagnostics)out<<d.format()<<'\n';if(!transportWriteArtifact(dir/"kernel.compile.txt",out.str()))return false;}
    if(!require(kernel.success,"transport kernel compile"))return false;
    std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n mov r13, 0\n mov r14, 3\n call process_set_state\n mov r1, native_trap_entry\n csrw tvec, r1\n";
    if(std::any_of(actors.begin(),actors.end(),[](const auto& actor){return actor.ns==1;}))boot+=" call transport_namespace_fixture\n";
    for(size_t index=0;index<actors.size();++index) {
        const auto& actor=actors[index];const auto path=actor.launch_path.empty()?"/tmp/"+actor.name:actor.launch_path;
        for(size_t i=0;i<=path.size();++i)boot+=" mov r1, "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"\n mov r2, "+std::to_string(10020+i)+"\n store r1, r2, 0\n";
        boot+=" mov r13, "+std::to_string(actor.ns)+"\n mov r14, 10020\n call transport_register_fixture\n mov r2, "+std::to_string(14200+index)+"\n store r13, r2, 0\n";
        if(actor.role>0 && binding_present && !actor.late_role)boot+=" mov r14, "+std::to_string(actor.role)+"\n call gfx_policy_bind_fixture\n mov r2, "+std::to_string(14400+index)+"\n store r13, r2, 0\n";
        if(actor.slot>0 && !actor.deferred) {
            if(actor.ns==0)boot+=" mov r13, 1\n mov r14, 10020\n mov r15, "+std::to_string(actor.slot)+"\n mov r16, "+std::to_string(actor.pid)+"\n call app_spawn\n";
            else boot+=" mov r13, "+std::to_string(actor.slot)+"\n mov r14, "+std::to_string(actor.pid)+"\n mov r15, "+std::to_string(actor.ns)+"\n mov r16, 1\n mov r17, 0\n mov r18, 0\n call process_create\n mov r13, "+std::to_string(actor.pid)+"\n mov r14, 10020\n mov r15, 0\n call app_launch\n";
            boot+=" mov r2, "+std::to_string(14300+index)+"\n store r13, r2, 0\n";
        }
    }
    boot+=" call transport_fixture_setup\n mov r13, 202\n call scheduler_switch_to_pid\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
    if(!boot_vector_seed.empty())boot.insert(boot.rfind(" eret\n"),boot_vector_seed);
    boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
    auto fw=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
    auto source=[&](int count){return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(count)+", 0, 27, "+std::to_string(fw)+", 2, 0\n";};
    auto assembled=vm::assembler::assembleV2(source(1));fw=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
    assembled=vm::assembler::assembleV2(source(1));if(assembled.success)assembled=vm::assembler::assembleV2(source(static_cast<int>(assembled.program.size())));
    if(!transportWriteArtifact(dir/"boot.tasm",source(static_cast<int>(assembled.program.size()))))return false;
    {std::ostringstream out;for(const auto& e:assembled.errors)out<<e.format()<<'\n';if(!transportWriteArtifact(dir/"assembly.txt",out.str()))return false;}
    if(!require(assembled.success,"transport boot assembly"))return false;
    host::TosImageManifest manifest;const auto image=host::bootImageFromAssembly(assembled,manifest,fs.image());std::string error;
    if(!require(host::writeBootImageFile((dir/"boot.tboot").string(),image,&error),"transport boot image "+error))return false;
    if(metadata_hook){std::ostringstream labels;labels<<"{";bool comma=false;for(const auto& [name,pc]:assembled.labels){if(comma)labels<<',';comma=true;labels<<"\""<<jsonEscape(name)<<"\":"<<pc;}labels<<"}\n";if(!transportWriteArtifact(dir/"final_labels.json",labels.str()))return false;}
    auto machine=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    if(!require(host::loadBootImageIntoVm(*machine,image,"",&error),"transport boot load "+error))return false;
    std::ofstream raw(dir/"raw_returns.jsonl"),lifecycle(dir/"lifecycle.jsonl"),records(dir/"records.jsonl");
    if(!require(raw.good() && lifecycle.good() && records.good(),"transport trace writers opened"))return false;
    struct Pending {int service=0,pc=0;long long a0=0,a1=0,a2=0,a3=0;};
    using Lifetime=std::tuple<int,int,long long>;
    std::map<Lifetime,Pending> pending;std::map<int,std::tuple<long long,int,long long>> states;
    bool completed=false,registered=false;long long steps=0;
    for(;steps<max_steps && machine->cycle_count<2000000000;++steps) {
        if(before_step)before_step(*machine);
        if(metadata_hook)metadata_hook(*machine,assembled.labels);
        isa::VectorContextInstruction observed_context;std::uint64_t observed_context_bits=0;
        if(observe_native_numeric && machine->privilege==vm::PrivilegeMode::Kernel){const auto fetched=machine->imem.fetch(machine->pc);if(fetched.second==vm::MemFaultCode::OK){observed_context=isa::decodeVectorContext(fetched.first);observed_context_bits=fetched.first.bits;}}
        vm::VMExecutionRecord rec;vm::step(*machine,&rec);
        // The VM's private VCTX path returns before filling its instruction
        // record. Enrich this local diagnostic observation from the actual
        // pre-step Kernel word; execution/status/PC/trap remain VM results.
        if(observed_context.valid && rec.attempted && !rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::Kernel && rec.physical_pc>=0 && rec.raw_bits==observed_context_bits){
            records<<"{\"event\":\"actual_private_VCTX_decode\",\"step\":"<<steps<<",\"pc\":"<<rec.pc<<",\"physical_pc\":"<<rec.physical_pc<<",\"raw_bits\":"<<rec.raw_bits<<",\"original_has_instruction\":false,\"decoder\":\"existing ISA2 private VCTX; test-only local annotation\",\"opcode\":"<<static_cast<int>(observed_context.opcode)<<",\"actual_final_pc\":"<<rec.final_pc<<",\"actual_trap_observed\":"<<(rec.trap_observed?"true":"false")<<"}\n";
            rec.has_instruction=true;rec.instruction.opcode=observed_context.opcode;rec.instruction.rs1=observed_context.context_register;
        }
        if(!registered && rec.before_privilege==vm::PrivilegeMode::User) {
            registered=true;std::ofstream registration(dir/"registration.jsonl");bool valid=true;
            for(size_t i=0;i<actors.size();++i){const auto& actor=actors[i];const auto id=serviceKernelWord(*machine,14200+static_cast<int>(i)),launched=serviceKernelWord(*machine,14300+static_cast<int>(i));const auto path=actor.launch_path.empty()?"/tmp/"+actor.name:actor.launch_path;registration<<"{\"path\":\""<<path<<"\",\"namespace\":"<<actor.ns<<",\"app_id\":"<<id<<",\"launch_return\":"<<launched<<",\"role_policy\":"<<actor.role<<",\"policy_present\":"<<(binding_present?"true":"false")<<"}\n";valid &= id>=0 && (actor.slot==0 || actor.deferred || (actor.ns==0?launched==actor.pid:launched>=20));}
            registration.flush();valid &= require(registration.good(),"transport registration artifact written");
            if(!require(valid,"transport successful explicit registrations and real launches"))break;
        }
        const int pid=static_cast<int>(serviceKernelWord(*machine,3020));
        const int runtime_slot=transportRuntimeSlot(*machine,pid);
        const long long runtime_generation=runtime_slot>=0?transportProcessGeneration(*machine,runtime_slot):0;
        const Lifetime runtime_identity{pid,runtime_slot,runtime_generation};
        bool boundary=steps%10000==0;
        if(observation_boundary && observation_boundary(*machine,rec))boundary=true;
        if(observe_user_memory && rec.attempted && rec.has_instruction && (rec.before_privilege==vm::PrivilegeMode::User || rec.instruction.opcode==isa::Opcode::ERET || rec.instruction.opcode==isa::Opcode::TSCAN || rec.instruction.opcode==isa::Opcode::VGATHER || rec.instruction.opcode==isa::Opcode::VSCATTER))boundary=true;
        if(observe_native_numeric && rec.attempted && rec.has_instruction && (rec.instruction.opcode==isa::Opcode::ALOAD || rec.instruction.opcode==isa::Opcode::AMUL || rec.instruction.opcode==isa::Opcode::ASTORE || rec.instruction.opcode==isa::Opcode::VMAC || rec.instruction.opcode==isa::Opcode::VCTXSTORE || rec.instruction.opcode==isa::Opcode::VCTXLOAD || rec.trap_observed || rec.instruction.opcode==isa::Opcode::HALT))boundary=true;
        if(observe_user_loads && rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User && (rec.instruction.opcode==isa::Opcode::LOAD || rec.trap_observed))boundary=true;
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User && rec.instruction.opcode==isa::Opcode::SYSCALL) {
            pending[runtime_identity]={rec.syscall_id,rec.pc,rec.syscall_arg0,rec.syscall_arg1,rec.syscall_arg2,rec.syscall_arg3};boundary=true;
            raw<<"{\"kind\":\"entry\",\"step\":"<<steps<<",\"pid\":"<<pid<<",\"slot\":"<<runtime_slot<<",\"process_generation\":"<<runtime_generation<<",\"service\":"<<rec.syscall_id<<",\"pc\":"<<rec.pc<<",\"args_integer_summary\":["<<rec.syscall_arg0<<','<<rec.syscall_arg1<<','<<rec.syscall_arg2<<','<<rec.syscall_arg3<<"],\"args_exact\":[";for(int r=13;r<=16;++r){if(r>13)raw<<',';transportExactWord(raw,machine->regfile.read(r));}raw<<"]}\n";
        }
        if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET) {
            const auto it=pending.find(runtime_identity);
            if(it!=pending.end() && machine->pc==it->second.pc+1) {
                const auto request=it->second;const auto status=vm::ops::toLong(machine->regfile.read(13)),payload=vm::ops::toLong(machine->regfile.read(14)),detail=vm::ops::toLong(machine->regfile.read(15));
                raw<<"{\"kind\":\"matched_return\",\"step\":"<<steps<<",\"pid\":"<<pid<<",\"slot\":"<<runtime_slot<<",\"process_generation\":"<<runtime_generation<<",\"service\":"<<request.service<<",\"saved_epc\":"<<request.pc<<",\"return_epc\":"<<machine->pc<<",\"tuple\":["<<status<<','<<payload<<','<<detail<<"],\"tuple_exact\":[";for(int r=13;r<=15;++r){if(r>13)raw<<',';transportExactWord(raw,machine->regfile.read(r));}raw<<"]}\n";
                int span=0,address=0;
                if(request.service==61 && status==1){span=8;address=static_cast<int>(request.a0);}
                if(request.service==62 && status==1){span=8;address=static_cast<int>(request.a1);}
                if(request.service==64 && status==1){span=16;address=static_cast<int>(request.a1);}
                if(request.service==63 && status==1){span=10;address=static_cast<int>(request.a3);}
                if(span>0){const int actor_slot=transportRuntimeSlot(*machine,pid);records<<"{\"step\":"<<steps<<",\"pid\":"<<pid<<",\"slot\":"<<actor_slot<<",\"process_generation\":"<<transportProcessGeneration(*machine,actor_slot)<<",\"service\":"<<request.service<<",\"words_integer_summary\":[";for(int i=0;i<span;++i){if(i)records<<',';records<<serviceUserWord(*machine,address+i,actor_slot);}records<<"],\"words_exact\":[";for(int i=0;i<span;++i){if(i)records<<',';transportExactWord(records,transportUserValue(*machine,address+i,actor_slot));}records<<"]}\n";}
                pending.erase(it);boundary=true;
            }
        }
        if(boundary) {
            for(int slot=1;slot<100;++slot) {
                const auto state=serviceKernelWord(*machine,390000+slot*9+2);
                const int row_pid=static_cast<int>(serviceKernelWord(*machine,390000+slot*9));
                const auto generation=transportProcessGeneration(*machine,slot);
                const auto identity_state=std::make_tuple(state,row_pid,generation);
                if(!states.count(slot) || identity_state!=states[slot]) {
                    lifecycle<<"{\"step\":"<<steps<<",\"slot\":"<<slot<<",\"pid\":"<<row_pid<<",\"process_generation\":"<<generation<<",\"before\":"<<(states.count(slot)?std::get<0>(states[slot]):-1)<<",\"after\":"<<state<<",\"kind\":"<<serviceKernelWord(*machine,138600+slot)<<",\"channel\":"<<serviceKernelWord(*machine,391700+slot)<<",\"deadline\":"<<serviceKernelWord(*machine,138700+slot)<<"}\n";
                    states[slot]=identity_state;
                }
            }
            for(auto it=pending.begin();it!=pending.end();) {
                const auto [old_pid,old_slot,old_generation]=it->first;
                if(old_slot<0 || serviceKernelWord(*machine,390000+old_slot*9)!=old_pid || serviceKernelWord(*machine,390000+old_slot*9+2)<=0 || transportProcessGeneration(*machine,old_slot)!=old_generation) {
                    raw<<"{\"kind\":\"lifetime_retired_pending\",\"step\":"<<steps<<",\"pid\":"<<old_pid<<",\"slot\":"<<old_slot<<",\"process_generation\":"<<old_generation<<",\"service\":"<<it->second.service<<",\"saved_epc\":"<<it->second.pc<<"}\n";
                    it=pending.erase(it);
                } else ++it;
            }
            if(observe(*machine,rec,steps)){completed=true;break;}
        }
        if(machine->status==vm::VMStatus::TRAPPED || machine->status==vm::VMStatus::HALTED)break;
    }
    std::ostringstream execution;execution<<"{\"completed\":"<<(completed?"true":"false")<<",\"steps\":"<<steps<<",\"cycles\":"<<machine->cycle_count<<",\"pc\":"<<machine->pc<<",\"cause\":"<<machine->cause<<",\"vm_status\":"<<static_cast<int>(machine->status)<<",\"current_pid\":"<<serviceKernelWord(*machine,3020)<<",\"current_context\":"<<serviceKernelWord(*machine,3019)<<",\"policy_present\":"<<(binding_present?"true":"false")<<",\"processes\":[";
    bool process_comma=false;for(int slot=0;slot<100;++slot)if(serviceKernelWord(*machine,390000+slot*9+2)>0){if(process_comma)execution<<',';process_comma=true;const auto ctx=serviceKernelWord(*machine,390000+slot*9+5);execution<<"{\"slot\":"<<slot<<",\"pid\":"<<serviceKernelWord(*machine,390000+slot*9)<<",\"state\":"<<serviceKernelWord(*machine,390000+slot*9+2)<<",\"context\":"<<ctx<<",\"vector_context\":"<<serviceKernelWord(*machine,390000+slot*9+8)<<",\"epc\":"<<serviceKernelWord(*machine,static_cast<int>(ctx))<<",\"sp\":"<<serviceKernelWord(*machine,static_cast<int>(ctx)+31)<<"}";}execution<<"]}\n";
    const bool execution_written=transportWriteArtifact(dir/"execution.json",execution.str());
    std::ostringstream vector_diagnostic;vector_diagnostic<<"{\"trap_exact\":";transportExactWord(vector_diagnostic,machine->trap_reg);vector_diagnostic<<",\"contexts\":[";
    bool vector_comma=false;for(int slot=0;slot<100;++slot)if(serviceKernelWord(*machine,390000+slot*9+2)>0){if(vector_comma)vector_diagnostic<<',';vector_comma=true;const auto vector=serviceKernelWord(*machine,390000+slot*9+8);vector_diagnostic<<"{\"slot\":"<<slot<<",\"words\":[";for(int word=0;word<279;++word){if(word)vector_diagnostic<<',';transportExactWord(vector_diagnostic,machine->dmem.load(static_cast<int>(vector)+word).first);}vector_diagnostic<<"]}";}vector_diagnostic<<"]}\n";
    const bool vector_written=transportWriteArtifact(dir/"vector_contexts.json",vector_diagnostic.str());
    const bool saved=require(host::detail::writeCheckpointState(dir/"terminal_state.bin",*machine,&error),"transport checkpoint "+error) && require(machine->block_device.writeSnapshotFile((dir/"disk.tdisk").string()),"transport disk snapshot");
    raw.flush();lifecycle.flush();records.flush();
    const bool traces=require(raw.good() && lifecycle.good() && records.good(),"transport trace writes completed");
    return require(completed,"transport actor scenario reached observed terminal condition") && saved && traces && execution_written && vector_written;
}

// Stage4 native drawing fixture. Runtime correctness remains unvalidated.
// Independent cases and oracles for the isolated drawing gate.
struct DrawingCase {
    std::string name,kind,algorithm;
    std::vector<int> a;
    std::vector<std::string> guest_args; // overrides only for unsupported finite input; valid dimensions are explicit fields
    bool invalid=false,reset=true,benchmark=false;
    int repetition=0;
    int destination_width=80,destination_height=60;
};
using DrawingPixels=std::vector<std::array<int,3>>;
DrawingPixels drawingPattern(int w,int h,int salt) {
    DrawingPixels p(w*h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<3;++c)
        p[y*w+x][c]=(x*17+y*11+c*53+salt)%256;
    return p;
}
long long drawingOracle(DrawingPixels& d,const DrawingPixels& source,const DrawingCase& c) {
    if(c.invalid)return -8; // validity fixed by independent case matrix, not production result
    long long count=0;auto put=[&](int x,int y,std::array<int,3> rgb){
        if(x>=0&&x<c.destination_width&&y>=0&&y<c.destination_height){d[y*c.destination_width+x]=rgb;++count;}};
    if(c.kind=="span") {
        for(int x=c.a[1];x<c.a[2];++x)put(x,c.a[0],{c.a[3],c.a[4],c.a[5]});
    } else if(c.kind=="line") {
        // Point enumerator: directed tie decisions use the same old e2.
        // It does not implement the proposed horizontal-run optimization.
        int x=c.a[0],y=c.a[1],dx=std::abs(c.a[2]-x),dy=-std::abs(c.a[3]-y);
        int sx=x<c.a[2]?1:-1,sy=y<c.a[3]?1:-1,err=dx+dy;
        for(;;){put(x,y,{c.a[4],c.a[5],c.a[6]});if(x==c.a[2]&&y==c.a[3])break;
            const int old_e2=2*err;
            if(old_e2>=dy){err+=dy;x+=sx;}if(old_e2<=dx){err+=dx;y+=sy;}}
    } else {
        const bool self=c.kind=="self";const auto snapshot=self?d:source;
        const int sw=self?c.destination_width:12,sh=self?c.destination_height:9;
        for(int j=0;j<c.a[5];++j)for(int i=0;i<c.a[4];++i){
            const int dx=c.a[0]+i,dy=c.a[1]+j,sx=c.a[2]+i,sy=c.a[3]+j;
            if(dx>=0&&dx<c.destination_width&&dy>=0&&dy<c.destination_height&&sx>=0&&sx<sw&&sy>=0&&sy<sh)
                put(dx,dy,snapshot[sy*sw+sx]);}
    }
    return count;
}
std::vector<DrawingCase> drawingCases() {
    std::vector<DrawingCase> cs;
    auto add=[&](std::string n,std::string k,std::vector<int>a,std::string alg="reference",bool reset=true){
        cs.push_back({n,k,alg,a,{},false,reset,false,0});};
    // Cases are generated in the proposal below: independent oracle data only.
    add("span_left","span",{5,-15,17,255,3,7},"reference",true);
    add("span_right","span",{7,72,95,4,200,31},"reference",true);
    add("span_top","span",{-1,0,80,1,2,3},"reference",true);
    add("span_bottom","span",{60,0,80,1,2,3},"reference",true);
    add("span_empty","span",{9,12,12,1,2,3},"reference",true);
    add("span_offscreen","span",{8,-100,-10,1,2,3},"reference",true);
    add("span_full","span",{0,0,80,255,255,255},"reference",true);
    add("copy_both_left_top","copy",{-3,-2,-4,-3,18,14},"reference",true);
    add("copy_both_right_bottom","copy",{76,57,9,7,10,8},"reference",true);
    add("copy_empty","copy",{0,0,0,0,0,9},"reference",true);
    add("copy_offscreen","copy",{-50,0,0,0,12,9},"reference",true);
    add("overlap_right","self",{12,10,8,10,30,15},"reference",true);
    add("overlap_left","self",{8,10,12,10,30,15},"reference",true);
    add("overlap_down","self",{8,13,8,10,30,20},"reference",true);
    add("overlap_up","self",{8,10,8,13,30,20},"reference",true);
    add("overlap_diagonal","self",{9,11,8,10,30,20},"reference",true);
    add("overlap_same","self",{0,0,0,0,80,60},"reference",true);
    add("line_0_forward_reference","line",{40,30,76,38,231,17,109},"reference",true);
    add("line_0_forward_hybrid","line",{40,30,76,38,231,17,109},"hybrid",true);
    add("line_0_reverse_reference","line",{76,38,40,30,231,17,109},"reference",true);
    add("line_0_reverse_hybrid","line",{76,38,40,30,231,17,109},"hybrid",true);
    add("line_1_forward_reference","line",{40,30,48,58,231,17,109},"reference",true);
    add("line_1_forward_hybrid","line",{40,30,48,58,231,17,109},"hybrid",true);
    add("line_1_reverse_reference","line",{48,58,40,30,231,17,109},"reference",true);
    add("line_1_reverse_hybrid","line",{48,58,40,30,231,17,109},"hybrid",true);
    add("line_2_forward_reference","line",{40,30,32,58,231,17,109},"reference",true);
    add("line_2_forward_hybrid","line",{40,30,32,58,231,17,109},"hybrid",true);
    add("line_2_reverse_reference","line",{32,58,40,30,231,17,109},"reference",true);
    add("line_2_reverse_hybrid","line",{32,58,40,30,231,17,109},"hybrid",true);
    add("line_3_forward_reference","line",{40,30,4,38,231,17,109},"reference",true);
    add("line_3_forward_hybrid","line",{40,30,4,38,231,17,109},"hybrid",true);
    add("line_3_reverse_reference","line",{4,38,40,30,231,17,109},"reference",true);
    add("line_3_reverse_hybrid","line",{4,38,40,30,231,17,109},"hybrid",true);
    add("line_4_forward_reference","line",{40,30,4,22,231,17,109},"reference",true);
    add("line_4_forward_hybrid","line",{40,30,4,22,231,17,109},"hybrid",true);
    add("line_4_reverse_reference","line",{4,22,40,30,231,17,109},"reference",true);
    add("line_4_reverse_hybrid","line",{4,22,40,30,231,17,109},"hybrid",true);
    add("line_5_forward_reference","line",{40,30,32,2,231,17,109},"reference",true);
    add("line_5_forward_hybrid","line",{40,30,32,2,231,17,109},"hybrid",true);
    add("line_5_reverse_reference","line",{32,2,40,30,231,17,109},"reference",true);
    add("line_5_reverse_hybrid","line",{32,2,40,30,231,17,109},"hybrid",true);
    add("line_6_forward_reference","line",{40,30,48,2,231,17,109},"reference",true);
    add("line_6_forward_hybrid","line",{40,30,48,2,231,17,109},"hybrid",true);
    add("line_6_reverse_reference","line",{48,2,40,30,231,17,109},"reference",true);
    add("line_6_reverse_hybrid","line",{48,2,40,30,231,17,109},"hybrid",true);
    add("line_7_forward_reference","line",{40,30,76,22,231,17,109},"reference",true);
    add("line_7_forward_hybrid","line",{40,30,76,22,231,17,109},"hybrid",true);
    add("line_7_reverse_reference","line",{76,22,40,30,231,17,109},"reference",true);
    add("line_7_reverse_hybrid","line",{76,22,40,30,231,17,109},"hybrid",true);
    add("line_8_forward_reference","line",{0,0,79,0,231,17,109},"reference",true);
    add("line_8_forward_hybrid","line",{0,0,79,0,231,17,109},"hybrid",true);
    add("line_8_reverse_reference","line",{79,0,0,0,231,17,109},"reference",true);
    add("line_8_reverse_hybrid","line",{79,0,0,0,231,17,109},"hybrid",true);
    add("line_9_forward_reference","line",{0,0,0,59,231,17,109},"reference",true);
    add("line_9_forward_hybrid","line",{0,0,0,59,231,17,109},"hybrid",true);
    add("line_9_reverse_reference","line",{0,59,0,0,231,17,109},"reference",true);
    add("line_9_reverse_hybrid","line",{0,59,0,0,231,17,109},"hybrid",true);
    add("line_10_forward_reference","line",{0,0,59,59,231,17,109},"reference",true);
    add("line_10_forward_hybrid","line",{0,0,59,59,231,17,109},"hybrid",true);
    add("line_10_reverse_reference","line",{59,59,0,0,231,17,109},"reference",true);
    add("line_10_reverse_hybrid","line",{59,59,0,0,231,17,109},"hybrid",true);
    add("line_11_forward_reference","line",{33,21,33,21,231,17,109},"reference",true);
    add("line_11_forward_hybrid","line",{33,21,33,21,231,17,109},"hybrid",true);
    add("line_11_reverse_reference","line",{33,21,33,21,231,17,109},"reference",true);
    add("line_11_reverse_hybrid","line",{33,21,33,21,231,17,109},"hybrid",true);
    add("line_12_forward_reference","line",{2,2,4,3,231,17,109},"reference",true);
    add("line_12_forward_hybrid","line",{2,2,4,3,231,17,109},"hybrid",true);
    add("line_12_reverse_reference","line",{4,3,2,2,231,17,109},"reference",true);
    add("line_12_reverse_hybrid","line",{4,3,2,2,231,17,109},"hybrid",true);
    add("line_13_forward_reference","line",{2,2,3,4,231,17,109},"reference",true);
    add("line_13_forward_hybrid","line",{2,2,3,4,231,17,109},"hybrid",true);
    add("line_13_reverse_reference","line",{3,4,2,2,231,17,109},"reference",true);
    add("line_13_reverse_hybrid","line",{3,4,2,2,231,17,109},"hybrid",true);
    add("line_14_forward_reference","line",{-30,-12,110,68,231,17,109},"reference",true);
    add("line_14_forward_hybrid","line",{-30,-12,110,68,231,17,109},"hybrid",true);
    add("line_14_reverse_reference","line",{110,68,-30,-12,231,17,109},"reference",true);
    add("line_14_reverse_hybrid","line",{110,68,-30,-12,231,17,109},"hybrid",true);
    add("line_15_forward_reference","line",{-4096,30,4096,31,231,17,109},"reference",true);
    add("line_15_forward_hybrid","line",{-4096,30,4096,31,231,17,109},"hybrid",true);
    add("line_15_reverse_reference","line",{4096,31,-4096,30,231,17,109},"reference",true);
    add("line_15_reverse_hybrid","line",{4096,31,-4096,30,231,17,109},"hybrid",true);
    add("line_16_forward_reference","line",{-4096,-4096,-4050,-4040,231,17,109},"reference",true);
    add("line_16_forward_hybrid","line",{-4096,-4096,-4050,-4040,231,17,109},"hybrid",true);
    add("line_16_reverse_reference","line",{-4050,-4040,-4096,-4096,231,17,109},"reference",true);
    add("line_16_reverse_hybrid","line",{-4050,-4040,-4096,-4096,231,17,109},"hybrid",true);
    add("line_17_forward_reference","line",{79,59,90,70,231,17,109},"reference",true);
    add("line_17_forward_hybrid","line",{79,59,90,70,231,17,109},"hybrid",true);
    add("line_17_reverse_reference","line",{90,70,79,59,231,17,109},"reference",true);
    add("line_17_reverse_hybrid","line",{90,70,79,59,231,17,109},"hybrid",true);
    add("line_18_forward_reference","line",{80,60,-1,-1,231,17,109},"reference",true);
    add("line_18_forward_hybrid","line",{80,60,-1,-1,231,17,109},"hybrid",true);
    add("line_18_reverse_reference","line",{-1,-1,80,60,231,17,109},"reference",true);
    add("line_18_reverse_hybrid","line",{-1,-1,80,60,231,17,109},"hybrid",true);
    add("layer_span","span",{20,-8,90,1,233,4},"hybrid",false);
    add("layer_copy","copy",{37,18,-3,-2,20,14},"hybrid",false);
    add("layer_overlap","self",{38,19,35,17,17,12},"hybrid",false);
    add("layer_line","line",{-25,50,105,4,250,3,230},"hybrid",false);
    cs.push_back({"span_reverse","span","reference",{},{"d","80","60","2","8","3","1","2","3"},true,true,false,0});
    cs.push_back({"fraction_x","line","reference",{},{"d","80","60","1/3","0","3","4","1","2","3"},true,true,false,0});
    cs.push_back({"fraction_width","span","reference",{},{"d","80+1/3","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"negative_width","span","reference",{},{"d","-1","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"oversized_width","span","reference",{},{"d","81","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"oversized_height","span","reference",{},{"d","80","61","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"zero_pointer","span","reference",{},{"0","80","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"fraction_pointer","span","reference",{},{"d+1/3","80","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"negative_pointer","span","reference",{},{"-1","80","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"declared_end","span","reference",{},{"373247","80","60","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"rgb_fraction","span","reference",{},{"d","80","60","1","0","3","1","2/3","3"},true,true,false,0});
    cs.push_back({"rgb_negative","line","reference",{},{"d","80","60","0","0","5","5","-1","2","3"},true,true,false,0});
    cs.push_back({"rgb_256","line","reference",{},{"d","80","60","0","0","5","5","1","2","256"},true,true,false,0});
    cs.push_back({"coord_4097","line","reference",{},{"d","80","60","-4097","0","5","5","1","2","3"},true,true,false,0});
    cs.push_back({"extent_8193","copy","reference",{},{"d","80","60","s","12","9","0","0","0","0","8193","1"},true,true,false,0});
    cs.push_back({"extent_fraction","copy","reference",{},{"d","80","60","s","12","9","0","0","0","0","3+1/3","1"},true,true,false,0});
    cs.push_back({"extent_negative","copy","reference",{},{"d","80","60","s","12","9","0","0","0","0","-1","1"},true,true,false,0});
    cs.push_back({"partial_intervals","copy","reference",{},{"d","80","60","d+9","12","9","0","0","0","0","3","2"},true,true,false,0});
    add("warm_horizontal_reference","line",{0,29,79,29,37,83,191},"reference",true);
    add("warm_horizontal_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    add("bench_horizontal_0_reference","line",{0,29,79,29,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_horizontal_0_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_horizontal_1_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_horizontal_1_reference","line",{0,29,79,29,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_horizontal_2_reference","line",{0,29,79,29,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_horizontal_2_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_horizontal_3_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_horizontal_3_reference","line",{0,29,79,29,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_horizontal_4_reference","line",{0,29,79,29,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_horizontal_4_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_horizontal_5_hybrid","line",{0,29,79,29,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("bench_horizontal_5_reference","line",{0,29,79,29,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("warm_shallow_reference","line",{0,2,79,27,37,83,191},"reference",true);
    add("warm_shallow_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    add("bench_shallow_0_reference","line",{0,2,79,27,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_shallow_0_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_shallow_1_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_shallow_1_reference","line",{0,2,79,27,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_shallow_2_reference","line",{0,2,79,27,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_shallow_2_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_shallow_3_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_shallow_3_reference","line",{0,2,79,27,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_shallow_4_reference","line",{0,2,79,27,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_shallow_4_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_shallow_5_hybrid","line",{0,2,79,27,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("bench_shallow_5_reference","line",{0,2,79,27,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("warm_steep_reference","line",{31,0,44,59,37,83,191},"reference",true);
    add("warm_steep_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    add("bench_steep_0_reference","line",{31,0,44,59,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_steep_0_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_steep_1_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_steep_1_reference","line",{31,0,44,59,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_steep_2_reference","line",{31,0,44,59,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_steep_2_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_steep_3_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_steep_3_reference","line",{31,0,44,59,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_steep_4_reference","line",{31,0,44,59,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_steep_4_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_steep_5_hybrid","line",{31,0,44,59,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("bench_steep_5_reference","line",{31,0,44,59,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("warm_clipped_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    add("warm_clipped_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    add("bench_clipped_0_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_clipped_0_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=0;
    add("bench_clipped_1_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_clipped_1_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=1;
    add("bench_clipped_2_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_clipped_2_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=2;
    add("bench_clipped_3_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_clipped_3_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=3;
    add("bench_clipped_4_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_clipped_4_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=4;
    add("bench_clipped_5_hybrid","line",{-40,2,115,51,37,83,191},"hybrid",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    add("bench_clipped_5_reference","line",{-40,2,115,51,37,83,191},"reference",true);
    cs.back().benchmark=true;cs.back().repetition=5;
    cs.push_back({"span_min_surface","span","reference",{0,0,1,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"line_min_surface_reference","line","reference",{0,0,0,0,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"line_min_surface_hybrid","line","hybrid",{0,0,0,0,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"copy_max_extents","copy","reference",{0,0,0,0,8192,8192},{},false,true,false,0});
    cs.push_back({"height_fraction","span","reference",{},{"d","80","60+1/3","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"height_zero","span","reference",{},{"d","80","0","1","0","3","1","2","3"},true,true,false,0});
    cs.push_back({"source_dimension_fraction","copy","reference",{},{"d","80","60","s","12+1/3","9","0","0","0","0","3","2"},true,true,false,0});
    cs.push_back({"same_base_different_dimensions","copy","reference",{},{"d","80","60","d","79","60","0","0","0","0","3","2"},true,true,false,0});
    cs.push_back({"fraction_x_hybrid","line","hybrid",{},{"d","80","60","1/3","0","3","4","1","2","3"},true,true,false,0});
    cs.push_back({"rgb_negative_hybrid","line","hybrid",{},{"d","80","60","0","0","5","5","-1","2","3"},true,true,false,0});
    cs.push_back({"rgb_256_hybrid","line","hybrid",{},{"d","80","60","0","0","5","5","1","2","256"},true,true,false,0});
    cs.push_back({"coord_4097_hybrid","line","hybrid",{},{"d","80","60","-4097","0","5","5","1","2","3"},true,true,false,0});
    // F1: clipping uses independent dimensions, while all4800 backing pixels remain compared.
    cs.push_back({"span_min_surface_outside","span","reference",{1,-2,3,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"line_min_surface_outside_reference","line","reference",{1,0,3,0,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"line_min_surface_outside_hybrid","line","hybrid",{0,1,0,3,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"span_min_surface_crossing","span","reference",{0,-2,3,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"line_min_surface_crossing_reference","line","reference",{0,-2,0,3,9,17,33},{},false,true,false,0,1,1});
    cs.push_back({"line_min_surface_crossing_hybrid","line","hybrid",{-2,0,3,0,9,17,33},{},false,true,false,0,1,1});
    return cs;
}
std::string drawingCall(const DrawingCase& c) {
    std::vector<std::string> a=c.guest_args;
    if(a.empty()) {
        auto append=[&]{for(int n:c.a)a.push_back(std::to_string(n));};
        if(c.kind=="line"){a={"d",std::to_string(c.destination_width),std::to_string(c.destination_height)};append();}
        else if(c.kind=="span"){a={"d",std::to_string(c.destination_width),std::to_string(c.destination_height)};append();}
        else {a={"d",std::to_string(c.destination_width),std::to_string(c.destination_height),c.kind=="self"?"d":"s",c.kind=="self"?std::to_string(c.destination_width):"12",c.kind=="self"?std::to_string(c.destination_height):"9"};append();}
    }
    const auto fn=c.kind=="line"?"draw_rgb_line_"+c.algorithm:c.kind=="span"?"draw_rgb_span":"draw_rgb_copy";
    std::string call=fn+"(";for(size_t i=0;i<a.size();++i){if(i)call+=",";call+=a[i];}return call+")";
}

// Typed predicates use the same native primitives as the audited SDK E2E.
bool drawingExactEquals(const sandbox::vm::TernaryValue& v,const sandbox::Triple& expected) {
    return v.mode==sandbox::TernaryMode::T40 && !v.isInvalid() && !v.asTriple().isSpecial()
        && !expected.isInvalid() && !expected.isSpecial() && v.asTriple().data==expected.data;
}
bool drawingIntegerEquals(const sandbox::vm::TernaryValue& v,long long expected) {
    return drawingExactEquals(v,sandbox::native_ops::fromIntT40(expected));
}
bool drawingIntegerValue(const sandbox::vm::TernaryValue& v,long long& number) {
    if(v.mode!=sandbox::TernaryMode::T40 || v.isInvalid() || v.asTriple().isSpecial())return false;
    number=sandbox::vm::ops::toLong(v);
    return drawingIntegerEquals(v,number);
}
// Compiled guest actors and checked evidence for the isolated drawing gate.
std::string drawingGuest(const std::vector<DrawingCase>& cases) {
    std::string g=R"TRIT(
fn drawing_marker(b: t40, phase: t40) -> t40 { unsafe {store(b,phase);} return sys_getpid(); }
fn drawing_fill(p: t40,w: t40,h: t40,salt: t40) -> t40 {
 var y: t40=0;while y<h {var x: t40=0;while x<w {var c: t40=0;
 while c<3 {unsafe {store(p+(y*w+x)*3+c,tmod(x*17+y*11+c*53+salt,256));}c=c+1;}x=x+1;}y=y+1;}return 1;
}
fn main() -> t40 {
 
 let q: t40=sys_sbrk(0);let b: t40=sys_sbrk(15000);let end: t40=sys_sbrk(0);
 // Evaluate all three real results before any heap dereference.
 if q<=2048 {sys_exit(93);return 0;}unsafe {if tmod(q,1)!=0 {sys_exit(93);return 0;}if tmod(b,1)!=0 {sys_exit(93);return 0;}if tmod(end,1)!=0 {sys_exit(93);return 0;}}
 if b!=q {sys_exit(93);return 0;}
 if end!=b+15000 {sys_exit(93);return 0;}
 if end>53946 {sys_exit(93);return 0;}
 let d: t40=b+96;let s: t40=b+14528;
 var i: t40=0;while i<15000 {unsafe {store(b+i,-91);}i=i+1;}
 unsafe {store(b+3,d);store(b+4,s);store(b+5,b);store(b+6,15000);}
)TRIT";
    // Genuine caller register pressure: values computed by normal sys_getpid,
    // remain live across every drawing call, and are checked at the end.
    for(int n=0;n<18;++n)g+="let live"+std::to_string(n)+": t40=sys_getpid()+"+std::to_string(n)+";\n";
    g+="drawing_fill(s,12,9,19);os_ipc_send(0,17);os_ipc_recv_blocking(1,b+31,-1);drawing_marker(b,1);\n";
    for(size_t n=0;n<cases.size();++n){const auto& c=cases[n];
        if(c.reset)g+="drawing_fill(d,80,60,0);\n";
        g+="drawing_marker(b,"+std::to_string(10+2*n)+");\nlet result"+std::to_string(n)+": t40="+drawingCall(c)+";\nunsafe {store(b+2,result"+std::to_string(n)+");}\ndrawing_marker(b,"+std::to_string(11+2*n)+");\n";
    }
    for(int n=0;n<2;++n){const auto pid=421+n;
        g+="let child"+std::to_string(n)+": t40=sys_fork("+std::to_string(21+n)+","+std::to_string(pid)+");\nif child"+std::to_string(n)+"==0 {var exceptional: t40=1;var k: t40=0;while k<1200 {exceptional=exceptional*3;k=k+1;}unsafe {store(b+7,exceptional);}drawing_marker(b,"+std::to_string(100000+n)+");\nlet rejected: t40=";
        g+=n==0?"draw_rgb_span(d,80,60,4,2,9,exceptional,2,3)":"draw_rgb_line_reference(d,80,60,exceptional,2,3,4,1,2,3)";
        g+=";unsafe {store(b+2,rejected);}drawing_marker(b,"+std::to_string(100010+n)+");sys_exit(45);return 0;}\nlet waited"+std::to_string(n)+": t40=sys_waitpid("+std::to_string(pid)+");unsafe {store(b+30,waited"+std::to_string(n)+");}drawing_marker(b,"+std::to_string(110000+n)+");os_ipc_send(0,"+std::to_string(18+n)+");os_ipc_recv_blocking(1,b+31,-1);drawing_marker(b,"+std::to_string(120000+n)+");\n";
    }
    for(int n=0;n<18;++n)g+="unsafe {store(b+10+"+std::to_string(n)+",live"+std::to_string(n)+");}\n";
    return g+"drawing_marker(b,999999);os_ipc_send(0,99);os_ipc_recv_blocking(1,b+31,-1);sys_exit(0);return 0;}\n";
}

std::string drawingSurvivor() { return R"TRIT(
fn main() -> t40 {
 let q: t40=sys_sbrk(0);let b: t40=sys_sbrk(96);let e: t40=sys_sbrk(0);
 if q<=2048 {sys_exit(93);return 0;}unsafe {if tmod(q,1)!=0 {sys_exit(93);return 0;}if tmod(b,1)!=0 {sys_exit(93);return 0;}if tmod(e,1)!=0 {sys_exit(93);return 0;}}if b!=q {sys_exit(93);return 0;}if e!=b+96 {sys_exit(93);return 0;}if e>53946 {sys_exit(93);return 0;}
 unsafe {store(b,-91);store(b+95,91);store(b+2,1/3);}
 var n: t40=0;while n<4 {os_ipc_recv_blocking(0,b+4,-1);
  unsafe {store(b+3,(n+3)/3);store(b+5,n);}
  sys_getpid();os_ipc_send(1,71+n);n=n+1;
 }sys_exit(0);return 0;
}
)TRIT"; }

bool drawingSha(const std::filesystem::path& dir,const char* name) {
    // Generated evidence helper only, not a repository wrapper. Same Python
    // SHA256 path used by existing native workflows; no guest state mutation.
    const std::string script=R"PY(import ast,pathlib,hashlib,json,sys
root=pathlib.Path(sys.argv[1]);output=pathlib.Path(sys.argv[2])
tree=ast.parse((root/'tools/trit_tool.py').read_text())
sources=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='SOURCE_INPUTS' for t in n.targets))
assert len(sources)==85 and len(set(sources))==85 and 'apps/span_draw.trit' in sources
out={name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in sources}
output.write_text(json.dumps(out,indent=2)+'\n')
)PY";
    if(!transportWriteArtifact(dir/"sha_program.txt",script))return false;
    const auto command="python - \""+std::filesystem::current_path().string()+"\" \""+(dir/name).string()+"\" < \""+(dir/"sha_program.txt").string()+"\"";
    const int exit_code=std::system(command.c_str());std::ofstream receipt(dir/"host_commands.jsonl",std::ios::app);receipt<<"{\"command\":\""<<jsonEscape(command)<<"\",\"system_exit\":"<<exit_code<<"}\n";receipt.flush();receipt.close();return require(exit_code==0&&!receipt.fail(),"drawing actual source SHA256 writer and command receipt");
}

bool drawingInventory(const std::filesystem::path& dir) {
    const std::string script=R"PY(import pathlib,hashlib,json,sys
root=pathlib.Path(sys.argv[1]);files={}
for p in sorted(root.rglob('*')):
    if p.is_file() and p.name not in ['artifact_inventory.json','report.json','host_commands.jsonl']:
        data=p.read_bytes();files[p.relative_to(root).as_posix()]={'sha256':hashlib.sha256(data).hexdigest(),'size_bytes':len(data)}
(root/'artifact_inventory.json').write_text(json.dumps({'algorithm':'SHA256','files':files,'outer_coupled_seals':['artifact_inventory.json','host_commands.jsonl','report.json']},indent=2)+'\n')
)PY";
    if(!transportWriteArtifact(dir/"inventory_program.txt",script))return false;
    const auto command="python - \""+dir.string()+"\" < \""+(dir/"inventory_program.txt").string()+"\"";
    const int exit_code=std::system(command.c_str());std::ofstream receipt(dir/"host_commands.jsonl",std::ios::app);receipt<<"{\"command\":\""<<jsonEscape(command)<<"\",\"system_exit\":"<<exit_code<<"}\n";receipt.flush();receipt.close();return require(exit_code==0&&!receipt.fail(),"drawing actual inventory command and checked receipt");
}

bool drawingOwnedEvidence(const std::filesystem::path& dir,size_t case_count,bool lifecycle_only=false) {
    bool ok=true;const std::vector<std::string> required={"report.json","source_hashes_before.json","source_hashes_after.json","drawing_actor.trit","drawing_actor.linked.tasm","drawing_actor.final.tasm","drawing_actor.catalog.json","drawing_survivor.trit","drawing_survivor.linked.tasm","drawing_survivor.final.tasm","drawing_survivor.catalog.json","boot.tasm","boot.tboot","disk.tdisk","terminal_state.bin","execution.json","registration.jsonl","lifecycle.jsonl","raw_returns.jsonl","ordered_cases.jsonl","drawing_raw_returns.jsonl","benchmark_raw.jsonl","counter_steps.jsonl","placement.jsonl","pixels.jsonl","guards.jsonl","special.jsonl","assertions.jsonl","case_inventory.json","artifact_inventory.json","host_commands.jsonl","benchmark_settings.json","selection.json","commands.json","criteria_map.json","bootstrap_policy.trit"};
    auto check_file=[&](const std::filesystem::path& p,bool empty=false){std::ifstream in(p,std::ios::binary);const bool opened=in.is_open();std::string bytes;if(opened)bytes.assign(std::istreambuf_iterator<char>(in),{});in.close();ok=require(opened&&!in.bad()&&(empty||!bytes.empty()),"drawing owned evidence closes "+p.string())&&ok;};
    for(const auto& name:required)if(name!="report.json")check_file(dir/name,lifecycle_only&&name=="benchmark_raw.jsonl");
    for(size_t n=0;n<case_count;++n)check_file(dir/("rgb_"+std::to_string(n)+".png"));
    for(const auto& entry:std::filesystem::recursive_directory_iterator(dir))if(entry.is_regular_file())check_file(entry.path(),entry.path().extension()==".txt"||entry.path().filename()=="records.jsonl"||(lifecycle_only&&entry.path().filename()=="benchmark_raw.jsonl"));
    return ok;
}

bool runCompositorDrawing(bool lifecycle_only=false) {
 using namespace sandbox;
 const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage4"/(lifecycle_only?"lifecycle":"");
 const auto selector=lifecycle_only?"--compositor-drawing-lifecycle-only":"--compositor-drawing-only";
 std::error_code artifact_error;std::filesystem::create_directories(root,artifact_error);if(!require(!artifact_error,"drawing artifact root creation"))return false;int serial=1;while(std::filesystem::exists(root/("run_"+std::to_string(serial))))++serial;
 const auto dir=root/("run_"+std::to_string(serial));const bool created=std::filesystem::create_directory(dir,artifact_error);if(!require(created&&!artifact_error,"drawing new append-only run directory"))return false;
 bool native_vm_executed=false,native_user_executed=false,drawing_call_executed=false;
 auto publish=[&](const char* status,bool executed,const std::string& why){std::ostringstream o;o<<"{\"status\":\""<<status<<"\",\"run_directory\":\""<<dir.lexically_relative(TRIT_TEST_BUILD_DIR).generic_string()<<"\",\"native_runtime_executed\":"<<(executed?"true":"false")<<",\"native_User_executed\":"<<(native_user_executed?"true":"false")<<",\"drawing_CALL_executed\":"<<(drawing_call_executed?"true":"false")<<",\"reason\":\""<<jsonEscape(why)<<"\",\"validation_scope\":\""<<(lifecycle_only?"native_drawing_lifecycle_and_owned_artifacts":"native_drawing_and_owned_artifacts")<<"\","<<(lifecycle_only?"\"raster_matrix_complete\":false,\"benchmark_executed\":false,":"")<<"\"coupled_completion_required_for_acceptance\":true,\"Stage4_acceptance\":false,\"protected_resource_integration\":false}\n";return transportWriteArtifact(dir/"report.json",o.str())&&transportWriteArtifact(root/"report.json",o.str());};
 if(!std::filesystem::is_regular_file("apps/span_draw.trit")){const bool reported=publish("INCOMPLETE",false,"feature-missing compile prerequisite apps/span_draw.trit; no other source validation or coupled native completion claimed");return require(reported,"missing-library prerequisite report publication")&&require(false,"drawing library absent; native runtime unexecuted");}
 if(!drawingSha(dir,"source_hashes_before.json")){publish("INCOMPLETE",false,"pre-execution source SHA writer or source closure failed");return false;}
 auto cases=drawingCases();
 if(lifecycle_only){const auto all_cases=cases;cases.clear();for(const auto& name:completionDrawingLifecycleCases){const auto found=std::find_if(all_cases.begin(),all_cases.end(),[&](const DrawingCase& c){return c.name==name;});if(!require(found!=all_cases.end()&&!found->benchmark,"exact existing nonbenchmark lifecycle case"))return false;cases.push_back(*found);}}
 const auto source=drawingPattern(12,9,19);DrawingPixels expected=drawingPattern(80,60,0);
 std::ofstream assertions(dir/"assertions.jsonl");
 std::ofstream trace(dir/"ordered_cases.jsonl"),raw(dir/"drawing_raw_returns.jsonl"),bench(dir/"benchmark_raw.jsonl"),placement(dir/"placement.jsonl"),pixels(dir/"pixels.jsonl"),guards(dir/"guards.jsonl"),special(dir/"special.jsonl"),counters(dir/"counter_steps.jsonl");
 if(!require(trace.good()&&raw.good()&&bench.good()&&placement.good()&&pixels.good()&&guards.good()&&special.good()&&counters.good()&&assertions.good(),"drawing checked trace writers open"))return false;
 bool ok=true,finished=false;int base=0,sbase=0,slot=-1,index=0,survivor_progress=0;bool parent_exit=false,super_exit=false,actor_cleanup_complete=false;
 std::map<int,std::pair<int,long long>> actor_exit_identity;
 bool supervisor_identity_captured=false;std::array<long long,9> supervisor_initial_row{};long long supervisor_initial_caps=0,supervisor_initial_generation=0;int supervisor_initial_slot=-1,parent_initial_slot=-1;long long parent_initial_generation=0;
 std::map<std::string,int> labels;std::map<int,vm::ExecutableImageHeaderV3> headers;
 struct Pending {int pc=0,service=0;long long arg=0;};std::map<std::tuple<int,int,long long>,Pending> pending;
 std::map<int,int> alloc_calls,wait_returns;std::map<int,long long> heapbase;
 std::map<int,bool> exception_safe,exception_fault,exception_return,exception_crashed,child_called;std::map<int,int> child_exits;std::map<int,DrawingPixels> before_exception;
 std::map<int,std::vector<vm::TernaryValue>> before_exception_source;
 std::map<int,int> progress_before;
 int before_pid=-1,before_slot=-1;long long before_generation=0;std::array<vm::TernaryValue,32> before_registers;std::map<int,int> special_multiplies;std::map<int,bool> finite_to_special;std::array<std::array<vm::TernaryValue,27>,8> before_vectors;int before_vector_length=0;std::map<int,bool> special_interval;
 struct Region {bool active=false;int depth=0,lr=0;long long gen=0;std::uint64_t user_cycles=0,kernel_cycles=0,instructions=0;};Region region;
 std::vector<int> call_returns;std::vector<Region> measured(cases.size());std::set<int> pngs;std::map<int,int> starts;
 auto check=[&](bool v,const std::string&w){if(assertions.is_open())assertions<<"{\"pass\":"<<(v?"true":"false")<<",\"assertion\":\""<<jsonEscape(w)<<"\"}\n";ok=require(v,w)&&ok;return v;};
 // Final diagnostics observe existing Kernel words; no guest or Kernel state is written.
 std::map<std::string,int> diagnostic_constants;
 for(const auto& name:std::vector<std::string>{"PROCESS_BASE","PROCESS_ROW_WORDS","PROCESS_MAX","PROC_CONTEXT","PROC_PARENT_PID_BASE","PROC_CAPS_BASE","PROC_EXIT_STATUS_BASE","WAIT_KIND_BASE","WAIT_CHANNEL_BASE","WAIT_DEADLINE_BASE","WAIT_PROC_NEXT_BASE","PROCESS_HEAP_BASE","PROCESS_HEAP_WORDS","PROCESS_CONTEXT_BASE","TASK_CONTEXT_WORDS","TASK_CONTEXT_EPC","IPC_BASE","IPC_CHANNEL_WORDS","IPC_OWNER_BASE","IPC_VERSION","IPC_READ","IPC_WRITE","IPC_COUNT","IPC_BUF","IPC_CAP","IPC_BUFFER_BASE","IPC_CAPACITY","WAIT_CHAN_IPC_BASE","WAIT_QUEUE_BASE","WAIT_QUEUE_WORDS","WAIT_QUEUE_MAX","WQ_CHANNEL","WQ_HEAD","WQ_VERSION","WAIT_QUEUE_TAIL_BASE","CURRENT_PID_ADDR","CURRENT_CONTEXT_ADDR","GFX_METADATA_BASE","GFX_PROC_GENERATION_BASE","PROC_PID","PROC_STATE","PROC_NAMESPACE","PROC_PRIORITY","PROC_QUOTA","PROC_VECTOR_CONTEXT","PROC_WAIT_CHANNEL","PROC_CLEANED_BASE","PROC_FREE","PROC_ZOMBIE","PROC_EXITED","PROC_CRASHED"}){
  const auto v=transportSourceConstant(name);if(!check(v>=0&&v<=std::numeric_limits<int>::max(),"final diagnostic source constant "+name))return false;diagnostic_constants[name]=static_cast<int>(v);
 }
 auto dc=[&](const std::string& name){return diagnostic_constants.at(name);};
 auto flush_special=[&]{special.flush();return check(special.good(),"final diagnostic/IPC/zeroexit evidence write and flush");};
 struct FinalIpcPending {int pc=0,service=0;long long step=0;std::array<vm::TernaryValue,4> args;};
 std::map<std::tuple<int,int,long long>,FinalIpcPending> final_ipc_pending;
 long long last_final_diagnostic_step=-1;
 auto final_state=[&](const vm::VMState&m,const vm::VMExecutionRecord&r,long long step,const char* reason){
  std::map<int,std::pair<vm::TernaryValue,vm::MemFaultCode>> observed_words;
  auto number=[&](int address,long long& v){const auto loaded=m.dmem.load(address);observed_words[address]=loaded;return loaded.second==vm::MemFaultCode::OK&&drawingIntegerValue(loaded.first,v);};
  auto word=[&](int address){long long v=0;return number(address,v)?std::to_string(v):std::string("null");};
  auto words=[&](int address,int count){special<<'[';for(int n=0;n<count;++n){if(n)special<<',';special<<word(address+n);}special<<']';};
  special<<"{\"event\":\"actual_drawing_final_state\",\"reason\":\""<<reason<<"\",\"step\":"<<step<<",\"attempted\":"<<(r.attempted?"true":"false")<<",\"pc\":"<<m.pc<<",\"record_pc\":"<<r.pc<<",\"final_pc\":"<<r.final_pc<<",\"privilege\":"<<static_cast<int>(m.privilege)<<",\"before_privilege\":"<<static_cast<int>(r.before_privilege)<<",\"cycles\":"<<m.cycle_count<<",\"vm_status\":"<<static_cast<int>(m.status)<<",\"current_pid\":"<<word(dc("CURRENT_PID_ADDR"))<<",\"current_context\":"<<word(dc("CURRENT_CONTEXT_ADDR"))<<",\"instruction_raw\":"<<r.raw_bits<<",\"has_instruction\":"<<(r.has_instruction?"true":"false")<<",\"opcode\":";
  if(r.has_instruction)special<<static_cast<int>(r.instruction.opcode);else special<<"null";
  special<<",\"finished\":"<<(finished?"true":"false")<<",\"parent_exit\":"<<(parent_exit?"true":"false")<<",\"super_exit\":"<<(super_exit?"true":"false")<<",\"actor_cleanup_complete\":"<<(actor_cleanup_complete?"true":"false")<<",\"actors\":[";
  for(int n=0;n<2;++n){if(n)special<<',';const int actor_slot=1+n,row=dc("PROCESS_BASE")+actor_slot*dc("PROCESS_ROW_WORDS");long long ctx=0;const bool context_integral=number(row+dc("PROC_CONTEXT"),ctx);
   special<<"{\"expected_pid\":"<<(n==0?202:201)<<",\"slot\":"<<actor_slot<<",\"generation\":"<<word(dc("GFX_METADATA_BASE")+dc("GFX_PROC_GENERATION_BASE")+actor_slot)<<",\"process_row\":";words(row,dc("PROCESS_ROW_WORDS"));
   for(const auto& field:std::vector<std::pair<std::string,std::string>>{{"parent","PROC_PARENT_PID_BASE"},{"caps","PROC_CAPS_BASE"},{"exit_status","PROC_EXIT_STATUS_BASE"},{"cleaned","PROC_CLEANED_BASE"},{"wait_kind","WAIT_KIND_BASE"},{"wait_channel","WAIT_CHANNEL_BASE"},{"deadline","WAIT_DEADLINE_BASE"},{"wait_proc_next","WAIT_PROC_NEXT_BASE"}})special<<",\""<<field.first<<"\":"<<word(dc(field.second)+actor_slot);
   special<<",\"heap\":";words(dc("PROCESS_HEAP_BASE")+actor_slot*dc("PROCESS_HEAP_WORDS"),dc("PROCESS_HEAP_WORDS"));
   const bool context_readable=context_integral&&ctx>0&&ctx>=dc("PROCESS_CONTEXT_BASE")&&ctx<dc("PROCESS_CONTEXT_BASE")+dc("PROCESS_MAX")*dc("TASK_CONTEXT_WORDS");
   special<<",\"context\":"<<word(row+dc("PROC_CONTEXT"))<<",\"context_readable\":"<<(context_readable?"true":"false")<<",\"context_epc\":";if(context_readable)special<<word(static_cast<int>(ctx)+dc("TASK_CONTEXT_EPC"));else special<<"null";special<<'}';
  }
  special<<"],\"ipc\":[";
  for(int channel=0;channel<2;++channel){if(channel)special<<',';const int row=dc("IPC_BASE")+channel*dc("IPC_CHANNEL_WORDS");long long read=0,buffer=0,capacity=0;const bool ring_integral=number(row+dc("IPC_READ"),read)&&number(row+dc("IPC_BUF"),buffer)&&number(row+dc("IPC_CAP"),capacity);int queue=-1;bool queue_metadata_valid=true;const auto wait_channel=dc("WAIT_CHAN_IPC_BASE")+channel;
   for(int q=0;q<dc("WAIT_QUEUE_MAX");++q){const int qrow=dc("WAIT_QUEUE_BASE")+q*dc("WAIT_QUEUE_WORDS");long long version=0,key=0;if(!number(qrow+dc("WQ_VERSION"),version)){queue_metadata_valid=false;continue;}if(version>0){if(!number(qrow+dc("WQ_CHANNEL"),key)){queue_metadata_valid=false;continue;}if(key==wait_channel){queue=q;break;}}}
   special<<"{\"channel\":"<<channel<<",\"owner\":"<<word(dc("IPC_OWNER_BASE")+channel)<<",\"row\":";words(row,dc("IPC_CHANNEL_WORDS"));special<<",\"version\":"<<word(row+dc("IPC_VERSION"))<<",\"head\":"<<word(row+dc("IPC_READ"))<<",\"read\":"<<word(row+dc("IPC_READ"))<<",\"write\":"<<word(row+dc("IPC_WRITE"))<<",\"count\":"<<word(row+dc("IPC_COUNT"))<<",\"head_word\":";
   if(ring_integral&&buffer==dc("IPC_BUFFER_BASE")+channel*dc("IPC_CAPACITY")&&capacity==dc("IPC_CAPACITY")&&read>=0&&read<capacity)special<<word(static_cast<int>(buffer+read));else special<<"null";
   special<<",\"wait_channel\":"<<wait_channel<<",\"wait_queue_metadata_valid\":"<<(queue_metadata_valid?"true":"false")<<",\"wait_queue_index\":"<<queue<<",\"wait_queue_head\":";if(queue>=0)special<<word(dc("WAIT_QUEUE_BASE")+queue*dc("WAIT_QUEUE_WORDS")+dc("WQ_HEAD"));else special<<"null";special<<",\"wait_queue_tail\":";if(queue>=0)special<<word(dc("WAIT_QUEUE_TAIL_BASE")+queue);else special<<"null";special<<",\"wait_queue_version\":";if(queue>=0)special<<word(dc("WAIT_QUEUE_BASE")+queue*dc("WAIT_QUEUE_WORDS")+dc("WQ_VERSION"));else special<<"null";special<<'}';
  }
  special<<"],\"captured_exit_identities\":[";bool comma=false;for(const auto& [actor_pid,identity]:actor_exit_identity){if(comma)special<<',';comma=true;special<<"{\"pid\":"<<actor_pid<<",\"slot\":"<<identity.first<<",\"generation\":"<<identity.second<<'}';}
  special<<"],\"pending_final_ipc\":[";comma=false;for(const auto& [identity,p]:final_ipc_pending){if(comma)special<<',';comma=true;const auto [actor_pid,actor_slot,generation]=identity;special<<"{\"pid\":"<<actor_pid<<",\"slot\":"<<actor_slot<<",\"generation\":"<<generation<<",\"service\":"<<p.service<<",\"entry_pc\":"<<p.pc<<",\"entry_step\":"<<p.step<<",\"return_observed\":false,\"args_exact\":[";for(int n=0;n<4;++n){if(n)special<<',';transportExactWord(special,p.args[n]);}special<<"]}";}
  special<<"],\"kernel_words_exact\":[";comma=false;for(const auto& [address,loaded]:observed_words){if(comma)special<<',';comma=true;long long v=0;const bool canonical=loaded.second==vm::MemFaultCode::OK&&drawingIntegerValue(loaded.first,v);special<<"{\"address\":"<<address<<",\"read_fault\":"<<static_cast<int>(loaded.second)<<",\"integer_summary\":";if(canonical)special<<v;else special<<"null";special<<",\"word\":";if(loaded.second==vm::MemFaultCode::OK)transportExactWord(special,loaded.first);else special<<"null";special<<'}';}
  special<<"]}\n";flush_special();last_final_diagnostic_step=step;
 };
 auto value=[&](const vm::VMState&m,int owner,int address){const auto ctx=serviceKernelWord(m,390000+owner*9+5),pt=serviceKernelWord(m,static_cast<int>(ctx)+4);vm::PageTableEntry pte;
  const auto word=m.dmem.load(static_cast<int>(pt)+address/vm::MMU_PAGE_WORDS).first;
  if(!check(vm::decodePageTableEntry(word,pte)&&pte.present&&pte.user&&pte.read&&pte.write&&!pte.execute&&!pte.global&&!pte.superpage,"drawing actual owned readable RGB mapping"))return vm::TernaryValue::invalid(sandbox::TernaryMode::T40);
  return m.dmem.load(pte.ppn*vm::MMU_PAGE_WORDS+address%vm::MMU_PAGE_WORDS).first;};
 auto rgb=[&](const vm::VMState&m,int owner,int address,int w,int h){DrawingPixels out(w*h);for(int p=0;p<w*h;++p)for(int c=0;c<3;++c){const auto v=value(m,owner,address+p*3+c);long long number=0;check(drawingIntegerValue(v,number)&&number>=0&&number<=255,"actual integral RGB word");out[p][c]=static_cast<int>(number);}return out;};
 auto guard=[&](const vm::VMState&m,int owner){bool good=true;for(auto range:std::vector<std::pair<int,int>>{{64,96},{14496,14528},{14852,15000}})for(int p=range.first;p<range.second;++p)good &= drawingIntegerEquals(value(m,owner,base+p),-91);guards<<"{\"slot\":"<<owner<<",\"generation\":"<<transportProcessGeneration(m,owner)<<",\"all_guard_words_equal\":"<<(good?"true":"false")<<"}\n";return check(good,"all actual drawing guards");};
 auto special_source_equal=[&](const vm::VMState&m,int owner,int pid){bool equal=before_exception_source[pid].size()==324;for(int p=0;p<324&&equal;++p){const auto a=value(m,owner,base+14528+p),b=before_exception_source[pid][p];equal=a.mode==b.mode&&a.bits.lo==b.bits.lo&&a.bits.hi==b.bits.hi;}return check(equal,"exceptional child entire source unchanged exact typed words");};
 auto save=[&](const vm::VMState&m,int owner,int case_id){const auto output=rgb(m,owner,base+96,80,60);host::TosFramebufferSnapshot frame;frame.mode=host::TosFramebufferMode::GraphicsRGB;frame.width=80;frame.height=60;for(const auto&p:output)frame.rgba.push_back(host::detail::rgba(p[0],p[1],p[2]));std::string error;check(host::detail::writeFramebufferPng(dir/("rgb_"+std::to_string(case_id)+".png"),frame,&error),"actual RGB PNG "+error);pngs.insert(case_id);pixels<<"{\"case\":"<<case_id<<",\"actual_rgb\":[";bool comma=false;for(const auto&p:output)for(int x:p){if(comma)pixels<<',';comma=true;pixels<<x;}pixels<<"]}\n";return output;};
 TransportActor painter{"drawing_actor",readTextFile("apps/span_draw.trit")+"\n"+drawingGuest(cases),1,202};
 TransportActor survivor{"drawing_survivor",drawingSurvivor(),2,201};
 auto catalog=[&](const std::string& name,int pid,const vm::ExecutableImageHeaderV3& h,const std::map<std::string,int>& actual){headers[pid]=h;if(pid==202)labels=actual;std::ostringstream out;out<<"{\"actor\":\""<<name<<"\",\"pid\":"<<pid<<",\"data_words\":"<<h.data_words<<",\"stack_words\":"<<h.stack_words<<",\"labels\":{";bool comma=false;for(const auto&[n,pc]:actual){if(comma)out<<',';comma=true;out<<'"'<<jsonEscape(n)<<"\":"<<pc;}out<<"}}\n";return transportWriteArtifact(dir/(name+".catalog.json"),out.str());};
 const bool runtime=runTransportActors(dir,{painter,survivor},"",[&](vm::VMState&m,const vm::VMExecutionRecord&r,long long step){
  if(finished&&!actor_cleanup_complete&&last_final_diagnostic_step>=0&&step-last_final_diagnostic_step>=1000000)final_state(m,r,step,"periodic_1000000_steps");
  if(!r.attempted)return false;native_vm_executed=true;const bool eret=r.has_instruction&&r.instruction.opcode==isa::Opcode::ERET;const int pid=eret?static_cast<int>(serviceKernelWord(m,3020)):before_pid,s=eret?transportRuntimeSlot(m,pid):before_slot;const auto gen=eret?(s<0?0:transportProcessGeneration(m,s)):before_generation;const auto identity=std::make_tuple(pid,s,gen);
  const bool user=r.before_privilege==vm::PrivilegeMode::User&&r.has_instruction;if(user)native_user_executed=true;
  if(user&&(pid==421||pid==422)&&!before_exception.count(pid)&&r.instruction.opcode==isa::Opcode::MUL) {
    const auto& a=before_registers[r.instruction.rs1];const auto& b=before_registers[r.instruction.rs2];const auto result=m.regfile.read(r.instruction.rd);
    if(drawingIntegerEquals(a,3)||drawingIntegerEquals(b,3)) {
        const auto& input=drawingIntegerEquals(a,3)?b:a;++special_multiplies[pid];if(special_multiplies[pid]==1)check(drawingIntegerEquals(input,1),"actual finite initial scalar1 before multiply3 sequence");
        check(input.mode==TernaryMode::T40&&!input.isInvalid()&&result.mode==TernaryMode::T40&&!result.isInvalid()&&!r.trap_observed,"actual child scalar multiply3 remains valid");
        if(!input.isInvalid()&&!input.asTriple().isSpecial()&&!result.isInvalid()&&result.asTriple().isSpecial())finite_to_special[pid]=true;
        special<<"{\"event\":\"actual_User_MUL\",\"pid\":"<<pid<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"physical_pc\":"<<r.physical_pc<<",\"raw_bits\":"<<r.raw_bits<<",\"input\":";transportExactWord(special,input);special<<",\"result\":";transportExactWord(special,result);special<<",\"trap\":"<<(r.trap_observed?"true":"false")<<"}\n";
    }
  }
  if(user&&(pid==421||pid==422)&&special_interval[pid]&&r.instruction.opcode==isa::Opcode::CALL) {
    const auto target=labels.find(pid==421?"draw_rgb_span":"draw_rgb_line_reference");
    if(target!=labels.end()&&r.final_pc==target->second){long long lr=0;check(!child_called[pid]&&drawingIntegerValue(m.regfile.readLR(),lr)&&lr==r.pc+1,"actual special child enters drawing target through User CALL/LR25");child_called[pid]=true;special<<"{\"event\":\"actual_special_drawing_CALL\",\"pid\":"<<pid<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"target\":"<<r.final_pc<<",\"LR25\":"<<lr<<",\"raw_bits\":"<<r.raw_bits<<"}\n";}
  }
  const bool forbidden_write_interval=user&&base>0&&(special_interval[pid]||(pid==202&&region.active&&index<static_cast<int>(cases.size())&&cases[index].invalid));
  if(forbidden_write_interval) {
    auto check_address=[&](long long address){check(address<base+64||address>=base+15000,"unsupported input executes no User drawing/source/guard write, including identical-value stores");};
    const auto op=r.instruction.opcode;long long address=0;
    if(op==isa::Opcode::STORE||op==isa::Opcode::TSTR||op==isa::Opcode::VSTORE||op==isa::Opcode::VSCATTER) {
      const bool writes=op!=isa::Opcode::TSTR||drawingIntegerEquals(m.regfile.read(r.instruction.rd),1);
      if(writes) {
        check(drawingIntegerValue(before_registers[r.instruction.rs1],address),"actual User store base is typed finite integral");
        if(op==isa::Opcode::STORE)check_address(address+r.instruction.imm);
        if(op==isa::Opcode::TSTR)check_address(address);
        if(op==isa::Opcode::VSTORE)for(int lane=0;lane<before_vector_length;++lane)check_address(address+r.instruction.imm+lane);
        if(op==isa::Opcode::VSCATTER){check(r.instruction.rs2<8,"actual scatter address-vector valid");if(r.instruction.rs2<8)for(int lane=0;lane<before_vector_length;++lane){long long offset=0;check(drawingIntegerValue(before_vectors[r.instruction.rs2][lane],offset),"actual scatter offset finite integral");check_address(address+offset);}}
      }
      special<<"{\"event\":\"actual_User_write_scan\",\"pid\":"<<pid<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"physical_pc\":"<<r.physical_pc<<",\"opcode\":"<<static_cast<int>(op)<<",\"raw_bits\":"<<r.raw_bits<<",\"trap_observed\":"<<(r.trap_observed?"true":"false")<<"}\n";
    }
  }
  if(user&&r.trap_observed&&r.trap_cause!=-1&&!(r.has_instruction&&r.instruction.opcode==isa::Opcode::SYSCALL)) {
    check((pid==421||pid==422)&&before_exception.count(pid)&&r.trap_cause==1,"only actual special offender may fault; all ordinary finite invalids must return -8 before writes");
  }
  if(region.active){if((user&&pid==202)||r.before_privilege==vm::PrivilegeMode::Kernel||(r.trap_observed&&r.trap_cause==-1))counters<<"{\"case\":"<<index<<",\"pid\":"<<pid<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"physical_pc\":"<<r.physical_pc<<",\"final_pc\":"<<r.final_pc<<",\"raw_bits\":"<<r.raw_bits<<",\"before_privilege\":"<<static_cast<int>(r.before_privilege)<<",\"cycle_before\":"<<r.cycle_before<<",\"cycle_after\":"<<r.cycle_after<<"}\n";const auto cost=r.cycle_after-r.cycle_before;if(user&&pid==202){region.user_cycles+=cost;++region.instructions;}else if(r.before_privilege==vm::PrivilegeMode::Kernel||(r.trap_observed&&r.trap_cause==-1))region.kernel_cycles+=cost;}
  if(user&&(r.instruction.opcode==isa::Opcode::CALL||r.instruction.opcode==isa::Opcode::CALLR)&&pid==202&&region.active){++region.depth;long long nested_lr=0;check(drawingIntegerValue(m.regfile.readLR(),nested_lr)&&nested_lr==r.pc+1,"actual nested CALL/CALLR LR25");call_returns.push_back(static_cast<int>(nested_lr));}
  if(user&&r.instruction.opcode==isa::Opcode::CALL&&pid==202&&!region.active&&index<static_cast<int>(cases.size())){const auto& c=cases[index];const auto function=c.kind=="line"?"draw_rgb_line_"+c.algorithm:c.kind=="span"?"draw_rgb_span":"draw_rgb_copy";const auto entry=labels.find(function);if(entry!=labels.end()&&r.final_pc==entry->second){check(!region.active,"one top-level drawing region");if(!c.invalid)check(drawingIntegerEquals(before_registers[13],base+96)&&drawingIntegerEquals(before_registers[14],c.destination_width)&&drawingIntegerEquals(before_registers[15],c.destination_height),"actual User CALL buffer/width/height matches independent valid case metadata");drawing_call_executed=true;region={true,1,static_cast<int>(vm::ops::toLong(m.regfile.readLR())),gen,r.cycle_after-r.cycle_before,0,1};call_returns.push_back(region.lr);check(region.lr==r.pc+1,"actual drawing CALL LR/EPC");counters<<"{\"event\":\"actual_outer_CALL\",\"case\":"<<index<<",\"pid\":"<<pid<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"target_pc\":"<<r.final_pc<<",\"actual_arg_width\":"<<vm::ops::toLong(before_registers[14])<<",\"actual_arg_height\":"<<vm::ops::toLong(before_registers[15])<<",\"LR25\":"<<region.lr<<",\"raw_bits\":"<<r.raw_bits<<",\"before_privilege\":"<<static_cast<int>(r.before_privilege)<<",\"cycle_before\":"<<r.cycle_before<<",\"cycle_after\":"<<r.cycle_after<<"}\n";}}
  if(user&&r.instruction.opcode==isa::Opcode::RET&&pid==202&&region.active){check(region.gen==gen,"drawing CALL/RET same actual lifetime");check(region.depth>0&&!call_returns.empty(),"drawing balanced nested RET");if(!call_returns.empty()){check(r.final_pc==call_returns.back(),"actual nested/outer RET matches its CALL LR25");call_returns.pop_back();}if(--region.depth==0){check(r.final_pc==region.lr,"actual outer RET target matches captured LR25");region.active=false;measured[index]=region;}}
  if(user&&r.instruction.opcode==isa::Opcode::SYSCALL){
   if((pid==202||pid==201)&&(r.syscall_id==23||r.syscall_id==54)){
    if(finished&&final_ipc_pending.count(identity)){special<<"{\"event\":\"actual_final_ipc_entry_overlaps_pending\",\"pid\":"<<pid<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"prior_entry_pc\":"<<final_ipc_pending.at(identity).pc<<",\"new_entry_pc\":"<<r.pc<<",\"step\":"<<step<<"}\n";flush_special();}FinalIpcPending p;p.pc=r.pc;p.service=r.syscall_id;p.step=step;for(int n=0;n<4;++n)p.args[n]=before_registers[13+n];final_ipc_pending[identity]=p;
    if(finished){special<<"{\"event\":\"actual_final_ipc_entry\",\"pid\":"<<pid<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"service\":"<<p.service<<",\"entry_pc\":"<<p.pc<<",\"return_observed\":false,\"args_exact\":[";for(int n=0;n<4;++n){if(n)special<<',';transportExactWord(special,p.args[n]);}special<<"]}\n";flush_special();}
   }
   if(region.active&&pid==202)check(false,"drawing helper must not syscall/allocate/GPU replace");
   if(r.syscall_id==19||r.syscall_id==43){check(!pending.count(identity),"unique pending allocation/wait per actual lifetime");pending[identity]={r.pc,r.syscall_id,r.syscall_arg0};}
   if(pid==201&&r.syscall_id==7&&sbase>0){check(drawingIntegerEquals(value(m,s,sbase),-91)&&drawingIntegerEquals(value(m,s,sbase+95),91),"surviving guest guards");check(drawingExactEquals(value(m,s,sbase+2),native_ops::divide(native_ops::fromIntT40(1),native_ops::fromIntT40(3))),"surviving guest exact fraction");++survivor_progress;}
   if(r.syscall_id==44){if(pid==421||pid==422){++child_exits[pid];check(exception_return[pid]&&!exception_fault[pid]&&r.syscall_arg0==45,"only explicit -8 child may execute one exit45");}if(pid==202)parent_exit=r.syscall_arg0==0;if(pid==201)super_exit=r.syscall_arg0==0;
    if(pid==202||pid==201){check(r.syscall_arg0==0&&s==(pid==202?1:2)&&gen>0&&!actor_exit_identity.count(pid),"actual actor zeroexit once in its owned slot/generation");actor_exit_identity[pid]={s,gen};special<<"{\"event\":\"actual_actor_zeroexit\",\"pid\":"<<pid<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"status\":"<<r.syscall_arg0<<"}\n";flush_special();}
   }
   if(r.syscall_id==7&&base>0&&(pid==202||pid==421||pid==422)){
    long long phase=0;check(drawingIntegerValue(value(m,s,base),phase),"actual control phase exact typed integer");
    if(pid==202&&phase==1){check(vm::ops::toLong(value(m,s,base+31))==71,"real survivor prefix IPC");}
    if(pid==202&&phase>=10&&phase<10+2*static_cast<long long>(cases.size())){const int n=static_cast<int>((phase-10)/2);const auto& c=cases[n];
     if((phase-10)%2==0){check(n==index&&++starts[n]==1,"ordered unique actual drawing begin");if(c.reset)expected=drawingPattern(80,60,0);check(rgb(m,s,base+96,80,60)==expected&&guard(m,s),"actual precall full buffer and guards");}
     else {const auto count=drawingOracle(expected,source,c);long long actual=0;check(drawingIntegerValue(value(m,s,base+2),actual),"actual draw result exact finite integral T40");const auto output=save(m,s,n);int differences=0;for(size_t p=0;p<output.size();++p)differences+=output[p]!=expected[p];check(!region.active&&measured[n].instructions>0,"actual completed drawing CALL/RET region");check(actual==count&&differences==0&&guard(m,s)&&rgb(m,s,base+14528,12,9)==source,"full independent directed/snapshot pixels,count,source,guards");trace<<"{\"case\":\""<<c.name<<"\",\"pid\":"<<pid<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"expected_count\":"<<count<<",\"actual_count\":"<<actual<<",\"different_pixels\":"<<differences<<"}\n";if(c.benchmark)bench<<"{\"case\":\""<<c.name<<"\",\"algorithm\":\""<<c.algorithm<<"\",\"repetition\":"<<c.repetition<<",\"User_call_ret_cycles\":"<<measured[n].user_cycles<<",\"User_instructions\":"<<measured[n].instructions<<",\"excluded_kernel_cycles\":"<<measured[n].kernel_cycles<<",\"pixels_equal\":"<<(differences==0?"true":"false")<<"}\n";++index;}
    }
    if(pid!=202&&phase==100000+pid-421){special_interval[pid]=true;check(special_multiplies[pid]==1200&&finite_to_special[pid],"actual finite arithmetic to valid special executed before drawing");before_exception[pid]=rgb(m,s,base+96,80,60);progress_before[pid]=survivor_progress;const auto v=value(m,s,base+7);check(v.mode==sandbox::TernaryMode::T40&&!v.isInvalid()&&v.asTriple().isSpecial(),"actual finite-arithmetic-defined valid special");for(int p=0;p<324;++p)before_exception_source[pid].push_back(value(m,s,base+14528+p));special<<"{\"event\":\"actual_valid_special_prefix\",\"pid\":"<<pid<<",\"input\":";transportExactWord(special,v);special<<"}\n";}
    if(pid!=202&&phase==100010+pid-421){special_interval[pid]=false;exception_return[pid]=true;exception_safe[pid]=child_called[pid]&&before_exception.count(pid)&&rgb(m,s,base+96,80,60)==before_exception[pid]&&guard(m,s)&&special_source_equal(m,s,pid);check(drawingIntegerEquals(value(m,s,base+2),-8)&&exception_safe[pid],"actual nonthrowing special rejection before write");}
    if(pid==202&&phase>=110000&&phase<110002){const int child=421+static_cast<int>(phase-110000);check(wait_returns[child]==1&&exception_safe[child]&&(exception_fault[child]?(exception_crashed[child]&&child_exits[child]==0):(exception_return[child]&&child_exits[child]==1)),"actual isolated special wait once with offender-only cause/exit");check(rgb(m,s,base+96,80,60)==expected&&guard(m,s)&&rgb(m,s,base+14528,12,9)==source,"parent drawing/source survives exceptional child");check(serviceKernelWord(m,390000+(21+child-421)*9+2)==0,"actual wait reaped offender child");}
    if(pid==202&&phase>=120000&&phase<120002){const int child=421+static_cast<int>(phase-120000);check(survivor_progress>progress_before[child]&&vm::ops::toLong(value(m,s,base+31))==72+child-421,"real surviving guest progress after exceptional wait");}
    if(pid==202&&phase==999999){long long checksum=0;for(int n=0;n<18;++n){const auto v=vm::ops::toLong(value(m,s,base+10+n));check(drawingIntegerEquals(value(m,s,base+10+n),202+n)&&v==202+n,"18 live values across ABI-pressure drawing calls");checksum+=v;}check(checksum==3789&&index==static_cast<int>(cases.size())&&pngs.size()==cases.size(),"complete drawing sequence and caller liveness");finished=true;if(last_final_diagnostic_step<0)final_state(m,r,step,"final_marker_999999");}
   }
  }
  if(user&&r.trap_observed&&r.trap_cause!=7&&r.trap_cause!=-1&&(pid==421||pid==422)){special_interval[pid]=false;exception_fault[pid]=true;exception_safe[pid]=child_called[pid]&&before_exception.count(pid)&&rgb(m,s,base+96,80,60)==before_exception[pid]&&guard(m,s)&&special_source_equal(m,s,pid);check(r.trap_cause==1&&exception_safe[pid],"actual exceptional comparison fault before RGB write");special<<"{\"event\":\"actual_User_fault\",\"pid\":"<<pid<<",\"pc\":"<<r.pc<<",\"physical_pc\":"<<r.physical_pc<<",\"actual_raw_bits\":"<<r.raw_bits<<",\"cause\":"<<r.trap_cause<<",\"attempted\":true,\"has_instruction\":true}\n";}
  if(eret&&final_ipc_pending.count(identity)&&m.pc==final_ipc_pending.at(identity).pc+1){const auto p=final_ipc_pending.at(identity);final_ipc_pending.erase(identity);
   if(finished){special<<"{\"event\":\"actual_final_ipc_matched_ERET\",\"pid\":"<<pid<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"step\":"<<step<<",\"service\":"<<p.service<<",\"entry_pc\":"<<p.pc<<",\"return_pc\":"<<m.pc<<",\"entry_step\":"<<p.step<<",\"args_exact\":[";for(int n=0;n<4;++n){if(n)special<<',';transportExactWord(special,p.args[n]);}special<<"],\"tuple_exact\":[";for(int n=13;n<=15;++n){if(n>13)special<<',';transportExactWord(special,m.regfile.read(n));}special<<"]}\n";flush_special();}
  }
  if(r.has_instruction&&r.instruction.opcode==isa::Opcode::ERET&&pending.count(identity)&&m.pc==pending[identity].pc+1){const auto p=pending[identity];pending.erase(identity);long long a=0,b=0,c=0;check(drawingIntegerValue(m.regfile.read(13),a)&&drawingIntegerValue(m.regfile.read(14),b)&&drawingIntegerValue(m.regfile.read(15),c),"matched raw tuple mode/invalid/special/integrality before accepting summaries");raw<<"{\"event\":\"actual_matched_ERET\",\"pid\":"<<pid<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"service\":"<<p.service<<",\"entry_pc\":"<<p.pc<<",\"return_pc\":"<<m.pc<<",\"tuple\":["<<a<<','<<b<<','<<c<<"]}\n";
   check(drawingIntegerEquals(m.regfile.read(13),a)&&drawingIntegerEquals(m.regfile.read(14),b)&&drawingIntegerEquals(m.regfile.read(15),c),"matched raw tuple is exact finite integral T40 before accepting summaries");
   if(p.service==19){const int n=alloc_calls[pid]++;const auto& h=headers.at(pid);const int initial=((std::max(h.data_words,h.stack_words)+728)/729)*729,words=pid==202?15000:96;check(a==1&&c==0&&b==(n<2?initial:initial+words)&&p.arg==(n==1?words:0),"actual header-derived query/grow/query raw tuple");if(n==0)heapbase[pid]=b;if(n==2){if(pid==202){base=static_cast<int>(b-15000);slot=s;}else sbase=static_cast<int>(b-96);const auto context=serviceKernelWord(m,390000+s*9+5),pt=serviceKernelWord(m,static_cast<int>(context)+4);placement<<"{\"pid\":"<<pid<<",\"slot\":"<<s<<",\"data_words\":"<<h.data_words<<",\"stack_words\":"<<h.stack_words<<",\"derived_initial\":"<<initial<<",\"actual_end\":"<<b<<",\"heap\":[";for(int k=0;k<4;++k){if(k)placement<<',';placement<<serviceKernelWord(m,668000+s*4+k);}placement<<"],\"PTEs\":[";for(int vpn=initial/729;vpn<(b+728)/729;++vpn){if(vpn!=initial/729)placement<<',';const auto w=m.dmem.load(static_cast<int>(pt)+vpn).first;vm::PageTableEntry e;check(vm::decodePageTableEntry(w,e)&&e.present&&e.user&&e.read&&e.write&&!e.execute&&!e.global&&!e.superpage&&e.ppn==1000+s*192+vpn,"actual canonical private allocation PTE including scratch replacements");transportExactWord(placement,w);}placement<<"]}\n";const auto heap=668000+s*4;check(serviceKernelWord(m,heap)==initial&&serviceKernelWord(m,heap+1)==b&&serviceKernelWord(m,heap+2)==74*729&&serviceKernelWord(m,heap+3)==(b+728)/729,"actual four heap metadata words canonical");long long sp=0;check(drawingIntegerValue(m.regfile.readSP(),sp)&&sp>0&&sp<=h.stack_words&&initial>=h.stack_words&&initial>=h.data_words&&b<=74*729,"actual User SP/header stack/data disjoint from bounded full allocation");
    if(pid==201){check(!supervisor_identity_captured&&s==2&&gen>0,"capture survivor owned allocation identity once");supervisor_initial_slot=s;supervisor_initial_generation=gen;
     for(int n=0;n<9;++n)check(drawingIntegerValue(m.dmem.load(dc("PROCESS_BASE")+s*dc("PROCESS_ROW_WORDS")+n).first,supervisor_initial_row[n]),"actual initial survivor row exact finite integral T40");
     check(drawingIntegerValue(m.dmem.load(dc("PROC_CAPS_BASE")+s).first,supervisor_initial_caps),"actual initial survivor caps exact finite integral T40");
     parent_initial_slot=transportRuntimeSlot(m,1);parent_initial_generation=parent_initial_slot<0?0:transportProcessGeneration(m,parent_initial_slot);
     check(supervisor_initial_row[dc("PROC_PID")]==201&&serviceKernelWord(m,dc("PROC_PARENT_PID_BASE")+s)==1&&parent_initial_slot>=0&&parent_initial_generation>0,"actual survivor owned identity retains live bootstrap parentPID1");supervisor_identity_captured=ok;
     special<<"{\"event\":\"actual_survivor_owned_identity\",\"step\":"<<step<<",\"slot\":"<<s<<",\"generation\":"<<gen<<",\"parent_pid\":1,\"parent_slot\":"<<parent_initial_slot<<",\"parent_generation\":"<<parent_initial_generation<<",\"process_row\":[";for(int n=0;n<9;++n){if(n)special<<',';special<<supervisor_initial_row[n];}special<<"],\"caps\":"<<supervisor_initial_caps<<"}\n";
    }
   }}
   else {const int child=static_cast<int>(p.arg);++wait_returns[child];const int result=exception_fault[child]?1:45;check(a==1&&b==result&&c==0&&wait_returns[child]==1,"actual canonical special wait tuple/EPC+1 once");}
  }
  for(int child:{421,422})if(exception_fault[child]){const int child_slot=21+child-421;if(serviceKernelWord(m,390000+child_slot*9+2)==9){check(serviceKernelWord(m,130100+child_slot)==1,"only actual exceptional offender CRASHED cause1");exception_crashed[child]=true;special<<"{\"event\":\"actual_offender_CRASHED\",\"pid\":"<<child<<",\"slot\":"<<child_slot<<",\"generation\":"<<transportProcessGeneration(m,child_slot)<<",\"step\":"<<step<<",\"exit_code\":"<<serviceKernelWord(m,130100+child_slot)<<"}\n";}}
  // The fixture's survivor is retained for live bootstrap parentPID1.
  // Observe full resource/wait cleanup before accepting its ZOMBIE row.
  if(ok&&finished&&parent_exit&&super_exit&&!actor_cleanup_complete){
   struct ActorCleanup {std::array<long long,9> row{};std::array<long long,4> heap{};long long parent=0,caps=0,status=0,wait_channel=0,wait_kind=0,deadline=0,next=0,cleaned=0,generation=0;};
   std::array<ActorCleanup,2> cleanup;bool cleanup_ready=supervisor_identity_captured,metadata_integral=true;
   auto observed_word=[&](int address){long long v=0;const auto loaded=m.dmem.load(address);metadata_integral=metadata_integral&&loaded.second==vm::MemFaultCode::OK&&drawingIntegerValue(loaded.first,v);return v;};
   for(int n=0;n<2;++n){const int actor_slot=1+n;auto& c=cleanup[n];
    for(int word=0;word<9;++word)c.row[word]=observed_word(dc("PROCESS_BASE")+actor_slot*dc("PROCESS_ROW_WORDS")+word);
    for(int word=0;word<4;++word){c.heap[word]=observed_word(dc("PROCESS_HEAP_BASE")+actor_slot*dc("PROCESS_HEAP_WORDS")+word);cleanup_ready=cleanup_ready&&c.heap[word]==0;}
    c.parent=observed_word(dc("PROC_PARENT_PID_BASE")+actor_slot);c.caps=observed_word(dc("PROC_CAPS_BASE")+actor_slot);c.status=observed_word(dc("PROC_EXIT_STATUS_BASE")+actor_slot);c.cleaned=observed_word(dc("PROC_CLEANED_BASE")+actor_slot);c.wait_channel=observed_word(dc("WAIT_CHANNEL_BASE")+actor_slot);c.wait_kind=observed_word(dc("WAIT_KIND_BASE")+actor_slot);c.deadline=observed_word(dc("WAIT_DEADLINE_BASE")+actor_slot);c.next=observed_word(dc("WAIT_PROC_NEXT_BASE")+actor_slot);c.generation=observed_word(dc("GFX_METADATA_BASE")+dc("GFX_PROC_GENERATION_BASE")+actor_slot);
    cleanup_ready=cleanup_ready&&c.status==0&&c.row[dc("PROC_WAIT_CHANNEL")]==0&&c.wait_channel==0&&c.wait_kind==0&&c.deadline==-1&&c.next==-1;
   }
   const auto& painter_cleanup=cleanup[0];const auto& survivor_cleanup=cleanup[1];
   for(long long word:painter_cleanup.row)cleanup_ready=cleanup_ready&&word==0;
   cleanup_ready=cleanup_ready&&painter_cleanup.parent==-1&&painter_cleanup.caps==0&&painter_cleanup.cleaned==0;
   cleanup_ready=cleanup_ready&&survivor_cleanup.row[dc("PROC_PID")]==201&&survivor_cleanup.row[dc("PROC_STATE")]==dc("PROC_ZOMBIE")&&survivor_cleanup.parent==1&&survivor_cleanup.cleaned==1&&survivor_cleanup.caps==supervisor_initial_caps&&supervisor_initial_slot==2&&survivor_cleanup.generation==supervisor_initial_generation;
   for(int field:{dc("PROC_NAMESPACE"),dc("PROC_PRIORITY"),dc("PROC_QUOTA"),dc("PROC_CONTEXT"),dc("PROC_VECTOR_CONTEXT")})cleanup_ready=cleanup_ready&&survivor_cleanup.row[field]==supervisor_initial_row[field];
   const int live_parent_slot=transportRuntimeSlot(m,1);const auto live_parent_state=live_parent_slot<0?0:observed_word(dc("PROCESS_BASE")+live_parent_slot*dc("PROCESS_ROW_WORDS")+dc("PROC_STATE"));
   std::array<long long,9> live_parent_row{};if(live_parent_slot>=0)for(int word=0;word<9;++word)live_parent_row[word]=observed_word(dc("PROCESS_BASE")+live_parent_slot*dc("PROCESS_ROW_WORDS")+word);
   cleanup_ready=cleanup_ready&&live_parent_slot==parent_initial_slot&&live_parent_slot>=0&&observed_word(dc("GFX_METADATA_BASE")+dc("GFX_PROC_GENERATION_BASE")+live_parent_slot)==parent_initial_generation&&live_parent_row[dc("PROC_PID")]==1&&live_parent_state>dc("PROC_FREE")&&live_parent_state!=dc("PROC_EXITED")&&live_parent_state!=dc("PROC_ZOMBIE")&&live_parent_state!=dc("PROC_CRASHED");
   cleanup_ready=cleanup_ready&&metadata_integral;
   if(cleanup_ready){
    check(actor_exit_identity.size()==2,"both actual zeroexit actor lifetimes captured before final cleanup");
    for(int n=0;n<2;++n){const int actor_pid=n==0?202:201;const auto found=actor_exit_identity.find(actor_pid);check(found!=actor_exit_identity.end()&&found->second.first==1+n&&found->second.second==cleanup[n].generation,"final actor cleanup matches actual zeroexit slot/generation");}
    check(cleanup_ready,"canonical FREE painter and cleaned retained ZOMBIE survivor for live parentPID1, complete heap/wait cleanup");
    if(ok){actor_cleanup_complete=true;
    special<<"{\"event\":\"actual_actor_cleanup_complete\",\"step\":"<<step<<",\"finished\":true,\"both_actual_zeroexits\":true,\"survivor_retained_for_parent_wait\":true,\"live_parent\":{\"pid\":1,\"slot\":"<<live_parent_slot<<",\"generation\":"<<parent_initial_generation<<",\"process_row\":[";
    for(int word=0;word<9;++word){if(word)special<<',';special<<live_parent_row[word];}special<<"]},\"actors\":[";
    for(int n=0;n<2;++n){if(n)special<<',';const auto& c=cleanup[n];special<<"{\"pid\":"<<(n==0?202:201)<<",\"slot\":"<<1+n<<",\"generation\":"<<c.generation<<",\"process_row\":[";for(int word=0;word<9;++word){if(word)special<<',';special<<c.row[word];}special<<"],\"parent\":"<<c.parent<<",\"caps\":"<<c.caps<<",\"exit_status\":"<<c.status<<",\"cleaned\":"<<c.cleaned<<",\"wait_channel\":"<<c.wait_channel<<",\"wait_kind\":"<<c.wait_kind<<",\"deadline\":"<<c.deadline<<",\"wait_proc_next\":"<<c.next<<",\"heap\":[";for(int word=0;word<4;++word){if(word)special<<',';special<<c.heap[word];}special<<"]}";}
    special<<"]}\n";flush_special();
    }
   }
  }
  return !ok||actor_cleanup_complete;
 },"",false,[&](const vm::VMState&m){before_pid=static_cast<int>(serviceKernelWord(m,3020));before_slot=transportRuntimeSlot(m,before_pid);before_generation=before_slot<0?0:transportProcessGeneration(m,before_slot);if(m.privilege==vm::PrivilegeMode::User){for(int n=0;n<32;++n)before_registers[n]=m.regfile.read(n);if(special_interval[before_pid]||(before_pid==202&&region.active&&index<static_cast<int>(cases.size())&&cases[index].invalid)){before_vector_length=m.vector_length;for(int v=0;v<8;++v)for(int lane=0;lane<27;++lane)before_vectors[v][lane]=m.vregfile.reg[v].lane[lane];}}},false,true,{},[](const vm::VMState&,const vm::VMExecutionRecord&){return true;},catalog,750000000);
 trace.flush();raw.flush();bench.flush();placement.flush();pixels.flush();guards.flush();special.flush();counters.flush();check(trace.good()&&raw.good()&&bench.good()&&placement.good()&&pixels.good()&&guards.good()&&special.good()&&counters.good(),"drawing all trace writes closed");trace.close();raw.close();bench.close();placement.close();pixels.close();guards.close();special.close();counters.close();check(!trace.fail()&&!raw.fail()&&!bench.fail()&&!placement.fail()&&!pixels.fail()&&!guards.fail()&&!special.fail()&&!counters.fail(),"all drawing traces checked after close");
 check(runtime&&finished&&parent_exit&&super_exit&&actor_cleanup_complete&&alloc_calls[202]==3&&alloc_calls[201]==3&&wait_returns[421]==1&&wait_returns[422]==1,"all real drawing lifetimes complete");
 const bool drawing_sequence_completed=runtime&&finished&&parent_exit&&super_exit&&actor_cleanup_complete&&alloc_calls[202]==3&&alloc_calls[201]==3&&wait_returns[421]==1&&wait_returns[422]==1;
 check(drawingSha(dir,"source_hashes_after.json"),"drawing final SHA receipt");
 check(readTextFile((dir/"source_hashes_before.json").string())==readTextFile((dir/"source_hashes_after.json").string()),"drawing runtime source freeze");
 check(transportWriteArtifact(dir/"benchmark_settings.json",lifecycle_only?"{\"executed\":false,\"warm\":false,\"reason\":\"focused lifecycle; no warmup or benchmark cases\"}\n":"{\"backend\":\"vm::step compiled native User/kernel\",\"jit_claim\":false,\"warm\":true,\"preallocated\":true,\"order\":\"AB/BA6 per four workloads\",\"region\":\"actual User CALL through matching RET including nested calls; argument staging/result store/marker SYS/kernel/host excluded\"}\n"),"benchmark settings");
 check(transportWriteArtifact(dir/"selection.json","{\"automatic_selection_accepted\":false,\"reason\":\"only measured geometry/color/workload records exist; root must accept winning ranges after execution; no extrapolation\"}\n"),"selection limits");
 std::ostringstream commands;commands<<"{\"native_VM_executed\":"<<(native_vm_executed?"true":"false")<<",\"actual_selector\":\""<<selector<<"\",\"native_User_executed\":"<<(native_user_executed?"true":"false")<<",\"drawing_CALL_executed\":"<<(drawing_call_executed?"true":"false")<<",\"runner_completed\":"<<(drawing_sequence_completed?"true":"false")<<",\"shell_exit_verified\":false,\"actual_outer_outcome\":\"recorded by coupled completion after all required evidence closes\"}\n";
 check(transportWriteArtifact(dir/"commands.json",commands.str()),"actual workflow invocation receipt");
 std::ostringstream criteria;criteria<<"{\"native_VM_executed\":"<<(native_vm_executed?"true":"false")<<",\"native_User_executed\":"<<(native_user_executed?"true":"false")<<",\"drawing_CALL_executed\":"<<(drawing_call_executed?"true":"false")<<",\"runner_completed\":"<<(drawing_sequence_completed?"true":"false")<<",\"protected_Stage2_integration\":false,\"Stage4_accepted\":false,\"criterion_ids\":"<<(lifecycle_only?"[\"api_pressure\",\"special_span_child\",\"special_line_child\",\"survivor_progress\",\"caller_liveness\"]":"[\"D1\",\"D2\",\"D3\",\"D4\",\"D5\"]")<<",\"assertions\":\"assertions.jsonl\",\"final_outcome\":\"report.json and actual coupled completion\"}\n";
 check(transportWriteArtifact(dir/"criteria_map.json",criteria.str()),"criteria receipt");
 std::ostringstream coverage;coverage<<"{\"case_count\":"<<cases.size()<<",\"actual_completed\":"<<index<<",\"expected_png\":[";for(size_t n=0;n<cases.size();++n){if(n)coverage<<',';coverage<<"\"rgb_"<<n<<".png\"";}coverage<<"],\"case_names\":[";for(size_t n=0;n<cases.size();++n){if(n)coverage<<',';coverage<<'"'<<jsonEscape(cases[n].name)<<'"';}coverage<<"]}\n";
 check(transportWriteArtifact(dir/"case_inventory.json",coverage.str()),"actual case and exact required PNG inventory");
 for(size_t n=0;n<cases.size();++n)check(pngs.count(static_cast<int>(n))&&std::filesystem::is_regular_file(dir/("rgb_"+std::to_string(n)+".png")),"every planned case produced actual PNG");
 assertions.flush();check(assertions.good(),"drawing assertions flush");assertions.close();check(!assertions.fail(),"drawing assertions close");
 check(drawingInventory(dir),"closed payload SHA256 inventory");
 check(drawingOwnedEvidence(dir,cases.size(),lifecycle_only),"all owned required files and exact PNG set closed before report");
 // Publication is last. The inventory seals all payload files except itself and
 // report.json; the existing coupled completion hashes both final files and the
 // stable summary, then records the actual outer return. No PASS is settled
 // before all payload writers have completed.
 const std::string status=ok&&runtime?"PASS":(native_vm_executed?"FAIL":"INCOMPLETE");
 const bool published=publish(status.c_str(),native_vm_executed,lifecycle_only?"drawing lifecycle result; full raster matrix, benchmarks, protected integration and Stage4 remain pending":"drawing-only result; protected integration and full Stage4 remain pending");
 if(!published){publish("FAIL",native_vm_executed,"final report publication failed; coupled completion must FAIL");return false;}
 return ok&&runtime;
}


std::string transportPathStores(int address,const std::string& path) {
    std::string source;
    for(size_t i=0;i<=path.size();++i)source+="unsafe { store("+std::to_string(address+i)+","+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"); }\n";
    return source;
}

std::string transportGuestCommon() {
    return R"TRIT(
fn tr_check(actual: t40, expected: t40, code: t40) -> t40 {
    unsafe {
        store(10042,load(10042)+1);
        if actual != expected { store(10040,load(10040)+1); if load(10041)==0 { store(10041,code); } }
    }
    return actual;
}

fn tr_positive(actual: t40, code: t40) -> t40 {
    if actual <= 0 { tr_check(actual,1,code); }
    return actual;
}
fn tr_send(channel: t40, value: t40) -> t40 {
    return tr_positive(os_ipc_send(channel,value),9901);
}
fn tr_recv(channel: t40) -> t40 {
    tr_check(os_ipc_recv_blocking(channel,10400,-1),1,9902);
    unsafe { return load(10400); }
}
fn tr_wait_blocked(pid: t40) -> t40 {
    var i: t40=0;
    while i < 100 {
        let n: t40=os_getproc(pid,10500);
        if n > 0 { unsafe { if load(10501)==3 { return 1; } } }
        os_sleep_ms(1); i=i+1;
    }
    tr_check(0,1,9903); return 0;
}
fn tr_reply_prepare(record: t40, reply: t40, status: t40, payload: t40, detail: t40) -> t40 {
    unsafe {
        store(reply,load(record)); store(reply+1,load(record+2));
        store(reply+2,load(record+3)); store(reply+3,load(record+4));
        store(reply+4,load(record+5)); store(reply+5,load(record+6));
        store(reply+6,load(record+7)); store(reply+7,status);
        store(reply+8,payload); store(reply+9,detail);
    }
    return 1;
}
fn tr_record_request(session: t40, record: t40) -> t40 {
    unsafe {
        tr_check(load(record),1,9801); tr_check(load(record+1),1,9802);
        tr_check(load(record+2),session,9803); tr_positive(load(record+4),9804);
        tr_positive(load(record+5),9805); tr_positive(load(record+7),9806);
        tr_check(load(record+8),4,9807); tr_check(load(record+9),-73,9808);
        tr_check(load(record+10),17,9809); tr_check(load(record+11),1/3,9810);
        tr_check(load(record+12),0,9811); tr_check(load(record+13),-9,9812);
        tr_check(load(record+14),200000,9813); tr_check(load(record+15),73,9814);
    }
    return 1;
}
)TRIT";
}

std::string transportClientSource(int index) {
    const int ready=30+index,channel=10+index;
    std::string body=transportGuestCommon()+"fn main() -> t40 { let session: t40=tr_recv("+std::to_string(channel)+");\n";
    body+="let joined: t40=os_gfx_join(session,11000,8); tr_check(joined,"+std::string(index==8?"-3":"session")+",1001);\n";
    body+=R"TRIT(unsafe { store(10000,session); store(10001,joined); }
        if joined != session { if joined != -3 { unsafe { store(10090,-777); } os_exit(1); return 0; } }
    )TRIT";
    body+="tr_send("+std::to_string(ready)+",1);\n";
    body+=R"TRIT(
        var command: t40=1;
        while command > 0 {
    )TRIT";
    body+="command=tr_recv("+std::to_string(channel)+");\n";
    body+=R"TRIT(
            unsafe { store(11100,4); store(11101,-73); store(11102,17); store(11103,1/3); store(11104,0); store(11105,-9); store(11106,200000); store(11107,73); }
            var expected: t40=-999;
            if command==1 { expected=-73; }
            if command==2 { expected=-12; }
            if command==3 { expected=-11; }
            if command==4 { expected=-3; }
            if command==11 { expected=-5; }
            if command==12 { expected=-12; }
            if command==13 { expected=0; }
            if expected != -999 {
                var reply_out: t40=11200;
                if command==11 { reply_out=12500; var g: t40=0; while g < 10 { unsafe { store(reply_out+g,619+g); } g=g+1; } }
                let result: t40=os_gfx_submit(session,11100,8,reply_out);
                tr_check(result,expected,1100+command);
                unsafe { store(10002,result); store(10003,load(10003)+1); }
                if command==1 {
                    unsafe { tr_check(load(11200),1,1121); tr_check(load(11201),session,1122); tr_check(load(11202),sys_getpid(),1123); tr_check(load(11207),1,1124); tr_check(load(11208),-73,1125); tr_check(load(11209),0,1126); }
                }
            }
            if command==5 {
                tr_check(os_gfx_leave(session),1,1201); tr_check(os_gfx_leave(session),-8,1202);
                tr_check(os_gfx_join(session,11020,8),-8,1203); tr_check(os_gfx_submit(session,11100,8,11200),-8,1204);
            }
            if command==6 {
                tr_check(os_gfx_register(11020,8),-8,1301);
                tr_check(os_gfx_receive(session,11300,16,0),-8,1302);
                tr_check(os_gfx_reply(session,11400,10),-8,1303);
                tr_check(os_gfx_join(session,11020,8),session,1304);
                unsafe { store(11019,317); store(11028,-317); store(11020,619); var g: t40=0; while g < 4 { store(16034+g,713+g); g=g+1; } }
                tr_check(os_gfx_join(session,11020,7),-8,1311); tr_check(os_gfx_join(session,11020,9),-8,1312);
                tr_check(os_gfx_join(session,11020,-1),-8,1313); tr_check(os_gfx_join(session,11020,8/3),-8,1314);
                tr_check(os_gfx_join(session,-1,8),-5,1315); tr_check(os_gfx_join(session,11020+1/3,8),-5,1316);
                tr_check(os_gfx_join(session,16034,8),-5,1317);
                tr_check(os_gfx_submit(session,11100,7,11200),-8,1321); tr_check(os_gfx_submit(session,11100,9,11200),-8,1322);
                tr_check(os_gfx_submit(session,11100,-1,11200),-8,1323); tr_check(os_gfx_submit(session,11100,8/3,11200),-8,1324);
                tr_check(os_gfx_submit(session,-1,8,11200),-5,1325); tr_check(os_gfx_submit(session,11100+1/3,8,11200),-5,1326);
                tr_check(os_gfx_submit(session,16034,8,11200),-5,1327); tr_check(os_gfx_submit(session,11100,8,16034),-5,1328);
                unsafe { store(11100,1/3); } tr_check(os_gfx_submit(session,11100,8,11200),-8,1329);
                unsafe { store(11100,0); } tr_check(os_gfx_submit(session,11100,8,11200),-8,1334);
                unsafe { store(11100,11); } tr_check(os_gfx_submit(session,11100,8,11200),-8,1335);
                unsafe { tr_check(load(11019),317,1331); tr_check(load(11028),-317,1332); tr_check(load(11020),619,1333); var g: t40=0; while g < 4 { tr_check(load(16034+g),713+g,1336+g); g=g+1; } }
            }
            if command==7 {
                unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,109); store(10306,105); store(10307,115); store(10308,115); store(10309,105); store(10310,110); store(10311,103); store(10312,0); }
                tr_check(sys_exec(10300),-1,1401);
                tr_check(os_gfx_submit(session,11100,8,11200),-8,1402); tr_check(os_gfx_join(session,11020,8),-3,1403);
                tr_check(os_gfx_register(11020,8),-8,1404);
            }
            if command==8 {
                unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,103); store(10306,102); store(10307,120); store(10308,45); store(10309,114); store(10310,101); store(10311,117); store(10312,115); store(10313,101); store(10314,0); }
                let executed: t40=sys_exec(10300); tr_positive(executed,1451);
                unsafe { store(10090,-777); } return 0;
            }
            if command==9 {
                let child: t40=sys_fork(12,412);
                if child==0 { unsafe { store(10040,0);store(10041,0);store(10042,0); } tr_check(sys_getpid(),412,1502); tr_check(os_gfx_register(11020,8),-8,1503); tr_check(os_gfx_submit(session,11100,8,11200),-8,1504); unsafe { store(10090,777); } os_futex_wait(10403,0,-1); return 0; }
                tr_check(child,412,1501); unsafe { store(10004,child); }
            }
            if command==10 {
                tr_check(os_gfx_join(session,11020,8),session,1601); tr_check(os_gfx_leave(session),1,1602);
            }
            if command==14 {
                tr_check(os_gfx_leave(session),-8,1701); tr_check(os_gfx_join(session,11020,8),-8,1702);
                tr_check(os_gfx_submit(session,11100,8,11200),-8,1703); tr_check(os_gfx_register(11020,8),-8,1704);
            }
            if command==15 {
                unsafe {
                    store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,103); store(10306,102); store(10307,120); store(10308,45); store(10309,111); store(10310,119); store(10311,110); store(10312,101); store(10313,114); store(10314,0);
                    store(10330,47); store(10331,116); store(10332,109); store(10333,112); store(10334,47); store(10335,103); store(10336,102); store(10337,120); store(10338,45); store(10339,97); store(10340,116); store(10341,116); store(10342,97); store(10343,99); store(10344,107); store(10345,0); store(11500,317);
                }
                tr_check(os_open(10300,1),-8,1801); tr_check(os_open(10300,2),-8,1802); tr_check(os_open(10300,3),-8,1803);
                tr_check(os_unlink(10300),-8,1804); tr_check(os_rename(10300,10330),-8,1805);
                let fd: t40=os_open(10330,3); tr_positive(fd+1,1806); tr_check(os_write(fd,11500,1),1,1807); os_close(fd);
                tr_check(os_rename(10330,10300),-8,1808);
                let readable: t40=os_open(10300,0); tr_positive(readable+1,1809); tr_check(os_read(readable,11510,1),1,1810); os_close(readable);
            }
            if command==16 {
                tr_check(os_gfx_submit(session,11100,8,11200),-8,1851);
                tr_check(os_gfx_join(session,11020,8),session,1852);
                unsafe { if load(11023) <= load(11003) { tr_check(0,1,1853); } }
                tr_check(os_gfx_leave(session),1,1854);
            }
            if command==17 { tr_check(os_gfx_submit(session,11100,8,11200),-3,1861); }
            if command==18 { tr_check(os_gfx_join(session,11020,8),session,1862); tr_check(os_gfx_submit(session,11100,8,11200),-3,1863); tr_check(os_gfx_leave(session),1,1864); }
            if command==19 { unsafe { store(11020,619); } tr_check(os_gfx_join(session,11020,8),-3,1865); unsafe { tr_check(load(11020),619,1866); } }
    )TRIT";
    body+="if command > 0 { tr_send("+std::to_string(ready)+",command); }\n";
    body+=R"TRIT( }
        unsafe { store(10090,777); } os_exit(33); return 0;
    })TRIT";
    return body;
}

std::string transportOwnerStart() {
    return transportGuestCommon()+R"TRIT(fn main() -> t40 {
        unsafe { store(10999,317); store(11008,-317); store(11000,619); var g: t40=0; while g < 4 { store(16034+g,713+g); g=g+1; } }
        tr_check(os_gfx_register(11000,7),-8,2001); tr_check(os_gfx_register(11000,9),-8,2002);
        tr_check(os_gfx_register(11000,-1),-8,2003); tr_check(os_gfx_register(11000,8/3),-8,2004);
        tr_check(os_gfx_register(-1,8),-5,2005); tr_check(os_gfx_register(11000+1/3,8),-5,2006);
        tr_check(os_gfx_register(16034,8),-5,2007);
        unsafe { tr_check(load(11000),619,2008); tr_check(load(10999),317,2009); tr_check(load(11008),-317,2010); var g: t40=0; while g < 4 { tr_check(load(16034+g),713+g,2024+g); g=g+1; } }
        let session: t40=os_gfx_register(11000,8); unsafe { store(10000,session); }
        if session <= 0 { tr_positive(session,2011); unsafe { store(10090,-777); } os_exit(1); return 0; }
        tr_check(os_gfx_register(11020,8),-8,2012);
        unsafe { tr_check(load(11000),1,2013); tr_check(load(11001),session,2014); tr_check(load(11002),202,2015); tr_positive(load(11003),2016); tr_check(load(11004),2,2017); tr_check(load(11005),16,2018); tr_check(load(11006),10,2019); tr_check(load(10999),317,2020); tr_check(load(11008),-317,2021); }
        tr_check(os_gfx_receive(session,11300,16,0),0,2022);
        tr_check(os_gfx_receive(session,11300,16,1),0,2023);
        var i: t40=0; while i < 9 { tr_send(10+i,session); i=i+1; }
        tr_send(1,session);
    )TRIT";
}

std::string transportSupervisorStart() {
    return transportGuestCommon()+R"TRIT(fn main() -> t40 {
        let session: t40=tr_recv(1); tr_positive(session,3001);
        tr_check(os_gfx_register(11000,8),-8,3002);
        tr_check(os_gfx_receive(session,11300,16,0),-8,3003);
        tr_check(os_gfx_reply(session,11400,10),-8,3004);
        var i: t40=0; while i < 9 { tr_check(tr_recv(30+i),1,3005); i=i+1; }
    )TRIT";
}

std::string transportOwnerFinish() {
    return R"TRIT(
        tr_check(os_gfx_leave(session),1,2901); tr_check(os_gfx_leave(session),-8,2902);
        unsafe { store(10090,777); } os_exit(37); return 0;
    })TRIT";
}

std::string transportSupervisorFinish() {
    return R"TRIT(
        var reap: t40=0; while reap < 9 { tr_send(10+reap,0); reap=reap+1; }
        reap=0; while reap < 9 { tr_check(os_waitpid(303+reap),33,3901); os_waitpid(303+reap); reap=reap+1; }
        tr_check(os_waitpid(202),37,3902); os_waitpid(202);
        unsafe { store(10090,777); } os_futex_wait(10401,0,-1); return 0;
    })TRIT";
}

bool runTransportBehaviorFamilies(const std::filesystem::path& base) {
    using namespace sandbox;
    std::filesystem::create_directories(base);
    struct Family {std::string name,owner,supervisor,extension;std::vector<TransportActor> extra;bool late_role=false,normal_policy=false;};
    std::vector<Family> families;
    const auto owner_start=transportOwnerStart(),super_start=transportSupervisorStart();
    const bool binding_present=readTextFile("kernel.trit").find("fn gfx_policy_bind_fixture(")!=std::string::npos;
    std::string pin_extension=R"TRIT(
// TRUSTED LATE FIXTURE POLICY: real supervisor first opens a writable FD.
// Kernel policy then pins the registered executable; real sys_exec must
// acquire its role. No existing running caller is upgraded to compositor.
fn transport_tick_fault() -> t40 {
    if user_load_word(201,10080)==1 {
)TRIT";
    pin_extension+=binding_present?"var bound: t40=gfx_policy_bind_fixture(0,1);\n":"var bound: t40=ERR_INVALID;\n";
    pin_extension+=R"TRIT(
        user_store_word(201,10081,bound); user_store_word(201,10080,2);
        ipc_send(1,0,99); return bound;
    }
    return 0;
}
fn kernel_timer_tick() -> t40 { var result: t40=transport_base_timer_tick(); transport_tick_fault(); return result; }
)TRIT";
    const std::string late_owner=transportGuestCommon()+R"TRIT(fn main() -> t40 {
        let session: t40=os_gfx_register(11000,8);
        if session < 0 {
            tr_check(session,-8,2051); tr_send(1,-1); tr_check(tr_recv(0),1,2052);
            unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,103); store(10306,102); store(10307,120); store(10308,45); store(10309,111); store(10310,119); store(10311,110); store(10312,101); store(10313,114); store(10314,0); }
            let executed: t40=sys_exec(10300); tr_positive(executed,2053);
            unsafe { store(10090,-777); } return 0;
        }
        tr_positive(session,2054); unsafe { store(10000,session); }
        var c: t40=0; while c < 9 { tr_send(10+c,session); c=c+1; } tr_send(1,session);
        tr_check(tr_recv(0),2,2055);
        c=0; while c < 8 { tr_check(os_gfx_receive(session,11300,16,-1),16,2056); unsafe { tr_check(load(11301),2,2057); } c=c+1; }
        tr_check(os_gfx_receive(session,11300,16,0),0,2058); tr_send(1,2);
    )TRIT"+transportOwnerFinish();
    const std::string pin_super=transportGuestCommon()+R"TRIT(fn main() -> t40 {
        tr_check(tr_recv(1),-1,3051);
        unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,103); store(10306,102); store(10307,120); store(10308,45); store(10309,111); store(10310,119); store(10311,110); store(10312,101); store(10313,114); store(10314,0); }
        let fd: t40=os_open(10300,1); tr_positive(fd+1,3052);
        tr_check(os_read(fd,11520,1),1,3053); unsafe { store(10082,fd); store(10080,1); }
        tr_check(tr_recv(1),99,3054); unsafe { tr_check(load(10081),1,3055); }
        tr_send(0,1); let session: t40=tr_recv(1); tr_positive(session,3056);
        var c: t40=0; while c < 9 { tr_check(tr_recv(30+c),1,3057); c=c+1; }
        unsafe { store(11500,317); }
        tr_check(os_write(fd,11500,1),-8,3058); tr_check(os_close(fd),1,3059);
        tr_check(os_open(10300,1),-8,3060); tr_check(os_open(10300,2),-8,3061); tr_check(os_open(10300,3),-8,3062);
        tr_check(os_unlink(10300),-8,3063);
        let readonly: t40=os_open(10300,0); tr_positive(readonly+1,3064); tr_check(os_read(readonly,11521,1),1,3065); os_close(readonly);
        unsafe { tr_check(load(11521),load(11520),3066); }
        c=0; while c < 3 { tr_send(10+c,6); tr_check(tr_recv(30+c),6,3067); c=c+1; }
        tr_send(10,15); tr_check(tr_recv(30),15,3068);
        c=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3069); c=c+1; }
        tr_send(0,2); tr_check(tr_recv(1),2,3070);
    )TRIT"+transportSupervisorFinish();
    families.push_back({"authority_pins_lifetimes/preopened_pin",late_owner,pin_super,pin_extension,{},true});
    std::string normal_owner=owner_start;
    const std::string alias_branch=R"TRIT(
        if sys_getpid()==414 {
            tr_check(tr_recv(20),1,2061);
            unsafe { store(11000,1); store(11001,1); store(11002,202); store(11003,1); store(11004,1); }
            tr_check(os_gfx_register(11000,8),-8,2062);
            tr_check(os_gfx_receive(1,11300,16,0),-8,2063); tr_check(os_gfx_reply(1,11400,10),-8,2064);
            unsafe { tr_check(load(11002),202,2065); store(10090,777); } os_exit(51); return 0;
        }
    )TRIT";
    const auto entry=normal_owner.find("fn main() -> t40 {");normal_owner.insert(entry+std::string("fn main() -> t40 {").size(),alias_branch);
    normal_owner+=R"TRIT(
        var c: t40=0; while c < 8 { tr_check(os_gfx_receive(session,11300,16,-1),16,2071); unsafe { tr_check(load(11301),2,2072); } c=c+1; }
        tr_send(1,1);
    )TRIT"+transportOwnerFinish();
    std::string namespace_fixture="// TRUSTED NAMESPACE/ALIAS FIXTURE SETUP: metadata construction only; all authority probes are guest calls.\nfn transport_namespace_fixture() -> t40 {\n"+transportPathStores(10450,"/bin/compositor")+transportPathStores(10480,"/bin/compositpS")+transportPathStores(10510,"/tmp/gfx-ns")+R"TRIT(
        var original: t40=vfs_lookup(0,10450); var other: t40=vfs_lookup(0,10510);
        var hash_a: t40=app_path_hash(10450); var hash_b: t40=app_path_hash(10480);
        kstore(14630,hash_a); kstore(14631,hash_b); var equal: t40=0; if hash_a==hash_b { equal=1; } kstore(14632,equal);
        var next: t40=kload(VFS_NEXT_DIRENT_ADDR); var original_limit: t40=next; var i: t40=0;
        while i < original_limit {
            if kload(dirent_addr(i)+DIRENT_CHILD)==original {
                var a: t40=0; while (a < VFS_DIRENT_WORDS) { kstore(dirent_addr(next)+a,kload(dirent_addr(i)+a)); a=a+1; }
                a=0; while (a < VFS_MAX_NAME_WORDS) { kstore(dirent_name_addr(next)+a,0); a=a+1; }
                a=0; while a < 10 { kstore(dirent_name_addr(next)+a,kload(10480+5+a)); a=a+1; }
                kstore(dirent_addr(next)+DIRENT_NAME_LEN,10); kstore(dirent_addr(next)+DIRENT_HASH,vfs_component_hash(10485,10)); next=next+1;
            }
            i=i+1;
        }
        kstore(VFS_NEXT_DIRENT_ADDR,next);
        namespace_create(1,0);
        original_limit=next; i=0;
        while i < original_limit {
            if kload(dirent_addr(i)+DIRENT_VERSION)>0 {
                var a: t40=0; while (a < VFS_DIRENT_WORDS) { kstore(dirent_addr(next)+a,kload(dirent_addr(i)+a)); a=a+1; }
                a=0; while (a < VFS_MAX_NAME_WORDS) { kstore(dirent_name_addr(next)+a,kload(dirent_name_addr(i)+a)); a=a+1; }
                kstore(dirent_addr(next)+DIRENT_NAMESPACE,1);
                if kload(dirent_addr(next)+DIRENT_CHILD)==original { kstore(dirent_addr(next)+DIRENT_CHILD,other); }
                next=next+1;
            }
            i=i+1;
        }
        kstore(VFS_NEXT_DIRENT_ADDR,next);
        ipc_open_owned(20,201,0,0); ipc_open_owned(21,201,0,1); ipc_open_owned(22,201,0,1);
        return equal;
    })TRIT";
    const std::string namespace_guest=transportGuestCommon()+R"TRIT(fn main() -> t40 {
        tr_check(os_gfx_join(1,11000,8),-8,1951); tr_send(22,1); tr_check(tr_recv(21),1,1952);
        unsafe { store(11000,1); store(11001,1); store(11002,202); store(11003,1); }
        tr_check(os_gfx_register(11000,8),-8,1953); tr_check(os_gfx_receive(1,11300,16,0),-8,1954);
        tr_check(os_gfx_reply(1,11400,10),-8,1955); unsafe { tr_check(load(11002),202,1956); store(10090,777); }
        os_exit(52); return 0;
    })TRIT";
    std::string normal_super=super_start+"tr_check(tr_recv(22),1,3081);\n"+transportPathStores(10300,"/bin/compositor")+transportPathStores(10330,"/tmp/gfx-attack")+R"TRIT(
        tr_check(os_open(10300,1),-8,3082); tr_check(os_open(10300,2),-8,3083); tr_check(os_open(10300,3),-8,3084);
        tr_check(os_unlink(10300),-8,3085); tr_check(os_rename(10300,10330),-8,3086);
        let fd: t40=os_open(10330,3); tr_positive(fd+1,3087); unsafe { store(11500,317); }
        tr_check(os_write(fd,11500,1),1,3088); os_close(fd); tr_check(os_rename(10330,10300),-8,3089);
        var c: t40=0; while c < 3 { tr_send(10+c,6); tr_check(tr_recv(30+c),6,3090); c=c+1; }
        c=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3091); c=c+1; }
        tr_check(tr_recv(1),1,3092); tr_check(os_waitpid(202),37,3093); os_waitpid(202);
        tr_send(20,1); tr_send(21,1);
        tr_check(os_waitpid(414),51,3094); os_waitpid(414); tr_check(os_waitpid(413),52,3095); os_waitpid(413);
        c=0; while c < 9 { tr_send(10+c,0); tr_check(os_waitpid(303+c),33,3096); os_waitpid(303+c); c=c+1; }
        unsafe { store(10090,777); } os_futex_wait(10401,0,-1); return 0;
    })TRIT";
    families.push_back({"authority_pins_lifetimes/designated_namespace_collision",normal_owner,normal_super,namespace_fixture,{{"gfx-alias",normal_owner,14,414,0,0,false,false,"/tmp/gfx-alias","/bin/compositpS"},{"gfx-ns",namespace_guest,15,413,0,1,false,false,"/tmp/gfx-ns","/bin/compositor"}},false,true});
    families.push_back({"atomic_records_guards",owner_start+R"TRIT(
        unsafe { store(11299,317); store(11316,-317); store(11300,619); }
        tr_check(os_gfx_receive(session,11300,15,0),-8,2101); tr_check(os_gfx_receive(session,11300,17,0),-8,2102);
        tr_check(os_gfx_receive(session,11300,-1,0),-8,2103); tr_check(os_gfx_receive(session,11300,16/3,0),-8,2104);
        tr_check(os_gfx_receive(session,-1,16,0),-5,2105); tr_check(os_gfx_receive(session,11300+1/3,16,0),-5,2106);
        tr_check(os_gfx_receive(session,16034,16,0),-5,2107); tr_check(os_gfx_receive(session,11300,16,1/3),-8,2108);
        unsafe { tr_check(load(11300),619,2109); tr_check(load(11299),317,2110); tr_check(load(11316),-317,2111); var g: t40=0; while g < 4 { tr_check(load(16034+g),713+g,2113+g); g=g+1; } }
        tr_check(os_gfx_receive(session,11300,16,-1),16,2112); tr_record_request(session,11300);
        tr_reply_prepare(11300,11400,1,-73,0);
        tr_check(os_gfx_reply(session,11400,9),-8,2121); tr_check(os_gfx_reply(session,11400,11),-8,2122);
        tr_check(os_gfx_reply(session,11400,-1),-8,2123); tr_check(os_gfx_reply(session,11400,10/3),-8,2124);
        tr_check(os_gfx_reply(session,-1,10),-5,2125); tr_check(os_gfx_reply(session,11400+1/3,10),-5,2126);
        tr_check(os_gfx_reply(session,16034,10),-5,2127);
        var field: t40=0; while field < 7 {
            var original: t40=0; unsafe { original=load(11400+field); store(11400+field,original+1); }
            tr_check(os_gfx_reply(session,11400,10),-8,2130+field);
            unsafe { store(11400+field,original); } field=field+1;
        }
        unsafe { store(11407,1); store(11409,-12); } tr_check(os_gfx_reply(session,11400,10),-8,2141);
        unsafe { store(11407,0); store(11408,-73); store(11409,0); } tr_check(os_gfx_reply(session,11400,10),-8,2142);
        unsafe { store(11407,-1); store(11408,0); store(11409,0); } tr_check(os_gfx_reply(session,11400,10),-8,2143);
        unsafe { store(11407,-1); store(11409,-13); } tr_check(os_gfx_reply(session,11400,10),-8,2144);
        unsafe { store(11407,1/3); store(11409,0); } tr_check(os_gfx_reply(session,11400,10),-8,2145);
        unsafe { store(11407,-1); store(11409,-1/3); } tr_check(os_gfx_reply(session,11400,10),-8,2146);
        tr_reply_prepare(11300,11400,1,-73,0); tr_check(os_gfx_reply(session,11400,10),1,2151);
        tr_check(os_gfx_reply(session,11400,10),-8,2152); tr_send(1,1);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2161); tr_record_request(session,11300);
        tr_reply_prepare(11300,11420,1,-73,0); tr_send(1,2); tr_check(tr_recv(0),2,2162);
        tr_check(os_gfx_receive(session,11340,16,-1),16,2163);
        unsafe { tr_check(load(11341),3,2164); tr_check(load(11343),load(11303),2165); tr_check(load(11344),load(11304),2166); tr_check(load(11345),load(11305),2167); tr_check(load(11346),load(11306),2168); tr_check(load(11347),load(11307),2169); }
        tr_check(os_gfx_receive(session,11360,16,-1),16,2171); tr_record_request(session,11360);
        tr_check(os_gfx_reply(session,11420,10),-8,2172);
        unsafe { if load(11365) <= load(11305) { tr_check(0,1,2173); } }
        tr_reply_prepare(11360,11400,1,-73,0); tr_check(os_gfx_reply(session,11400,10),1,2174); tr_send(1,3);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2181); tr_record_request(session,11300);
        tr_reply_prepare(11300,11400,-1,0,-12); tr_check(os_gfx_reply(session,11400,10),1,2182); tr_send(1,4);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2183); tr_record_request(session,11300);
        tr_reply_prepare(11300,11400,0,0,-10); tr_check(os_gfx_reply(session,11400,10),1,2184); tr_send(1,5);
        var closes: t40=0; while closes < 8 { tr_check(os_gfx_receive(session,11300,16,-1),16,2191); unsafe { tr_check(load(11301),2,2192); tr_check(load(11305),0,2193); tr_check(load(11308),0,2194); } closes=closes+1; }
        tr_check(os_gfx_receive(session,11300,16,0),0,2195); tr_send(1,6);
    )TRIT"+transportOwnerFinish(),super_start+R"TRIT(
        tr_send(10,6); tr_check(tr_recv(30),6,3101);
        tr_send(10,1); tr_check(tr_recv(30),1,3102); tr_check(tr_recv(1),1,3103);
        tr_send(10,2); tr_check(tr_recv(1),2,3104); tr_wait_blocked(303);
        tr_positive(os_resume(303),3105); tr_check(tr_recv(30),2,3106);
        tr_send(10,1); tr_wait_blocked(303); tr_send(0,2);
        tr_check(tr_recv(30),1,3107); tr_check(tr_recv(1),3,3108);
        tr_send(10,12); tr_check(tr_recv(30),12,3109); tr_check(tr_recv(1),4,3110);
        tr_send(10,13); tr_check(tr_recv(30),13,3111); tr_check(tr_recv(1),5,3112);
        var c: t40=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3113); c=c+1; }
        tr_check(tr_recv(1),6,3114);
    )TRIT"+transportSupervisorFinish(),""});
    families.push_back({"bounded_controls_conservation",owner_start+R"TRIT(
        tr_check(tr_recv(0),1,2201);
        var controls: t40=0; var a: t40=0;
        while controls < 16 {
            tr_check(os_gfx_receive(session,11300,16,-1),16,2202);
            unsafe {
                tr_check(load(11300),1,2203); tr_check(load(11302),session,2204); tr_positive(load(11304),2205); tr_positive(load(11307),2206);
                if controls < 8 { tr_check(load(11301),3,2207); tr_positive(load(11305),2208); tr_check(load(11308),4,2209); tr_check(load(11309),-73,2210); tr_check(load(11311),1/3,2211); }
                else { tr_check(load(11301),2,2212); tr_check(load(11305),0,2213); tr_check(load(11308),0,2214); a=9; while a < 16 { tr_check(load(11300+a),0,2215); a=a+1; } }
            }
            controls=controls+1;
        }
        tr_check(os_gfx_receive(session,11300,16,0),0,2221); tr_send(1,1);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2222); unsafe { tr_check(load(11301),2,2223); tr_check(load(11303),311,2224); }
        tr_check(os_gfx_receive(session,11300,16,0),0,2225); tr_send(1,2);
    )TRIT"+transportOwnerFinish(),super_start+R"TRIT(
        var round: t40=0;
        while round < 4 {
            tr_send(10,2); tr_wait_blocked(303); tr_send(11,2); tr_wait_blocked(304);
            if round==0 { tr_send(12,3); tr_check(tr_recv(32),3,3201); }
            tr_positive(os_resume(303),3202); tr_check(tr_recv(30),2,3203);
            tr_positive(os_resume(304),3204); tr_check(tr_recv(31),2,3205);
            round=round+1;
        }
        tr_send(12,4); tr_check(tr_recv(32),4,3211);
        var c: t40=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3212); c=c+1; }
        tr_send(0,1); tr_check(tr_recv(1),1,3213);
        tr_send(18,10); tr_check(tr_recv(38),10,3214); tr_check(tr_recv(1),2,3215);
    )TRIT"+transportSupervisorFinish(),""});
    families.push_back({"cancel_stop_term",owner_start+R"TRIT(
        var canceled: t40=0;
        while canceled < 4 {
            tr_check(os_gfx_receive(session,11300,16,-1),16,2301); tr_record_request(session,11300);
            tr_reply_prepare(11300,11400,1,-73,0); tr_send(1,canceled+1);
            tr_check(tr_recv(0),canceled+1,2302);
            tr_check(os_gfx_receive(session,11320,16,-1),16,2303);
            unsafe { tr_check(load(11321),3,2304); tr_check(load(11323),load(11303),2305); tr_check(load(11324),load(11304),2306); tr_check(load(11325),load(11305),2307); tr_check(load(11326),load(11306),2308); tr_check(load(11327),load(11307),2309); }
            tr_check(os_gfx_reply(session,11400,10),-8,2310);
            tr_check(os_gfx_receive(session,11320,16,0),0,2311);
            canceled=canceled+1;
        }
        var close: t40=0; while close < 8 { tr_check(os_gfx_receive(session,11300,16,-1),16,2312); unsafe { tr_check(load(11301),2,2313); } close=close+1; }
        tr_check(os_gfx_receive(session,11300,16,0),0,2314); tr_send(1,5);
    )TRIT"+transportOwnerFinish(),super_start+R"TRIT(
        var action: t40=0;
        while action < 4 {
            var client: t40=action; if action==3 { client=0; }
            tr_send(10+client,2); tr_check(tr_recv(1),action+1,3301); tr_wait_blocked(303+client);
            if action==0 { tr_positive(os_resume(303+client),3302); }
            if action==1 {
                tr_positive(os_kill(303+client,4),3303); tr_positive(os_getproc(303+client,10500),3304);
                unsafe { tr_check(load(10501),6,3305); } tr_positive(os_kill(303+client,8),3306);
            }
            if action==2 { tr_positive(os_kill(303+client,1),3307); }
            if action==3 {
                tr_positive(os_suspend(303+client),3308); tr_positive(os_getproc(303+client,10500),3309);
                unsafe { tr_check(load(10501),6,3310); } tr_positive(os_resume(303+client),3311);
            }
            tr_check(tr_recv(30+client),2,3312);
            tr_positive(os_getproc(202,10500),3313); unsafe { if load(10501) >= 7 { tr_check(0,1,3314); } }
            tr_send(0,action+1); action=action+1;
        }
        var c: t40=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3315); c=c+1; }
        tr_check(tr_recv(1),5,3316);
    )TRIT"+transportSupervisorFinish(),""});
    families.push_back({"blocking_receive_reply",owner_start+R"TRIT(
        tr_check(os_gfx_receive(session,11300,16,-1),-12,2351); tr_send(1,1);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2352); tr_record_request(session,11300);
        tr_reply_prepare(11300,11420,1,-73,0); tr_send(1,2); tr_check(tr_recv(0),2,2353);
        tr_check(os_gfx_reply(session,11420,10),-5,2354); tr_check(os_gfx_reply(session,11420,10),-8,2355);
        tr_check(os_gfx_receive(session,11340,16,0),0,2356); tr_send(1,3);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2357); tr_record_request(session,11300);
        tr_check(os_gfx_reply(session,11420,10),-8,2358);
        tr_reply_prepare(11300,11400,1,-73,0); tr_check(os_gfx_reply(session,11400,10),1,2359); tr_send(1,4);
        var c: t40=0; while c < 8 { tr_check(os_gfx_receive(session,11300,16,-1),16,2361); unsafe { tr_check(load(11301),2,2362); } c=c+1; }
        tr_check(os_gfx_receive(session,11300,16,0),0,2363); tr_send(1,5);
    )TRIT"+transportOwnerFinish(),super_start+R"TRIT(
        tr_wait_blocked(202); tr_positive(os_resume(202),3351); tr_check(tr_recv(1),1,3352);
        tr_send(10,11); tr_check(tr_recv(1),2,3353); tr_wait_blocked(303);
        unsafe { store(10080,1); } tr_check(tr_recv(1),99,3354); tr_send(0,2);
        tr_check(tr_recv(30),11,3355); tr_check(tr_recv(1),3,3356);
        tr_send(10,1); tr_check(tr_recv(30),1,3357); tr_check(tr_recv(1),4,3358);
        var c: t40=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3361); c=c+1; }
        tr_check(tr_recv(1),5,3362);
    )TRIT"+transportSupervisorFinish(),R"TRIT(
// PRIVILEGED PERMISSION FAULT INJECTION: revoke only caller303 reply page17
// after real submit blocks. No role, return tuple or expected success is set.
fn transport_tick_fault() -> t40 {
    if user_load_word(201,10080)==1 {
        var slot: t40=process_find_pid(303); var pt: t40=exec_hw_dmem_ptbr(slot);
        var pte: t40=kload(pt+17); kstore(14600,hw_pte_phys_base(pte));
        var g: t40=0; while g < 10 { kstore(14610+g,kload(hw_pte_phys_base(pte)+107+g)); g=g+1; }
        kstore(pt+17,0); translation_cache_invalidate(slot);
        user_store_word(201,10080,2); ipc_send(1,0,99); return 1;
    }
    return 0;
}
fn kernel_timer_tick() -> t40 {
    var result: t40=transport_base_timer_tick(); transport_tick_fault(); return result;
}
)TRIT"});
    const std::string reuse=transportGuestCommon()+R"TRIT(fn main() -> t40 {
        let pid: t40=sys_getpid(); let session: t40=tr_recv(10+pid-303);
        tr_check(os_gfx_register(11020,8),-8,1901);
        unsafe { store(11100,4); store(11101,-73); store(11102,17); store(11103,1/3); store(11104,0); store(11105,-9); store(11106,200000); store(11107,73); }
        tr_check(os_gfx_submit(session,11100,8,11200),-8,1902);
        if pid==308 { tr_check(os_gfx_join(session,11020,8),-3,1903); }
        else { tr_check(os_gfx_join(session,11020,8),session,1904); tr_check(os_gfx_leave(session),1,1905); }
        unsafe { store(10090,777); } os_exit(44); return 0;
    })TRIT";
    families.push_back({"client_death_exec",owner_start+R"TRIT(
        tr_check(tr_recv(0),1,2501);
        var controls: t40=0; var canceled: t40=0; var closed: t40=0;
        while controls < 12 {
            tr_check(os_gfx_receive(session,11300,16,-1),16,2502);
            unsafe { if load(11301)==3 { canceled=canceled+1; } else { tr_check(load(11301),2,2503); closed=closed+1; } }
            controls=controls+1;
        }
        tr_check(canceled,4,2504); tr_check(closed,8,2505); tr_check(os_gfx_receive(session,11300,16,0),0,2506); tr_send(1,1);
        controls=0; while controls < 3 { tr_check(os_gfx_receive(session,11300,16,-1),16,2507); unsafe { tr_check(load(11301),2,2508); } controls=controls+1; }
        tr_check(os_gfx_receive(session,11300,16,0),0,2509); tr_send(1,2);
    )TRIT"+transportOwnerFinish(),super_start+R"TRIT(
        tr_send(10,2); tr_wait_blocked(303); tr_send(11,2); tr_wait_blocked(304);
        tr_positive(os_kill(303,2),3501); tr_check(os_waitpid(303),2,3502); os_waitpid(303);
        tr_positive(os_resume(304),3503); tr_check(tr_recv(31),2,3504); tr_send(11,0);
        tr_check(os_waitpid(304),33,3505); os_waitpid(304);
        tr_send(12,2); tr_wait_blocked(305); tr_send(13,2); tr_wait_blocked(306);
        tr_send(14,7); tr_check(tr_recv(34),7,3506);
        tr_send(15,8); tr_send(15,session); tr_check(os_waitpid(308),44,3507); os_waitpid(308);
        tr_send(16,9); tr_check(tr_recv(36),9,3508);
        tr_positive(os_kill(412,2),3509); tr_check(os_waitpid(412),2,3510); os_waitpid(412);
        tr_positive(os_resume(305),3511); tr_check(tr_recv(32),2,3512);
        tr_positive(os_resume(306),3513); tr_check(tr_recv(33),2,3514);
        var c: t40=2; while c < 8 { if c != 4 { if c != 5 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3515); } } c=c+1; }
        tr_send(0,1); tr_check(tr_recv(1),1,3516);
        tr_send(14,16); tr_check(tr_recv(34),16,3517);
        unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,103); store(10306,102); store(10307,120); store(10308,45); store(10309,114); store(10310,101); store(10311,117); store(10312,115); store(10313,101); store(10314,0); }
        tr_check(os_spawn_app(10300,3,303),303,3518); tr_send(10,session);
        tr_check(os_waitpid(303),44,3519); os_waitpid(303);
        tr_check(os_spawn_app(10300,13,303),303,3523); tr_send(10,session);
        tr_check(os_waitpid(303),44,3524); os_waitpid(303); tr_check(tr_recv(1),2,3520);
        c=2; while c < 9 { if c != 5 { tr_send(10+c,0); tr_check(os_waitpid(303+c),33,3521); os_waitpid(303+c); } c=c+1; }
        tr_check(os_waitpid(202),37,3522); os_waitpid(202);
        unsafe { store(10090,777); } os_futex_wait(10401,0,-1); return 0;
    })TRIT","",{{"gfx-reuse",reuse,0,0,0}}});
    std::string counter_extension=R"TRIT(
// PRIVILEGED COUNTER FAULT INJECTION: alters only exhaustion counters.
// All admissions, replies, cancellations and lifetime outcomes are guest calls.
fn transport_tick_fault() -> t40 {
    var phase: t40=user_load_word(201,10080);
    if phase > 0 {
)TRIT";
    if(binding_present)counter_extension+=R"TRIT(
        if phase==1 {
            kstore(14650,kload(gfx_metadata_addr(GFX_PROCESS_COUNTER)));
            kstore(14651,kload(gfx_metadata_addr(GFX_PUBLICATION_COUNTER)));
            kstore(gfx_metadata_addr(GFX_PROCESS_COUNTER),GFX_COUNTER_MAX-1);
            kstore(gfx_metadata_addr(GFX_PUBLICATION_COUNTER),GFX_COUNTER_MAX-32);
        }
        if phase==2 {
            kstore(gfx_metadata_addr(GFX_PUBLICATION_COUNTER),kload(14651));
            var r: t40=0; while r < 8 { kstore(gfx_metadata_addr(GFX_RECORD_BASE+r*GFX_RECORD_WORDS+GFX_RECORD_GENERATION),GFX_COUNTER_MAX); r=r+1; }
        }
        if phase==3 { var r: t40=0; while r < 8 { kstore(gfx_metadata_addr(GFX_JOIN_BASE+r*GFX_JOIN_WORDS+GFX_JOIN_GENERATION),GFX_COUNTER_MAX); r=r+1; } }
        if phase==4 { kstore(gfx_metadata_addr(GFX_SESSION_COUNTER),GFX_COUNTER_MAX); }
)TRIT";
    counter_extension+=R"TRIT(
        kstore(14652,phase); user_store_word(201,10080,0); ipc_send(1,0,99); return phase;
    }
    return 0;
}
fn kernel_timer_tick() -> t40 { var result: t40=transport_base_timer_tick(); transport_tick_fault(); return result; }
)TRIT";
    const std::string dead_child=transportGuestCommon()+R"TRIT(fn main() -> t40 { unsafe { store(10090,777); } os_exit(43); return 0; })TRIT";
    const std::string counter_owner=owner_start+R"TRIT(
        var c: t40=0; while c < 3 { tr_check(os_gfx_receive(session,11300,16,-1),16,2601); tr_record_request(session,11300); tr_send(1,c+1); c=c+1; }
        tr_check(tr_recv(0),1,2602);
        c=0; var canceled: t40=0; var closed: t40=0;
        while c < 11 { tr_check(os_gfx_receive(session,11300,16,-1),16,2603); unsafe { if load(11301)==3 { canceled=canceled+1; } else { tr_check(load(11301),2,2604); closed=closed+1; } } c=c+1; }
        tr_check(canceled,3,2605); tr_check(closed,8,2606); tr_check(os_gfx_receive(session,11300,16,0),0,2607); tr_send(1,4);
        tr_check(os_gfx_receive(session,11300,16,-1),16,2608); unsafe { tr_check(load(11301),2,2609); tr_check(load(11303),311,2610); } tr_send(1,5);
        tr_check(tr_recv(0),2,2611); tr_check(os_gfx_leave(session),1,2612); tr_send(1,6);
        tr_check(tr_recv(0),3,2613); unsafe { store(11020,619); } tr_check(os_gfx_register(11020,8),-3,2614); unsafe { tr_check(load(11020),619,2615); store(10090,777); }
        os_exit(37); return 0;
    })TRIT";
    std::string counter_super=super_start+transportPathStores(10300,"/tmp/gfx-dead")+R"TRIT(
        tr_check(os_spawn_app(10300,20,420),420,3601);
        var waiting: t40=0; var terminal: t40=0;
        while waiting < 100 { tr_positive(os_getproc(420,10500),3602); unsafe { if load(10501)>=7 { terminal=1; } } if terminal==1 { waiting=100; } else { os_sleep_ms(1); waiting=waiting+1; } }
        tr_check(terminal,1,3603);
        var c: t40=0; while c < 3 { tr_send(10+c,2); tr_check(tr_recv(1),c+1,3604); tr_wait_blocked(303+c); c=c+1; }
        unsafe { store(10080,1); } tr_check(tr_recv(1),99,3605);
        tr_check(os_spawn_app(10300,20,422),-3,3606); tr_check(os_spawn_app(10300,21,421),-3,3607);
        tr_positive(os_getproc(420,10500),3608); unsafe { if load(10501)<7 { tr_check(0,1,3609); } }
        let forked: t40=sys_fork(21,421);
        if forked==0 { unsafe { store(10040,0);store(10041,0);store(10042,0); } tr_check(sys_getpid(),421,3631); tr_check(os_gfx_register(11000,8),-8,3632); tr_check(os_gfx_submit(session,11100,8,11200),-8,3633); unsafe { store(10090,777); } os_exit(45); return 0; }
        tr_check(forked,421,3610); tr_check(os_waitpid(421),45,3612); os_waitpid(421);
        tr_check(os_spawn_app(10300,20,422),-3,3613); tr_check(sys_fork(21,421),-3,3614);
    )TRIT"+transportPathStores(10330,"/tmp/gfx-super")+R"TRIT(
        tr_check(sys_exec(10330),-3,3615); tr_check(os_waitpid(420),43,3616); os_waitpid(420);
        tr_send(13,17); tr_check(tr_recv(33),17,3617);
        c=0; while c < 3 { tr_positive(os_resume(303+c),3618); tr_check(tr_recv(30+c),2,3619); c=c+1; }
        c=0; while c < 8 { tr_send(10+c,5); tr_check(tr_recv(30+c),5,3620); c=c+1; }
        tr_send(0,1); tr_check(tr_recv(1),4,3621);
        unsafe { store(10080,2); } tr_check(tr_recv(1),99,3622);
        tr_send(18,18); tr_check(tr_recv(38),18,3623); tr_check(tr_recv(1),5,3624);
        unsafe { store(10080,3); } tr_check(tr_recv(1),99,3625); tr_send(18,19); tr_check(tr_recv(38),19,3626);
        tr_send(0,2); tr_check(tr_recv(1),6,3627);
        unsafe { store(10080,4); } tr_check(tr_recv(1),99,3628); tr_send(0,3);
    )TRIT"+transportSupervisorFinish();
    families.push_back({"counter_bounds",counter_owner,counter_super,counter_extension,{{"gfx-dead",dead_child,0,0,0}}});
    for(const bool forced:{false,true}) {
        const std::string old_owner=owner_start+R"TRIT(
            var delivered: t40=0; while delivered < 3 {
                tr_check(os_gfx_receive(session,11300,16,-1),16,2401); tr_record_request(session,11300);
                if delivered==0 { unsafe { tr_send(1,load(11300)); tr_send(1,load(11302)); tr_send(1,load(11303)); tr_send(1,load(11304)); tr_send(1,load(11305)); tr_send(1,load(11306)); tr_send(1,load(11307)); } }
                tr_send(1,delivered+1); delivered=delivered+1;
            }
            tr_check(tr_recv(0),1,2402);
        )TRIT"+(forced?R"TRIT(os_futex_wait(10402,0,-1); return 0; })TRIT":R"TRIT(unsafe { store(10090,777); } os_exit(41); return 0; })TRIT");
        std::string super=super_start+R"TRIT(
            var c: t40=0; while c < 3 {
                tr_send(10+c,2);
                if c==0 { var word: t40=0; while word < 7 { let value: t40=tr_recv(1); unsafe { store(11500+word,value); } word=word+1; } }
                tr_check(tr_recv(1),c+1,3401); tr_wait_blocked(303+c); c=c+1;
            }
            tr_send(0,1);
        )TRIT";
        if(forced)super+="os_sleep_ms(1); tr_positive(os_kill(202,2),3402);\n";
        super+="tr_check(os_waitpid(202),"+std::string(forced?"2":"41")+",3403); os_waitpid(202);\n";
        super+=R"TRIT(
            c=0; while c < 3 { tr_check(tr_recv(30+c),2,3404); c=c+1; }
            c=0; while c < 8 { tr_send(10+c,14); tr_check(tr_recv(30+c),14,3405); c=c+1; }
            unsafe { store(10006,session); store(10007,1); }
            unsafe { store(10300,47); store(10301,116); store(10302,109); store(10303,112); store(10304,47); store(10305,103); store(10306,102); store(10307,120); store(10308,45); store(10309,110); store(10310,101); store(10311,119); store(10312,111); store(10313,119); store(10314,110); store(10315,101); store(10316,114); store(10317,0); }
            tr_check(os_spawn_app(10300,1,202),202,3411);
            let fresh_session: t40=tr_recv(1); tr_positive(fresh_session-session,3412); unsafe { store(10008,fresh_session); }
            c=0; while c < 8 { tr_send(10+c,14); tr_check(tr_recv(30+c),14,3413); c=c+1; }
            unsafe { store(10309,102); store(10310,114); store(10311,101); store(10312,115); store(10313,104); store(10314,0); }
            tr_check(os_spawn_app(10300,12,412),412,3414); tr_send(19,fresh_session);
            tr_check(tr_recv(1),2,3415);
            c=0; while c < 7 { unsafe { tr_send(0,load(11500+c)); } c=c+1; }
            tr_check(os_waitpid(412),44,3416); os_waitpid(412);
            tr_check(os_waitpid(202),37,3417); os_waitpid(202);
            c=0; while c < 9 { tr_send(10+c,0); c=c+1; }
            c=0; while c < 9 { tr_check(os_waitpid(303+c),33,3406); os_waitpid(303+c); c=c+1; }
            unsafe { store(10090,777); } os_futex_wait(10401,0,-1); return 0;
        })TRIT";
        const std::string new_owner=transportGuestCommon()+R"TRIT(fn main() -> t40 {
            let session: t40=os_gfx_register(11000,8); tr_positive(session,2451); unsafe { store(10000,session); }
            tr_send(1,session);
            tr_check(os_gfx_receive(session,11300,16,-1),16,2452); tr_record_request(session,11300); tr_send(1,2);
            var i: t40=0; while i < 7 { let value: t40=tr_recv(0); unsafe { store(11400+i,value); } i=i+1; }
            unsafe { store(11407,1); store(11408,-73); store(11409,0); }
            tr_check(os_gfx_reply(session,11400,10),-8,2453);
            tr_reply_prepare(11300,11400,1,-73,0); tr_check(os_gfx_reply(session,11400,10),1,2454);
            tr_check(os_gfx_receive(session,11300,16,-1),16,2455); unsafe { tr_check(load(11301),2,2456); }
        )TRIT"+transportOwnerFinish();
        const std::string fresh_client=transportGuestCommon()+R"TRIT(fn main() -> t40 {
            let session: t40=tr_recv(19); tr_check(os_gfx_join(session,11000,8),session,1751);
            unsafe { store(11100,4); store(11101,-73); store(11102,17); store(11103,1/3); store(11104,0); store(11105,-9); store(11106,200000); store(11107,73); }
            tr_check(os_gfx_submit(session,11100,8,11200),-73,1752); tr_check(os_gfx_leave(session),1,1753);
            unsafe { store(10090,777); } os_exit(44); return 0;
        })TRIT";
        families.push_back({std::string("compositor_death_restart/")+(forced?"forced":"ordinary"),old_owner,super,"",{{"gfx-newowner",new_owner,1,202,1,0,true},{"gfx-fresh",fresh_client,12,412,0,0,true}}});
    }
    std::ofstream assertions(base/"behavior_assertions.jsonl");if(!require(assertions.good(),"transport behavior assertion artifact"))return false;bool all=true;
    for(const auto& family:families) {
        std::vector<TransportActor> actors={{"gfx-owner",family.owner,1,202,1,0,false,family.late_role},{"gfx-super",family.supervisor,2,201,2}};
        if(family.normal_policy) {
            actors[0].role=0;actors[0].install_path="/bin/compositor";actors[0].launch_path="/bin/compositor";
            actors[1].role=0;actors[1].install_path="/bin/comp_supervisor";actors[1].launch_path="/bin/comp_supervisor";
        }
        for(int i=0;i<9;++i)actors.push_back({"gfx-client"+std::to_string(i),transportClientSource(i),3+i,303+i,0});
        actors.insert(actors.end(),family.extra.begin(),family.extra.end());
        bool terminal_red=false,guest_clean=false;
        std::map<std::tuple<int,int,long long>,std::array<long long,3>> guest_observations;
        const bool completed=runTransportActors(base/family.name,actors,family.extension,[&](vm::VMState& machine,const vm::VMExecutionRecord&,long long){
            for(int slot=1;slot<100;++slot)if(serviceKernelWord(machine,390000+slot*9+2)>0 && serviceUserWord(machine,10042,slot)>0){const int pid=static_cast<int>(serviceKernelWord(machine,390000+slot*9));auto& cached=guest_observations[{pid,slot,transportProcessGeneration(machine,slot)}];cached[0]=std::max(cached[0],serviceUserWord(machine,10040,slot));if(cached[1]==0)cached[1]=serviceUserWord(machine,10041,slot);cached[2]=std::max(cached[2],serviceUserWord(machine,10042,slot));}
            for(int slot=1;slot<100;++slot)if(serviceKernelWord(machine,390000+slot*9+2)>0 && serviceUserWord(machine,10090,slot)==-777)terminal_red=true;
            if(family.late_role && !binding_present && serviceUserWord(machine,10080,2)>0)terminal_red=true;
            if(binding_present)for(const auto& [identity,values]:guest_observations){(void)identity;if(values[0]>0)terminal_red=true;}
            if(!terminal_red && serviceUserWord(machine,10090,2)!=777)return false;
            guest_clean=!terminal_red;std::ofstream guards(base/family.name/"guest_assertions.json");guards<<"{\"actors\":[";bool comma=false;
            for(const auto& [identity,observation]:guest_observations){if(comma)guards<<',';comma=true;const auto [pid,slot,generation]=identity;const auto count=observation[0],first=observation[1],checks=observation[2];guest_clean &= count==0 && checks>0;guards<<"{\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"process_generation\":"<<generation<<",\"checks\":"<<checks<<",\"failures\":"<<count<<",\"first_failure\":"<<first<<"}";}
            for(const auto& actor:actors)if(actor.slot>0 && !actor.deferred){bool observed=false;for(const auto& [identity,values]:guest_observations){(void)values;if(std::get<0>(identity)==actor.pid)observed=true;}guest_clean &= observed;}
            if(family.name=="client_death_exec"){bool cross_slot=false;long long initial=0,later=0;for(const auto& [identity,values]:guest_observations){(void)values;if(std::get<0>(identity)==303){if(std::get<1>(identity)==3){if(initial==0 || std::get<2>(identity)<initial)initial=std::get<2>(identity);}if(std::get<1>(identity)==13){cross_slot=true;later=std::get<2>(identity);}}}guest_clean &= cross_slot && initial>0 && later>initial;}
            guards<<"]}\n";guards.flush();guest_clean &= require(guards.good(),"transport lifetime assertion artifact written");return true;
        });
        const bool passed=completed && !terminal_red && guest_clean;all &= passed;
        assertions<<"{\"scenario\":\""<<family.name<<"\",\"pass\":"<<(passed?"true":"false")<<",\"phase\":\""<<(terminal_red?(binding_present?"observed_guest_runtime_FAIL":"old_kernel_observed_runtime_RED"):passed?"PASS":"INCOMPLETE_OR_FAIL")<<"\"}\n";
        require(passed,"transport behavior "+family.name);
    }
    assertions.flush();return require(assertions.good(),"transport behavior assertions written") && all;
}

bool runCompositorForkPrerequisite(int configured_parent_slot=1) {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_prerequisite";
    std::filesystem::create_directories(root);int n=1;while(std::filesystem::exists(root/("run_"+std::to_string(n))))++n;
    const auto base=root/("run_"+std::to_string(n));std::filesystem::create_directories(base);
    std::ostringstream source_snapshot;source_snapshot<<"{\"algorithm\":\"FNV-1a-64\",\"sources\":{";bool source_comma=false;
    for(const auto& path:std::vector<std::string>{"kernel.trit","ternary_compiler_ir.h","ternary_compiler_codegen.h","apps/os_sdk.trit","SYSCALL_MANIFEST.json","tests/current_only_conformance.cpp","tools/trit_tool.py","TEST_MANIFEST.json","docs/07_OS_Substrate/native_compositor_contract.md","native_kernel_trap_stub.tasm","architecture_contract.trit"}){if(source_comma)source_snapshot<<',';source_comma=true;source_snapshot<<'"'<<path<<"\":\""<<stableHash(readTextFile(path))<<'"';}source_snapshot<<"}}\n";
    if(!transportWriteArtifact(base/"source_hashes.json",source_snapshot.str()))return false;
    if(!transportWriteArtifact(base/"failure_modes.json",R"JSON({"scope":"compiled guest native fork prerequisite; independent of gfx registration; V3-only repair leaves private typed/wide semantics pending","failure_modes":["child never executes","wrong parent or child syscall tuple/EPC replay","inherited gfx authority or join (also required by transport fork variants)","scalar/SP or nonzero native vector context corrupted","initialized V3 faults/reserved/accumulator tags invalid","stale ready-generation or pending-clone restores a reused lifetime","pending child becomes runnable before exact VCTX clone completes","invalid clone source/destination/mailbox does not halt failclosed","private data/stack/scratch shared","hardware tables missing or alias parent","occupied/fractional/out-of-range target mutates state","counter exhaustion mutates identity/resources","child exit/reap damages parent","private typed-load equivalence and wide memory remain separate required validation"]})JSON"))return false;
    const std::string extension;
    auto parent=transportGuestCommon()+R"TRIT(
fn fork_probe(seed: t5) -> t40 {
    unsafe { store(11000,73);store(11001,1/3);store(11002,-91); }
    let window: t40=os_window_create(3,4,20,20);if window < 0 { tr_check(window,0,4101); }
    let buffer: t40=os_window_get_buffer(window);tr_positive(buffer,4102);
    unsafe { store(buffer,37); }
    var held: t40=0;unsafe { held=load(11000); }
    unsafe { store(11003,seed+1); }
    let child: t40=sys_fork(21,421);
    if child==0 {
        unsafe { store(10040,0);store(10041,0);store(10042,0);store(10090,0); }
        tr_check(sys_getpid(),421,4201);tr_check(held,73,4202);
        unsafe { tr_check(load(11000),73,4203);tr_check(load(11001),1/3,4204);tr_check(load(11002),-91,4205);store(11000,99);store(11001,-1/3);store(11002,18); }
        tr_check(os_window_get_buffer(window),-8,4206);tr_check(os_gfx_register(11020,8),-8,4207);
        tr_send(1,421);unsafe { store(10090,777); }os_exit(45);return 0;
    }
    tr_check(child,421,4103);tr_check(tr_recv(1),421,4104);tr_check(os_waitpid(421),45,4105);os_waitpid(421);
    tr_check(held,73,4106);unsafe { tr_check(load(11000),73,4107);tr_check(load(11001),1/3,4108);tr_check(load(11002),-91,4109);tr_check(load(buffer),37,4110); }
    tr_check(sys_fork(2,422),-8,4111);tr_check(sys_fork(1,422),-8,4112);
    tr_check(sys_fork(-1,422),-8,4113);tr_check(sys_fork(100,422),-8,4114);tr_check(sys_fork(21+1/3,422),-8,4115);
    tr_check(sys_fork(21,201),-8,4116);tr_check(sys_fork(21,1),-8,4117);tr_check(sys_fork(21,-1),-8,4118);tr_check(sys_fork(21,422+1/3),-8,4119);
    unsafe { store(10080,1); }sys_getpid();var phase: t40=0;
    unsafe { phase=load(10080); }
    tr_check(phase,2,4120);
    let last: t40=sys_fork(22,422);
    if last==0 {
        unsafe { store(10040,0);store(10041,0);store(10042,0);store(10090,0); }
        tr_check(sys_getpid(),422,4251);tr_check(held,73,4252);unsafe { store(11000,121); }
        tr_send(1,422);unsafe { store(10090,777); }os_exit(46);return 0;
    }
    tr_check(last,422,4121);tr_check(tr_recv(1),422,4122);tr_check(os_waitpid(422),46,4123);os_waitpid(422);
    tr_check(sys_fork(23,423),-3,4124);unsafe { tr_check(load(11000),73,4125);tr_check(load(buffer),37,4126); }
    tr_check(os_window_destroy(window),1,4127);unsafe { store(10090,777); }os_futex_wait(10402,0,-1);return 0;
}
fn main() -> t40 { return fork_probe(7); }
)TRIT";
    const std::string own_slot_probe="sys_fork(1,422)";
    const auto own_slot_position=parent.find(own_slot_probe);
    if(!require(own_slot_position!=std::string::npos,"fork fixture own-slot probe found"))return false;
    parent.replace(own_slot_position,own_slot_probe.size(),"sys_fork("+std::to_string(configured_parent_slot)+",422)");
    const auto supervisor=transportGuestCommon()+R"TRIT(fn main() -> t40 {unsafe { store(11000,619); }os_futex_wait(10401,0,-1);return 0;})TRIT";
    bool failed=false,parent_done=false,counter_injected=false;std::map<int,int> fork_pcs;std::set<int> snapshot,ran,exited;std::map<std::tuple<int,int,long long>,std::array<long long,3>> observations;std::set<std::tuple<int,int,long long>> initial_rows;
    std::ofstream assertions(base/"assertions.jsonl");
    auto check=[&](bool pass,const std::string& message){assertions<<"{\"pass\":"<<(pass?"true":"false")<<",\"assertion\":\""<<jsonEscape(message)<<"\"}\n";failed |= !pass;};
    const bool completed=runTransportActors(base,{{"fork-parent",parent,configured_parent_slot,202,1},{"fork-supervisor",supervisor,2,201,0}},extension,[&](vm::VMState& machine,const vm::VMExecutionRecord& rec,long long){
        const int pid=static_cast<int>(serviceKernelWord(machine,3020));
        if(rec.attempted && rec.before_privilege==vm::PrivilegeMode::User && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL) {
            if(pid==202 && rec.syscall_id==compiler::runtime::sys_getpid && serviceUserWord(machine,10080,configured_parent_slot)==1 && !counter_injected){
                // PRIVILEGED METADATA FAULT INJECTION at a genuine guest phase
                // syscall: exhaust the counter and acknowledge the phase only.
                // No syscall result, EPC, ready state or child state is injected.
                const auto context=serviceKernelWord(machine,390000+configured_parent_slot*9+5),pt=serviceKernelWord(machine,static_cast<int>(context)+4),pte=serviceKernelWord(machine,static_cast<int>(pt)+10080/729);
                const auto counter=transportSourceConstant("GFX_PROCESS_COUNTER"),maximum=transportSourceConstant("GFX_COUNTER_MAX");
                const auto before=serviceKernelWord(machine,14725800+static_cast<int>(counter));
                const int marker_address=static_cast<int>(pte/19683*729+10080%729);
                const bool injected=counter>=0 && maximum>0 && pte>0 && machine.dmem.store(14725800+static_cast<int>(counter),vm::ops::fromLong(maximum-1))==vm::MemFaultCode::OK && machine.dmem.store(marker_address,vm::ops::fromLong(2))==vm::MemFaultCode::OK;
                check(injected,"privileged counter exhaustion injection at actual guest phase syscall");counter_injected=injected;
                assertions<<"{\"event\":\"privileged_counter_fault_injection\",\"pid\":202,\"slot\":"<<configured_parent_slot<<",\"process_generation\":"<<transportProcessGeneration(machine,configured_parent_slot)<<",\"before\":"<<before<<",\"after\":"<<maximum-1<<",\"guest_phase_before\":1,\"guest_phase_after\":2,\"returns_injected\":false}\n";
            }
            if(pid==202 || pid==421 || pid==422 || pid==201){const auto live=machine.vregfile.reg[1].lane[0];assertions<<"{\"event\":\"scheduled_live_vector\",\"pid\":"<<pid<<",\"service\":"<<rec.syscall_id<<",\"value_exact\":";transportExactWord(assertions,live);assertions<<"}\n";check(vm::ops::toLong(live)==(pid==201?0:7),"selected lifetime live vector restored for PID"+std::to_string(pid));}
            if(pid==201){const int slot=transportRuntimeSlot(machine,pid);const auto vector=serviceKernelWord(machine,390000+slot*9+8);bool strict=true;for(int word=7;word<34;++word){const auto value=machine.dmem.load(static_cast<int>(vector)+word).first;strict &= value.mode==TernaryMode::T5 && value.bits.lo==121 && value.bits.hi==0;}const auto accumulator=machine.dmem.load(static_cast<int>(vector)+5).first;strict &= accumulator.mode==TernaryMode::T40 && accumulator.isZero();for(int word=250;word<279;++word){const auto value=machine.dmem.load(static_cast<int>(vector)+word).first;strict &= value.mode==TernaryMode::T40 && value.isZero();}check(strict,"initialized supervisor exact canonical V3 faults/accumulator/reserved");check(serviceKernelWord(machine,14725800+164+slot)==transportProcessGeneration(machine,slot),"supervisor ready bound to current issued generation");}
            if(pid==421 || pid==422) {ran.insert(pid);if(rec.syscall_id==44)exited.insert(pid);}
            if(pid==202 && rec.syscall_id==20 && (rec.syscall_arg0==21 || rec.syscall_arg0==22) && (rec.syscall_arg1==421 || rec.syscall_arg1==422)) {
                fork_pcs[static_cast<int>(rec.syscall_arg1)]=rec.pc;
                const auto live_seed=machine.vregfile.reg[1].lane[0];
                assertions<<"{\"event\":\"live_parent_vector_seed_at_fork_entry\",\"pid\":202,\"value_exact\":";transportExactWord(assertions,live_seed);assertions<<"}\n";
                check(vm::ops::toLong(live_seed)==7,"native live vector seed7 survives until actual fork entry");
            }
        }
        if(rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET)for(const auto& [child_pid,pc]:fork_pcs)if(!snapshot.count(child_pid) && transportRuntimeSlot(machine,child_pid)>0 && serviceKernelWord(machine,669000+transportRuntimeSlot(machine,202)*32)==pc+1 && serviceKernelWord(machine,669000+transportRuntimeSlot(machine,202)*32+19)==child_pid) {
            snapshot.insert(child_pid);const int slot=transportRuntimeSlot(machine,child_pid);
            const int parent_slot=transportRuntimeSlot(machine,202);
            const auto parent_ctx=serviceKernelWord(machine,390000+parent_slot*9+5),child_ctx=slot<0?0:serviceKernelWord(machine,390000+slot*9+5);
            check(slot>0 && child_ctx>0,"created distinct native child context");
            check(child_ctx>0 && serviceKernelWord(machine,static_cast<int>(child_ctx))==pc+1,"child saved fork EPC advances once");
            check(child_ctx>0 && serviceKernelWord(machine,static_cast<int>(child_ctx)+18)==1 && serviceKernelWord(machine,static_cast<int>(child_ctx)+19)==0 && serviceKernelWord(machine,static_cast<int>(child_ctx)+20)==0,"child raw fork tuple [1,0,0]");
            const auto ppt=serviceKernelWord(machine,static_cast<int>(parent_ctx)+4),cpt=child_ctx>0?serviceKernelWord(machine,static_cast<int>(child_ctx)+4):0;
            assertions<<"{\"event\":\"native_fork_context_snapshot\",\"parent_pid\":202,\"child_pid\":"<<child_pid<<",\"slot\":"<<slot<<",\"parent_context\":"<<parent_ctx<<",\"child_context\":"<<child_ctx<<",\"expected_epc\":"<<pc+1<<",\"child_epc\":"<<serviceKernelWord(machine,static_cast<int>(child_ctx))<<",\"parent_sp\":"<<serviceKernelWord(machine,static_cast<int>(parent_ctx)+31)<<",\"child_sp\":"<<serviceKernelWord(machine,static_cast<int>(child_ctx)+31)<<",\"parent_dmem_ptbr\":"<<ppt<<",\"child_dmem_ptbr\":"<<cpt<<",\"child_imem_ptbr\":"<<serviceKernelWord(machine,static_cast<int>(child_ctx)+2)<<",\"child_tuple\":["<<serviceKernelWord(machine,static_cast<int>(child_ctx)+18)<<','<<serviceKernelWord(machine,static_cast<int>(child_ctx)+19)<<','<<serviceKernelWord(machine,static_cast<int>(child_ctx)+20)<<"]}\n";
            check(cpt==19300+slot*1024+512 && cpt!=ppt,"child owns hardware DMEM table");
            check(child_ctx>0 && serviceKernelWord(machine,static_cast<int>(child_ctx)+2)==19300+slot*1024,"child owns hardware IMEM table");
            check(child_ctx>0 && serviceKernelWord(machine,static_cast<int>(child_ctx)+31)==serviceKernelWord(machine,static_cast<int>(parent_ctx)+31),"live stack pointer copied");
            bool scalar_exact=child_ctx>0;for(int word=6;word<32 && scalar_exact;++word)if(word<18 || word>20){const auto a=machine.dmem.load(static_cast<int>(parent_ctx)+word).first,b=machine.dmem.load(static_cast<int>(child_ctx)+word).first;scalar_exact=a.mode==b.mode && a.bits.lo==b.bits.lo && a.bits.hi==b.bits.hi;}check(scalar_exact,"saved scalar register and SP tags copied exactly");
            const auto pvec=serviceKernelWord(machine,390000+parent_slot*9+8),cvec=slot<0?0:serviceKernelWord(machine,390000+slot*9+8);bool vector_equal=pvec>0 && cvec>0;
            const auto seed=machine.dmem.load(static_cast<int>(pvec)+34+27).first;
            assertions<<"{\"event\":\"native_parent_vector_seed\",\"pid\":202,\"child_pid\":"<<child_pid<<",\"value_exact\":";transportExactWord(assertions,seed);assertions<<"}\n";
            check(pvec>0 && vm::ops::toLong(seed)==7,"nonzero native seed7 observed in parent saved vector1 lane0 at fork");
            for(int word=0;word<279 && vector_equal;++word){const auto a=machine.dmem.load(static_cast<int>(pvec)+word).first,b=machine.dmem.load(static_cast<int>(cvec)+word).first;vector_equal=a.mode==b.mode && a.bits.lo==b.bits.lo && a.bits.hi==b.bits.hi;}
            check(vector_equal,"all279 stored V3 words copied exactly; nonzero seed checked separately");
            check(slot>0 && transportProcessGeneration(machine,slot)>transportProcessGeneration(machine,parent_slot),"globally fresh child generation");
            if(cpt>0)check(serviceKernelWord(machine,static_cast<int>(cpt)+64)==0,"external window mapping not inherited");
            if(cpt>0 && ppt>0){const auto a=serviceKernelWord(machine,static_cast<int>(ppt)+11000/729),b=serviceKernelWord(machine,static_cast<int>(cpt)+11000/729);check(a>0 && b>0 && a/19683!=b/19683,"private scratch has distinct physical backing");}
            if(cpt>0 && ppt>0){const auto pa=serviceKernelWord(machine,static_cast<int>(ppt)+11003/729)/19683*729+11003%729,ca=serviceKernelWord(machine,static_cast<int>(cpt)+11003/729)/19683*729+11003%729;const auto a=machine.dmem.load(static_cast<int>(pa)).first,b=machine.dmem.load(static_cast<int>(ca)).first;check(a.mode==TernaryMode::T40 && vm::ops::toLong(a)==8,"actual scalar STORE uses canonical T40 value8; former T5 expectation was a fixture error");check(vm::ops::toLong(a)==vm::ops::toLong(b),"private scalar value8 preserved; typed/wide semantics remain separate");}
        }
        for(int slot=1;slot<100;++slot)if(serviceKernelWord(machine,390000+slot*9+2)>0){const int row_pid=static_cast<int>(serviceKernelWord(machine,390000+slot*9));if(row_pid==202 || ran.count(row_pid)){const auto identity=std::make_tuple(row_pid,slot,transportProcessGeneration(machine,slot));const auto checks=serviceUserWord(machine,10042,slot);if(checks<=0){initial_rows.insert(identity);continue;}auto& observed=observations[identity];observed[0]=std::max(observed[0],serviceUserWord(machine,10040,slot));const auto first=serviceUserWord(machine,10041,slot);if(observed[1]==0 && first>0)observed[1]=first;observed[2]=std::max(observed[2],checks);if(observed[0]>0 && observed[1]>0){check(false,"real guest first failure "+std::to_string(observed[1])+" PID"+std::to_string(row_pid));}}}
        parent_done=serviceUserWord(machine,10090,configured_parent_slot)==777;return failed || parent_done;
    }," mov r1, 7\n vbcast.t5 v1, r1\n");
    if(parent_done){check(ran.count(421) && ran.count(422) && exited.count(421) && exited.count(422),"both children actually execute syscalls and exit");}
    std::ostringstream guests;guests<<"{\"lifetimes\":[";bool comma=false;for(const auto& [identity,observation]:observations){if(comma)guests<<',';comma=true;const auto [pid,slot,generation]=identity;guests<<"{\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"generation\":"<<generation<<",\"failures\":"<<observation[0]<<",\"first_failure\":"<<observation[1]<<",\"checks\":"<<observation[2]<<"}";}guests<<"]}\n";
    std::ostringstream initial;initial<<"{\"initial_rows_seen_before_genuine_checks\":[";comma=false;for(const auto& [pid,slot,generation]:initial_rows){if(comma)initial<<',';comma=true;initial<<"{\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"generation\":"<<generation<<",\"checks\":0,\"execution_proof\":false}";}initial<<"]}\n";
    const bool saved=transportWriteArtifact(base/"guest_assertions.json",guests.str()) && transportWriteArtifact(base/"initial_lifetimes.json",initial.str());assertions.flush();
    const bool passed=completed && parent_done && !failed && saved && assertions.good();
    std::ostringstream report;report<<"{\"status\":\""<<(passed?"PASS":completed && failed?"RED":"INCOMPLETE")<<"\",\"scope\":\"native-fork-v3-context-slice\",\"run_directory\":\"run_"<<n<<"\",\"parent_slot\":"<<configured_parent_slot<<",\"parent_completed\":"<<(parent_done?"true":"false")<<",\"full_fork_acceptance\":false,\"private_typed_wide_validation\":\"pending\",\"transport_acceptance\":false}\n";
    const bool report_saved=transportWriteArtifact(base/"report.json",report.str()) && transportWriteArtifact(root/"report.json",report.str());
    std::cout<<"compositor fork prerequisite artifact: "<<base.string()<<'\n';return require(passed && report_saved,"native fork prerequisite");
}

bool runCompositorForkWindowLifetimeProbe() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_window_lifetime";
    std::filesystem::create_directories(root);int n=1;while(std::filesystem::exists(root/("run_"+std::to_string(n))))++n;
    const auto base=root/("run_"+std::to_string(n));std::filesystem::create_directories(base);
    std::ostringstream sources;sources<<"{\"algorithm\":\"FNV-1a-64\",\"sources\":{";bool comma=false;
    for(const auto& path:std::vector<std::string>{"kernel.trit","native_kernel_trap_stub.tasm","tests/current_only_conformance.cpp","apps/os_sdk.trit","ternary_compiler_ir.h","ternary_compiler_codegen.h","SYSCALL_MANIFEST.json","TEST_MANIFEST.json","tools/trit_tool.py","docs/07_OS_Substrate/native_compositor_contract.md","architecture_contract.trit"}){if(comma)sources<<',';comma=true;sources<<'"'<<path<<"\":\""<<stableHash(readTextFile(path))<<'"';}sources<<"}}\n";
    if(!transportWriteArtifact(base/"source_hashes.json",sources.str()) || !transportWriteArtifact(base/"failure_modes.json",R"JSON({"scope":"local toy VM window lifetime after native fork; no external system","failure_modes":["child retains parent window buffer mapping","child read is inferred from page table rather than executed","missing child mapping halts whole VM instead of crashing child","parent waitpid does not wake once with real routed load page fault9","child cleanup changes parent window37/private data/fraction","copied counters falsely claim child execution","missing checkpoint or raw lifetime evidence"]})JSON"))return false;
    const auto parent=transportGuestCommon()+R"TRIT(
fn lifetime_read(buffer: t40) -> t40 { unsafe { return load(buffer); } }
fn main() -> t40 {
    unsafe { store(11000,73);store(11001,1/3); }
    let window: t40=os_window_create(3,4,20,20);if window<0 { tr_check(window,0,4301); }
    let buffer: t40=os_window_get_buffer(window);tr_positive(buffer,4302);
    unsafe { store(buffer,37);store(10083,buffer); }
    var held: t40=0;unsafe { held=load(11000); }
    let child: t40=sys_fork(21,421);
    if child==0 {
        unsafe { store(10040,0);store(10041,0);store(10042,0);store(10090,0); }
        tr_check(sys_getpid(),421,4351);tr_check(held,73,4352);
        unsafe { tr_check(load(11001),1/3,4353); }
        tr_check(os_window_get_buffer(window),-8,4354);
        let observed: t40=lifetime_read(buffer);
        unsafe { store(10091,observed);store(10090,777); }
        os_exit(45);return 0;
    }
    tr_check(child,421,4303);
    let status: t40=os_waitpid(421);
    // Parent isolation is checked before the expected child-fault assertion,
    // so the pre-repair RED still contains actual parent guard evidence.
    tr_check(held,73,4304);
    unsafe { tr_check(load(11000),73,4305);tr_check(load(11001),1/3,4306);tr_check(load(buffer),37,4307);store(10084,status); }
    tr_check(status,9,4311);
    os_waitpid(421);tr_check(os_window_destroy(window),1,4312);
    unsafe { store(10090,777); }os_futex_wait(10402,0,-1);return 0;
}
)TRIT";
    const auto supervisor=transportGuestCommon()+R"TRIT(fn main() -> t40 { os_futex_wait(10401,0,-1);return 0; })TRIT";
    bool failed=false,parent_done=false,child_ran=false,read_executed=false,read_fault=false,child_crashed=false,wait_pending=false;int wait_pc=-1,wait_returns=0;long long wait_payload=0;
    std::map<std::tuple<int,int,long long>,std::array<long long,3>> observations;
    std::array<vm::TernaryValue,32> before_registers{};
    int before_pid=0,before_pc=-1,before_slot=-1;long long before_generation=0;
    std::ofstream assertions(base/"assertions.jsonl");
    auto check=[&](bool pass,const std::string& text){assertions<<"{\"pass\":"<<(pass?"true":"false")<<",\"assertion\":\""<<jsonEscape(text)<<"\"}\n";failed |= !pass;};
    const bool completed=runTransportActors(base,{{"winlife-parent",parent,51,202,1},{"winlife-super",supervisor,2,201,0}},"",[&](vm::VMState& machine,const vm::VMExecutionRecord& rec,long long step){
        const int pid=static_cast<int>(serviceKernelWord(machine,3020)),slot=transportRuntimeSlot(machine,pid);
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User && rec.instruction.opcode==isa::Opcode::SYSCALL){
            if(pid==421){child_ran=true;if(rec.syscall_id==compiler::runtime::sys_exit)assertions<<"{\"event\":\"actual_child_continuation_exit\",\"pid\":421,\"slot\":"<<slot<<",\"process_generation\":"<<transportProcessGeneration(machine,slot)<<",\"exit_argument\":"<<rec.syscall_arg0<<",\"read_marker\":"<<serviceUserWord(machine,10090,slot)<<",\"read_value\":"<<serviceUserWord(machine,10091,slot)<<"}\n";}
            if(pid==202 && rec.syscall_id==compiler::runtime::sys_waitpid && rec.syscall_arg0==421 && wait_pc<0){wait_pc=rec.pc;wait_pending=true;}
        }
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User && before_pid==421 && before_pc==rec.pc && rec.instruction.opcode==isa::Opcode::LOAD){
            const auto address=vm::ops::toLong(before_registers[rec.instruction.rs1])+rec.instruction.imm;
            const auto buffer=serviceUserWord(machine,10083,51);
            if(address==buffer && buffer>0){read_executed=true;read_fault=rec.trap_observed && rec.trap_cause==isa::OS_CAUSE_LOAD_PAGE_FAULT;
                assertions<<"{\"event\":\"actual_child_window_address_load\",\"step\":"<<step<<",\"pid\":421,\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"pc\":"<<before_pc<<",\"virtual_address\":"<<address<<",\"load_completed\":"<<(!rec.trap_observed?"true":"false")<<",\"trap_observed\":"<<(rec.trap_observed?"true":"false")<<",\"trap_cause\":"<<rec.trap_cause<<",\"destination_register_after_exact\":";transportExactWord(assertions,machine.regfile.readPhysical(rec.instruction.rd));assertions<<"}\n";
            }
        }
        const int child_slot=transportRuntimeSlot(machine,421);if(child_slot>0 && serviceKernelWord(machine,390000+child_slot*9+2)==9 && serviceKernelWord(machine,130100+child_slot)==9)child_crashed=true;
        if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET && pid==202 && wait_pending && wait_pc>=0 && machine.pc==wait_pc+1){wait_pending=false;++wait_returns;wait_payload=vm::ops::toLong(machine.regfile.read(14));assertions<<"{\"event\":\"actual_parent_waitpid_return\",\"pid\":202,\"slot\":51,\"process_generation\":"<<transportProcessGeneration(machine,51)<<",\"return_epc\":"<<machine.pc<<",\"tuple\":["<<vm::ops::toLong(machine.regfile.read(13))<<','<<wait_payload<<','<<vm::ops::toLong(machine.regfile.read(15))<<"]}\n";}
        for(int row_slot:std::vector<int>{51,21})if(serviceKernelWord(machine,390000+row_slot*9+2)>0){const int row_pid=static_cast<int>(serviceKernelWord(machine,390000+row_slot*9));if(row_pid==202 || (row_pid==421 && child_ran)){const auto checks=serviceUserWord(machine,10042,row_slot);if(checks>0){auto& cached=observations[{row_pid,row_slot,transportProcessGeneration(machine,row_slot)}];cached[0]=std::max(cached[0],serviceUserWord(machine,10040,row_slot));const auto first=serviceUserWord(machine,10041,row_slot);if(cached[1]==0 && first>0)cached[1]=first;cached[2]=std::max(cached[2],checks);if(cached[0]>0 && cached[1]>0)check(false,"real guest first failure "+std::to_string(cached[1])+" PID"+std::to_string(row_pid));}}}
        parent_done=serviceUserWord(machine,10090,51)==777;return failed || parent_done;
    }," mov r1, 7\n vbcast.t5 v1, r1\n",true,[&](const vm::VMState& machine){if(machine.privilege==vm::PrivilegeMode::User){before_pid=static_cast<int>(serviceKernelWord(machine,3020));before_pc=machine.pc;before_slot=transportRuntimeSlot(machine,before_pid);before_generation=transportProcessGeneration(machine,before_slot);for(int r=0;r<32;++r)before_registers[r]=machine.regfile.readPhysical(r);}});
    check(child_ran,"child actually entered native User syscalls");check(read_executed,"child actually executed local VM window-address LOAD");
    if(parent_done){check(read_fault && child_crashed,"only child routed load page fault9 and became CRASHED");check(wait_returns==1 && wait_payload==9,"actual parent waitpid returns fault9 once");}
    std::ostringstream guests;guests<<"{\"lifetimes\":[";comma=false;for(const auto& [identity,count]:observations){if(comma)guests<<',';comma=true;const auto [pid,slot,generation]=identity;guests<<"{\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"generation\":"<<generation<<",\"checks\":"<<count[2]<<",\"failures\":"<<count[0]<<",\"first_failure\":"<<count[1]<<"}";}guests<<"]}\n";
    const bool saved=transportWriteArtifact(base/"guest_assertions.json",guests.str());assertions.flush();
    const bool passed=completed && parent_done && !failed && saved && assertions.good();
    std::ostringstream report;report<<"{\"status\":\""<<(passed?"PASS":completed && failed?"RED":"INCOMPLETE")<<"\",\"scope\":\"local-native-window-lifetime-probe\",\"run_directory\":\"run_"<<n<<"\",\"actual_child_load\":"<<(read_executed?"true":"false")<<",\"load_fault9\":"<<(read_fault?"true":"false")<<",\"waitpid_returns\":"<<wait_returns<<",\"waitpid_payload\":"<<wait_payload<<",\"full_fork_acceptance\":false,\"transport_acceptance\":false}\n";
    const bool report_saved=transportWriteArtifact(base/"report.json",report.str()) && transportWriteArtifact(root/"report.json",report.str());
    std::cout<<"local native window lifetime artifact: "<<base.string()<<'\n';return require(passed && report_saved,"native window lifetime probe");
}

bool runCompositorForkFatalUser() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_fatal_user";
    std::filesystem::create_directories(root);int n=1;while(std::filesystem::exists(root/("run_"+std::to_string(n))))++n;
    const auto base=root/("run_"+std::to_string(n));std::filesystem::create_directories(base);
    if(!transportWriteArtifact(base/"failure_modes.json",R"JSON({"specification_correction":"defined overflow is special-but-valid; independent invalid operand causes ordinary User fault","failure_modes":["finite inputs never reach actual User accumulator arithmetic","defined overflow mislabeled invalid or cause1","special accumulator/vector7 lost on real syscall save/restore","child stops before later native User work","independent invalid vector operand does not route cause1","ordinary User fault globally halts before cleanup","offender not CRASHED1 or parent wake tuple/EPC replayed","parent guards/fraction or survivor progress/vector7 lost","child copied counters falsely count as execution","malformed V3 acceptance relaxed","missing compiler/final assembly/wire patch/image/checkpoint/raw lifetime evidence"],"production_changes_allowed":false})JSON"))return false;
    std::ostringstream sources;sources<<"{\"algorithm\":\"FNV-1a-64\",\"sources\":{";bool comma=false;
    for(const auto& path:std::vector<std::string>{"kernel.trit","native_kernel_trap_stub.tasm","tests/current_only_conformance.cpp","apps/os_sdk.trit","ternary_compiler_ir.h","ternary_compiler_codegen.h","SYSCALL_MANIFEST.json","TEST_MANIFEST.json","tools/trit_tool.py","docs/07_OS_Substrate/native_compositor_contract.md","ternary_vm.h","ternary_vm_state.h","ternary_native_ops.h","ternary_scalar.h"}){if(comma)sources<<',';comma=true;sources<<'"'<<path<<"\":\""<<stableHash(readTextFile(path))<<'"';}sources<<"}}\n";
    if(!transportWriteArtifact(base/"source_hashes.json",sources.str()))return false;
    const auto parent=transportGuestCommon()+R"TRIT(
fn numeric_overflow(seed: t40) -> t40 { return seed; }
fn numeric_illegal(seed: t40) -> t40 { return seed; }
fn main() -> t40 {
    unsafe { store(11000,73);store(11001,1/3);store(11002,-91); }
    var held: t40=0;unsafe { held=load(11000); }
    let child: t40=sys_fork(21,421);
    if child==0 {
        unsafe { store(10040,0);store(10041,0);store(10042,0);store(10090,0); }
        tr_check(sys_getpid(),421,4451);tr_check(held,73,4452);
        os_sleep_ms(1);
        let special: t40=numeric_overflow(1);
        // No guest comparison of the special value. Observe its exact word.
        unsafe { store(11010,special);store(10080,1); }
        tr_check(sys_getpid(),421,4453);
        unsafe { store(11000,99);tr_check(load(11001),1/3,4454);store(10080,2); }
        numeric_illegal(3);
        unsafe { store(10090,999); }os_exit(45);return 0;
    }
    tr_check(child,421,4401);
    let status: t40=os_waitpid(421);
    tr_check(held,73,4402);
    unsafe { tr_check(load(11000),73,4403);tr_check(load(11001),1/3,4404);tr_check(load(11002),-91,4405); }
    tr_check(status,1,4406);os_waitpid(421);
    tr_send(10,73);tr_check(tr_recv(1),118,4407);
    unsafe { tr_check(load(11000),73,4408);tr_check(load(11001),1/3,4409);store(10090,777); }
    os_futex_wait(10402,0,-1);return 0;
}
)TRIT";
    const auto supervisor=transportGuestCommon()+R"TRIT(
fn numeric_seed(seed: t40) -> t40 { return seed; }
fn main() -> t40 {
    tr_check(numeric_seed(7),7,4471);tr_check(sys_getpid(),201,4472);
    unsafe { store(11000,619);store(11001,2/7); }
    let input: t40=tr_recv(10);tr_check(input,73,4473);
    var sum: t40=input;var i: t40=1;while i<10 { sum=sum+i;i=i+1; }
    tr_check(sum,118,4474);unsafe { tr_check(load(11000),619,4475);tr_check(load(11001),2/7,4476); }
    tr_send(1,sum);unsafe { store(10090,777); }os_futex_wait(10401,0,-1);return 0;
}
)TRIT";
    TransportActor p{"fatal-parent",parent,1,202,1},s{"fatal-super",supervisor,2,201,0};p.numeric_native_fixture=true;s.numeric_native_fixture=true;
    bool failed=false,parent_done=false,super_done=false,overflow=false,finite_loaded=false,arithmetic_no_fault=true,stored=false,loaded=false,roundtrip=false,illegal=false,crashed=false,reaped=false,child_further=false,continued=false,wait_pending=false;
    int before_pid=0,before_pc=-1,before_slot=-1,getpid_pc=-1,wait_pc=-1,wait_returns=0,multiplies=0;long long before_generation=0;
    vm::TernaryValue before_accumulator=vm::ops::fromLong(0),before_input=vm::ops::fromLong(0),before_multiplier=vm::ops::fromLong(0),overflow_word=vm::ops::fromLong(0);
    std::set<int> ran;std::map<std::tuple<int,int,long long>,std::array<long long,3>> observations;
    std::ofstream assertions(base/"assertions.jsonl"),numeric(base/"numeric_instructions.jsonl"),trap_path(base/"trap_path.jsonl");
    auto check=[&](bool pass,const std::string& text){assertions<<"{\"pass\":"<<(pass?"true":"false")<<",\"assertion\":\""<<jsonEscape(text)<<"\"}\n";failed |= !pass;};
    auto exact_equal=[](const vm::TernaryValue& a,const vm::TernaryValue& b){return a.mode==b.mode && a.bits.lo==b.bits.lo && a.bits.hi==b.bits.hi;};
    const bool completed=runTransportActors(base,{p,s},"",[&](vm::VMState& machine,const vm::VMExecutionRecord& rec,long long step){
        const int pid=static_cast<int>(serviceKernelWord(machine,3020)),slot=transportRuntimeSlot(machine,pid);
        const vm::TernaryValue accumulator=machine.accumulator;
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User){
            ran.insert(before_pid);
            if(before_pid==421 && rec.instruction.opcode==isa::Opcode::ALOAD){numeric<<"{\"event\":\"actual_User_ALOAD\",\"step\":"<<step<<",\"pid\":421,\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"pc\":"<<before_pc<<",\"input_exact\":";transportExactWord(numeric,before_input);numeric<<",\"accumulator_after_exact\":";transportExactWord(numeric,accumulator);numeric<<",\"trap_observed\":"<<(rec.trap_observed?"true":"false")<<"}\n";finite_loaded=!before_input.isInvalid() && !before_input.asTriple().isSpecial() && !accumulator.isInvalid() && !accumulator.asTriple().isSpecial();check(finite_loaded && vm::ops::toLong(accumulator)==1,"real User ALOAD valid finite1");}
            if(before_pid==421 && rec.instruction.opcode==isa::Opcode::AMUL){
                ++multiplies;arithmetic_no_fault &= !rec.trap_observed && !accumulator.isInvalid() && vm::ops::toLong(before_multiplier)==3;
                if(multiplies==1 || (!overflow && accumulator.asTriple().isSpecial())){
                    numeric<<"{\"event\":\"actual_User_AMUL\",\"step\":"<<step<<",\"pid\":"<<before_pid<<",\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"pc\":"<<before_pc<<",\"ordinal\":"<<multiplies<<",\"input_exact\":";transportExactWord(numeric,before_multiplier);numeric<<",\"accumulator_before_exact\":";transportExactWord(numeric,before_accumulator);numeric<<",\"before_special\":"<<(before_accumulator.asTriple().isSpecial()?"true":"false")<<",\"before_invalid\":"<<(before_accumulator.isInvalid()?"true":"false")<<",\"accumulator_after_exact\":";transportExactWord(numeric,accumulator);numeric<<",\"after_special\":"<<(accumulator.asTriple().isSpecial()?"true":"false")<<",\"after_invalid\":"<<(accumulator.isInvalid()?"true":"false")<<",\"trap_observed\":"<<(rec.trap_observed?"true":"false")<<",\"trap_cause\":"<<rec.trap_cause<<"}\n";
                }
                if(!overflow && accumulator.asTriple().isSpecial()){overflow=true;overflow_word=accumulator;check(!before_accumulator.isInvalid() && !before_accumulator.asTriple().isSpecial() && !rec.trap_observed && !accumulator.isInvalid(),"finite User multiply reaches defined special without cause1");}
            }
            if(rec.instruction.opcode==isa::Opcode::SYSCALL){
                if(before_pid==202 || before_pid==201 || before_pid==421){const auto vector=machine.vregfile.reg[1].lane[0];check(vm::ops::toLong(vector)==7,"actual scheduled User vector7 PID"+std::to_string(before_pid));}
                if(before_pid==421 && rec.syscall_id==compiler::runtime::sys_getpid && serviceUserWord(machine,10080,21)==1){getpid_pc=rec.pc;check(overflow && exact_equal(accumulator,overflow_word),"child enters real syscall with exact special accumulator");}
                if(before_pid==421 && rec.syscall_id==compiler::runtime::sys_exit)continued=true;
                if(before_pid==202 && rec.syscall_id==compiler::runtime::sys_waitpid && rec.syscall_arg0==421 && wait_pc<0){wait_pc=rec.pc;wait_pending=true;}
            }
            if(before_pid==421 && rec.instruction.opcode==isa::Opcode::VMAC){
                illegal=rec.instruction.rs1==8 && rec.trap_observed && rec.trap_cause==1;
                child_further=serviceUserWord(machine,10080,21)==2 && serviceUserWord(machine,11000,21)==99;
                numeric<<"{\"event\":\"actual_User_invalid_vector_operand\",\"step\":"<<step<<",\"pid\":421,\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"pc\":"<<before_pc<<",\"raw_bits\":"<<rec.raw_bits<<",\"source_vector\":"<<static_cast<int>(rec.instruction.rs1)<<",\"accumulator_before_exact\":";transportExactWord(numeric,before_accumulator);numeric<<",\"accumulator_after_exact\":";transportExactWord(numeric,accumulator);numeric<<",\"accumulator_invalid\":"<<(accumulator.isInvalid()?"true":"false")<<",\"trap_cause\":"<<rec.trap_cause<<"}\n";
                check(illegal && !before_accumulator.isInvalid() && exact_equal(before_accumulator,accumulator),"independent actual User invalid vector operand routes cause1 with valid unchanged accumulator");
                check(serviceKernelWord(machine,390000+9+2)==3 && wait_pending,"parent wait is genuinely pending at fatal User instruction");
            }
        }
        if(rec.attempted && rec.has_instruction && (rec.trap_observed || rec.instruction.opcode==isa::Opcode::VCTXSTORE || rec.instruction.opcode==isa::Opcode::VCTXLOAD || rec.instruction.opcode==isa::Opcode::ERET || rec.instruction.opcode==isa::Opcode::HALT)){
            if(illegal || (pid==421 && getpid_pc>=0 && !roundtrip))trap_path<<"{\"step\":"<<step<<",\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"process_generation\":"<<transportProcessGeneration(machine,slot)<<",\"pc\":"<<rec.pc<<",\"opcode\":"<<static_cast<int>(rec.instruction.opcode)<<",\"before_privilege\":"<<static_cast<int>(rec.before_privilege)<<",\"after_privilege\":"<<static_cast<int>(rec.after_privilege)<<",\"trap_cause\":"<<rec.trap_cause<<",\"accumulator_invalid\":"<<(accumulator.isInvalid()?"true":"false")<<"}\n";
            if(pid==421 && getpid_pc>=0 && !roundtrip && rec.instruction.opcode==isa::Opcode::VCTXSTORE){const auto context=serviceKernelWord(machine,390000+21*9+8);stored=exact_equal(machine.dmem.load(static_cast<int>(context)+5).first,overflow_word);check(stored,"native VCTXSTORE preserves exact defined special accumulator");}
            if(pid==421 && getpid_pc>=0 && !roundtrip && rec.instruction.opcode==isa::Opcode::VCTXLOAD){loaded=exact_equal(accumulator,overflow_word);check(loaded && vm::ops::toLong(machine.vregfile.reg[1].lane[0])==7,"native VCTXLOAD restores exact special and vector7");}
            if(pid==421 && getpid_pc>=0 && rec.instruction.opcode==isa::Opcode::ERET && machine.pc==getpid_pc+1){roundtrip=stored && loaded && exact_equal(accumulator,overflow_word);check(roundtrip,"actual child User EPC+1 after special syscall roundtrip");}
            if(pid==202 && wait_pending && rec.instruction.opcode==isa::Opcode::ERET && machine.pc==wait_pc+1){wait_pending=false;++wait_returns;const auto status=vm::ops::toLong(machine.regfile.read(13)),payload=vm::ops::toLong(machine.regfile.read(14)),detail=vm::ops::toLong(machine.regfile.read(15));assertions<<"{\"event\":\"actual_parent_waitpid_return\",\"pid\":202,\"slot\":1,\"process_generation\":"<<transportProcessGeneration(machine,1)<<",\"saved_epc\":"<<wait_pc<<",\"return_epc\":"<<machine.pc<<",\"tuple\":["<<status<<','<<payload<<','<<detail<<"]}\n";check(status==1 && payload==1 && detail==0,"parent canonical fatal wait tuple[1,1,0]");}
        }
        if(serviceKernelWord(machine,390000+21*9+2)==9 && serviceKernelWord(machine,130100+21)==1)crashed=true;
        if(crashed && serviceKernelWord(machine,390000+21*9+2)==0)reaped=true;
        for(int row_slot:std::vector<int>{1,2,21}){const int row_pid=static_cast<int>(serviceKernelWord(machine,390000+row_slot*9));if(ran.count(row_pid) && serviceKernelWord(machine,390000+row_slot*9+2)>0){const auto checks=serviceUserWord(machine,10042,row_slot);if(checks>0){auto& cached=observations[{row_pid,row_slot,transportProcessGeneration(machine,row_slot)}];cached[0]=std::max(cached[0],serviceUserWord(machine,10040,row_slot));const auto first=serviceUserWord(machine,10041,row_slot);if(cached[1]==0 && first>0)cached[1]=first;cached[2]=std::max(cached[2],checks);if(cached[0]>0 && cached[1]>0)check(false,"real guest first failure "+std::to_string(cached[1])+" PID"+std::to_string(row_pid));}}}
        parent_done=serviceUserWord(machine,10090,1)==777;super_done=serviceUserWord(machine,10090,2)==777;
        if(rec.instruction.opcode==isa::Opcode::HALT && rec.attempted)check(false,"unexpected global HALT during ordinary User lifecycle");
        return failed || (parent_done && super_done);
    }," mov r1, 7\n vbcast.t5 v1, r1\n",false,[&](const vm::VMState& machine){if(machine.privilege==vm::PrivilegeMode::User){before_pid=static_cast<int>(serviceKernelWord(machine,3020));before_pc=machine.pc;before_slot=transportRuntimeSlot(machine,before_pid);before_generation=transportProcessGeneration(machine,before_slot);before_accumulator=machine.accumulator;before_input=machine.regfile.readPhysical(19);before_multiplier=machine.regfile.readPhysical(20);}},true);
    check(finite_loaded && multiplies==1200 && overflow && arithmetic_no_fault,"actual finite overflow sequence completed without arithmetic fault");
    check(roundtrip && child_further,"special native roundtrip followed by actual finite child work");
    check(illegal && crashed && !continued,"only offender CRASHED1 without continuation exit45");
    check(wait_returns==1 && parent_done && super_done && reaped,"once parent wake, cleanup/reap and independent survivor progress");
    std::ostringstream guests;guests<<"{\"lifetimes\":[";comma=false;for(const auto& [identity,count]:observations){if(comma)guests<<',';comma=true;const auto [pid,slot,generation]=identity;guests<<"{\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"generation\":"<<generation<<",\"checks\":"<<count[2]<<",\"failures\":"<<count[0]<<",\"first_failure\":"<<count[1]<<"}";}guests<<"]}\n";
    const bool saved=transportWriteArtifact(base/"guest_assertions.json",guests.str());assertions.flush();numeric.flush();trap_path.flush();
    const bool passed=completed && !failed && saved && assertions.good() && numeric.good() && trap_path.good();
    std::ostringstream report;report<<"{\"status\":\""<<(passed?"PASS":ran.count(421) && failed?"RED":"INCOMPLETE")<<"\",\"scope\":\"native-defined-special-and-fatal-User-lifecycle\",\"run_directory\":\"run_"<<n<<"\",\"overflow_observed\":"<<(overflow?"true":"false")<<",\"native_special_roundtrip\":"<<(roundtrip?"true":"false")<<",\"independent_illegal_cause1\":"<<(illegal?"true":"false")<<",\"waitpid_returns\":"<<wait_returns<<",\"fixture_mutates_production_source\":false,\"malformed_v3_lifetime_validation\":\"pending separate fixture\",\"full_fork_acceptance\":false,\"transport_acceptance\":false}\n";
    const bool report_saved=transportWriteArtifact(base/"report.json",report.str()) && transportWriteArtifact(root/"report.json",report.str());
    std::cout<<"native numeric/fatal User artifact: "<<base.string()<<'\n';return require(passed && report_saved,"native numeric and fatal User lifecycle");
}

// Generic Kernel word classification E2E. Golden semantic expectations were
// frozen before this fixture; production VM/kernel classification remains absent.
struct WordClassInput {
    std::string name, provenance;
    int expected=0;
    bool seeded=false;
    sandbox::vm::TernaryValue input;
};
std::vector<WordClassInput> wordClassInputs() {
    using namespace sandbox;
    std::vector<WordClassInput> rows;
    auto native=[&](const char* name,int code){rows.push_back({name,"actual compiled User arithmetic/STORE or native extension",code,false,{}});};
    for(const auto* n:{"positive_zero","negative_zero","positive_integer","negative_integer","rgb255","rgb256"})native(n,5);
    for(const auto* n:{"third","negative_seventh","three_halves"})native(n,4);
    native("native_overflow",3);
    auto seed=[&](const char* name,int code,vm::TernaryValue value){rows.push_back({name,"trusted typed INPUT at compiled per-query setup marker; never a result",code,true,value});};
    seed("T1",1,vm::TernaryValue::zero(TernaryMode::T1));native("T5",1);
    seed("T10",1,vm::TernaryValue::zero(TernaryMode::T10));seed("T20",1,vm::TernaryValue::zero(TernaryMode::T20));native("T50",1);
    seed("L1",1,vm::TernaryValue::zero(TernaryMode::L1));seed("L5",1,vm::TernaryValue::zero(TernaryMode::L5));
    seed("L10",1,vm::TernaryValue::zero(TernaryMode::L10));seed("L20",1,vm::TernaryValue::zero(TernaryMode::L20));
    seed("L40",1,vm::TernaryValue::zero(TernaryMode::L40));seed("L50",1,vm::TernaryValue::zero(TernaryMode::L50));
    seed("invalid_positional",2,vm::TernaryValue::fromTriple(Triple{12157665459056928801ULL}));
    auto high=vm::ops::fromLong(17);high.bits.hi=1;seed("invalid_highbits",2,high);
    seed("special_overflow",3,vm::TernaryValue::fromTriple(Triple{Triple::OVERFLOW_DATA}));
    seed("special_underflow",3,vm::TernaryValue::fromTriple(Triple{Triple::UNDERFLOW_DATA}));
    // Construct exact INPUT encodings directly from their mathematical ternary
    // digits. No classifier, conversion-to-integer, or result drives the oracle.
    auto exact=[](int sign,int exponent,bool low=false){std::array<int8_t,40> digits{};digits[32]=static_cast<int8_t>(sign);if(low)digits[0]=1;
        int n=exponent;for(int i=33;i<40;++i){int r=n%3;if(r>1)r-=3;if(r<-1)r+=3;digits[i]=static_cast<int8_t>(r);n=(n-r)/3;}
        return vm::TernaryValue::fromTriple(Triple::pack(digits));};
    seed("huge_positive",5,exact(1,100));seed("huge_negative",5,exact(-1,100));
    seed("tiny_positive",4,exact(1,-100));seed("tiny_negative",4,exact(-1,-100));
    seed("exponent31_lowtrit",4,exact(1,31,true));seed("zero_exp_negative",5,exact(0,-100));seed("zero_exp_positive",5,exact(0,100));
    rows.push_back({"address_negative","fixture-only physical selector -1",0,false,{}});
    rows.push_back({"address_onepast","fixture-only physical selector actual dmem.size()",0,false,{}});
    return rows;
}
bool wordClassSame(const sandbox::vm::TernaryValue& a,const sandbox::vm::TernaryValue& b) {
    return a.mode==b.mode && a.bits.lo==b.bits.lo && a.bits.hi==b.bits.hi;
}
int wordClassPhysical(const sandbox::vm::VMState& m,int slot,int address) {
    const auto ctx=serviceKernelWord(m,390000+slot*9+5),pt=serviceKernelWord(m,static_cast<int>(ctx)+4);
    return static_cast<int>(serviceKernelWord(m,static_cast<int>(pt)+address/sandbox::vm::MMU_PAGE_WORDS)/19683*sandbox::vm::MMU_PAGE_WORDS)+address%sandbox::vm::MMU_PAGE_WORDS;
}
std::string wordClassGuestPrefix() {
    std::string g=transportGuestCommon()+R"TRIT(
fn word_seed(value: t40) -> t40 {return value;}
fn word_deny_read(value: t40) -> t40 {return value;}
fn word_deny_write(value: t40) -> t40 {return value;}
fn word_deny_rmw(value: t40) -> t40 {return value;}
fn main() -> t40 {
    unsafe {store(11000,73);store(11001,1/3);store(11002,-91);}
)TRIT";
    for(int n=0;n<18;++n)g+="let live"+std::to_string(n)+": t40=sys_getpid()+"+std::to_string(n)+";\n";
    return g;
}
std::string wordClassGuestEnd() {
    std::string g="tr_check(sys_getpid(),202,6100);unsafe {tr_check(load(11000),73,6101);tr_check(load(11001),1/3,6102);tr_check(load(11002),-91,6103);}\n";
    g+="var checksum: t40=0;\n";
    for(int n=0;n<18;++n)g+="tr_check(live"+std::to_string(n)+",202+"+std::to_string(n)+",6120+"+std::to_string(n)+");checksum=checksum+live"+std::to_string(n)+";\n";
    return g+"tr_check(checksum,3789,6140);unsafe {store(10091,checksum);store(10090,777);}os_futex_wait(10402,0,-1);return 0;}\n";
}
bool runKernelWordClassification() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/word_classification";
    std::filesystem::create_directories(root);int number=1;while(std::filesystem::exists(root/("run_"+std::to_string(number))))++number;
    const auto base=root/("run_"+std::to_string(number));std::filesystem::create_directories(base);
    const auto rows=wordClassInputs();bool failed=false;std::ofstream assertions(base/"assertions.jsonl");
    if(!require(assertions.good(),"word classification assertions open"))return false;
    auto check=[&](bool value,const std::string& text){assertions<<"{\"pass\":"<<(value?"true":"false")<<",\"assertion\":\""<<jsonEscape(text)<<"\"}\n";failed |= !value;};
    check(transportWriteArtifact(base/"failure_modes.json",R"JSON({"frozen_before_fixture":true,"failure_modes":["Kernel LOAD/type conversion faults before classification","invalid highbits hidden by asTriple","defined special confused with invalid","fraction truncated","huge integer overflows LL","tiny negative rounds to zero","noncanonical zero rejected","scratch not restored before ERET","User read/write/RMW accepted or faults global VM","source or guard mutated","checkpoint selector/source/PC changed","missing native query called PASS","getpid real tuple overwritten","L50 support falsely implied"],"scenarios":["combined native CSR denial children before query boot","typed User queries through real getpid base dispatcher","same native CSR53 step replayed from safe checkpoint","old missing implementation RED; later cases unobserved"],"unimplemented":[61,62,63,64,65,66]})JSON"),"source-owned frozen failure matrix written");
    std::ostringstream catalog;catalog<<"{\"expected_codes_are_independent_literals\":true,\"native_results_not_injected\":true,\"full_transport_acceptance\":false,\"Stage2_acceptance\":false,\"rows\":[";
    for(size_t i=0;i<rows.size();++i){if(i)catalog<<',';catalog<<"{\"name\":\""<<rows[i].name<<"\",\"expected_class\":"<<rows[i].expected<<",\"provenance\":\""<<rows[i].provenance<<"\",\"trusted_typed_input\":"<<(rows[i].seeded?"true":"false");if(rows[i].seeded){catalog<<",\"input_encoding\":";transportExactWord(catalog,rows[i].input);}catalog<<'}';}catalog<<"]}\n";
    check(transportWriteArtifact(base/"golden_catalog.json",catalog.str()),"source-owned independent golden catalog written");
    check(transportWriteArtifact(base/"commands.json",R"JSON({"selector":"--kernel-word-classification-only","build":"cmake --build build_current_cleanup --target trit_current_conformance","contract_check":"python tools/trit_tool.py contract-check","native_instruction_runner":"runTransportActors -> vm::step; checkpoint replay -> vm::step","forbidden_selectors_invoked":false,"full_transport_acceptance":false,"Stage2_acceptance":false})JSON"),"word classification commands recorded");
    const std::string hashProgram=R"PY(import ast,hashlib,json,pathlib,sys
r=pathlib.Path.cwd();t=ast.parse((r/'tools/trit_tool.py').read_text());p=next(ast.literal_eval(n.value) for n in t.body if isinstance(n,ast.Assign) and any(isinstance(x,ast.Name) and x.id=='SOURCE_INPUTS' for x in n.targets))
pathlib.Path(sys.argv[1]).write_text(json.dumps({'algorithm':'SHA256','sources':{f:hashlib.sha256((r/f).read_bytes()).hexdigest() for f in p}},indent=2)+'\n')
)PY";
    transportWriteArtifact(base/"source_hash_program.txt",hashProgram);
    auto hashes=[&](const char* leaf){return std::system(("python - \""+(base/leaf).string()+"\" < \""+(base/"source_hash_program.txt").string()+"\"").c_str())==0;};
    if(!hashes("source_sha256_before.json"))return false;
    std::string survivor=transportGuestCommon()+R"TRIT(
fn word_vector(seed: t40) -> t40 {return seed;}
fn main() -> t40 {word_vector(7);unsafe {store(11000,619);store(11001,-1/7);}var sum: t40=0;var n: t40=1;while n<10 {sum=sum+n;n=n+1;}
tr_check(sum,45,6201);tr_check(sys_getpid(),201,6202);tr_check(tr_recv(10),73,6203);tr_send(12,sum+73);unsafe {tr_check(load(11000),619,6204);tr_check(load(11001),-1/7,6205);store(10090,777);}os_futex_wait(10401,0,-1);return 0;}
)TRIT";
    auto survivorActor=[&](){TransportActor s{"word-survivor",survivor,2,201,0};s.memory_native_extensions={{"word_vector_entry","    vbcast.t5 v1, r19\n"}};return s;};
    // One independent boot: all three actual offending children are waited once
    // and reaped before the later classification boot can fail on old CSR code.
    std::string denial=wordClassGuestPrefix()+"tr_send(10,73);\n";
    const std::array<std::string,3> denyNames={"read","write","rmw"};
    for(int i=0;i<3;++i){const auto pid=421+i,slot=21+i;denial+="let child"+std::to_string(i)+": t40=sys_fork("+std::to_string(slot)+","+std::to_string(pid)+");\nif child"+std::to_string(i)+"==0 {unsafe {store(10040,0);store(10041,0);store(10042,0);store(11000,99);store(11001,-1/3);}tr_wait_blocked(202);word_deny_"+denyNames[i]+"(1);unsafe {store(10092,999);}os_exit(45);return 0;}\ntr_check(child"+std::to_string(i)+","+std::to_string(pid)+",6300);tr_check(os_waitpid("+std::to_string(pid)+"),"+std::to_string(i==0?2:1)+",6301);unsafe {tr_check(load(11000),73,6302);tr_check(load(11001),1/3,6303);}\n";}
    denial+="tr_check(tr_recv(12),118,6310);"+wordClassGuestEnd();
    TransportActor dp{"word-denial-parent",denial,1,202,0};
    dp.memory_native_extensions={{"word_deny_read_entry","    csrr r19, dmem_class\n"},{"word_deny_write_entry","    csrw dmem_class, r19\n"},{"word_deny_rmw_entry","    csrrw r19, dmem_class, r19\n"}};
    std::array<int,3> faultCounts{},waitCounts{};std::array<bool,3> reaped{};std::map<int,int> waitPcs;bool denialParent=false,denialSurvivor=false;
    std::ofstream denialTrace(base/"denial_events.jsonl");check(denialTrace.good(),"denial trace open");
    const bool denials=runTransportActors(base/"denials",{dp,survivorActor()},"",[&](vm::VMState&m,const vm::VMExecutionRecord&r,long long step){
        const int pid=static_cast<int>(serviceKernelWord(m,3020));
        if(r.attempted && r.has_instruction && r.before_privilege==vm::PrivilegeMode::User){
            if(r.instruction.opcode==isa::Opcode::SYSCALL){if(pid==202 && r.syscall_id==compiler::runtime::sys_waitpid)waitPcs[static_cast<int>(r.syscall_arg0)]=r.pc;
                check(vm::ops::toLong(m.vregfile.reg[1].lane[0])==7,"denial actual scheduled User vector7");}
            if(pid>=421 && pid<=423 && (r.instruction.opcode==isa::Opcode::CSRR || r.instruction.opcode==isa::Opcode::CSRW || r.instruction.opcode==isa::Opcode::CSRRW)){
                const int i=pid-421;const bool target=(i==2?r.instruction.rs2:r.instruction.imm)==architecture::v2::CSR_DMEM_CLASS;
                if(target){++faultCounts[i];check(r.trap_observed && r.trap_cause==(i==0?2:1),"actual User CSR53 denial exact existing cause");
                    check(serviceKernelWord(m,390000+9+2)==3,"parent really pending at offending child instruction");
                    denialTrace<<"{\"event\":\"actual_User_CSR_denial\",\"pid\":"<<pid<<",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"raw_bits\":"<<r.raw_bits<<",\"opcode\":"<<static_cast<int>(r.instruction.opcode)<<",\"before_privilege\":"<<static_cast<int>(r.before_privilege)<<",\"after_privilege\":"<<static_cast<int>(r.after_privilege)<<",\"cause\":"<<r.trap_cause<<"}\n";}}
        }
        if(r.attempted && r.has_instruction && r.instruction.opcode==isa::Opcode::ERET && r.after_privilege==vm::PrivilegeMode::User && pid==202){for(const auto& w:waitPcs)if(m.pc==w.second+1){const int i=w.first-421;if(i>=0&&i<3&&waitCounts[i]==0){++waitCounts[i];check(vm::ops::toLong(m.regfile.read(13))==1 && vm::ops::toLong(m.regfile.read(14))==(i==0?2:1) && vm::ops::toLong(m.regfile.read(15))==0,"denial parent actual canonical wait tuple");denialTrace<<"{\"event\":\"actual_wait_return\",\"child\":"<<w.first<<",\"saved_epc\":"<<w.second<<",\"return_epc\":"<<m.pc<<"}\n";}}}
        for(int i=0;i<3;++i)if(faultCounts[i] && serviceKernelWord(m,390000+(21+i)*9+2)==0)reaped[i]=true;
        for(int s:{1,2})check(serviceUserWord(m,10040,s)==0,"denial guest assertions remain zero");
        denialParent=serviceUserWord(m,10090,1)==777;denialSurvivor=serviceUserWord(m,10090,2)==777;
        return failed || (denialParent&&denialSurvivor);
    }," mov r1, 7\n vbcast.t5 v1, r1\n",false,{},true,false,{},[](const vm::VMState&,const vm::VMExecutionRecord&r){return r.trap_observed || (r.has_instruction&&(r.instruction.opcode==isa::Opcode::CSRR || r.instruction.opcode==isa::Opcode::CSRW || r.instruction.opcode==isa::Opcode::CSRRW));});
    for(int i=0;i<3;++i)check(faultCounts[i]==1 && waitCounts[i]==1 && reaped[i],"each CSR offender faults, waited once, reaped");
    check(denials&&denialParent&&denialSurvivor,"independent complete denial lifecycle before classification");
    std::string guest=wordClassGuestPrefix()+"word_seed(1);tr_send(10,73);tr_check(tr_recv(12),118,6400);\n";
    const std::array<std::string,9> literals={"0","-0","17","-17","255","256","1/3","-1/7","3/2"};
    for(size_t i=0;i<rows.size();++i){const int addr=12000+static_cast<int>(i)*64;guest+="unsafe {store("+std::to_string(addr-1)+",701);store("+std::to_string(addr+27)+",-701);}\n";if(i<9)guest+="unsafe {store("+std::to_string(addr)+","+literals[i]+");}\n";}
    for(size_t i=0;i<rows.size();++i){const int addr=12000+static_cast<int>(i)*64;guest+="unsafe {store(10080,"+std::to_string(i+1)+");store(10081,731);store(11010,-999);}\ntr_check(sys_getpid(),202,6410);unsafe {store(10081,0);tr_check(load(11010),"+std::to_string(rows[i].expected)+",6420+"+std::to_string(i)+");tr_check(load("+std::to_string(addr-1)+"),701,6460);tr_check(load("+std::to_string(addr+27)+"),-701,6461);store("+std::to_string(addr)+",0);}\n";}
    guest+="unsafe {store(10080,0);}"+wordClassGuestEnd();
    TransportActor qp{"word-query-parent",guest,1,202,0};
    qp.memory_native_extensions={{"word_seed_entry","    .require accumulator_ai\n    aload.t40 r19\n    mov r20, 3\n    mov r21, 1200\n    mov r22, 1\nword_class_multiply_loop:\n    amul.t40 r20\n    sub.t40 r21, r21, r22\n    brp r21, word_class_multiply_loop\n    astore.t40 r19\n    mov r23, 12576\n    store r19, r23, 0\n    mov r20, 0\n    vbcast.t5 v0, r20\n    mov r23, 12704\n    vstore.t5 v0, r23, 0\n    vbcast.t50 v0, r20\n    mov r23, 12896\n    vstore.t50 v0, r23, 0\n    mov r19, 1\n"}};
    const std::string wrapper=R"TRIT(
fn word_class_query(physical: t40) -> t40 {
    unsafe {var saved: t40=csr_read(scratch);csr_write(scratch,physical);var classification: t40=csr_read(dmem_class);csr_write(scratch,saved);return classification;}
}
fn kernel_syscall_dispatch(pid: t40, service: t40, arg0: t40, arg1: t40, arg2: t40, arg3: t40) -> t40 {
    var actual: t40=word_class_base_dispatch(pid,service,arg0,arg1,arg2,arg3);
    var actual_status: t40=kload(SYS_STATUS_ADDR);var actual_payload: t40=kload(SYS_PAYLOAD_ADDR);var actual_detail: t40=kload(SYS_DETAIL_ADDR);
    if service==7 {if pid==202 {if user_validate_span(pid,10080,4,0)>0 {if user_validate_span(pid,11010,1,1)>0 {
        var index: t40=user_load_word(pid,10080);
        if index>0 {if index<=34 {
            var address: t40=12000+(index-1)*64;var physical: t40=-1;
            if index<=32 {if user_validate_span(pid,address,1,0)>0 {physical=user_translate_dmem_addr(pid,address,0);}}
            if index==34 {physical=user_load_word(pid,10083);}
            var classification: t40=word_class_query(physical);user_store_word(pid,11010,classification);
        }}
    }}}}
    kstore(SYS_STATUS_ADDR,actual_status);kstore(SYS_PAYLOAD_ADDR,actual_payload);kstore(SYS_DETAIL_ADDR,actual_detail);return actual;
}
)TRIT";
    std::ofstream queryTrace(base/"query_events.jsonl"),inputs(base/"typed_inputs.jsonl"),replayTrace(base/"checkpoint_replay.jsonl");
    check(queryTrace.good()&&inputs.good()&&replayTrace.good(),"query input replay streams open");
    std::set<int> seeded,queried,returned;bool replaySaved=false,replayExecuted=false,missingRuntime=false,queryParent=false,querySurvivor=false;
    int beforeIndex=-1,selectedPhysical=-1,helperEntry=-1,multiplies=0;long long oldScratch=0;std::array<vm::TernaryValue,isa::REG_COUNT> beforeGpr{};vm::TernaryValue beforeWord,beforeLeft,beforeRight,beforeTrap;long long queryScratch=0;
    std::unique_ptr<vm::VMState> replay;int checkpointPc=-1;bool nativeQueryPending=false;std::map<int,long long> originalScratch;std::map<int,int> queryPcs;bool finiteLoaded=false,arithmeticNoFault=true,nativeSpecial=false;vm::TernaryValue priorAccumulator=vm::ops::fromLong(0);
    vm::VMExecutionRecord replayRecord;bool replayMatches=false;std::vector<vm::TernaryValue> beforeCsrs;
    auto csrState=[](const vm::VMState&m){std::vector<vm::TernaryValue> out;for(int id=0;id<53;++id)if(id!=5&&id!=7){vm::TernaryValue v;if(!m.readCSR(id,v))v=vm::TernaryValue::invalid(TernaryMode::T40);out.push_back(v);}return out;};
    auto sameWords=[](const std::vector<vm::TernaryValue>&a,const std::vector<vm::TernaryValue>&b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(!wordClassSame(a[i],b[i]))return false;return true;};
    auto rawState=[&](std::ostream&o,const vm::VMState&m){o<<"{\"scratch\":"<<m.scratch<<",\"epc\":"<<m.epc<<",\"cause\":"<<m.cause<<",\"status\":"<<m.packStatus()<<",\"tvec\":"<<m.tvec<<",\"page_fault_addr\":"<<m.page_fault_addr<<",\"page_fault_access\":"<<m.page_fault_access<<",\"user_dmem_ptbr\":"<<m.user_dmem_ptbr<<",\"user_dmem_pages\":"<<m.user_dmem_pages<<",\"gpr\":[";for(int n=0;n<isa::REG_COUNT;++n){if(n)o<<',';transportExactWord(o,m.regfile.readPhysical(n));}o<<"],\"other_csrs_except_cycle_and_timer_counter\":[";bool comma=false;for(int id=0;id<53;++id)if(id!=5&&id!=7){if(comma)o<<',';comma=true;vm::TernaryValue value;const bool readable=m.readCSR(id,value);o<<"{\"id\":"<<id<<",\"readable\":"<<(readable?"true":"false")<<",\"exact\":";transportExactWord(o,value);o<<'}';}o<<"],\"trap_reg\":";transportExactWord(o,m.trap_reg);o<<"}";};
    const bool queryRun=runTransportActors(base/"queries",{qp,survivorActor()},"",[&](vm::VMState&m,const vm::VMExecutionRecord&r,long long step){
        if(r.attempted&&r.has_instruction&&r.before_privilege==vm::PrivilegeMode::User&&r.instruction.opcode==isa::Opcode::SYSCALL){
            check(wordClassSame(m.vregfile.reg[1].lane[0],vm::TernaryValue::fromT5(native_ops::fromIntT5(7))),"classification scheduled User vector7 exact T5");
            if(serviceKernelWord(m,3020)==202&&r.syscall_id==compiler::runtime::sys_getpid&&serviceUserWord(m,10081,1)==731){const int index=static_cast<int>(serviceUserWord(m,10080,1))-1;queryPcs[index]=r.pc;queryTrace<<"{\"event\":\"actual_query_getpid_entry\",\"index\":"<<index<<",\"pc\":"<<r.pc<<"}\n";}
        }
        if(r.attempted&&r.has_instruction&&r.before_privilege==vm::PrivilegeMode::User&&r.instruction.opcode==isa::Opcode::VSTORE){const int address=static_cast<int>(vm::ops::toLong(m.regfile.read(23)));if(address==12704||address==12896){
            const auto mode=address==12704?TernaryMode::T5:TernaryMode::T50;
            inputs<<"{\"event\":\"actual_User_VSTORE_catalog_extent\",\"pc\":"<<r.pc<<",\"raw_bits\":"<<r.raw_bits<<",\"base\":"<<address<<",\"extent_words\":27,\"guard_offsets\":[-1,27],\"lanes\":[";
            for(int lane=0;lane<27;++lane){if(lane)inputs<<',';const auto value=transportUserValue(m,address+lane,1);transportExactWord(inputs,value);check(value.mode==mode,"actual User VSTORE preserves required numeric type tag in every owned lane");}inputs<<"]}\n";
        }}
        if(r.attempted&&r.has_instruction&&r.before_privilege==vm::PrivilegeMode::User&&r.instruction.opcode==isa::Opcode::ALOAD){priorAccumulator=static_cast<vm::TernaryValue>(m.accumulator);finiteLoaded=!r.trap_observed&&wordClassSame(priorAccumulator,vm::ops::fromLong(1));check(finiteLoaded,"actual User ALOAD starts from canonical finite1");}
        if(r.attempted&&r.has_instruction&&r.before_privilege==vm::PrivilegeMode::User&&r.instruction.opcode==isa::Opcode::AMUL){++multiplies;const auto accumulator=static_cast<vm::TernaryValue>(m.accumulator);arithmeticNoFault &= !r.trap_observed&&!accumulator.isInvalid()&&wordClassSame(m.regfile.read(20),vm::ops::fromLong(3));
            if(multiplies==1||(!nativeSpecial&&accumulator.asTriple().isSpecial())||multiplies==1200){inputs<<"{\"event\":\"actual_User_AMUL\",\"pc\":"<<r.pc<<",\"raw_bits\":"<<r.raw_bits<<",\"ordinal\":"<<multiplies<<",\"before_exact\":";transportExactWord(inputs,priorAccumulator);inputs<<",\"multiplier_exact\":";transportExactWord(inputs,m.regfile.read(20));inputs<<",\"after_exact\":";transportExactWord(inputs,accumulator);inputs<<",\"trap\":"<<(r.trap_observed?"true":"false")<<"}\n";}
            if(!nativeSpecial&&accumulator.asTriple().isSpecial()){nativeSpecial=true;check(finiteLoaded&&!priorAccumulator.isInvalid()&&!priorAccumulator.asTriple().isSpecial()&&!r.trap_observed&&!accumulator.isInvalid(),"actual finite User multiply reaches defined valid special without fault");}priorAccumulator=accumulator;
            if(multiplies==1200)check(nativeSpecial&&arithmeticNoFault,"actual User finite MUL3x1200 sequence complete");
        }
        if(nativeQueryPending && r.pc==checkpointPc && r.before_privilege==vm::PrivilegeMode::Kernel && r.has_instruction&&r.instruction.opcode==isa::Opcode::CSRR&&r.instruction.imm==architecture::v2::CSR_DMEM_CLASS){
            nativeQueryPending=false;queried.insert(beforeIndex);const int addr=12000+beforeIndex*64;const auto after=m.dmem.load(selectedPhysical);
            const bool unchanged=wordClassSame(beforeWord,after.first)&&wordClassSame(beforeLeft,transportUserValue(m,addr-1,1))&&wordClassSame(beforeRight,transportUserValue(m,addr+27,1));
            check(unchanged,"actual CSRR leaves original raw tagged source and guards unchanged");
            queryTrace<<"{\"event\":\"actual_Kernel_CSR53\",\"index\":"<<beforeIndex<<",\"name\":\""<<rows[beforeIndex].name<<"\",\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"raw_bits\":"<<r.raw_bits<<",\"csr\":"<<r.instruction.imm<<",\"before_privilege\":"<<static_cast<int>(r.before_privilege)<<",\"after_privilege\":"<<static_cast<int>(r.after_privilege)<<",\"selected_physical\":"<<selectedPhysical<<",\"scratch\":"<<queryScratch<<",\"trap\":"<<(r.trap_observed?"true":"false")<<",\"cause\":"<<r.trap_cause<<",\"source_before\":";transportExactWord(queryTrace,beforeWord);queryTrace<<",\"source_after\":";transportExactWord(queryTrace,after.first);queryTrace<<",\"source_and_guards_unchanged\":"<<(unchanged?"true":"false")<<",\"after\":";rawState(queryTrace,m);queryTrace<<"}\n";
            queryTrace<<"{\"event\":\"exact_query_guards_destination\",\"index\":"<<beforeIndex<<",\"left_before\":";transportExactWord(queryTrace,beforeLeft);queryTrace<<",\"left_after\":";transportExactWord(queryTrace,transportUserValue(m,addr-1,1));queryTrace<<",\"right_before\":";transportExactWord(queryTrace,beforeRight);queryTrace<<",\"right_after\":";transportExactWord(queryTrace,transportUserValue(m,addr+27,1));queryTrace<<",\"destination_after\":";transportExactWord(queryTrace,m.regfile.read(r.instruction.rd));queryTrace<<"}\n";
            if(beforeIndex==0&&replayExecuted){replayMatches=replayRecord.raw_bits==r.raw_bits&&replayRecord.pc==r.pc&&replayRecord.before_privilege==r.before_privilege&&replayRecord.after_privilege==r.after_privilege&&replayRecord.trap_observed==r.trap_observed&&replayRecord.trap_cause==r.trap_cause&&replay->pc==m.pc&&replay->status==m.status&&wordClassSame(replay->trap_reg,m.trap_reg)&&sameWords(csrState(*replay),csrState(m))&&wordClassSame(replay->dmem.load(selectedPhysical).first,after.first)&&wordClassSame(transportUserValue(*replay,addr-1,1),transportUserValue(m,addr-1,1))&&wordClassSame(transportUserValue(*replay,addr+27,1),transportUserValue(m,addr+27,1));
                for(int n=0;n<isa::REG_COUNT;++n)replayMatches &= wordClassSame(replay->regfile.readPhysical(n),m.regfile.readPhysical(n));
                check(replayMatches,"native checkpoint replay exactly matches original instruction result trap cause CSR GPR source guards");replayTrace<<"{\"compared_to_original_native_step\":true,\"exact_poststate_match\":"<<(replayMatches?"true":"false")<<"}\n";}
            if(r.trap_observed){missingRuntime=r.trap_cause==1&&replayMatches&&replayRecord.trap_observed&&replayRecord.trap_cause==1;}
            else {check(wordClassSame(m.regfile.read(r.instruction.rd),vm::ops::fromLong(rows[beforeIndex].expected)),"actual class equals independent canonical T40 literal");check(sameWords(beforeCsrs,csrState(m)),"CSR query preserves all other readable CSRs except cycle and decrementing timer counter");check(wordClassSame(beforeTrap,m.trap_reg),"CSR query preserves separate trap register");for(int n=0;n<isa::REG_COUNT;++n)if(n!=r.instruction.rd)check(wordClassSame(beforeGpr[n],m.regfile.readPhysical(n)),"CSR query preserves other GPR");}
        }
        if(r.attempted&&r.has_instruction&&r.instruction.opcode==isa::Opcode::ERET&&r.after_privilege==vm::PrivilegeMode::User&&serviceKernelWord(m,3020)==202&&beforeIndex>=0&&!r.trap_observed&&queryPcs.count(beforeIndex)&&m.pc==queryPcs[beforeIndex]+1&&!returned.count(beforeIndex)){
            bool tuple=wordClassSame(m.regfile.read(13),vm::ops::fromLong(1))&&wordClassSame(m.regfile.read(14),vm::ops::fromLong(202))&&wordClassSame(m.regfile.read(15),vm::ops::fromLong(0));
            const bool restored=m.scratch==originalScratch[beforeIndex],output=wordClassSame(transportUserValue(m,11010,1),vm::ops::fromLong(rows[beforeIndex].expected));check(tuple&&restored&&output,"matched real query getpid ERET EPC+1 exact tuple class and original scratch");
            queryTrace<<"{\"event\":\"matched_query_getpid_ERET\",\"index\":"<<beforeIndex<<",\"saved_epc\":"<<queryPcs[beforeIndex]<<",\"return_epc\":"<<m.pc<<",\"tuple_exact\":[";for(int n=13;n<=15;++n){if(n>13)queryTrace<<',';transportExactWord(queryTrace,m.regfile.read(n));}queryTrace<<"],\"scratch\":"<<m.scratch<<",\"output_exact\":";transportExactWord(queryTrace,transportUserValue(m,11010,1));queryTrace<<"}\n";if(tuple&&restored&&output)returned.insert(beforeIndex);
        }
        for(int s:{1,2})check(serviceUserWord(m,10040,s)==0,"classification guest assertions zero");
        queryParent=serviceUserWord(m,10090,1)==777;querySurvivor=serviceUserWord(m,10090,2)==777;return failed||(queryParent&&querySurvivor);
    }," mov r1, 7\n vbcast.t5 v1, r1\n",false,{},true,false,[&](vm::VMState&m,const std::map<std::string,int>& labels){
        helperEntry=labels.at("word_class_query_entry");
        if(m.privilege==vm::PrivilegeMode::User&&serviceKernelWord(m,3020)==202&&serviceUserWord(m,10081,1)==731){const int index=static_cast<int>(serviceUserWord(m,10080,1))-1;if(index>=0&&index<static_cast<int>(rows.size())&&seeded.insert(index).second){
            if(rows[index].seeded){check(index<21||replaySaved,"malformed input seeded only after safe native checkpoint reload");check(m.dmem.store(wordClassPhysical(m,1,12000+index*64),rows[index].input)==vm::MemFaultCode::OK,"trusted typed source INPUT seeding");}
            m.dmem.store(wordClassPhysical(m,1,10083),vm::ops::fromLong(m.dmem.size()));
            inputs<<"{\"index\":"<<index<<",\"name\":\""<<rows[index].name<<"\",\"provenance\":\""<<rows[index].provenance<<"\",\"expected_literal\":"<<rows[index].expected<<",\"input\":";transportExactWord(inputs,transportUserValue(m,12000+index*64,1));inputs<<",\"dmem_capacity\":"<<m.dmem.size()<<"}\n";}}
        if(m.privilege==vm::PrivilegeMode::Kernel&&m.pc==helperEntry){const int index=static_cast<int>(serviceUserWord(m,10080,1))-1;originalScratch[index]=m.scratch;oldScratch=m.scratch;queryTrace<<"{\"event\":\"actual_helper_entry\",\"index\":"<<index<<",\"old_scratch\":"<<oldScratch<<",\"real_base_tuple\":["<<serviceKernelWord(m,3021)<<','<<serviceKernelWord(m,3022)<<','<<serviceKernelWord(m,3023)<<"]}\n";}
        if(m.privilege==vm::PrivilegeMode::Kernel){const auto fetched=m.imem.fetch(m.pc);if(fetched.second==vm::MemFaultCode::OK){const auto iw=isa::VersionedInstructionCodec::decode(fetched.first,isa::IsaEncodingVersion::V2);if(iw.opcode==isa::Opcode::CSRR&&iw.imm==architecture::v2::CSR_DMEM_CLASS){
            beforeIndex=static_cast<int>(serviceUserWord(m,10080,1))-1;selectedPhysical=static_cast<int>(m.scratch);queryScratch=m.scratch;checkpointPc=m.pc;nativeQueryPending=true;beforeCsrs=csrState(m);beforeTrap=m.trap_reg;
            const int addr=12000+beforeIndex*64;beforeWord=m.dmem.load(selectedPhysical).first;beforeLeft=transportUserValue(m,addr-1,1);beforeRight=transportUserValue(m,addr+27,1);for(int n=0;n<isa::REG_COUNT;++n)beforeGpr[n]=m.regfile.readPhysical(n);
            queryTrace<<"{\"event\":\"immediately_before_actual_Kernel_CSR53\",\"index\":"<<beforeIndex<<",\"state\":";rawState(queryTrace,m);queryTrace<<"}\n";
            if(!replaySaved){std::string error;const bool saved=host::detail::writeCheckpointState(base/"before_native_csr53.bin",m,&error);const bool loaded=saved&&host::detail::readCheckpointState(base/"before_native_csr53.bin",replay,&error);replaySaved=loaded;check(loaded,"safe checkpoint before native CSR53 saved and reloaded "+error);
                if(loaded){check(replay->pc==m.pc&&replay->scratch==m.scratch&&wordClassSame(replay->dmem.load(selectedPhysical).first,beforeWord),"checkpoint preserves native PC selector and exact source");vm::VMExecutionRecord rr;vm::step(*replay,&rr);replayRecord=rr;replayExecuted=rr.attempted&&rr.has_instruction&&rr.pc==m.pc&&rr.instruction.opcode==isa::Opcode::CSRR&&rr.instruction.imm==architecture::v2::CSR_DMEM_CLASS;
                    replayTrace<<"{\"native_step_executed\":"<<(replayExecuted?"true":"false")<<",\"pc\":"<<rr.pc<<",\"raw_bits\":"<<rr.raw_bits<<",\"csr\":"<<rr.instruction.imm<<",\"privilege\":"<<static_cast<int>(rr.before_privilege)<<",\"trap\":"<<(rr.trap_observed?"true":"false")<<",\"cause\":"<<rr.trap_cause<<",\"scratch\":"<<replay->scratch<<",\"source\":";transportExactWord(replayTrace,replay->dmem.load(selectedPhysical).first);replayTrace<<"}\n";
                    check(replayExecuted,"reloaded VM executes actual same native CSR53 instruction");}}
        }}}
    },[](const vm::VMState&,const vm::VMExecutionRecord&r){return r.trap_observed||(r.has_instruction&&(r.instruction.opcode==isa::Opcode::CSRR||r.instruction.opcode==isa::Opcode::CSRW||r.instruction.opcode==isa::Opcode::VSTORE));},{},160000000,wrapper);
    std::ostringstream coverage;coverage<<"{\"catalog_count\":"<<rows.size()<<",\"queried\":[";bool comma=false;for(int i:queried){if(comma)coverage<<',';comma=true;coverage<<'"'<<rows[i].name<<'"';}coverage<<"],\"unobserved\":[";comma=false;for(size_t i=0;i<rows.size();++i)if(!returned.count(static_cast<int>(i))){if(comma)coverage<<',';comma=true;coverage<<'"'<<rows[i].name<<'"';}coverage<<"],\"cleanup\":\"each successful query source cleared by compiled User STORE only after raw per-query unchanged proof; unexecuted sources unseeded\"}\n";check(transportWriteArtifact(base/"scenario_inventory.json",coverage.str()),"scenario inventory closed");
    check(hashes("source_sha256_after.json")&&readTextFile(base/"source_sha256_before.json")==readTextFile(base/"source_sha256_after.json"),"source85 SHA256 unchanged during execution");
    auto closeStream=[](std::ofstream& out){out.flush();const bool flushed=out.good();out.close();return flushed&&!out.fail();};
    const bool denialClosed=closeStream(denialTrace),queryClosed=closeStream(queryTrace),inputsClosed=closeStream(inputs),replayClosed=closeStream(replayTrace);
    const bool tracesClosed=denialClosed&&queryClosed&&inputsClosed&&replayClosed;
    check(tracesClosed,"all native evidence streams flushed and closed");const bool assertionsClosed=closeStream(assertions);
    const bool green=queryRun&&denials&&!failed&&assertionsClosed&&tracesClosed&&returned.size()==rows.size()&&replaySaved&&replayExecuted&&replayMatches&&multiplies==1200&&finiteLoaded&&arithmeticNoFault&&nativeSpecial;
    std::ostringstream report;report<<"{\"status\":\""<<(green?"PASS":missingRuntime&&replayExecuted?"RED":"INCOMPLETE")<<"\",\"scope\":\"focused\",\"run_directory\":\"run_"<<number<<"\",\"native_missing_classification\":"<<(missingRuntime?"true":"false")<<",\"safe_checkpoint_replayed_native_instruction\":"<<(replayExecuted?"true":"false")<<",\"denials_completed\":"<<(denials?"true":"false")<<",\"queries_completed\":"<<returned.size()<<",\"full_transport_acceptance\":false,\"Stage2_acceptance\":false,\"syscalls61_66\":\"unimplemented and untested\"}\n";
    const bool reportSaved=transportWriteArtifact(base/"report.json",report.str())&&transportWriteArtifact(root/"report.json",report.str());
    std::ostringstream inventory;inventory<<"{\"algorithm\":\"FNV-1a-64\",\"self_excluded\":true,\"files\":{";bool inventoryComma=false;
    for(const auto& file:std::filesystem::recursive_directory_iterator(base))if(file.is_regular_file()){if(inventoryComma)inventory<<',';inventoryComma=true;const auto bytes=readTextFile(file.path());inventory<<'"'<<jsonEscape(std::filesystem::relative(file.path(),base).generic_string())<<"\":{\"size_bytes\":"<<bytes.size()<<",\"hash\":\""<<stableHash(bytes)<<"\"}";}inventory<<"}}\n";
    const bool inventorySaved=transportWriteArtifact(base/"artifact_inventory.json",inventory.str());
    std::cout<<"Kernel word classification artifact: "<<base.string()<<'\n';return require(green&&reportSaved&&inventorySaved,"Kernel word classification native E2E; old missing implementation remains RED");
}

bool runCompositorForkMemory() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_memory";
    std::filesystem::create_directories(root);int n=1;while(std::filesystem::exists(root/("run_"+std::to_string(n))))++n;
    const auto base=root/("run_"+std::to_string(n));std::filesystem::create_directories(base);
    if(!transportWriteArtifact(base/"failure_modes.json",R"JSON({"failure_modes":["missing actual User vector seed/store/read","finite T50 references have no nonzero high bits","scalar storage tags mistaken for typed equivalence","child typed lanes lose full numeric value","cross-page guards or mappings wrong","private mutations leak in either direction","missing real IPC/survivor/exit/wait/reap","copied child counters or injected results","missing artifacts labeled PASS"],"numeric_modes":[1,5,10,20,40,50],"L50":"separately unvalidated","production_changes":false})JSON"))return false;
    const std::string hash_program=R"PY(import hashlib,json,pathlib,sys
repo=pathlib.Path(sys.argv[1]);output=pathlib.Path(sys.argv[2])
paths=['kernel.trit','native_kernel_trap_stub.tasm','ternary_compiler_ir.h','ternary_compiler_codegen.h','apps/os_sdk.trit','SYSCALL_MANIFEST.json','tests/current_only_conformance.cpp','tools/trit_tool.py','TEST_MANIFEST.json','docs/07_OS_Substrate/native_compositor_contract.md','ternary_vm.h','ternary_vm_state.h','ternary_native_ops.h','ternary_scalar.h']
output.write_text(json.dumps({'algorithm':'SHA256','sources':{p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths}},indent=2)+'\n')
)PY";
    if(!transportWriteArtifact(base/"source_hash_program.txt",hash_program))return false;
    auto hashes=[&](const std::string& filename){auto quote=[](const std::string& value){return "\""+value+"\"";};const std::string command="python - "+quote(std::filesystem::current_path().string())+" "+quote((base/filename).string())+" < "+quote((base/"source_hash_program.txt").string());return require(std::system(command.c_str())==0 && !readTextFile(base/filename).empty(),"actual memory run SHA256 "+filename);};
    if(!hashes("source_hashes.json"))return false;
    const std::array<std::string,9> widths={"t1","t5","t10","t20","t40","t50","t50","t50","t50"};
    const std::array<int,9> modes={1,5,10,20,40,50,50,50,50};
    const std::array<std::array<int,9>,3> numerators={{{1,-8,1,-1,0,37,-1,0,1},{-1,7,2,-2,5,41,-2,2,19},{0,-9,-2,3,-4,17,-5,-2,23}}};
    const std::array<std::array<int,9>,3> denominators={{{1,1,3,7,1,3,7,1,1},{1,1,3,7,3,5,11,1,2},{1,1,3,7,9,7,13,1,2}}};
    std::string seed="    ; FIXTURE real User numeric vectors; no host memory values.\n    brz r19, memory_seed_initial\n    mov r23, 1\n    sub.t40 r23, r19, r23\n    brz r23, memory_seed_child\n    jmp memory_seed_parent\n";
    for(int generation=0;generation<3;++generation){seed+=std::string(generation==0?"memory_seed_initial:\n":generation==1?"memory_seed_child:\n":"memory_seed_parent:\n");
        for(int row=0;row<9;++row){const int addr=12000+64*row;const auto& width=widths[row];
            seed+="    mov r19, "+std::to_string(addr-1)+"\n    mov r20, "+std::to_string(701+row)+"\n    store r20, r19, 0\n    mov r19, "+std::to_string(addr+27)+"\n    mov r20, "+std::to_string(-701-row)+"\n    store r20, r19, 0\n    mov r20, "+std::to_string(numerators[generation][row])+"\n    vbcast."+width+" v0, r20\n";
            if(generation==0 && row==8){seed+="    mov r20, 3\n    vbcast.t50 v2, r20\n    mov r21, 10000\n    mov r22, 1\nmemory_special_loop:\n    vmul.t50 v0, v0, v2\n    sub.t40 r21, r21, r22\n    brp r21, memory_special_loop\n";}
            else if(denominators[generation][row]!=1){seed+="    mov r20, "+std::to_string(denominators[generation][row])+"\n    vbcast."+width+" v3, r20\n    vdiv."+width+" v0, v0, v3\n";}
            seed+="    mov r19, "+std::to_string(addr)+"\n    vstore."+width+" v0, r19, 0\n";
        }seed+="    jmp memory_seed_done\n";
    }seed+="memory_seed_done:\n    mov r19, 1\n";
    std::string read="    ; FIXTURE typed User loads are the reference, not scalar views.\n";for(int row=0;row<9;++row)read+="    mov r19, "+std::to_string(12000+64*row)+"\n    vload."+widths[row]+" v0, r19, 0\n";read+="    mov r19, 1\n";
    const auto common=transportGuestCommon()+R"TRIT(
fn memory_seed(phase: t40) -> t40 { return phase; }
fn memory_read(phase: t40) -> t40 { return phase; }
fn memory_guards() -> t40 {
    var row: t40=0;while row<9 {var addr: t40=12000+64*row;unsafe {tr_check(load(addr-1),701+row,4601);tr_check(load(addr+27),-701-row,4602);}row=row+1;}return 1;
}
)TRIT";
    const auto parent=common+R"TRIT(
fn main() -> t40 {
    unsafe {store(11000,73);store(11001,1/3);store(11002,-91);store(10080,0);}
    var held: t40=0;unsafe {held=load(11000);}memory_seed(0);memory_read(0);memory_guards();
    let child: t40=sys_fork(21,421);
    if child==0 {
        unsafe {store(10040,0);store(10041,0);store(10042,0);store(10090,0);store(10080,1);}
        tr_check(sys_getpid(),421,4651);memory_read(1);memory_guards();
        memory_seed(1);unsafe {store(10080,2);store(11000,99);store(11001,-1/3);}memory_read(2);memory_guards();
        tr_send(1,101);tr_check(tr_recv(11),202,4652);
        unsafe {store(10080,3);}memory_read(3);memory_guards();
        tr_check(held,73,4653);unsafe {tr_check(load(11000),99,4654);tr_check(load(11001),-1/3,4655);}
        tr_send(1,303);unsafe {store(10090,777);}os_exit(45);return 0;
    }
    tr_check(child,421,4611);tr_check(tr_recv(1),101,4612);
    unsafe {store(10080,4);}memory_read(4);memory_guards();
    tr_check(held,73,4613);unsafe {tr_check(load(11000),73,4614);tr_check(load(11001),1/3,4615);tr_check(load(11002),-91,4616);}
    tr_send(10,73);memory_seed(2);unsafe {store(10080,5);}memory_read(5);memory_guards();tr_send(11,202);
    tr_check(tr_recv(1),303,4617);unsafe {store(10080,6);}memory_read(6);memory_guards();tr_check(tr_recv(12),118,4618);
    let status: t40=os_waitpid(421);tr_check(held,73,4619);unsafe {tr_check(load(11000),73,4620);tr_check(load(11001),1/3,4621);tr_check(load(11002),-91,4622);}tr_check(status,45,4623);os_waitpid(421);
    unsafe {store(10080,7);}memory_read(7);memory_guards();unsafe {store(10090,777);}os_futex_wait(10402,0,-1);return 0;
}
)TRIT";
    const auto supervisor=transportGuestCommon()+R"TRIT(
fn memory_vector_seed(seed: t40) -> t40 {return seed;}
fn main() -> t40 {memory_vector_seed(7);var sum: t40=0;var i: t40=1;while i<10 {sum=sum+i;i=i+1;}tr_check(sum,45,4671);tr_check(sys_getpid(),201,4672);let input: t40=tr_recv(10);tr_check(input,73,4673);sum=sum+input;tr_check(sum,118,4674);tr_send(12,sum);unsafe {store(10090,777);}os_futex_wait(10401,0,-1);return 0;}
)TRIT";
    TransportActor p{"memory-parent",parent,1,202,0},s{"memory-super",supervisor,2,201,0};
    p.memory_native_extensions={{"memory_seed_entry",seed},{"memory_read_entry",read}};s.memory_native_extensions={{"memory_vector_seed_entry","    vbcast.t5 v1, r19\n"}};
    using Lanes=std::array<vm::TernaryValue,27>;std::map<std::pair<int,int>,Lanes> references;std::set<std::pair<int,int>> reads;
    std::array<vm::TernaryValue,32> before_scalar{};std::array<Lanes,8> before_vectors{};
    int before_pid=0,before_slot=-1,before_pc=-1,fork_pc=-1,wait_pc=-1,wait_returns=0,wide_mismatches=0,narrow_mismatches=0,special_multiplies=0;long long before_generation=0;
    bool behavior_failed=false,guest_failed=false,reference_complete=false,fork_observed=false,fork_entry=false,child_ran=false,child_exit=false,reaped=false,parent_done=false,super_done=false,native_fault=false,special_transition=false,wait_pending=false;
    std::map<std::tuple<int,int,long long>,std::array<long long,3>> counts;std::set<int> ran;std::map<int,int> fork_returns;std::set<std::tuple<int,int,long long>> fork_returned,fork_user_progress;
    std::ofstream assertions(base/"assertions.jsonl"),vectors(base/"vector_operations.jsonl"),mappings(base/"mapping_observations.jsonl"),wide_copy(base/"native_wide_copy.jsonl");
    bool copy_seen=false,copy_finished=false,copy_owned=true,copy_exact=true;long long scan_words=0,wide_words=0,narrow_words=0;std::vector<vm::TernaryValue> source_backing,source_v3;std::vector<long long> source_ptes;
    auto same_word=[](const vm::TernaryValue& a,const vm::TernaryValue& b){return a.mode==b.mode && a.bits.lo==b.bits.lo && a.bits.hi==b.bits.hi;};
    auto check=[&](bool pass,const std::string& text){assertions<<"{\"pass\":"<<(pass?"true":"false")<<",\"assertion\":\""<<jsonEscape(text)<<"\"}\n";behavior_failed |= !pass;};
    auto equal=[](const Lanes& a,const Lanes& b){for(int lane=0;lane<27;++lane)if(a[lane].mode!=b[lane].mode || a[lane].bits.lo!=b[lane].bits.lo || a[lane].bits.hi!=b[lane].bits.hi)return false;return true;};
    auto exact=[&](std::ostream& out,const Lanes& lanes){out<<'[';for(int lane=0;lane<27;++lane){if(lane)out<<',';transportExactWord(out,lanes[lane]);}out<<']';};
    const bool completed=runTransportActors(base,{p,s},"",[&](vm::VMState& machine,const vm::VMExecutionRecord& rec,long long step){
        const int pid=static_cast<int>(serviceKernelWord(machine,3020)),slot=transportRuntimeSlot(machine,pid);
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::Kernel && (rec.instruction.opcode==isa::Opcode::TSCAN || rec.instruction.opcode==isa::Opcode::VGATHER || rec.instruction.opcode==isa::Opcode::VSCATTER)){
            const auto mailbox=14725808;const int destination=static_cast<int>(serviceKernelWord(machine,mailbox+2)),source=static_cast<int>(serviceKernelWord(machine,mailbox+3));
            const int source_address=static_cast<int>(vm::ops::toLong(before_scalar[22])),destination_address=static_cast<int>(vm::ops::toLong(before_scalar[23]));
            const auto source_word=machine.dmem.load(source_address).first,destination_word=machine.dmem.load(destination_address).first;
            const auto source_gen=transportProcessGeneration(machine,source),destination_gen=transportProcessGeneration(machine,destination);
            const auto state=serviceKernelWord(machine,390000+destination*9+2),ready=serviceKernelWord(machine,14725800+164+destination);
            copy_owned &= source==1 && destination==21 && source_gen==3 && destination_gen==6 && state==6 && ready==0;
            if(!copy_seen){copy_seen=true;const int context=static_cast<int>(serviceKernelWord(machine,mailbox+1));for(int word=0;word<279;++word)source_v3.push_back(machine.dmem.load(context+word).first);for(int word=0;word<192*729;++word)source_backing.push_back(machine.dmem.load((1000+192)*729+word).first);const auto scalar=serviceKernelWord(machine,390000+9+5),pt=serviceKernelWord(machine,static_cast<int>(scalar)+4);for(int vpn=0;vpn<512;++vpn)source_ptes.push_back(serviceKernelWord(machine,static_cast<int>(pt)+vpn));}
            if(rec.instruction.opcode==isa::Opcode::TSCAN){++scan_words;const auto scan=vm::ops::toLong(machine.regfile.read(rec.instruction.rd));if(scan==32)++narrow_words;else if(scan>=-1 && scan<10)++wide_words;else copy_owned=false;}
            if(rec.instruction.opcode==isa::Opcode::VSCATTER)copy_exact &= same_word(source_word,destination_word);
            wide_copy<<"{\"step\":"<<step<<",\"kernel_pc\":"<<rec.pc<<",\"physical_pc\":"<<rec.physical_pc<<",\"actual_raw_bits\":"<<rec.raw_bits<<",\"opcode\":"<<static_cast<int>(rec.instruction.opcode)<<",\"selected_pid\":"<<pid<<",\"source_slot\":"<<source<<",\"source_generation\":"<<source_gen<<",\"destination_slot\":"<<destination<<",\"destination_generation\":"<<destination_gen<<",\"destination_state\":"<<state<<",\"destination_ready_generation\":"<<ready<<",\"source_physical_word\":"<<source_address<<",\"destination_physical_word\":"<<destination_address<<",\"scan_result\":"<<vm::ops::toLong(machine.regfile.read(21))<<",\"pre_scan_high_exact\":";transportExactWord(wide_copy,before_scalar[20]);wide_copy<<",\"source_exact\":";transportExactWord(wide_copy,source_word);wide_copy<<",\"destination_after_exact\":";transportExactWord(wide_copy,destination_word);wide_copy<<",\"actual_vector_result_exact\":";Lanes values;for(int lane=0;lane<27;++lane)values[lane]=machine.vregfile.reg[0].lane[lane];exact(wide_copy,values);wide_copy<<"}\n";
        }
        if(copy_seen && !copy_finished && serviceKernelWord(machine,14725800+164+21)==6){copy_finished=true;bool unchanged=true;const int context=700281;for(int word=0;word<279;++word)unchanged &= same_word(source_v3[word],machine.dmem.load(context+word).first);for(int word=0;word<192*729;++word)unchanged &= same_word(source_backing[word],machine.dmem.load((1000+192)*729+word).first);const auto scalar=serviceKernelWord(machine,390000+9+5),pt=serviceKernelWord(machine,static_cast<int>(scalar)+4);for(int vpn=0;vpn<512;++vpn)unchanged &= source_ptes[vpn]==serviceKernelWord(machine,static_cast<int>(pt)+vpn);check(unchanged,"native wide pass preserves full source backing mappings and saved V3");check(copy_owned && copy_exact,"actual classification/scatter preserves exact T50 with STOPPED unready child throughout");wide_copy<<"{\"event\":\"clone_ready_after_bounded_copy\",\"scan_words\":"<<scan_words<<",\"narrow_words\":"<<narrow_words<<",\"wide_words\":"<<wide_words<<",\"ready_generation\":6,\"source_unchanged\":"<<(unchanged?"true":"false")<<"}\n";}
        if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User){ran.insert(before_pid);if(before_pid==421)child_ran=true;
            const auto op=rec.instruction.opcode;
            if(fork_pc>=0 && before_pc==fork_pc+1 && (before_pid==202 || before_pid==421)){const auto identity=std::make_tuple(before_pid,before_slot,before_generation);if(fork_user_progress.insert(identity).second){assertions<<"{\"event\":\"actual_first_User_instruction_after_fork\",\"pid\":"<<before_pid<<",\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"pc\":"<<before_pc<<"}\n";check(fork_returned.count(identity)==1,"User fork continuation follows causal return PID"+std::to_string(before_pid));}}
            if(op==isa::Opcode::SYSCALL){check(vm::ops::toLong(machine.vregfile.reg[1].lane[0])==7,"scheduled User vector7 PID"+std::to_string(before_pid));if(before_pid==202 && rec.syscall_id==compiler::runtime::sys_fork){fork_pc=rec.pc;fork_entry=true;}if(before_pid==421 && rec.syscall_id==compiler::runtime::sys_exit){child_exit=rec.syscall_arg0==45;}if(before_pid==202 && rec.syscall_id==compiler::runtime::sys_waitpid && rec.syscall_arg0==421 && wait_pc<0){wait_pc=rec.pc;wait_pending=true;}}
            if(op==isa::Opcode::VLOAD || op==isa::Opcode::VSTORE || op==isa::Opcode::VBCAST || op==isa::Opcode::VDIV || op==isa::Opcode::VMUL){
                Lanes result;for(int lane=0;lane<27;++lane)result[lane]=machine.vregfile.reg[rec.instruction.rd].lane[lane];
                const int phase=static_cast<int>(serviceUserWord(machine,10080,before_slot));
                const long long address=(op==isa::Opcode::VLOAD || op==isa::Opcode::VSTORE)?vm::ops::toLong(before_scalar[rec.instruction.rs1])+rec.instruction.imm:0;
                vectors<<"{\"step\":"<<step<<",\"pid\":"<<before_pid<<",\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"pc\":"<<before_pc<<",\"opcode\":"<<static_cast<int>(op)<<",\"func\":"<<static_cast<int>(rec.instruction.func)<<",\"phase\":"<<phase<<",\"virtual_address\":"<<address<<",\"trap_observed\":"<<(rec.trap_observed?"true":"false")<<",\"trap_cause\":"<<rec.trap_cause<<",\"result_exact\":";exact(vectors,result);
                if(op==isa::Opcode::VBCAST){vectors<<",\"scalar_input_exact\":";transportExactWord(vectors,before_scalar[rec.instruction.rs1]);}
                if(op==isa::Opcode::VDIV || op==isa::Opcode::VMUL){vectors<<",\"left_input_exact\":";exact(vectors,before_vectors[rec.instruction.rs1]);vectors<<",\"right_input_exact\":";exact(vectors,before_vectors[rec.instruction.rs2]);}
                if(op==isa::Opcode::VLOAD || op==isa::Opcode::VSTORE){vectors<<",\"memory_after_exact\":[";for(int lane=0;lane<27;++lane){if(lane)vectors<<',';transportExactWord(vectors,transportUserValue(machine,static_cast<int>(address)+lane,before_slot));}vectors<<']';}
                vectors<<"}\n";
                if(op==isa::Opcode::VMUL && before_pid==202 && phase==0){++special_multiplies;if(!special_transition && result[0].asLongTripleRaw().isSpecial()){special_transition=true;check(!before_vectors[rec.instruction.rs1][0].isInvalid() && !before_vectors[rec.instruction.rs1][0].asLongTripleRaw().isSpecial() && !rec.trap_observed,"real finite T50 VMUL reaches defined special sentinel");}}
                if((op==isa::Opcode::VLOAD || op==isa::Opcode::VSTORE) && address>=12000 && address<=12512 && (address-12000)%64==0){
                    const int row=static_cast<int>((address-12000)/64),vpn=static_cast<int>(address/729),end_vpn=static_cast<int>((address+26)/729);const auto context=serviceKernelWord(machine,390000+before_slot*9+5),pt=serviceKernelWord(machine,static_cast<int>(context)+4);
                    mappings<<"{\"step\":"<<step<<",\"pid\":"<<before_pid<<",\"slot\":"<<before_slot<<",\"process_generation\":"<<before_generation<<",\"phase\":"<<phase<<",\"row\":"<<row<<",\"vpn\":"<<vpn<<",\"pte\":"<<serviceKernelWord(machine,static_cast<int>(pt)+vpn)<<",\"ppn\":"<<serviceKernelWord(machine,static_cast<int>(pt)+vpn)/19683<<",\"end_vpn\":"<<end_vpn<<",\"end_ppn\":"<<serviceKernelWord(machine,static_cast<int>(pt)+end_vpn)/19683<<"}\n";
                    if(op==isa::Opcode::VLOAD && !rec.trap_observed){reads.insert({phase,row});bool typed=true;for(const auto& word:result)typed &= static_cast<int>(word.mode)==modes[row] && !word.isInvalid();check(typed,"actual requested numeric mode row"+std::to_string(row)+" phase"+std::to_string(phase));
                        if(before_pid==202 && phase==0){references[{0,row}]=result;if(row==5 || row==6){bool high=true;for(const auto& word:result)high &= word.bits.hi!=0 && !word.asLongTripleRaw().isSpecial();check(high,"finite T50 fraction has genuine nonzero high bits row"+std::to_string(row));}if(row==8)reference_complete=references.size()==9;}
                        if(before_pid==421 && phase==1 && references.count({0,row})){const bool same=equal(result,references[{0,row}]);check(same,"child actual typed load equals parent reference row"+std::to_string(row));if(!same){if(row<5)++narrow_mismatches;else ++wide_mismatches;}}
                        if((before_pid==421 && phase==2) || (before_pid==202 && phase==5)){references[{phase,row}]=result;check(!equal(result,references[{0,row}]),"actual mutation differs from original row"+std::to_string(row)+" phase"+std::to_string(phase));}
                        const int ref=phase==4?0:phase==3?2:phase==6 || phase==7?5:-1;if(ref>=0)check(references.count({ref,row}) && equal(result,references[{ref,row}]),"private typed row preserved after peer mutation/reap row"+std::to_string(row)+" phase"+std::to_string(phase));
                    }
                }
                if(rec.trap_observed){native_fault=true;check(false,"actual User vector memory/arithmetic fault prevented protocol continuation");}
            }
        }
        if(!fork_observed && fork_pc>=0 && rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET){const auto pc=serviceKernelWord(machine,390000+9+5),cc=serviceKernelWord(machine,390000+21*9+5);if(cc>0 && serviceKernelWord(machine,static_cast<int>(pc))==fork_pc+1 && serviceKernelWord(machine,static_cast<int>(pc)+19)==421){fork_observed=true;check(serviceKernelWord(machine,static_cast<int>(pc)+18)==1 && serviceKernelWord(machine,static_cast<int>(pc)+20)==0 && serviceKernelWord(machine,static_cast<int>(cc))==fork_pc+1 && serviceKernelWord(machine,static_cast<int>(cc)+18)==1 && serviceKernelWord(machine,static_cast<int>(cc)+19)==0 && serviceKernelWord(machine,static_cast<int>(cc)+20)==0,"real fork parent[1,421,0]/child[1,0,0] saved EPC+1");
            const auto ppt=serviceKernelWord(machine,static_cast<int>(pc)+4),cpt=serviceKernelWord(machine,static_cast<int>(cc)+4);for(int vpn=16;vpn<=17;++vpn){const auto ppn=serviceKernelWord(machine,static_cast<int>(ppt)+vpn)/19683,cpn=serviceKernelWord(machine,static_cast<int>(cpt)+vpn)/19683;check(ppn==1000+192+74+vpn-13 && cpn==1000+21*192+74+vpn-13 && ppn!=cpn,"distinct canonical private backing VPN"+std::to_string(vpn));}assertions<<"{\"event\":\"completed_native_fork_snapshot\",\"parent_generation\":"<<transportProcessGeneration(machine,1)<<",\"child_generation\":"<<transportProcessGeneration(machine,21)<<",\"saved_epc\":"<<fork_pc+1<<",\"parent_context\":"<<pc<<",\"child_context\":"<<cc<<"}\n";}}
        if(fork_pc>=0 && rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET && rec.after_privilege==vm::PrivilegeMode::User && machine.pc==fork_pc+1 && (pid==202 || pid==421)){const auto identity=std::make_tuple(pid,slot,transportProcessGeneration(machine,slot));if(fork_returned.insert(identity).second){++fork_returns[pid];const auto status=vm::ops::toLong(machine.regfile.read(13)),payload=vm::ops::toLong(machine.regfile.read(14)),detail=vm::ops::toLong(machine.regfile.read(15));assertions<<"{\"event\":\"actual_fork_User_ERET\",\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"process_generation\":"<<transportProcessGeneration(machine,slot)<<",\"return_epc\":"<<machine.pc<<",\"tuple\":["<<status<<','<<payload<<','<<detail<<"]}\n";check(status==1 && payload==(pid==202?421:0) && detail==0,"actual live fork raw tuple PID"+std::to_string(pid));}else assertions<<"{\"event\":\"later_same_pc_resume_after_causal_fork_return\",\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"process_generation\":"<<transportProcessGeneration(machine,slot)<<",\"return_epc\":"<<machine.pc<<"}\n";}
        if(pid==202 && wait_pending && rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET && machine.pc==wait_pc+1){wait_pending=false;++wait_returns;check(vm::ops::toLong(machine.regfile.read(13))==1 && vm::ops::toLong(machine.regfile.read(14))==45 && vm::ops::toLong(machine.regfile.read(15))==0,"real parent wait returns once[1,45,0]/EPC+1");}
        if(child_exit && serviceKernelWord(machine,390000+21*9+2)==0)reaped=true;
        for(int row_slot:std::vector<int>{1,2,21}){const int row_pid=static_cast<int>(serviceKernelWord(machine,390000+row_slot*9));if(ran.count(row_pid) && serviceKernelWord(machine,390000+row_slot*9+2)>0){const auto checked=serviceUserWord(machine,10042,row_slot);if(checked>0){auto& cache=counts[{row_pid,row_slot,transportProcessGeneration(machine,row_slot)}];cache[0]=std::max(cache[0],serviceUserWord(machine,10040,row_slot));const auto first=serviceUserWord(machine,10041,row_slot);if(cache[1]==0 && first>0)cache[1]=first;cache[2]=std::max(cache[2],checked);if(cache[0]>0 && cache[1]>0){guest_failed=true;check(false,"genuine guest failure "+std::to_string(cache[1])+" PID"+std::to_string(row_pid));}}}}
        if(fork_entry && rec.attempted && ((rec.has_instruction && rec.instruction.opcode==isa::Opcode::HALT) || machine.status==vm::VMStatus::TRAPPED)){native_fault=true;check(false,"actual native fork/protocol execution stopped");}
        parent_done=serviceUserWord(machine,10090,1)==777;super_done=serviceUserWord(machine,10090,2)==777;
        // Host preservation mismatches deliberately do not stop later guest
        // reads/mutations/isolation. A genuine guest failure or trap may stop.
        return native_fault || guest_failed || (parent_done && super_done);
    }," mov r1, 7\n vbcast.t5 v1, r1\n",false,[&](const vm::VMState& machine){if(machine.privilege==vm::PrivilegeMode::User){before_pid=static_cast<int>(serviceKernelWord(machine,3020));before_slot=transportRuntimeSlot(machine,before_pid);before_generation=transportProcessGeneration(machine,before_slot);before_pc=machine.pc;for(int reg=0;reg<32;++reg)before_scalar[reg]=machine.regfile.readPhysical(reg);for(int reg=0;reg<8;++reg)for(int lane=0;lane<27;++lane)before_vectors[reg][lane]=machine.vregfile.reg[reg].lane[lane];}else if(fork_entry && !copy_finished && machine.privilege==vm::PrivilegeMode::Kernel){const auto fetched=machine.imem.fetch(machine.pc);if(fetched.second==vm::MemFaultCode::OK){const auto instruction=isa::VersionedInstructionCodec::decode(fetched.first,isa::IsaEncodingVersion::V2);if(instruction.opcode==isa::Opcode::TSCAN || instruction.opcode==isa::Opcode::VGATHER || instruction.opcode==isa::Opcode::VSCATTER)for(int reg=0;reg<32;++reg)before_scalar[reg]=machine.regfile.readPhysical(reg);}}},false,true);
    check(copy_seen && copy_finished && scan_words>0 && wide_words>=108 && narrow_words>0,"actual bounded numeric wide correction classification and completion observed");
    check(reference_complete && references.count({0,5}) && references.count({0,6}) && !equal(references[{0,5}],references[{0,6}]),"all parent typed references and distinct finite T50 fractions observed");
    check(fork_observed && child_ran && fork_returns[202]==1 && fork_returns[421]==1 && fork_user_progress.size()==2,"real native parent/child causal fork return once and actual User continuation");
    bool all_reads=true;for(int phase:std::vector<int>{0,1,2,3,4,5,6,7})for(int row=0;row<9;++row)all_reads &= reads.count({phase,row})>0;check(all_reads,"all actual typed reads for both mutation generations and reap");
    check(child_exit && reaped && wait_returns==1 && parent_done && super_done,"real IPC/survivor/child orderly exit/oncewait/reap completed");
    assertions<<"{\"event\":\"optional_T50_special_availability\",\"observed\":"<<(special_transition?"true":"false")<<",\"actual_multiplies\":"<<special_multiplies<<"}\n";
    std::ostringstream guests;guests<<"{\"lifetimes\":[";bool comma=false;for(const auto& [identity,count]:counts){if(comma)guests<<',';comma=true;const auto [pid,slot,generation]=identity;guests<<"{\"pid\":"<<pid<<",\"slot\":"<<slot<<",\"generation\":"<<generation<<",\"checks\":"<<count[2]<<",\"failures\":"<<count[0]<<",\"first_failure\":"<<count[1]<<"}";}guests<<"]}\n";
    const bool saved=transportWriteArtifact(base/"guest_assertions.json",guests.str());assertions.flush();vectors.flush();mappings.flush();wide_copy.flush();
    const bool freeze=hashes("source_hashes_final.json") && readTextFile(base/"source_hashes.json")==readTextFile(base/"source_hashes_final.json");
    const bool passed=completed && !behavior_failed && saved && freeze && assertions.good() && vectors.good() && mappings.good() && wide_copy.good();
    std::ostringstream report;report<<"{\"status\":\""<<(passed?"PASS":reference_complete && fork_entry && behavior_failed?"RED":"INCOMPLETE")<<"\",\"scope\":\"native numeric private memory fork\",\"run_directory\":\"run_"<<n<<"\",\"narrow_typed_mismatches\":"<<narrow_mismatches<<",\"wide_typed_mismatches\":"<<wide_mismatches<<",\"later_protocol_completed\":"<<(parent_done && super_done && reaped?"true":"false")<<",\"optional_T50_special\":\""<<(special_transition?"observed":"unavailable")<<"\",\"L50\":\"unvalidated\",\"fixture_mutates_production_source\":false,\"malformed_lifetime_validation\":\"pending\",\"full_fork_acceptance\":false,\"transport_acceptance\":false}\n";
    const bool reported=transportWriteArtifact(base/"report.json",report.str()) && transportWriteArtifact(root/"report.json",report.str());std::cout<<"native private memory artifact: "<<base.string()<<'\n';return require(passed && reported,"native private numeric memory fork");
}


bool runCompositorForkMalformedV3() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_malformed_v3";
    std::filesystem::create_directories(root);int number=1;while(std::filesystem::exists(root/("run_"+std::to_string(number))))++number;
    const auto base=root/("run_"+std::to_string(number));std::filesystem::create_directories(base);
    const std::string hash_program=R"PY(import hashlib,json,pathlib,sys
repo=pathlib.Path(sys.argv[1]);out=pathlib.Path(sys.argv[2])
paths=['kernel.trit','native_kernel_trap_stub.tasm','ternary_compiler_ir.h','ternary_compiler_codegen.h','apps/os_sdk.trit','SYSCALL_MANIFEST.json','tests/current_only_conformance.cpp','tools/trit_tool.py','TEST_MANIFEST.json','docs/07_OS_Substrate/native_compositor_contract.md','ternary_vm.h','ternary_vm_state.h','ternary_native_ops.h','ternary_scalar.h']
out.write_text(json.dumps({'sources':{p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths}},indent=2)+'\n')
)PY";
    if(!transportWriteArtifact(base/"source_hash_program.txt",hash_program))return false;
    auto hashes=[&](const std::string& name){const auto command="python - \""+std::filesystem::current_path().string()+"\" \""+(base/name).string()+"\" < \""+(base/"source_hash_program.txt").string()+"\"";return std::system(command.c_str())==0 && !readTextFile(base/name).empty();};
    if(!hashes("source_hashes.json"))return false;
    if(!transportWriteArtifact(base/"failure_modes.json",readTextFile(root/"preproduction_review.json")))return false;
    const std::vector<std::string> names={"control","pending_source_generation","pending_destination_generation","source_ready_generation","source_default_V3_pointer","destination_default_scalar_pointer","invalid_pending_source","self_pending_source","destination_BLOCKED","source_pending_dependency","finish_mailbox_generation","finish_mailbox_V3_pointer","selected_ready_generation","private_child_PTE_backing","private_child_PTE_flags","fractional_global_issuer"};
    const auto common=transportGuestCommon()+R"TRIT(
fn seed(phase: t40) -> t40 { return phase; }
fn read_rows(phase: t40) -> t40 { return phase; }
fn vector_seed() -> t40 { return 1; }
fn guards() -> t40 {unsafe {tr_check(load(11999),701,4801);tr_check(load(12027),-701,4802);tr_check(load(12063),702,4803);tr_check(load(12091),-702,4804);}return 1;}
)TRIT";
    const auto parent=common+R"TRIT(
fn main() -> t40 {
 vector_seed();unsafe {store(11000,73);store(11001,1/3);store(11002,-91);store(11999,701);store(12027,-701);store(12063,702);store(12091,-702);store(10080,0);}
 tr_check(7*17,119,4805);seed(0);read_rows(0);guards();tr_send(10,73);tr_check(tr_recv(11),118,4806);
 unsafe {tr_check(load(11001),1/3,4807);store(10080,1);}tr_check(sys_getpid(),202,4808);
 unsafe {store(10080,2);}let child: t40=sys_fork(21,421);
 if child==0 {
  unsafe {store(10040,0);store(10041,0);store(10042,0);store(10080,3);store(10090,0);}
  tr_check(sys_getpid(),421,4810);read_rows(1);guards();unsafe {tr_check(load(11000),73,4811);tr_check(load(11001),1/3,4812);}
  seed(1);unsafe {store(11000,99);store(10080,4);}read_rows(2);tr_send(13,101);tr_check(tr_recv(14),202,4813);
  unsafe {store(10080,5);}read_rows(5);guards();unsafe {tr_check(load(11000),99,4814);}
  tr_send(15,303);sys_exit(45);return 0;
 }
 tr_check(child,421,4820);tr_check(tr_recv(13),101,4821);unsafe {store(10080,6);}read_rows(3);guards();unsafe {tr_check(load(11000),73,4822);tr_check(load(11001),1/3,4823);}
 seed(2);unsafe {store(10080,7);}read_rows(4);tr_send(14,202);tr_check(tr_recv(15),303,4824);
 let status: t40=sys_waitpid(421,10500,0);guards();unsafe {tr_check(load(11000),73,4825);tr_check(load(11002),-91,4826);}tr_check(status,45,4827);sys_waitpid(421,10500,0);
 unsafe {store(10080,8);}read_rows(6);tr_send(12,303);unsafe {store(10090,777);store(10600,1);}sys_futex_wait(10600,1,-1);return 777;
}
)TRIT";
    const auto supervisor=common+R"TRIT(
fn main() -> t40 {
 vector_seed();unsafe {store(11000,19);store(11001,2/7);store(11002,-61);}
 var sum: t40=0;var i: t40=1;while i<10 {sum=sum+i;i=i+1;}tr_check(sum,45,4830);tr_check(sys_getpid(),201,4831);unsafe {tr_check(load(11001),2/7,4837);tr_check(load(11002),-61,4838);}
 let input: t40=tr_recv(10);tr_check(input,73,4832);sum=sum+input;tr_send(11,sum);tr_check(tr_recv(12),303,4833);
 unsafe {tr_check(load(11000),19,4834);tr_check(load(11001),2/7,4835);tr_check(load(11002),-61,4836);store(10090,777);store(10600,1);}sys_futex_wait(10600,1,-1);return 777;
}
)TRIT";
    std::string seed="    brz r19, malformed_seed_original\n    mov r23, 1\n    sub.t40 r23, r19, r23\n    brz r23, malformed_seed_child\n    jmp malformed_seed_parent\n";
    for(int phase=0;phase<3;++phase){seed+=phase==0?"malformed_seed_original:\n":phase==1?"malformed_seed_child:\n":"malformed_seed_parent:\n";for(int row=0;row<2;++row){const int numerator=phase==0?(row? -1:37):phase==1?(row? -2:41):(row? -5:17),denominator=phase==0?(row?7:3):phase==1?(row?11:5):(row?13:7);seed+=" mov r20, "+std::to_string(numerator)+"\n vbcast.t50 v0, r20\n mov r20, "+std::to_string(denominator)+"\n vbcast.t50 v2, r20\n vdiv.t50 v0, v0, v2\n mov r19, "+std::to_string(12000+64*row)+"\n vstore.t50 v0, r19, 0\n";}seed+=" jmp malformed_seed_done\n";}seed+="malformed_seed_done:\n mov r19, 1\n";
    TransportActor actor{"malformed-parent",parent,1,202,0,0};actor.memory_native_extensions={{"vector_seed_entry"," mov r20, 7\n vbcast.t5 v1, r20\n"},{"seed_entry",seed},{"read_rows_entry"," mov r19, 12000\n vload.t50 v0, r19, 0\n mov r19, 12064\n vload.t50 v0, r19, 0\n mov r19, 1\n"}};
    TransportActor super{"malformed-supervisor",supervisor,2,201,0,0};super.memory_native_extensions={{"vector_seed_entry"," mov r20, 7\n vbcast.t5 v1, r20\n"}};
    bool all=true;std::ostringstream reports;reports<<"[";bool comma=false;
    for(int variant=0;variant<static_cast<int>(names.size());++variant){const auto dir=base/names[variant];std::filesystem::create_directories(dir);
        std::ofstream trace(dir/"validation_trace.jsonl"),assertions(dir/"assertions.jsonl");bool clone_loaded=false,clone_stored=false,parent_entry_saved=false,actual_wait_blocked=false;std::set<int> guest_vector_seed;int cause_before=0;bool failed=false,injected=false,halt=false,helper_return=false,fault_branch=false,ordinary=false,fork_seen=false,parent_done=false,super_done=false,child_exit=false,reaped=false;int target_pc=-1,return_pc=-1,cell=-1,fork_pc=-1,ordinary_pc=-1,wait_pc=-1,wait_returns=0,user_after=0,eret_after=0,new_publication=0,payload_after=0;long long scans=0,scatters=0;std::map<std::string,int> labels;std::map<int,vm::TernaryValue> protected_words;std::map<int,int> guest_checks;std::set<int> ran;std::set<std::string> reached;std::map<std::pair<int,int>,std::array<vm::TernaryValue,27>> typed;std::array<vm::TernaryValue,32> before_regs{};int before_pid=0,before_slot=-1,before_pc=-1;std::set<int> causal_returns,continuations;vm::TernaryValue injected_value;
        auto equal_word=[](const vm::TernaryValue&a,const vm::TernaryValue&b){return a.mode==b.mode && a.bits.lo==b.bits.lo && a.bits.hi==b.bits.hi;};
        auto check=[&](bool ok,const std::string& text){assertions<<"{\"pass\":"<<(ok?"true":"false")<<",\"assertion\":\""<<jsonEscape(text)<<"\"}\n";failed |= !ok;};
        auto snapshot=[&](const vm::VMState&m){auto add=[&](int addr){protected_words[addr]=m.dmem.load(addr).first;};add(14725800);for(int word=0;word<8;++word)add(14725808+word);for(int slot:std::vector<int>{1,2,21}){const bool exists=serviceKernelWord(m,390000+slot*9+2)>0;if(!exists)continue;for(int word=0;word<9;++word)add(390000+slot*9+word);for(int offset:std::vector<int>{64,164,264,364,464})add(14725800+offset+slot);const int ctx=669000+slot*32,vctx=700002+slot*279;for(int word=0;word<32;++word)add(ctx+word);for(int word=0;word<279;++word)add(vctx+word);const int pt=static_cast<int>(serviceKernelWord(m,ctx+4));for(int vpn=0;vpn<512;++vpn)add(pt+vpn);for(int vpnAddr:std::vector<int>{11000,11001,11002,10040,10041,10042,10080,10090,11999,12027,12063,12091}){const int vpn=vpnAddr/729;const auto pte=serviceKernelWord(m,pt+vpn);if(pte>0)add(static_cast<int>(pte/19683)*729+vpnAddr%729);}for(int baseAddr:std::vector<int>{12000,12064})for(int lane=0;lane<27;++lane){const int addr=baseAddr+lane;const auto pte=serviceKernelWord(m,pt+addr/729);if(pte>0)add(static_cast<int>(pte/19683)*729+addr%729);}}};
        auto save_owned=[&](const vm::VMState&m,const std::string&name){std::ostringstream out;out<<"{\"words\":[";bool comma=false;for(const auto&[addr,value]:protected_words){if(comma)out<<',';comma=true;out<<"{\"physical_address\":"<<addr<<",\"exact\":";transportExactWord(out,m.dmem.load(addr).first);out<<"}";}out<<"]}\n";check(transportWriteArtifact(dir/name,out.str()),"owned state snapshot saved "+name);};
        const auto helper=variant==12?"native_v3_selected":variant>=9 && variant<=11?(variant==9?"native_fork_private_page":"native_v3_finish_clone"):variant>=13?"native_fork_private_page":"native_v3_next_clone";
        int last_call_target=-1,last_call_lr=-1;long long source_generation=0,destination_generation=0;
        const bool completed=runTransportActors(dir,{actor,super},"",[&](vm::VMState&m,const vm::VMExecutionRecord&r,long long step){
            const int pid=static_cast<int>(serviceKernelWord(m,3020));
            if(r.attempted && r.has_instruction){if(r.before_privilege==vm::PrivilegeMode::User){ran.insert(before_pid);if(injected)++user_after;
                if(r.instruction.opcode==isa::Opcode::VBCAST && r.instruction.rd==1){guest_vector_seed.insert(before_pid);check(vm::ops::toLong(m.vregfile.reg[1].lane[0])==7,"real User VBCAST vector7 PID"+std::to_string(before_pid));trace<<"{\"event\":\"actual_User_vector7_seed\",\"attempted\":true,\"has_instruction\":true,\"pid\":"<<before_pid<<",\"pc\":"<<r.pc<<",\"raw_bits\":"<<r.raw_bits<<"}\n";}
                if(r.instruction.opcode==isa::Opcode::SYSCALL){check(vm::ops::toLong(m.vregfile.reg[1].lane[0])==7,"actual scheduled vector7 PID"+std::to_string(before_pid));if(before_pid==202 && r.syscall_id==7 && serviceUserWord(m,10080,1)==1){ordinary=true;ordinary_pc=r.pc;}if(before_pid==202 && r.syscall_id==20){fork_seen=true;fork_pc=r.pc;}if(before_pid==421 && r.syscall_id==compiler::runtime::sys_exit)child_exit=r.syscall_arg0==45;if(before_pid==202 && r.syscall_id==compiler::runtime::sys_waitpid && r.syscall_arg0==421 && wait_pc<0)wait_pc=r.pc;}
                if(fork_pc>=0 && r.pc==fork_pc+1 && (before_pid==202 || before_pid==421) && continuations.insert(before_pid).second)trace<<"{\"event\":\"actual_first_User_fork_continuation\",\"attempted\":true,\"has_instruction\":true,\"pid\":"<<before_pid<<",\"pc\":"<<r.pc<<",\"raw_bits\":"<<r.raw_bits<<"}\n";
                if(r.instruction.opcode==isa::Opcode::VLOAD){const auto addr=vm::ops::toLong(before_regs[r.instruction.rs1])+r.instruction.imm;if(addr==12000 || addr==12064){const int row=addr==12000?0:1,phase=before_pid==421?static_cast<int>(serviceUserWord(m,10080,21)):static_cast<int>(serviceUserWord(m,10080,1));std::array<vm::TernaryValue,27> lanes;for(int lane=0;lane<27;++lane)lanes[lane]=m.vregfile.reg[0].lane[lane];const int key=before_pid==202 && phase==0?0:before_pid==421 && phase==3?1:before_pid==421 && phase==4?2:before_pid==202 && phase==6?3:before_pid==202 && phase==7?4:before_pid==421 && phase==5?5:6;
                    trace<<"{\"event\":\"actual_User_typed_load\",\"step\":"<<step<<",\"pid\":"<<before_pid<<",\"slot\":"<<before_slot<<",\"process_generation\":"<<transportProcessGeneration(m,before_slot)<<",\"pc\":"<<r.pc<<",\"row\":"<<row<<",\"phase\":"<<key<<",\"words\":[";for(int lane=0;lane<27;++lane){if(lane)trace<<',';transportExactWord(trace,lanes[lane]);}trace<<"]}\n";
                    if(key==0){bool finite=true;for(const auto&w:lanes)finite &= static_cast<int>(w.mode)==50 && w.bits.hi!=0 && !w.isInvalid() && !w.asLongTripleRaw().isSpecial();check(finite,"actual finite nonzero-high T50 parent reference row"+std::to_string(row));}
                    if(key==1 || key==3 || key==5 || key==6){const int ref=key==1 || key==3?0:key==5?2:4;bool same=typed.count({ref,row})>0;if(same)for(int lane=0;lane<27;++lane)same &= equal_word(typed[{ref,row}][lane],lanes[lane]);check(same,"actual typed private fork/isolation phase"+std::to_string(key)+" row"+std::to_string(row));}typed[{key,row}]=lanes;
                }}
            }
            if(r.before_privilege==vm::PrivilegeMode::Kernel && (r.instruction.opcode==isa::Opcode::VCTXLOAD || r.instruction.opcode==isa::Opcode::VCTXSTORE)){const auto address=vm::ops::toLong(m.regfile.read(r.instruction.rs1));if(fork_seen && r.instruction.opcode==isa::Opcode::VCTXLOAD && address==700281)clone_loaded=true;if(fork_seen && r.instruction.opcode==isa::Opcode::VCTXSTORE && address==700002+21*279)clone_stored=true;if(ordinary && r.instruction.opcode==isa::Opcode::VCTXSTORE && address==700281)parent_entry_saved=true;trace<<"{\"event\":\"actual_private_VCTX_boundary\",\"attempted\":true,\"original_has_instruction\":false,\"provenance\":\"validated actual physicalPC/raw privateVCTX annotation; records.jsonl\",\"pc\":"<<r.pc<<",\"physical_pc\":"<<r.physical_pc<<",\"raw_bits\":"<<r.raw_bits<<",\"opcode\":"<<static_cast<int>(r.instruction.opcode)<<",\"context_address\":"<<address<<"}\n";}
            if(r.before_privilege==vm::PrivilegeMode::Kernel && r.instruction.opcode==isa::Opcode::CALL){for(const auto&name:std::vector<std::string>{"native_v3_next_clone","native_fork_private_page","native_v3_finish_clone","native_v3_selected"})if(labels.count(name) && m.pc==labels[name]){reached.insert(name);last_call_target=m.pc;last_call_lr=static_cast<int>(vm::ops::toLong(m.regfile.readLR()));trace<<"{\"event\":\"actual_helper_CALL\",\"step\":"<<step<<",\"helper\":\""<<name<<"\",\"pc\":"<<r.pc<<",\"target\":"<<m.pc<<",\"actual_LR25\":"<<last_call_lr<<",\"raw_bits\":"<<r.raw_bits<<"}\n";}}
            if(r.before_privilege==vm::PrivilegeMode::Kernel && r.instruction.opcode==isa::Opcode::TSCAN)++scans;if(r.before_privilege==vm::PrivilegeMode::Kernel && r.instruction.opcode==isa::Opcode::VSCATTER){++scatters;if(injected)++payload_after;}
            if(r.instruction.opcode==isa::Opcode::ERET && r.after_privilege==vm::PrivilegeMode::User){if(injected)++eret_after;if(fork_pc>=0 && m.pc==fork_pc+1 && (pid==202 || pid==421) && causal_returns.insert(pid).second)check(vm::ops::toLong(m.regfile.read(13))==1 && vm::ops::toLong(m.regfile.read(14))==(pid==202?421:0) && vm::ops::toLong(m.regfile.read(15))==0,"actual causal live fork tuple PID"+std::to_string(pid));if(wait_pc>=0 && m.pc==wait_pc+1 && wait_returns==0){++wait_returns;check(vm::ops::toLong(m.regfile.read(13))==1 && vm::ops::toLong(m.regfile.read(14))==45 && vm::ops::toLong(m.regfile.read(15))==0,"actual pending wait returns[1,45,0]/EPC+1");}}
            if(injected){if(serviceKernelWord(m,14725800+164+21)>0 || serviceKernelWord(m,390000+21*9+2)==1)++new_publication;
                trace<<"{\"event\":\"actual_after_injection_instruction\",\"attempted\":true,\"has_instruction\":true,\"step\":"<<step<<",\"pc\":"<<r.pc<<",\"physical_pc\":"<<r.physical_pc<<",\"raw_bits\":"<<r.raw_bits<<",\"opcode\":"<<static_cast<int>(r.instruction.opcode)<<",\"privilege\":"<<static_cast<int>(r.before_privilege)<<",\"final_pc\":"<<r.final_pc<<",\"cause\":"<<m.cause<<",\"r13\":"<<vm::ops::toLong(m.regfile.read(13))<<"}\n";
                if(r.instruction.opcode==isa::Opcode::RET && m.pc==return_pc){helper_return=true;check(vm::ops::toLong(m.regfile.read(13))==-8,"actual target helper RET to capturedLR has -8");}
                if(r.instruction.opcode==isa::Opcode::BRN && m.pc==labels["native_vector_context_fault"]){fault_branch=true;check(helper_return,"actual fault BRN follows target helper rejection");}
                if(r.instruction.opcode==isa::Opcode::HALT){halt=true;save_owned(m,"owned_after.json");check(m.cause==cause_before,"actual pre-injection syscall cause preserved through KernelHALT");bool preserved=true;for(const auto&[addr,value]:protected_words)if(addr!=cell)preserved &= equal_word(value,m.dmem.load(addr).first);check(preserved,"protected default owned contexts guarded memory PTE identities and mailbox preserved except injected cell");check(r.pc==labels["native_vector_context_fault"] && r.before_privilege==vm::PrivilegeMode::Kernel && m.status==vm::VMStatus::HALTED,"actual native fault label KernelHALT");return true;}}
            }
            for(int slot:std::vector<int>{1,2,21})if(serviceKernelWord(m,390000+slot*9+2)>0 && ran.count(static_cast<int>(serviceKernelWord(m,390000+slot*9)))){const auto count=serviceUserWord(m,10042,slot);guest_checks[slot]=std::max(guest_checks[slot],static_cast<int>(count));if(count>0 && serviceUserWord(m,10040,slot)>0){check(false,"genuine guest assertion failure");return true;}}
            if(wait_pc>=0 && !actual_wait_blocked && serviceKernelWord(m,390000+9+2)==3 && serviceKernelWord(m,391700+1)==100421){actual_wait_blocked=true;trace<<"{\"event\":\"actual_pending_parent_wait421\",\"step\":"<<step<<",\"state\":3,\"channel\":100421}\n";}
            if(child_exit && serviceKernelWord(m,390000+21*9+2)==0)reaped=true;parent_done=serviceUserWord(m,10090,1)==777;super_done=serviceUserWord(m,10090,2)==777;return variant==0 && parent_done && super_done;
        }," mov r1, 7\n vbcast.t5 v1, r1\n",false,[&](const vm::VMState&m){if(m.privilege==vm::PrivilegeMode::User){before_pid=static_cast<int>(serviceKernelWord(m,3020));before_slot=transportRuntimeSlot(m,before_pid);before_pc=m.pc;for(int reg=0;reg<32;++reg)before_regs[reg]=m.regfile.readPhysical(reg);}},true,true,
        [&](vm::VMState&m,const std::map<std::string,int>& final_labels){if(labels.empty())labels=final_labels;if(variant==0 || injected || m.privilege!=vm::PrivilegeMode::Kernel || !labels.count(helper) || m.pc!=labels[helper])return;const bool selected=variant==12;if(selected){if(!ordinary || serviceKernelWord(m,3020)!=202 || serviceKernelWord(m,669032)!=ordinary_pc+1)return;}else{if(!fork_seen || serviceKernelWord(m,390000+21*9)!=421 || serviceKernelWord(m,14725800+264+21)!=2 || serviceKernelWord(m,390000+21*9+2)!=6)return;if(std::string(helper)=="native_fork_private_page" && vm::ops::toLong(m.regfile.read(13))!=16)return;}
            check(last_call_target==m.pc && last_call_lr==vm::ops::toLong(m.regfile.readLR()),"injection follows actual target CALL/capturedLR25");check(guest_checks[1]>0 && guest_checks[2]>0 && guest_vector_seed.count(202) && guest_vector_seed.count(201) && typed.count({0,0}) && typed.count({0,1}),"genuine compiled IPC/arithmetic/guards/finiteT50 prefix before metadata injection");if(std::string(helper)=="native_v3_finish_clone")check(clone_loaded && clone_stored && scans>0 && scatters==54,"finish injection follows actual completed bounded numeric copy");if(selected)check(parent_entry_saved,"selected injection follows genuine entry VCTXSTORE after ordinary parent syscall");if(std::string(helper)=="native_fork_private_page")check(clone_loaded && clone_stored,"private-page injection follows actual completed V3 clone");bool distinct=false;if(typed.count({0,0}) && typed.count({0,1}))for(int lane=0;lane<27;++lane)distinct |= !equal_word(typed[{0,0}][lane],typed[{0,1}][lane]);check(distinct,"two real finite T50 parent references are distinct");snapshot(m);save_owned(m,"owned_before.json");cause_before=m.cause;source_generation=transportProcessGeneration(m,1);destination_generation=transportProcessGeneration(m,21);return_pc=static_cast<int>(vm::ops::toLong(m.regfile.readLR()));target_pc=m.pc;
            switch(variant){case 1:cell=14725800+364+21;injected_value=vm::ops::fromLong(source_generation-1);break;case 2:cell=14725800+464+21;injected_value=vm::ops::fromLong(destination_generation-1);break;case 3:cell=14725800+164+1;injected_value=vm::ops::fromLong(source_generation-1);break;case 4:cell=390000+9+8;injected_value=vm::ops::fromLong(700002+2*279);break;case 5:cell=390000+21*9+5;injected_value=vm::ops::fromLong(669000+2*32);break;case 6:cell=14725800+264+21;injected_value=vm::ops::fromLong(101);break;case 7:cell=14725800+264+21;injected_value=vm::ops::fromLong(22);break;case 8:cell=390000+21*9+2;injected_value=vm::ops::fromLong(3);break;case 9:cell=14725800+264+1;injected_value=vm::ops::fromLong(3);break;case 10:cell=14725808+4;injected_value=vm::ops::fromLong(destination_generation-1);break;case 11:cell=14725808;injected_value=vm::ops::fromLong(700002+2*279);break;case 12:cell=14725800+164+1;injected_value=vm::ops::fromLong(source_generation-1);break;case 13:case 14:cell=static_cast<int>(serviceKernelWord(m,669000+21*32+4))+16;injected_value=vm::exec::addValue(m.dmem.load(cell).first,vm::ops::fromLong(variant==13?19683:3),TernaryMode::T40);break;case 15:cell=14725800;injected_value=vm::exec::addValue(m.dmem.load(cell).first,vm::exec::divideValue(vm::ops::fromLong(1),vm::ops::fromLong(3),TernaryMode::T40),TernaryMode::T40);break;}
            trace<<"{\"event\":\"privileged_single_metadata_cell_injection\",\"helper\":\""<<helper<<"\",\"kernel_pc\":"<<m.pc<<",\"selected_pid\":"<<serviceKernelWord(m,3020)<<",\"selected_context\":"<<serviceKernelWord(m,3019)<<",\"source_generation\":"<<source_generation<<",\"destination_generation\":"<<destination_generation<<",\"cell\":"<<cell<<",\"before\":";transportExactWord(trace,m.dmem.load(cell).first);trace<<",\"after\":";transportExactWord(trace,injected_value);trace<<",\"prior_scans\":"<<scans<<",\"prior_scatters\":"<<scatters<<",\"returns_faults_status_injected\":false}\n";check(m.dmem.store(cell,injected_value)==vm::MemFaultCode::OK,"single metadata cell store succeeded");injected=true;
        },[&](const vm::VMState&m,const vm::VMExecutionRecord&r){if(injected || (wait_pc>=0 && !actual_wait_blocked))return true;if(r.attempted && r.has_instruction && r.before_privilege==vm::PrivilegeMode::Kernel && r.instruction.opcode==isa::Opcode::CALL)for(const auto&name:std::vector<std::string>{"native_v3_next_clone","native_fork_private_page","native_v3_finish_clone","native_v3_selected"})if(labels.count(name) && m.pc==labels[name])return true;return false;});
        if(variant==0){check(completed && fork_seen && child_exit && reaped && wait_returns==1 && actual_wait_blocked && causal_returns.size()==2 && continuations.size()==2 && parent_done && super_done,"no-injection compiled control fork/IPC/isolation/exit/wait/reap actual progress");check(guest_vector_seed.size()==2 && clone_loaded && clone_stored && reached.size()==4 && typed.size()==14,"control reaches all helpers and all typed mutation/isolation observations");}
        else {check(completed && injected && helper_return && fault_branch && halt,"actual malformed helper return/branch/KernelHALT sequence");check(user_after==0 && eret_after==0 && new_publication==0 && payload_after==0,"no post-injection User return/instruction/publication/private scatter");
        }
        trace.flush();assertions.flush();const bool pass=completed && !failed && trace.good() && assertions.good();std::ostringstream report;report<<"{\"case\":\""<<names[variant]<<"\",\"status\":\""<<(pass?"PASS":injected?"RED":"INCOMPLETE")<<"\",\"injected\":"<<(injected?"true":"false")<<",\"helper_return\":"<<(helper_return?"true":"false")<<",\"fault_branch\":"<<(fault_branch?"true":"false")<<",\"kernel_halt\":"<<(halt?"true":"false")<<",\"guest_checks_parent\":"<<guest_checks[1]<<",\"guest_checks_supervisor\":"<<guest_checks[2]<<",\"full_fork_acceptance\":false,\"transport_acceptance\":false}\n";const bool saved=transportWriteArtifact(dir/"report.json",report.str()) && transportWriteArtifact(dir/"criteria_map.json",std::string("{\"runtime_completed\":")+(completed?"true":"false")+",\"assertions\":\"assertions.jsonl\",\"actual_records\":\"validation_trace.jsonl\",\"protected_snapshots\":\"owned_before.json/owned_after.json for injected variants\",\"full_fork_acceptance\":false,\"transport_acceptance\":false}\n");if(comma)reports<<',';comma=true;reports<<report.str();all &= pass && saved;if(!all)break;
    }
    reports<<"]";const bool freeze=hashes("source_hashes_final.json") && readTextFile(base/"source_hashes.json")==readTextFile(base/"source_hashes_final.json");std::ostringstream report;report<<"{\"status\":\""<<(all && freeze?"PASS":"FAIL")<<"\",\"run_directory\":\"run_"<<number<<"\",\"scope\":\"privileged native validator metadata fault injection\",\"cases\":"<<reports.str()<<",\"tampering_after_validation\":\"unvalidated\",\"L50\":\"unvalidated\",\"full_fork_acceptance\":false,\"transport_acceptance\":false}\n";const bool saved=transportWriteArtifact(base/"report.json",report.str()) && transportWriteArtifact(root/"report.json",report.str());return require(all && freeze && saved,"native malformed V3/fork metadata fixture");
}

bool runCompositorForkPrerequisiteVariants() {
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_prerequisite";
    const bool ordinary=runCompositorForkPrerequisite(1);const auto ordinary_report=readTextFile(root/"report.json");
    const bool high=runCompositorForkPrerequisite(51);const auto high_report=readTextFile(root/"report.json");
    const bool fatal_user=runCompositorForkFatalUser();const auto fatal_report=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_fatal_user/report.json");
    const bool memory=runCompositorForkMemory();const auto memory_report=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_memory/report.json");
    const bool malformed=runCompositorForkMalformedV3();const auto malformed_report=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_malformed_v3/report.json");
    const bool window_lifetime=runCompositorForkWindowLifetimeProbe();const auto window_report=readTextFile(std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport/fork_window_lifetime/report.json");
    std::ostringstream report;report<<"{\"status\":\""<<(ordinary && high && window_lifetime && fatal_user && memory && malformed?"PASS":"FAIL")<<"\",\"scope\":\"native-fork-v3-context-slice\",\"variants\":["<<ordinary_report<<','<<high_report<<','<<window_report<<','<<fatal_report<<','<<memory_report<<','<<malformed_report<<"],\"full_fork_acceptance\":false,\"private_typed_wide_validation\":\"numericT1/T5/T10/T20/T40/T50observed;L50unvalidated\",\"transport_acceptance\":false}\n";
    return transportWriteArtifact(root/"report.json",report.str()) && ordinary && high && window_lifetime && fatal_user && memory && malformed;
}

// Stage2 transport proof starts with compiled real callers; missing mappings

// Stage2 transport proof starts with compiled real callers; missing mappings
// and old-kernel returns remain preserved in append-only run directories.
bool runCompositorTransport() {
    using namespace sandbox;
    const auto root=std::filesystem::path(TRIT_TEST_BUILD_DIR)/"compositor_workflow_artifact/stage2_transport";
    std::filesystem::create_directories(root);
    int n=1;while(std::filesystem::exists(root/("run_"+std::to_string(n))))++n;
    const auto base=root/("run_"+std::to_string(n));std::filesystem::create_directories(base);
    const std::vector<std::pair<std::string,std::string>> scenarios={
        {"authority_pins_lifetimes","Authentic fixture role; three clients; broad63 and othernamespace denial; write/truncate/unlink/rename/preopened descriptor attacks; failed/successful exec/fork/reap reuse."},
        {"atomic_records_guards","Exact8/16/10 records, fractional/short/long/negative counts and spans, boundary-crossing guards, strict ABI reply tuples and signed opaque arguments."},
        {"bounded_controls_conservation","Queue2, requestpool8, join/closure8, outstanding1; fullqueue-11/pool-3; retained cancellations and closures drain once with original tokens."},
        {"blocking_receive_reply","Real blocked receive and submit; ready wake, timeout, invalid/duplicate/stale replies; output revoke wakes BAD_PTR."},
        {"cancel_stop_term","Explicit resume, STOP/CONT, TERM; causal caller completion before peer teardown, one EPC advance and no replay."},
        {"client_death_exec","Ordinary and forced client death plus exec release; controls survive queuefull; slot reuse has fresh identity and no join/pending/role."},
        {"compositor_death_restart","Ordinary/forced owner death wakes at least3 callers canceled; no auto restart; explicit fresh generation rejects old clients/replies."}
    };
    {std::ofstream o(base/"scenario_plan.json");o<<"{\"failure_modes_defined_before_production\":true,\"scenarios\":[";for(size_t i=0;i<scenarios.size();++i){if(i)o<<',';o<<"{\"name\":\""<<scenarios[i].first<<"\",\"behavior\":\""<<jsonEscape(scenarios[i].second)<<"\"}";}o<<"]}\n";}
    const std::string guest=R"TRIT(fn main() -> t40 {
        let session: t40=os_gfx_register(11000,8);
        unsafe { store(10000,session); }
        let joined: t40=os_gfx_join(session,11020,8);
        let submitted: t40=os_gfx_submit(session,11100,8,11200);
        let received: t40=os_gfx_receive(session,11300,16,0);
        let replied: t40=os_gfx_reply(session,11400,10);
        let left: t40=os_gfx_leave(session);
        unsafe { store(10001,joined); store(10002,submitted); store(10003,received); store(10004,replied); store(10005,left); store(10090,777); }
        os_futex_wait(10400,0,-1); return 0;
    })TRIT";
    std::ofstream(base/"interface_caller.trit")<<guest;
    compiler::CompilerOptions opts;opts.enable_vector_abi=true;opts.enable_vector_spilling=true;
    const auto app=compiler::compileSource("interface_caller.trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("apps/os_sdk.trit")+"\n"+guest,opts);
    {std::ofstream o(base/"interface_caller.compile.txt");for(const auto& d:app.diagnostics)o<<d.format()<<'\n';}
    {std::ofstream o(base/"source_hashes.json");o<<"{\"algorithm\":\"FNV-1a-64\",\"input\":\"file bytes\",\"sources\":{";bool comma=false;for(const auto& path:std::vector<std::string>{"kernel.trit","ternary_compiler_ir.h","ternary_compiler_codegen.h","apps/os_sdk.trit","SYSCALL_MANIFEST.json","tests/current_only_conformance.cpp","tools/trit_tool.py","TEST_MANIFEST.json","docs/07_OS_Substrate/native_compositor_contract.md"}){if(comma)o<<',';comma=true;o<<'"'<<path<<"\":\""<<stableHash(readTextFile(path))<<'"';}o<<"}}\n";}
    bool completed=false;std::map<int,std::array<long long,3>> returns;
    if(app.success) {
        compiler::LinkOptions lo;lo.stack_hint_words=2048;lo.dead_strip_functions=true;lo.standalone_halt_on_exit=false;lo.enable_vector_abi=true;lo.enable_vector_spilling=true;
        const auto linked=compiler::linkModules({app.object},lo);
        {std::ofstream o(base/"interface_caller.link.txt");for(const auto& d:linked.diagnostics)o<<d.format()<<'\n';}
        std::ofstream(base/"interface_caller.linked.tasm")<<linked.assembly;
        if(linked.success) {
            os::NativeVfsImageBuilder fs(32768);bool built=fs.installBaseLayout().ok();
            built &= require(fs.addExecutableImage("/tmp/gfx-probe",linked.assembled.program,linked.executable_header_v3,256).ok(),"transport fixture executable installation");
            const auto kernel=compiler::compileSource("transport_kernel.trit",readTextFile("architecture_contract.trit")+"\n"+readTextFile("kernel.trit"));
            {std::ofstream o(base/"kernel.compile.txt");for(const auto& d:kernel.diagnostics)o<<d.format()<<'\n';}
            std::string boot=".isa 2\n.require scalar_advanced lane vector accumulator_ai atomics mmu wait wide_t50 executable_v3 vector_geometry vector_context vector_spill\n.text\nboot:\n mov sp, 16383\n call kernel_init\n mov r13, 0\n mov r14, 3\n call process_set_state\n mov r1, native_trap_entry\n csrw tvec, r1\n";
            const std::string path="/tmp/gfx-probe";
            for(size_t i=0;i<=path.size();++i)boot+=" mov r1, "+std::to_string(i==path.size()?0:static_cast<unsigned char>(path[i]))+"\n mov r2, "+std::to_string(10020+i)+"\n store r1, r2, 0\n";
            boot+=" mov r13, 0\n mov r14, 10020\n mov r15, 63\n mov r16, 0\n mov r17, 1\n mov r18, 12000\n call app_register\n mov r2, 14200\n store r13, r2, 0\n mov r13, 1\n mov r14, 10020\n mov r15, 1\n mov r16, 202\n call app_spawn\n mov r2, 14300\n store r13, r2, 0\n mov r13, 202\n call scheduler_switch_to_pid\n mov r2, 3019\n load r1, r2, 0\n csrw scratch, r1\n load r3, r1, 0\n csrw epc, r3\n load r3, r1, 1\n csrw status, r3\n load r3, r1, 2\n csrw user_imem_ptbr, r3\n load r3, r1, 3\n csrw user_imem_pages, r3\n load r3, r1, 4\n csrw user_dmem_ptbr, r3\n load r3, r1, 5\n csrw user_dmem_pages, r3\n load r3, r1, 31\n copy sp, r3\n eret\n";
            boot+=readTextFile("native_kernel_trap_stub.tasm")+"\n"+kernel.assembly;
            auto fw=vm::executableFeatureWordNumeric(architecture::v3::REQUIRED_FEATURES,architecture::v3::FEATURE_V3_LAST);
            auto source=[&](int count){return boot+"\n.data\nboot_exec: .execheader3 0, "+std::to_string(count)+", 0, 27, "+std::to_string(fw)+", 2, 0\n";};
            auto assembled=vm::assembler::assembleV2(source(1));fw=vm::executableFeatureWordNumeric(assembled.required_features,architecture::v3::FEATURE_V3_LAST);
            assembled=vm::assembler::assembleV2(source(1));if(assembled.success)assembled=vm::assembler::assembleV2(source(static_cast<int>(assembled.program.size())));
            std::ofstream(base/"boot.tasm")<<source(static_cast<int>(assembled.program.size()));
            {std::ofstream o(base/"assembly.txt");for(const auto& d:assembled.errors)o<<d.format()<<'\n';}
            if(built && kernel.success && assembled.success) {
                host::TosImageManifest manifest;const auto image=host::bootImageFromAssembly(assembled,manifest,fs.image());std::string error;
                built &= host::writeBootImageFile((base/"boot.tboot").string(),image,&error);
                auto machine=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));built &= host::loadBootImageIntoVm(*machine,image,"",&error);
                std::ofstream raw(base/"raw_returns.jsonl");int pending_service=-1,pending_pc=-1;long long step=0;
                if(built)for(;step<120000000;++step) {
                    vm::VMExecutionRecord rec;vm::step(*machine,&rec);
                    if(rec.attempted && rec.has_instruction && rec.before_privilege==vm::PrivilegeMode::User && rec.instruction.opcode==isa::Opcode::SYSCALL){pending_service=rec.syscall_id;pending_pc=rec.pc;raw<<"{\"kind\":\"syscall_entry\",\"step\":"<<step<<",\"pid\":202,\"service\":"<<pending_service<<",\"pc\":"<<pending_pc<<"}\n";}
                    if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::ERET && pending_service>=0 && machine->pc==pending_pc+1 && serviceKernelWord(*machine,3020)==202){
                        const std::array<long long,3> tuple{{vm::ops::toLong(machine->regfile.read(13)),vm::ops::toLong(machine->regfile.read(14)),vm::ops::toLong(machine->regfile.read(15))}};
                        returns[pending_service]=tuple;raw<<"{\"kind\":\"matched_syscall_return\",\"step\":"<<step<<",\"pid\":202,\"service\":"<<pending_service<<",\"epc\":"<<machine->pc<<",\"tuple\":["<<tuple[0]<<','<<tuple[1]<<','<<tuple[2]<<"]}\n";pending_service=-1;
                    }
                    if(step%1000==0 && serviceUserWord(*machine,10090,1)==777){completed=true;break;}
                    if(machine->status==vm::VMStatus::TRAPPED || machine->status==vm::VMStatus::HALTED)break;
                }
                std::ofstream(base/"execution.json")<<"{\"completed\":"<<(completed?"true":"false")<<",\"steps\":"<<step<<",\"registration_return\":"<<serviceKernelWord(*machine,14200)<<",\"launch_return\":"<<serviceKernelWord(*machine,14300)<<",\"sdk_results\":["<<serviceUserWord(*machine,10000,1)<<','<<serviceUserWord(*machine,10001,1)<<','<<serviceUserWord(*machine,10002,1)<<','<<serviceUserWord(*machine,10003,1)<<','<<serviceUserWord(*machine,10004,1)<<','<<serviceUserWord(*machine,10005,1)<<"]}\n";
                completed=require(host::detail::writeCheckpointState(base/"terminal_state.bin",*machine,&error),"transport interface checkpoint "+error) && completed;
                completed=require(machine->block_device.writeSnapshotFile((base/"disk.tdisk").string()),"transport interface disk snapshot") && completed;
            }
        }
    }
    bool unknown_services=completed;for(int service=61;service<=66;++service)unknown_services &= returns.count(service)>0 && returns[service]==std::array<long long,3>{{-1,0,-8}};
    const bool fork_pass=runCompositorForkPrerequisiteVariants();
    const bool behavior_pass=app.success && runTransportBehaviorFamilies(base/"behavior");
    const bool passed=completed && fork_pass && behavior_pass;
    const std::string phase=passed?"authenticated_transport_GREEN":!app.success?"missing_interface_RED":unknown_services?"old_kernel_or_unauthorized_interface_RED":"fixture_INCOMPLETE";
    std::ofstream report(base/"report.json");report<<"{\"status\":\""<<(passed?"PASS":"INCOMPLETE")<<"\",\"scope\":\"focused\",\"phase\":\""<<phase<<"\",\"run_directory\":\"run_"<<n<<"\",\"later_stage_resource_display_gates\":\"pending\",\"criteria_complete\":"<<(passed?"true":"false")<<"}\n";report.flush();
    const bool report_written=require(report.good(),"transport append report written");report.close();
    std::ofstream summary(root/"report.json");summary<<readTextFile(base/"report.json");summary.flush();
    const bool summary_written=require(summary.good(),"transport summary written");
    std::cout<<"compositor transport artifact: "<<base.string()<<'\n';
    return require(passed && report_written && summary_written,"compositor transport "+phase);
}

int main(int argc, char** argv) {
    using namespace sandbox;
    using namespace sandbox::vm;
    bool cli_only = false, desktop_only = false; // additive final CLI/desktop selectors
    bool sdk_only = false;
    bool compositor_drawing_only = false;
    bool compositor_drawing_lifecycle_only = false;
    bool compositor_prerequisites_only = false, compositor_transport_only = false, compositor_fork_prerequisite_only = false;
    bool compositor_fork_window_lifetime_only = false, compositor_fork_fatal_user_only = false, compositor_fork_memory_only = false, compositor_fork_malformed_v3_only = false;
    bool stream_only = false;
    bool metrics_only = false;
    bool clock_only = false;
    bool clock_kernel_only = false;
    bool clock_idle_input_only = false; // additive idle input selector
    bool numeric_only = false;
    // BEGIN SERVICE LINK PREFLIGHT
    bool service_link_only = false;
// END SERVICE LINK PREFLIGHT
    // BEGIN SERVICE FIXTURE VALUES
    bool service_fixtures_only = false;
// END SERVICE FIXTURE VALUES
    bool kernel_word_classification_only = false;
    bool service_only = false;
    bool service_auto_only = false;
    bool service_auto_three_only = false;
    bool service_history_cap1_only = false;
    bool service_reservation_only = false; // additive reservation selector
    bool service_edges_only = false, service_coexistence_only = false; // additive service triage selectors
    bool extent_lifecycle_only = false;
    bool wal_shape_only = false;
    bool wal_only = false, wal_admission_only = false, service_errors_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--kernel-word-classification-only") kernel_word_classification_only = true;
        if (std::string(argv[i]) == "--cli-only") cli_only = true;
        if (std::string(argv[i]) == "--desktop-only") desktop_only = true;
        if (std::string(argv[i]) == "--wal-shape-only") wal_shape_only = true;
        if (std::string(argv[i]) == "--wal-only") wal_only = true;
        if (std::string(argv[i]) == "--wal-admission-only") wal_admission_only = true;
        if (std::string(argv[i]) == "--service-errors-only") service_errors_only = true;
        if (std::string(argv[i]) == "--sdk-only") sdk_only = true;
        if (std::string(argv[i]) == "--compositor-prerequisites-only") compositor_prerequisites_only = true;
        if (std::string(argv[i]) == "--compositor-drawing-only") compositor_drawing_only = true;
        if (std::string(argv[i]) == "--compositor-drawing-lifecycle-only") compositor_drawing_lifecycle_only = true;
        if (std::string(argv[i]) == "--compositor-transport-only") compositor_transport_only = true;
        if (std::string(argv[i]) == "--compositor-fork-prerequisite-only") compositor_fork_prerequisite_only = true;
        if (std::string(argv[i]) == "--compositor-fork-window-lifetime-only") compositor_fork_window_lifetime_only = true;
        if (std::string(argv[i]) == "--compositor-fork-fatal-user-only") compositor_fork_fatal_user_only = true;
        if (std::string(argv[i]) == "--compositor-fork-memory-only") compositor_fork_memory_only = true;
        if (std::string(argv[i]) == "--compositor-fork-malformed-v3-only") compositor_fork_malformed_v3_only = true;
        if (std::string(argv[i]) == "--stream-only") stream_only = true;
        if (std::string(argv[i]) == "--clock-kernel-only") clock_kernel_only = true;
        if (std::string(argv[i]) == "--clock-idle-input-only") clock_idle_input_only = true; // additive idle input selector
        if (std::string(argv[i]) == "--clock-only") clock_only = true;
        if (std::string(argv[i]) == "--metrics-only") metrics_only = true;
        if (std::string(argv[i]) == "--numeric-only") numeric_only = true;
        // BEGIN SERVICE LINK PREFLIGHT
        if (std::string(argv[i]) == "--service-link-only") service_link_only = true;
// END SERVICE LINK PREFLIGHT
        // BEGIN SERVICE FIXTURE VALUES
        if (std::string(argv[i]) == "--service-fixtures-only") service_fixtures_only = true;
// END SERVICE FIXTURE VALUES
        if (std::string(argv[i]) == "--service-only") service_only = true;
        if (std::string(argv[i]) == "--service-auto-only") service_auto_only = true;
        if (std::string(argv[i]) == "--service-auto-modes-only") service_auto_three_only = true;
        if (std::string(argv[i]) == "--service-history-cap1-only") service_history_cap1_only = true;
        if (std::string(argv[i]) == "--service-reservation-only") service_reservation_only = true; // additive reservation selector
        if (std::string(argv[i]) == "--service-edges-only") service_edges_only = true; // additive service triage selectors
        if (std::string(argv[i]) == "--service-coexistence-only") service_coexistence_only = true; // additive service triage selectors
        if (std::string(argv[i]) == "--extent-lifecycle-only") extent_lifecycle_only = true;
    }

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

    const std::string selected_selector = argc>1 ? argv[1] : "default";
    auto coupled=[&](const std::string& group,const std::string& scope,const std::vector<std::string>& names,const std::function<bool()>& run) {return completionWorkflow(group,selected_selector,scope,names,run);};
    if (kernel_word_classification_only) return coupled("kernel_word_classification","focused",{"csr_denials","typed_word_queries","native_checkpoint_replay","caller_liveness"},[]{return runKernelWordClassification();}) ? 0 : 1;
    if (compositor_fork_prerequisite_only) return runCompositorForkPrerequisiteVariants() ? 0 : 1;
    if (compositor_fork_window_lifetime_only) return runCompositorForkWindowLifetimeProbe() ? 0 : 1;
    if (compositor_fork_fatal_user_only) return runCompositorForkFatalUser() ? 0 : 1;
    if (compositor_fork_memory_only) return runCompositorForkMemory() ? 0 : 1;
    if (compositor_fork_malformed_v3_only) return runCompositorForkMalformedV3() ? 0 : 1;
    if (compositor_drawing_lifecycle_only && !std::filesystem::is_regular_file("apps/span_draw.trit")) return runCompositorDrawing(true) ? 0 : 1;
    if (compositor_drawing_lifecycle_only) return coupled("compositor_drawing_lifecycle","focused",{"api_pressure","special_span_child","special_line_child","survivor_progress","caller_liveness"},[]{return runCompositorDrawing(true);}) ? 0 : 1;
    if (compositor_drawing_only && !std::filesystem::is_regular_file("apps/span_draw.trit")) return runCompositorDrawing() ? 0 : 1; // compile prerequisite only; no native completion claim
    if (compositor_drawing_only) return coupled("compositor_drawing","focused",{"raster_and_copy","special_span_child","special_line_child","survivor_progress","benchmark_horizontal","benchmark_shallow","benchmark_steep","benchmark_clipped"},[]{return runCompositorDrawing();}) ? 0 : 1;
    if (compositor_transport_only) return coupled("compositor_transport","focused",{"authority_pins_lifetimes/preopened_pin","authority_pins_lifetimes/designated_namespace_collision","atomic_records_guards","bounded_controls_conservation","cancel_stop_term","blocking_receive_reply","client_death_exec","counter_bounds","compositor_death_restart/ordinary","compositor_death_restart/forced"},[]{return runCompositorTransport();}) ? 0 : 1;
    if (compositor_prerequisites_only) return coupled("compositor_prerequisites","focused",{"capability_pressure","ordinary_exit","forced_exit","explicit_cancel","signal_interrupt","ordinary_wakeup","timeout"},[]{return runCompositorPrerequisites();}) ? 0 : 1;
    if (cli_only) return coupled("cli","focused",{},[]{return runCliWorkflow();}) ? 0 : 1;
    if (desktop_only) return coupled("desktop","focused",{},[]{return runDesktopIsolated();}) ? 0 : 1;
    if (wal_shape_only) return coupled("wal_shape","focused",completionShapes,[]{return runWalWorkflow(false,false,true);}) ? 0 : 1;
    if (extent_lifecycle_only) return coupled("wal_extent","focused",{"extent_lifecycle","extent_lifecycle_recovery"},[]{return runWalWorkflow(false,true);}) ? 0 : 1;
    if (wal_admission_only) return coupled("wal_admission","focused",{"low_corrupt","low_broken_chain","low_invalid_highwater","low_invalid_domain"},[]{return runWalWorkflow(true);}) ? 0 : 1;
    if (wal_only) return coupled("wal_full","full",completionWalGuards,[]{return runWalWorkflow();}) ? 0 : 1;
    // BEGIN SERVICE LINK PREFLIGHT
    if (service_link_only) return coupled("service_link","compile-link-only",completionServiceNames(),[]{return runServiceWorkflow(false,{},true);}) ? 0 : 1;
// END SERVICE LINK PREFLIGHT
    // BEGIN SERVICE FIXTURE VALUES
    if (service_fixtures_only) return coupled("service_fixtures","focused",{"tokens_reuse_restart","control_full_stall_cancel"},[]{return runServiceWorkflow(false,{"tokens_reuse_restart","control_full_stall_cancel"});}) ? 0 : 1;
// END SERVICE FIXTURE VALUES
    if (service_errors_only) return coupled("service_errors","focused",{"permanent_write","fsync_failure"},[]{return runServiceWorkflow(false,{"permanent_write","fsync_failure"});}) ? 0 : 1;
    if (service_history_cap1_only) return coupled("service_history_cap1","focused",{"history_cap1"},[]{return runServiceWorkflow(false,{"history_cap1"});}) ? 0 : 1;
    if (service_reservation_only) return coupled("service_reservation","focused",{"reserved_head_full_pool"},[]{return runServiceWorkflow(false,{"reserved_head_full_pool"});}) ? 0 : 1; // additive reservation selector
    // BEGIN ADDITIVE SERVICE TRIAGE DISPATCH
    if (service_coexistence_only) return coupled("service_coexistence","focused-coexistence",{"coexistence"},[]{return runServiceCoexistenceWorkflow();}) ? 0 : 1;
    if (service_edges_only) {
        std::set<std::string> selected;
        for (const auto& scenario : serviceScenarios())
            if (!scenario.long_history) selected.insert(scenario.name);
        return coupled("service_edges","focused",std::vector<std::string>(selected.begin(),selected.end()),[&]{return runServiceWorkflow(false,selected);}) ? 0 : 1;
    }
    // END ADDITIVE SERVICE TRIAGE DISPATCH
    if (sdk_only) return coupled("sdk","focused",{},[]{return runSdkWorkflow();}) ? 0 : 1;
    if (service_auto_three_only) return coupled("service_auto_focused","focused-automatic-three",completionModes,[]{return runAutomaticStartWorkflow();}) ? 0 : 1;
    if (service_only || service_auto_only) {auto names=completionServiceNames();if(service_auto_only)names.insert(names.end(),completionModes.begin(),completionModes.end());return coupled(service_auto_only?"service_auto":"service_full","full",names,[&]{return runServiceWorkflow(service_auto_only);}) ? 0 : 1;}
    if (clock_kernel_only) return coupled("clock_kernel","focused",{},[]{return runClockKernelWorkflow();}) ? 0 : 1;
    if (clock_idle_input_only) return coupled("clock_idle_input","focused",{},[]{return runClockIdleInputWorkflow();}) ? 0 : 1; // additive idle input selector
    if (clock_only) {const bool kernel=coupled("clock_kernel","focused",{},[]{return runClockKernelWorkflow();});const bool service=coupled("clock_service","focused",{"clock_cap1","clock_cap2"},[]{return runServiceWorkflow(false,{"clock_cap1","clock_cap2"});});return kernel && service && coupled("clock_boundaries","focused",{},[]{return runClockKernelWorkflow(true);}) ? 0 : 1;}
    if (metrics_only) return coupled("metrics","focused",{},[]{return runMetricsWorkflow();}) ? 0 : 1;
    if (numeric_only) return coupled("numeric","focused",{},[]{return runNumericWorkflow();}) ? 0 : 1;

    if (stream_only) {
        if (!coupled("stream",stream_only?"focused":"full",{},[]{return runStreamWorkflow();})) return 1;
        return 0;
    }

    if (!coupled("wal_full","full",completionWalGuards,[]{return runWalWorkflow();})) return 1;
    if (!coupled("sdk","full",{},[]{return runSdkWorkflow();})) return 1;
    if (!coupled("numeric","full",{},[]{return runNumericWorkflow();})) return 1;
    if (!coupled("metrics","full",{},[]{return runMetricsWorkflow();})) return 1;
    if (!coupled("clock_kernel","full",{},[]{return runClockKernelWorkflow();}) || !coupled("clock_boundaries","full",{},[]{return runClockKernelWorkflow(true);})) return 1;
    if (!coupled("clock_idle_input","full",{},[]{return runClockIdleInputWorkflow();})) return 1; // additive idle input default gate
    if (!coupled("clock_service","focused",{"clock_cap1","clock_cap2"},[]{return runServiceWorkflow(false,{"clock_cap1","clock_cap2"});})) return 1;
    if (!coupled("desktop","full",{},[]{return runDesktopIsolated();})) return 1;
    if (!coupled("stream",stream_only?"focused":"full",{},[]{return runStreamWorkflow();})) return 1;
    if (!coupled("service_full","full",completionServiceNames(),[]{return runServiceWorkflow();})) return 1;

    if (!coupled("cli","full",{},[]{return runCliWorkflow();})) return 1;

    std::cout << "current platform conformance: ISA v2, executable ABI v3\n";
    return 0;
}
