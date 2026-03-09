// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_lora.h"

#ifdef SIMULATOR

namespace hal_lora {

bool init(const LoRaConfig&) { return false; }
void shutdown() {}
bool is_active() { return false; }
bool send(const uint8_t*, uint16_t) { return false; }
int receive(LoRaMessage*) { return 0; }

bool init_meshtastic_serial(int, int, int, uint32_t) { return false; }
bool init_meshtastic_ble(const char*) { return false; }
bool meshtastic_send_text(const char*, uint32_t) { return false; }
bool meshtastic_receive_poll() { return false; }
const char* meshtastic_get_last_message() { return nullptr; }
uint32_t meshtastic_get_last_message_time() { return 0; }
bool meshtastic_is_connected() { return false; }
int meshtastic_get_nodes(char* buf, size_t buf_size) {
    if (buf_size > 2) { buf[0] = '['; buf[1] = ']'; buf[2] = '\0'; return 2; }
    return 0;
}

int get_last_rssi() { return 0; }
float get_last_snr() { return 0.0f; }
uint32_t get_messages_sent() { return 0; }
uint32_t get_messages_received() { return 0; }
const char* get_mode() { return "inactive"; }

TestResult runTest() {
    return { false, false, false, 0, "inactive", 0 };
}

}  // namespace hal_lora

#else  // ESP32

#include <Arduino.h>
#include <HardwareSerial.h>
#include "debug_log.h"

// Meshtastic BLE service UUID
static const char* MESHTASTIC_SERVICE_UUID = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
// Meshtastic BLE characteristic UUIDs
static const char* MESHTASTIC_TORADIO_UUID = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char* MESHTASTIC_FROMRADIO_UUID = "2c55e69e-4993-11ed-b878-0242ac120002";
static const char* MESHTASTIC_FROMNUM_UUID = "ed9da18c-a800-4f66-a670-aa7547de15e6";

namespace hal_lora {

// --- Internal state ---
static const char* TAG = "lora";

enum class Mode {
    INACTIVE,
    LORA_RAW,
    MESHTASTIC_SERIAL,
    MESHTASTIC_BLE
};

static Mode _mode = Mode::INACTIVE;
static LoRaConfig _config;
static int16_t _last_rssi = 0;
static float _last_snr = 0.0f;
static uint32_t _msgs_sent = 0;
static uint32_t _msgs_received = 0;

// Meshtastic serial state (TEXTMSG mode — plain text, no protobuf)
static HardwareSerial* _mesh_serial = nullptr;
static char _mesh_rx_line[256];       // Line accumulator for incoming text
static uint16_t _mesh_rx_pos = 0;
static char _mesh_last_msg[256];      // Last complete received message
static bool _mesh_has_new_msg = false;
static uint32_t _mesh_last_msg_time = 0;
static uint32_t _mesh_last_rx_time = 0;  // Last time any byte was received

// ---------------------------------------------------------------------------
// Raw LoRa radio (SX1262/SX1276)
// ---------------------------------------------------------------------------

bool init(const LoRaConfig& config) {
    if (_mode != Mode::INACTIVE) {
        DBG_WARN(TAG, "Already initialized in mode: %s", get_mode());
        return false;
    }

    _config = config;

    DBG_INFO(TAG, "Init raw LoRa: freq=%lu BW=%u SF=%u CR=4/%u TX=%ddBm CRC=%s",
             (unsigned long)config.frequency, config.bandwidth,
             config.spreading_factor, config.coding_rate,
             config.tx_power, config.enable_crc ? "on" : "off");

    // TODO: Initialize SX1262/SX1276 via SPI
    // Requires board-specific pin definitions (LORA_CS, LORA_RST, LORA_DIO1, LORA_BUSY)
    // Will use RadioLib or direct SPI register access
    DBG_WARN(TAG, "Raw LoRa radio init not yet implemented (no hardware driver)");

    _mode = Mode::LORA_RAW;
    _msgs_sent = 0;
    _msgs_received = 0;

    return true;
}

void shutdown() {
    if (_mode == Mode::INACTIVE) return;

    DBG_INFO(TAG, "Shutting down (was %s)", get_mode());

    if (_mode == Mode::MESHTASTIC_SERIAL && _mesh_serial) {
        _mesh_serial->end();
        _mesh_serial = nullptr;
    }

    // TODO: Put radio to sleep / deinit BLE client

    _mode = Mode::INACTIVE;
    _mesh_rx_pos = 0;
    _mesh_last_msg[0] = '\0';
    _mesh_has_new_msg = false;
}

bool is_active() {
    return _mode != Mode::INACTIVE;
}

bool send(const uint8_t* data, uint16_t len) {
    if (_mode != Mode::LORA_RAW) {
        DBG_WARN(TAG, "send() requires raw LoRa mode (current: %s)", get_mode());
        return false;
    }

    if (!data || len == 0 || len > 256) {
        DBG_ERROR(TAG, "Invalid send: data=%p len=%u", data, len);
        return false;
    }

    // TODO: Transmit via SX1262/SX1276 SPI
    DBG_INFO(TAG, "send(%u bytes) — not yet implemented", len);

    _msgs_sent++;
    return false;  // Will return true once hardware driver is implemented
}

int receive(LoRaMessage* msg) {
    if (_mode != Mode::LORA_RAW || !msg) return 0;

    // TODO: Check SX1262/SX1276 for received packet via DIO interrupt or polling
    // Non-blocking: return 0 if no packet available

    return 0;
}

// ---------------------------------------------------------------------------
// Meshtastic serial bridge
// ---------------------------------------------------------------------------

bool init_meshtastic_serial(int uart_num, int rx_pin, int tx_pin, uint32_t baud) {
    if (_mode != Mode::INACTIVE) {
        DBG_WARN(TAG, "Already initialized in mode: %s", get_mode());
        return false;
    }

    DBG_INFO(TAG, "Init Meshtastic serial: UART%d RX=%d TX=%d baud=%lu",
             uart_num, rx_pin, tx_pin, (unsigned long)baud);

    // Use HardwareSerial for UART communication with Meshtastic node
    // UART1 or UART2 depending on board config
    switch (uart_num) {
        case 1:
            _mesh_serial = &Serial1;
            break;
        case 2:
            _mesh_serial = &Serial2;
            break;
        default:
            DBG_ERROR(TAG, "Invalid UART number: %d (use 1 or 2)", uart_num);
            return false;
    }

    _mesh_serial->begin(baud, SERIAL_8N1, rx_pin, tx_pin);
    _mesh_rx_pos = 0;
    _mesh_last_msg[0] = '\0';
    _mesh_has_new_msg = false;
    _mesh_last_msg_time = 0;
    _mesh_last_rx_time = 0;

    // TEXTMSG mode: plain text in/out, no protobuf framing needed.
    // The Meshtastic device must be configured with:
    //   serial.enabled = true
    //   serial.mode = TEXTMSG
    //   serial.baud = <matching baud rate>
    //   serial.rxd = <GPIO connected to our TX>
    //   serial.txd = <GPIO connected to our RX>

    _mode = Mode::MESHTASTIC_SERIAL;
    _msgs_sent = 0;
    _msgs_received = 0;

    DBG_INFO(TAG, "Meshtastic serial bridge ready on UART%d", uart_num);
    return true;
}

bool init_meshtastic_ble(const char* device_name) {
    if (_mode != Mode::INACTIVE) {
        DBG_WARN(TAG, "Already initialized in mode: %s", get_mode());
        return false;
    }

    DBG_INFO(TAG, "Init Meshtastic BLE: scanning for '%s'", device_name ? device_name : "(any)");

    // TODO: Use NimBLE client to:
    // 1. Scan for device advertising Meshtastic service UUID
    // 2. Connect and discover characteristics
    // 3. Subscribe to FROMRADIO notifications
    //
    // Service:  6ba1b218-15a8-461f-9fa8-5dcae273eafd
    // ToRadio:  f75c76d2-129e-4dad-a1dd-7866124401e7  (write)
    // FromRadio: 2c55e69e-4993-11ed-b878-0242ac120002 (read/notify)
    // FromNum:  ed9da18c-a800-4f66-a670-aa7547de15e6  (notify - new data available)

    DBG_WARN(TAG, "Meshtastic BLE client not yet implemented");
    (void)MESHTASTIC_SERVICE_UUID;
    (void)MESHTASTIC_TORADIO_UUID;
    (void)MESHTASTIC_FROMRADIO_UUID;
    (void)MESHTASTIC_FROMNUM_UUID;

    _mode = Mode::MESHTASTIC_BLE;
    _msgs_sent = 0;
    _msgs_received = 0;

    return true;
}

bool meshtastic_send_text(const char* text, uint32_t dest) {
    if (_mode != Mode::MESHTASTIC_SERIAL && _mode != Mode::MESHTASTIC_BLE) {
        DBG_WARN(TAG, "meshtastic_send_text() requires Meshtastic mode (current: %s)", get_mode());
        return false;
    }

    if (!text || text[0] == '\0') return false;

    DBG_INFO(TAG, "Meshtastic send text to 0x%08lX: \"%s\"",
             (unsigned long)dest, text);

    if (_mode == Mode::MESHTASTIC_SERIAL && _mesh_serial) {
        // TEXTMSG mode: write plain text + newline to UART.
        // The Meshtastic Serial Module in TEXTMSG mode accepts a line of text
        // and broadcasts it as a TEXT_MESSAGE_APP packet on the default channel.
        // Note: dest parameter is ignored in TEXTMSG mode (always broadcasts).
        size_t len = strlen(text);
        size_t written = _mesh_serial->write((const uint8_t*)text, len);
        _mesh_serial->write('\n');
        _mesh_serial->flush();

        if (written == len) {
            _msgs_sent++;
            DBG_INFO(TAG, "Sent %u bytes via Meshtastic serial", (unsigned)len);
            return true;
        } else {
            DBG_ERROR(TAG, "Serial write failed: wrote %u of %u bytes",
                      (unsigned)written, (unsigned)len);
            return false;
        }
    }

    if (_mode == Mode::MESHTASTIC_BLE) {
        // TODO: Write protobuf to ToRadio BLE characteristic
        DBG_WARN(TAG, "Meshtastic BLE write not yet implemented");
        return false;
    }

    return false;
}

bool meshtastic_receive_poll() {
    if (_mode != Mode::MESHTASTIC_SERIAL || !_mesh_serial) return false;

    bool got_message = false;

    while (_mesh_serial->available()) {
        char c = (char)_mesh_serial->read();
        _mesh_last_rx_time = millis();

        if (c == '\n' || c == '\r') {
            if (_mesh_rx_pos > 0) {
                // Complete line received
                _mesh_rx_line[_mesh_rx_pos] = '\0';

                // In TEXTMSG mode, incoming messages are prefixed with sender short name:
                //   "ABC: Hello world"
                // Store the full line including the prefix.
                strncpy(_mesh_last_msg, _mesh_rx_line, sizeof(_mesh_last_msg) - 1);
                _mesh_last_msg[sizeof(_mesh_last_msg) - 1] = '\0';
                _mesh_has_new_msg = true;
                _mesh_last_msg_time = millis();
                _msgs_received++;

                DBG_INFO(TAG, "Mesh RX: \"%s\"", _mesh_last_msg);
                got_message = true;
            }
            _mesh_rx_pos = 0;
        } else if (_mesh_rx_pos < sizeof(_mesh_rx_line) - 1) {
            _mesh_rx_line[_mesh_rx_pos++] = c;
        }
        // else: line too long, drop character
    }

    return got_message;
}

const char* meshtastic_get_last_message() {
    if (!_mesh_has_new_msg && _mesh_last_msg[0] == '\0') return nullptr;
    return _mesh_last_msg;
}

uint32_t meshtastic_get_last_message_time() {
    return _mesh_last_msg_time;
}

bool meshtastic_is_connected() {
    if (_mode != Mode::MESHTASTIC_SERIAL || !_mesh_serial) return false;
    // Consider connected if we received any data within the last 60 seconds.
    // Meshtastic serial module sends periodic output (nodeinfo, telemetry).
    if (_mesh_last_rx_time == 0) return false;
    return (millis() - _mesh_last_rx_time) < 60000;
}

int meshtastic_get_nodes(char* json_buf, size_t buf_size) {
    if (_mode != Mode::MESHTASTIC_SERIAL && _mode != Mode::MESHTASTIC_BLE) {
        if (buf_size > 2) { json_buf[0] = '['; json_buf[1] = ']'; json_buf[2] = '\0'; }
        return 2;
    }

    // In TEXTMSG mode we don't have access to the full node database.
    // Return local bridge status as a single-node array.
    return snprintf(json_buf, buf_size,
        "[{\"role\":\"bridge\",\"mode\":\"%s\",\"connected\":%s,"
        "\"tx\":%lu,\"rx\":%lu,\"last_rx_age_ms\":%lu}]",
        get_mode(),
        meshtastic_is_connected() ? "true" : "false",
        (unsigned long)_msgs_sent,
        (unsigned long)_msgs_received,
        _mesh_last_rx_time > 0 ? (unsigned long)(millis() - _mesh_last_rx_time) : 0UL);
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

int get_last_rssi() { return _last_rssi; }
float get_last_snr() { return _last_snr; }
uint32_t get_messages_sent() { return _msgs_sent; }
uint32_t get_messages_received() { return _msgs_received; }

const char* get_mode() {
    switch (_mode) {
        case Mode::LORA_RAW:            return "lora_raw";
        case Mode::MESHTASTIC_SERIAL:   return "meshtastic_serial";
        case Mode::MESHTASTIC_BLE:      return "meshtastic_ble";
        default:                        return "inactive";
    }
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

TestResult runTest() {
    TestResult result = {};
    uint32_t start = millis();

    DBG_INFO(TAG, "=== LoRa HAL Test ===");

    // Test 1: Raw LoRa init
    LoRaConfig cfg;
    result.init_ok = init(cfg);
    DBG_INFO(TAG, "Raw init: %s", result.init_ok ? "OK" : "FAIL");

    if (result.init_ok) {
        // Test 2: Send (will fail until hardware driver is wired up)
        uint8_t test_data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        result.send_ok = send(test_data, sizeof(test_data));
        DBG_INFO(TAG, "Send test: %s", result.send_ok ? "OK" : "FAIL (expected — no HW)");

        // Test 3: Receive (non-blocking)
        LoRaMessage msg;
        result.messages_received = receive(&msg);
        DBG_INFO(TAG, "Receive test: %d messages", result.messages_received);

        shutdown();
    }

    // Test 4: Meshtastic serial init (dry run — no actual UART wired)
    // Skip actual serial init to avoid claiming pins
    result.meshtastic_ok = false;
    DBG_INFO(TAG, "Meshtastic: skipped (no UART pins configured)");

    result.mode = get_mode();
    result.test_duration_ms = millis() - start;

    DBG_INFO(TAG, "=== LoRa Test Complete (%lums) ===", (unsigned long)result.test_duration_ms);

    return result;
}

}  // namespace hal_lora

#endif  // !SIMULATOR
