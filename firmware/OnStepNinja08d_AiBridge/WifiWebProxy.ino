// -----------------------------------------------------------------------------------
// Alpaca generic proxy (same endpoints, arguments and response format as AiBridge v7.15)  2026,10/22 NS3000
//
// - /api/external/alpaca?target=IP&port=80&device=focuser&number=0&property=position&method=GET|PUT|POST&params=a=1&b=2
//     relays to /api/v1/<device>/<number>/<property> of an Alpaca device on the LAN and returns its JSON in data
// - /api/external/discover  finds Alpaca devices on the LAN with UDP 32227 and adds each one's /management/v1/configureddevices
// - /api/external/status?target=, /api/external/goto?target=&ra=&dec=, /api/external/stop?target=  external telescope number 0 (same as AiBridge)
// - Safety: relays only to private IPv4 addresses on the LAN (10.x, 172.16-31.x, 192.168.x). Never to outside sites or to this device itself
//   Only the Alpaca API (/api/v1/) is relayed. device and property: lowercase letters and digits, number: digits only
//   Camera images (imagearray / imagearrayvariant) are refused (too large for the ESP32 memory). Responses up to 16KB
//   Waits up to 3 s for the device (TCP 9999 replies also wait during that time). The same guard (wwGuard, PIN rules) as the Web API
// - AiBridge sent the external GOTO with GET; here it is sent with PUT as the Alpaca standard says

#if NS3000_WIFI == ON && NS3000_WEB == ON

#include <HTTPClient.h>

#define PX_TIMEOUT_MS   3000
#define PX_MAX_BYTES    16384

// Is this IP allowed as a relay target? (a private IPv4 address on the LAN, not this device)
static bool pxTargetOk(const String &t, IPAddress &ip) {
  if (!ip.fromString(t)) return false;
  bool priv = ip[0] == 10 || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) || (ip[0] == 192 && ip[1] == 168);
  if (!priv) return false;
  if (ip == WiFi.localIP() || ip == WiFi.softAPIP()) return false;
  return true;
}
static bool pxWord(const String &s, bool digitsOnly) {
  if (s.length() == 0 || s.length() > 40) return false;
  for (unsigned int i = 0; i < s.length(); i++) { char c = s[i]; if (digitsOnly ? !isdigit((unsigned char)c) : !(isdigit((unsigned char)c) || (c >= 'a' && c <= 'z'))) return false; }
  return true;
}
static bool pxParamsOk(const String &p) {  // in the form a=1&b=xyz (no line breaks etc.)
  if (p.length() > 512) return false;
  for (unsigned int i = 0; i < p.length(); i++) if ((uint8_t)p[i] < 0x20) return false;
  return true;
}

// The relay itself. Returns true with the device's response in body, or false with the reason in err
static bool pxCall(const IPAddress &ip, int port, const String &path, const String &method, const String &params, String &body, String &err) {
  WiFiClient cl;
  HTTPClient http;
  http.setTimeout(PX_TIMEOUT_MS);
  http.setConnectTimeout(PX_TIMEOUT_MS);
  String url = "http://" + ip.toString() + ":" + String(port) + path;
  if (method == "GET" && params.length() > 0) url += "?" + params;
  if (!http.begin(cl, url)) { err = "Bad URL"; return false; }
  int code;
  if (method == "GET") code = http.GET();
  else {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    code = method == "PUT" ? http.PUT(params) : http.POST(params);
  }
  if (code <= 0) { err = "No answer from " + ip.toString() + ":" + String(port) + " (" + http.errorToString(code) + ")"; http.end(); return false; }
  int size = http.getSize();
  if (size > PX_MAX_BYTES) { err = "Response too large (" + String(size) + " bytes, max " + String(PX_MAX_BYTES) + ")"; http.end(); return false; }
  // responses of unknown size are also cut at 16KB
  body = ""; body.reserve(size > 0 ? size : 1024);
  WiFiClient *st = http.getStreamPtr();
  unsigned long t0 = millis();
  while (http.connected() && (size < 0 || (int)body.length() < size) && millis() - t0 < PX_TIMEOUT_MS) {
    while (st->available()) { body += (char)st->read(); if ((int)body.length() > PX_MAX_BYTES) { err = "Response too large (max " + String(PX_MAX_BYTES) + " bytes)"; http.end(); return false; } }
    if (size >= 0 && (int)body.length() >= size) break;
    delay(1);
  }
  http.end();
  if (code != 200) { err = "HTTP " + String(code) + " from the device: " + body.substring(0, 200); return false; }
  body.trim();
  if (!(body.startsWith("{") || body.startsWith("["))) { err = "The device did not answer JSON"; return false; }
  return true;
}

// ---- /api/external/alpaca ----
static void pxAlpaca() {
  if (!wwGuard()) return;
  if (!web.hasArg("target") || !web.hasArg("device") || !web.hasArg("property")) { wwErr(400, "Missing required parameters (target, device, property)"); return; }
  IPAddress ip;
  if (!pxTargetOk(web.arg("target"), ip)) { wwErr(403, "Target must be a private LAN IPv4 address of another device (10.x, 172.16-31.x, 192.168.x)"); return; }
  long port = web.hasArg("port") ? web.arg("port").toInt() : 80;
  String device = web.arg("device"), number = web.hasArg("number") ? web.arg("number") : "0", property = web.arg("property");
  device.toLowerCase(); property.toLowerCase();
  String method = web.hasArg("method") ? web.arg("method") : "GET"; method.toUpperCase();
  String params = web.hasArg("params") ? web.arg("params") : "";
  if (port < 1 || port > 65535) { wwErr(400, "Bad port"); return; }
  if (!pxWord(device, false) || !pxWord(number, true) || !pxWord(property, false)) { wwErr(400, "device / property: lowercase letters and digits only, number: digits only"); return; }
  if (method != "GET" && method != "PUT" && method != "POST") { wwErr(400, "Invalid method (use GET, PUT, or POST)"); return; }
  if (!pxParamsOk(params)) { wwErr(400, "Bad params"); return; }
  if (property == "imagearray" || property == "imagearrayvariant") { wwErr(413, "Camera images cannot be relayed through this device (too large). Read the image directly from the camera."); return; }
  String body, err;
  if (!pxCall(ip, port, "/api/v1/" + device + "/" + number + "/" + property, method, params, body, err)) { wwErr(502, "Failed to execute Alpaca command: " + err); return; }
  wwOk("Alpaca command executed", body);
}

// ---- /api/external/status, goto, stop (external telescope number 0) ----
static bool pxTarget(IPAddress &ip, int &port) {
  if (!web.hasArg("target")) { wwErr(400, "Missing target parameter"); return false; }
  if (!pxTargetOk(web.arg("target"), ip)) { wwErr(403, "Target must be a private LAN IPv4 address of another device"); return false; }
  port = web.hasArg("port") ? web.arg("port").toInt() : 80;
  if (port < 1 || port > 65535) { wwErr(400, "Bad port"); return false; }
  return true;
}
static void pxStatus() {
  if (!wwGuard()) return;
  IPAddress ip; int port; if (!pxTarget(ip, port)) return;
  String ra, dec, err;
  if (!pxCall(ip, port, "/api/v1/telescope/0/rightascension", "GET", "", ra, err) || !pxCall(ip, port, "/api/v1/telescope/0/declination", "GET", "", dec, err)) { wwErr(502, "Failed to get external telescope status: " + err); return; }
  wwOk("External status retrieved", "{\"ra\":" + ra + ",\"dec\":" + dec + "}");
}
static void pxGoto() {
  if (!wwGuard()) return;
  IPAddress ip; int port; if (!pxTarget(ip, port)) return;
  if (!web.hasArg("ra") || !web.hasArg("dec")) { wwErr(400, "Missing required parameters (target, ra, dec)"); return; }
  String ra = web.arg("ra"), dec = web.arg("dec");
  if (!pxParamsOk(ra) || !pxParamsOk(dec) || ra.indexOf('&') >= 0 || dec.indexOf('&') >= 0) { wwErr(400, "Bad ra or dec"); return; }
  String body, err;
  if (!pxCall(ip, port, "/api/v1/telescope/0/slewtocoordinatesasync", "PUT", "RightAscension=" + ra + "&Declination=" + dec + "&ClientID=4000&ClientTransactionID=1", body, err)) { wwErr(502, "Failed to send external GOTO: " + err); return; }
  wwOk("External GOTO command sent", body);
}
static void pxStop() {
  if (!wwGuard()) return;
  IPAddress ip; int port; if (!pxTarget(ip, port)) return;
  String body, err;
  if (!pxCall(ip, port, "/api/v1/telescope/0/abortslew", "PUT", "ClientID=4000&ClientTransactionID=1", body, err)) { wwErr(502, "Failed to send external stop: " + err); return; }
  wwOk("External stop command sent", body);
}

// ---- /api/external/discover ----
static void pxDiscover() {
  if (!wwGuard()) return;
  WiFiUDP u;
  u.begin(0);
  IPAddress targets[2] = {IPAddress(255, 255, 255, 255), IPAddress(0, 0, 0, 0)};
  if (WiFi.status() == WL_CONNECTED) { IPAddress l = WiFi.localIP(), m = WiFi.subnetMask(); targets[1] = IPAddress(l[0] | (~m[0] & 255), l[1] | (~m[1] & 255), l[2] | (~m[2] & 255), l[3] | (~m[3] & 255)); }
  for (int i = 0; i < 2; i++) if (targets[i] != IPAddress(0, 0, 0, 0)) { u.beginPacket(targets[i], 32227); u.print("alpacadiscovery1"); u.endPacket(); }
  IPAddress ips[10]; int ports[10]; int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 2000) {
    int sz = u.parsePacket();
    if (sz > 0 && sz < 255) {
      char b[256]; int len = u.read(b, 255); if (len < 0) len = 0; b[len] = 0;
      IPAddress rip = u.remoteIP();
      bool dup = false; for (int k = 0; k < n; k++) if (ips[k] == rip) dup = true;
      if (!dup && n < 10 && rip != WiFi.localIP() && rip != WiFi.softAPIP()) {  // skip this device itself
        int port = 80; const char *p = strstr(b, "\"AlpacaPort\":"); if (p) port = atoi(p + 13);
        ips[n] = rip; ports[n] = port > 0 ? port : 80; n++;
      }
    }
    delay(5);
  }
  u.stop();
  String list = "[";
  for (int i = 0; i < n; i++) {
    String devs, err;
    if (!pxCall(ips[i], ports[i], "/management/v1/configureddevices", "GET", "", devs, err)) devs = "[]";
    if (i) list += ",";
    list += "{\"ip\":\"" + ips[i].toString() + "\",\"port\":" + String(ports[i]) + ",\"devices\":" + devs + "}";
  }
  list += "]";
  wwOk("Discovery completed", list);
}

// called from wifiWebBegin
void proxyBegin() {
  web.on("/api/external/alpaca", pxAlpaca);
  web.on("/api/external/status", pxStatus);
  web.on("/api/external/goto", pxGoto);
  web.on("/api/external/stop", pxStop);
  web.on("/api/external/discover", pxDiscover);
}

#endif
