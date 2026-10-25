# Renders one fixture with the smooth filter under several values of one
# [Rendering] setting (plus bicubic for reference) and stacks a crop of each.
#   tools/render_settings_sweep.ps1 -Key edge_roundness -Values 0,0.5,1 -Fixture tests\fixtures\user1.png
# Restores the settings file afterwards.
param([string]$Exe = "build\Magnifier.exe", [string]$Fixture = "tests\fixtures\text.png",
      [string]$Key = "edge_roundness", [string[]]$Values = @("0", "0.5", "1"), [double]$Zoom = 4,
      [int]$X = 0, [int]$Y = 0, [int]$W = 120, [int]$H = 45, [string]$Out = "build\out\sweep.png")
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$ini = Join-Path $env:APPDATA "Magnifier\settings.ini"
$orig = Get-Content $ini -Raw
$outPath = Join-Path $root $Out
$cw = [int]($W * $Zoom); $ch = [int]($H * $Zoom)
$runs = @(@{ label = "bicubic"; filter = "bicubic"; value = $null }) + ($Values | ForEach-Object { @{ label = "$Key=$_"; filter = "smooth"; value = $_ } })
$sheet = [System.Drawing.Bitmap]::new($cw, ($ch + 6) * $runs.Count)
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::Red)
$i = 0
try {
    foreach ($r in $runs) {
        $text = $orig
        if ($r.value -ne $null) {
            if ($text -match "(?m)^$Key=") { $text = $text -replace "(?m)^$Key=.*$", "$Key=$($r.value)" }
            else { $text = $text -replace "(?m)^\[Rendering\]\s*$", "[Rendering]`r`n$Key=$($r.value)" }
        }
        Set-Content $ini $text -NoNewline
        $one = Join-Path $root ("build\out\sweep_{0}.png" -f [guid]::NewGuid().ToString("N").Substring(0, 8))
        $p = Start-Process (Join-Path $root $Exe) -ArgumentList "--render", (Join-Path $root $Fixture), $one, $Zoom, $r.filter -PassThru -Wait
        $img = [System.Drawing.Image]::FromFile($one)
        $g.DrawImage($img, [System.Drawing.Rectangle]::new(0, $i * ($ch + 6), $cw, $ch),
                     [System.Drawing.Rectangle]::new([int]($X * $Zoom), [int]($Y * $Zoom), $cw, $ch), 'Pixel')
        $img.Dispose(); Remove-Item $one; $i++
    }
} finally {
    Set-Content $ini $orig -NoNewline
}
$g.Dispose(); $sheet.Save($outPath); $sheet.Dispose()
"wrote $outPath (rows: $(($runs | ForEach-Object label) -join ', '))"
