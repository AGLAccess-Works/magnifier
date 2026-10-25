#include "image_io.h"

#include <wincodec.h>
#include <wrl/client.h>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> wic;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), L"WIC factory");
    return wic;
}

}  // namespace

bool LoadImageBGRA(const wchar_t* path, UINT& width, UINT& height, std::vector<uint32_t>& pixels) {
    auto wic = Factory();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (!wic ||
        !Check(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
               L"WIC open image") ||
        !Check(decoder->GetFrame(0, &frame), L"WIC frame") || !Check(wic->CreateFormatConverter(&converter), L"WIC conv") ||
        !Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom),
               L"WIC convert") ||
        !Check(converter->GetSize(&width, &height), L"WIC size"))
        return false;
    pixels.resize((size_t)width * height);
    return Check(converter->CopyPixels(nullptr, width * 4, width * height * 4, (BYTE*)pixels.data()), L"WIC pixels");
}

bool SavePngBGRA(const wchar_t* path, UINT width, UINT height, const void* data, UINT rowPitch) {
    // Copy so we can force alpha opaque without touching the caller's memory.
    std::vector<uint8_t> buf((size_t)width * height * 4);
    for (UINT y = 0; y < height; ++y) {
        memcpy(&buf[(size_t)y * width * 4], (const uint8_t*)data + (size_t)y * rowPitch, (size_t)width * 4);
        for (UINT x = 0; x < width; ++x) buf[((size_t)y * width + x) * 4 + 3] = 255;
    }
    auto wic = Factory();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return wic && Check(wic->CreateStream(&stream), L"WIC stream") &&
           Check(stream->InitializeFromFilename(path, GENERIC_WRITE), L"WIC open file") &&
           Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), L"WIC encoder") &&
           Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"WIC encoder init") &&
           Check(encoder->CreateNewFrame(&frame, nullptr), L"WIC frame") &&
           Check(frame->Initialize(nullptr), L"WIC frame init") &&
           Check(frame->SetSize(width, height), L"WIC size") && Check(frame->SetPixelFormat(&format), L"WIC format") &&
           format == GUID_WICPixelFormat32bppBGRA &&
           Check(frame->WritePixels(height, width * 4, (UINT)buf.size(), buf.data()), L"WIC write") &&
           Check(frame->Commit(), L"WIC frame commit") && Check(encoder->Commit(), L"WIC commit");
}
