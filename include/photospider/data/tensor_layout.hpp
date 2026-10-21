#pragma once
#include <cstdint>
#include <mutex>

#include "photospider/data/tensor_span.hpp"
namespace ps {
namespace data_internal {
class ResultDeviceAccess;
}
/** @brief Thread-safe per-Result growth capacity for retained storage.
 * @note Physical allocations are charged separately to their owning Root.
 * Growth remains charged until the corresponding backing owner retires.
 */
class PHOTOSPIDER_API TensorBudget final {
 public:
  explicit TensorBudget(std::uint64_t maximum_bytes);
  std::uint64_t maximum_bytes() const noexcept { return maximum_bytes_; }
  std::uint64_t live_bytes() const noexcept;

 private:
  friend class ResultBuilder;
  friend class data_internal::ResultDeviceAccess;
  Status charge(std::uint64_t bytes);
  void release(std::uint64_t bytes) noexcept;
  std::uint64_t maximum_bytes_;
  mutable std::mutex mutex_;
  std::uint64_t live_bytes_ = 0;
};

}  // namespace ps
