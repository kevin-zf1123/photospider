#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "photospider/data/tensor_span.hpp"
namespace ps {
enum class TensorStorageOrder : std::uint8_t { Continuous, Tiled };
/** @brief Thread-safe backed-page and metadata capacity across image owners.
 * @note Reserve must return an owner-retaining lease for the charged bytes;
 * the optional accounting domain identifies an already charged root budget.
 */
class PHOTOSPIDER_API TensorBudget final {
 public:
  using Reserve =
      std::function<Result<std::shared_ptr<void>>(std::uint64_t, bool)>;
  explicit TensorBudget(std::uint64_t maximum_bytes, Reserve reserve = {},
                        std::shared_ptr<void> accounting_domain = {});
  std::uint64_t maximum_bytes() const noexcept { return maximum_bytes_; }
  std::uint64_t live_bytes() const noexcept;
  /** @brief Identity of the external root that already accounts charges. */
  const void* accounting_domain() const noexcept {
    return accounting_domain_.get();
  }

 private:
  friend class ResultBuilder;
  Result<std::shared_ptr<void>> charge(std::uint64_t bytes,
                                       bool metadata = false,
                                       bool already_accounted = false);
  void release(std::uint64_t bytes) noexcept;
  std::uint64_t maximum_bytes_;
  mutable std::mutex mutex_;
  std::uint64_t live_bytes_ = 0;
  Reserve reserve_;
  std::function<void(const std::shared_ptr<void>&, std::uint64_t)>
      payload_committed_;
  std::shared_ptr<void> accounting_domain_;
};

}  // namespace ps
