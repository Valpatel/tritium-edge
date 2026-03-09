#pragma once
#include "ota_header.h"
#include <cstdint>
#include <cstddef>
#include <functional>

// ESP-NOW Mesh OTA: Peer-to-peer firmware distribution over ESP-NOW mesh
//
// Protocol:
//   1. Sender broadcasts OTA_ANNOUNCE with firmware header (version, size, board, signed)
//   2. Interested receivers reply OTA_REQUEST for chunk 0
//   3. Sender sends OTA_CHUNK packets (seq 0, 1, 2, ...)
//   4. Receiver ACKs every N chunks (sliding window)
//   5. After last chunk, receiver verifies CRC32 + ECDSA signature
//   6. Receiver sends OTA_STATUS with result
//
// Chunk size: 200 bytes (fits in 240-byte ESP-NOW payload with 40-byte protocol header)
// 1MB firmware = ~5120 chunks, ~25 seconds at 200 chunks/sec

static constexpr size_t OTA_MESH_CHUNK_SIZE = 200;
static constexpr size_t OTA_MESH_WINDOW_SIZE = 4;  // ACK every N chunks
static constexpr uint32_t OTA_MESH_CHUNK_TIMEOUT_MS = 5000;  // Timeout waiting for chunk
static constexpr uint32_t OTA_MESH_ACK_TIMEOUT_MS = 3000;    // Timeout waiting for ACK
static constexpr uint8_t OTA_MESH_MAX_RETRIES = 5;

// Mesh OTA packet types (carried inside mesh DATA messages)
enum class OtaMeshType : uint8_t {
    ANNOUNCE  = 0x40,  // Sender -> broadcast: "I have firmware v1.2.3"
    REQUEST   = 0x41,  // Receiver -> sender: "Send me chunk N"
    CHUNK     = 0x42,  // Sender -> receiver: chunk data
    ACK       = 0x43,  // Receiver -> sender: "Got chunks up to N"
    STATUS    = 0x44,  // Receiver -> sender: final result (success/fail)
    ABORT     = 0x45,  // Either -> other: cancel transfer
};

// OTA_ANNOUNCE payload (fits in single ESP-NOW packet)
struct __attribute__((packed)) OtaMeshAnnounce {
    OtaMeshType type;          // = ANNOUNCE
    uint16_t session_id;       // Random ID for this transfer
    uint32_t firmware_size;    // Total firmware payload size
    uint32_t firmware_crc32;   // CRC32 of firmware payload
    uint16_t total_chunks;     // Total number of chunks
    uint16_t header_version;   // OTA header version (1=unsigned, 2=signed)
    uint16_t flags;            // OTA header flags
    char version[24];          // Firmware version string
    char board[16];            // Target board
    // If signed, signature follows in a second ANNOUNCE packet (part 2)
};

// OTA_ANNOUNCE part 2: signature (only sent for signed firmware)
struct __attribute__((packed)) OtaMeshAnnounceSig {
    OtaMeshType type;          // = ANNOUNCE
    uint16_t session_id;
    uint8_t part;              // = 2 (signature part)
    uint8_t sig_r[32];
    uint8_t sig_s[32];
};

// OTA_CHUNK payload
struct __attribute__((packed)) OtaMeshChunkHeader {
    OtaMeshType type;          // = CHUNK
    uint16_t session_id;
    uint16_t seq;              // Chunk sequence number (0-based)
    uint8_t len;               // Actual data length in this chunk (<=200)
    // Followed by `len` bytes of firmware data
};

// OTA_REQUEST / OTA_ACK payload
struct __attribute__((packed)) OtaMeshAck {
    OtaMeshType type;          // = REQUEST, ACK, or ABORT
    uint16_t session_id;
    uint16_t next_seq;         // Next chunk expected (for ACK) or requested (for REQUEST)
};

// OTA_STATUS payload
struct __attribute__((packed)) OtaMeshStatus {
    OtaMeshType type;          // = STATUS
    uint16_t session_id;
    uint8_t success;           // 1=verified OK, 0=failed
    uint8_t error_code;        // 0=ok, 1=crc fail, 2=sig fail, 3=write fail, 4=board mismatch
};

// Callbacks
using OtaMeshProgressCb = std::function<void(size_t received, size_t total)>;
using OtaMeshResultCb = std::function<void(bool success, const char* version)>;

// Forward declaration
class EspNowHAL;

class OtaMesh {
public:
    // Initialize mesh OTA with reference to ESP-NOW HAL
    bool init(EspNowHAL* espnow);

    // --- Sender API ---
    // Start broadcasting firmware from SD card file
    bool startSend(const char* otaFilePath);
    // Start broadcasting firmware from a buffer (for gateway relaying from HTTP)
    bool startSendFromBuffer(const uint8_t* data, size_t len);
    void stopSend();
    bool isSending() const { return _sending; }

    // --- Receiver API ---
    // Enable/disable listening for OTA announcements
    void enableReceive(bool enable);
    bool isReceiving() const { return _receiving; }
    bool isTransferActive() const { return _transferActive; }

    // Call in loop()
    void process();

    // Callbacks
    void onProgress(OtaMeshProgressCb cb) { _progressCb = cb; }
    void onResult(OtaMeshResultCb cb) { _resultCb = cb; }

    // Stats
    struct Stats {
        uint32_t chunks_sent;
        uint32_t chunks_received;
        uint32_t retransmits;
        uint32_t acks_sent;
        uint32_t acks_received;
        uint32_t announces_sent;
        uint32_t announces_heard;
        uint32_t transfers_completed;
        uint32_t transfers_failed;
    };
    Stats getStats() const { return _stats; }

private:
    EspNowHAL* _espnow = nullptr;
    bool _sending = false;
    bool _receiving = false;
    bool _transferActive = false;

    // Sender state
    struct {
        uint16_t session_id;
        OtaFirmwareHeader hdr;
        OtaSignature sig;
        bool isSigned;
        uint16_t totalChunks;
        uint16_t nextChunk;        // Next chunk to send
        uint16_t ackedChunk;       // Last acknowledged chunk
        uint8_t retryCount;
        uint32_t lastSendMs;
        uint8_t targetMac[6];      // Receiver MAC (set on first REQUEST)
        bool hasTarget;
        // Source: SD file or buffer
        bool fromBuffer;
        const uint8_t* bufData;
        size_t bufLen;
        size_t headerSize;
    } _tx;

    // Receiver state
    struct {
        uint16_t session_id;
        OtaFirmwareHeader hdr;
        OtaSignature sig;
        bool isSigned;
        bool isEncrypted;
        uint16_t totalChunks;
        uint16_t nextExpected;     // Next chunk seq expected
        uint8_t senderMac[6];
        uint32_t lastChunkMs;
        size_t bytesReceived;
        bool flashStarted;
    } _rx;

    OtaMeshProgressCb _progressCb = nullptr;
    OtaMeshResultCb _resultCb = nullptr;
    Stats _stats = {};

    // Internal handlers
    void handleMeshData(const uint8_t* origin_mac, const uint8_t* data, uint8_t len, uint8_t hops);
    void handleAnnounce(const uint8_t* mac, const uint8_t* data, uint8_t len);
    void handleAnnounceSig(const uint8_t* mac, const uint8_t* data, uint8_t len);
    void handleChunk(const uint8_t* mac, const uint8_t* data, uint8_t len);
    void handleAck(const uint8_t* mac, const uint8_t* data, uint8_t len);
    void handleRequest(const uint8_t* mac, const uint8_t* data, uint8_t len);
    void handleStatus(const uint8_t* mac, const uint8_t* data, uint8_t len);
    void handleAbort(const uint8_t* mac, const uint8_t* data, uint8_t len);

    void sendAnnounce();
    bool sendChunk(uint16_t seq);
    void sendAck(uint16_t nextSeq);
    void sendStatus(bool success, uint8_t errorCode);
    void sendAbort();
    void abortTransfer(const char* reason);

    // Read chunk data from source (SD or buffer)
    size_t readChunkData(uint16_t seq, uint8_t* out, size_t maxLen);
};
