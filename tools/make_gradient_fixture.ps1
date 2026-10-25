# Writes tests\text\gradient-synthetic: text on smooth gradients (should be
# redrawn, with a matching gradient behind it) and on a noisy texture (should
# be left alone). No app text, so only OCR finds the words.
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "tests\text\gradient-synthetic"
New-Item -ItemType Directory -Force $dir | Out-Null
$w = 1146; $h = 480
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::White)
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
$font = New-Object System.Drawing.Font("Segoe UI", 11.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point)
$bold = New-Object System.Drawing.Font("Segoe UI", 11.5, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Point)
function Grad($x, $y, $ww, $hh, $c1, $c2, $mode) {
    $r = New-Object System.Drawing.Rectangle $x, $y, $ww, $hh
    $b = New-Object System.Drawing.Drawing2D.LinearGradientBrush($r, $c1, $c2, $mode)
    $g.FillRectangle($b, $r); $b.Dispose()
}
$C = [System.Drawing.Color]
# 1. Vertical grey gradient panel.
Grad 20 20 520 90 ($C::FromArgb(240,240,240)) ($C::FromArgb(206,206,206)) ([System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
$g.DrawString("Settings panel with a gentle gradient", $font, [System.Drawing.Brushes]::Black, 34, 48)
# 2. A window shadow fading out to the right.
Grad 600 20 520 90 ($C::FromArgb(214,214,214)) ($C::FromArgb(255,255,255)) ([System.Drawing.Drawing2D.LinearGradientMode]::Horizontal)
$g.DrawString("Headline beside a window shadow", $font, [System.Drawing.Brushes]::Black, 614, 48)
# 3. White text on a blue gradient button.
Grad 20 150 520 90 ($C::FromArgb(0,95,184)) ($C::FromArgb(0,120,212)) ([System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
$g.DrawString("Accept and continue", $bold, [System.Drawing.Brushes]::White, 34, 178)
# 4. Diagonal tint.
Grad 600 150 520 90 ($C::FromArgb(255,244,228)) ($C::FromArgb(236,224,250)) ([System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)
$g.DrawString("Tinted card with a diagonal blend", $font, [System.Drawing.Brushes]::Black, 614, 178)
# 5. Noisy texture: must not be redrawn.
$rnd = New-Object System.Random 7
for ($y = 280; $y -lt 370; $y++) { for ($x = 20; $x -lt 540; $x++) { $v = 200 + $rnd.Next(40); $bmp.SetPixel($x, $y, $C::FromArgb($v, $v - 10, $v - 20)) } }
$g.DrawString("Caption over a textured photo", $font, [System.Drawing.Brushes]::Black, 34, 308)
# 6. Plain white control.
$g.DrawString("Plain text on white for comparison", $font, [System.Drawing.Brushes]::Black, 614, 308)
$g.Dispose()
$bmp.Save((Join-Path $dir "screen.png")); $bmp.Dispose()
[IO.File]::WriteAllText((Join-Path $dir "runs.tsv"), "")
[IO.File]::WriteAllText((Join-Path $dir "meta.txt"), "left=0`ntop=0`nview=0.00 0.00 1146.00 480.00`nfocus=573 240`ndpi=96`n")
"wrote $dir"
