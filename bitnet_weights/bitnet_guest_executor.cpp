// Fixed-protocol official BitNet executor used by the manual external gate.
//
// The executable consumes only a provenance-locked safetensors payload and a
// tensor-by-tensor converted directory.  It deliberately has no synthetic
// fallback: a missing layer, malformed manifest, ABI-v3 failure, or token
// mismatch is a failed adapter invocation and never becomes a benchmark pass.

#include "bitnet_inference.h"
#include "bitnet_guest_vfs.h"
#include "executable_header_v3.h"
#include "external_asset_support.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using sandbox::bitnet::BitNetConfig;
using sandbox::bitnet::BitNetHostInference;
using sandbox::bitnet::BitNetLoader;
using sandbox::bitnet::external_assets::Sha256;

struct Arguments {
    std::string profile;
    std::string format;
    std::filesystem::path model;
    std::filesystem::path converted_dir;
    std::filesystem::path reference_evidence;
    std::filesystem::path output;
    std::vector<int> input_token_ids;
    std::vector<int> expected_token_ids;
    int threads = 4;
    int length = 32;
};

std::string jsonEscape(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 2);
    for (unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                const char hex[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hex[character >> 4]);
                output.push_back(hex[character & 0x0f]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    return output;
}

void writeIntArray(std::ostringstream& out, const std::vector<int>& values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out << ',';
        out << values[index];
    }
    out << ']';
}

bool parseTokenList(const std::string& text, std::vector<int>& values,
                    std::string& error) {
    values.clear();
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() &&
               (text[cursor] == ',' || text[cursor] == ' ' ||
                text[cursor] == '\t' || text[cursor] == '\r' ||
                text[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor == text.size()) break;
        if (text[cursor] < '0' || text[cursor] > '9') {
            error = "token list contains a non-negative integer error";
            return false;
        }
        long long value = 0;
        while (cursor < text.size() && text[cursor] >= '0' &&
               text[cursor] <= '9') {
            value = value * 10 + (text[cursor] - '0');
            if (value > 2147483647LL) {
                error = "token ID exceeds the supported integer range";
                return false;
            }
            ++cursor;
        }
        values.push_back(static_cast<int>(value));
        if (cursor < text.size() && text[cursor] != ',' &&
            text[cursor] != ' ' && text[cursor] != '\t' &&
            text[cursor] != '\r' && text[cursor] != '\n') {
            error = "token list has an invalid separator";
            return false;
        }
    }
    return !values.empty();
}

bool parsePositive(const std::string& text, int& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed, 10);
        if (consumed != text.size() || parsed <= 0) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseArguments(int argc, char** argv, Arguments& args, std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        auto next = [&](std::string& value) {
            if (index + 1 >= argc) return false;
            value = argv[++index];
            return true;
        };
        std::string value;
        if (name == "--profile" && next(value)) args.profile = value;
        else if (name == "--format" && next(value)) args.format = value;
        else if (name == "--model" && next(value)) args.model = value;
        else if (name == "--converted-dir" && next(value)) args.converted_dir = value;
        else if (name == "--reference-evidence" && next(value)) args.reference_evidence = value;
        else if (name == "--output" && next(value)) args.output = value;
        else if (name == "--input-token-ids" && next(value) &&
                 parseTokenList(value, args.input_token_ids, error)) {}
        else if (name == "--expected-token-ids" && next(value) &&
                 parseTokenList(value, args.expected_token_ids, error)) {}
        else if (name == "--threads" && next(value) && parsePositive(value, args.threads)) {}
        else if (name == "--length" && next(value) && parsePositive(value, args.length)) {}
        else {
            if (error.empty()) error = "invalid or incomplete argument: " + name;
            return false;
        }
    }
    if (args.profile.empty() || args.format.empty() || args.model.empty() ||
        args.converted_dir.empty() || args.output.empty() ||
        args.input_token_ids.empty() ||
        (args.profile != "official-slice" && args.expected_token_ids.empty())) {
        error = "profile, format, model, converted-dir, input token IDs, and output are required";
        return false;
    }
    if (args.profile != "official-slice" &&
        static_cast<int>(args.expected_token_ids.size()) != args.length) {
        error = "expected token vector length does not match --length";
        return false;
    }
    return true;
}

bool verifySliceEmbeddingRows(const Arguments& args,
                              const sandbox::bitnet::ReadOnlyGuestVfs& guest_vfs,
                              std::string& error) {
    sandbox::bitnet::SafeTensorReader source;
    if (!source.open(args.model.string(), error)) return false;
    const auto* info = source.tensor("model.embed_tokens.weight");
    if (!info || info->dtype != "BF16" || info->shape.size() != 2 ||
        info->shape[1] != 2560) {
        error = "official-slice source embedding has an unexpected shape";
        return false;
    }

    std::ifstream input;
    if (!guest_vfs.open("model_embed_tokens_weight.bf16", input)) {
        error = "official-slice converted embedding rows are missing";
        return false;
    }
    const std::uintmax_t expected_size =
        static_cast<std::uintmax_t>(args.input_token_ids.size()) * 2560U * 2U;
    std::uintmax_t observed_size = 0;
    if (!guest_vfs.fileSize("model_embed_tokens_weight.bf16", observed_size) ||
        observed_size != expected_size) {
        error = "official-slice converted embedding row geometry is invalid";
        return false;
    }

    std::vector<std::uint16_t> source_row;
    std::vector<std::uint8_t> converted_row(2560U * 2U);
    for (int token_id : args.input_token_ids) {
        if (!source.readBf16Row("model.embed_tokens.weight", token_id,
                                source_row, error)) return false;
        input.read(reinterpret_cast<char*>(converted_row.data()),
                   static_cast<std::streamsize>(converted_row.size()));
        if (!input || std::memcmp(converted_row.data(), source_row.data(),
                                  converted_row.size()) != 0) {
            error = "official-slice converted embedding row does not match source";
            return false;
        }
    }
    char trailing = 0;
    if (input.read(&trailing, 1)) {
        error = "official-slice converted embedding has trailing data";
        return false;
    }
    return true;
}

bool abiV3RuntimeAvailable() {
    using namespace sandbox::vm;
    VMState vm(256, 512);
    const auto header = makeExecutableHeaderV3(0, 1, 1, 27);
    return vm.configureArchitecture(header) &&
           vm.executable_version == sandbox::architecture::v3::EXECUTABLE_VERSION &&
           vm.function_abi_version == sandbox::architecture::v3::FUNCTION_ABI_VERSION &&
           vm.vector_abi_version == sandbox::architecture::v3::VECTOR_ABI_VERSION &&
           vm.vector_length == sandbox::architecture::v3::VECTOR_LANE_COUNT &&
           initializeTaskVectorContextV3(vm.dmem, 0, header) &&
           saveVectorContextV3(vm, vm.dmem, 0) &&
           restoreVectorContextV3(vm, vm.dmem, 0);
}

std::string tokenHash(const std::vector<int>& tokens) {
    std::ostringstream canonical;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (index != 0) canonical << ',';
        canonical << tokens[index];
    }
    Sha256 digest;
    const std::string bytes = canonical.str();
    digest.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return "sha256:" + digest.finalHex();
}

bool writeResult(const Arguments& args, bool status, bool abi_v3,
                 const std::vector<int>& token_ids, const std::string& detail,
                 double seconds) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"trit.bitnet_guest_execution.v1\",\n"
        << "  \"version\": 1,\n"
        << "  \"status\": " << (status ? "true" : "false") << ",\n"
        << "  \"abi_v3_runtime_available\": " << (abi_v3 ? "true" : "false") << ",\n"
        << "  \"profile\": \"" << jsonEscape(args.profile) << "\",\n"
        << "  \"format\": \"" << jsonEscape(args.format) << "\",\n"
        << "  \"threads\": " << args.threads << ",\n"
        << "  \"length\": " << args.length << ",\n"
        << "  \"input_token_ids\": ";
    writeIntArray(out, args.input_token_ids);
    out << ",\n  \"token_ids\": ";
    writeIntArray(out, token_ids);
    out << ",\n  \"token_hash\": \"" << jsonEscape(tokenHash(token_ids)) << "\",\n"
        << "  \"elapsed_seconds\": " << seconds << ",\n"
        << "  \"detail\": \"" << jsonEscape(detail) << "\"\n"
        << "}\n";
    const std::filesystem::path temporary =
        std::filesystem::path(args.output.string() + ".partial");
    std::error_code error;
    std::filesystem::create_directories(args.output.parent_path(), error);
    if (error) return false;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file << out.str();
        file.flush();
        if (!file.good()) return false;
    }
    std::filesystem::remove(args.output, error);
    error.clear();
    std::filesystem::rename(temporary, args.output, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Arguments args;
    std::string error;
    if (!parseArguments(argc, argv, args, error)) {
        std::cerr << "bitnet_guest_executor: " << error << "\n";
        return 2;
    }
    const bool abi_v3 = abiV3RuntimeAvailable();
    if (!abi_v3) {
        if (!writeResult(args, false, false, {}, "ABI-v3 VM/context probe failed", 0.0)) return 1;
        return 1;
    }
    std::error_code type_error;
    if (!std::filesystem::is_regular_file(args.model, type_error) || type_error ||
        !std::filesystem::is_directory(args.converted_dir, type_error) || type_error) {
        if (!writeResult(args, false, true, {},
                         "official model or converted directory is unavailable", 0.0)) return 1;
        return 1;
    }

    const sandbox::bitnet::ReadOnlyGuestVfs guest_vfs(args.converted_dir);
    if (!guest_vfs.valid() || guest_vfs.profile() != args.profile) {
        const std::string detail = guest_vfs.valid()
            ? "guest package profile does not match requested external profile"
            : "guest package validation failed: " + guest_vfs.error();
        if (!writeResult(args, false, true, {}, detail, 0.0)) return 1;
        return 1;
    }

    BitNetLoader loader(guest_vfs);
    if (!loader.hasValidManifest() || loader.layers.empty()) {
        if (!writeResult(args, false, true, {},
                         "converted tensor manifest is invalid or empty", 0.0)) return 1;
        return 1;
    }

    const bool slice_profile = args.profile == "official-slice";
    if (slice_profile && !verifySliceEmbeddingRows(args, guest_vfs, error)) {
        if (!writeResult(args, false, true, {},
                         "official-slice embedding verification failed: " + error, 0.0)) return 1;
        return 1;
    }

    BitNetConfig config;
    config.hidden_size = 2560;
    config.num_heads = 20;
    config.num_kv_heads = 5;
    config.head_dim = 128;
    config.intermediate_size = 6912;
    config.num_layers = slice_profile ? 1 : 30;
    config.vocab_size = 128256;
    config.max_position_embeddings = 4096;
    config.rope_theta = 500000.0f;

    const auto begin = std::chrono::steady_clock::now();
    BitNetHostInference inference(loader, config, args.threads);
    if (!inference.init(args.model.string(), error)) {
        if (!writeResult(args, false, true, {}, "BitNet initialization failed: " + error, 0.0)) return 1;
        return 1;
    }
    const char* node_trace_env = std::getenv("TRIT_BITNET_NODE_TRACE");
    if (node_trace_env != nullptr && *node_trace_env != '\0' &&
        !inference.setDebugTracePath(node_trace_env, error)) {
        if (!writeResult(args, false, true, {}, error, 0.0)) return 1;
        return 1;
    }

    const char* trace_env = std::getenv("TRIT_BITNET_TRACE");
    std::ofstream trace;
    const bool trace_enabled = trace_env != nullptr && *trace_env != '\0';
    if (trace_enabled) {
        trace.open(trace_env, std::ios::binary | std::ios::trunc);
        if (!trace) {
            if (!writeResult(args, false, true, {},
                             "unable to open TRIT_BITNET_TRACE", 0.0)) return 1;
            return 1;
        }
    }

    auto emitTrace = [&](int step, const sandbox::bitnet::HostForwardResult& result) {
        if (!trace_enabled || result.logits.empty()) return;
        std::vector<int> order(result.logits.size());
        for (std::size_t token = 0; token < result.logits.size(); ++token)
            order[token] = static_cast<int>(token);
        const std::size_t keep = std::min<std::size_t>(8, order.size());
        std::partial_sort(order.begin(), order.begin() + keep, order.end(),
            [&](int left, int right) {
                if (result.logits[static_cast<std::size_t>(left)] !=
                    result.logits[static_cast<std::size_t>(right)])
                    return result.logits[static_cast<std::size_t>(left)] >
                           result.logits[static_cast<std::size_t>(right)];
                return left < right;
            });
        trace << "step " << step << " top:" << std::setprecision(9);
        for (std::size_t rank = 0; rank < keep; ++rank) {
            const int token = order[rank];
            trace << " " << token << ":"
                  << result.logits[static_cast<std::size_t>(token)];
        }
        trace << "\n";
    };

    auto runGeneration = [&](std::vector<int>& generated, std::string& run_error) {
        inference.reset();
        sandbox::bitnet::HostForwardResult result;
        for (std::size_t index = 0; index < args.input_token_ids.size(); ++index) {
            const bool need_logits = trace_enabled &&
                index + 1 == args.input_token_ids.size();
            result = inference.evalToken(args.input_token_ids[index], need_logits);
            if (!result.ok) {
                run_error = "prompt evaluation failed: " + result.error;
                return false;
            }
        }
        generated.clear();
        generated.reserve(static_cast<std::size_t>(args.length));
        int next = result.greedy_token;
        for (int index = 0; index < args.length; ++index) {
            emitTrace(index, result);
            generated.push_back(next);
            if (index + 1 < args.length) {
                result = inference.evalToken(next, trace_enabled);
                if (!result.ok) {
                    run_error = "generation failed: " + result.error;
                    return false;
                }
                next = result.greedy_token;
            }
        }
        return true;
    };

    std::vector<int> generated;
    if (!runGeneration(generated, error)) {
        if (!writeResult(args, false, true, generated, error, 0.0)) return 1;
        return 1;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    bool match = generated == args.expected_token_ids;
    std::string detail;
    if (slice_profile) {
        std::vector<int> repeated;
        std::string repeat_error;
        const bool repeated_ok = runGeneration(repeated, repeat_error);
        match = repeated_ok && repeated == generated;
        detail = match
            ? "official-slice converted embedding rows and layer-zero execution were deterministic"
            : (repeat_error.empty()
                ? "official-slice repeated layer-zero execution differed"
                : "official-slice repeat failed: " + repeat_error);
    } else {
        detail = match
            ? "official safetensors-derived fixed-prompt execution matched reference"
            : "generated token IDs differ from the pinned official reference";
    }
    if (!writeResult(args, match, true, generated, detail, seconds)) return 1;
    return match ? 0 : 1;
}
