# drive-bridge.ps1
# ONE process that owns the serial port so the dashboard can be live WHILE we
# drive the motors. It does two things at once:
#   1. Reads the robot's "TLM ..." telemetry and writes telemetry.json/.js
#      (the dashboard polls telemetry.json).
#   2. Watches command.txt - whenever its contents change, the new line is
#      sent to the robot (e.g. "F 80" or "S"). This avoids a second program
#      opening the port and fighting for it.
#
# Usage:
#   .\drive-bridge.ps1                 # COM3 @ 9600
#   # then, from any other terminal, queue a command WITHOUT touching the port:
#   Set-Content .\command.txt "F 80"   # drive forward
#   Set-Content .\command.txt "S"      # stop
#
# Stop the bridge with Ctrl+C. Stop it before uploading new firmware.

param(
    [string]$Port = 'COM3',
    [int]$Baud = 9600
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $root) { $root = (Get-Location).Path }
$jsonOut = Join-Path $root 'telemetry.json'
$jsOut   = Join-Path $root 'telemetry.js'
$cmdFile = Join-Path $root 'command.txt'

# Start from a clean command file so a stale command is not re-sent on launch.
Set-Content -Path $cmdFile -Value '' -Encoding ascii

Write-Host "Opening $Port @ $Baud ... (Ctrl+C to stop)"
$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud)
$sp.NewLine     = "`n"
$sp.ReadTimeout = 300
$sp.DtrEnable   = $true   # note: toggling DTR resets the board
$sp.RtsEnable   = $true

try {
    $sp.Open()
} catch {
    Write-Host "ERROR opening ${Port}: $($_.Exception.Message)"
    Write-Host "Is another program (or another bridge) using the port? Close it and retry."
    return
}

# The board resets when the port opens; give the bootloader time before sending.
Start-Sleep -Milliseconds 1800

$lastWrite   = [DateTime]::MinValue
$writeEveryMs = 200
$numeric = 't','encL','encR','spdL','spdR','ax','ay','az','gx','gy','gz','angle','pwmL','pwmR','stby','mpu'
$lastCmd = ''

Write-Host "Streaming telemetry -> $jsonOut. Queue commands by writing command.txt."
try {
    while ($true) {
        # --- 1. forward any new command from command.txt to the robot ---
        if (Test-Path $cmdFile) {
            $cmd = (Get-Content -Path $cmdFile -Raw -ErrorAction SilentlyContinue)
            if ($null -ne $cmd) {
                $cmd = $cmd.Trim()
                if ($cmd.Length -gt 0 -and $cmd -ne $lastCmd) {
                    $sp.WriteLine($cmd)
                    $lastCmd = $cmd
                    Write-Host ("-> sent: {0}" -f $cmd)
                }
            }
        }

        # --- 2. read a telemetry line (non-blocking-ish via short timeout) ---
        $line = $null
        try { $line = $sp.ReadLine() } catch { $line = $null }   # timeout -> loop
        if (-not $line) { continue }
        $line = $line.Trim()
        if (-not $line.StartsWith('TLM')) { continue }

        $data = [ordered]@{}
        foreach ($tok in ($line -split '\s+')) {
            if ($tok -eq 'TLM') { continue }
            $kv = $tok -split '=', 2
            if ($kv.Count -ne 2) { continue }
            $k = $kv[0]; $v = $kv[1]
            if ($numeric -contains $k) {
                $n = 0.0
                if ([double]::TryParse($v, [ref]$n)) { $data[$k] = $n } else { $data[$k] = $v }
            } else {
                $data[$k] = $v
            }
        }
        if ($data.Count -eq 0) { continue }

        $data['Updated']  = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
        $data['Port']     = $Port
        $data['Baud']     = $Baud
        $data['MpuOk']    = ([double]($data['mpu']))  -eq 1
        $data['Standby']  = ([double]($data['stby'])) -eq 1
        $data['RightEncoderActive'] = ([double]($data['encR'])) -gt 0

        $now = Get-Date
        if (($now - $lastWrite).TotalMilliseconds -ge $writeEveryMs) {
            $json = ([pscustomobject]$data) | ConvertTo-Json -Depth 4
            $json | Out-File -FilePath $jsonOut -Encoding utf8
            "window.TELEMETRY = $json;" | Out-File -FilePath $jsOut -Encoding utf8
            $lastWrite = $now
        }
    }
}
finally {
    if ($sp -and $sp.IsOpen) { $sp.Close() }
    Write-Host "Serial port closed. Bridge stopped."
}
