#pragma once
#include <cstdint>
#include <cstddef>

// ECDSA P-256 signature verification for OTA firmware
// Uses ESP32's mbedtls (hardware-accelerated SHA-256)

class OtaVerify {
public:
    // Verify ECDSA P-256 signature over data using the embedded public key.
    // sig_r and sig_s are 32 bytes each (raw, big-endian).
    // data/data_len is the signed payload (firmware data only, no header).
    // Returns true if signature is valid.
    static bool verifySignature(const uint8_t* sig_r, const uint8_t* sig_s,
                                const uint8_t* data, size_t data_len);

    // Streaming verification for large firmware (avoids buffering entire image)
    // 1. Call beginVerify()
    // 2. Call updateVerify() with chunks of data
    // 3. Call finalizeVerify() with the signature
    static bool beginVerify();
    static bool updateVerify(const uint8_t* data, size_t len);
    static bool finalizeVerify(const uint8_t* sig_r, const uint8_t* sig_s);

    // Compute CRC32 of data
    static uint32_t crc32(const uint8_t* data, size_t len);

    // Streaming CRC32
    static void crc32Begin();
    static void crc32Update(const uint8_t* data, size_t len);
    static uint32_t crc32Finalize();

    // AES-256-CTR decryption for encrypted firmware
    // iv: 16-byte initialization vector (from first 16 bytes of encrypted payload)
    // Decrypt in-place: call decryptBlock() on each chunk sequentially.
    // The CTR counter state is maintained between calls.
    static bool decryptBegin(const uint8_t* iv);
    static bool decryptBlock(uint8_t* data, size_t len);
    static void decryptEnd();
};
