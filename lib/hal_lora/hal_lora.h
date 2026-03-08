// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include <cstdint>
#include <cstddef>

namespace hal_lora {

struct LoRaConfig {
    uint32_t frequency = 915000000;  // 915MHz US default
    uint8_t bandwidth = 125;         // kHz
    uint8_t spreading_factor = 7;
    uint8_t coding_rate = 5;         // 4/5
    int8_t tx_power = 17;            // dBm
    bool enable_crc = true;
};

struct LoRaMessage {
    uint8_t src_addr[4];
    uint8_t dst_addr[4];
    uint8_t data[256];
    uint16_t data_len;
    int16_t rssi;
    float snr;
    uint32_t timestamp_ms;
};

// Raw LoRa radio (SX1262/SX1276)
bool init(const LoRaConfig& config);
void shutdown();
bool is_active();
bool send(const uint8_t* data, uint16_t len);
int receive(LoRaMessage* msg);  // Non-blocking, returns 0 if no message

// Meshtastic serial bridge (for external Meshtastic nodes)
bool init_meshtastic_serial(int uart_num, int rx_pin, int tx_pin, uint32_t baud = 115200);
bool init_meshtastic_ble(const char* device_name);
bool meshtastic_send_text(const char* text, uint32_t dest = 0xFFFFFFFF);
int meshtastic_get_nodes(char* json_buf, size_t buf_size);

// Info
int get_last_rssi();
float get_last_snr();
uint32_t get_messages_sent();
uint32_t get_messages_received();
const char* get_mode();  // "lora_raw", "meshtastic_serial", "meshtastic_ble", "inactive"

// Test harness
struct TestResult {
    bool init_ok;
    bool send_ok;
    bool meshtastic_ok;
    int messages_received;
    const char* mode;
    uint32_t test_duration_ms;
};
TestResult runTest();

}  // namespace hal_lora
