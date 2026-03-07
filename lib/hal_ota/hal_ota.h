#pragma once
#include <cstdint>
#include <functional>

// OTA update modes
enum class OtaSource : uint8_t {
    WIFI_PUSH,      // HTTP server push (ElegantOTA style)
    WIFI_PULL,      // Download from URL
    SD_CARD,        // Read firmware.bin from SD card
};

enum class OtaState : uint8_t {
    IDLE,
    CHECKING,       // Checking for update
    DOWNLOADING,    // Downloading/reading firmware
    WRITING,        // Writing to flash
    VERIFYING,      // MD5 verification
    READY_REBOOT,   // Success, waiting for reboot
    ERROR,
};

using OtaProgressCb = std::function<void(size_t current, size_t total)>;
using OtaStateCb = std::function<void(OtaState state, const char* message)>;

class OtaHAL {
public:
    bool init();

    // WiFi push OTA - starts HTTP server on given port
    // Browse to http://<ip>:<port>/update to upload firmware
    bool startServer(uint16_t port = 8080);
    void stopServer();
    bool isServerRunning() const;

    // WiFi pull OTA - download from URL
    bool updateFromUrl(const char* url);

    // SD card OTA - read firmware file from SD
    bool updateFromSD(const char* path = "/firmware.bin");

    // State
    OtaState getState() const;
    uint8_t getProgress() const;  // 0-100
    const char* getLastError() const;
    const char* getCurrentVersion() const;

    // Callbacks
    void onProgress(OtaProgressCb cb);
    void onStateChange(OtaStateCb cb);

    // Must call in loop() for WiFi push OTA
    void process();

    // Reboot after successful update
    void reboot();

    // Confirm app is working after OTA (prevents automatic rollback)
    bool confirmApp();

    // Rollback to previous firmware (if dual OTA partitions)
    bool rollback();
    bool canRollback() const;

    // Partition info
    const char* getRunningPartition() const;
    size_t getMaxFirmwareSize() const;

    // Firmware attestation: SHA-256 hash of running firmware partition.
    // Returns hex string (64 chars) into `out` buffer (must be >= 65 bytes).
    // Fleet server can verify device integrity by comparing against known hashes.
    bool getFirmwareHash(char* out, size_t outLen) const;

    // Test harness - tests OTA infrastructure without actually flashing
    struct TestResult {
        bool partition_ok;       // Dual OTA partitions detected
        bool server_start_ok;   // HTTP server starts
        bool server_stop_ok;    // HTTP server stops
        bool rollback_check_ok; // Rollback API works
        bool sd_detect_ok;      // SD card accessible (if present)
        const char* running_partition;
        size_t max_firmware_size;
        uint32_t test_duration_ms;
    };
    TestResult runTest();

    // Validate OTA header fields (board, version). Returns true if OK.
    // Sets error message and returns false on rejection.
    bool validateHeader(const char* board, const char* version);

private:
    void _setState(OtaState state, const char* msg = nullptr);
    void _setError(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void _reportProgress(size_t current, size_t total);

    OtaState _state = OtaState::IDLE;
    uint8_t _progress = 0;
    char _error[128] = {0};
    char _version[32] = {0};
    OtaProgressCb _progressCb = nullptr;
    OtaStateCb _stateCb = nullptr;
    bool _serverRunning = false;

    // Allow HTTP handler free functions access to private members
    friend void _otaHandleUpload();
    friend void _otaHandleResult();
};
