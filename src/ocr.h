// Windows' built-in OCR (Windows.Media.Ocr), for text that exists only as
// pixels: no app reports it through UI Automation. What it reads is only a
// candidate; the text checker still has to confirm that a redraw of each word
// matches the pixels before anything is shown.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "text_source.h"

struct OcrWord {
    std::wstring text;
    RectD box;     // physical desktop pixels
    int line = 0;  // OCR line, numbered from 1 in each read
};

class OcrReader {
public:
    OcrReader();
    ~OcrReader();
    // Call on an MTA thread. False if no recognizer is installed for the
    // user's languages.
    bool Init();
    // Words inside area of the frame, read at scale times the size (0: the
    // usual 2x; less if the image would be too large).
    std::vector<OcrWord> Read(const CpuFrame& f, const RectD& area, int scale = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
