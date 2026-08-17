#include "encrypted_volume_host.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using sandbox::host::encrypted_volume::AuthenticatedEncryptionProvider;
using sandbox::host::encrypted_volume::ByteVector;
using sandbox::host::encrypted_volume::EncryptOptions;
using sandbox::host::encrypted_volume::kAesGcmNonceBytes;
using sandbox::host::encrypted_volume::kAesGcmTagBytes;
using sandbox::host::encrypted_volume::kAlgorithmTestOnly;
using sandbox::host::encrypted_volume::kNoncePrefixBytes;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

// Deterministic provider used only by this test executable.  It intentionally
// is not a cryptographic implementation and is never exposed by production
// code.  Its purpose is to exercise envelope framing when OpenSSL is omitted.
class TestOnlyDeterministicMock final : public AuthenticatedEncryptionProvider {
public:
    [[nodiscard]] std::uint32_t algorithmId() const override {
        return kAlgorithmTestOnly;
    }
    [[nodiscard]] std::size_t keyBytes() const override { return 32; }
    [[nodiscard]] std::size_t nonceBytes() const override {
        return kAesGcmNonceBytes;
    }
    [[nodiscard]] std::size_t tagBytes() const override {
        return kAesGcmTagBytes;
    }

    [[nodiscard]] bool encrypt(const ByteVector& key,
                               const ByteVector& nonce,
                               const ByteVector& aad,
                               const ByteVector& plaintext,
                               ByteVector& ciphertext,
                               ByteVector& tag,
                               std::string* error) const override {
        if (key.size() != keyBytes() || nonce.size() != nonceBytes()) {
            if (error) *error = "test-only mock input size is invalid";
            ciphertext.clear();
            tag.clear();
            return false;
        }
        ciphertext.resize(plaintext.size());
        for (std::size_t i = 0; i < plaintext.size(); ++i) {
            ciphertext[i] = static_cast<std::uint8_t>(
                plaintext[i] ^ streamByte(key, nonce, i));
        }
        tag = makeTag(key, nonce, aad, ciphertext);
        return true;
    }

    [[nodiscard]] bool decrypt(const ByteVector& key,
                               const ByteVector& nonce,
                               const ByteVector& aad,
                               const ByteVector& ciphertext,
                               const ByteVector& tag,
                               ByteVector& plaintext,
                               std::string* error) const override {
        plaintext.clear();
        if (key.size() != keyBytes() || nonce.size() != nonceBytes() ||
            tag.size() != tagBytes()) {
            if (error) *error = "test-only mock input size is invalid";
            return false;
        }
        const ByteVector expected = makeTag(key, nonce, aad, ciphertext);
        if (expected != tag) {
            if (error) *error = "test-only mock authentication failed";
            return false;
        }
        plaintext.resize(ciphertext.size());
        for (std::size_t i = 0; i < ciphertext.size(); ++i) {
            plaintext[i] = static_cast<std::uint8_t>(
                ciphertext[i] ^ streamByte(key, nonce, i));
        }
        return true;
    }

    [[nodiscard]] bool randomBytes(std::uint8_t* destination,
                                   std::size_t size,
                                   std::string*) const override {
        for (std::size_t i = 0; i < size; ++i)
            destination[i] = static_cast<std::uint8_t>(0xa0U + i);
        return true;
    }

private:
    static std::uint64_t fnv(const ByteVector& bytes, std::uint64_t hash) {
        for (std::uint8_t value : bytes) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static ByteVector makeTag(const ByteVector& key,
                              const ByteVector& nonce,
                              const ByteVector& aad,
                              const ByteVector& ciphertext) {
        std::uint64_t hash = 1469598103934665603ULL;
        hash = fnv(key, hash);
        hash = fnv(nonce, hash);
        hash = fnv(aad, hash);
        hash = fnv(ciphertext, hash);
        ByteVector tag(kAesGcmTagBytes, 0);
        for (std::size_t i = 0; i < tag.size(); ++i) {
            tag[i] = static_cast<std::uint8_t>(hash >> ((i % 8) * 8));
            if ((i % 8) == 7) hash = hash * 6364136223846793005ULL + 1;
        }
        return tag;
    }

    static std::uint8_t streamByte(const ByteVector& key,
                                   const ByteVector& nonce,
                                   std::size_t index) {
        std::uint64_t hash = fnv(key, 1469598103934665603ULL);
        hash = fnv(nonce, hash);
        hash ^= static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL;
        hash ^= hash >> 29;
        return static_cast<std::uint8_t>(hash & 0xffU);
    }
};

ByteVector makePlaintext() {
    ByteVector bytes(200'123);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        // Include zero runs: sparse image payloads contain many zero words.
        bytes[i] = (i % 257 == 0) ? 0 :
                   static_cast<std::uint8_t>((i * 31U + 7U) & 0xffU);
    }
    return bytes;
}

ByteVector makeKey(std::uint8_t seed) {
    ByteVector key(32);
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(seed + i * 3U);
    return key;
}

void appendLe32(ByteVector& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

void appendLe64(ByteVector& bytes, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffULL));
}

void writeLe32(ByteVector& bytes, std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes[offset + static_cast<std::size_t>(shift / 8)] =
            static_cast<std::uint8_t>((value >> shift) & 0xffU);
}

void writeLe64(ByteVector& bytes, std::size_t offset, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        bytes[offset + static_cast<std::size_t>(shift / 8)] =
            static_cast<std::uint8_t>((value >> shift) & 0xffULL);
}

std::uint64_t fnv1a(const ByteVector& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint8_t value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t readLe32At(const ByteVector& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::string hexEncode(const ByteVector& bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (std::uint8_t byte : bytes) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0fU]);
    }
    return result;
}

ByteVector makeCanonicalTdisk() {
    ByteVector records;
    appendLe32(records, 0);
    for (std::uint32_t word = 0; word < 27; ++word)
        appendLe64(records, word == 0 ? 1 : 0);
    appendLe32(records, 3);
    for (std::uint32_t word = 0; word < 27; ++word)
        appendLe64(records, word == 1 ? 2 : 0);

    ByteVector disk;
    appendLe64(disk, sandbox::host::encrypted_volume::kTdiskV2Magic);
    appendLe32(disk, 2);
    appendLe32(disk, 27);
    appendLe64(disk, 1); // nonzero generation
    appendLe64(disk, fnv1a(records));
    appendLe32(disk, 2);
    disk.insert(disk.end(), records.begin(), records.end());
    return disk;
}

bool hasTemporaryFor(const std::filesystem::path& directory,
                     const std::string& target_name) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) return true;
        const std::string name = entry.path().filename().string();
        if (name.rfind(target_name + ".tmp.", 0) == 0) return true;
    }
    return false;
}

class ErrorLeakingProvider final : public AuthenticatedEncryptionProvider {
public:
    [[nodiscard]] std::uint32_t algorithmId() const override {
        return kAlgorithmTestOnly;
    }
    [[nodiscard]] std::size_t keyBytes() const override { return 32; }
    [[nodiscard]] std::size_t nonceBytes() const override {
        return kAesGcmNonceBytes;
    }
    [[nodiscard]] std::size_t tagBytes() const override {
        return kAesGcmTagBytes;
    }

    [[nodiscard]] bool encrypt(const ByteVector& key,
                               const ByteVector&,
                               const ByteVector&,
                               const ByteVector&,
                               ByteVector& ciphertext,
                               ByteVector& tag,
                               std::string* error) const override {
        ciphertext.clear();
        tag.clear();
        if (error) *error = "provider leaked key material: " +
                            std::string(key.size(), 'K');
        return false;
    }

    [[nodiscard]] bool decrypt(const ByteVector& key,
                               const ByteVector&,
                               const ByteVector&,
                               const ByteVector&,
                               const ByteVector&,
                               ByteVector& plaintext,
                               std::string* error) const override {
        plaintext.clear();
        if (error) *error = "provider leaked key material: " +
                            std::string(key.size(), 'K');
        return false;
    }

    [[nodiscard]] bool randomBytes(std::uint8_t*, std::size_t,
                                   std::string*) const override {
        return false;
    }
};

void exerciseProvider(const AuthenticatedEncryptionProvider& provider,
                      const std::string& label) {
    const ByteVector plain = makePlaintext();
    const ByteVector key = makeKey(0x11);
    const ByteVector wrong_key = makeKey(0x71);
    EncryptOptions options;
    options.chunk_bytes = 4096;
    options.logical_block_words = 27;
    options.nonce_prefix = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    // Deterministic prefixes are reserved for the non-production framing
    // provider.  Production AES-GCM must obtain the prefix from its CSPRNG.
    options.nonce_prefix_provided = provider.algorithmId() == kAlgorithmTestOnly;

    ByteVector envelope;
    std::string error;
    expect(sandbox::host::encrypted_volume::encryptBytes(
               plain, key, provider, envelope, options, &error),
           label + " roundtrip encryption succeeds: " + error);
    expect(envelope.size() > plain.size(), label + " envelope carries framing and tags");

    ByteVector recovered;
    expect(sandbox::host::encrypted_volume::decryptBytes(
               envelope, key, provider, recovered, &error),
           label + " roundtrip decryption succeeds: " + error);
    expect(recovered == plain, label + " plaintext roundtrip matches");

    const auto recordEnd = [&](std::size_t start) {
        return start + 12U + kAesGcmTagBytes +
               static_cast<std::size_t>(readLe32At(envelope, start + 8U));
    };
    const std::size_t first_record = sandbox::host::encrypted_volume::kEnvelopeHeaderBytes;
    const std::size_t first_end = recordEnd(first_record);
    const std::size_t second_end = recordEnd(first_end);
    ByteVector reordered;
    reordered.insert(reordered.end(), envelope.begin(),
                     envelope.begin() + static_cast<std::ptrdiff_t>(
                         sandbox::host::encrypted_volume::kEnvelopeHeaderBytes));
    reordered.insert(reordered.end(), envelope.begin() +
                                      static_cast<std::ptrdiff_t>(first_end),
                     envelope.begin() + static_cast<std::ptrdiff_t>(second_end));
    reordered.insert(reordered.end(), envelope.begin() +
                                      static_cast<std::ptrdiff_t>(first_record),
                     envelope.begin() + static_cast<std::ptrdiff_t>(first_end));
    reordered.insert(reordered.end(), envelope.begin() +
                                      static_cast<std::ptrdiff_t>(second_end),
                     envelope.end());
    ByteVector reordered_output{0xee};
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               reordered, key, provider, reordered_output, &error),
           label + " reordered chunks are rejected");
    expect(reordered_output.empty(), label +
           " reordered-chunk failure leaves no plaintext");

    ByteVector wrong;
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               envelope, wrong_key, provider, wrong, &error),
           label + " wrong key is rejected");
    expect(wrong.empty(), label + " wrong-key failure does not expose partial plaintext");

    ByteVector tampered = envelope;
    tampered[tampered.size() - 1] ^= 0x80;
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " ciphertext tamper is rejected");
    expect(wrong.empty(), label + " ciphertext tamper leaves no plaintext");

    tampered = envelope;
    tampered[56] ^= 0x01; // nonce prefix is authenticated header data.
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " nonce/header tamper is rejected");

    EncryptOptions generated_options = options;
    generated_options.nonce_prefix_provided = false;
    generated_options.nonce_prefix.fill(0);
    ByteVector generated_envelope;
    error.clear();
    expect(sandbox::host::encrypted_volume::encryptBytes(
               plain, key, provider, generated_envelope, generated_options, &error),
           label + " provider-generated nonce prefix succeeds: " + error);
    recovered.clear();
    expect(sandbox::host::encrypted_volume::decryptBytes(
               generated_envelope, key, provider, recovered, &error) && recovered == plain,
           label + " provider-generated nonce prefix roundtrips");

    tampered = envelope;
    tampered.resize(tampered.size() - 1);
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " truncation is rejected");

    tampered = envelope;
    tampered.push_back(0);
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " trailing bytes are rejected");
}

#if defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
void exerciseOpenSslKnownAnswer() {
    sandbox::host::encrypted_volume::OpenSslAes256GcmProvider provider;
    const ByteVector key(32, 0);
    const ByteVector nonce(12, 0);
    const ByteVector aad;
    const ByteVector plaintext(16, 0);
    const ByteVector expected_ciphertext = {
        0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
        0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18};
    const ByteVector expected_tag = {
        0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
        0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19};
    ByteVector ciphertext;
    ByteVector tag;
    std::string error;
    expect(provider.encrypt(key, nonce, aad, plaintext, ciphertext, tag, &error),
           "OpenSSL AES-256-GCM known-answer encryption succeeds: " + error);
    expect(ciphertext == expected_ciphertext,
           "OpenSSL AES-256-GCM known-answer ciphertext matches");
    expect(tag == expected_tag, "OpenSSL AES-256-GCM known-answer tag matches");

    ByteVector recovered;
    expect(provider.decrypt(key, nonce, aad, expected_ciphertext, expected_tag,
                            recovered, &error) && recovered == plaintext,
           "OpenSSL AES-256-GCM known-answer decryption matches");

    EncryptOptions deterministic;
    deterministic.nonce_prefix = {1, 2, 3, 4, 5, 6, 7, 8};
    deterministic.nonce_prefix_provided = true;
    ByteVector envelope;
    expect(!sandbox::host::encrypted_volume::encryptBytes(
               ByteVector(4096, 0x5a), key, provider, envelope,
               deterministic, &error),
           "production OpenSSL rejects caller-supplied nonce prefixes");
}

void exerciseOpenSslRandomizedMultiChunk() {
    sandbox::host::encrypted_volume::OpenSslAes256GcmProvider provider;
    std::uint32_t state = 0x7f4a7c15U;
    ByteVector previous_prefix;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const std::uint32_t chunk_bytes =
            4096U << static_cast<unsigned>(iteration % 3);
        state = state * 1664525U + 1013904223U;
        const std::size_t plaintext_size =
            static_cast<std::size_t>(chunk_bytes) *
                static_cast<std::size_t>(2U + (state % 6U)) +
            static_cast<std::size_t>(state % chunk_bytes);
        ByteVector plaintext(plaintext_size);
        for (std::size_t index = 0; index < plaintext.size(); ++index) {
            state = state * 1664525U + 1013904223U;
            plaintext[index] = static_cast<std::uint8_t>(state >> 24);
        }
        const ByteVector key = makeKey(static_cast<std::uint8_t>(0x31U + iteration));
        EncryptOptions options;
        options.chunk_bytes = chunk_bytes;
        options.nonce_prefix_provided = false;
        ByteVector envelope;
        std::string error;
        expect(sandbox::host::encrypted_volume::encryptBytes(
                   plaintext, key, provider, envelope, options, &error),
               "OpenSSL randomized multi-chunk encryption succeeds: " + error);
        expect(envelope.size() > plaintext.size() &&
                   envelope.size() > sandbox::host::encrypted_volume::kEnvelopeHeaderBytes,
               "OpenSSL randomized multi-chunk envelope carries records");
        ByteVector prefix(
            envelope.begin() + static_cast<std::ptrdiff_t>(56),
            envelope.begin() + static_cast<std::ptrdiff_t>(64));
        expect(prefix != previous_prefix,
               "OpenSSL generates a fresh nonce prefix for each volume");
        previous_prefix = prefix;

        ByteVector recovered;
        expect(sandbox::host::encrypted_volume::decryptBytes(
                   envelope, key, provider, recovered, &error) &&
                   recovered == plaintext,
               "OpenSSL randomized multi-chunk roundtrip matches: " + error);
    }
}
#endif

void exerciseParserBounds(const AuthenticatedEncryptionProvider& provider) {
    const ByteVector plain = makePlaintext();
    const ByteVector key = makeKey(0x41);
    EncryptOptions options;
    options.chunk_bytes = 4096;
    options.logical_block_words = 27;
    options.nonce_prefix = {9, 8, 7, 6, 5, 4, 3, 2};
    options.nonce_prefix_provided = provider.algorithmId() == kAlgorithmTestOnly;

    ByteVector envelope;
    std::string error;
    expect(sandbox::host::encrypted_volume::encryptBytes(
               plain, key, provider, envelope, options, &error),
           "parser-boundary fixture encrypts: " + error);

    auto rejects = [&](ByteVector malformed, const std::string& label) {
        ByteVector output{0xaa};
        error = "stale diagnostic that must be replaced";
        expect(!sandbox::host::encrypted_volume::decryptBytes(
                   malformed, key, provider, output, &error), label);
        expect(output.empty(), label + " leaves no plaintext");
        expect(error.find("K") == std::string::npos,
               label + " does not expose key-like diagnostics");
    };

    ByteVector empty_header(
        sandbox::host::encrypted_volume::kEnvelopeMagic.begin(),
        sandbox::host::encrypted_volume::kEnvelopeMagic.end());
    appendLe32(empty_header, sandbox::host::encrypted_volume::kEnvelopeVersion);
    appendLe32(empty_header, kAlgorithmTestOnly);
    appendLe32(empty_header, sandbox::host::encrypted_volume::kEnvelopeHeaderBytes);
    appendLe32(empty_header, 0);
    appendLe32(empty_header, options.chunk_bytes);
    appendLe32(empty_header, 0);
    appendLe64(empty_header, 0);
    appendLe64(empty_header, 0);
    appendLe64(empty_header, 0);
    empty_header.insert(empty_header.end(), {1, 2, 3, 4, 5, 6, 7, 8});
    rejects(empty_header, "parser rejects unauthenticated empty envelopes");

    ByteVector malformed = envelope;
    writeLe64(malformed, 32, std::numeric_limits<std::uint64_t>::max());
    rejects(malformed, "parser rejects unreasonable logical size");

    malformed = envelope;
    writeLe64(malformed, 48, std::numeric_limits<std::uint64_t>::max());
    rejects(malformed, "parser rejects oversized chunk count");

    malformed = envelope;
    writeLe32(malformed, 24, 1);
    rejects(malformed, "parser rejects undersized chunk geometry");

    malformed = envelope;
    std::fill(malformed.begin() + 56, malformed.begin() + 64, 0);
    rejects(malformed, "parser rejects an all-zero nonce prefix");

    malformed = envelope;
    writeLe32(malformed, 68, 1); // first chunk plaintext length
    rejects(malformed, "parser rejects a non-canonical chunk length");

    malformed = envelope;
    writeLe32(malformed, 64, 1); // first chunk index
    rejects(malformed, "parser rejects a non-contiguous chunk index");

    malformed = envelope;
    malformed.resize(sandbox::host::encrypted_volume::kEnvelopeHeaderBytes);
    rejects(malformed, "parser rejects a missing record area");

    malformed = envelope;
    malformed.push_back(0);
    rejects(malformed, "parser rejects trailing envelope bytes");

    // Deterministic mutation coverage exercises every envelope region without
    // making a claim that the test-only provider is cryptographically strong.
    std::uint32_t state = 0x13579bdfU;
    for (int iteration = 0; iteration < 2048; ++iteration) {
        state = state * 1664525U + 1013904223U;
        const std::size_t index = state % envelope.size();
        state = state * 1664525U + 1013904223U;
        ByteVector mutated = envelope;
        mutated[index] ^= static_cast<std::uint8_t>(1U << (state & 7U));
        ByteVector output;
        const bool accepted = sandbox::host::encrypted_volume::decryptBytes(
            mutated, key, provider, output, &error);
        if (!accepted) {
            expect(output.empty(), "mutation rejection leaves no plaintext");
        } else {
            expect(output == plain,
                   "accepted envelope mutation must preserve authenticated plaintext");
        }
    }
}

void exerciseFileHelpers(const AuthenticatedEncryptionProvider& provider) {
    const auto root = std::filesystem::temp_directory_path() / "trit-encrypted-volume-tests";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const auto input = root / "disk.tdisk";
    const auto encrypted = root / "disk.tenc";
    const auto output = root / "disk.roundtrip.tdisk";
    const auto tdisk_input = root / "canonical.tdisk";
    const auto tdisk_encrypted = root / "canonical.tenc";
    const auto tdisk_output = root / "canonical.roundtrip.tdisk";
    const ByteVector plain = makePlaintext();
    const ByteVector key = makeKey(0x31);
    std::string error;
    {
        std::ofstream file(input, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(plain.data()),
                   static_cast<std::streamsize>(plain.size()));
    }
    EncryptOptions options;
    options.chunk_bytes = 8192;
    options.logical_block_words = 27;
    options.nonce_prefix = {1, 2, 3, 4, 5, 6, 7, 8};
    options.nonce_prefix_provided = provider.algorithmId() == kAlgorithmTestOnly;
    expect(sandbox::host::encrypted_volume::encryptFile(
               input.string(), encrypted.string(), key, provider, options, &error),
           "file helper encrypts opaque tDisk payload: " + error);
    expect(sandbox::host::encrypted_volume::decryptFile(
               encrypted.string(), output.string(), key, provider, &error),
           "file helper decrypts opaque tDisk payload: " + error);
    ByteVector recovered;
    expect(sandbox::host::encrypted_volume::readFile(output.string(), recovered, &error),
           "file helper reads decrypted payload: " + error);
    expect(recovered == plain, "file helper preserves sparse-image bytes");

    const auto no_replace = root / "no-replace.bin";
    const ByteVector original_marker = {0x41, 0x42, 0x43};
    const ByteVector replacement_marker = {0x58, 0x59, 0x5a};
    error.clear();
    expect(sandbox::host::encrypted_volume::writeFileAtomic(
               no_replace.string(), original_marker, &error, false),
           "atomic helper creates a missing no-replace destination: " + error);
    error.clear();
    expect(!sandbox::host::encrypted_volume::writeFileAtomic(
               no_replace.string(), replacement_marker, &error, false),
           "atomic helper refuses a destination appearing before promotion");
    recovered.clear();
    expect(sandbox::host::encrypted_volume::readFile(
               no_replace.string(), recovered, &error) && recovered == original_marker,
           "no-replace promotion preserves the existing destination");

    const ByteVector canonical = makeCanonicalTdisk();
    {
        std::ofstream file(tdisk_input, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(canonical.data()),
                   static_cast<std::streamsize>(canonical.size()));
    }
    error.clear();
    expect(sandbox::host::encrypted_volume::encryptTdiskFile(
               tdisk_input.string(), tdisk_encrypted.string(), key, provider,
               options, &error),
           "tDisk helper accepts canonical v2 marker: " + error);
    expect(sandbox::host::encrypted_volume::decryptTdiskFile(
               tdisk_encrypted.string(), tdisk_output.string(), key, provider, &error),
           "tDisk helper validates decrypted marker: " + error);
    recovered.clear();
    expect(sandbox::host::encrypted_volume::readFile(
               tdisk_output.string(), recovered, &error) && recovered == canonical,
           "tDisk helper preserves canonical sparse image bytes");

    ByteVector invalid = canonical;
    invalid[24] ^= 0x01; // checksum byte
    const auto invalid_input = root / "invalid-checksum.tdisk";
    {
        std::ofstream file(invalid_input, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(invalid.data()),
                   static_cast<std::streamsize>(invalid.size()));
    }
    error.clear();
    expect(!sandbox::host::encrypted_volume::encryptTdiskFile(
               invalid_input.string(), tdisk_encrypted.string(), key, provider,
               options, &error),
           "tDisk helper rejects checksum-invalid input");
    expect(error.find("checksum") != std::string::npos,
           "tDisk checksum rejection is actionable without sensitive data");

    const auto blocked_output = root / "blocked-output";
    std::filesystem::create_directory(blocked_output, ec);
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptFile(
               tdisk_encrypted.string(), blocked_output.string(), key, provider,
               &error),
           "decrypt helper refuses to promote over an output directory");
    expect(std::filesystem::is_directory(blocked_output) &&
               !hasTemporaryFor(root, blocked_output.filename().string()),
           "failed plaintext promotion removes its temporary file");

    const auto collision = root / "preexisting.tmp";
    {
        std::ofstream file(collision, std::ios::binary | std::ios::trunc);
        file.put('K');
    }
    bool created = false;
    error.clear();
    expect(!sandbox::host::encrypted_volume::detail::createRestrictedTemporaryFile(
               collision, ByteVector{0x42}, created, &error) && !created,
           "exclusive temporary creation rejects a pre-existing path");
    std::ifstream preserved(collision, std::ios::binary);
    char preserved_byte = 0;
    preserved.get(preserved_byte);
    expect(preserved_byte == 'K',
           "exclusive temporary creation leaves a pre-existing path unchanged");
    std::filesystem::remove_all(root, ec);
}

void exerciseKeyFileHandling() {
    const auto root = std::filesystem::temp_directory_path() /
                      "trit-encrypted-volume-key-tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto key_path = root / "volume.key";
    const ByteVector expected = makeKey(0x5a);
    {
        std::ofstream file(key_path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(expected.data()),
                   static_cast<std::streamsize>(expected.size()));
    }
    ByteVector loaded;
    std::string error;
    expect(sandbox::host::encrypted_volume::readKeyFile(
               key_path.string(), loaded, &error) && loaded == expected,
           "key reader accepts exactly 32 raw bytes: " + error);

    const auto hex_path = root / "volume-hex.key";
    {
        const std::string encoded = hexEncode(expected);
        std::ofstream file(hex_path, std::ios::binary | std::ios::trunc);
        file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    }
    loaded = {0xaa};
    error.clear();
    expect(sandbox::host::encrypted_volume::readAes256KeyFile(
               hex_path.string(), loaded, &error) && loaded == expected,
           "key reader decodes exactly 64 hexadecimal characters: " + error);

    const auto invalid_hex_path = root / "invalid-hex.key";
    {
        std::ofstream file(invalid_hex_path, std::ios::binary | std::ios::trunc);
        const std::string invalid_hex(64, 'g');
        file.write(invalid_hex.data(),
                   static_cast<std::streamsize>(invalid_hex.size()));
    }
    loaded = {0xaa};
    error.clear();
    expect(!sandbox::host::encrypted_volume::readKeyFile(
               invalid_hex_path.string(), loaded, &error) && loaded.empty(),
           "key reader rejects non-hex 64-character files");

    const auto short_path = root / "short.key";
    {
        std::ofstream file(short_path, std::ios::binary | std::ios::trunc);
        file.write("short", 5);
    }
    loaded = {0xaa};
    error = "stale";
    expect(!sandbox::host::encrypted_volume::readKeyFile(
               short_path.string(), loaded, &error) && loaded.empty(),
           "key reader rejects short key files");

    const auto long_path = root / "long.key";
    {
        std::ofstream file(long_path, std::ios::binary | std::ios::trunc);
        const ByteVector long_key(33, 0x42);
        file.write(reinterpret_cast<const char*>(long_key.data()),
                   static_cast<std::streamsize>(long_key.size()));
    }
    expect(!sandbox::host::encrypted_volume::readAes256KeyFile(
               long_path.string(), loaded, &error) && loaded.empty(),
           "key reader rejects long key files");

    const auto directory_path = root / "key-directory";
    std::filesystem::create_directory(directory_path, ec);
    expect(!sandbox::host::encrypted_volume::readKeyFile(
               directory_path.string(), loaded, &error) && loaded.empty(),
           "key reader rejects directories");

    std::filesystem::remove_all(root, ec);
}

void exerciseDiagnosticsRedaction() {
    ErrorLeakingProvider provider;
    const ByteVector key = makeKey(0x13);
    EncryptOptions options;
    options.nonce_prefix = {1, 2, 3, 4, 5, 6, 7, 8};
    options.nonce_prefix_provided = true;
    ByteVector envelope;
    std::string error = "old key-shaped diagnostic";
    expect(!sandbox::host::encrypted_volume::encryptBytes(
               ByteVector(4096, 0x11), key, provider, envelope, options, &error),
           "host rejects a provider failure");
    expect(error.find("KKKK") == std::string::npos &&
               error.find("key material") == std::string::npos,
           "host redacts provider diagnostics");
}

} // namespace

int main() {
    TestOnlyDeterministicMock mock;
    exerciseProvider(mock, "test-only deterministic provider");
    exerciseParserBounds(mock);
    exerciseFileHelpers(mock);
    exerciseKeyFileHandling();
    exerciseDiagnosticsRedaction();

    sandbox::host::encrypted_volume::FailClosedProvider fail_closed;
    ByteVector output;
    std::string error;
    expect(!sandbox::host::encrypted_volume::encryptBytes(
               ByteVector{1, 2, 3}, makeKey(0), fail_closed, output, {}, &error),
           "default provider fails closed without crypto dependency");

#if !defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    sandbox::host::encrypted_volume::OpenSslAes256GcmProvider optional_openssl;
    EncryptOptions fail_closed_options;
    fail_closed_options.nonce_prefix = {1, 2, 3, 4, 5, 6, 7, 8};
    fail_closed_options.nonce_prefix_provided = true;
    error.clear();
    expect(!sandbox::host::encrypted_volume::encryptBytes(
               ByteVector(4096, 0x22), makeKey(0x21), optional_openssl,
               output, fail_closed_options, &error),
           "unlinked OpenSSL provider fails closed without a fallback cipher");
#endif

#if defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    sandbox::host::encrypted_volume::OpenSslAes256GcmProvider openssl;
    exerciseOpenSslKnownAnswer();
    exerciseOpenSslRandomizedMultiChunk();
    exerciseProvider(openssl, "OpenSSL AES-256-GCM provider");
    exerciseParserBounds(openssl);
    exerciseFileHelpers(openssl);
#endif

    if (failures != 0) {
        std::cerr << failures << " encrypted-volume checks failed\n";
        return 1;
    }
    std::cout << "encrypted-volume checks passed\n";
    return 0;
}
