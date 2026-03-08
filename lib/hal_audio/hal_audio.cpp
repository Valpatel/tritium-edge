#include "hal_audio.h"

#ifdef SIMULATOR

// --- Simulator stubs ---

bool AudioHAL::init() {
    _initialized = true;
    _has_codec = true;
    _has_mic = true;
    _has_speaker = true;
    return true;
}

bool AudioHAL::setVolume(uint8_t volume) { _volume = volume; return true; }
bool AudioHAL::playTone(uint16_t freqHz, uint32_t durationMs) { return _initialized; }
bool AudioHAL::playBuffer(const int16_t* data, size_t samples) { return _initialized; }
bool AudioHAL::setSpeakerEnabled(bool enabled) { return _initialized; }
bool AudioHAL::startRecording(int16_t *buffer, size_t samples) { return false; }
size_t AudioHAL::readMic(int16_t *buffer, size_t maxSamples) { return 0; }
bool AudioHAL::setMicGain(uint8_t gain) { return _initialized; }
float AudioHAL::getMicLevel() { return 0.0f; }
bool AudioHAL::getSpectrum(float* bins, size_t numBins, const int16_t* samples, size_t numSamples) { return false; }

#else // ESP32

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#if __has_include(<lgfx/v1/platforms/common.hpp>)
#include <lgfx/v1/platforms/common.hpp>
#define HAS_LGFX_I2C 1
#else
#define HAS_LGFX_I2C 0
#endif
#include <math.h>

#ifndef HAS_AUDIO_CODEC
#define HAS_AUDIO_CODEC 0
#endif

// ES8311 Register Map
#define ES8311_REG00_RESET       0x00
#define ES8311_REG01_CLK_MGR     0x01
#define ES8311_REG02_CLK_MGR     0x02
#define ES8311_REG03_CLK_MGR     0x03
#define ES8311_REG04_CLK_MGR     0x04
#define ES8311_REG05_CLK_MGR     0x05
#define ES8311_REG06_CLK_MGR     0x06
#define ES8311_REG07_CLK_MGR     0x07
#define ES8311_REG08_CLK_MGR     0x08
#define ES8311_REG09_SDP_IN      0x09
#define ES8311_REG0A_SDP_OUT     0x0A
#define ES8311_REG0B_SYSTEM      0x0B
#define ES8311_REG0C_SYSTEM      0x0C
#define ES8311_REG0D_SYSTEM      0x0D
#define ES8311_REG0E_SYSTEM      0x0E
#define ES8311_REG0F_ADC         0x0F
#define ES8311_REG10_ADC         0x10
#define ES8311_REG11_ADC         0x11
#define ES8311_REG12_ADC         0x12
#define ES8311_REG13_ADC         0x13
#define ES8311_REG14_ADC         0x14
#define ES8311_REG15_DAC         0x15
#define ES8311_REG16_DAC         0x16
#define ES8311_REG17_ADC_VOL     0x17
#define ES8311_REG18_ADC_CTRL    0x18
#define ES8311_REG32_DAC_VOL     0x32
#define ES8311_REG37_DAC_CTRL    0x37
#define ES8311_REGFD_CHIPID1     0xFD
#define ES8311_REGFE_CHIPID2     0xFE
#define ES8311_REGFF_FLAG        0xFF

#define I2S_SAMPLE_RATE          16000
#define I2S_DMA_BUF_COUNT        4
#define I2S_DMA_BUF_LEN          512

bool AudioHAL::init(TwoWire &wire) {
#if !HAS_AUDIO_CODEC
    return false;
#else
    _wire = &wire;
    _use_lgfx = false;
    _i2s_port = 0;

#if defined(AUDIO_CODEC_ADDR)
    _codec_addr = AUDIO_CODEC_ADDR;
#else
    _codec_addr = 0x18;
#endif

    if (!initES8311()) return false;
    _has_codec = true;
    if (!initI2S()) return false;
    _has_mic = true;
    _has_speaker = true;
    _initialized = true;
    return true;
#endif
}

bool AudioHAL::initLgfx(uint8_t i2c_port, uint8_t addr) {
#if !HAS_AUDIO_CODEC || !HAS_LGFX_I2C
    return false;
#else
    _use_lgfx = true;
    _lgfx_port = i2c_port;
    _codec_addr = addr;
    _wire = nullptr;
    _i2s_port = 0;

    if (!initES8311()) return false;
    _has_codec = true;
    if (!initI2S()) return false;
    _has_mic = true;
    _has_speaker = true;
    _initialized = true;
    return true;
#endif
}

bool AudioHAL::initES8311() {
    // Check chip ID
    uint8_t id1 = readReg(ES8311_REGFD_CHIPID1);
    uint8_t id2 = readReg(ES8311_REGFE_CHIPID2);
    Serial.printf("[AUDIO] ES8311 chip ID: 0x%02X 0x%02X\n", id1, id2);
    if (id1 != 0x83 || id2 != 0x11) {
        Serial.println("[AUDIO] ES8311 chip ID mismatch");
    }

    // Init sequence based on Espressif ESP-ADF es8311.c reference driver
    // https://github.com/espressif/esp-adf/blob/master/components/audio_hal/driver/es8311/es8311.c

    // Enhance I2C noise immunity (written twice per ESP-ADF)
    writeReg(0x44, 0x08);
    writeReg(0x44, 0x08);

    // Initial clock and power sequence
    writeReg(ES8311_REG01_CLK_MGR, 0x30);  // MCLK from pin
    writeReg(ES8311_REG02_CLK_MGR, 0x00);  // No pre-divider/multiplier yet
    writeReg(ES8311_REG03_CLK_MGR, 0x10);  // ADC OSR = 16 (single speed)
    writeReg(ES8311_REG16_DAC, 0x24);      // DAC config
    writeReg(ES8311_REG04_CLK_MGR, 0x10);  // DAC OSR = 16
    writeReg(ES8311_REG05_CLK_MGR, 0x00);  // ADC/DAC clock div = 1

    // Power up analog blocks
    writeReg(ES8311_REG0B_SYSTEM, 0x00);   // Power up analog
    writeReg(ES8311_REG0C_SYSTEM, 0x00);   // Power up
    writeReg(0x10, 0x1F);                  // Power up: VMIDSEL + reference power
    writeReg(0x11, 0x7F);                  // Power up: all analog blocks

    // Enable CSM (Codec State Machine) - CRITICAL for ADC/DAC to work
    writeReg(ES8311_REG00_RESET, 0x80);
    delay(50);

    // Slave mode: clear bit 6 of reg 0x00
    uint8_t reg00 = readReg(ES8311_REG00_RESET);
    reg00 &= 0xBF;  // Slave mode (ESP32 is I2S master)
    writeReg(ES8311_REG00_RESET, reg00);

    // Enable all clocks: MCLK from pin, all clock enables on
    writeReg(ES8311_REG01_CLK_MGR, 0x3F);

    // Clock coefficients for MCLK=4.096MHz, fs=16kHz (from coeff_div table)
    // {4096000, 16000, pre_div=1, mult=x1, adc_div=1, dac_div=1,
    //  fs_mode=0, lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x20}
    writeReg(ES8311_REG02_CLK_MGR, 0x00);  // pre_div=1, mult=x1
    writeReg(ES8311_REG05_CLK_MGR, 0x00);  // adc_div=1, dac_div=1
    writeReg(ES8311_REG03_CLK_MGR, 0x10);  // fs_mode=0, adc_osr=0x10
    writeReg(ES8311_REG04_CLK_MGR, 0x10);  // dac_osr=0x10
    writeReg(ES8311_REG07_CLK_MGR, 0x00);  // LRCK divider high
    writeReg(ES8311_REG08_CLK_MGR, 0xFF);  // LRCK divider low

    // BCLK divider
    uint8_t reg06 = readReg(ES8311_REG06_CLK_MGR);
    reg06 &= ~0x20;  // Don't invert SCLK
    writeReg(ES8311_REG06_CLK_MGR, reg06);

    // I2S format: 16-bit, I2S Philips standard
    writeReg(ES8311_REG09_SDP_IN,  0x0C);  // DAC SDP: I2S, 16-bit
    writeReg(ES8311_REG0A_SDP_OUT, 0x0C);  // ADC SDP: I2S, 16-bit

    // System registers
    writeReg(ES8311_REG0D_SYSTEM, 0x01);   // Enable DAC and ADC
    writeReg(ES8311_REG0E_SYSTEM, 0x02);   // Reference circuit enable
    writeReg(0x13, 0x10);                  // ADC power reference

    // ADC configuration (microphone input)
    writeReg(ES8311_REG0F_ADC, 0x00);      // ADC normal operation
    writeReg(ES8311_REG10_ADC, 0x1C);      // ADC: single-ended, PGA gain +24dB
    writeReg(0x1B, 0x0A);                  // ADC equalizer/filter config
    writeReg(0x1C, 0x6A);                  // ADC equalizer/filter config

    // ADC volume
    writeReg(ES8311_REG17_ADC_VOL, 0xBF);  // ADC volume ~75%

    // DAC configuration (speaker output)
    writeReg(ES8311_REG15_DAC, 0x00);      // DAC normal operation

    // DAC volume
    writeReg(ES8311_REG32_DAC_VOL, 0xBF);  // DAC volume ~75%

    Serial.println("[AUDIO] ES8311 init complete (CSM enabled)");
    return true;
}

bool AudioHAL::initI2S() {
#if defined(AUDIO_I2S_BCLK)
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    i2s_config.sample_rate = I2S_SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = I2S_DMA_BUF_COUNT;
    i2s_config.dma_buf_len = I2S_DMA_BUF_LEN;
    i2s_config.use_apll = true;   // APLL for accurate MCLK
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = I2S_SAMPLE_RATE * 256;  // MCLK = 256 * fs

    if (i2s_driver_install((i2s_port_t)_i2s_port, &i2s_config, 0, NULL) != ESP_OK) {
        Serial.println("[AUDIO] I2S driver install failed");
        return false;
    }

    i2s_pin_config_t pin_config = {};
#if defined(AUDIO_I2S_MCLK) && AUDIO_I2S_MCLK >= 0
    pin_config.mck_io_num = AUDIO_I2S_MCLK;
#else
    pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
    pin_config.bck_io_num   = AUDIO_I2S_BCLK;
    pin_config.ws_io_num    = AUDIO_I2S_WS;
#if defined(AUDIO_I2S_DOUT) && AUDIO_I2S_DOUT >= 0
    pin_config.data_out_num = AUDIO_I2S_DOUT;
#else
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
#endif
#if defined(AUDIO_I2S_DIN) && AUDIO_I2S_DIN >= 0
    pin_config.data_in_num  = AUDIO_I2S_DIN;
#else
    pin_config.data_in_num  = I2S_PIN_NO_CHANGE;
#endif

    if (i2s_set_pin((i2s_port_t)_i2s_port, &pin_config) != ESP_OK) {
        Serial.println("[AUDIO] I2S pin config failed");
        i2s_driver_uninstall((i2s_port_t)_i2s_port);
        return false;
    }

    _sample_rate = I2S_SAMPLE_RATE;
    Serial.printf("[AUDIO] I2S initialized: %d Hz, MCLK=%d\n",
                  I2S_SAMPLE_RATE, I2S_SAMPLE_RATE * 256);
    return true;
#else
    return false;
#endif
}

bool AudioHAL::setVolume(uint8_t volume) {
    if (!_has_codec) return false;
    if (volume > 100) volume = 100;
    _volume = volume;
    // ES8311 DAC volume: 0x00=mute, 0xFF=max
    uint8_t reg_val = (volume * 255) / 100;
    writeReg(ES8311_REG32_DAC_VOL, reg_val);
    return true;
}

bool AudioHAL::setMicGain(uint8_t gain) {
    if (!_has_codec) return false;
    if (gain > 100) gain = 100;
    // ES8311 ADC volume: 0x00=mute, 0xFF=max
    uint8_t reg_val = (gain * 255) / 100;
    writeReg(ES8311_REG17_ADC_VOL, reg_val);
    return true;
}

bool AudioHAL::setSpeakerEnabled(bool enabled) {
#if defined(AUDIO_PA_EN) && AUDIO_PA_EN >= 0
    ::pinMode(AUDIO_PA_EN, OUTPUT);
    ::digitalWrite(AUDIO_PA_EN, enabled ? HIGH : LOW);
    return true;
#else
    // No PA enable pin - speaker always on when codec active
    return _has_speaker;
#endif
}

bool AudioHAL::playTone(uint16_t freqHz, uint32_t durationMs) {
    if (!_initialized) return false;
    const size_t bufLen = 256;
    int16_t buf[bufLen * 2]; // stereo
    uint32_t totalSamples = (_sample_rate * durationMs) / 1000;
    uint32_t written = 0;

    // Scale amplitude by volume
    float amplitude = 16000.0f * (_volume / 100.0f);

    while (written < totalSamples) {
        size_t chunk = (totalSamples - written < bufLen) ? (totalSamples - written) : bufLen;
        for (size_t i = 0; i < chunk; i++) {
            int16_t sample = (int16_t)(amplitude *
                sinf(2.0f * M_PI * freqHz * (written + i) / (float)_sample_rate));
            buf[i * 2]     = sample; // Left
            buf[i * 2 + 1] = sample; // Right
        }
        size_t bytesWritten = 0;
        i2s_write((i2s_port_t)_i2s_port, buf, chunk * 4, &bytesWritten, portMAX_DELAY);
        written += chunk;
    }
    return true;
}

bool AudioHAL::playBuffer(const int16_t* data, size_t samples) {
    if (!_initialized || !data) return false;
    size_t bytesWritten = 0;
    esp_err_t err = i2s_write((i2s_port_t)_i2s_port, data, samples * sizeof(int16_t),
                              &bytesWritten, portMAX_DELAY);
    return err == ESP_OK;
}

size_t AudioHAL::readMic(int16_t *buffer, size_t maxSamples) {
    if (!_has_mic || !buffer) return 0;
    size_t bytesRead = 0;
    // Read stereo (L+R) data, maxSamples in mono = maxSamples*2 stereo samples
    i2s_read((i2s_port_t)_i2s_port, buffer, maxSamples * sizeof(int16_t),
             &bytesRead, pdMS_TO_TICKS(100));
    return bytesRead / sizeof(int16_t);
}

bool AudioHAL::startRecording(int16_t *buffer, size_t samples) {
    if (!_has_mic || !buffer) return false;
    size_t bytesRead = 0;
    esp_err_t err = i2s_read((i2s_port_t)_i2s_port, buffer, samples * sizeof(int16_t),
                             &bytesRead, portMAX_DELAY);
    return err == ESP_OK;
}

float AudioHAL::getMicLevel() {
    if (!_has_mic) return 0.0f;

    // Read a small chunk and compute RMS
    static int16_t levelBuf[256];
    size_t read = readMic(levelBuf, 256);
    if (read == 0) return 0.0f;

    float sum = 0.0f;
    for (size_t i = 0; i < read; i++) {
        float s = levelBuf[i] / 32768.0f;
        sum += s * s;
    }
    float rms = sqrtf(sum / read);
    // Clamp to 0..1
    if (rms > 1.0f) rms = 1.0f;
    return rms;
}

bool AudioHAL::getSpectrum(float* bins, size_t numBins, const int16_t* samples, size_t numSamples) {
    if (!bins || !samples || numBins == 0 || numSamples == 0) return false;

    // Goertzel algorithm — efficient single-frequency DFT per bin
    // Much faster than full DFT: O(numSamples) per bin with no trig in inner loop
    float maxFreq = _sample_rate / 2.0f;
    float binWidth = maxFreq / numBins;

    for (size_t b = 0; b < numBins; b++) {
        float freq = (b + 0.5f) * binWidth;
        float k = freq / _sample_rate * numSamples;
        float w = 2.0f * M_PI * k / numSamples;
        float coeff = 2.0f * cosf(w);
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;

        for (size_t i = 0; i < numSamples; i++) {
            s0 = (samples[i] / 32768.0f) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        float magnitude = sqrtf(s1 * s1 + s2 * s2 - coeff * s1 * s2) / numSamples;
        bins[b] = magnitude;
    }
    return true;
}

void AudioHAL::writeReg(uint8_t reg, uint8_t val) {
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        uint8_t buf[2] = { reg, val };
        lgfx::i2c::transactionWrite(_lgfx_port, _codec_addr, buf, 2, 400000);
    } else
#endif
    {
        _wire->beginTransmission(_codec_addr);
        _wire->write(reg);
        _wire->write(val);
        _wire->endTransmission();
    }
}

uint8_t AudioHAL::readReg(uint8_t reg) {
    uint8_t val = 0;
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        lgfx::i2c::transactionWriteRead(_lgfx_port, _codec_addr,
            &reg, 1, &val, 1, 400000);
    } else
#endif
    {
        _wire->beginTransmission(_codec_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_codec_addr, (uint8_t)1);
        if (_wire->available()) val = _wire->read();
    }
    return val;
}

#endif // SIMULATOR
