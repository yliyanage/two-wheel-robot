# auto-launch.ps1
# Watches for the robot appearing - either over the USB cable (CH340 COM port) or
# wirelessly over its onboard BLE module (ELEGOO BT16 / HM-10 class). When it
# appears it automatically:
#   1. starts drive-bridge.ps1 (streams telemetry + relays commands), and
#   2. opens / reloads dashboard.html in your default browser.
# When the link drops it stops the bridge and waits for the next connect.
#
# Run this once and leave it running:
#   powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1            # auto (prefers Bluetooth/BLE)
#   powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1 -Link bt  # Bluetooth (BLE) only
#   powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1 -Link usb # USB cable only
#
# Stop it with Ctrl+C.

param(
    [int]$Baud = 9600,
    [int]$PollMs = 1000,
    [ValidateSet('auto','bt','usb')]
    [string]$Link = 'auto',   # auto = prefer BLE, fall back to USB cable

    # BLE identity of the robot (see .\ble.ps1 scan to discover it).
    [string]$BleMac     = '48872D76ECF9',
    [int]   $BleScanSec = 4,

    # Background mode: launch the bridge hidden (no console window) and mirror
    # status to a log file instead of the console. Used by the logon task so the
    # dashboard comes up automatically when you power on the robot, with no popups.
    [switch]$Background,
    [string]$LogFile
)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $root) { $root = (Get-Location).Path }
$dashboard = Join-Path $root 'dashboard.html'
$bridge    = Join-Path $root 'drive-bridge.ps1'
$bleExe    = Join-Path $root 'ble-bridge.exe'

# How the bridge child process is launched. In background mode it runs hidden so
# powering on the robot never flashes a console window in your face.
$bridgeStyle = if ($Background) { 'Hidden' } else { 'Normal' }
$bridgeFlags = if ($Background) { @('-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File') } `
                          else  { @('-ExecutionPolicy','Bypass','-NoExit','-File') }

if (-not $LogFile -and $Background) { $LogFile = Join-Path $root 'auto-launch.log' }

# Status line -> console (interactive) or log file (background).
function Write-Status([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    if ($LogFile) { try { Add-Content -Path $LogFile -Value $line -Encoding UTF8 } catch {} }
    if (-not $Background) { Write-Host $msg }
}

# Find the Nano's COM port over the USB cable, by its CH340 USB-serial description.
function Get-UsbPort {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match 'CH340' -and $_.Name -match 'COM(\d+)' } |
        Select-Object -First 1
    if ($dev -and $dev.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    return $null
}

# Is the robot's BLE module currently advertising? Runs a short scan with the
# helper and looks for the robot's MAC. Returns $true/$false.
function Test-BlePresent {
    if (-not (Test-Path $bleExe)) { return $false }
    $needle = ($BleMac -replace '[:\-]', '').ToUpper()
    $tmp    = Join-Path $env:TEMP ("robot-ble-scan-{0}.txt" -f $PID)
    try {
        $p = Start-Process -FilePath $bleExe -ArgumentList @('scan', "$BleScanSec") `
                -RedirectStandardOutput $tmp -RedirectStandardError ($tmp + '.err') `
                -NoNewWindow -PassThru
        $p.WaitForExit(($BleScanSec + 4) * 1000) | Out-Null
        if (-not $p.HasExited) { $p.Kill() }
        $hit = Select-String -Path $tmp -SimpleMatch $needle -Quiet -ErrorAction SilentlyContinue
        return [bool]$hit
    } catch { return $false }
    finally {
        Remove-Item $tmp, ($tmp + '.err') -ErrorAction SilentlyContinue
    }
}

# Resolve how the robot is reachable right now, honouring the link preference.
# Returns a hashtable describing the link, or $null when nothing is present:
#   @{ Kind = 'usb'; Port = 'COM3' }   or   @{ Kind = 'ble'; Mac = '...' }
function Get-RobotLink {
    switch ($Link) {
        'usb' {
            $p = Get-UsbPort
            if ($p) { return @{ Kind = 'usb'; Port = $p } }
            return $null
        }
        'bt' {
            if (Test-BlePresent) { return @{ Kind = 'ble'; Mac = $BleMac } }
            return $null
        }
        default {
            if (Test-BlePresent) { return @{ Kind = 'ble'; Mac = $BleMac } }  # prefer wireless
            $p = Get-UsbPort
            if ($p) { return @{ Kind = 'usb'; Port = $p } }
            return $null
        }
    }
}

$connected   = $false
$bridgeProc  = $null
$activeKind  = $null    # 'ble' or 'usb' for the current connection

Write-Status ("auto-launch watching for the robot (mode: {0}, background: {1})." -f $Link, [bool]$Background)
try {
    while ($true) {
        if (-not $connected) {
            # --- look for the robot coming up ---
            $robot = Get-RobotLink
            if ($robot) {
                $connected = $true
                $activeKind = $robot.Kind
                if ($robot.Kind -eq 'ble') {
                    Write-Status ("Robot up over BLE ({0}) - launching bridge + dashboard" -f $robot.Mac)
                    $bridgeProc = Start-Process powershell -PassThru -WindowStyle $bridgeStyle -ArgumentList (
                        $bridgeFlags + @("`"$bridge`"",'-Ble','-BleMac',$robot.Mac,'-Baud',"$Baud")
                    )
                } else {
                    Write-Status ("Robot up on {0} (USB) - launching bridge + dashboard" -f $robot.Port)
                    $bridgeProc = Start-Process powershell -PassThru -WindowStyle $bridgeStyle -ArgumentList (
                        $bridgeFlags + @("`"$bridge`"",'-Port',$robot.Port,'-Baud',"$Baud")
                    )
                }

                Start-Sleep -Milliseconds 2500       # let the bridge bind + first telemetry land
                Start-Process $dashboard              # open / reload the dashboard
            }
        }
        else {
            # --- already connected: detect the link going away ---
            # BLE: while the bridge holds the connection the robot no longer
            # advertises, so scanning would give a false "down". Instead watch
            # the bridge process (it exits when the BLE link drops). USB: watch
            # the COM port disappear.
            $down = $false
            if ($bridgeProc -and $bridgeProc.HasExited) {
                $down = $true
            } elseif ($activeKind -eq 'usb' -and -not (Get-UsbPort)) {
                $down = $true
            }

            if ($down) {
                $connected = $false
                $activeKind = $null
                Write-Status "Robot link down - stopping bridge"
                if ($bridgeProc -and -not $bridgeProc.HasExited) {
                    Stop-Process -Id $bridgeProc.Id -Force -ErrorAction SilentlyContinue
                }
                $bridgeProc = $null
            }
        }

        Start-Sleep -Milliseconds $PollMs
    }
}
finally {
    if ($bridgeProc -and -not $bridgeProc.HasExited) {
        Stop-Process -Id $bridgeProc.Id -Force -ErrorAction SilentlyContinue
    }
    Write-Status "auto-launch stopped."
}
