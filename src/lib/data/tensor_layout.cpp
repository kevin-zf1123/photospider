#include "photospider/data/tensor_layout.hpp"

#include "photospider/core/checked_math.hpp"
namespace ps {
TensorBudget::TensorBudget(std::uint64_t maximum_bytes)
    : maximum_bytes_(maximum_bytes) {}
std::uint64_t TensorBudget::live_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_bytes_;
}
Status TensorBudget::charge(std::uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!core_internal::can_add(bytes, live_bytes_, maximum_bytes_))
    return Status::failure(ErrorCode::ResourceExhausted,
                           "aggregate image page budget exceeded");
  live_bytes_ += bytes;
  return Status::success();
}
void TensorBudget::release(std::uint64_t bytes) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  live_bytes_ -= bytes;
}
}  // namespace ps
