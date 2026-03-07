#include "hal_ota.h"
#include "ota_header.h"
#include "ota_verify.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

static constexpr const char* TAG = "ota";

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

static WebServer* _server = nullptr;
static OtaHAL* _instance = nullptr;

// ---------------------------------------------------------------------------
// Minimal dark-themed HTML upload page
// ---------------------------------------------------------------------------
static const char OTA_UPLOAD_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Firmware Update</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,sans-serif;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#16213e;border-radius:12px;padding:2rem;max-width:420px;
      width:90%;box-shadow:0 4px 24px rgba(0,0,0,.4)}
h1{font-size:1.4rem;margin-bottom:1rem;color:#0f9b8e}
.info{font-size:.85rem;color:#8a8a9a;margin-bottom:1.5rem}
input[type=file]{display:block;width:100%;padding:.6rem;margin-bottom:1rem;
      background:#0f3460;border:1px solid #0f9b8e;border-radius:6px;color:#e0e0e0}
input[type=submit]{width:100%;padding:.7rem;background:#0f9b8e;color:#1a1a2e;
      border:none;border-radius:6px;font-size:1rem;font-weight:700;cursor:pointer}
input[type=submit]:hover{background:#12c4b3}
#prog{width:100%;height:20px;border-radius:6px;margin-top:1rem;display:none}
#msg{margin-top:1rem;font-size:.9rem;text-align:center}
.ok{color:#0f9b8e}.err{color:#e94560}
</style>
</head>
<body>
<div class="card">
<h1>OTA Firmware Update</h1>
<div class="info">Select a .bin firmware file to upload.</div>
<form method="POST" action="/update" enctype="multipart/form-data" id="fm">
<input type="file" name="firmware" accept=".bin" required>
<input type="submit" value="Upload &amp; Flash">
</form>
<progress id="prog" value="0" max="100"></progress>
<div id="msg"></div>
</div>
<script>
document.getElementById('fm').addEventListener('submit',function(e){
  e.preventDefault();
  var f=new FormData(this);
  var x=new XMLHttpRequest();
  var p=document.getElementById('prog');
  var m=document.getElementById('msg');
  p.style.display='block';
  m.textContent='Uploading...';m.className='';
  x.upload.addEventListener('progress',function(ev){
    if(ev.lengthComputable){p.value=Math.round(ev.loaded/ev.total*100);}
  });
  x.onreadystatechange=function(){
    if(x.readyState===4){
      if(x.status===200){m.textContent='Update OK! Rebooting...';m.className='ok';}
      else{m.textContent='Error: '+x.responseText;m.className='err';}
    }
  };
  x.open('POST','/update',true);
  x.send(f);
});
</script>
</body>
</html>
)rawhtml";

// ---------------------------------------------------------------------------
// Private helper methods
// ---------------------------------------------------------------------------
void OtaHAL::_setState(OtaState state, const char* msg) {
    _state = state;
    if (_stateCb) {
        _stateCb(state, msg ? msg : "");
    }
}

void OtaHAL::_setError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(_error, sizeof(_error), fmt, args);
    va_end(args);
    _setState(OtaState::ERROR, _error);
    DBG_ERROR(TAG, "%s", _error);
}

void OtaHAL::_reportProgress(size_t current, size_t total) {
    if (total > 0) {
        _progress = (uint8_t)((current * 100) / total);
    }
    if (_progressCb) {
        _progressCb(current, total);
    }
}

bool OtaHAL::validateHeader(const char* board, const char* version) {
    // Board target verification: reject firmware for a different board
    if (board && board[0] != '\0' && strcmp(board, "any") != 0) {
#ifdef DISPLAY_DRIVER
        // Compare against the board's display driver as an identifier
        if (strcmp(board, DISPLAY_DRIVER) != 0) {
            // Also check the environment/board name
            const char* thisBoard =
#if defined(BOARD_TOUCH_LCD_35BC)
                "touch-lcd-35bc";
#elif defined(BOARD_TOUCH_LCD_349)
                "touch-lcd-349";
#elif defined(BOARD_TOUCH_AMOLED_241B)
                "touch-amoled-241b";
#elif defined(BOARD_AMOLED_191M)
                "amoled-191m";
#elif defined(BOARD_TOUCH_AMOLED_18)
                "touch-amoled-18";
#elif defined(BOARD_TOUCH_LCD_43C_BOX)
                "touch-lcd-43c-box";
#else
                "unknown";
#endif
            if (strcmp(board, thisBoard) != 0) {
                _setError("Board mismatch: firmware for '%s', this is '%s'", board, thisBoard);
                return false;
            }
        }
#endif
    }

#ifdef OTA_ANTI_ROLLBACK
    // Version downgrade protection
    if (version && version[0] != '\0') {
        int cmp = otaVersionCompare(version, _version);
        if (cmp < 0) {
            _setError("Version downgrade rejected: %s < %s", version, _version);
            return false;
        }
        if (cmp == 0) {
            DBG_WARN(TAG, "Same version %s — allowing re-flash", version);
        }
    }
#endif

    return true;
}

// ---------------------------------------------------------------------------
// HTTP server handlers (friend functions for private member access)
// ---------------------------------------------------------------------------
static void handleUpdatePage() {
    _server->sendHeader("Connection", "close");
    _server->send(200, "text/html", OTA_UPLOAD_PAGE);
}

void _otaHandleResult() {
    _server->sendHeader("Connection", "close");
    if (Update.hasError()) {
        _server->send(500, "text/plain", "Update failed");
    } else {
        _server->send(200, "text/plain", "OK");
        if (_instance) {
            _instance->_setState(OtaState::READY_REBOOT, "Update complete, ready to reboot");
            DBG_INFO(TAG, "Push OTA complete, ready to reboot");
        }
    }
}

// Push OTA state for streaming header detection + verification
static struct {
    uint8_t headerBuf[128];
    size_t headerBufLen;
    bool headerParsed;
    bool hasOtaHeader;
    bool isSigned;
    bool isEncrypted;
    bool rejected;       // Set if header validation fails
    size_t headerSize;
    size_t fwSize;
    OtaSignature sig;
    // Encryption state
    uint8_t iv[16];
    size_t ivConsumed;
    bool decryptActive;
} _pushState;

void _otaHandleUpload() {
    HTTPUpload& upload = _server->upload();

    if (upload.status == UPLOAD_FILE_START) {
        DBG_INFO(TAG, "Push OTA start: %s", upload.filename.c_str());
        memset(&_pushState, 0, sizeof(_pushState));
        if (_instance) {
            _instance->_setState(OtaState::WRITING, "Receiving firmware");
            _instance->_progress = 0;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (_pushState.rejected) return;  // Skip processing after rejection

        // Buffer first 128 bytes to detect OTA header
        if (!_pushState.headerParsed) {
            size_t canCopy = sizeof(_pushState.headerBuf) - _pushState.headerBufLen;
            if (canCopy > upload.currentSize) canCopy = upload.currentSize;
            memcpy(_pushState.headerBuf + _pushState.headerBufLen, upload.buf, canCopy);
            _pushState.headerBufLen += canCopy;

            if (_pushState.headerBufLen >= sizeof(_pushState.headerBuf)) {
                _pushState.headerParsed = true;
                OtaFirmwareHeader hdr;
                memcpy(&hdr, _pushState.headerBuf, sizeof(hdr));

                _pushState.hasOtaHeader = hdr.isValid();
                if (_pushState.hasOtaHeader) {
                    _pushState.isSigned = hdr.isSigned();
                    _pushState.isEncrypted = hdr.isEncrypted();
                    _pushState.headerSize = hdr.totalHeaderSize();
                    _pushState.fwSize = hdr.firmware_size;
                    if (_pushState.isSigned) {
                        memcpy(&_pushState.sig, _pushState.headerBuf + sizeof(OtaFirmwareHeader), sizeof(OtaSignature));
                    }
                    DBG_INFO(TAG, "Push OTA header: v%u, fw=%u, signed=%d, encrypted=%d, ver=%s",
                             hdr.header_version, _pushState.fwSize, _pushState.isSigned,
                             _pushState.isEncrypted, hdr.version);

#ifdef OTA_REQUIRE_SIGNATURE
                    if (!_pushState.isSigned) {
                        DBG_ERROR(TAG, "Push OTA rejected: unsigned firmware");
                        _pushState.rejected = true;
                        if (_instance) _instance->_setError("Unsigned firmware rejected");
                        return;
                    }
#endif
                    if (_instance && !_instance->validateHeader(hdr.board, hdr.version)) {
                        _pushState.rejected = true;
                        return;
                    }

                    if (_pushState.isEncrypted && _pushState.fwSize <= 16) {
                        _pushState.rejected = true;
                        if (_instance) _instance->_setError("Encrypted payload too small");
                        return;
                    }

                    size_t flashSize = _pushState.isEncrypted ? _pushState.fwSize - 16 : _pushState.fwSize;
                    if (!Update.begin(flashSize)) {
                        DBG_ERROR(TAG, "Update.begin failed: %s", Update.errorString());
                        _pushState.rejected = true;
                        if (_instance) _instance->_setError("Update.begin failed: %s", Update.errorString());
                        return;
                    }

                    // Start streaming verification
                    OtaVerify::crc32Begin();
                    if (_pushState.isSigned) OtaVerify::beginVerify();

                    // Write any firmware data from the header buffer
                    if (_pushState.headerBufLen > _pushState.headerSize) {
                        size_t extra = _pushState.headerBufLen - _pushState.headerSize;
                        uint8_t* fwStart = _pushState.headerBuf + _pushState.headerSize;

                        // CRC32/signature over ciphertext
                        OtaVerify::crc32Update(fwStart, extra);
                        if (_pushState.isSigned) OtaVerify::updateVerify(fwStart, extra);

                        if (_pushState.isEncrypted) {
                            // Extract IV
                            size_t ivBytes = (extra < 16) ? extra : 16;
                            memcpy(_pushState.iv, fwStart, ivBytes);
                            _pushState.ivConsumed = ivBytes;

                            if (_pushState.ivConsumed >= 16) {
                                if (!OtaVerify::decryptBegin(_pushState.iv)) {
                                    _pushState.rejected = true;
                                    Update.abort();
                                    if (_instance) _instance->_setError("Decryption init failed");
                                    return;
                                }
                                _pushState.decryptActive = true;
                                if (extra > 16) {
                                    OtaVerify::decryptBlock(fwStart + 16, extra - 16);
                                    Update.write(fwStart + 16, extra - 16);
                                }
                            }
                        } else {
                            Update.write(fwStart, extra);
                        }
                    }
                } else {
                    // Raw binary — no OTA header
#ifdef OTA_REQUIRE_SIGNATURE
                    DBG_ERROR(TAG, "Push OTA rejected: raw binary");
                    _pushState.rejected = true;
                    if (_instance) _instance->_setError("Raw firmware rejected");
                    return;
#endif
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                        _pushState.rejected = true;
                        if (_instance) _instance->_setError("Update.begin failed: %s", Update.errorString());
                        return;
                    }
                    Update.write(_pushState.headerBuf, _pushState.headerBufLen);
                }

                // Write remaining data from this chunk
                size_t remaining = upload.currentSize - canCopy;
                if (remaining > 0) {
                    if (_pushState.hasOtaHeader) {
                        OtaVerify::crc32Update(upload.buf + canCopy, remaining);
                        if (_pushState.isSigned) OtaVerify::updateVerify(upload.buf + canCopy, remaining);

                        if (_pushState.isEncrypted) {
                            uint8_t* rPtr = upload.buf + canCopy;
                            size_t rLen = remaining;

                            if (_pushState.ivConsumed < 16) {
                                size_t ivNeeded = 16 - _pushState.ivConsumed;
                                size_t ivBytes = (rLen < ivNeeded) ? rLen : ivNeeded;
                                memcpy(_pushState.iv + _pushState.ivConsumed, rPtr, ivBytes);
                                _pushState.ivConsumed += ivBytes;
                                rPtr += ivBytes;
                                rLen -= ivBytes;
                                if (_pushState.ivConsumed >= 16) {
                                    if (!OtaVerify::decryptBegin(_pushState.iv)) {
                                        _pushState.rejected = true;
                                        Update.abort();
                                        if (_instance) _instance->_setError("Decryption init failed");
                                        return;
                                    }
                                    _pushState.decryptActive = true;
                                }
                            }
                            if (_pushState.decryptActive && rLen > 0) {
                                OtaVerify::decryptBlock(rPtr, rLen);
                                Update.write(rPtr, rLen);
                            }
                        } else {
                            Update.write(upload.buf + canCopy, remaining);
                        }
                    } else {
                        Update.write(upload.buf + canCopy, remaining);
                    }
                }
            }
            // Still buffering header — don't write yet
        } else {
            // Header already parsed, stream firmware data
            if (_pushState.hasOtaHeader) {
                // CRC32/signature over raw (possibly encrypted) data
                OtaVerify::crc32Update(upload.buf, upload.currentSize);
                if (_pushState.isSigned) OtaVerify::updateVerify(upload.buf, upload.currentSize);
            }

            if (_pushState.isEncrypted) {
                uint8_t* dataPtr = upload.buf;
                size_t dataLen = upload.currentSize;

                // Still extracting IV?
                if (_pushState.ivConsumed < 16) {
                    size_t ivNeeded = 16 - _pushState.ivConsumed;
                    size_t ivBytes = (dataLen < ivNeeded) ? dataLen : ivNeeded;
                    memcpy(_pushState.iv + _pushState.ivConsumed, dataPtr, ivBytes);
                    _pushState.ivConsumed += ivBytes;
                    dataPtr += ivBytes;
                    dataLen -= ivBytes;

                    if (_pushState.ivConsumed >= 16) {
                        if (!OtaVerify::decryptBegin(_pushState.iv)) {
                            _pushState.rejected = true;
                            Update.abort();
                            if (_instance) _instance->_setError("Decryption init failed");
                            return;
                        }
                        _pushState.decryptActive = true;
                    }
                }

                if (_pushState.decryptActive && dataLen > 0) {
                    OtaVerify::decryptBlock(dataPtr, dataLen);
                    if (Update.write(dataPtr, dataLen) != dataLen) {
                        DBG_ERROR(TAG, "Update.write failed: %s", Update.errorString());
                        if (_instance) _instance->_setError("Update.write failed: %s", Update.errorString());
                    }
                }
            } else {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    DBG_ERROR(TAG, "Update.write failed: %s", Update.errorString());
                    if (_instance) _instance->_setError("Update.write failed: %s", Update.errorString());
                }
            }

            if (_instance) {
                size_t written = Update.progress();
                size_t total = _pushState.fwSize > 0 ? _pushState.fwSize : Update.size();
                _instance->_reportProgress(written, total > 0 ? total : written);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (_pushState.decryptActive) OtaVerify::decryptEnd();
        if (_pushState.rejected) {
            Update.abort();
            return;
        }
        if (_instance) {
            _instance->_setState(OtaState::VERIFYING, "Verifying firmware");
        }

        // Verify CRC32 and signature for OTA-headered uploads
        if (_pushState.hasOtaHeader) {
            OtaFirmwareHeader hdr;
            memcpy(&hdr, _pushState.headerBuf, sizeof(hdr));
            uint32_t actualCrc = OtaVerify::crc32Finalize();
            if (actualCrc != hdr.firmware_crc32) {
                DBG_ERROR(TAG, "Push OTA CRC mismatch: expected 0x%08X, got 0x%08X",
                         hdr.firmware_crc32, actualCrc);
                Update.abort();
                if (_instance) _instance->_setError("CRC32 mismatch");
                return;
            }
            DBG_INFO(TAG, "Push OTA CRC32 OK: 0x%08X", actualCrc);

            if (_pushState.isSigned) {
                bool sigOk = OtaVerify::finalizeVerify(_pushState.sig.r, _pushState.sig.s);
                if (!sigOk) {
                    DBG_ERROR(TAG, "Push OTA signature INVALID");
                    Update.abort();
                    if (_instance) _instance->_setError("ECDSA signature verification FAILED");
                    return;
                }
                DBG_INFO(TAG, "Push OTA ECDSA signature verified OK");
            }
        }

        if (Update.end(true)) {
            DBG_INFO(TAG, "Push OTA success, %u bytes", upload.totalSize);
        } else {
            DBG_ERROR(TAG, "Update.end failed: %s", Update.errorString());
            if (_instance) {
                _instance->_setError("Update.end failed: %s", Update.errorString());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// OtaHAL implementation
// ---------------------------------------------------------------------------
bool OtaHAL::init() {
    _instance = this;
    _state = OtaState::IDLE;
    _progress = 0;
    _error[0] = '\0';

    // Read version from running app description
    const esp_app_desc_t* desc = esp_ota_get_app_description();
    if (desc) {
        snprintf(_version, sizeof(_version), "%s", desc->version);
    } else {
        strncpy(_version, "unknown", sizeof(_version));
    }

    DBG_INFO(TAG, "OTA HAL initialized, firmware version: %s", _version);
    return true;
}

bool OtaHAL::startServer(uint16_t port) {
    if (_serverRunning) {
        DBG_WARN(TAG, "OTA server already running");
        return true;
    }

    if (_server) {
        delete _server;
        _server = nullptr;
    }

    _server = new WebServer(port);
    _server->on("/update", HTTP_GET, handleUpdatePage);
    _server->on("/update", HTTP_POST, _otaHandleResult, _otaHandleUpload);
    _server->begin();
    _serverRunning = true;

    DBG_INFO(TAG, "OTA HTTP server started on port %u", port);
    return true;
}

void OtaHAL::stopServer() {
    if (_server) {
        _server->stop();
        delete _server;
        _server = nullptr;
    }
    _serverRunning = false;
    DBG_INFO(TAG, "OTA HTTP server stopped");
}

bool OtaHAL::isServerRunning() const {
    return _serverRunning;
}

bool OtaHAL::updateFromUrl(const char* url) {
    if (!url || strlen(url) == 0) {
        _setError("Invalid URL");
        return false;
    }

    DBG_INFO(TAG, "Pull OTA from URL: %s", url);
    _setState(OtaState::DOWNLOADING, "Downloading firmware");

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        _setError("HTTP GET failed, code: %d", httpCode);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        _setError("Invalid content length: %d", contentLength);
        http.end();
        return false;
    }

    DBG_INFO(TAG, "Download size: %d bytes", contentLength);

    WiFiClient* stream = http.getStreamPtr();

    // Read first 128 bytes to check for OTA header
    uint8_t headerBuf[128];
    size_t headerRead = 0;
    while (headerRead < sizeof(headerBuf) && http.connected()) {
        size_t avail = stream->available();
        if (avail) {
            size_t toRead = ((sizeof(headerBuf) - headerRead) < avail) ?
                            (sizeof(headerBuf) - headerRead) : avail;
            size_t n = stream->readBytes(headerBuf + headerRead, toRead);
            headerRead += n;
        }
        delay(1);
    }

    OtaFirmwareHeader hdr;
    OtaSignature sig = {};
    memcpy(&hdr, headerBuf, sizeof(hdr));

    bool hasOtaHeader = hdr.isValid();
    bool isSigned = hasOtaHeader && hdr.isSigned();
    size_t headerSize = 0;
    size_t fwSize = 0;

    bool isEncrypted = false;

    if (hasOtaHeader) {
        headerSize = hdr.totalHeaderSize();
        fwSize = hdr.firmware_size;
        isEncrypted = hdr.isEncrypted();
        DBG_INFO(TAG, "OTA header: v%u, fw=%u bytes, signed=%d, encrypted=%d, ver=%s",
                 hdr.header_version, fwSize, isSigned, isEncrypted, hdr.version);

        if (isSigned) {
            memcpy(&sig, headerBuf + sizeof(OtaFirmwareHeader), sizeof(sig));
        }

#ifdef OTA_REQUIRE_SIGNATURE
        if (!isSigned) {
            _setError("Firmware not signed (OTA_REQUIRE_SIGNATURE)");
            http.end();
            return false;
        }
#endif

        // Validate board target and version
        if (!validateHeader(hdr.board, hdr.version)) {
            http.end();
            return false;
        }

        // Verify firmware size vs content length
        if (fwSize + headerSize != (size_t)contentLength) {
            _setError("Size mismatch: hdr says %u+%u, got %d", fwSize, headerSize, contentLength);
            http.end();
            return false;
        }

        // Encrypted payload: first 16 bytes are IV, rest is ciphertext
        if (isEncrypted && fwSize <= 16) {
            _setError("Encrypted payload too small: %u bytes", fwSize);
            http.end();
            return false;
        }
    } else {
        // Raw binary — no header
        fwSize = contentLength;
        DBG_INFO(TAG, "Raw firmware (no OTA header), %u bytes", fwSize);

#ifdef OTA_REQUIRE_SIGNATURE
        _setError("Raw firmware rejected (OTA_REQUIRE_SIGNATURE)");
        http.end();
        return false;
#endif
    }

    // Sanity check firmware size
    static constexpr size_t MIN_FW_SIZE = 32768;  // 32KB minimum
    size_t flashSize = isEncrypted ? fwSize - 16 : fwSize;  // Subtract IV for encrypted
    if (flashSize < MIN_FW_SIZE) {
        _setError("Firmware too small: %u bytes (min %u)", flashSize, MIN_FW_SIZE);
        http.end();
        return false;
    }

    if (!Update.begin(flashSize)) {
        _setError("Not enough space: %s", Update.errorString());
        http.end();
        return false;
    }

    _setState(OtaState::WRITING, "Writing firmware to flash");

    // Start streaming verification
    OtaVerify::crc32Begin();
    bool doSigVerify = isSigned;
    if (doSigVerify) {
        // Signature covers firmware data only (not header)
        OtaVerify::beginVerify();
    }

    uint8_t buf[1024];
    size_t written = 0;       // Bytes of payload consumed (including IV for encrypted)
    size_t flashWritten = 0;  // Bytes written to flash (excludes IV)

    // For encrypted firmware, we need to extract the 16-byte IV first
    uint8_t iv[16];
    bool decryptActive = false;
    size_t ivConsumed = 0;  // How many IV bytes we've extracted so far

    // Write any firmware data already buffered in headerBuf
    if (hasOtaHeader && headerRead > headerSize) {
        size_t extra = headerRead - headerSize;
        uint8_t* fwStart = headerBuf + headerSize;

        OtaVerify::crc32Update(fwStart, extra);
        if (doSigVerify) OtaVerify::updateVerify(fwStart, extra);
        written += extra;

        if (isEncrypted) {
            // Extract IV from the start of the payload
            size_t ivBytes = (extra < 16) ? extra : 16;
            memcpy(iv, fwStart, ivBytes);
            ivConsumed = ivBytes;

            if (ivConsumed >= 16) {
                if (!OtaVerify::decryptBegin(iv)) {
                    _setError("Decryption init failed");
                    Update.abort();
                    http.end();
                    return false;
                }
                decryptActive = true;
                // Write any data beyond IV
                if (extra > 16) {
                    size_t dataLen = extra - 16;
                    OtaVerify::decryptBlock(fwStart + 16, dataLen);
                    size_t w = Update.write(fwStart + 16, dataLen);
                    if (w != dataLen) {
                        _setError("Write failed at header overflow");
                        Update.abort();
                        http.end();
                        OtaVerify::decryptEnd();
                        return false;
                    }
                    flashWritten += w;
                }
            }
        } else {
            size_t w = Update.write(fwStart, extra);
            if (w != extra) {
                _setError("Write failed at header overflow");
                Update.abort();
                http.end();
                return false;
            }
            flashWritten += w;
        }
    } else if (!hasOtaHeader) {
        // Raw binary — write the header bytes as firmware
        size_t w = Update.write(headerBuf, headerRead);
        if (w != headerRead) {
            _setError("Write failed at start");
            Update.abort();
            http.end();
            return false;
        }
        OtaVerify::crc32Update(headerBuf, headerRead);
        written += headerRead;
        flashWritten += w;
    }

    // Stream remaining firmware data (with stall timeout)
    static constexpr uint32_t STALL_TIMEOUT_MS = 30000;  // 30s stall timeout
    uint32_t lastDataMs = millis();
    while (http.connected() && written < fwSize) {
        size_t available = stream->available();
        if (available) {
            lastDataMs = millis();
            size_t toRead = (available < sizeof(buf)) ? available : sizeof(buf);
            if (toRead > fwSize - written) toRead = fwSize - written;
            size_t bytesRead = stream->readBytes(buf, toRead);

            // CRC32 and signature cover the ciphertext (encrypt-then-sign)
            OtaVerify::crc32Update(buf, bytesRead);
            if (doSigVerify) OtaVerify::updateVerify(buf, bytesRead);
            written += bytesRead;

            if (isEncrypted) {
                uint8_t* dataPtr = buf;
                size_t dataLen = bytesRead;

                // Still extracting IV?
                if (ivConsumed < 16) {
                    size_t ivNeeded = 16 - ivConsumed;
                    size_t ivBytes = (dataLen < ivNeeded) ? dataLen : ivNeeded;
                    memcpy(iv + ivConsumed, dataPtr, ivBytes);
                    ivConsumed += ivBytes;
                    dataPtr += ivBytes;
                    dataLen -= ivBytes;

                    if (ivConsumed >= 16) {
                        if (!OtaVerify::decryptBegin(iv)) {
                            _setError("Decryption init failed");
                            Update.abort();
                            http.end();
                            return false;
                        }
                        decryptActive = true;
                    }
                }

                // Decrypt and write remaining data
                if (decryptActive && dataLen > 0) {
                    OtaVerify::decryptBlock(dataPtr, dataLen);
                    size_t w = Update.write(dataPtr, dataLen);
                    if (w != dataLen) {
                        _setError("Write failed at %u: %s", flashWritten, Update.errorString());
                        Update.abort();
                        http.end();
                        OtaVerify::decryptEnd();
                        return false;
                    }
                    flashWritten += w;
                }
            } else {
                size_t bytesWritten = Update.write(buf, bytesRead);
                if (bytesWritten != bytesRead) {
                    _setError("Write failed at %u: %s", flashWritten, Update.errorString());
                    Update.abort();
                    http.end();
                    return false;
                }
                flashWritten += bytesWritten;
            }
            _reportProgress(written, fwSize);
        } else if (millis() - lastDataMs > STALL_TIMEOUT_MS) {
            _setError("Download stalled at %u/%u bytes", written, fwSize);
            Update.abort();
            http.end();
            if (decryptActive) OtaVerify::decryptEnd();
            return false;
        }
        delay(1);
    }

    if (decryptActive) OtaVerify::decryptEnd();

    if (written < fwSize) {
        _setError("Download incomplete: %u/%u bytes", written, fwSize);
        Update.abort();
        http.end();
        return false;
    }

    http.end();

    _setState(OtaState::VERIFYING, "Verifying firmware");

    // CRC32 verification
    if (hasOtaHeader) {
        uint32_t actualCrc = OtaVerify::crc32Finalize();
        if (actualCrc != hdr.firmware_crc32) {
            _setError("CRC32 mismatch: expected 0x%08X, got 0x%08X",
                      hdr.firmware_crc32, actualCrc);
            Update.abort();
            return false;
        }
        DBG_INFO(TAG, "CRC32 OK: 0x%08X", actualCrc);
    }

    // Signature verification
    if (doSigVerify) {
        bool sigOk = OtaVerify::finalizeVerify(sig.r, sig.s);
        if (!sigOk) {
            _setError("ECDSA signature verification FAILED");
            Update.abort();
            return false;
        }
        DBG_INFO(TAG, "ECDSA signature verified OK");
    }

    if (!Update.end(true)) {
        _setError("Flash verification failed: %s", Update.errorString());
        return false;
    }

    _setState(OtaState::READY_REBOOT, "Update complete, ready to reboot");
    DBG_INFO(TAG, "Pull OTA complete: %u bytes written, signed=%d, encrypted=%d",
             flashWritten, doSigVerify, isEncrypted);
    return true;
}

bool OtaHAL::updateFromSD(const char* path) {
    if (!path || strlen(path) == 0) {
        _setError("Invalid SD path");
        return false;
    }

    DBG_INFO(TAG, "SD OTA from: %s", path);
    _setState(OtaState::CHECKING, "Checking SD card");

    if (!SD_MMC.begin()) {
        _setError("SD card mount failed");
        return false;
    }

    File firmware = SD_MMC.open(path, FILE_READ);
    if (!firmware) {
        _setError("Cannot open %s", path);
        return false;
    }

    size_t fileSize = firmware.size();
    if (fileSize == 0) {
        _setError("Firmware file is empty");
        firmware.close();
        return false;
    }

    DBG_INFO(TAG, "SD file size: %u bytes", fileSize);

    // Read first 128 bytes to check for OTA header
    uint8_t headerBuf[128];
    size_t headerRead = firmware.read(headerBuf, sizeof(headerBuf));

    OtaFirmwareHeader hdr;
    OtaSignature sig = {};
    memcpy(&hdr, headerBuf, sizeof(hdr));

    bool hasOtaHeader = hdr.isValid();
    bool isSigned = hasOtaHeader && hdr.isSigned();
    size_t headerSize = 0;
    size_t fwSize = 0;

    bool isEncrypted = false;

    if (hasOtaHeader) {
        headerSize = hdr.totalHeaderSize();
        fwSize = hdr.firmware_size;
        isEncrypted = hdr.isEncrypted();
        DBG_INFO(TAG, "OTA header: v%u, fw=%u bytes, signed=%d, encrypted=%d, ver=%s",
                 hdr.header_version, fwSize, isSigned, isEncrypted, hdr.version);

        if (isSigned) {
            memcpy(&sig, headerBuf + sizeof(OtaFirmwareHeader), sizeof(sig));
        }

#ifdef OTA_REQUIRE_SIGNATURE
        if (!isSigned) {
            _setError("SD firmware not signed (OTA_REQUIRE_SIGNATURE)");
            firmware.close();
            return false;
        }
#endif

        // Validate board target and version
        if (!validateHeader(hdr.board, hdr.version)) {
            firmware.close();
            return false;
        }

        if (fwSize + headerSize != fileSize) {
            _setError("SD size mismatch: hdr %u+%u != file %u", fwSize, headerSize, fileSize);
            firmware.close();
            return false;
        }

        if (isEncrypted && fwSize <= 16) {
            _setError("Encrypted payload too small: %u bytes", fwSize);
            firmware.close();
            return false;
        }

        // Seek to start of firmware data (payload)
        firmware.seek(headerSize);
    } else {
        // Raw binary
        fwSize = fileSize;
        firmware.seek(0);
        DBG_INFO(TAG, "Raw firmware (no OTA header), %u bytes", fwSize);

#ifdef OTA_REQUIRE_SIGNATURE
        _setError("SD raw firmware rejected (OTA_REQUIRE_SIGNATURE)");
        firmware.close();
        return false;
#endif
    }

    // Sanity check firmware size
    static constexpr size_t MIN_FW_SIZE = 32768;  // 32KB minimum
    size_t flashSize = isEncrypted ? fwSize - 16 : fwSize;
    if (flashSize < MIN_FW_SIZE) {
        _setError("Firmware too small: %u bytes (min %u)", flashSize, MIN_FW_SIZE);
        firmware.close();
        return false;
    }

    if (!Update.begin(flashSize)) {
        _setError("Not enough space: %s", Update.errorString());
        firmware.close();
        return false;
    }

    _setState(OtaState::WRITING, "Writing firmware from SD");

    OtaVerify::crc32Begin();
    bool doSigVerify = isSigned;
    if (doSigVerify) {
        OtaVerify::beginVerify();
    }

    // For encrypted firmware, read IV first
    bool decryptActive = false;
    if (isEncrypted) {
        uint8_t iv[16];
        size_t ivRead = firmware.read(iv, 16);
        if (ivRead != 16) {
            _setError("Failed to read encryption IV");
            Update.abort();
            firmware.close();
            return false;
        }
        // CRC32/signature cover the IV too (it's part of the payload)
        OtaVerify::crc32Update(iv, 16);
        if (doSigVerify) OtaVerify::updateVerify(iv, 16);

        if (!OtaVerify::decryptBegin(iv)) {
            _setError("Decryption init failed");
            Update.abort();
            firmware.close();
            return false;
        }
        decryptActive = true;
        DBG_INFO(TAG, "AES-256-CTR decryption started, IV=%02x%02x%02x%02x...",
                 iv[0], iv[1], iv[2], iv[3]);
    }

    uint8_t buf[1024];
    size_t payloadRead = isEncrypted ? 16 : 0;  // IV already consumed
    size_t flashWritten = 0;

    while (payloadRead < fwSize && firmware.available()) {
        size_t toRead = fwSize - payloadRead;
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        size_t bytesRead = firmware.read(buf, toRead);

        // CRC32 and signature cover ciphertext (encrypt-then-sign)
        OtaVerify::crc32Update(buf, bytesRead);
        if (doSigVerify) OtaVerify::updateVerify(buf, bytesRead);
        payloadRead += bytesRead;

        // Decrypt in-place before writing to flash
        if (decryptActive) {
            OtaVerify::decryptBlock(buf, bytesRead);
        }

        size_t bytesWritten = Update.write(buf, bytesRead);
        if (bytesWritten != bytesRead) {
            _setError("Write failed at %u: %s", flashWritten, Update.errorString());
            Update.abort();
            firmware.close();
            if (decryptActive) OtaVerify::decryptEnd();
            return false;
        }
        flashWritten += bytesWritten;
        _reportProgress(payloadRead, fwSize);
    }

    if (decryptActive) OtaVerify::decryptEnd();

    firmware.close();

    _setState(OtaState::VERIFYING, "Verifying firmware");

    // CRC32 verification
    if (hasOtaHeader) {
        uint32_t actualCrc = OtaVerify::crc32Finalize();
        if (actualCrc != hdr.firmware_crc32) {
            _setError("CRC32 mismatch: expected 0x%08X, got 0x%08X",
                      hdr.firmware_crc32, actualCrc);
            Update.abort();
            return false;
        }
        DBG_INFO(TAG, "CRC32 OK: 0x%08X", actualCrc);
    }

    // Signature verification
    if (doSigVerify) {
        bool sigOk = OtaVerify::finalizeVerify(sig.r, sig.s);
        if (!sigOk) {
            _setError("ECDSA signature verification FAILED");
            Update.abort();
            return false;
        }
        DBG_INFO(TAG, "ECDSA signature verified OK");
    }

    if (!Update.end(true)) {
        _setError("Flash verification failed: %s", Update.errorString());
        return false;
    }

    // Rename firmware to .bak
    char bakPath[128];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
    SD_MMC.remove(bakPath);
    SD_MMC.rename(path, bakPath);
    DBG_INFO(TAG, "Renamed %s -> %s", path, bakPath);

    _setState(OtaState::READY_REBOOT, "SD update complete, ready to reboot");
    DBG_INFO(TAG, "SD OTA complete: %u bytes written, signed=%d, encrypted=%d",
             flashWritten, doSigVerify, isEncrypted);
    return true;
}

OtaState OtaHAL::getState() const {
    return _state;
}

uint8_t OtaHAL::getProgress() const {
    return _progress;
}

const char* OtaHAL::getLastError() const {
    return _error;
}

const char* OtaHAL::getCurrentVersion() const {
    return _version;
}

void OtaHAL::onProgress(OtaProgressCb cb) {
    _progressCb = cb;
}

void OtaHAL::onStateChange(OtaStateCb cb) {
    _stateCb = cb;
}

void OtaHAL::process() {
    if (_serverRunning && _server) {
        _server->handleClient();
    }
}

void OtaHAL::reboot() {
    DBG_INFO(TAG, "Rebooting...");
    delay(200);
    ESP.restart();
}

bool OtaHAL::confirmApp() {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        DBG_INFO(TAG, "App confirmed valid (rollback cancelled)");
        return true;
    }
    DBG_WARN(TAG, "confirmApp: esp_err 0x%x (may already be confirmed)", err);
    return err == ESP_OK;
}

bool OtaHAL::rollback() {
    const esp_partition_t* prev = esp_ota_get_last_invalid_partition();
    if (!prev) {
        prev = esp_ota_get_next_update_partition(nullptr);
    }
    if (!prev) {
        _setError("No partition available for rollback");
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(prev);
    if (err != ESP_OK) {
        _setError("Rollback failed: esp_err 0x%x", err);
        return false;
    }

    DBG_INFO(TAG, "Rollback set to partition: %s", prev->label);
    _setState(OtaState::READY_REBOOT, "Rollback ready, reboot to apply");
    return true;
}

bool OtaHAL::canRollback() const {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!running || !next) return false;
    // Can rollback if there is a different OTA partition available
    return (running != next);
}

const char* OtaHAL::getRunningPartition() const {
    const esp_partition_t* part = esp_ota_get_running_partition();
    return part ? part->label : "unknown";
}

size_t OtaHAL::getMaxFirmwareSize() const {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return part ? part->size : 0;
}

bool OtaHAL::getFirmwareHash(char* out, size_t outLen) const {
    if (!out || outLen < 65) return false;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return false;

    // Get actual firmware size from app description
    const esp_app_desc_t* desc = esp_ota_get_app_description();
    // Use partition size as upper bound if desc unavailable
    size_t fwSize = running->size;

    // Stream SHA-256 over partition data in 4KB chunks
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t buf[4096];
    size_t offset = 0;
    while (offset < fwSize) {
        size_t toRead = (fwSize - offset < sizeof(buf)) ? fwSize - offset : sizeof(buf);
        if (esp_partition_read(running, offset, buf, toRead) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        mbedtls_sha256_update(&ctx, buf, toRead);
        offset += toRead;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    for (int i = 0; i < 32; i++) {
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    }
    return true;
}

OtaHAL::TestResult OtaHAL::runTest() {
    TestResult result = {};
    uint32_t startMs = millis();

    DBG_INFO(TAG, "--- OTA Test Begin ---");

    // Check dual OTA partitions
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    result.partition_ok = (running != nullptr && next != nullptr && running != next);
    result.running_partition = running ? running->label : "none";
    result.max_firmware_size = next ? next->size : 0;

    DBG_INFO(TAG, "Running partition: %s", result.running_partition);
    DBG_INFO(TAG, "Next OTA partition: %s (%u bytes)",
             next ? next->label : "none", result.max_firmware_size);
    DBG_INFO(TAG, "Dual OTA partitions: %s", result.partition_ok ? "YES" : "NO");

    // Test server start/stop
    bool wasRunning = _serverRunning;
    result.server_start_ok = startServer(8079);  // Use non-default port for test
    if (result.server_start_ok) {
        DBG_INFO(TAG, "Server start: OK");
        stopServer();
        result.server_stop_ok = !_serverRunning;
        DBG_INFO(TAG, "Server stop: %s", result.server_stop_ok ? "OK" : "FAIL");
    } else {
        DBG_ERROR(TAG, "Server start: FAIL");
        result.server_stop_ok = false;
    }

    // Restore previous server state if it was running
    if (wasRunning) {
        startServer();
    }

    // Test rollback API
    result.rollback_check_ok = (running != nullptr);
    DBG_INFO(TAG, "Rollback API: %s, canRollback: %s",
             result.rollback_check_ok ? "OK" : "FAIL",
             canRollback() ? "yes" : "no");

    // Test SD card access
    if (SD_MMC.begin()) {
        result.sd_detect_ok = true;
        DBG_INFO(TAG, "SD card: detected (%llu MB total)",
                 SD_MMC.totalBytes() / (1024 * 1024));
    } else {
        result.sd_detect_ok = false;
        DBG_INFO(TAG, "SD card: not detected");
    }

    result.test_duration_ms = millis() - startMs;
    DBG_INFO(TAG, "--- OTA Test End (%u ms) ---", result.test_duration_ms);

    return result;
}

// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#else // SIMULATOR

void OtaHAL::_setState(OtaState, const char*) {}
void OtaHAL::_setError(const char*, ...) {}
void OtaHAL::_reportProgress(size_t, size_t) {}

bool OtaHAL::init() {
    DBG_INFO(TAG, "OTA HAL init (simulator stub)");
    strncpy(_version, "sim-0.0.0", sizeof(_version));
    return false;
}

bool OtaHAL::startServer(uint16_t) { return false; }
void OtaHAL::stopServer() {}
bool OtaHAL::isServerRunning() const { return false; }
bool OtaHAL::updateFromUrl(const char*) { return false; }
bool OtaHAL::updateFromSD(const char*) { return false; }

OtaState OtaHAL::getState() const { return OtaState::IDLE; }
uint8_t OtaHAL::getProgress() const { return 0; }
const char* OtaHAL::getLastError() const { return _error; }
const char* OtaHAL::getCurrentVersion() const { return _version; }

void OtaHAL::onProgress(OtaProgressCb cb) { _progressCb = cb; }
void OtaHAL::onStateChange(OtaStateCb cb) { _stateCb = cb; }
void OtaHAL::process() {}
void OtaHAL::reboot() {}
bool OtaHAL::confirmApp() { return true; }
bool OtaHAL::rollback() { return false; }
bool OtaHAL::canRollback() const { return false; }
const char* OtaHAL::getRunningPartition() const { return "simulator"; }
size_t OtaHAL::getMaxFirmwareSize() const { return 0; }
bool OtaHAL::getFirmwareHash(char*, size_t) const { return false; }

OtaHAL::TestResult OtaHAL::runTest() {
    TestResult r = {};
    r.running_partition = "simulator";
    return r;
}

#endif // SIMULATOR
