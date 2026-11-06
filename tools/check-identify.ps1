# Checks, without anyone watching, that fovea's windows cover the right
# monitors: runs `fovea --identify` and reads the colour at the centre of each
# window's monitor from a screenshot. Needs this session's desktop unlocked
# (a locked one captures the lock screen).
#
#   powershell -ExecutionPolicy Bypass -File tools\check-identify.ps1 `
#       -Exe build\debug\apps\fovea\fovea.exe -Config config\<file>.toml
#
# The expected colours come from fovea's own output ("identify rgb(...)").
# Exit codes: 0 every monitor right; 1 a wrong colour, a missing window or a
# fovea failure; 2 the desktop is locked.
param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$Config,
      [int]$StartupTimeoutSeconds = 30)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Dpi {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
"@
# Physical pixels, like the monitor bounds fovea (SDL) reports; a DPI-unaware
# process would get scaled coordinates on monitors with display scaling.
[void][Dpi]::SetProcessDpiAwarenessContext([IntPtr]-4)  # PER_MONITOR_AWARE_V2

$session = (Get-Process -Id $PID).SessionId
if (Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $session }) {
    Write-Output "The desktop is locked; unlock it and run again."
    exit 2
}

$out = New-TemporaryFile
$err = New-TemporaryFile
$p = Start-Process -FilePath $Exe -ArgumentList '--seconds', '10', '--identify', $Config `
    -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
$null = $p.Handle  # keeps ExitCode readable after exit (a PowerShell quirk)

function Finish([int]$code, [string]$message) {
    if ($message) { Write-Output $message }
    if (-not $p.HasExited) { $p.Kill() }
    $p.WaitForExit()
    $errors = Get-Content $err
    if ($errors) { Write-Output "fovea said:"; $errors | ForEach-Object { "  $_" } }
    Remove-Item $out, $err
    exit $code
}

# Wait until fovea says every window is up and filled.
$deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
while (-not (Select-String -Path $out -SimpleMatch 'Esc or close a window to quit.' -Quiet)) {
    if ($p.HasExited) { Finish 1 "fovea exited before its windows were up (exit $($p.ExitCode))" }
    if ((Get-Date) -gt $deadline) { Finish 1 "fovea's windows were not up after $StartupTimeoutSeconds s" }
    Start-Sleep -Milliseconds 200
}
Start-Sleep -Milliseconds 500  # let the compositor show the first frame

$monitors = @{}
foreach ($line in Get-Content $out) {
    if ($line -match '^\s+(\d+): (\d+)x(\d+) at \((-?\d+), (-?\d+)\)') {
        $monitors[[int]$Matches[1]] = @([int]$Matches[2], [int]$Matches[3], [int]$Matches[4], [int]$Matches[5])
    }
}
$checked = 0
$failed = $false
foreach ($line in Get-Content $out) {
    if ($line -notmatch '^\s+window "(.+)": on monitor (-?\d+), .*identify rgb\((\d+),(\d+),(\d+)\)') { continue }
    $name = $Matches[1]; $index = [int]$Matches[2]; $want = @([int]$Matches[3], [int]$Matches[4], [int]$Matches[5])
    $checked++
    if (-not $monitors.ContainsKey($index)) {
        "{0,-8} monitor {1}: not a listed monitor  WRONG" -f $name, $index
        $failed = $true
        continue
    }
    $m = $monitors[$index]
    $cx = $m[2] + [int]($m[0] / 2); $cy = $m[3] + [int]($m[1] / 2)
    $bmp = New-Object System.Drawing.Bitmap 1, 1
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($cx, $cy, 0, 0, (New-Object System.Drawing.Size 1, 1))
    $c = $bmp.GetPixel(0, 0); $g.Dispose(); $bmp.Dispose()
    $ok = ([Math]::Abs($c.R - $want[0]) -le 8) -and ([Math]::Abs($c.G - $want[1]) -le 8) -and ([Math]::Abs($c.B - $want[2]) -le 8)
    "{0,-8} monitor {1}: rgb({2},{3},{4}), expected rgb({5},{6},{7})  {8}" -f $name, $index, $c.R, $c.G, $c.B, $want[0], $want[1], $want[2], ($(if ($ok) { 'ok' } else { 'WRONG' }))
    $failed = $failed -or -not $ok
}
if ($checked -eq 0) { Finish 1 "fovea reported no windows" }

$p.WaitForExit()
if ($p.ExitCode -ne 0) { Finish 1 "fovea exited with $($p.ExitCode)" }
Finish $(if ($failed) { 1 } else { 0 }) ""
