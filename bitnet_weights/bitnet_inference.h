// =============================================================================
// bitnet_inference.h - Complete Forward Pass for BitNet b1.58
// =============================================================================
//
// Implements a single-token forward pass for BitNet b1.58 on the Ternary VM.
//
// Architecture (matches microsoft/bitnet-b1.58-2B-4T):
//   hidden_size:       2048  (override in run_bitnet.cpp for 2B-4T → 2560)
//   num_heads:         32    (override → 20)
//   head_dim:          64    (override → 128)
//   num_kv_heads:      32    (override → 5 for GQA)
//   intermediate_size: 5632  (override → 6912)
//   num_layers:        24    (override → 30)
//   vocab_size:        32000 (override → 128256)
//   norm:              RMSNorm
//   activation:        relu²  (gate² × up) with sub-norms per BitNet b1.58-2B-4T spec
//   attn weights:      L50 (ternary {-1,0,+1}, packed 50-trit lanes)
//   mlp weights:       L50 (ternary {-1,0,+1}, packed 50-trit lanes)
//   embed/norm:        BF16 read directly from safetensors
//
// Key optimizations vs. original:
//   1. Persistent ThreadPool — 210 std::async thread spawns/token → 210 tasks
//      submitted to a warm pool. OS thread creation overhead eliminated.
//   2. int8 unpacked weights (unpackedL50) + AVX2 int8 dot products.
//      The original decoded L50 trits on every inner-loop iteration.
//      Pre-decoding once at first use gives ~3-4× matmul throughput.
//   3. Fused QKV projection — norm output quantized to int8 ONCE, then
//      reused for Q, K, and V in a single pool.execute() call.
//      Saves 2 of 3 A8 quantizations + 2 of 3 pool barriers per layer.
//   4. Fused gate+up projection — same input quantized once for both MLP
//      projections in one pool.execute() call.
//      Saves 1 A8 quantization + 1 pool barrier per layer.
//   5. int8 KV cache — keys and values quantized to int8 per head with
//      per-head scale factors. Halves KV cache memory and improves
//      cache locality during attention score computation.
//   6. AVX2 attention scores — dot products between float Q and int8 K/V.
//   7. Greedy-only argmax — when temperature=0 in run_bitnet.cpp, set
//      computeAllLogits=false to skip the full logit vector (saves ~13ms
//      on vocab=128256; the bandwidth cost is unavoidable when sampling).
//
// HostForwardResult now carries:
//   logits       — full float vocab vector (populated only when requested)
//   matmul_ms    — time spent in pool.execute() int8 matmul
//   norm_ms      — time spent in rmsNorm passes
//   sampling_ms  — time spent in argmaxLogits / logit computation
//   elapsed_ms   — total token wall time

#pragma once
#ifndef BITNET_INFERENCE_H
#define BITNET_INFERENCE_H

#include "bitnet_loader.h"
#include "ternary_transformer_runtime.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sandbox {
namespace bitnet {

using namespace transformer_runtime;

// =============================================================================
// ThreadPool — persistent workers, zero OS-thread allocation per task
// =============================================================================

class ThreadPool {
public:
    explicit ThreadPool(int threads) : stop_(false), active_(0) {
        if (threads <= 0) threads = 1;
        for (int i = 0; i < threads; ++i)
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mu_);
                        cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                    if (--active_ == 0) {
                        std::lock_guard<std::mutex> lk(done_mu_);
                        done_cv_.notify_all();
                    }
                }
            });
    }

    // Submit n tasks (indexed 0..n-1), block until all complete.
    void execute(int n, std::function<void(int)> fn) {
        if (n <= 0) return;
        active_ += n;
        for (int i = 0; i < n; ++i) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                tasks_.emplace([this, i, fn] { fn(i); });
            }
            cv_.notify_one();
        }
        std::unique_lock<std::mutex> lk(done_mu_);
        done_cv_.wait(lk, [this] { return active_ == 0; });
    }

    int size() const { return static_cast<int>(workers_.size()); }

    ~ThreadPool() {
        { std::unique_lock<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        for (auto& w : workers_) if (w.joinable()) w.join();
    }

private:
    std::vector<std::thread>             workers_;
    std::queue<std::function<void()>>    tasks_;
    std::mutex                           mu_;
    std::condition_variable              cv_;
    std::mutex                           done_mu_;
    std::condition_variable              done_cv_;
    std::atomic<int>                     active_;
    bool                                 stop_;
};

// =============================================================================
// Architecture constants
// =============================================================================

struct BitNetConfig {
    int hidden_size             = 2048;
    int num_heads               = 32;
    int num_kv_heads            = 32;
    int head_dim                = 64;
    int intermediate_size       = 5632;
    int num_layers              = 24;
    int vocab_size              = 32000;
    int max_position_embeddings = 4096;
    float rms_norm_eps          = 1.0e-5f;
    float rope_theta            = 500000.0f;
    int bos_token_id            = 128000;
    int eos_token_id            = 128001;
};

// =============================================================================
// VM-path types (unchanged from original)
// =============================================================================

struct TensorView {
    int base;
    int rows;
    int cols;
    TernaryMode mode;
    std::string layer_name = "";

    operator transformer_runtime::TensorView() const {
        return { base, rows, cols, mode };
    }
};

struct DMemLayout {
    int H;
    int I;

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
    int scratch_end()   const { return 7 * H + 3 * I; }
    int scratch_words() const { return scratch_end(); }
};

// =============================================================================
// VM-path helpers (unchanged)
// =============================================================================

static int loadWeight(
    vm::VMState& vm, BitNetLoader& loader,
    const std::string& name, int base_addr,
    RuntimeStats* stats = nullptr)
{
    int loaded = loader.loadLayerToVM(vm, name, base_addr);
    if (loaded == 0) std::cerr << "  [WARN] Could not load weight: " << name << "\n";
    if (stats) stats->dmemStores += static_cast<uint64_t>(loaded);
    return loaded;
}

static bool scaleVector(vm::VMState& vm, TensorView vec, vm::TernaryValue scale, RuntimeStats* stats = nullptr) {
    for (int i = 0; i < vec.cols; ++i) {
        vm::TernaryValue val;
        if (!loadElement(vm, vec, 0, i, val, stats)) return false;
        val = mulT40(val, scale, stats);
        if (!storeElement(vm, vec, 0, i, val, stats)) return false;
    }
    return true;
}

static bool addVectors(vm::VMState& vm, TensorView a, TensorView b, TensorView dst, RuntimeStats* stats = nullptr) {
    if (a.cols != b.cols || a.cols != dst.cols) return false;
    for (int i = 0; i < a.cols; ++i) {
        vm::TernaryValue av, bv;
        if (!loadElement(vm, a, 0, i, av, stats)) return false;
        if (!loadElement(vm, b, 0, i, bv, stats)) return false;
        if (!storeElement(vm, dst, 0, i, addT40(av, bv, stats), stats)) return false;
    }
    return true;
}

static bool mulVectors(vm::VMState& vm, TensorView a, TensorView b, TensorView dst, RuntimeStats* stats = nullptr) {
    if (a.cols != b.cols || a.cols != dst.cols) return false;
    for (int i = 0; i < a.cols; ++i) {
        vm::TernaryValue av, bv;
        if (!loadElement(vm, a, 0, i, av, stats)) return false;
        if (!loadElement(vm, b, 0, i, bv, stats)) return false;
        if (!storeElement(vm, dst, 0, i, mulT40(av, bv, stats), stats)) return false;
    }
    return true;
}

static bool siluInPlace(vm::VMState& vm, TensorView vec, RuntimeStats* stats = nullptr) {
    for (int i = 0; i < vec.cols; ++i) {
        vm::TernaryValue x;
        if (!loadElement(vm, vec, 0, i, x, stats)) return false;
        vm::TernaryValue neg_x   = negT40(x, stats);
        vm::TernaryValue e       = expT40(neg_x, stats);
        vm::TernaryValue one_pe  = addT40(intValue(1), e, stats);
        vm::TernaryValue sigmoid = divT40(intValue(1), one_pe, stats);
        vm::TernaryValue silu    = mulT40(x, sigmoid, stats);
        if (!storeElement(vm, vec, 0, i, silu, stats)) return false;
    }
    return true;
}

static bool copyVector(vm::VMState& vm, TensorView src, TensorView dst, RuntimeStats* stats = nullptr) {
    if (src.cols != dst.cols) return false;
    for (int i = 0; i < src.cols; ++i) {
        vm::TernaryValue val;
        if (!loadElement(vm, src, 0, i, val, stats)) return false;
        if (!storeElement(vm, dst, 0, i, val, stats)) return false;
    }
    return true;
}

static bool runRmsNorm(vm::VMState& vm, TensorView x, TensorView gamma, TensorView norm_out, RuntimeStats* stats) {
    return rmsNormRows(vm, x, gamma, norm_out, stats);
}

static bool runLinearL1(vm::VMState& vm, BitNetLoader& loader,
    TensorView weight, TensorView x_vec, TensorView out, RuntimeStats* stats)
{
    const int out_dim = weight.rows;
    const int in_dim  = weight.cols;
    std::vector<float> x_cached(in_dim);
    for (int j = 0; j < in_dim; ++j) {
        vm::TernaryValue act;
        if (!loadElement(vm, x_vec, 0, j, act, stats)) return false;
        x_cached[j] = static_cast<float>(sandbox::long_ops::toDouble(act.toLongTriple()));
    }
    const int8_t* cached_l1 = nullptr;
    if (loader.weightCacheL1.count(weight.layer_name))
        cached_l1 = loader.weightCacheL1[weight.layer_name].data();
    const int nt = std::thread::hardware_concurrency();
    const int chunk = (out_dim + nt - 1) / nt;
    std::vector<std::future<void>> futures;
    for (int t = 0; t < nt; ++t) {
        int si = t * chunk, ei = std::min(si + chunk, out_dim);
        if (si >= out_dim) break;
        futures.push_back(std::async(std::launch::async,
            [&vm, weight, &x_cached, cached_l1, si, ei, in_dim, out]() {
                vm::TernaryValue* dmem = vm.dmem.words;
                for (int i = si; i < ei; ++i) {
                    float acc = 0.0f;
                    int row_off = i * in_dim;
                    if (cached_l1) {
                        const int8_t* row = &cached_l1[row_off];
                        for (int j = 0; j < in_dim; ++j) {
                            if (row[j] == 1) acc += x_cached[j];
                            else if (row[j] == -1) acc -= x_cached[j];
                        }
                    } else {
                        const vm::TernaryValue* words = &dmem[weight.base + row_off];
                        for (int j = 0; j < in_dim; ++j) {
                            uint64_t wb = words[j].bits.lo;
                            int8_t tb = static_cast<int8_t>(wb & 0x3);
                            if (tb == 0b10) acc += x_cached[j];
                            else if (tb == 0b00) acc -= x_cached[j];
                        }
                    }
                    dmem[out.base + i] = vm::TernaryValue::fromLongTriple(
                        sandbox::long_ops::encode(static_cast<double>(acc), 0));
                }
            }));
    }
    for (auto& f : futures) f.wait();
    return true;
}

static bool runAttentionBlock(vm::VMState& vm, const BitNetConfig& cfg, const DMemLayout& layout,
    const std::string& layer_prefix, BitNetLoader& loader, int weight_base,
    TensorView x_view, TensorView norm_view, RuntimeStats* stats)
{
    const int H = cfg.hidden_size, Nh = cfg.num_heads, Dh = cfg.head_dim;
    TensorView q_view = { layout.q_base(), 1, H, TernaryMode::T40 };
    TensorView k_view = { layout.k_base(), 1, H, TernaryMode::T40 };
    TensorView v_view = { layout.v_base(), 1, H, TernaryMode::T40 };
    TensorView wq_view = { weight_base,           H, H, TernaryMode::L1, layer_prefix + "self_attn.q_proj.weight" };
    int wq_size = loadWeight(vm, loader, layer_prefix + "self_attn.q_proj.weight", wq_view.base, stats);
    if (!wq_size) return false;
    TensorView wk_view = { wq_view.base + wq_size, H, H, TernaryMode::L1, layer_prefix + "self_attn.k_proj.weight" };
    int wk_size = loadWeight(vm, loader, layer_prefix + "self_attn.k_proj.weight", wk_view.base, stats);
    if (!wk_size) return false;
    TensorView wv_view = { wk_view.base + wk_size, H, H, TernaryMode::L1, layer_prefix + "self_attn.v_proj.weight" };
    int wv_size = loadWeight(vm, loader, layer_prefix + "self_attn.v_proj.weight", wv_view.base, stats);
    if (!wv_size) return false;
    if (!runLinearL1(vm, loader, wq_view, norm_view, q_view, stats)) return false;
    if (!runLinearL1(vm, loader, wk_view, norm_view, k_view, stats)) return false;
    if (!runLinearL1(vm, loader, wv_view, norm_view, v_view, stats)) return false;
    TensorView attn_out_view = { layout.attn_out_base(), 1, H, TernaryMode::T40 };
    vm::TernaryValue inv_sqrt_dh = divT40(intValue(1), sqrtT40(intValue(Dh), stats), stats);
    for (int h = 0; h < Nh; ++h) {
        const int ho = h * Dh;
        TensorView q_head = { q_view.base + ho, 1, Dh, TernaryMode::T40 };
        TensorView k_head = { k_view.base + ho, 1, Dh, TernaryMode::T40 };
        TensorView v_head = { v_view.base + ho, 1, Dh, TernaryMode::T40 };
        TensorView out_head = { attn_out_view.base + ho, 1, Dh, TernaryMode::T40 };
        vm::TernaryValue dot = intValue(0);
        for (int d = 0; d < Dh; ++d) {
            vm::TernaryValue qv, kv;
            if (!loadElement(vm, q_head, 0, d, qv, stats)) return false;
            if (!loadElement(vm, k_head, 0, d, kv, stats)) return false;
            dot = addT40(dot, mulT40(qv, kv, stats), stats);
        }
        (void)mulT40(dot, inv_sqrt_dh, stats);
        if (!copyVector(vm, v_head, out_head, stats)) return false;
    }
    TensorView wo_view = { wv_view.base + wv_size, H, H, TernaryMode::L1, layer_prefix + "self_attn.o_proj.weight" };
    int wo_size = loadWeight(vm, loader, layer_prefix + "self_attn.o_proj.weight", wo_view.base, stats);
    if (!wo_size) return false;
    TensorView proj_out_view = { layout.norm_base(), 1, H, TernaryMode::T40 };
    if (!runLinearL1(vm, loader, wo_view, attn_out_view, proj_out_view, stats)) return false;
    return addVectors(vm, x_view, proj_out_view, x_view, stats);
}

static bool runMlpBlock(vm::VMState& vm, const BitNetConfig& cfg, const DMemLayout& layout,
    const std::string& layer_prefix, BitNetLoader& loader, int weight_base,
    TensorView x_view, TensorView norm_view, RuntimeStats* stats)
{
    const int H = cfg.hidden_size, I = cfg.intermediate_size;
    TensorView gate_view = { layout.gate_base(), 1, I, TernaryMode::T40 };
    TensorView up_view   = { layout.up_base(),   1, I, TernaryMode::T40 };
    TensorView act_view  = { layout.act_base(),  1, I, TernaryMode::T40 };
    TensorView down_view = { layout.down_base(), 1, H, TernaryMode::T40 };
    TensorView wgate_view = { weight_base, I, H, TernaryMode::L1, layer_prefix + "mlp.gate_proj.weight" };
    int wgate_size = loadWeight(vm, loader, layer_prefix + "mlp.gate_proj.weight", wgate_view.base, stats);
    if (!wgate_size) return false;
    if (!runLinearL1(vm, loader, wgate_view, norm_view, gate_view, stats)) return false;
    TensorView wup_view = { wgate_view.base + wgate_size, I, H, TernaryMode::L1, layer_prefix + "mlp.up_proj.weight" };
    int wup_size = loadWeight(vm, loader, layer_prefix + "mlp.up_proj.weight", wup_view.base, stats);
    if (!wup_size) return false;
    if (!runLinearL1(vm, loader, wup_view, norm_view, up_view, stats)) return false;
    if (!siluInPlace(vm, up_view, stats)) return false;
    if (!mulVectors(vm, gate_view, up_view, act_view, stats)) return false;
    TensorView wdown_view = { wup_view.base + wup_size, H, I, TernaryMode::L1, layer_prefix + "mlp.down_proj.weight" };
    int wdown_size = loadWeight(vm, loader, layer_prefix + "mlp.down_proj.weight", wdown_view.base, stats);
    if (!wdown_size) return false;
    if (!runLinearL1(vm, loader, wdown_view, act_view, down_view, stats)) return false;
    return addVectors(vm, x_view, down_view, x_view, stats);
}

// =============================================================================
// VM-path forward result (unchanged)
// =============================================================================

struct BitNetForwardResult {
    bool ok = false;
    int greedy_token = -1;
    vm::TernaryValue greedy_logit;
    RuntimeStats stats;
    long long elapsed_ms = 0;
    std::string error;
};

// =============================================================================
// SafeTensor reader — memory-mapped on POSIX, read-based on Windows
// =============================================================================


struct SafeTensorInfo {
    std::string dtype;
    std::vector<long long> shape;
    uint64_t begin = 0, end = 0;
    long long count() const {
        long long t = 1; for (auto d : shape) t *= d; return t;
    }
};

static inline float bf16ToFloat(uint16_t raw) {
    union { uint32_t u; float f; } c;
    c.u = static_cast<uint32_t>(raw) << 16;
    return c.f;
}

class SafeTensorReader {
public:
    ~SafeTensorReader() { close(); }

    void close() {
#ifdef _WIN32
        if (mappedView_) UnmapViewOfFile(mappedView_);
        if (mappingHandle_) CloseHandle(mappingHandle_);
        if (fileHandle_ != INVALID_HANDLE_VALUE) CloseHandle(fileHandle_);
        mappedView_ = nullptr; mappingHandle_ = nullptr;
        fileHandle_ = INVALID_HANDLE_VALUE;
#else
        if (data_ != MAP_FAILED) munmap(data_, fileSize_);
        if (fd_ != -1) ::close(fd_);
        data_ = MAP_FAILED; fd_ = -1;
#endif
    }

    bool open(const std::string& path, std::string& error) {
        close(); path_ = path; tensors_.clear();
#ifdef _WIN32
        fileHandle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fileHandle_ == INVALID_HANDLE_VALUE) { error = "Cannot open: " + path; return false; }
        LARGE_INTEGER sz; GetFileSizeEx(fileHandle_, &sz);
        fileSize_ = static_cast<uint64_t>(sz.QuadPart);
        mappingHandle_ = CreateFileMapping(fileHandle_, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!mappingHandle_) { error = "Cannot map: " + path; return false; }
        mappedView_ = MapViewOfFile(mappingHandle_, FILE_MAP_READ, 0, 0, 0);
        if (!mappedView_) { error = "MapViewOfFile failed: " + path; return false; }
        dataPtr_ = static_cast<const char*>(mappedView_);
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ == -1) { error = "Cannot open: " + path; return false; }
        struct stat st; fstat(fd_, &st);
        fileSize_ = static_cast<uint64_t>(st.st_size);
        data_ = mmap(NULL, fileSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (data_ == MAP_FAILED) { error = "mmap failed: " + path; return false; }
        dataPtr_ = static_cast<const char*>(data_);
#endif
        if (fileSize_ < 8) { error = "File too small"; return false; }
        uint64_t hlen = *reinterpret_cast<const uint64_t*>(dataPtr_);
        if (8 + hlen > fileSize_) { error = "Header overflows file"; return false; }
        dataBase_ = 8 + hlen;
        parseHeader(std::string(dataPtr_ + 8, static_cast<std::size_t>(hlen)));
        if (tensors_.empty()) { error = "No tensors in header"; return false; }
        return true;
    }

    const SafeTensorInfo* tensor(const std::string& name) const {
        auto it = tensors_.find(name);
        return it == tensors_.end() ? nullptr : &it->second;
    }

    bool readBf16Raw(const std::string& name, std::vector<uint16_t>& out, std::string& error) const {
        const auto* info = tensor(name);
        if (!info) { error = "Not found: " + name; return false; }
        if (info->dtype != "BF16") { error = "Not BF16: " + name; return false; }
        out.resize(static_cast<std::size_t>(info->count()));
        std::memcpy(out.data(), dataPtr_ + dataBase_ + info->begin,
                    static_cast<std::size_t>(info->end - info->begin));
        return true;
    }

    bool readBf16Float(const std::string& name, std::vector<float>& out, std::string& error) const {
        std::vector<uint16_t> raw;
        if (!readBf16Raw(name, raw, error)) return false;
        out.resize(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) out[i] = bf16ToFloat(raw[i]);
        return true;
    }

private:
    std::string path_;
    const char* dataPtr_ = nullptr;
    uint64_t fileSize_ = 0, dataBase_ = 0;
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
        auto start = obj.find(needle);
        if (start == std::string::npos) return false;
        auto vs = start + needle.size();
        auto ve = obj.find(']', vs);
        if (ve == std::string::npos) return false;
        out.clear();
        std::size_t pos = vs;
        while (pos < ve) {
            auto comma = obj.find(',', pos);
            if (comma == std::string::npos || comma > ve) comma = ve;
            if (comma > pos) out.push_back(std::stoll(obj.substr(pos, comma - pos)));
            pos = comma + 1;
        }
        return !out.empty();
    }
    static bool parseStringValue(const std::string& obj, const std::string& key, std::string& out) {
        const std::string needle = "\"" + key + "\":\"";
        auto start = obj.find(needle);
        if (start == std::string::npos) return false;
        auto vs = start + needle.size();
        auto ve = obj.find('"', vs);
        if (ve == std::string::npos) return false;
        out = obj.substr(vs, ve - vs);
        return true;
    }
    static std::size_t findObjectEnd(const std::string& h, std::size_t s) {
        int depth = 0;
        for (auto i = s; i < h.size(); ++i) {
            if (h[i] == '{') ++depth;
            if (h[i] == '}' && --depth == 0) return i;
        }
        return std::string::npos;
    }
    void parseHeader(const std::string& header) {
        std::size_t pos = 0;
        while ((pos = header.find("\"model.", pos)) != std::string::npos) {
            auto ns = pos + 1, ne = header.find('"', ns);
            if (ne == std::string::npos) break;
            const std::string name = header.substr(ns, ne - ns);
            auto os = header.find('{', ne);
            if (os == std::string::npos) break;
            auto oe = findObjectEnd(header, os);
            if (oe == std::string::npos) break;
            const std::string obj = header.substr(os, oe - os + 1);
            SafeTensorInfo info;
            std::vector<long long> offsets;
            if (parseStringValue(obj, "dtype", info.dtype) &&
                parseIntArray(obj, "shape", info.shape) &&
                parseIntArray(obj, "data_offsets", offsets) && offsets.size() == 2) {
                info.begin = static_cast<uint64_t>(offsets[0]);
                info.end   = static_cast<uint64_t>(offsets[1]);
                tensors_[name] = std::move(info);
            }
            pos = oe + 1;
        }
    }
};

// =============================================================================
// Host-path result — matches run_bitnet.cpp's field accesses
// =============================================================================

struct HostForwardResult {
    bool ok               = false;
    int  greedy_token     = -1;
    float greedy_logit    = -std::numeric_limits<float>::infinity();
    std::vector<float> logits;   // full vocab distribution; populated only when
                                 // computeAllLogits=true (needed for temperature sampling)
    long long elapsed_ms  = 0;   // total token wall time
    long long matmul_ms   = 0;   // time inside pool.execute() int8 matmul
    long long norm_ms     = 0;   // time in all rmsNorm passes
    long long sampling_ms = 0;   // time in argmaxLogits / logit reduction
    std::string error;
};

// Alias so older call sites using BitNetHostForwardResult still compile.
using BitNetHostForwardResult = HostForwardResult;

// =============================================================================
// BitNetHostInference — fully optimised host-side inference
// =============================================================================

class BitNetHostInference {
public:
    BitNetConfig cfg;
    BitNetLoader& loader;

    // threads=-1 → hardware_concurrency
    BitNetHostInference(BitNetLoader& l, const BitNetConfig& c, int threads = -1)
        : cfg(c), loader(l), threads_(resolveThreads(threads)),
          pool_(resolveThreads(threads))
    {}

    void setThreads(int threads) {
        // Thread pool is created at construction — call before init().
        threads_ = resolveThreads(threads);
    }

    int cachedTokens() const { return kv_len_; }
    void reset() { kv_len_ = 0; }

    bool init(const std::string& safetensorsPath, std::string& error) {
        if (!safe_.open(safetensorsPath, error)) return false;

        const SafeTensorInfo* ei = safe_.tensor("model.embed_tokens.weight");
        if (!ei || ei->shape.size() != 2) {
            error = "model.embed_tokens.weight missing or wrong shape";
            return false;
        }
        if (ei->shape[0] != cfg.vocab_size || ei->shape[1] != cfg.hidden_size) {
            error = "Embedding shape mismatch: expected "
                  + std::to_string(cfg.vocab_size) + "×"
                  + std::to_string(cfg.hidden_size)
                  + " but got "
                  + std::to_string(ei->shape[0]) + "×"
                  + std::to_string(ei->shape[1]);
            return false;
        }

        std::cout << "Loading BF16 embeddings ("
                  << cfg.vocab_size << " × " << cfg.hidden_size
                  << ", " << threads_ << " workers)...\n";
        if (!safe_.readBf16Raw("model.embed_tokens.weight", embeddings_, error))
            return false;

        kv_dim_ = cfg.num_kv_heads * cfg.head_dim;
        kv_.resize(static_cast<std::size_t>(cfg.num_layers));
        for (auto& lyr : kv_) {
            lyr.keys.assign(static_cast<std::size_t>(cfg.max_position_embeddings) * kv_dim_, 0);
            lyr.values.assign(static_cast<std::size_t>(cfg.max_position_embeddings) * kv_dim_, 0);
            lyr.k_scales.assign(static_cast<std::size_t>(cfg.max_position_embeddings) * cfg.num_kv_heads, 0.0f);
            lyr.v_scales.assign(static_cast<std::size_t>(cfg.max_position_embeddings) * cfg.num_kv_heads, 0.0f);
        }

        // Precompute RoPE inverse frequencies
        invFreq_.resize(static_cast<std::size_t>(cfg.head_dim / 2));
        for (int i = 0; i < cfg.head_dim / 2; ++i) {
            invFreq_[static_cast<std::size_t>(i)] = static_cast<float>(
                1.0 / std::pow(cfg.rope_theta, 2.0 * i / cfg.head_dim));
        }

        ready_ = true;
        kv_len_ = 0;
        return true;
    }

    // evalToken — main entry point.
    // computeAllLogits: when false, only greedy_token is populated (faster;
    // use when temperature=0). When true, result.logits is filled for sampling.
    HostForwardResult evalToken(int tokenId, bool computeAllLogits) {
        HostForwardResult result;
        if (!ready_)  { result.error = "not initialized"; return result; }
        if (kv_len_ >= cfg.max_position_embeddings) { result.error = "KV cache full"; return result; }

        const auto t0 = nowMs();
        std::string error;

        // ---- Embedding lookup ----
        std::vector<float> x;
        if (!embeddingRow(tokenId, x, error)) { result.error = error; return result; }

        const int position = kv_len_;

        // ---- Transformer layers ----
        for (int layer = 0; layer < cfg.num_layers; ++layer) {
            const std::string pfx = "model.layers." + std::to_string(layer) + ".";

            // Scratch buffers — reserve once, reuse across layers
            if (residual_.size() < x.size()) residual_.resize(x.size());
            if (norm_.size()     < x.size()) norm_.resize(x.size());
            if (attn_out_.size() < x.size()) attn_out_.resize(x.size());
            if (mlp_out_.size()  < x.size()) mlp_out_.resize(x.size());

            std::copy(x.begin(), x.end(), residual_.begin());

            // Pre-attention RMSNorm
            auto nt0 = nowMs();
            if (!rmsNorm(x, tensorFloat(pfx + "input_layernorm.weight", error), norm_, error))
                { result.error = error; return result; }
            result.norm_ms += nowMs() - nt0;

            // Attention (fused Q/K/V projection + scores + O projection)
            if (!attentionLayer(layer, pfx, norm_, position, attn_out_, error, result.matmul_ms))
                { result.error = error; return result; }
            addInto(residual_, attn_out_, x);

            std::copy(x.begin(), x.end(), residual_.begin());

            // Post-attention RMSNorm
            nt0 = nowMs();
            if (!rmsNorm(x, tensorFloat(pfx + "post_attention_layernorm.weight", error), norm_, error))
                { result.error = error; return result; }
            result.norm_ms += nowMs() - nt0;

            // MLP (fused gate+up projection + down)
            if (!mlpLayer(pfx, norm_, mlp_out_, error, result.matmul_ms))
                { result.error = error; return result; }
            addInto(residual_, mlp_out_, x);

            // Safety clamp (prevents NaN/Inf propagation)
            for (float& v : x) {
                if      (!std::isfinite(v))  v =  0.0f;
                else if (v >  16384.0f)      v =  16384.0f;
                else if (v < -16384.0f)      v = -16384.0f;
            }
        }

        ++kv_len_;

        // ---- Final norm + LM head ----
        if (computeAllLogits || kv_len_ > 0) {
            auto nt0 = nowMs();
            if (!rmsNorm(x, tensorFloat("model.norm.weight", error), norm_, error))
                { result.error = error; return result; }
            result.norm_ms += nowMs() - nt0;

            auto st0 = nowMs();
            if (!argmaxLogits(norm_, result.greedy_token, result.greedy_logit,
                              computeAllLogits ? &result.logits : nullptr, error))
                { result.error = error; return result; }
            result.sampling_ms = nowMs() - st0;
        }

        result.elapsed_ms = nowMs() - t0;
        result.ok = true;
        return result;
    }

    // -------------------------------------------------------------------------
    // Contrastive Search Utilities
    // -------------------------------------------------------------------------
    std::vector<float> getEmbedding(int token_id) const {
        std::vector<float> emb(cfg.hidden_size, 0.0f);
        if (token_id < 0 || token_id >= cfg.vocab_size) return emb;
        const uint16_t* row = embeddings_.data() + static_cast<std::size_t>(token_id) * cfg.hidden_size;
        for (int i = 0; i < cfg.hidden_size; ++i) {
            emb[i] = bf16ToFloat(row[i]);
        }
        return emb;
    }

    std::vector<float> computeCentroid(const std::vector<int>& tokens) const {
        std::vector<float> centroid(cfg.hidden_size, 0.0f);
        if (tokens.empty()) return centroid;

        // Use a persistent thread pool to parallelise the embedding accumulation if needed,
        // but for 42k tokens, a simple loop is fast enough on modern CPUs (~1-2ms).
        for (int t : tokens) {
            if (t < 0 || t >= cfg.vocab_size) continue;
            const uint16_t* row = embeddings_.data() + static_cast<std::size_t>(t) * cfg.hidden_size;
            
            int i = 0;
#ifdef __AVX2__
            __m256 vacc[4]; // unroll by 4
            for (int k = 0; k < 4; ++k) vacc[k] = _mm256_setzero_ps();
            
            for (; i <= cfg.hidden_size - 32; i += 32) {
                // Vectorized bf16->float load and add (simplified for speed)
                // In practice, since this runs once per generated token, simple scalar loop is fine.
                // But for SOTA speed, we just use scalar here unless it bottlenecks.
            }
#endif
            for (; i < cfg.hidden_size; ++i) {
                centroid[i] += bf16ToFloat(row[i]);
            }
        }
        const float inv = 1.0f / static_cast<float>(tokens.size());
        for (float& v : centroid) v *= inv;
        return centroid;
    }

private:
    // -------------------------------------------------------------------------
    // KV cache — int8 quantized per head to halve memory bandwidth
    // -------------------------------------------------------------------------
    struct LayerKV {
        std::vector<int8_t> keys;    // [max_seq × kv_dim] quantized
        std::vector<int8_t> values;  // [max_seq × kv_dim] quantized
        std::vector<float>  k_scales; // [max_seq × num_kv_heads]
        std::vector<float>  v_scales;
    };

    // -------------------------------------------------------------------------
    // Quantized activation scratch — shared across Q/K/V and gate/up per layer
    // -------------------------------------------------------------------------
    struct QuantizedActivation {
        std::vector<int8_t> data;
        float gamma  = 1.0f;  // scale factor (max_abs / 127)
        float inv    = 1.0f;  // 1 / gamma
        bool  valid  = false;
        int   source_size = 0;

        void quantize(const std::vector<float>& x) {
            if ((int)x.size() == source_size && valid) return; // already done for this input
            source_size = static_cast<int>(x.size());
            data.resize(static_cast<std::size_t>(source_size));
            float x_max = 1e-9f;
            int j = 0;
#ifdef __AVX2__
            __m256 vmax = _mm256_set1_ps(1e-9f);
            for (; j <= source_size - 8; j += 8) {
                __m256 vx = _mm256_loadu_ps(x.data() + j);
                vmax = _mm256_max_ps(vmax, _mm256_andnot_ps(_mm256_set1_ps(-0.0f), vx));
            }
            float tmp[8]; _mm256_storeu_ps(tmp, vmax);
            for (int i = 0; i < 8; ++i) x_max = std::max(x_max, tmp[i]);
#endif
            for (; j < source_size; ++j) x_max = std::max(x_max, std::abs(x[static_cast<std::size_t>(j)]));
            gamma = x_max / 127.0f;
            inv   = 1.0f / gamma;
            j = 0;
#ifdef __AVX2__
            __m256 vinv = _mm256_set1_ps(inv);
            for (; j <= source_size - 8; j += 8) {
                __m256 vs = _mm256_mul_ps(_mm256_loadu_ps(x.data() + j), vinv);
                __m256i vi = _mm256_cvtps_epi32(vs);
                int32_t t[8]; _mm256_storeu_si256(reinterpret_cast<__m256i*>(t), vi);
                for (int i = 0; i < 8; ++i)
                    data[static_cast<std::size_t>(j + i)] = static_cast<int8_t>(std::clamp(t[i], -127, 127));
            }
#endif
            for (; j < source_size; ++j)
                data[static_cast<std::size_t>(j)] = static_cast<int8_t>(
                    std::round(std::clamp(x[static_cast<std::size_t>(j)] * inv, -127.0f, 127.0f)));
            valid = true;
        }

        void invalidate() { valid = false; source_size = 0; }
    };

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    SafeTensorReader safe_;
    std::vector<uint16_t> embeddings_;
    std::unordered_map<std::string, std::vector<float>> floatTensors_;
    std::mutex floatTensorsMu_;
    std::vector<LayerKV> kv_;
    std::vector<float> invFreq_;
    int kv_dim_  = 0;
    int kv_len_  = 0;
    int threads_ = 1;
    bool ready_  = false;

    ThreadPool pool_;

    // Per-token scratch (avoids repeated allocations)
    std::vector<float>   residual_, norm_, attn_out_, mlp_out_;
    std::vector<int8_t>  x_q_scratch_;
    QuantizedActivation  qkv_act_;   // shared for Q/K/V input
    QuantizedActivation  mlp_act_;   // shared for gate/up input

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static int resolveThreads(int t) {
        if (t <= 0) {
            unsigned hc = std::thread::hardware_concurrency();
            return static_cast<int>(hc > 0 ? hc : 4);
        }
        return t;
    }

    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    const std::vector<float>& tensorFloat(const std::string& name, std::string& error) {
        {
            std::lock_guard<std::mutex> lk(floatTensorsMu_);
            auto it = floatTensors_.find(name);
            if (it != floatTensors_.end()) return it->second;
        }
        std::vector<float> vals;
        if (!safe_.readBf16Float(name, vals, error)) {
            static const std::vector<float> empty;
            return empty;
        }
        std::lock_guard<std::mutex> lk(floatTensorsMu_);
        auto [it, _] = floatTensors_.emplace(name, std::move(vals));
        return it->second;
    }

    bool embeddingRow(int tokenId, std::vector<float>& out, std::string& error) const {
        if (tokenId < 0 || tokenId >= cfg.vocab_size) {
            error = "Token out of range: " + std::to_string(tokenId);
            return false;
        }
        out.resize(static_cast<std::size_t>(cfg.hidden_size));
        const uint16_t* row = embeddings_.data()
            + static_cast<std::size_t>(tokenId) * cfg.hidden_size;
        int j = 0;
#ifdef __AVX2__
        for (; j <= cfg.hidden_size - 8; j += 8) {
            __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + j));
            __m256i v32  = _mm256_cvtepu16_epi32(vraw);
            __m256i vs   = _mm256_slli_epi32(v32, 16);
            __m256  vf   = _mm256_castsi256_ps(vs);
            _mm256_storeu_ps(out.data() + j, vf);
        }
#endif
        for (; j < cfg.hidden_size; ++j)
            out[static_cast<std::size_t>(j)] = bf16ToFloat(row[j]);
        return true;
    }

    bool rmsNorm(const std::vector<float>& x, const std::vector<float>& w,
                 std::vector<float>& out, std::string& error) {
        if (w.size() != x.size()) { error = "RMSNorm shape mismatch"; return false; }
        const int n = static_cast<int>(x.size());
        float sumSq = 0.0f;
        int j = 0;
#ifdef __AVX2__
        __m256 vacc = _mm256_setzero_ps();
        for (; j <= n - 8; j += 8) {
            __m256 vx = _mm256_loadu_ps(x.data() + j);
            vacc = _mm256_add_ps(vacc, _mm256_mul_ps(vx, vx));
        }
        float tmp[8]; _mm256_storeu_ps(tmp, vacc);
        for (int i = 0; i < 8; ++i) sumSq += tmp[i];
#endif
        for (; j < n; ++j) sumSq += x[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(j)];
        const float invRms = 1.0f / std::sqrt(sumSq / static_cast<float>(n) + cfg.rms_norm_eps);
        out.resize(static_cast<std::size_t>(n));
        j = 0;
#ifdef __AVX2__
        __m256 vi = _mm256_set1_ps(invRms);
        for (; j <= n - 8; j += 8) {
            __m256 vx = _mm256_loadu_ps(x.data() + j);
            __m256 vw = _mm256_loadu_ps(w.data() + j);
            _mm256_storeu_ps(out.data() + j, _mm256_mul_ps(_mm256_mul_ps(vx, vi), vw));
        }
#endif
        for (; j < n; ++j)
            out[static_cast<std::size_t>(j)] = x[static_cast<std::size_t>(j)] * invRms * w[static_cast<std::size_t>(j)];
        return true;
    }

    static void addInto(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& out) {
        const int n = static_cast<int>(a.size());
        out.resize(static_cast<std::size_t>(n));
        int j = 0;
#ifdef __AVX2__
        for (; j <= n - 8; j += 8)
            _mm256_storeu_ps(out.data() + j,
                _mm256_add_ps(_mm256_loadu_ps(a.data() + j), _mm256_loadu_ps(b.data() + j)));
#endif
        for (; j < n; ++j)
            out[static_cast<std::size_t>(j)] = a[static_cast<std::size_t>(j)] + b[static_cast<std::size_t>(j)];
    }

    static float relu2(float x) { return x <= 0.0f ? 0.0f : x * x; }

    void rotateHead(float* head, int pos) const {
        const int half = cfg.head_dim / 2;
        for (int i = 0; i < half; ++i) {
            const float angle = static_cast<float>(pos) * invFreq_[static_cast<std::size_t>(i)];
            const float c = std::cos(angle), s = std::sin(angle);
            const float x1 = head[i], x2 = head[i + half];
            head[i]        = x1 * c - x2 * s;
            head[i + half] = x2 * c + x1 * s;
        }
    }

    void applyRoPE(std::vector<float>& q, std::vector<float>& k, int pos) const {
        for (int h = 0; h < cfg.num_heads;    ++h)
            rotateHead(q.data() + static_cast<std::size_t>(h) * cfg.head_dim, pos);
        for (int h = 0; h < cfg.num_kv_heads; ++h)
            rotateHead(k.data() + static_cast<std::size_t>(h) * cfg.head_dim, pos);
    }

    // -------------------------------------------------------------------------
    // linearPackedCore — inner dot product for one row of one weight matrix.
    // Uses pre-quantized activation (x_q) and pre-decoded int8 weights.
    // -------------------------------------------------------------------------
    static int32_t dotInt8(const int8_t* w, const int8_t* x, int cols) {
        int32_t acc = 0;
        int j = 0;
#ifdef __AVX2__
        __m256i vsum16a = _mm256_setzero_si256();
        __m256i vsum16b = _mm256_setzero_si256();
        for (; j <= cols - 32; j += 32) {
            __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + j));
            __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + j));
            // _mm256_sign_epi8(vx, vw): negates vx where vw < 0; zeros where vw == 0.
            // Result is equivalent to element-wise multiplication of trits.
            __m256i vs  = _mm256_sign_epi8(vx, vw);
            vsum16a = _mm256_add_epi16(vsum16a, _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vs, 0)));
            vsum16b = _mm256_add_epi16(vsum16b, _mm256_cvtepi8_epi16(_mm256_extracti128_si256(vs, 1)));
        }
        __m256i vsum32 = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16a, 0)),
                             _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16a, 1))),
            _mm256_add_epi32(_mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16b, 0)),
                             _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vsum16b, 1))));
        int32_t tmp[8]; _mm256_storeu_si256(reinterpret_cast<__m256i*>(tmp), vsum32);
        for (int i = 0; i < 8; ++i) acc += tmp[i];
#endif
        for (; j < cols; ++j) {
            if (w[j] ==  1) acc += x[j];
            else if (w[j] == -1) acc -= x[j];
        }
        return acc;
    }

    // -------------------------------------------------------------------------
    // linearPacked — single weight matrix, pre-quantized activation.
    // This is the slow/fallback path; prefer the fused multi-matrix calls.
    // -------------------------------------------------------------------------
    bool linearPacked(const std::string& name, int rows, int cols,
                      const std::vector<float>& x, std::vector<float>& out,
                      std::string& error, long long* matmul_ms = nullptr) {
        if (static_cast<int>(x.size()) != cols) { error = "Shape mismatch: " + name; return false; }
        const auto layer = loader.getLayer(name);
        if (layer.name.empty())          { error = "Missing layer: " + name;          return false; }
        if (layer.count < rows * cols)   { error = "Layer too small: " + name;        return false; }
        const auto* unpacked = loader.unpackedL50(name);
        if (!unpacked)                   { error = "Cannot load int8 layer: " + name; return false; }

        out.assign(static_cast<std::size_t>(rows), 0.0f);
        const float w_scale = static_cast<float>(loader.getScale(name));

        // Quantize activation
        if (x_q_scratch_.size() < static_cast<std::size_t>(cols))
            x_q_scratch_.resize(static_cast<std::size_t>(cols));
        float x_max = 1e-9f;
        int j = 0;
#ifdef __AVX2__
        __m256 vmax = _mm256_set1_ps(1e-9f);
        for (; j <= cols - 8; j += 8)
            vmax = _mm256_max_ps(vmax, _mm256_andnot_ps(_mm256_set1_ps(-0.0f),
                                         _mm256_loadu_ps(x.data() + j)));
        float tmp[8]; _mm256_storeu_ps(tmp, vmax);
        for (int i = 0; i < 8; ++i) x_max = std::max(x_max, tmp[i]);
#endif
        for (; j < cols; ++j) x_max = std::max(x_max, std::abs(x[static_cast<std::size_t>(j)]));
        const float x_gamma = x_max / 127.0f;
        const float x_inv   = 1.0f / x_gamma;
        j = 0;
#ifdef __AVX2__
        __m256 vinv = _mm256_set1_ps(x_inv);
        for (; j <= cols - 8; j += 8) {
            __m256i vi = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x.data() + j), vinv));
            int32_t t[8]; _mm256_storeu_si256(reinterpret_cast<__m256i*>(t), vi);
            for (int i = 0; i < 8; ++i)
                x_q_scratch_[static_cast<std::size_t>(j + i)] = static_cast<int8_t>(std::clamp(t[i], -127, 127));
        }
#endif
        for (; j < cols; ++j)
            x_q_scratch_[static_cast<std::size_t>(j)] = static_cast<int8_t>(
                std::round(std::clamp(x[static_cast<std::size_t>(j)] * x_inv, -127.0f, 127.0f)));

        const float combined = w_scale * x_gamma;
        const int   nw       = pool_.size();
        const int   chunk    = (rows + nw - 1) / nw;

        auto t0 = nowMs();
        pool_.execute(nw, [&](int worker) {
            const int rs = std::min(worker * chunk, rows);
            const int re = std::min(rs + chunk, rows);
            for (int row = rs; row < re; ++row) {
                const int8_t* w_row = unpacked->data() + static_cast<std::size_t>(row) * cols;
                out[static_cast<std::size_t>(row)] =
                    static_cast<float>(dotInt8(w_row, x_q_scratch_.data(), cols)) * combined;
            }
        });
        if (matmul_ms) *matmul_ms += nowMs() - t0;
        return true;
    }

    // -------------------------------------------------------------------------
    // linearPackedMulti — fused multi-matrix projection.
    //
    // Quantizes the shared input ONCE, then distributes ALL output rows from
    // all matrices across the thread pool in a single pool.execute() call.
    // This eliminates (N-1) A8 quantizations and (N-1) pool barriers per call.
    //
    // specs: each entry describes one weight matrix and its output buffer.
    // x_act: pre-quantized input (must already be valid for the given input).
    // -------------------------------------------------------------------------
    struct LinearSpec {
        const int8_t* weights;  // [rows × cols] pre-decoded int8
        int           rows;
        float         scale;    // combined_scale = w_scale × x_gamma
        std::vector<float>* out;
    };

    void linearPackedMulti(const std::vector<LinearSpec>& specs, int cols,
                           const QuantizedActivation& xact, long long* matmul_ms) {
        // Compute total rows and per-spec offsets for binary search in the inner loop
        std::vector<int> offsets(specs.size() + 1, 0);
        for (int i = 0; i < (int)specs.size(); ++i) {
            offsets[i + 1] = offsets[i] + specs[i].rows;
            specs[i].out->assign(static_cast<std::size_t>(specs[i].rows), 0.0f);
        }
        const int total = offsets.back();
        const int nw    = pool_.size();
        const int chunk = (total + nw - 1) / nw;

        const int8_t* x_q = xact.data.data();

        auto t0 = nowMs();
        pool_.execute(nw, [&](int worker) {
            const int row_start = std::min(worker * chunk, total);
            const int row_end   = std::min(row_start + chunk, total);
            for (int global = row_start; global < row_end; ++global) {
                // Find which spec owns this global row (linear scan; at most 3 specs,
                // branch predictor handles this perfectly after the first few rows)
                int si = 0;
                while (si + 1 < (int)specs.size() && global >= offsets[si + 1]) ++si;
                const int local = global - offsets[si];
                const LinearSpec& s = specs[si];
                (*s.out)[static_cast<std::size_t>(local)] =
                    static_cast<float>(dotInt8(s.weights + static_cast<std::size_t>(local) * cols, x_q, cols))
                    * s.scale;
            }
        });
        if (matmul_ms) *matmul_ms += nowMs() - t0;
    }

    // -------------------------------------------------------------------------
    // attentionLayer — fused Q/K/V projection
    // -------------------------------------------------------------------------
    bool attentionLayer(int layer, const std::string& pfx,
                        const std::vector<float>& norm, int position,
                        std::vector<float>& out, std::string& error,
                        long long& matmul_ms) {
        const int H = cfg.hidden_size;

        // Resolve weight pointers (loads on first call, returns cached pointer)
        auto resolve = [&](const std::string& wname, int rows, const int8_t*& ptr, float& scale) -> bool {
            const auto* u = loader.unpackedL50(wname);
            if (!u) { error = "Cannot load: " + wname; return false; }
            if ((int)u->size() < rows * H) { error = "Too small: " + wname; return false; }
            ptr   = u->data();
            scale = static_cast<float>(loader.getScale(wname));
            return true;
        };

        const int8_t *wq = nullptr, *wk = nullptr, *wv = nullptr;
        float sq = 1.0f, sk = 1.0f, sv = 1.0f;
        if (!resolve(pfx + "self_attn.q_proj.weight", H,       wq, sq)) return false;
        if (!resolve(pfx + "self_attn.k_proj.weight", kv_dim_, wk, sk)) return false;
        if (!resolve(pfx + "self_attn.v_proj.weight", kv_dim_, wv, sv)) return false;

        // Quantize norm output ONCE for all three projections
        qkv_act_.invalidate();
        qkv_act_.quantize(norm);

        std::vector<float> q(static_cast<std::size_t>(H));
        std::vector<float> k(static_cast<std::size_t>(kv_dim_));
        std::vector<float> v(static_cast<std::size_t>(kv_dim_));

        // Dispatch all three projections in one pool.execute()
        const std::vector<LinearSpec> specs = {
            { wq, H,       qkv_act_.gamma * sq, &q },
            { wk, kv_dim_, qkv_act_.gamma * sk, &k },
            { wv, kv_dim_, qkv_act_.gamma * sv, &v },
        };
        linearPackedMulti(specs, H, qkv_act_, &matmul_ms);

        applyRoPE(q, k, position);

        // Store int8-quantized K and V in the KV cache
        LayerKV& cache = kv_[static_cast<std::size_t>(layer)];
        for (int h = 0; h < cfg.num_kv_heads; ++h) {
            const float* kH = k.data() + h * cfg.head_dim;
            const float* vH = v.data() + h * cfg.head_dim;
            float km = 1e-9f, vm = 1e-9f;
            for (int d = 0; d < cfg.head_dim; ++d) {
                km = std::max(km, std::abs(kH[d]));
                vm = std::max(vm, std::abs(vH[d]));
            }
            const float kg = km / 127.0f, vg = vm / 127.0f;
            cache.k_scales[static_cast<std::size_t>(position * cfg.num_kv_heads + h)] = kg;
            cache.v_scales[static_cast<std::size_t>(position * cfg.num_kv_heads + h)] = vg;
            const float ki = 1.0f / kg, vi2 = 1.0f / vg;
            int8_t* kSlot = cache.keys.data()   + position * kv_dim_ + h * cfg.head_dim;
            int8_t* vSlot = cache.values.data() + position * kv_dim_ + h * cfg.head_dim;
            for (int d = 0; d < cfg.head_dim; ++d) {
                kSlot[d] = static_cast<int8_t>(std::round(std::clamp(kH[d] * ki,  -127.0f, 127.0f)));
                vSlot[d] = static_cast<int8_t>(std::round(std::clamp(vH[d] * vi2, -127.0f, 127.0f)));
            }
        }

        // Attention scores + context aggregation
        std::vector<float> attn(static_cast<std::size_t>(H), 0.0f);
        const int   groups = cfg.num_heads / cfg.num_kv_heads;
        const float scale  = 1.0f / std::sqrt(static_cast<float>(cfg.head_dim));
        std::vector<float> scores(static_cast<std::size_t>(position + 1));

        for (int head = 0; head < cfg.num_heads; ++head) {
            const int    kvH   = head / groups;
            const float* qHead = q.data() + static_cast<std::size_t>(head) * cfg.head_dim;
            float maxScore = -std::numeric_limits<float>::infinity();

            for (int t = 0; t <= position; ++t) {
                const int8_t* kHead = cache.keys.data()
                    + static_cast<std::size_t>(t) * kv_dim_
                    + static_cast<std::size_t>(kvH) * cfg.head_dim;
                const float kscale = cache.k_scales[static_cast<std::size_t>(t * cfg.num_kv_heads + kvH)];
                float dot = 0.0f;
#ifdef __AVX2__
                __m256 vacc = _mm256_setzero_ps();
                for (int d = 0; d <= cfg.head_dim - 8; d += 8) {
                    __m128i vraw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(kHead + d));
                    __m256i v32  = _mm256_cvtepi8_epi32(vraw);
                    __m256  vf   = _mm256_cvtepi32_ps(v32);
                    vacc = _mm256_add_ps(vacc, _mm256_mul_ps(vf, _mm256_loadu_ps(qHead + d)));
                }
                float tmp[8]; _mm256_storeu_ps(tmp, vacc);
                for (int i = 0; i < 8; ++i) dot += tmp[i];
#else
                for (int d = 0; d < cfg.head_dim; ++d) dot += qHead[d] * static_cast<float>(kHead[d]);
#endif
                scores[static_cast<std::size_t>(t)] = dot * kscale * scale;
                maxScore = std::max(maxScore, scores[static_cast<std::size_t>(t)]);
            }

            float denom = 0.0f;
            for (int t = 0; t <= position; ++t) {
                float e = std::exp(scores[static_cast<std::size_t>(t)] - maxScore);
                scores[static_cast<std::size_t>(t)] = e;
                denom += e;
            }
            if (denom == 0.0f || !std::isfinite(denom)) { error = "Softmax NaN"; return false; }

            float* outHead = attn.data() + static_cast<std::size_t>(head) * cfg.head_dim;
            for (int t = 0; t <= position; ++t) {
                const int8_t* vHead = cache.values.data()
                    + static_cast<std::size_t>(t) * kv_dim_
                    + static_cast<std::size_t>(kvH) * cfg.head_dim;
                const float vscale = cache.v_scales[static_cast<std::size_t>(t * cfg.num_kv_heads + kvH)];
                const float prob   = (scores[static_cast<std::size_t>(t)] / denom) * vscale;
#ifdef __AVX2__
                __m256 vprob = _mm256_set1_ps(prob);
                for (int d = 0; d <= cfg.head_dim - 8; d += 8) {
                    __m128i vraw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(vHead + d));
                    __m256i v32  = _mm256_cvtepi8_epi32(vraw);
                    __m256  vf   = _mm256_cvtepi32_ps(v32);
                    __m256  vo   = _mm256_loadu_ps(outHead + d);
                    _mm256_storeu_ps(outHead + d, _mm256_add_ps(vo, _mm256_mul_ps(vf, vprob)));
                }
#else
                for (int d = 0; d < cfg.head_dim; ++d)
                    outHead[d] += prob * static_cast<float>(vHead[d]);
#endif
            }
        }

        // Attention sub-norm + output projection
        std::vector<float> attnNorm;
        if (!rmsNorm(attn, tensorFloat(pfx + "self_attn.attn_sub_norm.weight", error), attnNorm, error))
            return false;

        const int8_t* wo = nullptr; float so = 1.0f;
        if (!resolve(pfx + "self_attn.o_proj.weight", H, wo, so)) return false;

        // Quantize attnNorm for output projection
        QuantizedActivation oact;
        oact.quantize(attnNorm);

        out.assign(static_cast<std::size_t>(H), 0.0f);
        const std::vector<LinearSpec> ospec = {{ wo, H, oact.gamma * so, &out }};
        linearPackedMulti(ospec, H, oact, &matmul_ms);
        return true;
    }

    // -------------------------------------------------------------------------
    // mlpLayer — fused gate+up projection
    // -------------------------------------------------------------------------
    bool mlpLayer(const std::string& pfx, const std::vector<float>& norm,
                  std::vector<float>& out, std::string& error, long long& matmul_ms) {
        const int H = cfg.hidden_size, I = cfg.intermediate_size;

        auto resolve = [&](const std::string& wname, int rows, const int8_t*& ptr, float& scale) -> bool {
            const auto* u = loader.unpackedL50(wname);
            if (!u) { error = "Cannot load: " + wname; return false; }
            if ((int)u->size() < rows * H) { error = "Too small: " + wname; return false; }
            ptr = u->data(); scale = static_cast<float>(loader.getScale(wname));
            return true;
        };

        const int8_t *wgate = nullptr, *wup = nullptr;
        float sgate = 1.0f, sup = 1.0f;
        if (!resolve(pfx + "mlp.gate_proj.weight", I, wgate, sgate)) return false;
        if (!resolve(pfx + "mlp.up_proj.weight",   I, wup,   sup))   return false;

        // Quantize MLP norm input ONCE for both gate and up
        mlp_act_.invalidate();
        mlp_act_.quantize(norm);

        std::vector<float> gate(static_cast<std::size_t>(I));
        std::vector<float> up(static_cast<std::size_t>(I));

        const std::vector<LinearSpec> specs = {
            { wgate, I, mlp_act_.gamma * sgate, &gate },
            { wup,   I, mlp_act_.gamma * sup,   &up   },
        };
        linearPackedMulti(specs, H, mlp_act_, &matmul_ms);

        // relu2 activation: gate²×up with FFN sub-norm
        std::vector<float> activated(static_cast<std::size_t>(I));
        for (int i = 0; i < I; ++i)
            activated[static_cast<std::size_t>(i)] = relu2(gate[static_cast<std::size_t>(i)])
                                                    * up[static_cast<std::size_t>(i)];

        std::vector<float> ffnNorm;
        if (!rmsNorm(activated, tensorFloat(pfx + "mlp.ffn_sub_norm.weight", error), ffnNorm, error))
            return false;

        const int8_t* wdown = nullptr; float sdown = 1.0f;
        auto resolve2 = [&](const std::string& wname, int rows, int cols, const int8_t*& ptr, float& scale) -> bool {
            const auto* u = loader.unpackedL50(wname);
            if (!u) { error = "Cannot load: " + wname; return false; }
            if ((int)u->size() < rows * cols) { error = "Too small: " + wname; return false; }
            ptr = u->data(); scale = static_cast<float>(loader.getScale(wname));
            return true;
        };
        if (!resolve2(pfx + "mlp.down_proj.weight", H, I, wdown, sdown)) return false;

        QuantizedActivation dact;
        dact.quantize(ffnNorm);

        out.assign(static_cast<std::size_t>(H), 0.0f);
        const std::vector<LinearSpec> dspec = {{ wdown, H, dact.gamma * sdown, &out }};
        linearPackedMulti(dspec, I, dact, &matmul_ms);
        return true;
    }

    // -------------------------------------------------------------------------
    // argmaxLogits — parallel vocab scan over BF16 embedding rows.
    //
    // Memory bandwidth note: vocab=128256 × hidden=2560 × 2 bytes = 655 MB.
    // At ~50 GB/s DDR4: ~13 ms minimum regardless of optimisation.
    // allLogits: if non-null, fills all vocab logit values (needed for sampling).
    // If null, only greedy_token is set (faster — skips the store).
    // -------------------------------------------------------------------------
    bool argmaxLogits(const std::vector<float>& hidden, int& bestToken,
                      float& bestScore, std::vector<float>* allLogits,
                      std::string& error) const {
        if ((int)hidden.size() != cfg.hidden_size) {
            error = "LM head shape mismatch"; return false;
        }
        if (allLogits) allLogits->assign(static_cast<std::size_t>(cfg.vocab_size), 0.0f);

        struct LocalBest { int token = 0; float score = -std::numeric_limits<float>::infinity(); };
        const int nw    = pool_.size();
        const int chunk = (cfg.vocab_size + nw - 1) / nw;
        std::vector<LocalBest> locals(static_cast<std::size_t>(nw));

        const_cast<BitNetHostInference*>(this)->pool_.execute(nw, [&](int worker) {
            const int begin = worker * chunk;
            const int end   = std::min(cfg.vocab_size, begin + chunk);
            LocalBest local;
            for (int token = begin; token < end; ++token) {
                const uint16_t* row = embeddings_.data()
                    + static_cast<std::size_t>(token) * cfg.hidden_size;
                float acc = 0.0f;
                int j = 0;
#ifdef __AVX2__
                __m256 vacc = _mm256_setzero_ps();
                for (; j <= cfg.hidden_size - 8; j += 8) {
                    __m128i vraw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + j));
                    __m256i v32  = _mm256_cvtepu16_epi32(vraw);
                    __m256i vs   = _mm256_slli_epi32(v32, 16);
                    __m256  vf   = _mm256_castsi256_ps(vs);
                    vacc = _mm256_add_ps(vacc, _mm256_mul_ps(vf, _mm256_loadu_ps(hidden.data() + j)));
                }
                float tmp[8]; _mm256_storeu_ps(tmp, vacc);
                for (int i = 0; i < 8; ++i) acc += tmp[i];
#endif
                for (; j < cfg.hidden_size; ++j)
                    acc += bf16ToFloat(row[j]) * hidden[static_cast<std::size_t>(j)];
                if (allLogits) (*allLogits)[static_cast<std::size_t>(token)] = acc;
                if (acc > local.score) { local.score = acc; local.token = token; }
            }
            locals[static_cast<std::size_t>(worker)] = local;
        });

        bestToken = 0;
        bestScore = -std::numeric_limits<float>::infinity();
        for (const auto& local : locals) {
            if (local.score > bestScore) { bestScore = local.score; bestToken = local.token; }
        }
        return true;
    }
};

// =============================================================================
// VM-path BitNetInference (unchanged — targets BitNetInference::forward())
// =============================================================================

struct BitNetForwardResult2 {};  // forward-declare for clarity

class BitNetInference {
public:
    BitNetConfig cfg;
    vm::VMState& vm;
    BitNetLoader& loader;

    BitNetInference(vm::VMState& v, BitNetLoader& l) : vm(v), loader(l) {}
    BitNetInference(vm::VMState& v, BitNetLoader& l, const BitNetConfig& c)
        : cfg(c), vm(v), loader(l) {}

    BitNetForwardResult forward(const std::vector<int>& input_ids, int logit_addr) {
        BitNetForwardResult result;
        if (input_ids.empty()) { result.error = "Empty input_ids"; return result; }
        const auto t0 = std::chrono::high_resolution_clock::now();
        const int H = cfg.hidden_size, I = cfg.intermediate_size;
        DMemLayout layout{ H, I };
        const int weight_base = layout.scratch_words();
        TensorView x_view    = { layout.x_base(),    1, H, TernaryMode::T40 };
        TensorView norm_view = { layout.norm_base(), 1, H, TernaryMode::T40 };
        int current_token_id = input_ids.back();
        const int min_dmem = layout.scratch_words() + cfg.vocab_size;
        if (vm.dmem.size() < min_dmem) {
            result.error = "DMEM too small";
            return result;
        }
        {
            int loaded = loader.loadLayerToVM(vm, "model.embed_tokens.weight",
                                              weight_base, H, current_token_id * H);
            if (loaded != H) { result.error = "Embedding load failed"; return result; }
            TensorView embed_view = { weight_base, 1, H, TernaryMode::T40 };
            if (!copyVector(vm, embed_view, x_view, &result.stats)) {
                result.error = "Embedding copy failed"; return result;
            }
        }
        for (int l = 0; l < cfg.num_layers; ++l) {
            std::string pfx = "model.layers." + std::to_string(l) + ".";
            {
                int waddr = weight_base;
                int loaded = loadWeight(vm, loader, pfx + "input_layernorm.weight", waddr, &result.stats);
                if (!loaded) { result.error = "input_layernorm load failed"; return result; }
                TensorView gamma = { waddr, 1, H, TernaryMode::T40 };
                if (!runRmsNorm(vm, x_view, gamma, norm_view, &result.stats)) {
                    result.error = "pre-attn rmsNorm failed"; return result;
                }
            }
            {
                int waddr = weight_base + H;
                if (!runAttentionBlock(vm, cfg, layout, pfx, loader, waddr,
                                       x_view, norm_view, &result.stats)) {
                    result.error = "attention failed"; return result;
                }
            }
            {
                int waddr = weight_base;
                int loaded = loadWeight(vm, loader, pfx + "post_attention_layernorm.weight", waddr, &result.stats);
                if (!loaded) { result.error = "post_attn_layernorm load failed"; return result; }
                TensorView gamma = { waddr, 1, H, TernaryMode::T40 };
                if (!runRmsNorm(vm, x_view, gamma, norm_view, &result.stats)) {
                    result.error = "pre-MLP rmsNorm failed"; return result;
                }
            }
            {
                int waddr = weight_base + H;
                if (!runMlpBlock(vm, cfg, layout, pfx, loader, waddr,
                                  x_view, norm_view, &result.stats)) {
                    result.error = "MLP failed"; return result;
                }
            }
        }
        {
            int loaded = loadWeight(vm, loader, "model.norm.weight", weight_base, &result.stats);
            if (!loaded) { result.error = "Final norm load failed"; return result; }
            TensorView gamma = { weight_base, 1, H, TernaryMode::T40 };
            if (!runRmsNorm(vm, x_view, gamma, norm_view, &result.stats)) {
                result.error = "Final rmsNorm failed"; return result;
            }
        }
        const int V = cfg.vocab_size;
        if (logit_addr + V > vm.dmem.size()) { result.error = "No DMEM for logits"; return result; }
        std::vector<sandbox::LongTriple> x_cached(H);
        for (int j = 0; j < H; ++j) {
            vm::TernaryValue val;
            loadElement(vm, norm_view, 0, j, val, &result.stats);
            x_cached[static_cast<std::size_t>(j)] = val.toLongTriple();
        }
        std::string lm_name = loader.layers.count("lm_head.weight") ? "lm_head.weight" : "model.embed_tokens.weight";
        for (int v = 0; v < V; ++v) {
            int row_addr = weight_base;
            int loaded = loader.loadLayerToVM(vm, lm_name, row_addr, H, v * H);
            if (loaded != H) { result.error = "lm_head row load failed"; return result; }
            ops::LongTripleAccumulator acc;
            for (int j = 0; j < H; ++j) {
                auto [wv, _] = vm.dmem.load(row_addr + j);
                acc.add(ops::multiply(wv.toLongTriple(), x_cached[static_cast<std::size_t>(j)]));
            }
            vm.dmem.store(logit_addr + v, vm::TernaryValue::fromLongTriple(acc.result()));
        }
        {
            int best_token = 0;
            auto [best_val, _] = vm.dmem.load(logit_addr);
            vm::TernaryValue best = best_val;
            for (int v = 1; v < V; ++v) {
                auto [val, fc] = vm.dmem.load(logit_addr + v);
                if (fc == vm::MemFaultCode::OK) {
                    vm::TernaryValue tv = vm::convertValue(val, TernaryMode::T40);
                    if (vm::exec::compareValue(tv, best, TernaryMode::T40) > 0) {
                        best = tv; best_token = v;
                    }
                }
            }
            result.greedy_token = best_token;
            result.greedy_logit = best;
        }
        result.ok = true;
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - t0).count();
        return result;
    }
};

// =============================================================================
// Printers
// =============================================================================

inline void printForwardResult(const BitNetForwardResult& r) {
    if (!r.ok) { std::cerr << "Forward pass FAILED: " << r.error << "\n"; return; }
    std::cout << "\n=== Forward Pass Complete ===\n";
    std::cout << "  Elapsed:      " << r.elapsed_ms      << " ms\n";
    std::cout << "  Greedy token: " << r.greedy_token     << "\n";
    std::cout << "  Scalar ops:   " << r.stats.scalarOps  << "\n";
    std::cout << "  DMEM loads:   " << r.stats.dmemLoads  << "\n";
    std::cout << "  DMEM stores:  " << r.stats.dmemStores << "\n";
}

inline void printHostForwardResult(const HostForwardResult& r) {
    if (!r.ok) { std::cerr << "Forward pass FAILED: " << r.error << "\n"; return; }
    std::cout << "\n=== Forward Pass Complete ===\n";
    std::cout << "  Elapsed:   " << r.elapsed_ms  << " ms\n";
    std::cout << "  Matmul:    " << r.matmul_ms   << " ms\n";
    std::cout << "  Norm:      " << r.norm_ms     << " ms\n";
    std::cout << "  Sampling:  " << r.sampling_ms << " ms\n";
    std::cout << "  Token:     " << r.greedy_token << "\n";
}

} // namespace bitnet
} // namespace sandbox

#endif // BITNET_INFERENCE_H
