/* ================================================================
   RC SIM RACING ARCADE — MASTER TRANSMITTER (Desk Side)
   ----------------------------------------------------------------
   Board : ENGLAB ESP32 DevKit (USB, Arduino core)
   Role  : Bridge the browser (Web Serial, 115200 baud) to the RC
           fleet over ESP-NOW.

   Serial protocol (lean, newline-terminated ASCII):
     Browser -> master:
       CAR,AA,BB,CC,DD,EE,FF   -> swap the active target peer (MAC hex)
       DRIVE,<servo>,<motor>   -> servo 0..180, motor -255..255 (neg = reverse)
     Master -> browser:
       READY,MASTER,<mac>      Boot banner (this master's MAC)
       OK,CAR,<mac>            Peer swap accepted
       ACK,<seq>,OK|FAIL       ESP-NOW delivery status per frame
       TELEM,<millivolts>,<rssi>,<failsafes>,<flags>
                               Car-side telemetry, relayed as-is
       CLAIM,<masterMac>,<carMac>
                               Another master reported it's driving carMac
                               (best-effort ESP-NOW broadcast presence)
       ERR,<code>              Parse/target error

   Security: an optional Primary Master Key (PMK) enables ESP-NOW
   payload encryption on the DRIVE peer. Set ENABLE_CRYPTO=true and
   put the same 16-byte PMK+LMK on every master and every car.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

/* ---------- Optional payload encryption ----------
   Anyone within 2.4 GHz range who knows a car's MAC could otherwise send
   DriveFrames to it. Turning this on requires the SAME 16-byte keys on
   BOTH the master AND every car. Keys below are placeholders — change
   them, and treat them as a shared secret for your fleet. Broadcast
   presence stays unencrypted (it's public-by-design "who's driving what"). */
static const bool     ENABLE_CRYPTO = false;
static const uint8_t  PMK[16] = { 'R','C','A','R','C','A','D','E','_','P','M','K','_','_','_','1' };
static const uint8_t  LMK[16] = { 'R','C','A','R','C','A','D','E','_','L','M','K','_','_','_','1' };

/* ---------- Wire formats shared with the receiver ---------- */
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180
  int16_t  motor;   // -255..255  (negative = reverse, 0 = stop)
  uint32_t seq;     // rolling sequence for loss/latency diagnostics
} DriveFrame;

// Sent BY the car BACK to the master.
// v2: adds failsafes count + status flags (bit0 = brownout suspected).
typedef struct __attribute__((packed)) {
  uint16_t vbat_mv;   // battery millivolts (0 if unmeasured)
  int8_t   rssi;      // dBm the car heard from the master
  uint16_t failsafes; // number of link-lost failsafes since boot
  uint8_t  flags;     // bit0: brownout hint; bit1..7 reserved
} TelemetryFrame;

// Broadcast BY each master to announce which car (if any) it's driving.
// Sent to ff:ff:ff:ff:ff:ff periodically; other masters relay to their browser.
typedef struct __attribute__((packed)) {
  char     tag[4];  // "CLM\0"
  uint8_t  car[6];  // claimed car MAC (all zero = idle / released)
} PresenceFrame;

/* ---------- Constants ---------- */
static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint32_t PRESENCE_MS  = 1000;  // announce our claim once/sec

/* ---------- State ---------- */
static uint8_t   currentPeer[6] = {0};
static bool      peerActive     = false;
static uint32_t  txSeq          = 0;
static char      lineBuf[64];
static uint8_t   lineLen        = 0;
static uint8_t   claimedCar[6]  = {0};  // what THIS master claims
static uint32_t  lastPresenceMs = 0;

/* ---------- Helpers ---------- */
static void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (mac[i] < 0x10) Serial.print('0');
    Serial.print(mac[i], HEX);
  }
}

// ESP-NOW send-status callback. Emits "ACK,<seq>,OK|FAIL" for the browser
// to compute latency + packet loss. Presence broadcasts also generate an
// ACK but we filter those (they always target the broadcast MAC).
static void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (info && memcmp(info->des_addr, BROADCAST, 6) == 0) return;
  Serial.print("ACK,");
  Serial.print(txSeq);
  Serial.print(',');
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ESP-NOW receive: telemetry from the car OR presence from another master.
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(TelemetryFrame)) {
    TelemetryFrame t;
    memcpy(&t, data, sizeof(t));
    Serial.print("TELEM,");
    Serial.print(t.vbat_mv);   Serial.print(',');
    Serial.print(t.rssi);      Serial.print(',');
    Serial.print(t.failsafes); Serial.print(',');
    Serial.println(t.flags);
    return;
  }
  if (len == sizeof(PresenceFrame)) {
    PresenceFrame p;
    memcpy(&p, data, sizeof(p));
    if (p.tag[0] == 'C' && p.tag[1] == 'L' && p.tag[2] == 'M') {
      Serial.print("CLAIM,");
      printMac(info->src_addr); Serial.print(',');
      printMac(p.car);          Serial.println();
    }
    return;
  }
}

/* Register a peer (encrypted if ENABLE_CRYPTO, else clear). */
static bool addPeerFor(const uint8_t mac[6], bool encrypted) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = encrypted;
  if (encrypted) memcpy(peer.lmk, LMK, 16);
  return esp_now_add_peer(&peer) == ESP_OK;
}

/* Swap (or refresh) the single active DRIVE peer. */
static bool swapPeer(uint8_t mac[6]) {
  if (peerActive && memcmp(currentPeer, mac, 6) != 0) {
    esp_now_del_peer(currentPeer);
    peerActive = false;
  }
  if (addPeerFor(mac, ENABLE_CRYPTO)) {
    memcpy(currentPeer, mac, 6);
    memcpy(claimedCar,  mac, 6); // announce this car in our next presence beacon
    peerActive = true;
    return true;
  }
  return false;
}

/* Parse one "hex" token like "AA" (0..255). Returns -1 on error. */
static int parseHexByte(const char *tok) {
  int v = (int) strtol(tok, nullptr, 16);
  if (v < 0 || v > 255) return -1;
  return v;
}

/* ---------- Command handlers ---------- */
static void handleCar(char *args) {
  uint8_t mac[6];
  int n = 0;
  char *tok = strtok(args, ",");
  while (tok && n < 6) {
    int b = parseHexByte(tok);
    if (b < 0) { Serial.println("ERR,CAR_BADBYTE"); return; }
    mac[n++] = (uint8_t) b;
    tok = strtok(nullptr, ",");
  }
  if (n != 6) { Serial.println("ERR,CAR_LEN"); return; }

  if (swapPeer(mac)) {
    Serial.print("OK,CAR,");
    printMac(mac);
    Serial.println();
  } else {
    Serial.println("ERR,CAR_ADDPEER");
  }
}

static void handleDrive(char *args) {
  char *sTok = strtok(args, ",");
  char *mTok = strtok(nullptr, ",");
  if (!sTok || !mTok) { Serial.println("ERR,DRIVE_LEN"); return; }

  int servo = atoi(sTok);
  int motor = atoi(mTok);
  servo = constrain(servo, 0, 180);
  motor = constrain(motor, -255, 255);

  if (!peerActive) { Serial.println("ERR,NO_TARGET"); return; }

  DriveFrame frame;
  frame.servo = (uint8_t) servo;
  frame.motor = (int16_t) motor;
  frame.seq   = ++txSeq;
  esp_now_send(currentPeer, (uint8_t *) &frame, sizeof(frame));
}

/* Presence broadcast — advertise which car this master is currently driving. */
static void sendPresence() {
  PresenceFrame p = {};
  p.tag[0] = 'C'; p.tag[1] = 'L'; p.tag[2] = 'M'; p.tag[3] = 0;
  memcpy(p.car, claimedCar, 6);
  esp_now_send(BROADCAST, (uint8_t *) &p, sizeof(p));
}

/* Dispatch a completed line. */
static void processLine(char *line) {
  if (strncmp(line, "CAR,", 4) == 0) {
    handleCar(line + 4);
  } else if (strncmp(line, "DRIVE,", 6) == 0) {
    handleDrive(line + 6);
  } else if (strcmp(line, "RELEASE") == 0) {
    memset(claimedCar, 0, 6); // browser deselected the car
  } else if (line[0] != '\0') {
    Serial.println("ERR,UNKNOWN");
  }
}

static void pumpSerial() {
  while (Serial.available()) {
    char c = (char) Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        processLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;
      Serial.println("ERR,OVERFLOW");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERR,ESPNOW_INIT");
    ESP.restart();
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  if (ENABLE_CRYPTO) esp_now_set_pmk(PMK);

  // Broadcast peer for presence beacons (always unencrypted so any master hears).
  addPeerFor(BROADCAST, false);

  Serial.print("READY,MASTER,");
  Serial.println(WiFi.macAddress());
}

void loop() {
  pumpSerial();
  const uint32_t now = millis();
  if (now - lastPresenceMs >= PRESENCE_MS) {
    lastPresenceMs = now;
    sendPresence();
  }
}
