#include "ota_verify.h"
#include "debug_log.h"
#include <cstring>

static constexpr const char* TAG = "ota_verify";

#ifndef SIMULATOR

#include "ota_public_key.h"
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/bignum.h>
#include <mbedtls/aes.h>

// Check for encryption key — only available if ota_encryption_key.h exists
#if __has_include("ota_encryption_key.h")
#include "ota_encryption_key.h"
#define HAS_ENCRYPTION_KEY 1
#else
#define HAS_ENCRYPTION_KEY 0
#endif

// Streaming SHA-256 context for large firmware verification
static mbedtls_sha256_context _sha256_ctx;
static bool _sha256_active = false;
static size_t _sha256_total = 0;

// Streaming CRC32
static uint32_t _crc32_accum = 0;

bool OtaVerify::verifySignature(const uint8_t* sig_r, const uint8_t* sig_s,
                                 const uint8_t* data, size_t data_len) {
    // Compute SHA-256 of data (ESP32 Arduino mbedtls functions return void)
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, data_len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    // Set up ECDSA verification
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = -1;

    // Load P-256 curve
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        DBG_ERROR(TAG, "Failed to load P-256 curve: -0x%04x", -ret);
        goto cleanup;
    }

    // Load public key from embedded constant
    ret = mbedtls_ecp_point_read_binary(&grp, &Q,
                                         OTA_SIGNING_PUBLIC_KEY,
                                         sizeof(OTA_SIGNING_PUBLIC_KEY));
    if (ret != 0) {
        DBG_ERROR(TAG, "Failed to load public key: -0x%04x", -ret);
        goto cleanup;
    }

    // Load signature components (big-endian)
    ret = mbedtls_mpi_read_binary(&r, sig_r, 32);
    if (ret != 0) goto cleanup;
    ret = mbedtls_mpi_read_binary(&s, sig_s, 32);
    if (ret != 0) goto cleanup;

    // Verify ECDSA signature
    ret = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s);

cleanup:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);

    if (ret == 0) {
        DBG_INFO(TAG, "Signature VALID");
        return true;
    } else {
        DBG_ERROR(TAG, "Signature INVALID (ret=-0x%04x)", -ret);
        return false;
    }
}

bool OtaVerify::beginVerify() {
    if (_sha256_active) {
        mbedtls_sha256_free(&_sha256_ctx);
    }
    mbedtls_sha256_init(&_sha256_ctx);
    mbedtls_sha256_starts(&_sha256_ctx, 0);  // void on ESP32 Arduino mbedtls
    _sha256_active = true;
    _sha256_total = 0;
    return true;
}

bool OtaVerify::updateVerify(const uint8_t* data, size_t len) {
    if (!_sha256_active) return false;
    mbedtls_sha256_update(&_sha256_ctx, data, len);  // void on ESP32 Arduino mbedtls
    _sha256_total += len;
    return true;
}

bool OtaVerify::finalizeVerify(const uint8_t* sig_r, const uint8_t* sig_s) {
    if (!_sha256_active) return false;
    _sha256_active = false;

    uint8_t hash[32];
    mbedtls_sha256_finish(&_sha256_ctx, hash);
    mbedtls_sha256_free(&_sha256_ctx);

    DBG_DEBUG(TAG, "SHA-256 of %u bytes: %02x%02x%02x%02x%02x%02x%02x%02x...",
              (unsigned)_sha256_total,
              hash[0], hash[1], hash[2], hash[3],
              hash[4], hash[5], hash[6], hash[7]);

    // Verify using same ECDSA code
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = -1;

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) goto cleanup;

    ret = mbedtls_ecp_point_read_binary(&grp, &Q,
                                         OTA_SIGNING_PUBLIC_KEY,
                                         sizeof(OTA_SIGNING_PUBLIC_KEY));
    if (ret != 0) goto cleanup;

    ret = mbedtls_mpi_read_binary(&r, sig_r, 32);
    if (ret != 0) goto cleanup;
    ret = mbedtls_mpi_read_binary(&s, sig_s, 32);
    if (ret != 0) goto cleanup;

    ret = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s);

cleanup:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);

    if (ret == 0) {
        DBG_INFO(TAG, "Streaming signature VALID");
        return true;
    } else {
        DBG_ERROR(TAG, "Streaming signature INVALID (ret=-0x%04x)", -ret);
        return false;
    }
}

uint32_t OtaVerify::crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

void OtaVerify::crc32Begin() {
    _crc32_accum = 0xFFFFFFFF;
}

void OtaVerify::crc32Update(const uint8_t* data, size_t len) {
    uint32_t crc = _crc32_accum;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    _crc32_accum = crc;
}

uint32_t OtaVerify::crc32Finalize() {
    return _crc32_accum ^ 0xFFFFFFFF;
}

// AES-256-CTR decryption
static mbedtls_aes_context _aes_ctx;
static uint8_t _aes_nonce_counter[16];
static uint8_t _aes_stream_block[16];
static size_t _aes_nc_off = 0;
static bool _aes_active = false;

bool OtaVerify::decryptBegin(const uint8_t* iv) {
#if HAS_ENCRYPTION_KEY
    if (_aes_active) {
        mbedtls_aes_free(&_aes_ctx);
    }
    mbedtls_aes_init(&_aes_ctx);
    int ret = mbedtls_aes_setkey_enc(&_aes_ctx, OTA_ENCRYPTION_KEY, 256);
    if (ret != 0) {
        DBG_ERROR(TAG, "AES key setup failed: -0x%04x", -ret);
        return false;
    }
    memcpy(_aes_nonce_counter, iv, 16);
    memset(_aes_stream_block, 0, 16);
    _aes_nc_off = 0;
    _aes_active = true;
    DBG_INFO(TAG, "AES-256-CTR decryption initialized");
    return true;
#else
    DBG_ERROR(TAG, "No encryption key compiled in");
    return false;
#endif
}

bool OtaVerify::decryptBlock(uint8_t* data, size_t len) {
#if HAS_ENCRYPTION_KEY
    if (!_aes_active) return false;
    int ret = mbedtls_aes_crypt_ctr(&_aes_ctx, len,
                                     &_aes_nc_off,
                                     _aes_nonce_counter,
                                     _aes_stream_block,
                                     data, data);
    return ret == 0;
#else
    return false;
#endif
}

void OtaVerify::decryptEnd() {
    if (_aes_active) {
        mbedtls_aes_free(&_aes_ctx);
        _aes_active = false;
        // Zero sensitive state
        memset(_aes_nonce_counter, 0, 16);
        memset(_aes_stream_block, 0, 16);
    }
}

#else // SIMULATOR

bool OtaVerify::verifySignature(const uint8_t*, const uint8_t*,
                                 const uint8_t*, size_t) { return false; }
bool OtaVerify::beginVerify() { return false; }
bool OtaVerify::updateVerify(const uint8_t*, size_t) { return false; }
bool OtaVerify::finalizeVerify(const uint8_t*, const uint8_t*) { return false; }
uint32_t OtaVerify::crc32(const uint8_t*, size_t) { return 0; }
bool OtaVerify::decryptBegin(const uint8_t*) { return false; }
bool OtaVerify::decryptBlock(uint8_t*, size_t) { return false; }
void OtaVerify::decryptEnd() {}
void OtaVerify::crc32Begin() {}
void OtaVerify::crc32Update(const uint8_t*, size_t) {}
uint32_t OtaVerify::crc32Finalize() { return 0; }

#endif
