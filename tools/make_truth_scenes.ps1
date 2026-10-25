# Writes tests\text\truth-*: screens of text whose every word is known, for
# measuring the text redraw against the truth (tools\text_tests.ps1 scores
# them). Each scene has screen.png, meta.txt, an empty runs.tsv (no app
# reports the text, so only OCR can find it) and truth.tsv: every word drawn,
# with its box.
#
# Text is drawn with GDI ClearType, as most Windows apps draw it, or with
# greyscale smoothing; in common fonts at 100% and 200% display scaling;
# on plain, dark, coloured, gradient and busy picture backgrounds. Words
# include look-alike letters (rn/m, l/I/1, O/0) in names, codes, dates and
# prices. Same seed, same scenes.
#   powershell -ExecutionPolicy Bypass -File tools\make_truth_scenes.ps1
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$root = Split-Path $PSScriptRoot -Parent
$rand = New-Object System.Random 20261022

$prose = ("The quarterly report is ready for review before Friday. Please check the figures in section two " +
    "and add any comments to the shared folder. Our team will meet on Tuesday morning to agree the final " +
    "version, then send it to the board. Remember to update the contact list and confirm the room booking. " +
    "Modern offices often turn the corner room into a quiet space for calls. Illustrated guides explain " +
    "how to install the software, clear the cache and restart the computer when the update has finished.").Split(" ")
$ui = "Settings File Edit View Insert Format Tools Help Search Downloads Documents Pictures Recent Pinned Accept " +
    "Cancel Apply Save Share Print Open Close Reply Forward Delete Archive Calendar Inbox Drafts Sent Outbox " +
    "Notifications Accessibility Display Bluetooth Network Personalisation Updates Account Privacy Sign out"
$ui = $ui.Split(" ")
$tricky = ("modern burn corner barn turn learn Illinois Install Illustrator illegal clear clock cloud docked " +
    "olive 1001 l0g ID-0O1l INV-20419 A1B2-C3D4 R2-D2 Q3_report_v2.xlsx j.smith@example.com www.example.co.uk/help " +
    "14:05 09:30 22/10/2026 31/01/2025 $([char]0xA3)1,249.99 $([char]0xA3)19.99 37.5% 100% 0.75 rn m rnm mm nn ll II 11 OO 00 O0 0O").Split(" ")

function Pick($list) { $list[$rand.Next($list.Count)] }
function Words($kind, $n) {
    $out = @()
    for ($i = 0; $i -lt $n; $i++) {
        switch ($kind) {
            "prose" { $out += $prose[($script:proseAt++) % $prose.Count] }
            "ui" { $out += Pick $ui }
            "tricky" { $out += Pick $tricky }
            "mixed" { $r = $rand.NextDouble(); $out += $(if ($r -lt 0.5) { $prose[($script:proseAt++) % $prose.Count] } elseif ($r -lt 0.75) { Pick $ui } else { Pick $tricky }) }
        }
    }
    $out
}
$script:proseAt = 0

function Colour($hex) { [System.Drawing.ColorTranslator]::FromHtml($hex) }

# Fills a line's band. Kinds: white, grey, dark, blue, gradient, photo.
function Band($g, $kind, $x, $y, $w, $h) {
    $r = New-Object System.Drawing.Rectangle $x, $y, $w, $h
    switch ($kind) {
        "white" { $g.FillRectangle((New-Object System.Drawing.SolidBrush (Colour "#FFFFFF")), $r) }
        "grey" { $g.FillRectangle((New-Object System.Drawing.SolidBrush (Colour "#F3F3F3")), $r) }
        "dark" { $g.FillRectangle((New-Object System.Drawing.SolidBrush (Colour "#1F1F1F")), $r) }
        "blue" { $g.FillRectangle((New-Object System.Drawing.SolidBrush (Colour "#0067C0")), $r) }
        "gradient" {
            $b = New-Object System.Drawing.Drawing2D.LinearGradientBrush $r, (Colour "#FFFFFF"), (Colour "#C9DDF2"), 0.0
            $g.FillRectangle($b, $r)
        }
        "photo" {
            # A busy picture: soft overlapping blobs of colour.
            $g.FillRectangle((New-Object System.Drawing.SolidBrush (Colour "#7A8F6A")), $r)
            for ($i = 0; $i -lt 60; $i++) {
                $c = [System.Drawing.Color]::FromArgb(110, $rand.Next(40, 230), $rand.Next(40, 230), $rand.Next(40, 230))
                $d = $rand.Next([int]($h * 0.6), [int]($h * 2.5))
                $g.FillEllipse((New-Object System.Drawing.SolidBrush $c), $x + $rand.Next($w) - $d / 2, $y + $rand.Next($h) - $d / 2, $d, $d)
            }
        }
    }
}
function TextColour($kind) {
    switch ($kind) { "dark" { "#E6E6E6" } "blue" { "#FFFFFF" } "photo" { "#FFFFFF" } default { "#1A1A1A" } }
}

# One scene: $lines is a list of @{font; pt; bold; band; words; colour?}.
function Scene($name, $dpi, $smoothing, $lines) {
    $scale = $dpi / 96.0
    $W = [int](1150 * $scale); $margin = [int](12 * $scale)
    # Height: each line's height plus spacing.
    $heights = foreach ($l in $lines) { [int]([math]::Ceiling($l.pt * $dpi / 72 * 1.9)) }
    $H = ($heights | Measure-Object -Sum).Sum + 2 * $margin
    $bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $truth = New-Object System.Collections.Generic.List[string]
    $truth.Add("# word`tx`ty`tw`th")
    $y = $margin
    for ($li = 0; $li -lt $lines.Count; $li++) {
        $l = $lines[$li]; $lh = $heights[$li]
        Band $g $l.band 0 $y $W $lh
        $style = if ($l.bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular }
        $font = New-Object System.Drawing.Font $l.font, ([single]($l.pt * $dpi / 72)), $style, ([System.Drawing.GraphicsUnit]::Pixel)
        $fore = Colour $(if ($l.colour) { $l.colour } else { TextColour $l.band })
        $space = [int][math]::Ceiling($font.Size * 0.3)
        $x = $margin
        $ty = $y + [int](($lh - $font.Height) / 2)
        # (GDI's own text calls draw unsmoothed black text into an image, so
        # GDI+ draws it, with ClearType or greyscale smoothing.)
        $g.TextRenderingHint = if ($smoothing -eq "cleartype") { [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit } else { [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit }
        $fmt = [System.Drawing.StringFormat]::GenericTypographic
        foreach ($word in $l.words) {
            $m = $g.MeasureString($word, $font, 10000, $fmt)
            $size = New-Object System.Drawing.Size ([int][math]::Ceiling($m.Width)), $font.Height
            if ($x + $size.Width -gt $W - $margin) { break }
            $g.DrawString($word, $font, (New-Object System.Drawing.SolidBrush $fore), [single]$x, [single]$ty, $fmt)
            $truth.Add(("{0}`t{1}`t{2}`t{3}`t{4}" -f $word, $x, $ty, $size.Width, $font.Height))
            $x += $size.Width + $space
        }
        $y += $lh
    }
    $dir = Join-Path $root "tests\text\$name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $bmp.Save((Join-Path $dir "screen.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [IO.File]::WriteAllLines((Join-Path $dir "truth.tsv"), $truth, $utf8)
    [IO.File]::WriteAllText((Join-Path $dir "runs.tsv"), "", $utf8)
    [IO.File]::WriteAllText((Join-Path $dir "meta.txt"), ("left=0`ntop=0`nview=0.00 0.00 {0}.00 {1}.00`nfocus={2} {3}`ndpi={4}`n" -f $W, $H, [int]($W / 2), [int]($H / 2), $dpi), $utf8)
    "{0}: {1}x{2}, {3} words" -f $name, $W, $H, ($truth.Count - 1)
}

$fonts = "Segoe UI", "Calibri", "Arial", "Times New Roman", "Georgia", "Verdana", "Consolas", "Tahoma"
$sizes = 8, 9, 10, 11, 12, 14
$bands = "white", "grey", "dark", "blue", "white", "gradient", "white", "grey"
foreach ($dpi in 96, 192) {
    foreach ($f in $fonts) {
        $lines = @()
        for ($i = 0; $i -lt 8; $i++) {
            $lines += @{ font = $f; pt = $sizes[$i % $sizes.Count]; bold = ($i % 4 -eq 3); band = $bands[$i]; words = (Words "mixed" 14) }
        }
        Scene ("truth-{0}-{1}" -f ($f -replace " ", "").ToLower(), $dpi) $dpi "cleartype" $lines
    }
    # Look-alike letters, small sizes, the usual UI fonts.
    $lines = @()
    foreach ($f in "Segoe UI", "Calibri", "Arial", "Tahoma") { foreach ($pt in 8, 9, 11) { $lines += @{ font = $f; pt = $pt; bold = $false; band = "white"; words = (Words "tricky" 12) } } }
    Scene "truth-lookalikes-$dpi" $dpi "cleartype" $lines
    # Greyscale smoothing (no ClearType), as many web and modern apps draw text.
    $lines = @()
    foreach ($f in "Segoe UI", "Arial", "Georgia", "Verdana") { foreach ($pt in 9, 12) { $lines += @{ font = $f; pt = $pt; bold = $false; band = "white"; words = (Words "mixed" 12) } } }
    Scene "truth-greyscale-$dpi" $dpi "greyscale" $lines
    # Coloured text: links, warnings, success messages.
    $lines = @()
    foreach ($c in "#0B57D0", "#C42B1C", "#107C10", "#8A5A00", "#6B6B6B", "#5B2D90") { $lines += @{ font = "Segoe UI"; pt = 10; bold = $false; band = "white"; colour = $c; words = (Words "ui" 12) } }
    Scene "truth-coloured-$dpi" $dpi "cleartype" $lines
    # Text over a busy picture: should be left alone (missed), never wrong.
    $lines = @()
    foreach ($pt in 10, 12, 14, 18) { $lines += @{ font = "Segoe UI"; pt = $pt; bold = ($pt -ge 14); band = "photo"; words = (Words "prose" 10) } }
    Scene "truth-photo-$dpi" $dpi "cleartype" $lines
}
