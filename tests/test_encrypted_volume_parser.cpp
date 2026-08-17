#include "encrypted_volume_host.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

using sandbox::host::encrypted_volume::AuthenticatedEncryptionProvider;
using sandbox::host::encrypted_volume::ByteVector;
using sandbox::host::encrypted_volume::EncryptOptions;
using sandbox::host::encrypted_volume::kAesGcmNonceBytes;
using sandbox::host::encrypted_volume::kAesGcmTagBytes;
using sandbox::host::encrypted_volume::kAlgorithmTestOnly;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
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
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class FramingProvider final : public AuthenticatedEncryptionProvider {
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
            if (error) *error = "test framing input is invalid";
            ciphertext.clear();
            tag.clear();
            return false;
        }
        ciphertext = plaintext;
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
            tag.size() != tagBytes() || makeTag(key, nonce, aad, ciphertext) != tag) {
            if (error) *error = "test framing authentication failed";
            return false;
        }
        plaintext = ciphertext;
        return true;
    }

    [[nodiscard]] bool randomBytes(std::uint8_t* destination,
                                   std::size_t size,
                                   std::string*) const override {
        if (size != 8 || !destination) return false;
        for (std::size_t index = 0; index < size; ++index)
            destination[index] = static_cast<std::uint8_t>(0x70U + index);
        return true;
    }

private:
    static ByteVector makeTag(const ByteVector& key,
                              const ByteVector& nonce,
                              const ByteVector& aad,
                              const ByteVector& ciphertext) {
        ByteVector input;
        input.insert(input.end(), key.begin(), key.end());
        input.insert(input.end(), nonce.begin(), nonce.end());
        input.insert(input.end(), aad.begin(), aad.end());
        input.insert(input.end(), ciphertext.begin(), ciphertext.end());
        std::uint64_t hash = fnv1a(input);
        ByteVector tag(kAesGcmTagBytes, 0);
        for (std::size_t index = 0; index < tag.size(); ++index) {
            tag[index] = static_cast<std::uint8_t>(hash >> ((index % 8) * 8));
            if ((index % 8) == 7) hash = hash * 6364136223846793005ULL + 1;
        }
        return tag;
    }
};

ByteVector makeKey() {
    ByteVector key(32);
    for (std::size_t index = 0; index < key.size(); ++index)
        key[index] = static_cast<std::uint8_t>(index * 5U + 3U);
    return key;
}

ByteVector makeCanonicalTdisk() {
    ByteVector records;
    appendLe32(records, 1);
    for (std::uint32_t word = 0; word < 27; ++word)
        appendLe64(records, word == 0 ? 7 : 0);
    appendLe32(records, 5);
    for (std::uint32_t word = 0; word < 27; ++word)
        appendLe64(records, word == 1 ? 9 : 0);

    ByteVector disk;
    appendLe64(disk, sandbox::host::encrypted_volume::kTdiskV2Magic);
    appendLe32(disk, 2);
    appendLe32(disk, 27);
    appendLe64(disk, 4);
    appendLe64(disk, fnv1a(records));
    appendLe32(disk, 2);
    disk.insert(disk.end(), records.begin(), records.end());
    return disk;
}

void refreshTdiskChecksum(ByteVector& disk) {
    const ByteVector records(disk.begin() + 36, disk.end());
    writeLe64(disk, 24, fnv1a(records));
}

void testEnvelopeParser() {
    FramingProvider provider;
    const ByteVector key = makeKey();
    ByteVector plain(8193);
    for (std::size_t index = 0; index < plain.size(); ++index)
        plain[index] = static_cast<std::uint8_t>(index * 17U + 11U);

    EncryptOptions options;
    options.chunk_bytes = 4096;
    options.nonce_prefix = {1, 3, 5, 7, 9, 11, 13, 15};
    options.nonce_prefix_provided = true;
    ByteVector envelope;
    std::string error;
    expect(sandbox::host::encrypted_volume::encryptBytes(
               plain, key, provider, envelope, options, &error),
           "parser fixture encryption succeeds: " + error);

    ByteVector recovered;
    expect(sandbox::host::encrypted_volume::decryptBytes(
               envelope, key, provider, recovered, &error) && recovered == plain,
           "parser fixture roundtrip succeeds");

    auto rejects = [&](ByteVector malformed, const std::string& label) {
        ByteVector output{0xff};
        expect(!sandbox::host::encrypted_volume::decryptBytes(
                   malformed, key, provider, output, &error), label);
        expect(output.empty(), label + " does not expose plaintext");
    };

    for (std::size_t length = 0; length < 128; ++length) {
        ByteVector truncated(length, static_cast<std::uint8_t>(length ^ 0xa5U));
        rejects(std::move(truncated),
                "parser rejects bounded arbitrary input length " +
                    std::to_string(length));
    }

    ByteVector malformed = envelope;
    writeLe64(malformed, 32, std::numeric_limits<std::uint64_t>::max());
    rejects(malformed, "parser rejects logical-size overflow input");
    malformed = envelope;
    writeLe64(malformed, 48, std::numeric_limits<std::uint64_t>::max());
    rejects(malformed, "parser rejects chunk-count overflow input");
    malformed = envelope;
    writeLe32(malformed, 68, 1);
    rejects(malformed, "parser rejects a short non-final chunk");
    malformed = envelope;
    malformed.resize(malformed.size() - 1);
    rejects(malformed, "parser rejects truncation before staging");
    malformed = envelope;
    malformed.push_back(0);
    rejects(malformed, "parser rejects trailing bytes before staging");

    std::uint32_t state = 0x2468ace1U;
    for (int iteration = 0; iteration < 4096; ++iteration) {
        state = state * 1103515245U + 12345U;
        const std::size_t index = state % envelope.size();
        state = state * 1103515245U + 12345U;
        ByteVector mutated = envelope;
        mutated[index] ^= static_cast<std::uint8_t>(1U << (state & 7U));
        ByteVector output;
        const bool accepted = sandbox::host::encrypted_volume::decryptBytes(
            mutated, key, provider, output, &error);
        if (accepted) {
            expect(output == plain,
                   "accepted mutation preserves authenticated plaintext");
        } else {
            expect(output.empty(), "rejected mutation does not expose plaintext");
        }
    }
}

void testCanonicalDiskParser() {
    const ByteVector canonical = makeCanonicalTdisk();
    std::string error;
    expect(sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               canonical, &error),
           "canonical tDisk fixture is accepted: " + error);

    ByteVector malformed = canonical;
    malformed[24] ^= 1;
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "tDisk checksum mutation is rejected");

    malformed = canonical;
    malformed.push_back(0);
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "tDisk trailing bytes are rejected");

    malformed = canonical;
    writeLe32(malformed, 32, 3); // count no longer matches exact record area
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "tDisk record-count mismatch is rejected");

    malformed = canonical;
    writeLe32(malformed, 36, 0xffffffffU); // first block index is negative
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "negative tDisk block index is rejected");

    malformed = canonical;
    writeLe32(malformed, 256, 1); // second record duplicates the first index
    refreshTdiskChecksum(malformed);
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "duplicate tDisk block indices are rejected");

    malformed = canonical;
    std::fill(malformed.begin() + 260, malformed.end(), 0);
    refreshTdiskChecksum(malformed);
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "all-zero tDisk records are rejected");

    malformed = canonical;
    writeLe64(malformed, 268,
              sandbox::host::encrypted_volume::kMaximumT40Raw);
    refreshTdiskChecksum(malformed);
    expect(sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "maximum valid raw T40 word is accepted");

    malformed = canonical;
    writeLe64(malformed, 268,
              sandbox::host::encrypted_volume::kMaximumT40Raw + 1);
    refreshTdiskChecksum(malformed);
    expect(!sandbox::host::encrypted_volume::isCanonicalTdiskV2(
               malformed, &error), "invalid raw T40 words are rejected");
}

} // namespace

int main() {
    testEnvelopeParser();
    testCanonicalDiskParser();
    if (failures != 0) {
        std::cerr << failures << " encrypted-volume parser checks failed\n";
        return 1;
    }
    std::cout << "encrypted-volume parser checks passed\n";
    return 0;
}
