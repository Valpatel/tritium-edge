# Acoustic Modem — Data Over Sound

> "A speaker and a microphone are a modem."

## Overview

The acoustic modem transmits and receives digital data as audio tones through the ES8311 codec's speaker and microphone on the ESP32-S3-Touch-LCD-3.5B-C board. It uses Binary FSK (Frequency Shift Keying) — the same modulation used by early telephone modems, radioteletype (RTTY), and AX.25 packet radio.

This is the foundational communication primitive for Tritium: any device with a speaker and a microphone can join the network, with zero configuration and no RF licensing.

## Protocol

### Modulation: Binary FSK

Two carrier frequencies encode binary data:

| Symbol | Meaning | Default Frequency |
|--------|---------|-------------------|
| Mark   | 1       | 1200 Hz           |
| Space  | 0       | 2400 Hz            |

Each bit occupies one symbol period of `1/baud_rate` seconds. At 300 baud, each symbol is 3.33 ms (53 samples at 16 kHz).

The frequencies are chosen to be:
- Well within the ES8311's passband (the codec handles 20 Hz - 8 kHz at 16 kHz sample rate)
- Harmonically related (2:1 ratio) for clean generation
- Far enough apart for reliable Goertzel discrimination
- Low enough to propagate well through small speakers

### Frame Format

```
[SILENCE] [PREAMBLE] [SYNC] [LEN_HI] [LEN_LO] [DATA...] [CRC_HI] [CRC_LO] [SILENCE]
```

| Field     | Size      | Description                                      |
|-----------|-----------|--------------------------------------------------|
| SILENCE   | 1 symbol  | Zero-amplitude settle time for AGC                |
| PREAMBLE  | 32 bits   | Alternating 0/1 for receiver clock synchronization |
| SYNC      | 8 bits    | 0x7E (HDLC flag) marks start of frame             |
| LEN       | 16 bits   | Big-endian payload length (max 1024)               |
| DATA      | N bytes   | Payload (optionally FEC-encoded)                   |
| CRC       | 16 bits   | CRC-16/CCITT over LEN + DATA                      |
| SILENCE   | 1 symbol  | Trailing silence                                   |

### Bit Ordering

MSB first within each byte (network byte order). This matches the convention used by HDLC, AX.25, and most serial protocols.

### CRC-16/CCITT

Polynomial: `0x1021`, initial value: `0xFFFF`. Computed over the length field and data bytes. Transmitted big-endian after the data.

### Error Correction

Three modes:

| Mode       | Overhead | Description                                  |
|------------|----------|----------------------------------------------|
| NONE       | 0%       | CRC16 detection only                          |
| REPEAT_3   | 200%     | Triple-repeat each byte, bitwise majority vote |
| HAMMING_7_4| 75%      | Hamming(7,4) SEC-DED (future)                 |

REPEAT_3 is simple but effective: at 300 baud it reduces throughput to ~100 effective baud but can correct any single-byte error per triple.

## Tone Detection: Goertzel Algorithm

The receiver uses the Goertzel algorithm rather than FFT for tone detection. Goertzel computes the magnitude of a single DFT bin in O(N) time with only 3 multiply-accumulates per sample and no trig functions in the inner loop. For detecting exactly 2 frequencies, this is significantly faster than a full FFT.

```
For each sample x[n]:
    s0 = x[n] + coeff * s1 - s2
    s2 = s1
    s1 = s0

Power = s1^2 + s2^2 - coeff * s1 * s2
```

Where `coeff = 2 * cos(2 * pi * k / N)` is precomputed for each target frequency.

The bit decision is simple: if `magnitude(mark) > magnitude(space)`, the bit is 1.

## Performance Characteristics

### Throughput

| Baud Rate | Raw bps | With REPEAT_3 | 64-byte frame time |
|-----------|---------|---------------|-------------------|
| 300       | 300     | 100           | ~2.0 sec          |
| 600       | 600     | 200           | ~1.0 sec          |
| 1200      | 1200    | 400           | ~0.5 sec          |
| 2400      | 2400    | 800           | ~0.3 sec          |

Frame overhead at 300 baud: preamble (32 bits) + sync (8) + length (16) + CRC (16) = 72 bits = 240 ms.

### Range

Expected range depends heavily on speaker/mic quality and environment:

| Environment        | Estimated Range |
|-------------------|-----------------|
| Quiet room         | 3-10 meters     |
| Noisy room         | 0.5-2 meters    |
| Device-to-device   | 1-5 meters      |
| Through a wall     | Unlikely at 300 baud |

The ES8311's small onboard speaker and MEMS microphone are not optimized for this use case. External speakers/mics could significantly extend range.

### Latency

- TX latency: frame generation is real-time (samples computed as played)
- RX latency: preamble detection + frame reception + decoding
- Round-trip for 32-byte message at 300 baud: ~1.5 sec

## Configuration

```cpp
AcousticModemConfig cfg;
cfg.freq_mark  = 1200;   // Hz, binary 1
cfg.freq_space = 2400;   // Hz, binary 0
cfg.baud_rate  = 300;    // symbols/sec
cfg.preamble_bits = 32;  // sync preamble length
cfg.sync_word = 0x7E;    // frame delimiter
cfg.error_correction = AcousticErrorCorrection::NONE;
cfg.detection_threshold = 0.15f;  // Goertzel energy threshold
cfg.tx_amplitude = 0.8f; // output amplitude (0.0 - 1.0)
```

### Frequency Selection Guide

| Use Case                    | Mark / Space | Baud | Notes                        |
|-----------------------------|-------------|------|------------------------------|
| Maximum reliability         | 1200 / 2400 | 300  | Default. Bell 103 inspired.  |
| Higher throughput           | 2400 / 4800 | 1200 | Needs quiet environment.     |
| Ultrasonic (inaudible)      | 6000 / 7000 | 300  | Near Nyquist at 16 kHz.      |
| Sub-1kHz (wall penetration) | 400 / 800   | 150  | Better propagation, slower.  |

## Use Cases

### 1. Bootstrap / Provisioning
When WiFi and BLE are unavailable or untrusted, the acoustic modem can transmit WiFi credentials, device identity tokens, or network configuration to a new device. Just put the devices near each other and send.

### 2. Covert Channel
Audio signals don't appear on RF spectrum analyzers. An acoustic modem operating at ultrasonic frequencies (6-7 kHz, inaudible to most adults) provides a low-bandwidth side channel that is invisible to standard network monitoring.

### 3. Sensor Relay
Devices in RF-hostile environments (metal enclosures, Faraday cages, underwater housings) can relay sensor readings acoustically. A device inside a sealed enclosure transmits data to one outside.

### 4. Mesh Audio Bridge
Two ESP32 devices can bridge networks acoustically: Device A receives data via WiFi/ESP-NOW, encodes it as audio, Device B decodes and forwards via its own radio. No physical connection needed.

### 5. Cross-Platform Data Transfer
Any device with a speaker can transmit to any device with a microphone. Phone to ESP32, laptop to ESP32, ESP32 to ESP32 — no pairing, no drivers, no protocol negotiation.

## Comparison to Existing Projects

| Project    | Platform    | Modulation    | Max bps | Notes                            |
|------------|------------|---------------|---------|----------------------------------|
| **ggwave** | Multi      | Multi-tone FSK | ~300    | 6 protocols, JS/C++/mobile. Best-in-class but heavy dependency. |
| **minimodem** | Linux CLI | Bell 103/202 FSK | 1200 | Unix philosophy, not embedded.   |
| **chirp.io** | Mobile SDK | Chirp        | ~300    | Proprietary, discontinued.        |
| **quiet** | C library  | OFDM          | ~8000   | Complex, x86-focused.            |
| **This**  | ESP32      | Binary FSK    | ~2400   | Minimal, zero-dependency, runs on MCU. |

Key differentiator: this implementation has zero external dependencies beyond the existing hal_audio HAL. It runs entirely on the ESP32-S3 with no OS, no FFT library, and no floating-point DSP library. The Goertzel algorithm needs only basic arithmetic.

## Future Directions

### Chirp Spread Spectrum (CSS)
Replace fixed FSK tones with linear frequency chirps (like LoRa). Benefits:
- Processing gain against narrowband interference
- Better SNR in noisy environments
- Range extension
- Multiple simultaneous transmissions possible

### Multi-Tone FSK (MFSK)
Use 4, 8, or 16 frequencies to encode 2, 3, or 4 bits per symbol. Doubles or quadruples throughput at the same symbol rate. Requires more Goertzel bins but the algorithm scales linearly.

### Adaptive Rate
Start at 300 baud, measure error rate, and increase speed if the channel is clean. Fall back automatically when errors increase.

### OFDM
Full orthogonal frequency division multiplexing for maximum throughput. Complex to implement on ESP32 but could push throughput to 4-8 kbps.

### Acoustic Mesh
Combine with hal_espnow to create a hybrid RF/acoustic mesh. Devices that can hear each other acoustically form an acoustic subnet; those with ESP-NOW form an RF subnet. Bridge nodes connect the two.

## Hardware Notes

### ES8311 on 3.5B-C
- Sample rate: 16 kHz (Nyquist = 8 kHz)
- Bit depth: 16-bit signed
- I2S: stereo, APLL for accurate MCLK
- MCLK: 4.096 MHz (256 * fs)
- Speaker: small onboard, adequate for same-room communication
- Microphone: MEMS, omnidirectional

### I2S Integration
The modem uses `AudioHAL::playBuffer()` for TX and `AudioHAL::readMic()` for RX. It does not manage I2S directly — all audio I/O goes through hal_audio. This means:
- Audio codec must be initialized before modem init
- Modem TX/RX is blocking (uses the single I2S port)
- Cannot transmit and receive simultaneously (half-duplex)
- Other audio uses (tones, recording) conflict with modem operation

### Memory Usage
- Code: ~4 KB flash
- Stack: ~2 KB during TX/RX (tone generation buffers)
- Heap: RX allocates temporary buffers for frame decoding (proportional to frame size)
- At 300 baud, 256-byte frame: ~110 KB of audio samples buffered during RX
