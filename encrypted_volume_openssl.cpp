#include "encrypted_volume_host.h"

#include <climits>

#if defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace sandbox::host::encrypted_volume {
namespace {

void setError(std::string* error, const char* text) {
    if (error) *error = text;
}

void cleanse(ByteVector& value) {
#if defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
#else
    std::fill(value.begin(), value.end(), 0);
#endif
    value.clear();
    value.shrink_to_fit();
}

#if defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
struct OpenSslErrorQueueGuard final {
    OpenSslErrorQueueGuard() { ERR_clear_error(); }
    ~OpenSslErrorQueueGuard() { ERR_clear_error(); }
};

EVP_CIPHER* fetchAes256Gcm(std::string* error) {
    // EVP_CIPHER_fetch is the OpenSSL 3 provider boundary.  The caller never
    // falls back to a legacy or home-grown cipher if the provider is absent.
    EVP_CIPHER* cipher = EVP_CIPHER_fetch(nullptr, "AES-256-GCM", nullptr);
    if (!cipher) {
        setError(error, "OpenSSL AES-256-GCM provider is unavailable");
    }
    return cipher;
}

bool validAesGcmSizes(const ByteVector& key,
                     const ByteVector& nonce,
                     const ByteVector& aad,
                     const ByteVector& payload,
                     const ByteVector* tag,
                     std::string* error) {
    if (key.size() != kAes256KeyBytes || nonce.size() != kAesGcmNonceBytes ||
        (tag && tag->size() != kAesGcmTagBytes) ||
        aad.size() > static_cast<std::size_t>(INT_MAX) ||
        payload.size() > static_cast<std::size_t>(INT_MAX)) {
        setError(error, "OpenSSL AES-256-GCM input size is invalid");
        return false;
    }
    return true;
}

bool updateAad(EVP_CIPHER_CTX* context,
               const ByteVector& aad,
               std::string* error) {
    if (aad.empty()) return true;
    int ignored = 0;
    if (EVP_EncryptUpdate(context, nullptr, &ignored,
                          aad.data(), static_cast<int>(aad.size())) <= 0) {
        setError(error, "OpenSSL AES-256-GCM AAD processing failed");
        return false;
    }
    return true;
}

bool updateDecryptAad(EVP_CIPHER_CTX* context,
                      const ByteVector& aad,
                      std::string* error) {
    if (aad.empty()) return true;
    int ignored = 0;
    if (EVP_DecryptUpdate(context, nullptr, &ignored,
                          aad.data(), static_cast<int>(aad.size())) <= 0) {
        setError(error, "OpenSSL AES-256-GCM AAD processing failed");
        return false;
    }
    return true;
}
#endif

} // namespace

bool OpenSslAes256GcmProvider::encrypt(const ByteVector& key,
                                       const ByteVector& nonce,
                                       const ByteVector& aad,
                                       const ByteVector& plaintext,
                                       ByteVector& ciphertext,
                                       ByteVector& tag,
                                       std::string* error) const {
    cleanse(ciphertext);
    cleanse(tag);
    if (error) error->clear();
#if !defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    (void)key;
    (void)nonce;
    (void)aad;
    (void)plaintext;
    setError(error, "OpenSSL AES-256-GCM provider is not linked");
    return false;
#else
    OpenSslErrorQueueGuard error_queue;
    if (!validAesGcmSizes(key, nonce, aad, plaintext, nullptr, error)) return false;
    EVP_CIPHER* cipher = fetchAes256Gcm(error);
    if (!cipher) return false;
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) {
        EVP_CIPHER_free(cipher);
        setError(error, "OpenSSL AES-256-GCM context allocation failed");
        return false;
    }
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(context, cipher, nullptr, nullptr, nullptr) <= 0 ||
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), nullptr) <= 0 ||
            EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) <= 0) {
            setError(error, "OpenSSL AES-256-GCM initialization failed");
            break;
        }
        if (!updateAad(context, aad, error)) break;
        ciphertext.resize(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
        int written = 0;
        int total = 0;
        if (!plaintext.empty() &&
            EVP_EncryptUpdate(context, ciphertext.data(), &written,
                              plaintext.data(), static_cast<int>(plaintext.size())) <= 0) {
            setError(error, "OpenSSL AES-256-GCM encryption failed");
            break;
        }
        total += written;
        written = 0;
        if (EVP_EncryptFinal_ex(context, ciphertext.data() + total, &written) <= 0) {
            setError(error, "OpenSSL AES-256-GCM finalization failed");
            break;
        }
        total += written;
        ciphertext.resize(static_cast<std::size_t>(total));
        tag.resize(kAesGcmTagBytes);
        if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG,
                                static_cast<int>(tag.size()), tag.data()) <= 0) {
            setError(error, "OpenSSL AES-256-GCM tag generation failed");
            ciphertext.clear();
            tag.clear();
            break;
        }
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(context);
    EVP_CIPHER_free(cipher);
    if (!ok) {
        cleanse(ciphertext);
        cleanse(tag);
        if (error && error->empty()) *error = "OpenSSL AES-256-GCM encryption failed";
    }
    return ok;
#endif
}

bool OpenSslAes256GcmProvider::decrypt(const ByteVector& key,
                                       const ByteVector& nonce,
                                       const ByteVector& aad,
                                       const ByteVector& ciphertext,
                                       const ByteVector& tag,
                                       ByteVector& plaintext,
                                       std::string* error) const {
    cleanse(plaintext);
    if (error) error->clear();
#if !defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    (void)key;
    (void)nonce;
    (void)aad;
    (void)ciphertext;
    (void)tag;
    setError(error, "OpenSSL AES-256-GCM provider is not linked");
    return false;
#else
    OpenSslErrorQueueGuard error_queue;
    if (!validAesGcmSizes(key, nonce, aad, ciphertext, &tag, error)) return false;
    EVP_CIPHER* cipher = fetchAes256Gcm(error);
    if (!cipher) return false;
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) {
        EVP_CIPHER_free(cipher);
        setError(error, "OpenSSL AES-256-GCM context allocation failed");
        return false;
    }
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(context, cipher, nullptr, nullptr, nullptr) <= 0 ||
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), nullptr) <= 0 ||
            EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) <= 0) {
            setError(error, "OpenSSL AES-256-GCM initialization failed");
            break;
        }
        if (!updateDecryptAad(context, aad, error)) break;
        plaintext.resize(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
        int written = 0;
        int total = 0;
        if (!ciphertext.empty() &&
            EVP_DecryptUpdate(context, plaintext.data(), &written,
                              ciphertext.data(), static_cast<int>(ciphertext.size())) <= 0) {
            setError(error, "OpenSSL AES-256-GCM decryption failed");
            break;
        }
        total += written;
        if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG,
                                static_cast<int>(tag.size()),
                                const_cast<std::uint8_t*>(tag.data())) <= 0) {
            setError(error, "OpenSSL AES-256-GCM tag setup failed");
            break;
        }
        written = 0;
        // A non-positive final result is the only signal needed by callers;
        // do not expose OpenSSL's detailed error queue or any input material.
        if (EVP_DecryptFinal_ex(context, plaintext.data() + total, &written) <= 0) {
            setError(error, "encrypted-volume authentication failed");
            break;
        }
        total += written;
        plaintext.resize(static_cast<std::size_t>(total));
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(context);
    EVP_CIPHER_free(cipher);
    if (!ok) {
        cleanse(plaintext);
        if (error && error->empty()) *error = "encrypted-volume authentication failed";
    }
    return ok;
#endif
}

bool OpenSslAes256GcmProvider::randomBytes(std::uint8_t* destination,
                                           std::size_t size,
                                           std::string* error) const {
    if (error) error->clear();
#if !defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    (void)destination;
    (void)size;
    if (destination && size != 0) std::fill(destination, destination + size, 0);
    setError(error, "OpenSSL AES-256-GCM provider is not linked");
    return false;
#else
    OpenSslErrorQueueGuard error_queue;
    if (size > static_cast<std::size_t>(INT_MAX) || (size != 0 && !destination)) {
        setError(error, "OpenSSL secure-random request is invalid");
        return false;
    }
    if (size == 0) return true;
    if (RAND_bytes(destination, static_cast<int>(size)) != 1) {
        OPENSSL_cleanse(destination, size);
        setError(error, "OpenSSL secure-random request failed");
        return false;
    }
    return true;
#endif
}

} // namespace sandbox::host::encrypted_volume
