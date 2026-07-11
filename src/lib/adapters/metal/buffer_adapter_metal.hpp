#pragma once

#include "image_buffer.hpp"

// Avoid including Metal headers in public C++ headers.
// We pass the device as an opaque pointer to ensure a single C++ ABI signature
// across both C++ (*.cpp) and Objective-C++ (*.mm) translation units.

namespace ps {

// Upload CPU ImageBuffer into a new GPU (Metal) ImageBuffer with an owned
// MTLTexture.
// - Supports FLOAT32 with 1 or 4 channels.
// - Throws on unsupported formats or platforms.
ImageBuffer upload_to_metal(const ImageBuffer& cpu_buffer, void* device);

// Download a GPU (Metal) ImageBuffer back to a new CPU ImageBuffer.
// - Expects the input buffer.device == Device::GPU_METAL and context holding an
// MTLTexture.
ImageBuffer download_from_metal(const ImageBuffer& gpu_buffer);

}  // namespace ps
