// -----------------------------------------------------------------------------------
// Built-in pages in English (Console and the PIN page). Used when NS3000_JAPANESE is OFF in Config.h  2026,10/23 NS3000
// Included from WifiWeb.ino. The Japanese version (Lang_ja.h) has the same names.

// Note: do not write "//" or \" (escaped quote) inside these raw strings (the Arduino preprocessor misreads them and the build breaks). Write URLs as https:&#47;&#47;
static const char wwIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LUNA OnStepNinja Console</title>
<style>
:root{--bg:#0b0f17;--card:#151b26;--line:#2a3445;--txt:#e6edf3;--sub:#8b97a8;--acc:#4da3ff;--bad:#f87171}
body{margin:0;background:var(--bg);color:var(--txt);font-family:-apple-system,"Segoe UI",Roboto,sans-serif;padding:14px;max-width:560px;margin:0 auto}
h1{font-size:18px}.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.lbl{font-size:12px;color:var(--sub)}input,button{font:inherit;color:var(--txt);background:#1d2533;border:1px solid var(--line);border-radius:8px;padding:8px}
button{cursor:pointer}a{color:var(--acc)}table{width:100%;border-collapse:collapse}td{padding:4px;border-bottom:1px solid var(--line)}
.err{color:var(--bad)}code{color:var(--sub)}.k{display:inline-block;min-width:9.5em;color:var(--sub)}.ok{color:#34d399}.wn{color:#fbbf24}.sv{display:inline-block;margin-right:1.6em;white-space:nowrap}.foot{font-size:11px;color:var(--sub);text-align:center;margin-top:18px;line-height:1.7}
</style></head><body>
<h1><span style="font-size:22px;white-space:nowrap">LUNA OnStepNinja Console</span> <span id="ver" class="lbl" style="font-size:14px;white-space:nowrap">NS-4000</span></h1>
<div class="card"><div class="lbl">Network Information</div><div id="net" style="line-height:1.8">—</div><div id="hb" style="line-height:1.8"></div></div>
<p class="lbl"><a href="/control.html">NS-4000 Control</a> · <a href="/settings.html">Settings</a> · <a href="/starchart_en.html">Star Chart</a> · <a href="/files">File Manager</a></p>
<div class="card"><div class="lbl">Status</div><div id="st">—</div></div>
<div class="card"><div class="lbl">Files (SPIFFS) <span id="fs"></span></div>
<table id="files"></table>
<div class="lbl">Add or delete files in the <a href="/files">File Manager</a> (open for 30 minutes after power-on).</div></div>
<div class="card"><div class="lbl">Alpaca Discovery (ASCOM Alpaca devices on the LAN)</div>
<p style="margin:8px 0"><button id="axBtn" onclick="alpFind()">&#128269; Search</button> <span id="axMsg" class="lbl"></span></p>
<table id="axList"></table></div>
<p class="foot">LUNA OnStepNinja NS-4000 v1.1.0 · <a href="https:&#47;&#47;github.com/OnStepNinja" target="_blank" rel="noopener">&#127769; LUNA Project</a><br>Based on <a href="https:&#47;&#47;github.com/hjd1964/OnStep" target="_blank" rel="noopener">OnStep</a> (GPL v3), LUNA AiBridge</p>
<script>
const $=id=>document.getElementById(id);
async function api(p,o){const r=await fetch(p,o);const j=await r.json();if(!r.ok||j.status!=="success")throw new Error(j.message||("HTTP "+r.status));return j}
async function poll(){try{const j=await api("/api/status");const d=j.data;$("st").innerHTML="<span class=sv>"+esc(d.ra)+"</span><span class=sv>"+esc(d.dec)+"</span><span class=sv>"+esc(d.state)+"</span>"}catch(e){$("st").textContent="—"}}
async function list(){try{const j=await api("/api/files/list");$("fs").textContent="Used "+j.used+" / "+j.total+" bytes";const t=$("files");t.innerHTML="";
for(const f of j.files){const tr=t.insertRow();const a=document.createElement("a");a.href="/"+f.name;a.textContent=f.name;tr.insertCell().appendChild(a);tr.insertCell().textContent=f.size;
const b=document.createElement("button");b.textContent="Open";b.onclick=()=>window.open("/"+f.name,"_blank");tr.insertCell().appendChild(b)}}catch(e){$("fs").textContent=e.message}}
function esc(t){return String(t==null?"":t).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;")}
function row(k,v){return "<div><span class=k>"+k+"</span><b>"+v+"</b></div>"}
async function info(){try{const d=(await api("/api/system/info")).data;$("ver").textContent=String(d.version).replace(/^LUNA OnStepNinja\s*/,"");
const sta=d.staConnected===false||d.staIp==="0.0.0.0"?"— <span class=wn>Not connected</span>":esc(d.staIp)+" <span class=ok>Connected</span>";
$("net").innerHTML=row("Location Name:",esc(d.location||"—"))+row("AP SSID:",esc(d.apSsid))+row("AP Address:",esc(d.apIp))+row("STA Address:",sta)+row("Command Ports:",esc(d.cmdPorts||"9999, 9998")+" (LX200)")
}catch(e){$("net").textContent="—"}}
async function hbPoll(){try{const h=(await api("/api/handbox")).data;let s;
if(h.state==2){let q="";if(h.rssi!==0){q=h.rssi>=-60?"<span class=ok>Good</span>":h.rssi>=-80?"<span class=wn>Fair</span>":"<span class=err>Weak (near the limit)</span>"}
s=esc(h.addr)+" <span class=ok>Connected</span> "+(h.rssi===0?"(measuring RSSI)":h.rssi+" dBm "+q+" <span class=lbl>weakest "+h.rssiMin+"</span>")
+(h.vbat>0?" &nbsp;Battery "+(h.vbat/1000).toFixed(2)+" V "+(h.vbat>=3000?"":h.vbat>=2900?"<span class=wn>Low</span>":"<span class=err>Replace</span>"):"")}
else if(h.state==1)s="<span class=wn>Registered, not connected</span>";else s="— <span class=wn>Not registered</span>";
$("hb").innerHTML=row("BLE Handbox:",s)}catch(e){$("hb").innerHTML=""}}
async function alpFind(){const b=$("axBtn"),t=$("axList");b.disabled=true;$("axMsg").textContent="Searching... (2-5 s)";t.innerHTML="";
try{const j=await api("/api/external/discover");const L=j.data||[];let n=0;
for(const s of L){const v=s.devices&&s.devices.Value?s.devices.Value:[];const url="http:"+"/"+"/"+s.ip+":"+s.port+"/setup";
const h=t.insertRow();h.innerHTML="<td colspan=3><b>"+esc(s.ip)+":"+esc(s.port)+"</b> <a href='"+esc(url)+"' target=_blank rel=noopener>setup</a></td>";
if(!v.length){const r=t.insertRow();r.innerHTML="<td colspan=3 class=lbl>(could not read the device list)</td>"}
for(const d of v){const r=t.insertRow();r.insertCell().textContent=d.DeviceType;r.insertCell().textContent="#"+d.DeviceNumber;r.insertCell().textContent=d.DeviceName;n++}}
$("axMsg").textContent=L.length?L.length+" Alpaca server(s), "+n+" device(s)":"Nothing found (a firewall on a PC may block its Alpaca server)"}
catch(e){$("axMsg").innerHTML="<span class=err>"+esc(e.message)+"</span>"}b.disabled=false}
info();setInterval(info,15000);hbPoll();setInterval(hbPoll,2000);
poll();list();setInterval(poll,2000);
</script></body></html>)HTML";

// ---- PIN page (same look as AiBridge Host) ----
static const char wwPinHtml[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PIN - LUNA OnStepNinja</title>
<style>body{font-family:-apple-system,"Segoe UI",sans-serif;background:#05080f;color:#e6edf3;max-width:340px;margin:18vh auto;padding:0 20px;text-align:center}
input{font-size:2rem;letter-spacing:.5em;text-align:center;width:100%;padding:10px;margin:16px 0;box-sizing:border-box;background:#0b111c;color:#e6edf3;border:1px solid #2a3445;border-radius:10px}
button{font-size:1rem;padding:10px 28px;border-radius:10px;border:1px solid #2f4a75;background:#16233a;color:#cfe3ff;cursor:pointer}.err{color:#f87171}</style></head>
<body><h2>&#128301; LUNA OnStepNinja</h2><p>Enter the PIN</p>%MSG%
<form method="POST" action="/pin/login"><input type="hidden" name="next" value="%NEXT%">
<input name="pin" type="password" inputmode="numeric" pattern="[0-9]{4}" maxlength="4" autocomplete="off" autofocus>
<button type="submit">OK</button></form></body></html>)HTML";
#define WW_TXT_PIN_LOCKED "Too many wrong PINs. Try again in 5 minutes."
#define WW_TXT_PIN_WRONG  "Wrong PIN"
