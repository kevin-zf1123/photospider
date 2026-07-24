// FILE: plugins/ops/metal/perlin_noise_metal.mm

#include "metal/perlin_noise_metal.hpp"

#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "metal/metal_exception_boundary.hpp"
#include "photospider/plugin/opencv_adapter.hpp"

// This specific OpenCV header is required for the interop function
#include <opencv2/core/core_c.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <opencv2/videoio/registry.hpp>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Metal Shading Language source for the Perlin compute kernel.
 *
 * @note The process-wide MetalState compiles this immutable source once. The
 * pointer refers to static storage for the lifetime of the process.
 */
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

namespace ps {
namespace ops {

/**
 * @brief Reads one optional integer parameter from a public node snapshot.
 * @param node Public operation identity and effective parameters.
 * @param key Parameter name to read.
 * @param fallback Value returned when the parameter is absent.
 * @return Int64 or exactly integral Double converted after range validation.
 * @throws plugin::ParameterTypeError for a non-numeric or fractional value.
 * @throws std::out_of_range when the value does not fit int.
 * @note Numeric alternatives are inspected explicitly; exact public accessors
 *       never perform cross-alternative conversion.
 */
int parameter_int(const plugin::NodeView& node, std::string_view key,
                  int fallback) {
  const plugin::ParameterValue* value = node.find_parameter(key);
  if (!value) {
    return fallback;
  }
  double numeric = 0.0;
  if (value->is_int64()) {
    numeric = static_cast<double>(value->as_int64());
  } else if (value->is_double()) {
    numeric = value->as_double();
  } else {
    throw plugin::ParameterTypeError(plugin::ParameterKind::Int64,
                                     value->kind());
  }
  if (!std::isfinite(numeric) || std::trunc(numeric) != numeric) {
    throw plugin::ParameterTypeError(plugin::ParameterKind::Int64,
                                     value->kind());
  }
  if (numeric < static_cast<double>(std::numeric_limits<int>::min()) ||
      numeric > static_cast<double>(std::numeric_limits<int>::max())) {
    throw std::out_of_range("Metal Perlin integer parameter is out of range");
  }
  return static_cast<int>(numeric);
}

/**
 * @brief Reads one optional numeric parameter from a public node snapshot.
 * @param node Public operation identity and effective parameters.
 * @param key Parameter name to read.
 * @param fallback Value returned when the parameter is absent.
 * @return Double or integer alternative converted to double.
 * @throws plugin::ParameterTypeError for a non-numeric parameter.
 * @note Boolean and string alternatives are never converted implicitly.
 */
double parameter_double(const plugin::NodeView& node, std::string_view key,
                        double fallback) {
  const plugin::ParameterValue* value = node.find_parameter(key);
  if (!value) {
    return fallback;
  }
  if (value->is_double()) {
    return value->as_double();
  }
  if (value->is_int64()) {
    return static_cast<double>(value->as_int64());
  }
  throw plugin::ParameterTypeError(plugin::ParameterKind::Double,
                                   value->kind());
}

/**
 * @brief Process-wide Metal objects required by the Perlin source operation.
 *
 * @note Objective-C references retain their objects for the lifetime of this
 * state. Construction happens once through GetMetalState and calls are
 * serialized separately by g_metal_perlin_mutex. That provider-private mutex
 * protects shared Metal backend state only; it is not an OpenCV or execution
 * exclusivity policy.
 */
struct MetalState {
  /** @brief Default Metal device retained by the process-wide state. */
  id<MTLDevice> device;
  /** @brief Command queue used for serialized Perlin dispatches. */
  id<MTLCommandQueue> commandQueue;
  /** @brief Compiled Perlin compute pipeline. */
  id<MTLComputePipelineState> pipelineState;

  /**
   * @brief Creates the device, command queue, shader library, and pipeline.
   *
   * @throws std::bad_alloc if C++ or Objective-C bridge allocation exhausts
   * memory.
   * @throws std::runtime_error if any required Metal object cannot be
   * created.
   * @note Construction is invoked once. A failed std::call_once attempt may
   * be retried by a later caller according to the standard once-flag rules.
   */
  MetalState() {
    NSLog(@"Initializing MetalState...");
    device = MTLCreateSystemDefaultDevice();
    if (!device)
      throw std::runtime_error("Failed to create Metal device.");

    commandQueue = [device newCommandQueue];
    if (!commandQueue)
      throw std::runtime_error("Failed to create Metal command queue.");

    NSError* error = nil;
    NSString* sourceString =
        [NSString stringWithUTF8String:perlin_shader_source];
    id<MTLLibrary> library = [device newLibraryWithSource:sourceString
                                                  options:nil
                                                    error:&error];
    if (!library) {
      NSLog(@"FATAL: Metal library creation failed. Error: %@", error);
      throw std::runtime_error("Failed to compile Metal shader.");
    }

    NSString* kernelName = @"perlin_noise_kernel";
    id<MTLFunction> kernelFunction = [library newFunctionWithName:kernelName];
    if (!kernelFunction) {
      NSLog(@"FATAL: Failed to find Metal function named '%@' in the library.",
            kernelName);
      throw std::runtime_error("Failed to find Metal kernel function.");
    }

    pipelineState = [device newComputePipelineStateWithFunction:kernelFunction
                                                          error:&error];
    if (!pipelineState) {
      NSLog(@"FATAL: Metal pipeline state creation failed. Error: %@", error);
      throw std::runtime_error("Failed to create Metal pipeline state.");
    }
    NSLog(@"MetalState initialized successfully.");
  }
};

/**
 * @brief Owns the process-wide lazily initialized Metal state.
 *
 * @note g_metal_state is populated exactly once through g_metal_state_flag
 * after successful construction. Its unique_ptr exclusively owns MetalState,
 * whose Objective-C fields retain the Metal objects. Static teardown destroys
 * the state and releases those objects; no caller may retain a reference past
 * that teardown.
 */
static std::unique_ptr<MetalState> g_metal_state;

/**
 * @brief Synchronizes process-wide Metal state initialization.
 *
 * @note The flag has static lifetime and guards only construction/publication
 * of g_metal_state. A failed initialization attempt leaves it unset so a later
 * caller may retry; command encoding is synchronized separately.
 */
static std::once_flag g_metal_state_flag;

/**
 * @brief Returns the process-wide initialized Metal state.
 *
 * @return Borrowed reference valid until g_metal_state static teardown.
 * @throws std::bad_alloc unchanged if state allocation exhausts memory.
 * @throws std::system_error when std::call_once cannot coordinate state
 * initialization.
 * @throws std::runtime_error when MetalState construction fails.
 * @note std::call_once serializes construction. The returned reference remains
 * owned by g_metal_state and is valid only until static teardown; callers must
 * separately serialize command encoding through g_metal_perlin_mutex.
 */
static MetalState& GetMetalState() {
  std::call_once(g_metal_state_flag,
                 []() { g_metal_state = std::make_unique<MetalState>(); });
  return *g_metal_state;
}

/**
 * @brief Prewarms the process-wide Metal state for the loader.
 *
 * @return Nothing.
 * @throws std::bad_alloc unchanged if state allocation exhausts memory.
 * @throws std::system_error when std::call_once cannot coordinate state
 * initialization.
 * @throws std::runtime_error when Metal initialization fails.
 * @note The function is idempotent after successful std::call_once completion.
 */
void perlin_noise_metal_eager_init() {
  (void)GetMetalState();
}

/**
 * @brief Serializes process-wide Metal Perlin command encoding and readback.
 *
 * @note The mutex has static lifetime and owns no Metal object. Each operation
 * holds it from entry into the serialized boundary through CPU readback; the
 * independent g_metal_state_flag remains responsible for state initialization.
 * This DSO-private boundary does not serialize CPU OpenCV providers or declare
 * execution-domain-wide exclusivity.
 */
static std::mutex g_metal_perlin_mutex;

/**
 * @brief Executes the Metal Perlin source operation with contextual errors.
 *
 * @param node Public effective parameters controlling width, height, grid size,
 * and random seed.
 * @param inputs Unused borrowed source-operation input list.
 * @return Public output owning a CPU copy of the generated image.
 * @throws std::bad_alloc unchanged from parameter parsing, working buffers, or
 * output conversion; also propagates diagnostic-construction exhaustion.
 * @throws std::runtime_error with the current stage for other standard or
 * unknown failures.
 * @note Calls acquire g_metal_perlin_mutex inside the portable contextual
 * boundary and use an autorelease pool. It does not mutate OpenCV thread-local
 * OpenCL policy. Returned storage does not retain Metal resources.
 */
plugin::OperationOutput op_perlin_noise_metal(
    const plugin::NodeView& node,
    plugin::ArrayView<plugin::OperationInputView> inputs) {
  (void)inputs;
  @autoreleasepool {
    const char* dbg_stage = "start";
    return detail::run_serialized_metal_exception_boundary(
        "perlin_noise_metal", dbg_stage, g_metal_perlin_mutex,
        [&]() -> plugin::OperationOutput {
          int width = parameter_int(node, "width", 256);
          int height = parameter_int(node, "height", 256);
          float scale =
              static_cast<float>(parameter_double(node, "grid_size", 1.0));
          int seed = parameter_int(node, "seed", -1);
          dbg_stage = "validate_parameters";
          if (width <= 0 || height <= 0) {
            throw std::invalid_argument(
                "width and height must both be positive");
          }

          dbg_stage = "metal_state";
          MetalState& metal = GetMetalState();
          id<MTLDevice> device = metal.device;
          id<MTLCommandQueue> commandQueue = metal.commandQueue;
          id<MTLComputePipelineState> pipelineState = metal.pipelineState;

          // FIX: 设备上限保护（大尺寸时给出清晰报错）
          // NSUInteger maxDim = device.maxTextureDimension2D;
          // if (width > (int)maxDim || height > (int)maxDim) {
          //     throw std::runtime_error("Requested texture size exceeds
          //     device.maxTextureDimension2D");
          // }

          // 输出纹理：单通道 32F（与 CV/CI 链路一致）
          dbg_stage = "create_texture";
          MTLTextureDescriptor* texDesc = [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                           width:width
                                          height:height
                                       mipmapped:NO];
          texDesc.usage =
              MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
          id<MTLTexture> outTexture = [device newTextureWithDescriptor:texDesc];
          if (!outTexture) {
            throw std::runtime_error("Failed to create output MTLTexture.");
          }

          // Perlin permutation/参数缓冲
          dbg_stage = "alloc_permutation";
          std::vector<int> p_vec(512);
          std::iota(p_vec.begin(), p_vec.begin() + 256, 0);
          /*std::mt19937 g(std::random_device{}());*/ std::mt19937 g;
          if (seed == -1) {
            g.seed(std::random_device{}());
          } else {
            g.seed(seed);
          }
          std::shuffle(p_vec.begin(), p_vec.begin() + 256, g);
          std::copy(p_vec.begin(), p_vec.begin() + 256, p_vec.begin() + 256);

          dbg_stage = "create_buffers";
          id<MTLBuffer> p_buffer =
              [device newBufferWithBytes:p_vec.data()
                                  length:512 * sizeof(int)
                                 options:MTLResourceStorageModeShared];
          id<MTLBuffer> scale_buffer =
              [device newBufferWithBytes:&scale
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

          id<MTLComputeCommandEncoder> encoder =
              [commandBuffer computeCommandEncoder];
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
          NSUInteger h = std::max<NSUInteger>(
              1, pipelineState.maxTotalThreadsPerThreadgroup / w);
          MTLSize threadsPerThreadgroup = MTLSizeMake(w, h, 1);

          [encoder dispatchThreads:threadsPerGrid
              threadsPerThreadgroup:threadsPerThreadgroup];
          [encoder endEncoding];

          // FIX: 提交并等待 GPU 完成，防止 CPU 过早读取
          dbg_stage = "submit_wait";
          [commandBuffer commit];
          [commandBuffer waitUntilCompleted];

          // 直接拷贝纹理数据回 CPU：避免 CoreImage/CVPixelBuffer 并发不稳定
          dbg_stage = "readback_texture";
          MTLRegion region = MTLRegionMake2D(0, 0, width, height);
          const size_t bytesPerRow = sizeof(float) * static_cast<size_t>(width);
          std::vector<float> host_buffer(static_cast<size_t>(width) *
                                         static_cast<size_t>(height));
          [outTexture getBytes:host_buffer.data()
                   bytesPerRow:bytesPerRow
                    fromRegion:region
                   mipmapLevel:0];

          cv::Mat mat_view(height, width, CV_32FC1, host_buffer.data(),
                           bytesPerRow);
          cv::Mat mat_copy = mat_view.clone();

          dbg_stage = "wrap_result";
          plugin::OperationOutput result;
          result.image_buffer = plugin::opencv::from_mat(mat_copy);
          return result;
        });
  }
}

}  // namespace ops
}  // namespace ps
