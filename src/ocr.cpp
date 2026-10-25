#include "ocr.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"

namespace wmo = winrt::Windows::Media::Ocr;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;

struct OcrReader::Impl {
    wmo::OcrEngine engine{nullptr};
    int maxDim = 0;
};

OcrReader::OcrReader() = default;
OcrReader::~OcrReader() = default;

bool OcrReader::Init() {
    try {
        auto engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            Log(L"OCR: no recognizer installed for the user's languages");
            return false;
        }
        m = std::make_unique<Impl>();
        m->engine = engine;
        m->maxDim = (int)wmo::OcrEngine::MaxImageDimension();
        Log(L"OCR ready (%s, images up to %d px)", engine.RecognizerLanguage().DisplayName().c_str(), m->maxDim);
        return true;
    } catch (const winrt::hresult_error& e) {
        Log(L"OCR unavailable: %s", e.message().c_str());
        return false;
    }
}

std::vector<OcrWord> OcrReader::Read(const CpuFrame& f, const RectD& area, int scale) {
    std::vector<OcrWord> out;
    if (!m) return out;
    int x0 = (std::max)(f.left, (int)std::floor(area.l)), y0 = (std::max)(f.top, (int)std::floor(area.t));
    int x1 = (std::min)(f.left + f.width, (int)std::ceil(area.r)), y1 = (std::min)(f.top + f.height, (int)std::ceil(area.b));
    int w = (std::min)(x1 - x0, m->maxDim), h = (std::min)(y1 - y0, m->maxDim);
    if (w < 16 || h < 16) return out;
    // Screen text is small for OCR; reading it at twice the size finds more.
    if (scale <= 0) scale = 2;
    while (scale > 1 && (std::max)(w, h) * scale > m->maxDim) --scale;
    const int iw = w * scale, ih = h * scale;
    try {
        wss::Buffer buf((uint32_t)iw * ih * 4);
        buf.Length((uint32_t)iw * ih * 4);
        uint32_t* dst = reinterpret_cast<uint32_t*>(buf.data());
        for (int y = 0; y < ih; ++y) {
            const uint32_t* src = &f.pixels[(size_t)(y0 + y / scale - f.top) * f.width + (x0 - f.left)];
            uint32_t* row = dst + (size_t)y * iw;
            if (scale == 1) {
                for (int x = 0; x < iw; ++x) row[x] = src[x] | 0xFF000000u;
            } else {
                for (int x = 0; x < iw; ++x) row[x] = src[x / scale] | 0xFF000000u;
            }
        }
        auto bmp = wgi::SoftwareBitmap::CreateCopyFromBuffer(buf, wgi::BitmapPixelFormat::Bgra8, iw, ih,
                                                              wgi::BitmapAlphaMode::Premultiplied);
        wmo::OcrResult result = m->engine.RecognizeAsync(bmp).get();
        if (auto angle = result.TextAngle(); angle && std::fabs(angle.Value()) > 1.0) return out;  // rotated text
        int lineNo = 0;
        for (const auto& line : result.Lines()) {
            ++lineNo;
            for (const auto& word : line.Words()) {
                auto r = word.BoundingRect();
                out.push_back({std::wstring(word.Text()),
                               {x0 + r.X / scale, y0 + r.Y / scale, x0 + (r.X + r.Width) / scale,
                                y0 + (r.Y + r.Height) / scale},
                               lineNo});
            }
        }
    } catch (const winrt::hresult_error& e) {
        Log(L"OCR failed: %s", e.message().c_str());
        out.clear();
    }
    return out;
}
