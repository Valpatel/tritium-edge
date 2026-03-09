// Tritium-OS OTA Routes — REST API for firmware update management
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ota_routes.h"

#if HAS_ASYNC_WEBSERVER

#include "ota_manager.h"
#include "debug_log.h"
#include <cstdio>
#include <cstring>

static constexpr const char* TAG = "ota_routes";

#ifndef SIMULATOR

#include <Arduino.h>
#include <esp_ota_ops.h>

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
font-size:13px;padding:16px;max-width:720px;margin:0 auto;
background-image:linear-gradient(rgba(0,240,255,0.02) 1px,transparent 1px),
linear-gradient(90deg,rgba(0,240,255,0.02) 1px,transparent 1px);
background-size:80px 80px}
.scanline{position:fixed;top:0;left:0;right:0;bottom:0;pointer-events:none;z-index:999;
background:repeating-linear-gradient(transparent,transparent 2px,rgba(0,240,255,0.008) 2px,rgba(0,240,255,0.008) 4px)}
h1{color:var(--cyan);font-size:16px;letter-spacing:0.15em;text-transform:uppercase;
padding:12px 0;border-bottom:1px solid rgba(0,240,255,0.15);margin-bottom:16px;
text-shadow:0 0 12px rgba(0,240,255,0.3)}
.panel{background:var(--s2);border:1px solid rgba(0,240,255,0.08);border-radius:4px;
padding:14px;margin-bottom:12px;transition:border-color 0.25s,box-shadow 0.25s}
.panel:hover{border-color:rgba(0,240,255,0.25);box-shadow:0 0 12px rgba(0,240,255,0.08)}
.panel-title{color:var(--bright);font-size:11px;letter-spacing:0.12em;
text-transform:uppercase;margin-bottom:10px;display:flex;align-items:center;gap:6px}
.panel-title::before{content:"["}
.panel-title::after{content:"]"}
.info-grid{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;font-size:12px}
.info-grid .k{color:var(--ghost)}
.info-grid .v{color:var(--cyan);font-family:'Courier New',monospace}
.drop-zone{border:2px dashed rgba(0,240,255,0.2);border-radius:6px;padding:32px 16px;
text-align:center;cursor:pointer;transition:all 0.25s;position:relative;background:var(--s1)}
.drop-zone:hover,.drop-zone.over{border-color:var(--cyan);
background:rgba(0,240,255,0.03);box-shadow:0 0 20px rgba(0,240,255,0.08)}
.drop-zone input{position:absolute;inset:0;opacity:0;cursor:pointer}
.drop-zone .label{color:var(--ghost);font-size:13px}
.drop-zone .label span{color:var(--cyan)}
.file-info{margin-top:8px;font-size:11px;color:var(--ghost);display:none}
.file-info.show{display:block}
.progress-wrap{margin:12px 0;display:none}
.progress-wrap.show{display:block}
.progress-bar{height:20px;background:var(--s3);border-radius:3px;overflow:hidden;
border:1px solid rgba(0,240,255,0.1)}
.progress-fill{height:100%;width:0%;background:linear-gradient(90deg,rgba(0,240,255,0.6),var(--cyan));
transition:width 0.3s;position:relative}
.progress-fill::after{content:"";position:absolute;inset:0;
background:linear-gradient(90deg,transparent,rgba(255,255,255,0.1),transparent);
animation:shimmer 1.5s infinite}
@keyframes shimmer{0%{transform:translateX(-100%)}100%{transform:translateX(100%)}}
.progress-text{text-align:center;font-size:12px;margin-top:4px;color:var(--cyan)}
.btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.btn{padding:7px 16px;border:1px solid rgba(0,240,255,0.3);background:transparent;
color:var(--cyan);font-family:inherit;font-size:11px;letter-spacing:0.08em;
text-transform:uppercase;cursor:pointer;border-radius:3px;transition:all 0.25s}
.btn:hover{background:rgba(0,240,255,0.1);box-shadow:0 0 12px rgba(0,240,255,0.15);
transform:translateY(-1px)}
.btn:disabled{opacity:0.4;cursor:not-allowed;transform:none;box-shadow:none}
.btn.danger{border-color:rgba(255,42,109,0.3);color:var(--mag)}
.btn.danger:hover{background:rgba(255,42,109,0.1);box-shadow:0 0 12px rgba(255,42,109,0.15)}
.btn.success{border-color:rgba(5,255,161,0.3);color:var(--green)}
.btn.success:hover{background:rgba(5,255,161,0.1);box-shadow:0 0 12px rgba(5,255,161,0.15)}
table{width:100%;border-collapse:collapse;font-size:12px;margin-top:6px}
th{text-align:left;color:var(--ghost);font-weight:normal;font-size:10px;
text-transform:uppercase;letter-spacing:0.1em;padding:4px 8px;
border-bottom:1px solid rgba(0,240,255,0.08)}
td{padding:5px 8px;border-bottom:1px solid rgba(255,255,255,0.03)}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:6px}
.dot.ok{background:var(--green);box-shadow:0 0 6px rgba(5,255,161,0.4)}
.dot.fail{background:var(--mag);box-shadow:0 0 6px rgba(255,42,109,0.4)}
.dot.pending{background:var(--yellow);box-shadow:0 0 6px rgba(252,238,10,0.4)}
@keyframes breathe{0%,100%{opacity:1}50%{opacity:0.5}}
.breathing{animation:breathe 1.5s ease-in-out infinite}
.url-input{width:100%;padding:7px 10px;background:var(--s1);border:1px solid rgba(0,240,255,0.15);
color:var(--cyan);font-family:inherit;font-size:12px;border-radius:3px;margin-bottom:8px}
.url-input:focus{outline:none;border-color:rgba(0,240,255,0.4);
box-shadow:0 0 8px rgba(0,240,255,0.1)}
.url-input::placeholder{color:var(--ghost);opacity:0.5}
.danger-zone{border-color:rgba(255,42,109,0.15)}
.danger-zone:hover{border-color:rgba(255,42,109,0.3);box-shadow:0 0 12px rgba(255,42,109,0.08)}
.status-msg{padding:8px 12px;border-radius:3px;font-size:12px;margin:8px 0;display:none}
.status-msg.show{display:block}
.status-msg.ok{background:rgba(5,255,161,0.08);border:1px solid rgba(5,255,161,0.2);color:var(--green)}
.status-msg.err{background:rgba(255,42,109,0.08);border:1px solid rgba(255,42,109,0.2);color:var(--mag)}
.mesh-count{font-size:20px;color:var(--cyan);font-weight:bold}
.state-label{display:inline-block;padding:2px 8px;border-radius:2px;font-size:10px;
text-transform:uppercase;letter-spacing:0.1em}
.state-idle{background:rgba(136,136,170,0.15);color:var(--ghost)}
.state-active{background:rgba(0,240,255,0.15);color:var(--cyan)}
.state-ok{background:rgba(5,255,161,0.15);color:var(--green)}
.state-err{background:rgba(255,42,109,0.15);color:var(--mag)}
.modal-bg{position:fixed;inset:0;background:rgba(0,0,0,0.7);backdrop-filter:blur(6px);
display:none;align-items:center;justify-content:center;z-index:100}
.modal-bg.show{display:flex}
.modal{background:var(--s2);border:1px solid rgba(0,240,255,0.2);border-radius:6px;
padding:20px;max-width:360px;width:90%;text-align:center}
.modal h3{color:var(--bright);font-size:13px;margin-bottom:12px;text-transform:uppercase;letter-spacing:0.1em}
.modal p{color:var(--ghost);font-size:12px;margin-bottom:16px}
.modal .btn-row{justify-content:center}
</style>
</head>
<body>
<div class="scanline"></div>
<h1>// Tritium-OS Firmware Update</h1>
<div class="panel">
<div class="panel-title">System Info</div>
<div class="info-grid">
<span class="k">Current</span><span class="v" id="cur-ver">---</span>
<span class="k">Partition</span><span class="v" id="cur-part">---</span>
<span class="k">Next Partition</span><span class="v" id="next-part">---</span>
<span class="k">Partition Size</span><span class="v" id="part-size">---</span>
<span class="k">State</span><span class="v" id="ota-state"><span class="state-label state-idle">IDLE</span></span>
<span class="k">Uptime</span><span class="v" id="uptime">---</span>
</div>
</div>
<div class="panel">
<div class="panel-title">Firmware Upload</div>
<div class="drop-zone" id="drop-zone">
<input type="file" id="fw-file" accept=".bin,.ota">
<div class="label">Drop <span>firmware.bin</span> here or click to browse</div>
</div>
<div class="file-info" id="file-info">
<span id="file-name"></span> &mdash; <span id="file-size"></span>
</div>
<div class="progress-wrap" id="progress-wrap">
<div class="progress-bar"><div class="progress-fill" id="progress-fill"></div></div>
<div class="progress-text" id="progress-text">0%</div>
</div>
<div id="upload-msg" class="status-msg"></div>
<div class="btn-row">
<button class="btn" id="btn-upload" disabled>Upload &amp; Flash</button>
</div>
</div>
<div class="panel">
<div class="panel-title">Update from URL</div>
<input type="text" class="url-input" id="url-input" placeholder="https://example.com/firmware.bin">
<div class="btn-row">
<button class="btn" id="btn-url">Pull Update</button>
</div>
</div>
<div class="panel">
<div class="panel-title">Update History</div>
<table id="history-table">
<thead><tr><th>Version</th><th>Time</th><th>Source</th><th>Status</th></tr></thead>
<tbody id="history-body"><tr><td colspan="4" style="color:var(--ghost)">Loading...</td></tr></tbody>
</table>
</div>
<div class="panel">
<div class="panel-title">Mesh Distribution</div>
<div style="display:flex;align-items:center;gap:16px;margin-bottom:10px">
<div><span class="mesh-count" id="mesh-peers">--</span> <span style="color:var(--ghost);font-size:11px">mesh peers</span></div>
<div style="font-size:11px;color:var(--ghost)">Push current firmware to all connected mesh nodes</div>
</div>
<div class="btn-row">
<button class="btn success" id="btn-mesh">Distribute to Fleet</button>
</div>
</div>
<div class="panel danger-zone">
<div class="panel-title" style="color:var(--mag)">Danger Zone</div>
<div class="btn-row">
<button class="btn danger" id="btn-rollback">Rollback Firmware</button>
<button class="btn danger" id="btn-reboot">Reboot Now</button>
</div>
</div>
<div class="modal-bg" id="modal">
<div class="modal">
<h3 id="modal-title">Confirm</h3>
<p id="modal-text">Are you sure?</p>
<div class="btn-row">
<button class="btn" id="modal-cancel">Cancel</button>
<button class="btn danger" id="modal-confirm">Confirm</button>
</div>
</div>
</div>
<script>
(function(){
const $=s=>document.getElementById(s);
const api=p=>fetch('/api/ota/'+p);
const post=(p,b)=>fetch('/api/ota/'+p,{method:'POST',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):undefined});
const STATES=['IDLE','CHECKING','DOWNLOADING','WRITING','VERIFYING','READY_REBOOT','FAILED'];
const STATE_CLASS=['state-idle','state-active','state-active','state-active','state-active','state-ok','state-err'];
function fmtBytes(b){
if(b>=1048576)return(b/1048576).toFixed(1)+'MB';
if(b>=1024)return(b/1024).toFixed(1)+'KB';
return b+'B';}
function fmtTime(s){
if(s<60)return s+'s';
if(s<3600)return Math.floor(s/60)+'m '+s%60+'s';
return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';}
let pollTimer=null;
function refreshStatus(){
api('status').then(r=>r.json()).then(d=>{
$('cur-ver').textContent=d.current_version||'unknown';
$('cur-part').textContent=d.active_partition||'?';
$('next-part').textContent=d.next_partition||'?';
$('part-size').textContent=fmtBytes(d.partition_size||0);
$('uptime').textContent=fmtTime(d.uptime||0);
let si=d.state||0,sn=STATES[si]||'UNKNOWN',sc=STATE_CLASS[si]||'state-idle';
$('ota-state').innerHTML='<span class="state-label '+sc+'">'+sn+'</span>';
if(si>=2&&si<=4){$('progress-wrap').classList.add('show');let pct=d.progress||0;
$('progress-fill').style.width=pct+'%';
$('progress-text').textContent=pct+'%  '+fmtBytes(d.bytes_written)+' / '+fmtBytes(d.total_bytes);}
if(si===5){showMsg('upload-msg','ok','Firmware ready. Reboot to apply.');stopPoll();}
if(si===6){showMsg('upload-msg','err','Error: '+(d.error||'unknown'));stopPoll();}
}).catch(()=>{});}
function startPoll(){if(!pollTimer)pollTimer=setInterval(refreshStatus,1500);}
function stopPoll(){if(pollTimer){clearInterval(pollTimer);pollTimer=null;}}
function showMsg(id,cls,msg){let el=$(id);el.className='status-msg show '+cls;el.textContent=msg;}
let selectedFile=null;
const dz=$('drop-zone'),fi=$('fw-file');
fi.addEventListener('change',function(){if(fi.files.length)selectFile(fi.files[0]);});
dz.addEventListener('dragover',function(e){e.preventDefault();dz.classList.add('over');});
dz.addEventListener('dragleave',function(){dz.classList.remove('over');});
dz.addEventListener('drop',function(e){e.preventDefault();dz.classList.remove('over');
if(e.dataTransfer.files.length)selectFile(e.dataTransfer.files[0]);});
function selectFile(f){selectedFile=f;$('file-name').textContent=f.name;
$('file-size').textContent=fmtBytes(f.size);$('file-info').classList.add('show');
$('btn-upload').disabled=false;$('upload-msg').classList.remove('show');}
$('btn-upload').addEventListener('click',function(){
if(!selectedFile)return;let btn=this;btn.disabled=true;
$('progress-wrap').classList.add('show');$('progress-fill').style.width='0%';
$('progress-text').textContent='0%';$('upload-msg').classList.remove('show');
let fd=new FormData();fd.append('firmware',selectedFile);
let xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/upload',true);
xhr.upload.addEventListener('progress',function(e){if(e.lengthComputable){
let pct=Math.round(e.loaded/e.total*100);$('progress-fill').style.width=pct+'%';
$('progress-text').textContent=pct+'%  '+fmtBytes(e.loaded)+' / '+fmtBytes(e.total);}});
xhr.onreadystatechange=function(){if(xhr.readyState===4){
if(xhr.status===200){showMsg('upload-msg','ok','Upload complete. Ready to reboot.');
$('progress-fill').style.width='100%';$('progress-text').textContent='100%  Complete';
}else{let msg='Upload failed';try{msg=JSON.parse(xhr.responseText).msg||msg;}catch(e){}
showMsg('upload-msg','err',msg);}btn.disabled=false;refreshStatus();}};
xhr.send(fd);startPoll();});
$('btn-url').addEventListener('click',function(){
let url=$('url-input').value.trim();if(!url){$('url-input').focus();return;}
this.disabled=true;$('progress-wrap').classList.add('show');startPoll();
post('url',{url:url}).then(r=>r.json()).then(d=>{
if(d.ok)showMsg('upload-msg','ok',d.msg);else showMsg('upload-msg','err',d.msg);
$('btn-url').disabled=false;refreshStatus();
}).catch(()=>{showMsg('upload-msg','err','Request failed');$('btn-url').disabled=false;});});
function loadHistory(){
api('history').then(r=>r.json()).then(entries=>{
let tb=$('history-body');
if(!entries.length){tb.innerHTML='<tr><td colspan="4" style="color:var(--ghost)">No history</td></tr>';return;}
let html='';entries.forEach(function(e){let dotCls=e.success?'ok':'fail';
let statusTxt=e.success?'OK':'FAILED';let time=e.timestamp?fmtTime(e.timestamp):'--';
html+='<tr><td style="color:var(--cyan)">'+e.version+'</td><td>'+time+'</td>';
html+='<td>'+e.source+'</td><td><span class="dot '+dotCls+'"></span>'+statusTxt+'</td></tr>';});
tb.innerHTML=html;}).catch(()=>{
$('history-body').innerHTML='<tr><td colspan="4" style="color:var(--mag)">Load failed</td></tr>';});}
$('btn-mesh').addEventListener('click',function(){
confirm_action('Distribute Firmware','Push current firmware to all mesh peers?',function(){
post('mesh-push').then(r=>r.json()).then(d=>{
if(d.ok)showMsg('upload-msg','ok','Mesh distribution started');
else showMsg('upload-msg','err',d.msg||'Failed');
}).catch(()=>showMsg('upload-msg','err','Request failed'));});});
$('btn-rollback').addEventListener('click',function(){
confirm_action('Rollback Firmware','Revert to previous firmware partition?',function(){
post('rollback').then(r=>r.json()).then(d=>{
if(d.ok)showMsg('upload-msg','ok',d.msg);else showMsg('upload-msg','err',d.msg||'Failed');
refreshStatus();}).catch(()=>showMsg('upload-msg','err','Request failed'));});});
$('btn-reboot').addEventListener('click',function(){
confirm_action('Reboot Device','Reboot now? Active connections will be lost.',function(){
post('reboot').then(()=>{showMsg('upload-msg','ok','Rebooting... page will reload in 10s');
setTimeout(()=>location.reload(),10000);
}).catch(()=>showMsg('upload-msg','err','Request failed'));});});
let modalCb=null;
function confirm_action(title,text,cb){$('modal-title').textContent=title;
$('modal-text').textContent=text;$('modal').classList.add('show');modalCb=cb;}
$('modal-cancel').addEventListener('click',function(){$('modal').classList.remove('show');modalCb=null;});
$('modal-confirm').addEventListener('click',function(){$('modal').classList.remove('show');if(modalCb)modalCb();modalCb=null;});
$('modal').addEventListener('click',function(e){if(e.target===$('modal')){$('modal').classList.remove('show');modalCb=null;}});
refreshStatus();loadHistory();setInterval(refreshStatus,5000);
})();
</script>
</body>
</html>)rawhtml";

namespace ota_routes {

// ---------------------------------------------------------------------------
// GET /api/ota/status
// ---------------------------------------------------------------------------
static void handleStatus(AsyncWebServerRequest* request) {
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

    request->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// POST /api/ota/upload — multipart firmware upload
// ---------------------------------------------------------------------------
static void handleUploadDone(AsyncWebServerRequest* request) {
    const auto& st = ota_manager::getStatus();
    if (st.state == ota_manager::OTA_READY_REBOOT) {
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"Upload complete, ready to reboot\"}");
    } else {
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
        request->send(500, "application/json", json);
    }
}

static void handleUploadData(AsyncWebServerRequest* request,
                              const String& filename, size_t index,
                              uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        DBG_INFO(TAG, "Upload start: %s", filename.c_str());
    }

    ota_manager::updateFromUpload(data, len, final);

    if (final) {
        DBG_INFO(TAG, "Upload end: %u bytes total", (unsigned)(index + len));
    }
}

// ---------------------------------------------------------------------------
// POST /api/ota/url — pull update from URL
// ---------------------------------------------------------------------------
static void handleUrl(AsyncWebServerRequest* request) {
    if (!request->hasParam("body", true)) {
        request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing body\"}");
        return;
    }

    String body = request->getParam("body", true)->value();

    // Simple JSON extraction for "url" field
    int urlStart = body.indexOf("\"url\"");
    if (urlStart < 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"msg\":\"Missing url field\"}");
        return;
    }

    // Find the value string
    int colonPos = body.indexOf(':', urlStart);
    int quoteStart = body.indexOf('"', colonPos + 1);
    int quoteEnd = body.indexOf('"', quoteStart + 1);
    if (quoteStart < 0 || quoteEnd < 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"msg\":\"Invalid url value\"}");
        return;
    }

    String url = body.substring(quoteStart + 1, quoteEnd);
    DBG_INFO(TAG, "URL update: %s", url.c_str());

    // Run URL update (blocking — consider async in future)
    bool ok = ota_manager::updateFromUrl(url.c_str());
    if (ok) {
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"URL update complete\"}");
    } else {
        const auto& st = ota_manager::getStatus();
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
        request->send(500, "application/json", json);
    }
}

// ---------------------------------------------------------------------------
// POST /api/ota/rollback
// ---------------------------------------------------------------------------
static void handleRollback(AsyncWebServerRequest* request) {
    bool ok = ota_manager::rollback();
    if (ok) {
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"Rollback set, reboot to apply\"}");
    } else {
        const auto& st = ota_manager::getStatus();
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
        request->send(500, "application/json", json);
    }
}

// ---------------------------------------------------------------------------
// POST /api/ota/reboot
// ---------------------------------------------------------------------------
static void handleReboot(AsyncWebServerRequest* request) {
    request->send(200, "application/json",
                  "{\"ok\":true,\"msg\":\"Rebooting...\"}");
    // Delay to let response send, then reboot
    delay(500);
    ota_manager::reboot();
}

// ---------------------------------------------------------------------------
// GET /api/ota/history
// ---------------------------------------------------------------------------
static void handleHistory(AsyncWebServerRequest* request) {
    ota_manager::OtaHistoryEntry entries[5];
    int count = ota_manager::getHistory(entries, 5);

    char json[1024];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "[");

    for (int i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"version\":\"%s\",\"timestamp\":%u,"
                        "\"success\":%s,\"source\":\"%s\"}",
                        entries[i].version, entries[i].timestamp,
                        entries[i].success ? "true" : "false",
                        entries[i].source);
    }

    pos += snprintf(json + pos, sizeof(json) - pos, "]");
    request->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// POST /api/ota/mesh-push
// ---------------------------------------------------------------------------
static void handleMeshPush(AsyncWebServerRequest* request) {
    bool ok = ota_manager::meshDistribute();
    if (ok) {
        request->send(200, "application/json",
                      "{\"ok\":true,\"msg\":\"Mesh distribution started\"}");
    } else {
        request->send(500, "application/json",
                      "{\"ok\":false,\"msg\":\"Mesh distribution failed\"}");
    }
}

// ---------------------------------------------------------------------------
// GET /ota — web UI page
// ---------------------------------------------------------------------------
static void handlePage(AsyncWebServerRequest* request) {
    request->send_P(200, "text/html", OTA_PAGE_HTML);
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------
void registerRoutes(AsyncWebServer* server) {
    if (!server) return;

    ota_manager::init();

    server->on("/api/ota/status", HTTP_GET, handleStatus);
    server->on("/api/ota/upload", HTTP_POST, handleUploadDone, handleUploadData);
    server->on("/api/ota/url", HTTP_POST, handleUrl);
    server->on("/api/ota/rollback", HTTP_POST, handleRollback);
    server->on("/api/ota/reboot", HTTP_POST, handleReboot);
    server->on("/api/ota/history", HTTP_GET, handleHistory);
    server->on("/api/ota/mesh-push", HTTP_POST, handleMeshPush);
    server->on("/ota", HTTP_GET, handlePage);

    DBG_INFO(TAG, "OTA routes registered");
}

}  // namespace ota_routes

#else  // SIMULATOR

namespace ota_routes {
void registerRoutes(AsyncWebServer*) {}
}

#endif  // SIMULATOR

#endif  // HAS_ASYNC_WEBSERVER
