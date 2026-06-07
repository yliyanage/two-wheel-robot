# probe-send.ps1  -  send ONE command to the pin-probe firmware and capture output.
# Opening the port resets the board (DTR), so we wait for boot, then send.
#
# Usage:
#   .\probe-send.ps1 "T 5 7 1 45 300 3"     # pulse PWM5/dir7=HIGH, pwm45, 300ms, STBY=pin3
#   .\probe-send.ps1 "S"                      # emergency stop
param(
    [Parameter(Mandatory=$true)][string]$Cmd,
    [string]$Port = 'COM3',
    [int]$Baud = 9600
)
$ErrorActionPreference = 'Stop'
$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud)
$sp.NewLine = "`n"; $sp.ReadTimeout = 500; $sp.DtrEnable = $true; $sp.RtsEnable = $true
try { $sp.Open() } catch { Write-Host "ERR open ${Port}: $($_.Exception.Message)"; return }
Start-Sleep -Milliseconds 1800          # let the board finish resetting/booting
$sp.DiscardInBuffer()
$sp.WriteLine($Cmd)                       # send the command
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(2.5)
while ((Get-Date) -lt $deadline) {
    try { [void]$sb.Append($sp.ReadExisting()) } catch {}
    Start-Sleep -Milliseconds 80
}
$sp.Close()
$out = ($sb.ToString() -replace '[^\x20-\x7E\r\n]', '.')
Write-Host "SENT: $Cmd"
Write-Host "----- RESPONSE -----"
Write-Host $out
