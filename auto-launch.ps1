# auto-launch.ps1
# Watches for the Nano (CH340 USB-serial) being plugged in. When it appears it
# automatically:
#   1. starts drive-bridge.ps1 (streams telemetry + relays commands), and
#   2. opens / reloads dashboard.html in your default browser.
# When the board is unplugged it stops the bridge and waits for the next connect.
#
# Run this once and leave it running:
#   powershell -ExecutionPolicy Bypass -File .\auto-launch.ps1
#
# Stop it with Ctrl+C.

param(
    [int]$Baud = 9600,
    [int]$PollMs = 1000
)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $root) { $root = (Get-Location).Path }
$dashboard = Join-Path $root 'dashboard.html'
$bridge    = Join-Path $root 'drive-bridge.ps1'

# Find the Nano's COM port by its CH340 USB-serial description.
function Get-NanoPort {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match 'CH340' -and $_.Name -match 'COM(\d+)' } |
        Select-Object -First 1
    if ($dev -and $dev.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
    return $null
}

$connected   = $false
$bridgeProc  = $null

Write-Host "auto-launch watching for the Nano (CH340). Ctrl+C to stop."
try {
    while ($true) {
        $port = Get-NanoPort

        if ($port -and -not $connected) {
            # --- rising edge: board just connected ---
            $connected = $true
            Write-Host ("[{0}] Nano connected on {1} - launching bridge + dashboard" -f (Get-Date -Format HH:mm:ss), $port)

            # Start the bridge in its own window so it owns the serial port.
            $bridgeProc = Start-Process powershell -PassThru -ArgumentList @(
                '-ExecutionPolicy','Bypass','-NoExit','-File',"`"$bridge`"",'-Port',$port,'-Baud',"$Baud"
            )

            Start-Sleep -Milliseconds 2500           # let the bridge bind + first telemetry land
            Start-Process $dashboard                  # open / reload the dashboard
        }
        elseif (-not $port -and $connected) {
            # --- falling edge: board unplugged ---
            $connected = $false
            Write-Host ("[{0}] Nano disconnected - stopping bridge" -f (Get-Date -Format HH:mm:ss))
            if ($bridgeProc -and -not $bridgeProc.HasExited) {
                Stop-Process -Id $bridgeProc.Id -Force -ErrorAction SilentlyContinue
            }
            $bridgeProc = $null
        }

        Start-Sleep -Milliseconds $PollMs
    }
}
finally {
    if ($bridgeProc -and -not $bridgeProc.HasExited) {
        Stop-Process -Id $bridgeProc.Id -Force -ErrorAction SilentlyContinue
    }
    Write-Host "auto-launch stopped."
}
