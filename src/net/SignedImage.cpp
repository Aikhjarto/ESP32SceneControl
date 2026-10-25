#include "net/SignedImage.h"

#include <algorithm>
#include <cstring>

#include "mbedtls/ecdsa.h"

namespace ss {

SignedImage::SignedImage() { mbedtls_sha256_init(&sha_); }

SignedImage::~SignedImage() { mbedtls_sha256_free(&sha_); }

bool SignedImage::begin(size_t total, std::string& err) {
    if (total <= kSigBlock) {
        err = "file too small to carry a signature";
        return false;
    }
    total_ = total;
    imageLen_ = total - kSigBlock;
    fed_ = 0;
    mbedtls_sha256_starts(&sha_, 0);
    return true;
}

void SignedImage::feed(const uint8_t* data, size_t len) {
    len = std::min(len, total_ - fed_);
    size_t imagePart = fed_ < imageLen_ ? std::min(len, imageLen_ - fed_) : 0;
    mbedtls_sha256_update(&sha_, data, imagePart);
    for (size_t i = imagePart; i < len; ++i) sig_[fed_ + i - imageLen_] = data[i];
    fed_ += len;
}

bool SignedImage::verify(const uint8_t publicKey[64], std::string& err) {
    if (fed_ != total_) {
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
    mbedtls_sha256_finish(&sha_, hash);

    uint8_t point[65];
    point[0] = 0x04;  // uncompressed X||Y
    std::memcpy(point + 1, publicKey, 64);

    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &q, point, sizeof(point));
    if (rc == 0) rc = mbedtls_mpi_read_binary(&r, sig_ + 4, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&s, sig_ + 36, 32);
    if (rc == 0) rc = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) err = "signature check failed (unsigned image or signed with another key)";
    return rc == 0;
}

}  // namespace ss
