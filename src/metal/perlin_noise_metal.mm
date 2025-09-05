// FILE: src/metal/perlin_noise_metal.mm

#include "perlin_noise_metal.hpp"
#include "node.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/param_utils.hpp"

#import <Metal/Metal.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>

// This specific OpenCV header is required for the interop function
#include <opencv2/videoio/registry.hpp>
#import <vector>
#include <numeric>
#include <algorithm>
#include <random>
#include <mutex>

#include <opencv2/core/core_c.h>
#include <opencv2/core/ocl.hpp>

// --- Metal Shaders (Unchanged) ---
const char* perlin_shader_source = R"(
    #include <metal_stdlib>
    using namespace metal;
    float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    float lerp(float t, float a, float b) { return a + t * (b - a); }
    float grad(int hash, float x, float y) {
        int h = hash & 3;
        float u = (h & 1) == 0 ? x : -x;
        float v = (h & 2) == 0 ? y : -y;
        return u + v;
    }
    kernel void perlin_noise_kernel(
        texture2d<float, access::write> outTexture [[texture(0)]],
        device const int* p [[buffer(0)]],
        device const float* scale [[buffer(1)]],
        uint2 gid [[thread_position_in_grid]])
    {
        if (gid.x >= outTexture.get_width() || gid.y >= outTexture.get_height()) { return; }
        float s = *scale;
        float nx = float(gid.x) / float(outTexture.get_width()) * s;
        float ny = float(gid.y) / float(outTexture.get_height()) * s;
        int X = int(floor(nx)) & 255;
        int Y = int(floor(ny)) & 255;
        float xf = nx - floor(nx);
        float yf = ny - floor(ny);
        float u = fade(xf);
        float v = fade(yf);
        int aa = p[p[X] + Y];
        int ab = p[p[X] + Y + 1];
        int ba = p[p[X + 1] + Y];
        int bb = p[p[X + 1] + Y + 1];
        float res = lerp(v,
                         lerp(u, grad(aa, xf, yf), grad(ba, xf - 1.0f, yf)),
                         lerp(u, grad(ab, xf, yf - 1.0f), grad(bb, xf - 1.0f, yf - 1.0f)));
        float final_color = (res + 1.0f) / 2.0f;
        outTexture.write(final_color, gid);
    }
)";

namespace ps { namespace ops {

// static int as_int_flexible_metal(const YAML::Node& n, const std::string& key, int defv) {
//     if (!n || !n[key]) return defv;
//     try { return n[key].as<int>(); } catch (...) { return defv; }
// }
// static double as_double_flexible_metal(const YAML::Node& n, const std::string& key, double defv) {
//     if (!n || !n[key]) return defv;
//     try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
// }

// --- Metal & CoreImage State Management (Unchanged) ---
struct MetalState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLComputePipelineState> pipelineState;

    MetalState() {
        NSLog(@"Initializing MetalState...");
        device = MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("Failed to create Metal device.");
        
        commandQueue = [device newCommandQueue];
        if (!commandQueue) throw std::runtime_error("Failed to create Metal command queue.");

        NSError* error = nil;
        NSString* sourceString = [NSString stringWithUTF8String:perlin_shader_source];
        id<MTLLibrary> library = [device newLibraryWithSource:sourceString options:nil error:&error];
        if (!library) {
            NSLog(@"FATAL: Metal library creation failed. Error: %@", error);
            throw std::runtime_error("Failed to compile Metal shader.");
        }
        
        NSString* kernelName = @"perlin_noise_kernel";
        id<MTLFunction> kernelFunction = [library newFunctionWithName:kernelName];
        if (!kernelFunction) {
            NSLog(@"FATAL: Failed to find Metal function named '%@' in the library.", kernelName);
            throw std::runtime_error("Failed to find Metal kernel function.");
        }
        
        pipelineState = [device newComputePipelineStateWithFunction:kernelFunction error:&error];
        if (!pipelineState) {
            NSLog(@"FATAL: Metal pipeline state creation failed. Error: %@", error);
            throw std::runtime_error("Failed to create Metal pipeline state.");
        }
        NSLog(@"MetalState initialized successfully.");
    }
};

static std::unique_ptr<MetalState> g_metal_state;
static std::once_flag g_metal_state_flag;
static MetalState& GetMetalState() {
    std::call_once(g_metal_state_flag, [](){ g_metal_state = std::make_unique<MetalState>(); });
    return *g_metal_state;
}

// Eager init API (for optional prewarm by plugin loader)
extern "C" void perlin_noise_metal_eager_init() {
    (void)GetMetalState();
}

// --- START: MODIFIED FUNCTION ---
// Serialize Metal Perlin executions to avoid driver/CIContext concurrency crashes
static std::mutex g_metal_perlin_mutex;

NodeOutput op_perlin_noise_metal(const Node& node, const std::vector<const NodeOutput*>&) {
    @autoreleasepool {
    const char* dbg_stage = "start";
    std::lock_guard<std::mutex> metal_lock(g_metal_perlin_mutex);
    try {
    // FIX: 关闭 OpenCV 的 OpenCL（只需做一次；放在这里最省事）
    static std::once_flag ocl_once;
    std::call_once(ocl_once, []{
        cv::ocl::setUseOpenCL(false);
    });

    const auto& P = node.runtime_parameters;
    int width  = as_int_flexible(P, "width", 256);
    int height = as_int_flexible(P, "height", 256);
    float scale = as_double_flexible(P, "grid_size", 1.0);
    int seed = as_int_flexible(P, "seed", -1);

    dbg_stage = "metal_state";
    MetalState& metal = GetMetalState();
    id<MTLDevice> device = metal.device;
    id<MTLCommandQueue> commandQueue = metal.commandQueue;
    id<MTLComputePipelineState> pipelineState = metal.pipelineState;

    // FIX: 设备上限保护（大尺寸时给出清晰报错）
    // NSUInteger maxDim = device.maxTextureDimension2D;
    // if (width > (int)maxDim || height > (int)maxDim) {
    //     throw std::runtime_error("Requested texture size exceeds device.maxTextureDimension2D");
    // }

    // 输出纹理：单通道 32F（与 CV/CI 链路一致）
    dbg_stage = "create_texture";
    MTLTextureDescriptor* texDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    texDesc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    id<MTLTexture> outTexture = [device newTextureWithDescriptor:texDesc];
    if (!outTexture) {
        throw std::runtime_error("Failed to create output MTLTexture.");
    }

    // Perlin permutation/参数缓冲
    dbg_stage = "alloc_permutation";
    std::vector<int> p_vec(512);
    std::iota(p_vec.begin(), p_vec.begin() + 256, 0);
    /*std::mt19937 g(std::random_device{}());*/std::mt19937 g;
    if (seed == -1) {
        g.seed(std::random_device{}());
    } else {
        g.seed(seed);
    }
    std::shuffle(p_vec.begin(), p_vec.begin() + 256, g);
    std::copy(p_vec.begin(), p_vec.begin() + 256, p_vec.begin() + 256);

    dbg_stage = "create_buffers";
    id<MTLBuffer> p_buffer     = [device newBufferWithBytes:p_vec.data()
                                                     length:512 * sizeof(int)
                                                    options:MTLResourceStorageModeShared];
    id<MTLBuffer> scale_buffer = [device newBufferWithBytes:&scale
                                                     length:sizeof(float)
                                                    options:MTLResourceStorageModeShared];
    if (!p_buffer || !scale_buffer) {
        throw std::runtime_error("Failed to create Metal buffers.");
    }

    // 编码与调度
    dbg_stage = "encode";
    id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
    if (!commandBuffer) {
        throw std::runtime_error("Failed to create command buffer.");
    }

    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!encoder) {
        throw std::runtime_error("Failed to create compute encoder.");
    }

    [encoder setComputePipelineState:pipelineState];
    [encoder setTexture:outTexture atIndex:0];
    [encoder setBuffer:p_buffer offset:0 atIndex:0];
    [encoder setBuffer:scale_buffer offset:0 atIndex:1];

    // FIX: 使用 dispatchThreads 并计算健壮的 threadgroupSize
    MTLSize threadsPerGrid = MTLSizeMake(width, height, 1);
    NSUInteger w = pipelineState.threadExecutionWidth;
    NSUInteger h = std::max<NSUInteger>(1, pipelineState.maxTotalThreadsPerThreadgroup / w);
    MTLSize threadsPerThreadgroup = MTLSizeMake(w, h, 1);

    [encoder dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];
    [encoder endEncoding];

    // FIX: 提交并等待 GPU 完成，防止 CPU 过早读取
    dbg_stage = "submit_wait";
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    // 直接拷贝纹理数据回 CPU：避免 CoreImage/CVPixelBuffer 并发不稳定
    dbg_stage = "readback_texture";
    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    const size_t bytesPerRow = sizeof(float) * static_cast<size_t>(width);
    std::vector<float> host_buffer(static_cast<size_t>(width) * static_cast<size_t>(height));
    [outTexture getBytes:host_buffer.data() bytesPerRow:bytesPerRow fromRegion:region mipmapLevel:0];

    cv::Mat mat_view(height, width, CV_32FC1, host_buffer.data(), bytesPerRow);
    cv::Mat mat_copy = mat_view.clone();

    dbg_stage = "wrap_result";
    NodeOutput result;
    result.image_buffer = fromCvMat(mat_copy);
    return result;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("perlin_noise_metal[") + dbg_stage + "]: " + e.what());
    } catch (...) {
        throw std::runtime_error(std::string("perlin_noise_metal[") + dbg_stage + "]: unknown exception");
    }
    }
}


}} // namespace ps::ops
