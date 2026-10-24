// -----------------------------------------------------------------------------------
// Wi-Fi bridge (AiBridge features merged, stage 2)  2026,10/21 NS3000
//
// - Wi-Fi is AP+STA, same as AiBridge. Settings: NS3000_WIFI_* in Config.h
// - LX200 commands on TCP 9999 / 9998 (SkySafari, LUNA Host Lab, ASCOM driver).
//   Normal ':'..'#' commands, ';'..'#' commands with checksum, and the 1-character ACK (0x06)
// - Wi-Fi and TCP run in a core 0 task (wifiBridgeTask). The tracking interrupts and the OnStep loop stay on core 1
// - Commands go to core 1 through a queue; wifiBridgeService() (called from loop2 every time) feeds them one by one into cmdW.
//   OnStep's own processCommands() handles them and the reply (_replyW) comes back through a queue. The motion code is unchanged

#if NS3000_WIFI == ON

#include <WiFi.h>

#define WB_PORTS        2         // 9999, 9998
#define WB_CLIENTS      4         // 0 = 9999, 1 = 9998, 2 = Web (WifiWeb.ino), 3 = BLE handbox (BleHand.ino)
#define WB_CMD_MAX      48
#define WB_REPLY_MAX    56
#define WB_IDLE_MS      30000UL   // close after 30 s without traffic (same as AiBridge; against half-open connections)
#define WB_FRAME_MS     100UL     // drop a partial command if nothing arrives for 100ms (same as AiBridge)
#define WB_REPLY_MS     1000      // longest wait for a reply (ms)

typedef struct { char cmd[WB_CMD_MAX]; uint8_t client; uint32_t seq; } wbReq_t;
typedef struct { char reply[WB_REPLY_MAX]; uint32_t seq; } wbResp_t;

// Connection statistics (per port). Read with :GXW4# (9999) / :GXW5# (9998)  2026,10/21 NS3000
typedef struct {
  volatile uint32_t accepts;       // connections accepted
  volatile uint32_t peerCloses;    // closed by the peer
  volatile uint32_t idleCloses;    // closed by us after 30 s without traffic
  volatile uint32_t replyTimeouts; // replies longer than WB_REPLY_MS
  volatile uint32_t commands;      // commands handled
  volatile uint32_t maxReplyMs;    // longest reply time (ms)
} wbStats_t;
wbStats_t wbStats[WB_CLIENTS];

QueueHandle_t wbReqQ = NULL;
QueueHandle_t wbRespQ[WB_CLIENTS] = {NULL, NULL, NULL, NULL};
String wbApSsid = "";
volatile bool wbStarted = false;

// ---- Core 1 side: pass queued commands to OnStep one by one (called from loop2 every time) ----
void wifiBridgeService() {
  static bool busy=false;
  static uint8_t client=0;
  static uint32_t seq=0;
  if (wbReqQ == NULL) return;

  if (busy) {
    if (_replyWDone) {
      wbResp_t r; strncpy(r.reply,_replyW,WB_REPLY_MAX-1); r.reply[WB_REPLY_MAX-1]=0; r.seq=seq;
      xQueueSend(wbRespQ[client],&r,0);
      busy=false;
    }
    return;
  }

  wbReq_t q;
  if (xQueueReceive(wbReqQ,&q,0) != pdTRUE) return;
  cmdW.flush(); _replyW[0]=0; _replyWDone=false;
  for (int i=0; q.cmd[i] != 0; i++) cmdW.add(q.cmd[i]);
  if (cmdW.ready()) { busy=true; client=q.client; seq=q.seq; }
  else { cmdW.flush(); wbResp_t r; r.reply[0]=0; r.seq=q.seq; xQueueSend(wbRespQ[q.client],&r,0); }  // malformed command: no reply
}

// ---- Information for :GXWn# (called from core 1) ----
void wifiBridgeInfo(int n, char *reply) {
  switch (n) {
    case 0: strcpy(reply, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "0.0.0.0"); break;
    case 1: strncpy(reply, wbApSsid.c_str(), 40); reply[40]=0; break;
    case 2: strcpy(reply, WiFi.softAPIP().toString().c_str()); break;
    case 3: sprintf(reply, "%d", (int)WiFi.status()); break;  // 3 = connected
    default: {  // 4 = 9999, 5 = 9998: accepted,peer closed,idle closed,reply timeouts,commands,longest ms
      wbStats_t &t=wbStats[n == 4 ? 0 : 1];
      sprintf(reply, "%lu,%lu,%lu,%lu,%lu,%lu", (unsigned long)t.accepts, (unsigned long)t.peerCloses, (unsigned long)t.idleCloses,
        (unsigned long)t.replyTimeouts, (unsigned long)t.commands, (unsigned long)t.maxReplyMs);
    } break;
  }
}

// ---- Core 0 side: pass one command to core 1 and wait for the reply (shared by TCP and Web) ----
// Each command gets a number and only the reply with the same number is used (a late reply to an earlier command is not mixed up)
// Returns true when a reply arrives (reply is empty for commands without one). false after WB_REPLY_MS
bool wbCommand(uint8_t idx, const char *cmd, char *reply, int replyMax) {
  static uint32_t nextSeq=1;
  reply[0]=0;
  wbReq_t q; strncpy(q.cmd,cmd,WB_CMD_MAX-1); q.cmd[WB_CMD_MAX-1]=0; q.client=idx; q.seq=nextSeq++;
  unsigned long t0=millis();
  if (xQueueSend(wbReqQ,&q,pdMS_TO_TICKS(200)) != pdTRUE) { wbStats[idx].replyTimeouts++; return false; }
  wbResp_t r;
  for (;;) {
    long left=(long)WB_REPLY_MS-(long)(millis()-t0);
    if (left <= 0 || xQueueReceive(wbRespQ[idx],&r,pdMS_TO_TICKS(left)) != pdTRUE) { wbStats[idx].replyTimeouts++; return false; }
    if (r.seq == q.seq) break;  // discard old replies and keep waiting
  }
  unsigned long ms=millis()-t0;
  wbStats[idx].commands++; if (ms > wbStats[idx].maxReplyMs) wbStats[idx].maxReplyMs=ms;
  strncpy(reply,r.reply,replyMax-1); reply[replyMax-1]=0;
  return true;
}

// ---- Core 0 side: state and handling of one TCP connection ----
// (member functions, so the types do not collide with the prototypes Arduino generates)
struct wbConn_t {
  WiFiServer *server;
  WiFiClient client;
  bool connected;
  char buf[WB_CMD_MAX];
  int len;
  unsigned long frameStart;
  unsigned long lastActivity;
  uint8_t idx;

  void reset() { len=0; buf[0]=0; }

  // pass one command to core 1, wait for the reply and send it back
  void execute() {
    char reply[WB_REPLY_MAX];
    if (wbCommand(idx,buf,reply,sizeof(reply)) && reply[0] != 0) client.print(reply);
  }

  void poll() {
    if (!connected) {
      WiFiClient n = server->available();
      if (n) { client=n; connected=true; client.setNoDelay(true); reset(); lastActivity=millis(); wbStats[idx].accepts++; }
      return;
    }
    if (!client.connected()) { wbStats[idx].peerCloses++; client.stop(); connected=false; reset(); return; }
    if (millis()-lastActivity > WB_IDLE_MS) { wbStats[idx].idleCloses++; client.stop(); connected=false; reset(); return; }
    if (len > 0 && millis()-frameStart > WB_FRAME_MS) reset();

    while (client.available()) {
      char ch = client.read();
      if (len == 0) {
        if (ch == (char)6) {  // ACK: complete in 1 character
          buf[0]=(char)6; buf[1]=0; execute(); lastActivity=millis(); reset();
        } else if (ch == ':' || ch == ';') {
          buf[0]=ch; len=1; buf[1]=0; frameStart=millis();
        }
        continue;  // discard anything else
      }
      if (len < WB_CMD_MAX-1) { buf[len++]=ch; buf[len]=0; }
      else { reset(); continue; }  // too long
      if (ch == '#') { execute(); lastActivity=millis(); reset(); }
    }
  }
};

void wifiBridgeTask(void *p) {
  // Start Wi-Fi (AP+STA, same as AiBridge. STA uses DHCP)
#if NS3000_WEB == ON
  nsCfgLoad();  // mount SPIFFS and read the config file (Wi-Fi, location name, PIN)  2026,10/22
#endif
  WiFi.mode(WIFI_AP_STA);
#if NS3000_BLE == ON
  WiFi.setSleep(true);   // with BLE at the same time, the ESP32 requires modem sleep (without it, starting BLE stops)  2026,10/22
#else
  WiFi.setSleep(false);
#endif
  {
    uint64_t mac = ESP.getEfuseMac();
    char s[5]; snprintf(s,sizeof(s),"%04X",(uint16_t)(mac & 0xFFFF));
    wbApSsid = String(NS3000_WIFI_AP_PREFIX) + String(s);
  }
  if (strlen(NS3000_WIFI_AP_PASS) >= 8) WiFi.softAP(wbApSsid.c_str(), NS3000_WIFI_AP_PASS);
  else WiFi.softAP(wbApSsid.c_str());
#if NS3000_WEB == ON
  WiFi.begin(nsCfgSsid(), nsCfgPass());  // values from the config file /aibridge_net_cfg.dat (or Config.h). Does not wait for the connection (the AP works at once)  2026,10/22
#else
  WiFi.begin(NS3000_WIFI_STA_SSID, NS3000_WIFI_STA_PASS);  // does not wait for the connection (the AP works at once)
#endif

  static WiFiServer server9999(9999), server9998(9998);
  server9999.begin(); server9998.begin();
  static wbConn_t conn[WB_PORTS];
  conn[0].server=&server9999; conn[1].server=&server9998;
  for (int i=0; i<WB_PORTS; i++) { conn[i].idx=i; conn[i].connected=false; conn[i].reset(); }
#if NS3000_WEB == ON
  alpacaBegin();  // reply to Alpaca discovery (UDP 32227)  2026,10/22
  wifiWebBegin();  // Web (port 80) and SPIFFS  2026,10/21 NS3000
#endif
  wbStarted=true;

  unsigned long lastRetry=millis();
  for (;;) {
    for (int i=0; i<WB_PORTS; i++) conn[i].poll();
#if NS3000_WEB == ON
    wifiWebPoll();
    alpacaPoll();
#endif
    // if the STA connection is lost, reconnect every 30 seconds
    if (WiFi.status() != WL_CONNECTED && millis()-lastRetry > 30000UL) { lastRetry=millis(); WiFi.reconnect(); }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// call at the end of setup()
void wifiBridgeBegin() {
  wbReqQ = xQueueCreate(4, sizeof(wbReq_t));
  for (int i=0; i<WB_CLIENTS; i++) wbRespQ[i] = xQueueCreate(2, sizeof(wbResp_t));
  memset((void*)wbStats, 0, sizeof(wbStats));
  xTaskCreatePinnedToCore(wifiBridgeTask, "wifiBridge", 12288, NULL, 1, NULL, 0);  // core 0 (12KB because it also runs the Web server and SPIFFS)
#if NS3000_BLE == ON
  bleHandBegin();  // BLE handbox (BleHand.ino)  2026,10/22
#endif
}

#endif
