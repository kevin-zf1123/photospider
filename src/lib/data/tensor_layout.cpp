#include "photospider/data/tensor_layout.hpp"

#include <memory>
#include <utility>

#include "photospider/core/checked_math.hpp"
namespace ps {
TensorBudget::TensorBudget(std::uint64_t maximum_bytes, Reserve reserve,
                           std::shared_ptr<void> accounting_domain)
    : maximum_bytes_(maximum_bytes),
      reserve_(std::move(reserve)),
      accounting_domain_(std::move(accounting_domain)) {
}  // NOLINT(whitespace/indent_namespace)
std::uint64_t TensorBudget::live_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_bytes_;
}
Result<std::shared_ptr<void>> TensorBudget::charge(std::uint64_t bytes,
                                                   bool metadata,
                                                   bool already_accounted) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!core_internal::can_add(bytes, live_bytes_, maximum_bytes_))
    return Result<std::shared_ptr<void>>(Status::failure(
        ErrorCode::ResourceExhausted, "aggregate image page budget exceeded"));
  std::shared_ptr<void> lease;
  if (reserve_ && bytes && !already_accounted) {
    auto reserved = reserve_(bytes, metadata);
    if (!reserved.ok())
      return reserved;
    lease = reserved.take_value();
  }
  live_bytes_ += bytes;
  return Result<std::shared_ptr<void>>(std::move(lease));
}
void TensorBudget::release(std::uint64_t bytes) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  live_bytes_ -= bytes;
}
}  // namespace ps
