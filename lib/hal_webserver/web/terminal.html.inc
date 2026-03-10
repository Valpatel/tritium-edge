R"rawliteral(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Tritium Terminal</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--cyan:#00f0ff;--mag:#ff2a6d;--grn:#05ffa1;--yel:#fcee0a;--void:#0a0a0f;
--surf1:#0e0e14;--ghost:#8888aa;--text:#c8d0dc}
@font-face{font-family:'JB';src:local('JetBrains Mono'),local('Fira Code'),
local('Consolas'),local('Courier New');font-display:swap}
body{background:var(--void);color:var(--cyan);font-family:'JB','Courier New',monospace;
font-size:13px;height:100vh;display:flex;flex-direction:column;overflow:hidden}
#hdr{display:flex;align-items:center;padding:8px 12px;background:var(--surf1);
border-bottom:1px solid rgba(0,240,255,0.12);gap:12px;flex-shrink:0;z-index:10}
#hdr .title{font-size:14px;letter-spacing:0.1em;text-transform:uppercase;
color:var(--cyan);text-shadow:0 0 8px rgba(0,240,255,0.3)}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px}
.dot.on{background:var(--grn);box-shadow:0 0 6px rgba(5,255,161,0.6);
animation:pulse 2s infinite}
.dot.off{background:#333}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
#status-text{color:var(--ghost);font-size:11px}
.spacer{flex:1}
#hdr button{background:transparent;border:1px solid rgba(0,240,255,0.2);
color:var(--cyan);padding:4px 12px;font-family:inherit;font-size:11px;
cursor:pointer;border-radius:3px;text-transform:uppercase;letter-spacing:0.05em}
#hdr button:hover{border-color:var(--cyan);background:rgba(0,240,255,0.08);
box-shadow:0 0 8px rgba(0,240,255,0.15)}
#hdr button.close-btn{border-color:rgba(255,42,109,0.3);color:var(--mag)}
#hdr button.close-btn:hover{border-color:var(--mag);background:rgba(255,42,109,0.08)}
#output{flex:1;overflow-y:auto;padding:8px 12px;line-height:1.6;
word-wrap:break-word;white-space:pre-wrap;scrollbar-width:thin;
scrollbar-color:rgba(0,240,255,0.2) transparent}
#output::-webkit-scrollbar{width:6px}
#output::-webkit-scrollbar-track{background:transparent}
#output::-webkit-scrollbar-thumb{background:rgba(0,240,255,0.2);border-radius:3px}
#output::-webkit-scrollbar-thumb:hover{background:rgba(0,240,255,0.4)}
.line{color:var(--text)}.line:hover{background:rgba(0,240,255,0.03);cursor:pointer}
.tag-tritium{color:var(--cyan)}.tag-fingerprint{color:var(--grn)}
.tag-wifi,.tag-web{color:var(--grn)}.tag-svc{color:var(--yel)}
.tag-cmd{color:var(--ghost)}.tag-error,.tag-E{color:var(--mag)}
.tag-W{color:var(--yel)}.tag-ble{color:#b388ff}.tag-mesh,.tag-espnow{color:#ff9800}
.tag-diag{color:var(--yel)}.tag-sd{color:var(--ghost)}
.tag-heartbeat{color:var(--grn)}.prompt-char{color:var(--mag)}
#input-row{display:flex;align-items:center;padding:6px 12px;
border-top:1px solid rgba(0,240,255,0.12);background:var(--surf1);flex-shrink:0}
#prompt{color:var(--mag);margin-right:8px;font-size:14px;user-select:none}
#cmd{flex:1;background:transparent;border:none;color:var(--cyan);
font-family:inherit;font-size:13px;outline:none;caret-color:var(--cyan)}
#cmd::placeholder{color:rgba(0,240,255,0.2)}
.scanline{position:fixed;top:0;left:0;right:0;bottom:0;pointer-events:none;
background:repeating-linear-gradient(transparent,transparent 2px,
rgba(0,240,255,0.008) 2px,rgba(0,240,255,0.008) 4px);z-index:999}
.toast{position:fixed;top:50px;right:20px;padding:8px 16px;border-radius:4px;
font-size:12px;z-index:1000;animation:fadeIn 0.3s ease;max-width:300px}
.toast.info{background:rgba(0,240,255,0.1);border:1px solid rgba(0,240,255,0.3);color:var(--cyan)}
.toast.error{background:rgba(255,42,109,0.1);border:1px solid rgba(255,42,109,0.3);color:var(--mag)}
.toast.success{background:rgba(5,255,161,0.1);border:1px solid rgba(5,255,161,0.3);color:var(--grn)}
.toast.warning{background:rgba(252,238,10,0.1);border:1px solid rgba(252,238,10,0.3);color:var(--yel)}
@keyframes fadeIn{from{opacity:0;transform:translateY(-10px)}to{opacity:1;transform:none}}
#tab-hint{position:fixed;bottom:40px;left:12px;color:var(--ghost);font-size:11px;
opacity:0;transition:opacity 0.2s;pointer-events:none;z-index:10}
</style>
</head><body>
<div class="scanline"></div>
<div id="hdr">
<span class="title">[ Tritium Terminal ]</span>
<span class="dot off" id="dot"></span>
<span id="status-text">Disconnected</span>
<span class="spacer"></span>
<button onclick="clearOutput()">Clear</button>
<button class="close-btn" onclick="window.close();history.back()">Close</button>
</div>
<div id="output"></div>
<div id="input-row">
<span id="prompt">&gt;</span>
<input id="cmd" type="text" placeholder="Type a command..." autocomplete="off" spellcheck="false">
</div>
<div id="tab-hint"></div>
<script>
(function(){
'use strict';
var ws=null,output=document.getElementById('output'),cmd=document.getElementById('cmd'),
dot=document.getElementById('dot'),statusText=document.getElementById('status-text'),
tabHint=document.getElementById('tab-hint');
var MAX_LINES=500,autoScroll=true,history=[],histIdx=-1,histDraft='';
var CMDS=['IDENTIFY','SERVICES','WIFI_SCAN','WIFI_STATUS','WIFI_LIST','BLE_SCAN',
'BLE_STATUS','SET','GET','REBOOT','OTA_STATUS','SD_FORMAT','MESH_STATUS','MESH_PEERS',
'DIAG','DIAG_HEALTH','DIAG_EVENTS','HELP'];

try{var sh=localStorage.getItem('tritium_cmd_history');
if(sh)history=JSON.parse(sh).slice(-100);}catch(e){}

function saveHistory(){
try{localStorage.setItem('tritium_cmd_history',JSON.stringify(history.slice(-100)));}catch(e){}}

function connect(){
var proto=location.protocol==='https:'?'wss:':'ws:';
var url=proto+'//'+location.host+'/ws';
ws=new WebSocket(url);
ws.onopen=function(){
dot.className='dot on';statusText.textContent='Connected';
ws.send(JSON.stringify({type:'subscribe',channels:['serial','events','status']}));
addLine('[terminal] Connected to '+location.host,'tag-tritium');
};
ws.onclose=function(){
dot.className='dot off';statusText.textContent='Disconnected';
addLine('[terminal] Connection lost. Reconnecting...','tag-error');
setTimeout(connect,2000);
};
ws.onerror=function(){ws.close();};
ws.onmessage=function(e){
try{var msg=JSON.parse(e.data);handleMsg(msg);}catch(ex){}
};
}

function handleMsg(msg){
if(msg.type==='serial'){
var text=msg.data||'';
text=text.replace(/\n$/,'');
if(text)addLine(text);
}else if(msg.type==='event'){
addLine('[event] '+msg.name+(msg.data?' '+JSON.stringify(msg.data):''),'tag-svc');
}else if(msg.type==='toast'){
showToast(msg.message,msg.level||'info');
}else if(msg.type==='pong'){
addLine('[pong] server time: '+msg.time,'tag-cmd');
}else if(msg.type==='error'){
addLine('[error] '+msg.message,'tag-error');
}
}

function addLine(text,cls){
var div=document.createElement('div');
div.className='line'+(cls?' '+cls:'');
if(!cls)div.innerHTML=colorize(escHtml(text));
else div.textContent=text;
div.addEventListener('click',function(){
navigator.clipboard.writeText(this.textContent).then(function(){
showToast('Copied to clipboard','info');
}).catch(function(){});
});
output.appendChild(div);
while(output.children.length>MAX_LINES)output.removeChild(output.firstChild);
if(autoScroll)output.scrollTop=output.scrollHeight;
}

function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}

function colorize(text){
var m=text.match(/^\[([^\]]+)\]/);
if(m){
var tag=m[1].toLowerCase();
var cls='';
if(tag==='e'||tag==='error')cls='tag-error';
else if(tag==='w'||tag==='warn'||tag==='warning')cls='tag-W';
else if(tag==='tritium')cls='tag-tritium';
else if(tag==='fingerprint')cls='tag-fingerprint';
else if(tag==='wifi'||tag==='web'||tag==='webserver')cls='tag-wifi';
else if(tag==='svc')cls='tag-svc';
else if(tag==='cmd')cls='tag-cmd';
else if(tag==='ble'||tag==='ble_scanner')cls='tag-ble';
else if(tag==='mesh'||tag==='espnow')cls='tag-mesh';
else if(tag==='diag')cls='tag-diag';
else if(tag==='sd')cls='tag-sd';
else if(tag==='heartbeat')cls='tag-heartbeat';
else if(tag==='terminal')cls='tag-tritium';
else if(tag==='event')cls='tag-svc';
if(cls){
var prefix=text.substring(0,m[0].length);
var rest=text.substring(m[0].length);
return '<span class="'+cls+'">'+prefix+'</span>'+rest;
}
}
if(text.charAt(0)==='>'&&text.charAt(1)===' '){
return '<span class="prompt-char">&gt;</span>'+text.substring(1);
}
return text;
}

function sendCmd(c){
if(!ws||ws.readyState!==1){addLine('[terminal] Not connected','tag-error');return;}
ws.send(JSON.stringify({type:'serial',data:c+'\n'}));
if(c&&(history.length===0||history[history.length-1]!==c)){
history.push(c);saveHistory();}
histIdx=-1;histDraft='';
}

function clearOutput(){while(output.firstChild)output.removeChild(output.firstChild);}
window.clearOutput=clearOutput;

output.addEventListener('scroll',function(){
var atBottom=output.scrollHeight-output.scrollTop-output.clientHeight<30;
autoScroll=atBottom;
});

function tabComplete(val){
var upper=val.toUpperCase();
return CMDS.filter(function(c){return c.indexOf(upper)===0;});
}

cmd.addEventListener('keydown',function(e){
if(e.key==='Enter'){
var v=cmd.value.trim();
if(v)sendCmd(v);
cmd.value='';tabHint.style.opacity='0';
}else if(e.key==='ArrowUp'){
e.preventDefault();
if(history.length===0)return;
if(histIdx===-1){histDraft=cmd.value;histIdx=history.length-1;}
else if(histIdx>0)histIdx--;
cmd.value=history[histIdx];
}else if(e.key==='ArrowDown'){
e.preventDefault();
if(histIdx===-1)return;
if(histIdx<history.length-1){histIdx++;cmd.value=history[histIdx];}
else{histIdx=-1;cmd.value=histDraft;}
}else if(e.key==='Tab'){
e.preventDefault();
var v=cmd.value.trim();
if(!v)return;
var matches=tabComplete(v);
if(matches.length===1){cmd.value=matches[0];tabHint.style.opacity='0';}
else if(matches.length>1){
tabHint.textContent=matches.join('  ');
tabHint.style.opacity='1';
setTimeout(function(){tabHint.style.opacity='0';},3000);
}
}else if(e.key==='c'&&e.ctrlKey){
e.preventDefault();
sendCmd('\x03');
addLine('^C','tag-cmd');
cmd.value='';
}else if(e.key==='l'&&e.ctrlKey){
e.preventDefault();clearOutput();
}
});

cmd.addEventListener('input',function(){tabHint.style.opacity='0';});

function showToast(msg,level){
var t=document.createElement('div');
t.className='toast '+(level||'info');
t.textContent=msg;
document.body.appendChild(t);
setTimeout(function(){t.style.opacity='0';t.style.transition='opacity 0.3s';
setTimeout(function(){t.remove();},300);},3000);
}

document.body.addEventListener('click',function(e){
if(!window.getSelection().toString()&&e.target.tagName!=='BUTTON')cmd.focus();
});

cmd.focus();
addLine('[terminal] Tritium-OS Web Terminal','tag-tritium');
addLine('[terminal] Connecting...','tag-tritium');
connect();
})();
</script>
</body></html>)rawliteral"
