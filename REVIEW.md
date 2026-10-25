# Engineering review — Magnifier

**Alex Liddell · AGL Access Works**
Source review notes prepared on 25 October 2026 from `Magnifier-share.zip`.

## Low vision review focus

Check everyday reading, locating keyboard focus, changing zoom or text size, adjusting contrast and recovering after changing displays. Include people who use magnification alone and people who combine it with a screen reader. Record readability, visual searching, effort and any clipped or unreachable controls. Automated build results do not establish how well these tasks work for users.

## Start here

1. Read the [README](README.md) for features, hotkeys, build instructions and known limits.
2. Read [SECURITY.md](SECURITY.md), particularly capture, pointer restoration and UIAccess installation.
3. Build on Windows with Visual Studio 2022 C++ Build Tools, CMake, Ninja and the Windows SDK: `build.cmd`.
4. Review viewport maths in `src/viewport.*`, capture/rendering in `src/capture.*` and `src/renderer.*`, and text verification in `src/text_source.*`.

## Verification

On 25 October 2026, the supplied viewport tests were compiled with C++20 and warnings enabled on macOS. Every check passed. This checks the platform-independent geometry only.

GitHub Actions builds Release, Debug and UIAccess configurations on Windows and runs the viewport tests. The UIAccess executable is built but not signed, installed or launched. Check the [actual workflow results](https://github.com/AGLAccess-Works/magnifier/actions) for current status.

The [25 October Windows run](https://github.com/AGLAccess-Works/magnifier/actions) passed all three configurations (Release, Debug and UIAccess), including their viewport tests.

Full application compilation requires Windows/MSVC. Text replay fixtures, real screen capture, rendering quality, multi-monitor/DPI changes, pointer restoration, live accessibility use and UIAccess installation still require Windows verification. The README describes the supplied text and rendering tools; the automated build does not establish that these acceptance checks pass.

## Repository preparation

Original Git metadata was excluded from the archive import. Application logic and fixtures were retained. Added review/contribution documentation, attribution, editor defaults, signing-file exclusions and a Windows workflow. Windows executable details now identify AGL Access Works and Alex Liddell. See [credits and licensing status](CREDITS.md).
