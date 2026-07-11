#include "adapter/buffer_adapter_metal.hpp"
#include "adapter/buffer_adapter_opencv.hpp"

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

#include <stdexcept>
#include <vector>
#include <cstring>

namespace ps {

#ifdef __APPLE__

// Small RAII holder to manage an MTLTexture lifetime across C++ boundaries.
struct MtlTextureHolder {
    id<MTLTexture> tex = nil;
    explicit MtlTextureHolder(id<MTLTexture> t) : tex(t) {}
    ~MtlTextureHolder() {
        // Under ARC, objects created with +new already autoreleased; explicit release for safety only
        // If compiled without ARC, this still safely decrements retain count.
        if (tex) {
            [tex release];
            tex = nil;
        }
    }
};

static void ensure_supported_format_or_throw(const ImageBuffer& buf) {
    if (buf.type != DataType::FLOAT32) {
        throw std::runtime_error("upload_to_metal: only FLOAT32 supported for now");
    }
    if (!(buf.channels == 1 || buf.channels == 4)) {
        throw std::runtime_error("upload_to_metal: only 1 or 4 channels supported for now");
    }
}

ImageBuffer upload_to_metal(const ImageBuffer& cpu_buffer, void* device) {
    if (!device) throw std::runtime_error("upload_to_metal: Metal device is null");
    if (cpu_buffer.device != Device::CPU) {
        throw std::runtime_error("upload_to_metal: input must be CPU buffer");
    }
    if (cpu_buffer.width <= 0 || cpu_buffer.height <= 0) {
        throw std::runtime_error("upload_to_metal: invalid input dimensions");
    }
    ensure_supported_format_or_throw(cpu_buffer);

    const int width = cpu_buffer.width;
    const int height = cpu_buffer.height;
    const int channels = cpu_buffer.channels;
    const size_t elem_size = sizeof(float);

    MTLPixelFormat fmt = (channels == 1) ? MTLPixelFormatR32Float : MTLPixelFormatRGBA32Float;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [(id<MTLDevice>)device newTextureWithDescriptor:desc];
    if (!texture) throw std::runtime_error("upload_to_metal: failed to create MTLTexture");

    // Prepare staging pointer: respect step (stride) if mismatched
    const size_t expected_bpr = width * channels * elem_size;
    const size_t src_bpr = cpu_buffer.step ? cpu_buffer.step : expected_bpr;

    if (!cpu_buffer.data) {
        throw std::runtime_error("upload_to_metal: CPU buffer has no data");
    }

    if (src_bpr == expected_bpr) {
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [texture replaceRegion:region mipmapLevel:0 withBytes:cpu_buffer.data.get() bytesPerRow:expected_bpr];
    } else {
        // Pack row-by-row into a contiguous vector
        std::vector<unsigned char> tmp(height * expected_bpr);
        const unsigned char* src = static_cast<const unsigned char*>(cpu_buffer.data.get());
        for (int y = 0; y < height; ++y) {
            std::memcpy(&tmp[size_t(y) * expected_bpr], src + size_t(y) * src_bpr, expected_bpr);
        }
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [texture replaceRegion:region mipmapLevel:0 withBytes:tmp.data() bytesPerRow:expected_bpr];
    }

    ImageBuffer out;
    out.width = width;
    out.height = height;
    out.channels = channels;
    out.type = DataType::FLOAT32;
    out.device = Device::GPU_METAL;
    out.step = expected_bpr;
    // Wrap holder in shared_ptr<void>
    out.context.reset(new MtlTextureHolder(texture), [](void* p){ delete static_cast<MtlTextureHolder*>(p); });
    return out;
}

ImageBuffer download_from_metal(const ImageBuffer& gpu_buffer) {
    if (gpu_buffer.device != Device::GPU_METAL) {
        throw std::runtime_error("download_from_metal: input is not GPU_METAL");
    }
    if (!gpu_buffer.context) throw std::runtime_error("download_from_metal: missing Metal context");
    auto holder = static_cast<MtlTextureHolder*>(gpu_buffer.context.get());
    id<MTLTexture> texture = holder->tex;
    if (!texture) throw std::runtime_error("download_from_metal: MTLTexture is null");

    const int width = static_cast<int>(texture.width);
    const int height = static_cast<int>(texture.height);
    MTLPixelFormat fmt = texture.pixelFormat;
    int channels = 0;
    if (fmt == MTLPixelFormatR32Float) channels = 1;
    else if (fmt == MTLPixelFormatRGBA32Float) channels = 4;
    else throw std::runtime_error("download_from_metal: unsupported texture format");

    const size_t elem_size = sizeof(float);
    const size_t bpr = size_t(width) * size_t(channels) * elem_size;
    std::vector<unsigned char> tmp(height * bpr);
    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [texture getBytes:tmp.data() bytesPerRow:bpr fromRegion:region mipmapLevel:0];

    // Wrap into ImageBuffer with CPU device
    ImageBuffer out;
    out.width = width;
    out.height = height;
    out.channels = channels;
    out.type = DataType::FLOAT32;
    out.device = Device::CPU;
    out.step = bpr;
    // Copy into owned CPU buffer
    auto mem = std::shared_ptr<unsigned char[]>(new unsigned char[tmp.size()]);
    std::memcpy(mem.get(), tmp.data(), tmp.size());
    out.data.reset(mem.get(), [mem](void*) mutable { mem.reset(); });
    return out;
}

#else // non-Apple stubs

ImageBuffer upload_to_metal(const ImageBuffer&, id) {
    throw std::runtime_error("upload_to_metal: Metal not supported on this platform");
}
ImageBuffer download_from_metal(const ImageBuffer&) {
    throw std::runtime_error("download_from_metal: Metal not supported on this platform");
}

#endif

} // namespace ps
