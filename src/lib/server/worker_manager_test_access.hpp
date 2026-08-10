/**
 * @file worker_manager_test_access.hpp
 * @brief Exposes source-private worker-manager fault and ownership seams.
 */
#pragma once

namespace ps::server {

/**
 * @brief Source-private access to exact descriptor-reset behavior.
 *
 * This non-installed seam lets maintained tests substitute one allocation-free
 * close callback while exercising the same ownership transition used by
 * `WorkerManager` parent-side descriptors.
 *
 * @throws Static access operations document their own validation failures.
 * @note Product code never accepts a caller-provided close callback.
 */
class WorkerManagerTestAccess final {
 public:
  /**
   * @brief Allocation-free close callback used by the descriptor test seam.
   * @param descriptor Nonnegative descriptor whose ownership was already
   * cleared or replaced.
   * @param context Borrowed opaque context supplied to the seam.
   * @return A close-style result; tests may return `-1` with `errno == EINTR`.
   * @throws Nothing; callbacks must not raise across the `noexcept` boundary.
   */
  using DescriptorCloseCall = int (*)(int descriptor, void* context) noexcept;

  /**
   * @brief Exercises the manager's exact descriptor replacement primitive.
   * @param descriptor Non-null source-private descriptor owner to replace.
   * @param replacement Replacement descriptor or `-1`.
   * @param close_call Non-null allocation-free close-style callback.
   * @param context Borrowed opaque callback context, which may be null.
   * @return Nothing after the ownership transition and one close attempt.
   * @throws std::invalid_argument when `descriptor` or `close_call` is null.
   * @note The callback result is ignored by the product boundary. This seam is
   * non-installed and grants no access to a live `WorkerManager` registry.
   */
  static void reset_descriptor_for_test(int* descriptor, int replacement,
                                        DescriptorCloseCall close_call,
                                        void* context);
};

}  // namespace ps::server
