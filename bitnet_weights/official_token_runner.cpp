// Token-level deterministic runner for the pinned llama.cpp reference build.
//
// The normal completion executable exposes generated text, which is not
// always losslessly re-tokenizable for arbitrary model revisions.  This small
// adapter uses the same pinned llama.cpp API and greedy sampler but emits the
// actual generated token IDs, keeping external reference evidence exact.

#include "llama.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(const char * name) {
    std::cerr << "usage: " << name
              << " --model MODEL --prompt TEXT --length N --threads N"
              << " [--trace-path PATH] [--node-trace-path PATH]\n";
}

bool valueAfter(int & index, int argc, char ** argv, std::string & value) {
    if (index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

bool integerAfter(int & index, int argc, char ** argv, int & value) {
    std::string raw;
    if (!valueAfter(index, argc, argv, raw)) return false;
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(raw, &consumed);
        if (consumed != raw.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

void writeTrace(std::ofstream & trace, const float * logits, int vocabSize, int step) {
    if (!trace || logits == nullptr || vocabSize <= 0) return;
    std::vector<int> order(static_cast<std::size_t>(vocabSize));
    for (int token = 0; token < vocabSize; ++token) order[static_cast<std::size_t>(token)] = token;
    const int keep = std::min(8, vocabSize);
    std::partial_sort(order.begin(), order.begin() + keep, order.end(),
        [logits](int left, int right) {
            if (logits[left] != logits[right]) return logits[left] > logits[right];
            return left < right;
        });
    trace << "step " << step << " top:";
    trace << std::setprecision(9);
    for (int rank = 0; rank < keep; ++rank) {
        const int token = order[static_cast<std::size_t>(rank)];
        trace << " " << token << ":" << logits[token];
    }
    trace << "\n";
}

struct NodeTraceState {
    std::ofstream * output = nullptr;
    int decode = 0;
};

bool nodeTraceCallback(ggml_tensor * tensor, bool ask, void * userData) {
    auto * state = static_cast<NodeTraceState *>(userData);
    if (state == nullptr || state->output == nullptr || tensor == nullptr) return true;
    const std::string name = tensor->name;
    const bool interesting =
        name.find("Qcur") != std::string::npos ||
        name.find("Kcur") != std::string::npos ||
        name.find("Vcur") != std::string::npos ||
        name.find("kqv_out") != std::string::npos ||
        name.find("attn_norm") != std::string::npos ||
        name.find("attn_sub_norm") != std::string::npos ||
        name.find("attn_out") != std::string::npos ||
        name.find("ffn_norm") != std::string::npos ||
        name.find("ffn_silu") != std::string::npos ||
        name.find("ffn_swiglu") != std::string::npos ||
        name.find("ffn_sub_norm") != std::string::npos ||
        name.find("ffn_down") != std::string::npos ||
        name.find("l_out") != std::string::npos ||
        name.find("result_norm") != std::string::npos ||
        name.find("result_output") != std::string::npos;
    if (!interesting) return false;
    if (ask) return true;

    auto & output = *state->output;
    const int64_t count = ggml_nelements(tensor);
    const int64_t keep = std::min<int64_t>(8, count);
    const char * data = static_cast<const char *>(tensor->data);
    const char * last = data;
    if (tensor->ne[1] > 1) {
        last += static_cast<std::ptrdiff_t>(tensor->ne[1] - 1) * tensor->nb[1];
    }
    output << "decode " << state->decode << " node " << name
           << " type " << ggml_type_name(tensor->type)
           << " count " << count << " values:" << std::setprecision(9);
    for (int64_t index = 0; index < keep; ++index) {
        float value = 0.0f;
        if (tensor->type == GGML_TYPE_F32) {
            value = static_cast<const float *>(tensor->data)[index];
        } else if (tensor->type == GGML_TYPE_F16) {
            value = ggml_fp16_to_fp32(static_cast<const ggml_fp16_t *>(tensor->data)[index]);
        } else {
            output << " unsupported";
            break;
        }
        output << " " << value;
    }
    output << " last_values:";
    for (int64_t index = 0; index < keep; ++index) {
        float value = 0.0f;
        if (tensor->type == GGML_TYPE_F32) {
            value = reinterpret_cast<const float *>(last)[index];
        } else if (tensor->type == GGML_TYPE_F16) {
            value = ggml_fp16_to_fp32(reinterpret_cast<const ggml_fp16_t *>(last)[index]);
        } else {
            output << " unsupported";
            break;
        }
        output << " " << value;
    }
    if (std::getenv("TRIT_BITNET_NODE_TRACE_ALL") != nullptr && tensor->ne[1] > 0) {
        output << " all_last_values:";
        const int64_t lastCount = tensor->ne[0];
        for (int64_t index = 0; index < lastCount; ++index) {
            float value = 0.0f;
            if (tensor->type == GGML_TYPE_F32) {
                value = reinterpret_cast<const float *>(last)[index];
            } else if (tensor->type == GGML_TYPE_F16) {
                value = ggml_fp16_to_fp32(reinterpret_cast<const ggml_fp16_t *>(last)[index]);
            }
            output << " " << value;
        }
    }
    if (std::getenv("TRIT_BITNET_NODE_TRACE_MATRIX") != nullptr &&
            (name.find("Kcur-5") != std::string::npos ||
             name.find("Vcur-5") != std::string::npos ||
             name.find("l_out-4") != std::string::npos ||
             name.find("attn_norm-5") != std::string::npos)) {
        output << " all_tensor_values:";
        for (int64_t column = 0; column < tensor->ne[1]; ++column) {
            const char * columnData = data + column * tensor->nb[1];
            for (int64_t row = 0; row < tensor->ne[0]; ++row) {
                float value = 0.0f;
                if (tensor->type == GGML_TYPE_F32) {
                    value = reinterpret_cast<const float *>(columnData)[row];
                } else if (tensor->type == GGML_TYPE_F16) {
                    value = ggml_fp16_to_fp32(
                        reinterpret_cast<const ggml_fp16_t *>(columnData)[row]);
                }
                output << " " << value;
            }
        }
    }
    output << "\n";
    return true;
}

} // namespace

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    std::string model_path;
    std::string prompt;
    std::string trace_path;
    std::string node_trace_path;
    int length = 0;
    int threads = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--model" || option == "-m") {
            if (!valueAfter(index, argc, argv, model_path)) {
                usage(argv[0]);
                return 2;
            }
        } else if (option == "--prompt" || option == "-p") {
            if (!valueAfter(index, argc, argv, prompt)) {
                usage(argv[0]);
                return 2;
            }
        } else if (option == "--length" || option == "--predict" || option == "-n") {
            if (!integerAfter(index, argc, argv, length)) {
                usage(argv[0]);
                return 2;
            }
        } else if (option == "--threads" || option == "-t") {
            if (!integerAfter(index, argc, argv, threads)) {
                usage(argv[0]);
                return 2;
            }
        } else if (option == "--trace-path") {
            if (!valueAfter(index, argc, argv, trace_path)) {
                usage(argv[0]);
                return 2;
            }
        } else if (option == "--node-trace-path") {
            if (!valueAfter(index, argc, argv, node_trace_path)) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (model_path.empty() || prompt.empty() || length <= 0 || threads <= 0) {
        usage(argv[0]);
        return 2;
    }

    std::ofstream trace;
    if (!trace_path.empty()) {
        trace.open(trace_path, std::ios::binary | std::ios::trunc);
        if (!trace) {
            std::cerr << "official token runner: unable to open trace path\n";
            return 2;
        }
    }
    std::ofstream node_trace;
    NodeTraceState node_trace_state;
    if (!node_trace_path.empty()) {
        node_trace.open(node_trace_path, std::ios::binary | std::ios::trunc);
        if (!node_trace) {
            std::cerr << "official token runner: unable to open node trace path\n";
            return 2;
        }
        node_trace_state.output = &node_trace;
    }

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0;
    llama_model * model = llama_model_load_from_file(model_path.c_str(), model_params);
    if (model == nullptr) {
        std::cerr << "official token runner: unable to load model\n";
        return 1;
    }

    const llama_vocab * vocab = llama_model_get_vocab(model);
    const int prompt_size = -llama_tokenize(
        vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    if (prompt_size <= 0) {
        std::cerr << "official token runner: unable to size prompt tokenization\n";
        llama_model_free(model);
        return 1;
    }
    std::vector<llama_token> prompt_tokens(static_cast<std::size_t>(prompt_size));
    if (llama_tokenize(
            vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), prompt_tokens.data(),
            prompt_size, true, true) < 0) {
        std::cerr << "official token runner: unable to tokenize prompt\n";
        llama_model_free(model);
        return 1;
    }

    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx = static_cast<uint32_t>(prompt_tokens.size() + length);
    context_params.n_batch = static_cast<uint32_t>(prompt_tokens.size());
    context_params.n_threads = threads;
    context_params.n_threads_batch = threads;
    context_params.cb_eval = node_trace_path.empty() ? nullptr : nodeTraceCallback;
    context_params.cb_eval_user_data = node_trace_path.empty() ? nullptr : &node_trace_state;
    llama_context * context = llama_init_from_model(model, context_params);
    if (context == nullptr) {
        std::cerr << "official token runner: unable to create context\n";
        llama_model_free(model);
        return 1;
    }

    llama_sampler_chain_params sampler_params = llama_sampler_chain_default_params();
    llama_sampler * sampler = llama_sampler_chain_init(sampler_params);
    if (sampler == nullptr) {
        std::cerr << "official token runner: unable to create sampler\n";
        llama_free(context);
        llama_model_free(model);
        return 1;
    }
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
    node_trace_state.decode = 0;
    if (llama_decode(context, batch) != 0) {
        std::cerr << "official token runner: prompt evaluation failed\n";
        llama_sampler_free(sampler);
        llama_free(context);
        llama_model_free(model);
        return 1;
    }

    writeTrace(trace, llama_get_logits_ith(context, -1),
               llama_vocab_n_tokens(vocab), 0);

    std::vector<llama_token> generated;
    generated.reserve(static_cast<std::size_t>(length));
    for (int index = 0; index < length; ++index) {
        const llama_token token = llama_sampler_sample(sampler, context, -1);
        if (llama_vocab_is_eog(vocab, token)) {
            std::cerr << "official token runner: model ended before requested length\n";
            llama_sampler_free(sampler);
            llama_free(context);
            llama_model_free(model);
            return 1;
        }
        generated.push_back(token);
        llama_sampler_accept(sampler, token);
        batch = llama_batch_get_one(&generated.back(), 1);
        node_trace_state.decode = index + 1;
        if (index + 1 < length && llama_decode(context, batch) != 0) {
            std::cerr << "official token runner: generation evaluation failed\n";
            llama_sampler_free(sampler);
            llama_free(context);
            llama_model_free(model);
            return 1;
        }
        if (index + 1 < length) {
            writeTrace(trace, llama_get_logits_ith(context, -1),
                       llama_vocab_n_tokens(vocab), index + 1);
        }
    }

    std::cout << "input_token_ids:";
    for (const llama_token token : prompt_tokens) std::cout << " " << token;
    std::cout << "\ntoken_ids:";
    for (const llama_token token : generated) std::cout << " " << token;
    std::cout << "\n";

    llama_sampler_free(sampler);
    llama_free(context);
    llama_model_free(model);
    return 0;
}
