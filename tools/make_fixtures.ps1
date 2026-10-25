# Generates tests/fixtures/text.png: typical small screen text in the main
# anti-aliasing styles, for tuning the scaling filters with --render.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot "..\tests\fixtures\text.png"
$bmp = New-Object System.Drawing.Bitmap(420, 300)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::White)
$sample = "Hamburgefonstiv quick 0123 @&%"
$y = 4
$styles = @(
    @{ hint = 'ClearTypeGridFit'; fg = 'Black'; bg = $null },
    @{ hint = 'AntiAliasGridFit'; fg = 'Black'; bg = $null },
    @{ hint = 'AntiAliasGridFit'; fg = 'Gray'; bg = $null },
    @{ hint = 'AntiAliasGridFit'; fg = 'White'; bg = 'Black' },
    @{ hint = 'ClearTypeGridFit'; fg = 'Blue'; bg = $null }
)
foreach ($st in $styles) {
    foreach ($size in 8, 9, 11) {
        $font = New-Object System.Drawing.Font("Segoe UI", $size)
        $h = [int]($font.GetHeight($g)) + 2
        if ($st.bg) { $g.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromName($st.bg))), 0, $y, 420, $h) }
        $g.TextRenderingHint = $st.hint
        $g.DrawString($sample, $font, (New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromName($st.fg))), 4, $y)
        $y += $h
        $font.Dispose()
    }
}
# Line art: thin diagonals and a circle.
$g.SmoothingMode = 'AntiAlias'
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::Black, 1)
$g.DrawLine($pen, 300, $y - 60, 410, $y - 10)
$g.DrawEllipse($pen, 340, $y - 100, 40, 40)
$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"wrote $out"
