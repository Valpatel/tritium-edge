#pragma once
// Acoustic Modem HAL — FSK data over speaker/microphone
//
// "A speaker and a microphone are a modem."
//
// Uses Binary FSK (Frequency Shift Keying) to encode data as audio tones.
// Transmits via I2S speaker, receives via I2S microphone. Goertzel algorithm
// for efficient single-frequency detection on the receive path.
//
// Usage:
//   #include "hal_acoustic_modem.h"
//   #include "hal_audio.h"
//
//   AudioHAL audio;
//   audio.initLgfx(0, 0x18);
//
//   AcousticModem modem;
//   AcousticModemConfig cfg;       // defaults: 1200/2400 Hz, 300 baud
//   modem.init(audio, cfg);
//
//   // Transmit
//   modem.send((const uint8_t*)"hello", 5);
//
//   // Receive
//   uint8_t buf[256];
//   int len = modem.receive(buf, sizeof(buf), 5000);

#include <cstdint>
#include <cstddef>

class AudioHAL;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

enum class AcousticErrorCorrection : uint8_t {
    NONE = 0,       // CRC16 only (detect errors)
    REPEAT_3 = 1,   // Triple-repeat each byte, majority vote (1/3 throughput)
    HAMMING_7_4 = 2 // Hamming(7,4) — 4 data bits per 7 transmitted (future)
};

struct AcousticModemConfig {
    // FSK frequency pair (Hz). Must be below Nyquist (sample_rate / 2).
    // Default: Bell 103 inspired — 1200 Hz = mark (1), 2400 Hz = space (0)
    uint16_t freq_mark  = 1200;   // binary 1
    uint16_t freq_space = 2400;   // binary 0

    // Baud rate: symbols per second. Each symbol = 1 bit for BFSK.
    // Higher = faster but less robust. 300 is very reliable over speaker.
    // Supported: 50, 75, 110, 150, 300, 600, 1200, 2400
    uint16_t baud_rate = 300;

    // Preamble: alternating 0/1 bits for receiver clock sync.
    // Length in bits. Minimum 16, recommended 32-64.
    uint16_t preamble_bits = 32;

    // Sync word after preamble — fixed pattern to mark frame start.
    // Default: 0x7E (HDLC flag)
    uint8_t sync_word = 0x7E;

    // Error correction level
    AcousticErrorCorrection error_correction = AcousticErrorCorrection::NONE;

    // Goertzel detection threshold (0.0-1.0). Lower = more sensitive.
    // Tune based on environment noise floor.
    float detection_threshold = 0.15f;

    // TX amplitude (0.0-1.0). Scales the output waveform.
    float tx_amplitude = 0.8f;
};

// ---------------------------------------------------------------------------
// Frame statistics
// ---------------------------------------------------------------------------

struct AcousticModemStats {
    uint32_t frames_sent = 0;
    uint32_t frames_received = 0;
    uint32_t crc_errors = 0;
    uint32_t sync_timeouts = 0;
    float last_snr_db = 0.0f;    // signal-to-noise of last reception
};

// ---------------------------------------------------------------------------
// Receive state (internal, exposed for is_receiving)
// ---------------------------------------------------------------------------

enum class AcousticRxState : uint8_t {
    IDLE = 0,
    WAIT_PREAMBLE,
    WAIT_SYNC,
    RECEIVING_DATA
};

// ---------------------------------------------------------------------------
// AcousticModem
// ---------------------------------------------------------------------------

class AcousticModem {
public:
    // Initialize modem. Requires an already-initialized AudioHAL.
    // Returns false if audio is not available.
    bool init(AudioHAL &audio, const AcousticModemConfig &config = {});

    // --- Transmit ---

    // Encode data as FSK audio and play through speaker.
    // Blocks until transmission is complete.
    // Returns number of bytes sent, or -1 on error.
    int send(const uint8_t *data, size_t len);

    // --- Receive ---

    // Listen for an incoming FSK frame.
    // Blocks up to timeout_ms. Returns number of bytes decoded into buf,
    // or 0 if timeout, or -1 on error (CRC fail, etc).
    int receive(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

    // Check if receiver is currently mid-frame.
    bool is_receiving() const { return _rx_state != AcousticRxState::IDLE; }

    // --- Info ---

    bool available() const { return _initialized; }
    const AcousticModemConfig &config() const { return _config; }
    const AcousticModemStats &stats() const { return _stats; }

    // Effective data throughput in bits/sec (accounting for overhead)
    float throughput_bps() const;

    // Duration of a single symbol in microseconds
    uint32_t symbol_duration_us() const;

    // Number of audio samples per symbol at current sample rate
    uint16_t samples_per_symbol() const;

private:
    bool _initialized = false;
    AudioHAL *_audio = nullptr;
    AcousticModemConfig _config;
    AcousticModemStats _stats;
    AcousticRxState _rx_state = AcousticRxState::IDLE;

    // Sample rate from AudioHAL (typically 16000)
    uint32_t _sample_rate = 16000;

    // Precomputed Goertzel coefficients for mark/space detection
    float _goertzel_coeff_mark = 0.0f;
    float _goertzel_coeff_space = 0.0f;

    // --- TX helpers ---
    void generateTone(int16_t *buf, size_t num_samples, uint16_t freq,
                      float amplitude, uint32_t &phase_acc);
    void transmitBit(bool bit, uint32_t &phase_acc);
    void transmitByte(uint8_t byte, uint32_t &phase_acc);
    void transmitPreamble(uint32_t &phase_acc);
    void transmitSync(uint32_t &phase_acc);

    // --- RX helpers ---
    float goertzelMagnitude(const int16_t *samples, size_t num_samples,
                            float coeff);
    bool decodeBit(const int16_t *symbol_samples, size_t num_samples);
    uint8_t decodeByte(const int16_t *samples, size_t samples_per_sym);
    bool detectPreamble(const int16_t *samples, size_t num_samples);
    bool detectSync(const int16_t *samples, size_t num_samples);

    // --- Error correction ---
    static uint16_t crc16(const uint8_t *data, size_t len);
    size_t applyFEC(const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t out_max);
    size_t decodeFEC(const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_max);
};
