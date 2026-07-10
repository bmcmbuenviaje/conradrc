/* ================================================================
   RC SIM RACING ARCADE — MASTER TRANSMITTER (Desk Side)
   ----------------------------------------------------------------
   Board : ENGLAB ESP32 DevKit (USB, Arduino core)
   Role  : Bridge the browser (Web Serial, 115200 baud) to the RC
           fleet over ESP-NOW.

   Serial protocol (lean, newline-terminated ASCII from the web app):
     CAR,AA,BB,CC,DD,EE,FF   -> swap the active target peer (MAC bytes hex)
     DRIVE,<servo>,<motor>   -> servo 0..180, motor -255..255 (neg = reverse)

   The active target is added/updated as an ESP-NOW peer on the fly.
   DRIVE frames are forwarded verbatim as a compact binary struct.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

/* ---------- Wire format shared with the receiver ---------- */
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180
  int16_t  motor;   // -255..255  (negative = reverse, 0 = stop)
  uint32_t seq;     // rolling sequence for loss/latency diagnostics
} DriveFrame;

/* ---------- State ---------- */
static uint8_t   currentPeer[6] = {0};
static bool      peerActive     = false;
static uint32_t  txSeq          = 0;
static char      lineBuf[64];
static uint8_t   lineLen        = 0;

/* ---------- Helpers ---------- */
static void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (mac[i] < 0x10) Serial.print('0');
    Serial.print(mac[i], HEX);
  }
}

// ESP-NOW send-status callback (Arduino-ESP32 3.x signature).
static void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // Emit a terse telemetry line the browser read-loop can display.
  Serial.print("ACK,");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

/* Swap (or refresh) the single active peer from six hex byte tokens. */
static bool swapPeer(uint8_t mac[6]) {
  // Remove the previous peer if one is registered.
  if (peerActive && memcmp(currentPeer, mac, 6) != 0) {
    esp_now_del_peer(currentPeer);
    peerActive = false;
  }

  if (esp_now_is_peer_exist(mac)) {
    memcpy(currentPeer, mac, 6);
    peerActive = true;
    return true;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;           // 0 = use current WiFi channel
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;

  if (esp_now_add_peer(&peer) == ESP_OK) {
    memcpy(currentPeer, mac, 6);
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
  // args points past "CAR," — expect six comma-separated hex bytes.
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
  // args points past "DRIVE," — expect "<servo>,<motor>".
  char *sTok = strtok(args, ",");
  char *mTok = strtok(nullptr, ",");
  if (!sTok || !mTok) { Serial.println("ERR,DRIVE_LEN"); return; }

  int servo = atoi(sTok);
  int motor = atoi(mTok);
  servo = constrain(servo, 0, 180);
  motor = constrain(motor, -255, 255); // signed: negative = reverse

  if (!peerActive) { Serial.println("ERR,NO_TARGET"); return; }

  DriveFrame frame;
  frame.servo = (uint8_t) servo;
  frame.motor = (int16_t) motor;
  frame.seq   = ++txSeq;

  esp_now_send(currentPeer, (uint8_t *) &frame, sizeof(frame));
  // No verbose echo per-frame at 50Hz; ACK callback reports link health.
}

/* Dispatch a completed line. */
static void processLine(char *line) {
  if (strncmp(line, "CAR,", 4) == 0) {
    handleCar(line + 4);
  } else if (strncmp(line, "DRIVE,", 6) == 0) {
    handleDrive(line + 6);
  } else if (line[0] != '\0') {
    Serial.println("ERR,UNKNOWN");
  }
}

/* ---------- Non-blocking serial line reader ---------- */
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
      lineLen = 0; // overflow guard — drop the malformed line
      Serial.println("ERR,OVERFLOW");
    }
  }
}

/* ---------- Arduino lifecycle ---------- */
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

  Serial.print("READY,MASTER,");
  Serial.println(WiFi.macAddress());
}

void loop() {
  pumpSerial();
}
