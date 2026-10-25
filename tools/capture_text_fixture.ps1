# Records a text test scene: brings a window forward (by title), then saves a
# screenshot and the UI Automation text candidates around a point in it.
#   powershell -ExecutionPolicy Bypass -File tools\capture_text_fixture.ps1 -Name explorer -Title "Documents" [-FX 0.3 -FY 0.4]
# FX/FY place the point as a fraction of the window (default: its centre).
# -NoActivate records whatever is on screen (for the Start menu).
param([Parameter(Mandatory)][string]$Name, [string]$Title, [double]$FX = 0.5, [double]$FY = 0.5,
      [int]$X = -1, [int]$Y = -1, [switch]$NoActivate)
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\Magnifier.exe"
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Fx {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
"@
[Fx]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
if (-not $NoActivate) {
    $shell = New-Object -ComObject WScript.Shell
    if (-not $shell.AppActivate($Title)) { throw "No window titled '$Title'" }
    Start-Sleep -Milliseconds 1500
}
if ($X -lt 0) {
    $r = New-Object Fx+RECT
    [Fx]::GetWindowRect([Fx]::GetForegroundWindow(), [ref]$r) | Out-Null
    $X = [int]($r.l + ($r.r - $r.l) * $FX); $Y = [int]($r.t + ($r.b - $r.t) * $FY)
}
$dir = Join-Path $root "tests\text\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
$p = Start-Process $exe -ArgumentList "--capture-fixture `"$dir`" $X $Y" -PassThru -Wait
"$Name at $X,$Y -> exit $($p.ExitCode)"
