// Tritium-OS OTA Routes — REST API for firmware update management
// Uses ESP-IDF native esp_http_server + esp_ota_ops.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ota_routes.h"
#include "ota_manager.h"
#include "debug_log.h"
#include "tritium_compat.h"

#include <cstdio>
#include <cstring>
#include <esp_ota_ops.h>

static constexpr const char* TAG = "ota_routes";

#ifndef SIMULATOR

// ---------------------------------------------------------------------------
// Embedded OTA web UI page (Valpatel design language)
// ---------------------------------------------------------------------------
static const char OTA_PAGE_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS // Firmware Update</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--cyan:#00f0ff;--mag:#ff2a6d;--green:#05ffa1;--yellow:#fcee0a;
--void:#0a0a0f;--s1:#0e0e14;--s2:#12121a;--s3:#1a1a2e;
--ghost:#8888aa;--text:#c8d0dc;--bright:#e0e0ff}
body{background:var(--void);color:var(--text);font-family:'Courier New',monospace;
font-size:13px;padding:16px;max-width:720px;margin:0 auto}
h1{color:var(--cyan);font-size:16px;letter-spacing:0.15em;text-transform:uppercase;
padding:12px 0;border-bottom:1px solid rgba(0,240,255,0.15);margin-bottom:16px}
.panel{background:var(--s2);border:1px solid rgba(0,240,255,0.08);border-radius:4px;
padding:14px;margin-bottom:12px}
.panel-title{color:var(--bright);font-size:11px;letter-spacing:0.12em;
text-transform:uppercase;margin-bottom:10px}
.info-grid{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;font-size:12px}
.info-grid .k{color:var(--ghost)}.info-grid .v{color:var(--cyan)}
.btn{padding:7px 16px;border:1px solid rgba(0,240,255,0.3);background:transparent;
color:var(--cyan);font-family:inherit;font-size:11px;cursor:pointer;border-radius:3px}
.btn:disabled{opacity:0.4;cursor:not-allowed}
.btn.danger{border-color:rgba(255,42,109,0.3);color:var(--mag)}
.btn.success{border-color:rgba(5,255,161,0.3);color:var(--green)}
.btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.progress-wrap{margin:12px 0;display:none}.progress-wrap.show{display:block}
.progress-bar{height:20px;background:var(--s3);border-radius:3px;overflow:hidden}
.progress-fill{height:100%;width:0%;background:var(--cyan);transition:width 0.3s}
.progress-text{text-align:center;font-size:12px;margin-top:4px;color:var(--cyan)}
.status-msg{padding:8px 12px;border-radius:3px;font-size:12px;margin:8px 0;display:none}
.status-msg.show{display:block}
.status-msg.ok{background:rgba(5,255,161,0.08);color:var(--green)}
.status-msg.err{background:rgba(255,42,109,0.08);color:var(--mag)}
.url-input{width:100%;padding:7px 10px;background:var(--s1);border:1px solid rgba(0,240,255,0.15);
color:var(--cyan);font-family:inherit;font-size:12px;border-radius:3px;margin-bottom:8px}
table{width:100%;border-collapse:collapse;font-size:12px;margin-top:6px}
th{text-align:left;color:var(--ghost);font-size:10px;padding:4px 8px;border-bottom:1px solid rgba(0,240,255,0.08)}
td{padding:5px 8px;border-bottom:1px solid rgba(255,255,255,0.03)}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:6px}
.dot.ok{background:var(--green)}.dot.fail{background:var(--mag)}
.state-label{display:inline-block;padding:2px 8px;border-radius:2px;font-size:10px;text-transform:uppercase}
.state-idle{background:rgba(136,136,170,0.15);color:var(--ghost)}
.state-active{background:rgba(0,240,255,0.15);color:var(--cyan)}
.state-ok{background:rgba(5,255,161,0.15);color:var(--green)}
.state-err{background:rgba(255,42,109,0.15);color:var(--mag)}
.danger-zone{border-color:rgba(255,42,109,0.15)}
</style>
</head>
<body>
<h1>// Tritium-OS Firmware Update</h1>
<div class="panel"><div class="panel-title">System Info</div>
<div class="info-grid">
<span class="k">Current</span><span class="v" id="cur-ver">---</span>
<span class="k">Partition</span><span class="v" id="cur-part">---</span>
<span class="k">Next</span><span class="v" id="next-part">---</span>
<span class="k">Size</span><span class="v" id="part-size">---</span>
<span class="k">State</span><span class="v" id="ota-state"><span class="state-label state-idle">IDLE</span></span>
<span class="k">Uptime</span><span class="v" id="uptime">---</span>
</div></div>
<div class="panel"><div class="panel-title">Firmware Upload</div>
<div style="border:2px dashed rgba(0,240,255,0.2);border-radius:6px;padding:32px 16px;text-align:center;position:relative;background:var(--s1)">
<input type="file" id="fw-file" accept=".bin,.ota" style="position:absolute;inset:0;opacity:0;cursor:pointer">
<div style="color:var(--ghost)">Drop <span style="color:var(--cyan)">firmware.bin</span> here or click</div>
</div>
<div id="file-info" style="display:none;margin-top:8px;font-size:11px;color:var(--ghost)"><span id="file-name"></span> — <span id="file-size"></span></div>
<div class="progress-wrap" id="progress-wrap"><div class="progress-bar"><div class="progress-fill" id="progress-fill"></div></div><div class="progress-text" id="progress-text">0%</div></div>
<div id="upload-msg" class="status-msg"></div>
<div class="btn-row"><button class="btn" id="btn-upload" disabled>Upload &amp; Flash</button></div>
</div>
<div class="panel"><div class="panel-title">URL Update</div>
<input type="text" class="url-input" id="url-input" placeholder="https://example.com/firmware.bin">
<div class="btn-row"><button class="btn" id="btn-url">Pull Update</button></div></div>
<div class="panel"><div class="panel-title">History</div>
<table><thead><tr><th>Version</th><th>Time</th><th>Source</th><th>Status</th></tr></thead>
<tbody id="history-body"><tr><td colspan="4" style="color:var(--ghost)">Loading...</td></tr></tbody></table></div>
<div class="panel"><div class="panel-title">Mesh</div>
<div class="btn-row"><button class="btn success" id="btn-mesh">Distribute to Fleet</button></div></div>
<div class="panel danger-zone"><div class="panel-title" style="color:var(--mag)">Danger Zone</div>
<div class="btn-row"><button class="btn danger" id="btn-rollback">Rollback</button><button class="btn danger" id="btn-reboot">Reboot</button></div></div>
<script>
(function(){
const $=s=>document.getElementById(s),api=p=>fetch('/api/ota/'+p),
post=(p,b)=>fetch('/api/ota/'+p,{method:'POST',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):undefined});
const STATES=['IDLE','CHECKING','DOWNLOADING','WRITING','VERIFYING','READY_REBOOT','FAILED'],
SC=['state-idle','state-active','state-active','state-active','state-active','state-ok','state-err'];
function fb(b){return b>=1048576?(b/1048576).toFixed(1)+'MB':b>=1024?(b/1024).toFixed(1)+'KB':b+'B';}
function ft(s){return s<60?s+'s':s<3600?Math.floor(s/60)+'m':Math.floor(s/3600)+'h';}
let pt=null;
function rs(){api('status').then(r=>r.json()).then(d=>{
$('cur-ver').textContent=d.current_version||'?';$('cur-part').textContent=d.active_partition||'?';
$('next-part').textContent=d.next_partition||'?';$('part-size').textContent=fb(d.partition_size||0);
$('uptime').textContent=ft(d.uptime||0);let si=d.state||0;
$('ota-state').innerHTML='<span class="state-label '+(SC[si]||'state-idle')+'">'+(STATES[si]||'?')+'</span>';
}).catch(()=>{});}
function sm(id,c,m){let e=$(id);e.className='status-msg show '+c;e.textContent=m;}
let sf=null;$('fw-file').onchange=function(){if(this.files.length){sf=this.files[0];
$('file-name').textContent=sf.name;$('file-size').textContent=fb(sf.size);
$('file-info').style.display='block';$('btn-upload').disabled=false;}};
$('btn-upload').onclick=function(){if(!sf)return;this.disabled=true;
$('progress-wrap').classList.add('show');
let xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/upload',true);
xhr.upload.onprogress=function(e){if(e.lengthComputable){let p=Math.round(e.loaded/e.total*100);
$('progress-fill').style.width=p+'%';$('progress-text').textContent=p+'%';}};
xhr.onload=function(){if(xhr.status===200)sm('upload-msg','ok','Done');else sm('upload-msg','err','Failed');
$('btn-upload').disabled=false;rs();};xhr.send(sf);};
$('btn-url').onclick=function(){let u=$('url-input').value.trim();if(!u)return;this.disabled=true;
post('url',{url:u}).then(r=>r.json()).then(d=>{sm('upload-msg',d.ok?'ok':'err',d.msg);
$('btn-url').disabled=false;rs();}).catch(()=>{sm('upload-msg','err','Failed');$('btn-url').disabled=false;});};
$('btn-rollback').onclick=function(){post('rollback').then(r=>r.json()).then(d=>{
sm('upload-msg',d.ok?'ok':'err',d.msg);rs();});};
$('btn-reboot').onclick=function(){post('reboot').then(()=>{sm('upload-msg','ok','Rebooting...');
setTimeout(()=>location.reload(),10000);});};
$('btn-mesh').onclick=function(){post('mesh-push').then(r=>r.json()).then(d=>{
sm('upload-msg',d.ok?'ok':'err',d.msg||'Failed');});};
api('history').then(r=>r.json()).then(es=>{let tb=$('history-body');if(!es.length)return;
let h='';es.forEach(e=>{h+='<tr><td style="color:var(--cyan)">'+e.version+'</td><td>'+
(e.timestamp?ft(e.timestamp):'--')+'</td><td>'+e.source+'</td><td><span class="dot '+
(e.success?'ok':'fail')+'"></span>'+(e.success?'OK':'FAIL')+'</td></tr>';});tb.innerHTML=h;}).catch(()=>{});
rs();setInterval(rs,5000);})();
</script>
</body></html>)rawhtml";

// ── Helpers ──────────────────────────────────────────────────────────────────

static int recvBody(httpd_req_t* req, char* buf, size_t buf_size) {
    int total = 0;
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)buf_size) return -1;
    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf + total, remaining);
        if (recv <= 0) return -1;
        total += recv;
        remaining -= recv;
    }
    buf[total] = '\0';
    return total;
}

static bool jsonExtractString(const char* json, const char* key, char* out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* start = strstr(json, pattern);
    if (!start) return false;
    start += strlen(pattern);
    size_t i = 0;
    while (start[i] && start[i] != '"' && i < out_size - 1) { out[i] = start[i]; i++; }
    out[i] = '\0';
    return i > 0;
}

namespace ota_routes {

// GET /api/ota/status
static esp_err_t handleStatus(httpd_req_t* req) {
    const auto& st = ota_manager::getStatus();
    char json[512];
    snprintf(json, sizeof(json),
             "{\"state\":%u,\"progress\":%u,\"bytes_written\":%u,"
             "\"total_bytes\":%u,\"current_version\":\"%s\","
             "\"new_version\":\"%s\",\"error\":\"%s\","
             "\"active_partition\":\"%s\",\"next_partition\":\"%s\","
             "\"partition_size\":%u,\"uptime\":%lu}",
             (unsigned)st.state, (unsigned)st.progress_pct,
             st.bytes_written, st.total_bytes,
             st.current_version, st.new_version, st.error_msg,
             st.active_partition ? st.active_partition : "?",
             st.next_partition ? st.next_partition : "?",
             st.partition_size,
             (unsigned long)(millis() / 1000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// POST /api/ota/upload — firmware upload (raw binary body)
static esp_err_t handleUpload(httpd_req_t* req) {
    DBG_INFO(TAG, "Upload start: %d bytes", req->content_len);
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : sizeof(buf));
        if (recv <= 0) {
            httpd_resp_set_status(req, "500");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Receive failed\"}", HTTPD_RESP_USE_STRLEN);
        }
        bool is_final = (remaining - recv) <= 0;
        ota_manager::updateFromUpload((uint8_t*)buf, recv, is_final);
        remaining -= recv;
    }
    DBG_INFO(TAG, "Upload end: %d bytes total", req->content_len);
    const auto& st = ota_manager::getStatus();
    httpd_resp_set_type(req, "application/json");
    if (st.state == ota_manager::OTA_READY_REBOOT) {
        return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Upload complete, ready to reboot\"}", HTTPD_RESP_USE_STRLEN);
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
    httpd_resp_set_status(req, "500");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// POST /api/ota/url
static esp_err_t handleUrl(httpd_req_t* req) {
    char body[512];
    if (recvBody(req, body, sizeof(body)) < 0) {
        httpd_resp_set_status(req, "400");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Missing body\"}", HTTPD_RESP_USE_STRLEN);
    }
    char url[256];
    if (!jsonExtractString(body, "url", url, sizeof(url))) {
        httpd_resp_set_status(req, "400");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Missing url field\"}", HTTPD_RESP_USE_STRLEN);
    }
    DBG_INFO(TAG, "URL update: %s", url);
    bool ok = ota_manager::updateFromUrl(url);
    httpd_resp_set_type(req, "application/json");
    if (ok) return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"URL update complete\"}", HTTPD_RESP_USE_STRLEN);
    const auto& st = ota_manager::getStatus();
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
    httpd_resp_set_status(req, "500");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// POST /api/ota/rollback
static esp_err_t handleRollback(httpd_req_t* req) {
    bool ok = ota_manager::rollback();
    httpd_resp_set_type(req, "application/json");
    if (ok) return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Rollback set, reboot to apply\"}", HTTPD_RESP_USE_STRLEN);
    const auto& st = ota_manager::getStatus();
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
    httpd_resp_set_status(req, "500");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// POST /api/ota/reboot
static esp_err_t handleReboot(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    delay(500);
    ota_manager::reboot();
    return ESP_OK;
}

// GET /api/ota/history
static esp_err_t handleHistory(httpd_req_t* req) {
    ota_manager::OtaHistoryEntry entries[5];
    int count = ota_manager::getHistory(entries, 5);
    char json[1024];
    int pos = snprintf(json, sizeof(json), "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"version\":\"%s\",\"timestamp\":%u,\"success\":%s,\"source\":\"%s\"}",
            entries[i].version, entries[i].timestamp,
            entries[i].success ? "true" : "false", entries[i].source);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

// POST /api/ota/mesh-push
static esp_err_t handleMeshPush(httpd_req_t* req) {
    bool ok = ota_manager::meshDistribute();
    httpd_resp_set_type(req, "application/json");
    if (ok) return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Mesh distribution started\"}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_set_status(req, "500");
    return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Mesh distribution failed\"}", HTTPD_RESP_USE_STRLEN);
}

// GET /ota — web UI page
static esp_err_t handlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, OTA_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

// Route registration
void registerRoutes(httpd_handle_t server) {
    if (!server) return;
    ota_manager::init();

    #define REG(uri_str, method_val, handler_fn) do { \
        httpd_uri_t _u = {}; \
        _u.uri = uri_str; \
        _u.method = method_val; \
        _u.handler = handler_fn; \
        _u.user_ctx = nullptr; \
        httpd_register_uri_handler(server, &_u); \
    } while (0)

    REG("/api/ota/status",    HTTP_GET,  handleStatus);
    REG("/api/ota/upload",    HTTP_POST, handleUpload);
    REG("/api/ota/url",       HTTP_POST, handleUrl);
    REG("/api/ota/rollback",  HTTP_POST, handleRollback);
    REG("/api/ota/reboot",    HTTP_POST, handleReboot);
    REG("/api/ota/history",   HTTP_GET,  handleHistory);
    REG("/api/ota/mesh-push", HTTP_POST, handleMeshPush);
    REG("/ota",               HTTP_GET,  handlePage);

    #undef REG
    DBG_INFO(TAG, "OTA routes registered");
}

}  // namespace ota_routes

#else  // SIMULATOR

namespace ota_routes {
void registerRoutes(httpd_handle_t) {}
}

#endif  // SIMULATOR
