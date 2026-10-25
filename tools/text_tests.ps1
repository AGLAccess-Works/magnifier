# Replays every recorded text scene under tests\text through the real checks
# (Magnifier.exe --text-test) and compares each result with its expected.txt.
#   powershell -ExecutionPolicy Bypass -File tools\text_tests.ps1 [-Name gradient-synthetic] [-Accept]
# Results and magnified renders go to build\out\text-tests\<scene>.
# -Accept saves the current results as the new expected ones.
#
# A run is identified by its kind, text and position (to 3 px). "Lost" runs
# were shown before and are not now; "new" runs were not shown before.
# Collisions (redrawn text overlapping other redrawn text) and mixed-style
# neighbours are counted from the summary lines.
param([string]$Name = "*", [switch]$Accept)
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\Magnifier.exe"
$outRoot = Join-Path $root "build\out\text-tests"
New-Item -ItemType Directory -Force $outRoot | Out-Null

function Keys($path) {
    $k = @{}
    if (-not (Test-Path $path)) { return $k }
    foreach ($line in [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8)) {
        if ($line.StartsWith("#")) { continue }
        $f = $line.Split("`t")
        if ($f.Count -lt 3) { continue }
        $xy = $f[2].Split(",")
        $key = "{0}`t{1}`t{2},{3}" -f $f[0], $f[1], [math]::Round([double]$xy[0] / 3), [math]::Round([double]$xy[1] / 3)
        $k[$key] = $line
    }
    return $k
}
function Summary($path) {
    if (-not (Test-Path $path)) { return "(none)" }
    $s = [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8) | Where-Object { $_ -match "^# (runs|collisions|mixed|not redrawn)" }
    return ($s -replace "^# ", "") -join "; "
}

# Scenes with a truth.tsv are also scored against the words really on screen
# (see Score in src/text_fixture.cpp); totals are printed at the end, and
# written to build\out\text-tests\scores.tsv.
function Fields($line) {
    $h = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=([\d.]+)')) { $h[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
    return $h
}
$scores = New-Object System.Collections.Generic.List[object]

$failed = 0
foreach ($dir in Get-ChildItem (Join-Path $root "tests\text") -Directory -Filter $Name) {
    $out = Join-Path $outRoot $dir.Name
    $p = Start-Process $exe -ArgumentList "--text-test `"$($dir.FullName)`" `"$out`"" -PassThru -Wait -WindowStyle Hidden
    $result = Join-Path $out "result.txt"
    $expected = Join-Path $dir.FullName "expected.txt"
    if ($p.ExitCode -ne 0 -or -not (Test-Path $result)) { "{0}: FAILED to run (exit {1})" -f $dir.Name, $p.ExitCode; $failed++; continue }
    $scoreFile = Join-Path $out "score.txt"
    if (Test-Path $scoreFile) {
        $lines = [IO.File]::ReadAllLines($scoreFile, [Text.Encoding]::UTF8)
        $s = Fields (($lines | Where-Object { $_ -like "# score*" }) -join " ")
        $t = Fields (($lines | Where-Object { $_ -like "# timing*" }) -join " ")
        $scores.Add([pscustomobject]@{ scene = $dir.Name; truth = $s.Count -gt 0
            words = $s.words; correct = $s.correct; wrong = $s.wrong; punct = $s.punct; partial = $s.partial; missed = $s.missed; phantom = $s.phantom
            app_ms = $t.app_ms; ocr_read_ms = $t.ocr_read_ms; ocr_verify_ms = $t.ocr_verify_ms })
    }
    if ($Accept) { Copy-Item $result $expected -Force; "{0}: accepted  [{1}]" -f $dir.Name, (Summary $result); continue }
    $now = Keys $result; $was = Keys $expected
    $lost = @($was.Keys | Where-Object { -not $now.ContainsKey($_) })
    $new = @($now.Keys | Where-Object { -not $was.ContainsKey($_) })
    "{0}: {1} lost, {2} new" -f $dir.Name, $lost.Count, $new.Count
    "    before: " + (Summary $expected)
    "    now:    " + (Summary $result)
    foreach ($k in $lost | Sort-Object) { "    - " + $was[$k] }
    foreach ($k in $new | Sort-Object) { "    + " + $now[$k] }
    if ($lost.Count) { $failed++ }
}

if ($scores.Count) {
    $scores | Export-Csv -NoTypeInformation -Delimiter "`t" (Join-Path $outRoot "scores.tsv")
    $scored = @($scores | Where-Object truth)
    if ($scored.Count) {
        ""
        "Against the truth ({0} scenes):" -f $scored.Count
        "  {0,-26} {1,6} {2,8} {3,6} {4,6} {5,7} {6,8} {7,7}" -f "scene", "words", "correct", "wrong", "punct", "phantom", "partial", "missed"
        foreach ($s in $scored) {
            "  {0,-26} {1,6} {2,7:P0} {3,6} {4,6} {5,7} {6,8} {7,7}" -f $s.scene, $s.words, ($s.correct / [math]::Max($s.words, 1)), $s.wrong, $s.punct, $s.phantom, $s.partial, $s.missed
        }
        $sum = { param($k) ($scored | Measure-Object -Property $k -Sum).Sum }
        $words = & $sum words; $correct = & $sum correct; $wrong = (& $sum wrong) + (& $sum phantom); $drawn = $correct + $wrong + (& $sum punct) + (& $sum partial)
        "  Coverage (true words redrawn correctly): {0:P1} of {1}" -f ($correct / [math]::Max($words, 1)), $words
        "  Wrong words (wrong text or phantoms, as a share of redrawn words): {0} = {1:P2}" -f $wrong, ($wrong / [math]::Max($drawn, 1))
        "  Punctuation-only differences: {0}; partly redrawn words (rest left blurry): {1}; missed: {2}" -f (& $sum punct), (& $sum partial), (& $sum missed)
    }
    $timed = @($scores | Where-Object { $_.ocr_read_ms -ne $null })
    $avg = { param($k) ($timed | Measure-Object -Property $k -Average).Average }
    "  Check time per scene, average: app text {0:N0} ms, OCR {1:N0} ms, checking OCR words {2:N0} ms" -f (& $avg app_ms), (& $avg ocr_read_ms), (& $avg ocr_verify_ms)
}
if ($failed) { "$failed scene(s) lost runs or failed"; exit 1 }
