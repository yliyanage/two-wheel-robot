# STRIDE Threat Model - Tumbller Robot Telemetry Project

Scope: an Arduino Nano (Elegoo Tumbller self-balancing robot) connected to a
Windows PC over **USB-serial (CH340, COM3)** or **Bluetooth Low Energy** (the
robot's onboard `ELEGOO BT16` HM-10/CC2541 module, service `FFE0` / characteristic
`FFE1`). A PowerShell **bridge** reads the robot's telemetry and writes
`telemetry.json`; a local **dashboard** (`dashboard.html`) displays it and can send
a small whitelist of drive commands. Over BLE the link is carried by a .NET helper
(`ble-bridge.exe`). The firmware enables the motor driver only while a command is
active, under a hard PWM cap and a 3 s auto-stop watchdog.

This is a hobby/desktop system, not a production deployment. The model focuses on
the realistic risks: **physical safety (the robot moving unexpectedly)**, data
integrity, and not bricking the board. Every mitigation has a concrete rollback.

---

## Trust boundaries

```
[Operator] --USB/BLE--> [Arduino Nano firmware] --serial/FFE1--> [drive-bridge.ps1] --file--> [telemetry.json] --> [dashboard.html]
                                                  (BLE only: via ble-bridge.exe pipe)
```

1. **USB / serial port** - only one process can hold COM3 at a time.
2. **BLE link** - the module accepts one central at a time (the PC *or* the phone
   app, not both). Plain-text GATT, **no pairing/auth** on the `FFE1` characteristic
   (HM-10 default) - anyone in radio range could connect when the PC/phone is not.
3. **Firmware <-> PC** - plain-text serial/GATT, no auth (by design; local/bench).
4. **Bridge -> files -> dashboard** - local files; the dashboard also POSTs commands
   to `http://127.0.0.1:8787/cmd` (loopback only).

The dashboard can command the robot, but only through a **whitelist** (`F [pwm]`,
`S`, `M <l> <r>`); the firmware caps PWM and auto-stops after 3 s. Intended for
**bench testing with the wheels off the ground**.

---

## STRIDE analysis

### S - Spoofing
- **Risk:** A different device enumerates as COM3 and feeds fake telemetry; or the
  dashboard reads a stale `telemetry.json` left by an old run. Over **BLE**, an
  attacker in radio range could connect to the unauthenticated `FFE1` characteristic
  and either spoof telemetry or send drive commands while the PC/phone is
  disconnected.
- **Likelihood/impact:** Low / Low-Medium (BLE adds range but the robot is
  bench-only, wheels off the ground, PWM-capped and watchdog-stopped).
- **Mitigations:** Bridge only accepts lines starting with `TLM`; dashboard shows a
  `Last packet` timestamp and a `Bridge: receiving/waiting` pill so stale data is
  visible, plus a `Transport` pill (USB/Bluetooth). Verify the port with Device
  Manager (CH340 = `VID_1A86&PID_7523`); verify the BLE peer MAC with `.\ble.ps1
  scan` (expected `48:87:2D:76:EC:F9`). Keep BLE connected from the PC/phone so no
  third party can occupy the single-central slot; power the robot off when idle. For
  a hardened setup, replace the stock module with one that enforces pairing, or use
  USB only.
- **Rollback:** Delete `telemetry.json`/`telemetry.js`; the dashboard falls back to
  the seed and shows "waiting".

### T - Tampering
- **Risk:** Telemetry corrupted in transit (electrical noise) or files edited.
- **Impact:** Misleading readings; on a balancing bot a bad angle could later drive
  wrong motor output (once motors are enabled).
- **Mitigations:** Per-token `key=value` parsing tolerates partial/garbled lines
  (bad tokens are dropped, not guessed). Motors stay in STANDBY, so corrupted data
  cannot cause motion today. Numeric fields are type-checked before use.
- **Rollback:** Re-flash known-good firmware (see Rollback section); regenerate the
  data files by restarting the bridge.

### R - Repudiation
- **Risk:** No record of what firmware was on the board or when changes were made.
- **Mitigations:** Firmware is built reproducibly from `tumbller-robot.ino` via
  `arduino-cli` with a pinned core (`arduino:avr 1.8.8`); git history records sketch
  changes. The bridge stamps each packet with `Updated`.
- **Rollback:** `git checkout <commit> -- tumbller-robot.ino` then recompile/upload.

### I - Information disclosure
- **Risk:** Low. Telemetry is non-sensitive (angles, encoder counts). Files are local.
- **Mitigations:** Nothing is sent over a network; the dashboard runs from `file://`.
  No secrets, tokens, or PII in this project.
- **Rollback:** N/A.

### D - Denial of service  (most relevant category here)
- **Risk 1:** **Port contention** - the bridge holds COM3, so `arduino-cli upload`
  fails ("not in sync" / "unable to open port").
- **Risk 2:** The flaky terminal / HDD load stalls the bridge.
- **Mitigations:** Documented rule: **stop the bridge before uploading**. The bridge
  closes the port cleanly on Ctrl+C (`finally` block). Disk writes are throttled to
  ~5 Hz to spare the old HDD.
- **Rollback:** Close the bridge terminal (or `Stop-Process`); the port frees
  immediately. Re-run the bridge afterwards.

### E - Elevation of privilege  /  **PHYSICAL SAFETY (highest priority)**
- **Risk:** The robot moves unexpectedly - the real "privilege escalation" here is
  *code gaining control of the motors*. Causes: wrong pin map enabling the TB6612
  driver, a future bug taking the driver out of STANDBY, or a board reset re-running
  motor code.
- **Mitigations:**
  - Firmware drives `PIN_MOTOR_STBY = LOW` (motors disabled) and PWM = 0 in `setup()`,
    **before** anything else.
  - No code path currently raises STBY; motor enable is deferred until pins are
    verified.
  - Pin map is flagged `VERIFY` in source; documented "wheels off the ground" rule
    for every upload (a board reset briefly re-runs old firmware).
  - The bridge/dashboard are read-only and cannot move the robot.
- **Rollback:** Cut power with the robot's physical switch / pull the battery (the
  guaranteed stop), then re-flash the safe sketch.

---

## Rollback playbook (quick reference)

| Situation | Action |
|---|---|
| **Robot is moving / unsafe** | **Power switch OFF or pull the battery.** Physical cut is the ground truth. |
| Upload fails "unable to open port" | Stop the bridge (`drive-bridge.ps1` / `auto-launch.ps1`) to free COM3, then upload. |
| Upload fails "not in sync" | Switch profile: `arduino-cli upload -p COM3 -m nano-old` (old bootloader). |
| Bad firmware behavior | `git checkout -- tumbller-robot.ino`, then `arduino-cli compile -e` + `upload`. |
| Telemetry looks wrong/stale | Delete `telemetry.json` + `telemetry.js`, restart the bridge. |
| Want the factory firmware back | It is gone (AVR flash can't be read back as source). Re-flash Elegoo's official Tumbller sketch from their download page to restore stock behavior. |
| Toolchain broken | Reinstall: delete `%LOCALAPPDATA%\Programs\arduino-cli`, re-run install; core in `%LOCALAPPDATA%\Arduino15`. |

---

## Residual risks / notes

- **The original factory firmware was overwritten** and cannot be recovered from the
  chip. Restoring stock behavior means re-flashing Elegoo's published source.
- **The pin map is unverified.** Until confirmed against Elegoo's documentation, do
  not enable the motors. The right encoder reads zero - a hardware/wiring item to
  investigate (loose connector or non-interrupt pin), tracked in the dashboard.
- **Self-balancing is intentionally disabled.** Re-enabling it makes the robot drive;
  that step needs its own review (PID limits, tilt cutoff so a fallen robot stops).
