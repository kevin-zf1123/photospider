// FILE: plugins/ops/metal/perlin_noise_metal.mm

#include "metal/perlin_noise_metal.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "execution/device_execution_context.hpp"
#include "metal/metal_exception_boundary.hpp"

/**
 * @brief Metal Shading Language source for the Perlin compute kernel.
 *
 * @note The executor pipeline cache validates and compiles this immutable
 * source once for the stable Perlin cache key.
 */
constexpr std::string_view kPerlinShaderSource = R"(
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

/**
 * @brief Stable executor cache key for the repository Perlin pipeline.
 *
 * @note Reusing this key with different source/function identity is rejected
 * by the process-owned Metal executor.
 */
constexpr std::string_view kPerlinPipelineCacheKey =
    "photospider.perlin_noise_metal.v1";

/** @brief Metal compute entry point paired with kPerlinPipelineCacheKey. */
constexpr std::string_view kPerlinFunctionName = "perlin_noise_kernel";

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
 * @brief Executes the Metal Perlin source operation with contextual errors.
 *
 * @param node Public effective parameters controlling width, height, grid size,
 * and random seed.
 * @param inputs Unused borrowed source-operation input list.
 * @return Canonical empty public output; the source-private Host adapter takes
 * the pending revision-preserving host Value from MetalExecutionContext.
 * @throws std::bad_alloc unchanged from parameter parsing, working buffers, or
 * output conversion; also propagates diagnostic-construction exhaustion.
 * @throws std::runtime_error with the current stage for other standard or
 * unknown failures.
 * @note The process Metal executor serializes entry and supplies a borrowed
 * queue, invocation allocator, pipeline cache, and explicit transfer
 * publication boundary. The operation commits without waiting; the command
 * buffer completion handler settles the Value fence and stale-safe residency.
 */
plugin::OperationOutput op_perlin_noise_metal(
    const plugin::NodeView& node,
    plugin::ArrayView<plugin::OperationInputView> inputs) {
  (void)inputs;
  @autoreleasepool {
    const char* dbg_stage = "start";
    return detail::run_metal_exception_boundary(
        "perlin_noise_metal", dbg_stage, [&]() -> plugin::OperationOutput {
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

          dbg_stage = "executor_context";
          execution::MetalExecutionContext& context =
              execution::require_current_metal_execution_context();
          id<MTLCommandQueue> command_queue =
              (__bridge id<MTLCommandQueue>)context.command_queue_handle();
          if (command_queue == nil) {
            throw std::runtime_error(
                "Metal executor supplied a null command queue.");
          }

          dbg_stage = "pipeline_cache";
          id<MTLComputePipelineState> pipeline_state =
              (__bridge id<MTLComputePipelineState>)
                  context.find_or_create_compute_pipeline(
                      kPerlinPipelineCacheKey, kPerlinShaderSource,
                      kPerlinFunctionName);

          dbg_stage = "resource_plan";
          context.prepare_float32_texture_to_host_resources(
              static_cast<std::uint32_t>(width),
              static_cast<std::uint32_t>(height),
              std::vector<std::size_t>{p_vec.size() * sizeof(int),
                                       sizeof(scale)});

          dbg_stage = "create_texture";
          id<MTLTexture> out_texture =
              (__bridge id<MTLTexture>)
                  context.allocate_persistent_float32_texture_2d(
                      static_cast<std::uint32_t>(width),
                      static_cast<std::uint32_t>(height));

          dbg_stage = "create_buffers";
          id<MTLBuffer> p_buffer =
              (__bridge id<MTLBuffer>)
                  context.allocate_device_scratch_buffer_copy(
                      p_vec.data(), p_vec.size() * sizeof(int));
          id<MTLBuffer> scale_buffer =
              (__bridge id<MTLBuffer>)context
                  .allocate_device_scratch_buffer_copy(&scale, sizeof(scale));

          dbg_stage = "encode";
          id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
          if (command_buffer == nil) {
            throw std::runtime_error("Failed to create command buffer.");
          }

          id<MTLComputeCommandEncoder> encoder =
              [command_buffer computeCommandEncoder];
          if (encoder == nil) {
            throw std::runtime_error("Failed to create compute encoder.");
          }

          [encoder setComputePipelineState:pipeline_state];
          [encoder setTexture:out_texture atIndex:0];
          [encoder setBuffer:p_buffer offset:0 atIndex:0];
          [encoder setBuffer:scale_buffer offset:0 atIndex:1];

          MTLSize threadsPerGrid = MTLSizeMake(width, height, 1);
          NSUInteger w = pipeline_state.threadExecutionWidth;
          NSUInteger h = std::max<NSUInteger>(
              1, pipeline_state.maxTotalThreadsPerThreadgroup / w);
          MTLSize threadsPerThreadgroup = MTLSizeMake(w, h, 1);

          [encoder dispatchThreads:threadsPerGrid
              threadsPerThreadgroup:threadsPerThreadgroup];
          [encoder endEncoding];

          dbg_stage = "explicit_transfer";
          context.publish_float32_texture_to_host(
              (__bridge void*)command_buffer, (__bridge void*)out_texture,
              static_cast<std::uint32_t>(width),
              static_cast<std::uint32_t>(height));
          return {};
        });
  }
}

}  // namespace ops
}  // namespace ps
