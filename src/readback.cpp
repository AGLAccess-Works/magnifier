#include "readback.h"

std::shared_ptr<CpuFrame> FrameReadback::Poll(ID3D11Device* device, ID3D11DeviceContext* context,
                                              ID3D11Texture2D* source, int left, int top, bool start) {
    std::shared_ptr<CpuFrame> done;
    if (m_inFlight) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(m_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return nullptr;
        m_inFlight = false;
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC desc;
            m_staging->GetDesc(&desc);
            done = std::make_shared<CpuFrame>();
            done->version = ++m_version;
            done->left = m_left;
            done->top = m_top;
            done->width = (int)desc.Width;
            done->height = (int)desc.Height;
            done->pixels.resize((size_t)desc.Width * desc.Height);
            for (UINT y = 0; y < desc.Height; ++y)
                memcpy(&done->pixels[(size_t)y * desc.Width], (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch,
                       desc.Width * 4);
            context->Unmap(m_staging.Get(), 0);
        }
    }
    if (start && source && !m_inFlight) {
        D3D11_TEXTURE2D_DESC desc;
        source->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC have = {};
        if (m_staging) m_staging->GetDesc(&have);
        if (!m_staging || have.Width != desc.Width || have.Height != desc.Height) {
            m_staging.Reset();
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_staging))) return done;
        }
        context->CopyResource(m_staging.Get(), source);
        m_left = left;
        m_top = top;
        m_inFlight = true;
    }
    return done;
}
