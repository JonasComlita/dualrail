#include "encrypted_volume_host.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#endif

namespace {

using sandbox::host::encrypted_volume::ByteVector;
using sandbox::host::encrypted_volume::EncryptOptions;
using sandbox::host::encrypted_volume::OpenSslAes256GcmProvider;
namespace encrypted = sandbox::host::encrypted_volume;
namespace detail = sandbox::host::encrypted_volume::detail;

void usage() {
    std::cerr <<
        "usage:\n"
        "  trit_volume inspect [--json] ENVELOPE\n"
        "  trit_volume encrypt --key-file KEY [--chunk-bytes N] [--overwrite] INPUT.tdisk OUTPUT.tenc\n"
        "  trit_volume decrypt --key-file KEY [--overwrite] INPUT.tenc OUTPUT.tdisk\n"
        "  trit_volume attach/run --key-file KEY ENVELOPE -- COMMAND [ARGS...]\n";
}

void printError(const std::string& error) {
    std::cerr << "trit_volume: " << (error.empty() ? "operation failed" : error)
              << "\n";
}

bool hasPath(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool parseUnsigned(const std::string& text, std::uint32_t& value) {
    if (text.empty()) return false;
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || parsed > 0xffffffffULL) return false;
        value = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

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

struct EnvelopeInfo {
    std::uint32_t algorithm = 0;
    std::uint32_t chunk_bytes = 0;
    std::uint32_t logical_block_words = 0;
    std::uint64_t logical_size = 0;
    std::uint64_t logical_block_count = 0;
    std::uint64_t chunk_count = 0;
};

bool inspectEnvelope(const std::string& path, EnvelopeInfo& info,
                     std::string& error) {
    ByteVector bytes;
    if (!encrypted::readFile(path, bytes, &error)) return false;
    std::array<std::uint8_t, encrypted::kNoncePrefixBytes> prefix{};
    if (!detail::parseHeader(bytes, info.algorithm, info.chunk_bytes,
                             info.logical_block_words, info.logical_size,
                             info.logical_block_count, info.chunk_count,
                             prefix, &error)) {
        return false;
    }
    // Never expose the nonce prefix in diagnostics.  It is authenticated
    // envelope metadata, but it remains secret nonce material by policy.
    return true;
}

int inspectCommand(const std::string& path, bool json) {
    EnvelopeInfo info;
    std::string error;
    if (!inspectEnvelope(path, info, error)) {
        printError(error);
        return 1;
    }
    const char* algorithm = info.algorithm == encrypted::kAlgorithmAes256Gcm
        ? "AES-256-GCM" : "unknown";
    if (json) {
        std::cout << "{\n"
                  << "  \"schema\": \"trit.encrypted_volume_inspection.v1\",\n"
                  << "  \"valid_header\": true,\n"
                  << "  \"version\": " << encrypted::kEnvelopeVersion << ",\n"
                  << "  \"algorithm\": \"" << algorithm << "\",\n"
                  << "  \"header_bytes\": " << encrypted::kEnvelopeHeaderBytes << ",\n"
                  << "  \"chunk_bytes\": " << info.chunk_bytes << ",\n"
                  << "  \"logical_block_words\": " << info.logical_block_words << ",\n"
                  << "  \"logical_block_count\": " << info.logical_block_count << ",\n"
                  << "  \"logical_size\": " << info.logical_size << ",\n"
                  << "  \"chunk_count\": " << info.chunk_count << "\n"
                  << "}\n";
    } else {
        std::cout << "TRITENC1\n"
                  << "version: " << encrypted::kEnvelopeVersion << "\n"
                  << "algorithm: " << algorithm << "\n"
                  << "chunk_bytes: " << info.chunk_bytes << "\n"
                  << "logical_size: " << info.logical_size << "\n"
                  << "logical_block_words: " << info.logical_block_words << "\n"
                  << "logical_block_count: " << info.logical_block_count << "\n"
                  << "chunk_count: " << info.chunk_count << "\n";
    }
    return 0;
}

bool readKey(const std::string& key_path, ByteVector& key, std::string& error) {
    if (key_path.empty()) {
        error = "--key-file is required";
        return false;
    }
    return encrypted::readAes256KeyFile(key_path, key, &error);
}

bool checkDestination(const std::string& path, bool overwrite, std::string& error) {
    if (!overwrite && hasPath(path)) {
        error = "destination exists; pass --overwrite to replace it";
        return false;
    }
    return true;
}

int encryptCommand(const std::string& input, const std::string& output,
                   const std::string& key_path, std::uint32_t chunk_bytes,
                   bool overwrite) {
    std::string error;
    if (!checkDestination(output, overwrite, error)) {
        printError(error);
        return 2;
    }
    ByteVector key;
    if (!readKey(key_path, key, error)) {
        printError(error);
        return 2;
    }
    EncryptOptions options;
    options.chunk_bytes = chunk_bytes;
    OpenSslAes256GcmProvider provider;
    const bool ok = encrypted::encryptTdiskFile(input, output, key, provider,
                                                options, &error, overwrite);
    encrypted::detail::clearSensitive(key);
    if (!ok) {
        printError(error);
        return 1;
    }
    std::cout << "encrypted " << output << "\n";
    return 0;
}

int decryptCommand(const std::string& input, const std::string& output,
                   const std::string& key_path, bool overwrite) {
    std::string error;
    if (!checkDestination(output, overwrite, error)) {
        printError(error);
        return 2;
    }
    ByteVector key;
    if (!readKey(key_path, key, error)) {
        printError(error);
        return 2;
    }
    OpenSslAes256GcmProvider provider;
    const bool ok = encrypted::decryptTdiskFile(input, output, key, provider,
                                                &error, overwrite);
    encrypted::detail::clearSensitive(key);
    if (!ok) {
        printError(error);
        return 1;
    }
    std::cout << "decrypted " << output << "\n";
    return 0;
}

#if defined(_WIN32)
void setPlaintextEnvironment(const std::string& path) {
    _putenv_s("TRIT_VOLUME_PLAINTEXT", path.c_str());
}
void clearPlaintextEnvironment() {
    _putenv_s("TRIT_VOLUME_PLAINTEXT", "");
}
#else
void setPlaintextEnvironment(const std::string& path) {
    setenv("TRIT_VOLUME_PLAINTEXT", path.c_str(), 1);
}
void clearPlaintextEnvironment() {
    unsetenv("TRIT_VOLUME_PLAINTEXT");
}
#endif

std::string shellQuote(const std::string& value) {
#if defined(_WIN32)
    std::string quoted = "\"";
    for (char character : value) {
        if (character == '"') quoted += "\\\"";
        else if (character == '\\') quoted += "\\\\";
        else quoted.push_back(character);
    }
    quoted += "\"";
    return quoted;
#else
    std::string quoted = "'";
    for (char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted.push_back(character);
    }
    quoted += "'";
    return quoted;
#endif
}

int runCommand(const std::vector<std::string>& command) {
#if defined(_WIN32)
    if (command.empty()) return 127;
    std::vector<const char*> argv;
    argv.reserve(command.size() + 1);
    for (const std::string& argument : command) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    const int result = _spawnvp(_P_WAIT, command.front().c_str(), argv.data());
    return result < 0 ? 127 : result;
#else
    std::string joined;
    for (const auto& argument : command) {
        if (!joined.empty()) joined.push_back(' ');
        joined += shellQuote(argument);
    }
    return std::system(joined.c_str());
#endif
}

bool decryptToExclusiveTemporaryFile(const std::string& envelope_path,
                                     const ByteVector& key,
                                     const OpenSslAes256GcmProvider& provider,
                                     std::filesystem::path& output,
                                     std::string& error) {
    ByteVector envelope;
    ByteVector plaintext;
    if (!encrypted::readFile(envelope_path, envelope, &error) ||
        !encrypted::decryptBytes(envelope, key, provider, plaintext, &error)) {
        detail::clearSensitive(envelope);
        detail::clearSensitive(plaintext);
        return false;
    }
    detail::clearSensitive(envelope);
    if (!encrypted::isCanonicalTdiskV2(plaintext, &error)) {
        detail::clearSensitive(plaintext);
        return false;
    }

    // The name is not treated as a security boundary.  The helper uses
    // CREATE_NEW/O_EXCL and owner-only permissions, so a pre-created path is
    // harmless; retrying a bounded number of times handles the benign case of
    // a sequence collision without falling back to a replace operation.
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "trit-volume-attach.tdisk";
    for (int attempt = 0; attempt != 32; ++attempt) {
        const std::filesystem::path candidate = detail::temporaryPathFor(base);
        bool created = false;
        error.clear();
        if (detail::createRestrictedTemporaryFile(candidate, plaintext, created,
                                                   &error)) {
            output = candidate;
            detail::clearSensitive(plaintext);
            return true;
        }
        if (!created && !std::filesystem::exists(candidate)) {
            // A real I/O or permission failure is not made better by retrying.
            break;
        }
    }
    detail::clearSensitive(plaintext);
    if (error.empty()) error = "failed to create exclusive plaintext temporary";
    return false;
}

class PlaintextCleanup final {
public:
    explicit PlaintextCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~PlaintextCleanup() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    PlaintextCleanup(const PlaintextCleanup&) = delete;
    PlaintextCleanup& operator=(const PlaintextCleanup&) = delete;

private:
    std::filesystem::path path_;
};

int attachRunCommand(const std::string& envelope_path,
                     const std::string& key_path,
                     const std::vector<std::string>& command) {
    if (command.empty()) {
        printError("attach/run requires a command after --");
        return 2;
    }
    std::string error;
    ByteVector key;
    if (!readKey(key_path, key, error)) {
        printError(error);
        return 2;
    }

    std::filesystem::path temp_plain;
    OpenSslAes256GcmProvider provider;
    if (!decryptToExclusiveTemporaryFile(envelope_path, key, provider,
                                         temp_plain, error)) {
        encrypted::detail::clearSensitive(key);
        printError(error);
        return 1;
    }
    PlaintextCleanup plaintext_cleanup(temp_plain);
    setPlaintextEnvironment(temp_plain.string());
    const int child_status = runCommand(command);
    clearPlaintextEnvironment();

    // The existing loader is the attach boundary: the plaintext must remain a
    // canonical tDisk v2 image before it is promoted back to the envelope.
    // encryptTdiskFile writes through its own restricted temporary and atomic
    // replacement, so a failed re-encryption leaves the old envelope intact.
    const bool reencrypted = encrypted::encryptTdiskFile(
        temp_plain.string(), envelope_path, key, provider, {}, &error);
    encrypted::detail::clearSensitive(key);
    if (!reencrypted) {
        printError("failed to re-encrypt attached volume: " + error);
        return 1;
    }
    if (child_status != 0) return child_status;
    std::cout << "attached volume detached and re-encrypted\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string action = argv[1];
    if (action == "inspect") {
        bool json = false;
        std::string path;
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--json") json = true;
            else if (path.empty()) path = argument;
            else { usage(); return 2; }
        }
        if (path.empty()) { usage(); return 2; }
        return inspectCommand(path, json);
    }

    std::string key_path;
    std::string input;
    std::string output;
    std::uint32_t chunk_bytes = encrypted::kDefaultChunkBytes;
    bool overwrite = false;
    std::vector<std::string> command;
    bool after_separator = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (after_separator) {
            command.push_back(argument);
        } else if (argument == "--") {
            after_separator = true;
        } else if (argument == "--overwrite") {
            overwrite = true;
        } else if (argument == "--key-file" && index + 1 < argc) {
            key_path = argv[++index];
        } else if (argument == "--chunk-bytes" && index + 1 < argc) {
            if (!parseUnsigned(argv[++index], chunk_bytes)) {
                printError("--chunk-bytes must be an unsigned 32-bit integer");
                return 2;
            }
        } else if (input.empty()) {
            input = argument;
        } else if (output.empty()) {
            output = argument;
        } else {
            usage();
            return 2;
        }
    }

    if (key_path.empty() || input.empty()) {
        usage();
        return 2;
    }
    if (action == "encrypt") {
        if (output.empty()) { usage(); return 2; }
        return encryptCommand(input, output, key_path, chunk_bytes, overwrite);
    }
    if (action == "decrypt") {
        if (output.empty()) { usage(); return 2; }
        return decryptCommand(input, output, key_path, overwrite);
    }
    if (action == "attach/run" || action == "attach" || action == "run")
        return attachRunCommand(input, key_path, command);

    usage();
    return 2;
}
