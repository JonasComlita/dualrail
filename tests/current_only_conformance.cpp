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
            << "\",\"sleep_units\":\"250 delivered kernel ticks; quantum 100000 guest cycles; masked expiries coalesce in one pending latch\",\"failures\":[";
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
    check(app_count==59,"AC1 actual59 app registrations");
    const std::regex path_key("\\\"guest_path\\\"");
    check(static_cast<size_t>(std::distance(std::sregex_iterator(app_manifest.begin(),app_manifest.end(),path_key),std::sregex_iterator()))==59,"AC1 manifest preserves exactly59 apps");
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
    i = 0;
    while i < 800 { sys_getpid(); i = i + 1; }
    os_service_metrics(BUFFER, 7);
    unsafe { store(REPORT + 10, load(BUFFER + 2)); store(REPORT + 11, 101); }
    os_sleep_ms(250);
    os_service_metrics(BUFFER, 7);
    unsafe { store(REPORT + 12, load(BUFFER + 2)); store(REPORT + 11, 102); }
    os_sleep_ms(250);
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
    int ordinary_returns=0;
    bool timer_preserved=true;
    long long irq_entries=0,idle_acks=0;
    for(int n=0;n<30'000'000;++n) {
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
        if(rec.attempted && rec.has_instruction && rec.instruction.opcode==isa::Opcode::SYSCALL && rec.syscall_id==7) {
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
    check(userword(*machine,10010)>userword(*machine,10016), "AC5 syscall-heavy guest delivers ticks beyond pre-loop tick");
    check(userword(*machine,10012)-userword(*machine,10010)>=250 && userword(*machine,10013)-userword(*machine,10012)>=250, "AC4 deadline ticks250 repeated");
    transitions.close();
    const auto parked_cycle = machine->cycle_count;
    const auto parked_epc = machine->epc;
    const auto parked_tick = word(*machine,3033);
    const auto parked_run = vm::run(*machine,1000);
    check(machine->status==vm::VMStatus::WAITING && machine->cycle_count==parked_cycle && parked_run.steps==0 && machine->epc==parked_epc, "AC6 no-deadline park without fabricated cycles/instructions");
    auto batch_vm=std::make_unique<vm::VMState>(host::profileForManifest(image.manifest));
    check(host::loadBootImageIntoVm(*batch_vm,image,"",&error),"AC7 fresh vm run image");
    const auto batch_result=vm::run(*batch_vm,30'000'000);
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
            (void)reference.runForSteps(30'000'000);
            check(replay.restoreCheckpointBundle((artifact/"timed_replay_bundle").string(),&error),"AC8 idle replay restore");
            (void)replay.replayFromCheckpoint(30'000'000,&error);
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
    for(int i=0;i<17;++i) { if(i) guest_report << ','; guest_report << userword(*machine,10000+i); }
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
            for(int n=0;n<30'000'000;++n) {
                const auto result = api==0 ? runtime.runForSteps(8192).status : runtime.stepOnce();
                if(result==vm::VMStatus::TRAPPED) break;
                if(result==vm::VMStatus::WAITING && runtime.snapshot().cycles==previous_cycles) break;
                previous_cycles=runtime.snapshot().cycles;
                if(api==0 && n>6000) break;
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

int main(int argc, char** argv) {
    using namespace sandbox;
    using namespace sandbox::vm;
    bool stream_only = false;
    bool metrics_only = false;
    bool numeric_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--stream-only") stream_only = true;
        if (std::string(argv[i]) == "--metrics-only") metrics_only = true;
        if (std::string(argv[i]) == "--numeric-only") numeric_only = true;
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

    if (metrics_only) return runMetricsWorkflow() ? 0 : 1;
    if (numeric_only) return runNumericWorkflow() ? 0 : 1;

    if (stream_only) {
        if (!runStreamWorkflow()) return 1;
        return 0;
    }

    if (!runNumericWorkflow()) return 1;
    if (!runMetricsWorkflow()) return 1;
    if (!runDesktopWorkflow()) return 1;
    if (!runStreamWorkflow()) return 1;

    std::cout << "current platform conformance: ISA v2, executable ABI v3\n";
    return 0;
}
