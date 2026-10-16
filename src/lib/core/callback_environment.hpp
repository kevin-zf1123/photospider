#pragma once

#include <cfenv>  // NOLINT(build/c++11)
#include <memory>
#include <utility>

#include "photospider/core/status.hpp"

namespace ps::core_internal {
/** @brief Restores thread-local floating state after a foreign callback.
 * The callback inherits the worker's environment. Numerical mode selection
 * belongs to the callback; saving does not change rounding or exception flags.
 */
class CallbackEnvironment final {
 public:
  CallbackEnvironment() noexcept : saved_(std::fegetenv(&previous_) == 0) {}
  ~CallbackEnvironment() noexcept {
    if (saved_)
      std::fesetenv(&previous_);
  }
  CallbackEnvironment(const CallbackEnvironment&) = delete;
  CallbackEnvironment& operator=(const CallbackEnvironment&) = delete;
  bool active() const noexcept { return saved_; }

 private:
  std::fenv_t previous_{};
  bool saved_;
};

/** @brief Invoke one Status/Result callback with state restoration on unwind.
 */
template <class Callback>
auto invoke_callback(Callback&& callback) -> decltype(callback()) {
  CallbackEnvironment environment;
  if (!environment.active())
    return decltype(callback())(
        Status{ErrorCode::OperationFailed, "cannot save floating environment"});
  return std::forward<Callback>(callback)();
}
/** @brief Own foreign callback captures with guarded destruction. */
template <class T>
std::shared_ptr<T> own_callbacks(T value) {
  CallbackEnvironment environment;
  try {
    return std::shared_ptr<T>(new T(std::move(value)), [](T* object) {
      CallbackEnvironment restore;
      delete object;
    });
  } catch (...) {
    value = T{};
    throw;
  }
}
/** @brief Retains foreign prepared state and guards its eventual deleter. */
inline std::shared_ptr<const void> protect_callback_state(
    std::shared_ptr<const void> state) {
  if (!state)
    return {};
  struct Owner {
    std::shared_ptr<const void> state;
    ~Owner() {
      CallbackEnvironment environment;
      state.reset();
    }
  };
  CallbackEnvironment environment;
  try {
    auto owner = std::make_shared<Owner>();
    owner->state = std::move(state);
    return std::shared_ptr<const void>(owner, owner->state.get());
  } catch (...) {
    state.reset();
    throw;
  }
}
}  // namespace ps::core_internal
