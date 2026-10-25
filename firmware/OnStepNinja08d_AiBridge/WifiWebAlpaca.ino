// -----------------------------------------------------------------------------------
// ASCOM Alpaca Telescope and the reply to Alpaca discovery  2026,10/22 NS3000
//
// - /api/v1/telescope/0/<member>  follows ITelescopeV3 and maps to OnStep LX200 commands (passed to core 1 with wwLx, the same path as the Web API)
// - /management/apiversions, /management/v1/description, /management/v1/configureddevices, /setup (link to the settings page)
// - Answers "alpacadiscovery1" on UDP 32227 with {"AlpacaPort":80} (NINA and others find it automatically). wifiBridgeTask calls alpacaPoll()
// - Replies in the Alpaca form {"Value":..,"ClientTransactionID":n,"ServerTransactionID":n,"ErrorNumber":n,"ErrorMessage":".."}
// - What is not supported returns NotImplemented (0x400) as ASCOM specifies:
//     synchronous GOTO (SlewToCoordinates; the Web server would stop while waiting. CanSlew = false, the async version works), MoveAxis,
//     tracking rate offsets (RA/Dec rate), setting SideOfPier, DestinationSideOfPier, aperture and focal length, refraction setting, setting the guide rate
// - Same guard as the Web API (wwGuard). NINA and other non-browser clients need no PIN. Pages from other sites are refused
// - OnStep 4.24j behaviour: unpark and GOTO are refused until the date and time are set after power-on (UTCDate PUT sets them)

#if NS3000_WIFI == ON && NS3000_WEB == ON

#include <WiFiUdp.h>

#define ALP_NOT_IMPLEMENTED  0x400
#define ALP_INVALID_VALUE    0x401
#define ALP_VALUE_NOT_SET    0x402
#define ALP_NOT_CONNECTED    0x407
#define ALP_INVALID_PARKED   0x408
#define ALP_INVALID_OP       0x40B
#define ALP_UNSPECIFIED      0x4FF

static WiFiUDP alpUdp;
static bool alpConnected = false;
static uint32_t alpServerTx = 0;
static double alpTargetRa = 0, alpTargetDec = 0;
static bool alpTargetRaSet = false, alpTargetDecSet = false;
static double alpSiteElev = 0, alpSettle = 0;

// ---- Arguments (Alpaca argument names are case-insensitive) ----
static bool alpHas(const char *name) { for (int i = 0; i < web.args(); i++) if (web.argName(i).equalsIgnoreCase(name)) return true; return false; }
static String alpArg(const char *name) { for (int i = 0; i < web.args(); i++) if (web.argName(i).equalsIgnoreCase(name)) return web.arg(i); return ""; }
static uint32_t alpClientTx() { String v = alpArg("ClientTransactionID"); v.trim(); long n = v.toInt(); return n > 0 ? (uint32_t)n : 0; }

// ---- Replies ----
static void alpReply(const String &valueJson, int err, const String &msg) {
  String j = "{";
  if (valueJson.length() > 0) j += "\"Value\":" + valueJson + ",";
  j += "\"ClientTransactionID\":" + String(alpClientTx()) + ",\"ServerTransactionID\":" + String(++alpServerTx);
  j += ",\"ErrorNumber\":" + String(err) + ",\"ErrorMessage\":\"" + wwEsc(msg) + "\"}";
  if (wwAllowOrigin.length() > 0) { web.sendHeader("Access-Control-Allow-Origin", wwAllowOrigin); web.sendHeader("Vary", "Origin"); }
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}
static void alpOk(const String &valueJson) { alpReply(valueJson, 0, ""); }
static void alpDone() { alpReply("", 0, ""); }
static void alpErr(int err, const String &msg) { alpReply("", err, msg); }
static void alpNotImpl(const String &what) { alpErr(ALP_NOT_IMPLEMENTED, what + " is not implemented"); }
static void alpBad(const String &msg) { web.send(400, "text/plain", msg); }  // no or unreadable argument: HTTP 400 (as Alpaca specifies)
static String alpBool(bool b) { return b ? "true" : "false"; }
static String alpNum(double v, int d) { char b[32]; dtostrf(v, 1, d, b); return String(b); }

// ---- Reading and writing OnStep values ----
// "HH:MM:SS.SSSS" or "+DD*MM:SS.SSS" to a number (any non-digit is a separator)
static bool alpSex(String s, double &v) {
  s = wwStrip(s); if (s.length() == 0 || s == "0") return false;
  bool neg = s.startsWith("-"); if (s.startsWith("+") || s.startsWith("-")) s = s.substring(1);
  double p[3] = {0, 0, 0}; int k = 0; String cur = "";
  for (unsigned int i = 0; i <= s.length(); i++) {
    char c = i < s.length() ? s[i] : ' ';
    if (isdigit((unsigned char)c) || c == '.') cur += c;
    else if (cur.length() > 0) { if (k < 3) p[k++] = cur.toFloat(); cur = ""; }
  }
  if (k == 0) return false;
  v = p[0] + p[1] / 60.0 + p[2] / 3600.0; if (neg) v = -v;
  return true;
}
static bool alpGet(const char *cmd, double &v) { String r; if (!wwLx(cmd, r)) return false; return alpSex(r, v); }
static String alpHms(double h) {  // HH:MM:SS.SSSS
  while (h < 0) h += 24; while (h >= 24) h -= 24;
  long t = (long)(h * 36000000.0 + 0.5); int H = t / 36000000L; t %= 36000000L; int M = t / 600000L; t %= 600000L;
  char b[24]; snprintf(b, sizeof(b), "%02d:%02d:%02ld.%04ld", H, M, t / 10000L, t % 10000L); return String(b);
}
static String alpDms(double d, int degDigits) {  // sDD*MM:SS.SSS (2 or 3 degree digits). Rounding to seconds makes the ConformU round-trip test off by 1/1000 arcsec, so send 1/1000 s
  char sg = d < 0 ? '-' : '+'; d = fabs(d);
  long t = (long)(d * 3600000.0 + 0.5); int D = t / 3600000L; t %= 3600000L; int M = t / 60000L; t %= 60000L;
  char b[28]; if (degDigits == 3) snprintf(b, sizeof(b), "%c%03d*%02d:%02ld.%03ld", sg, D, M, t / 1000L, t % 1000L); else snprintf(b, sizeof(b), "%c%02d*%02d:%02ld.%03ld", sg, D, M, t / 1000L, t % 1000L);
  return String(b);
}
static String alpDmsDec(double d) {  // sDD*MM:SS.SSS (high-precision declination)
  char sg = d < 0 ? '-' : '+'; d = fabs(d);
  long t = (long)(d * 3600000.0 + 0.5); int D = t / 3600000L; t %= 3600000L; int M = t / 60000L; t %= 60000L;
  char b[28]; snprintf(b, sizeof(b), "%c%02d*%02d:%02ld.%03ld", sg, D, M, t / 1000L, t % 1000L); return String(b);
}

// :GU# flags
// (a function returning a struct collides with the prototypes Arduino generates, so the result goes into the global alpG)
struct AlpGU_t { bool ok, tracking, slewing, parked, home, guiding; char type; };
static AlpGU_t alpG;
static bool alpStatus() {
  AlpGU_t &g = alpG; g.ok = g.tracking = g.slewing = g.parked = g.home = g.guiding = false; g.type = 'E';
  String r; if (!wwLx("GU", r)) return false;
  r = wwStrip(r); int ti = -1;
  for (unsigned int i = 0; i < r.length(); i++) if (r[i] == 'E' || r[i] == 'K' || r[i] == 'A') { ti = i; break; }
  if (ti < 0) return false;
  String fl = r.substring(0, ti);
  g.ok = true; g.type = r[ti];
  g.tracking = fl.indexOf('n') < 0; g.slewing = fl.indexOf('N') < 0; g.parked = fl.indexOf('P') >= 0;
  g.home = fl.indexOf('H') >= 0; g.guiding = fl.indexOf('G') >= 0;
  return true;
}
// Send an LX200 command; true if it replies "1" (OnStep boolean reply)
static bool alpLx1(const String &cmd) { String r; if (!wwLx(cmd, r)) return false; return wwStrip(r).startsWith("1"); }
static int alpTrackRate() { double hz; if (!alpGet("GT", hz)) return 0; if (fabs(hz - 57.9) < 0.3) return 1; if (fabs(hz - 60.0) < 0.05) return 2; if (fabs(hz - 60.136) < 0.014) return 3; return 0; }

// Meaning of the GOTO reply (:MS#) (OnStep)
static String alpGotoMsg(const String &r) {
  switch (r.length() ? r[0] : '?') {
    case '1': return "Below the horizon limit"; case '2': return "Above the overhead limit"; case '3': return "Controller in standby (set the date and time first)";
    case '4': return "Mount is parked"; case '5': return "Slew already in progress"; case '6': return "Outside limits"; case '7': return "Hardware fault";
    case '8': return "Already in motion"; case '9': return "Unspecified error"; default: return "GOTO refused (:MS# = " + r + ")";
  }
}

// Device name shown by Alpaca clients (discovery list, NINA): the Location Name set in the File Manager + "(OnStepNinja)",
// so that several mounts can be told apart, e.g. "NS4000(OnStepNinja)"  2026,10/24
static String alpDevName() { return (nsLoc.length() ? nsLoc : String("NS4000")) + "(OnStepNinja)"; }

// ---- Management ----
static void alpMgmtVersions() { alpOk("[1]"); }
static String alpUid() { uint64_t mac = ESP.getEfuseMac(); char b[40]; snprintf(b, sizeof(b), "ns4000-telescope-%08lx%04lx", (unsigned long)(mac & 0xFFFFFFFF), (unsigned long)((mac >> 32) & 0xFFFF)); return String(b); }
static void alpMgmtDesc() {
  String v; wwLx("=V", v);
  alpOk("{\"ServerName\":\"LUNA OnStepNinja NS-4000\",\"Manufacturer\":\"LUNA Project (OnStepNinja)\",\"ManufacturerVersion\":\"" + wwEsc(wwStrip(v))
        + "\",\"Location\":\"" + wwEsc(nsLoc) + "\"}");
}
static void alpMgmtDevices() {
  alpOk("[{\"DeviceName\":\"" + wwEsc(alpDevName()) + "\",\"DeviceType\":\"Telescope\",\"DeviceNumber\":0,\"UniqueID\":\"" + alpUid() + "\"}]");
}

// ---- Telescope ----
static void alpTelescope(const String &m, bool put) {
  // members answered even when not connected
  if (m == "connected") {
    if (put) { String v = alpArg("Connected"); v.toLowerCase(); if (v != "true" && v != "false") { alpBad("Connected must be true or false"); return; } alpConnected = v == "true"; alpDone(); }
    else alpOk(alpBool(alpConnected));
    return;
  }
  if (m == "connecting" && !put) { alpOk("false"); return; }
  if ((m == "connect" || m == "disconnect") && put) { alpConnected = m == "connect"; alpDone(); return; }
  if (m == "description") { alpOk("\"LUNA OnStepNinja NS-4000 mount (OnStep 4.24j based)\""); return; }
  if (m == "driverinfo") { alpOk("\"LUNA OnStepNinja NS-4000 Alpaca Telescope (built into the firmware). Based on OnStep (GPL v3), LUNA AiBridge\""); return; }
  if (m == "driverversion") { alpOk("\"1.1.1\""); return; }  // same as the firmware version  2026,10/23
  if (m == "interfaceversion") { alpOk("3"); return; }
  if (m == "name") { alpOk("\"" + wwEsc(alpDevName()) + "\""); return; }
  if (m == "supportedactions") { alpOk("[]"); return; }
  if (m == "action" || m == "commandblind" || m == "commandbool" || m == "commandstring") { alpNotImpl(m); return; }

  // capabilities (answered regardless of connection)
  if (m == "canfindhome" || m == "canpark" || m == "canpulseguide" || m == "cansetpark" || m == "cansettracking" || m == "canslewasync"
      || m == "cansync" || m == "canunpark") { alpOk("true"); return; }
  if (m == "canslew" || m == "canslewaltaz" || m == "canslewaltazasync" || m == "cansyncaltaz" || m == "cansetdeclinationrate"
      || m == "cansetrightascensionrate" || m == "cansetguiderates" || m == "cansetpierside") { alpOk("false"); return; }
  if (m == "canmoveaxis") { if (!alpHas("Axis")) { alpBad("Missing Axis"); return; } long a = alpArg("Axis").toInt(); if (a < 0 || a > 2) { alpErr(ALP_INVALID_VALUE, "Axis must be 0, 1 or 2"); return; } alpOk("false"); return; }
  if (m == "trackingrates") { alpOk("[0,1,2,3]"); return; }
  if (m == "equatorialsystem") { alpOk("1"); return; }  // JNow (OnStep coordinates are of the date)

  if (!alpConnected) { alpErr(ALP_NOT_CONNECTED, "Not connected"); return; }

  double v;
  // ---- Reading ----
  if (!put) {
    if (m == "rightascension") { if (alpGet("GRH", v)) alpOk(alpNum(v, 7)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    if (m == "declination") { if (alpGet("GDH", v)) alpOk(alpNum(v, 6)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    if (m == "altitude") { if (alpGet("GA", v)) alpOk(alpNum(v, 5)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    if (m == "azimuth") { if (alpGet("GZ", v)) alpOk(alpNum(v, 5)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    if (m == "siderealtime") { if (alpGet("GSa", v) || alpGet("GS", v)) alpOk(alpNum(v, 6)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    if (m == "sitelatitude") { if (alpGet("GtH", v)) alpOk(alpNum(v, 6)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    if (m == "sitelongitude") { if (alpGet("GgH", v)) alpOk(alpNum(-v, 6)); else alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }  // OnStep: east is negative; ASCOM: east is positive
    if (m == "siteelevation") { alpOk(alpNum(alpSiteElev, 1)); return; }
    if (m == "slewsettletime") { alpOk(String((long)alpSettle)); return; }
    if (m == "targetrightascension") { if (alpTargetRaSet) alpOk(alpNum(alpTargetRa, 7)); else alpErr(ALP_VALUE_NOT_SET, "Target RA not set"); return; }
    if (m == "targetdeclination") { if (alpTargetDecSet) alpOk(alpNum(alpTargetDec, 6)); else alpErr(ALP_VALUE_NOT_SET, "Target Dec not set"); return; }
    AlpGU_t &g = alpG;
    if (m == "tracking" || m == "slewing" || m == "atpark" || m == "athome" || m == "ispulseguiding" || m == "alignmentmode") {
      alpStatus(); if (!g.ok) { alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
      if (m == "tracking") alpOk(alpBool(g.tracking));
      else if (m == "slewing") alpOk(alpBool(g.slewing));
      else if (m == "atpark") alpOk(alpBool(g.parked));
      else if (m == "athome") alpOk(alpBool(g.home));
      else if (m == "ispulseguiding") alpOk(alpBool(g.guiding));
      else alpOk(g.type == 'A' ? "0" : g.type == 'K' ? "1" : "2");  // AltAz / Polar / GermanPolar
      return;
    }
    if (m == "trackingrate") { alpOk(String(alpTrackRate())); return; }
    if (m == "sideofpier") { String r; if (!wwLx("Gm", r)) { alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; } r = wwStrip(r); alpOk(r == "E" ? "0" : r == "W" ? "1" : "-1"); return; }
    if (m == "guideraterightascension" || m == "guideratedeclination") { double x; if (!alpGet("GX90", x)) x = 1.0; alpOk(alpNum(x * 15.0410686 / 3600.0, 8)); return; }  // multiple of sidereal -> deg/s
    if (m == "rightascensionrate" || m == "declinationrate") { alpOk("0.0"); return; }
    if (m == "utcdate") {
      String t, d; if (!wwLx("GX80", t) || !wwLx("GX81", d)) { alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
      t = wwStrip(t); d = wwStrip(d);  // HH:MM:SS and MM/DD/YY
      if (d.length() < 8 || t.length() < 8) { alpErr(ALP_UNSPECIFIED, "Bad date/time from OnStep"); return; }
      alpOk("\"20" + d.substring(6, 8) + "-" + d.substring(0, 2) + "-" + d.substring(3, 5) + "T" + t.substring(0, 8) + ".0000000Z\""); return;
    }
    if (m == "axisrates") { if (!alpHas("Axis")) { alpBad("Missing Axis"); return; } alpOk("[]"); return; }
    if (m == "doesrefraction" || m == "aperturearea" || m == "aperturediameter" || m == "focallength") { alpNotImpl(m); return; }
    if (m == "destinationsideofpier") { alpNotImpl(m); return; }
    alpErr(ALP_NOT_IMPLEMENTED, "Unknown member: " + m);
    return;
  }

  // ---- Writing and commands ----
  if (m == "tracking") {
    String b = alpArg("Tracking"); b.toLowerCase(); if (b != "true" && b != "false") { alpBad("Tracking must be true or false"); return; }
    alpStatus(); AlpGU_t &g = alpG; if (g.parked) { alpErr(ALP_INVALID_PARKED, "Mount is parked"); return; }
    if (alpLx1(b == "true" ? "Te" : "Td")) alpDone(); else alpErr(ALP_INVALID_OP, "OnStep refused (standby or moving)");
    return;
  }
  if (m == "trackingrate") {
    if (!alpHas("TrackingRate")) { alpBad("Missing TrackingRate"); return; }
    long r = alpArg("TrackingRate").toInt(); if (r < 0 || r > 3) { alpErr(ALP_INVALID_VALUE, "TrackingRate must be 0..3"); return; }
    String x; wwLx(r == 0 ? "TQ" : r == 1 ? "TL" : r == 2 ? "TS" : "TK", x); alpDone(); return;
  }
  if (m == "targetrightascension") {
    if (!alpHas("TargetRightAscension")) { alpBad("Missing TargetRightAscension"); return; }
    double h = alpArg("TargetRightAscension").toFloat(); if (h < 0 || h >= 24) { alpErr(ALP_INVALID_VALUE, "TargetRightAscension must be 0..24"); return; }
    alpTargetRa = h; alpTargetRaSet = true; alpDone(); return;
  }
  if (m == "targetdeclination") {
    if (!alpHas("TargetDeclination")) { alpBad("Missing TargetDeclination"); return; }
    double d = alpArg("TargetDeclination").toFloat(); if (d < -90 || d > 90) { alpErr(ALP_INVALID_VALUE, "TargetDeclination must be -90..90"); return; }
    alpTargetDec = d; alpTargetDecSet = true; alpDone(); return;
  }
  if (m == "sitelatitude") {
    if (!alpHas("SiteLatitude")) { alpBad("Missing SiteLatitude"); return; }
    double d = alpArg("SiteLatitude").toFloat(); if (d < -90 || d > 90) { alpErr(ALP_INVALID_VALUE, "SiteLatitude must be -90..90"); return; }
    if (alpLx1("St" + alpDms(d, 2))) alpDone(); else alpErr(ALP_UNSPECIFIED, "OnStep refused"); return;
  }
  if (m == "sitelongitude") {
    if (!alpHas("SiteLongitude")) { alpBad("Missing SiteLongitude"); return; }
    double d = alpArg("SiteLongitude").toFloat(); if (d < -180 || d > 180) { alpErr(ALP_INVALID_VALUE, "SiteLongitude must be -180..180"); return; }
    if (alpLx1("Sg" + alpDms(-d, 3))) alpDone(); else alpErr(ALP_UNSPECIFIED, "OnStep refused"); return;  // OnStep: east is negative
  }
  if (m == "siteelevation") {
    if (!alpHas("SiteElevation")) { alpBad("Missing SiteElevation"); return; }
    double e = alpArg("SiteElevation").toFloat(); if (e < -300 || e > 10000) { alpErr(ALP_INVALID_VALUE, "SiteElevation must be -300..10000"); return; }
    alpSiteElev = e; alpDone(); return;
  }
  if (m == "slewsettletime") {
    if (!alpHas("SlewSettleTime")) { alpBad("Missing SlewSettleTime"); return; }
    long s = alpArg("SlewSettleTime").toInt(); if (s < 0) { alpErr(ALP_INVALID_VALUE, "SlewSettleTime must be >= 0"); return; }
    alpSettle = s; alpDone(); return;
  }
  if (m == "utcdate") {
    // e.g. 2026-10-22T08:55:30.123Z -> local = UTC - :GG# (OnStep :GG# is "add to local time to get UTC")
    String s = alpArg("UTCDate"); s.trim();
    int Y, Mo, D, h, mi; float se;
    if (s.length() < 19 || sscanf(s.c_str(), "%d-%d-%dT%d:%d:%f", &Y, &Mo, &D, &h, &mi, &se) < 6) { alpErr(ALP_INVALID_VALUE, "UTCDate must be ISO 8601 (YYYY-MM-DDTHH:MM:SSZ)"); return; }
    double gg = 0; String g; if (wwLx("GG", g)) gg = wwStrip(g).toFloat();
    struct tm tmv = {}; tmv.tm_year = Y - 1900; tmv.tm_mon = Mo - 1; tmv.tm_mday = D; tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = (int)se;
    time_t t = mktime(&tmv) - (time_t)(gg * 3600.0);  // UTC -> local (TZ is not set, so mktime treats the time as UTC)
    struct tm *lt = gmtime(&t);
    char dbuf[16], tbuf[16];
    snprintf(dbuf, sizeof(dbuf), "SC%02d/%02d/%02d", lt->tm_mon + 1, lt->tm_mday, lt->tm_year % 100);
    snprintf(tbuf, sizeof(tbuf), "SL%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
    if (alpLx1(dbuf) && alpLx1(tbuf)) alpDone(); else alpErr(ALP_UNSPECIFIED, "OnStep refused the date/time");
    return;
  }
  if (m == "abortslew") {
    alpStatus(); AlpGU_t &g = alpG; if (g.parked) { alpErr(ALP_INVALID_PARKED, "Mount is parked"); return; }
    String r; wwLx("Q", r); alpDone(); return;
  }
  if (m == "slewtocoordinatesasync" || m == "synctocoordinates") {
    if (!alpHas("RightAscension") || !alpHas("Declination")) { alpBad("Missing RightAscension or Declination"); return; }
    double ra = alpArg("RightAscension").toFloat(), de = alpArg("Declination").toFloat();
    if (ra < 0 || ra >= 24) { alpErr(ALP_INVALID_VALUE, "RightAscension must be 0..24"); return; }
    if (de < -90 || de > 90) { alpErr(ALP_INVALID_VALUE, "Declination must be -90..90"); return; }
    alpTargetRa = ra; alpTargetDec = de; alpTargetRaSet = alpTargetDecSet = true;
    // fall through to target
  }
  if (m == "slewtocoordinatesasync" || m == "slewtotargetasync" || m == "synctocoordinates" || m == "synctotarget") {
    if (!alpTargetRaSet || !alpTargetDecSet) { alpErr(ALP_INVALID_OP, "Target not set"); return; }
    alpStatus(); AlpGU_t &g = alpG; if (g.parked) { alpErr(ALP_INVALID_PARKED, "Mount is parked"); return; }
    bool sync = m.startsWith("sync");
    if (!sync && !g.tracking) { alpErr(ALP_INVALID_OP, "Tracking is off (turn tracking on before a slew)"); return; }
    if (!alpLx1("Sr" + alpHms(alpTargetRa)) || !alpLx1("Sd" + alpDmsDec(alpTargetDec))) { alpErr(ALP_INVALID_VALUE, "OnStep refused the coordinates"); return; }
    String r; if (!wwLx(sync ? "CM" : "MS", r)) { alpErr(ALP_UNSPECIFIED, "No reply from OnStep"); return; }
    r = wwStrip(r);
    if (sync) { if (r.length() == 0 || r.startsWith("E")) alpErr(ALP_INVALID_OP, "Sync refused: " + r); else alpDone(); }
    else if (r.startsWith("0")) alpDone(); else alpErr(ALP_INVALID_OP, alpGotoMsg(r));
    return;
  }
  if (m == "park") {
    alpStatus(); AlpGU_t &g = alpG; if (g.parked) { alpDone(); return; }
    if (alpLx1("hP")) alpDone(); else alpErr(ALP_INVALID_OP, "Park refused (standby, or park position not set)"); return;
  }
  if (m == "unpark") {
    alpStatus(); AlpGU_t &g = alpG; if (!g.parked) { alpDone(); return; }
    if (alpLx1("hR")) alpDone(); else alpErr(ALP_INVALID_OP, "Unpark refused (set the date and time first)"); return;
  }
  if (m == "setpark") { if (alpLx1("hQ")) alpDone(); else alpErr(ALP_INVALID_OP, "SetPark refused"); return; }
  if (m == "findhome") {
    alpStatus(); AlpGU_t &g = alpG; if (g.parked) { alpErr(ALP_INVALID_PARKED, "Mount is parked"); return; }
    String r; wwLx("hC", r); alpDone(); return;
  }
  if (m == "pulseguide") {
    if (!alpHas("Direction") || !alpHas("Duration")) { alpBad("Missing Direction or Duration"); return; }
    long d = alpArg("Direction").toInt(), ms = alpArg("Duration").toInt();
    if (d < 0 || d > 3) { alpErr(ALP_INVALID_VALUE, "Direction must be 0..3"); return; }
    if (ms < 0 || ms > 16399) { alpErr(ALP_INVALID_VALUE, "Duration must be 0..16399 ms"); return; }
    alpStatus(); AlpGU_t &g = alpG; if (g.parked) { alpErr(ALP_INVALID_PARKED, "Mount is parked"); return; }
    char c[16]; snprintf(c, sizeof(c), "Mg%c%ld", "nsew"[d], ms);
    String r; wwLx(c, r); alpDone(); return;
  }
  if (m == "moveaxis" || m == "slewtocoordinates" || m == "slewtotarget" || m == "slewtoaltaz" || m == "slewtoaltazasync" || m == "synctoaltaz"
      || m == "rightascensionrate" || m == "declinationrate" || m == "sideofpier" || m == "guideraterightascension" || m == "guideratedeclination"
      || m == "doesrefraction") { alpNotImpl(m); return; }
  alpErr(ALP_NOT_IMPLEMENTED, "Unknown member: " + m);
}

// Called from wwNotFound. Answers and returns true for Alpaca URLs
bool alpacaHandle() {
  String u = web.uri(); u.toLowerCase();
  bool put = web.method() == HTTP_PUT;
  if (u == "/management/apiversions" || u == "/management/v1/description" || u == "/management/v1/configureddevices"
      || u.startsWith("/api/v1/telescope/0/")) {
    if (!wwGuard()) return true;
    if (u == "/management/apiversions") alpMgmtVersions();
    else if (u == "/management/v1/description") alpMgmtDesc();
    else if (u == "/management/v1/configureddevices") alpMgmtDevices();
    else alpTelescope(u.substring(strlen("/api/v1/telescope/0/")), put);
    return true;
  }
  if (u.startsWith("/api/v1/")) {  // there are no other device types or numbers
    if (!wwGuard()) return true;
    web.send(400, "text/plain", "Only telescope 0 is available on this device");
    return true;
  }
  if (u == "/setup" || u == "/setup/v1/telescope/0/setup") {  // Alpaca setup page -> this device's settings page
    web.sendHeader("Location", "/settings.html"); web.send(302, "text/plain", "See /settings.html");
    return true;
  }
  return false;
}

// ---- Reply to discovery (UDP 32227) ----
void alpacaBegin() { alpUdp.begin(32227); }
void alpacaPoll() {
  int n = alpUdp.parsePacket();
  if (n <= 0) return;
  char buf[64]; int len = alpUdp.read(buf, sizeof(buf) - 1); if (len < 0) len = 0; buf[len] = 0;
  if (n <= 63 && strstr(buf, "alpacadiscovery1") != NULL) {
    alpUdp.beginPacket(alpUdp.remoteIP(), alpUdp.remotePort());
    alpUdp.print("{\"AlpacaPort\":80}");
    alpUdp.endPacket();
  }
}

#endif
