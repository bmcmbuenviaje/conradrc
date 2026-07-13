# 🧪 Bench-Test & Commissioning Checklist

Run through this **once per new car** and **once per event, per cabinet** before you put wheels on the ground. Take ~15 minutes; save yourself a crashed car.

Everything is a checkbox — go top-to-bottom, don't skip. Steps marked ⚠️ mean **wheels up / prop off** — the car will move if you get it wrong.

---

## 0 · One-time fleet setup

- [ ] All ESP32 firmware compiled from the **same commit** — mixed `DriveFrame` / `TelemetryFrame` layouts corrupt packets silently.
- [ ] Master + every car have matching `ENABLE_CRYPTO` and (if true) matching `PMK` / `LMK`.
- [ ] Each car's MAC labeled on a piece of tape on the chassis.
- [ ] Roster (`⇩ Export`) backed up somewhere off the cabinet — localStorage is not durable.

---

## 1 · Desk-side smoke test (no car powered)

- [ ] Master flashed and USB-connected. Serial monitor **closed**.
- [ ] Open the Pages URL in Chrome/Edge. **No red console errors.**
- [ ] Optional: open [test.html](test.html) — should show **N passed · 0 failed**.
- [ ] Click **Connect** → the browser dialog lists the master port → status flips to `CONNECTED`.
- [ ] The **Safety** panel says `DISARMED`. Do **not** arm yet.
- [ ] The HUD shows `— ms` latency and `—` battery (no car online — expected).

---

## 2 · Car boot (⚠️ wheels up, prop off, ESC disarmed)

- [ ] Chassis **on jack stands / blocks** — nothing touching the ground.
- [ ] Power the car. Serial output shows `READY,RECEIVER,<mac>` (hobby) or `READY,RECEIVER_TOY,<mac>` (toy-grade).
- [ ] MAC on the car's tape **matches** what's registered in the lobby.
- [ ] In the lobby: select the car. HUD `Target` becomes that MAC's last 8 hex digits.
- [ ] After ~2 seconds, HUD `Latency` shows a real number (single-digit / low-teens ms), `Battery` and `RSSI` populate.
- [ ] Open the **🛰 Fleet Dashboard**: this car reads `ACTIVE`. Battery ≥ your low-battery threshold (default 7.0 V for 2S).

---

## 3 · Control mapping

- [ ] Wiggle the wheel — **Steer Axis** in Racing Wheel panel matches (0 at center, ±1 at lock).
- [ ] If not: **🎮 Test & Map Controls** → confirm steering / throttle / brake axes, save.
- [ ] Press each pedal — the correct axis lights up green in the mapper.
- [ ] Bind shift-up / shift-down / reverse buttons if you use manual.
- [ ] Close the mapper. Steer to full lock left/right — HUD `Steering` reads `0°` and `180°`.

---

## 4 · Steering trim & endpoint (⚠️ wheels up)

- [ ] With controller **centered**, does the servo point straight? If not:
  - [ ] **Steering Tuning → Trim** slider until the wheels are dead straight.
- [ ] Full lock left/right — do the tie rods hit the chassis?
  - [ ] If yes, **EPA** slider down (start 90%, back off until nothing binds).
- [ ] Save this per-car: **Edit** the vehicle → set `Steer Trim` and `EPA` → **Save**. Selecting the car will re-apply.
- [ ] Verify **Invert Steering** is correct — left on controller = left on wheels.

---

## 5 · Throttle / ESC calibration (⚠️ wheels up)

- [ ] **First run only** — press **🛠 Calibrate ESC** in Drivetrain and follow your ESC's beep sequence. Skip this if the ESC is already calibrated.
- [ ] Dead-zone: with feet off pedals, throttle bar reads `0`. If it flickers, raise **Dead-zone** in the mapper.
- [ ] Set **Max Power** to **30 %** for the first drive.
- [ ] **⏻ ARM** — should succeed (throttle at rest).
- [ ] Tap gas briefly — motor spins the correct direction (forward from your POV). If reversed: swap two motor phases (brushless) or motor leads (brushed), **or** invert in firmware.
- [ ] **■ STOP** — motor cuts, Safety badge flashes `E-STOP`. Re-arm to continue.

---

## 6 · Reverse & engine braking (⚠️ wheels up, manual mode only)

- [ ] Press **◄ REVERSE** → HUD gear turns to `R` (amber). Tap gas — motor turns opposite direction.
- [ ] If it doesn't reverse: your ESC needs reverse enabled in its own programming (many are forward + brake by default).
- [ ] Disengage reverse. Switch to **Manual**. Floor pedal in gear 1 — car spins wheels, RPM hits redline, **shift-light strip flashes red**.
- [ ] Upshift — RPM drops, motor climbs again. Repeat through gears 1–6.
- [ ] Lift throttle mid-run — motor briefly pulses **negative** (engine brake). If the wheels *reverse* instead of braking: lower **Engine Braking** slider or set ESC to brake-only mode.

---

## 7 · Failsafe & safety

- [ ] Arm the car → **Alt-Tab** away → the app **auto-disarms** (Safety badge back to `DISARMED`, motor neutral). Return, re-arm.
- [ ] Arm → unplug the wheel → app auto-disarms.
- [ ] Arm → yank the master USB → app auto-disarms + link chip goes red.
- [ ] Arm → press **`Space`** → E-STOP.
- [ ] Cut car power mid-drive → wait 500 ms → serial log shows `FAILSAFE,LINK_LOST` (from the last frame the receiver held before quitting; you'll see it on next power-up as `failsafes` in TELEM).

---

## 8 · Range check (wheels down but no drive)

- [ ] Walk the car to the far corner of your intended play area, keeping it powered.
- [ ] HUD `RSSI` stays better than **−80 dBm** everywhere you plan to drive. Worse than that and packets will start dropping.
- [ ] `Latency` stays under **20 ms**, `packet loss` under **2 %**.
- [ ] If RSSI is bad: raise the master (line-of-sight helps a lot at 2.4 GHz), or add an external antenna to the ESP32.

---

## 9 · First real lap (wheels down)

- [ ] Empty area, at least 3 m clear.
- [ ] **Max Power ≤ 30 %** for the first lap. Manual mode + gear 1 for extra caution.
- [ ] Keep the STOP button visible / spacebar within reach.
- [ ] Do one slow lap. Adjust trim / EPA / dead-zone in-app as you go — changes are live.
- [ ] Only *after* one clean lap: raise Max Power one step at a time.

---

## 10 · Post-run

- [ ] Battery voltage under load stays above your low-battery warning threshold.
- [ ] Check the **Fleet Dashboard** — `failsafes` count didn't jump (each jump = a link loss during the run).
- [ ] Save your final per-car tune (**Edit → Save**). Take a snapshot via **📷** for the record.
- [ ] `⇩ Export` roster JSON, paste it into a chat / file — that's your durable backup.

---

## Common failure modes & first-response

| Symptom | Try this first |
|---|---|
| Car twitches at rest | Raise dead-zone → recalibrate ESC neutral → check RSSI |
| Car pulls to one side | Steering trim (per-car), then check tie-rod length |
| Steering binds at full lock | Lower EPA to 85–90 % |
| Motor is too fast to control | Max Power 30 %, switch to Manual gear 1–3 |
| Long-distance stutter | Elevate the master; check RSSI < -80 |
| `ACK,FAIL` bursts | Range issue OR another master on the same channel — check Fleet Dashboard for `IN USE` claims |
| Car keeps disarming | Something dropped focus — check tab, wheel, and USB |
| Reverse does nothing | Enable reverse in the ESC's own programming |
| Engine brake nudges backward at low speed | Lower Engine Braking slider or set ESC to forward+brake mode |

---

*Print this. Keep it at the cabinet.*
