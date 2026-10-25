// Text re-rendering, step one: finding out what text is on screen.
//
// Two threads:
//  - The scanner asks UI Automation for candidate text around the magnified
//    view: formatted text runs from apps that expose their text (documents,
//    web pages, editors), and the labels of ordinary controls (buttons, menu
//    and list items, tabs, links, title bars) from every window in view.
//  - The checker works on every captured CPU frame and never waits on UI
//    Automation. It confirms that each published run's pixels are unchanged,
//    re-checks runs whose pixels changed (a hover colour, say), and verifies
//    new candidates: pixels must be plain text on a plain background and a
//    redraw of the reported text must match them. For labels it first finds
//    where the words sit in the control and at what size.
// The renderer only ever draws runs the checker has accepted for the current
// pixels.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "shell_icons.h"
#include "viewport.h"

// A CPU copy of a captured frame, in desktop coordinates.
struct CpuFrame {
    uint64_t version = 0;
    int left = 0, top = 0;   // desktop position of pixel (0,0)
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;  // BGRA

    uint32_t At(int x, int y) const { return pixels[(size_t)(y - top) * width + (x - left)]; }
    bool Contains(const RectD& r) const {
        return r.l >= left && r.t >= top && r.r <= left + width && r.b <= top + height;
    }
};

// A background that changes smoothly across a run (a window's shadow, a
// tinted panel): its colour at desktop point (x, y) is the run's background
// plus dx * (x - centre.x) + dy * (y - centre.y) per channel (r, g, b, in
// 0..1 units per pixel), the centre being that of the run's rect.
struct BgSlope {
    float dx[3] = {}, dy[3] = {};
    bool Flat() const { return !dx[0] && !dx[1] && !dx[2] && !dy[0] && !dy[1] && !dy[2]; }
};

// A word of a run placed on its own (see MatchWords).
struct WordPlace {
    int start = 0, length = 0;  // characters of TextRun::text
    double x = 0;               // desktop x of the word's layout box
    double dy = 0;              // vertical nudge
};

struct TextRun {
    std::wstring text;
    RectD rect;              // where the text is drawn (physical desktop pixels)
    std::wstring font;       // family name (reported by the app, or the system UI font for labels)
    double sizePx = 0;       // em size in physical pixels
    int weight = 400;
    bool italic = false;
    uint32_t foreground = 0; // 0xRRGGBB, checked against the pixels
    uint32_t background = 0; // 0xRRGGBB, measured from the pixels (at the rect's centre)
    BgSlope bgSlope;         // how the background changes across the run
    bool appForeground = false;  // the app reported a foreground colour
    double offsetX = 0, offsetY = 0;  // nudge that best lines the redraw up with the pixels
    double match = 0;                 // how well the redraw matches the pixels (correlation)
    double padLeft = 0, padRight = 0; // extra repaint so the app's own glyphs are fully covered
    double inkTop = 0, inkBottom = 0; // if set, where the app's glyphs are: the repaint stays near them
                                      // (OCR words; tight lines would otherwise be painted over)
    std::vector<WordPlace> words;     // if set, each word is drawn at its own matched place
    bool ocr = false;                 // found by OCR (pixels only); app-reported text takes priority
    uint64_t ocrLine = 0;             // OCR: words of one line share a style

    // Candidates only: a control label whose text position inside source is
    // not yet known (the checker finds it).
    bool label = false;
    bool control = false;    // placed from a control's label (else document text or OCR)
    // An icon (not text): Windows' large copy of what the control shows,
    // drawn over rect (the icon's square on screen); text is the item's name.
    std::shared_ptr<const IconImage> icon;
    bool iconFill = false;   // the icon's surroundings are flat: repaint them in background first
    RectD source;            // the control's box, for labels; else same as rect
    std::wstring key;        // identity across scans: text + source box
    std::vector<std::wstring> alternatives;  // labels: other readings of the name to try

    // Pixels under the run when it was last verified, to spot changes.
    int refX = 0, refY = 0, refW = 0, refH = 0;
    std::shared_ptr<const std::vector<uint32_t>> ref;
};

struct TextScene {
    uint64_t generation = 0;
    std::vector<TextRun> runs;
};

class TextSource {
public:
    void Start();
    void Stop();
    void SetEnabled(bool on);
    // The font used when the app's own is not installed (must match the renderer).
    void SetFonts(const std::wstring& fallback, bool useAppFont);
    // Read text that exists only as pixels with Windows OCR.
    void SetOcr(bool on);
    // Draw icons from Windows' large copies.
    void SetIcons(bool on);

    // Render thread: the source area being shown, and where attention is
    // (pointer or caret), used to pick the main text host.
    void SetView(const RectD& view, Vec2 focus);
    // Render thread: a fresh CPU frame.
    void SubmitFrame(std::shared_ptr<const CpuFrame> frame);
    // Any thread: drop runs touching area (nullptr = everything) right away,
    // and rescan soon. Used for scrolling and typing.
    void Invalidate(const RectD* area);

    // Any thread: scan soon (a menu or other window just appeared).
    void RequestScan();

    std::shared_ptr<const TextScene> Scene();

#if MAGNIFIER_DEV_TOOLS
    // Diagnostics (--scan-probe): one scan around view, logging where the
    // time goes. Call on a COM-initialised (MTA) thread.
    static void ProbeScan(const RectD& view, Vec2 focus);
    // Text tests: log each OCR word's look-alike check.
    static void TraceOcr(bool on);
#endif

    // Text tests (text_fixture.h): the scanner's candidates around view now
    // (call on an MTA thread), and the whole check of candidates and then OCR
    // against a still frame, deciding as the live threads do.
    static std::vector<TextRun> ScanCandidates(const RectD& view, Vec2 focus);
    // How long each stage of CheckStill took, in milliseconds.
    struct StillTiming {
        double appText = 0;    // checking the text apps reported
        double ocrRead = 0;    // Windows OCR (both readings)
        double ocrVerify = 0;  // checking the OCR words against the pixels
    };
    static std::vector<TextRun> CheckStill(const CpuFrame& f, std::vector<TextRun> candidates, const RectD& view,
                                           Vec2 focus, double dpi, const std::wstring& fallback, bool useAppFont,
                                           bool ocr, StillTiming* timing = nullptr);

private:
    void ScanLoop();
    void CheckLoop();
    void OcrLoop();
    void Publish(std::vector<TextRun> runs);  // caller holds m_mu

    std::thread m_scanner, m_checker, m_ocr;
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_stop = false;
    bool m_enabled = true;
    bool m_rescan = true;
    std::wstring m_fallbackFont = L"Segoe UI";
    bool m_useAppFont = true;
    bool m_ocrEnabled = true;
    bool m_iconsEnabled = true;
    ShellIcons m_icons;
    RectD m_view;
    Vec2 m_focus;
    std::shared_ptr<const CpuFrame> m_frame;  // newest
    std::shared_ptr<const TextScene> m_scene = std::make_shared<TextScene>();
    uint64_t m_generation = 0;

    // Scanner -> checker hand-over.
    std::vector<TextRun> m_candidates;
    std::shared_ptr<const CpuFrame> m_candidatesFrame;  // frame from before the scan
    bool m_candidatesFresh = false;
    bool m_candidatesNewScan = false;

    // OCR -> checker hand-over.
    std::vector<TextRun> m_ocrCandidates;  // checked, with reference pixels
    bool m_ocrFresh = false;
};
