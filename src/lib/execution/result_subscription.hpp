#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/** @brief Per-caller serialized observer for certified Result publications.
 * @details Owns a copy of the callback, caller cancellation token, and weak
 * revision watermarks keyed by logical step and Result object identity.
 * Initialize before concurrent delivery. Notifications are serialized;
 * non-increasing revisions for the same step and object are suppressed, while
 * a different object at the same step has an independent revision stream.
 * Each notification reclaims at most eight expired weak watermark entries.
 * Callback exceptions are converted to Status failures.
 * @note A cancelled caller receives no notification admitted after its token is
 * observed. `close()` rejects new deliveries and waits for an admitted callback
 * to finish, then releases callback captures outside the mutex. Do not re-enter
 * `notify` or `close` from the callback.
 */
class ResultSubscription final {
 public:
  using Callback = std::function<Status(ValueRef, const ResultRef&)>;
  ResultSubscription(const ResourceBudget& resources,
                     CancellationToken cancellation, Callback callback)
      : cancellation_(std::move(cancellation)),
        callback_(std::move(callback)),
        revisions_(std::less<Key>{}, ResourceAllocator<Revision>(resources)),
        cleanup_(revisions_.end()) {}

  void initialize(std::size_t steps) { steps_ = steps; }

  bool has_callback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(callback_);
  }

  bool enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !closed_ && callback_ && failure_.ok() && !cancellation_.cancelled();
  }

  Status status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_;
  }

  ErrorCode code() const noexcept {
    return failure_code_.load(std::memory_order_relaxed);
  }

  Status notify(std::size_t index, ValueRef output, const ResultRef& result,
                std::uint64_t revision) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return closed_ || !active_; });
    if (closed_ || !callback_ || !failure_.ok() || cancellation_.cancelled())
      return Status::success();
    if (index >= steps_)
      return Status{ErrorCode::Internal, {}};
    const Key key{index, result.object_id()};
    const auto scanned = std::min<std::size_t>(8, revisions_.size());
    for (std::size_t i = 0; i < scanned; ++i) {
      if (cleanup_ == revisions_.end())
        cleanup_ = revisions_.begin();
      auto entry = cleanup_++;
      if (entry->first != key && !entry->second.object.lock().valid())
        revisions_.erase(entry);
    }
    auto previous = revisions_.find(key);
    if (previous != revisions_.end() && revision <= previous->second.revision)
      return Status::success();
    revisions_.insert_or_assign(key, Watermark{result.weak(), revision});
    active_ = true;
    lock.unlock();
    Status delivered;
    try {
      delivered = callback_(output, result);
    } catch (const std::bad_alloc&) {
      delivered = Status{ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      delivered = Status{ErrorCode::OperationFailed, {}};
    }
    lock.lock();
    failure_ = std::move(delivered);
    failure_code_.store(failure_.code, std::memory_order_relaxed);
    active_ = false;
    changed_.notify_all();
    return failure_;
  }

  void close() {
    std::unique_lock<std::mutex> lock(mutex_);
    closed_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return !active_; });
    auto retired = std::move(callback_);
    lock.unlock();
  }

 private:
  CancellationToken cancellation_;
  Callback callback_;
  using Key = std::pair<std::size_t, std::uint64_t>;
  struct Watermark {
    WeakResultRef object;
    std::uint64_t revision;
  };
  using Revision = std::pair<const Key, Watermark>;
  using Revisions =
      std::map<Key, Watermark, std::less<Key>, ResourceAllocator<Revision>>;
  Revisions revisions_;
  Revisions::iterator cleanup_;
  std::size_t steps_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Status failure_;
  std::atomic<ErrorCode> failure_code_{ErrorCode::Ok};
  bool closed_ = false, active_ = false;
};
}  // namespace ps::execution_internal
