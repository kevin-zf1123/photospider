#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"

namespace ps::execution_internal {
/** @brief Context-local active producers and weak completed required objects.
 * Keys include one frozen input-bundle identity and the static result contract.
 * No source callback is shared across separately captured mutable sources.
 */
class SharedResults final {
 public:
  ~SharedResults() noexcept { shutdown(); }
  struct Call;
  struct Entry;
  struct Group {
    explicit Group(const ResourceBudget& budget) : stop(budget) {}
    ResourceLease lease;
    std::recursive_mutex mutex;
    std::array<std::weak_ptr<Call>, 64> calls;
    CancellationSource stop;
    void refresh();
    bool interested(std::string_view key, const Call* except = nullptr);
  };
  struct Call {
    ResourceLease lease;
    std::shared_ptr<Group> group;
    CancellationToken caller;
    ResourceVector<ResourceString> keys;
    std::atomic<bool> done{false};
    ~Call() {
      if (group)
        group->refresh();
    }
  };
  class RunLease {
   public:
    RunLease() = default;
    void refresh() const {
      if (call_)
        call_->group->refresh();
    }
    void retire_user() const {
      if (call_) {
        call_->done = true;
        call_->group->refresh();
      }
    }

   private:
    friend class SharedResults;
    std::shared_ptr<Call> call_;
  };
  /** @brief Raw caller interests for one frozen input scope; no producer
   * token enters this cancellation calculation, so it cannot self-sustain.
   */
  Result<RunLease> join_call(std::string_view key, const ResourceBudget& budget,
                             CancellationToken caller,
                             ResourceVector<ResourceString> keys) {
    using Answer = Result<RunLease>;
    std::lock_guard<std::mutex> table_lock(mutex_);
    if (!shutdown_) {
      shutdown_.emplace(budget);
      entries_ = make_resource_map<std::shared_ptr<Entry>>(budget);
      groups_ = make_resource_map<std::shared_ptr<Group>>(budget);
    }
    if (stopping_)
      return Answer(Status{ErrorCode::Cancelled, {}});
    for (auto it = groups_.begin(); it != groups_.end();) {
      it->second->refresh();
      if (it->second->stop.token().cancelled())
        it = groups_.erase(it);
      else
        ++it;
    }
    auto found = groups_.find(key);
    auto group =
        found == groups_.end() ? std::shared_ptr<Group>{} : found->second;
    std::unique_lock<std::recursive_mutex> group_lock;
    if (group) {
      group_lock = std::unique_lock<std::recursive_mutex>(group->mutex);
      if (group->stop.token().cancelled()) {
        group_lock.unlock();
        group.reset();
      }
    }
    if (!group) {
      auto bytes = sizeof(Group);
      auto capacity = ResourceCapacity::host(bytes, bytes);
      capacity[ResourceKind::Entries] = 1;
      auto lease = budget.reserve(capacity);
      if (!lease.ok())
        return Answer(lease.status());
      group = std::make_shared<Group>(budget);
      group->lease = lease.take_value();
      group_lock = std::unique_lock<std::recursive_mutex>(group->mutex);
    }
    auto slot = std::find_if(group->calls.begin(), group->calls.end(),
                             [](const auto& c) { return c.expired(); });
    if (slot == group->calls.end())
      return Answer(Status{ErrorCode::ResourceExhausted, "frozen call limit"});
    ResourceVector<ResourceString> owned_keys{
        ResourceAllocator<ResourceString>(budget)};
    owned_keys.reserve(keys.size());
    for (const auto& item : keys)
      owned_keys.emplace_back(item.data(), item.size(),
                              ResourceAllocator<char>(budget));
    std::uint64_t bytes = sizeof(Call);
    auto capacity = ResourceCapacity::host(bytes, bytes);
    capacity[ResourceKind::Entries] = 1;
    auto lease = budget.reserve(capacity);
    if (!lease.ok())
      return Answer(lease.status());
    auto call = std::shared_ptr<Call>(new Call());
    call->lease = lease.take_value();
    call->group = group;
    call->caller = std::move(caller);
    call->keys = std::move(owned_keys);
    *slot = call;
    groups_.insert_or_assign(
        ResourceString(key.data(), key.size(), ResourceAllocator<char>(budget)),
        group);
    RunLease result;
    result.call_ = std::move(call);
    return Answer(std::move(result));
  }
  struct Waiter;
  struct Entry {
    Entry(const ResourceBudget& budget, std::string_view identity)
        : producer_stop(budget),
          key(identity.data(), identity.size(),
              ResourceAllocator<char>(budget)) {}
    ResourceLease lease;
    mutable std::recursive_mutex mutex;
    std::condition_variable_any changed;
    std::array<std::weak_ptr<Waiter>, 64> waiters;
    CancellationSource producer_stop;
    std::shared_ptr<Group> group;
    ResourceString key;
    ResultRef published;
    WeakResultRef completed;
    ErrorCode failure = ErrorCode::Ok;
    bool finished = false;
    void refresh_locked();
  };
  struct Waiter {
    ResourceLease lease;
    std::shared_ptr<Entry> entry;
    CancellationToken cancellation;
    std::shared_ptr<Call> call;
    ~Waiter() {
      if (!entry)
        return;
      std::lock_guard<std::recursive_mutex> lock(entry->mutex);
      // shared_ptr's strong count is already zero, so this slot is expired.
      entry->refresh_locked();
      entry->changed.notify_all();
    }
  };
  class Lease final {
   public:
    Lease() = default;
    bool valid() const noexcept { return waiter_ != nullptr; }
    bool producer() const noexcept { return producer_; }
    CancellationToken token() const { return token_; }
    void refresh() const {
      if (!waiter_)
        return;
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      waiter_->entry->refresh_locked();
    }
    bool has_other_waiters() const {
      if (!waiter_)
        return false;
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      return waiter_->entry->group->interested(waiter_->entry->key,
                                               waiter_->call.get());
    }
    /** @brief Closes admission atomically when this coordinator has no peers.
     */
    bool continue_for_peers() const {
      if (!waiter_ || !producer_)
        return false;
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      if (waiter_->entry->group->interested(waiter_->entry->key,
                                            waiter_->call.get()))
        return true;
      waiter_->entry->producer_stop.cancel();
      waiter_->entry->changed.notify_all();
      return false;
    }
    /** @brief Publishes monotone coverage; old entries never erase new epochs.
     */
    void publish(const ResultRef& result, bool complete) const {
      if (!waiter_ || !producer_)
        return;
      auto& entry = *waiter_->entry;
      std::lock_guard<std::recursive_mutex> lock(entry.mutex);
      if (entry.finished)
        return;
      entry.published = result;
      if (complete) {
        entry.completed = result.weak();
        entry.finished = true;
      }
      entry.changed.notify_all();
    }
    void fail(ErrorCode code) const {
      if (!waiter_ || !producer_)
        return;
      auto& entry = *waiter_->entry;
      std::lock_guard<std::recursive_mutex> lock(entry.mutex);
      if (!entry.finished) {
        entry.finished = true;
        entry.failure = code == ErrorCode::Ok ? ErrorCode::Internal : code;
        entry.changed.notify_all();
      }
    }
    Result<ResultRef> wait(bool complete, std::uint32_t field,
                           std::uint64_t minimum_rows,
                           const CancellationToken& cancellation,
                           const std::function<Status()>& pump = {}) const {
      if (!waiter_)
        return Result<ResultRef>(Status{ErrorCode::Stale, {}});
      auto& entry = *waiter_->entry;
      std::unique_lock<std::recursive_mutex> lock(entry.mutex);
      for (;;) {
        entry.refresh_locked();
        if (cancellation.cancelled())
          return Result<ResultRef>(Status{ErrorCode::Cancelled, {}});
        auto result =
            entry.published.valid() ? entry.published : entry.completed.lock();
        if (result.valid()) {
          auto descriptor = result.descriptor(complete);
          if (descriptor.ok() &&
              (complete || (field < descriptor.value().field_count() &&
                            (descriptor.value().sealed() ||
                             descriptor.value().rows(field) >= minimum_rows))))
            return Result<ResultRef>(std::move(result));
        }
        if (entry.finished)
          return Result<ResultRef>(Status{entry.failure == ErrorCode::Ok
                                              ? ErrorCode::NotFound
                                              : entry.failure,
                                          {}});
        if (token_.cancelled())
          return Result<ResultRef>(Status{ErrorCode::Cancelled, {}});
        if (pump) {
          lock.unlock();
          auto status = pump();
          lock.lock();
          if (!status.ok())
            return Result<ResultRef>(status);
        }
        entry.changed.wait_for(lock, std::chrono::milliseconds(2));
      }
    }

   private:
    friend class SharedResults;
    std::shared_ptr<Waiter> waiter_;
    CancellationToken token_;
    bool producer_ = false;
  };
  Result<Lease> acquire(std::string_view key, const ResourceBudget& budget,
                        CancellationToken cancellation, const RunLease& run) {
    using Answer = Result<Lease>;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !run.call_)
      return Answer(Status{ErrorCode::Cancelled, {}});
    // Dead weak entries are expendable metadata, never mandatory data owners.
    for (auto it = entries_.begin(); it != entries_.end();) {
      auto entry = it->second;
      bool expired = false;
      {
        std::lock_guard<std::recursive_mutex> entry_lock(entry->mutex);
        entry->refresh_locked();
        expired =
            (entry->finished || entry->producer_stop.token().cancelled()) &&
            !entry->published.valid() && !entry->completed.lock().valid();
      }
      if (expired)
        it = entries_.erase(it);
      else
        ++it;
    }
    auto found = entries_.find(key);
    auto entry =
        found == entries_.end() ? std::shared_ptr<Entry>{} : found->second;
    bool producer = !entry;
    std::unique_lock<std::recursive_mutex> admission_lock;
    if (entry) {
      admission_lock = std::unique_lock<std::recursive_mutex>(entry->mutex);
      producer = entry->failure != ErrorCode::Ok ||
                 (!entry->finished && entry->producer_stop.token().cancelled());
    }
    if (producer) {
      if (admission_lock.owns_lock())
        admission_lock.unlock();
      auto capacity = ResourceCapacity::host(sizeof(Entry), sizeof(Entry));
      capacity[ResourceKind::Entries] = 1;
      auto lease = budget.reserve(capacity);
      if (!lease.ok())
        return Answer(lease.status());
      entry = std::make_shared<Entry>(budget, key);
      entry->lease = lease.take_value();
      entry->group = run.call_->group;
      admission_lock = std::unique_lock<std::recursive_mutex>(entry->mutex);
    }
    auto capacity = ResourceCapacity::host(sizeof(Waiter), sizeof(Waiter));
    capacity[ResourceKind::Entries] = 1;
    auto admitted = budget.reserve(capacity);
    if (!admitted.ok())
      return Answer(admitted.status());
    // Separate allocation permits expiry while the entry retains a weak slot.
    auto waiter = std::shared_ptr<Waiter>(new Waiter());
    waiter->lease = admitted.take_value();
    waiter->entry = entry;
    waiter->cancellation = std::move(cancellation);
    waiter->call = run.call_;
    {
      std::lock_guard<std::recursive_mutex> entry_lock(entry->mutex);
      auto slot = std::find_if(entry->waiters.begin(), entry->waiters.end(),
                               [](const auto& weak) { return weak.expired(); });
      if (slot == entry->waiters.end())
        return Answer(
            Status{ErrorCode::ResourceExhausted, "shared result waiter limit"});
      *slot = waiter;
    }
    auto combined = CancellationToken::combine(
        {entry->producer_stop.token(), shutdown_->token()}, budget);
    if (!combined.ok())
      return Answer(combined.status());
    if (producer)
      entries_.insert_or_assign(ResourceString(key.data(), key.size(),
                                               ResourceAllocator<char>(budget)),
                                entry);
    Lease lease;
    lease.waiter_ = std::move(waiter);
    lease.producer_ = producer;
    lease.token_ = combined.take_value();
    return Answer(std::move(lease));
  }
  void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    if (shutdown_)
      shutdown_->cancel();
    for (const auto& group : groups_)
      group.second->stop.cancel();
    for (const auto& item : entries_) {
      std::lock_guard<std::recursive_mutex> entry_lock(item.second->mutex);
      item.second->producer_stop.cancel();
      item.second->changed.notify_all();
    }
  }

 private:
  std::mutex mutex_;
  bool stopping_ = false;
  std::optional<CancellationSource> shutdown_;
  ResourceMap<std::shared_ptr<Entry>> entries_;
  ResourceMap<std::shared_ptr<Group>> groups_;
};
inline void SharedResults::Group::refresh() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  bool alive = false;
  for (const auto& slot : calls) {
    auto call = slot.lock();
    alive |= call && !call->done.load() && !call->caller.cancelled();
  }
  if (!alive)
    stop.cancel();
}
inline bool SharedResults::Group::interested(std::string_view key,
                                             const Call* except) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  for (const auto& slot : calls) {
    auto call = slot.lock();
    if (call && call.get() != except && !call->done.load() &&
        !call->caller.cancelled() &&
        std::binary_search(call->keys.begin(), call->keys.end(), key,
                           ResourceStringLess{}))
      return true;
  }
  return false;
}
inline void SharedResults::Entry::refresh_locked() {
  bool alive = group && group->interested(key), attached = false;
  for (const auto& slot : waiters) {
    auto waiter = slot.lock();
    attached |= static_cast<bool>(waiter);
  }
  if (!alive)
    producer_stop.cancel();
  if (!attached)
    published = {};
}
}  // namespace ps::execution_internal
