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
| `drive-bridge.ps1` | Owns the link (USB COM port **or** BLE via `-Ble`): streams `TLM` lines into `telemetry.json`/`.js`, relays commands from `command.txt`, and serves a localhost command endpoint (`http://127.0.0.1:8787/cmd`) for the dashboard buttons (no port conflict). |
| `auto-launch.ps1` | Watches for the robot (BLE advertising **or** Nano plugged in); auto-starts the bridge on whichever is present and opens/reloads the dashboard. |
| `ble-bridge.cs` / `ble-bridge.exe` | .NET helper that bridges the robot's BLE serial characteristic (`FFE0`/`FFE1`) to a stdin/stdout pipe. Modes: `scan`, `enum`, `tunnel`. Built without Visual Studio (see `ble.ps1`). |
| `ble.ps1` | Compiles `ble-bridge.cs` with `csc.exe` against the Windows Runtime metadata, then runs it (e.g. `.\ble.ps1 scan`). |
| `dashboard.html` | Live dashboard (angle, wheel speeds, IMU, encoders, motor state, transport + link health) plus manual troubleshoot controls (per-motor + spin buttons). |
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
With the bridge running, use the **Manual control / troubleshoot** card in the
dashboard (per-motor CW/CCW, both forward/reverse, spin left/right, STOP) - the
buttons POST to the bridge's local command endpoint.

Or queue commands directly via `command.txt`:
```powershell
Set-Content .\command.txt "F 80"      # both wheels forward (auto-stops after 3s)
Set-Content .\command.txt "M 80 -80" # left fwd, right reverse (spin in place)
Set-Content .\command.txt "S"         # stop now
```
> `M <left> <right>` takes signed PWM per motor: `+` = forward, `-` = reverse,
> `0` = stopped. Magnitudes are capped at `PWM_MAX` and the 3s watchdog still applies.

### C. Stop everything safely
- Press **Ctrl+C** in the bridge terminal (frees the port, closes it cleanly).
- To stop the *robot* itself: **physical power switch / pull the battery.**

---

## 4b. Wireless link over Bluetooth

## 4b. Wireless link over Bluetooth (BLE)

**Working as of 2026-06-07.** The robot's onboard module is **BLE (Bluetooth Low
Energy)**, not Classic/SPP - an active Classic inquiry from the PC reliably finds
other nearby devices but never the robot, while a BLE scan finds it advertising as
**`ELEGOO BT16`**. It exposes the classic HM-10 / CC2541 serial profile:

- **MAC:** `48:87:2D:76:EC:F9`
- **Service:** `0000FFE0-0000-1000-8000-00805F9B34FB`
- **Characteristic `FFE1`** (`0000FFE1-...`): `Read, Write, WriteWithoutResponse,
  Notify` - a single characteristic carries **both** directions (notify =
  telemetry in, write = commands out).

Because BLE gets **no Windows `COMx` port**, a small .NET helper bridges it:

- **`ble-bridge.exe`** (built from `ble-bridge.cs`) connects to the BLE module and
  tunnels `FFE1` as a plain stdin/stdout pipe: notifications -> stdout, stdin ->
  write. It has three modes: `scan`, `enum <mac>`, `tunnel <mac> <svc> <notify> <write>`.
- **`drive-bridge.ps1 -Ble`** launches that helper instead of opening a COM port,
  and keeps everything else identical (telemetry parsing, `telemetry.json`/`.js`,
  `command.txt`, the `http://127.0.0.1:8787/cmd` endpoint). Telemetry is tagged
  `"Link":"bluetooth"` and the dashboard shows **Transport: Bluetooth**.

> **No firmware change needed.** The BLE module shares the Nano's hardware UART
> (D0/D1) with the USB port, so the existing `drive-forward.ino` telemetry and
> `F`/`S`/`M` commands flow over BLE unchanged.

### Run it wirelessly

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

# auto-launch (auto prefers BLE, falls back to USB; opens the dashboard):
powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1            # auto
powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1 -Link bt  # BLE only
powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1 -Link usb # USB only

# or run the bridge directly over BLE:
.\drive-bridge.ps1 -Ble
```

> BLE allows **one** central connection at a time - close the Elegoo phone app
> (disconnect it) before connecting from the PC, or the PC connect will fail.

### Rebuild / re-discover the helper

```powershell
.\ble.ps1 -Rebuild scan 14         # rebuild ble-bridge.exe and scan for the robot
.\ble.ps1 enum 48872D76ECF9        # list the robot's GATT services/characteristics
```

`ble.ps1` compiles `ble-bridge.cs` with the .NET Framework `csc.exe` against the
Windows Runtime metadata (`Windows.Devices.winmd` etc.) - no Visual Studio or .NET
SDK required. If your robot has a different MAC, find it with `.\ble.ps1 scan` and
pass `-BleMac <hex>` to `auto-launch.ps1` / `drive-bridge.ps1`.

### Alternative: a Classic SPP module

If you prefer a `COMx`-style link, add an **HC-05 / HC-06** on the Nano UART (keep
baud **9600**); Windows then exposes a serial port and the bridge works without the
BLE helper. Wiring: VCC->5V, GND->GND, module TXD->Nano RX (D0), module RXD->Nano
TX (D1) via a 1k/2k divider. Disconnect it before a USB upload (shares D0/D1).

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

**Wireless link: working over BLE.** PC has a working Bluetooth USB dongle
(`Bluetooth 5.0 USB Adapter`, verified). The robot's onboard module is **BLE**
(advertises as `ELEGOO BT16`, HM-10/CC2541 serial profile, service `FFE0` /
characteristic `FFE1`). Since BLE gets no Windows `COMx` port, `ble-bridge.exe`
tunnels the BLE serial characteristic to a pipe and `drive-bridge.ps1 -Ble` consumes
it, so the dashboard runs wirelessly with no firmware change. `auto-launch.ps1`
auto-detects BLE (preferred) or USB. See section 4b for usage.

Done:
- **Pin map verified** via `pin-probe`: STBY=8, left motor PWM=5/DIR=7, right motor
  PWM=6/DIR=12, left encoder=2, right encoder=4. Forward = both DIR pins LOW.
- **Right encoder works** - it just needed a pin-change interrupt (pin 4 is not a
  hardware-interrupt pin). Both encoders now report.
- **Drive-forward firmware** with PWM cap + 3s watchdog + emergency stop, plus the
  signed per-motor `M l r` command for the dashboard troubleshoot buttons.
- **Live dashboard while driving** via `drive-bridge.ps1`; `auto-launch.ps1` opens it
  on connect and now **auto-detects the Bluetooth SPP port** (prefers wireless).

Next:
- **Decide the wireless path** (section 4b): the stock module is BLE (no COM port),
  so to drive the PC dashboard wirelessly either add an HC-05/HC-06 SPP module on the
  Nano UART (then it just works), or build a BLE-GATT bridge to replace the serial
  read/write. The telemetry + control pipeline already works over any COM port.
- Self-balancing controller (PID + tilt cutoff) is still to be written.

See `THREAT-MODEL.md` for the full risk analysis and rollback playbook.
