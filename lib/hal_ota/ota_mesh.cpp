#include "ota_mesh.h"
#include "ota_verify.h"
#include "hal_espnow.h"
#include "debug_log.h"
#include <cstring>

static constexpr const char* TAG = "ota_mesh";

#ifndef SIMULATOR

#include "tritium_compat.h"
#include "esp_ota_ops.h"
#include <esp_partition.h>
#include <esp_random.h>
#include <sys/stat.h>

// ESP-IDF OTA state for mesh receiver
static esp_ota_handle_t _mesh_ota_handle = 0;
static const esp_partition_t* _mesh_ota_partition = nullptr;

bool OtaMesh::init(EspNowHAL* espnow) {
    if (!espnow || !espnow->isReady()) {
        DBG_ERROR(TAG, "ESP-NOW not ready");
        return false;
    }
    _espnow = espnow;
    memset(&_tx, 0, sizeof(_tx));
    memset(&_rx, 0, sizeof(_rx));
    memset(&_stats, 0, sizeof(_stats));

    // Register mesh data callback for OTA messages
    _espnow->onMeshReceive([this](const uint8_t* mac, const uint8_t* data, uint8_t len, uint8_t hops) {
        if (len >= 1 && data[0] >= (uint8_t)OtaMeshType::ANNOUNCE && data[0] <= (uint8_t)OtaMeshType::ABORT) {
            handleMeshData(mac, data, len, hops);
        }
    });

    DBG_INFO(TAG, "Mesh OTA initialized");
    return true;
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

bool OtaMesh::startSend(const char* otaFilePath) {
    if (_sending) {
        DBG_WARN(TAG, "Already sending");
        return false;
    }

    char sdpath[128];
    snprintf(sdpath, sizeof(sdpath), "/sdcard%s", otaFilePath);
    FILE* f = fopen(sdpath, "rb");
    if (!f) {
        DBG_ERROR(TAG, "Cannot open %s", otaFilePath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize < 64) {
        DBG_ERROR(TAG, "File too small");
        fclose(f);
        return false;
    }

    // Read header
    uint8_t headerBuf[128];
    fread(headerBuf, 1, sizeof(headerBuf), f);
    fclose(f);

    OtaFirmwareHeader hdr;
    memcpy(&hdr, headerBuf, sizeof(hdr));
    if (!hdr.isValid()) {
        DBG_ERROR(TAG, "Invalid OTA header in %s", otaFilePath);
        return false;
    }

    _tx.hdr = hdr;
    _tx.isSigned = hdr.isSigned();
    _tx.headerSize = hdr.totalHeaderSize();
    if (_tx.isSigned) {
        memcpy(&_tx.sig, headerBuf + sizeof(OtaFirmwareHeader), sizeof(OtaSignature));
    }

    // We'll read chunks from SD on demand
    _tx.fromBuffer = false;
    _tx.bufData = nullptr;
    _tx.bufLen = 0;
    _tx.totalChunks = (hdr.firmware_size + OTA_MESH_CHUNK_SIZE - 1) / OTA_MESH_CHUNK_SIZE;
    _tx.nextChunk = 0;
    _tx.ackedChunk = 0;
    _tx.retryCount = 0;
    _tx.hasTarget = false;
    _tx.session_id = (uint16_t)(esp_random() & 0xFFFF);

    _sending = true;
    DBG_INFO(TAG, "Mesh OTA send: v%s, %u bytes, %u chunks, session 0x%04X",
             hdr.version, hdr.firmware_size, _tx.totalChunks, _tx.session_id);

    sendAnnounce();
    return true;
}

bool OtaMesh::startSendFromBuffer(const uint8_t* data, size_t len) {
    if (_sending) return false;
    if (len < 64) return false;

    OtaFirmwareHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (!hdr.isValid()) return false;

    _tx.hdr = hdr;
    _tx.isSigned = hdr.isSigned();
    _tx.headerSize = hdr.totalHeaderSize();
    if (_tx.isSigned) {
        memcpy(&_tx.sig, data + sizeof(OtaFirmwareHeader), sizeof(OtaSignature));
    }

    _tx.fromBuffer = true;
    _tx.bufData = data;
    _tx.bufLen = len;
    _tx.totalChunks = (hdr.firmware_size + OTA_MESH_CHUNK_SIZE - 1) / OTA_MESH_CHUNK_SIZE;
    _tx.nextChunk = 0;
    _tx.ackedChunk = 0;
    _tx.retryCount = 0;
    _tx.hasTarget = false;
    _tx.session_id = (uint16_t)(esp_random() & 0xFFFF);

    _sending = true;
    sendAnnounce();
    return true;
}

void OtaMesh::stopSend() {
    if (_sending) {
        sendAbort();
        _sending = false;
        DBG_INFO(TAG, "Mesh OTA send stopped");
    }
}

void OtaMesh::enableReceive(bool enable) {
    _receiving = enable;
    if (!enable && _transferActive) {
        abortTransfer("Receive disabled");
    }
    DBG_INFO(TAG, "Mesh OTA receive: %s", enable ? "enabled" : "disabled");
}

// ---------------------------------------------------------------------------
// Process loop
// ---------------------------------------------------------------------------

void OtaMesh::process() {
    if (!_espnow) return;
    uint32_t now = millis();

    // Sender: periodic announce if no target yet
    if (_sending && !_tx.hasTarget) {
        if (now - _tx.lastSendMs > 5000) {
            sendAnnounce();
        }
    }

    // Sender: retransmit on ACK timeout
    if (_sending && _tx.hasTarget && _tx.nextChunk > _tx.ackedChunk) {
        if (now - _tx.lastSendMs > OTA_MESH_ACK_TIMEOUT_MS) {
            _tx.retryCount++;
            if (_tx.retryCount > OTA_MESH_MAX_RETRIES) {
                DBG_ERROR(TAG, "Mesh OTA send: max retries, aborting");
                sendAbort();
                _sending = false;
                _stats.transfers_failed++;
                return;
            }
            DBG_WARN(TAG, "Retransmit chunk %u (retry %u)", _tx.ackedChunk, _tx.retryCount);
            _tx.nextChunk = _tx.ackedChunk;
            sendChunk(_tx.nextChunk);
            _stats.retransmits++;
        }
    }

    // Receiver: chunk timeout
    if (_transferActive) {
        if (now - _rx.lastChunkMs > OTA_MESH_CHUNK_TIMEOUT_MS) {
            DBG_WARN(TAG, "Chunk timeout at seq %u", _rx.nextExpected);
            // Request retransmit
            sendAck(_rx.nextExpected);
        }
    }
}

// ---------------------------------------------------------------------------
// Mesh data handler
// ---------------------------------------------------------------------------

void OtaMesh::handleMeshData(const uint8_t* mac, const uint8_t* data, uint8_t len, uint8_t hops) {
    if (len < 1) return;
    OtaMeshType type = (OtaMeshType)data[0];

    switch (type) {
        case OtaMeshType::ANNOUNCE:
            if (_receiving && !_transferActive) {
                if (len >= sizeof(OtaMeshAnnounceSig) && len < sizeof(OtaMeshAnnounce)) {
                    // Could be signature part
                    handleAnnounceSig(mac, data, len);
                } else {
                    handleAnnounce(mac, data, len);
                }
            }
            break;
        case OtaMeshType::CHUNK:
            if (_transferActive) handleChunk(mac, data, len);
            break;
        case OtaMeshType::ACK:
            if (_sending) handleAck(mac, data, len);
            break;
        case OtaMeshType::REQUEST:
            if (_sending) handleRequest(mac, data, len);
            break;
        case OtaMeshType::STATUS:
            if (_sending) handleStatus(mac, data, len);
            break;
        case OtaMeshType::ABORT:
            handleAbort(mac, data, len);
            break;
        default:
            break;
    }
}

void OtaMesh::handleAnnounce(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshAnnounce)) return;
    const OtaMeshAnnounce* ann = (const OtaMeshAnnounce*)data;

    _stats.announces_heard++;
    DBG_INFO(TAG, "OTA announce: v%s, %u bytes, %u chunks, session 0x%04X",
             ann->version, ann->firmware_size, ann->total_chunks, ann->session_id);

    // Check board target
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

    if (strcmp(ann->board, "any") != 0 && strcmp(ann->board, thisBoard) != 0) {
        DBG_INFO(TAG, "Board mismatch: %s vs %s, ignoring", ann->board, thisBoard);
        return;
    }

#ifdef OTA_REQUIRE_SIGNATURE
    if (!(ann->flags & 0x01)) {
        DBG_WARN(TAG, "Unsigned firmware rejected");
        return;
    }
#endif

    // Accept this transfer
    memset(&_rx.hdr, 0, sizeof(OtaFirmwareHeader));
    _rx.hdr.magic = OtaFirmwareHeader::MAGIC;
    _rx.hdr.header_version = ann->header_version;
    _rx.hdr.flags = ann->flags;
    _rx.hdr.firmware_size = ann->firmware_size;
    _rx.hdr.firmware_crc32 = ann->firmware_crc32;
    memcpy(_rx.hdr.version, ann->version, 24);
    memcpy(_rx.hdr.board, ann->board, 16);

    _rx.session_id = ann->session_id;
    _rx.isSigned = (ann->header_version == OtaFirmwareHeader::HEADER_VER_SIGNED) && (ann->flags & 0x01);
    _rx.isEncrypted = (ann->flags & 0x02) != 0;
    _rx.totalChunks = ann->total_chunks;
    _rx.nextExpected = 0;
    _rx.bytesReceived = 0;
    _rx.flashStarted = false;
    memcpy(_rx.senderMac, mac, 6);
    memset(&_rx.sig, 0, sizeof(_rx.sig));

    if (_rx.isSigned) {
        // Wait for signature announce part before starting
        DBG_INFO(TAG, "Waiting for signature part...");
        _transferActive = true;
        _rx.lastChunkMs = millis();
    } else {
        // Start receiving immediately
        _transferActive = true;
        _rx.lastChunkMs = millis();

        // Send REQUEST for chunk 0
        OtaMeshAck req;
        req.type = OtaMeshType::REQUEST;
        req.session_id = _rx.session_id;
        req.next_seq = 0;
        _espnow->meshSend(_rx.senderMac, (const uint8_t*)&req, sizeof(req));
        DBG_INFO(TAG, "Requested chunk 0 from sender");
    }
}

void OtaMesh::handleAnnounceSig(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshAnnounceSig)) return;
    const OtaMeshAnnounceSig* sigPkt = (const OtaMeshAnnounceSig*)data;

    if (!_transferActive || sigPkt->session_id != _rx.session_id) return;
    if (sigPkt->part != 2) return;

    memcpy(_rx.sig.r, sigPkt->sig_r, 32);
    memcpy(_rx.sig.s, sigPkt->sig_s, 32);
    DBG_INFO(TAG, "Got signature, requesting chunk 0");

    OtaMeshAck req;
    req.type = OtaMeshType::REQUEST;
    req.session_id = _rx.session_id;
    req.next_seq = 0;
    _espnow->meshSend(_rx.senderMac, (const uint8_t*)&req, sizeof(req));
}

void OtaMesh::handleChunk(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshChunkHeader)) return;
    const OtaMeshChunkHeader* chk = (const OtaMeshChunkHeader*)data;

    if (chk->session_id != _rx.session_id) return;
    if (chk->seq != _rx.nextExpected) {
        // Out of order — re-request expected chunk
        if (chk->seq > _rx.nextExpected) {
            sendAck(_rx.nextExpected);
        }
        return;
    }

    const uint8_t* chunkData = data + sizeof(OtaMeshChunkHeader);
    size_t chunkLen = chk->len;
    if (sizeof(OtaMeshChunkHeader) + chunkLen > len) return;  // Truncated

    _rx.lastChunkMs = millis();

    // Start flash on first chunk
    if (!_rx.flashStarted) {
        size_t flashSize = _rx.isEncrypted ? _rx.hdr.firmware_size - 16 : _rx.hdr.firmware_size;

        _mesh_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!_mesh_ota_partition) {
            DBG_ERROR(TAG, "No OTA partition available");
            sendStatus(false, 3);
            abortTransfer("No OTA partition");
            return;
        }

        esp_err_t err = esp_ota_begin(_mesh_ota_partition, flashSize, &_mesh_ota_handle);
        if (err != ESP_OK) {
            DBG_ERROR(TAG, "esp_ota_begin failed: 0x%x", err);
            sendStatus(false, 3);
            abortTransfer("esp_ota_begin failed");
            return;
        }
        OtaVerify::crc32Begin();
        if (_rx.isSigned) OtaVerify::beginVerify();
        _rx.flashStarted = true;
    }

    // CRC32 + signature verification over ciphertext (encrypt-then-sign)
    OtaVerify::crc32Update(chunkData, chunkLen);
    if (_rx.isSigned) OtaVerify::updateVerify(chunkData, chunkLen);

    // Copy to mutable buffer (decryptBlock is in-place)
    uint8_t decBuf[OTA_MESH_CHUNK_SIZE];
    memcpy(decBuf, chunkData, chunkLen);
    uint8_t* writeData = decBuf;
    size_t writeLen = chunkLen;

    if (_rx.isEncrypted) {
        // First 16 bytes of payload are IV
        if (_rx.bytesReceived < 16) {
            size_t ivRemaining = 16 - _rx.bytesReceived;
            // IV bytes are consumed but not written to flash
            if (_rx.bytesReceived + chunkLen <= 16) {
                // Entire chunk is IV data
                _rx.bytesReceived += chunkLen;
                _rx.nextExpected++;
                _stats.chunks_received++;
                if (_rx.nextExpected % OTA_MESH_WINDOW_SIZE == 0) {
                    sendAck(_rx.nextExpected);
                }
                if (_progressCb) _progressCb(_rx.bytesReceived, _rx.hdr.firmware_size);
                return;
            }
            // Partial IV in this chunk — init decryption with first 16 bytes
            // This is a simplification: we assume chunk 0 has at least 16 bytes
            if (!OtaVerify::decryptBegin(decBuf)) {
                abortTransfer("Decryption init failed");
                sendStatus(false, 3);
                return;
            }
            size_t dataOffset = ivRemaining;
            size_t dataLen = chunkLen - dataOffset;
            OtaVerify::decryptBlock(decBuf + dataOffset, dataLen);
            writeData = decBuf + dataOffset;
            writeLen = dataLen;
        } else {
            OtaVerify::decryptBlock(decBuf, chunkLen);
            writeData = decBuf;
            writeLen = chunkLen;
        }
    }

    // Write to flash via ESP-IDF OTA
    if (writeLen > 0) {
        esp_err_t werr = esp_ota_write(_mesh_ota_handle, writeData, writeLen);
        if (werr != ESP_OK) {
            DBG_ERROR(TAG, "esp_ota_write failed at chunk %u: 0x%x", chk->seq, werr);
            sendStatus(false, 3);
            abortTransfer("Flash write failed");
            return;
        }
    }

    _rx.bytesReceived += chunkLen;
    _rx.nextExpected++;
    _stats.chunks_received++;

    if (_progressCb) _progressCb(_rx.bytesReceived, _rx.hdr.firmware_size);

    // ACK every window
    if (_rx.nextExpected % OTA_MESH_WINDOW_SIZE == 0 || _rx.nextExpected >= _rx.totalChunks) {
        sendAck(_rx.nextExpected);
    }

    // Last chunk?
    if (_rx.nextExpected >= _rx.totalChunks) {
        if (_rx.isEncrypted) OtaVerify::decryptEnd();

        // Verify CRC32
        uint32_t actualCrc = OtaVerify::crc32Finalize();
        if (actualCrc != _rx.hdr.firmware_crc32) {
            DBG_ERROR(TAG, "CRC mismatch: 0x%08X vs 0x%08X", actualCrc, _rx.hdr.firmware_crc32);
            esp_ota_abort(_mesh_ota_handle);
            sendStatus(false, 1);
            abortTransfer("CRC32 mismatch");
            _stats.transfers_failed++;
            return;
        }
        DBG_INFO(TAG, "CRC32 OK: 0x%08X", actualCrc);

        // Verify signature
        if (_rx.isSigned) {
            bool sigOk = OtaVerify::finalizeVerify(_rx.sig.r, _rx.sig.s);
            if (!sigOk) {
                DBG_ERROR(TAG, "Signature INVALID");
                esp_ota_abort(_mesh_ota_handle);
                sendStatus(false, 2);
                abortTransfer("Signature verification failed");
                _stats.transfers_failed++;
                return;
            }
            DBG_INFO(TAG, "ECDSA signature verified OK");
        }

        esp_err_t enderr = esp_ota_end(_mesh_ota_handle);
        if (enderr != ESP_OK) {
            DBG_ERROR(TAG, "esp_ota_end failed: 0x%x", enderr);
            sendStatus(false, 3);
            abortTransfer("Flash finalize failed");
            _stats.transfers_failed++;
            return;
        }

        enderr = esp_ota_set_boot_partition(_mesh_ota_partition);
        if (enderr != ESP_OK) {
            DBG_ERROR(TAG, "esp_ota_set_boot_partition failed: 0x%x", enderr);
            sendStatus(false, 3);
            abortTransfer("Set boot partition failed");
            _stats.transfers_failed++;
            return;
        }

        sendStatus(true, 0);
        _transferActive = false;
        _stats.transfers_completed++;
        DBG_INFO(TAG, "Mesh OTA complete: v%s, %u bytes", _rx.hdr.version, (unsigned)_rx.bytesReceived);

        if (_resultCb) _resultCb(true, _rx.hdr.version);
    }
}

void OtaMesh::handleAck(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshAck)) return;
    const OtaMeshAck* ack = (const OtaMeshAck*)data;
    if (ack->session_id != _tx.session_id) return;

    _tx.ackedChunk = ack->next_seq;
    _tx.retryCount = 0;
    _stats.acks_received++;

    // Send next window of chunks
    for (uint16_t i = 0; i < OTA_MESH_WINDOW_SIZE && _tx.nextChunk < _tx.totalChunks; i++) {
        if (!sendChunk(_tx.nextChunk)) break;
        _tx.nextChunk++;
    }

    if (_tx.ackedChunk >= _tx.totalChunks) {
        DBG_INFO(TAG, "All chunks acknowledged");
    }
}

void OtaMesh::handleRequest(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshAck)) return;
    const OtaMeshAck* req = (const OtaMeshAck*)data;
    if (req->session_id != _tx.session_id) return;

    if (!_tx.hasTarget) {
        memcpy(_tx.targetMac, mac, 6);
        _tx.hasTarget = true;
        DBG_INFO(TAG, "Receiver connected: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    _tx.nextChunk = req->next_seq;
    _tx.retryCount = 0;

    // Send first window
    for (uint16_t i = 0; i < OTA_MESH_WINDOW_SIZE && _tx.nextChunk < _tx.totalChunks; i++) {
        if (!sendChunk(_tx.nextChunk)) break;
        _tx.nextChunk++;
    }
}

void OtaMesh::handleStatus(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshStatus)) return;
    const OtaMeshStatus* st = (const OtaMeshStatus*)data;
    if (st->session_id != _tx.session_id) return;

    _sending = false;
    if (st->success) {
        _stats.transfers_completed++;
        DBG_INFO(TAG, "Receiver confirmed: OTA success");
    } else {
        _stats.transfers_failed++;
        DBG_ERROR(TAG, "Receiver reported failure, error=%u", st->error_code);
    }
}

void OtaMesh::handleAbort(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    if (len < sizeof(OtaMeshAck)) return;
    const OtaMeshAck* ab = (const OtaMeshAck*)data;

    if (_sending && ab->session_id == _tx.session_id) {
        _sending = false;
        DBG_WARN(TAG, "Transfer aborted by receiver");
    }
    if (_transferActive && ab->session_id == _rx.session_id) {
        if (_rx.flashStarted) esp_ota_abort(_mesh_ota_handle);
        if (_rx.isEncrypted) OtaVerify::decryptEnd();
        _transferActive = false;
        DBG_WARN(TAG, "Transfer aborted by sender");
        if (_resultCb) _resultCb(false, _rx.hdr.version);
    }
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

void OtaMesh::sendAnnounce() {
    OtaMeshAnnounce ann;
    memset(&ann, 0, sizeof(ann));
    ann.type = OtaMeshType::ANNOUNCE;
    ann.session_id = _tx.session_id;
    ann.firmware_size = _tx.hdr.firmware_size;
    ann.firmware_crc32 = _tx.hdr.firmware_crc32;
    ann.total_chunks = _tx.totalChunks;
    ann.header_version = _tx.hdr.header_version;
    ann.flags = _tx.hdr.flags;
    memcpy(ann.version, _tx.hdr.version, 24);
    memcpy(ann.board, _tx.hdr.board, 16);

    _espnow->meshBroadcast((const uint8_t*)&ann, sizeof(ann));
    _tx.lastSendMs = millis();
    _stats.announces_sent++;

    // Send signature part if signed
    if (_tx.isSigned) {
        OtaMeshAnnounceSig sigPkt;
        sigPkt.type = OtaMeshType::ANNOUNCE;
        sigPkt.session_id = _tx.session_id;
        sigPkt.part = 2;
        memcpy(sigPkt.sig_r, _tx.sig.r, 32);
        memcpy(sigPkt.sig_s, _tx.sig.s, 32);
        _espnow->meshBroadcast((const uint8_t*)&sigPkt, sizeof(sigPkt));
    }

    DBG_INFO(TAG, "Announce sent: v%s, session 0x%04X", _tx.hdr.version, _tx.session_id);
}

bool OtaMesh::sendChunk(uint16_t seq) {
    uint8_t pkt[sizeof(OtaMeshChunkHeader) + OTA_MESH_CHUNK_SIZE];
    OtaMeshChunkHeader* chk = (OtaMeshChunkHeader*)pkt;

    chk->type = OtaMeshType::CHUNK;
    chk->session_id = _tx.session_id;
    chk->seq = seq;

    size_t dataLen = readChunkData(seq, pkt + sizeof(OtaMeshChunkHeader), OTA_MESH_CHUNK_SIZE);
    if (dataLen == 0 && seq < _tx.totalChunks) {
        DBG_ERROR(TAG, "Failed to read chunk %u", seq);
        return false;
    }
    chk->len = (uint8_t)dataLen;

    bool ok;
    if (_tx.hasTarget) {
        ok = _espnow->meshSend(_tx.targetMac, pkt, sizeof(OtaMeshChunkHeader) + dataLen);
    } else {
        ok = _espnow->meshBroadcast(pkt, sizeof(OtaMeshChunkHeader) + dataLen);
    }

    if (ok) {
        _tx.lastSendMs = millis();
        _stats.chunks_sent++;
    }
    return ok;
}

void OtaMesh::sendAck(uint16_t nextSeq) {
    OtaMeshAck ack;
    ack.type = OtaMeshType::ACK;
    ack.session_id = _rx.session_id;
    ack.next_seq = nextSeq;
    _espnow->meshSend(_rx.senderMac, (const uint8_t*)&ack, sizeof(ack));
    _stats.acks_sent++;
}

void OtaMesh::sendStatus(bool success, uint8_t errorCode) {
    OtaMeshStatus st;
    st.type = OtaMeshType::STATUS;
    st.session_id = _rx.session_id;
    st.success = success ? 1 : 0;
    st.error_code = errorCode;
    _espnow->meshSend(_rx.senderMac, (const uint8_t*)&st, sizeof(st));
}

void OtaMesh::sendAbort() {
    OtaMeshAck ab;
    ab.type = OtaMeshType::ABORT;
    if (_sending) {
        ab.session_id = _tx.session_id;
        if (_tx.hasTarget) {
            _espnow->meshSend(_tx.targetMac, (const uint8_t*)&ab, sizeof(ab));
        } else {
            _espnow->meshBroadcast((const uint8_t*)&ab, sizeof(ab));
        }
    }
    if (_transferActive) {
        ab.session_id = _rx.session_id;
        _espnow->meshSend(_rx.senderMac, (const uint8_t*)&ab, sizeof(ab));
    }
}

void OtaMesh::abortTransfer(const char* reason) {
    DBG_ERROR(TAG, "Transfer aborted: %s", reason);
    if (_rx.flashStarted) esp_ota_abort(_mesh_ota_handle);
    if (_rx.isEncrypted) OtaVerify::decryptEnd();
    _transferActive = false;
    if (_resultCb) _resultCb(false, _rx.hdr.version);
}

size_t OtaMesh::readChunkData(uint16_t seq, uint8_t* out, size_t maxLen) {
    size_t offset = (size_t)seq * OTA_MESH_CHUNK_SIZE;
    size_t remaining = _tx.hdr.firmware_size - offset;
    size_t toRead = (remaining < maxLen) ? remaining : maxLen;

    if (_tx.fromBuffer) {
        size_t bufOffset = _tx.headerSize + offset;
        if (bufOffset + toRead > _tx.bufLen) return 0;
        memcpy(out, _tx.bufData + bufOffset, toRead);
        return toRead;
    } else {
        // Read from SD
        FILE* f = fopen("/sdcard/firmware.ota", "rb");
        if (!f) return 0;
        fseek(f, _tx.headerSize + offset, SEEK_SET);
        size_t n = fread(out, 1, toRead, f);
        fclose(f);
        return n;
    }
}

#else // SIMULATOR

bool OtaMesh::init(EspNowHAL*) { return false; }
bool OtaMesh::startSend(const char*) { return false; }
bool OtaMesh::startSendFromBuffer(const uint8_t*, size_t) { return false; }
void OtaMesh::stopSend() {}
void OtaMesh::enableReceive(bool) {}
void OtaMesh::process() {}
void OtaMesh::handleMeshData(const uint8_t*, const uint8_t*, uint8_t, uint8_t) {}
void OtaMesh::handleAnnounce(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::handleAnnounceSig(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::handleChunk(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::handleAck(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::handleRequest(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::handleStatus(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::handleAbort(const uint8_t*, const uint8_t*, uint8_t) {}
void OtaMesh::sendAnnounce() {}
bool OtaMesh::sendChunk(uint16_t) { return false; }
void OtaMesh::sendAck(uint16_t) {}
void OtaMesh::sendStatus(bool, uint8_t) {}
void OtaMesh::sendAbort() {}
void OtaMesh::abortTransfer(const char*) {}
size_t OtaMesh::readChunkData(uint16_t, uint8_t*, size_t) { return 0; }

#endif
