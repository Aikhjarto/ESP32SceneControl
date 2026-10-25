// Streaming check of an espsecure "version 1" signed image: the firmware image followed by
// a 68-byte block (uint32 0, then ECDSA P-256 r||s big-endian over SHA-256 of the image).
// Uses only the PSA Crypto API, so it also builds on the host for testing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "psa/crypto.h"

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
    psa_hash_operation_t hash_;
    bool hashOk_ = false;
    size_t total_ = 0;
    size_t imageLen_ = 0;
    size_t fed_ = 0;
    uint8_t sig_[kSigBlock] = {};
};

}  // namespace ss
