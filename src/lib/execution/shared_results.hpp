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

#include "core/stored_failure.hpp"
#include "execution/resource_observation.hpp"
#include "photospider/data/quality.hpp"
#include "photospider/data/result.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps::execution_internal {
/** @brief Coordinates compatible producer calls within one frozen input scope.
 * Each caller contributes direct result keys. Active producer dependencies
 * propagate those caller interests to the exact child keys requested by the
 * producer, so an upstream producer remains useful while a live caller needs
 * it. The table also keeps weak references to completed required objects. Keys
 * include the frozen input-bundle identity and static result contract; source
 * callbacks from separately captured mutable sources remain independent.
 * @details The table can also retain a parked structured coordinator while a
 * live peer needs its producer. Peer polling drives registered coordinators on
 * the polling thread; it does not create a worker. Context shutdown cancels
 * shared work and drains retained drivers after releasing the table lock.
 */
class SharedResults final {
 public:
  ~SharedResults() noexcept { shutdown(); }
  /** @brief A resumable coordinator retained for peer-driven producer work.
   * @details `drive` advances on the calling thread and reports completion;
   * `drain` retires remaining callbacks and state before the registry releases
   * its strong owner.
   */
  struct Driver {
    virtual ~Driver() = default;
    virtual bool drive(bool drain_unneeded) = 0;
    virtual void drain() noexcept = 0;
  };
  /** @brief Registers a strong driver owner while the context admits work.
   * @return True when retained; false after shutdown has begun.
   * @throws std::bad_alloc If registry storage cannot admit the driver.
   * @note Failed adoption leaves the caller responsible for synchronous drain.
   */
  bool adopt(std::shared_ptr<Driver> driver) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !shutdown_)
      return false;
    for (auto& slot : drivers_)
      if (!slot) {
        slot = std::move(driver);
        return true;
      }
    drivers_.push_back(std::move(driver));
    return true;
  }
  /** @brief Lets the calling thread advance currently registered drivers.
   * @details The table lock is released before calling `drive`. Ordinary peer
   * polling uses each driver's nonblocking acquisition; `drain_unneeded`
   * permits a driver to wait while retiring work with no remaining peer.
   */
  void pump(bool drain_unneeded = false) {
    std::size_t size;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      size = drivers_.size();
    }
    for (std::size_t i = 0; i < size; ++i) {
      std::shared_ptr<Driver> driver;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (i >= drivers_.size())
          break;
        driver = drivers_[i];
      }
      if (!driver || !driver->drive(drain_unneeded))
        continue;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (i < drivers_.size() && drivers_[i] == driver)
          drivers_[i].reset();
      }
    }
  }
  /** @brief Removes this registry's strong owner for a driver.
   * @note The caller remains responsible for its own lifetime and drain.
   */
  void withdraw(const Driver* driver) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : drivers_)
      if (slot.get() == driver)
        slot.reset();
  }
  struct Call;
  struct Entry;
  struct Counters {
    ResourceLease lease;
    std::mutex mutex;
    std::uint64_t active = 0, shared = 0;
  };
  /** @brief Caller-interest and dependency state for one frozen input scope.
   * @details Up to 64 call slots contribute direct result keys. Interested
   * callers exclude completed or cancelled calls and an optional querying call;
   * producer cancellation tokens do not create caller interest. Dependency
   * edges belong to a specific producer entry epoch. Completion, failure, or
   * producer lease retirement ends that epoch and removes its edges; slot reuse
   * clears the former caller's bits before admitting the replacement call.
   */
  struct Group {
    struct Node {
      explicit Node(const ResourceBudget& budget)
          : children(ResourceAllocator<Node*>(budget)) {}
      ResourceVector<Node*> children;
      std::uint64_t callers = 0, direct = 0;
      const Entry* producer = nullptr;
      Node* next = nullptr;
      bool queued = false;
    };
    explicit Group(const ResourceBudget& budget)
        : resources(budget),
          nodes(make_resource_map<Node>(budget)),
          stop(budget) {}
    ResourceLease lease;
    std::recursive_mutex mutex;
    std::array<std::weak_ptr<Call>, 64> calls;
    ResourceBudget resources;
    ResourceMap<Node> nodes;
    CancellationSource stop;
    void refresh();
    /** @brief Tests live raw caller interest without allocating. */
    bool interested(std::string_view key, const Call* except = nullptr);
    Result<Node*> node(std::string_view key);
    Status spread(Node* node, std::uint64_t callers);
    Status add_interest(std::size_t slot, std::string_view key);
    /** @brief Binds a producer epoch to its key and starts dependency tracking.
     * @details Replacing an epoch first removes the prior epoch's dependency
     * edges and recomputes caller interest.
     */
    Status begin_producer(std::string_view key, const Entry* producer);
    /** @brief Removes dependency edges only when `producer` is the current
     * epoch for `key`; stale epochs cannot clear a replacement's edges.
     * @details Interest is recomputed with the group's intrusive queue, without
     * allocating during producer retirement.
     */
    void end_producer(std::string_view key, const Entry* producer);
    void rebuild_interests();
    /** @brief Adds an exact child dependency for the current producer epoch.
     * @details Admission accounts for scratch work before committing caller
     * bits or the edge. A failed admission leaves no partial propagation or
     * dependency edge.
     */
    Status add_dependency(std::string_view parent, std::string_view child,
                          const Entry* producer);
    void clear_slot(std::size_t slot);
  };
  struct Call {
    ResourceLease lease;
    std::shared_ptr<Group> group;
    CancellationToken caller;
    ResourceVector<ResourceString> keys;
    std::size_t slot = 0;
    std::atomic<bool> done{false};
    ~Call() {
      if (group)
        group->refresh();
    }
  };
  /** @brief One caller's raw interests in a frozen input scope.
   * @details A lease records up to one group slot's direct keys. Producer
   * leases do not themselves root caller interest, preventing self-sustaining
   * dependency chains.
   */
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
    /** @brief Adds a direct key interest for this caller. */
    Status add_interest(std::string_view key,
                        const ResourceBudget& budget) const {
      if (!call_)
        return Status::success();
      std::lock_guard<std::recursive_mutex> lock(call_->group->mutex);
      auto found = std::lower_bound(call_->keys.begin(), call_->keys.end(), key,
                                    ResourceStringLess{});
      if (found == call_->keys.end() || std::string_view(*found) != key)
        call_->keys.insert(found,
                           ResourceString(key.data(), key.size(),
                                          ResourceAllocator<char>(budget)));
      return call_->group->add_interest(call_->slot, key);
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
    if (stopping_)
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (!counters_) {
      auto lease = budget.reserve(
          ResourceCapacity::host(sizeof(Counters), sizeof(Counters)));
      if (!lease.ok())
        return Answer(lease.status());
      auto counters = std::make_shared<Counters>();
      counters->lease = lease.take_value();
      counters_ = std::move(counters);
    }
    if (!shutdown_) {
      shutdown_.emplace(budget);
      entries_ = make_resource_map<std::shared_ptr<Entry>>(budget);
      groups_ = make_resource_map<std::shared_ptr<Group>>(budget);
      drivers_ = ResourceVector<std::shared_ptr<Driver>>(
          ResourceAllocator<std::shared_ptr<Driver>>(budget));
    }
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
    call->slot = static_cast<std::size_t>(slot - group->calls.begin());
    auto cleared = group->resources.consume({group->nodes.size()});
    if (!cleared.ok())
      return Answer(cleared);
    group->clear_slot(call->slot);
    *slot = call;
    for (const auto& key : call->keys) {
      auto interested = group->add_interest(call->slot, key);
      if (!interested.ok())
        return Answer(interested);
    }
    groups_.insert_or_assign(
        ResourceString(key.data(), key.size(), ResourceAllocator<char>(budget)),
        group);
    RunLease result;
    result.call_ = std::move(call);
    return Answer(std::move(result));
  }
  // Counter owners outlive an epoch without retaining its waiters or driver.
  struct ProducerPayload {
    PayloadObservation own;
    mutable std::mutex mutex;
    std::shared_ptr<PayloadObservation> joint;
    std::uint64_t peak() const noexcept {
      std::lock_guard<std::mutex> lock(mutex);
      return std::max(own.peaks().first, joint ? joint->peaks().first : 0);
    }
  };
  struct Waiter;
  struct Entry {
    Entry(const ResourceBudget& budget, std::string_view identity)
        : payload(std::make_shared<ProducerPayload>()),
          producer_stop(budget),
          key(identity.data(), identity.size(),
              ResourceAllocator<char>(budget)) {}
    ~Entry() { finish_count(); }
    void finish_count() noexcept {
      if (counted) {
        std::lock_guard<std::mutex> lock(counters->mutex);
        --counters->active;
        counted = false;
      }
    }
    std::shared_ptr<Counters> counters;
    bool counted = false;
    ResourceLease lease;
    mutable std::recursive_mutex mutex;
    std::condition_variable_any changed;
    std::array<std::weak_ptr<Waiter>, 64> waiters;
    std::shared_ptr<ProducerPayload> payload;
    CancellationSource producer_stop;
    std::shared_ptr<Group> group;
    ResourceString key;
    ResultRef published;
    WeakResultRef completed;
    core_internal::StoredFailure failure;
    /** Quality evidence remains attached to this shared producer entry. */
    std::optional<QualityReport> quality;
    Backend backend = Backend::Cpu;
    bool fallback_taint = false;
    bool finished = false;
    void refresh_locked();
  };
  struct Waiter {
    ResourceLease lease;
    std::shared_ptr<Entry> entry;
    CancellationToken cancellation;
    std::shared_ptr<Call> call;
    bool producer = false;
    ~Waiter() {
      if (!entry)
        return;
      std::lock_guard<std::recursive_mutex> lock(entry->mutex);
      if (producer)
        entry->finish_count();
      if (producer)
        entry->group->end_producer(entry->key, entry.get());
      // shared_ptr's strong count is already zero, so this slot is expired.
      entry->refresh_locked();
      entry->changed.notify_all();
    }
  };
  /** @brief Access to one shared producer entry or its published results.
   * @details Producer leases bind exact child dependencies to their Entry
   * epoch; waiter leases observe the same entry without extending raw caller
   * interest.
   */
  class Lease final {
   public:
    Lease() = default;
    bool valid() const noexcept { return waiter_ != nullptr; }
    bool producer() const noexcept { return producer_; }
    CancellationToken token() const { return token_; }
    std::shared_ptr<PayloadObservation> payload_observation() const {
      if (!waiter_ || !producer_)
        return {};
      auto payload = waiter_->entry->payload;
      std::lock_guard<std::mutex> lock(payload->mutex);
      return payload->joint
                 ? payload->joint
                 : std::shared_ptr<PayloadObservation>(payload, &payload->own);
    }
    std::shared_ptr<ProducerPayload> active_payload() const {
      return waiter_ && active_epoch_ ? waiter_->entry->payload : nullptr;
    }
    void observe_joint_payload(
        const std::shared_ptr<PayloadObservation>& observation) const {
      if (!waiter_ || !producer_)
        return;
      auto payload = waiter_->entry->payload;
      std::lock_guard<std::mutex> lock(payload->mutex);
      // An Actor joins at most one group; release disables joint re-admission.
      payload->joint = observation;
    }
    void refresh() const {
      if (!waiter_)
        return;
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      waiter_->entry->refresh_locked();
    }
    /** @brief Adds an exact child key to this producer epoch's dependencies.
     * @details A nonproducer lease has no dependency authority. The group
     * admits propagation work before committing the edge and caller bits.
     */
    Status add_dependency(std::string_view child) const {
      if (!waiter_ || !producer_)
        return Status::success();
      return waiter_->entry->group->add_dependency(waiter_->entry->key, child,
                                                   waiter_->entry.get());
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
     * @details The shared entry retains any quality report with its published
     * Result so waiters observe the same evidence. The completed-output content
     * cache separately skips quality-bearing results.
     */
    void publish(const ResultRef& result, bool complete, Backend backend,
                 bool fallback_taint = false,
                 std::optional<QualityReport> quality = {}) const {
      if (!waiter_ || !producer_)
        return;
      auto& entry = *waiter_->entry;
      std::lock_guard<std::recursive_mutex> lock(entry.mutex);
      if (entry.finished)
        return;
      entry.published = result;
      entry.quality = std::move(quality);
      entry.backend = backend;
      entry.fallback_taint = entry.fallback_taint || fallback_taint;
      if (complete) {
        entry.completed = result.weak();
        entry.finished = true;
        entry.finish_count();
        entry.group->end_producer(entry.key, &entry);
      }
      entry.changed.notify_all();
    }
    Backend backend() const {
      if (!waiter_)
        return Backend::Cpu;
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      return waiter_->entry->backend;
    }
    bool fallback_taint() const {
      if (!waiter_)
        return false;
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      return waiter_->entry->fallback_taint;
    }
    std::optional<QualityReport> quality() const {
      if (!waiter_)
        return {};
      std::lock_guard<std::recursive_mutex> lock(waiter_->entry->mutex);
      return waiter_->entry->quality;
    }
    /** @brief Retains the producer failure and its associated quality evidence.
     * Waiters read both from this entry; producer epochs remain independent.
     */
    void fail(const Status& status,
              std::optional<QualityReport> quality = {}) const {
      if (!waiter_ || !producer_)
        return;
      auto& entry = *waiter_->entry;
      std::lock_guard<std::recursive_mutex> lock(entry.mutex);
      if (!entry.finished) {
        entry.finished = true;
        entry.finish_count();
        entry.failure.record(status);
        entry.quality = std::move(quality);
        entry.group->end_producer(entry.key, &entry);
        entry.changed.notify_all();
      }
    }
    /** @brief Checks for a requested publication with a bounded wait.
     * @return The matching object, an empty optional after up to 2 ms, or an
     * error.
     */
    Result<std::optional<ResultRef>> poll(
        bool complete, std::uint32_t field, std::uint64_t minimum_rows,
        const CancellationToken& cancellation) const {
      using Answer = Result<std::optional<ResultRef>>;
      if (!waiter_)
        return Answer(Status{ErrorCode::Stale, {}});
      auto& entry = *waiter_->entry;
      std::unique_lock<std::recursive_mutex> lock(entry.mutex);
      auto ready =
          inspect_locked(entry, complete, field, minimum_rows, cancellation);
      if (!ready.ok() || ready.value())
        return ready;
      entry.changed.wait_for(lock, std::chrono::milliseconds(2));
      return inspect_locked(entry, complete, field, minimum_rows, cancellation);
    }
    /** @brief Repeats `poll` until the requested object or an error is ready.
     * @note An optional pump runs after an empty poll and before the next poll.
     */
    Result<ResultRef> wait(bool complete, std::uint32_t field,
                           std::uint64_t minimum_rows,
                           const CancellationToken& cancellation,
                           const std::function<Status()>& pump = {}) const {
      for (;;) {
        auto ready = poll(complete, field, minimum_rows, cancellation);
        if (!ready.ok())
          return Result<ResultRef>(ready.status());
        if (ready.value())
          return Result<ResultRef>(std::move(*ready.value()));
        if (pump) {
          auto status = pump();
          if (!status.ok())
            return Result<ResultRef>(status);
        }
      }
    }

   private:
    Result<std::optional<ResultRef>> inspect_locked(
        Entry& entry, bool complete, std::uint32_t field,
        std::uint64_t minimum_rows,
        const CancellationToken& cancellation) const {
      entry.refresh_locked();
      if (entry.finished && !entry.failure.ok() &&
          entry.failure.origin() == FailureOrigin::Protocol)
        return Result<std::optional<ResultRef>>(entry.failure.status());
      if (cancellation.cancelled())
        return Result<std::optional<ResultRef>>(
            Status{ErrorCode::Cancelled,
                   {},
                   FailureReason::Cancelled,
                   {FailureOrigin::Cancellation, FailureScope::Waiter}});
      auto result =
          entry.published.valid() ? entry.published : entry.completed.lock();
      if (result.valid()) {
        auto descriptor = result.descriptor(complete);
        if (descriptor.ok() &&
            (complete || (field < descriptor.value().field_count() &&
                          (descriptor.value().sealed() ||
                           descriptor.value().rows(field) >= minimum_rows))))
          return Result<std::optional<ResultRef>>(
              std::optional<ResultRef>(std::move(result)));
      }
      if (entry.finished)
        return Result<std::optional<ResultRef>>(
            entry.failure.ok() ? Status{ErrorCode::NotFound, {}}
                               : entry.failure.status());
      if (token_.cancelled())
        return Result<std::optional<ResultRef>>(
            Status{ErrorCode::Cancelled, {}});
      return Result<std::optional<ResultRef>>(std::optional<ResultRef>{});
    }
    friend class SharedResults;
    std::shared_ptr<Waiter> waiter_;
    CancellationToken token_;
    bool producer_ = false;
    bool active_epoch_ = false;
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
      producer = !entry->failure.ok() ||
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
    waiter->producer = producer;
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
    if (producer) {
      auto begun = entry->group->begin_producer(key, entry.get());
      if (!begun.ok())
        return Answer(begun);
      entries_.insert_or_assign(ResourceString(key.data(), key.size(),
                                               ResourceAllocator<char>(budget)),
                                entry);
    }
    Lease lease;
    lease.waiter_ = std::move(waiter);
    lease.producer_ = producer;
    lease.active_epoch_ = !entry->finished;
    lease.token_ = combined.take_value();
    {
      std::lock_guard<std::mutex> counter_lock(counters_->mutex);
      if (producer) {
        entry->counters = counters_;
        entry->counted = true;
        ++counters_->active;
      } else if (counters_->shared != UINT64_MAX) {
        ++counters_->shared;
      }
    }
    return Answer(std::move(lease));
  }
  std::pair<std::uint64_t, std::uint64_t> statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!counters_)
      return {};
    std::lock_guard<std::mutex> counter_lock(counters_->mutex);
    return {counters_->active, counters_->shared};
  }
  /** @brief Stops admission, cancels shared work, then drains parked drivers.
   * @details The table lock is released before calling driver retirement code.
   */
  void shutdown() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
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
    auto drivers = std::move(drivers_);
    lock.unlock();
    for (const auto& driver : drivers)
      if (driver)
        driver->drain();
  }

 private:
  std::mutex mutex_;
  bool stopping_ = false;
  std::optional<CancellationSource> shutdown_;
  std::shared_ptr<Counters> counters_;
  ResourceMap<std::shared_ptr<Entry>> entries_;
  ResourceMap<std::shared_ptr<Group>> groups_;
  ResourceVector<std::shared_ptr<Driver>> drivers_;
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
  const auto found = nodes.find(key);
  if (found == nodes.end())
    return false;
  const auto mask = found->second.callers;
  for (std::size_t slot = 0; slot < calls.size(); ++slot) {
    if (!(mask & (std::uint64_t{1} << slot)))
      continue;
    auto call = calls[slot].lock();
    if (call && call.get() != except && !call->done.load() &&
        !call->caller.cancelled())
      return true;
  }
  return false;
}
inline Result<SharedResults::Group::Node*> SharedResults::Group::node(
    std::string_view key) {
  using Answer = Result<Node*>;
  auto found = nodes.find(key);
  if (found != nodes.end())
    return Answer(&found->second);
  auto charged = resources.consume({key.size() + 1});
  if (!charged.ok())
    return Answer(charged);
  auto inserted =
      nodes.emplace(ResourceString(key.data(), key.size(),
                                   ResourceAllocator<char>(resources)),
                    Node(resources));
  return Answer(&inserted.first->second);
}
inline Status SharedResults::Group::spread(Node* node, std::uint64_t callers) {
  if (!(callers & ~node->callers))
    return Status::success();
  ResourceVector<Node*> pending{ResourceAllocator<Node*>(resources)};
  pending.push_back(node);
  for (std::size_t position = 0; position < pending.size(); ++position) {
    auto* current = pending[position];
    auto charged = resources.consume({1 + current->children.size()});
    if (!charged.ok())
      return charged;
    for (auto* child : current->children) {
      if (!(callers & ~child->callers))
        continue;
      charged = resources.consume({pending.size() + 1});
      if (!charged.ok())
        return charged;
      if (std::find(pending.begin(), pending.end(), child) == pending.end())
        pending.push_back(child);
    }
  }
  // Commit only after all scratch allocation and cumulative work admission.
  for (auto* current : pending)
    current->callers |= callers;
  return Status::success();
}
inline Status SharedResults::Group::add_interest(std::size_t slot,
                                                 std::string_view key) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto target = node(key);
  if (!target.ok())
    return target.status();
  const auto mask = std::uint64_t{1} << slot;
  auto propagated = spread(target.value(), mask);
  if (propagated.ok())
    target.value()->direct |= mask;
  return propagated;
}
inline Status SharedResults::Group::begin_producer(std::string_view key,
                                                   const Entry* producer) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto target = node(key);
  if (!target.ok())
    return target.status();
  if (!target.value()->children.empty()) {
    target.value()->children.clear();
    rebuild_interests();
  }
  target.value()->producer = producer;
  return Status::success();
}
inline void SharedResults::Group::end_producer(std::string_view key,
                                               const Entry* producer) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  auto found = nodes.find(key);
  if (found == nodes.end() || found->second.producer != producer)
    return;
  found->second.producer = nullptr;
  if (!found->second.children.empty()) {
    found->second.children.clear();
    rebuild_interests();
  }
}
inline void SharedResults::Group::rebuild_interests() {
  Node* head = nullptr;
  Node* tail = nullptr;
  const auto enqueue = [&](Node* node) {
    if (node->queued)
      return;
    node->queued = true;
    node->next = nullptr;
    if (tail)
      tail->next = node;
    else
      head = node;
    tail = node;
  };
  for (auto& item : nodes) {
    auto& node = item.second;
    node.callers = node.direct;
    node.queued = false;
    node.next = nullptr;
  }
  for (auto& item : nodes)
    if (item.second.callers)
      enqueue(&item.second);
  // Retirement cannot allocate or fail admission. Each requeue adds new bits,
  // so even a cyclic graph reaches a fixed point in at most 64 visits per node.
  while (head) {
    auto* current = head;
    head = current->next;
    if (!head)
      tail = nullptr;
    current->queued = false;
    for (auto* child : current->children) {
      if (!(current->callers & ~child->callers))
        continue;
      child->callers |= current->callers;
      enqueue(child);
    }
  }
}
inline Status SharedResults::Group::add_dependency(std::string_view parent,
                                                   std::string_view child,
                                                   const Entry* producer) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (parent == child)
    return Status{ErrorCode::Cycle, {}};
  auto from = node(parent), to = node(child);
  if (!from.ok() || !to.ok())
    return !from.ok() ? from.status() : to.status();
  if (from.value()->producer != producer)
    return Status{ErrorCode::Cancelled, {}};
  auto& children = from.value()->children;
  auto charged = resources.consume({children.size() + 1});
  if (!charged.ok())
    return charged;
  const bool insert =
      std::find(children.begin(), children.end(), to.value()) == children.end();
  if (insert)
    children.reserve(children.size() + 1);
  auto propagated = spread(to.value(), from.value()->callers);
  if (!propagated.ok())
    return propagated;
  if (insert)
    children.push_back(to.value());
  return Status::success();
}
inline void SharedResults::Group::clear_slot(std::size_t slot) {
  const auto keep = ~(std::uint64_t{1} << slot);
  for (auto& item : nodes) {
    item.second.callers &= keep;
    item.second.direct &= keep;
  }
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
