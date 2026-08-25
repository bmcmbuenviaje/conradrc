# 📌 ESP32 pin maps — any board, any RC

The receiver sketches flash on **any WiFi-capable ESP32** — classic ESP32,
S2, S3, C3, C6 — from the **same file**. Each sketch has a
`#if defined(CONFIG_IDF_TARGET_…)` block that auto-selects the pins below from
your **Tools → Board** choice in the Arduino IDE. Flash the same sketch; it
configures itself. The boot banner prints `BOARD,<name>` so you can confirm the
right variant was detected.

> These are **sane defaults on broken-out, non-strapping GPIOs**. Boards vary —
> **verify against your board's silkscreen** and edit the numbers in the sketch's
> pin block if a pin isn't exposed on your board.

## Which sketch?

| RC type | Motors | Sketch |
|---|---|---|
| **Hobby-grade** | steering **servo** + **ESC** (brushed/brushless) | [`car_receiver`](../electronics/car_receiver/car_receiver.ino) |
| **Toy-grade** | 2 plain **DC motors** via an **H-bridge** (TB6612 / DRV8833 / L298N) | [`car_receiver_toygrade`](../electronics/car_receiver_toygrade/car_receiver_toygrade.ino) |
| **Master transmitter** (desk) | none — USB + ESP-NOW only | [`master_transmitter`](../electronics/master_transmitter/master_transmitter.ino) — **any** ESP32, no pins to map |
| **Onboard FPV camera** | camera + PSRAM only | [`esp32cam_fpv`](../electronics/esp32cam_fpv/esp32cam_fpv.ino) — **camera boards only** (see note) |

---

## Hobby-grade (`car_receiver`) — pin map

| Signal | Classic ESP32 | ESP32-S3 | ESP32-S2 | ESP32-C3 | ESP32-C6 |
|---|---|---|---|---|---|
| Steering servo | 18 | 4 | 4 | 3 | 2 |
| ESC / drive | 19 | 5 | 5 | 4 | 3 |
| Headlight | 22 | 6 | 6 | 5 | 4 |
| Brake light | 23 | 7 | 7 | 6 | 5 |
| Signal L | 21 | 15 | 8 | 7 | 6 |
| Signal R | 5 | 16 | 9 | 10 | 7 |
| Reverse light | 4 | 17 | 10 | 20 | 18 |
| Horn | 25 | 18 | 11 | 21 | 19 |
| Battery sense (ADC1) | 34 | 1 | 1 | 0 | 0 |

**On a mini board you rarely wire all of these** — steering + ESC are the only
must-haves. Leave any light pin unwired.

## Toy-grade (`car_receiver_toygrade`) — pin map

| Signal | Classic ESP32 | ESP32-S3 | ESP32-S2 | ESP32-C3 | ESP32-C6 |
|---|---|---|---|---|---|
| AIN1 (drive dir) | 25 | 4 | 4 | 3 | 2 |
| AIN2 (drive dir) | 26 | 5 | 5 | 4 | 3 |
| PWMA (drive speed) | 27 | 6 | 6 | 5 | 4 |
| BIN1 (steer dir) | 32 | 7 | 7 | 6 | 5 |
| BIN2 (steer dir) | 33 | 15 | 8 | 7 | 6 |
| PWMB (steer speed) | 14 | 16 | 9 | 10 | 7 |
| STBY (enable) | 13 | 17 | 10 | 20 | 18 |
| Battery sense (ADC1) | 34 | 1 | 1 | 0 | 0 |

If your H-bridge has **no STBY pin** (L298N, most DRV8833 breakouts), just leave
STBY unwired — it's harmless.

---

## Per-board notes (read before wiring)

**Classic ESP32 (WROOM/WROVER, D1 Mini)** — the reference mapping. GPIO 34 is
input-only (fine for battery sense). Avoid 6–11 (SPI flash) and strapping pins
0/2/12/15.

**ESP32-S3** — lots of GPIOs. Avoid 0/45/46 (strapping), 19/20 (USB), 26–32 (SPI
flash), and 33–37 on WROOM-2 / Octal-PSRAM parts. ADC1 = GPIO 1–10.

**ESP32-S2** — single core, WiFi only (no BLE — irrelevant here). Avoid 0/45/46
(strapping), 19/20 (USB), 26–32 (flash). ADC1 = GPIO 1–10.

**ESP32-C3 ("Super Mini")** — the popular tiny board. **Only ~11 usable GPIOs.**
Avoid **2, 8, 9** (strapping — 8/9 affect boot) and **18/19** (USB). GPIO 20/21
are UART0 but free on native-USB boards (the Super Mini uses USB CDC for serial).
ADC1 = GPIO 0–4. Only ~6 LEDC channels — plenty for servo + ESC (+ a few lights).

**ESP32-C6** — WiFi 6; needs a recent Arduino-ESP32 core (3.x). Avoid 8/9/15
(strapping) and 12/13 (USB). ADC1 = GPIO 0–6.

---

## The two fleet-wide rules (all boards)

Mixed board **types** work together fine — a C3 mini car and a classic ESP32 car
drive from the same master — **as long as these match across every device:**

1. **`ENABLE_LR_MODE`** — must be identical on the master and every car. Your
   master ships with `true`, so every receiver must be `true` too, or they can't
   hear each other.
2. **ESP-NOW channel + crypto** — receivers learn the master and follow its
   channel automatically; just keep `ENABLE_CRYPTO` (and the PMK/LMK keys) the
   same everywhere.

Register each car by its **MAC** in the lobby (printed as `READY,RECEIVER,<mac>`
at boot) and pick it from the roster — the mini RC is just another car.

---

## ⚠️ FPV camera is hardware-specific

The `esp32cam_fpv` sketch needs a board with a **camera interface + PSRAM**
(AI-Thinker ESP32-CAM pinout). A plain mini ESP32-C3/S2 has **no camera
peripheral** and can't do onboard FPV. For a *tiny* FPV cam, use a
**XIAO ESP32-S3 Sense** or **Freenove ESP32-S3-CAM** — ask and the streaming
sketch can be re-pinned for those.
