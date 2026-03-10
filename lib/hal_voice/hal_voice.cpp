#include "hal_voice.h"
#include "hal_audio.h"
#include <cstring>
#include <cmath>

#ifdef SIMULATOR
#include <cstdlib>
static uint32_t millis() { return 0; }
#else
#include "tritium_compat.h"
#endif

// Frame analysis parameters
static constexpr size_t FRAME_SIZE = 512;       // ~32ms at 16kHz
static constexpr size_t FRAME_STRIDE = 256;     // 50% overlap
static constexpr size_t NUM_MEL_BINS = 13;      // Mel filterbank bins
static constexpr float PRE_EMPHASIS = 0.97f;

bool VoiceHAL::init(AudioHAL* audio) {
    if (!audio || !audio->available()) return false;
    _audio = audio;

    // Default VAD config
    _vadConfig.energyThreshold = 0.02f;
    _vadConfig.minSpeechMs = 200;
    _vadConfig.maxSpeechMs = 3000;
    _vadConfig.silenceTimeoutMs = 500;
    _vadConfig.cooldownMs = 1000;

    // Allocate capture buffer in PSRAM if available
#ifndef SIMULATOR
    _captureBuf = (int16_t*)ps_malloc(CAPTURE_BUF_SIZE * sizeof(int16_t));
    if (!_captureBuf) {
        _captureBuf = (int16_t*)malloc(CAPTURE_BUF_SIZE * sizeof(int16_t));
    }
#else
    _captureBuf = (int16_t*)malloc(CAPTURE_BUF_SIZE * sizeof(int16_t));
#endif

    if (!_captureBuf) return false;
    memset(_captureBuf, 0, CAPTURE_BUF_SIZE * sizeof(int16_t));

    _state = VoiceState::IDLE;
    return true;
}

void VoiceHAL::setVADConfig(const VADConfig& config) {
    _vadConfig = config;
}

bool VoiceHAL::addCommand(const char* label, uint8_t commandId) {
    if (_numCommands >= VOICE_MAX_COMMANDS) return false;

    VoiceTemplate& tmpl = _commands[_numCommands];
    tmpl.commandId = commandId;
    strncpy(tmpl.label, label, sizeof(tmpl.label) - 1);
    tmpl.label[sizeof(tmpl.label) - 1] = '\0';
    tmpl.numFrames = 0;
    tmpl.numSamples = 0;
    memset(tmpl.features, 0, sizeof(tmpl.features));

    _numCommands++;
    return true;
}

bool VoiceHAL::trainCommand(uint8_t commandId) {
    // Find the command template
    VoiceTemplate* tmpl = nullptr;
    for (int i = 0; i < _numCommands; i++) {
        if (_commands[i].commandId == commandId) {
            tmpl = &_commands[i];
            break;
        }
    }
    if (!tmpl) return false;

    // Record audio sample
    size_t samplesToRecord = _audio->getSampleRate() * 2;  // 2 seconds
    if (samplesToRecord > CAPTURE_BUF_SIZE) samplesToRecord = CAPTURE_BUF_SIZE;

    if (!_audio->startRecording(_captureBuf, samplesToRecord)) return false;

    // Find speech region using VAD
    size_t speechStart = 0, speechEnd = samplesToRecord;
    float threshold = _vadConfig.energyThreshold * 32768.0f;

    // Find start of speech
    for (size_t i = 0; i < samplesToRecord - FRAME_SIZE; i += FRAME_STRIDE) {
        float energy = 0;
        for (size_t j = 0; j < FRAME_SIZE; j++) {
            float s = _captureBuf[i + j];
            energy += s * s;
        }
        energy = sqrtf(energy / FRAME_SIZE);
        if (energy > threshold * 32768.0f) {
            speechStart = (i > FRAME_STRIDE) ? i - FRAME_STRIDE : 0;
            break;
        }
    }

    // Find end of speech
    for (size_t i = samplesToRecord - FRAME_SIZE; i > speechStart; i -= FRAME_STRIDE) {
        float energy = 0;
        for (size_t j = 0; j < FRAME_SIZE; j++) {
            float s = _captureBuf[i + j];
            energy += s * s;
        }
        energy = sqrtf(energy / FRAME_SIZE);
        if (energy > threshold * 32768.0f) {
            speechEnd = (i + FRAME_SIZE + FRAME_STRIDE < samplesToRecord)
                        ? i + FRAME_SIZE + FRAME_STRIDE : samplesToRecord;
            break;
        }
    }

    // Extract features from speech region
    float newFeatures[VOICE_MAX_FRAMES][VOICE_NUM_FEATURES];
    uint8_t newFrames = 0;
    extractFeatures(_captureBuf + speechStart, speechEnd - speechStart,
                    newFeatures, newFrames);

    if (newFrames == 0) return false;

    // Average with existing template (incremental training)
    if (tmpl->numSamples == 0) {
        memcpy(tmpl->features, newFeatures, sizeof(newFeatures));
        tmpl->numFrames = newFrames;
    } else {
        // Use the frame count from the template with more frames
        uint8_t maxFrames = (newFrames > tmpl->numFrames) ? newFrames : tmpl->numFrames;
        float weight = 1.0f / (tmpl->numSamples + 1);

        for (uint8_t f = 0; f < maxFrames; f++) {
            for (size_t c = 0; c < VOICE_NUM_FEATURES; c++) {
                float oldVal = (f < tmpl->numFrames) ? tmpl->features[f][c] : 0.0f;
                float newVal = (f < newFrames) ? newFeatures[f][c] : 0.0f;
                tmpl->features[f][c] = oldVal * (1.0f - weight) + newVal * weight;
            }
        }
        tmpl->numFrames = maxFrames;
    }
    tmpl->numSamples++;
    return true;
}

bool VoiceHAL::removeCommand(uint8_t commandId) {
    for (int i = 0; i < _numCommands; i++) {
        if (_commands[i].commandId == commandId) {
            // Shift remaining commands
            for (int j = i; j < _numCommands - 1; j++) {
                _commands[j] = _commands[j + 1];
            }
            _numCommands--;
            return true;
        }
    }
    return false;
}

void VoiceHAL::process() {
    if (!_audio || !_audio->available() || !_enabled) return;

    // Read current audio level
    _instantLevel = _audio->getMicLevel();

    switch (_state) {
        case VoiceState::IDLE:       processIdle();      break;
        case VoiceState::LISTENING:  processListening();  break;
        case VoiceState::PROCESSING: processAudio();      break;
        case VoiceState::COMMAND_READY: break;  // Wait for getCommand()
    }
}

void VoiceHAL::processIdle() {
    uint32_t now = millis();

    // Check cooldown
    if (now - _lastDetectionMs < _vadConfig.cooldownMs) return;

    // Check if voice activity detected
    if (_instantLevel > _vadConfig.energyThreshold) {
        _voiceActive = true;
        _speechStartMs = now;
        _lastSpeechMs = now;
        _capturePos = 0;
        setState(VoiceState::LISTENING);
    }
}

void VoiceHAL::processListening() {
    uint32_t now = millis();

    // Read audio into capture buffer
    size_t spaceLeft = CAPTURE_BUF_SIZE - _capturePos;
    if (spaceLeft > 0) {
        size_t toRead = (spaceLeft > 512) ? 512 : spaceLeft;
        size_t read = _audio->readMic(_captureBuf + _capturePos, toRead);
        _capturePos += read;
    }

    // Track voice activity
    if (_instantLevel > _vadConfig.energyThreshold) {
        _lastSpeechMs = now;
        _voiceActive = true;
    } else {
        _voiceActive = false;
    }

    // Check if speech ended (silence timeout)
    bool speechEnded = (now - _lastSpeechMs > _vadConfig.silenceTimeoutMs);

    // Check if max duration exceeded
    bool maxExceeded = (now - _speechStartMs > _vadConfig.maxSpeechMs);

    // Check if buffer full
    bool bufferFull = (_capturePos >= CAPTURE_BUF_SIZE);

    if (speechEnded || maxExceeded || bufferFull) {
        uint32_t speechDuration = now - _speechStartMs;

        // Only process if minimum duration met
        if (speechDuration >= _vadConfig.minSpeechMs && _capturePos > FRAME_SIZE) {
            setState(VoiceState::PROCESSING);
        } else {
            // Too short - discard
            setState(VoiceState::IDLE);
        }
    }
}

void VoiceHAL::processAudio() {
    if (_numCommands == 0 || _capturePos < FRAME_SIZE) {
        setState(VoiceState::IDLE);
        return;
    }

    // Extract features from captured audio
    float capturedFeatures[VOICE_MAX_FRAMES][VOICE_NUM_FEATURES];
    uint8_t capturedFrames = 0;
    extractFeatures(_captureBuf, _capturePos, capturedFeatures, capturedFrames);

    if (capturedFrames == 0) {
        setState(VoiceState::IDLE);
        return;
    }

    // Match against all trained commands
    float bestScore = 0.0f;
    int bestIdx = -1;

    for (int i = 0; i < _numCommands; i++) {
        if (_commands[i].numSamples == 0) continue;  // Untrained command

        float score = matchTemplate(capturedFeatures, capturedFrames, _commands[i]);
        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }

    // Threshold for acceptance (adjustable)
    float acceptThreshold = 0.5f;

    if (bestIdx >= 0 && bestScore > acceptThreshold) {
        _lastDetection.commandId = _commands[bestIdx].commandId;
        _lastDetection.confidence = bestScore;
        _lastDetection.timestamp = millis();
        _lastDetection.label = _commands[bestIdx].label;
        _lastDetectionMs = millis();

        if (_commandCb) {
            _commandCb(_lastDetection);
        }
        setState(VoiceState::COMMAND_READY);
    } else {
        setState(VoiceState::IDLE);
    }
}

VoiceDetection VoiceHAL::getCommand() {
    VoiceDetection det = _lastDetection;
    _lastDetection.commandId = VOICE_CMD_NONE;
    _lastDetection.confidence = 0.0f;
    setState(VoiceState::IDLE);
    return det;
}

void VoiceHAL::setState(VoiceState newState) {
    if (_state != newState) {
        _state = newState;
        if (_stateCb) {
            _stateCb(newState);
        }
    }
}

// ============================================================================
// Feature Extraction
// ============================================================================

void VoiceHAL::extractFeatures(const int16_t* samples, size_t numSamples,
                                float features[][VOICE_NUM_FEATURES],
                                uint8_t& numFrames) {
    numFrames = 0;
    if (!samples || numSamples < FRAME_SIZE) return;

    // Pre-emphasis filter buffer
    float frame[FRAME_SIZE];

    size_t pos = 0;
    while (pos + FRAME_SIZE <= numSamples && numFrames < VOICE_MAX_FRAMES) {
        // Apply pre-emphasis and normalize
        frame[0] = samples[pos] / 32768.0f;
        for (size_t i = 1; i < FRAME_SIZE; i++) {
            frame[i] = (samples[pos + i] - PRE_EMPHASIS * samples[pos + i - 1]) / 32768.0f;
        }

        // Apply Hamming window
        for (size_t i = 0; i < FRAME_SIZE; i++) {
            frame[i] *= 0.54f - 0.46f * cosf(2.0f * M_PI * i / (FRAME_SIZE - 1));
        }

        // Compute power spectrum (magnitude of DFT at mel-spaced frequencies)
        float spectrum[FRAME_SIZE / 2];
        for (size_t k = 0; k < FRAME_SIZE / 2; k++) {
            float realSum = 0.0f, imagSum = 0.0f;
            // Optimize: only compute DFT at frequencies we need
            // Use Goertzel-like selective computation
            for (size_t n = 0; n < FRAME_SIZE; n++) {
                float angle = 2.0f * M_PI * k * n / FRAME_SIZE;
                realSum += frame[n] * cosf(angle);
                imagSum += frame[n] * sinf(angle);
            }
            spectrum[k] = realSum * realSum + imagSum * imagSum;
        }

        // Apply mel filterbank to get MFCC-like features
        computeMelFilterbank(spectrum, FRAME_SIZE / 2,
                             features[numFrames], NUM_MEL_BINS);

        // Apply log compression (standard for MFCCs)
        for (size_t i = 0; i < NUM_MEL_BINS; i++) {
            features[numFrames][i] = logf(features[numFrames][i] + 1e-10f);
        }

        numFrames++;
        pos += FRAME_STRIDE;
    }
}

void VoiceHAL::computeMelFilterbank(const float* spectrum, size_t specLen,
                                     float* melBins, size_t numBins) {
    // Mel scale conversion
    // mel(f) = 2595 * log10(1 + f/700)
    // f(mel) = 700 * (10^(mel/2595) - 1)

    float sampleRate = _audio ? _audio->getSampleRate() : 16000.0f;
    float maxFreq = sampleRate / 2.0f;
    float maxMel = 2595.0f * log10f(1.0f + maxFreq / 700.0f);

    for (size_t b = 0; b < numBins; b++) {
        // Center frequency for this mel bin
        float melLow = maxMel * b / (numBins + 1);
        float melCenter = maxMel * (b + 1) / (numBins + 1);
        float melHigh = maxMel * (b + 2) / (numBins + 1);

        float fLow = 700.0f * (powf(10.0f, melLow / 2595.0f) - 1.0f);
        float fCenter = 700.0f * (powf(10.0f, melCenter / 2595.0f) - 1.0f);
        float fHigh = 700.0f * (powf(10.0f, melHigh / 2595.0f) - 1.0f);

        // Convert to spectrum bin indices
        size_t kLow = (size_t)(fLow / maxFreq * specLen);
        size_t kCenter = (size_t)(fCenter / maxFreq * specLen);
        size_t kHigh = (size_t)(fHigh / maxFreq * specLen);

        if (kLow >= specLen) kLow = specLen - 1;
        if (kCenter >= specLen) kCenter = specLen - 1;
        if (kHigh >= specLen) kHigh = specLen - 1;

        // Triangular filter
        float sum = 0.0f;
        for (size_t k = kLow; k <= kHigh; k++) {
            float weight;
            if (k <= kCenter) {
                weight = (kCenter > kLow) ? (float)(k - kLow) / (kCenter - kLow) : 1.0f;
            } else {
                weight = (kHigh > kCenter) ? (float)(kHigh - k) / (kHigh - kCenter) : 1.0f;
            }
            sum += spectrum[k] * weight;
        }
        melBins[b] = sum;
    }
}

// ============================================================================
// Template Matching (DTW - Dynamic Time Warping)
// ============================================================================

float VoiceHAL::matchTemplate(const float features[][VOICE_NUM_FEATURES],
                               uint8_t numFrames,
                               const VoiceTemplate& tmpl) {
    if (numFrames == 0 || tmpl.numFrames == 0) return 0.0f;

    // Dynamic Time Warping distance
    // Use a cost matrix (allocate on stack for small sizes)
    float cost[VOICE_MAX_FRAMES][VOICE_MAX_FRAMES];

    // Initialize
    for (uint8_t i = 0; i < numFrames; i++) {
        for (uint8_t j = 0; j < tmpl.numFrames; j++) {
            // Euclidean distance between feature vectors
            float dist = 0.0f;
            for (size_t c = 0; c < VOICE_NUM_FEATURES; c++) {
                float diff = features[i][c] - tmpl.features[j][c];
                dist += diff * diff;
            }
            dist = sqrtf(dist);

            if (i == 0 && j == 0) {
                cost[0][0] = dist;
            } else if (i == 0) {
                cost[0][j] = cost[0][j - 1] + dist;
            } else if (j == 0) {
                cost[i][0] = cost[i - 1][0] + dist;
            } else {
                float m = cost[i - 1][j - 1];
                if (cost[i - 1][j] < m) m = cost[i - 1][j];
                if (cost[i][j - 1] < m) m = cost[i][j - 1];
                cost[i][j] = m + dist;
            }
        }
    }

    // Normalize by path length
    float dtwDist = cost[numFrames - 1][tmpl.numFrames - 1];
    float pathLen = (float)(numFrames + tmpl.numFrames);
    float normalizedDist = dtwDist / pathLen;

    // Convert distance to similarity score (0-1)
    // Use exponential decay: score = exp(-dist / scale)
    float scale = 5.0f;  // Adjustable sensitivity
    float score = expf(-normalizedDist / scale);

    return score;
}
