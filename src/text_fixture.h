// Text tests: recordings of real screens, replayed through the real checks.
//
//   --capture-fixture <dir> [x y]   screenshot around the point (default: the
//                                   pointer) plus every text candidate UI
//                                   Automation reports there, as seen at 3x
//   --text-test <dir> <outdir>      replays a recording: the same checks the
//                                   live magnifier makes (app text, then OCR),
//                                   writing result.txt (every run shown, plus
//                                   collision and style counts) and
//                                   render.png (the magnified view with the
//                                   redrawn text)
//
// tools/text_tests.ps1 runs every recording under tests/text and compares
// each result with its saved expected.txt.
#pragma once

#include <windows.h>

#include <string>

int CaptureTextFixture(const std::wstring& dir, POINT pt);
int RunTextTest(const std::wstring& dir, const std::wstring& outDir);
