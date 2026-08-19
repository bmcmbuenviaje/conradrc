/* ================================================================
   RC SIM RACING ARCADE — ONBOARD FPV CAMERA (ESP32-CAM)
   ----------------------------------------------------------------
   Board  : AI-Thinker ESP32-CAM (OV2640)  — select "AI Thinker
            ESP32-CAM" in Arduino IDE (Tools → Board → ESP32 Arduino).
   Role   : Ride on the car, stream Motion-JPEG over WiFi so the
            arcade web app can show a first-person view.

   The web app's "Onboard camera (MJPEG URL)" source points an <img>
   at this stream. We send an "Access-Control-Allow-Origin: *" header
   so the app can ALSO snapshot / record / forward the feed to a
   remote phone viewer (canvas capture needs CORS to stay untainted).

   URLs this exposes:
     http://<cam-ip>/         tiny status page (port 80)
     http://<cam-ip>:81/stream   the MJPEG stream  ← paste THIS in the app

   ---- TWO NETWORK MODES (pick one below) -------------------------
   USE_AP = true  (default, recommended for an isolated station):
       The camera IS its own WiFi access point. Join it from the PC:
         SSID: "RC-FPV-01"   PASS: "rcarcade"
       Then the stream is ALWAYS at:  http://192.168.4.1:81/stream
       No router needed. Each car = its own AP on its own channel →
       no cross-car congestion.

       >>> STATION AUTO-SUGGEST <<<  Set ONE number — STATION_ID — and the
       sketch derives a UNIQUE SSID and a NON-OVERLAPPING channel for you,
       cycling the only three that don't overlap on 2.4 GHz (1 / 6 / 11):
         STATION_ID 1 -> "RC-FPV-01"  ch 1
         STATION_ID 2 -> "RC-FPV-02"  ch 6
         STATION_ID 3 -> "RC-FPV-03"  ch 11
         STATION_ID 4 -> "RC-FPV-04"  ch 1   (wraps)
       So at an event you just number the cars 1,2,3,… and channels sort
       themselves out. The chosen SSID + channel print to Serial at boot.

   USE_AP = false (join an existing WiFi):
       Fill in STA_SSID / STA_PASS. The cam prints its DHCP IP to the
       Serial Monitor at boot — use that IP in the stream URL.

   ---- LATENCY / BANDWIDTH KNOBS ----------------------------------
   FRAMESIZE_QVGA (320x240) + quality ~12 keeps latency ~120-200 ms
   and bandwidth low (fits more cameras per band). Bump to
   FRAMESIZE_VGA for a sharper picture at the cost of latency.

   ---- POWER ------------------------------------------------------
   Feed the ESP32-CAM a solid 5V (>=500 mA). Brownouts show as
   pink/green corrupt frames or constant reboots. The onboard 3.3V
   regulator is weak — power the 5V pin, not 3.3V.
   ================================================================ */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "soc/soc.h"          // brownout register
#include "soc/rtc_cntl_reg.h"

/* ---------------- NETWORK MODE ---------------- */
static const bool USE_AP = true;                 // true = own AP, false = join WiFi

// AP-mode settings (USE_AP = true) — set STATION_ID only; SSID + channel auto-derive.
static const int   STATION_ID   = 1;             // 1,2,3,… unique per station/car
static const char *AP_SSID_BASE = "RC-FPV";      // SSID becomes "<BASE>-NN"
static const char *AP_PASS      = "rcarcade";    // >= 8 chars, or "" for open
static const int   CH_MAP[3]    = { 1, 6, 11 };  // the only non-overlapping 2.4 GHz channels

// Derived at boot from STATION_ID (filled in setup()).
static char apSsid[24] = {0};
static int  apChannel  = 1;

// STA-mode settings (USE_AP = false)
static const char *STA_SSID = "YOUR_WIFI";
static const char *STA_PASS = "YOUR_PASSWORD";

/* ---------------- AI-Thinker ESP32-CAM pinout ---------------- */
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

/* ---------------- MJPEG multipart boundary ---------------- */
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t stream_httpd = NULL;
static httpd_handle_t index_httpd  = NULL;

/* ---------------- Camera init ---------------- */
static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;   // newest frame = lowest latency
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  // With PSRAM we can double-buffer at a bigger size; without it, stay small.
  if (psramFound()) {
    config.frame_size  = FRAMESIZE_QVGA;      // 320x240 — bump to VGA for sharper
    config.jpeg_quality = 12;                 // 10 (better) .. 63 (worse/smaller)
    config.fb_count     = 2;
  } else {
    config.frame_size  = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("CAM_INIT_FAIL 0x%x\n", err); return false; }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, config.frame_size);
    s->set_vflip(s, 0);        // set to 1 if the cam is mounted upside-down
    s->set_hmirror(s, 0);      // set to 1 to mirror left/right
  }
  return true;
}

/* ---------------- /stream handler (port 81) ---------------- */
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  // CORS — lets the web app sample/record/forward the feed, not just show it.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *) fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
    fb = NULL;
    if (res != ESP_OK) break;   // client disconnected
  }
  return res;
}

/* ---------------- /ping handler (port 81) ----------------
   Tiny, fast reply so the web app can measure round-trip latency to the
   camera (the controllable, network part of glass-to-glass lag). CORS so
   the app can read the timing. */
static esp_err_t ping_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "1", 1);
}

/* ---------------- / index handler (port 80) ---------------- */
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char *page =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='background:#0a0f1c;color:#cfe;font-family:monospace;text-align:center'>"
    "<h3>RC-FPV camera online</h3>"
    "<p>Stream URL for the arcade app:</p>"
    "<p><b>http://" "%HOST%" ":81/stream</b></p>"
    "<img style='max-width:100%;border:1px solid #235' src=':81/stream'>"
    "</body>";
  // We don't template %HOST% server-side (keep it tiny); the text is a hint.
  return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 4;

  // Index on port 80
  cfg.server_port = 80;
  cfg.ctrl_port   = 32080;
  if (httpd_start(&index_httpd, &cfg) == ESP_OK) {
    httpd_uri_t idx = { "/", HTTP_GET, index_handler, NULL };
    httpd_register_uri_handler(index_httpd, &idx);
  }

  // Stream + ping on port 81
  cfg.server_port = 81;
  cfg.ctrl_port   = 32081;
  if (httpd_start(&stream_httpd, &cfg) == ESP_OK) {
    httpd_uri_t st = { "/stream", HTTP_GET, stream_handler, NULL };
    httpd_register_uri_handler(stream_httpd, &st);
    httpd_uri_t pg = { "/ping", HTTP_GET, ping_handler, NULL };
    httpd_register_uri_handler(stream_httpd, &pg);
  }
}

/* ---------------- Lifecycle ---------------- */
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // ignore transient brownout resets
  Serial.begin(115200);
  delay(200);
  Serial.println();

  if (!initCamera()) { Serial.println("Camera init failed — check ribbon + 5V power."); delay(3000); ESP.restart(); }

  if (USE_AP) {
    // Auto-derive a unique SSID + a non-overlapping channel from STATION_ID.
    int id = STATION_ID < 1 ? 1 : STATION_ID;
    snprintf(apSsid, sizeof(apSsid), "%s-%02d", AP_SSID_BASE, id);
    apChannel = CH_MAP[(id - 1) % 3];
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid, (strlen(AP_PASS) >= 8 ? AP_PASS : NULL), apChannel);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP up: SSID=\"%s\"  ch=%d  (station %d)\n", apSsid, apChannel, id);
    Serial.print("Stream URL: http://"); Serial.print(ip); Serial.println(":81/stream");
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASS);
    Serial.print("Joining WiFi");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(400); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Stream URL: http://"); Serial.print(WiFi.localIP()); Serial.println(":81/stream");
    } else {
      Serial.println("WiFi join failed — check STA_SSID / STA_PASS. Rebooting.");
      delay(2000); ESP.restart();
    }
  }

  startServers();
  Serial.println("READY,FPV_CAM");
}

void loop() {
  delay(1000);   // all work happens in the HTTP server tasks
}
