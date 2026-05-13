// =============================================================================
// run_bitnet.cpp - BitNet b1.58 incremental inference entry point
// =============================================================================
//
// Fixes vs. previous version:
//   - result.matmul_ms / norm_ms / sampling_ms now exist in HostForwardResult
//   - computeAllLogits passed as false when temp==0 (skips 655 MB logit scan)
//   - Sampling uses only result.logits (pre-populated when computeAllLogits=true)
//   - Repetition penalty applied before softmax (correct order)
//   - Thread count resolved once; passed through to inference constructor
//   - Generation stats include per-token breakdown from last token

#include "bitnet_inference.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace sandbox;
using namespace sandbox::bitnet;
namespace fs = std::filesystem;

// =============================================================================
// Helpers
// =============================================================================

static std::string defaultSafetensorsPath(const std::string& weights_dir) {
    fs::path weightsPath(weights_dir);
    fs::path bitnetDir = weightsPath.parent_path();

    for (const fs::path& candidate : {
            bitnetDir / "model" / "model.safetensors",
            fs::path("bitnet_weights") / "model" / "model.safetensors",
            fs::path("model") / "model.safetensors",
    }) {
        if (fs::exists(candidate)) return candidate.string();
    }
    return (bitnetDir / "model" / "model.safetensors").string();
}

// Sample from a logit vector with temperature + top-p.
// Returns the selected token id.
static int sampleLogits(
    std::vector<float>& logits,   // modified in-place by temperature scaling
    float temp,
    float top_p,
    int   greedy_token,
    std::mt19937& rng)
{
    if (temp <= 0.0f) return greedy_token;  // greedy

    // Temperature scaling
    for (float& l : logits) l /= temp;

    // Softmax
    const float max_l = *std::max_element(logits.begin(), logits.end());
    std::vector<std::pair<float, int>> probs;
    probs.reserve(logits.size());
    float sum_exp = 0.0f;
    for (int j = 0; j < static_cast<int>(logits.size()); ++j) {
        const float e = std::exp(logits[static_cast<std::size_t>(j)] - max_l);
        probs.emplace_back(e, j);
        sum_exp += e;
    }
    for (auto& [p, _] : probs) p /= sum_exp;

    // Top-p nucleus truncation
    if (top_p < 1.0f) {
        std::sort(probs.begin(), probs.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        float cumulative = 0.0f;
        std::size_t cutoff = probs.size();
        for (std::size_t j = 0; j < probs.size(); ++j) {
            cumulative += probs[j].first;
            if (cumulative >= top_p) { cutoff = j + 1; break; }
        }
        probs.resize(cutoff);
        float new_sum = 0.0f;
        for (auto& [p, _] : probs) new_sum += p;
        for (auto& [p, _] : probs) p /= new_sum;
    }

    // Multinomial sample
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    float r = dist(rng);
    float cumulative = 0.0f;
    for (auto& [p, id] : probs) {
        cumulative += p;
        if (r <= cumulative) return id;
    }
    return probs.back().second;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    // ---- Parse arguments ----
    std::string weights_dir    = "../bitnet_weights/converted";
    std::string safetensors_path;
    std::vector<int> token_ids;
    int   max_new_tokens     = 64;
    int   threads            = -1;   // -1 → hardware_concurrency
    float temp               = 0.7f;
    float top_p              = 1.0f;
    float repetition_penalty = 1.1f;
    int   stop_token         = 128001; // <|end_of_text|> for Llama-3 tokenizer

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--model"   && i + 1 < argc) safetensors_path  = argv[++i];
        else if (arg == "--dir"     && i + 1 < argc) weights_dir        = argv[++i];
        else if (arg == "--tokens"  && i + 1 < argc) max_new_tokens     = std::atoi(argv[++i]);
        else if (arg == "--threads" && i + 1 < argc) threads            = std::atoi(argv[++i]);
        else if (arg == "--temp"    && i + 1 < argc) temp               = std::atof(argv[++i]);
        else if (arg == "--top_p"   && i + 1 < argc) top_p              = std::atof(argv[++i]);
        else if (arg == "--penalty" && i + 1 < argc) repetition_penalty = std::atof(argv[++i]);
        else if (arg == "--stop"    && i + 1 < argc) stop_token         = std::atoi(argv[++i]);
        // Legacy flag name
        else if (arg == "--safetensors" && i + 1 < argc) safetensors_path = argv[++i];
        else {
            try { token_ids.push_back(std::stoi(arg)); }
            catch (...) { std::cerr << "Unknown arg or non-integer token: " << arg << "\n"; }
        }
    }
    if (token_ids.empty()) token_ids.push_back(128000); // <|begin_of_text|>
    if (safetensors_path.empty()) safetensors_path = defaultSafetensorsPath(weights_dir);

    // Whether we need the full logit vector (sampling) or just greedy token
    const bool need_logits = (temp > 0.0f);

    // ---- Banner ----
    std::cout << "=== BitNet b1.58 Incremental Inference ===\n";
    std::cout << "Weights dir:  " << weights_dir     << "\n";
    std::cout << "Safetensors:  " << safetensors_path << "\n";
    std::cout << "Prompt tokens:";
    for (int id : token_ids) std::cout << " " << id;
    std::cout << "\n";
    std::cout << "Temperature:  " << temp << "  top_p: " << top_p
              << "  penalty: " << repetition_penalty << "\n\n";

    // ---- Load manifest ----
    BitNetLoader loader(weights_dir);
    if (loader.layers.empty()) {
        std::cerr << "Error: No layers in " << weights_dir << "/manifest.csv\n";
        return 1;
    }
    std::cout << "Manifest: " << loader.layers.size() << " layers loaded.\n";

    // ---- Model config for microsoft/bitnet-b1.58-2B-4T ----
    BitNetConfig cfg;
    cfg.hidden_size             = 2560;
    cfg.num_heads               = 20;
    cfg.num_kv_heads            = 5;    // GQA: groups = 20/5 = 4
    cfg.head_dim                = 128;  // 2560 / 20
    cfg.intermediate_size       = 6912;
    cfg.num_layers              = 30;
    cfg.vocab_size              = 128256;
    cfg.max_position_embeddings = 4096;
    cfg.rms_norm_eps            = 1.0e-5f;
    cfg.rope_theta              = 500000.0f;

    const int actual_threads = (threads > 0)
        ? threads
        : static_cast<int>(std::thread::hardware_concurrency() > 0
            ? std::thread::hardware_concurrency() : 4u);

    std::cout << "Workers:      " << actual_threads << "\n\n";

    // ---- Construct and initialise inference engine ----
    BitNetHostInference inference(loader, cfg, actual_threads);

    std::string init_error;
    if (!inference.init(safetensors_path, init_error)) {
        std::cerr << "Init failed: " << init_error << "\n";
        return 1;
    }

    // ---- Prefill prompt ----
    std::cout << "Prefilling " << token_ids.size() << " prompt token(s)...\n";
    HostForwardResult result;
    for (std::size_t i = 0; i < token_ids.size(); ++i) {
        // Only compute logits on the last prompt token — that is what we sample from.
        const bool last = (i + 1 == token_ids.size());
        result = inference.evalToken(token_ids[i], last && need_logits);
        if (!result.ok) {
            std::cerr << "Prefill failed at token " << i << ": " << result.error << "\n";
            return 1;
        }
        if (last) {
            std::cout << "Prefill complete. Elapsed: " << result.elapsed_ms << " ms"
                      << "  (matmul=" << result.matmul_ms
                      << "ms  norm=" << result.norm_ms
                      << "ms  sampling=" << result.sampling_ms << "ms)\n\n";
        }
    }

    // ---- Autoregressive generation ----
    std::cout << "Generating (max " << max_new_tokens << " tokens)...\n";
    std::deque<int> recent_tokens; // for repetition penalty window
    std::mt19937 rng(42);

    // Accumulate timing stats across generated tokens
    long long total_elapsed_ms  = 0;
    long long total_matmul_ms   = 0;
    long long total_norm_ms     = 0;
    long long total_sampling_ms = 0;
    int generated_count = 0;

    const auto gen_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < max_new_tokens; ++i) {

        // ---- Apply repetition penalty to logit vector ----
        if (repetition_penalty != 1.0f && !recent_tokens.empty() && !result.logits.empty()) {
            // Build set of recently seen tokens (deduped for efficiency)
            std::map<int, bool> seen;
            for (int t : recent_tokens) seen[t] = true;
            for (auto& [token, _] : seen) {
                if (static_cast<std::size_t>(token) < result.logits.size()) {
                    float& l = result.logits[static_cast<std::size_t>(token)];
                    l = (l > 0.0f) ? l / repetition_penalty : l * repetition_penalty;
                }
            }
        }

        // ---- Sample or pick greedily ----
        const int next_token = need_logits
            ? sampleLogits(result.logits, temp, top_p, result.greedy_token, rng)
            : result.greedy_token;

        // ---- Emit token to stdout for chat_bridge.py ----
        // Line 1: diagnostic (timing + cache size)
        std::cout << "Generated token: " << next_token
                  << " (cache=" << inference.cachedTokens() << ")"
                  << " [matmul=" << result.matmul_ms
                  << "ms norm=" << result.norm_ms
                  << "ms sampling=" << result.sampling_ms << "ms]\n" << std::flush;
        // Line 2: the token id for the Python bridge to decode
        std::cout << "Next predicted token: " << next_token << "\n" << std::flush;

        if (next_token == stop_token) {
            std::cout << "[Stop token reached]\n";
            break;
        }

        // Update repetition window
        recent_tokens.push_back(next_token);
        if (recent_tokens.size() > 128) recent_tokens.pop_front();

        // ---- Forward pass for next token ----
        result = inference.evalToken(next_token, need_logits);
        if (!result.ok) {
            std::cerr << "Forward pass failed: " << result.error << "\n";
            return 1;
        }

        total_elapsed_ms  += result.elapsed_ms;
        total_matmul_ms   += result.matmul_ms;
        total_norm_ms     += result.norm_ms;
        total_sampling_ms += result.sampling_ms;
        ++generated_count;
    }

    const auto gen_end = std::chrono::high_resolution_clock::now();
    const long long wall_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(gen_end - gen_start).count();

    // ---- Generation stats ----
    std::cout << "\n=== Generation Stats ===\n";
    std::cout << "  Tokens generated:  " << generated_count << "\n";
    std::cout << "  Wall time:         " << wall_ms << " ms\n";
    if (generated_count > 0) {
        const double tps = generated_count * 1000.0 / static_cast<double>(wall_ms > 0 ? wall_ms : 1);
        std::cout << "  Throughput:        " << tps << " tokens/s\n";
        std::cout << "  Avg per token:     " << (total_elapsed_ms  / generated_count) << " ms\n";
        std::cout << "    matmul:          " << (total_matmul_ms   / generated_count) << " ms\n";
        std::cout << "    norm:            " << (total_norm_ms     / generated_count) << " ms\n";
        std::cout << "    sampling/logits: " << (total_sampling_ms / generated_count) << " ms\n";

        // Flag known bottlenecks
        const long long avg_total   = total_elapsed_ms  / generated_count;
        const long long avg_matmul  = total_matmul_ms   / generated_count;
        const long long avg_sample  = total_sampling_ms / generated_count;

        if (avg_matmul  > avg_total * 6 / 10)
            std::cout << "  NOTE: matmul dominant — check -O3 -mavx2 in compiler flags\n";
        if (avg_sample  > 15)
            std::cout << "  NOTE: logit scan >" << avg_sample << "ms — "
                         "expected for vocab=" << cfg.vocab_size << " (bandwidth bound)\n";
    }
    std::cout << "========================\n" << std::flush;

    return 0;
}
