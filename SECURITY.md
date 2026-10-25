# Security notes

What Magnifier loads, what it can reach, what it changes on the PC, and how it is built. These notes come from an audit of the source and of the built `Magnifier.exe` (its import table and load configuration, read with `dumpbin`).

## Dependencies

Magnifier has no third-party code and ships as one executable. It needs nothing installed beyond Windows 10 version 2004 or later.

- **No redistributables.** The C and C++ runtimes are linked in statically, so it does not need the Visual C++ Redistributable.
- **No runtime shader compiler.** Shaders are compiled with `fxc` at build time and linked into the exe as bytecode. It does not load `d3dcompiler_47.dll` or keep a shader cache on disk.
- **Every DLL it imports is part of Windows:**

| DLL | Used for |
|---|---|
| `d3d11`, `dxgi`, `dcomp`, `dwmapi` | Drawing the magnified view and pacing it to the display |
| `d2d1`, `dwrite` | Drawing text, icons and on-screen messages |
| `user32`, `gdi32`, `shell32`, `shlwapi`, `oleaut32`, `ole32` | Windows, tray icon, hotkeys, shell icons, COM |
| `magnification` | Hiding the pointer (`MagShowSystemCursor`, UIAccess build only) |
| `oleacc` | Text cursor position from apps that report it through Microsoft Active Accessibility (Chromium, many editors). This is the documented way magnifiers track the caret. |
| `wtsapi32` | Notice of Remote Desktop reconnects and unlocking, so it can rebuild the display |
| `shcore` | Monitor DPI (`GetDpiForMonitor`) |
| `windowscodecs` | PNG reading and writing (snapshots, test images) |
| WinRT (`Windows.Graphics.Capture`, `Windows.Media.Ocr`) | Screen capture and on-device text recognition |
| `api-ms-win-*` | Windows API sets that resolve to the above |

The exe tells Windows to load the DLLs it imports from System32 only (`/DEPENDENTLOADFLAG:0x800`). So a DLL with the same name placed beside the exe, or in the current folder, is never loaded.

## Build hardening

| Setting | Effect |
|---|---|
| `/DYNAMICBASE`, `/HIGHENTROPYVA` | Full 64-bit address space layout randomisation |
| `/NXCOMPAT` | Data Execution Prevention |
| `/guard:cf` | Control Flow Guard on indirect calls |
| `/CETCOMPAT`, `/guard:ehcont` | Compatible with hardware shadow stacks (Intel CET / AMD shadow stack), with exception-handling continuation metadata |
| `/sdl`, `/GS` | Extra compile-time checks and stack buffer overrun detection |
| `/W4 /permissive-` | Strict warnings and standards conformance; the build has no warnings |

## Privileges

- The manifest asks for `asInvoker`: Magnifier runs as the signed-in user, never elevated.
- The **UIAccess build** also asks for UIAccess. This lets its window sit above the Start menu and shell flyouts, as Windows' own Magnifier does. Windows grants it only to an exe that is signed by a trusted certificate and installed in a protected folder (`C:\Program Files\Magnifier`, which only administrators can change).
- `tools\install_uiaccess.ps1` signs the copy **after** it is in Program Files, so the file cannot be swapped between signing and installing. It creates a new self-signed code-signing certificate for each install, trusts it in Local Machine **Trusted Root Certification Authorities** and **Trusted Publishers**, signs, then **deletes the private key**. Nothing on the PC can sign anything else with the trusted certificate afterwards. Certificates from earlier installs are removed first. The script does not build anything, so no build tools run elevated. `tools\uninstall_uiaccess.ps1` removes the installed files, the shortcuts and the certificate.
- The UIAccess build contains none of the test and tuning modes. Its command line accepts only `--settings` (open the settings window) and `--watchdog <pid>` (the pointer watchdog, below).

## What it reads

- **The screen**, through Windows Graphics Capture, for the monitor being magnified. Frames stay in memory and are never written to disk, except when you press the snapshot hotkey.
- **Text and control names on screen**, through UI Automation (the accessibility interface screen readers use) and Windows OCR, which runs on the PC. They are used only to redraw that text sharply, are kept in memory only while on screen, and are never logged or saved.
- **Icons** for files and apps on screen, from the Windows shell. It asks for icons only, never thumbnails, so thumbnail handlers are not loaded. As in Explorer, an app that installs its own icon handler for its file types can have that handler loaded to supply the icon. Turn icons off (`[Icons] enabled=false`, or the settings window) to avoid this.
- **Pointer images**, from the user's pointer scheme (`HKCU\Control Panel\Cursors`).

It makes **no network connections**.

## What it writes

- `%APPDATA%\Magnifier\settings.ini`: settings.
- `%APPDATA%\Magnifier\log.txt`: what it did and how long it took, never screen text or file names. It starts afresh at each start and stops growing at about 2 MB.
- `Pictures\Magnifier\*.png`: only when you press the snapshot hotkey.
- A shortcut in your Startup folder: only if you tick *Start Magnifier when I sign in*.

## What it changes while it runs

- **Pointer.** The real pointer must not show at its unmagnified position, so the standard Windows pointers are swapped for transparent ones (`SetSystemCursor`). In the UIAccess build `MagShowSystemCursor` also hides it. The user's own pointers are restored on exit, by the crash handler, at the next start, and by a small **watchdog** process. The watchdog is the same exe started with `--watchdog <pid>`; it waits for the magnifier to exit in any way, then restores the pointers.
- **Hotkeys** (`RegisterHotKey`) for its own shortcuts.
- **A low-level mouse hook** that only acts on the mouse wheel. It swallows the wheel while the zoom keys are held (Ctrl+Alt by default), and on any other wheel turn it notes that text has scrolled. It never reads or changes any other input.
- **WinEvent hooks** (out of process) that report focus, caret and window changes.

## Input it accepts

- `settings.ini` is read with fixed-size buffers. Numbers are range-checked, and values that are not numbers (including `nan` and `inf`) fall back to defaults. Colours must be exactly six hex digits. Hotkeys are parsed from a fixed key list.
- Its hidden control window accepts one message from other processes (`ChangeWindowMessageFilterEx`): "open the settings window", sent when it is started a second time with `--settings`.

## Reporting a problem

Report security problems privately to the repository owner rather than in a public issue.
