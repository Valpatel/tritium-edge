#include "hal_ble_ota.h"
#include "debug_log.h"

static constexpr const char* TAG = "ble_ota";

#ifndef SIMULATOR

#include <NimBLEDevice.h>
#include "esp_ota_ops.h"
#include <esp_ota_ops.h>
#include "ota_header.h"
#include "ota_verify.h"

static NimBLEServer* _server = nullptr;
static NimBLECharacteristic* _ctrlChar = nullptr;
static NimBLECharacteristic* _dataChar = nullptr;
static BleOtaHAL* _instance = nullptr;

// BLE Server callbacks
class BleOtaServerCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server) override {
        if (_instance) {
            _instance->_connected = true;
            DBG_INFO(TAG, "BLE client connected");
        }
    }
    void onDisconnect(NimBLEServer* server) override {
        if (_instance) {
            _instance->_connected = false;
            DBG_INFO(TAG, "BLE client disconnected");
            if (_instance->_transferring && _instance->_received < _instance->_fw_size) {
                // Only abort if transfer is still in progress (not during verification)
                _instance->_abortTransfer("client disconnected");
            }
            // Restart advertising
            NimBLEDevice::startAdvertising();
        }
    }
};

// Control characteristic callbacks
class BleOtaCtrlCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar) override {
        NimBLEAttValue value = pChar->getValue();
        DBG_INFO(TAG, "BLE ctrl write: %u bytes", value.length());
        if (_instance && value.length() > 0) {
            _instance->_handleCtrlWrite(value.data(), value.length());
        }
    }
};

// Data characteristic callbacks
class BleOtaDataCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar) override {
        NimBLEAttValue value = pChar->getValue();
        if (_instance && value.length() > 0) {
            _instance->_handleDataWrite(value.data(), value.length());
        }
    }
};

bool BleOtaHAL::init(const char* deviceName) {
    if (_initialized) return true;
    _instance = this;

    NimBLEDevice::init(deviceName);
    // Request large MTU for faster transfer
    NimBLEDevice::setMTU(517);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new BleOtaServerCallback());

    // Create OTA service
    NimBLEService* service = _server->createService(BLE_OTA_SERVICE_UUID);

    // Control characteristic (read + write + notify)
    _ctrlChar = service->createCharacteristic(
        BLE_OTA_CTRL_CHAR_UUID,
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::NOTIFY
    );
    _ctrlChar->setCallbacks(new BleOtaCtrlCallback());

    // Data characteristic (write + write-no-response)
    _dataChar = service->createCharacteristic(
        BLE_OTA_DATA_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR
    );
    _dataChar->setCallbacks(new BleOtaDataCallback());

    service->start();

    // Start advertising
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_OTA_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    NimBLEDevice::startAdvertising();

    _initialized = true;
    DBG_INFO(TAG, "BLE OTA initialized, advertising as '%s'", deviceName);
    return true;
}

void BleOtaHAL::process() {
    // Deferred finish: ECDSA verification needs more stack than NimBLE callback provides
    if (_finish_pending) {
        _finish_pending = false;
        DBG_INFO(TAG, "Running deferred _finishTransfer from main loop");
        _finishTransfer();
    }
}

void BleOtaHAL::stop() {
    if (!_initialized) return;
    if (_transferring) {
        _abortTransfer("stopping BLE");
    }
    NimBLEDevice::deinit(true);
    _initialized = false;
    _server = nullptr;
    _ctrlChar = nullptr;
    _dataChar = nullptr;
    DBG_INFO(TAG, "BLE OTA stopped");
}

bool BleOtaHAL::isConnected() const { return _connected; }
bool BleOtaHAL::isTransferring() const { return _transferring; }
uint8_t BleOtaHAL::getProgress() const { return _progress; }

void BleOtaHAL::onProgress(BleOtaProgressCb cb) { _progressCb = cb; }

void BleOtaHAL::_sendCtrlResponse(BleOtaResp resp, const uint8_t* data, size_t len) {
    if (!_ctrlChar) return;
    uint8_t buf[256];
    buf[0] = (uint8_t)resp;
    if (data && len > 0 && len < sizeof(buf) - 1) {
        memcpy(buf + 1, data, len);
    }
    _ctrlChar->setValue(buf, 1 + len);
    _ctrlChar->notify();
}

void BleOtaHAL::_handleCtrlWrite(const uint8_t* data, size_t len) {
    if (len < 1) return;
    BleOtaCmd cmd = (BleOtaCmd)data[0];

    switch (cmd) {
        case BleOtaCmd::BEGIN: {
            if (len < 9) {
                _abortTransfer("bad BEGIN (need 9 bytes: cmd + size + crc)");
                return;
            }
            memcpy(&_fw_size, data + 1, 4);
            memcpy(&_fw_crc, data + 5, 4);
            _received = 0;
            _progress = 0;
            _has_signature = false;

            DBG_INFO(TAG, "BLE OTA begin: %u bytes, crc=0x%08X", _fw_size, _fw_crc);

            if (!Update.begin(_fw_size)) {
                char err[64];
                snprintf(err, sizeof(err), "begin failed: %s", Update.errorString());
                DBG_ERROR(TAG, "%s", err);
                _sendCtrlResponse(BleOtaResp::FAIL, (const uint8_t*)err, strlen(err));
                return;
            }

            // Start streaming CRC verification
            OtaVerify::crc32Begin();

            _transferring = true;
            _sendCtrlResponse(BleOtaResp::READY);
            break;
        }

        case BleOtaCmd::SIG: {
            if (len < 65) {  // cmd(1) + r(32) + s(32) = 65
                _sendCtrlResponse(BleOtaResp::FAIL,
                    (const uint8_t*)"bad SIG length", 14);
                return;
            }
            memcpy(_sig_r, data + 1, 32);
            memcpy(_sig_s, data + 33, 32);
            _has_signature = true;

            // Start streaming signature verification (covers firmware data only)
            OtaVerify::beginVerify();

            DBG_INFO(TAG, "Received BLE OTA signature for verification");
            uint8_t ok = 1;
            _sendCtrlResponse(BleOtaResp::READY, &ok, 1);
            break;
        }

        case BleOtaCmd::ABORT: {
            _abortTransfer("client requested abort");
            break;
        }

        case BleOtaCmd::INFO_REQUEST: {
            const esp_app_desc_t* desc = esp_ota_get_app_description();
            char info[200];
            snprintf(info, sizeof(info),
                     "{\"version\":\"%s\",\"max_size\":%u,\"ble_ota\":true}",
                     desc ? desc->version : "unknown",
                     (unsigned)(esp_ota_get_next_update_partition(nullptr)->size));
            _sendCtrlResponse(BleOtaResp::INFO, (const uint8_t*)info, strlen(info));
            break;
        }
    }
}

void BleOtaHAL::_handleDataWrite(const uint8_t* data, size_t len) {
    if (!_transferring || len == 0) return;

    // Update CRC32 and signature hash
    OtaVerify::crc32Update(data, len);
    if (_has_signature) {
        OtaVerify::updateVerify(data, len);
    }

    size_t written = Update.write((uint8_t*)data, len);
    if (written != len) {
        char err[64];
        snprintf(err, sizeof(err), "write failed at %u: %s", _received, Update.errorString());
        _abortTransfer(err);
        return;
    }

    _received += written;
    uint8_t newPct = (_received * 100) / _fw_size;

    if (_received >= _fw_size) {
        // Defer everything to process() — notify() from within NimBLE callback
        // can overflow the nimble_host task stack when combined with Update.write
        _progress = 100;
        _finish_pending = true;
    } else if (newPct != _progress) {
        _progress = newPct;
        _sendCtrlResponse(BleOtaResp::PROGRESS, &_progress, 1);
        if (_progressCb) {
            _progressCb(_progress, _received, _fw_size);
        }
    }
}

void BleOtaHAL::_finishTransfer() {
    _transferring = false;

    // Send 100% progress from main loop context (safe stack)
    uint8_t pct = 100;
    _sendCtrlResponse(BleOtaResp::PROGRESS, &pct, 1);
    if (_progressCb) {
        _progressCb(100, _received, _fw_size);
    }

    // Verify CRC32
    uint32_t actualCrc = OtaVerify::crc32Finalize();
    if (_fw_crc != 0 && actualCrc != _fw_crc) {
        char err[64];
        snprintf(err, sizeof(err), "CRC32 mismatch: 0x%08X vs 0x%08X", _fw_crc, actualCrc);
        DBG_ERROR(TAG, "%s", err);
        _sendCtrlResponse(BleOtaResp::FAIL, (const uint8_t*)err, strlen(err));
        Update.abort();
        return;
    }
    DBG_INFO(TAG, "BLE OTA CRC32 verified: 0x%08X", actualCrc);

    // Verify signature (required when OTA_REQUIRE_SIGNATURE is set)
#ifdef OTA_REQUIRE_SIGNATURE
    if (!_has_signature) {
        const char* err = "unsigned firmware rejected (signature required)";
        DBG_ERROR(TAG, "%s", err);
        _sendCtrlResponse(BleOtaResp::FAIL, (const uint8_t*)err, strlen(err));
        Update.abort();
        return;
    }
#endif
    if (_has_signature) {
        bool sigOk = OtaVerify::finalizeVerify(_sig_r, _sig_s);
        if (!sigOk) {
            const char* err = "signature verification failed";
            DBG_ERROR(TAG, "%s", err);
            _sendCtrlResponse(BleOtaResp::FAIL, (const uint8_t*)err, strlen(err));
            Update.abort();
            return;
        }
        DBG_INFO(TAG, "BLE OTA ECDSA signature verified");
    }

    if (Update.end(true)) {
        _progress = 100;
        DBG_INFO(TAG, "BLE OTA complete, %u bytes", _received);
        _sendCtrlResponse(BleOtaResp::OK);
        delay(500);
        ESP.restart();
    } else {
        char err[64];
        snprintf(err, sizeof(err), "end failed: %s", Update.errorString());
        DBG_ERROR(TAG, "%s", err);
        _sendCtrlResponse(BleOtaResp::FAIL, (const uint8_t*)err, strlen(err));
    }
}

void BleOtaHAL::_abortTransfer(const char* reason) {
    if (_transferring) {
        Update.abort();
    }
    _transferring = false;
    _received = 0;
    _progress = 0;
    DBG_WARN(TAG, "BLE OTA aborted: %s", reason);
    _sendCtrlResponse(BleOtaResp::FAIL, (const uint8_t*)reason, strlen(reason));
}

#else // SIMULATOR

bool BleOtaHAL::init(const char*) { return false; }
void BleOtaHAL::process() {}
void BleOtaHAL::stop() {}
bool BleOtaHAL::isConnected() const { return false; }
bool BleOtaHAL::isTransferring() const { return false; }
uint8_t BleOtaHAL::getProgress() const { return 0; }
void BleOtaHAL::onProgress(BleOtaProgressCb) {}
void BleOtaHAL::_sendCtrlResponse(BleOtaResp, const uint8_t*, size_t) {}
void BleOtaHAL::_handleCtrlWrite(const uint8_t*, size_t) {}
void BleOtaHAL::_handleDataWrite(const uint8_t*, size_t) {}
void BleOtaHAL::_finishTransfer() {}
void BleOtaHAL::_abortTransfer(const char*) {}

#endif
