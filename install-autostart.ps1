# install-autostart.ps1
# Makes the dashboard come up automatically and go LIVE whenever you power on the
# robot - across reboots, with no windows to click. It registers a logon
# autostart that launches:
#
#     auto-launch.ps1 -Background
#
# auto-launch then watches (hidden) for the robot over Bluetooth or the USB
# cable; the instant it appears it starts drive-bridge.ps1 and opens the
# dashboard, which then shows live telemetry. Power the robot off and the bridge
# stops; power it on again and the dashboard reconnects on its own.
#
# It uses a Scheduled Task when allowed (more resilient: auto-restart on crash),
# and otherwise falls back to a Startup-folder shortcut. Both run at logon for
# the CURRENT user and need no admin rights.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1            # install / update
#   powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1 -Status    # show install state
#   powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1 -Start     # start it now too
#   powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1 -Uninstall # remove it
#
# To also see it working right now without rebooting, add -Start (or run
# .\auto-launch.ps1 yourself in a terminal).

param(
    [ValidateSet('auto','bt','usb')]
    [string]$Link = 'auto',     # how auto-launch should look for the robot
    [int]   $Baud = 9600,
    [switch]$Start,             # also start the watcher immediately after installing
    [switch]$Uninstall,         # remove the autostart and exit
    [switch]$Status             # just print whether it is installed / running
)

$ErrorActionPreference = 'Stop'
$TaskName = 'TumbllerDashboardAutoLaunch'
$root     = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $root) { $root = (Get-Location).Path }
$auto     = Join-Path $root 'auto-launch.ps1'
$log      = Join-Path $root 'auto-launch.log'
$psExe    = Join-Path $PSHOME 'powershell.exe'
$argLine  = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -Background -Link {1} -Baud {2}' -f $auto, $Link, $Baud
$startup  = [Environment]::GetFolderPath('Startup')
$lnkPath  = Join-Path $startup 'Tumbller Dashboard AutoLaunch.lnk'

function Test-TaskInstalled { [bool](Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) }

function Start-Watcher {
    # Launch the watcher right now, hidden, regardless of install method.
    if (Test-TaskInstalled) { Start-ScheduledTask -TaskName $TaskName }
    else { Start-Process -FilePath $psExe -ArgumentList $argLine -WindowStyle Hidden | Out-Null }
}

function Show-Status {
    $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($t) {
        $info = Get-ScheduledTaskInfo -TaskName $TaskName -ErrorAction SilentlyContinue
        Write-Host ("Method   : Scheduled Task '{0}'" -f $TaskName)
        Write-Host ("State    : {0}" -f $t.State)
        if ($info) {
            Write-Host ("LastRun  : {0}  (result 0x{1:X})" -f $info.LastRunTime, $info.LastTaskResult)
            Write-Host ("NextRun  : {0}" -f $info.NextRunTime)
        }
    } elseif (Test-Path $lnkPath) {
        Write-Host ("Method   : Startup-folder shortcut")
        Write-Host ("Shortcut : {0}" -f $lnkPath)
        $running = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
                   Where-Object { $_.CommandLine -match 'auto-launch\.ps1' }
        Write-Host ("Running  : {0}" -f ([bool]$running))
    } else {
        Write-Host "Not installed (no scheduled task and no Startup shortcut)."
        return
    }
    Write-Host ("Runs     : auto-launch.ps1 -Background -Link {0}" -f $Link)
    Write-Host ("Log      : {0}" -f $log)
}

# Install via Startup-folder shortcut (admin-free fallback).
function Install-StartupShortcut {
    $wsh = New-Object -ComObject WScript.Shell
    $sc  = $wsh.CreateShortcut($lnkPath)
    $sc.TargetPath       = $psExe
    $sc.Arguments        = $argLine
    $sc.WorkingDirectory = $root
    $sc.WindowStyle      = 7      # minimized; -WindowStyle Hidden keeps it invisible
    $sc.Description      = 'Watches for the Tumbller robot (USB/BLE) and auto-launches the live dashboard at logon.'
    $sc.Save()
}

if ($Status)   { Show-Status; return }

if ($Uninstall) {
    $removed = $false
    if (Test-TaskInstalled) { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false; $removed = $true }
    if (Test-Path $lnkPath) { Remove-Item $lnkPath -Force; $removed = $true }
    # Stop any watcher that is currently running.
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match 'auto-launch\.ps1' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if ($removed) { Write-Host "Auto-launch removed. The dashboard will no longer start at logon." }
    else { Write-Host "Nothing to remove - auto-launch was not installed." }
    return
}

if (-not (Test-Path $auto)) { throw "Cannot find auto-launch.ps1 at $auto" }

# Prefer a Scheduled Task (resilient: restarts on crash). If the machine policy
# blocks registering tasks (Access denied, common without admin), fall back to a
# Startup-folder shortcut, which needs no special rights.
$method = $null
try {
    $action  = New-ScheduledTaskAction -Execute $psExe -Argument $argLine -WorkingDirectory $root
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    $principal = New-ScheduledTaskPrincipal -UserId ("{0}\{1}" -f $env:USERDOMAIN, $env:USERNAME) `
                    -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet `
                    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                    -StartWhenAvailable `
                    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
                    -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings `
        -Description 'Watches for the Tumbller robot (USB/BLE) and auto-launches the live dashboard at logon.' `
        -Force -ErrorAction Stop | Out-Null
    # If a stale Startup shortcut existed, drop it so we don't double-launch.
    if (Test-Path $lnkPath) { Remove-Item $lnkPath -Force -ErrorAction SilentlyContinue }
    $method = 'task'
} catch {
    Write-Host ("Scheduled Task unavailable ({0}). Falling back to a Startup-folder shortcut." -f $_.Exception.Message.Trim())
    Install-StartupShortcut
    $method = 'shortcut'
}

if ($method -eq 'task') {
    Write-Host "Installed Scheduled Task '$TaskName' (auto-restarts if it ever crashes)."
} else {
    Write-Host "Installed Startup-folder shortcut:"
    Write-Host ("  {0}" -f $lnkPath)
}
Write-Host "It runs at every logon and brings the dashboard live when you power on the robot."
Write-Host ("Watching mode: {0}   Log: {1}" -f $Link, $log)

if ($Start) {
    Start-Watcher
    Write-Host "Started it now too - power on the robot and the dashboard will open on its own."
}

Write-Host ""
Write-Host "Manage it later:"
Write-Host "  .\install-autostart.ps1 -Status      # check state"
Write-Host "  .\install-autostart.ps1 -Uninstall   # turn off auto-launch"

