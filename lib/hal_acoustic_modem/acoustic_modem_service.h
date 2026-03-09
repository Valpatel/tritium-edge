#pragma once
// Acoustic Modem service adapter — wraps AcousticModem + AudioHAL as a ServiceInterface.
// Priority 70.

#include "service.h"

#if defined(HAS_AUDIO_CODEC) && HAS_AUDIO_CODEC && __has_include("hal_acoustic_modem.h")
#include "hal_audio.h"
#include "hal_acoustic_modem.h"
#endif

class AcousticModemService : public ServiceInterface {
public:
    const char* name() const override { return "acoustic_modem"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD; }
    int initPriority() const override { return 70; }

    bool init() override {
#if defined(HAS_AUDIO_CODEC) && HAS_AUDIO_CODEC && __has_include("hal_acoustic_modem.h")
        _audio_ok = _audio.initLgfx(0);
        Serial.printf("[tritium] Audio codec: %s\n", _audio_ok ? "OK" : "FAIL");
        if (_audio_ok) {
            AcousticModemConfig modem_cfg;
            _modem_ok = _modem.init(_audio, modem_cfg);
            Serial.printf("[tritium] Acoustic modem: %s\n",
                          _modem_ok ? "OK" : "FAIL");
            return _modem_ok;
        }
        return false;
#else
        return false;
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(HAS_AUDIO_CODEC) && HAS_AUDIO_CODEC && __has_include("hal_acoustic_modem.h")
        if (strcmp(cmd, "MODEM_SEND") == 0) {
            if (!_modem_ok) {
                Serial.printf("[modem] Not initialized\n");
            } else {
                if (!args || args[0] == '\0') {
                    Serial.printf("[modem] Usage: MODEM_SEND <hex> (even number of hex chars)\n");
                    return true;
                }
                // Parse hex string to bytes
                const char* hex = args;
                size_t hex_len = strlen(hex);
                size_t byte_len = hex_len / 2;
                if (byte_len == 0 || (hex_len % 2) != 0) {
                    Serial.printf("[modem] Usage: MODEM_SEND <hex> (even number of hex chars)\n");
                } else {
                    uint8_t tx_buf[256];
                    if (byte_len > sizeof(tx_buf)) byte_len = sizeof(tx_buf);
                    bool parse_ok = true;
                    for (size_t i = 0; i < byte_len; i++) {
                        char hi = hex[i * 2], lo = hex[i * 2 + 1];
                        auto hexval = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                            return -1;
                        };
                        int h = hexval(hi), l = hexval(lo);
                        if (h < 0 || l < 0) { parse_ok = false; break; }
                        tx_buf[i] = (uint8_t)((h << 4) | l);
                    }
                    if (!parse_ok) {
                        Serial.printf("[modem] Invalid hex characters\n");
                    } else {
                        Serial.printf("[modem] Sending %u bytes...\n", (unsigned)byte_len);
                        int sent = _modem.send(tx_buf, byte_len);
                        Serial.printf("[modem] Sent: %d bytes\n", sent);
                    }
                }
            }
            return true;
        }
        if (strcmp(cmd, "MODEM_LISTEN") == 0) {
            if (!_modem_ok) {
                Serial.printf("[modem] Not initialized\n");
            } else {
                uint32_t timeout_ms = 5000;  // default 5s
                if (args && args[0] != '\0') {
                    timeout_ms = (uint32_t)atol(args);
                    if (timeout_ms == 0) timeout_ms = 5000;
                }
                Serial.printf("[modem] Listening for %lu ms...\n", (unsigned long)timeout_ms);
                uint8_t rx_buf[256];
                int len = _modem.receive(rx_buf, sizeof(rx_buf), timeout_ms);
                if (len > 0) {
                    Serial.printf("[modem] Received %d bytes: ", len);
                    for (int i = 0; i < len; i++) Serial.printf("%02X", rx_buf[i]);
                    Serial.printf("\n");
                } else if (len == 0) {
                    Serial.printf("[modem] Timeout — no data received\n");
                } else {
                    Serial.printf("[modem] Error (CRC fail or decode error)\n");
                }
            }
            return true;
        }
        if (strcmp(cmd, "MODEM_STATS") == 0) {
            if (!_modem_ok) {
                Serial.printf("[modem] Not initialized\n");
            } else {
                const auto& s = _modem.stats();
                Serial.printf("[modem] frames_sent=%lu frames_received=%lu "
                              "crc_errors=%lu sync_timeouts=%lu snr=%.1f dB\n",
                              (unsigned long)s.frames_sent,
                              (unsigned long)s.frames_received,
                              (unsigned long)s.crc_errors,
                              (unsigned long)s.sync_timeouts,
                              s.last_snr_db);
            }
            return true;
        }
        if (strcmp(cmd, "MODEM_TEST") == 0) {
            if (!_modem_ok) {
                Serial.printf("[modem] Not initialized\n");
            } else {
                const uint8_t test_msg[] = "TRITIUM";
                Serial.printf("[modem] Self-test: sending 'TRITIUM'...\n");
                int sent = _modem.send(test_msg, sizeof(test_msg) - 1);
                Serial.printf("[modem] Sent %d bytes, listening for echo...\n", sent);
                uint8_t rx_buf[256];
                int len = _modem.receive(rx_buf, sizeof(rx_buf), 3000);
                if (len > 0) {
                    rx_buf[len < 255 ? len : 255] = '\0';
                    Serial.printf("[modem] Echo received: '%s' (%d bytes)\n",
                                  (const char*)rx_buf, len);
                } else {
                    Serial.printf("[modem] No echo (timeout or error)\n");
                }
                const auto& s = _modem.stats();
                Serial.printf("[modem] Stats: sent=%lu recv=%lu crc_err=%lu snr=%.1f dB\n",
                              (unsigned long)s.frames_sent,
                              (unsigned long)s.frames_received,
                              (unsigned long)s.crc_errors,
                              s.last_snr_db);
            }
            return true;
        }
#endif
        return false;
    }

private:
#if defined(HAS_AUDIO_CODEC) && HAS_AUDIO_CODEC && __has_include("hal_acoustic_modem.h")
    AudioHAL _audio;
    bool _audio_ok = false;
    AcousticModem _modem;
    bool _modem_ok = false;
#endif
};
