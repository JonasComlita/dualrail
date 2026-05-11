// =============================================================================
// run_bitnet.cpp - BitNet b1.58 incremental inference entry point
// =============================================================================

#include "bitnet_weights/bitnet_inference.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <random>
#include <algorithm>
#include <map>
#include <deque>

using namespace sandbox;
using namespace sandbox::bitnet;
namespace fs = std::filesystem;

static std::string defaultSafetensorsPath(const std::string& weights_dir) {
    fs::path weightsPath(weights_dir);
    fs::path bitnetDir = weightsPath.parent_path();
    fs::path candidate = bitnetDir / "model" / "model.safetensors";
    if (fs::exists(candidate)) return candidate.string();

    candidate = fs::path("bitnet_weights") / "model" / "model.safetensors";
    if (fs::exists(candidate)) return candidate.string();

    return (bitnetDir / "model" / "model.safetensors").string();
}

int main(int argc, char* argv[]) {
    std::string weights_dir = "../bitnet_weights/converted";
    std::string safetensors_path;
    std::vector<int> token_ids;
    int max_new_tokens = 64;
    int threads = -1;
    float temp = 1.0f;
    float top_p = 1.0f;
    float repetition_penalty = 1.1f;
    int stop_token = 128001; // <|end_of_text|> for Llama3 base

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) safetensors_path = argv[++i];
        else if (arg == "--dir" && i + 1 < argc) weights_dir = argv[++i];
        else if (arg == "--tokens" && i + 1 < argc) max_new_tokens = std::atoi(argv[++i]);
        else if (arg == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (arg == "--temp" && i + 1 < argc) temp = std::atof(argv[++i]);
        else if (arg == "--top_p" && i + 1 < argc) top_p = std::atof(argv[++i]);
        else if (arg == "--penalty" && i + 1 < argc) repetition_penalty = std::atof(argv[++i]);
        else if (arg == "--stop" && i + 1 < argc) stop_token = std::atoi(argv[++i]);
        else if (arg == "--safetensors" && i + 1 < argc) safetensors_path = argv[++i];
        else token_ids.push_back(std::stoi(arg));
    }
    if (token_ids.empty()) token_ids.push_back(128000); // <|begin_of_text|>
    if (safetensors_path.empty()) safetensors_path = defaultSafetensorsPath(weights_dir);

    std::cout << "=== BitNet b1.58 Incremental Inference ===\n";
    std::cout << "Weights:      " << weights_dir << "\n";
    std::cout << "Safetensors:  " << safetensors_path << "\n";
    std::cout << "Input tokens: ";
    for (int id : token_ids) std::cout << id << " ";
    std::cout << "\n\n";

    BitNetLoader loader(weights_dir);
    if (loader.layers.empty()) {
        std::cerr << "Error: No layers found in " << weights_dir
                  << "/manifest.csv\n";
        return 1;
    }
    std::cout << "Manifest loaded: " << loader.layers.size() << " layers.\n\n";

    BitNetConfig cfg;
    cfg.hidden_size       = 2560;
    cfg.num_heads         = 20;
    cfg.num_kv_heads      = 5;
    cfg.head_dim          = 128;
    cfg.intermediate_size = 6912;
    cfg.num_layers        = 30;
    cfg.vocab_size        = 128256;
    cfg.max_position_embeddings = 4096;
    cfg.rms_norm_eps      = 1.0e-5f;
    cfg.rope_theta        = 500000.0f;

    const int actual_threads = (threads > 0 ? threads : (std::thread::hardware_concurrency() > 0 ? static_cast<int>(std::thread::hardware_concurrency()) : 1));
    BitNetHostInference inference(loader, cfg, actual_threads);

    std::string init_error;
    if (!inference.init(safetensors_path, init_error)) {
        std::cerr << "Initialization failed: " << init_error << "\n";
        return 1;
    }

    std::cout << "Prefilling prompt...\n";
    BitNetHostForwardResult result;
    for (std::size_t i = 0; i < token_ids.size(); ++i) {
        result = inference.evalToken(token_ids[i], i + 1 == token_ids.size());
        if (!result.ok) return 1;
    }

    std::cout << "Generating response with KV cache...\n";
    std::deque<int> history;
    std::mt19937 gen(42);

    const auto gen_start = std::chrono::high_resolution_clock::now();
    int generated_count = 0;
    for (int i = 0; i < max_new_tokens; ++i) {
        std::vector<float>& logits = result.logits;
        
        // Sampling logic omitted for brevity in chunk but remains...
        // [Existing Sampling Code]

        // Apply repetition penalty
        if (repetition_penalty != 1.0f) {
            std::map<int, bool> seen;
            for (int h : history) seen[h] = true;
            for (auto const& [token, _] : seen) {
                if (logits[token] > 0) logits[token] /= repetition_penalty;
                else logits[token] *= repetition_penalty;
            }
        }

        int next_token = result.greedy_token;
        if (temp > 0.0f) {
            for (float& l : logits) l /= temp;
            struct TokenProb { int id; float prob; };
            std::vector<TokenProb> probs(logits.size());
            float max_l = *std::max_element(logits.begin(), logits.end());
            float sum_exp = 0.0f;
            for (size_t j = 0; j < logits.size(); ++j) {
                probs[j] = { (int)j, std::exp(logits[j] - max_l) };
                sum_exp += probs[j].prob;
            }
            for (auto& p : probs) p.prob /= sum_exp;

            if (top_p < 1.0f) {
                std::sort(probs.begin(), probs.end(), [](const TokenProb& a, const TokenProb& b) {
                    return a.prob > b.prob;
                });
                float cumulative = 0.0f;
                int cutoff = (int)probs.size();
                for (int j = 0; j < (int)probs.size(); ++j) {
                    cumulative += probs[j].prob;
                    if (cumulative >= top_p) {
                        cutoff = j + 1;
                        break;
                    }
                }
                probs.resize(cutoff);
                float new_sum = 0.0f;
                for (auto& p : probs) new_sum += p.prob;
                for (auto& p : probs) p.prob /= new_sum;
            }
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);
            float r = dist(gen);
            float cumulative = 0.0f;
            for (auto& p : probs) {
                cumulative += p.prob;
                if (r <= cumulative) {
                    next_token = p.id;
                    break;
                }
            }
        }

        std::cout << "Generated token: " << next_token << " (sampled, cache " << inference.cachedTokens() << ")\n" << std::flush;
        std::cout << "Next predicted token: " << next_token << "\n" << std::flush;
        if (next_token == stop_token) break;

        history.push_back(next_token);
        if (history.size() > 128) history.pop_front();

        result = inference.evalToken(next_token, true);
        if (!result.ok) {
            std::cerr << "Forward pass failed: " << result.error << "\n";
            return 1;
        }
        generated_count++;
    }
    const auto gen_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(gen_end - gen_start).count();
    std::cout << "\n=== Generation Stats ===\n";
    std::cout << "  Tokens:         " << generated_count << "\n";
    std::cout << "  Total time:     " << total_ms << " ms\n";
    if (total_ms > 0) {
        std::cout << "  Throughput:     " << (generated_count * 1000.0 / total_ms) << " tokens/s\n";
    }
    std::cout << "========================\n" << std::flush;

    return 0;
}
