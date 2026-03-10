#include "hal_provision.h"
#include "debug_log.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

static constexpr const char* TAG = "prov";

// ============================================================================
// Minimal JSON helpers — no dependency on ArduinoJson
// ============================================================================

// Extract a string value for a given key from a JSON string.
// Returns true if found; copies into out (null-terminated, up to outSize-1 chars).
static bool jsonGetString(const char* json, const char* key, char* out, size_t outSize) {
    if (!json || !key || !out || outSize == 0) return false;
    out[0] = '\0';

    // Build search pattern: "key":"
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    // Skip optional whitespace and colon
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++; // skip opening quote
    size_t i = 0;
    while (*p && *p != '"' && i < outSize - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++; // skip escape backslash
            if (*p == 'n') { out[i++] = '\n'; }
            else if (*p == 't') { out[i++] = '\t'; }
            else if (*p == '\\') { out[i++] = '\\'; }
            else if (*p == '"') { out[i++] = '"'; }
            else { out[i++] = *p; }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

// Extract an integer value for a given key.
static bool jsonGetInt(const char* json, const char* key, int* out) {
    if (!json || !key || !out) return false;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    // Could be a number or a quoted number
    if (*p == '"') p++;
    *out = atoi(p);
    return true;
}

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <chrono>

static const char* SIM_FS_ROOT = "./littlefs";
static const char* SIM_SD_ROOT = "./sdcard";

static uint32_t sim_millis() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start).count();
}

static void sim_mkdirs(const char* path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            ::mkdir(tmp, 0755);
            *p = '/';
        }
    }
    ::mkdir(tmp, 0755);
}

static void sim_fs_path(const char* path, char* out, size_t outSize) {
    snprintf(out, outSize, "%s%s", SIM_FS_ROOT, path);
}

static void sim_sd_path(const char* path, char* out, size_t outSize) {
    snprintf(out, outSize, "%s%s", SIM_SD_ROOT, path);
}

static bool sim_file_exists(const char* hostPath) {
    struct stat st;
    return stat(hostPath, &st) == 0;
}

static bool sim_read_file(const char* hostPath, char* buf, size_t bufSize, size_t* outLen) {
    FILE* f = fopen(hostPath, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, bufSize, f);
    fclose(f);
    if (outLen) *outLen = n;
    return true;
}

static bool sim_write_file(const char* hostPath, const char* data, size_t len) {
    // Ensure parent dir exists
    char dir[512];
    strncpy(dir, hostPath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char* sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; sim_mkdirs(dir); }
    FILE* f = fopen(hostPath, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

static bool sim_remove_file(const char* hostPath) {
    return ::remove(hostPath) == 0;
}

// --- ProvisionHAL implementation (simulator) ---

bool ProvisionHAL::init() {
    char hp[512];
    sim_fs_path(PROV_DIR, hp, sizeof(hp));
    sim_mkdirs(hp);
    DBG_INFO(TAG, "Provision HAL init (simulator)");

    _loadIdentity();
    _checkProvisioned();
    DBG_INFO(TAG, "State: %s", _state == ProvisionState::PROVISIONED ? "PROVISIONED" : "UNPROVISIONED");
    return true;
}

ProvisionState ProvisionHAL::getState() const { return _state; }
bool ProvisionHAL::isProvisioned() const { return _state == ProvisionState::PROVISIONED; }
const DeviceIdentity& ProvisionHAL::getIdentity() const { return _identity; }

bool ProvisionHAL::_checkProvisioned() {
    char hp1[512], hp2[512], hp3[512];
    sim_fs_path(CA_CERT_PATH, hp1, sizeof(hp1));
    sim_fs_path(CLIENT_CERT_PATH, hp2, sizeof(hp2));
    sim_fs_path(CLIENT_KEY_PATH, hp3, sizeof(hp3));

    bool hasCerts = sim_file_exists(hp1) && sim_file_exists(hp2) && sim_file_exists(hp3);
    if (hasCerts && _identity.provisioned) {
        _state = ProvisionState::PROVISIONED;
    } else {
        _state = ProvisionState::UNPROVISIONED;
    }
    return _state == ProvisionState::PROVISIONED;
}

void ProvisionHAL::_ensureProvDir() {
    char hp[512];
    sim_fs_path(PROV_DIR, hp, sizeof(hp));
    sim_mkdirs(hp);
}

bool ProvisionHAL::_loadIdentity() {
    char hp[512];
    sim_fs_path(IDENTITY_PATH, hp, sizeof(hp));
    char buf[1024];
    size_t n = 0;
    if (!sim_read_file(hp, buf, sizeof(buf) - 1, &n)) return false;
    buf[n] = '\0';
    return _parseDeviceJson(buf);
}

bool ProvisionHAL::_saveIdentity() {
    _ensureProvDir();
    char json[1024];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"device_id\": \"%s\",\n"
        "  \"device_name\": \"%s\",\n"
        "  \"server_url\": \"%s\",\n"
        "  \"mqtt_broker\": \"%s\",\n"
        "  \"mqtt_port\": %u,\n"
        "  \"provisioned\": %s\n"
        "}",
        _identity.device_id, _identity.device_name,
        _identity.server_url, _identity.mqtt_broker,
        _identity.mqtt_port,
        _identity.provisioned ? "true" : "false");

    char hp[512];
    sim_fs_path(IDENTITY_PATH, hp, sizeof(hp));
    return sim_write_file(hp, json, strlen(json));
}

bool ProvisionHAL::_parseDeviceJson(const char* json) {
    jsonGetString(json, "device_id", _identity.device_id, sizeof(_identity.device_id));
    jsonGetString(json, "device_name", _identity.device_name, sizeof(_identity.device_name));
    jsonGetString(json, "server_url", _identity.server_url, sizeof(_identity.server_url));
    jsonGetString(json, "mqtt_broker", _identity.mqtt_broker, sizeof(_identity.mqtt_broker));

    int port = 8883;
    jsonGetInt(json, "mqtt_port", &port);
    _identity.mqtt_port = (uint16_t)port;

    // Check for "provisioned" field
    const char* prov = strstr(json, "\"provisioned\"");
    if (prov) {
        _identity.provisioned = (strstr(prov, "true") != nullptr);
    }
    return _identity.device_id[0] != '\0';
}

bool ProvisionHAL::_parseFactoryWifiJson(const char* json) {
    char ssid[64] = {};
    char pass[64] = {};
    jsonGetString(json, "ssid", ssid, sizeof(ssid));
    jsonGetString(json, "password", pass, sizeof(pass));
    if (ssid[0] == '\0') return false;

    // Save to LittleFS
    char hp[512];
    sim_fs_path(FACTORY_WIFI_PATH, hp, sizeof(hp));
    return sim_write_file(hp, json, strlen(json));
}

bool ProvisionHAL::_copyFileFromSD(const char* sdPath, const char* fsPath) {
    char sdHp[512];
    sim_sd_path(sdPath, sdHp, sizeof(sdHp));

    // Read from SD
    FILE* sf = fopen(sdHp, "rb");
    if (!sf) {
        DBG_ERROR(TAG, "Cannot open SD file: %s", sdPath);
        return false;
    }
    fseek(sf, 0, SEEK_END);
    long sz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) {
        DBG_ERROR(TAG, "Invalid file size %ld for %s", sz, sdPath);
        fclose(sf);
        return false;
    }
    char* buf = (char*)malloc(sz);
    if (!buf) { fclose(sf); return false; }
    size_t rd = fread(buf, 1, sz, sf);
    fclose(sf);

    // Write to LittleFS
    char fsHp[512];
    sim_fs_path(fsPath, fsHp, sizeof(fsHp));
    bool ok = sim_write_file(fsHp, buf, rd);
    free(buf);

    if (ok) {
        DBG_INFO(TAG, "Copied %s -> %s (%zu bytes)", sdPath, fsPath, rd);
    } else {
        DBG_ERROR(TAG, "Failed to write %s", fsPath);
    }
    return ok;
}

bool ProvisionHAL::provisionFromSD(const char* sdPath) {
    DBG_INFO(TAG, "Provisioning from SD: %s", sdPath);

    char path[256];

    // Copy certificates
    snprintf(path, sizeof(path), "%s/ca.pem", sdPath);
    if (!_copyFileFromSD(path, CA_CERT_PATH)) return false;

    snprintf(path, sizeof(path), "%s/client.crt", sdPath);
    if (!_copyFileFromSD(path, CLIENT_CERT_PATH)) return false;

    snprintf(path, sizeof(path), "%s/client.key", sdPath);
    if (!_copyFileFromSD(path, CLIENT_KEY_PATH)) return false;

    // Parse device.json
    snprintf(path, sizeof(path), "%s/device.json", sdPath);
    char sdHp[512];
    sim_sd_path(path, sdHp, sizeof(sdHp));
    char jsonBuf[1024];
    size_t jsonLen = 0;
    if (sim_read_file(sdHp, jsonBuf, sizeof(jsonBuf) - 1, &jsonLen)) {
        jsonBuf[jsonLen] = '\0';
        _parseDeviceJson(jsonBuf);
        _identity.provisioned = true;
        _saveIdentity();
        DBG_INFO(TAG, "Device identity loaded: id=%s name=%s", _identity.device_id, _identity.device_name);
    } else {
        DBG_ERROR(TAG, "Cannot read device.json from SD");
        return false;
    }

    // Parse factory_wifi.json (optional)
    snprintf(path, sizeof(path), "%s/factory_wifi.json", sdPath);
    sim_sd_path(path, sdHp, sizeof(sdHp));
    if (sim_read_file(sdHp, jsonBuf, sizeof(jsonBuf) - 1, &jsonLen)) {
        jsonBuf[jsonLen] = '\0';
        _parseFactoryWifiJson(jsonBuf);
        DBG_INFO(TAG, "Factory WiFi config loaded");
    } else {
        DBG_INFO(TAG, "No factory_wifi.json on SD (optional)");
    }

    _checkProvisioned();
    DBG_INFO(TAG, "Provisioning complete, state: %s",
             _state == ProvisionState::PROVISIONED ? "PROVISIONED" : "FAILED");
    return _state == ProvisionState::PROVISIONED;
}

bool ProvisionHAL::startUSBProvision() {
    _usbActive = true;
    _usbBufLen = 0;
    DBG_INFO(TAG, "USB provisioning started (simulator stub)");
    return true;
}

bool ProvisionHAL::processUSBProvision() {
    // Simulator: no serial input
    return false;
}

bool ProvisionHAL::isUSBProvisionActive() const { return _usbActive; }

bool ProvisionHAL::_processUSBCommand(const char* json, size_t len) {
    (void)len;
    char cmd[32] = {};
    jsonGetString(json, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "provision") != 0) {
        DBG_ERROR(TAG, "Unknown USB command: %s", cmd);
        return false;
    }

    // Extract certs and identity
    char pem[4096];
    if (jsonGetString(json, "ca_pem", pem, sizeof(pem))) {
        importCACert(pem, strlen(pem));
    }
    if (jsonGetString(json, "client_crt", pem, sizeof(pem))) {
        importClientCert(pem, strlen(pem));
    }
    if (jsonGetString(json, "client_key", pem, sizeof(pem))) {
        importClientKey(pem, strlen(pem));
    }

    // Identity fields
    jsonGetString(json, "device_id", _identity.device_id, sizeof(_identity.device_id));
    jsonGetString(json, "device_name", _identity.device_name, sizeof(_identity.device_name));
    jsonGetString(json, "server_url", _identity.server_url, sizeof(_identity.server_url));
    jsonGetString(json, "mqtt_broker", _identity.mqtt_broker, sizeof(_identity.mqtt_broker));
    int port = 8883;
    jsonGetInt(json, "mqtt_port", &port);
    _identity.mqtt_port = (uint16_t)port;
    _identity.provisioned = true;
    bool saved = _saveIdentity();

    _checkProvisioned();
    _usbActive = false;
    DBG_INFO(TAG, "USB provisioning complete");
    // Consider success if identity was saved, even without certs (cert-less mode)
    return saved && _identity.device_id[0] != '\0';
}

bool ProvisionHAL::getCACert(char* buf, size_t bufSize, size_t* outLen) {
    char hp[512];
    sim_fs_path(CA_CERT_PATH, hp, sizeof(hp));
    size_t n = 0;
    bool ok = sim_read_file(hp, buf, bufSize - 1, &n);
    if (ok) buf[n] = '\0';
    if (outLen) *outLen = n;
    return ok;
}

bool ProvisionHAL::getClientCert(char* buf, size_t bufSize, size_t* outLen) {
    char hp[512];
    sim_fs_path(CLIENT_CERT_PATH, hp, sizeof(hp));
    size_t n = 0;
    bool ok = sim_read_file(hp, buf, bufSize - 1, &n);
    if (ok) buf[n] = '\0';
    if (outLen) *outLen = n;
    return ok;
}

bool ProvisionHAL::getClientKey(char* buf, size_t bufSize, size_t* outLen) {
    char hp[512];
    sim_fs_path(CLIENT_KEY_PATH, hp, sizeof(hp));
    size_t n = 0;
    bool ok = sim_read_file(hp, buf, bufSize - 1, &n);
    if (ok) buf[n] = '\0';
    if (outLen) *outLen = n;
    return ok;
}

bool ProvisionHAL::hasFactoryWiFi() const {
    char hp[512];
    sim_fs_path(FACTORY_WIFI_PATH, hp, sizeof(hp));
    return sim_file_exists(hp);
}

bool ProvisionHAL::getFactoryWiFi(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    char hp[512];
    sim_fs_path(FACTORY_WIFI_PATH, hp, sizeof(hp));
    char buf[512];
    size_t n = 0;
    if (!sim_read_file(hp, buf, sizeof(buf) - 1, &n)) return false;
    buf[n] = '\0';
    bool ok = jsonGetString(buf, "ssid", ssid, ssidLen);
    jsonGetString(buf, "password", pass, passLen);
    return ok;
}

bool ProvisionHAL::clearFactoryWiFi() {
    char hp[512];
    sim_fs_path(FACTORY_WIFI_PATH, hp, sizeof(hp));
    bool ok = sim_remove_file(hp);
    if (ok) DBG_INFO(TAG, "Factory WiFi cleared");
    return ok;
}

bool ProvisionHAL::importCACert(const char* pem, size_t len) {
    char hp[512];
    sim_fs_path(CA_CERT_PATH, hp, sizeof(hp));
    bool ok = sim_write_file(hp, pem, len);
    if (ok) DBG_INFO(TAG, "CA cert imported (%zu bytes)", len);
    return ok;
}

bool ProvisionHAL::importClientCert(const char* pem, size_t len) {
    char hp[512];
    sim_fs_path(CLIENT_CERT_PATH, hp, sizeof(hp));
    bool ok = sim_write_file(hp, pem, len);
    if (ok) DBG_INFO(TAG, "Client cert imported (%zu bytes)", len);
    return ok;
}

bool ProvisionHAL::importClientKey(const char* pem, size_t len) {
    char hp[512];
    sim_fs_path(CLIENT_KEY_PATH, hp, sizeof(hp));
    bool ok = sim_write_file(hp, pem, len);
    if (ok) DBG_INFO(TAG, "Client key imported (%zu bytes)", len);
    return ok;
}

bool ProvisionHAL::setDeviceIdentity(const DeviceIdentity& id) {
    memcpy(&_identity, &id, sizeof(DeviceIdentity));
    bool ok = _saveIdentity();
    if (ok) {
        _checkProvisioned();
        DBG_INFO(TAG, "Device identity set: id=%s", _identity.device_id);
    }
    return ok;
}

bool ProvisionHAL::factoryReset() {
    DBG_WARN(TAG, "Factory reset — erasing all provisioning data");
    char hp[512];

    sim_fs_path(CA_CERT_PATH, hp, sizeof(hp));       sim_remove_file(hp);
    sim_fs_path(CLIENT_CERT_PATH, hp, sizeof(hp));    sim_remove_file(hp);
    sim_fs_path(CLIENT_KEY_PATH, hp, sizeof(hp));     sim_remove_file(hp);
    sim_fs_path(IDENTITY_PATH, hp, sizeof(hp));       sim_remove_file(hp);
    sim_fs_path(FACTORY_WIFI_PATH, hp, sizeof(hp));   sim_remove_file(hp);

    memset(&_identity, 0, sizeof(_identity));
    _state = ProvisionState::UNPROVISIONED;
    DBG_INFO(TAG, "Factory reset complete");
    return true;
}

ProvisionHAL::TestResult ProvisionHAL::runTest() {
    TestResult r = {};
    uint32_t t0 = sim_millis();

    DBG_INFO(TAG, "--- Provision HAL Test Begin (simulator) ---");

    // init test
    r.init_ok = init();

    // fs access test
    char hp[512];
    sim_fs_path(PROV_DIR, hp, sizeof(hp));
    r.fs_ok = sim_file_exists(hp);

    // cert write test
    const char* testCert = "-----BEGIN CERTIFICATE-----\nTEST_CERT_DATA_1234567890\n-----END CERTIFICATE-----\n";
    size_t testLen = strlen(testCert);
    r.cert_write_ok = importCACert(testCert, testLen);

    // cert read test
    char readBuf[512] = {};
    size_t readLen = 0;
    r.cert_read_ok = getCACert(readBuf, sizeof(readBuf), &readLen);

    // verify test
    r.cert_verify_ok = (readLen == testLen) && (memcmp(readBuf, testCert, testLen) == 0);

    // identity test
    DeviceIdentity testId = {};
    strncpy(testId.device_id, "test-device-001", sizeof(testId.device_id) - 1);
    strncpy(testId.device_name, "Test Device", sizeof(testId.device_name) - 1);
    strncpy(testId.server_url, "https://test.example.com", sizeof(testId.server_url) - 1);
    strncpy(testId.mqtt_broker, "mqtt.example.com", sizeof(testId.mqtt_broker) - 1);
    testId.mqtt_port = 8883;
    testId.provisioned = true;

    bool setOk = setDeviceIdentity(testId);
    // Reload identity
    bool loadOk = _loadIdentity();
    bool idMatch = (strcmp(_identity.device_id, "test-device-001") == 0) &&
                   (strcmp(_identity.device_name, "Test Device") == 0) &&
                   (_identity.mqtt_port == 8883);
    r.identity_ok = setOk && loadOk && idMatch;

    // factory wifi test
    const char* wifiJson = "{\"ssid\":\"TestNetwork\",\"password\":\"TestPass123\"}";
    sim_fs_path(FACTORY_WIFI_PATH, hp, sizeof(hp));
    bool wifiSave = sim_write_file(hp, wifiJson, strlen(wifiJson));
    bool wifiHas = hasFactoryWiFi();
    char ssid[64] = {}, pass[64] = {};
    bool wifiGet = getFactoryWiFi(ssid, sizeof(ssid), pass, sizeof(pass));
    bool wifiMatch = (strcmp(ssid, "TestNetwork") == 0) && (strcmp(pass, "TestPass123") == 0);
    bool wifiClear = clearFactoryWiFi();
    bool wifiGone = !hasFactoryWiFi();
    r.factory_wifi_ok = wifiSave && wifiHas && wifiGet && wifiMatch && wifiClear && wifiGone;

    // factory reset test
    r.factory_reset_ok = factoryReset();
    r.factory_reset_ok = r.factory_reset_ok && (_state == ProvisionState::UNPROVISIONED);

    r.state = _state;
    r.test_duration_ms = sim_millis() - t0;

    bool allPassed = r.init_ok && r.fs_ok && r.cert_write_ok && r.cert_read_ok &&
                     r.cert_verify_ok && r.identity_ok && r.factory_wifi_ok && r.factory_reset_ok;

    DBG_INFO(TAG, "--- Provision HAL Test Complete ---");
    DBG_INFO(TAG, "init:%s fs:%s cert_w:%s cert_r:%s cert_v:%s id:%s wifi:%s reset:%s",
             r.init_ok ? "OK" : "FAIL", r.fs_ok ? "OK" : "FAIL",
             r.cert_write_ok ? "OK" : "FAIL", r.cert_read_ok ? "OK" : "FAIL",
             r.cert_verify_ok ? "OK" : "FAIL", r.identity_ok ? "OK" : "FAIL",
             r.factory_wifi_ok ? "OK" : "FAIL", r.factory_reset_ok ? "OK" : "FAIL");
    DBG_INFO(TAG, "All passed: %s, Duration: %u ms", allPassed ? "YES" : "NO", r.test_duration_ms);

    return r;
}

// --- BLE provisioning stubs (simulator — no real BLE) ---
bool ProvisionHAL::startBLEProvision() {
    DBG_INFO(TAG, "BLE provisioning not available in simulator");
    return false;
}

void ProvisionHAL::stopBLEProvision() {}

bool ProvisionHAL::isBLEProvisionActive() const { return false; }

bool ProvisionHAL::processBLEProvision() { return false; }

// --- Web provisioning stubs (simulator) ---
bool ProvisionHAL::startWebProvision() {
    DBG_INFO(TAG, "Web provisioning not available in simulator");
    return false;
}

void ProvisionHAL::stopWebProvision() {}

bool ProvisionHAL::isWebProvisionActive() const { return false; }

// ============================================================================
// ESP32 — real LittleFS + SD_MMC
// ============================================================================
#else

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <FS.h>
#include <SD_MMC.h>

#ifndef HAS_SDCARD
#define HAS_SDCARD 0
#endif

bool ProvisionHAL::init() {
    if (!LittleFS.begin(true)) {
        DBG_ERROR(TAG, "LittleFS mount failed");
        _state = ProvisionState::ERROR;
        return false;
    }

    // Create provisioning directory
    if (!LittleFS.exists(PROV_DIR)) {
        if (!LittleFS.mkdir(PROV_DIR)) {
            DBG_ERROR(TAG, "Failed to create %s directory", PROV_DIR);
        }
    }
    if (!LittleFS.exists(PROV_DIR)) {
        DBG_ERROR(TAG, "%s directory still missing after mkdir", PROV_DIR);
    }

    DBG_INFO(TAG, "Provision HAL init — LittleFS total: %zu  used: %zu",
             (size_t)LittleFS.totalBytes(), (size_t)LittleFS.usedBytes());

    _loadIdentity();
    _checkProvisioned();
    DBG_INFO(TAG, "State: %s", _state == ProvisionState::PROVISIONED ? "PROVISIONED" : "UNPROVISIONED");
    return true;
}

ProvisionState ProvisionHAL::getState() const { return _state; }
bool ProvisionHAL::isProvisioned() const { return _state == ProvisionState::PROVISIONED; }
const DeviceIdentity& ProvisionHAL::getIdentity() const { return _identity; }

bool ProvisionHAL::_checkProvisioned() {
    bool hasCerts = LittleFS.exists(CA_CERT_PATH) &&
                    LittleFS.exists(CLIENT_CERT_PATH) &&
                    LittleFS.exists(CLIENT_KEY_PATH);
    if (hasCerts && _identity.provisioned) {
        _state = ProvisionState::PROVISIONED;
    } else {
        _state = ProvisionState::UNPROVISIONED;
    }
    return _state == ProvisionState::PROVISIONED;
}

bool ProvisionHAL::_loadIdentity() {
    File f = LittleFS.open(IDENTITY_PATH, "r");
    if (!f) return false;
    size_t sz = f.size();
    if (sz == 0 || sz > 1024) { f.close(); return false; }
    char buf[1024];
    size_t n = f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    buf[n] = '\0';
    return _parseDeviceJson(buf);
}

bool ProvisionHAL::_saveIdentity() {
    _ensureProvDir();
    char json[1024];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"device_id\": \"%s\",\n"
        "  \"device_name\": \"%s\",\n"
        "  \"server_url\": \"%s\",\n"
        "  \"mqtt_broker\": \"%s\",\n"
        "  \"mqtt_port\": %u,\n"
        "  \"provisioned\": %s\n"
        "}",
        _identity.device_id, _identity.device_name,
        _identity.server_url, _identity.mqtt_broker,
        _identity.mqtt_port,
        _identity.provisioned ? "true" : "false");

    File f = LittleFS.open(IDENTITY_PATH, "w");
    if (!f) {
        DBG_ERROR(TAG, "Cannot write identity file");
        return false;
    }
    size_t w = f.write((const uint8_t*)json, strlen(json));
    f.close();
    return w == strlen(json);
}

bool ProvisionHAL::_parseDeviceJson(const char* json) {
    jsonGetString(json, "device_id", _identity.device_id, sizeof(_identity.device_id));
    jsonGetString(json, "device_name", _identity.device_name, sizeof(_identity.device_name));
    jsonGetString(json, "server_url", _identity.server_url, sizeof(_identity.server_url));
    jsonGetString(json, "mqtt_broker", _identity.mqtt_broker, sizeof(_identity.mqtt_broker));

    int port = 8883;
    jsonGetInt(json, "mqtt_port", &port);
    _identity.mqtt_port = (uint16_t)port;

    const char* prov = strstr(json, "\"provisioned\"");
    if (prov) {
        _identity.provisioned = (strstr(prov, "true") != nullptr);
    }
    return _identity.device_id[0] != '\0';
}

bool ProvisionHAL::_parseFactoryWifiJson(const char* json) {
    char ssid[64] = {};
    jsonGetString(json, "ssid", ssid, sizeof(ssid));
    if (ssid[0] == '\0') return false;

    // Save raw JSON to LittleFS
    File f = LittleFS.open(FACTORY_WIFI_PATH, "w");
    if (!f) return false;
    f.write((const uint8_t*)json, strlen(json));
    f.close();
    return true;
}

bool ProvisionHAL::_copyFileFromSD(const char* sdPath, const char* fsPath) {
#if HAS_SDCARD
    File sf = SD_MMC.open(sdPath, FILE_READ);
    if (!sf) {
        DBG_ERROR(TAG, "Cannot open SD file: %s", sdPath);
        return false;
    }
    size_t sz = sf.size();
    if (sz == 0 || sz > 16384) {
        DBG_ERROR(TAG, "Invalid file size %zu for %s", sz, sdPath);
        sf.close();
        return false;
    }

    char* buf = (char*)malloc(sz);
    if (!buf) {
        DBG_ERROR(TAG, "malloc failed for %zu bytes", sz);
        sf.close();
        return false;
    }
    size_t rd = sf.readBytes(buf, sz);
    sf.close();

    File df = LittleFS.open(fsPath, "w");
    if (!df) {
        DBG_ERROR(TAG, "Cannot write to %s", fsPath);
        free(buf);
        return false;
    }
    size_t w = df.write((const uint8_t*)buf, rd);
    df.close();
    free(buf);

    if (w != rd) {
        DBG_ERROR(TAG, "Short write %zu/%zu for %s", w, rd, fsPath);
        return false;
    }
    DBG_INFO(TAG, "Copied %s -> %s (%zu bytes)", sdPath, fsPath, rd);
    return true;
#else
    DBG_ERROR(TAG, "SD card not available");
    return false;
#endif
}

bool ProvisionHAL::provisionFromSD(const char* sdPath) {
    DBG_INFO(TAG, "Provisioning from SD: %s", sdPath);

    char path[256];

    // Copy certificates
    snprintf(path, sizeof(path), "%s/ca.pem", sdPath);
    if (!_copyFileFromSD(path, CA_CERT_PATH)) return false;

    snprintf(path, sizeof(path), "%s/client.crt", sdPath);
    if (!_copyFileFromSD(path, CLIENT_CERT_PATH)) return false;

    snprintf(path, sizeof(path), "%s/client.key", sdPath);
    if (!_copyFileFromSD(path, CLIENT_KEY_PATH)) return false;

    // Parse device.json
    snprintf(path, sizeof(path), "%s/device.json", sdPath);
#if HAS_SDCARD
    File jf = SD_MMC.open(path, FILE_READ);
    if (!jf) {
        DBG_ERROR(TAG, "Cannot read %s from SD", path);
        return false;
    }
    char jsonBuf[1024];
    size_t jsonLen = jf.readBytes(jsonBuf, sizeof(jsonBuf) - 1);
    jf.close();
    jsonBuf[jsonLen] = '\0';
    _parseDeviceJson(jsonBuf);
    _identity.provisioned = true;
    _saveIdentity();
    DBG_INFO(TAG, "Device identity loaded: id=%s name=%s", _identity.device_id, _identity.device_name);

    // Parse factory_wifi.json (optional)
    snprintf(path, sizeof(path), "%s/factory_wifi.json", sdPath);
    File wf = SD_MMC.open(path, FILE_READ);
    if (wf) {
        jsonLen = wf.readBytes(jsonBuf, sizeof(jsonBuf) - 1);
        wf.close();
        jsonBuf[jsonLen] = '\0';
        _parseFactoryWifiJson(jsonBuf);
        DBG_INFO(TAG, "Factory WiFi config loaded");
    } else {
        DBG_INFO(TAG, "No factory_wifi.json on SD (optional)");
    }
#else
    DBG_ERROR(TAG, "SD card not available");
    return false;
#endif

    _checkProvisioned();
    DBG_INFO(TAG, "Provisioning complete, state: %s",
             _state == ProvisionState::PROVISIONED ? "PROVISIONED" : "FAILED");
    return _state == ProvisionState::PROVISIONED;
}

bool ProvisionHAL::startUSBProvision() {
    _usbActive = true;
    _usbBufLen = 0;
    if (!_usbBuf) _usbBuf = (char*)heap_caps_calloc(1, PROV_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!_usbBuf) _usbBuf = (char*)calloc(1, PROV_BUF_SIZE);
    if (!_usbBuf) return false;
    Serial.println("{\"status\":\"ready\",\"msg\":\"Send provisioning JSON\"}");
    DBG_INFO(TAG, "USB provisioning started — waiting for data on Serial");
    return true;
}

bool ProvisionHAL::processUSBProvision() {
    if (!_usbActive) return false;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (_usbBufLen > 0) {
                _usbBuf[_usbBufLen] = '\0';
                bool ok = _processUSBCommand(_usbBuf, _usbBufLen);
                _usbBufLen = 0;
                if (ok) {
                    Serial.println("{\"status\":\"ok\",\"msg\":\"Provisioned\"}");
                } else {
                    Serial.println("{\"status\":\"error\",\"msg\":\"Provisioning failed\"}");
                }
                return ok;
            }
        } else if (_usbBufLen < PROV_BUF_SIZE - 1) {
            _usbBuf[_usbBufLen++] = c;
        } else {
            // Buffer overflow — reset
            DBG_ERROR(TAG, "USB buffer overflow, resetting");
            _usbBufLen = 0;
        }
    }
    return false;
}

bool ProvisionHAL::isUSBProvisionActive() const { return _usbActive; }

bool ProvisionHAL::_processUSBCommand(const char* json, size_t len) {
    (void)len;
    char cmd[32] = {};
    jsonGetString(json, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "provision") != 0) {
        DBG_ERROR(TAG, "Unknown USB command: %s", cmd);
        return false;
    }

    // Extract certs and identity
    char pem[4096];
    if (jsonGetString(json, "ca_pem", pem, sizeof(pem))) {
        importCACert(pem, strlen(pem));
    }
    if (jsonGetString(json, "client_crt", pem, sizeof(pem))) {
        importClientCert(pem, strlen(pem));
    }
    if (jsonGetString(json, "client_key", pem, sizeof(pem))) {
        importClientKey(pem, strlen(pem));
    }

    // Identity fields
    jsonGetString(json, "device_id", _identity.device_id, sizeof(_identity.device_id));
    jsonGetString(json, "device_name", _identity.device_name, sizeof(_identity.device_name));
    jsonGetString(json, "server_url", _identity.server_url, sizeof(_identity.server_url));
    jsonGetString(json, "mqtt_broker", _identity.mqtt_broker, sizeof(_identity.mqtt_broker));
    int port = 8883;
    jsonGetInt(json, "mqtt_port", &port);
    _identity.mqtt_port = (uint16_t)port;
    _identity.provisioned = true;
    bool saved = _saveIdentity();

    _checkProvisioned();
    _usbActive = false;
    DBG_INFO(TAG, "USB provisioning complete");
    // Consider success if identity was saved, even without certs (cert-less mode)
    return saved && _identity.device_id[0] != '\0';
}

bool ProvisionHAL::getCACert(char* buf, size_t bufSize, size_t* outLen) {
    File f = LittleFS.open(CA_CERT_PATH, "r");
    if (!f) return false;
    size_t n = f.readBytes(buf, bufSize - 1);
    f.close();
    buf[n] = '\0';
    if (outLen) *outLen = n;
    return n > 0;
}

bool ProvisionHAL::getClientCert(char* buf, size_t bufSize, size_t* outLen) {
    File f = LittleFS.open(CLIENT_CERT_PATH, "r");
    if (!f) return false;
    size_t n = f.readBytes(buf, bufSize - 1);
    f.close();
    buf[n] = '\0';
    if (outLen) *outLen = n;
    return n > 0;
}

bool ProvisionHAL::getClientKey(char* buf, size_t bufSize, size_t* outLen) {
    File f = LittleFS.open(CLIENT_KEY_PATH, "r");
    if (!f) return false;
    size_t n = f.readBytes(buf, bufSize - 1);
    f.close();
    buf[n] = '\0';
    if (outLen) *outLen = n;
    return n > 0;
}

bool ProvisionHAL::hasFactoryWiFi() const {
    return LittleFS.exists(FACTORY_WIFI_PATH);
}

bool ProvisionHAL::getFactoryWiFi(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    File f = LittleFS.open(FACTORY_WIFI_PATH, "r");
    if (!f) return false;
    char buf[512];
    size_t n = f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    buf[n] = '\0';
    bool ok = jsonGetString(buf, "ssid", ssid, ssidLen);
    jsonGetString(buf, "password", pass, passLen);
    return ok;
}

bool ProvisionHAL::clearFactoryWiFi() {
    bool ok = LittleFS.remove(FACTORY_WIFI_PATH);
    if (ok) DBG_INFO(TAG, "Factory WiFi cleared");
    return ok;
}

void ProvisionHAL::_ensureProvDir() {
    if (!LittleFS.exists(PROV_DIR)) {
        LittleFS.mkdir(PROV_DIR);
    }
}

bool ProvisionHAL::importCACert(const char* pem, size_t len) {
    _ensureProvDir();
    File f = LittleFS.open(CA_CERT_PATH, "w");
    if (!f) return false;
    size_t w = f.write((const uint8_t*)pem, len);
    f.close();
    if (w == len) DBG_INFO(TAG, "CA cert imported (%zu bytes)", len);
    return w == len;
}

bool ProvisionHAL::importClientCert(const char* pem, size_t len) {
    _ensureProvDir();
    File f = LittleFS.open(CLIENT_CERT_PATH, "w");
    if (!f) return false;
    size_t w = f.write((const uint8_t*)pem, len);
    f.close();
    if (w == len) DBG_INFO(TAG, "Client cert imported (%zu bytes)", len);
    return w == len;
}

bool ProvisionHAL::importClientKey(const char* pem, size_t len) {
    _ensureProvDir();
    File f = LittleFS.open(CLIENT_KEY_PATH, "w");
    if (!f) return false;
    size_t w = f.write((const uint8_t*)pem, len);
    f.close();
    if (w == len) DBG_INFO(TAG, "Client key imported (%zu bytes)", len);
    return w == len;
}

bool ProvisionHAL::setDeviceIdentity(const DeviceIdentity& id) {
    memcpy(&_identity, &id, sizeof(DeviceIdentity));
    bool ok = _saveIdentity();
    if (ok) {
        _checkProvisioned();
        DBG_INFO(TAG, "Device identity set: id=%s", _identity.device_id);
    }
    return ok;
}

bool ProvisionHAL::factoryReset() {
    DBG_WARN(TAG, "Factory reset — erasing all provisioning data");

    LittleFS.remove(CA_CERT_PATH);
    LittleFS.remove(CLIENT_CERT_PATH);
    LittleFS.remove(CLIENT_KEY_PATH);
    LittleFS.remove(IDENTITY_PATH);
    LittleFS.remove(FACTORY_WIFI_PATH);

    memset(&_identity, 0, sizeof(_identity));
    _state = ProvisionState::UNPROVISIONED;
    DBG_INFO(TAG, "Factory reset complete");
    return true;
}

ProvisionHAL::TestResult ProvisionHAL::runTest() {
    TestResult r = {};
    uint32_t t0 = millis();

    DBG_INFO(TAG, "--- Provision HAL Test Begin ---");

    // init test
    r.init_ok = init();

    // fs access test
    r.fs_ok = LittleFS.exists(PROV_DIR);

    // cert write test
    const char* testCert = "-----BEGIN CERTIFICATE-----\nTEST_CERT_DATA_1234567890\n-----END CERTIFICATE-----\n";
    size_t testLen = strlen(testCert);
    r.cert_write_ok = importCACert(testCert, testLen);

    // cert read test
    char readBuf[512] = {};
    size_t readLen = 0;
    r.cert_read_ok = getCACert(readBuf, sizeof(readBuf), &readLen);

    // verify test
    r.cert_verify_ok = (readLen == testLen) && (memcmp(readBuf, testCert, testLen) == 0);

    // identity test
    DeviceIdentity testId = {};
    strncpy(testId.device_id, "test-device-001", sizeof(testId.device_id) - 1);
    strncpy(testId.device_name, "Test Device", sizeof(testId.device_name) - 1);
    strncpy(testId.server_url, "https://test.example.com", sizeof(testId.server_url) - 1);
    strncpy(testId.mqtt_broker, "mqtt.example.com", sizeof(testId.mqtt_broker) - 1);
    testId.mqtt_port = 8883;
    testId.provisioned = true;

    bool setOk = setDeviceIdentity(testId);
    bool loadOk = _loadIdentity();
    bool idMatch = (strcmp(_identity.device_id, "test-device-001") == 0) &&
                   (strcmp(_identity.device_name, "Test Device") == 0) &&
                   (_identity.mqtt_port == 8883);
    r.identity_ok = setOk && loadOk && idMatch;

    // factory wifi test
    const char* wifiJson = "{\"ssid\":\"TestNetwork\",\"password\":\"TestPass123\"}";
    _parseFactoryWifiJson(wifiJson);
    bool wifiHas = hasFactoryWiFi();
    char ssid[64] = {}, pass[64] = {};
    bool wifiGet = getFactoryWiFi(ssid, sizeof(ssid), pass, sizeof(pass));
    bool wifiMatch = (strcmp(ssid, "TestNetwork") == 0) && (strcmp(pass, "TestPass123") == 0);
    bool wifiClear = clearFactoryWiFi();
    bool wifiGone = !hasFactoryWiFi();
    r.factory_wifi_ok = wifiHas && wifiGet && wifiMatch && wifiClear && wifiGone;

    // factory reset test
    r.factory_reset_ok = factoryReset();
    r.factory_reset_ok = r.factory_reset_ok && (_state == ProvisionState::UNPROVISIONED);

    r.state = _state;
    r.test_duration_ms = millis() - t0;

    bool allPassed = r.init_ok && r.fs_ok && r.cert_write_ok && r.cert_read_ok &&
                     r.cert_verify_ok && r.identity_ok && r.factory_wifi_ok && r.factory_reset_ok;

    DBG_INFO(TAG, "--- Provision HAL Test Complete ---");
    DBG_INFO(TAG, "init:%s fs:%s cert_w:%s cert_r:%s cert_v:%s id:%s wifi:%s reset:%s",
             r.init_ok ? "OK" : "FAIL", r.fs_ok ? "OK" : "FAIL",
             r.cert_write_ok ? "OK" : "FAIL", r.cert_read_ok ? "OK" : "FAIL",
             r.cert_verify_ok ? "OK" : "FAIL", r.identity_ok ? "OK" : "FAIL",
             r.factory_wifi_ok ? "OK" : "FAIL", r.factory_reset_ok ? "OK" : "FAIL");
    DBG_INFO(TAG, "All passed: %s, Duration: %u ms", allPassed ? "YES" : "NO", r.test_duration_ms);

    return r;
}

// ============================================================================
// BLE Provisioning — NimBLE GATT service
// Created by Matthew Valancy / Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0
// ============================================================================
#if defined(ENABLE_BLE)

#include <esp_mac.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Preferences.h>

// Provisioning service UUIDs (from COMMISSIONING.md spec)
static const char* PROV_SERVICE_UUID = "00001234-0000-1000-8000-00805f9b34fb";
static const char* PROV_CHAR_UUID    = "00001235-0000-1000-8000-00805f9b34fb";

// Forward declarations for NimBLE callbacks
static BLEServer* s_provServer = nullptr;
static BLECharacteristic* s_provChar = nullptr;
static bool s_provConnected = false;
static bool s_provComplete = false;
static ProvisionHAL* s_provInstance = nullptr;

// Tracks whether a complete JSON payload has been received
static char s_provJsonBuf[4096] = {};
static size_t s_provJsonLen = 0;

// Braces depth tracker for detecting complete JSON
static int s_braceDepth = 0;
static bool s_inString = false;
static bool s_escape = false;

static void _resetJsonParser() {
    s_provJsonLen = 0;
    s_braceDepth = 0;
    s_inString = false;
    s_escape = false;
    memset(s_provJsonBuf, 0, sizeof(s_provJsonBuf));
}

// Track brace nesting to detect complete JSON objects
static bool _appendAndCheckComplete(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s_provJsonLen >= sizeof(s_provJsonBuf) - 1) {
            Serial.printf("[provision] BLE JSON buffer overflow, resetting\n");
            _resetJsonParser();
            return false;
        }
        char c = data[i];
        s_provJsonBuf[s_provJsonLen++] = c;

        if (s_escape) {
            s_escape = false;
            continue;
        }
        if (c == '\\' && s_inString) {
            s_escape = true;
            continue;
        }
        if (c == '"') {
            s_inString = !s_inString;
            continue;
        }
        if (!s_inString) {
            if (c == '{') s_braceDepth++;
            else if (c == '}') {
                s_braceDepth--;
                if (s_braceDepth == 0 && s_provJsonLen > 1) {
                    s_provJsonBuf[s_provJsonLen] = '\0';
                    return true;  // Complete JSON object received
                }
            }
        }
    }
    return false;
}

class ProvBLEServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* server) override {
        s_provConnected = true;
        Serial.printf("[provision] BLE client connected\n");
    }
    void onDisconnect(BLEServer* server) override {
        s_provConnected = false;
        Serial.printf("[provision] BLE client disconnected\n");
        if (!s_provComplete) {
            // Re-advertise if provisioning didn't complete
            server->startAdvertising();
        }
    }
};

class ProvBLECharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        std::string val = characteristic->getValue();
        if (val.empty()) return;

        Serial.printf("[provision] BLE chunk received: %zu bytes\n", val.length());

        bool complete = _appendAndCheckComplete(val.c_str(), val.length());
        if (complete) {
            Serial.printf("[provision] Complete JSON received (%zu bytes)\n", s_provJsonLen);
            s_provComplete = true;
        }
    }
};

bool ProvisionHAL::startBLEProvision() {
    if (_bleActive) {
        DBG_WARN(TAG, "BLE provisioning already active");
        return true;
    }

    s_provInstance = this;
    s_provComplete = false;
    s_provConnected = false;
    _resetJsonParser();

    // Generate device name from MAC for discoverability
    char devName[32];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(devName, sizeof(devName), "Tritium-%02X%02X", mac[4], mac[5]);

    BLEDevice::init(devName);
    s_provServer = BLEDevice::createServer();
    s_provServer->setCallbacks(new ProvBLEServerCallbacks());

    BLEService* service = s_provServer->createService(PROV_SERVICE_UUID);

    s_provChar = service->createCharacteristic(
        PROV_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    s_provChar->addDescriptor(new BLE2902());
    s_provChar->setCallbacks(new ProvBLECharCallbacks());

    service->start();

    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(PROV_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    BLEDevice::startAdvertising();

    _bleActive = true;
    DBG_INFO(TAG, "BLE provisioning started — advertising as \"%s\"", devName);
    DBG_INFO(TAG, "Service: %s", PROV_SERVICE_UUID);
    return true;
}

void ProvisionHAL::stopBLEProvision() {
    if (!_bleActive) return;

    BLEDevice::deinit(true);
    s_provServer = nullptr;
    s_provChar = nullptr;
    s_provConnected = false;
    s_provComplete = false;
    s_provInstance = nullptr;
    _resetJsonParser();

    _bleActive = false;
    DBG_INFO(TAG, "BLE provisioning stopped");
}

bool ProvisionHAL::isBLEProvisionActive() const {
    return _bleActive;
}

bool ProvisionHAL::processBLEProvision() {
    if (!_bleActive) return false;
    if (!s_provComplete) return false;

    // We have a complete JSON payload — parse it
    DBG_INFO(TAG, "Processing BLE provision payload");

    char cmd[32] = {};
    jsonGetString(s_provJsonBuf, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "provision") != 0) {
        DBG_ERROR(TAG, "Unknown BLE command: \"%s\"", cmd);
        // Notify client of error
        if (s_provChar && s_provConnected) {
            const char* err = "{\"status\":\"error\",\"msg\":\"Unknown command\"}";
            s_provChar->setValue((uint8_t*)err, strlen(err));
            s_provChar->notify();
        }
        _resetJsonParser();
        s_provComplete = false;
        return false;
    }

    // Extract provisioning fields
    char ssid[64] = {};
    char password[64] = {};
    char serverUrl[256] = {};
    char deviceName[64] = {};
    char deviceId[64] = {};
    char mqttBroker[128] = {};
    int mqttPort = 8883;

    jsonGetString(s_provJsonBuf, "ssid", ssid, sizeof(ssid));
    jsonGetString(s_provJsonBuf, "password", password, sizeof(password));
    jsonGetString(s_provJsonBuf, "server_url", serverUrl, sizeof(serverUrl));
    jsonGetString(s_provJsonBuf, "device_name", deviceName, sizeof(deviceName));
    jsonGetString(s_provJsonBuf, "device_id", deviceId, sizeof(deviceId));
    jsonGetString(s_provJsonBuf, "mqtt_broker", mqttBroker, sizeof(mqttBroker));
    jsonGetInt(s_provJsonBuf, "mqtt_port", &mqttPort);

    // Validate — at minimum we need an SSID
    if (ssid[0] == '\0') {
        DBG_ERROR(TAG, "BLE provision missing required field: ssid");
        if (s_provChar && s_provConnected) {
            const char* err = "{\"status\":\"error\",\"msg\":\"Missing ssid\"}";
            s_provChar->setValue((uint8_t*)err, strlen(err));
            s_provChar->notify();
        }
        _resetJsonParser();
        s_provComplete = false;
        return false;
    }

    DBG_INFO(TAG, "BLE provision: ssid=\"%s\" server=\"%s\" name=\"%s\"",
             ssid, serverUrl, deviceName);

    // Save WiFi credentials to NVS via Preferences
    {
        Preferences prefs;
        prefs.begin("ble_prov", false);
        prefs.putString("ssid", ssid);
        prefs.putString("password", password);
        if (serverUrl[0]) prefs.putString("server_url", serverUrl);
        if (deviceName[0]) prefs.putString("dev_name", deviceName);
        prefs.end();
        DBG_INFO(TAG, "WiFi credentials saved to NVS");
    }

    // Also save as factory WiFi JSON for hal_wifi pickup
    {
        char wifiJson[256];
        snprintf(wifiJson, sizeof(wifiJson),
                 "{\"ssid\":\"%s\",\"password\":\"%s\"}", ssid, password);
        _parseFactoryWifiJson(wifiJson);
        DBG_INFO(TAG, "Factory WiFi config saved");
    }

    // Update device identity if fields provided
    if (deviceId[0]) {
        strncpy(_identity.device_id, deviceId, sizeof(_identity.device_id) - 1);
    }
    if (deviceName[0]) {
        strncpy(_identity.device_name, deviceName, sizeof(_identity.device_name) - 1);
    }
    if (serverUrl[0]) {
        strncpy(_identity.server_url, serverUrl, sizeof(_identity.server_url) - 1);
    }
    if (mqttBroker[0]) {
        strncpy(_identity.mqtt_broker, mqttBroker, sizeof(_identity.mqtt_broker) - 1);
    }
    _identity.mqtt_port = (uint16_t)mqttPort;
    _identity.provisioned = true;
    _saveIdentity();

    // Notify client of success
    if (s_provChar && s_provConnected) {
        const char* ok = "{\"status\":\"ok\",\"msg\":\"Provisioned — rebooting\"}";
        s_provChar->setValue((uint8_t*)ok, strlen(ok));
        s_provChar->notify();
    }

    DBG_INFO(TAG, "BLE provisioning complete — rebooting in 2 seconds");

    // Short delay to allow notification delivery, then disconnect and reboot
    delay(2000);
    stopBLEProvision();
    delay(500);
    ESP.restart();

    // Unreachable, but satisfy return type
    return true;
}

#else  // !ENABLE_BLE

bool ProvisionHAL::startBLEProvision() {
    DBG_WARN(TAG, "BLE provisioning not available (ENABLE_BLE not defined)");
    return false;
}

void ProvisionHAL::stopBLEProvision() {}

bool ProvisionHAL::isBLEProvisionActive() const { return false; }

bool ProvisionHAL::processBLEProvision() { return false; }

#endif  // ENABLE_BLE

// ============================================================================
// Web provisioning stubs (ESP32 — not yet implemented)
// ============================================================================
bool ProvisionHAL::startWebProvision() {
    DBG_WARN(TAG, "Web provisioning not yet implemented");
    return false;
}

void ProvisionHAL::stopWebProvision() {
    _webActive = false;
}

bool ProvisionHAL::isWebProvisionActive() const {
    return _webActive;
}

#endif // SIMULATOR
