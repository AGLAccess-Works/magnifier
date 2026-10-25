// Monitor capture through Windows Graphics Capture.
//
// The frame pool is free-threaded and polled from the render thread, so the
// D3D immediate context is only ever touched by one thread. Each new frame is
// copied into a texture we own, because WGC only delivers frames when the
// screen changes and we need to keep drawing the last image between frames.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

class MonitorCapture {
public:
    bool Start(ID3D11Device* device, HMONITOR monitor);
    void Stop();

    // Takes the newest frame, if any arrived since the last call, and copies it
    // into the owned texture. Returns true when the texture changed.
    bool Poll(ID3D11DeviceContext* context);

    bool HasFrame() const { return m_hasFrame; }
    ID3D11ShaderResourceView* View() const { return m_srv.Get(); }
    ID3D11Texture2D* Texture() const { return m_texture.Get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

private:
    bool EnsureTexture(UINT w, UINT h);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_rtDevice{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::Windows::Graphics::SizeInt32 m_poolSize{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    UINT m_width = 0, m_height = 0;
    bool m_hasFrame = false;
};
