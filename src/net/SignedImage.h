// Streaming check of an espsecure "version 1" signed image: the firmware image followed by
// a 68-byte block (uint32 0, then ECDSA P-256 r||s big-endian over SHA-256 of the image).
// Depends only on mbedTLS, so it also builds on the host for testing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mbedtls/sha256.h"

namespace ss {

class SignedImage {
public:
    static constexpr size_t kSigBlock = 4 + 64;

    SignedImage();
    ~SignedImage();
    SignedImage(const SignedImage&) = delete;
    SignedImage& operator=(const SignedImage&) = delete;

    // total = image + signature block, as announced by the sender.
    bool begin(size_t total, std::string& err);
    void feed(const uint8_t* data, size_t len);  // in order; bytes past `total` are ignored
    // publicKey = 64 bytes X||Y (espsecure extract_public_key --version 1).
    bool verify(const uint8_t publicKey[64], std::string& err);

private:
    mbedtls_sha256_context sha_;
    size_t total_ = 0;
    size_t imageLen_ = 0;
    size_t fed_ = 0;
    uint8_t sig_[kSigBlock] = {};
};

}  // namespace ss
