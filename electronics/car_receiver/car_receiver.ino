/* ================================================================
   RC SIM RACING ARCADE — CAR RECEIVER (Vehicle Side)
   ----------------------------------------------------------------
   Board : ESP32 (Arduino core) mounted in the RC chassis
   Role  : Receive ESP-NOW DriveFrames from the master transmitter
           and drive hardware PWM outputs:
              - Steering servo   -> GPIO 18 (D18)
              - ESC / drive motor-> GPIO 19 (D19)

   Steering : standard proportional servo, 0..180 deg (90 = center).
   ESC      : servo-style pulse; -255..255 signed motor (neg = reverse).

   TELEMETRY: learns the master's MAC from the first frame it receives,
   then periodically sends back battery voltage, RSSI, failsafe count,
   and status flags. The master relays these to the browser HUD.

   Security: if ENABLE_CRYPTO=true here AND on the master, ESP-NOW
   payloads on the master-to-car link are encrypted. The PMK/LMK below
   MUST match every master and every car on the same fleet.

   Rate-limit: incoming frames are throttled (min interval MIN_RX_INTERVAL_MS)
   so a misbehaving/malicious peer can't jam the loop.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESP32Servo.h>
#include <esp_system.h>
#include <esp_mac.h>

/* ---------- Optional payload encryption (must match master) ---------- */
static const bool     ENABLE_CRYPTO = false;
static const uint8_t  PMK[16] = { 'R','C','A','R','C','A','D','E','_','P','M','K','_','_','_','1' };
static const uint8_t  LMK[16] = { 'R','C','A','R','C','A','D','E','_','L','M','K','_','_','_','1' };

/* ---------- Long Range (LR) mode (must match master) ----------
   ESP-NOW LR uses a lower-bitrate modulation for extended range (~1.5-3x
   gain, ~5-10 ms extra latency). MUST be identical on master and every
   car — mixing LR and normal means one side can't hear the other. */
static const bool     ENABLE_LR_MODE = true;

/* ---------- Pin map ---------- */
static const int PIN_STEER   = 18;  // D18 — steering servo signal
static const int PIN_ESC     = 19;  // D19 — ESC / drive motor signal
static const int PIN_BATTERY = 34;  // ADC1 — battery sense via divider (optional)

/* ---------- Battery sense (optional) ---------- */
static const float BATTERY_DIVIDER = 4.03f;
static const bool  BATTERY_ENABLED = false;

/* ---------- ESC pulse calibration ---------- */
static const int ESC_MIN_US     = 1000;
static const int ESC_NEUTRAL_US = 1500;
static const int ESC_MAX_US     = 2000;

/* ---------- Safety ---------- */
static const uint32_t FAILSAFE_MS         = 500; // cut throttle if no frame for this long
static const uint32_t MIN_RX_INTERVAL_MS  = 4;   // reject > ~250 Hz burst frames

/* ---------- Wire formats (must match the transmitter exactly) ---------- */
typedef struct __attribute__((packed)) {
  uint8_t  servo;
  int16_t  motor;
  uint32_t seq;
} DriveFrame;

typedef struct __attribute__((packed)) {
  uint16_t vbat_mv;
  int8_t   rssi;
  uint16_t failsafes;
  uint8_t  flags;      // bit0: brownout hint
} TelemetryFrame;

/* ---------- Actuators & state ---------- */
Servo steering;
Servo esc;

static volatile uint8_t  rxServo    = 90;
static volatile int16_t  rxMotor    = 0;
static volatile uint32_t lastRxMs   = 0;
static volatile uint32_t lastSeq    = 0;
static volatile bool     haveFrame  = false;
static volatile int8_t   lastRssi   = 0;
static volatile uint16_t failsafeCt = 0;
static volatile uint8_t  statusFlags= 0;
static volatile uint32_t rxDropped  = 0;
static volatile uint32_t lastAcceptMs = 0;

static uint8_t  masterMac[6] = {0};
static bool     haveMaster   = false;
static uint32_t lastTelemMs  = 0;

/* Battery millivolts (0 if sensing disabled). */
static uint16_t readBatteryMv() {
  if (!BATTERY_ENABLED) return 0;
  int raw = analogReadMilliVolts(PIN_BATTERY);
  return (uint16_t) constrain((int)(raw * BATTERY_DIVIDER), 0, 65535);
}

/* Send battery + RSSI + failsafes + flags back to the master. */
static void sendTelemetry() {
  if (!haveMaster) return;
  TelemetryFrame t;
  t.vbat_mv   = readBatteryMv();
  t.rssi      = lastRssi;
  t.failsafes = failsafeCt;
  t.flags     = statusFlags;
  esp_now_send(masterMac, (uint8_t *) &t, sizeof(t));
}

/* ESP-NOW receive callback: rate-limited, validates length, learns master MAC. */
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(DriveFrame)) return;
  const uint32_t now = millis();
  if (now - lastAcceptMs < MIN_RX_INTERVAL_MS) { rxDropped++; return; }
  lastAcceptMs = now;

  DriveFrame frame;
  memcpy(&frame, data, sizeof(frame));

  rxServo   = constrain(frame.servo, 0, 180);
  rxMotor   = constrain(frame.motor, -255, 255);
  lastSeq   = frame.seq;
  lastRxMs  = now;
  haveFrame = true;
  if (info->rx_ctrl) lastRssi = info->rx_ctrl->rssi;

  if (!haveMaster) {
    memcpy(masterMac, info->src_addr, 6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMac, 6);
    peer.channel = 0; peer.ifidx = WIFI_IF_STA;
    peer.encrypt = ENABLE_CRYPTO;
    if (ENABLE_CRYPTO) memcpy(peer.lmk, LMK, 16);
    if (esp_now_add_peer(&peer) == ESP_OK) haveMaster = true;
  }
}

static void applyOutputs() {
  steering.write(rxServo);
  int us = map(rxMotor, -255, 255, ESC_MIN_US, ESC_MAX_US);
  esc.writeMicroseconds(us);
}
static void applyFailsafe() {
  rxMotor = 0;
  steering.write(90);
  esc.writeMicroseconds(ESC_NEUTRAL_US);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Report a suspected brownout on the previous power cycle (helps diagnose sag).
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_BROWNOUT) statusFlags |= 0x01;

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  steering.setPeriodHertz(50);
  esc.setPeriodHertz(50);
  steering.attach(PIN_STEER, 500, 2500);
  esc.attach(PIN_ESC, 1000, 2000);
  applyFailsafe();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (ENABLE_LR_MODE) {
    esp_err_t lrErr = esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
      WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (lrErr != ESP_OK) Serial.println("ERR,LR_SET");
  }
  if (esp_now_init() != ESP_OK) { Serial.println("ERR,ESPNOW_INIT"); ESP.restart(); }
  if (ENABLE_CRYPTO) esp_now_set_pmk(PMK);
  esp_now_register_recv_cb(onDataRecv);

  // Read the MAC from eFuse directly. WiFi.macAddress() sometimes returns
  // 00:00:… on ESP32 core 3.x if the WiFi driver hasn't cached the eFuse
  // read by the time we print. esp_read_mac() is deterministic.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("READY,RECEIVER,");
  Serial.println(macStr);
}

void loop() {
  const uint32_t now = millis();

  if (haveFrame && (now - lastRxMs) <= FAILSAFE_MS) {
    applyOutputs();
  } else if (haveFrame) {
    applyFailsafe();
    haveFrame = false;
    failsafeCt++;
    Serial.println("FAILSAFE,LINK_LOST");
  }

  // Heartbeat telemetry back to the master (~5 Hz).
  if (haveMaster && (now - lastTelemMs) >= 200) {
    lastTelemMs = now;
    sendTelemetry();
  }

  delay(5);
}
