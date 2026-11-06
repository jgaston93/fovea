# Checks, without anyone watching, that fovea's windows cover the right
# monitors: runs `fovea --identify` and reads the colour at the centre of each
# monitor from a screenshot. Needs an unlocked desktop (a locked one
# captures the lock screen).
#
#   powershell -ExecutionPolicy Bypass -File tools\check-identify.ps1 `
#       -Exe build\debug\apps\fovea\fovea.exe -Config config\<file>.toml
#
# Prints each monitor's centre colour and the colour expected there: red,
# green, blue, yellow, magenta, cyan for channels in config order. Exits 1 if
# any monitor shows the wrong colour.
param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$Config)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

if (Get-Process LogonUI -ErrorAction SilentlyContinue) {
    Write-Output "The desktop is locked; unlock it and run again."
    exit 2
}

# Which monitor each channel is on, from fovea's own report.
$colours = @(@(200, 30, 30), @(30, 170, 60), @(40, 80, 220), @(220, 200, 40), @(190, 60, 190), @(40, 190, 200))
$out = New-TemporaryFile
$p = Start-Process -FilePath $Exe -ArgumentList '--seconds', '8', '--identify', $Config `
    -PassThru -WindowStyle Hidden -RedirectStandardOutput $out
Start-Sleep -Seconds 4

$monitors = @{}
$i = 0
foreach ($line in Get-Content $out) {
    if ($line -match '^\s+(\d+): (\d+)x(\d+) at \((-?\d+), (-?\d+)\)') {
        $monitors[[int]$Matches[1]] = @([int]$Matches[2], [int]$Matches[3], [int]$Matches[4], [int]$Matches[5])
    }
}
$failed = $false
foreach ($line in Get-Content $out) {
    if ($line -match '^\s+window "(.+)": on monitor (\d+)') {
        $m = $monitors[[int]$Matches[2]]
        $cx = $m[2] + [int]($m[0] / 2); $cy = $m[3] + [int]($m[1] / 2)
        $bmp = New-Object System.Drawing.Bitmap 1, 1
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($cx, $cy, 0, 0, (New-Object System.Drawing.Size 1, 1))
        $c = $bmp.GetPixel(0, 0); $g.Dispose(); $bmp.Dispose()
        $want = $colours[$i % $colours.Count]
        $ok = ([Math]::Abs($c.R - $want[0]) -le 8) -and ([Math]::Abs($c.G - $want[1]) -le 8) -and ([Math]::Abs($c.B - $want[2]) -le 8)
        "{0,-8} monitor {1}: rgb({2},{3},{4}), expected rgb({5},{6},{7})  {8}" -f $Matches[1], $Matches[2], $c.R, $c.G, $c.B, $want[0], $want[1], $want[2], ($(if ($ok) { 'ok' } else { 'WRONG' }))
        $failed = $failed -or -not $ok
        $i++
    }
}
$p.WaitForExit()
Remove-Item $out
if ($i -eq 0) { Write-Output "fovea reported no windows (exit $($p.ExitCode))"; exit 1 }
if ($failed) { exit 1 }
