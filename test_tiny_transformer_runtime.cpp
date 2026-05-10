#include "ternary_transformer_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace rt = sandbox::transformer_runtime;

int g_failures = 0;
rt::RuntimeStats g_tinyStats{};
long long g_tinyRuntimeUs = 0;
int g_tinyDmemWords = 0;
long double g_tinyMaxError = 0.0L;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

long double toLongDouble(sandbox::vm::TernaryValue value) {
    sandbox::LongTriple t = value.toLongTriple();
    if (t.isZero()) return 0.0L;
    auto decoded = sandbox::long_ops::decode(t);
    return decoded.first * std::pow(3.0L, static_cast<long double>(decoded.second));
}

long double readLongDouble(
    const sandbox::vm::VMState& vm,
    rt::TensorView view,
    int row,
    int col) {

    sandbox::vm::TernaryValue value;
    expect(rt::loadElement(vm, view, row, col, value), "read tensor element");
    return toLongDouble(value);
}

void expectNear(long double got, long double want, long double tol, const std::string& label) {
    const long double diff = std::fabs(got - want);
    const long double scale = std::max(1.0L, std::fabs(want));
    expect(diff <= tol * scale,
           label + " got " + std::to_string(static_cast<double>(got)) +
           " want " + std::to_string(static_cast<double>(want)));
}

void storeInt(
    sandbox::vm::VMState& vm,
    rt::TensorView view,
    int row,
    int col,
    long long value) {

    expect(rt::storeElement(vm, view, row, col, rt::intValue(value, view.mode)),
           "store integer tensor element");
}

void storeRatio(
    sandbox::vm::VMState& vm,
    rt::TensorView view,
    int row,
    int col,
    long long numerator,
    long long denominator) {

    expect(rt::storeElement(vm, view, row, col, rt::ratioValue(numerator, denominator)),
           "store ratio tensor element");
}

sandbox::vm::TernaryValue l1Value(int8_t trit) {
    sandbox::TritLane1 lane;
    lane.setTrit(0, trit);
    return sandbox::vm::TernaryValue::fromL1(lane);
}

void storeL1(
    sandbox::vm::VMState& vm,
    rt::TensorView view,
    int row,
    int col,
    int8_t trit) {

    expect(rt::storeElement(vm, view, row, col, l1Value(trit)),
           "store L1 tensor element");
}

using Matrix = std::vector<std::vector<long double>>;

Matrix matmulHost(const Matrix& a, const Matrix& b) {
    Matrix out(a.size(), std::vector<long double>(b[0].size(), 0.0L));
    for (std::size_t row = 0; row < a.size(); ++row) {
        for (std::size_t col = 0; col < b[0].size(); ++col) {
            for (std::size_t k = 0; k < b.size(); ++k) {
                out[row][col] += a[row][k] * b[k][col];
            }
        }
    }
    return out;
}

Matrix transposeHost(const Matrix& input) {
    Matrix out(input[0].size(), std::vector<long double>(input.size(), 0.0L));
    for (std::size_t row = 0; row < input.size(); ++row) {
        for (std::size_t col = 0; col < input[row].size(); ++col) {
            out[col][row] = input[row][col];
        }
    }
    return out;
}

Matrix softmaxHost(const Matrix& logits) {
    Matrix out = logits;
    for (std::size_t row = 0; row < logits.size(); ++row) {
        long double maxValue = logits[row][0];
        for (long double value : logits[row]) maxValue = std::max(maxValue, value);
        long double sum = 0.0L;
        for (std::size_t col = 0; col < logits[row].size(); ++col) {
            out[row][col] = std::exp(logits[row][col] - maxValue);
            sum += out[row][col];
        }
        for (long double& value : out[row]) value /= sum;
    }
    return out;
}

Matrix layerNormHost(const Matrix& input) {
    Matrix out = input;
    for (std::size_t row = 0; row < input.size(); ++row) {
        long double mean = 0.0L;
        for (long double value : input[row]) mean += value;
        mean /= static_cast<long double>(input[row].size());

        long double variance = 0.0L;
        for (long double value : input[row]) {
            const long double centered = value - mean;
            variance += centered * centered;
        }
        variance /= static_cast<long double>(input[row].size());
        const long double denom = std::sqrt(variance + 0.001L);

        for (std::size_t col = 0; col < input[row].size(); ++col) {
            out[row][col] = (input[row][col] - mean) / denom;
        }
    }
    return out;
}

void storeMatrix(
    sandbox::vm::VMState& vm,
    rt::TensorView view,
    const Matrix& matrix) {

    for (int row = 0; row < view.rows; ++row) {
        for (int col = 0; col < view.cols; ++col) {
            const long double scaled = matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
            const long long numerator = static_cast<long long>(std::llround(scaled * 1000000.0L));
            storeRatio(vm, view, row, col, numerator, 1000000);
        }
    }
}

Matrix readMatrix(const sandbox::vm::VMState& vm, rt::TensorView view) {
    Matrix out(static_cast<std::size_t>(view.rows), std::vector<long double>(static_cast<std::size_t>(view.cols)));
    for (int row = 0; row < view.rows; ++row) {
        for (int col = 0; col < view.cols; ++col) {
            out[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] =
                readLongDouble(vm, view, row, col);
        }
    }
    return out;
}

void testExpSoftmaxAndActivation() {
    std::cout << "[1] runtime exp, softmax, tanh, gelu\n";

    for (long long x : {-1LL, 0LL, 1LL}) {
        sandbox::vm::TernaryValue got = rt::expT50(rt::intValue(x));
        sandbox::LongTriple oracle = sandbox::ops::exp(sandbox::native_ops::fromInt(x));
        expectNear(toLongDouble(got), toLongDouble(sandbox::vm::TernaryValue::fromLongTriple(oracle)),
                   1e-8L, "exp runtime x=" + std::to_string(x));
    }

    sandbox::vm::VMState vm(16, 64);
    rt::TensorView logits{0, 1, 3, sandbox::TernaryMode::T50};
    rt::TensorView probs{10, 1, 3, sandbox::TernaryMode::T50};
    storeInt(vm, logits, 0, 0, 1);
    storeInt(vm, logits, 0, 1, 2);
    storeInt(vm, logits, 0, 2, 3);
    expect(rt::softmaxRows(vm, logits, probs), "softmax rows succeeds");

    const Matrix expected = softmaxHost({{1.0L, 2.0L, 3.0L}});
    long double sum = 0.0L;
    for (int col = 0; col < 3; ++col) {
        const long double got = readLongDouble(vm, probs, 0, col);
        sum += got;
        expectNear(got, expected[0][static_cast<std::size_t>(col)], 2e-5L,
                   "softmax probability " + std::to_string(col));
    }
    expectNear(sum, 1.0L, 2e-5L, "softmax sums to one");

    for (long long x : {-1LL, 0LL, 1LL}) {
        const long double xd = static_cast<long double>(x);
        expectNear(toLongDouble(rt::tanhT50(rt::intValue(x))), std::tanh(xd), 2e-5L,
                   "tanh runtime x=" + std::to_string(x));
        const long double gelu = 0.5L * xd * (1.0L + std::tanh(0.797885L * (xd + 0.044715L * xd * xd * xd)));
        expectNear(toLongDouble(rt::geluT50(rt::intValue(x))), gelu, 3e-5L,
                   "gelu runtime x=" + std::to_string(x));
    }
}

void testMatmulLayerNormAndT1Dot() {
    std::cout << "[2] matmul, layer norm, and T1 dot path\n";

    sandbox::vm::VMState vm(16, 256);
    rt::TensorView a{0, 2, 3, sandbox::TernaryMode::T20};
    rt::TensorView b{10, 3, 2, sandbox::TernaryMode::T20};
    rt::TensorView scalarOut{20, 2, 2, sandbox::TernaryMode::T50};
    rt::TensorView accumOut{30, 2, 2, sandbox::TernaryMode::T50};

    const long long av[2][3] = {{1, 2, 3}, {-1, 0, 4}};
    const long long bv[3][2] = {{2, -1}, {0, 3}, {1, 1}};
    const long long expected[2][2] = {{5, 8}, {2, 5}};
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) storeInt(vm, a, row, col, av[row][col]);
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 2; ++col) storeInt(vm, b, row, col, bv[row][col]);
    }

    expect(rt::matmulScalar(vm, a, b, scalarOut), "scalar matmul succeeds");
    expect(rt::matmulAccumulator(vm, a, b, accumOut), "accumulator matmul succeeds");
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            expectNear(readLongDouble(vm, scalarOut, row, col), expected[row][col], 1e-12L, "scalar matmul value");
            expectNear(readLongDouble(vm, accumOut, row, col), expected[row][col], 1e-12L, "accumulator matmul value");
        }
    }

    rt::TensorView lnIn{50, 1, 3, sandbox::TernaryMode::T50};
    rt::TensorView gamma{60, 1, 3, sandbox::TernaryMode::T50};
    rt::TensorView beta{70, 1, 3, sandbox::TernaryMode::T50};
    rt::TensorView lnOut{80, 1, 3, sandbox::TernaryMode::T50};
    for (int col = 0; col < 3; ++col) {
        storeInt(vm, lnIn, 0, col, col + 1);
        storeInt(vm, gamma, 0, col, 1);
        storeInt(vm, beta, 0, col, 0);
    }
    expect(rt::layerNormRows(vm, lnIn, gamma, beta, lnOut), "layer norm succeeds");
    Matrix lnExpected = layerNormHost({{1.0L, 2.0L, 3.0L}});
    for (int col = 0; col < 3; ++col) {
        expectNear(readLongDouble(vm, lnOut, 0, col), lnExpected[0][static_cast<std::size_t>(col)],
                   2e-5L, "layer norm value");
    }

    rt::TensorView t1A{100, 2, 3, sandbox::TernaryMode::L1};
    rt::TensorView t1B{110, 3, 2, sandbox::TernaryMode::L1};
    rt::TensorView t1Out{120, 2, 2, sandbox::TernaryMode::T50};
    const int8_t a1[2][3] = {{1, 0, -1}, {-1, 1, 1}};
    const int8_t b1[3][2] = {{1, -1}, {1, 1}, {-1, 0}};
    const long long e1[2][2] = {{2, -1}, {-1, 2}};
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) storeL1(vm, t1A, row, col, a1[row][col]);
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 2; ++col) storeL1(vm, t1B, row, col, b1[row][col]);
    }
    expect(rt::matmulT1Dot(vm, t1A, t1B, t1Out), "T1 dot matmul succeeds");
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            expectNear(readLongDouble(vm, t1Out, row, col), e1[row][col], 1e-12L, "T1 dot value");
        }
    }
}

void testTinyTransformerFixture() {
    std::cout << "[3] deterministic tiny character model fixture\n";

    sandbox::vm::VMState vm(64, 512);
    rt::RuntimeStats stats{};
    const auto start = std::chrono::high_resolution_clock::now();

    rt::TensorView hidden{0, 2, 3, sandbox::TernaryMode::T50};
    rt::TensorView gamma{10, 1, 3, sandbox::TernaryMode::T50};
    rt::TensorView beta{20, 1, 3, sandbox::TernaryMode::T50};
    rt::TensorView norm{30, 2, 3, sandbox::TernaryMode::T50};
    rt::TensorView normT{40, 3, 2, sandbox::TernaryMode::T50};
    rt::TensorView scores{50, 2, 2, sandbox::TernaryMode::T50};
    rt::TensorView attn{60, 2, 2, sandbox::TernaryMode::T50};
    rt::TensorView context{70, 2, 3, sandbox::TernaryMode::T50};
    rt::TensorView wout{90, 3, 4, sandbox::TernaryMode::T50};
    rt::TensorView logits{110, 2, 4, sandbox::TernaryMode::T50};
    rt::TensorView probs{130, 2, 4, sandbox::TernaryMode::T50};

    const Matrix embeddings = {
        {1.0L, 0.0L, -1.0L},
        {0.0L, 1.0L, 1.0L},
        {-1.0L, 1.0L, 0.0L},
        {1.0L, -1.0L, 1.0L},
    };
    const Matrix pos = {
        {0.0L, 1.0L, 0.0L},
        {1.0L, 0.0L, -1.0L},
    };
    const int tokens[2] = {1, 3};
    Matrix hiddenHost(2, std::vector<long double>(3, 0.0L));
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            hiddenHost[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] =
                embeddings[static_cast<std::size_t>(tokens[row])][static_cast<std::size_t>(col)] +
                pos[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
        }
    }
    storeMatrix(vm, hidden, hiddenHost);
    for (int col = 0; col < 3; ++col) {
        storeInt(vm, gamma, 0, col, 1);
        storeInt(vm, beta, 0, col, 0);
    }

    const Matrix woutHost = {
        {1.0L, -1.0L, 0.0L, 1.0L},
        {0.0L, 1.0L, 1.0L, -1.0L},
        {-1.0L, 0.0L, 1.0L, 1.0L},
    };
    storeMatrix(vm, wout, woutHost);

    expect(rt::layerNormRows(vm, hidden, gamma, beta, norm, &stats), "tiny layer norm succeeds");
    Matrix normHost = layerNormHost(hiddenHost);
    Matrix normRead = readMatrix(vm, norm);
    storeMatrix(vm, normT, transposeHost(normRead));
    expect(rt::matmulAccumulator(vm, norm, normT, scores, &stats), "tiny score matmul succeeds");
    expect(rt::softmaxRows(vm, scores, attn, &stats), "tiny attention softmax succeeds");
    expect(rt::matmulAccumulator(vm, attn, norm, context, &stats), "tiny context matmul succeeds");
    expect(rt::matmulAccumulator(vm, context, wout, logits, &stats), "tiny logits matmul succeeds");
    expect(rt::softmaxRows(vm, logits, probs, &stats), "tiny logits softmax succeeds");

    Matrix scoreHost = matmulHost(normHost, transposeHost(normHost));
    Matrix attnHost = softmaxHost(scoreHost);
    Matrix contextHost = matmulHost(attnHost, normHost);
    Matrix logitsHost = matmulHost(contextHost, woutHost);
    Matrix probsHost = softmaxHost(logitsHost);

    Matrix probsGot = readMatrix(vm, probs);
    long double maxError = 0.0L;
    for (int row = 0; row < probs.rows; ++row) {
        long double rowSum = 0.0L;
        for (int col = 0; col < probs.cols; ++col) {
            const long double got = probsGot[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
            const long double want = probsHost[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
            maxError = std::max(maxError, std::fabs(got - want));
            rowSum += got;
            expectNear(got, want, 1e-3L, "tiny final probability");
        }
        expectNear(rowSum, 1.0L, 1e-4L, "tiny probability row sum");
    }

    const auto end = std::chrono::high_resolution_clock::now();
    g_tinyStats = stats;
    g_tinyRuntimeUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    g_tinyDmemWords = 138;
    g_tinyMaxError = maxError;
}

void testNoBridgeRuntimeHeader() {
    std::cout << "[4] runtime no-bridge scan\n";

    const std::vector<std::string> banned = {
        "long double",
        "toDouble",
        "fromDouble",
        "long_ops::decode",
        "long_ops::encode",
        "native_ops::exp",
        "ops::exp",
    };

    std::ifstream in("ternary_transformer_runtime.h");
    expect(in.good(), "open ternary_transformer_runtime.h");
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) line = line.substr(0, comment);
        for (const std::string& token : banned) {
            expect(line.find(token) == std::string::npos,
                   "runtime header contains bridge token " + token);
        }
    }
}

void appendTinyRuntimeResult() {
    std::ofstream md("tuning_results.md", std::ios_base::app);
    if (!md.is_open()) return;
    md << "\n## Phase 5B Tiny Transformer VM Runtime Fixture\n\n";
    md << "| Runtime | DMEM Words | Scalar Ops | DMEM Loads | DMEM Stores | Runtime (us) | Max Error |\n";
    md << "| ------- | ---------: | ---------: | ---------: | ----------: | -----------: | --------: |\n";
    md << "| VM tensor runtime | " << g_tinyDmemWords
       << " | " << g_tinyStats.scalarOps
       << " | " << g_tinyStats.dmemLoads
       << " | " << g_tinyStats.dmemStores
       << " | " << g_tinyRuntimeUs
       << " | " << static_cast<double>(g_tinyMaxError)
       << " |\n";
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testExpSoftmaxAndActivation();
    testMatmulLayerNormAndT1Dot();
    testTinyTransformerFixture();
    testNoBridgeRuntimeHeader();
    appendTinyRuntimeResult();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " tiny transformer runtime failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll tiny transformer runtime tests passed\n";
    return EXIT_SUCCESS;
}
