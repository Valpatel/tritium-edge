#pragma once
#include "app.h"
#include "hal_ota.h"
#include "hal_sdcard.h"
#include "hal_espnow.h"
#include "hal_ble_ota.h"
#include "hal_provision.h"
#include "wifi_manager.h"
#include "ota_header.h"
#include "ota_verify.h"
#include "ota_mesh.h"
#include "ota_audit.h"

// ESP-NOW OTA protocol message types
enum class OtaMsgType : uint8_t {
    OFFER     = 0x10,  // "I have firmware v{X}, size {N}, CRC {C}"
    REQUEST   = 0x11,  // "Send me chunk {idx}"
    CHUNK     = 0x12,  // "Here is chunk {idx} with {len} bytes"
    DONE      = 0x13,  // "Transfer complete"
    ABORT     = 0x14,  // "Abort transfer"
    SIG       = 0x15,  // ECDSA P-256 signature (r[32] + s[32])
};

struct __attribute__((packed)) OtaOffer {
    OtaMsgType type;
    uint32_t firmware_size;
    uint32_t crc32;
    uint16_t chunk_count;
    uint8_t  version_len;
    uint8_t  is_signed;    // 1 if ECDSA signature will follow
    char     version[24];
};

struct __attribute__((packed)) OtaSigMsg {
    OtaMsgType type;
    uint8_t r[32];  // ECDSA P-256 signature r component
    uint8_t s[32];  // ECDSA P-256 signature s component
};

struct __attribute__((packed)) OtaChunkRequest {
    OtaMsgType type;
    uint16_t chunk_idx;
};

struct __attribute__((packed)) OtaChunk {
    OtaMsgType type;
    uint16_t chunk_idx;
    uint16_t data_len;
    // data follows (up to 220 bytes)
};

static constexpr uint16_t OTA_CHUNK_DATA_SIZE = 220;  // fits in 240-byte ESP-NOW payload with header

// Serial protocol commands
// PC sends: "OTA_BEGIN <size> <crc32>\n"
// Device responds: "OTA_READY\n"
// PC sends raw binary chunks, device ACKs each with "OTA_ACK <offset>\n"
// PC sends: "OTA_END\n"
// Device responds: "OTA_OK\n" or "OTA_FAIL <reason>\n"

class OtaApp : public App {
public:
    const char* name() override { return "OTA"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    // Display (esp_lcd framebuffer rendering)
    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0, _h = 0;
    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    static constexpr int CHUNK_ROWS = 40;
    void drawStatus();
    void pushFramebuffer();

    // OTA core
    OtaHAL _ota;
    SDCardHAL _sd;
    EspNowHAL _espnow;
    BleOtaHAL _ble_ota;

    ProvisionHAL _provision;
    WifiManager _wifi;
    bool _prov_ok = false;
    bool _wifi_ok = false;

    bool _ota_ok = false;
    bool _sd_ok = false;
    bool _espnow_ok = false;
    bool _ble_ok = false;

    // SD card OTA
    bool checkSDUpdate();

    // Serial OTA protocol
    void handleSerialOTA();
    enum class SerialOtaState { IDLE, RECEIVING, DONE, ERROR };
    SerialOtaState _serial_state = SerialOtaState::IDLE;
    uint32_t _serial_fw_size = 0;
    uint32_t _serial_fw_crc = 0;
    uint32_t _serial_received = 0;
    char _serial_buf[160] = {};  // Large enough for OTA_SIG command (8 + 64 + 1 + 64 hex)
    uint8_t _serial_buf_idx = 0;

    // ESP-NOW P2P OTA
    void handleEspNowOTA(const uint8_t* src, const uint8_t* data, uint8_t len, uint8_t hops);
    void offerFirmwareToMesh();
    bool startEspNowReceive(uint32_t size, uint32_t crc, uint16_t chunkCount);
    void requestNextChunk(const uint8_t* peer);

    enum class P2PState { IDLE, OFFERING, RECEIVING, SENDING, DONE, ERROR };
    P2PState _p2p_state = P2PState::IDLE;
    uint8_t _p2p_peer[6] = {};
    uint32_t _p2p_fw_size = 0;
    uint32_t _p2p_fw_crc = 0;
    uint16_t _p2p_chunk_count = 0;
    uint16_t _p2p_next_chunk = 0;
    uint32_t _p2p_received = 0;
    uint32_t _p2p_last_activity = 0;
    bool _p2p_signed = false;
    OtaSignature _p2p_signature = {};
    static constexpr uint32_t P2P_TIMEOUT_MS = 10000;

    // For sending firmware from SD to peers
    bool _have_firmware_file = false;
    uint32_t _offer_timer = 0;
    static constexpr uint32_t OFFER_INTERVAL_MS = 5000;

    // Mesh OTA (robust chunked P2P with flow control)
    OtaMesh _mesh_ota;

    // OTA audit log (persistent to LittleFS)
    OtaAuditLog _audit;

    // Serial OTA: signed firmware support + validation
    bool _serial_signed = false;
    OtaSignature _serial_signature = {};

    // Validate .ota header (board, version) — command: OTA_HEADER <hex>
    bool _validateOtaHeader(const OtaFirmwareHeader& hdr);

    // Fleet heartbeat (when WiFi connected + provisioned)
    uint32_t _heartbeat_timer = 0;
    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 30000;  // 30s
    void sendHeartbeat();

    // Mutual exclusion: only one OTA source active at a time
    enum class OtaSource : uint8_t { NONE, SRC_SERIAL, SRC_BLE, SRC_P2P, SRC_SD };
    OtaSource _ota_source = OtaSource::NONE;
    bool _claimOta(OtaSource src);
    void _releaseOta();

    // App confirmation — delay confirm to detect boot loops
    uint32_t _app_confirm_timer = 0;
    bool _app_confirmed = false;
    static constexpr uint32_t APP_CONFIRM_DELAY_MS = 30000;  // 30s stability window

    // OTA rate limiting — prevent flash wear from rapid retry
    uint32_t _last_ota_attempt_ms = 0;
    uint8_t _ota_fail_count = 0;
    static constexpr uint32_t OTA_COOLDOWN_MS = 5000;     // 5s between attempts
    static constexpr uint32_t OTA_FAIL_COOLDOWN_MS = 30000; // 30s after 3 failures
    static constexpr uint8_t OTA_MAX_FAILS = 3;
    bool _otaRateLimitOk();

    // Status
    char _status_line[64] = "Initializing...";
    uint8_t _progress = 0;
    uint32_t _status_timer = 0;
};
