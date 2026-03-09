// Tritium-OS File Manager — REST API for LittleFS and SD card browsing,
// upload, download, editing, and management.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "file_manager.h"

#if HAS_ASYNC_WEBSERVER

#include "debug_log.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <FS.h>
#include <cstring>

static const char* TAG = "filemgr";

// ── Path sanitization ────────────────────────────────────────────────────────

static bool isPathSafe(const String& path) {
    if (path.length() == 0) return false;
    if (path[0] != '/') return false;
    if (path.indexOf("..") >= 0) return false;
    return true;
}

// ── Storage selection helper ─────────────────────────────────────────────────

static fs::FS* getStorage(const String& storage) {
    if (storage == "sdcard") {
        return &SD_MMC;
    }
    return &LittleFS;
}

static bool isStorageMounted(const String& storage) {
    if (storage == "sdcard") {
        return SD_MMC.cardType() != CARD_NONE;
    }
    return true;  // LittleFS is always available once mounted at boot
}

// ── MIME type detection ──────────────────────────────────────────────────────

static const char* getMimeType(const String& path) {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".txt"))  return "text/plain";
    if (path.endsWith(".csv"))  return "text/csv";
    if (path.endsWith(".log"))  return "text/plain";
    if (path.endsWith(".xml"))  return "text/xml";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".gif"))  return "image/gif";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".bin"))  return "application/octet-stream";
    if (path.endsWith(".gz"))   return "application/gzip";
    if (path.endsWith(".zip"))  return "application/zip";
    return "application/octet-stream";
}

static bool isTextFile(const String& path) {
    return path.endsWith(".txt") || path.endsWith(".json") ||
           path.endsWith(".csv") || path.endsWith(".log")  ||
           path.endsWith(".xml") || path.endsWith(".html") ||
           path.endsWith(".htm") || path.endsWith(".css")  ||
           path.endsWith(".js")  || path.endsWith(".ini")  ||
           path.endsWith(".cfg") || path.endsWith(".conf") ||
           path.endsWith(".yaml")|| path.endsWith(".yml")  ||
           path.endsWith(".md")  || path.endsWith(".sh");
}

// ── Size limit helpers ───────────────────────────────────────────────────────

static size_t getMaxUploadSize(const String& storage) {
    if (storage == "sdcard") return 100 * 1024 * 1024;  // 100 MB
    return 1 * 1024 * 1024;  // 1 MB for LittleFS
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

// Escape a string for JSON output
static String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Count children in a directory
static int countChildren(fs::FS* fs, const String& path) {
    File dir = fs->open(path);
    if (!dir || !dir.isDirectory()) return 0;
    int count = 0;
    File entry = dir.openNextFile();
    while (entry) {
        count++;
        entry = dir.openNextFile();
    }
    return count;
}

// ── Route handlers ───────────────────────────────────────────────────────────

namespace file_manager {

void registerRoutes(AsyncWebServer* server) {
    if (!server) return;

    // ── GET /api/fs/list ─────────────────────────────────────────────────
    server->on("/api/fs/list", HTTP_GET, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "/";
        String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

        if (!isPathSafe(path)) {
            req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
            return;
        }
        if (!isStorageMounted(storage)) {
            req->send(503, "application/json", "{\"error\":\"Storage not mounted\"}");
            return;
        }

        fs::FS* fs = getStorage(storage);
        File dir = fs->open(path);
        if (!dir || !dir.isDirectory()) {
            req->send(404, "application/json", "{\"error\":\"Directory not found\"}");
            return;
        }

        String json = "{\"path\":\"" + jsonEscape(path) + "\",\"storage\":\"" + storage + "\",\"entries\":[";

        bool first = true;
        File entry = dir.openNextFile();
        while (entry) {
            if (!first) json += ",";
            first = false;

            String name = String(entry.name());
            // Strip parent path — entry.name() returns full path on ESP32
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) name = name.substring(lastSlash + 1);

            if (entry.isDirectory()) {
                String fullPath = path;
                if (!fullPath.endsWith("/")) fullPath += "/";
                fullPath += name;
                int children = countChildren(fs, fullPath);
                json += "{\"name\":\"" + jsonEscape(name) + "\",\"type\":\"dir\",\"children\":" + String(children) + "}";
            } else {
                json += "{\"name\":\"" + jsonEscape(name) + "\",\"type\":\"file\",\"size\":" + String(entry.size());
                time_t t = entry.getLastWrite();
                if (t > 0) json += ",\"modified\":" + String((unsigned long)t);
                json += "}";
            }
            entry = dir.openNextFile();
        }

        json += "],";

        // Storage stats
        if (storage == "sdcard") {
            json += "\"total_bytes\":" + String((unsigned long long)SD_MMC.totalBytes());
            json += ",\"used_bytes\":" + String((unsigned long long)SD_MMC.usedBytes());
            uint64_t free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
            json += ",\"free_bytes\":" + String((unsigned long long)free);
        } else {
            json += "\"total_bytes\":" + String(LittleFS.totalBytes());
            json += ",\"used_bytes\":" + String(LittleFS.usedBytes());
            json += ",\"free_bytes\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes());
        }
        json += "}";

        req->send(200, "application/json", json);
        DBG_INFO(TAG, "list: %s (%s)", path.c_str(), storage.c_str());
    });

    // ── GET /api/fs/read ─────────────────────────────────────────────────
    server->on("/api/fs/read", HTTP_GET, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "";
        String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

        if (!isPathSafe(path)) {
            req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
            return;
        }
        if (!isStorageMounted(storage)) {
            req->send(503, "application/json", "{\"error\":\"Storage not mounted\"}");
            return;
        }

        fs::FS* fs = getStorage(storage);
        File file = fs->open(path, "r");
        if (!file || file.isDirectory()) {
            req->send(404, "application/json", "{\"error\":\"File not found\"}");
            return;
        }

        size_t sz = file.size();
        // Cap read at 1 MB for text viewing
        if (sz > 1024 * 1024) {
            file.close();
            req->send(413, "application/json", "{\"error\":\"File too large for inline viewing\"}");
            return;
        }

        String content;
        content.reserve(sz);
        while (file.available()) {
            content += (char)file.read();
        }
        file.close();

        const char* mime = isTextFile(path) ? "text/plain" : getMimeType(path);
        req->send(200, mime, content);
        DBG_INFO(TAG, "read: %s (%zu bytes)", path.c_str(), sz);
    });

    // ── GET /api/fs/download ─────────────────────────────────────────────
    server->on("/api/fs/download", HTTP_GET, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "";
        String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

        if (!isPathSafe(path)) {
            req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
            return;
        }
        if (!isStorageMounted(storage)) {
            req->send(503, "application/json", "{\"error\":\"Storage not mounted\"}");
            return;
        }

        fs::FS* fs = getStorage(storage);
        File file = fs->open(path, "r");
        if (!file || file.isDirectory()) {
            req->send(404, "application/json", "{\"error\":\"File not found\"}");
            return;
        }

        // Extract filename for Content-Disposition
        String filename = path;
        int lastSlash = filename.lastIndexOf('/');
        if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);

        AsyncWebServerResponse* response = req->beginResponse(
            *fs, path, getMimeType(path), true  // true = download/attachment
        );
        response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        req->send(response);
        DBG_INFO(TAG, "download: %s", path.c_str());
    });

    // ── POST /api/fs/write ───────────────────────────────────────────────
    server->on("/api/fs/write", HTTP_POST,
        // Request handler (called after body is received)
        [](AsyncWebServerRequest* req) {
            // Response sent from body handler; nothing here
        },
        nullptr,  // upload handler
        // Body handler
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            String path = req->hasParam("path") ? req->getParam("path")->value() : "";
            String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

            if (!isPathSafe(path)) {
                if (index == 0) req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
                return;
            }
            if (total > getMaxUploadSize(storage)) {
                if (index == 0) req->send(413, "application/json", "{\"error\":\"File too large\"}");
                return;
            }

            fs::FS* fs = getStorage(storage);

            if (index == 0) {
                // First chunk — open file for writing
                File file = fs->open(path, "w");
                if (!file) {
                    req->send(500, "application/json", "{\"error\":\"Failed to create file\"}");
                    return;
                }
                file.write(data, len);
                file.close();
                DBG_INFO(TAG, "write begin: %s (%zu bytes total)", path.c_str(), total);
            } else {
                // Subsequent chunks — append
                File file = fs->open(path, "a");
                if (file) {
                    file.write(data, len);
                    file.close();
                }
            }

            // Last chunk — send response
            if (index + len == total) {
                String json = "{\"ok\":true,\"path\":\"" + jsonEscape(path) + "\",\"size\":" + String(total) + "}";
                req->send(200, "application/json", json);
                DBG_INFO(TAG, "write complete: %s (%zu bytes)", path.c_str(), total);
            }
        }
    );

    // ── POST /api/fs/upload (multipart) ──────────────────────────────────
    server->on("/api/fs/upload", HTTP_POST,
        // Request complete handler
        [](AsyncWebServerRequest* req) {
            req->send(200, "application/json", "{\"ok\":true}");
        },
        // Upload handler (multipart file chunks)
        [](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final)
        {
            String dir = req->hasParam("path") ? req->getParam("path")->value() : "/";
            String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

            if (!isPathSafe(dir)) return;

            String path = dir;
            if (!path.endsWith("/")) path += "/";
            path += filename;

            fs::FS* fs = getStorage(storage);

            if (index == 0) {
                DBG_INFO(TAG, "upload begin: %s -> %s", filename.c_str(), path.c_str());
                File file = fs->open(path, "w");
                if (file) {
                    file.write(data, len);
                    file.close();
                }
            } else {
                File file = fs->open(path, "a");
                if (file) {
                    file.write(data, len);
                    file.close();
                }
            }

            if (final) {
                DBG_INFO(TAG, "upload complete: %s (%zu bytes)", path.c_str(), index + len);
            }
        }
    );

    // ── DELETE /api/fs/delete ────────────────────────────────────────────
    server->on("/api/fs/delete", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "";
        String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

        if (!isPathSafe(path)) {
            req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
            return;
        }
        if (!isStorageMounted(storage)) {
            req->send(503, "application/json", "{\"error\":\"Storage not mounted\"}");
            return;
        }

        fs::FS* fs = getStorage(storage);

        if (!fs->exists(path)) {
            req->send(404, "application/json", "{\"error\":\"File not found\"}");
            return;
        }

        // Check if it's a directory
        File f = fs->open(path);
        bool isDir = f && f.isDirectory();
        f.close();

        bool ok;
        if (isDir) {
            ok = fs->rmdir(path);
        } else {
            ok = fs->remove(path);
        }

        if (ok) {
            req->send(200, "application/json", "{\"ok\":true}");
            DBG_INFO(TAG, "delete: %s", path.c_str());
        } else {
            req->send(500, "application/json", "{\"error\":\"Delete failed\"}");
            DBG_WARN(TAG, "delete failed: %s", path.c_str());
        }
    });

    // ── POST /api/fs/mkdir ───────────────────────────────────────────────
    server->on("/api/fs/mkdir", HTTP_POST, [](AsyncWebServerRequest* req) {
        String path = req->hasParam("path") ? req->getParam("path")->value() : "";
        String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

        if (!isPathSafe(path)) {
            req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
            return;
        }
        if (!isStorageMounted(storage)) {
            req->send(503, "application/json", "{\"error\":\"Storage not mounted\"}");
            return;
        }

        fs::FS* fs = getStorage(storage);
        if (fs->mkdir(path)) {
            req->send(200, "application/json", "{\"ok\":true}");
            DBG_INFO(TAG, "mkdir: %s", path.c_str());
        } else {
            req->send(500, "application/json", "{\"error\":\"Failed to create directory\"}");
        }
    });

    // ── POST /api/fs/rename ──────────────────────────────────────────────
    server->on("/api/fs/rename", HTTP_POST, [](AsyncWebServerRequest* req) {
        String from = req->hasParam("from") ? req->getParam("from")->value() : "";
        String to   = req->hasParam("to")   ? req->getParam("to")->value()   : "";
        String storage = req->hasParam("storage") ? req->getParam("storage")->value() : "littlefs";

        if (!isPathSafe(from) || !isPathSafe(to)) {
            req->send(400, "application/json", "{\"error\":\"Invalid path\"}");
            return;
        }
        if (!isStorageMounted(storage)) {
            req->send(503, "application/json", "{\"error\":\"Storage not mounted\"}");
            return;
        }

        fs::FS* fs = getStorage(storage);
        if (!fs->exists(from)) {
            req->send(404, "application/json", "{\"error\":\"Source not found\"}");
            return;
        }

        if (fs->rename(from, to)) {
            req->send(200, "application/json", "{\"ok\":true}");
            DBG_INFO(TAG, "rename: %s -> %s", from.c_str(), to.c_str());
        } else {
            req->send(500, "application/json", "{\"error\":\"Rename failed\"}");
        }
    });

    // ── GET /api/fs/stats ────────────────────────────────────────────────
    server->on("/api/fs/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        String json = "{";

        // LittleFS stats
        json += "\"littlefs\":{";
        json += "\"total\":" + String(LittleFS.totalBytes());
        json += ",\"used\":" + String(LittleFS.usedBytes());
        json += ",\"free\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes());
        json += "}";

        // SD card stats
        bool sdMounted = SD_MMC.cardType() != CARD_NONE;
        json += ",\"sdcard\":{";
        json += "\"mounted\":" + String(sdMounted ? "true" : "false");
        if (sdMounted) {
            json += ",\"total\":" + String((unsigned long long)SD_MMC.totalBytes());
            json += ",\"used\":" + String((unsigned long long)SD_MMC.usedBytes());
            uint64_t free = SD_MMC.totalBytes() - SD_MMC.usedBytes();
            json += ",\"free\":" + String((unsigned long long)free);
        }
        json += "}";

        json += "}";
        req->send(200, "application/json", json);
    });

    // ── GET /files — serve the web UI ────────────────────────────────────
    server->on("/files", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(LittleFS, "/web/files.html", "text/html");
    });

    DBG_INFO(TAG, "File manager routes registered (/api/fs/* + /files)");
}

}  // namespace file_manager

#endif  // HAS_ASYNC_WEBSERVER
