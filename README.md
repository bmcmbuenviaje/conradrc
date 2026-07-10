# 🏁 RC SIM Racing Arcade Lobby

A multi-cabinet, cross-platform **RC Sim Racing Arcade**. Any user sitting at a local sim rig (Chromebook or Windows PC) opens a single static web app, picks a car from a visual lobby grid, and drives it in real time with an FPV video feed and **&lt;20 ms** target control latency.

The whole control chain is browser → USB → radio → car:

```
┌─────────────────────────┐        USB Serial          ┌──────────────────────┐      ESP-NOW 2.4GHz      ┌──────────────────────┐
│   BROWSER (GitHub Pages) │  ───── 115200 baud ─────▶  │  MASTER TRANSMITTER  │  ──── DriveFrame ────▶  │    CAR RECEIVER(S)   │
│  Gamepad API  @ 50 Hz    │   CAR,.. / DRIVE,..\n      │   ENGLAB ESP32 DevKit│    servo/motor/seq      │  ESP32 in RC chassis │
│  Web Serial API          │  ◀──── ACK / telemetry ──  │   dynamic peer swap  │                          │  Servo D18 · ESC D19 │
│  MediaDevices (FPV)      │                            └──────────────────────┘                          └──────────────────────┘
└─────────────────────────┘
```

- **Frontend** — [`index.html`](index.html): a single, dependency-free HTML/CSS/JS file. No Node, npm, or build step.
- **Master transmitter** — [`electronics/master_transmitter/master_transmitter.ino`](electronics/master_transmitter/master_transmitter.ino): decodes serial packets and forwards them over ESP-NOW, swapping the target peer on the fly.
- **Car receiver** — [`electronics/car_receiver/car_receiver.ino`](electronics/car_receiver/car_receiver.ino): receives ESP-NOW frames and drives a steering servo (GPIO 18) and ESC/motor (GPIO 19).

---

## Table of Contents

1. [Hardware you need](#-hardware-you-need)
2. [Software you need](#-software-you-need)
3. [Quick start (TL;DR)](#-quick-start-tldr)
4. [Part A — Deploy the web app to GitHub Pages](#part-a--deploy-the-web-app-to-github-pages)
5. [Part B — Set up the Arduino toolchain](#part-b--set-up-the-arduino-toolchain)
6. [Part C — Flash the Master Transmitter](#part-c--flash-the-master-transmitter)
7. [Part D — Flash the Car Receivers](#part-d--flash-the-car-receivers)
8. [Part E — Register each car's MAC in the lobby](#part-e--register-each-cars-mac-in-the-lobby)
9. [Part F — Wire the car hardware](#part-f--wire-the-car-hardware)
10. [Using the arcade](#-using-the-arcade)
11. [Serial protocol reference](#-serial-protocol-reference)
12. [Calibration & tuning](#-calibration--tuning)
13. [Troubleshooting](#-troubleshooting)
14. [Repository layout](#-repository-layout)

---

## 🔧 Hardware you need

### Desk side (one per cabinet)
| Item | Notes |
|------|-------|
| **ENGLAB ESP32 DevKit** (or any ESP32 DevKitC) | The master transmitter. Connects to the PC over USB. |
| **USB data cable** | Must be a **data** cable, not charge-only. |
| **Racing wheel / gamepad** | Anything the browser Gamepad API sees (Logitech G-series, Xbox pad, etc.). |
| **5.8 GHz FPV receiver + USB frame grabber** | A UVC video capture dongle the browser sees as a webcam (640×480 @ 60 fps). |
| **Chromebook or Windows PC** | Running Chrome or Edge. |

### Vehicle side (one per car)
| Item | Notes |
|------|-------|
| **ESP32 chip/board** | One per RC car. |
| **Proportional steering servo** | Signal → **GPIO 18 (D18)**. |
| **ESC + brushed/brushless motor** *(or a motor driver)* | Signal → **GPIO 19 (D19)**. |
| **RC car chassis** | Hobby-grade or a modified toy-grade car. |
| **Battery / BEC** | Powers the ESP32, servo, and ESC. **Do not** power the servo/ESC from the ESP32's 3.3 V rail. |
| **5.8 GHz FPV camera + video transmitter** | Streams video back to the desk-side frame grabber. |

> ⚠️ **Common ground is required.** The ESP32, servo, and ESC must share a ground reference, or PWM signals will be erratic.

---

## 💻 Software you need

- **Google Chrome or Microsoft Edge** (v89+). The Web Serial API is **not** available in Firefox or Safari. On a Chromebook this works out of the box.
- **Arduino IDE 2.x** (or Arduino CLI) to flash the ESP32s.
- **USB-to-UART driver** for your ESP32 board if it isn't detected: [CP210x](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) or [CH340](https://www.wch-ic.com/downloads/CH341SER_ZIP.html).

---

## 🚀 Quick start (TL;DR)

1. Enable **GitHub Pages** on this repo (Settings → Pages → Deploy from `main` / root). Your app lives at `https://bmcmbuenviaje.github.io/conradrc/`.
2. Install the **ESP32 board package** and the **ESP32Servo** library in Arduino IDE.
3. Flash [`car_receiver.ino`](electronics/car_receiver/car_receiver.ino) to each car. Open the Serial Monitor at 115200 and copy the `READY,RECEIVER,<MAC>` line.
4. Paste those MACs into the `VEHICLES` array in [`index.html`](index.html), commit, and push.
5. Flash [`master_transmitter.ino`](electronics/master_transmitter/master_transmitter.ino) to the desk-side ESP32.
6. Open the Pages URL in Chrome → **Connect** the transmitter → bind your wheel → start the FPV feed → pick a car → drive.

---

## Part A — Deploy the web app to GitHub Pages

The frontend is fully static, so GitHub Pages serves it directly — no build step.

1. Go to the repo on GitHub → **Settings** → **Pages**.
2. Under **Build and deployment**, set **Source** = **Deploy from a branch**.
3. Choose **Branch: `main`**, **Folder: `/ (root)`**, then **Save**.
4. Wait ~1 minute. GitHub shows the live URL:
   ```
   https://bmcmbuenviaje.github.io/conradrc/
   ```
5. Open that URL in Chrome/Edge.

> 🔒 **Why HTTPS matters:** Web Serial, MediaDevices, and Gamepad APIs require a *secure context*. GitHub Pages is served over HTTPS, so all three work. `http://` or opening the file over a network share will silently disable Serial/camera access. (Local `file://` and `http://localhost` are also treated as secure if you want to test locally.)

### Testing locally (optional)
```bash
# From the repo root — any static server works because there is no build step:
python -m http.server 8080
# then open http://localhost:8080
```

---

## Part B — Set up the Arduino toolchain

Do this once on the machine you'll flash from.

1. Install **Arduino IDE 2.x** from <https://www.arduino.cc/en/software>.
2. **Add the ESP32 board manager URL:**
   - `File → Preferences → Additional boards manager URLs`, add:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
3. **Install the ESP32 core:**
   - `Tools → Board → Boards Manager` → search **esp32** → install **"esp32 by Espressif Systems"**.
   - ⚠️ **Install version 3.x** (e.g. 3.0.0+). This firmware uses the 3.x ESP-NOW callback signatures (`esp_now_recv_info_t`, `wifi_tx_info_t`). If you're pinned to core 2.x, see [Troubleshooting](#-troubleshooting).
4. **Install the servo library** (receiver only):
   - `Tools → Manage Libraries` → search **ESP32Servo** → install **"ESP32Servo" by Kevin Harrington / John K. Bennett**.

Board settings that work for most DevKitC / ENGLAB boards:
- **Board:** `ESP32 Dev Module`
- **Upload Speed:** `921600` (drop to `115200` if uploads fail)
- **Port:** the COM/tty port that appears when you plug the board in

---

## Part C — Flash the Master Transmitter

This is the ESP32 that stays plugged into the PC.

1. Open [`electronics/master_transmitter/master_transmitter.ino`](electronics/master_transmitter/master_transmitter.ino) in Arduino IDE.
2. Select the correct **Board** and **Port**.
3. Click **Upload** (→).
4. Open **Serial Monitor** at **115200 baud**. You should see:
   ```
   READY,MASTER,3C:61:05:XX:XX:XX
   ```
5. **Close the Serial Monitor.** Only one program can hold the port at a time — leave it closed so the browser can connect.

You do **not** need to hard-code the master's MAC anywhere; the browser talks to it over USB and it forwards to whichever car you select.

---

## Part D — Flash the Car Receivers

Repeat for **every** car.

1. Open [`electronics/car_receiver/car_receiver.ino`](electronics/car_receiver/car_receiver.ino).
2. Select **Board** and **Port** for the car's ESP32.
3. Click **Upload**.
4. Open **Serial Monitor** at **115200**. Record the printed MAC — you'll need it in the next step:
   ```
   READY,RECEIVER,AA:BB:CC:11:22:33
   ```
5. Label the car with its MAC (a piece of tape helps) and move to the next chassis.

The same `car_receiver.ino` is flashed to every car unchanged — cars are distinguished purely by their MAC address.

---

## Part E — Register each car's MAC in the lobby

The web app targets cars by MAC. Edit the `VEHICLES` array near the top of the `<script>` block in [`index.html`](index.html):

```js
const VEHICLES = [
  { id: 'rally',  name: 'RALLY <span>CAR</span>',  sprite: '🏎️', mac: 'AA:BB:CC:11:22:33', /* ← paste real MAC */
    accent: '#00e5ff', glow: 'rgba(0,229,255,0.35)',  stats: { spd: 78, grip: 90, acc: 70 } },
  { id: 'buggy',  name: 'DUNE <span>BUGGY</span>', sprite: '🚙', mac: 'AA:BB:CC:44:55:66', /* ← paste real MAC */
    accent: '#b6ff00', glow: 'rgba(182,255,0,0.30)',  stats: { spd: 65, grip: 72, acc: 95 } },
  { id: 'drift',  name: 'DRIFT <span>KING</span>', sprite: '🚗', mac: 'AA:BB:CC:77:88:99', /* ← paste real MAC */
    accent: '#ff2e97', glow: 'rgba(255,46,151,0.32)', stats: { spd: 88, grip: 55, acc: 82 } },
];
```

- Replace each `mac:` with the real `READY,RECEIVER,...` value from Part D.
- Add or remove entries to match how many cars you have — the grid renders automatically.
- The MAC format is **colon-separated uppercase hex** (`AA:BB:CC:DD:EE:FF`). The app converts it to the `CAR,AA,BB,...` serial packet for you.

Commit and push so GitHub Pages picks up the change:
```bash
git add index.html
git commit -m "Register real car MAC addresses"
git push
```

---

## Part F — Wire the car hardware

| ESP32 pin | Connects to | Signal |
|-----------|-------------|--------|
| **GPIO 18 (D18)** | Steering servo signal wire | 50 Hz servo PWM, 500–2500 µs |
| **GPIO 19 (D19)** | ESC signal wire | 50 Hz servo-style PWM, 1000–2000 µs |
| **GND** | Servo GND **and** ESC GND | Common ground (required) |
| **5 V / VIN** | *From BEC/ESC, not from USB* | Power for the ESP32 |

```
                ESP32 (car)
              ┌───────────┐
   Servo  ◀── │ GPIO 18   │
   ESC    ◀── │ GPIO 19   │
   GND    ─── │ GND ──────┼─── common ground (servo + ESC + battery)
   5V     ─── │ VIN       │ ◀── BEC / ESC 5V rail
              └───────────┘
```

**Failsafe behavior:** if the car stops receiving frames for **500 ms**, the receiver centers the steering (90°) and cuts the throttle to neutral automatically. This is defined by `FAILSAFE_MS` in the receiver firmware.

---

## 🎮 Using the arcade

1. Open the GitHub Pages URL in **Chrome/Edge**.
2. **USB Transmitter panel → Connect.** A browser dialog lists serial ports; pick the master ESP32. Status flips to `CONNECTED` and the chip turns green.
3. **Racing Wheel panel:** press any button/pedal on your wheel to bind it. Steering axis and poll rate appear live.
4. **FPV Video panel:** pick your capture device and click **Start Frame Grabber**. Grant camera permission. The viewport shows the live feed with the telemetry HUD.
5. **Select Your Vehicle:** click a car in the grid. This sends a `CAR,..` peer swap; the car's MAC appears as the HUD **Target**.
6. **Drive.** Steering and throttle bars, HUD numbers, and the TX-rate readout update at 50 Hz. The Telemetry Log shows outgoing `CAR`/`DRIVE` packets and any `ACK`/`RX` lines from the transmitter.

**Hot-swapping:** you can unplug/replug the USB transmitter or switch cars at any time. The app handles disconnects cleanly and re-asserts the current target when you reconnect.

---

## 📡 Serial protocol reference

All packets are lean, newline-terminated ASCII at **115200 baud**.

### Browser → Master transmitter
| Packet | Meaning |
|--------|---------|
| `CAR,AA,BB,CC,DD,EE,FF\n` | Swap the active ESP-NOW peer to this MAC (six hex bytes). |
| `DRIVE,<servo>,<motor>\n` | Steering `0–180`, motor `0–255`. Sent only when a value changes. |

### Master transmitter → Browser (telemetry, shown in the log)
| Packet | Meaning |
|--------|---------|
| `READY,MASTER,<mac>` | Boot banner. |
| `OK,CAR,<mac>` | Peer swap accepted. |
| `ACK,OK` / `ACK,FAIL` | ESP-NOW delivery status for the last frame. |
| `ERR,<code>` | Parse/target error (`CAR_LEN`, `NO_TARGET`, `OVERFLOW`, …). |

### Over the air (Master → Car, binary)
A packed `DriveFrame` struct — identical on both firmware files:
```c
typedef struct __attribute__((packed)) {
  uint8_t  servo;   // 0..180
  uint8_t  motor;   // 0..255
  uint32_t seq;     // rolling sequence for diagnostics
} DriveFrame;
```

---

## 🎚️ Calibration & tuning

**In the web app** ([`index.html`](index.html), tuning constants near the top of the script):
| Constant | Default | Purpose |
|----------|---------|---------|
| `POLL_HZ` | `50` | Gamepad poll + TX loop frequency. |
| `DEADZONE` | `5` | Throttle cutoff (of 0–255) to stop motor creep. |
| `AXIS_STEER` | `0` | Wheel steering axis index. |
| `AXIS_THROTTLE` | `1` | Gas pedal axis index. |

- **Steering map:** axis `[-1.0, 1.0]` → servo `[0°, 180°]`, center `90°` (`axisToServo`).
- **Throttle map:** pedal → `0–255`, with values `≤ 5` forced to `0` (`pedalToPwm`).
- If your wheel's axes are ordered differently, change `AXIS_STEER` / `AXIS_THROTTLE`. If your pedal rests at `0` instead of `-1`, call `pedalToPwm(axis, false)`.

**In the receiver** ([`car_receiver.ino`](electronics/car_receiver/car_receiver.ino)):
| Constant | Default | Purpose |
|----------|---------|---------|
| `ESC_NEUTRAL_US` | `1500` | Motor-stopped pulse width. |
| `ESC_MAX_US` | `2000` | Full-forward pulse width. |
| `FAILSAFE_MS` | `500` | Cut throttle if no frame arrives within this window. |

- If steering is reversed, swap the servo horn or invert in `axisToServo`.
- For **forward + reverse**, widen the ESC map (e.g. `1000–2000 µs` centered at `1500`) and remap `motor` accordingly.

---

## 🩺 Troubleshooting

| Symptom | Fix |
|---------|-----|
| **"Connect" does nothing / no port dialog** | You're not on Chrome/Edge, or not on HTTPS/localhost. Web Serial needs a secure context. |
| **Port not listed** | Install the CP210x/CH340 USB driver; make sure the Arduino Serial Monitor is **closed**. |
| **Compile error about `esp_now_recv_info_t` / `wifi_tx_info_t`** | You're on ESP32 core **2.x**. Upgrade to core **3.x**, or change the callbacks to the 2.x signatures (`onDataRecv(const uint8_t *mac, ...)` and `onDataSent(const uint8_t *mac, esp_now_send_status_t)`). |
| **`ESP32Servo.h: No such file`** | Install the **ESP32Servo** library (Part B). |
| **Car doesn't move but `ACK,OK` appears** | Check wiring/common ground; verify servo on D18 and ESC on D19; make sure the ESC is armed/calibrated. |
| **`ACK,FAIL` in the log** | Wrong MAC, car powered off, or out of ESP-NOW range. Confirm the MAC in `VEHICLES` matches the car's `READY,RECEIVER` line. |
| **Motor creeps at rest** | Raise `DEADZONE` in `index.html`, or recalibrate `ESC_NEUTRAL_US`. |
| **Camera won't start** | Grant camera permission; pick the correct capture device; a 640×480@60 source may fall back to a lower rate — that's fine. |
| **Master & car must be on the same WiFi channel** | Both use channel `0` (current channel). If you add WiFi/AP code, pin both to the same channel. |

---

## 📁 Repository layout

```
conradrc/
├── index.html                                  # Static arcade lobby web app (deploy to GitHub Pages)
├── README.md                                   # This file
└── electronics/
    ├── master_transmitter/
    │   └── master_transmitter.ino              # Desk-side ESP32: serial ↔ ESP-NOW bridge
    └── car_receiver/
        └── car_receiver.ino                    # Car-side ESP32: ESP-NOW → servo (D18) + ESC (D19)
```

---

*Built for zero-perceptible-latency (&lt;20 ms) local RC sim racing. Static build · Web Serial 115200 · ESP-NOW mesh.*
