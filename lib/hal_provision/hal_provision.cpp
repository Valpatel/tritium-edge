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
    _saveIdentity();

    _checkProvisioned();
    _usbActive = false;
    DBG_INFO(TAG, "USB provisioning complete");
    return _state == ProvisionState::PROVISIONED;
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

// ============================================================================
// ESP32 — real LittleFS + SD_MMC
// ============================================================================
#else

#include "tritium_compat.h"
#include <esp_heap_caps.h>
// LittleFS via VFS — use POSIX file ops on /littlefs/
// Filesystem via VFS — use POSIX file ops
// SD card via VFS — use POSIX file ops on /sdcard/

#include <sys/stat.h>
#include "esp_littlefs.h"
#include <cstdio>

#ifndef HAS_SDCARD
#define HAS_SDCARD 0
#endif

// Mount prefix for LittleFS VFS (mounted by webserver_service.h)
static constexpr const char* LFS_PREFIX = "/littlefs";

// Build full VFS path from a relative LittleFS path
static void lfs_path(const char* path, char* out, size_t outSize) {
    snprintf(out, outSize, "%s%s", LFS_PREFIX, path);
}

// Check if a file/dir exists via stat()
static bool lfs_exists(const char* path) {
    char fp[512];
    lfs_path(path, fp, sizeof(fp));
    struct stat st;
    return stat(fp, &st) == 0;
}

// Read a file into a buffer, returns bytes read
static bool lfs_read_file(const char* path, char* buf, size_t bufSize, size_t* outLen) {
    char fp[512];
    lfs_path(path, fp, sizeof(fp));
    FILE* f = fopen(fp, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, bufSize, f);
    fclose(f);
    if (outLen) *outLen = n;
    return true;
}

// Write data to a file
static bool lfs_write_file(const char* path, const char* data, size_t len) {
    char fp[512];
    lfs_path(path, fp, sizeof(fp));
    FILE* f = fopen(fp, "w");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

// Remove a file
static bool lfs_remove_file(const char* path) {
    char fp[512];
    lfs_path(path, fp, sizeof(fp));
    return remove(fp) == 0;
}

bool ProvisionHAL::init() {
    // LittleFS is already mounted at /littlefs by esp_vfs_littlefs_register()
    // in webserver_service.h. Just create the provisioning directory if needed.

    // Create provisioning directory
    if (!lfs_exists(PROV_DIR)) {
        char fp[512];
        lfs_path(PROV_DIR, fp, sizeof(fp));
        ::mkdir(fp, 0755);
    }

    {
        size_t total = 0, used = 0;
        if (esp_littlefs_info("littlefs", &total, &used) == ESP_OK) {
            DBG_INFO(TAG, "Provision HAL init — LittleFS total: %zu  used: %zu", total, used);
        } else {
            DBG_INFO(TAG, "Provision HAL init — LittleFS mounted");
        }
    }

    _loadIdentity();
    _checkProvisioned();
    DBG_INFO(TAG, "State: %s", _state == ProvisionState::PROVISIONED ? "PROVISIONED" : "UNPROVISIONED");
    return true;
}

ProvisionState ProvisionHAL::getState() const { return _state; }
bool ProvisionHAL::isProvisioned() const { return _state == ProvisionState::PROVISIONED; }
const DeviceIdentity& ProvisionHAL::getIdentity() const { return _identity; }

bool ProvisionHAL::_checkProvisioned() {
    bool hasCerts = lfs_exists(CA_CERT_PATH) &&
                    lfs_exists(CLIENT_CERT_PATH) &&
                    lfs_exists(CLIENT_KEY_PATH);
    if (hasCerts && _identity.provisioned) {
        _state = ProvisionState::PROVISIONED;
    } else {
        _state = ProvisionState::UNPROVISIONED;
    }
    return _state == ProvisionState::PROVISIONED;
}

bool ProvisionHAL::_loadIdentity() {
    char buf[1024];
    size_t n = 0;
    if (!lfs_read_file(IDENTITY_PATH, buf, sizeof(buf) - 1, &n)) return false;
    if (n == 0 || n > 1024) return false;
    buf[n] = '\0';
    return _parseDeviceJson(buf);
}

bool ProvisionHAL::_saveIdentity() {
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

    if (!lfs_write_file(IDENTITY_PATH, json, strlen(json))) {
        DBG_ERROR(TAG, "Cannot write identity file");
        return false;
    }
    return true;
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
    return lfs_write_file(FACTORY_WIFI_PATH, json, strlen(json));
}

bool ProvisionHAL::_copyFileFromSD(const char* sdPath, const char* fsPath) {
#if HAS_SDCARD
    // SD card is mounted at /sdcard via VFS
    char sdFullPath[512];
    snprintf(sdFullPath, sizeof(sdFullPath), "/sdcard%s", sdPath);

    FILE* sf = fopen(sdFullPath, "r");
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
    if (!buf) {
        DBG_ERROR(TAG, "malloc failed for %ld bytes", sz);
        fclose(sf);
        return false;
    }
    size_t rd = fread(buf, 1, sz, sf);
    fclose(sf);

    if (!lfs_write_file(fsPath, buf, rd)) {
        DBG_ERROR(TAG, "Cannot write to %s", fsPath);
        free(buf);
        return false;
    }
    free(buf);

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
    {
        char sdFullPath[512];
        snprintf(sdFullPath, sizeof(sdFullPath), "/sdcard%s", path);
        FILE* jf = fopen(sdFullPath, "r");
        if (!jf) {
            DBG_ERROR(TAG, "Cannot read %s from SD", path);
            return false;
        }
        char jsonBuf[1024];
        size_t jsonLen = fread(jsonBuf, 1, sizeof(jsonBuf) - 1, jf);
        fclose(jf);
        jsonBuf[jsonLen] = '\0';
        _parseDeviceJson(jsonBuf);
        _identity.provisioned = true;
        _saveIdentity();
        DBG_INFO(TAG, "Device identity loaded: id=%s name=%s", _identity.device_id, _identity.device_name);

        // Parse factory_wifi.json (optional)
        snprintf(path, sizeof(path), "%s/factory_wifi.json", sdPath);
        snprintf(sdFullPath, sizeof(sdFullPath), "/sdcard%s", path);
        FILE* wf = fopen(sdFullPath, "r");
        if (wf) {
            jsonLen = fread(jsonBuf, 1, sizeof(jsonBuf) - 1, wf);
            fclose(wf);
            jsonBuf[jsonLen] = '\0';
            _parseFactoryWifiJson(jsonBuf);
            DBG_INFO(TAG, "Factory WiFi config loaded");
        } else {
            DBG_INFO(TAG, "No factory_wifi.json on SD (optional)");
        }
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
    if (!_usbBuf) {
        _usbBuf = (char*)heap_caps_malloc(PROV_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!_usbBuf) _usbBuf = (char*)malloc(PROV_BUF_SIZE);
    }
    if (!_usbBuf) return false;
    _usbActive = true;
    _usbBufLen = 0;
    memset(_usbBuf, 0, PROV_BUF_SIZE);
    Serial.printf("{\"status\":\"ready\",\"msg\":\"Send provisioning JSON\"}\n");
    DBG_INFO(TAG, "USB provisioning started — waiting for data on Serial");
    return true;
}

bool ProvisionHAL::processUSBProvision() {
    if (!_usbActive || !_usbBuf) return false;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (_usbBufLen > 0) {
                _usbBuf[_usbBufLen] = '\0';
                bool ok = _processUSBCommand(_usbBuf, _usbBufLen);
                _usbBufLen = 0;
                if (ok) {
                    Serial.printf("{\"status\":\"ok\",\"msg\":\"Provisioned\"}\n");
                } else {
                    Serial.printf("{\"status\":\"error\",\"msg\":\"Provisioning failed\"}\n");
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
    _saveIdentity();

    _checkProvisioned();
    _usbActive = false;
    DBG_INFO(TAG, "USB provisioning complete");
    return _state == ProvisionState::PROVISIONED;
}

bool ProvisionHAL::getCACert(char* buf, size_t bufSize, size_t* outLen) {
    size_t n = 0;
    if (!lfs_read_file(CA_CERT_PATH, buf, bufSize - 1, &n)) return false;
    buf[n] = '\0';
    if (outLen) *outLen = n;
    return n > 0;
}

bool ProvisionHAL::getClientCert(char* buf, size_t bufSize, size_t* outLen) {
    size_t n = 0;
    if (!lfs_read_file(CLIENT_CERT_PATH, buf, bufSize - 1, &n)) return false;
    buf[n] = '\0';
    if (outLen) *outLen = n;
    return n > 0;
}

bool ProvisionHAL::getClientKey(char* buf, size_t bufSize, size_t* outLen) {
    size_t n = 0;
    if (!lfs_read_file(CLIENT_KEY_PATH, buf, bufSize - 1, &n)) return false;
    buf[n] = '\0';
    if (outLen) *outLen = n;
    return n > 0;
}

bool ProvisionHAL::hasFactoryWiFi() const {
    return lfs_exists(FACTORY_WIFI_PATH);
}

bool ProvisionHAL::getFactoryWiFi(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    char buf[512];
    size_t n = 0;
    if (!lfs_read_file(FACTORY_WIFI_PATH, buf, sizeof(buf) - 1, &n)) return false;
    buf[n] = '\0';
    bool ok = jsonGetString(buf, "ssid", ssid, ssidLen);
    jsonGetString(buf, "password", pass, passLen);
    return ok;
}

bool ProvisionHAL::clearFactoryWiFi() {
    bool ok = lfs_remove_file(FACTORY_WIFI_PATH);
    if (ok) DBG_INFO(TAG, "Factory WiFi cleared");
    return ok;
}

bool ProvisionHAL::importCACert(const char* pem, size_t len) {
    bool ok = lfs_write_file(CA_CERT_PATH, pem, len);
    if (ok) DBG_INFO(TAG, "CA cert imported (%zu bytes)", len);
    return ok;
}

bool ProvisionHAL::importClientCert(const char* pem, size_t len) {
    bool ok = lfs_write_file(CLIENT_CERT_PATH, pem, len);
    if (ok) DBG_INFO(TAG, "Client cert imported (%zu bytes)", len);
    return ok;
}

bool ProvisionHAL::importClientKey(const char* pem, size_t len) {
    bool ok = lfs_write_file(CLIENT_KEY_PATH, pem, len);
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

    lfs_remove_file(CA_CERT_PATH);
    lfs_remove_file(CLIENT_CERT_PATH);
    lfs_remove_file(CLIENT_KEY_PATH);
    lfs_remove_file(IDENTITY_PATH);
    lfs_remove_file(FACTORY_WIFI_PATH);

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
    r.fs_ok = lfs_exists(PROV_DIR);

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

#endif // SIMULATOR
