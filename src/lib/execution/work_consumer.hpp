#pragma once

#include <cstdint>
#include <functional>
#include <type_traits>

#include "photospider/core/status.hpp"

namespace ps::execution_internal {
// Synchronous borrowing adapter. Legacy callers retain their owning API;
// planar numerical checkpoints return the host's stable status by reference.
class WorkConsumer final {
 public:
  using Owned = std::function<Status(std::uint64_t)>;
  using Borrowed = std::function<const Status&(std::uint64_t)>;
  WorkConsumer(std::nullptr_t = nullptr) {}  // NOLINT(runtime/explicit)
  WorkConsumer(const Borrowed& callback)
      : borrowed_(&callback) {}  // NOLINT(runtime/explicit)
  template <class F, std::enable_if_t<!std::is_same_v<F, WorkConsumer> &&
                                          !std::is_pointer_v<F>,
                                      int> = 0>
  WorkConsumer(const F& f) : context_(&f) {  // NOLINT(runtime/explicit)
    check_ = [](const void* context, std::uint64_t amount) {
      const auto& callback = *static_cast<const F*>(context);
      decltype(auto) status = callback(amount);
      if (!status.ok())
        throw status;
    };
    invoke_ = [](const void* context, std::uint64_t amount,
                 Status& result) -> const Status& {
      const auto& callback = *static_cast<const F*>(context);
      if constexpr (std::is_same_v<
                        std::invoke_result_t<const F&, std::uint64_t>,
                        const Status&>) {
        return callback(amount);
      } else {
        result = callback(amount);
        return result;
      }
    };
  }
  template <class F>
  WorkConsumer(const F* f) {
    if (f)
      *this = WorkConsumer(*f);
  }  // NOLINT(runtime/explicit)
  void check(std::uint64_t amount) const {
    if (borrowed_) {
      const auto& status = (*borrowed_)(amount);
      if (!status.ok())
        throw status;
    } else {
      check_(context_, amount);
    }
  }
  const WorkConsumer& operator*() const { return *this; }
  const Status& operator()(std::uint64_t amount) const {
    return borrowed_ ? (*borrowed_)(amount)
                     : invoke_(context_, amount, result_);
  }

 private:
  const Borrowed* borrowed_ = nullptr;
  void (*check_)(const void*, std::uint64_t) = nullptr;
  const void* context_ = nullptr;
  const Status& (*invoke_)(const void*, std::uint64_t, Status&) = nullptr;
  mutable Status result_;
};
}  // namespace ps::execution_internal
