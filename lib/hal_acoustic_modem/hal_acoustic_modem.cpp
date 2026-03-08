// Acoustic Modem — FSK data over speaker/microphone
//
// Protocol:
//   [PREAMBLE] [SYNC] [LEN_HI] [LEN_LO] [DATA...] [CRC_HI] [CRC_LO]
//
// - Preamble: alternating 0/1 bits for AGC settle and clock recovery
// - Sync: 0x7E byte marks start of frame
// - Length: 16-bit big-endian payload length
// - Data: payload bytes (MSB first per byte)
// - CRC: CRC-16/CCITT over length + data bytes
//
// Each bit is one FSK symbol: mark (1) = freq_mark Hz, space (0) = freq_space Hz.
// Symbol duration = 1/baud_rate seconds.
//
// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_acoustic_modem.h"
#include "hal_audio.h"

#include <cstring>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Maximum frame payload size
static constexpr size_t MAX_FRAME_PAYLOAD = 1024;

// Maximum samples per symbol: at 16 kHz sample rate, 50 baud = 320 samples.
// Higher sample rates or lower baud rates need this bumped.
static constexpr size_t MAX_SAMPLES_PER_SYMBOL = 320;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool AcousticModem::init(AudioHAL &audio, const AcousticModemConfig &config) {
    if (!audio.available()) {
        return false;
    }
    _audio = &audio;
    _config = config;
    _sample_rate = audio.getSampleRate();

    // Validate: both frequencies must be below Nyquist
    uint32_t nyquist = _sample_rate / 2;
    if (_config.freq_mark >= nyquist || _config.freq_space >= nyquist) {
        return false;
    }

    // Validate: frequencies must be distinct
    if (_config.freq_mark == _config.freq_space) {
        return false;
    }

    // Validate: baud rate must yield at least 2 samples per symbol
    if (_sample_rate / _config.baud_rate < 2) {
        return false;
    }

    // Precompute Goertzel coefficients for the two FSK frequencies.
    // These depend on the number of samples per symbol (block size for Goertzel).
    uint16_t N = samples_per_symbol();
    float k_mark = (float)_config.freq_mark / _sample_rate * N;
    float k_space = (float)_config.freq_space / _sample_rate * N;
    _goertzel_coeff_mark = 2.0f * cosf(2.0f * M_PI * k_mark / N);
    _goertzel_coeff_space = 2.0f * cosf(2.0f * M_PI * k_space / N);

    _stats = {};
    _rx_state = AcousticRxState::IDLE;
    _initialized = true;

#ifndef SIMULATOR
    Serial.printf("[AMODEM] Init: mark=%dHz space=%dHz baud=%d sps=%d\n",
                  _config.freq_mark, _config.freq_space,
                  _config.baud_rate, N);
#endif

    return true;
}

// ---------------------------------------------------------------------------
// Info helpers
// ---------------------------------------------------------------------------

float AcousticModem::throughput_bps() const {
    if (!_initialized) return 0.0f;
    // Overhead per frame: preamble + sync(8) + length(16) + crc(16) bits
    // For a 64-byte payload:
    //   total_bits = preamble + 8 + 16 + (64*8) + 16 = preamble + 552
    //   efficiency = 512 / total_bits
    // This returns raw data rate; actual throughput depends on payload size.
    float symbol_rate = (float)_config.baud_rate;
    float fec_factor = 1.0f;
    if (_config.error_correction == AcousticErrorCorrection::REPEAT_3) {
        fec_factor = 3.0f;
    } else if (_config.error_correction == AcousticErrorCorrection::HAMMING_7_4) {
        fec_factor = 7.0f / 4.0f;
    }
    return symbol_rate / fec_factor;
}

uint32_t AcousticModem::symbol_duration_us() const {
    if (_config.baud_rate == 0) return 0;
    return 1000000U / _config.baud_rate;
}

uint16_t AcousticModem::samples_per_symbol() const {
    if (_config.baud_rate == 0) return 0;
    return (uint16_t)(_sample_rate / _config.baud_rate);
}

// ---------------------------------------------------------------------------
// CRC-16/CCITT (polynomial 0x1021, init 0xFFFF)
// ---------------------------------------------------------------------------

uint16_t AcousticModem::crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// FEC: Forward Error Correction
// ---------------------------------------------------------------------------

size_t AcousticModem::applyFEC(const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t out_max) {
    switch (_config.error_correction) {
    case AcousticErrorCorrection::REPEAT_3: {
        // Triple each byte
        size_t needed = in_len * 3;
        if (needed > out_max) return 0;
        for (size_t i = 0; i < in_len; i++) {
            out[i * 3 + 0] = in[i];
            out[i * 3 + 1] = in[i];
            out[i * 3 + 2] = in[i];
        }
        return needed;
    }
    case AcousticErrorCorrection::HAMMING_7_4:
        // Future: Hamming(7,4) encoding
        // Fall through to NONE for now
    case AcousticErrorCorrection::NONE:
    default:
        if (in_len > out_max) return 0;
        memcpy(out, in, in_len);
        return in_len;
    }
}

size_t AcousticModem::decodeFEC(const uint8_t *in, size_t in_len,
                                 uint8_t *out, size_t out_max) {
    switch (_config.error_correction) {
    case AcousticErrorCorrection::REPEAT_3: {
        // Majority vote over triples
        size_t out_len = in_len / 3;
        if (out_len > out_max) return 0;
        for (size_t i = 0; i < out_len; i++) {
            uint8_t a = in[i * 3 + 0];
            uint8_t b = in[i * 3 + 1];
            uint8_t c = in[i * 3 + 2];
            // Bitwise majority vote
            out[i] = (a & b) | (b & c) | (a & c);
        }
        return out_len;
    }
    case AcousticErrorCorrection::HAMMING_7_4:
        // Future
    case AcousticErrorCorrection::NONE:
    default:
        if (in_len > out_max) return 0;
        memcpy(out, in, in_len);
        return in_len;
    }
}

// ---------------------------------------------------------------------------
// TX: Tone generation
// ---------------------------------------------------------------------------

void AcousticModem::generateTone(int16_t *buf, size_t num_samples,
                                  uint16_t freq, float amplitude,
                                  uint32_t &phase_acc) {
    // Generate a continuous-phase sine wave into buf (mono samples).
    // phase_acc tracks cumulative phase in fixed-point (units of samples)
    // to maintain phase continuity across calls.
    for (size_t i = 0; i < num_samples; i++) {
        float t = (float)(phase_acc + i) / (float)_sample_rate;
        float sample = amplitude * sinf(2.0f * M_PI * freq * t);
        // Clamp to int16 range
        int32_t s = (int32_t)(sample * 32000.0f);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
    phase_acc += num_samples;
}

void AcousticModem::transmitBit(bool bit, uint32_t &phase_acc) {
    uint16_t N = samples_per_symbol();
    // Stereo output: duplicate mono to L+R
    int16_t mono[MAX_SAMPLES_PER_SYMBOL];
    int16_t stereo[MAX_SAMPLES_PER_SYMBOL * 2];

    uint16_t freq = bit ? _config.freq_mark : _config.freq_space;
    generateTone(mono, N, freq, _config.tx_amplitude, phase_acc);

    for (uint16_t i = 0; i < N; i++) {
        stereo[i * 2 + 0] = mono[i]; // Left
        stereo[i * 2 + 1] = mono[i]; // Right
    }

    _audio->playBuffer(stereo, N * 2);
}

void AcousticModem::transmitByte(uint8_t byte, uint32_t &phase_acc) {
    // MSB first
    for (int b = 7; b >= 0; b--) {
        transmitBit((byte >> b) & 1, phase_acc);
    }
}

void AcousticModem::transmitPreamble(uint32_t &phase_acc) {
    for (uint16_t i = 0; i < _config.preamble_bits; i++) {
        transmitBit(i & 1, phase_acc);
    }
}

void AcousticModem::transmitSync(uint32_t &phase_acc) {
    transmitByte(_config.sync_word, phase_acc);
}

// ---------------------------------------------------------------------------
// TX: Send
// ---------------------------------------------------------------------------

int AcousticModem::send(const uint8_t *data, size_t len) {
    if (!_initialized || !data || len == 0) return -1;
    if (len > MAX_FRAME_PAYLOAD) return -1;

    // Build the frame: [LEN_HI][LEN_LO][DATA...][CRC_HI][CRC_LO]
    // Apply FEC to the entire frame (length + data + crc)
    uint8_t frame[MAX_FRAME_PAYLOAD + 4]; // length(2) + data + crc(2)
    frame[0] = (uint8_t)(len >> 8);
    frame[1] = (uint8_t)(len & 0xFF);
    memcpy(&frame[2], data, len);

    uint16_t crc = crc16(frame, len + 2);
    frame[len + 2] = (uint8_t)(crc >> 8);
    frame[len + 3] = (uint8_t)(crc & 0xFF);

    size_t frame_len = len + 4;

    // Apply FEC encoding
    uint8_t fec_buf[MAX_FRAME_PAYLOAD * 3 + 12];
    size_t fec_len = applyFEC(frame, frame_len, fec_buf, sizeof(fec_buf));
    if (fec_len == 0) return -1;

    // Enable speaker
    _audio->setSpeakerEnabled(true);

    uint32_t phase_acc = 0;

    // Leading silence (half-symbol of zeros) for AGC settle
    uint16_t N = samples_per_symbol();
    int16_t silence[MAX_SAMPLES_PER_SYMBOL * 2];
    memset(silence, 0, N * 2 * sizeof(int16_t));
    _audio->playBuffer(silence, N * 2);

    // Transmit preamble
    transmitPreamble(phase_acc);

    // Transmit sync word
    transmitSync(phase_acc);

    // Transmit FEC-encoded frame bytes
    for (size_t i = 0; i < fec_len; i++) {
        transmitByte(fec_buf[i], phase_acc);
    }

    // Trailing silence
    _audio->playBuffer(silence, N * 2);

    _stats.frames_sent++;

#ifndef SIMULATOR
    Serial.printf("[AMODEM] TX: %d bytes, %d FEC bytes, %d symbols\n",
                  (int)len, (int)fec_len, (int)(fec_len * 8 +
                  _config.preamble_bits + 8));
#endif

    return (int)len;
}

// ---------------------------------------------------------------------------
// RX: Goertzel single-frequency magnitude
// ---------------------------------------------------------------------------

float AcousticModem::goertzelMagnitude(const int16_t *samples,
                                        size_t num_samples, float coeff) {
    // Goertzel algorithm: O(N) computation of a single DFT bin.
    // coeff = 2*cos(2*pi*k/N) is precomputed for the target frequency.
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;

    for (size_t i = 0; i < num_samples; i++) {
        s0 = (samples[i] / 32768.0f) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    // Power = s1^2 + s2^2 - coeff*s1*s2 (avoids final complex multiply)
    float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (power < 0.0f) power = 0.0f;
    return sqrtf(power) / num_samples;
}

// ---------------------------------------------------------------------------
// RX: Bit and byte decoding
// ---------------------------------------------------------------------------

bool AcousticModem::decodeBit(const int16_t *symbol_samples,
                               size_t num_samples) {
    float mag_mark = goertzelMagnitude(symbol_samples, num_samples,
                                        _goertzel_coeff_mark);
    float mag_space = goertzelMagnitude(symbol_samples, num_samples,
                                         _goertzel_coeff_space);
    // Mark (1) if mark magnitude > space magnitude
    return mag_mark > mag_space;
}

uint8_t AcousticModem::decodeByte(const int16_t *samples,
                                   size_t samples_per_sym) {
    uint8_t byte = 0;
    for (int b = 7; b >= 0; b--) {
        if (decodeBit(samples, samples_per_sym)) {
            byte |= (1 << b);
        }
        samples += samples_per_sym;
    }
    return byte;
}

// ---------------------------------------------------------------------------
// RX: Preamble and sync detection
// ---------------------------------------------------------------------------

bool AcousticModem::detectPreamble(const int16_t *samples,
                                    size_t num_samples) {
    // Look for alternating mark/space pattern.
    // Check at least 8 consecutive alternating bits.
    uint16_t N = samples_per_symbol();
    if (num_samples < N * 8) return false;

    int transitions = 0;
    bool prev = decodeBit(samples, N);

    for (size_t i = 1; i < 8; i++) {
        bool curr = decodeBit(samples + i * N, N);
        if (curr != prev) transitions++;
        prev = curr;
    }

    // At least 6 out of 7 transitions = preamble detected
    return transitions >= 6;
}

bool AcousticModem::detectSync(const int16_t *samples, size_t num_samples) {
    uint16_t N = samples_per_symbol();
    if (num_samples < N * 8) return false;

    uint8_t byte = decodeByte(samples, N);
    return byte == _config.sync_word;
}

// ---------------------------------------------------------------------------
// RX: Receive
// ---------------------------------------------------------------------------

int AcousticModem::receive(uint8_t *buf, size_t max_len, uint32_t timeout_ms) {
    if (!_initialized || !buf || max_len == 0) return -1;

    uint16_t N = samples_per_symbol();
    uint16_t samples_per_byte = N * 8;

    // We'll read audio in chunks of one symbol at a time, sliding through
    // looking for preamble, then sync, then frame data.

    // Working buffer: enough for preamble detection + one full frame read.
    // For 300 baud at 16kHz: N=53 samples/symbol. Max frame ~1024 bytes =
    // 8192 bits = 8192*53 = ~434K samples. That's a lot of PSRAM.
    // For this prototype, we limit to shorter frames and use a rolling buffer.

    // Rolling buffer: hold enough for preamble + sync + max frame
    // At 300 baud, 16kHz: 1 symbol = 53 samples = 106 bytes
    // 256 byte payload = 2048 bits + overhead ~2200 bits = ~117K samples
    // Use a more modest buffer and read in symbol-sized chunks.

    size_t rx_buf_samples = N * 2; // two symbols of audio for analysis
    int16_t rx_audio[rx_buf_samples];

    // State machine for reception
    _rx_state = AcousticRxState::WAIT_PREAMBLE;

    uint32_t start_ms = 0;
#ifndef SIMULATOR
    start_ms = millis();
#endif

    // Preamble detection: scan symbol-by-symbol
    int preamble_count = 0;
    int required_preamble = 8; // need 8+ alternating bits

    while (_rx_state == AcousticRxState::WAIT_PREAMBLE) {
#ifndef SIMULATOR
        if (millis() - start_ms > timeout_ms) {
            _rx_state = AcousticRxState::IDLE;
            _stats.sync_timeouts++;
            return 0; // timeout
        }
#else
        // Simulator: immediate timeout
        _rx_state = AcousticRxState::IDLE;
        return 0;
#endif

        // Read one symbol of audio
        size_t got = _audio->readMic(rx_audio, N);
        if (got < N) continue;

        // Check energy: is there a tone present at all?
        float mag_mark = goertzelMagnitude(rx_audio, N, _goertzel_coeff_mark);
        float mag_space = goertzelMagnitude(rx_audio, N, _goertzel_coeff_space);
        float energy = mag_mark + mag_space;

        if (energy < _config.detection_threshold) {
            preamble_count = 0;
            continue;
        }

        // Decode this symbol's bit
        bool bit = mag_mark > mag_space;
        static bool last_bit = false;

        if (preamble_count > 0 && bit != last_bit) {
            preamble_count++;
        } else if (preamble_count == 0) {
            preamble_count = 1;
        } else {
            // Same bit twice — might be end of preamble / start of sync
            if (preamble_count >= required_preamble) {
                _rx_state = AcousticRxState::WAIT_SYNC;
                // Re-process this symbol as potential sync byte start
                break;
            }
            preamble_count = 1;
        }
        last_bit = bit;
    }

    // Sync detection: read 8 symbols (1 byte) and check for sync word
    if (_rx_state == AcousticRxState::WAIT_SYNC) {
        int16_t sync_audio[samples_per_byte];
        size_t total_read = 0;

        while (total_read < samples_per_byte) {
#ifndef SIMULATOR
            if (millis() - start_ms > timeout_ms) {
                _rx_state = AcousticRxState::IDLE;
                _stats.sync_timeouts++;
                return 0;
            }
#endif
            size_t got = _audio->readMic(sync_audio + total_read,
                                          samples_per_byte - total_read);
            total_read += got;
        }

        uint8_t sync_byte = decodeByte(sync_audio, N);
        if (sync_byte != _config.sync_word) {
            // Try one more byte in case we're off by a bit
            total_read = 0;
            while (total_read < samples_per_byte) {
#ifndef SIMULATOR
                if (millis() - start_ms > timeout_ms) {
                    _rx_state = AcousticRxState::IDLE;
                    _stats.sync_timeouts++;
                    return 0;
                }
#endif
                size_t got = _audio->readMic(sync_audio + total_read,
                                              samples_per_byte - total_read);
                total_read += got;
            }
            sync_byte = decodeByte(sync_audio, N);
            if (sync_byte != _config.sync_word) {
                _rx_state = AcousticRxState::IDLE;
                _stats.sync_timeouts++;
                return 0;
            }
        }

        _rx_state = AcousticRxState::RECEIVING_DATA;
    }

    // Read frame: length (2 bytes) first
    if (_rx_state != AcousticRxState::RECEIVING_DATA) {
        _rx_state = AcousticRxState::IDLE;
        return -1;
    }

    // Read length field (2 bytes = 16 symbols)
    size_t len_audio_size = samples_per_byte * 2;
    int16_t *len_audio = (int16_t *)malloc(len_audio_size * sizeof(int16_t));
    if (!len_audio) {
        _rx_state = AcousticRxState::IDLE;
        return -1;
    }

    size_t total_read = 0;
    while (total_read < len_audio_size) {
#ifndef SIMULATOR
        if (millis() - start_ms > timeout_ms) {
            free(len_audio);
            _rx_state = AcousticRxState::IDLE;
            _stats.sync_timeouts++;
            return 0;
        }
#endif
        size_t got = _audio->readMic(len_audio + total_read,
                                      len_audio_size - total_read);
        total_read += got;
    }

    uint8_t len_hi = decodeByte(len_audio, N);
    uint8_t len_lo = decodeByte(len_audio + samples_per_byte, N);
    free(len_audio);

    uint16_t payload_len = ((uint16_t)len_hi << 8) | len_lo;

    if (payload_len == 0 || payload_len > MAX_FRAME_PAYLOAD) {
        _rx_state = AcousticRxState::IDLE;
        _stats.crc_errors++;
        return -1;
    }

    // Determine FEC-encoded frame size (payload + CRC = payload_len + 2)
    size_t frame_len = payload_len + 2; // data + CRC
    size_t fec_frame_len = frame_len;
    if (_config.error_correction == AcousticErrorCorrection::REPEAT_3) {
        fec_frame_len = frame_len * 3;
    }

    // Read FEC-encoded data + CRC
    size_t data_audio_size = (size_t)samples_per_byte * fec_frame_len;
    int16_t *data_audio = (int16_t *)malloc(data_audio_size * sizeof(int16_t));
    if (!data_audio) {
        _rx_state = AcousticRxState::IDLE;
        return -1;
    }

    total_read = 0;
    while (total_read < data_audio_size) {
#ifndef SIMULATOR
        if (millis() - start_ms > timeout_ms) {
            free(data_audio);
            _rx_state = AcousticRxState::IDLE;
            _stats.sync_timeouts++;
            return 0;
        }
#endif
        size_t got = _audio->readMic(data_audio + total_read,
                                      data_audio_size - total_read);
        total_read += got;
    }

    // Decode bytes from audio
    uint8_t fec_bytes[fec_frame_len];
    for (size_t i = 0; i < fec_frame_len; i++) {
        fec_bytes[i] = decodeByte(data_audio + i * samples_per_byte, N);
    }
    free(data_audio);

    // Apply FEC decoding
    uint8_t decoded[frame_len];
    size_t decoded_len = decodeFEC(fec_bytes, fec_frame_len,
                                    decoded, sizeof(decoded));
    if (decoded_len != frame_len) {
        _rx_state = AcousticRxState::IDLE;
        _stats.crc_errors++;
        return -1;
    }

    // Verify CRC over [LEN_HI, LEN_LO, DATA...]
    // We need to reconstruct the full CRC input: length + data
    uint8_t crc_input[payload_len + 2];
    crc_input[0] = len_hi;
    crc_input[1] = len_lo;
    memcpy(&crc_input[2], decoded, payload_len);

    uint16_t expected_crc = ((uint16_t)decoded[payload_len] << 8) |
                             decoded[payload_len + 1];
    uint16_t actual_crc = crc16(crc_input, payload_len + 2);

    if (actual_crc != expected_crc) {
        _rx_state = AcousticRxState::IDLE;
        _stats.crc_errors++;
#ifndef SIMULATOR
        Serial.printf("[AMODEM] RX CRC error: expected 0x%04X got 0x%04X\n",
                      expected_crc, actual_crc);
#endif
        return -1;
    }

    // Copy payload to output buffer
    size_t copy_len = payload_len;
    if (copy_len > max_len) copy_len = max_len;
    memcpy(buf, decoded, copy_len);

    _rx_state = AcousticRxState::IDLE;
    _stats.frames_received++;

#ifndef SIMULATOR
    Serial.printf("[AMODEM] RX: %d bytes OK (CRC 0x%04X)\n",
                  (int)copy_len, actual_crc);
#endif

    return (int)copy_len;
}
