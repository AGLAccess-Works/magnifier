// PNG/BMP/etc. reading and PNG writing through WIC. Pixels are 32-bit BGRA.
// The calling thread must have COM initialised.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

bool LoadImageBGRA(const wchar_t* path, UINT& width, UINT& height, std::vector<uint32_t>& pixels);

// Writes an opaque PNG (alpha is forced to 255, as the output has no alpha).
bool SavePngBGRA(const wchar_t* path, UINT width, UINT height, const void* data, UINT rowPitch);
