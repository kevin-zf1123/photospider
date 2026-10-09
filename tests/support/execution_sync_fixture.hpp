#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <utility>

#include "execution/execution_test_hooks.hpp"

namespace ps::test {
// Declare after futures and before launching them. Unwinding releases the gate
// before future destruction can wait for an asynchronous callback.
template <class Release>
class OnExit final {
 public:
  explicit OnExit(Release release) : release_(std::move(release)) {}
  ~OnExit() { release_(); }
  OnExit(const OnExit&) = delete;
  OnExit& operator=(const OnExit&) = delete;

 private:
  Release release_;
};

// Borrowed hooks must outlive the last callback, including timeout cleanup.
class ExecutionHookScope final {
 public:
  explicit ExecutionHookScope(
      const execution_testing::ExecutionTestHooks& hooks)
      : hooks_(hooks) {
    execution_testing::install_execution_test_hooks(&hooks_);
  }
  ~ExecutionHookScope() {
    execution_testing::install_execution_test_hooks(nullptr);
  }
  ExecutionHookScope(const ExecutionHookScope&) = delete;
  ExecutionHookScope& operator=(const ExecutionHookScope&) = delete;

 private:
  execution_testing::ExecutionTestHooks hooks_;
};

class SharedJoinEvent final {
 public:
  SharedJoinEvent(std::string_view operation, std::uint64_t node)
      : operation_(operation), node_(node), hooks_(callbacks(this)) {}
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5),
                             [&] { return joined_; });
  }

 private:
  static execution_testing::ExecutionTestHooks callbacks(
      SharedJoinEvent* self) {
    execution_testing::ExecutionTestHooks hooks;
    hooks.structured_context = self;
    hooks.shared_joined = [](void* context, std::string_view operation,
                             std::uint64_t node, const void*) noexcept {
      auto& event = *static_cast<SharedJoinEvent*>(context);
      if (operation != event.operation_ || node != event.node_)
        return;
      std::lock_guard<std::mutex> lock(event.mutex_);
      event.joined_ = true;
      event.changed_.notify_all();
    };
    return hooks;
  }
  std::string_view operation_;
  std::uint64_t node_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool joined_ = false;
  ExecutionHookScope hooks_;
};

class PhaseWorkGate final {
 public:
  PhaseWorkGate(std::string_view operation, std::uint64_t checkpoint)
      : operation_(operation),
        checkpoint_(checkpoint),
        hooks_(callbacks(this)) {}
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5),
                             [&] { return entered_; });
  }
  void release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }
  bool cancelled_in_service() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entered_ && !timed_out_ && entries_ == 1 && progressed_ &&
           finished_ && result_ == ErrorCode::Cancelled;
  }

 private:
  using Point = execution_testing::PhaseWorkPoint;
  static execution_testing::ExecutionTestHooks callbacks(PhaseWorkGate* self) {
    execution_testing::ExecutionTestHooks hooks;
    hooks.structured_context = self;
    hooks.phase_work_started = [](void* context, const Point& point) noexcept {
      static_cast<PhaseWorkGate*>(context)->before(point);
    };
    hooks.phase_work_finished = [](void* context, const Point& point,
                                   ErrorCode code) noexcept {
      static_cast<PhaseWorkGate*>(context)->after(point, code);
    };
    return hooks;
  }
  bool selected(const Point& point) const {
    return point.operation == operation_ && point.backend == Backend::Cpu;
  }
  void before(const Point& point) noexcept {
    if (!selected(point) || point.ordinal != checkpoint_)
      return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (entered_)
      return;
    ++entries_;
    entered_ = true;
    actor_ = point.actor;
    poll_ = point.poll;
    progressed_ = last_actor_ == actor_ && last_poll_ == poll_ &&
                  last_ordinal_ + 1 == checkpoint_ &&
                  last_result_ == ErrorCode::Ok;
    changed_.notify_all();
    if (!changed_.wait_for(lock, std::chrono::seconds(15),
                           [&] { return released_; }))
      timed_out_ = true;
  }
  void after(const Point& point, ErrorCode code) noexcept {
    if (!selected(point))
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (entered_ && !finished_ && point.actor == actor_ &&
        point.poll == poll_ && point.ordinal == checkpoint_) {
      finished_ = true;
      result_ = code;
    }
    last_actor_ = point.actor;
    last_poll_ = point.poll;
    last_ordinal_ = point.ordinal;
    last_result_ = code;
  }
  std::string_view operation_;
  std::uint64_t checkpoint_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false, released_ = false, timed_out_ = false;
  bool progressed_ = false, finished_ = false;
  unsigned entries_ = 0;
  const void* actor_ = nullptr;
  const void* last_actor_ = nullptr;
  std::uint32_t poll_ = 0, last_poll_ = 0;
  std::uint64_t last_ordinal_ = 0;
  ErrorCode result_ = ErrorCode::Internal, last_result_ = ErrorCode::Internal;
  ExecutionHookScope hooks_;
};
}  // namespace ps::test
