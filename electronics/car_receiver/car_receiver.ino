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

   TELEMETRY: the car learns the master's MAC from the first frame it
   receives, then periodically sends back battery voltage + RSSI, which
   the master relays to the browser HUD.

   NOTE: Flash each chassis, then read its printed MAC on boot and
         paste that MAC into the web lobby's roster so the master can
         target it with a CAR,.. swap.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <ESP32Servo.h>

/* ---------- Pin map ---------- */
static const int PIN_STEER   = 18;  // D18 — steering servo signal
static const int PIN_ESC     = 19;  // D19 — ESC / drive motor signal
static const int PIN_BATTERY = 34;  // ADC1 — battery sense via divider (optional)

/* ---------- Battery sense (optional) ----------
   Wire the pack through a divider into PIN_BATTERY so the pin never exceeds
   ~3.3 V. Set the ratio to (Vbattery / Vpin). Example: 2S (8.4V max) through
   a 100k/33k divider -> ratio ~4.03. Leave as-is if you don't wire a divider
   (it will just report a meaningless/zero voltage). */
static const float BATTERY_DIVIDER = 4.03f;
static const bool  BATTERY_ENABLED = false; // set true once a divider is wired

/* ---------- ESC pulse calibration (microseconds) ---------- */
static const int ESC_MIN_US     = 1000; // full reverse
static const int ESC_NEUTRAL_US = 1500; // motor stopped
static const int ESC_MAX_US     = 2000; // full forward
/* NOTE: reverse requires a reverse-capable ESC (many need a brake/reverse
   or "double-tap" mode enabled in the ESC's own programming). A forward-only
   ESC will simply treat sub-neutral pulses as neutral/brake. */

/* ---------- Failsafe ---------- */
static const uint32_t FAILSAFE_MS = 500; // cut throttle if no frame for this long

/* ---------- Wire formats (must match the transmitter exactly) ---------- */
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180
  int16_t  motor;   // -255..255  (negative = reverse, 0 = stop)
  uint32_t seq;     // rolling sequence
} DriveFrame;

typedef struct __attribute__((packed)) {
  uint16_t vbat_mv; // battery millivolts (0 if unmeasured)
  int8_t   rssi;    // dBm the car heard from the master
} TelemetryFrame;

/* ---------- Actuators & state ---------- */
Servo steering;
Servo esc;

static volatile uint8_t  rxServo   = 90;   // default: wheels centered
static volatile int16_t  rxMotor   = 0;    // default: stopped (signed, -255..255)
static volatile uint32_t lastRxMs  = 0;
static volatile uint32_t lastSeq   = 0;
static volatile bool     haveFrame = false;
static volatile int8_t   lastRssi  = 0;

static uint8_t  masterMac[6] = {0};
static bool     haveMaster   = false;
static uint32_t lastTelemMs  = 0;

/* ---------- ESP-NOW receive callback (Arduino-ESP32 3.x signature) ---------- */
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(DriveFrame)) return; // ignore malformed / foreign packets
  DriveFrame frame;
  memcpy(&frame, data, sizeof(frame));

  rxServo   = constrain(frame.servo, 0, 180);
  rxMotor   = constrain(frame.motor, -255, 255); // signed: negative = reverse
  lastSeq   = frame.seq;
  lastRxMs  = millis();
  haveFrame = true;
  if (info->rx_ctrl) lastRssi = info->rx_ctrl->rssi;

  // Learn the master's MAC on first contact so we can send telemetry back.
  if (!haveMaster) {
    memcpy(masterMac, info->src_addr, 6);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMac, 6);
    peer.channel = 0; peer.encrypt = false; peer.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peer) == ESP_OK) haveMaster = true;
  }
}

/* Read battery voltage in millivolts (0 if sensing disabled). */
static uint16_t readBatteryMv() {
  if (!BATTERY_ENABLED) return 0;
  int raw = analogReadMilliVolts(PIN_BATTERY); // pin millivolts (ADC calibrated)
  return (uint16_t) constrain((int)(raw * BATTERY_DIVIDER), 0, 65535);
}

/* Send battery + RSSI back to the master (which relays to the browser). */
static void sendTelemetry() {
  if (!haveMaster) return;
  TelemetryFrame t;
  t.vbat_mv = readBatteryMv();
  t.rssi    = lastRssi;
  esp_now_send(masterMac, (uint8_t *) &t, sizeof(t));
}

/* Apply the latest command to the physical outputs. */
static void applyOutputs() {
  // Steering: direct degree write.
  steering.write(rxServo);

  // ESC: map signed -255..255 -> min(reverse)..neutral..max(forward) pulse.
  int us = map(rxMotor, -255, 255, ESC_MIN_US, ESC_MAX_US);
  esc.writeMicroseconds(us);
}

static void applyFailsafe() {
  rxMotor = 0;
  steering.write(90);
  esc.writeMicroseconds(ESC_NEUTRAL_US);
}

/* ---------- Arduino lifecycle ---------- */
void setup() {
  Serial.begin(115200);
  delay(200);

  // Servo timer allocation for the ESP32 core.
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  steering.setPeriodHertz(50);
  esc.setPeriodHertz(50);
  steering.attach(PIN_STEER, 500, 2500); // standard servo pulse window
  esc.attach(PIN_ESC, 1000, 2000);       // typical ESC pulse window

  // Arm to safe defaults.
  applyFailsafe();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERR,ESPNOW_INIT");
    ESP.restart();
  }
  esp_now_register_recv_cb(onDataRecv);

  // Print this chassis' MAC — copy it into the web lobby's VEHICLES table.
  Serial.print("READY,RECEIVER,");
  Serial.println(WiFi.macAddress());
}

void loop() {
  const uint32_t now = millis();

  if (haveFrame && (now - lastRxMs) <= FAILSAFE_MS) {
    applyOutputs();
  } else if (haveFrame) {
    // Link went quiet — coast to safe state until frames resume.
    applyFailsafe();
    haveFrame = false;
    Serial.println("FAILSAFE,LINK_LOST");
  }

  // Heartbeat telemetry back to the master (~5 Hz).
  if (haveMaster && (now - lastTelemMs) >= 200) {
    lastTelemMs = now;
    sendTelemetry();
  }

  delay(5); // ~200 Hz servicing; ESP-NOW callback captures every frame regardless
}
