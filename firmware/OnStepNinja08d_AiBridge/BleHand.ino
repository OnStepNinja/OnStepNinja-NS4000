// -----------------------------------------------------------------------------------
// BLE wireless handbox (mount side = central)  2026,10/22 NS3000
//
// - The handbox (ESP32 / ESP32-C3, separate sketch addons/BleHandBox) is the peripheral. The mount scans and connects
//   The peripheral can sleep between events with slave latency, so the handbox battery lasts longer
// - NimBLE-Arduino 1.4.3. BLE runs on core 0 (with Wi-Fi). The tracking interrupts and the OnStep loop stay on core 1
// - The handbox reports its buttons in 4 bytes (on change, every 250ms while pressed, every 2 s when released)
//     [0] 0x4E ('N', format marker)  [1] buttons bit0 N bit1 S bit2 E bit3 W bit4 Stop bit5 Speed  [2] handbox battery voltage (20mV units; 0xFF = not measured; always 0xFF up to v0.2)  [3] sequence number
//   The speed button works like the NS-3000 SEL button (uses the Guide.ino code as is: slew -> move -> center -> guide, with the same sound and LED)
//   The mount turns button changes into LX200 commands (:Mn# :Qn# :Rn# :Q#) and passes them to OnStep through the same queue as Wi-Fi (client 3)
// - Safety: while the handbox is moving the mount, if the reports stop for 1 s or the link is lost, :Q# stops the mount
//   (when the handbox is not moving the mount, e.g. during a SkySafari GOTO, nothing is stopped)
// - Registration: within 30 minutes after power-on, with no handbox registered, the first one found is paired (bonded) and registered
//   After that only that one is accepted (a device that cannot encrypt is disconnected). Remove the registration in the File Manager (within 30 minutes)
// - License: this file is part of the NS-4000 firmware, GPL v3 like OnStep. The handbox sketch (addons/BleHandBox) is a separate program with its own license (freeware, no commercial use)
// - Using Wi-Fi and BLE at the same time requires Wi-Fi power saving (modem sleep) on the ESP32 (WifiBridge.ino)

#if NS3000_WIFI == ON && NS3000_BLE == ON

#include <NimBLEDevice.h>

#define BH_CLIENT       3                                          // wbCommand() client number (0/1 = TCP, 2 = Web)
#define BH_SVC_UUID     "af926cb9-6e68-4e48-ae4f-7db856cea953"
#define BH_KEY_UUID     "8ec6a12d-99f3-4c2c-9e16-c6d188d5e9d0"
#define BH_ALIVE_MS     1000UL                                     // while moving, stop if no report arrives for longer than this
#define BH_REG_MS       (30UL * 60UL * 1000UL)                     // registration is possible for 30 minutes after power-on (same as the File Manager)

typedef struct { uint8_t d[4]; } bhPkt_t;
static QueueHandle_t bhQ = NULL;
volatile uint8_t bhState = 0;          // 0 = not registered, 1 = registered (not connected), 2 = connected
volatile bool bhForgetReq = false;     // "Remove registration" in the File Manager
volatile bool bhSelReq = false;        // the speed button was pressed (ST4() in Guide.ino handles it like the NS-3000 SEL button)
char bhAddr[18] = "";                  // address of the registered (connected) handbox
volatile uint8_t bhVbat = 0xFF;   // handbox battery voltage (20mV units, 0xFF = unknown). Shown in the Console  2026,10/23
volatile int8_t bhRssi = 0, bhRssiMin = 0;   // signal strength while connected (dBm, 0 = not measured). Min = weakest value in this connection
volatile uint32_t bhConnects = 0, bhAliveStops = 0, bhLinkStops = 0, bhPkts = 0;

static NimBLEAddress *bhFound = NULL;  // device found by the scan
static volatile bool bhHaveFound = false;

class BhAdv : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *d) {
    if (bhHaveFound || !d->isAdvertisingService(NimBLEUUID(BH_SVC_UUID))) return;
    NimBLEAddress a = d->getAddress();
    if (NimBLEDevice::getNumBonds() > 0) { if (!NimBLEDevice::isBonded(a)) return; }  // if registered, only that one
    else if (millis() >= BH_REG_MS) return;                                            // not registered and 30 minutes have passed: do not register a new one
    *bhFound = a; bhHaveFound = true;
    NimBLEDevice::getScan()->stop();
  }
};

extern volatile bool wbStarted;  // WifiBridge.ino (files are joined in name order, so it is not visible here yet)
static void bhSend(const char *cmd) { char r[64]; wbCommand(BH_CLIENT, cmd, r, sizeof(r)); }

void bleHandTask(void *p) {
  while (!wbStarted) vTaskDelay(pdMS_TO_TICKS(100));  // Wi-Fi first (start BLE after modem sleep is on)
  NimBLEDevice::init("NS-4000");
  NimBLEDevice::setSecurityAuth(true, false, true);   // bonding, Just Works (the handbox has no display), LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  bhFound = new NimBLEAddress();
  NimBLEScan *sc = NimBLEDevice::getScan();
  static BhAdv adv;
  sc->setAdvertisedDeviceCallbacks(&adv, false);
  sc->setActiveScan(false);
  sc->setInterval(160); sc->setWindow(30);            // the radio is shared with Wi-Fi, so listen only briefly (30ms of every 160ms)
  NimBLEClient *cl = NimBLEDevice::createClient();
  cl->setConnectionParams(24, 40, 0, 200);            // interval 30-50ms, disconnect after 2 s without packets
  cl->setConnectTimeout(5);

  uint8_t keys = 0;
  unsigned long lastPkt = 0;
  for (;;) {
    if (bhForgetReq) {  // remove the registration
      if (cl->isConnected()) cl->disconnect();
      NimBLEDevice::deleteAllBonds();
      bhAddr[0] = 0; bhForgetReq = false;
    }
    if (!cl->isConnected()) {
      if (bhState == 2 && (keys & 0x0F)) { bhSend(":Q#"); bhLinkStops++; }  // the link was lost while the handbox was moving the mount
      keys = 0; bhRssi = 0; bhVbat = 0xFF;  // after a disconnect the voltage is unknown too (do not show an old value right after reconnecting)
      bhState = NimBLEDevice::getNumBonds() > 0 ? 1 : 0;
      if (bhState == 0 && millis() >= BH_REG_MS) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }  // no registration and 30 minutes passed: stop searching
      xQueueReset(bhQ);
      bhHaveFound = false;
      sc->start(3, false);  // search for up to 3 s (stops as soon as one is found)
      sc->clearResults();
      if (!bhHaveFound) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      if (!cl->connect(*bhFound)) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      if (!cl->secureConnection()) { cl->disconnect(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }  // cannot encrypt (not the registered device): disconnect
      NimBLERemoteService *s = cl->getService(BH_SVC_UUID);
      NimBLERemoteCharacteristic *c = s ? s->getCharacteristic(BH_KEY_UUID) : NULL;
      if (!c || !c->subscribe(true, [](NimBLERemoteCharacteristic *rc, uint8_t *d, size_t n, bool isNotify) {
            if (n >= 4) { bhPkt_t k; memcpy(k.d, d, 4); xQueueSend(bhQ, &k, 0); }
          })) { cl->disconnect(); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
      strncpy(bhAddr, cl->getPeerAddress().toString().c_str(), sizeof(bhAddr) - 1); bhAddr[sizeof(bhAddr) - 1] = 0;
      bhState = 2; bhConnects++; lastPkt = millis(); bhRssi = 0; bhRssiMin = 0;
    }

    // Signal strength (for the Console; a guide to the distance). Read every 40ms and use the strongest value of the last 1 s (25 readings).
    // When the handbox has nothing to send it skips up to 4 connection events. Reading a skipped event returns a noise value (-100 or less),
    // so only the values of received events (the strongest) are used (a quirk of the ESP32 reading)
    {
      static unsigned long lastRssi = 0;
      static int8_t ring[25]; static uint8_t ri = 0, rn = 0;
      if (millis() - lastRssi >= 40) {
        lastRssi = millis();
        int r = cl->getRssi();
        if (r != 0) {
          ring[ri] = (int8_t)r; ri = (ri + 1) % 25; if (rn < 25) rn++;
          int8_t best = -128; for (uint8_t i = 0; i < rn; i++) if (ring[i] > best) best = ring[i];
          bhRssi = best; if (rn >= 25 && (bhRssiMin == 0 || best < bhRssiMin)) bhRssiMin = best;
        }
      }
      if (bhRssi == 0) rn = 0;  // start counting again after a disconnect
    }

    bhPkt_t k;
    if (xQueueReceive(bhQ, &k, pdMS_TO_TICKS(50)) == pdTRUE && k.d[0] == 0x4E) {
      lastPkt = millis(); bhPkts++;
      uint8_t b = k.d[1];
      char cmd[8];
      if ((b & 0x10) && !(keys & 0x10)) bhSend(":Q#");  // stop button (the moment it is pressed)
      if ((b & 0x20) && !(keys & 0x20)) bhSelReq = true;  // speed button (the moment it is pressed)
      if (k.d[2] != 0xFF) bhVbat = k.d[2];  // battery voltage (this byte used to be the rate 0..9, which was not used, so it now carries the voltage)
      static const char dirs[4] = {'n', 's', 'e', 'w'};
      for (int i = 0; i < 4; i++) {
        uint8_t bit = 1 << i;
        if ((b ^ keys) & bit) { sprintf(cmd, (b & bit) ? ":M%c#" : ":Q%c#", dirs[i]); bhSend(cmd); }
      }
      keys = b;
    }
    if ((keys & 0x0F) && millis() - lastPkt > BH_ALIVE_MS) { bhSend(":Q#"); keys &= 0xF0; bhAliveStops++; }  // the reports stopped
  }
}

// called from wifiBridgeBegin()
void bleHandBegin() {
  bhQ = xQueueCreate(8, sizeof(bhPkt_t));
  xTaskCreatePinnedToCore(bleHandTask, "bleHand", 6144, NULL, 1, NULL, 0);
}

#endif
