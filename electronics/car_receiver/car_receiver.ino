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

/* ================================================================
   PIN MAP — auto-selected per ESP32 variant.
   ----------------------------------------------------------------
   The SAME sketch flashes on any WiFi ESP32 (classic, S2, S3, C3,
   C6). Arduino picks the block below from your Tools -> Board choice.
   These are SANE DEFAULTS on broken-out, non-strapping GPIOs — but
   boards vary, so VERIFY against your board's silkscreen and edit the
   numbers here if a pin isn't exposed. Pins you don't use (e.g. lights
   on a mini board) can be left unwired.
   Full per-board table: docs/PINOUTS.md
   ================================================================ */
#if   defined(CONFIG_IDF_TARGET_ESP32S3)
  #define BOARD_NAME "ESP32-S3"
  #define PIN_STEER 4
  #define PIN_ESC   5
  #define PIN_HEADLIGHT 6
  #define PIN_BRAKELIGHT 7
  #define PIN_SIGNAL_L 15
  #define PIN_SIGNAL_R 16
  #define PIN_REVERSE_LIGHT 17
  #define PIN_HORN 18
  #define PIN_BATTERY 1          // ADC1
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  #define BOARD_NAME "ESP32-S2"
  #define PIN_STEER 4
  #define PIN_ESC   5
  #define PIN_HEADLIGHT 6
  #define PIN_BRAKELIGHT 7
  #define PIN_SIGNAL_L 8
  #define PIN_SIGNAL_R 9
  #define PIN_REVERSE_LIGHT 10
  #define PIN_HORN 11
  #define PIN_BATTERY 1          // ADC1
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // C3 "Super Mini": few pins. Avoid strapping 2/8/9 and USB 18/19.
  #define BOARD_NAME "ESP32-C3"
  #define PIN_STEER 3
  #define PIN_ESC   4
  #define PIN_HEADLIGHT 5
  #define PIN_BRAKELIGHT 6
  #define PIN_SIGNAL_L 7
  #define PIN_SIGNAL_R 10
  #define PIN_REVERSE_LIGHT 20   // UART0 RX — free on native-USB boards
  #define PIN_HORN 21            // UART0 TX — free on native-USB boards
  #define PIN_BATTERY 0          // ADC1
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define BOARD_NAME "ESP32-C6"
  #define PIN_STEER 2
  #define PIN_ESC   3
  #define PIN_HEADLIGHT 4
  #define PIN_BRAKELIGHT 5
  #define PIN_SIGNAL_L 6
  #define PIN_SIGNAL_R 7
  #define PIN_REVERSE_LIGHT 18
  #define PIN_HORN 19
  #define PIN_BATTERY 0          // ADC1
#else  // classic ESP32 (WROOM/WROVER, D1 Mini) — the original mapping
  #define BOARD_NAME "ESP32"
  #define PIN_STEER 18           // D18 — steering servo signal
  #define PIN_ESC   19           // D19 — ESC / drive motor signal
  #define PIN_HEADLIGHT 22       // white LEDs, front
  #define PIN_BRAKELIGHT 23      // red LEDs, rear
  #define PIN_SIGNAL_L 21        // amber, front-left + rear-left
  #define PIN_SIGNAL_R 5         // amber, front-right + rear-right
  #define PIN_REVERSE_LIGHT 4    // white LEDs, rear
  #define PIN_HORN 25            // piezo buzzer / siren MOSFET
  #define PIN_BATTERY 34         // ADC1 — input-only pin
#endif
// Lighting outputs: small LEDs via a ~330 ohm resistor; MOSFET for bright arrays.
// Leave any light pin unwired if that light doesn't exist on your car.

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
// DriveFrame v2: adds a lights bitfield (previously ended at seq).
// Bits: 0=headlight  1=highbeam/flash  2=leftSignal  3=rightSignal
//       4=brake      5=reverseLight    6=horn        7=aux (reserved)
typedef struct __attribute__((packed)) {
  uint8_t  servo;
  int16_t  motor;
  uint8_t  lights;
  uint32_t seq;
} DriveFrame;

/* Light bitflags (shared with master + browser). */
static const uint8_t LIGHT_HEAD     = 0x01;
static const uint8_t LIGHT_HIGHBEAM = 0x02;
static const uint8_t LIGHT_LEFT     = 0x04;
static const uint8_t LIGHT_RIGHT    = 0x08;
static const uint8_t LIGHT_BRAKE    = 0x10;
static const uint8_t LIGHT_REVERSE  = 0x20;
static const uint8_t LIGHT_HORN     = 0x40;
static const uint8_t LIGHT_AUX      = 0x80;

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
static volatile uint8_t  rxLights   = 0;
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
  rxLights  = frame.lights;
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
  rxMotor  = 0;
  rxLights = LIGHT_BRAKE;   // keep brake lights on during failsafe as a visual "stopped" cue
  steering.write(90);
  esc.writeMicroseconds(ESC_NEUTRAL_US);
}

/* Update the LED / horn outputs based on rxLights. Blinker runs off millis()
   so the flashing rate is deterministic and doesn't stutter with network jitter. */
static void updateLights() {
  static uint32_t lastToggle = 0;
  static bool     blinkOn    = false;
  const uint32_t now = millis();
  if (now - lastToggle >= 350) { lastToggle = now; blinkOn = !blinkOn; } // ~1.4 Hz

  const uint8_t L = rxLights;
  const bool leftOn  = (L & LIGHT_LEFT)  && blinkOn;
  const bool rightOn = (L & LIGHT_RIGHT) && blinkOn;

  digitalWrite(PIN_HEADLIGHT,     (L & (LIGHT_HEAD | LIGHT_HIGHBEAM)) ? HIGH : LOW);
  digitalWrite(PIN_BRAKELIGHT,    (L & LIGHT_BRAKE)   ? HIGH : LOW);
  digitalWrite(PIN_SIGNAL_L,      leftOn              ? HIGH : LOW);
  digitalWrite(PIN_SIGNAL_R,      rightOn             ? HIGH : LOW);
  digitalWrite(PIN_REVERSE_LIGHT, (L & LIGHT_REVERSE) ? HIGH : LOW);
  digitalWrite(PIN_HORN,          (L & LIGHT_HORN)    ? HIGH : LOW);
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

  // Light outputs (safe defaults — off)
  pinMode(PIN_HEADLIGHT,     OUTPUT); digitalWrite(PIN_HEADLIGHT,     LOW);
  pinMode(PIN_BRAKELIGHT,    OUTPUT); digitalWrite(PIN_BRAKELIGHT,    LOW);
  pinMode(PIN_SIGNAL_L,      OUTPUT); digitalWrite(PIN_SIGNAL_L,      LOW);
  pinMode(PIN_SIGNAL_R,      OUTPUT); digitalWrite(PIN_SIGNAL_R,      LOW);
  pinMode(PIN_REVERSE_LIGHT, OUTPUT); digitalWrite(PIN_REVERSE_LIGHT, LOW);
  pinMode(PIN_HORN,          OUTPUT); digitalWrite(PIN_HORN,          LOW);

  applyFailsafe();
  updateLights();

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
  Serial.print("BOARD,");
  Serial.println(BOARD_NAME);
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
  // Update lights every loop iteration so blinkers stay smooth.
  updateLights();

  // Heartbeat telemetry back to the master (~5 Hz).
  if (haveMaster && (now - lastTelemMs) >= 200) {
    lastTelemMs = now;
    sendTelemetry();
  }

  delay(5);
}
