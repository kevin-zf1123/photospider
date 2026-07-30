#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "photospider/data/value.hpp"

/**
 * @file device_execution_context.hpp
 * @brief Private borrowed native-resource context for device operations.
 *
 * The implementation lives in the shared operation runtime so the Host image
 * and operation DSOs observe one thread-local binding. This source-tree header
 * is not installed and exposes no native SDK type.
 */

namespace ps::execution {

/**
 * @brief Borrows Metal resources owned by the current process executor.
 *
 * The interface grants synchronous access to one executor-owned command queue,
 * an invocation allocator, and a persistent compute-pipeline cache. Returned
 * handles remain owned by the executor and are valid only until the matching
 * operation callback returns.
 *
 * @throws Concrete allocation and pipeline operations report validation,
 * allocation, or native Metal failures as standard exceptions.
 * @note Callers MUST NOT retain this object or any returned native handle past
 * the current callback. The opaque handles never transfer ownership.
 */
class MetalExecutionContext {
 public:
  /** @brief Opaque borrowed Objective-C object pointer. */
  using NativeHandle = void*;

  /**
   * @brief Releases a concrete invocation context.
   * @throws Nothing.
   * @note Only the executor owns and destroys the concrete object.
   */
  virtual ~MetalExecutionContext() = default;

  /**
   * @brief Borrows the executor-owned Metal command queue.
   * @return Non-null opaque `id<MTLCommandQueue>` for this invocation.
   * @throws Nothing.
   * @note The handle MUST NOT be retained after callback return.
   */
  virtual NativeHandle command_queue_handle() const noexcept = 0;

  /**
   * @brief Allocates one invocation-owned writable R32Float 2D texture.
   * @param width Positive texture width in pixels.
   * @param height Positive texture height in pixels.
   * @return Non-null opaque `id<MTLTexture>` retained through callback exit.
   * @throws std::invalid_argument for a zero dimension.
   * @throws std::runtime_error when native allocation fails.
   * @note The invocation allocator releases the texture after callback exit.
   */
  virtual NativeHandle allocate_float32_texture_2d(std::uint32_t width,
                                                   std::uint32_t height) = 0;

  /**
   * @brief Allocates one invocation-owned shared buffer initialized by copy.
   * @param bytes Non-null source bytes borrowed for this call.
   * @param size Positive byte count.
   * @return Non-null opaque `id<MTLBuffer>` retained through callback exit.
   * @throws std::invalid_argument for null bytes or zero size.
   * @throws std::runtime_error when native allocation fails.
   * @note The source may retire immediately after this method returns.
   */
  virtual NativeHandle allocate_shared_buffer_copy(const void* bytes,
                                                   std::size_t size) = 0;

  /**
   * @brief Resolves one executor-lifetime compute pipeline.
   * @param cache_key Nonempty stable operation-owned pipeline key.
   * @param source Nonempty UTF-8 Metal Shading Language source.
   * @param function_name Nonempty UTF-8 compute entry-point name.
   * @return Non-null opaque `id<MTLComputePipelineState>`.
   * @throws std::invalid_argument for empty or invalid UTF-8 inputs, or when
   * an existing key is reused with a different source/function identity.
   * @throws std::runtime_error when native compilation or pipeline creation
   * fails.
   * @note Matching repeated requests reuse the executor-owned cache entry.
   */
  virtual NativeHandle find_or_create_compute_pipeline(
      std::string_view cache_key, std::string_view source,
      std::string_view function_name) = 0;

  /**
   * @brief Encodes and submits an explicit R32Float texture-to-host transfer.
   * @param command_buffer_handle Non-null uncommitted id<MTLCommandBuffer>
   * whose earlier encoders produced texture_handle.
   * @param texture_handle Non-null R32Float id<MTLTexture>.
   * @param width Positive texture width matching its logical output.
   * @param height Positive texture height matching its logical output.
   * @return Nothing after pending source/destination Values, exact completion
   * identity, transfer blit, native completion handler, and commit are
   * installed.
   * @throws std::invalid_argument for missing handles or dimensions.
   * @throws std::logic_error without ComputeRun completion lineage or after a
   * prior output publication in the same operation callback.
   * @throws std::overflow_error for byte arithmetic or identity exhaustion.
   * @throws std::runtime_error for native allocation/encoder failures.
   * @throws std::bad_alloc for retained publication/completion ownership.
   * @note The method never waits and never calls texture getBytes. The
   * destination is a host-visible revision-preserving replica whose ReadyFence
   * settles from the command-buffer completion handler.
   */
  virtual void publish_float32_texture_to_host(
      NativeHandle command_buffer_handle, NativeHandle texture_handle,
      std::uint32_t width, std::uint32_t height) = 0;

  /**
   * @brief Submits an explicit host-to-R32Float-texture transfer.
   * @param source Ready host-visible rank-two FLOAT32 Value to copy.
   * @param width Positive texture width matching the source descriptor.
   * @param height Positive texture height matching the source descriptor.
   * @return Nothing after a distinct pending Metal Value, exact completion
   * identity, buffer-to-texture blit, native callback, and commit are
   * installed.
   * @throws std::invalid_argument for invalid shape, encoding, layout, or
   * dimensions.
   * @throws ReadyFenceAccessError when the source producer is not Ready.
   * @throws BufferAccessError when the source binding is not host-readable.
   * @throws std::logic_error without ComputeRun completion lineage or after a
   * prior output publication in the same operation callback.
   * @throws std::overflow_error for byte arithmetic or identity exhaustion.
   * @throws std::runtime_error for native allocation/encoder failures.
   * @throws std::bad_alloc for retained publication/completion ownership.
   * @note The method performs only explicitly requested source access and
   * never waits for Metal. The pending device-local destination preserves the
   * source logical revision and settles from the command-buffer callback.
   */
  virtual void publish_float32_host_to_texture(Value source,
                                               std::uint32_t width,
                                               std::uint32_t height) = 0;

  /**
   * @brief Takes the pending replica published by this operation.
   * @return Pending host or device Value, or an invalid sentinel when no
   * transfer was published.
   * @throws Nothing.
   * @note The host adapter calls this before the invocation context retires.
   * Repeated calls return an invalid sentinel after the first transfer.
   */
  virtual Value take_published_value() noexcept = 0;
};

/**
 * @brief Returns the current borrowed Metal invocation context if present.
 * @return Borrowed context pointer, or null outside Metal executor entry.
 * @throws Nothing.
 * @note The pointer remains valid only within the surrounding callback scope.
 */
MetalExecutionContext* current_metal_execution_context() noexcept;

/**
 * @brief Requires the current borrowed Metal invocation context.
 * @return Borrowed context reference for the surrounding callback.
 * @throws std::logic_error outside Metal executor entry.
 * @note This function never discovers or creates a native device fallback.
 */
MetalExecutionContext& require_current_metal_execution_context();

/**
 * @brief Installs one borrowed Metal context for a lexical executor scope.
 *
 * Nested scopes from distinct executor objects or direct context tests are
 * supported and restore the previous pointer. This TLS capability does not
 * permit recursive callback entry through the same `DeviceExecutor`; that
 * entry is rejected before a new context scope is constructed. Construction
 * and destruction allocate no storage.
 *
 * @throws Nothing.
 * @note The context must outlive this scope. One scope is thread-affine and
 * MUST be destroyed on the thread where it was constructed.
 */
class ScopedMetalExecutionContext final {
 public:
  /**
   * @brief Publishes one context on the calling thread.
   * @param context Borrowed executor-owned invocation context.
   * @throws Nothing.
   */
  explicit ScopedMetalExecutionContext(MetalExecutionContext& context) noexcept;

  /**
   * @brief Restores the context that preceded this scope.
   * @throws Nothing.
   */
  ~ScopedMetalExecutionContext() noexcept;

  /**
   * @brief Prevents duplicating one thread-local restoration obligation.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  ScopedMetalExecutionContext(const ScopedMetalExecutionContext& other) =
      delete;

  /**
   * @brief Prevents replacing one thread-local restoration obligation.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  ScopedMetalExecutionContext& operator=(
      const ScopedMetalExecutionContext& other) = delete;

 private:
  /** @brief Context pointer restored by the destructor. */
  MetalExecutionContext* previous_ = nullptr;
};

}  // namespace ps::execution
