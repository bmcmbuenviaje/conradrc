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

/* ---------- Drive motor (H-bridge A) ---------- */
static const int AIN1 = 25;
static const int AIN2 = 26;
static const int PWMA = 27;

/* ---------- Steering motor (H-bridge B, bang-bang) ---------- */
static const int BIN1 = 32;
static const int BIN2 = 33;
static const int PWMB = 14;

/* ---------- Standby (TB6612 STBY; tie high) ---------- */
static const int STBY = 13;

/* ---------- Battery sense (optional) ---------- */
static const int   PIN_BATTERY     = 34;
static const float BATTERY_DIVIDER = 4.03f;
static const bool  BATTERY_ENABLED = false;

/* ---------- Tuning ---------- */
static const int      PWM_FREQ    = 20000; // 20 kHz — above audible range
static const int      PWM_RES     = 8;     // 8-bit duty (0..255)
static const int      MOTOR_CREEP = 8;     // |motor| <= this -> brake/stop
static const int      STEER_DEAD  = 12;    // degrees around center -> no steer
static const int      STEER_DUTY  = 255;   // bang-bang steering is full-on
static const uint32_t FAILSAFE_MS = 500;

/* ---------- Wire formats (must match the transmitter) ---------- */
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180 (quantized to L/C/R here)
  int16_t  motor;   // -255..255 (negative = reverse)
  uint32_t seq;
} DriveFrame;

typedef struct __attribute__((packed)) {
  uint16_t vbat_mv;
  int8_t   rssi;
} TelemetryFrame;

/* ---------- State ---------- */
static volatile uint8_t  rxServo   = 90;
static volatile int16_t  rxMotor   = 0;
static volatile uint32_t lastRxMs  = 0;
static volatile bool     haveFrame = false;
static volatile int8_t   lastRssi  = 0;

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
  DriveFrame frame;
  memcpy(&frame, data, sizeof(frame));
  rxServo  = constrain(frame.servo, 0, 180);
  rxMotor  = constrain(frame.motor, -255, 255);
  lastRxMs = millis();
  haveFrame = true;
  if (info->rx_ctrl) lastRssi = info->rx_ctrl->rssi;

  if (!haveMaster) {
    memcpy(masterMac, info->src_addr, 6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMac, 6);
    peer.channel = 0; peer.encrypt = false; peer.ifidx = WIFI_IF_STA;
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
  TelemetryFrame t; t.vbat_mv = readBatteryMv(); t.rssi = lastRssi;
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

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) { Serial.println("ERR,ESPNOW_INIT"); ESP.restart(); }
  esp_now_register_recv_cb(onDataRecv);

  Serial.print("READY,RECEIVER_TOY,");
  Serial.println(WiFi.macAddress());
}

void loop() {
  const uint32_t now = millis();
  if (haveFrame && (now - lastRxMs) <= FAILSAFE_MS) {
    applyOutputs();
  } else if (haveFrame) {
    applyFailsafe();
    haveFrame = false;
    Serial.println("FAILSAFE,LINK_LOST");
  }
  if (haveMaster && (now - lastTelemMs) >= 200) { lastTelemMs = now; sendTelemetry(); }
  delay(5);
}
