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
#   .\drive-bridge.ps1                 # USB serial, COM3 @ 9600
#   .\drive-bridge.ps1 -Ble            # WIRELESS over the robot's BLE module
#   # then, from any other terminal, queue a command WITHOUT touching the link:
#   Set-Content .\command.txt "F 80"   # drive forward
#   Set-Content .\command.txt "S"      # stop
#
# In -Ble mode the bridge launches ble-bridge.exe, which connects to the robot's
# onboard BLE serial module (HM-10 class, service FFE0 / characteristic FFE1)
# and tunnels it as a stdin/stdout pipe. Everything else (telemetry parsing,
# telemetry.json/.js, command.txt watch, the HTTP /cmd endpoint) is identical,
# so the dashboard works the same whether the robot is on USB or Bluetooth.
#
# Stop the bridge with Ctrl+C. Stop it before uploading new firmware.

param(
    [string]$Port = 'COM3',
    [int]$Baud = 9600,
    [int]$HttpPort = 8787,

    # --- wireless (BLE) mode ---
    [switch]$Ble,
    [string]$BleMac     = '48872D76ECF9',
    [string]$BleService = '0000ffe0-0000-1000-8000-00805f9b34fb',
    [string]$BleChar    = '0000ffe1-0000-1000-8000-00805f9b34fb',
    [string]$BleExe
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $root) { $root = (Get-Location).Path }
$jsonOut = Join-Path $root 'telemetry.json'
$jsOut   = Join-Path $root 'telemetry.js'
$cmdFile = Join-Path $root 'command.txt'

# Start from a clean command file so a stale command is not re-sent on launch.
Set-Content -Path $cmdFile -Value '' -Encoding ascii

# ---------------------------------------------------------------------------
# Open the link to the robot. Two backends expose the SAME two operations:
#   & $sendLine "F 80"   -> send one command line to the robot
#   & $readLine          -> return one telemetry line, or $null if none ready
# so the main loop below does not care whether we are on USB serial or BLE.
# ---------------------------------------------------------------------------
$sp        = $null     # serial backend
$bleProc   = $null     # BLE helper process
$bleQueue  = $null     # telemetry lines received from the BLE helper
$bleEvents = @()       # registered event subscriptions (cleaned up at exit)
$linkName  = if ($Ble) { "BLE $BleMac" } else { $Port }

if ($Ble) {
    if (-not $BleExe) { $BleExe = Join-Path $root 'ble-bridge.exe' }
    if (-not (Test-Path $BleExe)) {
        Write-Host "ERROR: BLE helper not found at $BleExe. Build it with .\ble.ps1 -Rebuild first."
        return
    }
    Write-Host "Connecting over BLE to $BleMac via $([System.IO.Path]::GetFileName($BleExe)) ... (Ctrl+C to stop)"

    $bleQueue = New-Object System.Collections.Concurrent.ConcurrentQueue[string]

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $BleExe
    $psi.Arguments              = "tunnel $BleMac $BleService $BleChar $BleChar"
    $psi.UseShellExecute        = $false
    $psi.RedirectStandardInput  = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.CreateNoWindow         = $true

    $bleProc = New-Object System.Diagnostics.Process
    $bleProc.StartInfo = $psi
    $bleProc.EnableRaisingEvents = $true

    # Event-driven, line-buffered reads fill the queue (no raw threads, which
    # cannot reliably see script-scope vars under PowerShell 5.1).
    $onOut = Register-ObjectEvent -InputObject $bleProc -EventName OutputDataReceived -MessageData $bleQueue -Action {
        if ($EventArgs.Data) { $Event.MessageData.Enqueue($EventArgs.Data) }
    }
    $onErr = Register-ObjectEvent -InputObject $bleProc -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { Write-Host ("   [ble] {0}" -f $EventArgs.Data) }
    }
    $bleEvents = @($onOut, $onErr)

    try {
        [void]$bleProc.Start()
        $bleProc.BeginOutputReadLine()
        $bleProc.BeginErrorReadLine()
    } catch {
        Write-Host "ERROR launching BLE helper: $($_.Exception.Message)"
        return
    }

    $sendLine = {
        param($text)
        if ($bleProc -and -not $bleProc.HasExited) { $bleProc.StandardInput.WriteLine($text) }
    }.GetNewClosure()

    $readLine = {
        $item = $null
        if ($bleQueue.TryDequeue([ref]$item)) { return $item }
        Start-Sleep -Milliseconds 10   # avoid a busy spin when idle
        return $null
    }.GetNewClosure()
}
else {
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

    $sendLine = { param($text) $sp.WriteLine($text) }.GetNewClosure()
    $readLine = {
        try { return $sp.ReadLine() } catch { return $null }   # timeout -> $null
    }.GetNewClosure()
}

$lastWrite   = [DateTime]::MinValue
$writeEveryMs = 200
$numeric = 't','encL','encR','spdL','spdR','ax','ay','az','gx','gy','gz','angle','pwmL','pwmR','stby','mpu','bal','tgt','kp','ki','kd','trim','knl','ksmc','smc','vbat'
$lastCmd = ''

# Only these command shapes are accepted from the dashboard (browser) endpoint.
# Keeps the serial link from being fed arbitrary input by a web page.
#   F [pwm] | S | M l r | B 0|1 | C | D fwd turn | K kp kd | V kp ki | T kp kd | N kn ks l p | Z trim
$numTok = '-?\d+(\.\d+)?'
$cmdPattern = "^(F(\s+\d{1,3})?|S|M\s+-?\d{1,3}(\s+-?\d{1,3})?|B\s+[01]|C|D\s+$numTok\s+$numTok|K\s+$numTok\s+$numTok|V\s+$numTok\s+$numTok|T\s+$numTok\s+$numTok|N\s+$numTok(\s+$numTok){0,3}|Z\s+$numTok)$"

# --- optional local command endpoint so the dashboard can drive the robot ----
# Binds to 127.0.0.1 only (no admin URL ACL needed, not reachable off-box).
$listener = $null
$ctxTask  = $null
try {
    $listener = New-Object System.Net.HttpListener
    $listener.Prefixes.Add("http://127.0.0.1:$HttpPort/")
    $listener.Start()
    Write-Host "Command endpoint: http://127.0.0.1:$HttpPort/cmd (dashboard buttons)"
} catch {
    Write-Host "WARN: command endpoint disabled ($($_.Exception.Message)). Buttons will be offline."
    $listener = $null
}

Write-Host "Streaming telemetry -> $jsonOut. Queue commands by writing command.txt."
try {
    while ($true) {
        # --- 0a. BLE link health: if the tunnel helper exits (robot powered off
        # or out of range) stop the whole bridge so the watcher can reconnect on
        # the next power-on. Without this the loop would spin forever on a dead
        # link and auto-launch would never notice the bridge was stale.
        if ($Ble -and $bleProc -and $bleProc.HasExited) {
            Write-Host "BLE tunnel ended (robot off / out of range). Stopping bridge so it can reconnect."
            break
        }

        # --- 0. handle a pending dashboard button command (HTTP) ---
        if ($listener) {
            if (-not $ctxTask) { $ctxTask = $listener.GetContextAsync() }
            if ($ctxTask.IsCompleted) {
                $ctx = $ctxTask.Result
                $ctxTask = $null
                $req = $ctx.Request
                $res = $ctx.Response
                $res.Headers.Add('Access-Control-Allow-Origin', '*')
                $res.Headers.Add('Access-Control-Allow-Methods', 'POST, OPTIONS')
                $res.Headers.Add('Access-Control-Allow-Headers', 'Content-Type')
                $res.Headers.Add('Access-Control-Allow-Private-Network', 'true')
                try {
                    if ($req.HttpMethod -eq 'OPTIONS') {
                        $res.StatusCode = 204
                    } else {
                        $reader = New-Object System.IO.StreamReader($req.InputStream, $req.ContentEncoding)
                        $cmdIn  = $reader.ReadToEnd().Trim()
                        $reader.Close()
                        if ($cmdIn -match $cmdPattern) {
                            & $sendLine $cmdIn
                            $lastCmd = $cmdIn
                            Write-Host ("-> sent (http): {0}" -f $cmdIn)
                            $msg = "OK $cmdIn"
                            $res.StatusCode = 200
                        } else {
                            $msg = "REJECTED"
                            $res.StatusCode = 400
                        }
                        $buf = [System.Text.Encoding]::UTF8.GetBytes($msg)
                        $res.OutputStream.Write($buf, 0, $buf.Length)
                    }
                } catch {
                    # ignore a malformed request; keep streaming telemetry
                } finally {
                    $res.Close()
                }
            }
        }

        # --- 1. forward any new command from command.txt to the robot ---
        if (Test-Path $cmdFile) {
            $cmd = (Get-Content -Path $cmdFile -Raw -ErrorAction SilentlyContinue)
            if ($null -ne $cmd) {
                $cmd = $cmd.Trim()
                if ($cmd.Length -gt 0 -and $cmd -ne $lastCmd) {
                    & $sendLine $cmd
                    $lastCmd = $cmd
                    Write-Host ("-> sent: {0}" -f $cmd)
                }
            }
        }

        # --- 2. read a telemetry line (non-blocking-ish via short timeout) ---
        $line = & $readLine
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
        $data['Port']     = if ($Ble) { "BLE:$BleMac" } else { $Port }
        $data['Baud']     = if ($Ble) { 0 } else { $Baud }
        $data['Link']     = if ($Ble) { 'bluetooth' } else { 'usb' }
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
    if ($listener) { try { $listener.Stop(); $listener.Close() } catch {} }
    if ($sp -and $sp.IsOpen) { $sp.Close() }
    foreach ($ev in $bleEvents) { if ($ev) { try { Unregister-Event -SourceIdentifier $ev.Name -ErrorAction SilentlyContinue } catch {} } }
    if ($bleProc) {
        try { if (-not $bleProc.HasExited) { $bleProc.StandardInput.WriteLine('S') } } catch {}
        try { if (-not $bleProc.HasExited) { $bleProc.Kill() } } catch {}
        try { $bleProc.Dispose() } catch {}
    }
    Write-Host "Link to $linkName closed. Bridge stopped."
}
