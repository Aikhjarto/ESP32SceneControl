#include "net/SignedImage.h"

#include <algorithm>
#include <cstring>

namespace ss {

static constexpr psa_algorithm_t kAlg = PSA_ALG_ECDSA(PSA_ALG_SHA_256);

SignedImage::SignedImage() : hash_(psa_hash_operation_init()) {}

SignedImage::~SignedImage() { psa_hash_abort(&hash_); }

bool SignedImage::begin(size_t total, std::string& err) {
    if (total <= kSigBlock) {
        err = "file too small to carry a signature";
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        err = "crypto init failed";
        return false;
    }
    total_ = total;
    imageLen_ = total - kSigBlock;
    fed_ = 0;
    psa_hash_abort(&hash_);
    hashOk_ = psa_hash_setup(&hash_, PSA_ALG_SHA_256) == PSA_SUCCESS;
    if (!hashOk_) err = "SHA-256 setup failed";
    return hashOk_;
}

void SignedImage::feed(const uint8_t* data, size_t len) {
    len = std::min(len, total_ - fed_);
    size_t imagePart = fed_ < imageLen_ ? std::min(len, imageLen_ - fed_) : 0;
    if (imagePart && psa_hash_update(&hash_, data, imagePart) != PSA_SUCCESS) hashOk_ = false;
    for (size_t i = imagePart; i < len; ++i) sig_[fed_ + i - imageLen_] = data[i];
    fed_ += len;
}

bool SignedImage::verify(const uint8_t publicKey[64], std::string& err) {
    if (fed_ != total_ || !hashOk_) {
        err = "incomplete image";
        return false;
    }
    static const uint8_t kNoKey[64] = {};
    if (std::memcmp(publicKey, kNoKey, sizeof(kNoKey)) == 0) {
        err = "this firmware was built without an OTA verification key";
        return false;
    }
    uint32_t version;
    std::memcpy(&version, sig_, 4);
    if (version != 0) {
        err = "unsupported signature block (image not signed with espsecure --version 1)";
        return false;
    }
    uint8_t hash[32];
    size_t hashLen = 0;
    if (psa_hash_finish(&hash_, hash, sizeof(hash), &hashLen) != PSA_SUCCESS) {
        err = "SHA-256 failed";
        return false;
    }

    uint8_t point[65];
    point[0] = 0x04;  // uncompressed X||Y
    std::memcpy(point + 1, publicKey, 64);

    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, kAlg);
    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, point, sizeof(point), &key);
    if (st == PSA_SUCCESS) {
        // PSA takes the raw r||s form, exactly as stored in the signature block.
        st = psa_verify_hash(key, kAlg, hash, hashLen, sig_ + 4, 64);
        psa_destroy_key(key);
    }
    if (st != PSA_SUCCESS) err = "signature check failed (unsigned image or signed with another key)";
    return st == PSA_SUCCESS;
}

}  // namespace ss
