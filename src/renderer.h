// D3D11 renderer that draws the magnified view into a DirectComposition swap
// chain attached to the overlay window.
#pragma once

#include <d2d1_1.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cursor.h"
#include "fonts.h"
#include "settings.h"
#include "shell_icons.h"

// One run of text drawn fresh from the font, in backbuffer pixels.
struct TextDraw {
    std::wstring text;
    float left = 0, top = 0, width = 0, height = 0;  // where the app drew it
    float boxLeft = 0, boxTop = 0, boxRight = 0, boxBottom = 0;  // area to repaint
    std::wstring family;          // font the app used
    float emPx = 0;               // em size at output scale
    int weight = 400;
    bool italic = false;
    uint32_t foreground = 0, background = 0;  // 0xRRGGBB before colour mode
    std::shared_ptr<const IconImage> icon;  // an icon instead of text: drawn into left/top/width/height
    bool iconFill = false;                   // repaint the box in background colour first
    bool bgGradient = false;      // the background changes smoothly: these are the box's corners
    uint32_t bgCorners[4] = {};   // top left, top right, bottom left, bottom right
    float opacity = 1;            // fades in when it first appears

    struct Word {
        std::wstring text;
        float left = 0, dy = 0;   // output position of the word's layout box, vertical nudge
    };
    std::vector<Word> words;      // if set, drawn word by word instead of as one run
};

struct RenderParams {
    // Visible source region, as a continuous texel coordinate in the capture
    // texture, and the zoom factor.
    double srcOriginX = 0, srcOriginY = 0;
    double zoom = 1;

    Filter filter = Filter::Smooth;
    double edgeStrength = 1, edgeWidth = 1, textBoldness = 0.35, edgeRoundness = 0.5;
    double graphicsSharpness = 0.8;
    ColorMode color = ColorMode::Normal;
    uint32_t foreground = 0, background = 0;
    double brightness = 0, contrast = 1;

    bool drawCursor = false;
    double cursorX = 0, cursorY = 0;  // backbuffer position of the hotspot
    double cursorScale = 1;           // output pixels per source pixel

    uint64_t sourceVersion = 0;  // changes whenever the source image does

    const std::vector<TextDraw>* text = nullptr;  // re-rendered text runs
    std::wstring fallbackFont = L"Segoe UI";      // when the app's font is not installed
    bool useAppFont = true;

    const wchar_t* message = nullptr;  // on-screen message, e.g. "Zoom 3x"
    float messageOpacity = 1;
};

class Renderer {
public:
    // hwnd == nullptr renders offscreen (no window, no present).
    bool Init(HWND hwnd, UINT width, UINT height);
    void Shutdown();

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    HANDLE FrameWaitable() const { return m_waitable; }

    void SetCursor(const CursorImage& image);

    // Draws and presents. Returns the Present result so the caller can spot a
    // lost device. If snapshotPath is set, the finished frame is also saved
    // as a PNG (a lossless copy of exactly what was shown).
    HRESULT Render(ID3D11ShaderResourceView* source, UINT sourceWidth, UINT sourceHeight, const RenderParams& p,
                   const wchar_t* snapshotPath = nullptr);

private:
    bool CreateSwapChain(HWND hwnd);
    bool CreateTargets();
    bool CompileShaders();
    bool CreateTextResources();
    bool EnsureStats(UINT w, UINT h);
    void RunStats(ID3D11ShaderResourceView* source, UINT w, UINT h);
    void DrawMessage(const wchar_t* text, float opacity);
    void DrawTextRuns(const RenderParams& p);
    IDWriteTextFormat* TextFormat(const std::wstring& family, int weight, bool italic, float em);

    bool SaveBackBuffer(const wchar_t* path);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain2> m_swapChain;
    ComPtr<IDCompositionDevice> m_dcomp;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual> m_dcompVisual;
    ComPtr<ID3D11Texture2D> m_target;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    HANDLE m_waitable = nullptr;
    UINT m_width = 0, m_height = 0;

    ComPtr<ID3D11VertexShader> m_fullscreenVS, m_cursorVS;
    ComPtr<ID3D11PixelShader> m_filterPS[(int)Filter::Count];
    ComPtr<ID3D11PixelShader> m_cursorPS, m_cursorInvertPS;
    ComPtr<ID3D11PixelShader> m_statsHPS, m_statsVPS;

    // Smooth filter pre-pass: [0,1] horizontal lo/hi, [2,3] final lo/hi.
    ComPtr<ID3D11Texture2D> m_stats[4];
    ComPtr<ID3D11ShaderResourceView> m_statsSrv[4];
    ComPtr<ID3D11RenderTargetView> m_statsRtv[4];
    UINT m_statsW = 0, m_statsH = 0;
    uint64_t m_statsVersion = ~0ull;
    ID3D11ShaderResourceView* m_statsSource = nullptr;
    ComPtr<ID3D11Buffer> m_params, m_cursorParams;
    ComPtr<ID3D11SamplerState> m_linear;
    ComPtr<ID3D11BlendState> m_premultBlend, m_invertBlend;

    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1DeviceContext> m_d2d;
    ComPtr<ID2D1Bitmap1> m_d2dTarget;
    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<IDWriteTextFormat> m_messageFormat;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<IDWriteFontCollection> m_systemFonts;
    std::map<std::wstring, ComPtr<IDWriteTextFormat>> m_formats;
    std::unique_ptr<FontResolver> m_resolver;
    // Icons as Direct2D bitmaps, kept while they are in use.
    std::map<const IconImage*, std::pair<std::shared_ptr<const IconImage>, ComPtr<ID2D1Bitmap>>> m_iconBitmaps;

    ComPtr<ID3D11ShaderResourceView> m_cursorColour, m_cursorInvert;
    UINT m_cursorW = 0, m_cursorH = 0;
    double m_cursorHotX = 0, m_cursorHotY = 0, m_cursorSourcePerImage = 1;
    bool m_cursorHasInvert = false;
};
