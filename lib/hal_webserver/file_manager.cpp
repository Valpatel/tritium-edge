// Tritium-OS File Manager — REST API for LittleFS and SD card browsing,
// upload, download, editing, and management.
// Uses ESP-IDF native esp_http_server + POSIX file I/O via VFS.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "file_manager.h"
#include "debug_log.h"
#include "tritium_compat.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <vector>
#include <string>

static const char* TAG = "filemgr";

// ── VFS mount points ─────────────────────────────────────────────────────────
// LittleFS is mounted at /littlefs, SD card at /sdcard by their respective
// init code. We use POSIX fopen/fread/etc — VFS handles the routing.

static const char* getLfsBase()    { return "/littlefs"; }
static const char* getSdcardBase() { return "/sdcard"; }

// ── Path sanitization ────────────────────────────────────────────────────────

static bool isPathSafe(const char* path) {
    if (!path || path[0] == '\0') return false;
    if (path[0] != '/') return false;
    if (strstr(path, "..") != nullptr) return false;
    return true;
}

// ── Storage selection helper ─────────────────────────────────────────────────

static const char* getStorageBase(const char* storage) {
    if (storage && strcmp(storage, "sdcard") == 0) {
        return getSdcardBase();
    }
    return getLfsBase();
}

static bool isStorageMounted(const char* storage) {
    if (storage && strcmp(storage, "sdcard") == 0) {
        struct stat st;
        return (stat(getSdcardBase(), &st) == 0);
    }
    return true;  // LittleFS is always available once mounted at boot
}

// Build a full VFS path: base + relative_path
static void buildFullPath(char* out, size_t out_size, const char* base, const char* rel) {
    if (rel && rel[0] == '/') {
        snprintf(out, out_size, "%s%s", base, rel);
    } else {
        snprintf(out, out_size, "%s/%s", base, rel ? rel : "");
    }
}

// ── MIME type detection ──────────────────────────────────────────────────────

static const char* getMimeType(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0)  return "text/css";
    if (strcmp(dot, ".js") == 0)   return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".txt") == 0)  return "text/plain";
    if (strcmp(dot, ".csv") == 0)  return "text/csv";
    if (strcmp(dot, ".log") == 0)  return "text/plain";
    if (strcmp(dot, ".xml") == 0)  return "text/xml";
    if (strcmp(dot, ".png") == 0)  return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)  return "image/gif";
    if (strcmp(dot, ".ico") == 0)  return "image/x-icon";
    if (strcmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(dot, ".bin") == 0)  return "application/octet-stream";
    if (strcmp(dot, ".gz") == 0)   return "application/gzip";
    if (strcmp(dot, ".zip") == 0)  return "application/zip";
    return "application/octet-stream";
}

static bool isTextFile(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    static const char* text_exts[] = {
        ".txt", ".json", ".csv", ".log", ".xml", ".html", ".htm",
        ".css", ".js", ".ini", ".cfg", ".conf", ".yaml", ".yml",
        ".md", ".sh", nullptr
    };
    for (int i = 0; text_exts[i]; i++) {
        if (strcmp(dot, text_exts[i]) == 0) return true;
    }
    return false;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────

static int jsonEscapeInto(char* out, size_t out_size, const char* s) {
    int pos = 0;
    for (int i = 0; s[i] && pos < (int)out_size - 2; i++) {
        char c = s[i];
        switch (c) {
            case '"':  if (pos + 2 < (int)out_size) { out[pos++] = '\\'; out[pos++] = '"'; } break;
            case '\\': if (pos + 2 < (int)out_size) { out[pos++] = '\\'; out[pos++] = '\\'; } break;
            case '\n': if (pos + 2 < (int)out_size) { out[pos++] = '\\'; out[pos++] = 'n'; } break;
            case '\r': if (pos + 2 < (int)out_size) { out[pos++] = '\\'; out[pos++] = 'r'; } break;
            case '\t': if (pos + 2 < (int)out_size) { out[pos++] = '\\'; out[pos++] = 't'; } break;
            default:   out[pos++] = c; break;
        }
    }
    out[pos] = '\0';
    return pos;
}

// ── Query parameter helper ───────────────────────────────────────────────────

static bool getQueryParam(httpd_req_t* req, const char* key, char* out, size_t out_size) {
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return false;
    char* query = (char*)malloc(qlen + 1);
    if (!query) return false;
    if (httpd_req_get_url_query_str(req, query, qlen + 1) != ESP_OK) {
        free(query);
        return false;
    }
    esp_err_t err = httpd_query_key_value(query, key, out, out_size);
    free(query);
    return (err == ESP_OK);
}

// ── Send JSON error helper ───────────────────────────────────────────────────

static esp_err_t sendJsonError(httpd_req_t* req, const char* status, const char* msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sendJsonOk(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// ── Count children in a directory ────────────────────────────────────────────

static int countChildren(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) return 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        count++;
    }
    closedir(dir);
    return count;
}

// ── Route handlers ───────────────────────────────────────────────────────────

// GET /api/fs/list
static esp_err_t handle_fs_list(httpd_req_t* req) {
    char path[128] = "/";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    DIR* dir = opendir(fullPath);
    if (!dir) return sendJsonError(req, "404", "Directory not found");

    // Build JSON response in a heap buffer
    char* json = (char*)malloc(4096);
    if (!json) { closedir(dir); return sendJsonError(req, "500", "Out of memory"); }

    char escaped[128];
    jsonEscapeInto(escaped, sizeof(escaped), path);
    int pos = snprintf(json, 4096, "{\"path\":\"%s\",\"storage\":\"%s\",\"entries\":[", escaped, storage);

    bool first = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!first && pos < 4000) json[pos++] = ',';
        first = false;

        char entryPath[256];
        snprintf(entryPath, sizeof(entryPath), "%s/%s", fullPath, entry->d_name);
        struct stat st;
        bool isDir = (entry->d_type == DT_DIR);
        bool hasStat = (stat(entryPath, &st) == 0);

        jsonEscapeInto(escaped, sizeof(escaped), entry->d_name);

        if (isDir) {
            int children = countChildren(entryPath);
            pos += snprintf(json + pos, 4096 - pos,
                "{\"name\":\"%s\",\"type\":\"dir\",\"children\":%d}", escaped, children);
        } else {
            pos += snprintf(json + pos, 4096 - pos,
                "{\"name\":\"%s\",\"type\":\"file\",\"size\":%lu",
                escaped, hasStat ? (unsigned long)st.st_size : 0UL);
            if (hasStat && st.st_mtime > 0) {
                pos += snprintf(json + pos, 4096 - pos, ",\"modified\":%lu", (unsigned long)st.st_mtime);
            }
            pos += snprintf(json + pos, 4096 - pos, "}");
        }
    }
    closedir(dir);

    // Storage stats (approximate — esp_littlefs provides these via VFS)
    pos += snprintf(json + pos, 4096 - pos, "],");

    // For now, report 0 for stats — the actual values require
    // filesystem-specific APIs that differ between LittleFS and FATFS
    pos += snprintf(json + pos, 4096 - pos,
        "\"total_bytes\":0,\"used_bytes\":0,\"free_bytes\":0}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    DBG_INFO(TAG, "list: %s (%s)", path, storage);
    return ESP_OK;
}

// GET /api/fs/read
static esp_err_t handle_fs_read(httpd_req_t* req) {
    char path[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    struct stat st;
    if (stat(fullPath, &st) != 0 || S_ISDIR(st.st_mode)) {
        return sendJsonError(req, "404", "File not found");
    }

    if (st.st_size > 1024 * 1024) {
        return sendJsonError(req, "413", "File too large for inline viewing");
    }

    FILE* f = fopen(fullPath, "r");
    if (!f) return sendJsonError(req, "404", "File not found");

    const char* mime = isTextFile(path) ? "text/plain" : getMimeType(path);
    httpd_resp_set_type(req, mime);

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked response

    DBG_INFO(TAG, "read: %s (%ld bytes)", path, (long)st.st_size);
    return ESP_OK;
}

// GET /api/fs/download
static esp_err_t handle_fs_download(httpd_req_t* req) {
    char path[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    struct stat st;
    if (stat(fullPath, &st) != 0 || S_ISDIR(st.st_mode)) {
        return sendJsonError(req, "404", "File not found");
    }

    FILE* f = fopen(fullPath, "r");
    if (!f) return sendJsonError(req, "404", "File not found");

    // Extract filename
    const char* filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    httpd_resp_set_type(req, getMimeType(path));

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);

    DBG_INFO(TAG, "download: %s", path);
    return ESP_OK;
}

// POST /api/fs/write — write body content to a file
static esp_err_t handle_fs_write(httpd_req_t* req) {
    char path[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    FILE* f = fopen(fullPath, "w");
    if (!f) return sendJsonError(req, "500", "Failed to create file");

    char buf[512];
    int remaining = req->content_len;
    size_t total = 0;

    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : sizeof(buf));
        if (recv <= 0) {
            fclose(f);
            return sendJsonError(req, "500", "Receive failed");
        }
        fwrite(buf, 1, recv, f);
        remaining -= recv;
        total += recv;
    }
    fclose(f);

    char resp[128];
    char escaped[128];
    jsonEscapeInto(escaped, sizeof(escaped), path);
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\",\"size\":%zu}", escaped, total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    DBG_INFO(TAG, "write complete: %s (%zu bytes)", path, total);
    return ESP_OK;
}

// POST /api/fs/upload — multipart firmware upload
// For simplicity, this receives the raw body and writes it.
// Full multipart parsing can be added later if needed.
static esp_err_t handle_fs_upload(httpd_req_t* req) {
    // Multipart upload is complex with esp_http_server.
    // For now, accept raw body POST with path in query params.
    return handle_fs_write(req);
}

// DELETE /api/fs/delete
static esp_err_t handle_fs_delete(httpd_req_t* req) {
    char path[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    struct stat st;
    if (stat(fullPath, &st) != 0) {
        return sendJsonError(req, "404", "File not found");
    }

    int ok;
    if (S_ISDIR(st.st_mode)) {
        ok = (rmdir(fullPath) == 0) ? 1 : 0;
    } else {
        ok = (unlink(fullPath) == 0) ? 1 : 0;
    }

    if (ok) {
        DBG_INFO(TAG, "delete: %s", path);
        return sendJsonOk(req);
    } else {
        DBG_WARN(TAG, "delete failed: %s", path);
        return sendJsonError(req, "500", "Delete failed");
    }
}

// POST /api/fs/mkdir
static esp_err_t handle_fs_mkdir(httpd_req_t* req) {
    char path[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    if (mkdir(fullPath, 0755) == 0) {
        DBG_INFO(TAG, "mkdir: %s", path);
        return sendJsonOk(req);
    }
    return sendJsonError(req, "500", "Failed to create directory");
}

// POST /api/fs/rename
static esp_err_t handle_fs_rename(httpd_req_t* req) {
    char from[128] = "";
    char to[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "from", from, sizeof(from));
    getQueryParam(req, "to", to, sizeof(to));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(from) || !isPathSafe(to)) return sendJsonError(req, "400", "Invalid path");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fromFull[256], toFull[256];
    buildFullPath(fromFull, sizeof(fromFull), base, from);
    buildFullPath(toFull, sizeof(toFull), base, to);

    struct stat st;
    if (stat(fromFull, &st) != 0) {
        return sendJsonError(req, "404", "Source not found");
    }

    if (rename(fromFull, toFull) == 0) {
        DBG_INFO(TAG, "rename: %s -> %s", from, to);
        return sendJsonOk(req);
    }
    return sendJsonError(req, "500", "Rename failed");
}

// GET /api/fs/stats
static esp_err_t handle_fs_stats(httpd_req_t* req) {
    // Basic stats — detailed FS stats require filesystem-specific APIs
    char json[256];
    bool sdMounted = isStorageMounted("sdcard");
    snprintf(json, sizeof(json),
        "{\"littlefs\":{\"total\":0,\"used\":0,\"free\":0},"
        "\"sdcard\":{\"mounted\":%s}}",
        sdMounted ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// POST /api/fs/rmdir-recursive
static esp_err_t handle_fs_rmdir_recursive(httpd_req_t* req) {
    char path[128] = "";
    char storage[16] = "littlefs";
    getQueryParam(req, "path", path, sizeof(path));
    getQueryParam(req, "storage", storage, sizeof(storage));

    if (!isPathSafe(path)) return sendJsonError(req, "400", "Invalid path");
    if (strcmp(path, "/") == 0) return sendJsonError(req, "400", "Cannot delete root directory");
    if (!isStorageMounted(storage)) return sendJsonError(req, "503", "Storage not mounted");

    const char* base = getStorageBase(storage);
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), base, path);

    struct stat st;
    if (stat(fullPath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return sendJsonError(req, "404", "Directory not found");
    }

    // Recursive delete using iterative stack approach
    struct DirEntry {
        std::string path;
        bool isDir;
    };

    std::vector<DirEntry> entries;
    std::vector<std::string> dirsToScan;
    dirsToScan.push_back(fullPath);

    int fileCount = 0, dirCount = 0;

    while (!dirsToScan.empty()) {
        std::string current = dirsToScan.back();
        dirsToScan.pop_back();

        DIR* d = opendir(current.c_str());
        if (!d) continue;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string entryPath = current + "/" + entry->d_name;
            if (entry->d_type == DT_DIR) {
                dirsToScan.push_back(entryPath);
                entries.push_back({entryPath, true});
                dirCount++;
            } else {
                entries.push_back({entryPath, false});
                fileCount++;
            }
        }
        closedir(d);
    }

    bool allOk = true;
    for (auto& e : entries) {
        if (!e.isDir) {
            if (unlink(e.path.c_str()) != 0) {
                DBG_WARN(TAG, "rmdir-recursive: failed to remove %s", e.path.c_str());
                allOk = false;
            }
        }
    }
    for (int i = entries.size() - 1; i >= 0; i--) {
        if (entries[i].isDir) {
            if (rmdir(entries[i].path.c_str()) != 0) {
                DBG_WARN(TAG, "rmdir-recursive: failed to rmdir %s", entries[i].path.c_str());
                allOk = false;
            }
        }
    }
    if (rmdir(fullPath) != 0) {
        DBG_WARN(TAG, "rmdir-recursive: failed to rmdir root %s", fullPath);
        allOk = false;
    }

    if (allOk) {
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":true,\"files_deleted\":%d,\"dirs_deleted\":%d}",
                 fileCount, dirCount + 1);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        DBG_INFO(TAG, "rmdir-recursive: %s (%d files, %d dirs)", path, fileCount, dirCount + 1);
    } else {
        return sendJsonError(req, "500", "Some items could not be deleted");
    }
    return ESP_OK;
}

// GET /files — serve the web UI (from LittleFS)
static esp_err_t handle_files_page(httpd_req_t* req) {
    char fullPath[256];
    buildFullPath(fullPath, sizeof(fullPath), getLfsBase(), "/web/files.html");

    FILE* f = fopen(fullPath, "r");
    if (!f) {
        httpd_resp_set_status(req, "404");
        return httpd_resp_send(req, "File manager page not found", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "text/html");
    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ── Route registration ───────────────────────────────────────────────────────

namespace file_manager {

void registerRoutes(httpd_handle_t server) {
    if (!server) return;

    // Helper macro for registering handlers
    #define REG(uri_str, method_val, handler_fn) do { \
        httpd_uri_t _u = {}; \
        _u.uri = uri_str; \
        _u.method = method_val; \
        _u.handler = handler_fn; \
        _u.user_ctx = nullptr; \
        httpd_register_uri_handler(server, &_u); \
    } while (0)

    REG("/api/fs/list",            HTTP_GET,    handle_fs_list);
    REG("/api/fs/read",            HTTP_GET,    handle_fs_read);
    REG("/api/fs/download",        HTTP_GET,    handle_fs_download);
    REG("/api/fs/write",           HTTP_POST,   handle_fs_write);
    REG("/api/fs/upload",          HTTP_POST,   handle_fs_upload);
    REG("/api/fs/delete",          HTTP_DELETE,  handle_fs_delete);
    REG("/api/fs/mkdir",           HTTP_POST,   handle_fs_mkdir);
    REG("/api/fs/rename",          HTTP_POST,   handle_fs_rename);
    REG("/api/fs/stats",           HTTP_GET,    handle_fs_stats);
    REG("/api/fs/rmdir-recursive", HTTP_POST,   handle_fs_rmdir_recursive);
    REG("/files",                  HTTP_GET,    handle_files_page);

    #undef REG

    DBG_INFO(TAG, "File manager routes registered (/api/fs/* + /files)");
}

}  // namespace file_manager
