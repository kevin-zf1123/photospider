#include "execution/metal_device_executor.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "execution/device_execution_context.hpp"

namespace ps::execution {
namespace {

/**
 * @brief Converts a required UTF-8 view to an Objective-C string.
 * @param value Nonempty UTF-8 bytes borrowed for this call.
 * @param role Diagnostic role used when conversion fails.
 * @return Strong local string value managed by ARC.
 * @throws std::invalid_argument for empty or invalid UTF-8 input.
 * @throws std::bad_alloc when diagnostic storage exhausts memory.
 * @note The returned object is retained by the caller's strong local or cache.
 */
NSString* make_required_utf8_string(std::string_view value, const char* role) {
  if (value.empty()) {
    throw std::invalid_argument(std::string(role) + " must not be empty.");
  }
  NSString* result = [[NSString alloc] initWithBytes:value.data()
                                              length:value.size()
                                            encoding:NSUTF8StringEncoding];
  if (result == nil) {
    throw std::invalid_argument(std::string(role) +
                                " must contain valid UTF-8.");
  }
  return result;
}

/**
 * @brief Builds one stable native Metal failure diagnostic.
 * @param prefix Stage-specific failure prefix.
 * @param error Optional native error object.
 * @return Owned diagnostic string.
 * @throws std::bad_alloc when result construction exhausts memory.
 * @note A missing or non-UTF-8 native description preserves the prefix alone.
 */
std::string metal_failure_message(const char* prefix, NSError* error) {
  std::string result(prefix);
  if (error != nil) {
    const char* description = [[error localizedDescription] UTF8String];
    if (description != nullptr) {
      result.append(": ");
      result.append(description);
    }
  }
  return result;
}

/**
 * @brief Real process-owned Metal device executor.
 *
 * One executor retains its device, command queue, and validated pipeline cache
 * until service destruction. The execution mutex serializes direct registry
 * calls in addition to the service's single Metal lane.
 *
 * @throws std::system_error from mutex operations.
 * @note The class owns no Run, Graph, ready-store, or resource-ledger state.
 */
class MetalDeviceExecutor final : public DeviceExecutor {
 public:
  /**
   * @brief Binds one already-created native device and command queue.
   * @param device Non-null process-owned Metal device.
   * @param command_queue Non-null process-owned command queue.
   * @throws std::invalid_argument if either native object is null.
   * @throws std::bad_alloc when cache dictionaries cannot be created.
   * @note ARC retains both native objects for the complete executor lifetime.
   */
  MetalDeviceExecutor(id<MTLDevice> device, id<MTLCommandQueue> command_queue)
      : device_(device),
        command_queue_(command_queue),
        pipelines_([[NSMutableDictionary alloc] init]),
        pipeline_sources_([[NSMutableDictionary alloc] init]),
        pipeline_functions_([[NSMutableDictionary alloc] init]) {
    if (device_ == nil || command_queue_ == nil) {
      throw std::invalid_argument(
          "MetalDeviceExecutor requires a device and command queue.");
    }
    if (pipelines_ == nil || pipeline_sources_ == nil ||
        pipeline_functions_ == nil) {
      throw std::bad_alloc{};
    }
  }

  /** @copydoc DeviceExecutor::device */
  Device device() const noexcept override { return Device::GPU_METAL; }

  /** @copydoc DeviceExecutor::execute */
  void execute(DeviceExecutorInvocation& invocation) override {
    @autoreleasepool {
      std::lock_guard<std::mutex> lock(mutex_);
      if (invocation_count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "Metal executor invocation counter exhausted.");
      }
      ++invocation_count_;
      InvocationContext context(*this);
      ScopedMetalExecutionContext scope(context);
      invocation.run();
    }
  }

  /** @copydoc DeviceExecutor::diagnostics */
  DeviceExecutorDiagnostics diagnostics() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeviceExecutorDiagnostics{
        Device::GPU_METAL, command_queue_ != nil,
        invocation_count_, total_allocations_,
        live_allocations_, static_cast<std::uint64_t>([pipelines_ count]),
    };
  }

 private:
  /**
   * @brief Invocation-bounded allocator and pipeline-cache facade.
   *
   * @throws Native allocation, validation, and pipeline errors from methods.
   * @note The enclosing executor mutex remains held for this entire lifetime.
   */
  class InvocationContext final : public MetalExecutionContext {
   public:
    /**
     * @brief Starts one empty native allocation scope.
     * @param executor Locked executor whose resources are borrowed.
     * @throws std::bad_alloc when the retention array cannot be created.
     */
    explicit InvocationContext(MetalDeviceExecutor& executor)
        : executor_(executor), resources_([[NSMutableArray alloc] init]) {
      if (resources_ == nil) {
        throw std::bad_alloc{};
      }
    }

    /**
     * @brief Releases every invocation allocation and updates diagnostics.
     * @throws Nothing.
     * @note The executor mutex remains held and retained_count_ cannot exceed
     * the executor's live allocation count.
     */
    ~InvocationContext() noexcept override {
      if (executor_.live_allocations_ < retained_count_) {
        std::terminate();
      }
      executor_.live_allocations_ -= retained_count_;
    }

    /** @copydoc MetalExecutionContext::command_queue_handle */
    NativeHandle command_queue_handle() const noexcept override {
      return (__bridge void*)executor_.command_queue_;
    }

    /** @copydoc MetalExecutionContext::allocate_float32_texture_2d */
    NativeHandle allocate_float32_texture_2d(std::uint32_t width,
                                             std::uint32_t height) override {
      if (width == 0U || height == 0U) {
        throw std::invalid_argument(
            "Metal texture dimensions must be positive.");
      }
      MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                       width:static_cast<NSUInteger>(width)
                                      height:static_cast<NSUInteger>(height)
                                   mipmapped:NO];
      descriptor.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
      id<MTLTexture> texture =
          [executor_.device_ newTextureWithDescriptor:descriptor];
      if (texture == nil) {
        throw std::runtime_error(
            "Metal executor failed to allocate an R32Float texture.");
      }
      retain_resource(texture);
      return (__bridge void*)texture;
    }

    /** @copydoc MetalExecutionContext::allocate_shared_buffer_copy */
    NativeHandle allocate_shared_buffer_copy(const void* bytes,
                                             std::size_t size) override {
      if (bytes == nullptr || size == 0U) {
        throw std::invalid_argument(
            "Metal shared-buffer copy requires nonempty source bytes.");
      }
      id<MTLBuffer> buffer =
          [executor_.device_ newBufferWithBytes:bytes
                                         length:size
                                        options:MTLResourceStorageModeShared];
      if (buffer == nil) {
        throw std::runtime_error(
            "Metal executor failed to allocate a shared buffer.");
      }
      retain_resource(buffer);
      return (__bridge void*)buffer;
    }

    /** @copydoc MetalExecutionContext::find_or_create_compute_pipeline */
    NativeHandle find_or_create_compute_pipeline(
        std::string_view cache_key, std::string_view source,
        std::string_view function_name) override {
      NSString* key = make_required_utf8_string(cache_key, "Pipeline key");
      NSString* source_string =
          make_required_utf8_string(source, "Pipeline source");
      NSString* function_string =
          make_required_utf8_string(function_name, "Pipeline function");

      id<MTLComputePipelineState> cached = executor_.pipelines_[key];
      if (cached != nil) {
        NSString* cached_source = executor_.pipeline_sources_[key];
        NSString* cached_function = executor_.pipeline_functions_[key];
        if (![cached_source isEqualToString:source_string] ||
            ![cached_function isEqualToString:function_string]) {
          throw std::invalid_argument(
              "Metal pipeline cache key has a conflicting identity.");
        }
        return (__bridge void*)cached;
      }

      NSError* error = nil;
      id<MTLLibrary> library =
          [executor_.device_ newLibraryWithSource:source_string
                                          options:nil
                                            error:&error];
      if (library == nil) {
        throw std::runtime_error(
            metal_failure_message("Metal shader compilation failed", error));
      }
      id<MTLFunction> function = [library newFunctionWithName:function_string];
      if (function == nil) {
        throw std::runtime_error(
            "Metal compute function was absent from compiled source.");
      }
      id<MTLComputePipelineState> pipeline =
          [executor_.device_ newComputePipelineStateWithFunction:function
                                                           error:&error];
      if (pipeline == nil) {
        throw std::runtime_error(
            metal_failure_message("Metal pipeline creation failed", error));
      }
      executor_.pipelines_[key] = pipeline;
      executor_.pipeline_sources_[key] = source_string;
      executor_.pipeline_functions_[key] = function_string;
      return (__bridge void*)pipeline;
    }

   private:
    /**
     * @brief Retains one native resource through invocation exit.
     * @param resource Non-null texture or buffer to retain.
     * @return Nothing.
     * @throws std::overflow_error when diagnostic counters are exhausted.
     * @note Counters advance only after the retention array accepts ownership.
     */
    void retain_resource(id<MTLResource> resource) {
      if (executor_.total_allocations_ ==
              std::numeric_limits<std::uint64_t>::max() ||
          executor_.live_allocations_ ==
              std::numeric_limits<std::uint64_t>::max() ||
          retained_count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "Metal executor allocation counter exhausted.");
      }
      [resources_ addObject:resource];
      ++executor_.total_allocations_;
      ++executor_.live_allocations_;
      ++retained_count_;
    }

    /** @brief Locked executor whose queue/cache/counters are borrowed. */
    MetalDeviceExecutor& executor_;

    /** @brief Strong owners for all invocation-created textures and buffers. */
    NSMutableArray<id<MTLResource>>* __strong resources_;

    /** @brief Number of resources released and debited at scope exit. */
    std::uint64_t retained_count_ = 0U;
  };

  /** @brief Process-owned default Metal device retained by ARC. */
  id<MTLDevice> __strong device_;

  /** @brief Process-owned command queue retained until executor destruction. */
  id<MTLCommandQueue> __strong command_queue_;

  /** @brief Serializes invocation resources, cache mutation, and snapshots. */
  mutable std::mutex mutex_;

  /** @brief Stable cache-key to executor-owned pipeline mapping. */
  NSMutableDictionary<NSString*, id<MTLComputePipelineState>>* __strong
      pipelines_;

  /** @brief Source identity paired with every pipeline cache key. */
  NSMutableDictionary<NSString*, NSString*>* __strong pipeline_sources_;

  /** @brief Function identity paired with every pipeline cache key. */
  NSMutableDictionary<NSString*, NSString*>* __strong pipeline_functions_;

  /** @brief Accepted invocation entries, including throwing providers. */
  std::uint64_t invocation_count_ = 0U;

  /** @brief Cumulative textures and buffers retained by invocation allocators.
   */
  std::uint64_t total_allocations_ = 0U;

  /** @brief Native allocations retained by the active invocation. */
  std::uint64_t live_allocations_ = 0U;
};

}  // namespace

/** @copydoc make_default_metal_device_executor */
std::unique_ptr<DeviceExecutor> make_default_metal_device_executor() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return nullptr;
    }
    id<MTLCommandQueue> command_queue = [device newCommandQueue];
    if (command_queue == nil) {
      return nullptr;
    }
    return std::make_unique<MetalDeviceExecutor>(device, command_queue);
  }
}

}  // namespace ps::execution
