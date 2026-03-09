#pragma once
// LoRa/Meshtastic service adapter — wraps hal_lora namespace as a ServiceInterface.
// Priority 50.
// Handles both meshtastic serial and raw LoRa init paths.

#include "service.h"

#if defined(ENABLE_LORA) && __has_include("hal_lora.h")
#include "hal_lora.h"
#endif

class LoraService : public ServiceInterface {
public:
    const char* name() const override { return "lora"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD; }
    int initPriority() const override { return 50; }

    bool init() override {
#if defined(ENABLE_LORA)
        // Default: init raw LoRa. Override with MESHTASTIC_UART_NUM build flag
        // to use Meshtastic TEXTMSG serial bridge instead.
#if defined(MESHTASTIC_UART_NUM) && defined(MESHTASTIC_RX_PIN) && defined(MESHTASTIC_TX_PIN)
        uint32_t mesh_baud = 115200;
#if defined(MESHTASTIC_BAUD)
        mesh_baud = MESHTASTIC_BAUD;
#endif
        if (hal_lora::init_meshtastic_serial(MESHTASTIC_UART_NUM,
                                              MESHTASTIC_RX_PIN,
                                              MESHTASTIC_TX_PIN,
                                              mesh_baud)) {
            Serial.printf("[tritium] Meshtastic: active (UART%d, %lu baud)\n",
                          MESHTASTIC_UART_NUM, (unsigned long)mesh_baud);
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] Meshtastic: serial init failed\n");
            return false;
        }
#else
        hal_lora::LoRaConfig lora_cfg;
        if (hal_lora::init(lora_cfg)) {
            Serial.printf("[tritium] LoRa: active (%s)\n", hal_lora::get_mode());
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] LoRa: init failed\n");
            return false;
        }
#endif
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_LORA)
        if (_active) hal_lora::meshtastic_receive_poll();
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_LORA)
        if (strcmp(cmd, "MESH_SEND") == 0) {
            if (!args || args[0] == '\0') {
                Serial.printf("[lora] Usage: MESH_SEND <text>\n");
            } else if (hal_lora::meshtastic_send_text(args)) {
                Serial.printf("[lora] Sent: \"%s\"\n", args);
            } else {
                Serial.printf("[lora] Send failed (mode: %s)\n", hal_lora::get_mode());
            }
            return true;
        }
        if (strcmp(cmd, "MESH_RECV") == 0) {
            const char* msg = hal_lora::meshtastic_get_last_message();
            if (msg) {
                uint32_t age = millis() - hal_lora::meshtastic_get_last_message_time();
                Serial.printf("[lora] Last message (%lu ms ago): \"%s\"\n",
                              (unsigned long)age, msg);
            } else {
                Serial.printf("[lora] No messages received yet\n");
            }
            return true;
        }
        if (strcmp(cmd, "MESH_STATUS") == 0) {
            Serial.printf("[lora] Mode: %s\n", hal_lora::get_mode());
            Serial.printf("[lora] Connected: %s\n",
                          hal_lora::meshtastic_is_connected() ? "yes" : "no");
            Serial.printf("[lora] TX: %lu  RX: %lu\n",
                          (unsigned long)hal_lora::get_messages_sent(),
                          (unsigned long)hal_lora::get_messages_received());
            static char nbuf[512];
            hal_lora::meshtastic_get_nodes(nbuf, sizeof(nbuf));
            Serial.printf("[lora] Nodes: %s\n", nbuf);
            return true;
        }
#endif
        return false;
    }

private:
    bool _active = false;
};
