#include "capture.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>

#include "log.h"

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

bool MonitorCapture::Start(ID3D11Device* device, HMONITOR monitor) {
    Stop();
    m_device = device;
    try {
        ComPtr<IDXGIDevice> dxgi;
        if (!Check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), L"QueryInterface IDXGIDevice")) return false;
        winrt::com_ptr<IInspectable> inspectable;
        if (!Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()),
                   L"CreateDirect3D11DeviceFromDXGIDevice"))
            return false;
        m_rtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (!Check(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                             winrt::put_abi(m_item)),
                   L"CreateForMonitor"))
            return false;

        m_poolSize = m_item.Size();
        m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_poolSize);
        m_session = m_pool.CreateCaptureSession(m_item);

        // We draw our own enlarged pointer, so leave the real one out.
        try { m_session.IsCursorCaptureEnabled(false); } catch (...) { Log(L"IsCursorCaptureEnabled unsupported"); }
        // Windows 11 can drop the yellow capture border.
        try { m_session.IsBorderRequired(false); } catch (...) { Log(L"IsBorderRequired unsupported"); }

        m_session.StartCapture();
        Log(L"Capture started: %dx%d", m_poolSize.Width, m_poolSize.Height);
        return true;
    } catch (const winrt::hresult_error& e) {
        Log(L"Capture start failed: 0x%08X %s", (unsigned)e.code(), e.message().c_str());
        Stop();
        return false;
    }
}

void MonitorCapture::Stop() {
    try {
        if (m_session) m_session.Close();
        if (m_pool) m_pool.Close();
    } catch (...) {
    }
    m_session = nullptr;
    m_pool = nullptr;
    m_item = nullptr;
    m_rtDevice = nullptr;
    m_texture.Reset();
    m_srv.Reset();
    m_width = m_height = 0;
    m_hasFrame = false;
}

bool MonitorCapture::EnsureTexture(UINT w, UINT h) {
    if (m_texture && m_width == w && m_height == h) return true;
    m_texture.Reset();
    m_srv.Reset();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!Check(m_device->CreateTexture2D(&desc, nullptr, &m_texture), L"CreateTexture2D capture copy")) return false;
    if (!Check(m_device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv), L"CreateSRV capture copy"))
        return false;
    m_width = w;
    m_height = h;
    return true;
}

bool MonitorCapture::Poll(ID3D11DeviceContext* context) {
    if (!m_pool) return false;
    try {
        // Drain to the newest frame so latency never accumulates.
        wgc::Direct3D11CaptureFrame frame{nullptr};
        while (auto next = m_pool.TryGetNextFrame()) frame = next;
        if (!frame) return false;

        auto size = frame.ContentSize();
        if (size.Width != m_poolSize.Width || size.Height != m_poolSize.Height) {
            // Monitor resolution changed. Recreate the pool and wait for a
            // frame of the new size rather than copying a partial one.
            m_poolSize = size;
            m_pool.Recreate(m_rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            Log(L"Capture size changed: %dx%d", size.Width, size.Height);
            return false;
        }

        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> src;
        if (!Check(access->GetInterface(IID_PPV_ARGS(&src)), L"Frame GetInterface")) return false;

        if (!EnsureTexture((UINT)size.Width, (UINT)size.Height)) return false;
        D3D11_BOX box = {0, 0, 0, (UINT)size.Width, (UINT)size.Height, 1};
        context->CopySubresourceRegion(m_texture.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
        m_hasFrame = true;
        return true;
    } catch (const winrt::hresult_error& e) {
        Log(L"Capture poll failed: 0x%08X", (unsigned)e.code());
        return false;
    }
}
