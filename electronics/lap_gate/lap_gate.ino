/* ================================================================
   RC SIM RACING ARCADE — IR LAP GATE
   ----------------------------------------------------------------
   Board  : any WiFi ESP32 (classic, S2, S3, C3, C6)
   Sensor : an IR break-beam pair (emitter + receiver) spanning the
            start/finish line. When a car breaks the beam, the receiver
            output changes state and we broadcast a "lap" over ESP-NOW.

   The master transmitter hears the broadcast and relays it to the
   browser as "LAP,<gate>,<seq>"; the app auto-starts the race timer on
   the first crossing and logs each lap after that, updating the
   per-car leaderboard.

   ---- SENSOR WIRING (typical 3-wire IR break-beam receiver) ----
      Receiver VCC -> 5V (or 3.3V per your module)   GND -> GND
      Receiver OUT -> SENSOR_PIN (GPIO below)
   Most modules pull OUT LOW while the beam is BLOCKED and leave it HIGH
   when clear (BEAM_BLOCKED_LEVEL = LOW). If yours is inverted, flip
   BEAM_BLOCKED_LEVEL to HIGH. A photodiode/phototransistor + comparator
   (LM393) module works well and is cheap. Align emitter and receiver
   across the track at axle height.

   ---- MUST MATCH THE FLEET ----
   ENABLE_LR_MODE must equal the master + cars. The gate broadcasts on
   the current channel; keep it on the same channel as the master.
   ================================================================ */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>

/* ---------- Identity ---------- */
static const uint8_t GATE_ID = 1;         // 1..255; unique if you run several gates

/* ---------- Long Range (LR) mode — must match master + cars ---------- */
static const bool ENABLE_LR_MODE = true;

/* ---------- Pins (defaults are broken-out, non-strapping) ---------- */
#if   defined(CONFIG_IDF_TARGET_ESP32C3)
  static const int SENSOR_PIN = 3;
  static const int LED_PIN    = 8;   // C3 super-mini onboard LED (active-low on many)
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  static const int SENSOR_PIN = 4;
  static const int LED_PIN    = 2;
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  static const int SENSOR_PIN = 4;
  static const int LED_PIN    = 15;
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  static const int SENSOR_PIN = 2;
  static const int LED_PIN    = 15;
#else  // classic ESP32
  static const int SENSOR_PIN = 4;
  static const int LED_PIN    = 2;
#endif

/* ---------- Behaviour ---------- */
static const int      BEAM_BLOCKED_LEVEL = LOW;   // level read when the beam is broken
static const uint32_t DEBOUNCE_MS        = 1500;  // ignore re-triggers within this window
static const bool     USE_PULLUP         = true;  // enable internal pull-up on the sensor pin

/* ---------- ESP-NOW ---------- */
static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct __attribute__((packed)) {
  char     tag[4];  // "LAP\0"
  uint8_t  gate;
  uint32_t seq;
} LapFrame;

static uint32_t lastTripMs = 0;
static uint32_t seq        = 0;

static void sendLap() {
  LapFrame f = {};
  f.tag[0]='L'; f.tag[1]='A'; f.tag[2]='P'; f.tag[3]=0;
  f.gate = GATE_ID;
  f.seq  = ++seq;
  esp_now_send(BROADCAST, (uint8_t *) &f, sizeof(f));
  Serial.print("TRIP,"); Serial.print(GATE_ID); Serial.print(','); Serial.println(seq);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(SENSOR_PIN, USE_PULLUP ? INPUT_PULLUP : INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (ENABLE_LR_MODE) {
    esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  }
  if (esp_now_init() != ESP_OK) { Serial.println("ERR,ESPNOW_INIT"); ESP.restart(); }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0; peer.ifidx = WIFI_IF_STA; peer.encrypt = false;
  esp_now_add_peer(&peer);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("READY,LAPGATE,"); Serial.print(GATE_ID); Serial.print(','); Serial.println(macStr);
}

void loop() {
  const bool blocked = (digitalRead(SENSOR_PIN) == BEAM_BLOCKED_LEVEL);
  const uint32_t now = millis();
  if (blocked && (now - lastTripMs) > DEBOUNCE_MS) {
    lastTripMs = now;
    digitalWrite(LED_PIN, HIGH);
    sendLap();
  }
  // Brief LED flash after a trip, then off.
  if (now - lastTripMs > 120) digitalWrite(LED_PIN, LOW);
  delay(2);
}
