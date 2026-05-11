#include "ternary_ir.h"
#include "ternary_vm.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace ir = sandbox::ir;

struct BenchRow {
    std::string name;
    int assemblyWords = 0;
    int steps = 0;
    uint64_t checksum = 0;
    std::string baseline;
    std::string delta;
    std::string notes;
};

struct RunData {
    bool ok = false;
    std::string assembly;
    std::vector<sandbox::isa::TritWord27> program;
    sandbox::vm::RunResult result{};
    sandbox::vm::VMState vm;
};

bool g_allRunsOk = true;

uint64_t mix(uint64_t hash, uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

uint64_t checksumLongs(const std::vector<long long>& values) {
    uint64_t hash = 1469598103934665603ULL;
    for (long long value : values) {
        hash = mix(hash, static_cast<uint64_t>(value));
    }
    return hash;
}

std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::string stepDelta(int steps, int baselineSteps) {
    if (steps <= 0 || baselineSteps <= 0) return "n/a";
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << (static_cast<double>(baselineSteps) / static_cast<double>(steps))
        << "x baseline/row";
    return out.str();
}

long long scalarLong(const sandbox::vm::VMState& vm, ir::Value value) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(value.reg)));
}

long long memoryLong(const sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
}

sandbox::vm::TernaryValue l1Value(int8_t trit) {
    sandbox::TritLane1 lane;
    lane.setTrit(0, trit);
    return sandbox::vm::TernaryValue::fromL1(lane);
}

RunData runAssembly(
    const std::string& assembly,
    const std::function<void(sandbox::vm::VMState&)>& setup = {},
    int maxSteps = 100000) {

    RunData data;
    data.assembly = assembly;
    auto assembled = sandbox::vm::assembler::assemble(assembly);
    if (!assembled.success) return data;
    data.program = assembled.program;
    data.vm = sandbox::vm::VMState(1024, 1024);
    if (!sandbox::vm::loadAndReset(data.vm, data.program)) return data;
    if (setup) setup(data.vm);
    data.result = sandbox::vm::run(data.vm, maxSteps);
    data.ok = data.result.halted();
    return data;
}

RunData runIr(
    const ir::Program& program,
    const std::function<void(sandbox::vm::VMState&)>& setup = {},
    int maxSteps = 100000) {

    auto lowered = program.lower();
    if (!lowered.success) {
        RunData failed;
        failed.assembly = lowered.assembly;
        return failed;
    }
    return runAssembly(lowered.assembly, setup, maxSteps);
}

BenchRow makeRow(
    const std::string& name,
    const RunData& run,
    uint64_t checksum,
    const std::string& baseline,
    const std::string& delta,
    const std::string& notes) {

    if (!run.ok) g_allRunsOk = false;
    return BenchRow{name,
                    static_cast<int>(run.program.size()),
                    run.result.steps,
                    checksum,
                    baseline,
                    delta,
                    notes};
}

std::pair<BenchRow, BenchRow> benchmarkDot() {
    const int8_t a[] = {1, 1, 0, -1};
    const int8_t b[] = {1, -1, 1, -1};

    ir::Program t1;
    ir::Value va = t1.vparam(ir::Type::L1);
    ir::Value vb = t1.vparam(ir::Type::L1);
    ir::Value dot = t1.vdotT1(va, vb);
    t1.halt();
    RunData t1Run = runIr(t1, [&](sandbox::vm::VMState& vm) {
        vm.vector_length = 4;
        vm.vregfile.reset(vm.vector_length);
        vm.vector_faults.reset(vm.vector_length);
        for (int lane = 0; lane < 4; ++lane) {
            vm.vregfile.reg[static_cast<std::size_t>(va.reg)].write(lane, l1Value(a[lane]));
            vm.vregfile.reg[static_cast<std::size_t>(vb.reg)].write(lane, l1Value(b[lane]));
        }
    });
    const uint64_t t1Checksum = checksumLongs({scalarLong(t1Run.vm, dot)});

    ir::Program t50;
    ir::Value sum;
    bool haveSum = false;
    for (int lane = 0; lane < 4; ++lane) {
        ir::Value av = t50.constant(ir::Type::T50, a[lane]);
        ir::Value bv = t50.constant(ir::Type::T50, b[lane]);
        ir::Value product = t50.mul(av, bv);
        if (!haveSum) {
            sum = product;
            haveSum = true;
        } else {
            sum = t50.add(sum, product);
        }
    }
    t50.halt();
    RunData t50Run = runIr(t50);
    const uint64_t t50Checksum = checksumLongs({scalarLong(t50Run.vm, sum)});

    BenchRow baseline = makeRow("T50 scalar dot", t50Run, t50Checksum, "baseline", "baseline",
                                "unrolled T50 multiply-add");
    BenchRow candidate = makeRow("T1 vector dot", t1Run, t1Checksum, "T50 scalar dot",
                                 stepDelta(t1Run.result.steps, t50Run.result.steps),
                                 "vdot.t1 over four L1 lanes");
    return {candidate, baseline};
}

BenchRow benchmarkTernaryRouting() {
    const int values[] = {-3, 0, 4, -1, 2};
    ir::Program program;
    ir::Value total = program.constant(ir::Type::T20, 0);
    ir::Value zero = program.constant(ir::Type::T20, 0);
    ir::Value negWeight = program.constant(ir::Type::T20, 100);
    ir::Value zeroWeight = program.constant(ir::Type::T20, 10);
    ir::Value posWeight = program.constant(ir::Type::T20, 1);
    for (int value : values) {
        ir::Value x = program.constant(ir::Type::T20, value);
        ir::Value cond = program.cmp(x, zero);
        ir::Value routed = program.tsel(cond, negWeight, zeroWeight, posWeight);
        ir::Value next = program.add(total, routed);
        program.release(x);
        program.release(cond);
        program.release(routed);
        program.release(total);
        total = next;
    }
    program.halt();
    RunData run = runIr(program);
    return makeRow("three-way routing ternary", run, checksumLongs({scalarLong(run.vm, total)}),
                   "binary-style branch routing", "see paired row",
                   "tcmp plus tsel, straight-line routing");
}

BenchRow benchmarkBinaryRouting(int ternarySteps) {
    std::ostringstream src;
    const int values[] = {-3, 0, 4, -1, 2};
    src << "mov.t20 r1, 0\n"
        << "mov.t20 r2, 0\n"
        << "mov.t20 r3, 100\n"
        << "mov.t20 r4, 10\n"
        << "mov.t20 r5, 1\n";
    for (int i = 0; i < 5; ++i) {
        src << "mov.t20 r6, " << values[i] << "\n"
            << "tcmp.t20 r7, r6, r2\n"
            << "brn r7, neg_" << i << "\n"
            << "brz r7, zero_" << i << "\n"
            << "add.t20 r1, r1, r5\n"
            << "jmp done_" << i << "\n"
            << "neg_" << i << ":\n"
            << "add.t20 r1, r1, r3\n"
            << "jmp done_" << i << "\n"
            << "zero_" << i << ":\n"
            << "add.t20 r1, r1, r4\n"
            << "done_" << i << ":\n";
    }
    src << "halt\n";
    RunData run = runAssembly(src.str());
    return makeRow("three-way routing binary-style", run,
                   checksumLongs({sandbox::vm::ops::toLong(run.vm.regfile.read(1))}),
                   "three-way routing ternary",
                   stepDelta(run.result.steps, ternarySteps),
                   "paired branch checks for negative/zero/positive");
}

BenchRow benchmarkOutlierTernary() {
    const int values[] = {-5, -1, 0, 2, 7};
    ir::Program program;
    ir::Value total = program.constant(ir::Type::T20, 0);
    ir::Value low = program.constant(ir::Type::T20, -2);
    ir::Value high = program.constant(ir::Type::T20, 2);
    ir::Value negWeight = program.constant(ir::Type::T20, 100);
    ir::Value inWeight = program.constant(ir::Type::T20, 10);
    ir::Value posWeight = program.constant(ir::Type::T20, 1);
    for (int value : values) {
        ir::Value x = program.constant(ir::Type::T20, value);
        ir::Value lowCmp = program.cmp(x, low);
        ir::Value highCmp = program.cmp(x, high);
        ir::Value highRoute = program.tsel(highCmp, inWeight, inWeight, posWeight);
        ir::Value route = program.tsel(lowCmp, negWeight, highRoute, highRoute);
        ir::Value next = program.add(total, route);
        program.release(x);
        program.release(lowCmp);
        program.release(highCmp);
        program.release(highRoute);
        program.release(route);
        program.release(total);
        total = next;
    }
    program.halt();
    RunData run = runIr(program);
    return makeRow("two-tailed outliers ternary", run, checksumLongs({scalarLong(run.vm, total)}),
                   "binary-style outlier branches", "see paired row",
                   "low/high compare routed with tsel");
}

BenchRow benchmarkOutlierBinary(int ternarySteps) {
    const int values[] = {-5, -1, 0, 2, 7};
    std::ostringstream src;
    src << "mov.t20 r1, 0\n"
        << "mov.t20 r2, -2\n"
        << "mov.t20 r3, 2\n"
        << "mov.t20 r4, 100\n"
        << "mov.t20 r5, 10\n"
        << "mov.t20 r6, 1\n";
    for (int i = 0; i < 5; ++i) {
        src << "mov.t20 r7, " << values[i] << "\n"
            << "tcmp.t20 r8, r7, r2\n"
            << "brn r8, low_" << i << "\n"
            << "tcmp.t20 r8, r7, r3\n"
            << "brp r8, high_" << i << "\n"
            << "add.t20 r1, r1, r5\n"
            << "jmp done_out_" << i << "\n"
            << "low_" << i << ":\n"
            << "add.t20 r1, r1, r4\n"
            << "jmp done_out_" << i << "\n"
            << "high_" << i << ":\n"
            << "add.t20 r1, r1, r6\n"
            << "done_out_" << i << ":\n";
    }
    src << "halt\n";
    RunData run = runAssembly(src.str());
    return makeRow("two-tailed outliers binary-style", run,
                   checksumLongs({sandbox::vm::ops::toLong(run.vm.regfile.read(1))}),
                   "two-tailed outliers ternary",
                   stepDelta(run.result.steps, ternarySteps),
                   "branching low/high threshold checks");
}

BenchRow benchmarkHeapTernary() {
    const int groups[4][3] = {{9, 4, 7}, {6, 8, 3}, {5, 2, 10}, {1, 11, 12}};
    ir::Program program;
    ir::Value total = program.constant(ir::Type::T20, 0);
    for (const auto& group : groups) {
        ir::Value a = program.constant(ir::Type::T20, group[0]);
        ir::Value b = program.constant(ir::Type::T20, group[1]);
        ir::Value c = program.constant(ir::Type::T20, group[2]);
        ir::Value ab = program.min(a, b);
        ir::Value best = program.min(ab, c);
        ir::Value next = program.add(total, best);
        program.release(a);
        program.release(b);
        program.release(c);
        program.release(ab);
        program.release(best);
        program.release(total);
        total = next;
    }
    program.halt();
    RunData run = runIr(program);
    return makeRow("ternary heap small sequence", run, checksumLongs({scalarLong(run.vm, total)}),
                   "binary heap small sequence", "see paired row",
                   "four fixed 3-child sift choices");
}

BenchRow benchmarkHeapBinary(int ternarySteps) {
    const int pairs[6][2] = {{9, 4}, {7, 6}, {8, 3}, {5, 2}, {10, 1}, {11, 12}};
    ir::Program program;
    ir::Value total = program.constant(ir::Type::T20, 0);
    for (const auto& pair : pairs) {
        ir::Value a = program.constant(ir::Type::T20, pair[0]);
        ir::Value b = program.constant(ir::Type::T20, pair[1]);
        ir::Value best = program.min(a, b);
        ir::Value next = program.add(total, best);
        program.release(a);
        program.release(b);
        program.release(best);
        program.release(total);
        total = next;
    }
    program.halt();
    RunData run = runIr(program);
    return makeRow("binary heap small sequence", run, checksumLongs({scalarLong(run.vm, total)}),
                   "ternary heap small sequence",
                   stepDelta(run.result.steps, ternarySteps),
                   "six fixed 2-child sift choices");
}

BenchRow benchmarkMatmul(ir::Type type) {
    const int a[2][3] = {{1, 2, 3}, {-1, 0, 4}};
    const int b[3][2] = {{2, -1}, {0, 3}, {1, 1}};
    ir::Program program;
    ir::Value base = program.zero(ir::Type::T50);
    int outIndex = 0;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            ir::Value sum;
            bool haveSum = false;
            for (int k = 0; k < 3; ++k) {
                ir::Value av = program.constant(type, a[row][k]);
                ir::Value bv = program.constant(type, b[k][col]);
                ir::Value product = program.mul(av, bv);
                program.release(av);
                program.release(bv);
                if (!haveSum) {
                    sum = product;
                    haveSum = true;
                } else {
                    ir::Value next = program.add(sum, product);
                    program.release(sum);
                    program.release(product);
                    sum = next;
                }
            }
            program.store(sum, base, outIndex++);
            program.release(sum);
        }
    }
    program.halt();
    RunData run = runIr(program);
    std::vector<long long> outputs;
    for (int i = 0; i < 4; ++i) outputs.push_back(memoryLong(run.vm, i));
    return makeRow(std::string("small matmul ") + ir::suffix(type), run, checksumLongs(outputs),
                   "same IR shape at other widths", "n/a",
                   "IR-generated 2x3 * 3x2 scalar kernel");
}

void writeResults(const std::vector<BenchRow>& rows) {
    std::ofstream out("architecture_benchmark_results.md");
    out << "# Architecture Benchmark Results\n\n";
    out << "Generated by `benchmark_architecture.cpp`. These are VM step-count and "
           "program-size comparisons, not host wall-clock speed claims.\n\n";
    out << "| Benchmark | Assembly Words | VM Steps | Checksum | Comparison Baseline | Step Delta | Notes |\n";
    out << "| --------- | -------------: | -------: | -------- | ------------------- | ---------- | ----- |\n";
    for (const BenchRow& row : rows) {
        out << "| " << row.name
            << " | " << row.assemblyWords
            << " | " << row.steps
            << " | `" << hex64(row.checksum)
            << "` | " << row.baseline
            << " | " << row.delta
            << " | " << row.notes
            << " |\n";
    }
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    std::vector<BenchRow> rows;

    auto [t1Dot, t50Dot] = benchmarkDot();
    rows.push_back(t1Dot);
    rows.push_back(t50Dot);

    BenchRow ternaryRouting = benchmarkTernaryRouting();
    BenchRow binaryRouting = benchmarkBinaryRouting(ternaryRouting.steps);
    rows.push_back(ternaryRouting);
    rows.push_back(binaryRouting);

    BenchRow ternaryOutliers = benchmarkOutlierTernary();
    BenchRow binaryOutliers = benchmarkOutlierBinary(ternaryOutliers.steps);
    rows.push_back(ternaryOutliers);
    rows.push_back(binaryOutliers);

    BenchRow ternaryHeap = benchmarkHeapTernary();
    BenchRow binaryHeap = benchmarkHeapBinary(ternaryHeap.steps);
    rows.push_back(ternaryHeap);
    rows.push_back(binaryHeap);

    rows.push_back(benchmarkMatmul(ir::Type::T10));
    rows.push_back(benchmarkMatmul(ir::Type::T20));
    rows.push_back(benchmarkMatmul(ir::Type::T50));

    writeResults(rows);

    if (!g_allRunsOk) {
        std::cerr << "One or more architecture benchmark VM programs failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "Wrote architecture_benchmark_results.md with "
              << rows.size() << " rows\n";
    return EXIT_SUCCESS;
}
