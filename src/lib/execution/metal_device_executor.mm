#include "execution/metal_device_executor.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/pending_value.hpp"
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
 * @brief Checked compact geometry for one R32Float texture transfer.
 *
 * @throws Nothing for aggregate value operations.
 */
struct Float32TransferGeometry final {
  /** @brief Active bytes in one tightly packed texture row. */
  std::size_t bytes_per_row = 0U;
  /** @brief Complete tightly packed transfer byte envelope. */
  std::size_t storage_size = 0U;
};

/**
 * @brief Validates dimensions and computes a compact R32Float byte envelope.
 * @param width Positive texture width.
 * @param height Positive texture height.
 * @return Checked row and complete byte sizes.
 * @throws std::invalid_argument when either dimension is zero.
 * @throws std::overflow_error when byte or stride arithmetic is
 * unrepresentable.
 * @note The helper allocates no storage and performs no native work.
 */
Float32TransferGeometry checked_float32_transfer_geometry(
    std::uint32_t width, std::uint32_t height) {
  if (width == 0U || height == 0U) {
    throw std::invalid_argument("Metal transfer dimensions must be positive.");
  }
  const std::size_t width_size = static_cast<std::size_t>(width);
  if (width_size > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    throw std::overflow_error("Metal transfer row size overflow.");
  }
  const std::size_t bytes_per_row = width_size * sizeof(float);
  if (height > std::numeric_limits<std::size_t>::max() / bytes_per_row) {
    throw std::overflow_error("Metal transfer allocation size overflow.");
  }
  if (bytes_per_row >
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::overflow_error("Metal transfer stride overflow.");
  }
  return Float32TransferGeometry{
      bytes_per_row, static_cast<std::size_t>(height) * bytes_per_row};
}

/**
 * @brief Shared C++ owner retaining one native Metal allocation.
 *
 * @throws Nothing after ARC has retained the supplied resource.
 * @note BufferHandle stores this owner type-erased; public Value APIs never
 * expose the Objective-C handle.
 */
class MetalResourceOwner final {
 public:
  /**
   * @brief Retains one non-null native resource.
   * @param resource Texture or buffer retained through Value lifetime.
   * @throws std::invalid_argument when resource is null.
   */
  explicit MetalResourceOwner(id<MTLResource> resource) : resource_(resource) {
    if (resource_ == nil) {
      throw std::invalid_argument(
          "Metal Value owner requires a native resource.");
    }
  }

 private:
  /** @brief ARC-retained texture or buffer. */
  id<MTLResource> __strong resource_;
};

/**
 * @brief Removes one native transfer admission unless command commit succeeds.
 *
 * @throws ResidencyManager admission exceptions from construction.
 * @note The identity is copied so Objective-C/C++ unwinding cannot invalidate
 * cleanup order. Destruction contains synchronization errors because it runs
 * on a failure path that must preserve the original exception.
 */
class ScopedTransferAdmission final {
 public:
  /**
   * @brief Registers and owns one exact pending transfer admission.
   * @param residency_manager Non-null process publication authority.
   * @param identity Exact source/destination submission identity.
   * @throws std::invalid_argument for a null manager or stale identity.
   * @throws std::bad_alloc or std::system_error from registry ownership.
   */
  ScopedTransferAdmission(std::shared_ptr<ResidencyManager> residency_manager,
                          const DeviceCompletionIdentity& identity)
      : residency_manager_(std::move(residency_manager)), identity_(identity) {
    if (!residency_manager_) {
      throw std::invalid_argument(
          "Transfer admission guard requires a residency manager.");
    }
    residency_manager_->register_transfer(identity_);
  }

  /**
   * @brief Discards an unresolved exact admission.
   * @throws Nothing; cleanup failures cannot replace native submission failure.
   */
  ~ScopedTransferAdmission() noexcept {
    if (!committed_) {
      try {
        (void)residency_manager_->discard_transfer(identity_);
      } catch (...) {
      }
    }
  }

  /**
   * @brief Marks native command submission successful.
   * @return Nothing.
   * @throws Nothing.
   * @note The command completion owner assumes responsibility for terminal
   * acceptance or rejection after this call.
   */
  void release() noexcept { committed_ = true; }

  /**
   * @brief Prevents duplicating exact admission cleanup ownership.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  ScopedTransferAdmission(const ScopedTransferAdmission&) = delete;
  /**
   * @brief Prevents replacing exact admission cleanup ownership.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  ScopedTransferAdmission& operator=(const ScopedTransferAdmission&) = delete;

 private:
  /** @brief Process publication authority used for exact cleanup. */
  std::shared_ptr<ResidencyManager> residency_manager_;
  /** @brief Complete copied identity registered by construction. */
  DeviceCompletionIdentity identity_;
  /** @brief True after native command commit transferred cleanup ownership. */
  bool committed_ = false;
};

/**
 * @brief Owns both pending publications until one command buffer terminates.
 *
 * @throws Nothing from settle(); publication construction occurs beforehand.
 * @note The native completion block is the sole caller. A device source may
 * settle after physical execution, but an already-stale generation terminally
 * fails its destination before Ready can release dependent work.
 */
class MetalTransferCompletion final {
 public:
  /**
   * @brief Takes complete transfer publication ownership.
   * @param source Pending device-local source.
   * @param destination Pending host-visible replica.
   * @param identity Exact admitted completion identity.
   * @param residency_manager Non-null process publication authority.
   * @throws std::invalid_argument when the manager is null.
   */
  MetalTransferCompletion(PendingDeviceValuePublication source,
                          PendingDeviceValuePublication destination,
                          DeviceCompletionIdentity identity,
                          std::shared_ptr<ResidencyManager> residency_manager)
      : source_(std::move(source)),
        destination_(std::move(destination)),
        identity_(std::move(identity)),
        residency_manager_(std::move(residency_manager)) {
    if (!residency_manager_) {
      throw std::invalid_argument(
          "Metal transfer completion requires a residency manager.");
    }
  }

  /**
   * @brief Settles producer fences and conditionally publishes residency.
   * @param command_buffer Terminal native command buffer.
   * @return Nothing.
   * @throws Nothing; failure to create diagnostics falls back to cancellation
   * when producer capabilities destruct.
   * @note Ready is published only after Metal reports successful completion of
   * both compute and explicit blit encoders.
   */
  void settle(id<MTLCommandBuffer> command_buffer) noexcept {
    try {
      if (command_buffer.status == MTLCommandBufferStatusError) {
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Transfer,
            static_cast<std::int64_t>(command_buffer.error.code),
            metal_failure_message("Metal command buffer failed",
                                  command_buffer.error));
        (void)source_.producer.complete_failed(failure);
        (void)destination_.producer.complete_failed(failure);
        (void)residency_manager_->discard_transfer(identity_);
        return;
      }
      if (command_buffer.status != MTLCommandBufferStatusCompleted) {
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Execution,
            static_cast<std::int64_t>(command_buffer.status),
            "Metal completion callback observed a nonterminal status.");
        (void)source_.producer.complete_failed(failure);
        (void)destination_.producer.complete_failed(failure);
        (void)residency_manager_->discard_transfer(identity_);
        return;
      }
      const ResidencyCompletionDisposition disposition =
          residency_manager_->publish_ready_transfer(
              identity_, source_.value, destination_.value, &source_.producer,
              destination_.producer);
      if (disposition != ResidencyCompletionDisposition::Published) {
        (void)source_.producer.complete_ready();
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Execution,
            disposition == ResidencyCompletionDisposition::Stale ? 85 : 86,
            disposition == ResidencyCompletionDisposition::Stale
                ? "Metal transfer completion was superseded before "
                  "destination publication."
                : "Metal transfer completion no longer has an exact "
                  "admission.");
        (void)destination_.producer.complete_failed(failure);
      }
    } catch (...) {
      try {
        const ReadyFenceFailure failure(ReadyFenceFailureDomain::Execution, -1,
                                        "Metal completion handling failed.");
        if (source_.producer.valid()) {
          (void)source_.producer.complete_failed(failure);
        }
        if (destination_.producer.valid()) {
          (void)destination_.producer.complete_failed(failure);
        }
      } catch (...) {
      }
      try {
        (void)residency_manager_->discard_transfer(identity_);
      } catch (...) {
      }
    }
  }

 private:
  /** @brief Pending device-local source and terminal capability. */
  PendingDeviceValuePublication source_;
  /** @brief Pending host-visible replica and terminal capability. */
  PendingDeviceValuePublication destination_;
  /** @brief Exact identity admitted before native commit. */
  DeviceCompletionIdentity identity_;
  /** @brief Process-domain conditional replica publisher. */
  std::shared_ptr<ResidencyManager> residency_manager_;
};

/**
 * @brief Owns one CPU-to-Metal destination until its command buffer terminates.
 *
 * @throws Nothing from settle(); publication construction occurs beforehand.
 * @note The source is already Ready and host-visible before submission. The
 * completion callback owns only destination fence settlement and exact
 * residency acceptance; it exposes no native pointer or visible Graph commit.
 */
class MetalUploadCompletion final {
 public:
  /**
   * @brief Takes complete upload publication ownership.
   * @param source Ready immutable host source retained through completion.
   * @param destination Pending device-local destination.
   * @param identity Exact admitted completion identity.
   * @param residency_manager Non-null process publication authority.
   * @throws std::invalid_argument when source or manager is invalid.
   */
  MetalUploadCompletion(Value source, PendingDeviceValuePublication destination,
                        DeviceCompletionIdentity identity,
                        std::shared_ptr<ResidencyManager> residency_manager)
      : source_(std::move(source)),
        destination_(std::move(destination)),
        identity_(std::move(identity)),
        residency_manager_(std::move(residency_manager)) {
    if (!source_.valid() || !residency_manager_) {
      throw std::invalid_argument(
          "Metal upload completion requires source and residency manager.");
    }
  }

  /**
   * @brief Settles the destination and conditionally publishes residency.
   * @param command_buffer Terminal native upload command buffer.
   * @return Nothing.
   * @throws Nothing; diagnostic allocation failure falls back to producer
   * cancellation and exact admission discard.
   * @note Ready is published only after the buffer-to-texture blit and native
   * visibility work have completed successfully.
   */
  void settle(id<MTLCommandBuffer> command_buffer) noexcept {
    try {
      if (command_buffer.status == MTLCommandBufferStatusError) {
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Transfer,
            static_cast<std::int64_t>(command_buffer.error.code),
            metal_failure_message("Metal upload command buffer failed",
                                  command_buffer.error));
        (void)destination_.producer.complete_failed(failure);
        (void)residency_manager_->discard_transfer(identity_);
        return;
      }
      if (command_buffer.status != MTLCommandBufferStatusCompleted) {
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Execution,
            static_cast<std::int64_t>(command_buffer.status),
            "Metal upload callback observed a nonterminal status.");
        (void)destination_.producer.complete_failed(failure);
        (void)residency_manager_->discard_transfer(identity_);
        return;
      }
      const ResidencyCompletionDisposition disposition =
          residency_manager_->publish_ready_transfer(
              identity_, source_, destination_.value, nullptr,
              destination_.producer);
      if (disposition != ResidencyCompletionDisposition::Published) {
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Execution,
            disposition == ResidencyCompletionDisposition::Stale ? 85 : 86,
            disposition == ResidencyCompletionDisposition::Stale
                ? "Metal upload completion was superseded before destination "
                  "publication."
                : "Metal upload completion no longer has an exact admission.");
        (void)destination_.producer.complete_failed(failure);
      }
    } catch (...) {
      try {
        const ReadyFenceFailure failure(
            ReadyFenceFailureDomain::Execution, -1,
            "Metal upload completion handling failed.");
        if (destination_.producer.valid()) {
          (void)destination_.producer.complete_failed(failure);
        }
      } catch (...) {
      }
      try {
        (void)residency_manager_->discard_transfer(identity_);
      } catch (...) {
      }
    }
  }

 private:
  /** @brief Ready CPU source retained for exact completion validation. */
  Value source_;
  /** @brief Pending device-local destination and terminal capability. */
  PendingDeviceValuePublication destination_;
  /** @brief Exact identity admitted before native commit. */
  DeviceCompletionIdentity identity_;
  /** @brief Process-domain conditional replica publisher. */
  std::shared_ptr<ResidencyManager> residency_manager_;
};

/**
 * @brief Real process-owned Metal device executor.
 *
 * One executor retains its device, command queue, and validated pipeline cache
 * until service destruction. An admission monitor serializes direct registry
 * calls in addition to the service's single Metal lane while leaving waiting
 * submissions observable through diagnostics.
 *
 * @throws std::system_error when admission-monitor construction or an
 * operation's initial state-mutex acquisition fails.
 * @note `DeviceExecutor::execute()` rejects synchronous same-executor callback
 * re-entry before this implementation reaches admission or diagnostic
 * counters. The class owns no Run, Graph, ready-store, or resource-ledger
 * state.
 */
class MetalDeviceExecutor final : public DeviceExecutor {
 public:
  /**
   * @brief Binds one already-created native device and command queue.
   * @param device Non-null process-owned Metal device.
   * @param command_queue Non-null process-owned command queue.
   * @param residency_manager Non-null replica publication authority.
   * @throws std::invalid_argument if either native object is null.
   * @throws std::bad_alloc when cache dictionaries cannot be created.
   * @throws std::system_error when the callback-admission condition variable
   * cannot be initialized.
   * @note ARC retains both native objects for the complete executor lifetime.
   */
  MetalDeviceExecutor(id<MTLDevice> device, id<MTLCommandQueue> command_queue,
                      std::shared_ptr<ResidencyManager> residency_manager)
      : device_(device),
        command_queue_(command_queue),
        residency_manager_(std::move(residency_manager)),
        pipelines_([[NSMutableDictionary alloc] init]),
        pipeline_sources_([[NSMutableDictionary alloc] init]),
        pipeline_functions_([[NSMutableDictionary alloc] init]) {
    if (device_ == nil || command_queue_ == nil || !residency_manager_) {
      throw std::invalid_argument(
          "MetalDeviceExecutor requires device, queue, and residency manager.");
    }
    if (pipelines_ == nil || pipeline_sources_ == nil ||
        pipeline_functions_ == nil) {
      throw std::bad_alloc{};
    }
  }

  /** @copydoc DeviceExecutor::device */
  Device device() const noexcept override { return Device::GPU_METAL; }

  /** @copydoc DeviceExecutor::execute_impl */
  void execute_impl(DeviceExecutorInvocation& invocation) override {
    @autoreleasepool {
      InvocationAdmission admission(*this);
      InvocationContext context(*this, invocation.completion_seed());
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
   * @throws std::system_error when initial acquisition of `state_mutex_`
   * fails.
   * @note C++17 non-timed predicate waiting does not propagate synchronization
   * exceptions. The predicate is non-throwing; failure to re-lock and satisfy
   * the wait postcondition terminates the process. One admission is
   * thread-affine and cannot outlive its executor.
   */
  class InvocationAdmission final {
   public:
    /**
     * @brief Submits and waits for one exclusive callback entry.
     * @param executor Live executor whose admission monitor is entered.
     * @throws std::overflow_error before waiting when a diagnostic counter is
     * exhausted.
     * @throws std::system_error when initial acquisition of `state_mutex_`
     * fails.
     * @note The C++17 non-timed wait itself propagates no synchronization
     * exception. Its predicate is non-throwing; a failed re-lock that cannot
     * satisfy the wait postcondition terminates the process.
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
          lock, [this]() noexcept { return !executor_.callback_active_; });
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
     * @param completion_seed Optional exact ComputeRun/task lineage.
     * @throws std::bad_alloc when the retention array cannot be created.
     */
    InvocationContext(MetalDeviceExecutor& executor,
                      std::optional<DeviceCompletionSeed> completion_seed)
        : executor_(executor),
          completion_seed_(std::move(completion_seed)),
          resources_([[NSMutableArray alloc] init]) {
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

    /** @copydoc MetalExecutionContext::publish_float32_texture_to_host */
    void publish_float32_texture_to_host(NativeHandle command_buffer_handle,
                                         NativeHandle texture_handle,
                                         std::uint32_t width,
                                         std::uint32_t height) override {
      if (command_buffer_handle == nullptr || texture_handle == nullptr ||
          width == 0U || height == 0U) {
        throw std::invalid_argument(
            "Metal transfer requires command buffer, texture, and dimensions.");
      }
      if (!completion_seed_.has_value()) {
        throw std::logic_error(
            "Metal transfer requires ComputeRun completion lineage.");
      }
      if (published_value_.valid()) {
        throw std::logic_error(
            "Metal invocation already published an operation Value.");
      }
      const Float32TransferGeometry geometry =
          checked_float32_transfer_geometry(width, height);
      const std::size_t bytes_per_row = geometry.bytes_per_row;
      const std::size_t storage_size = geometry.storage_size;

      id<MTLCommandBuffer> command_buffer =
          (__bridge id<MTLCommandBuffer>)command_buffer_handle;
      id<MTLTexture> texture = (__bridge id<MTLTexture>)texture_handle;
      if (texture.pixelFormat != MTLPixelFormatR32Float ||
          texture.width != static_cast<NSUInteger>(width) ||
          texture.height != static_cast<NSUInteger>(height)) {
        throw std::invalid_argument(
            "Metal transfer texture does not match R32Float dimensions.");
      }

      id<MTLBuffer> host_buffer =
          [executor_.device_ newBufferWithLength:storage_size
                                         options:MTLResourceStorageModeShared];
      if (host_buffer == nil || host_buffer.contents == nullptr) {
        throw std::runtime_error(
            "Metal executor failed to allocate host-visible transfer buffer.");
      }
      retain_resource(host_buffer);

      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      if (blit == nil) {
        throw std::runtime_error(
            "Metal executor failed to create transfer blit encoder.");
      }
      [blit copyFromTexture:texture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width, height, 1)
                          toBuffer:host_buffer
                 destinationOffset:0
            destinationBytesPerRow:bytes_per_row
          destinationBytesPerImage:storage_size];
      [blit endEncoding];

      DenseTensorDescriptor descriptor{
          {static_cast<std::size_t>(height), static_cast<std::size_t>(width)},
          ElementSemantics::FloatingPoint,
          StorageEncoding{32U},
      };
      const std::optional<ImageFacet> image_facet =
          ImageFacet{1U, 0U, std::nullopt};
      const StridedLayout layout{{static_cast<std::ptrdiff_t>(bytes_per_row),
                                  static_cast<std::ptrdiff_t>(sizeof(float))},
                                 0U};
      auto texture_owner = std::make_shared<MetalResourceOwner>(texture);
      PendingDeviceValuePublication source =
          PendingDeviceValuePublisher::publish_dense_tensor(
              descriptor, image_facet, layout, texture_owner,
              (__bridge void*)texture, nullptr, storage_size,
              DeviceId(DeviceBackend::Metal), MemoryDomain::DeviceLocal);
      auto buffer_owner = std::make_shared<MetalResourceOwner>(host_buffer);
      PendingDeviceValuePublication destination =
          PendingDeviceValuePublisher::publish_dense_tensor(
              descriptor, image_facet, layout, buffer_owner,
              (__bridge void*)host_buffer,
              static_cast<std::byte*>(host_buffer.contents), storage_size,
              DeviceId(DeviceBackend::CPU), MemoryDomain::HostPinned,
              source.value.revision_id());
      DeviceCompletionIdentity identity(*completion_seed_, source.value,
                                        destination.value);
      Value published_destination = destination.value;
      auto completion = std::make_shared<MetalTransferCompletion>(
          std::move(source), std::move(destination), identity,
          executor_.residency_manager_);
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        completion->settle(completed);
      }];
      ScopedTransferAdmission admission(executor_.residency_manager_, identity);
      published_value_ = std::move(published_destination);
      [command_buffer commit];
      admission.release();
    }

    /** @copydoc MetalExecutionContext::publish_float32_host_to_texture */
    void publish_float32_host_to_texture(Value source, std::uint32_t width,
                                         std::uint32_t height) override {
      if (!source.valid()) {
        throw std::invalid_argument(
            "Metal upload requires a valid source Value.");
      }
      if (!completion_seed_.has_value()) {
        throw std::logic_error(
            "Metal upload requires ComputeRun completion lineage.");
      }
      if (published_value_.valid()) {
        throw std::logic_error(
            "Metal invocation already published an operation Value.");
      }
      const Float32TransferGeometry geometry =
          checked_float32_transfer_geometry(width, height);
      const DenseTensorDescriptor& descriptor =
          source.dense_tensor_descriptor();
      const StridedLayout& source_layout = source.strided_layout();
      if (descriptor.shape.size() != 2U ||
          descriptor.shape[0] != static_cast<std::size_t>(height) ||
          descriptor.shape[1] != static_cast<std::size_t>(width) ||
          descriptor.element_semantics != ElementSemantics::FloatingPoint ||
          descriptor.storage_encoding.bit_width != 32U ||
          source_layout.byte_strides.size() != 2U ||
          source_layout.byte_strides[0] <
              static_cast<std::ptrdiff_t>(geometry.bytes_per_row) ||
          source_layout.byte_strides[1] !=
              static_cast<std::ptrdiff_t>(sizeof(float))) {
        throw std::invalid_argument(
            "Metal upload requires a row-major rank-two FLOAT32 source.");
      }
      const AccessPlan plan = source.plan_access(
          AccessTarget{DeviceId(DeviceBackend::Metal),
                       MemoryDomain::DeviceLocal, false, true});
      if (plan.kind() != AccessPlanKind::Transfer) {
        throw std::invalid_argument(
            "Metal upload requires an explicit Transfer access plan.");
      }

      const DenseTensorView source_view(source);
      id<MTLBuffer> staging_buffer =
          [executor_.device_ newBufferWithLength:geometry.storage_size
                                         options:MTLResourceStorageModeShared];
      if (staging_buffer == nil || staging_buffer.contents == nullptr) {
        throw std::runtime_error(
            "Metal executor failed to allocate upload staging buffer.");
      }
      retain_resource(staging_buffer);
      auto* staging_bytes = static_cast<std::byte*>(staging_buffer.contents);
      const std::size_t source_row_stride =
          static_cast<std::size_t>(source_layout.byte_strides[0]);
      for (std::size_t row = 0U; row < static_cast<std::size_t>(height);
           ++row) {
        std::memcpy(staging_bytes + row * geometry.bytes_per_row,
                    source_view.data() + row * source_row_stride,
                    geometry.bytes_per_row);
      }

      MTLTextureDescriptor* texture_descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                       width:static_cast<NSUInteger>(width)
                                      height:static_cast<NSUInteger>(height)
                                   mipmapped:NO];
      texture_descriptor.usage =
          MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
      id<MTLTexture> texture =
          [executor_.device_ newTextureWithDescriptor:texture_descriptor];
      if (texture == nil) {
        throw std::runtime_error(
            "Metal executor failed to allocate upload texture.");
      }
      retain_resource(texture);

      id<MTLCommandBuffer> command_buffer =
          [executor_.command_queue_ commandBuffer];
      if (command_buffer == nil) {
        throw std::runtime_error(
            "Metal executor failed to create upload command buffer.");
      }
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      if (blit == nil) {
        throw std::runtime_error(
            "Metal executor failed to create upload blit encoder.");
      }
      [blit copyFromBuffer:staging_buffer
                 sourceOffset:0
            sourceBytesPerRow:geometry.bytes_per_row
          sourceBytesPerImage:geometry.storage_size
                   sourceSize:MTLSizeMake(width, height, 1)
                    toTexture:texture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];

      const StridedLayout destination_layout{
          {static_cast<std::ptrdiff_t>(geometry.bytes_per_row),
           static_cast<std::ptrdiff_t>(sizeof(float))},
          0U};
      auto texture_owner = std::make_shared<MetalResourceOwner>(texture);
      PendingDeviceValuePublication destination =
          PendingDeviceValuePublisher::publish_dense_tensor(
              descriptor, source.image_facet(), destination_layout,
              texture_owner, (__bridge void*)texture, nullptr,
              geometry.storage_size, DeviceId(DeviceBackend::Metal),
              MemoryDomain::DeviceLocal, source.revision_id());
      DeviceCompletionIdentity identity(*completion_seed_, source,
                                        destination.value);
      Value published_destination = destination.value;
      auto completion = std::make_shared<MetalUploadCompletion>(
          source, std::move(destination), identity,
          executor_.residency_manager_);
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        completion->settle(completed);
      }];
      ScopedTransferAdmission admission(executor_.residency_manager_, identity);
      published_value_ = std::move(published_destination);
      [command_buffer commit];
      admission.release();
    }

    /** @copydoc MetalExecutionContext::take_published_value */
    Value take_published_value() noexcept override {
      Value result;
      std::swap(result, published_value_);
      return result;
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

    /** @brief Exact optional Run/task lineage supplied by the invocation. */
    std::optional<DeviceCompletionSeed> completion_seed_;

    /** @brief Pending host replica taken once by the Host adapter. */
    Value published_value_;

    /** @brief Strong owners for all invocation-created textures and buffers. */
    NSMutableArray<id<MTLResource>>* __strong resources_;

    /** @brief Number of resources released and debited at scope exit. */
    std::uint64_t retained_count_ = 0U;
  };

  /** @brief Process-owned default Metal device retained by ARC. */
  id<MTLDevice> __strong device_;

  /** @brief Process-owned command queue retained until executor destruction. */
  id<MTLCommandQueue> __strong command_queue_;

  /** @brief Exact replica publication authority shared with the registry. */
  std::shared_ptr<ResidencyManager> residency_manager_;

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
std::unique_ptr<DeviceExecutor> make_default_metal_device_executor(
    std::shared_ptr<ResidencyManager> residency_manager) {
  if (!residency_manager) {
    throw std::invalid_argument("Metal executor requires a residency manager.");
  }
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return nullptr;
    }
    id<MTLCommandQueue> command_queue = [device newCommandQueue];
    if (command_queue == nil) {
      return nullptr;
    }
    return std::make_unique<MetalDeviceExecutor>(device, command_queue,
                                                 std::move(residency_manager));
  }
}

}  // namespace ps::execution
