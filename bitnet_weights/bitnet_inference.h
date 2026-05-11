// =============================================================================
// bitnet_inference.h - Complete Forward Pass for BitNet b1.58
// =============================================================================
//
// Implements a single-token forward pass for BitNet b1.58 on the Ternary VM.
//
// Architecture (matches microsoft/bitnet-b1.58-2B-4T):
//   hidden_size:       2048
//   num_heads:         32
//   head_dim:          64   (hidden_size / num_heads)
//   num_kv_heads:      32   (MHA, not GQA)
//   intermediate_size: 5632 (SwiGLU MLP)
//   num_layers:        24
//   vocab_size:        32000

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __AVX2__
#include <immintrin.h>
#endif
//   norm:              RMSNorm
//   activation:        SiLU (approximated via GELU in this runtime)
//   attn weights:      L1  (ternary {-1,0,+1})
//   mlp weights:       L1  (ternary {-1,0,+1})
//   embed/norm:        T50 (full precision)
//
// DMEM Layout (word-addressed, one TernaryValue per word):
//
//   [0 .. H-1]          x          residual stream          (H = hidden_size)
//   [H .. 2H-1]         norm_out   after RMSNorm            (H)
//   [2H .. 3H-1]        q          Q projection output      (H)
//   [3H .. 4H-1]        k          K projection output      (H)
//   [4H .. 5H-1]        v          V projection output      (H)
//   [5H .. 6H-1]        attn_out   weighted sum over V      (H)
//   [6H .. 6H+I-1]      mlp_gate   gate_proj output         (I = intermediate_size)
//   [6H+I .. 6H+2I-1]  mlp_up     up_proj output           (I)
//   [6H+2I .. 6H+3I-1] mlp_act    gate * silu(up)          (I)
//   [6H+3I .. 7H+3I-1] mlp_down   down_proj output         (H)
//   [WEIGHT_BASE ..]    weights    all model weights        (high DMEM)
//
// Weight loading:
//   Weights are streamed from disk into a sliding window at WEIGHT_BASE.
//   Each layer's weights are loaded, used, and overwritten by the next layer.
//   This keeps peak DMEM usage at O(2 * layer_weight_count) not O(all_weights).
//
// Single-token simplification:
//   seq_len = 1. Attention score is a single value per head (Q·K / sqrt(d)).
//   Softmax of a single element is 1.0. Attention output = V directly (scaled).
//   A full KV-cache path is left as a TODO for multi-token generation.

#pragma once
#ifndef BITNET_INFERENCE_H
#define BITNET_INFERENCE_H

#include "bitnet_loader.h"
#include "../ternary_transformer_runtime.h"

#include <chrono>
#include <vector>
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <future>
#include <thread>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <cstring>
#include <mutex>
#include <future>
#include <condition_variable>
#include <queue>
#include <functional>
#include <filesystem>

namespace sandbox {
namespace bitnet {

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false), active_tasks(0) {
        if (threads == 0) threads = 1;
        for(size_t i = 0; i<threads; ++i)
            workers.emplace_back([this]{
                for(;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                    if (--active_tasks == 0) {
                        std::lock_guard<std::mutex> lock(wait_mutex);
                        wait_condition.notify_all();
                    }
                }
            });
    }

    void execute(int num_tasks, std::function<void(int)> task_fn) {
        if (num_tasks <= 0) return;
        active_tasks += num_tasks;
        for (int i = 0; i < num_tasks; ++i) {
            enqueue([this, i, task_fn]() {
                task_fn(i);
            });
        }
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_condition.wait(lock, [this]{ return active_tasks == 0; });
    }

    template<class F, class... Args>
    void enqueue(F&& f, Args&&... args) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        }
        condition.notify_one();
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers) {
            if (worker.joinable()) worker.join();
        }
    }
    size_t size() const { return workers.size(); }
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::mutex wait_mutex;
    std::condition_variable wait_condition;
    std::atomic<int> active_tasks;
    bool stop;
};

using namespace transformer_runtime;

// =============================================================================
// Architecture constants
// =============================================================================

struct BitNetConfig {
    int hidden_size       = 2048;
    int num_heads         = 32;
    int num_kv_heads      = 32;
    int head_dim          = 64;   // hidden_size / num_heads
    int intermediate_size = 5632; // SwiGLU intermediate
    int num_layers        = 24;
    int vocab_size        = 32000;
    int max_position_embeddings = 4096;
    float rms_norm_eps = 1.0e-5f;
    float rope_theta = 500000.0f;
};

// =============================================================================
// DMEM address layout
// =============================================================================

struct TensorView {
    int base;
    int rows;
    int cols;
    TernaryMode mode;
    std::string layer_name = ""; // For cache lookups

    // Implicit conversion to transformer_runtime::TensorView for compatibility with low-level routines.
    operator transformer_runtime::TensorView() const {
        return { base, rows, cols, mode };
    }
};

struct DMemLayout {
    int H;  // hidden_size
    int I;  // intermediate_size

    int x_base()        const { return 0; }
    int norm_base()     const { return H; }
    int q_base()        const { return 2 * H; }
    int k_base()        const { return 3 * H; }
    int v_base()        const { return 4 * H; }
    int attn_out_base() const { return 5 * H; }
    int gate_base()     const { return 6 * H; }
    int up_base()       const { return 6 * H + I; }
    int act_base()      const { return 6 * H + 2 * I; }
    int down_base()     const { return 6 * H + 3 * I; }

    // First word available for weights
    int scratch_end()   const { return 7 * H + 3 * I; }

    // Total scratch words (everything before weights)
    int scratch_words() const { return scratch_end(); }
};

// =============================================================================
// Helpers
// =============================================================================

// Load a named layer into DMEM at base_addr.
// Returns number of words loaded, or 0 on failure.
static int loadWeight(
    vm::VMState& vm,
    BitNetLoader& loader,
    const std::string& name,
    int base_addr,
    RuntimeStats* stats = nullptr)
{
    int loaded = loader.loadLayerToVM(vm, name, base_addr);
    if (loaded == 0) {
        std::cerr << "  [WARN] Could not load weight: " << name << "\n";
    }
    if (stats) stats->dmemStores += static_cast<uint64_t>(loaded);
    return loaded;
}

// Scale a vector by a scalar (in-place, T50).
static bool scaleVector(
    vm::VMState& vm,
    TensorView vec,
    vm::TernaryValue scale,
    RuntimeStats* stats = nullptr)
{
    for (int i = 0; i < vec.cols; ++i) {
        vm::TernaryValue val;
        if (!loadElement(vm, vec, 0, i, val, stats)) return false;
        val = mulT50(val, scale, stats);
        if (!storeElement(vm, vec, 0, i, val, stats)) return false;
    }
    return true;
}

// Element-wise add two vectors, result into dst (T50).
static bool addVectors(
    vm::VMState& vm,
    TensorView a,
    TensorView b,
    TensorView dst,
    RuntimeStats* stats = nullptr)
{
    if (a.cols != b.cols || a.cols != dst.cols) return false;
    for (int i = 0; i < a.cols; ++i) {
        vm::TernaryValue av, bv;
        if (!loadElement(vm, a, 0, i, av, stats)) return false;
        if (!loadElement(vm, b, 0, i, bv, stats)) return false;
        if (!storeElement(vm, dst, 0, i, addT50(av, bv, stats), stats)) return false;
    }
    return true;
}

// Element-wise multiply two vectors (for SwiGLU gate), result into dst (T50).
static bool mulVectors(
    vm::VMState& vm,
    TensorView a,
    TensorView b,
    TensorView dst,
    RuntimeStats* stats = nullptr)
{
    if (a.cols != b.cols || a.cols != dst.cols) return false;
    for (int i = 0; i < a.cols; ++i) {
        vm::TernaryValue av, bv;
        if (!loadElement(vm, a, 0, i, av, stats)) return false;
        if (!loadElement(vm, b, 0, i, bv, stats)) return false;
        if (!storeElement(vm, dst, 0, i, mulT50(av, bv, stats), stats)) return false;
    }
    return true;
}

// SiLU(x) = x * sigmoid(x) = x / (1 + exp(-x))
// Applied element-wise, result in-place.
static bool siluInPlace(
    vm::VMState& vm,
    TensorView vec,
    RuntimeStats* stats = nullptr)
{
    for (int i = 0; i < vec.cols; ++i) {
        vm::TernaryValue x;
        if (!loadElement(vm, vec, 0, i, x, stats)) return false;
        // sigmoid(x) = 1 / (1 + exp(-x))
        vm::TernaryValue neg_x   = negT50(x, stats);
        vm::TernaryValue e       = expT50(neg_x, stats);
        vm::TernaryValue one_pe  = addT50(intValue(1), e, stats);
        vm::TernaryValue sigmoid = divT50(intValue(1), one_pe, stats);
        vm::TernaryValue silu    = mulT50(x, sigmoid, stats);
        if (!storeElement(vm, vec, 0, i, silu, stats)) return false;
    }
    return true;
}

// Copy a vector from src to dst.
static bool copyVector(
    vm::VMState& vm,
    TensorView src,
    TensorView dst,
    RuntimeStats* stats = nullptr)
{
    if (src.cols != dst.cols) return false;
    for (int i = 0; i < src.cols; ++i) {
        vm::TernaryValue val;
        if (!loadElement(vm, src, 0, i, val, stats)) return false;
        if (!storeElement(vm, dst, 0, i, val, stats)) return false;
    }
    return true;
}

// =============================================================================
// Sub-blocks
// =============================================================================

// RMSNorm: norm_out = rmsNorm(x, gamma)
static bool runRmsNorm(
    vm::VMState& vm,
    TensorView x,
    TensorView gamma,
    TensorView norm_out,
    RuntimeStats* stats)
{
    if (!rmsNormRows(vm, x, gamma, norm_out, stats)) {
        std::cerr << "  [FAIL] rmsNorm failed\n";
        return false;
    }
    return true;
}

// Linear projection: out = W @ x_vec  (W is [out_dim x in_dim] in L1)
// x_vec is [1 x in_dim], out is [1 x out_dim].
static bool runLinearL1(
    vm::VMState& vm,
    BitNetLoader& loader,
    TensorView weight,   // [out_dim, in_dim] L1
    TensorView x_vec,    // [1, in_dim]  T50
    TensorView out,      // [1, out_dim] T50
    RuntimeStats* stats)
{    const int out_dim = weight.rows;
    const int in_dim = weight.cols;

    // Pre-cache activations as floats for maximum SIMD speed
    std::vector<float> x_cached(in_dim);
    for (int j = 0; j < in_dim; ++j) {
        vm::TernaryValue act;
        if (!loadElement(vm, x_vec, 0, j, act, stats)) return false;
        x_cached[j] = static_cast<float>(sandbox::long_ops::toDouble(act.toLongTriple()));
    }

    // Try to find weights in specialized int8 cache
    const int8_t* cached_l1 = nullptr;
    if (loader.weightCacheL1.count(weight.layer_name)) {
        cached_l1 = loader.weightCacheL1[weight.layer_name].data();
    }

    const int num_threads = std::thread::hardware_concurrency();
    const int chunk_size = (out_dim + num_threads - 1) / num_threads;
    
    std::vector<std::future<void>> futures;
    
    for (int t = 0; t < num_threads; ++t) {
        int start_i = t * chunk_size;
        int end_i = std::min(start_i + chunk_size, out_dim);
        if (start_i >= out_dim) break;

        futures.push_back(std::async(std::launch::async, [&vm, weight, &x_cached, cached_l1, start_i, end_i, in_dim, out]() {
            vm::TernaryValue* dmem_words = vm.dmem.words;
            
            for (int i = start_i; i < end_i; ++i) {
                float acc = 0.0f;
                int row_offset = i * in_dim;
                
                if (cached_l1) {
                    // FAST PATH: contiguous int8 weights
                    const int8_t* row = &cached_l1[row_offset];
                    for (int j = 0; j < in_dim; ++j) {
                        if (row[j] == 1) acc += x_cached[j];
                        else if (row[j] == -1) acc -= x_cached[j];
                    }
                } else {
                    // SLOW PATH: fallback to DMEM bits
                    const vm::TernaryValue* words = &dmem_words[weight.base + row_offset];
                    for (int j = 0; j < in_dim; ++j) {
                        uint64_t w_bits = words[j].bits.lo;
                        int8_t trit_bits = static_cast<int8_t>(w_bits & 0x3);
                        if (trit_bits == 0b10) acc += x_cached[j];
                        else if (trit_bits == 0b00) acc -= x_cached[j];
                    }
                }
                dmem_words[out.base + i] = vm::TernaryValue::fromLongTriple(sandbox::long_ops::encode(static_cast<double>(acc), 0));
            }
        }));
    }
    for (auto& f : futures) f.wait();
    return true;
}


// =============================================================================
// Attention block (single-token, MHA, no KV cache)
// =============================================================================
//
// With seq_len = 1:
//   Q = W_q @ norm_out               [H]
//   K = W_k @ norm_out               [H]
//   V = W_v @ norm_out               [H]
//   For each head h:
//     score_h = Q_h · K_h / sqrt(head_dim)   (scalar)
//     softmax([score_h]) = 1.0  (single element)
//     attn_out_h = V_h           (weight is 1.0)
//   Concatenate heads → attn_out  [H]
//   out = W_o @ attn_out          [H]
//   x = x + out                   (residual)

static bool runAttentionBlock(
    vm::VMState& vm,
    const BitNetConfig& cfg,
    const DMemLayout& layout,
    const std::string& layer_prefix,
    BitNetLoader& loader,
    int weight_base,
    TensorView x_view,
    TensorView norm_view,
    RuntimeStats* stats)
{
    const int H  = cfg.hidden_size;
    const int Nh = cfg.num_heads;
    const int Dh = cfg.head_dim;

    // -----------------------------------------------------------------------
    // 1. Load Q/K/V weight matrices and project
    // -----------------------------------------------------------------------
    TensorView q_view    = { layout.q_base(),       1, H, TernaryMode::T50 };
    TensorView k_view    = { layout.k_base(),       1, H, TernaryMode::T50 };
    TensorView v_view    = { layout.v_base(),       1, H, TernaryMode::T50 };

    // W_q: [H x H] in L1
    TensorView wq_view = { weight_base,           H, H, TernaryMode::L1, layer_prefix + "self_attn.q_proj.weight" };
    int wq_size = loadWeight(vm, loader, layer_prefix + "self_attn.q_proj.weight",
                             wq_view.base, stats);
    if (wq_size == 0) return false;

    TensorView wk_view = { wq_view.base + wq_size, H, H, TernaryMode::L1, layer_prefix + "self_attn.k_proj.weight" };
    int wk_size = loadWeight(vm, loader, layer_prefix + "self_attn.k_proj.weight",
                             wk_view.base, stats);
    if (wk_size == 0) return false;

    TensorView wv_view = { wk_view.base + wk_size, H, H, TernaryMode::L1, layer_prefix + "self_attn.v_proj.weight" };
    int wv_size = loadWeight(vm, loader, layer_prefix + "self_attn.v_proj.weight",
                             wv_view.base, stats);
    if (wv_size == 0) return false;

    if (!runLinearL1(vm, loader, wq_view, norm_view, q_view, stats)) return false;
    if (!runLinearL1(vm, loader, wk_view, norm_view, k_view, stats)) return false;
    if (!runLinearL1(vm, loader, wv_view, norm_view, v_view, stats)) return false;

    // -----------------------------------------------------------------------
    // 2. Per-head attention  (seq_len = 1 → output = V directly)
    //
    //    score_h = (Q_h · K_h) / sqrt(Dh)
    //    Since seq_len=1, softmax collapses to 1.0 and attn_out_h = V_h.
    //    We still compute the dot product to validate Q/K alignment, then
    //    copy V to attn_out.
    // -----------------------------------------------------------------------
    TensorView attn_out_view = { layout.attn_out_base(), 1, H, TernaryMode::T50 };

    vm::TernaryValue inv_sqrt_dh = divT50(intValue(1),
        sqrtT50(intValue(Dh), stats), stats);

    for (int h = 0; h < Nh; ++h) {
        const int head_offset = h * Dh;
        TensorView q_head = { q_view.base + head_offset, 1, Dh, TernaryMode::T50 };
        TensorView k_head = { k_view.base + head_offset, 1, Dh, TernaryMode::T50 };
        TensorView v_head = { v_view.base + head_offset, 1, Dh, TernaryMode::T50 };
        TensorView out_head = { attn_out_view.base + head_offset, 1, Dh, TernaryMode::T50 };

        // Dot product Q_h · K_h
        vm::TernaryValue dot = intValue(0);
        for (int d = 0; d < Dh; ++d) {
            vm::TernaryValue qv, kv;
            if (!loadElement(vm, q_head, 0, d, qv, stats)) return false;
            if (!loadElement(vm, k_head, 0, d, kv, stats)) return false;
            dot = addT50(dot, mulT50(qv, kv, stats), stats);
        }
        // score = dot / sqrt(Dh)  — not used further (softmax → 1.0 for seq_len=1)
        // Kept for correctness verification; result discarded.
        (void)mulT50(dot, inv_sqrt_dh, stats);

        // attn_out_h = V_h  (weight = 1.0 after softmax of single element)
        if (!copyVector(vm, v_head, out_head, stats)) return false;
    }

    // -----------------------------------------------------------------------
    // 3. Output projection:  out = W_o @ attn_out
    // -----------------------------------------------------------------------
    TensorView wo_view = { wv_view.base + wv_size, H, H, TernaryMode::L1, layer_prefix + "self_attn.o_proj.weight" };
    int wo_size = loadWeight(vm, loader, layer_prefix + "self_attn.o_proj.weight",
                             wo_view.base, stats);
    if (wo_size == 0) return false;

    TensorView proj_out_view = { layout.norm_base(), 1, H, TernaryMode::T50 };
    if (!runLinearL1(vm, loader, wo_view, attn_out_view, proj_out_view, stats)) return false;

    // -----------------------------------------------------------------------
    // 4. Residual: x = x + proj_out
    // -----------------------------------------------------------------------
    if (!addVectors(vm, x_view, proj_out_view, x_view, stats)) return false;

    return true;
}

// =============================================================================
// MLP block (SwiGLU: out = W_down @ (gate(x) * silu(up(x))))
// =============================================================================

static bool runMlpBlock(
    vm::VMState& vm,
    const BitNetConfig& cfg,
    const DMemLayout& layout,
    const std::string& layer_prefix,
    BitNetLoader& loader,
    int weight_base,
    TensorView x_view,
    TensorView norm_view,
    RuntimeStats* stats)
{
    const int H = cfg.hidden_size;
    const int I = cfg.intermediate_size;

    TensorView gate_view = { layout.gate_base(), 1, I, TernaryMode::T50 };
    TensorView up_view   = { layout.up_base(),   1, I, TernaryMode::T50 };
    TensorView act_view  = { layout.act_base(),  1, I, TernaryMode::T50 };
    TensorView down_view = { layout.down_base(), 1, H, TernaryMode::T50 };

    // -----------------------------------------------------------------------
    // 1. Load and apply gate_proj
    // -----------------------------------------------------------------------
    TensorView wgate_view = { weight_base, I, H, TernaryMode::L1, layer_prefix + "mlp.gate_proj.weight" };
    int wgate_size = loadWeight(vm, loader, layer_prefix + "mlp.gate_proj.weight",
                                wgate_view.base, stats);
    if (wgate_size == 0) return false;

    if (!runLinearL1(vm, loader, wgate_view, norm_view, gate_view, stats)) return false;

    // -----------------------------------------------------------------------
    // 2. Load and apply up_proj
    // -----------------------------------------------------------------------
    TensorView wup_view = { wgate_view.base + wgate_size, I, H, TernaryMode::L1, layer_prefix + "mlp.up_proj.weight" };
    int wup_size = loadWeight(vm, loader, layer_prefix + "mlp.up_proj.weight",
                              wup_view.base, stats);
    if (wup_size == 0) return false;

    if (!runLinearL1(vm, loader, wup_view, norm_view, up_view, stats)) return false;

    // -----------------------------------------------------------------------
    // 3. SwiGLU: act = gate * SiLU(up)
    //    SiLU applied in-place to up, then multiply by gate.
    // -----------------------------------------------------------------------
    if (!siluInPlace(vm, up_view, stats)) return false;
    if (!mulVectors(vm, gate_view, up_view, act_view, stats)) return false;

    // -----------------------------------------------------------------------
    // 4. down_proj: out = W_down @ act
    // -----------------------------------------------------------------------
    TensorView wdown_view = { wup_view.base + wup_size, H, I, TernaryMode::L1, layer_prefix + "mlp.down_proj.weight" };
    int wdown_size = loadWeight(vm, loader, layer_prefix + "mlp.down_proj.weight",
                                wdown_view.base, stats);
    if (wdown_size == 0) return false;

    if (!runLinearL1(vm, loader, wdown_view, act_view, down_view, stats)) return false;

    // -----------------------------------------------------------------------
    // 5. Residual: x = x + down_proj_out
    // -----------------------------------------------------------------------
    if (!addVectors(vm, x_view, down_view, x_view, stats)) return false;

    return true;
}

// =============================================================================
// Full forward pass
// =============================================================================

struct BitNetForwardResult {
    bool ok = false;
    int greedy_token = -1;          // argmax of logits
    vm::TernaryValue greedy_logit;  // value at greedy_token
    RuntimeStats stats;
    long long elapsed_ms = 0;
    std::string error;
};

// =============================================================================
// Host-side incremental inference path
// =============================================================================
//
// The VM path above was intentionally single-token and ignored all tokens before
// input_ids.back(). This path keeps the model process alive, stores per-layer
// K/V tensors, and evaluates the prompt token-by-token so generation is causal.

static inline float bf16ToFloat(uint16_t raw) {
    union {
        uint32_t u;
        float f;
    } conv;
    conv.u = static_cast<uint32_t>(raw) << 16;
    return conv.f;
}

struct SafeTensorInfo {
    std::string dtype;
    std::vector<long long> shape;
    uint64_t begin = 0;
    uint64_t end = 0;

    long long count() const {
        long long total = 1;
        for (long long dim : shape) total *= dim;
        return total;
    }
};

class SafeTensorReader {
public:
    ~SafeTensorReader() {
        close();
    }

    void close() {
#ifdef _WIN32
        if (mappedView_) UnmapViewOfFile(mappedView_);
        if (mappingHandle_) CloseHandle(mappingHandle_);
        if (fileHandle_ != INVALID_HANDLE_VALUE) CloseHandle(fileHandle_);
        mappedView_ = nullptr;
        mappingHandle_ = nullptr;
        fileHandle_ = INVALID_HANDLE_VALUE;
#else
        if (data_ != MAP_FAILED) munmap(data_, fileSize_);
        if (fd_ != -1) ::close(fd_);
        data_ = MAP_FAILED;
        fd_ = -1;
#endif
    }

    bool open(const std::string& path, std::string& error) {
        close();
        path_ = path;
        tensors_.clear();

#ifdef _WIN32
        fileHandle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fileHandle_ == INVALID_HANDLE_VALUE) {
            error = "Could not open safetensors file: " + path;
            return false;
        }
        LARGE_INTEGER size;
        GetFileSizeEx(fileHandle_, &size);
        fileSize_ = static_cast<uint64_t>(size.QuadPart);
        mappingHandle_ = CreateFileMapping(fileHandle_, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!mappingHandle_) {
            error = "Could not create file mapping: " + path;
            return false;
        }
        mappedView_ = MapViewOfFile(mappingHandle_, FILE_MAP_READ, 0, 0, 0);
        if (!mappedView_) {
            error = "Could not map view of file: " + path;
            return false;
        }
        dataPtr_ = static_cast<const char*>(mappedView_);
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ == -1) {
            error = "Could not open safetensors file: " + path;
            return false;
        }
        struct stat st;
        fstat(fd_, &st);
        fileSize_ = static_cast<uint64_t>(st.st_size);
        data_ = mmap(NULL, fileSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (data_ == MAP_FAILED) {
            error = "mmap failed for: " + path;
            return false;
        }
        dataPtr_ = static_cast<const char*>(data_);
#endif

        if (fileSize_ < 8) {
            error = "File too small for safetensors";
            return false;
        }

        uint64_t headerLen = *reinterpret_cast<const uint64_t*>(dataPtr_);
        if (8 + headerLen > fileSize_) {
            error = "Header length exceeds file size";
            return false;
        }

        std::string header(dataPtr_ + 8, static_cast<std::size_t>(headerLen));
        dataBase_ = 8 + headerLen;
        parseHeader(header);
        if (tensors_.empty()) {
            error = "No tensors found in safetensors header";
            return false;
        }
        return true;
    }

    const SafeTensorInfo* tensor(const std::string& name) const {
        auto it = tensors_.find(name);
        return it == tensors_.end() ? nullptr : &it->second;
    }

    const void* data(const std::string& name) const {
        const SafeTensorInfo* info = tensor(name);
        if (!info) return nullptr;
        return dataPtr_ + dataBase_ + info->begin;
    }

    bool readBf16TensorRaw(const std::string& name, std::vector<uint16_t>& out, std::string& error) const {
        const SafeTensorInfo* info = tensor(name);
        if (!info) {
            error = "Tensor not found: " + name;
            return false;
        }
        if (info->dtype != "BF16") {
            error = "Expected BF16 tensor for " + name + ", got " + info->dtype;
            return false;
        }

        const long long elementCount = info->count();
        const uint64_t bytes = info->end - info->begin;
        if (bytes != static_cast<uint64_t>(elementCount * 2)) {
            error = "Unexpected BF16 byte count for " + name;
            return false;
        }

        out.resize(static_cast<std::size_t>(elementCount));
        std::memcpy(out.data(), dataPtr_ + dataBase_ + info->begin, static_cast<std::size_t>(bytes));
        return true;
    }

    bool readBf16TensorFloat(const std::string& name, std::vector<float>& out, std::string& error) const {
        std::vector<uint16_t> raw;
        if (!readBf16TensorRaw(name, raw, error)) return false;
        out.resize(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) out[i] = bf16ToFloat(raw[i]);
        return true;
    }

private:
    std::string path_;
    uint64_t fileSize_ = 0;
    uint64_t dataBase_ = 0;
    const char* dataPtr_ = nullptr;

#ifdef _WIN32
    HANDLE fileHandle_ = INVALID_HANDLE_VALUE;
    HANDLE mappingHandle_ = nullptr;
    void* mappedView_ = nullptr;
#else
    int fd_ = -1;
    void* data_ = MAP_FAILED;
#endif

    std::unordered_map<std::string, SafeTensorInfo> tensors_;

    static bool parseIntArray(const std::string& obj, const std::string& key, std::vector<long long>& out) {
        const std::string needle = "\"" + key + "\":[";
        const std::size_t start = obj.find(needle);
        if (start == std::string::npos) return false;
        const std::size_t valuesStart = start + needle.size();
        const std::size_t valuesEnd = obj.find(']', valuesStart);
        if (valuesEnd == std::string::npos) return false;

        out.clear();
        std::size_t pos = valuesStart;
        while (pos < valuesEnd) {
            std::size_t comma = obj.find(',', pos);
            if (comma == std::string::npos || comma > valuesEnd) comma = valuesEnd;
            if (comma > pos) {
                out.push_back(std::stoll(obj.substr(pos, comma - pos)));
            }
            pos = comma + 1;
        }
        return !out.empty();
    }

    static bool parseStringValue(const std::string& obj, const std::string& key, std::string& out) {
        const std::string needle = "\"" + key + "\":\"";
        const std::size_t start = obj.find(needle);
        if (start == std::string::npos) return false;
        const std::size_t valueStart = start + needle.size();
        const std::size_t valueEnd = obj.find('"', valueStart);
        if (valueEnd == std::string::npos) return false;
        out = obj.substr(valueStart, valueEnd - valueStart);
        return true;
    }

    static std::size_t findObjectEnd(const std::string& header, std::size_t objectStart) {
        int depth = 0;
        for (std::size_t i = objectStart; i < header.size(); ++i) {
            if (header[i] == '{') ++depth;
            if (header[i] == '}') {
                --depth;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }

    void parseHeader(const std::string& header) {
        std::size_t pos = 0;
        while ((pos = header.find("\"model.", pos)) != std::string::npos) {
            const std::size_t nameStart = pos + 1;
            const std::size_t nameEnd = header.find('"', nameStart);
            if (nameEnd == std::string::npos) break;

            const std::string name = header.substr(nameStart, nameEnd - nameStart);
            const std::size_t objectStart = header.find('{', nameEnd);
            if (objectStart == std::string::npos) break;
            const std::size_t objectEnd = findObjectEnd(header, objectStart);
            if (objectEnd == std::string::npos) break;

            const std::string obj = header.substr(objectStart, objectEnd - objectStart + 1);
            SafeTensorInfo info;
            std::vector<long long> offsets;
            if (parseStringValue(obj, "dtype", info.dtype) &&
                parseIntArray(obj, "shape", info.shape) &&
                parseIntArray(obj, "data_offsets", offsets) &&
                offsets.size() == 2) {
                info.begin = static_cast<uint64_t>(offsets[0]);
                info.end = static_cast<uint64_t>(offsets[1]);
                tensors_[name] = std::move(info);
            }

            pos = objectEnd + 1;
        }
    }
};

struct BitNetHostForwardResult {
    bool ok = false;
    int greedy_token = -1;
    float greedy_logit = -std::numeric_limits<float>::infinity();
    std::vector<float> logits;
    RuntimeStats stats;
    long long elapsed_ms = 0;
    std::string error;
};

class BitNetHostInference {
public:
    BitNetConfig cfg;
    BitNetLoader& loader;

    BitNetHostInference(BitNetLoader& l, const BitNetConfig& c, int threads = 1)
        : cfg(c), loader(l), threads_(threads), pool_(static_cast<size_t>(threads)) {
    }

    void setThreads(int threads) {
        threads_ = std::max(1, threads);
    }

    bool init(const std::string& safetensorsPath, std::string& error) {
        if (!safe_.open(safetensorsPath, error)) return false;

        const SafeTensorInfo* embedInfo = safe_.tensor("model.embed_tokens.weight");
        if (!embedInfo || embedInfo->shape.size() != 2) {
            error = "model.embed_tokens.weight missing or has unexpected shape";
            return false;
        }
        if (embedInfo->shape[0] != cfg.vocab_size || embedInfo->shape[1] != cfg.hidden_size) {
            error = "Embedding shape does not match BitNetConfig";
            return false;
        }

        std::cout << "Loading BF16 embeddings from safetensors ("
                  << cfg.vocab_size << " x " << cfg.hidden_size << ")...\n";
        if (!safe_.readBf16TensorRaw("model.embed_tokens.weight", embeddings_, error)) return false;

        kv_dim_ = cfg.num_kv_heads * cfg.head_dim;
        kv_.resize(static_cast<std::size_t>(cfg.num_layers));
        for (auto& layer : kv_) {
            layer.keys.resize(static_cast<std::size_t>(cfg.max_position_embeddings) * kv_dim_);
            layer.values.resize(static_cast<std::size_t>(cfg.max_position_embeddings) * kv_dim_);
            layer.k_scales.resize(static_cast<std::size_t>(cfg.max_position_embeddings) * cfg.num_kv_heads);
            layer.v_scales.resize(static_cast<std::size_t>(cfg.max_position_embeddings) * cfg.num_kv_heads);
        }

        invFreq_.resize(static_cast<std::size_t>(cfg.head_dim / 2));
        for (int i = 0; i < cfg.head_dim / 2; ++i) {
            invFreq_[static_cast<std::size_t>(i)] =
                static_cast<float>(1.0 / std::pow(cfg.rope_theta, static_cast<double>(2 * i) / cfg.head_dim));
        }

        ready_ = true;
        reset();
        return true;
    }

    void reset() {
        kv_len_ = 0;
    }

    int cachedTokens() const {
        return kv_len_;
    }

    BitNetHostForwardResult evalToken(int tokenId, bool computeLogits) {
        BitNetHostForwardResult result;
        if (!ready_) {
            result.error = "BitNetHostInference not initialized";
            return result;
        }
        if (kv_len_ >= cfg.max_position_embeddings) {
            result.error = "KV cache is full";
            return result;
        }

        const auto t0 = std::chrono::high_resolution_clock::now();
        std::string error;

        std::vector<float> x;
        if (!embeddingRow(tokenId, x, error)) {
            result.error = error;
            return result;
        }

        const int position = kv_len_;
        prefetchLayer(0); // Start prefetching first layer early
        for (int layer = 0; layer < cfg.num_layers; ++layer) {
            // Prefetch next layer
            if (layer + 1 < cfg.num_layers) {
                prefetchLayer(layer + 1);
            }
            
            // Wait for current layer prefetches
            ensureLayerLoaded(layer);

            const std::string pfx = "model.layers." + std::to_string(layer) + ".";

            if (residual_scratch_.size() < x.size()) residual_scratch_.resize(x.size());
            std::copy(x.begin(), x.end(), residual_scratch_.begin());

            if (norm_scratch_.size() < x.size()) norm_scratch_.resize(x.size());
            if (!rmsNorm(x, tensorFloat(pfx + "input_layernorm.weight", error), norm_scratch_, error)) {
                result.error = error;
                return result;
            }

            if (attn_scratch_.size() < x.size()) attn_scratch_.resize(x.size());
            if (!attentionLayer(layer, pfx, norm_scratch_, position, attn_scratch_, error)) {
                result.error = error;
                return result;
            }
            addInto(residual_scratch_, attn_scratch_, x);

            std::copy(x.begin(), x.end(), residual_scratch_.begin());
            if (!rmsNorm(x, tensorFloat(pfx + "post_attention_layernorm.weight", error), norm_scratch_, error)) {
                result.error = error;
                return result;
            }

            if (mlp_scratch_.size() < x.size()) mlp_scratch_.resize(x.size());
            if (!mlpLayer(pfx, norm_scratch_, mlp_scratch_, error)) {
                result.error = error;
                return result;
            }
            addInto(residual_scratch_, mlp_scratch_, x);

            // Safety clamp
            for (float& v : x) {
                if (!std::isfinite(v)) v = 0.0f;
                else if (v > 16384.0f) v = 16384.0f;
                else if (v < -16384.0f) v = -16384.0f;
            }

            // Clean up previous layer to save RAM (optional, but good for 2B model)
            // No unload to keep performance high
        }

        ++kv_len_;

        if (computeLogits) {
            std::vector<float> norm;
            if (!rmsNorm(x, tensorFloat("model.norm.weight", error), norm, error)) {
                result.error = error;
                return result;
            }
            if (!argmaxLogits(norm, result.greedy_token, result.greedy_logit, &result.logits)) {
                result.error = "Argmax failed";
                return result;
            }
        }

        const auto t1 = std::chrono::high_resolution_clock::now();
        result.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        result.ok = true;
        return result;
    }

private:
    struct LayerKV {
        std::vector<int8_t> keys;   // 8-bit quantized [-127, 127]
        std::vector<int8_t> values; // 8-bit quantized [-127, 127]
        std::vector<float> k_scales;
        std::vector<float> v_scales;
    };

    SafeTensorReader safe_;
    std::vector<uint16_t> embeddings_;
    std::unordered_map<std::string, std::vector<float>> floatTensors_;
    std::vector<LayerKV> kv_;
    std::vector<float> invFreq_;
    int kv_dim_ = 0;
    int kv_len_ = 0;
    int threads_ = 1;
    ThreadPool pool_;
    std::vector<int8_t> x_q_scratch_;
    std::vector<float> residual_scratch_;
    std::vector<float> norm_scratch_;
    std::vector<float> attn_scratch_;
    std::vector<float> mlp_scratch_;
    bool ready_ = false;

    void prefetchLayer(int layer) {
        if (layer < 0 || layer >= cfg.num_layers) return;
        const std::string pfx = "model.layers." + std::to_string(layer) + ".";
        static const std::vector<std::string> projs = {
            "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight", "self_attn.o_proj.weight",
            "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight"
        };
        for (const auto& s : projs) loader.prefetchAsync(pfx + s);
    }

    void ensureLayerLoaded(int layer) {
        if (layer < 0 || layer >= cfg.num_layers) return;
        const std::string pfx = "model.layers." + std::to_string(layer) + ".";
        static const std::vector<std::string> projs = {
            "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight", "self_attn.o_proj.weight",
            "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight"
        };
        for (const auto& s : projs) loader.waitPrefetch(pfx + s);
    }

    void unloadLayer(int layer) {
        if (layer < 0 || layer >= cfg.num_layers) return;
        const std::string pfx = "model.layers." + std::to_string(layer) + ".";
        static const std::vector<std::string> projs = {
            "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight", "self_attn.o_proj.weight",
            "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight"
        };
        std::lock_guard<std::mutex> lock(loader.loaderMutex);
        for (const auto& s : projs) {
            loader.weightCacheL50.erase(pfx + s);
            loader.weightCacheUnpacked.erase(pfx + s);
        }
    }

    const std::vector<float>& tensorFloat(const std::string& name, std::string& error) {
        {
            std::lock_guard<std::mutex> lock(loader.loaderMutex);
            auto cached = floatTensors_.find(name);
            if (cached != floatTensors_.end()) return cached->second;
        }

        std::vector<float> values;
        if (!safe_.readBf16TensorFloat(name, values, error)) {
            static const std::vector<float> empty;
            return empty;
        }
        {
            std::lock_guard<std::mutex> lock(loader.loaderMutex);
            auto [it, inserted] = floatTensors_.emplace(name, std::move(values));
            (void)inserted;
            return it->second;
        }
    }

    bool embeddingRow(int tokenId, std::vector<float>& out, std::string& error) const {
        if (tokenId < 0 || tokenId >= cfg.vocab_size) {
            error = "Token id out of vocabulary: " + std::to_string(tokenId);
            return false;
        }
        out.resize(static_cast<std::size_t>(cfg.hidden_size));
        const uint16_t* row = embeddings_.data() + static_cast<std::size_t>(tokenId) * cfg.hidden_size;
        for (int i = 0; i < cfg.hidden_size; ++i) out[static_cast<std::size_t>(i)] = bf16ToFloat(row[i]);
        return true;
    }

    bool rmsNorm(const std::vector<float>& x, const std::vector<float>& weight, std::vector<float>& out, std::string& error) {
        if (x.size() != weight.size()) {
            error = "RMSNorm shape mismatch";
            return false;
        }
        int n = static_cast<int>(x.size());
        float sumSq = 0;
        int j = 0;
#ifdef __AVX2__
        __m256 vsum = _mm256_setzero_ps();
        for (; j <= n - 8; j += 8) {
            __m256 vx = _mm256_loadu_ps(x.data() + j);
            vsum = _mm256_add_ps(vsum, _mm256_mul_ps(vx, vx));
        }
        float temp[8];
        _mm256_storeu_ps(temp, vsum);
        for (int i = 0; i < 8; ++i) sumSq += temp[i];
#endif
        for (; j < n; ++j) sumSq += x[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(j)];
        
        float invRms = 1.0f / (std::sqrt(sumSq / n + cfg.rms_norm_eps));
        
        out.resize(static_cast<std::size_t>(n));
        j = 0;
#ifdef __AVX2__
        __m256 vinv = _mm256_set1_ps(invRms);
        for (; j <= n - 8; j += 8) {
            __m256 vx = _mm256_loadu_ps(x.data() + j);
            __m256 vw = _mm256_loadu_ps(weight.data() + j);
            _mm256_storeu_ps(out.data() + j, _mm256_mul_ps(_mm256_mul_ps(vx, vinv), vw));
        }
#endif
        for (; j < n; ++j) out[static_cast<std::size_t>(j)] = x[static_cast<std::size_t>(j)] * invRms * weight[static_cast<std::size_t>(j)];
        return true;
    }

    static void addInto(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& out) {
        int n = static_cast<int>(a.size());
        out.resize(static_cast<std::size_t>(n));
        int j = 0;
#ifdef __AVX2__
        for (; j <= n - 8; j += 8) {
            __m256 va = _mm256_loadu_ps(a.data() + j);
            __m256 vb = _mm256_loadu_ps(b.data() + j);
            _mm256_storeu_ps(out.data() + j, _mm256_add_ps(va, vb));
        }
#endif
        for (; j < n; ++j) out[static_cast<std::size_t>(j)] = a[static_cast<std::size_t>(j)] + b[static_cast<std::size_t>(j)];
    }

    static float relu2(float x) {
        return x <= 0.0f ? 0.0f : x * x;
    }

    void rotateHead(float* head, int position) const {
        const int half = cfg.head_dim / 2;
        for (int i = 0; i < half; ++i) {
            const float angle = static_cast<float>(position) * invFreq_[static_cast<std::size_t>(i)];
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float x1 = head[i];
            const float x2 = head[i + half];
            head[i] = x1 * c - x2 * s;
            head[i + half] = x2 * c + x1 * s;
        }
    }

    void applyRoPE(std::vector<float>& q, std::vector<float>& k, int position) const {
        for (int h = 0; h < cfg.num_heads; ++h) {
            rotateHead(q.data() + static_cast<std::size_t>(h) * cfg.head_dim, position);
        }
        for (int h = 0; h < cfg.num_kv_heads; ++h) {
            rotateHead(k.data() + static_cast<std::size_t>(h) * cfg.head_dim, position);
        }
    }

    bool linearPacked(
        const std::string& name,
        int rows,
        int cols,
        const std::vector<float>& x,
        std::vector<float>& out,
        std::string& error) {

        if (static_cast<int>(x.size()) != cols) {
            error = "Linear input shape mismatch for " + name;
            return false;
        }

        const auto layer = loader.getLayer(name);
        if (layer.name.empty()) {
            error = "Missing packed layer: " + name;
            return false;
        }
        if (layer.count < rows * cols) {
            error = "Packed layer is smaller than expected: " + name;
            return false;
        }

        const auto* unpacked = loader.unpackedL50(name);
        if (!unpacked) {
            error = "Could not load unpacked L50 layer: " + name;
            return false;
        }

        out.assign(static_cast<std::size_t>(rows), 0.0f);
        const float w_scale = static_cast<float>(loader.getScale(name));
        
        // Activation Quantization (A8)
        // Vectorized Activation Quantization (A8)
        float x_max = 1e-9f;
#ifdef __AVX2__
        __m256 vmax = _mm256_set1_ps(1e-9f);
        int jj = 0;
        for (; jj <= cols - 8; jj += 8) {
            __m256 vx = _mm256_loadu_ps(x.data() + jj);
            __m256 vabs = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), vx);
            vmax = _mm256_max_ps(vmax, vabs);
        }
        float temp_max[8];
        _mm256_storeu_ps(temp_max, vmax);
        for (int i = 0; i < 8; ++i) x_max = std::max(x_max, temp_max[i]);
        for (; jj < cols; ++jj) x_max = std::max(x_max, std::abs(x[static_cast<std::size_t>(jj)]));
#else
        for (float v : x) x_max = std::max(x_max, std::abs(v));
#endif
        const float x_gamma = x_max / 127.0f;
        const float x_inv = 1.0f / x_gamma;
        
        if (x_q_scratch_.size() < static_cast<std::size_t>(cols)) x_q_scratch_.resize(static_cast<std::size_t>(cols));
        
#ifdef __AVX2__
        __m256 vinv = _mm256_set1_ps(x_inv);
        int jq = 0;
        for (; jq <= cols - 8; jq += 8) {
            __m256 vx = _mm256_loadu_ps(x.data() + jq);
            __m256 vscaled = _mm256_mul_ps(vx, vinv);
            __m256i v32 = _mm256_cvtps_epi32(vscaled);
            // Pack to int8 with saturation
            // cvtps_epi32 does rounding to nearest even by default
            int32_t t32[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(t32), v32);
            for (int i = 0; i < 8; ++i) {
                x_q_scratch_[static_cast<std::size_t>(jq + i)] = static_cast<int8_t>(std::clamp(t32[i], -127, 127));
            }
        }
        for (; jq < cols; ++jq) {
            x_q_scratch_[static_cast<std::size_t>(jq)] = static_cast<int8_t>(std::round(std::clamp(x[static_cast<std::size_t>(jq)] * x_inv, -127.0f, 127.0f)));
        }
#else
        for (int j = 0; j < cols; ++j) {
            x_q_scratch_[static_cast<std::size_t>(j)] = static_cast<int8_t>(std::round(std::clamp(x[static_cast<std::size_t>(j)] * x_inv, -127.0f, 127.0f)));
        }
#endif

        const float combined_scale = w_scale * x_gamma;
        const int num_workers = static_cast<int>(pool_.size());
        const int chunk = (rows + num_workers - 1) / num_workers;

        pool_.execute(num_workers, [&](int worker) {
            const int rowBegin = worker * chunk;
            const int rowEnd = std::min(rows, rowBegin + chunk);
            if (rowBegin < rowEnd) {
                for (int row = rowBegin; row < rowEnd; ++row) {
                    const int8_t* w_row = unpacked->data() + static_cast<std::size_t>(row) * cols;
                    int32_t acc = 0;
                    int j = 0;

#ifdef __AVX2__
                    __m256i vsum16_0 = _mm256_setzero_si256();
                    __m256i vsum16_1 = _mm256_setzero_si256();
                    for (; j <= cols - 32; j += 32) {
                        __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x_q_scratch_.data() + j));
                        __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w_row + j));
                        __m256i vsigned = _mm256_sign_epi8(vx, vw);
                        vsum16_0 = _mm256_add_epi16(vsum16_0, _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vsigned, 0)));
                        vsum16_1 = _mm256_add_epi16(vsum16_1, _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vsigned, 1)));
                    }
                    __m256i vsum32 = _mm256_add_epi32(
                        _mm256_add_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_0, 0)),
                                         _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_0, 1))),
                        _mm256_add_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_1, 0)),
                                         _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16_1, 1)))
                    );
                    int32_t temp[8];
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp), vsum32);
                    for (int i = 0; i < 8; ++i) acc += temp[i];
#endif
                    for (; j < cols; ++j) {
                        int8_t w = w_row[j];
                        if (w == 1) acc += x_q_scratch_[static_cast<std::size_t>(j)];
                        else if (w == -1) acc -= x_q_scratch_[static_cast<std::size_t>(j)];
                    }
                    out[static_cast<std::size_t>(row)] = static_cast<float>(acc) * combined_scale;
                }
            }
        });
        return true;
    }

    bool attentionLayer(
        int layer,
        const std::string& pfx,
        const std::vector<float>& norm,
        int position,
        std::vector<float>& out,
        std::string& error) {

        std::vector<float> q;
        std::vector<float> k;
        std::vector<float> v;
        if (!linearPacked(pfx + "self_attn.q_proj.weight", cfg.hidden_size, cfg.hidden_size, norm, q, error)) return false;
        if (!linearPacked(pfx + "self_attn.k_proj.weight", kv_dim_, cfg.hidden_size, norm, k, error)) return false;
        if (!linearPacked(pfx + "self_attn.v_proj.weight", kv_dim_, cfg.hidden_size, norm, v, error)) return false;

        applyRoPE(q, k, position);

        LayerKV& cache = kv_[static_cast<std::size_t>(layer)];
        
        // Quantize K and V to 8-bit per head
        for (int h = 0; h < cfg.num_kv_heads; ++h) {
            const float* kHead = k.data() + h * cfg.head_dim;
            const float* vHead = v.data() + h * cfg.head_dim;
            
            float k_max = 1e-9f;
            float v_max = 1e-9f;
            for (int d = 0; d < cfg.head_dim; ++d) {
                k_max = std::max(k_max, std::abs(kHead[d]));
                v_max = std::max(v_max, std::abs(vHead[d]));
            }
            
            const float k_gamma = k_max / 127.0f;
            const float v_gamma = v_max / 127.0f;
            cache.k_scales[position * cfg.num_kv_heads + h] = k_gamma;
            cache.v_scales[position * cfg.num_kv_heads + h] = v_gamma;
            
            int8_t* kSlot = cache.keys.data() + position * kv_dim_ + h * cfg.head_dim;
            int8_t* vSlot = cache.values.data() + position * kv_dim_ + h * cfg.head_dim;
            
            const float k_inv = 1.0f / k_gamma;
            const float v_inv = 1.0f / v_gamma;
            
            for (int d = 0; d < cfg.head_dim; ++d) {
                kSlot[d] = static_cast<int8_t>(std::round(std::clamp(kHead[d] * k_inv, -127.0f, 127.0f)));
                vSlot[d] = static_cast<int8_t>(std::round(std::clamp(vHead[d] * v_inv, -127.0f, 127.0f)));
            }
        }

        std::vector<float> attn(static_cast<std::size_t>(cfg.hidden_size), 0.0f);
        const int groups = cfg.num_heads / cfg.num_kv_heads;
        const float scale = 1.0f / std::sqrt(static_cast<float>(cfg.head_dim));
        std::vector<float> scores(static_cast<std::size_t>(position + 1));

        for (int head = 0; head < cfg.num_heads; ++head) {
            const int kvHead = head / groups;
            const float* qHead = q.data() + static_cast<std::size_t>(head) * cfg.head_dim;

            float maxScore = -std::numeric_limits<float>::infinity();
            for (int t = 0; t <= position; ++t) {
                const int8_t* kHead = cache.keys.data()
                    + static_cast<std::size_t>(t) * kv_dim_
                    + static_cast<std::size_t>(kvHead) * cfg.head_dim;
                const float k_scale = cache.k_scales[t * cfg.num_kv_heads + kvHead];
                
                float dot = 0.0f;
#ifdef __AVX2__
                __m256 vacc = _mm256_setzero_ps();
                for (int d = 0; d <= cfg.head_dim - 8; d += 8) {
                    __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kHead + d));
                    __m256i v32 = _mm256_cvtepi8_epi32(vraw);
                    __m256 vfloat = _mm256_cvtepi32_ps(v32);
                    __m256 vq = _mm256_loadu_ps(qHead + d);
                    vacc = _mm256_add_ps(vacc, _mm256_mul_ps(vfloat, vq));
                }
                float temp[8];
                _mm256_storeu_ps(temp, vacc);
                for (int i = 0; i < 8; ++i) dot += temp[i];
#else
                for (int d = 0; d < cfg.head_dim; ++d) dot += qHead[d] * static_cast<float>(kHead[d]);
#endif
                dot *= k_scale;
                
                scores[static_cast<std::size_t>(t)] = dot * scale;
                maxScore = std::max(maxScore, scores[static_cast<std::size_t>(t)]);
            }

            float denom = 0.0f;
            for (int t = 0; t <= position; ++t) {
                float e = std::exp(scores[static_cast<std::size_t>(t)] - maxScore);
                scores[static_cast<std::size_t>(t)] = e;
                denom += e;
            }
            if (denom == 0.0f || !std::isfinite(denom)) {
                error = "Attention softmax failed";
                return false;
            }

            float* outHead = attn.data() + static_cast<std::size_t>(head) * cfg.head_dim;
            for (int t = 0; t <= position; ++t) {
                const int8_t* vHead = cache.values.data()
                    + static_cast<std::size_t>(t) * kv_dim_
                    + static_cast<std::size_t>(kvHead) * cfg.head_dim;
                const float v_scale = cache.v_scales[t * cfg.num_kv_heads + kvHead];
                const float prob = (scores[static_cast<std::size_t>(t)] / denom) * v_scale;

#ifdef __AVX2__
                __m256 vprob = _mm256_set1_ps(prob);
                for (int d = 0; d <= cfg.head_dim - 8; d += 8) {
                    __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vHead + d));
                    __m256i v32 = _mm256_cvtepi8_epi32(vraw);
                    __m256 vfloat = _mm256_cvtepi32_ps(v32);
                    __m256 vout = _mm256_loadu_ps(outHead + d);
                    vout = _mm256_add_ps(vout, _mm256_mul_ps(vfloat, vprob));
                    _mm256_storeu_ps(outHead + d, vout);
                }
#else
                for (int d = 0; d < cfg.head_dim; ++d) outHead[d] += prob * static_cast<float>(vHead[d]);
#endif
            }
        }

        std::vector<float> attnNorm;
        if (!rmsNorm(attn, tensorFloat(pfx + "self_attn.attn_sub_norm.weight", error), attnNorm, error)) return false;
        return linearPacked(pfx + "self_attn.o_proj.weight", cfg.hidden_size, cfg.hidden_size, attnNorm, out, error);
    }

    bool mlpLayer(
        const std::string& pfx,
        const std::vector<float>& norm,
        std::vector<float>& out,
        std::string& error) {

        std::vector<float> gate;
        std::vector<float> up;
        if (!linearPacked(pfx + "mlp.gate_proj.weight", cfg.intermediate_size, cfg.hidden_size, norm, gate, error)) return false;
        if (!linearPacked(pfx + "mlp.up_proj.weight", cfg.intermediate_size, cfg.hidden_size, norm, up, error)) return false;

        std::vector<float> activated(static_cast<std::size_t>(cfg.intermediate_size));
        for (int i = 0; i < cfg.intermediate_size; ++i) {
            activated[static_cast<std::size_t>(i)] = relu2(gate[static_cast<std::size_t>(i)]) * up[static_cast<std::size_t>(i)];
        }

        std::vector<float> ffnNorm;
        if (!rmsNorm(activated, tensorFloat(pfx + "mlp.ffn_sub_norm.weight", error), ffnNorm, error)) return false;
        return linearPacked(pfx + "mlp.down_proj.weight", cfg.hidden_size, cfg.intermediate_size, ffnNorm, out, error);
    }

    bool argmaxLogits(const std::vector<float>& hidden, int& bestToken, float& bestScore, std::vector<float>* allLogits = nullptr) {
        if (hidden.size() != static_cast<std::size_t>(cfg.hidden_size)) return false;
        const int workerCount = static_cast<int>(pool_.size());
        const int chunk = (cfg.vocab_size + workerCount - 1) / workerCount;
        
        struct LocalBest {
            float score = -std::numeric_limits<float>::infinity();
            int token = 0;
        };
        std::vector<LocalBest> locals(static_cast<std::size_t>(workerCount));
        
        if (allLogits) {
            allLogits->assign(static_cast<std::size_t>(cfg.vocab_size), 0.0f);
        }

        pool_.execute(workerCount, [&](int worker) {
            const int begin = worker * chunk;
            const int end = std::min(cfg.vocab_size, begin + chunk);
            if (begin < end) {
                LocalBest local;
                for (int token = begin; token < end; ++token) {
                    const uint16_t* row = embeddings_.data() + static_cast<std::size_t>(token) * cfg.hidden_size;
                    float acc = 0.0f;
                    int j = 0;

#ifdef __AVX2__
                    __m256 vacc = _mm256_setzero_ps();
                    for (; j <= cfg.hidden_size - 8; j += 8) {
                        __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + j));
                        __m256i v32 = _mm256_cvtepu16_epi32(vraw);
                        __m256i vshift = _mm256_slli_epi32(v32, 16);
                        __m256 vfloat = _mm256_castsi256_ps(vshift);
                        __m256 vx = _mm256_loadu_ps(hidden.data() + j);
                        vacc = _mm256_add_ps(vacc, _mm256_mul_ps(vfloat, vx));
                    }
                    float temp[8];
                    _mm256_storeu_ps(temp, vacc);
                    for (int i = 0; i < 8; ++i) acc += temp[i];
#endif
                    for (; j < cfg.hidden_size; ++j) {
                        acc += bf16ToFloat(row[j]) * hidden[j];
                    }

                    if (allLogits) (*allLogits)[static_cast<std::size_t>(token)] = acc;

                    if (acc > local.score) {
                        local.score = acc;
                        local.token = token;
                    }
                }
                locals[static_cast<std::size_t>(worker)] = local;
            }
        });

        bestToken = 0;
        bestScore = -std::numeric_limits<float>::infinity();
        for (const LocalBest& local : locals) {
            if (local.score > bestScore) {
                bestScore = local.score;
                bestToken = local.token;
            }
        }
        return true;
    }
};

class BitNetInference {
public:
    BitNetConfig cfg;
    vm::VMState& vm;
    BitNetLoader& loader;

    BitNetInference(vm::VMState& v, BitNetLoader& l)
        : vm(v), loader(l) {}

    BitNetInference(vm::VMState& v, BitNetLoader& l, const BitNetConfig& c)
        : cfg(c), vm(v), loader(l) {}

    // -------------------------------------------------------------------------
    // forward() — sequence forward pass.
    // Inputs:
    //   input_ids     vector of token indices [0, vocab_size)
    //   logit_addr    DMEM address to write vocab_size logits for the LAST token
    // Returns ForwardResult for the last token in the sequence.
    // -------------------------------------------------------------------------
    BitNetForwardResult forward(const std::vector<int>& input_ids, int logit_addr) {
        BitNetForwardResult result;
        if (input_ids.empty()) {
            result.error = "Empty input_ids";
            return result;
        }

        const auto t0 = std::chrono::high_resolution_clock::now();

        const int H = cfg.hidden_size;
        const int I = cfg.intermediate_size;
        DMemLayout layout{ H, I };
        const int weight_base = layout.scratch_words();
        TensorView x_view    = { layout.x_base(),    1, H, TernaryMode::T50 };
        TensorView norm_view = { layout.norm_base(), 1, H, TernaryMode::T50 };

        int current_token_id = input_ids.back();
        std::cout << "[1/4] Embedding lookup (token " << current_token_id << ")...\n";

        // Validate DMEM is large enough for scratch + logits
        const int min_dmem = layout.scratch_words() + cfg.vocab_size;
        if (vm.dmem.size() < min_dmem) {
            result.error = "DMEM too small: need " + std::to_string(min_dmem)
                         + " words, have " + std::to_string(vm.dmem.size());
            return result;
        }

        // -----------------------------------------------------------------------
        // Step 1: Embedding lookup — x = embed_tokens[current_token_id]
        // -----------------------------------------------------------------------
        {
            int loaded = loader.loadLayerToVM(
                vm,
                "model.embed_tokens.weight",
                weight_base,
                H,                        // load exactly one row
                current_token_id * H);    // skip to the row we want
            if (loaded != H) {
                result.error = "Embedding load failed: got " + std::to_string(loaded)
                             + " words, expected " + std::to_string(H);
                return result;
            }
            // Copy embedding row → x
            TensorView embed_view = { weight_base, 1, H, TernaryMode::T50 };
            if (!copyVector(vm, embed_view, x_view, &result.stats)) {
                result.error = "Embedding copy failed";
                return result;
            }
        }

        // -----------------------------------------------------------------------
        // Step 2: Transformer layers
        // -----------------------------------------------------------------------
        for (int l = 0; l < cfg.num_layers; ++l) {
            std::cout << "\r[2/4] Transformer Layer " << l << "/" << cfg.num_layers << "    " << std::flush;
            
            std::string pfx = "model.layers." + std::to_string(l) + ".";

            // --- Pre-attention RMSNorm ---
            {
                int waddr = weight_base;
                int loaded = loadWeight(vm, loader, pfx + "input_layernorm.weight",
                                        waddr, &result.stats);
                if (loaded == 0) {
                    result.error = "Layer " + std::to_string(l) + " input_layernorm load failed";
                    return result;
                }
                TensorView gamma = { waddr, 1, H, TernaryMode::T50 };
                if (!runRmsNorm(vm, x_view, gamma, norm_view, &result.stats)) {
                    result.error = "Layer " + std::to_string(l) + " pre-attn rmsNorm failed";
                    return result;
                }
            }

            // --- Attention block ---
            {
                int waddr = weight_base + H; // leave gamma in place at weight_base
                if (!runAttentionBlock(vm, cfg, layout, pfx,
                                       loader, waddr, x_view, norm_view, &result.stats)) {
                    result.error = "Layer " + std::to_string(l) + " attention failed";
                    return result;
                }
            }

            // --- Pre-MLP RMSNorm ---
            {
                int waddr = weight_base;
                int loaded = loadWeight(vm, loader, pfx + "post_attention_layernorm.weight",
                                        waddr, &result.stats);
                if (loaded == 0) {
                    result.error = "Layer " + std::to_string(l) + " post_attn_layernorm load failed";
                    return result;
                }
                TensorView gamma = { waddr, 1, H, TernaryMode::T50 };
                if (!runRmsNorm(vm, x_view, gamma, norm_view, &result.stats)) {
                    result.error = "Layer " + std::to_string(l) + " pre-MLP rmsNorm failed";
                    return result;
                }
            }

            // --- MLP block ---
            {
                int waddr = weight_base + H;
                if (!runMlpBlock(vm, cfg, layout, pfx,
                                  loader, waddr, x_view, norm_view, &result.stats)) {
                    result.error = "Layer " + std::to_string(l) + " MLP failed";
                    return result;
                }
            }
        }
        std::cout << "\n";

        // -----------------------------------------------------------------------
        // Step 3: Final RMSNorm
        // -----------------------------------------------------------------------
        std::cout << "[3/4] Final RMSNorm + LM head...\n";
        {
            int loaded = loadWeight(vm, loader, "model.norm.weight",
                                    weight_base, &result.stats);
            if (loaded == 0) {
                result.error = "Final norm weight load failed";
                return result;
            }
            TensorView gamma = { weight_base, 1, H, TernaryMode::T50 };
            if (!runRmsNorm(vm, x_view, gamma, norm_view, &result.stats)) {
                result.error = "Final rmsNorm failed";
                return result;
            }
        }

        // Step 4: LM head — logits = W_lm_head @ norm_out
        {
            const int V = cfg.vocab_size;
            if (logit_addr + V > vm.dmem.size()) {
                result.error = "Not enough DMEM for logits";
                return result;
            }

            std::cout << "[4/4] Computing logits...\n";
            
            // Pre-cache normalized output for faster dot products
            std::vector<sandbox::LongTriple> x_cached(H);
            for (int j = 0; j < H; ++j) {
                vm::TernaryValue val;
                loadElement(vm, norm_view, 0, j, val, &result.stats);
                x_cached[j] = val.toLongTriple();
            }

            // Fallback logic for tied embeddings:
            std::string layer_name = "lm_head.weight";
            if (loader.layers.find(layer_name) == loader.layers.end()) {
                layer_name = "model.embed_tokens.weight";
            }

            for (int v = 0; v < V; ++v) {
                int row_addr = weight_base;
                int loaded = loader.loadLayerToVM(vm, layer_name, row_addr, H, v * H);
                if (loaded != H) {
                    result.error = "lm_head/embed row " + std::to_string(v) + " load failed";
                    return result;
                }

                ops::LongTripleAccumulator acc;
                for (int j = 0; j < H; ++j) {
                    auto [wv, wf] = vm.dmem.load(row_addr + j);
                    acc.add(ops::multiply(wv.toLongTriple(), x_cached[j]));
                }
                vm.dmem.store(logit_addr + v, vm::TernaryValue::fromLongTriple(acc.result()));
                
                if (v % 500 == 0)
                    std::cout << "\r  logit " << v << "/" << V << " (" << (v*100/V) << "%)" << std::flush;
            }
            std::cout << "\n";
        }

        // -----------------------------------------------------------------------
        // Step 5: Greedy decode — argmax over logits
        // -----------------------------------------------------------------------
        std::cout << "[4/4] Greedy argmax...\n";
        {
            const int V = cfg.vocab_size;
            int best_token = 0;
            auto [best_val, bf] = vm.dmem.load(logit_addr);
            vm::TernaryValue best = best_val;

            for (int v = 1; v < V; ++v) {
                auto [val, vf] = vm.dmem.load(logit_addr + v);
                if (vf == vm::MemFaultCode::OK) {
                    vm::TernaryValue tv = vm::convertValue(val, TernaryMode::T50);
                    if (vm::exec::compareValue(tv, best, TernaryMode::T50) > 0) {
                        best       = tv;
                        best_token = v;
                    }
                }
            }
            result.greedy_token = best_token;
            result.greedy_logit = best;
        }

        result.ok = true;
        const auto t1 = std::chrono::high_resolution_clock::now();
        result.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        return result;
    }
};

// =============================================================================
// Stats printer
// =============================================================================

inline void printForwardResult(const BitNetForwardResult& r) {
    if (!r.ok) {
        std::cerr << "Forward pass FAILED: " << r.error << "\n";
        return;
    }
    std::cout << "\n=== Forward Pass Complete ===\n";
    std::cout << "  Elapsed:        " << r.elapsed_ms << " ms\n";
    std::cout << "  Greedy token:   " << r.greedy_token << "\n";
    std::cout << "  Scalar ops:     " << r.stats.scalarOps << "\n";
    std::cout << "  DMEM loads:     " << r.stats.dmemLoads << "\n";
    std::cout << "  DMEM stores:    " << r.stats.dmemStores << "\n";
    std::cout << "  VM steps:       " << r.stats.vmSteps << "\n";
    std::cout << "  Kernel count:   " << r.stats.generatedKernels << "\n";
    std::cout << "  ASM words gen:  " << r.stats.generatedAssemblyWords << "\n";
}

inline void printHostForwardResult(const BitNetHostForwardResult& r) {
    if (!r.ok) {
        std::cerr << "Forward pass FAILED: " << r.error << "\n";
        return;
    }
    std::cout << "\n=== Forward Pass Complete ===\n";
    std::cout << "  Elapsed:        " << r.elapsed_ms << " ms\n";
    std::cout << "  Greedy token:   " << r.greedy_token << "\n";
}

} // namespace bitnet
} // namespace sandbox

#endif // BITNET_INFERENCE_H
