// -----------------------------------------------------------------------------------
// Web (port 80) and SPIFFS (AiBridge features merged, stage 3-1)  2026,10/21 NS3000
//
// - Same endpoints as AiBridge v7.15: /api/command?cmd=Xx  /api/status  /api/goto?ra=&dec=  /api/stop
//   Same reply format {"status":"success","message":"...","timestamp":n,"data":...}. NS-4000 Control (control.html, originally index2.html of AiBridge) works as is
// - File management: /api/files/list, /api/files/upload (POST, multipart), /api/files/delete?filename=
//   Writing and deleting are refused during GOTO (interrupts stop while the flash is written)
// - Guard (wwGuard; same rules as guard.go of AiBridge Host, decided by the Origin and Sec-Fetch-Site headers browsers add):
//   1. Refuse unless the Host header is an IP address, a name without '.', or *.local (against DNS rebinding; also applied to pages)
//   2. Allow if the Origin is in NS3000_WEB_ALLOW_ORIGINS
//   3. Allow if Sec-Fetch-Site is same-origin / none (pages served by this device, typed in the address bar)
//   4. Other browser API requests (pages of other sites) need the PIN (?pin= or the X-PIN header)
//   5. Old browsers without Sec-Fetch-Site: with an Origin, allow only our own; without one, treat as a non-browser tool and allow
//   Never reply Access-Control-Allow-Origin: *. Only the Origin that was allowed is returned
//   (TCP 9999/9998 cannot carry a PIN because of how LX200 works. Same as AiBridge)
// - /mcp: file and system information tools for LUNA Host Lab, in the same form as LUNA (wwMcp below)
// - Other paths serve files from SPIFFS. "/" is index.html, or the small built-in page if there is none
// - wifiWebPoll() is called from the Wi-Fi task (core 0). Commands to OnStep go to core 1 with wbCommand() (same path as TCP)

#if NS3000_WIFI == ON && NS3000_WEB == ON

#include <WebServer.h>
#include <SPIFFS.h>

#define WW_CLIENT 2   // wbCommand() client number (0/1 are TCP)

WebServer web(80);
bool wwSpiffsOk=false;

// ---- Config file /aibridge_net_cfg.dat (same name and format as AiBridge: SSID= PASSWORD= LOCATION=, plus PIN=)  2026,10/22 NS3000 ----
// - Read at boot. If missing, the Config.h values are used (NS3000_WIFI_STA_SSID / _PASS, NS3000_WEB_PIN)
// - Readable and writable with /api/wifi/config only for 30 minutes after power-on (same time as the AiBridge File Manager). After that nobody can touch it. A power cycle opens it again
// - The file can never be read from the Web (hidden in the file list, download, delete, /mcp and /api/ui/upload; wwSafeName returns "")
// - PIN (any 4 digits; empty = no PIN), the same idea as AiBridge Host / Host Lab:
//     When set, this device's pages (browser) show the PIN page -> session cookie (HttpOnly, SameSite=Strict). Changing the PIN logs everyone out
//     Host and Host Lab pages (NS3000_WEB_ALLOW_ORIGINS), non-browser tools and TCP 9999/9998 need no PIN
//     Pages of other sites need ?pin= or the X-PIN / X-AiBridge-Pin / X-HostLab-Pin header
//     After 5 wrong PINs that client is refused for 5 minutes
#include "mbedtls/md.h"
#define NS_CFG_FILE       "/aibridge_net_cfg.dat"
#define NS_CFG_WINDOW_MS  (30UL * 60UL * 1000UL)
#define NS_PIN_COOKIE     "ns4000_session"
static String nsSsid = NS3000_WIFI_STA_SSID, nsPass = NS3000_WIFI_STA_PASS, nsLoc = "NS4000", nsPin = NS3000_WEB_PIN;

static bool wwCfgOpen() { return millis() < NS_CFG_WINDOW_MS; }
static unsigned long wwCfgLeftSec() { return wwCfgOpen() ? (NS_CFG_WINDOW_MS - millis()) / 1000UL : 0; }

// Called from the Wi-Fi task at boot (before connecting to Wi-Fi). SPIFFS is mounted here too
void nsCfgLoad() {
  wwSpiffsOk = SPIFFS.begin(true);  // formats automatically the first time (a few seconds; at boot the mount is idle, so tracking is not affected)
  if (!wwSpiffsOk || !SPIFFS.exists(NS_CFG_FILE)) return;
  File f = SPIFFS.open(NS_CFG_FILE, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.startsWith("SSID=")) nsSsid = line.substring(5);
    else if (line.startsWith("PASSWORD=")) nsPass = line.substring(9);
    else if (line.startsWith("LOCATION=")) nsLoc = line.substring(9);
    else if (line.startsWith("PIN=")) nsPin = line.substring(4);
  }
  f.close();
  if (!(nsPin.length() == 0 || (nsPin.length() == 4 && isdigit(nsPin[0]) && isdigit(nsPin[1]) && isdigit(nsPin[2]) && isdigit(nsPin[3])))) nsPin = "";  // broken value: no PIN
}
const char *nsCfgSsid() { return nsSsid.c_str(); }
const char *nsCfgPass() { return nsPass.c_str(); }

static bool wwCfgSave(const String &ssid, const String &pass, const String &loc, const String &pin) {
  File f = SPIFFS.open(NS_CFG_FILE, "w");
  if (!f) return false;
  f.println("SSID=" + ssid);
  f.println("PASSWORD=" + pass);
  f.println("LOCATION=" + loc);
  f.println("PIN=" + pin);
  f.close();
  return true;
}

// Session cookie: SHA-256 (chip MAC + PIN). It changes with the PIN, so everyone's cookie becomes invalid
static String wwToken() {
  if (nsPin.length() == 0) return "";
  uint64_t mac = ESP.getEfuseMac();
  char in[64]; snprintf(in, sizeof(in), "%08lx%08lx:%s:ns4000-session", (unsigned long)(mac >> 32), (unsigned long)(mac & 0xFFFFFFFF), nsPin.c_str());
  unsigned char out[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)in, strlen(in), out);
  char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  return String(hex);
}
static bool wwSameStr(const String &a, const String &b) {  // with the same length, compare every character (do not exit early)
  if (a.length() != b.length() || a.length() == 0) return false;
  uint8_t d = 0; for (unsigned int i = 0; i < a.length(); i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}
static bool wwCookieOk() {
  if (nsPin.length() == 0) return true;
  String c = web.header("Cookie"), key = String(NS_PIN_COOKIE) + "=";
  int i = c.indexOf(key); if (i < 0) return false;
  int e = c.indexOf(';', i); String v = c.substring(i + key.length(), e < 0 ? c.length() : e); v.trim();
  return wwSameStr(v, wwToken());
}
static void wwSetCookie() {
  if (nsPin.length() == 0) return;
  web.sendHeader("Set-Cookie", String(NS_PIN_COOKIE) + "=" + wwToken() + "; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict");
}

// Counting wrong PINs (per client IP; 5 wrong = refused for 5 minutes)
struct wwFail_t { uint32_t ip; uint8_t n; unsigned long t; };
static wwFail_t wwFails[8];
static uint32_t wwIp() { return (uint32_t)web.client().remoteIP(); }
// Slot number for the client IP (-1 if none; with create, the oldest slot is reused). Returns a number because a function returning a struct collides with the prototypes Arduino generates
static int wwFailSlot(bool create) {
  uint32_t ip = wwIp(); int old = 0;
  for (int i = 0; i < 8; i++) { if (wwFails[i].ip == ip) return i; if (wwFails[i].t < wwFails[old].t) old = i; }
  if (!create) return -1;
  wwFails[old].ip = ip; wwFails[old].n = 0; wwFails[old].t = millis();
  return old;
}
static bool wwPinLocked() { int i = wwFailSlot(false); return i >= 0 && wwFails[i].n >= 5 && millis() - wwFails[i].t < 300000UL; }
static void wwPinFailed() { int i = wwFailSlot(true); if (millis() - wwFails[i].t > 300000UL) wwFails[i].n = 0; if (wwFails[i].n < 255) wwFails[i].n++; wwFails[i].t = millis(); }
static bool wwPinMatch(const String &v) {
  if (nsPin.length() == 0 || v.length() == 0) return false;
  if (wwPinLocked()) return false;
  if (wwSameStr(v, nsPin)) return true;
  wwPinFailed();
  return false;
}
// Is it a browser? It has Origin or Sec-Fetch-Site, or the User-Agent starts with "Mozilla/" (some old browsers send no Sec-Fetch-Site)  2026,10/22
// curl, the PC side of Host Lab (Go), Python etc. do not say "Mozilla/", so they stay non-browser tools that need no PIN
// The File Manager page and the endpoints it uses. Usable without the PIN for 30 minutes after power-on (if the PIN is forgotten, a power cycle helps)  2026,10/22
static bool wwFmPath() {
  String u = web.uri();
  if (u == "/files" || u == "/api/wifi/config" || u == "/api/system/info" || u == "/api/system/format_spiffs"
      || u == "/api/files/list" || u == "/api/files/upload" || u == "/api/files/delete" || u == "/api/handbox/forget") return true;
  // only commands sent from the File Manager "LX200 Command Test" (Referer /files). /api/command from other pages needs the PIN  2026,10/22
  if (u == "/api/command") {
    String ref = web.header("Referer"); int a = ref.indexOf("://"); int b = a < 0 ? -1 : ref.indexOf('/', a + 3);
    String path = b < 0 ? "" : ref.substring(b); int q = path.indexOf('?'); if (q >= 0) path = path.substring(0, q);
    return path == "/files";
  }
  return false;
}
// May files be written or deleted? (call after wwGuard) Yes during the 30 minutes. After that, refuse this device's own pages (browser)
// and allow only Host / Host Lab pages (allow list) and non-browser tools (e.g. the PC side of Host Lab). Host and Host Lab are not restricted  2026,10/22
static bool wwFileWriteOk() {
  if (wwCfgOpen()) return true;
  if (!wwIsBrowser()) return true;
  if (wwOriginListed(web.header("Origin"))) return true;  // from a Host / Host Lab page
  String ref = web.header("Referer"); int a = ref.indexOf("://"); int b = a < 0 ? -1 : ref.indexOf('/', a + 3);
  return wwOriginListed(b < 0 ? ref : ref.substring(0, b));
}
static bool wwIsBrowser() {
  return web.header("Origin").length() > 0 || web.header("Sec-Fetch-Site").length() > 0 || web.header("User-Agent").startsWith("Mozilla/");
}

// ---- Built-in pages and their texts (Console, PIN page): Lang_en.h (English) or Lang_ja.h (Japanese)  2026,10/23 NS3000 ----
// The Japanese file is not part of the public (English) release. Choose with NS3000_JAPANESE in Config.h.
#if NS3000_JAPANESE == ON
  #include "Lang_ja.h"
#else
  #include "Lang_en.h"
#endif

// ---- Building replies (same format as createJsonResponse of AiBridge) ----
static String wwEsc(const String &s) {
  String o; o.reserve(s.length()+4);
  for (unsigned int i=0; i<s.length(); i++) {
    char c=s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b,sizeof(b),"\\u%04x",(uint8_t)c); o += b; }
    else o += c;
  }
  return o;
}
static String wwJson(const char *status, const String &message, const String &dataJson) {
  String j = "{\"status\":\""; j += status; j += "\",\"message\":\""; j += wwEsc(message);
  j += "\",\"timestamp\":"; j += String(millis());
  if (dataJson.length() > 0) { j += ",\"data\":"; j += dataJson; }
  j += "}";
  return j;
}
static String wwAllowOrigin = "";  // Origin of another page allowed by wwGuardCheck() (returned in the reply)
static String wwGuardError = "";
static void wwSend(int code, const String &json) {
  if (wwAllowOrigin.length() > 0) { web.sendHeader("Access-Control-Allow-Origin", wwAllowOrigin); web.sendHeader("Vary", "Origin"); }
  web.sendHeader("Cache-Control", "no-store");
  web.send(code, "application/json", json);
}
static void wwOk(const String &message, const String &dataJson) { wwSend(200, wwJson("success", message, dataJson)); }
static void wwErr(int code, const String &message) { wwSend(code, wwJson("error", message, "")); }

// Send one LX200 command and get the reply (: and # are added)
static bool wwLx(const String &body, String &reply) {
  char cmd[WB_CMD_MAX], rep[WB_REPLY_MAX];
  snprintf(cmd, sizeof(cmd), ":%s#", body.c_str());
  bool ok = wbCommand(WW_CLIENT, cmd, rep, sizeof(rep));
  reply = String(rep);
  return ok;
}
static String wwStrip(String s) { s.replace("#",""); s.trim(); return s; }

// Is the PIN from ?pin= or the X-PIN / X-AiBridge-Pin / X-HostLab-Pin header correct? (always false when no PIN is set)
static bool wwPinOk() {
  String v = web.hasArg("pin") ? web.arg("pin") : "";
  if (v.length() == 0) v = web.header("X-PIN");
  if (v.length() == 0) v = web.header("X-AiBridge-Pin");
  if (v.length() == 0) v = web.header("X-HostLab-Pin");
  return wwPinMatch(v);
}

// ---- Guard ----
static bool wwHostOk() {  // rule 1
  String h = web.hostHeader(); h.toLowerCase();
  int c = h.lastIndexOf(':'); if (c > 0) h = h.substring(0, c);
  if (h.length() == 0) return true;  // HTTP/1.0 etc. (not a browser)
  bool ip = true; for (unsigned int i=0; i<h.length(); i++) if (!isdigit(h[i]) && h[i] != '.') { ip = false; break; }
  if (ip) return true;
  if (h.indexOf('.') < 0) return true;
  return h.endsWith(".local");
}
static String wwNormOrigin(String o) { o.trim(); o.toLowerCase(); while (o.endsWith("/")) o.remove(o.length()-1); return o; }
static bool wwOriginListed(const String &o) {  // rule 2
  if (o.length() == 0) return false;
  String list = String(" ") + NS3000_WEB_ALLOW_ORIGINS + " "; list.toLowerCase();
  return list.indexOf(" " + wwNormOrigin(o) + " ") >= 0;
}
// true if allowed. The reason for refusing is in wwGuardError (and wwGuardCode); the Origin to return is in wwAllowOrigin
// 2026,10/22: once the user sets a PIN, API requests from this device's pages also need the session cookie (same as AiBridge Host)
static int wwGuardCode = 403;
static bool wwGuardCheck() {
  wwAllowOrigin = ""; wwGuardError = ""; wwGuardCode = 403;
  if (!wwHostOk()) { wwGuardError = "Forbidden host name"; return false; }
  String origin = web.header("Origin"), site = web.header("Sec-Fetch-Site");
  site.toLowerCase();
  String o = wwNormOrigin(origin), self = wwNormOrigin("http://" + web.hostHeader());
  if (wwOriginListed(origin)) { wwAllowOrigin = origin; return true; }        // 2. Host / Host Lab page
  if (!wwIsBrowser()) return true;                                             // non-browser tools (PC side of Host Lab, curl)
  // Chrome sends no Sec-Fetch-Site to http:// pages either (only HTTPS and localhost). A same-origin GET has no Origin either, so use the Referer  2026,10/22
  String ref = web.header("Referer"), refO = "";
  if (ref.length() > 0) { int a = ref.indexOf("://"); int b = a < 0 ? -1 : ref.indexOf('/', a + 3); refO = wwNormOrigin(b < 0 ? ref : ref.substring(0, b)); }
  bool refOther = refO.length() > 0 && refO != self;                                        // came from a page of another site (e.g. an image link)
  if (refOther && wwOriginListed(refO)) { return true; }                                     // from a Host / Host Lab page
  bool own = site == "same-origin" || site == "none" || (site.length() == 0 && !refOther && (o.length() == 0 || o == self));   // 3, 5. this device's own pages
  if (own) {
    if (wwCookieOk() || wwPinOk()) return true;                                // no PIN set, or a valid cookie or PIN
    if (wwCfgOpen() && wwFmPath()) return true;                                // for 30 minutes after power-on the File Manager needs no PIN
    wwGuardError = "PIN required"; wwGuardCode = 401; return false;
  }
  if (wwPinOk()) { if (origin.length() > 0) wwAllowOrigin = origin; return true; }  // 4. page of another site
  wwGuardError = "PIN required (request from another site)";
  return false;
}
// Call first in every API endpoint. When refusing, replies 403 (401 when the PIN is needed) and returns false
static bool wwGuard() {
  if (wwGuardCheck()) return true;
  wwErr(wwGuardCode, wwGuardError);
  return false;
}
// Guard for pages (HTML etc.): check the host name and, if a PIN is set, require the browser's session cookie for this device's pages (otherwise the PIN page)
static String wwUrlEnc(const String &s) {
  String o; const char *hx = "0123456789ABCDEF";
  for (unsigned int i = 0; i < s.length(); i++) { char c = s[i];
    if (isalnum((unsigned char)c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~') o += c; else { o += '%'; o += hx[(c >> 4) & 15]; o += hx[c & 15]; } }
  return o;
}
static bool wwPageGuard() {
  if (!wwHostOk()) { web.send(403, "text/plain", "Forbidden host name"); return false; }
  if (nsPin.length() == 0 || !wwIsBrowser() || wwCookieOk() || wwOriginListed(web.header("Origin"))) return true;
  if (wwCfgOpen() && wwFmPath()) return true;  // for 30 minutes after power-on the File Manager page needs no PIN
  web.sendHeader("Location", "/pin?next=" + wwUrlEnc(web.uri()));
  web.send(303, "text/plain", "PIN required");
  return false;
}

// Is a GOTO in progress? (moving if :GU# has no 'N')
static bool wwSlewing() {
  String gu; if (!wwLx("GU", gu)) return true;  // if unknown, assume moving (safe side)
  return gu.indexOf('N') < 0;
}

// ---- /api/command?cmd=GR ----
static void wwCommand() {
  if (!wwGuard()) return;
  if (!web.hasArg("cmd")) { wwErr(400, "Missing cmd parameter"); return; }
  String c = web.arg("cmd"); c.trim();
  if (c.startsWith(":")) c = c.substring(1);
  if (c.endsWith("#")) c = c.substring(0, c.length()-1);
  if (c.length() == 0 || c.length() > WB_CMD_MAX-4) { wwErr(400, "Bad cmd"); return; }
  String r;
  if (!wwLx(c, r)) { wwErr(504, "No reply from OnStep"); return; }
  wwOk("Command executed", "\"" + wwEsc(r) + "\"");
}

// ---- /api/status (the form control.html uses) ----
static void wwStatus() {
  if (!wwGuard()) return;
  String ra, dec, gu;
  if (!wwLx("GR", ra) || !wwLx("GD", dec) || !wwLx("GU", gu)) { wwErr(504, "No reply from OnStep"); return; }
  gu = wwStrip(gu);
  const char *state;
  if (gu.indexOf('P') >= 0) state = "Parked";
  else if (gu.indexOf('N') < 0) state = "Slewing";
  else if (gu.indexOf('n') < 0) state = "Tracking";
  else state = "Stopped";
  String d = "{\"ra\":\"" + wwEsc(wwStrip(ra)) + "\",\"dec\":\"" + wwEsc(wwStrip(dec)) + "\",\"state\":\"" + state + "\",\"status\":\"" + wwEsc(gu) + "\",\"connected\":true}";
  wwOk("Status retrieved", d);
}

// ---- /api/goto?ra=HH:MM:SS&dec=sDD*MM:SS ----
static void wwGoto() {
  if (!wwGuard()) return;
  if (!web.hasArg("ra") || !web.hasArg("dec")) { wwErr(400, "Missing ra or dec parameter"); return; }
  String ra = web.arg("ra"), dec = web.arg("dec"), r1, r2, ms;
  ra.trim(); dec.trim();
  if (!dec.startsWith("+") && !dec.startsWith("-")) dec = "+" + dec;
  if (!wwLx("Sr" + ra, r1) || !wwLx("Sd" + dec, r2)) { wwErr(504, "No reply from OnStep"); return; }
  if (wwStrip(r1) != "1" || wwStrip(r2) != "1") { wwErr(400, "Bad ra or dec (OnStep refused)"); return; }
  if (!wwLx("MS", ms)) { wwErr(504, "No reply from OnStep"); return; }
  ms = wwStrip(ms);
  if (ms.startsWith("0")) wwOk("GOTO command sent", "\"RA:" + wwEsc(ra) + " DEC:" + wwEsc(dec) + "\"");
  else wwErr(409, "GOTO refused by OnStep (:MS# = " + ms + ")");  // 1=below horizon 2=above limit 3=standby 4=parked 5=GOTO in progress 6=out of range etc.
}

// ---- /api/stop ----
static void wwStop() {
  if (!wwGuard()) return;
  String r; wwLx("Q", r);
  wwOk("Emergency stop executed", "");
}

// ---- /api/buzzer (same as AiBridge; used by the bell button of the star chart)  2026,10/21 NS3000 ----
static void wwBuzzer() {
  if (!wwGuard()) return;
  String r; wwLx("=SD", r);
  wwOk("Buzzer activated", "");
}

// ---- /api/ui/upload?filename=/x.json (same as AiBridge; saves the POST body as a file; the star chart saves its settings with it)  2026,10/21 NS3000 ----
static void wwUiUpload() {
  if (!wwGuard()) return;
  if (!wwSpiffsOk) { wwErr(500, "SPIFFS not mounted"); return; }
  String n = wwSafeName(web.hasArg("filename") ? web.arg("filename") : "index2.html");  // default name is the same as AiBridge
  if (n == "") { wwErr(400, "Bad filename"); return; }
  String body = web.arg("plain");
  if (body.length() == 0) { wwErr(400, "Empty body"); return; }
  if (body.length() > 16384) { wwErr(413, "Too large (max 16KB). Use /api/files/upload"); return; }
  if (wwSlewing()) { wwErr(409, "Busy: GOTO in progress"); return; }
  File f = SPIFFS.open(n, "w");
  if (!f) { wwErr(500, "Failed to open file"); return; }
  size_t w = f.print(body); f.close();
  if (w != body.length()) { wwErr(500, "Write failed (SPIFFS full?)"); return; }
  wwOk("Saved", "{\"filename\":\"" + wwEsc(n) + "\",\"bytes\":" + String(body.length()) + "}");
}

// ---- /api/system/info ----
static void wwInfo() {
  if (!wwGuard()) return;
  String v, maxp; wwLx("=V", v); wwLx("GXWP", maxp);
  String d = "{\"version\":\"" + wwEsc(wwStrip(v)) + "\",\"maxStepPps\":\"" + wwEsc(wwStrip(maxp)) + "\"";
  d += ",\"staIp\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("0.0.0.0")) + "\"";
  d += ",\"staConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",\"cmdPorts\":\"9999, 9998\"";  // for the Network Information of the Console  2026,10/22
  d += ",\"apSsid\":\"" + wwEsc(wbApSsid) + "\",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  d += ",\"freeHeap\":" + String(ESP.getFreeHeap()) + ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap());
  if (wwSpiffsOk) d += ",\"spiffsTotal\":" + String(SPIFFS.totalBytes()) + ",\"spiffsUsed\":" + String(SPIFFS.usedBytes());
  d += ",\"location\":\"" + wwEsc(nsLoc) + "\",\"pinSet\":" + String(nsPin.length() > 0 ? "true" : "false") + ",\"cfgLeftSec\":" + String(wwCfgLeftSec());
#if NS3000_BLE == ON
  // BLE handbox: 0 = not registered, 1 = registered (not connected), 2 = connected  2026,10/22
  d += ",\"handbox\":" + String(bhState) + ",\"handboxAddr\":\"" + wwEsc(String(bhAddr)) + "\",\"handboxStats\":\"" + String(bhConnects) + "," + String(bhPkts) + "," + String(bhAliveStops) + "," + String(bhLinkStops) + "\"";
#endif
  d += "}";
  wwOk("System info", d);
}

// ---- File management ----
static bool wwHidden(const String &n) { return n == NS_CFG_FILE || n == "/~upload.tmp"; }  // files hidden from the Web  2026,10/22
static String wwSafeName(String n) {  // allow only a single-level name starting with "/"
  n.trim();
  if (!n.startsWith("/")) n = "/" + n;
  if (n.length() < 2 || n.length() > 31 || n.indexOf("..") >= 0 || n.indexOf('/', 1) >= 0) return "";
  if (wwHidden(n)) return "";  // the config file and the temporary file are never handled  2026,10/22
  return n;
}

static void wwFileList() {
  if (!wwGuard()) return;
  if (!wwSpiffsOk) { wwErr(500, "SPIFFS not mounted"); return; }
  String j = "{\"status\":\"success\",\"files\":[";
  File root = SPIFFS.open("/");
  File f = root.openNextFile();
  bool first = true;
  while (f) {
    String name = String(f.name()); if (name.startsWith("/")) name = name.substring(1);
    if (wwHidden("/" + name)) { f = root.openNextFile(); continue; }  // the config file is not listed
    if (!first) j += ",";
    j += "{\"name\":\"" + wwEsc(name) + "\",\"size\":" + String(f.size()) + "}";
    first = false;
    f = root.openNextFile();
  }
  j += "],\"total\":" + String(SPIFFS.totalBytes()) + ",\"used\":" + String(SPIFFS.usedBytes()) + "}";
  wwSend(200, j);
}

static void wwFileDelete() {
  if (!wwGuard()) return;
  if (!wwSpiffsOk) { wwErr(500, "SPIFFS not mounted"); return; }
  if (!web.hasArg("filename")) { wwErr(400, "Missing filename parameter"); return; }
  String n = wwSafeName(web.arg("filename"));
  if (n == "") { wwErr(400, "Bad filename"); return; }
  if (!wwFileWriteOk()) { wwErr(403, "File Manager is locked 30 minutes after power-on. Restart (power cycle) to open it again."); return; }
  if (!SPIFFS.exists(n)) { wwErr(404, "File not found"); return; }
  if (wwSlewing()) { wwErr(409, "Busy: GOTO in progress"); return; }
  if (SPIFFS.remove(n)) wwOk("File deleted successfully", ""); else wwErr(500, "Failed to delete file");
}

// Upload in 2 parts: the body handler (wwUploadBody) runs while receiving, then wwUploadDone is called
static File wwUpFile;
static String wwUpError = "";
static String wwUpTmp = "";
static void wwUploadBody() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    wwUpError = "";
    wwUpTmp = "";
    if (!wwGuardCheck()) { wwUpError = wwGuardError; return; }
    if (!wwFileWriteOk()) { wwUpError = "Locked: File Manager is locked 30 minutes after power-on. Restart (power cycle) to open it again."; return; }
    if (!wwSpiffsOk) { wwUpError = "SPIFFS not mounted"; return; }
    String n = wwSafeName(u.filename);
    if (n == "") { wwUpError = "Bad filename (1 to 30 chars, no folders)"; return; }
    if (wwSlewing()) { wwUpError = "Busy: GOTO in progress"; return; }
    wwUpTmp = n;
    wwUpFile = SPIFFS.open("/~upload.tmp", "w");  // the original file is not damaged if the upload stops halfway
    if (!wwUpFile) wwUpError = "Failed to open file for writing";
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (wwUpError == "" && wwUpFile) {
      if (wwUpFile.write(u.buf, u.currentSize) != u.currentSize) wwUpError = "Write failed (SPIFFS full?)";
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (wwUpFile) wwUpFile.close();
    if (wwUpError == "") {
      SPIFFS.remove(wwUpTmp);
      if (!SPIFFS.rename("/~upload.tmp", wwUpTmp)) wwUpError = "Rename failed";
    } else SPIFFS.remove("/~upload.tmp");
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (wwUpFile) wwUpFile.close();
    SPIFFS.remove("/~upload.tmp");
    wwUpError = "Upload aborted";
  }
}
static void wwUploadDone() {
  wwGuardCheck();  // to decide which Origin to return
  if (wwUpError != "") { wwErr(wwUpError.startsWith("PIN") || wwUpError.startsWith("Forbidden") || wwUpError.startsWith("Locked") ? 403 : 400, wwUpError); return; }
  wwOk("File uploaded successfully", "\"" + wwEsc(wwUpTmp.substring(1)) + "\"");
}


// ---- /api/wifi/config (same endpoint as AiBridge; only for 30 minutes after power-on; needs the PIN if one is set)  2026,10/22 NS3000 ----
static bool wwCfgGuard() {
  if (!wwGuard()) return false;
  if (!wwCfgOpen()) { wwErr(403, "Locked: Wi-Fi / PIN settings are available only for 30 minutes after power-on. Restart (power cycle) to open again."); return false; }
  return true;  // no PIN during the 30 minutes (the 30 minutes are the key; after that nobody can touch it)
}
static void wwWifiCfgGet() {
  if (!wwCfgGuard()) return;
  wwOk("WiFi config", "{\"ssid\":\"" + wwEsc(nsSsid) + "\",\"password\":\"" + wwEsc(nsPass) + "\",\"location\":\"" + wwEsc(nsLoc)
       + "\",\"pin\":\"" + wwEsc(nsPin) + "\",\"cfgLeftSec\":" + String(wwCfgLeftSec()) + "}");
}
// ---- /api/handbox (BLE handbox state and signal strength; the Console reads it every 2 s; a light endpoint that does not ask OnStep)  2026,10/23 ----
static void wwHandbox() {
  if (!wwGuard()) return;
#if NS3000_BLE == ON
  wwOk("Handbox", "{\"state\":" + String(bhState) + ",\"addr\":\"" + wwEsc(String(bhAddr)) + "\",\"rssi\":" + String((int)bhRssi) + ",\"rssiMin\":" + String((int)bhRssiMin)
       + ",\"vbat\":" + String(bhVbat == 0xFF ? -1 : (int)bhVbat * 20)
       + ",\"connects\":" + String(bhConnects) + ",\"packets\":" + String(bhPkts) + ",\"aliveStops\":" + String(bhAliveStops) + ",\"linkStops\":" + String(bhLinkStops) + "}");
#else
  wwErr(404, "BLE is off in this firmware");
#endif
}
// ---- /api/handbox/forget (remove the BLE handbox registration; only for 30 minutes after power-on, like the Wi-Fi settings)  2026,10/22 NS3000 ----
static void wwHandboxForget() {
  if (!wwCfgGuard()) return;
#if NS3000_BLE == ON
  bhForgetReq = true;
  wwOk("Handbox registration removed. The next handbox found within 30 minutes after power-on will be registered.", "");
#else
  wwErr(404, "BLE is off in this firmware");
#endif
}
static bool wwOneLine(const String &s, unsigned int maxLen) { if (s.length() > maxLen) return false; for (unsigned int i = 0; i < s.length(); i++) if ((uint8_t)s[i] < 0x20) return false; return true; }
static void wwWifiCfgPost() {
  if (!wwCfgGuard()) return;
  if (!wwSpiffsOk) { wwErr(500, "SPIFFS not mounted"); return; }
  String ssid = web.arg("ssid"), pass = web.arg("password"), loc = web.arg("location"), pin = web.arg("access_pin");
  ssid.trim(); loc.trim(); pin.trim();
  if (ssid.length() == 0 || !wwOneLine(ssid, 32)) { wwErr(400, "SSID: 1 to 32 characters"); return; }
  if (!wwOneLine(pass, 63)) { wwErr(400, "Password: up to 63 characters"); return; }
  if (loc.length() == 0) loc = "NS4000";
  if (!wwOneLine(loc, 32)) { wwErr(400, "Location: up to 32 characters"); return; }
  if (!(pin.length() == 0 || (pin.length() == 4 && isdigit(pin[0]) && isdigit(pin[1]) && isdigit(pin[2]) && isdigit(pin[3])))) { wwErr(400, "PIN must be exactly 4 digits (or empty for no PIN)"); return; }
  if (wwSlewing()) { wwErr(409, "Busy: GOTO in progress"); return; }
  if (!wwCfgSave(ssid, pass, loc, pin)) { wwErr(500, "Failed to save config"); return; }
  bool pinChanged = pin != nsPin;
  nsLoc = loc; nsPin = pin;                       // the location name and PIN take effect at once
  nsSsid = ssid; nsPass = pass;                   // Wi-Fi takes effect after a restart (power cycle)
  for (int i = 0; i < 8; i++) wwFails[i].n = 0;
  if (pinChanged) wwSetCookie();                  // the person who just set it does not need to enter it again
  wwOk("Saved. Restart (power cycle) to apply Wi-Fi changes." + String(pin.length() ? " PIN is active now." : " PIN is off."), "");
}

// ---- /api/system/format_spiffs (same as AiBridge; GET = status, POST ?confirm=YES = format; formatting only during the 30 minutes) ----
static void wwSpiffsStatus() {
  if (!wwGuard()) return;
  size_t t = wwSpiffsOk ? SPIFFS.totalBytes() : 0, u = wwSpiffsOk ? SPIFFS.usedBytes() : 0;
  wwOk("SPIFFS status", "{\"totalBytes\":" + String(t) + ",\"usedBytes\":" + String(u) + ",\"freeBytes\":" + String(t - u)
       + ",\"formatted\":" + String(t > 0 ? "true" : "false") + ",\"mounted\":" + String(wwSpiffsOk ? "true" : "false") + "}");
}
static void wwSpiffsFormat() {
  if (!wwCfgGuard()) return;
  if (web.arg("confirm") != "YES") { wwErr(400, "Add confirm=YES"); return; }
  if (wwSlewing()) { wwErr(409, "Busy: GOTO in progress"); return; }
  SPIFFS.end();
  bool ok = SPIFFS.format();
  wwSpiffsOk = SPIFFS.begin(true);
  if (!ok) { wwErr(500, "Format failed"); return; }
  if (!wwCfgSave(nsSsid, nsPass, nsLoc, nsPin)) { wwErr(500, "Formatted, but failed to save the config again"); return; }  // keep the settings
  size_t t = SPIFFS.totalBytes(), u = SPIFFS.usedBytes();
  wwOk("SPIFFS formatted", "{\"totalBytes\":" + String(t) + ",\"usedBytes\":" + String(u) + ",\"freeBytes\":" + String(t - u) + ",\"formatted\":true,\"mounted\":true}");
}

// ---- PIN page: in Lang_en.h / Lang_ja.h (wwPinHtml, WW_TXT_PIN_LOCKED, WW_TXT_PIN_WRONG) ----
static String wwHtmlEsc(const String &s) { String o; for (unsigned int i = 0; i < s.length(); i++) { char c = s[i];
  if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c; } return o; }
static String wwSafeNext(String n) { if (!n.startsWith("/") || n.startsWith("//") || n.indexOf('\\') >= 0) return "/"; return n; }
static void wwPinPage(int code, const String &msg) {
  String h = FPSTR(wwPinHtml);
  h.replace("%MSG%", msg.length() ? "<p class=\"err\">" + wwHtmlEsc(msg) + "</p>" : "");
  h.replace("%NEXT%", wwHtmlEsc(wwSafeNext(web.arg("next"))));
  web.sendHeader("Cache-Control", "no-store");
  web.send(code, "text/html; charset=utf-8", h);
}
static void wwPinGet() {
  if (!wwHostOk()) { web.send(403, "text/plain", "Forbidden host name"); return; }
  if (nsPin.length() == 0 || wwCookieOk()) { web.sendHeader("Location", wwSafeNext(web.arg("next"))); web.send(303); return; }
  wwPinPage(200, "");
}
static void wwPinLogin() {
  if (!wwHostOk()) { web.send(403, "text/plain", "Forbidden host name"); return; }
  String next = wwSafeNext(web.arg("next"));
  if (nsPin.length() == 0) { web.sendHeader("Location", next); web.send(303); return; }
  if (wwPinLocked()) { wwPinPage(429, WW_TXT_PIN_LOCKED); return; }
  if (wwPinMatch(web.arg("pin"))) { wwSetCookie(); web.sendHeader("Location", next); web.send(303); return; }
  wwPinPage(401, wwPinLocked() ? WW_TXT_PIN_LOCKED : WW_TXT_PIN_WRONG);
}

// ---- File Manager (/files). A copy of the AiBridge File Manager page, with a PIN field and the time left added  2026,10/22 NS3000 ----
// Note: do not write "//" inside this raw string (the Arduino preprocessor takes it for a comment)
static const char wwFilesHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><title>LUNA OnStepNinja File Manager</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>
body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #1a1a2e; color: #eee; }
.container { max-width: 900px; margin: 0 auto; background: #16213e; border-radius: 10px; padding: 20px; }
h1 { text-align: center; color: #4fc3f7; margin-bottom: 30px; }
.section { background: #0f3460; padding: 15px; border-radius: 8px; margin-bottom: 20px; }
.section h3 { color: #4fc3f7; margin-top: 0; }
.btn { background: linear-gradient(145deg, #4fc3f7, #29b6f6); border: none; color: white; padding: 10px 15px; border-radius: 8px; cursor: pointer; font-size: 14px; margin: 5px; }
.btn:hover { background: linear-gradient(145deg, #29b6f6, #0288d1); }
.btn:disabled { opacity: .4; cursor: default; }
.btn-back { background: linear-gradient(145deg, #4caf50, #388e3c) !important; }
.navigation { text-align: center; margin-bottom: 20px; }
.file-list { background: #000; color: #0f0; padding: 15px; border-radius: 5px; margin: 10px 0; font-family: 'Courier New', monospace; min-height: 100px; white-space: pre-line; }
.upload-section input[type='file'] { padding: 8px; border-radius: 5px; border: 1px solid #4fc3f7; background: #0f3460; color: #eee; margin-right: 10px; }
.inp { padding: 8px; border-radius: 5px; border: 1px solid #4fc3f7; background: #0f3460; color: #eee; width: 300px; max-width: 90%; margin-right: 10px; }
.lab { display: block; margin-bottom: 5px; color: #4fc3f7; }
.note { color: #aaa; font-size: 12px; margin-top: 5px; }
.lock { color: #ffb74d; }
.foot { text-align: center; color: #888; font-size: 12px; line-height: 1.7; }
.foot a { color: #4fc3f7; }
</style></head><body>
<div class='container'>
<h1>LUNA OnStepNinja File Manager</h1>
<div class='navigation'><button class='btn btn-back' onclick='window.location.href="/"'>&larr; Back to Console</button></div>

<div class='section'>
<h3>WiFi Configuration</h3>
<div id='cfg-window' class='note'>Checking...</div>
<div style='margin: 10px 0;'><label class='lab'>SSID (Network Name):</label><input type='text' id='wifiSSID' placeholder='Enter WiFi SSID' class='inp'></div>
<div style='margin: 10px 0;'><label class='lab'>Password:</label><input type='text' id='wifiPassword' placeholder='Enter WiFi Password' class='inp'></div>
<div style='margin: 10px 0;'><label class='lab'>Location Name:</label><input type='text' id='locationName' placeholder='MainScope, Garage, Observatory...' class='inp'></div>
<div style='margin: 10px 0;'><label class='lab'>PIN (4 digits, empty = no PIN):</label><input type='text' id='accessPin' placeholder='e.g. 1234' maxlength='4' inputmode='numeric' class='inp'>
<div class='note'>With a PIN, the pages of this device ask for it once (then remembered). AiBridge Host, LUNA Host Lab, SkySafari (TCP) and non-browser tools do not need it.<br>
Forgot the PIN? Restart (power cycle): this File Manager opens without the PIN for 30 minutes after power-on.</div></div>
<div style='margin: 15px 0;'>
<button class='btn cfgbtn' onclick='loadWiFiConfig()'>Load Current Config</button>
<button class='btn cfgbtn' onclick='saveWiFiConfig()' style='background: linear-gradient(145deg, #4caf50, #388e3c) !important;'>Save Config</button>
</div>
<div id='wifi-status' style='margin-top: 10px; color: #4fc3f7;'>WiFi configuration settings</div>
</div>

<div class='section'>
<h3>BLE Handbox</h3>
<div id='hb-status' class='note'>&mdash;</div>
<div class='note'>Within 30 minutes after power-on, the first handbox found is paired and registered. After that only that handbox can connect.</div>
<div style='margin: 15px 0;'><button class='btn btn-danger cfgbtn' onclick='forgetHandbox()' style='background: linear-gradient(145deg, #f44336, #d32f2f) !important;'>Remove Registration</button></div>
</div>

<div class='section'>
<h3>SPIFFS Management</h3>
<div style='margin: 15px 0;'>
<button class='btn' onclick='checkSpiffsStatus()'>Check Status</button>
<button class='btn btn-danger cfgbtn' onclick='formatSpiffs()' style='background: linear-gradient(145deg, #f44336, #d32f2f) !important; margin-left: 10px;'>Format SPIFFS</button>
</div>
<div id='spiffs-status' style='margin-top: 10px; color: #4fc3f7;'>Click Check Status to view SPIFFS information</div>
</div>

<div class='section'>
<h3>Upload File</h3>
<input type='file' id='fileInput'>
<button class='btn' onclick='uploadFile()'>Upload</button>
<div id='upload-status' style='margin-top: 10px; color: #4fc3f7;'>Select file and click Upload (max 150KB)</div>
</div>

<div class='section'>
<h3>File List</h3>
<button class='btn' onclick='refreshFileList()'>Refresh File List</button>
<div id='file-list' class='file-list'>Click 'Refresh File List' to load files</div>
</div>

<div class='section'>
<h3>Delete File</h3>
<input type='text' id='deleteFileName' placeholder='Enter file name (e.g. index.html)' class='inp' style='width: 250px;'>
<button class='btn btn-danger' onclick='deleteFile()' style='background: linear-gradient(145deg, #f44336, #d32f2f) !important;'>Delete</button>
<div id='delete-status' style='margin-top: 10px; color: #4fc3f7;'>Enter file name and click Delete</div>
</div>

<div class='section'>
<h3>LX200 Command Test</h3>
<input type='text' id='lxCmd' value='GVN' placeholder='e.g. GVN, GR, GU' class='inp' style='width: 250px;'>
<button class='btn' onclick='sendLx()'>Send</button>
<div class='note'>Sends one LX200 command directly to OnStep (no colon or #; e.g. GVN, GR, GD, GU). For checking and testing. Some commands move the mount or change settings: use with care.</div>
<div id='lx-out' class='file-list' style='min-height: 40px;'>&mdash;</div>
</div>

<p class='foot'>LUNA OnStepNinja NS-4000 &middot; <a href='https:&#47;&#47;github.com/OnStepNinja' target='_blank' rel='noopener'>&#127769; LUNA Project</a></p>
</div>
<script>
const $ = id => document.getElementById(id);
function setStatus(id, t) { $(id).textContent = t; }
const ofetch = window.fetch.bind(window);
window.fetch = (u, o) => ofetch(u, o).then(r => { if (r.status === 401) { location.href = '/pin?next=' + encodeURIComponent('/files'); } return r; });
function cfgWindow() {
  fetch('/api/system/info').then(r => r.json()).then(d => {
    const s = (d.data || {}).cfgLeftSec || 0;
    const hb = (d.data || {}).handbox;
    $('hb-status').textContent = hb === undefined ? 'BLE is off in this firmware' : ['Not registered', 'Registered (not connected): ', 'Connected: '][hb] + (hb ? (d.data.handboxAddr || '') : '');
    const open = s > 0;
    document.querySelectorAll('.cfgbtn').forEach(b => b.disabled = !open);
    ['wifiSSID', 'wifiPassword', 'locationName', 'accessPin'].forEach(i => $(i).disabled = !open);
    $('cfg-window').className = open ? 'note' : 'note lock';
    $('cfg-window').textContent = open ? 'WiFi / PIN settings are open for ' + Math.ceil(s / 60) + ' more minute(s) after power-on (then locked).'
      : 'Locked: WiFi / PIN settings are available only for 30 minutes after power-on. Restart (power cycle) to open again.';
  }).catch(() => {});
}
function loadWiFiConfig() {
  setStatus('wifi-status', 'Loading WiFi config...');
  fetch('/api/wifi/config').then(r => r.json()).then(data => {
    if (data.status === 'success') {
      $('wifiSSID').value = data.data.ssid || '';
      $('wifiPassword').value = data.data.password || '';
      $('locationName').value = data.data.location || 'NS4000';
      $('accessPin').value = data.data.pin || '';
      setStatus('wifi-status', 'WiFi config loaded successfully');
    } else setStatus('wifi-status', 'Error loading config: ' + data.message);
  }).catch(e => setStatus('wifi-status', 'Network error: ' + e.message));
}
function saveWiFiConfig() {
  const ssid = $('wifiSSID').value.trim(), password = $('wifiPassword').value, location = $('locationName').value.trim() || 'NS4000', pin = $('accessPin').value.trim();
  if (ssid === '') { alert('Please enter SSID'); return; }
  if (pin !== '' && !/^[0-9]{4}$/.test(pin)) { alert('PIN must be exactly 4 digits (or empty for no PIN)'); return; }
  setStatus('wifi-status', 'Saving WiFi config...');
  const fd = new FormData(); fd.append('ssid', ssid); fd.append('password', password); fd.append('location', location); fd.append('access_pin', pin);
  fetch('/api/wifi/config', { method: 'POST', body: fd }).then(r => r.json()).then(data => {
    if (data.status === 'success') { setStatus('wifi-status', data.message); alert('WiFi configuration saved successfully!\nRestart the device to apply WiFi changes.' + (pin ? '\nPIN is active now.' : '')); }
    else { setStatus('wifi-status', 'Save error: ' + data.message); alert('Save error: ' + data.message); }
  }).catch(e => { setStatus('wifi-status', 'Network error: ' + e.message); alert('Network error: ' + e.message); });
}
function uploadFile() {
  const file = $('fileInput').files[0];
  if (!file) { alert('Please select a file'); return; }
  if (file.size > 150 * 1024) { alert('File size exceeds 150KB limit. Upload cancelled.'); return; }
  setStatus('upload-status', 'Uploading...');
  const fd = new FormData(); fd.append('file', file);
  fetch('/api/files/upload', { method: 'POST', body: fd }).then(r => r.json()).then(data => {
    if (data.status === 'success') { setStatus('upload-status', 'File uploaded successfully!'); $('fileInput').value = ''; alert('File uploaded successfully!'); }
    else { setStatus('upload-status', 'Upload error: ' + data.message); alert('Upload error: ' + data.message); }
  }).catch(e => { setStatus('upload-status', 'Network error: ' + e.message); alert('Network error: ' + e.message); });
}
function refreshFileList() {
  setStatus('file-list', 'Loading...');
  fetch('/api/files/list').then(r => r.json()).then(data => {
    if (data.status === 'success') {
      const files = data.files;
      let t = files.length === 0 ? 'No files found' : 'Files found: ' + files.length + '\n\n';
      files.forEach((f, i) => { t += (i + 1) + '. ' + f.name + ' ' + f.size + ' bytes\n'; });
      setStatus('file-list', t);
    } else setStatus('file-list', 'Error: ' + data.message);
  }).catch(e => setStatus('file-list', 'Network error: ' + e.message));
}
function deleteFile() {
  const name = $('deleteFileName').value.trim();
  if (name === '') { alert('Please enter a file name'); return; }
  if (!confirm('Delete file: ' + name + '?')) return;
  setStatus('delete-status', 'Deleting...');
  fetch('/api/files/delete?filename=' + encodeURIComponent(name), { method: 'DELETE' }).then(r => r.json()).then(data => {
    if (data.status === 'success') { setStatus('delete-status', 'File deleted successfully!'); $('deleteFileName').value = ''; alert('File deleted successfully!'); }
    else { setStatus('delete-status', 'Delete error: ' + data.message); alert('Delete error: ' + data.message); }
  }).catch(e => { setStatus('delete-status', 'Network error: ' + e.message); alert('Network error: ' + e.message); });
}
function spiffsText(d) { const mb = x => (x / 1024 / 1024).toFixed(2);
  return 'SPIFFS Status: Total ' + mb(d.totalBytes) + ' MB / Used ' + mb(d.usedBytes) + ' MB (' + ((d.usedBytes / d.totalBytes) * 100).toFixed(1) + '%) / Free ' + mb(d.freeBytes) + ' MB / Mounted: ' + (d.mounted ? 'Yes' : 'No'); }
function checkSpiffsStatus() {
  setStatus('spiffs-status', 'Checking SPIFFS status...');
  fetch('/api/system/format_spiffs').then(r => r.json()).then(data => {
    setStatus('spiffs-status', data.status === 'success' ? spiffsText(data.data) : 'Error: ' + data.message);
  }).catch(e => setStatus('spiffs-status', 'Network error: ' + e.message));
}
function formatSpiffs() {
  if (!confirm('WARNING: This will erase ALL files in SPIFFS (the WiFi / PIN settings are kept)!\n\nAre you sure you want to format?')) return;
  setStatus('spiffs-status', 'Formatting SPIFFS...');
  fetch('/api/system/format_spiffs?confirm=YES', { method: 'POST' }).then(r => r.json()).then(data => {
    if (data.status === 'success') { setStatus('spiffs-status', 'Format Successful! ' + spiffsText(data.data)); alert('SPIFFS formatted successfully!'); }
    else { setStatus('spiffs-status', 'Format Failed! Error: ' + data.message); alert('Format failed: ' + data.message); }
  }).catch(e => { setStatus('spiffs-status', 'Network Error! ' + e.message); alert('Network error: ' + e.message); });
}
function forgetHandbox() {
  if (!confirm('Remove the registered BLE handbox?')) return;
  fetch('/api/handbox/forget', { method: 'POST' }).then(r => r.json()).then(data => { alert(data.message); setTimeout(cfgWindow, 1500); })
    .catch(e => alert('Network error: ' + e.message));
}
function sendLx() {
  const c = $('lxCmd').value.trim();
  if (c === '') return;
  setStatus('lx-out', 'Sending :' + c + '# ...');
  fetch('/api/command?cmd=' + encodeURIComponent(c)).then(r => r.json()).then(data => {
    setStatus('lx-out', data.status === 'success' ? ':' + c.replace(/^:|#$/g, '') + '#  ->  ' + (data.data === '' ? '(no reply)' : data.data) : 'Error: ' + data.message);
  }).catch(e => setStatus('lx-out', 'Network error: ' + e.message));
}
cfgWindow(); setInterval(cfgWindow, 30000);
</script></body></html>)HTML";
// Page shown after the 30 minutes (same look as the AiBridge File Manager Access Denied page)
static const char wwFilesDeniedHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><title>LUNA OnStepNinja - Access Denied</title><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #1a1a2e; color: #eee; text-align: center; }
.container { max-width: 600px; margin: 0 auto; background: #16213e; border-radius: 10px; padding: 20px; }
h1 { color: #f44336; }
.btn { background: linear-gradient(145deg, #4caf50, #388e3c); border: none; color: white; padding: 12px 20px; border-radius: 8px; cursor: pointer; font-size: 14px; margin: 10px; text-decoration: none; display: inline-block; }</style></head><body>
<div class='container'><h1>LUNA OnStepNinja File Manager Access Denied</h1>
<p>File Manager has been locked 30 minutes after power-on.</p>
<p>Please restart the system (power cycle) to access File Manager again.</p>
<a href='/' class='btn'>&larr; Back to Console</a></div></body></html>)HTML";
static void wwFilesPage() {
  if (!wwHostOk()) { web.send(403, "text/plain", "Forbidden host name"); return; }
  if (!wwCfgOpen()) { web.sendHeader("Cache-Control", "no-store"); web.send_P(403, "text/html; charset=utf-8", wwFilesDeniedHtml); return; }  // cannot be opened after the 30 minutes (even with the PIN)
  if (!wwPageGuard()) return;
  web.sendHeader("Cache-Control", "no-store");
  web.send_P(200, "text/html; charset=utf-8", wwFilesHtml);
}


// ---- /mcp: for LUNA Host Lab (same JSON-RPC form as LUNA; files and system information only; no tools that move the mount)  2026,10/21 NS3000 ----
// Tools: get_system_info, get_mount_status, buzzer, get_license_info, fs_list, fs_read (up to 4KB), fs_write / fs_append (up to 4KB per call), fs_delete. Reply texts are the same as LUNA
// (Host Lab checks the byte count in "Success: ... (N bytes)" and the size in fs_list). Lua and other tools reply "Error: not supported"
// OnStep has no JSON library, so a small reader takes only the needed fields (method, id, name, path, content)

// Find the value of key in the JSON. Strings are returned unescaped, other values as written. false if not found
static bool wwJsonGet(const String &js, const char *key, String &out, bool &isString) {
  int n = js.length();
  for (int i = 0; i < n; i++) {
    if (js[i] != '"') continue;
    int j = i + 1; String tok;
    while (j < n && js[j] != '"') { if (js[j] == '\\' && j + 1 < n) { tok += js[j]; j++; } tok += js[j]; j++; }
    int k = j + 1; while (k < n && (js[k] == ' ' || js[k] == '\t' || js[k] == '\r' || js[k] == '\n')) k++;
    if (k < n && js[k] == ':' && tok == key) {
      k++; while (k < n && (js[k] == ' ' || js[k] == '\t' || js[k] == '\r' || js[k] == '\n')) k++;
      out = "";
      if (k < n && js[k] == '"') {
        isString = true; k++;
        while (k < n && js[k] != '"') {
          char c = js[k];
          if (c == '\\' && k + 1 < n) {
            char e = js[++k];
            if (e == 'n') out += '\n'; else if (e == 'r') out += '\r'; else if (e == 't') out += '\t';
            else if (e == 'b') out += '\b'; else if (e == 'f') out += '\f';
            else if (e == 'u' && k + 4 < n) {
              unsigned int cp = strtoul(js.substring(k + 1, k + 5).c_str(), NULL, 16); k += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF && k + 6 < n && js[k + 1] == '\\' && js[k + 2] == 'u') {  // surrogate pair
                unsigned int lo = strtoul(js.substring(k + 3, k + 7).c_str(), NULL, 16); k += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              }
              if (cp < 0x80) out += (char)cp;
              else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
              else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
              else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            } else out += e;  // \" \\ \/
          } else out += c;
          k++;
        }
        return true;
      }
      isString = false;
      while (k < n && js[k] != ',' && js[k] != '}' && js[k] != ']') { out += js[k]; k++; }
      out.trim();
      return true;
    }
    i = j;  // move to the end of the string
  }
  return false;
}

static String wwMcpText(const String &text) {
  return "{\"content\":[{\"type\":\"text\",\"text\":\"" + wwEsc(text) + "\"}]}";
}

static String wwMcpSystemInfo() {
  String v; wwLx("=V", v); v = wwStrip(v);
  String r = "{\"location\":\"" + wwEsc(nsLoc) + "\",\"firmware\":\"" + wwEsc(v) + "\",\"board_type\":\"OnStepNinja NS-4000 (OnStep 4.24j based)\"";
  r += ",\"chip_model\":\"" + String(ESP.getChipModel()) + "\",\"chip_revision\":" + String(ESP.getChipRevision());
  r += ",\"cpu_freq_mhz\":" + String(ESP.getCpuFreqMHz()) + ",\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"heap_size\":" + String(ESP.getHeapSize());
  r += ",\"uptime_ms\":" + String(millis());
  if (wwSpiffsOk) r += ",\"spiffs_total\":" + String(SPIFFS.totalBytes()) + ",\"spiffs_used\":" + String(SPIFFS.usedBytes());
  r += ",\"sta_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("0.0.0.0")) + "\",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
  r += ",\"lua\":false}";
  return r;
}

// Mount status for AI (read only; nothing that moves the mount)  2026,10/24
static String wwMcpMountStatus() {
  String ra, dec, gu;
  if (!wwLx("GR", ra) || !wwLx("GD", dec) || !wwLx("GU", gu)) return "Error: No reply from OnStep";
  gu = wwStrip(gu);
  bool parked = gu.indexOf('P') >= 0, slewing = gu.indexOf('N') < 0, tracking = gu.indexOf('n') < 0 && !slewing;
  String r = "{\"ra\":\"" + wwEsc(wwStrip(ra)) + "\",\"dec\":\"" + wwEsc(wwStrip(dec)) + "\",\"state\":\"" + String(parked ? "parked" : slewing ? "slewing" : tracking ? "tracking" : "stopped") + "\"";
  r += ",\"tracking\":" + String(tracking ? "true" : "false") + ",\"slewing\":" + String(slewing ? "true" : "false") + ",\"parked\":" + String(parked ? "true" : "false");
  r += ",\"at_home\":" + String(gu.indexOf('H') >= 0 ? "true" : "false");
#if NS3000_BLE == ON
  r += ",\"handbox\":{\"state\":\"" + String(bhState == 2 ? "connected" : bhState == 1 ? "registered_not_connected" : "not_registered") + "\"";
  if (bhState == 2) {
    if (bhRssi != 0) r += ",\"rssi_dbm\":" + String((int)bhRssi) + ",\"rssi_weakest_dbm\":" + String((int)bhRssiMin);
    if (bhVbat != 0xFF) r += ",\"battery_v\":" + String(bhVbat * 0.02f, 2);
  }
  r += "}";
#endif
  return r + "}";
}

// License information (GPL v3 "Appropriate Legal Notices" also for this interface)  2026,10/24
static String wwMcpLicense() {
  return "{\"firmware\":\"LUNA OnStepNinja NS-4000\",\"developer\":\"Nishioka Sadahiko\","
         "\"copyright\":\"Copyright (c) 2026 Nishioka Sadahiko (NS-4000: the additions and changes to OnStep)\","
         "\"license\":\"GNU GPL v3 (no warranty)\",\"based_on\":\"OnStep (GPL v3) https://github.com/hjd1964/OnStep - the OnStep code belongs to its authors (see the copyright notices in the source)\","
         "\"source\":\"https://github.com/OnStepNinja/OnStepNinja-NS4000\",\"note\":\"Derived work, not an official OnStep release\","
         "\"companion_apps_and_handbox\":\"Freeware, Copyright (c) 2026 Nishioka Sadahiko: personal use free, at-cost copies to friends OK, no sales, no commercial use, no re-hosting\","
         "\"third_party\":\"NimBLE-Arduino (Apache 2.0), arduino-esp32 (LGPL 2.1), ESP-IDF (Apache 2.0), d3-celestial star data (BSD 3-Clause)\"}";
}

static String wwMcpFsList() {
  String r = "{\"files\":[";
  bool first = true;
  if (wwSpiffsOk) {
    File root = SPIFFS.open("/"); File f = root.openNextFile();
    while (f) {
      String name = String(f.name()); if (name.startsWith("/")) name = name.substring(1);
      if (wwHidden("/" + name)) { f = root.openNextFile(); continue; }  // the config file is not listed
      if (!first) r += ",";
      r += "{\"name\":\"" + wwEsc(name) + "\",\"size\":" + String(f.size()) + "}";
      first = false; f = root.openNextFile();
    }
  }
  return r + "]}";
}

static String wwMcpTool(const String &tool, const String &body) {
  String path, content; bool s;
  bool hasPath = wwJsonGet(body, "path", path, s);
  bool hasContent = wwJsonGet(body, "content", content, s);
  if (tool == "get_system_info") return wwMcpSystemInfo();
  if (tool == "get_mount_status") return wwMcpMountStatus();
  if (tool == "get_license_info") return wwMcpLicense();
  if (tool == "buzzer") {  // a short beep; at most once every 2 seconds (so that it cannot keep sounding at night)
    static unsigned long last = 0;
    if (last != 0 && millis() - last < 2000UL) return "Error: Buzzer can be used once every 2 seconds";
    last = millis();
    String r; wwLx("=SD", r);
    return "Success: Buzzer sounded";
  }
  if (!wwSpiffsOk && tool.startsWith("fs_")) return "Error: SPIFFS not mounted";
  if (tool == "fs_list") return wwMcpFsList();
  if (tool == "fs_read" || tool == "fs_delete" || tool == "fs_write" || tool == "fs_append") {
    String p = hasPath ? wwSafeName(path) : "";
    if (p == "") return "Error: Bad path (one level, 1 to 30 characters)";
    if (tool == "fs_read") {
      if (!SPIFFS.exists(p)) return "Error: File not found";
      File f = SPIFFS.open(p, "r"); if (!f) return "Error: Cannot open file";
      if (f.size() > 4096) { f.close(); return "Error: File too large (max 4KB)"; }
      String c; c.reserve(f.size() + 1); while (f.available()) c += (char)f.read(); f.close();
      return c;
    }
    if (wwSlewing()) return "Error: Busy: GOTO in progress";
    if (tool == "fs_delete") {
      if (!SPIFFS.exists(p)) return "Error: File not found";
      return SPIFFS.remove(p) ? "Success: File deleted" : "Error: Failed to delete file";
    }
    if (!hasContent) return "Error: Missing content";
    if (content.length() > 4096) return "Error: Content too large (max 4KB)";
    File f = SPIFFS.open(p, tool == "fs_write" ? "w" : "a");
    if (!f) return tool == "fs_write" ? "Error: Cannot open file for writing" : "Error: Cannot open file for appending";
    size_t w = f.print(content); f.close();
    if (w != content.length()) return "Error: Write failed (SPIFFS full?)";
    return tool == "fs_write" ? "Success: File written (" + String(content.length()) + " bytes)"
                              : "Success: Appended to file (" + String(content.length()) + " bytes)";
  }
  return "Error: " + tool + " is not supported on OnStepNinja NS-4000 (no Lua)";
}

static const char wwMcpToolsList[] PROGMEM = R"JSON({"tools":[
{"name":"get_system_info","description":"OnStepNinja NS-4000 (OnStep based mount controller) info: firmware, memory, SPIFFS, IP","inputSchema":{"type":"object","properties":{}}},
{"name":"get_mount_status","description":"Read-only mount status: RA, Dec, tracking / slewing / parked, and the BLE handbox (connection, signal strength, battery voltage). Never moves the mount","inputSchema":{"type":"object","properties":{}}},
{"name":"buzzer","description":"Sound the mount buzzer once (short beep). At most once every 2 seconds. Does not move the mount","inputSchema":{"type":"object","properties":{}}},
{"name":"get_license_info","description":"License of the NS-4000 firmware (GPL v3, based on OnStep), its source, and the companion apps","inputSchema":{"type":"object","properties":{}}},
{"name":"fs_list","description":"List files in SPIFFS","inputSchema":{"type":"object","properties":{}}},
{"name":"fs_read","description":"Read a text file (max 4KB)","inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}},
{"name":"fs_write","description":"Write (replace) a text file (max 4KB per call). Refused during GOTO","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}},
{"name":"fs_append","description":"Append to a text file (max 4KB per call). Refused during GOTO","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}},
{"name":"fs_delete","description":"Delete a file. Refused during GOTO","inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}
]})JSON";

static void wwMcpSend(const String &json) {
  if (wwAllowOrigin.length() > 0) { web.sendHeader("Access-Control-Allow-Origin", wwAllowOrigin); web.sendHeader("Vary", "Origin"); }
  web.send(200, "application/json", json);
}

static void wwMcp() {
  if (!wwGuardCheck()) {
    web.send(403, "application/json", "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,\"message\":\"" + wwEsc(wwGuardError) + "\"}}");
    return;
  }
  String body = web.arg("plain"), method, id, name; bool s;
  if (!wwJsonGet(body, "method", method, s)) { wwMcpSend("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}"); return; }
  bool idIsStr = false;
  if (!wwJsonGet(body, "id", id, idIsStr)) { web.send(202, "text/plain", ""); return; }  // notifications (no id) get no reply
  String idJson = idIsStr ? "\"" + wwEsc(id) + "\"" : id;
  String head = "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",";
  if (method == "initialize") {
    String v; wwLx("=V", v);
    wwMcpSend(head + "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"OnStepNinja NS-4000\",\"version\":\"" + wwEsc(wwStrip(v)) + "\"}}}");
  } else if (method == "tools/list") {
    wwMcpSend(head + "\"result\":" + String(FPSTR(wwMcpToolsList)) + "}");
  } else if (method == "tools/call") {
    if (!wwJsonGet(body, "name", name, s)) { wwMcpSend(head + "\"error\":{\"code\":-32602,\"message\":\"Missing tool name\"}}"); return; }
    wwMcpSend(head + "\"result\":" + wwMcpText(wwMcpTool(name, body)) + "}");
  } else if (method == "ping") {
    wwMcpSend(head + "\"result\":{}}");
  } else {
    wwMcpSend(head + "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
  }
}

// ---- Everything else: serve files from SPIFFS ----
static String wwContentType(const String &p) {
  if (p.endsWith(".html") || p.endsWith(".htm")) return "text/html; charset=utf-8";
  if (p.endsWith(".js")) return "application/javascript";
  if (p.endsWith(".css")) return "text/css";
  if (p.endsWith(".json")) return "application/json";
  if (p.endsWith(".png")) return "image/png";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".svg")) return "image/svg+xml";
  if (p.endsWith(".ico")) return "image/x-icon";
  if (p.endsWith(".txt") || p.endsWith(".lua")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}
static void wwNotFound() {
  if (!wwHostOk()) { web.send(403, "text/plain", "Forbidden host name"); return; }  // rule 1 also for pages
  if (web.method() != HTTP_OPTIONS && alpacaHandle()) return;   // ASCOM Alpaca (WifiWebAlpaca.ino)  2026,10/22
  if (web.method() != HTTP_OPTIONS && !wwPageGuard()) return;  // needs the session cookie when a PIN is set  2026,10/22
  if (web.method() == HTTP_OPTIONS) {  // preflight from other pages (CORS). wwGuard() decides on the real request, so allow it here
    String o = web.header("Origin");
    if (o.length() > 0) { web.sendHeader("Access-Control-Allow-Origin", o); web.sendHeader("Vary", "Origin"); }
    web.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
    web.sendHeader("Access-Control-Allow-Headers", "X-PIN, X-AiBridge-Pin, X-HostLab-Pin, Content-Type");
    web.send(204);
    return;
  }
  String p = web.uri();
  if (p == "/") p = "/index.html";
  if (wwHidden(p)) { web.send(404, "text/plain", "Not found"); return; }  // the config file is never served  2026,10/22
  if (wwSpiffsOk) {
    String gz = p + ".gz";
    if (SPIFFS.exists(gz)) { File f = SPIFFS.open(gz, "r"); web.streamFile(f, wwContentType(p)); f.close(); return; }  // streamFile adds the gzip header
    if (SPIFFS.exists(p)) { File f = SPIFFS.open(p, "r"); web.streamFile(f, wwContentType(p)); f.close(); return; }
  }
  if (p == "/index.html") { web.send_P(200, "text/html; charset=utf-8", wwIndexHtml); return; }
  web.send(404, "text/plain", "Not found");
}

// ---- Called from the Wi-Fi task (core 0) ----
void wifiWebBegin() {
  if (!wwSpiffsOk) wwSpiffsOk = SPIFFS.begin(true);  // normally already mounted by nsCfgLoad()
  web.on("/api/command", HTTP_GET, wwCommand);
  web.on("/api/status", HTTP_GET, wwStatus);
  web.on("/api/goto", HTTP_GET, wwGoto);
  web.on("/api/stop", HTTP_GET, wwStop);
  web.on("/api/system/info", HTTP_GET, wwInfo);
  web.on("/api/buzzer", wwBuzzer);                    // bell button of the star chart
  web.on("/api/ui/upload", HTTP_POST, wwUiUpload);    // saving the star chart settings
  web.on("/api/files/list", HTTP_GET, wwFileList);
  web.on("/api/files/delete", HTTP_POST, wwFileDelete);
  web.on("/api/files/delete", HTTP_DELETE, wwFileDelete);  // the AiBridge File Manager sends DELETE
  web.on("/files", HTTP_GET, wwFilesPage);                 // File Manager (same page as AiBridge)  2026,10/22
  web.on("/api/wifi/config", HTTP_GET, wwWifiCfgGet);      // only for 30 minutes after power-on
  web.on("/api/wifi/config", HTTP_POST, wwWifiCfgPost);
  web.on("/api/handbox", HTTP_GET, wwHandbox);                // state and signal strength  2026,10/23
  web.on("/api/handbox/forget", HTTP_POST, wwHandboxForget);  // only for 30 minutes after power-on  2026,10/22
  web.on("/api/system/format_spiffs", HTTP_GET, wwSpiffsStatus);
  web.on("/api/system/format_spiffs", HTTP_POST, wwSpiffsFormat);
  web.on("/pin", HTTP_GET, wwPinGet);                      // PIN page
  web.on("/pin/login", HTTP_POST, wwPinLogin);
  web.on("/api/files/upload", HTTP_POST, wwUploadDone, wwUploadBody);
  web.on("/mcp", HTTP_POST, wwMcp);  // for LUNA Host Lab
  proxyBegin();                       // Alpaca generic proxy /api/external/* (WifiWebProxy.ino)  2026,10/22
  web.onNotFound(wwNotFound);
  const char *hdrs[] = {"Origin", "Sec-Fetch-Site", "X-PIN", "X-AiBridge-Pin", "X-HostLab-Pin", "Cookie", "User-Agent", "Referer"};
  web.collectHeaders(hdrs, 8);
  web.begin();
}

void wifiWebPoll() {
  web.handleClient();
}

#endif
