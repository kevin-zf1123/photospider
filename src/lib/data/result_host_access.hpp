#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "photospider/data/storage.hpp"

namespace ps::data_internal {
// Private observation at actual sample exposure. The data layer knows neither
// execution lanes nor GPU devices; the installed caller classifies storage.
class ResultHostAccessScope final {
 public:
  using Observer =
      std::function<Status(const std::shared_ptr<const CpuStorage>&)>;
  explicit ResultHostAccessScope(Observer observer)
      : observer_(std::move(observer)), previous_(active_) {
    active_ = &observer_;
  }
  ~ResultHostAccessScope() { active_ = previous_; }
  ResultHostAccessScope(const ResultHostAccessScope&) = delete;
  ResultHostAccessScope& operator=(const ResultHostAccessScope&) = delete;
  static Observer capture() { return active_ ? *active_ : Observer{}; }
  static Status observe(const std::shared_ptr<const CpuStorage>& storage) {
    return active_ && *active_ ? (*active_)(storage) : Status::success();
  }

 private:
  Observer observer_;
  const Observer* previous_;
  inline static thread_local const Observer* active_ = nullptr;
};
}  // namespace ps::data_internal
