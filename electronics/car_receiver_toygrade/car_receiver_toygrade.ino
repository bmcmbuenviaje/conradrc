/* ================================================================
   RC SIM RACING ARCADE — CAR RECEIVER, TOY-GRADE VARIANT
   ----------------------------------------------------------------
   Board  : ESP32 (Arduino core) mounted in a TOY-GRADE RC chassis
   Driver : A dual H-bridge motor driver (TB6612FNG / DRV8833 / L298N)
            replaces the toy's original RX board.

   Why a different sketch? Toy-grade cars don't use a servo + ESC. They
   use two plain DC motors driven by an H-bridge:
      - DRIVE motor  : PWM speed + direction (forward / reverse / brake)
      - STEER motor  : bang-bang (full-left / center / full-right); the
                       front end is spring-centered, so "center" = coast.

   This sketch speaks the SAME ESP-NOW protocol as the hobby-grade
   receiver (DriveFrame in, TelemetryFrame out), so the web app, gears,
   reverse and engine-braking all work unchanged — only the output
   stage differs. Steering degrees (0..180) are quantized to L/C/R.

   Wiring (TB6612FNG example — adjust pins to taste):
      Drive motor  -> AO1/AO2      STEER motor -> BO1/BO2
      AIN1=GPIO25  AIN2=GPIO26  PWMA=GPIO27  (drive)
      BIN1=GPIO32  BIN2=GPIO33  PWMB=GPIO14  (steer)
      STBY=GPIO13 (tie high to enable)   Common GND with ESP32 + battery.

   OPTIONAL: if your toy has (or you add) a real steering servo, use the
   hobby-grade sketch instead — it gives proportional steering.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_mac.h>

/* ---------- Optional payload encryption (must match master) ---------- */
static const bool     ENABLE_CRYPTO = false;
static const uint8_t  PMK[16] = { 'R','C','A','R','C','A','D','E','_','P','M','K','_','_','_','1' };
static const uint8_t  LMK[16] = { 'R','C','A','R','C','A','D','E','_','L','M','K','_','_','_','1' };

/* ---------- Long Range (LR) mode (must match master) ---------- */
static const bool     ENABLE_LR_MODE = true;

/* ================================================================
   PIN MAP — auto-selected per ESP32 variant.
   ----------------------------------------------------------------
   Same sketch flashes on any WiFi ESP32 (classic, S2, S3, C3, C6);
   Arduino picks the block from your Tools -> Board choice. Sane
   defaults on broken-out, non-strapping GPIOs — VERIFY against your
   board's silkscreen and edit if a pin isn't exposed. If your H-bridge
   has no STBY pin (e.g. L298N/DRV8833), just leave STBY unwired.
   Full per-board table: docs/PINOUTS.md
   ================================================================ */
#if   defined(CONFIG_IDF_TARGET_ESP32S3)
  #define BOARD_NAME "ESP32-S3"
  #define AIN1 4
  #define AIN2 5
  #define PWMA 6
  #define BIN1 7
  #define BIN2 15
  #define PWMB 16
  #define STBY 17
  #define PIN_BATTERY 1
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  #define BOARD_NAME "ESP32-S2"
  #define AIN1 4
  #define AIN2 5
  #define PWMA 6
  #define BIN1 7
  #define BIN2 8
  #define PWMB 9
  #define STBY 10
  #define PIN_BATTERY 1
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // C3 "Super Mini": avoid strapping 2/8/9 and USB 18/19.
  #define BOARD_NAME "ESP32-C3"
  #define AIN1 3
  #define AIN2 4
  #define PWMA 5
  #define BIN1 6
  #define BIN2 7
  #define PWMB 10
  #define STBY 20
  #define PIN_BATTERY 0
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define BOARD_NAME "ESP32-C6"
  #define AIN1 2
  #define AIN2 3
  #define PWMA 4
  #define BIN1 5
  #define BIN2 6
  #define PWMB 7
  #define STBY 18
  #define PIN_BATTERY 0
#else  // classic ESP32 (WROOM/WROVER, D1 Mini) — original TB6612 mapping
  #define BOARD_NAME "ESP32"
  #define AIN1 25
  #define AIN2 26
  #define PWMA 27
  #define BIN1 32
  #define BIN2 33
  #define PWMB 14
  #define STBY 13
  #define PIN_BATTERY 34
#endif

/* ---------- Battery sense (optional) ---------- */
static const float BATTERY_DIVIDER = 4.03f;
static const bool  BATTERY_ENABLED = false;

/* ---------- Tuning ---------- */
static const int      PWM_FREQ    = 20000; // 20 kHz — above audible range
static const int      PWM_RES     = 8;     // 8-bit duty (0..255)
static const int      MOTOR_CREEP = 8;     // |motor| <= this -> brake/stop
static const int      STEER_DEAD  = 12;    // degrees around center -> no steer
static const int      STEER_DUTY  = 255;   // bang-bang steering is full-on
static const uint32_t FAILSAFE_MS = 500;
static const uint32_t MIN_RX_INTERVAL_MS = 4; // rate-limit

/* ---------- Wire formats (must match the transmitter) ----------
   DriveFrame v2: adds a lights bitfield. Same as the hobby-grade
   receiver — see car_receiver.ino for the bit layout. */
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180 (quantized to L/C/R here)
  int16_t  motor;   // -255..255 (negative = reverse)
  uint8_t  lights;  // bitfield
  uint32_t seq;
} DriveFrame;

static const uint8_t LIGHT_HEAD     = 0x01;
static const uint8_t LIGHT_HIGHBEAM = 0x02;
static const uint8_t LIGHT_LEFT     = 0x04;
static const uint8_t LIGHT_RIGHT    = 0x08;
static const uint8_t LIGHT_BRAKE    = 0x10;
static const uint8_t LIGHT_REVERSE  = 0x20;
static const uint8_t LIGHT_HORN     = 0x40;

// This variant accepts the lights byte on the wire (so it stays protocol-
// compatible with the master + browser) but does not drive any light pins by
// default. If you wire LEDs on your toy chassis, copy the PIN_HEADLIGHT /
// PIN_BRAKELIGHT / PIN_SIGNAL_L / PIN_SIGNAL_R / PIN_REVERSE_LIGHT /
// PIN_HORN block AND the updateLights() function from car_receiver.ino
// (hobby-grade), pick free GPIOs (avoid the 7 already used by the H-bridge),
// and call updateLights() from loop().

typedef struct __attribute__((packed)) {
  uint16_t vbat_mv;
  int8_t   rssi;
  uint16_t failsafes;
  uint8_t  flags;
} TelemetryFrame;

/* ---------- State ---------- */
static volatile uint8_t  rxServo   = 90;
static volatile int16_t  rxMotor   = 0;
static volatile uint8_t  rxLights  = 0;
static volatile uint32_t lastRxMs  = 0;
static volatile bool     haveFrame = false;
static volatile int8_t   lastRssi  = 0;
static volatile uint16_t failsafeCt = 0;
static volatile uint8_t  statusFlags = 0;
static volatile uint32_t lastAcceptMs = 0;

static uint8_t  masterMac[6] = {0};
static bool     haveMaster   = false;
static uint32_t lastTelemMs  = 0;

/* ---------- Motor helpers ---------- */
static void driveMotor(int m) {
  int duty = abs(m);
  if (duty <= MOTOR_CREEP) {           // brake (both low = coast; both high = brake)
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, LOW); ledcWrite(PWMA, 0); return;
  }
  if (m > 0) { digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW); }
  else       { digitalWrite(AIN1, LOW);  digitalWrite(AIN2, HIGH); }
  ledcWrite(PWMA, constrain(duty, 0, 255));
}

static void steerMotor(int deg) {
  if (deg < 90 - STEER_DEAD) {         // steer left
    digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW); ledcWrite(PWMB, STEER_DUTY);
  } else if (deg > 90 + STEER_DEAD) {  // steer right
    digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH); ledcWrite(PWMB, STEER_DUTY);
  } else {                             // center — release (spring returns)
    digitalWrite(BIN1, LOW); digitalWrite(BIN2, LOW); ledcWrite(PWMB, 0);
  }
}

static void applyOutputs() { steerMotor(rxServo); driveMotor(rxMotor); }
static void applyFailsafe() {
  rxMotor = 0;
  digitalWrite(AIN1, LOW); digitalWrite(AIN2, LOW); ledcWrite(PWMA, 0);
  digitalWrite(BIN1, LOW); digitalWrite(BIN2, LOW); ledcWrite(PWMB, 0);
}

/* ---------- ESP-NOW ---------- */
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(DriveFrame)) return;
  const uint32_t now = millis();
  if (now - lastAcceptMs < MIN_RX_INTERVAL_MS) return; // rate-limit
  lastAcceptMs = now;

  DriveFrame frame;
  memcpy(&frame, data, sizeof(frame));
  rxServo  = constrain(frame.servo, 0, 180);
  rxMotor  = constrain(frame.motor, -255, 255);
  rxLights = frame.lights;
  lastRxMs = now;
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

static uint16_t readBatteryMv() {
  if (!BATTERY_ENABLED) return 0;
  int raw = analogReadMilliVolts(PIN_BATTERY);
  return (uint16_t) constrain((int)(raw * BATTERY_DIVIDER), 0, 65535);
}
static void sendTelemetry() {
  if (!haveMaster) return;
  TelemetryFrame t;
  t.vbat_mv = readBatteryMv(); t.rssi = lastRssi;
  t.failsafes = failsafeCt; t.flags = statusFlags;
  esp_now_send(masterMac, (uint8_t *) &t, sizeof(t));
}

/* ---------- Lifecycle ---------- */
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT); pinMode(BIN2, OUTPUT);
  pinMode(STBY, OUTPUT); digitalWrite(STBY, HIGH);
  ledcAttach(PWMA, PWM_FREQ, PWM_RES);   // Arduino-ESP32 3.x LEDC API
  ledcAttach(PWMB, PWM_FREQ, PWM_RES);
  applyFailsafe();

  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_BROWNOUT) statusFlags |= 0x01;

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

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("BOARD,");
  Serial.println(BOARD_NAME);
  Serial.print("READY,RECEIVER_TOY,");
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
  if (haveMaster && (now - lastTelemMs) >= 200) { lastTelemMs = now; sendTelemetry(); }
  delay(5);
}
