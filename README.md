# two-wheel-robot

Telemetry + drive firmware and a live dashboard for an **Elegoo Tumbller**
self-balancing two-wheel robot (Arduino Nano, CH340 USB-serial). The robot streams
sensor data over the serial port; a small PowerShell bridge turns that into JSON;
a local HTML dashboard shows it in real time. The bridge can also relay drive
commands back to the robot.

**Safety:** motor drive is gated behind explicit commands, a hard PWM cap, and a
3-second auto-stop watchdog, and is meant for **bench testing with the wheels off
the ground**. This is a self-balancing robot - it cannot stand on the floor until a
balance controller is running.

---

## 1. Hardware

- **Board:** Arduino Nano clone (CH340, `VID_1A86&PID_7523`) on **COM3**.
- **Robot:** Elegoo Tumbller - 2x encoder DC motors, TB6612FNG driver,
  MPU6050 gyro/accel on I2C (A4=SDA, A5=SCL).
- Identified from the stock firmware's serial output
  (`encoder_count_left_a` / `encoder_count_right_a`).

---

## 2. Files

| File | What it is |
|---|---|
| `drive-forward/drive-forward.ino` | Drive firmware. Drives both motors forward on the verified pins, reads both encoders + MPU6050, emits full `TLM ...` telemetry. Commands `F [pwm]` / `S`; PWM cap + 3s watchdog. |
| `pin-probe/pin-probe.ino` | Safe pin-verification sketch (pulses one or both motors briefly, reports encoder delta). Used to confirm the pin map. |
| `pin-probe/probe-send.ps1` | Sends a single probe command and captures the response. |
| `tumbller-robot.ino` | Original telemetry-only sketch (motors in STANDBY). Kept for reference; pin map here is superseded by the verified one. |
| `sketch.yaml` | arduino-cli build/upload config. Profiles `nano-new` (default) and `nano-old`; port `COM3`. |
| `drive-bridge.ps1` | Owns COM3: streams `TLM` lines into `telemetry.json`/`.js` and relays commands from `command.txt` to the robot (no port conflict). |
| `auto-launch.ps1` | Watches for the Nano being plugged in; auto-starts the bridge and opens/reloads the dashboard. |
| `dashboard.html` | Read-only live dashboard (angle, wheel speeds, IMU, encoders, motor state, link health). |
| `vendor/chart.umd.min.js` | Chart.js, vendored locally (works offline). |
| `THREAT-MODEL.md` | STRIDE threat model + rollback playbook. |

Generated at runtime (safe to delete, git-ignored): `telemetry.json`, `telemetry.js`, `command.txt`, `build/`.

---

## 3. Toolchain

`arduino-cli` is installed user-scoped at
`%LOCALAPPDATA%\Programs\arduino-cli\arduino-cli.exe`, with the `arduino:avr` 1.8.8
core. A convenience variable for the commands below:

```powershell
$cli = Join-Path $env:LOCALAPPDATA 'Programs\arduino-cli\arduino-cli.exe'
```

---

## 4. Common workflows

### A. Build + upload firmware
```powershell
Set-Location 'c:\Users\yasitha shehan\Projects\tumbller-robot'
& $cli compile -e
& $cli upload -p COM3
```
> **Stop the bridge first** (it holds COM3). **Put the wheels off the ground** -
> a board reset briefly re-runs firmware during upload.
> If upload fails with *"not in sync"*, use the other bootloader:
> `& $cli upload -p COM3 -m nano-old`

### B. Run the live dashboard
Easiest - let it auto-start when you plug the board in:
```powershell
Set-Location 'c:\Users\yasitha shehan\Projects\two-wheel-robot'
powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1
```
Connect the Nano: the bridge starts and the dashboard opens automatically. It
auto-refreshes 4x/second.

Or run the bridge manually:
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
.\drive-bridge.ps1            # streams telemetry into telemetry.json (Ctrl+C to stop)
```
Then open `dashboard.html` in a browser.

### B2. Drive the motors (wheels off the ground)
With the bridge running, queue commands via `command.txt`:
```powershell
Set-Content .\command.txt "F 80"   # drive both wheels forward (auto-stops after 3s)
Set-Content .\command.txt "S"      # stop now
```

### C. Stop everything safely
- Press **Ctrl+C** in the bridge terminal (frees COM3, closes the port cleanly).
- To stop the *robot* itself: **physical power switch / pull the battery.**

---

## 5. Telemetry format

One line per cycle on the serial port at 9600 baud:

```
TLM t=<ms> encL=<n> encR=<n> spdL=<c/s> spdR=<c/s> ax=.. ay=.. az=.. gx=.. gy=.. gz=.. angle=<deg> pwmL=0 pwmR=0 stby=1 mpu=<0|1>
```

- `encL/encR` - encoder counts; `spdL/spdR` - counts/second.
- `ax..gz` - raw MPU6050 accel/gyro; `angle` - **approximate** pitch from the accel.
- `stby=1` - motor driver disabled. `mpu=1` - IMU responding on I2C.

---

## 6. Status / next steps

**Paused** pending a wireless link so the robot can be tested untethered.

Done:
- **Pin map verified** via `pin-probe`: STBY=8, left motor PWM=5/DIR=7, right motor
  PWM=6/DIR=12, left encoder=2, right encoder=4. Forward = both DIR pins LOW.
- **Right encoder works** - it just needed a pin-change interrupt (pin 4 is not a
  hardware-interrupt pin). Both encoders now report.
- **Drive-forward firmware** with PWM cap + 3s watchdog + emergency stop.
- **Live dashboard while driving** via `drive-bridge.ps1`; `auto-launch.ps1` opens it
  on connect.

Next (when a wireless receiver is available):
- This PC has **no Bluetooth radio** (the "Wi-Fi" is a BT-less Realtek USB stick), so
  wireless telemetry needs added hardware: a USB Bluetooth dongle (if the robot has a
  BT module on its UART), an ESP8266/ESP32 Wi-Fi bridge, or an HC-12/nRF24 radio pair
  (the spare Uno R3 can be the PC-side receiver).
- Self-balancing controller (PID + tilt cutoff) is still to be written.

See `THREAT-MODEL.md` for the full risk analysis and rollback playbook.
