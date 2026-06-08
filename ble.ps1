# ble.ps1 - build (if needed) and run the BLE helper (ble-bridge.cs).
# Compiles ble-bridge.cs to ble-bridge.exe with the .NET Framework csc.exe,
# referencing the system WinMD files so WinRT BLE APIs are available. Then runs
# the helper with whatever arguments you pass through.
#
#   .\ble.ps1 scan 14
#   .\ble.ps1 enum <MAC>
#   .\ble.ps1 tunnel <MAC> <serviceUuid> <notifyUuid> <writeUuid>
#
# Use -Rebuild to force recompilation.

[CmdletBinding()]
param(
    [switch]$Rebuild,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $root) { $root = (Get-Location).Path }
$src = Join-Path $root 'ble-bridge.cs'
$exe = Join-Path $root 'ble-bridge.exe'

$rt  = [System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
$csc = Join-Path $rt 'csc.exe'
$wm  = Join-Path $env:WINDIR 'System32\WinMetadata'

function Build {
    if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc" }
    # No 'await' is used in ble-bridge.cs (WinRT IAsyncOperation is driven via its
    # Completed callback), so we only need the WinMD type metadata + core BCL.
    $refs = @(
        (Join-Path $wm 'Windows.Foundation.winmd'),
        (Join-Path $wm 'Windows.Devices.winmd'),
        (Join-Path $wm 'Windows.Storage.winmd'),
        (Join-Path $rt 'System.dll'),
        (Join-Path $rt 'System.Core.dll'),
        # WinRT event '+=' compiles to EventRegistrationToken plumbing that lives
        # in this interop assembly. (Safe to include now that the helper avoids
        # 'await', which is the part that was version-sensitive.)
        (Join-Path $rt 'System.Runtime.InteropServices.WindowsRuntime.dll')
    )
    # Referencing the WinMD pulls in System.Attribute etc. -> still need the
    # System.Runtime contract facade. No VS / ref-assembly pack here, so take it
    # from the GAC. (We do NOT need the WindowsRuntime interop facade because the
    # helper avoids 'await'.)
    $gac = Join-Path $env:WINDIR 'Microsoft.Net\assembly\GAC_MSIL'
    foreach ($fn in @('System.Runtime')) {
        $dll = Get-ChildItem (Join-Path $gac $fn) -Recurse -Filter "$fn.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($dll) { $refs += $dll.FullName } else { throw "facade not found in GAC: $fn" }
    }
    foreach ($r in $refs) { if (-not (Test-Path $r)) { throw "missing reference: $r" } }
    $refArgs = $refs | ForEach-Object { "/reference:`"$_`"" }
    $argList = @('/nologo', '/target:exe', "/out:`"$exe`"") + $refArgs + @("`"$src`"")
    Write-Host "Compiling ble-bridge.exe ..."
    $p = Start-Process -FilePath $csc -ArgumentList $argList -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $root 'csc.out.txt') -RedirectStandardError (Join-Path $root 'csc.err.txt')
    $out = (Get-Content (Join-Path $root 'csc.out.txt') -ErrorAction SilentlyContinue) -join "`n"
    $err = (Get-Content (Join-Path $root 'csc.err.txt') -ErrorAction SilentlyContinue) -join "`n"
    Remove-Item (Join-Path $root 'csc.out.txt'),(Join-Path $root 'csc.err.txt') -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0 -or -not (Test-Path $exe)) {
        throw "csc failed (exit $($p.ExitCode)):`n$out`n$err"
    }
    if ($out.Trim() -or $err.Trim()) { Write-Host ($out + $err) }
    Write-Host "Built $exe"
}

$needBuild = $Rebuild -or -not (Test-Path $exe) -or `
    ((Get-Item $src).LastWriteTime -gt (Get-Item $exe -ErrorAction SilentlyContinue).LastWriteTime)
if ($needBuild) { Build }

if ($Args -and $Args.Count -gt 0) {
    & $exe @Args
}
