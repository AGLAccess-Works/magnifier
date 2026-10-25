# Magnifier

**An AGL Access Works project by Alex Liddell.** Built to support low vision access on Windows.

[![Windows build](https://github.com/AGLAccess-Works/magnifier/actions/workflows/windows.yml/badge.svg)](https://github.com/AGLAccess-Works/magnifier/actions/workflows/windows.yml)

[Start the engineering review](REVIEW.md) · [Credits](CREDITS.md) · [Build checks](https://github.com/AGLAccess-Works/magnifier/actions) · [Report an issue](https://github.com/AGLAccess-Works/magnifier/issues)

> Source review repository. No installable release has been published here. Read the build instructions and verification limits before evaluating the app.

A full-screen magnifier for Windows 10 (version 2004 or later) and Windows 11. It magnifies the monitor the pointer is on from 1× to 16×. Text is redrawn sharply from its font, and icons from Windows' own large copies, so they stay crisp at high zoom instead of turning into enlarged pixels.

It is a single executable written in C++ against public Windows APIs only (Direct3D 11, Direct2D, DirectWrite, Windows Graphics Capture, UI Automation, Windows OCR). It uses no third-party libraries and needs nothing installed beyond Windows. See [SECURITY.md](SECURITY.md) for what it loads, what it can access and how it is built.

## Low vision access

Magnifier is part of my work at **AGL Access Works** to make everyday Windows tasks easier for people with low vision. It brings together magnification, sharper text and icons, pointer enlargement, focus tracking and adjustable colours so people can adapt the view to their reading needs.

- **Reading:** zoom from 1× to 16×, with sharp text redraw where it matches the screen.
- **Finding your place:** follow the pointer, text cursor and keyboard focus.
- **Visual comfort:** choose inversion, greyscale or two-colour display, then adjust brightness and contrast.
- **Using the controls:** use keyboard shortcuts or the settings window to change the view.

[Relay](https://github.com/AGLAccess-Works/relay-source) is my related Windows controller access project. Its [visual access guide](https://github.com/AGLAccess-Works/relay-source/blob/main/docs/VISUAL-ACCESS.md) covers large text, contrast, focus and simplified controls. Relay's current Magnifier shortcuts operate Windows' built-in Magnifier; this standalone app is a separate project.

These features still need hands-on evaluation with people with low vision, including mixed display scaling and everyday reading tasks. See [verification and remaining checks](REVIEW.md).

## Features

- **Full-screen magnification** of the monitor the pointer is on, 1× to 16×, with fractional steps (1.25, 1.5, 1.75 …). Zoom changes glide smoothly around the pointer.
- **Windows' own pointer.** The real pointer is hidden and drawn at its magnified position, so whatever is under it is what you click. It is Windows' pointer exactly as you have set it in **Settings > Accessibility > Mouse pointer and touch** (style, size and colour), loaded sharp at the magnified size. Changes there show up straight away. Your pointers are always restored on exit, and a small watchdog process restores them even if the magnifier crashes or is ended.
- **Smooth scaling.** The default `smooth` filter finds which pixels are text ink, gives each word one consistent colour and stroke weight, and draws clean, curved edges. ClearType colour fringes come out as clean text. Pictures get edge-aware resampling, and icons and line art keep crisp edges. `nearest`, `bilinear`, `bicubic` and `lanczos` are also available.
- **Sharp text.** Text in view is drawn again from its font at full output size. It comes from three sources:
  - **Documents** that apps report through UI Automation (browsers and Electron apps, Word, Notepad and other editors).
  - **Control labels** in every window in view (buttons, menus, list items, tabs, links, title bars), placed where the pixels show the words.
  - **Text in pictures** read by Windows OCR, used only where the redraw matches the pixels.

  Each run is drawn only after a redraw of it has been checked against the screen pixels, and it is re-checked on every new frame. Anything that does not match (a hover highlight, an animation, wrong text) falls back to the scaling filter. The line with an active text cursor is left to the filter, so the app's own blinking caret stays visible.
- **Sharp icons** for files and folders in Explorer and on the desktop, and for apps in the Start menu and on the taskbar, drawn from Windows' 256-pixel copies.
- **Following** the mouse, the text cursor and keyboard focus. After you type, moving the mouse takes over again.
- **Colour modes:** normal, inverted, inverted brightness (keeps hue), greyscale, inverted greyscale and two colours (yellow on black by default), plus brightness and contrast.
- **Settings window, hotkeys and tray icon.** Settings are saved in `%APPDATA%\Magnifier\settings.ini`.
- **Robust:** it recovers from a lost graphics device, display changes, Remote Desktop reconnects and the lock screen.

## Hotkeys

| Keys | Action |
|---|---|
| Ctrl+Alt+= (or +), Ctrl+Alt+Up, Ctrl+Alt+Num+ | Zoom in |
| Ctrl+Alt+-, Ctrl+Alt+Down, Ctrl+Alt+Num- | Zoom out |
| Ctrl+Alt + mouse wheel | Zoom in or out |
| Ctrl+Alt+M | Magnifier on/off |
| Ctrl+Alt+I | Invert colours on/off |
| Ctrl+Alt+C | Next colour mode |
| Ctrl+Alt+F | Next scaling filter |
| Ctrl+Alt+T | Sharp text on/off |
| Ctrl+Alt+O | Settings window |
| Ctrl+Alt+S | Save what the magnifier shows to Pictures\Magnifier (ordinary screenshots show the unmagnified screen) |
| Ctrl+Alt+Shift+Q | Quit |

Double-click the tray icon for the settings window. Right-click it for on/off, invert, the settings file, reload and exit.

## Settings

The settings window covers everyday options. Everything is in `%APPDATA%\Magnifier\settings.ini`, written on first run. Edit it, then choose **Reload settings** from the tray menu.

- `[Magnifier] enabled`, `zoom`
- `[Rendering] filter`, `edge_strength` (0–1, how strongly `smooth` crisps edges), `edge_width` (anti-aliasing width in screen pixels), `text_boldness` (0–1, how much faint strokes are filled in), `edge_roundness` (0–1, sharper corners or rounder curves), `graphics_sharpness` (0–1, crisp edges in icons and graphics)
- `[Colour] mode`, `foreground`, `background` (as `#RRGGBB`), `brightness`, `contrast`
- `[Tracking] mouse`, `caret`, `focus`, `mouse_margin`, `caret_margin` (fraction of the view, 0–0.5), `pan_time` (seconds)
- `[Text] enabled`, `font` (used when the app's font is not installed), `use_app_font` (false always uses `font`, for example a high-legibility font), `min_zoom` (text is left to the filter below this zoom level), `ocr`
- `[Icons] enabled`
- `[Pointer] max_size` (the pointer grows with the zoom up to this many times its size)
- `[Hotkeys]` a comma-separated list per action, for example `ZoomIn=Ctrl+Alt+Equals, Ctrl+Alt+Up`, plus `WheelZoom=Ctrl+Alt` (the keys to hold while turning the wheel)

Values outside their range are clamped and unreadable values fall back to the default. A log of what the magnifier did (never the text it read) is written to `%APPDATA%\Magnifier\log.txt`.

## Building

You need the Visual Studio 2022 Build Tools with the C++ workload, which includes CMake, Ninja and the Windows SDK.

```
build.cmd           Release build into build\, then runs the unit tests
build.cmd Debug     Debug build into build-debug\
build.cmd UIAccess  Release build that asks for UIAccess, into build-uiaccess\
```

Shaders are compiled with `fxc` at build time and linked into the exe, and the C++ runtime is linked in statically. The build turns on Control Flow Guard, CET shadow-stack compatibility and the usual ASLR and DEP settings.

## Installing with UIAccess

Windows only lets a program cover the Start menu, taskbar flyouts and notification centre if it has UIAccess, as Windows' own Magnifier does. Windows grants UIAccess only to a program that asks for it in its manifest, is signed by a certificate the PC trusts, and is installed in a protected folder such as Program Files.

1. From an ordinary command prompt: `build.cmd UIAccess`
2. From an elevated PowerShell (right-click, **Run as administrator**) in the repository folder:

   ```
   powershell -ExecutionPolicy Bypass -File tools\install_uiaccess.ps1
   ```

The script explains each step and asks before it changes anything. It copies the exe to `C:\Program Files\Magnifier\` and adds Start menu shortcuts (*Magnifier (UIAccess)* and *Magnifier settings*). Then it creates a new self-signed code-signing certificate, trusts it on this PC, signs the installed copy and deletes the certificate's private key. Because the key is gone, nothing on the PC can sign anything else with the trusted certificate. Each install makes a fresh certificate and removes the old ones.

Start the installed copy from the Start menu as yourself, not from the elevated window. When it works, the log says `Running with UIAccess`. To remove everything (the installed folder, the shortcuts and the certificate):

```
powershell -ExecutionPolicy Bypass -File tools\uninstall_uiaccess.ps1
```

Your settings in `%APPDATA%\Magnifier` are kept.

## Testing

`build.cmd` runs the unit tests for the viewport maths. The Release and Debug builds also have test and tuning modes. These are left out of the UIAccess build, because they read and write files named on the command line.

- `tools\text_tests.ps1` replays recorded text scenes in `tests\text` through the real text checks and compares the result with each scene's `expected.txt`. `tools\capture_text_fixture.ps1` records a new scene from a window on screen, and `tools\make_gradient_fixture.ps1` makes the synthetic one.
- **Scoring against the truth.** A scene with a `truth.tsv` (every word really on screen, with its box) is also scored. Each true word comes out *correct*, *wrong* (different letters, or punctuation only), *partial* (part of it redrawn, the rest left to the filter) or *missed*, and redrawn text over no real word counts as a *phantom*. The run ends with coverage (true words redrawn correctly), the wrong-word rate and the time each stage of the check took, and writes them to `build\out\text-tests\scores.tsv`. `tests\text\baseline-scores.tsv` keeps a baseline to compare against. `tools\make_truth_scenes.ps1` makes the `truth-*` scenes: text in eight common fonts at 100% and 200% display scaling, on plain, dark, coloured, gradient and photo backgrounds, with ClearType or greyscale smoothing, including look-alike letters (rn/m, l/I/1, O/0) in names, codes, dates and prices.
- The live magnifier logs how long text takes to turn sharp: `Text scan` (asking apps for their text), `Text check` (first and last run accepted after the scan) and `OCR` lines in `%APPDATA%\Magnifier\log.txt`.
- `Magnifier.exe --render in.png out.png 4 smooth` runs the real shaders on an image file instead of the screen. `tools\render_compare.ps1` stacks several filters side by side, and `tools\render_settings_sweep.ps1` does the same for several values of one setting. Set `MAGNIFIER_BENCH=120` to log the average frame time.
- `Magnifier.exe --snapshot out.png --exit-after 4` saves a lossless copy of exactly what was drawn, then quits.

## How it fits together

| File | Role |
|---|---|
| `src/main.cpp` | UI thread (tray, hotkeys, display and session changes) and the render loop, which decides what to follow. |
| `src/capture.*` | Windows Graphics Capture of one monitor, polled from the render thread. |
| `src/overlay.*` | Topmost, click-through output window, excluded from screen capture so it never magnifies itself. |
| `src/renderer.*` | Direct3D 11 and a DirectComposition swap chain: the scaling filter, redrawn text and icons (Direct2D), the pointer and on-screen messages. |
| `src/shaders.hlsl` | Scaling filters (including the `smooth` statistics pre-pass), colour modes and pointer drawing. |
| `src/viewport.*` | Coordinate maths: edge and centre tracking, zoom about a point, smoothing. Unit-tested in `tests/`. |
| `src/tracking.*` | WinEvent hooks feed a worker that finds the caret and focus rectangles. |
| `src/cursor.*`, `src/cursor_hide.*` | Read Windows' pointer images (including XOR pointers such as the I-beam); hide and restore the real pointer; the watchdog. |
| `src/text_source.*` | Sharp text: a scanner thread collects document text and control labels through UI Automation; a checker thread verifies them against each new frame and places them. |
| `src/ocr.*` | Windows OCR for text that exists only as pixels. |
| `src/shell_icons.*` | Large icons from the Windows shell for what is on screen. |
| `src/fonts.*`, `src/text_draw.h` | Font choice and text layout, shared by the renderer and the checks so both lay text out identically. |
| `src/readback.*` | Non-blocking CPU copies of the captured frame for the checks. |
| `src/settings.*`, `src/settings_window.*` | Settings file and the settings window. |
| `src/text_fixture.*` | Text test scenes (Release and Debug builds only). |

## Known limits

- **Sharp text needs a match.** Text is redrawn only where a redraw matches the pixels. Text over photos or textures, unusual fonts, and text that changes constantly (timers, a line being typed) stay on the scaling filter. Web apps often use fonts that are not installed in Windows, so their text is drawn in the closest installed font, placed word by word.
- **Icons** are drawn sharp for Explorer, the desktop, the Start menu and the taskbar. Icons elsewhere use the scaling filter.
- **One monitor at a time:** the monitor the pointer (or text cursor) is on.
- **Without UIAccess**, the Start menu, notification centre and some shell flyouts appear unmagnified on top, and apps' own pointers cannot be hidden. The UIAccess install fixes both.
- **Over Remote Desktop**, the screen is compressed before it reaches you, so some softness and lag come from the connection rather than the magnifier.
- **Context menus and tooltips** are covered as soon as they appear, but one frame may show them unmagnified.
