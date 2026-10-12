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
    std::string guest = "fn main() -> t40 {\n";
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
        "fsync","close_success","close_invalid","reopen_fd0","bad_output_span","short_read37_of64","eof0","read_close","spawn_missing_fallback"};
    const std::vector<int> services={12,15,47,12,15,47,13,13,12,14,14,14,13,57};
    const std::vector<std::string> calls={"os_open(10350, 0)","os_write(-1, 10400, 37)","os_fsync(-1)",
        "os_open(10300, 3)","os_write(fd, 10400, 37)","os_fsync(fd)","os_close(fd)","os_close(fd)",
        "os_open(10300, 0)","os_read(rd, -1, 64)","os_read(rd, 10500, 64)","os_read(rd, 10600, 64)","os_close(rd)","os_spawn_app(10350, 2, 3)"};
    for(size_t i=0;i<calls.size();++i) {
        guest += "unsafe { store(10090, " + std::to_string(i) + "); }\n";
        const std::string variable=i==3?"fd":i==8?"rd":"result"+std::to_string(i);
        guest += "let " + variable + ": t40 = " + calls[i] + "; unsafe { store(" + std::to_string(10000+i) + ", " + variable + "); }\n";
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
            awaiting=false;
        }
        if(!machine->trap_active && userword(*machine,10091)==777) { reached=true; break; }
        if(machine->status!=vm::VMStatus::RUNNING && machine->status!=vm::VMStatus::WAITING) break;
    }
    raw.close();
    check(reached,"guest completed all calls and live-value stores pc="+std::to_string(machine->pc)+" status="+std::to_string(static_cast<int>(machine->status))+" cause="+std::to_string(machine->cause));
    if(!reached) (void)host::detail::writeCheckpointState(artifact/"failed_vm_state.bin",*machine,&error);
    check(!awaiting && records.size()==32,"all 32 actual syscall ERET returns captured");
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
            if(i==13) check(r.status==-1 && r.payload==0 && r.detail>0 && actual==-1,"real spawn missing executable positive-detail fallback");
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
INPUTS = json.loads('["APP_MANIFEST.json", "ARCHITECTURE_MANIFEST.json", "CMakeLists.txt", "IMAGE_FORMAT_MANIFEST.json", "SYSCALL_MANIFEST.json", "TEST_MANIFEST.json", "apps/about.trit", "apps/bin_core.trit", "apps/calculator.trit", "apps/clear.trit", "apps/crash.trit", "apps/date.trit", "apps/desktop.trit", "apps/doctor.trit", "apps/file_manager.trit", "apps/fsck.trit", "apps/help.trit", "apps/init.trit", "apps/kill.trit", "apps/libwidget.trit", "apps/log.trit", "apps/login.trit", "apps/ls.trit", "apps/mount.trit", "apps/os_sdk.trit", "apps/paint.trit", "apps/passwd.trit", "apps/ps.trit", "apps/reboot.trit", "apps/service_host.trit", "apps/service_runtime.trit", "apps/service_stub.trit", "apps/settings.trit", "apps/shell.trit", "apps/shutdown.trit", "apps/sleep.trit", "apps/sync.trit", "apps/sysinfo.trit", "apps/task_manager.trit", "apps/terminal.trit", "apps/test.trit", "apps/text_editor.trit", "architecture_contract.h", "architecture_contract.trit", "architecture_instruction_codec.h", "architecture_isa_v2.h", "architecture_v2_assembler_directives.h", "architecture_v2_assembler_types.h", "architecture_v2_support.h", "build_tos_image.cpp", "executable_header_common.h", "executable_header_v3.h", "kernel.trit", "native_kernel_trap_stub.tasm", "ternary_asm.h", "ternary_backend.h", "ternary_binary_io.h", "ternary_compiler.h", "ternary_compiler_ast.h", "ternary_compiler_cfg.h", "ternary_compiler_codegen.h", "ternary_compiler_ir.h", "ternary_compiler_lexer.h", "ternary_compiler_parser.h", "ternary_compiler_types.h", "ternary_dataflow.trit", "ternary_host_runtime.h", "ternary_ir.h", "ternary_isa.h", "ternary_lanes.h", "ternary_math.h", "ternary_native_ops.h", "ternary_os.h", "ternary_redo_wal.h", "ternary_scalar.h", "ternary_simd.h", "ternary_stream.trit", "ternary_symbolic_encoding.h", "ternary_uint128.h", "ternary_vm.h", "ternary_vm_state.h", "tests/current_only_conformance.cpp", "tools/trit_tool.py", "tritc.cpp"]')
CANDIDATE = json.loads('{"schema_version": 1, "platform": {"isa_version": 2, "executable_abi_version": 3, "function_abi_version": 3, "syscall_abi_version": 2, "vector_abi_version": 1, "tboot_version": 3, "tdisk_version": 2}, "conformance": {"source": "tests/current_only_conformance.cpp", "target": "trit_current_conformance", "ctest_name": "trit_current_conformance", "selectors": ["--cli-only", "--clock-idle-input-only", "--clock-kernel-only", "--clock-only", "--desktop-only", "--extent-lifecycle-only", "--metrics-only", "--numeric-only", "--sdk-only", "--service-auto-modes-only", "--service-auto-only", "--service-coexistence-only", "--service-edges-only", "--service-errors-only", "--service-fixtures-only", "--service-history-cap1-only", "--service-link-only", "--service-only", "--service-reservation-only", "--stream-only", "--wal-admission-only", "--wal-only", "--wal-shape-only"], "default_workflows": ["wal_full", "sdk", "numeric", "metrics", "clock_kernel", "clock_boundaries", "clock_idle_input", "clock_service", "desktop", "stream", "service_full", "cli"], "separate_required_workflows": ["service_auto"], "timeout_seconds": 0}, "build": {"directory": "build_current_cleanup", "configure": ["cmake", "-S", ".", "-B", "build_current_cleanup"], "targets": {"current_images": {"direct_dependencies": ["release/TernaryOS/ternary-os.tboot", "release/TernaryOS/ternary-os.tdisk"], "producer": "build_tos_image"}, "current_validate": {"direct_dependencies": ["trit_current_conformance"], "runs": "trit_current_conformance"}, "ci_production": {"direct_dependencies_if_BUILD_TESTING": ["tritc", "build_tos_image", "ctest"], "direct_dependencies_if_not_BUILD_TESTING": ["tritc", "build_tos_image", "current_validate"]}, "stage_tos_release": {"available_if": "TRIT_ENABLE_SDL_RELEASE AND SDL2_FOUND AND run_tos_sdl.cpp exists", "direct_dependencies": ["current_images", "run_tos_sdl"]}, "smoke_tos_release": {"available_if": "stage_tos_release available", "direct_dependencies": ["stage_tos_release"], "frames": 10}}}, "cli": {"source": "tools/trit_tool.py", "commands": ["doctor", "contract-check", "test", "export-diagnostics"]}}')
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
const std::vector<std::string> completionSourcePaths = {"APP_MANIFEST.json","ARCHITECTURE_MANIFEST.json","CMakeLists.txt","IMAGE_FORMAT_MANIFEST.json","SYSCALL_MANIFEST.json","TEST_MANIFEST.json","apps/about.trit","apps/bin_core.trit","apps/calculator.trit","apps/clear.trit","apps/crash.trit","apps/date.trit","apps/desktop.trit","apps/doctor.trit","apps/file_manager.trit","apps/fsck.trit","apps/help.trit","apps/init.trit","apps/kill.trit","apps/libwidget.trit","apps/log.trit","apps/login.trit","apps/ls.trit","apps/mount.trit","apps/os_sdk.trit","apps/paint.trit","apps/passwd.trit","apps/ps.trit","apps/reboot.trit","apps/service_host.trit","apps/service_runtime.trit","apps/service_stub.trit","apps/settings.trit","apps/shell.trit","apps/shutdown.trit","apps/sleep.trit","apps/sync.trit","apps/sysinfo.trit","apps/task_manager.trit","apps/terminal.trit","apps/test.trit","apps/text_editor.trit","architecture_contract.h","architecture_contract.trit","architecture_instruction_codec.h","architecture_isa_v2.h","architecture_v2_assembler_directives.h","architecture_v2_assembler_types.h","architecture_v2_support.h","build_tos_image.cpp","executable_header_common.h","executable_header_v3.h","kernel.trit","native_kernel_trap_stub.tasm","ternary_asm.h","ternary_backend.h","ternary_binary_io.h","ternary_compiler.h","ternary_compiler_ast.h","ternary_compiler_cfg.h","ternary_compiler_codegen.h","ternary_compiler_ir.h","ternary_compiler_lexer.h","ternary_compiler_parser.h","ternary_compiler_types.h","ternary_dataflow.trit","ternary_host_runtime.h","ternary_ir.h","ternary_isa.h","ternary_lanes.h","ternary_math.h","ternary_native_ops.h","ternary_os.h","ternary_redo_wal.h","ternary_scalar.h","ternary_simd.h","ternary_stream.trit","ternary_symbolic_encoding.h","ternary_uint128.h","ternary_vm.h","ternary_vm_state.h","tests/current_only_conformance.cpp","tools/trit_tool.py","tritc.cpp"}; // injected settled84 closure
const std::map<std::string,std::string> completionReports = {{"wal_full","service_workflow_artifact/wal/report.json"},{"wal_admission","service_workflow_artifact/wal/admission_low/report.json"},{"wal_shape","service_workflow_artifact/wal/checkpoint_shapes/checkpoint_shapes_report.json"},{"wal_extent","service_workflow_artifact/wal/extent_lifecycle/extent_lifecycle_report.json"},{"service_full","service_workflow_artifact/service/report.json"},{"service_errors","service_workflow_artifact/service/report.json"},{"service_fixtures","service_workflow_artifact/service/report.json"},{"service_history_cap1","service_workflow_artifact/service/report.json"},{"service_reservation","service_workflow_artifact/service/report.json"},{"service_edges","service_workflow_artifact/service/report.json"},{"service_link","service_workflow_artifact/service_link_preflight/link_preflight_report.json"},{"service_auto","service_workflow_artifact/service/automatic_start/automatic_report.json"},{"service_auto_focused","service_workflow_artifact/review/automatic_start/focused/automatic_report.json"},{"service_coexistence","service_workflow_artifact/service/coexistence/coexistence_report.json"},{"sdk","service_workflow_artifact/sdk/sdk_report.json"},{"metrics","service_workflow_artifact/metrics/phase2_report.json"},{"stream","service_workflow_artifact/stream/phase1_report.json"},{"numeric","numeric_workflow_artifact/report.json"},{"desktop",""},{"clock_kernel","service_workflow_artifact/clock/kernel/report.json"},{"clock_boundaries","service_workflow_artifact/clock/kernel_boundaries/report.json"},{"clock_idle_input","service_workflow_artifact/clock/idle_input/report.json"},{"clock_service","service_workflow_artifact/clock/service/report.json"},{"cli","service_workflow_artifact/final_gate/cli_e2e/scenario_report.json"}}; // injected actual groups
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
    const std::map<std::string,std::vector<std::string>> extra={{"sdk",{"embedded_guest.trit","guest.linked.tasm","sdk.tboot","raw_return_records.jsonl","sdk_results.json","vfs_content.json","disk.tdisk"}},{"metrics",{"embedded_guest.trit","guest.linked.tasm","metrics.tboot","timer_transitions.jsonl","kernel_assertions.json","idle_accounting.json","guest_report.json","busy_syscall_accounting.json","vm_run_accounting.json"}},{"stream",{"embedded_guest.trit","guest.linked.tasm","source_hashes.json","stream-events.json"}},{"numeric",{"guest.trit","guest_O0.tasm","guest_O1.tasm","guest_O2.tasm","accumulator.tasm","mid_reduction.bin"}},{"clock_kernel",{"embedded_guest.trit","guest.linked.tasm","clock.tboot","execution.json","actual_boot_rate.jsonl","actual_waits.jsonl","queue_transitions.jsonl","terminal_state.bin"}},{"clock_boundaries",{"embedded_guest.trit","guest.linked.tasm","clock.tboot","execution.json","actual_boot_rate.jsonl","actual_waits.jsonl","queue_transitions.jsonl","terminal_state.bin"}},{"clock_idle_input",{"boot.tboot","phases.jsonl","eret_observations.jsonl","prefix_observations.jsonl","prefix_state.bin","guest0.trit","guest1.trit","guest2.trit"}}}; // injected actual smaller group evidence
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

int main(int argc, char** argv) {
    using namespace sandbox;
    using namespace sandbox::vm;
    bool cli_only = false, desktop_only = false; // additive final CLI/desktop selectors
    bool sdk_only = false;
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
        if (std::string(argv[i]) == "--cli-only") cli_only = true;
        if (std::string(argv[i]) == "--desktop-only") desktop_only = true;
        if (std::string(argv[i]) == "--wal-shape-only") wal_shape_only = true;
        if (std::string(argv[i]) == "--wal-only") wal_only = true;
        if (std::string(argv[i]) == "--wal-admission-only") wal_admission_only = true;
        if (std::string(argv[i]) == "--service-errors-only") service_errors_only = true;
        if (std::string(argv[i]) == "--sdk-only") sdk_only = true;
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
