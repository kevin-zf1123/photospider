#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

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
 * Nested scopes are supported and restore the previous pointer. Construction
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
