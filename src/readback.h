// Copies the captured frame to the CPU for the text checks, without stalling
// the render thread: a copy is started on one call and collected on a later
// one, once the GPU has finished it.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>

#include "text_source.h"

class FrameReadback {
public:
    // Collects a finished copy if there is one, and starts a new copy of
    // source when start is true and none is in flight.
    std::shared_ptr<CpuFrame> Poll(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                                   int left, int top, bool start);

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    bool m_inFlight = false;
    int m_left = 0, m_top = 0;
    uint64_t m_version = 0;
};
