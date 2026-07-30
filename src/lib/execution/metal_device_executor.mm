#include "execution/metal_device_executor.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <condition_variable>
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
 * until service destruction. An admission monitor serializes direct registry
 * calls in addition to the service's single Metal lane while leaving waiting
 * submissions observable through diagnostics.
 *
 * @throws std::system_error from mutex or condition-variable operations.
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
      InvocationAdmission admission(*this);
      InvocationContext context(*this);
      ScopedMetalExecutionContext scope(context);
      invocation.run();
    }
  }

  /** @copydoc DeviceExecutor::diagnostics */
  DeviceExecutorDiagnostics diagnostics() const override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return DeviceExecutorDiagnostics{
        Device::GPU_METAL,       command_queue_ != nil, submission_count_,
        invocation_count_,       total_allocations_,    live_allocations_,
        pipeline_cache_entries_,
    };
  }

 private:
  /**
   * @brief Owns one serialized callback-admission obligation.
   *
   * Construction records the submission while holding `state_mutex_`. If a
   * callback is already active, condition-variable waiting atomically releases
   * that mutex, making `submission_count_ > invocation_count_` observable.
   * Once admitted, construction marks one active callback and advances the
   * serialized entry count. Destruction clears that activity only after the
   * invocation context and TLS scope have retired.
   *
   * @throws std::overflow_error before waiting when either monotonic counter
   * is exhausted.
   * @throws std::system_error from mutex or condition-variable operations.
   * @note One admission is thread-affine and cannot outlive its executor.
   */
  class InvocationAdmission final {
   public:
    /**
     * @brief Submits and waits for one exclusive callback entry.
     * @param executor Live executor whose admission monitor is entered.
     * @throws std::overflow_error before waiting when a diagnostic counter is
     * exhausted.
     * @throws std::system_error from mutex or condition-variable operations.
     */
    explicit InvocationAdmission(MetalDeviceExecutor& executor)
        : executor_(executor) {
      std::unique_lock<std::mutex> lock(executor_.state_mutex_);
      if (executor_.submission_count_ ==
              std::numeric_limits<std::uint64_t>::max() ||
          executor_.invocation_count_ ==
              std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "Metal executor invocation counters exhausted.");
      }
      ++executor_.submission_count_;
      executor_.callback_available_.wait(
          lock, [this] { return !executor_.callback_active_; });
      executor_.callback_active_ = true;
      ++executor_.invocation_count_;
      active_ = true;
    }

    /**
     * @brief Releases callback admission and wakes one queued submission.
     * @throws Nothing; synchronization failure terminates because an active
     * callback must never leave the executor permanently admitted.
     */
    ~InvocationAdmission() noexcept {
      try {
        {
          std::lock_guard<std::mutex> lock(executor_.state_mutex_);
          if (!active_ || !executor_.callback_active_) {
            std::terminate();
          }
          executor_.callback_active_ = false;
          active_ = false;
        }
        executor_.callback_available_.notify_one();
      } catch (...) {
        std::terminate();
      }
    }

    /**
     * @brief Prevents duplicating one callback-admission obligation.
     * @param other Unused source because copying is forbidden.
     * @throws Nothing because this operation is deleted.
     */
    InvocationAdmission(const InvocationAdmission& other) = delete;

    /**
     * @brief Prevents replacing one callback-admission obligation.
     * @param other Unused source because assignment is forbidden.
     * @return No value because this operation is deleted.
     * @throws Nothing because this operation is deleted.
     */
    InvocationAdmission& operator=(const InvocationAdmission& other) = delete;

   private:
    /** @brief Executor whose active-callback state this value must release. */
    MetalDeviceExecutor& executor_;

    /** @brief Whether construction acquired one callback admission. */
    bool active_ = false;
  };

  /**
   * @brief Invocation-bounded allocator and pipeline-cache facade.
   *
   * @throws Native allocation, validation, and pipeline errors from methods.
   * @note The enclosing `InvocationAdmission` guarantees that no competing
   * callback mutates native resources. Diagnostics briefly share
   * `state_mutex_` but never wait for this context to retire.
   */
  class InvocationContext final : public MetalExecutionContext {
   public:
    /**
     * @brief Starts one empty native allocation scope.
     * @param executor Exclusively admitted executor whose resources are
     * borrowed.
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
     * @throws Nothing; synchronization failure or a counter invariant breach
     * terminates because the executor cannot expose leaked live allocations.
     * @note Callback admission remains active and retained_count_ cannot exceed
     * the executor's live allocation count.
     */
    ~InvocationContext() noexcept override {
      try {
        std::lock_guard<std::mutex> lock(executor_.state_mutex_);
        if (executor_.live_allocations_ < retained_count_) {
          std::terminate();
        }
        executor_.live_allocations_ -= retained_count_;
      } catch (...) {
        std::terminate();
      }
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
      {
        std::lock_guard<std::mutex> lock(executor_.state_mutex_);
        if (executor_.pipeline_cache_entries_ ==
            std::numeric_limits<std::uint64_t>::max()) {
          throw std::overflow_error(
              "Metal executor pipeline-cache counter exhausted.");
        }
        executor_.pipelines_[key] = pipeline;
        executor_.pipeline_sources_[key] = source_string;
        executor_.pipeline_functions_[key] = function_string;
        ++executor_.pipeline_cache_entries_;
      }
      return (__bridge void*)pipeline;
    }

   private:
    /**
     * @brief Retains one native resource through invocation exit.
     * @param resource Non-null texture or buffer to retain.
     * @return Nothing.
     * @throws std::overflow_error when diagnostic counters are exhausted.
     * @throws std::system_error when diagnostic-state locking fails.
     * @note The diagnostic state lock makes retention and all three counter
     * advances one snapshot-visible transition.
     */
    void retain_resource(id<MTLResource> resource) {
      std::lock_guard<std::mutex> lock(executor_.state_mutex_);
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

    /** @brief Exclusively admitted executor whose resources are borrowed. */
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

  /**
   * @brief Protects callback admission and every copied diagnostic field.
   *
   * @note The mutex is never held while provider code runs. A queued caller
   * releases it through `callback_available_`, so diagnostics can observe that
   * caller while another callback remains active.
   */
  mutable std::mutex state_mutex_;

  /** @brief Wakes queued submissions after the active callback retires. */
  std::condition_variable callback_available_;

  /** @brief Whether exactly one invocation currently owns callback admission.
   */
  bool callback_active_ = false;

  /** @brief Stable cache-key to executor-owned pipeline mapping. */
  NSMutableDictionary<NSString*, id<MTLComputePipelineState>>* __strong
      pipelines_;

  /** @brief Source identity paired with every pipeline cache key. */
  NSMutableDictionary<NSString*, NSString*>* __strong pipeline_sources_;

  /** @brief Function identity paired with every pipeline cache key. */
  NSMutableDictionary<NSString*, NSString*>* __strong pipeline_functions_;

  /**
   * @brief Calls that reached executor admission before serialized waiting.
   *
   * @note Protected by `state_mutex_`; monotonic and checked before increment.
   */
  std::uint64_t submission_count_ = 0U;

  /**
   * @brief Serialized invocation entries, including throwing providers.
   *
   * @note Protected by `state_mutex_`; never exceeds `submission_count_`.
   */
  std::uint64_t invocation_count_ = 0U;

  /** @brief Cumulative textures and buffers retained by invocation allocators.
   */
  std::uint64_t total_allocations_ = 0U;

  /** @brief Native allocations retained by the active invocation. */
  std::uint64_t live_allocations_ = 0U;

  /** @brief Persistent pipeline entries mirrored for lock-safe diagnostics. */
  std::uint64_t pipeline_cache_entries_ = 0U;
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
