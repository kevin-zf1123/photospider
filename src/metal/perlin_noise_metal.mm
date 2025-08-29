// FILE: src/metal/perlin_noise_metal.mm

#include "perlin_noise_metal.hpp"
#include "node.hpp"

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

static int as_int_flexible_metal(const YAML::Node& n, const std::string& key, int defv) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<int>(); } catch (...) { return defv; }
}
static double as_double_flexible_metal(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
}

// --- Metal & CoreImage State Management (Unchanged) ---
struct MetalState {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLComputePipelineState> pipelineState;
    CIContext* ci_context; 

    MetalState() {
        NSLog(@"Initializing MetalState...");
        device = MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("Failed to create Metal device.");
        
        commandQueue = [device newCommandQueue];
        if (!commandQueue) throw std::runtime_error("Failed to create Metal command queue.");
        
        ci_context = [CIContext contextWithMTLDevice:device];
        if (!ci_context) throw std::runtime_error("Failed to create CIContext from MTLDevice.");
        // Under MRR, contextWithMTLDevice returns an autoreleased object.
        // Retain to keep it alive across calls/threads; released in destructor.
        [ci_context retain];

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

    ~MetalState() {
        if (ci_context) {
            [ci_context release];
        }
    }
};

static std::unique_ptr<MetalState> g_metal_state;
static std::once_flag g_metal_state_flag;
static MetalState& GetMetalState() {
    std::call_once(g_metal_state_flag, [](){ g_metal_state = std::make_unique<MetalState>(); });
    return *g_metal_state;
}

// --- START: MODIFIED FUNCTION ---
NodeOutput op_perlin_noise_metal(const Node& node, const std::vector<const NodeOutput*>&) {
    @autoreleasepool {
    // FIX: 关闭 OpenCV 的 OpenCL（只需做一次；放在这里最省事）
    static std::once_flag ocl_once;
    std::call_once(ocl_once, []{
        cv::ocl::setUseOpenCL(false);
    });

    const auto& P = node.runtime_parameters;
    int width  = as_int_flexible_metal(P, "width", 256);
    int height = as_int_flexible_metal(P, "height", 256);
    float scale = as_double_flexible_metal(P, "grid_size", 1.0);

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
    std::vector<int> p_vec(512);
    std::iota(p_vec.begin(), p_vec.begin() + 256, 0);
    /*std::mt19937 g(std::random_device{}());*/std::mt19937 g(1);
    std::shuffle(p_vec.begin(), p_vec.begin() + 256, g);
    std::copy(p_vec.begin(), p_vec.begin() + 256, p_vec.begin() + 256);

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
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    // ---- Zero-copy 路径：MTLTexture -> CIImage -> CVPixelBuffer -> cv::Mat ----
    // 1) 从 MTLTexture 创建 CIImage
    CIImage* ciImage = [CIImage imageWithMTLTexture:outTexture options:nil];
    if (!ciImage) {
        throw std::runtime_error("Failed to create CIImage from MTLTexture.");
    }

    // 2) 创建匹配格式的 CVPixelBuffer（单通道 32F），开启 Metal 兼容 & IOSurface
    CVPixelBufferRef pixelBuffer = nullptr;
    NSDictionary* pbOptions = @{
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,            // FIX: Metal 兼容
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{}             // FIX: 必须有一个空字典以启用 IOSurface
    };
    CVReturn status = CVPixelBufferCreate(kCFAllocatorDefault,
                                          width,
                                          height,
                                          kCVPixelFormatType_OneComponent32Float,
                                          (__bridge CFDictionaryRef)pbOptions,
                                          &pixelBuffer);
    if (status != kCVReturnSuccess || !pixelBuffer) {
        throw std::runtime_error("Failed to create CVPixelBuffer.");
    }

    // 3) 用 CIContext 渲染到 PixelBuffer（与 metal.ci_context 同设备）
    [metal.ci_context render:ciImage toCVPixelBuffer:pixelBuffer];

    // 4) 将 CVPixelBuffer 安全地包装为 cv::Mat —— 关键：使用 bytesPerRow
    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    void*   baseAddress  = CVPixelBufferGetBaseAddress(pixelBuffer);
    size_t  bytesPerRow  = CVPixelBufferGetBytesPerRow(pixelBuffer);
    size_t  pbWidth      = CVPixelBufferGetWidth(pixelBuffer);
    size_t  pbHeight     = CVPixelBufferGetHeight(pixelBuffer);

    if (!baseAddress) { /* ... 原来的报错处理 ... */ }
    if (CVPixelBufferIsPlanar(pixelBuffer)) { /* ... 原来的报错处理 ... */ }
    if (pbWidth != (size_t)width || pbHeight != (size_t)height) { /* ... 原来的报错处理 ... */ }

    // ✅ 用带 step 的构造函数拿到一个“视图”
    cv::Mat mat_view((int)pbHeight, (int)pbWidth, CV_32FC1, baseAddress, bytesPerRow);

    // ✅ 关键：**总是 clone** 成为自有内存（与 PixelBuffer 脱钩）
    cv::Mat mat_copy = mat_view.clone();

    // ✅ 现在把数据拷到 UMat（立即拷贝，避免延迟引用）
    NodeOutput result;
    mat_copy.copyTo(result.image_umatrix);

    // 现在再解锁并释放 PixelBuffer，UMat 已有自己的一份数据
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pixelBuffer);

    return result;
    }
}
// --- END: MODIFIED FUNCTION ---

}} // namespace ps::ops
