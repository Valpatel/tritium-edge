#pragma once
#include <cstdint>
#include <functional>

// BLE OTA Update Service
//
// BLE Service UUID: 0x1827 (Object Transfer Service - repurposed for OTA)
// Characteristics:
//   OTA Control (write+notify): receives commands, sends status
//   OTA Data (write-no-response): receives firmware chunks
//
// Protocol:
//   PC -> OTA_CTRL: BEGIN <size_le32> <crc32_le32> [SIG <64B r> <64B s>]
//   Device -> OTA_CTRL: READY / FAIL <reason>
//   PC -> OTA_DATA: <chunks up to MTU-3 bytes each>
//   Device -> OTA_CTRL: PROGRESS <pct> (every 4KB)
//   Device -> OTA_CTRL: OK / FAIL <reason>
//
// Control protocol (binary):
//   Command byte:
//     0x01 = BEGIN (followed by 4-byte size LE, 4-byte CRC32 LE)
//     0x02 = ABORT
//     0x03 = INFO_REQUEST
//     0x04 = SIG (followed by 64-byte signature)
//   Response byte:
//     0x10 = READY
//     0x11 = PROGRESS (followed by 1-byte percent)
//     0x12 = OK
//     0x13 = FAIL (followed by null-terminated reason)
//     0x14 = INFO (followed by JSON device info)

// UUIDs
#define BLE_OTA_SERVICE_UUID        "fb1e4001-54ae-4a28-9f74-dfccb248601d"
#define BLE_OTA_CTRL_CHAR_UUID      "fb1e4002-54ae-4a28-9f74-dfccb248601d"
#define BLE_OTA_DATA_CHAR_UUID      "fb1e4003-54ae-4a28-9f74-dfccb248601d"

enum class BleOtaCmd : uint8_t {
    BEGIN        = 0x01,
    ABORT        = 0x02,
    INFO_REQUEST = 0x03,
    SIG          = 0x04,
};

enum class BleOtaResp : uint8_t {
    READY    = 0x10,
    PROGRESS = 0x11,
    OK       = 0x12,
    FAIL     = 0x13,
    INFO     = 0x14,
};

using BleOtaProgressCb = std::function<void(uint8_t pct, uint32_t received, uint32_t total)>;

class BleOtaHAL {
public:
    bool init(const char* deviceName = "ESP32-OTA");
    void process();
    void stop();

    bool isConnected() const;
    bool isTransferring() const;
    uint8_t getProgress() const;

    void onProgress(BleOtaProgressCb cb);

private:
    bool _initialized = false;
    bool _connected = false;
    bool _transferring = false;
    uint32_t _fw_size = 0;
    uint32_t _fw_crc = 0;
    uint32_t _received = 0;
    uint8_t _progress = 0;
    BleOtaProgressCb _progressCb = nullptr;

    // Signature support
    bool _has_signature = false;
    uint8_t _sig_r[32] = {};
    uint8_t _sig_s[32] = {};

    void _sendCtrlResponse(BleOtaResp resp, const uint8_t* data = nullptr, size_t len = 0);
    void _handleCtrlWrite(const uint8_t* data, size_t len);
    void _handleDataWrite(const uint8_t* data, size_t len);
    void _finishTransfer();
    void _abortTransfer(const char* reason);

    // Deferred finish: ECDSA verification runs in process() to avoid stack overflow in NimBLE callback
    volatile bool _finish_pending = false;

    friend class BleOtaServerCallback;
    friend class BleOtaCtrlCallback;
    friend class BleOtaDataCallback;
};
