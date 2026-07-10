/* ================================================================
   RC SIM RACING ARCADE — CAR RECEIVER (Vehicle Side)
   ----------------------------------------------------------------
   Board : ESP32 (Arduino core) mounted in the RC chassis
   Role  : Receive ESP-NOW DriveFrames from the master transmitter
           and drive hardware PWM outputs:
              - Steering servo   -> GPIO 18 (D18)
              - ESC / drive motor-> GPIO 19 (D19)

   Steering : standard proportional servo, 0..180 deg (90 = center).
   ESC      : servo-style pulse; neutral ~1500us, forward-only mapping
              from the 0..255 motor byte in this baseline blueprint.

   NOTE: Flash each chassis, then read its printed MAC on boot and
         paste that MAC into the web lobby's VEHICLES table so the
         master can target it with a CAR,.. swap.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <ESP32Servo.h>

/* ---------- Pin map ---------- */
static const int PIN_STEER = 18;  // D18 — steering servo signal
static const int PIN_ESC   = 19;  // D19 — ESC / drive motor signal

/* ---------- ESC pulse calibration (microseconds) ---------- */
static const int ESC_MIN_US     = 1000; // full reverse
static const int ESC_NEUTRAL_US = 1500; // motor stopped
static const int ESC_MAX_US     = 2000; // full forward
/* NOTE: reverse requires a reverse-capable ESC (many need a brake/reverse
   or "double-tap" mode enabled in the ESC's own programming). A forward-only
   ESC will simply treat sub-neutral pulses as neutral/brake. */

/* ---------- Failsafe ---------- */
static const uint32_t FAILSAFE_MS = 500; // cut throttle if no frame for this long

/* ---------- Wire format (must match the transmitter exactly) ---------- */
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180
  int16_t  motor;   // -255..255  (negative = reverse, 0 = stop)
  uint32_t seq;     // rolling sequence
} DriveFrame;

/* ---------- Actuators & state ---------- */
Servo steering;
Servo esc;

static volatile uint8_t  rxServo   = 90;   // default: wheels centered
static volatile int16_t  rxMotor   = 0;    // default: stopped (signed, -255..255)
static volatile uint32_t lastRxMs  = 0;
static volatile uint32_t lastSeq   = 0;
static volatile bool     haveFrame = false;

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

  delay(5); // ~200 Hz servicing; ESP-NOW callback captures every frame regardless
}
