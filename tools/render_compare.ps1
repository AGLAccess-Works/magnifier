# Renders tests/fixtures/text.png through the real shaders with several
# filters and stacks a crop of each, for side-by-side comparison.
#   tools/render_compare.ps1 -Exe build\Magnifier.exe -Zoom 3 -Filters bicubic,smooth
param([string]$Exe = "build\Magnifier.exe", [double]$Zoom = 3, [string[]]$Filters = @("bicubic", "smooth"),
      [int]$X = 0, [int]$Y = 0, [int]$W = 220, [int]$H = 70, [string]$Out = "build\out\compare.png",
      [string]$Fixture = "tests\fixtures\text.png")
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exePath = Join-Path $root $Exe; $fix = Join-Path $root $Fixture; $outPath = Join-Path $root $Out
New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
$cw = [int]($W * $Zoom); $ch = [int]($H * $Zoom)
$sheet = [System.Drawing.Bitmap]::new($cw, ($ch + 6) * $Filters.Count)
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::Red)
$i = 0
foreach ($f in $Filters) {
    $one = Join-Path (Split-Path $outPath) ("render_{0}_{1}.png" -f $f, [guid]::NewGuid().ToString("N").Substring(0, 8))
    $p = Start-Process $exePath -ArgumentList "--render", $fix, $one, $Zoom, $f -PassThru -Wait
    if ($p.ExitCode -ne 0) { "render $f failed: $($p.ExitCode)"; continue }
    $img = [System.Drawing.Image]::FromFile($one)
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle(0, ($i * ($ch + 6)), $cw, $ch)),
                 (New-Object System.Drawing.Rectangle([int]($X * $Zoom), [int]($Y * $Zoom), $cw, $ch)), 'Pixel')
    $img.Dispose(); Remove-Item $one; $i++
}
$g.Dispose(); $sheet.Save($outPath); $sheet.Dispose()
"wrote $outPath"
