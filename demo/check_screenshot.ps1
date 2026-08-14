# AYVideo_EngineDemo screenshot acceptance (V3, design.md §12).
#
# Path 1 uploads solid magenta (255,0,255). PostProcess gamma=2.2 leaves
# pure 0/255 channels unchanged, so expected encoded RGB is still
# 255,0,255. Path 2 uses mid-grey I420 (Y=128,U=V=128) → ~equal RGB.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File check_screenshot.ps1 <frame_30.tga> [<frame_60.tga> ...]
# Exit 0 = PASS (non-black + expected color ±8), else 1.
param(
    [string[]]$Files,
    [ValidateSet("1","2")]
    [string]$Path = "1"
)

function Check-Tga([string]$path, [string]$demoPath) {
    $data = [System.IO.File]::ReadAllBytes($path)
    $w = [BitConverter]::ToUInt16($data, 12)
    $h = [BitConverter]::ToUInt16($data, 14)
    $bpp = $data[16]
    $bytespp = $bpp / 8
    $n = [int]$w * [int]$h
    Write-Host ("== {0} == {1}x{2} bpp={3} path={4}" -f $path, $w, $h, $bpp, $demoPath)
    $nonblack = 0L
    $hit = 0L
    if ($demoPath -eq "2") {
        # Mid-grey I420 (Y=128) after BT.601 + PostProcess gamma ≈ 188.
        $er = 188; $eg = 188; $eb = 188; $tol = 12
    } else {
        $er = 255; $eg = 0; $eb = 255; $tol = 8
    }
    for ($i = 0; $i -lt $n; $i++) {
        $off = 18 + $i * $bytespp
        $b = $data[$off]; $g = $data[$off + 1]; $r = $data[$off + 2]
        if ($r -gt 8 -or $g -gt 8 -or $b -gt 8) { $nonblack++ }
        if ([Math]::Abs($r - $er) -le $tol -and
            [Math]::Abs($g - $eg) -le $tol -and
            [Math]::Abs($b - $eb) -le $tol) {
            $hit++
        }
    }
    $pct = 100.0 * $nonblack / $n
    $hitPct = 100.0 * $hit / $n
    Write-Host ("nonblack={0}/{1} ({2:F1}%) target_hits={3} ({4:F1}%)" -f $nonblack, $n, $pct, $hit, $hitPct)
    $ok = $nonblack -gt 10000 -and $hit -gt 5000
    Write-Host ("VERDICT: {0}" -f ($(if ($ok) { "PASS" } else { "FAIL" })))
    return $ok
}

if ($Files.Count -eq 0) {
    Write-Host "usage: check_screenshot.ps1 [-Path 1|2] <frame_30.tga> [more...]"
    exit 2
}
$allOk = $true
foreach ($f in $Files) {
    $allOk = (Check-Tga $f $Path) -and $allOk
}
if (-not $allOk) { exit 1 }
exit 0
