#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "execution/demand_context.hpp"
#include "execution/demand_query.hpp"
#include "execution/execution_bindings.hpp"
#include "execution/execution_context_state.hpp"
#include "execution/tile_binding_changes.hpp"
#include "execution/tile_records.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/execution/execution.hpp"

namespace ps {
using execution_internal::demand_key;
using execution_internal::TileRecords;
namespace {
// Checked publication metadata weight; failure leaves the old generation
// intact.
Result<std::uint64_t> diagnostic_byte_sum(std::uint64_t left,
                                          std::uint64_t right) {
  if (!core_internal::can_add(left, right)) {
    return Result<std::uint64_t>(
        Status::failure(ErrorCode::ResourceExhausted,
                        "execution diagnostic byte count overflows uint64"));
  }
  return Result<std::uint64_t>(left + right);
}
}  // namespace

// Handle publications and call leases share one serialized coordinator. The
// raw context borrow is granted only by an active call lease. close waits for
// all such leases before clearing it; owning bundles retire outside this lock.
struct DemandHandle::Impl {
  struct Bundle {
    explicit Bundle(const ResourceBudget& root) : resources(root) {}
    ResourceBudget resources;
    ExecutionPlan plan;
    ExecutionBindings bindings;
  };
  struct Publication {
    DemandQuery query;
    std::map<std::string, TileRequest> dirty;
  };
  std::weak_ptr<execution_internal::DemandCoordinator> owner;
  std::shared_ptr<const DemandHandle::Impl::Bundle> bundle;
  std::shared_ptr<const TileRecords> records;
  std::atomic<std::uint64_t> generation{1};
  CancellationSource cancellation;
  DemandConfig config;
  std::uint64_t revision = 0, metadata_entries = 0;
  std::map<std::string, std::shared_ptr<const Publication>> publications;
};
namespace execution_internal {
/** @brief One context lifetime/publication lock; owns no threads or pixels. */
struct DemandCoordinator : std::enable_shared_from_this<DemandCoordinator> {
  std::mutex mutex;
  std::condition_variable changed;
  CancellationSource shutdown;
  bool closing = false;
  std::size_t calls = 0;
  std::uint64_t next = 1;
  std::uint32_t maximum_handles;
  std::uint64_t maximum_calls;
  ExecutionContext* context = nullptr;
  struct HandleEntry {
    std::weak_ptr<DemandHandle::Impl> handle;
    CancellationToken cancellation;
  };
  std::map<std::uint64_t, HandleEntry> handles;
  DemandCoordinator(std::uint32_t maximum_handles, std::uint64_t maximum_calls)
      : maximum_handles(maximum_handles), maximum_calls(maximum_calls) {}
  struct Lease {
    std::shared_ptr<DemandCoordinator> owner;
    std::shared_ptr<DemandHandle::Impl> handle;
    std::shared_ptr<const DemandHandle::Impl::Bundle> bundle;
    std::uint64_t generation = 0, revision = 0;
    CancellationToken cancellation;
    bool active = false;
    ~Lease() {
      if (active) {
        std::lock_guard<std::mutex> lock(owner->mutex);
        --owner->calls;
        owner->changed.notify_all();
      }
    }
    Status stop(Status status = {}) const {
      if (status.detail.origin == FailureOrigin::Protocol)
        return status;
      if (cancellation.cancelled())
        return Status{ErrorCode::Cancelled, {}};
      if (!bundle->plan.current() ||
          handle->generation.load(std::memory_order_acquire) != generation)
        return Status{ErrorCode::Stale, {}};
      return status;
    }
  };
  Result<std::shared_ptr<Lease>> acquire(
      const std::shared_ptr<DemandHandle::Impl>& handle,
      const CancellationToken& cancellation) {
    auto lease = std::make_shared<Lease>();
    lease->owner = shared_from_this();
    lease->handle = handle;
    auto combined = CancellationToken::combine(
        {shutdown.token(), handle->cancellation.token(), cancellation});
    if (!combined.ok())
      return Result<std::shared_ptr<Lease>>(combined.status());
    lease->cancellation = combined.take_value();
    std::lock_guard<std::mutex> lock(mutex);
    if (closing || lease->cancellation.cancelled())
      return Result<std::shared_ptr<Lease>>(Status{ErrorCode::Cancelled, {}});
    if (!handle->bundle || !handle->bundle->plan.current())
      return Result<std::shared_ptr<Lease>>(Status{ErrorCode::Stale, {}});
    if (calls >= maximum_calls)
      return Result<std::shared_ptr<Lease>>(
          Status{ErrorCode::ResourceExhausted, "active demand call limit"});
    lease->bundle = handle->bundle;
    lease->generation = handle->generation.load(std::memory_order_relaxed);
    lease->revision = handle->revision;
    ++calls;
    lease->active = true;
    return Result<std::shared_ptr<Lease>>(std::move(lease));
  }
  void close() noexcept {
    std::unique_lock<std::mutex> lock(mutex);
    closing = true;
    shutdown.cancel();
    for (const auto& item : handles)
      if (auto handle = item.second.handle.lock()) {
        handle->cancellation.cancel();
        handle->publications.clear();
        handle->metadata_entries = 0;
        handle->records.reset();
        auto retired = std::move(handle->bundle);
        lock.unlock();
        retired.reset();
        lock.lock();
      }
    changed.wait(lock, [&] { return calls == 0; });
    handles.clear();
    context = nullptr;
  }
};
std::shared_ptr<DemandCoordinator> make_demand_coordinator(
    std::uint32_t handles, std::uint64_t calls) {
  return std::make_shared<DemandCoordinator>(handles, calls);
}
void bind_demand_context(const std::shared_ptr<DemandCoordinator>& owner,
                         ExecutionContext* context) noexcept {
  owner->context = context;
}
void close_demand_coordinator(
    const std::shared_ptr<DemandCoordinator>& owner) noexcept {
  owner->close();
}
}  // namespace execution_internal
namespace {
Result<DemandQuery> normalize_tile_demands(const DemandQuery& query,
                                           const ExecutionPlan& plan,
                                           std::uint64_t maximum,
                                           const ResourceBudget& root) {
  auto checked = demand_key(query, plan, maximum, root);
  if (!checked.ok())
    return Result<DemandQuery>(checked.status());
  DemandQuery result;
  for (const auto& item : query) {
    const auto found = plan.outputs().find(item.first);
    if (found == plan.outputs().end())
      return Result<DemandQuery>(
          Status{ErrorCode::InvalidArgument, "unknown demand output"});
    const auto& step = plan.output(item.first);
    auto closed = item.second.resolve(step.tile_grids);
    if (!closed.ok())
      return Result<DemandQuery>(closed.status());
    result.emplace(item.first, closed.take_value());
  }
  return Result<DemandQuery>(std::move(result));
}
Status unite_named(std::map<std::string, TileRequest>* target,
                   const std::map<std::string, TileRequest>& values,
                   const ResourceBudget& root) {
  for (const auto& item : values) {
    auto work = root.consume({1 + item.first.size()});
    if (!work.ok())
      return work;
    auto found = target->find(item.first);
    auto merged = found == target->end()
                      ? item.second.copy_to(root)
                      : found->second.unite(item.second, root);
    if (!merged.ok())
      return merged.status();
    target->insert_or_assign(item.first, merged.take_value());
  }
  return Status::success();
}
Result<TileRequest> query_units(const TileRequest& query,
                                const PlanOutput& step,
                                const ResourceBudget& root,
                                std::uint64_t maximum) {
  auto normalized = query.resolve(step.tile_grids);
  if (!normalized.ok())
    return normalized;
  if (normalized.value().interval_count() > maximum)
    return Result<TileRequest>(
        Status{ErrorCode::ResourceExhausted, "demand metadata limit"});
  return normalized.value().copy_to(root);
}
template <class Publications>
Result<std::shared_ptr<const TileRecords>> retained_records(
    const TileRecords& previous, const TileRecords::Snapshot& updates,
    const Publications& publications, const ExecutionPlan& plan,
    const ResourceBudget& root, std::uint64_t generation,
    const CancellationToken& cancellation) {
  TileRecord roots(root);
  for (const auto& item : publications) {
    for (const auto& named : item.second->query) {
      const auto index = plan.output(named.first).record_index;
      for (const auto& member : named.second.members) {
        auto status = roots.include(
            {PlanStepInput{index}, 0, member.first, member.second}, root);
        if (!status.ok())
          return Result<std::shared_ptr<const TileRecords>>(status);
      }
    }
  }
  return previous.replaced(
      generation, updates, roots,
      [&](const PlanStepInput& input, std::uint32_t output,
          std::uint32_t slot) {
        using Answer = Result<TileRecords::ProducerUnits>;
        if (input.step_index >= plan.steps().size()) {
          const auto index = input.step_index - plan.steps().size();
          if (index >= plan.return_signature().size() || output != 0 ||
              slot >= plan.return_signature()[index].tile_grids.size())
            return Answer(Status{ErrorCode::InvalidArgument, {}});
          return Answer(TileRecords::ProducerUnits{
              plan.return_signature()[index].tile_grids[slot].tile_count()});
        }
        const auto& step = plan.steps()[input.step_index];
        if (output != step.output_index || slot >= step.tile_grids.size())
          return Answer(Status{ErrorCode::InvalidArgument, {}});
        return Answer(
            TileRecords::ProducerUnits{step.tile_grids[slot].tile_count()});
      },
      [&](std::uint64_t n) {
        return cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                        : root.consume({n});
      });
}
template <class Publications>
Result<std::uint64_t> retained_weight(const Publications& publications,
                                      const TileRecords& records,
                                      const ExecutionPlan& plan,
                                      const ResourceBudget& root,
                                      std::uint64_t maximum) {
  auto size = records.metadata_size();
  if (!size.ok())
    return size;
  std::uint64_t weight = size.value();
  for (const auto& item : publications) {
    auto key = demand_key(item.second->query, plan, maximum, root);
    if (!key.ok())
      return Result<std::uint64_t>(key.status());
    auto next = diagnostic_byte_sum(weight, key.value().entries);
    if (!next.ok())
      return next;
    weight = next.value();
    for (const auto& named : item.second->dirty) {
      next = diagnostic_byte_sum(weight, 1 + named.second.interval_count());
      if (!next.ok())
        return next;
      weight = next.value();
    }
  }
  return weight <= maximum
             ? Result<std::uint64_t>(weight)
             : Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted,
                                            "retained demand metadata limit"});
}
}  // namespace
Result<DemandHandle> ExecutionContext::open_demand(const ExecutionPlan& plan,
                                                   ExecutionBindings bindings,
                                                   DemandConfig config) {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock() != impl_->operation_registry)
    return Result<DemandHandle>(
        Status{ErrorCode::Stale, "invalid or foreign demand plan"});
  if (!config.maximum_metadata_entries ||
      config.maximum_metadata_entries > 1048576)
    return Result<DemandHandle>(
        Status{ErrorCode::InvalidArgument, "invalid demand metadata limit"});
  auto validated = execution_internal::preflight_regional_bindings(
      plan, bindings, {}, *impl_->budget);
  if (!validated.ok())
    return Result<DemandHandle>(validated.status());
  const auto root = *impl_->budget;
  auto bundle = std::allocate_shared<DemandHandle::Impl::Bundle>(
      ResourceAllocator<DemandHandle::Impl::Bundle>(root), root);
  bundle->plan = plan;
  bundle->bindings = std::move(bindings);
  if (!plan.current())
    return Result<DemandHandle>(Status{ErrorCode::Stale, {}});
  auto state = std::make_shared<DemandHandle::Impl>();
  state->owner = impl_->demands;
  state->config = config;
  state->records = std::allocate_shared<TileRecords>(
      ResourceAllocator<TileRecords>(root), root, 1);
  state->bundle = std::move(bundle);
  auto& owner = *impl_->demands;
  std::lock_guard<std::mutex> lock(owner.mutex);
  if (owner.closing)
    return Result<DemandHandle>(Status{ErrorCode::Cancelled, {}});
  for (auto i = owner.handles.begin(); i != owner.handles.end();) {
    // Do not acquire a temporary last owner under the publication lock:
    // input allocation deleters may reenter another demand in this context.
    if (i->second.handle.expired() || i->second.cancellation.cancelled())
      i = owner.handles.erase(i);
    else
      ++i;
  }
  if (owner.handles.size() >= owner.maximum_handles || owner.next == UINT64_MAX)
    return Result<DemandHandle>(
        Status{ErrorCode::ResourceExhausted, "demand handle limit"});
  owner.handles.emplace(owner.next++,
                        execution_internal::DemandCoordinator::HandleEntry{
                            state, state->cancellation.token()});
  return Result<DemandHandle>(DemandHandle(std::move(state)));
}
DemandHandle::DemandHandle(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Result<DemandResult> DemandHandle::request(
    const DemandQuery& query, const CancellationToken& cancellation,
    const ExecutionOptions& options) const {
  if (!impl_)
    return Result<DemandResult>(
        Status{ErrorCode::Stale, "invalid demand handle"});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<DemandResult>(
        Status{ErrorCode::Cancelled, "demand context retired"});
  if (cancellation.cancelled() ||
      options.dependencies.sets.cancellation.cancelled())
    return Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
  auto combined = CancellationToken::combine(
      {cancellation, options.dependencies.sets.cancellation});
  if (!combined.ok())
    return Result<DemandResult>(combined.status());
  auto begun = owner->acquire(impl_, combined.value());
  if (!begun.ok())
    return Result<DemandResult>(begun.status());
  auto lease = begun.take_value();
  const auto failure = [&](Status status) {
    return Result<DemandResult>(lease->stop(std::move(status)));
  };
  auto normalized = normalize_tile_demands(
      query, lease->bundle->plan, options.dependencies.sets.maximum_boxes,
      lease->bundle->resources);
  if (!normalized.ok())
    return failure(normalized.status());
  DemandQuery original = normalized.take_value();
  auto key = demand_key(original, lease->bundle->plan,
                        std::min(impl_->config.maximum_metadata_entries,
                                 options.dependencies.sets.maximum_boxes),
                        lease->bundle->resources);
  if (!key.ok())
    return failure(key.status());
  auto run = lease->bundle->plan;
  run.current_check_ = [current = run.current_check_, handle = impl_,
                        generation = lease->generation] {
    return current && current() &&
           handle->generation.load(std::memory_order_acquire) == generation;
  };
  auto executed = owner->context->execute_fragments(
      run, lease->bundle->bindings, original, lease->cancellation, options);
  if (!executed.ok())
    return failure(executed.status());
  auto result = executed.take_value();
  result.generation = lease->generation;
  const auto& root = lease->bundle->resources;
  ResourceAllocationScope allocation(root);
  auto publication = std::allocate_shared<Impl::Publication>(
      ResourceAllocator<Impl::Publication>(root));
  publication->query = std::move(original);
  for (const auto& named : publication->query)
    publication->dirty.emplace(named.first, TileRequest{});
  auto updates =
      result.dependencies.tiles_
          ? result.dependencies.tiles_->snapshot()
          : Result<TileRecords::Snapshot>(TileRecords::Snapshot{
                ResourceAllocator<std::shared_ptr<const TileRecord>>(root)});
  if (!updates.ok())
    return failure(updates.status());
  std::map<std::string, std::shared_ptr<const Impl::Publication>> publications;
  std::shared_ptr<const TileRecords> current_records;
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    auto status = lease->stop();
    if (!status.ok())
      return Result<DemandResult>(status);
    if (impl_->revision == UINT64_MAX)
      return Result<DemandResult>(Status{ErrorCode::ResourceExhausted, {}});
    auto previous = impl_->publications.find(key.value().value);
    if (previous != impl_->publications.end() &&
        previous->second->query != publication->query)
      return Result<DemandResult>(
          Status{ErrorCode::Internal, "query identity collision"});
    publications = impl_->publications;
    current_records = impl_->records;
    lease->revision = impl_->revision;
  }
  publications.insert_or_assign(key.value().value, publication);
  for (auto& item : publications) {
    auto copied = std::allocate_shared<Impl::Publication>(
        ResourceAllocator<Impl::Publication>(root), *item.second);
    for (auto& named : copied->dirty) {
      const auto index = lease->bundle->plan.output(named.first).record_index;
      TileRequest remaining_request;
      for (const auto& member : named.second.members) {
        ResourceVector<TileIndex> remaining{ResourceAllocator<TileIndex>(root)};
        for (auto tile : member.second) {
          const TileUnit unit{index, 0, member.first, tile};
          auto fresh = result.dependencies.tile_record(unit);
          if (!fresh.ok())
            return failure(fresh.status());
          if (!fresh.value())
            remaining.push_back(tile);
        }
        auto selected =
            TileSet::from_indices(root, remaining.data(), remaining.size());
        if (!selected.ok())
          return failure(selected.status());
        if (!selected.value().empty())
          remaining_request.members.emplace(member.first,
                                            selected.take_value());
      }
      named.second = std::move(remaining_request);
    }
    item.second = std::move(copied);
  }
  auto records = retained_records(*current_records, updates.value(),
                                  publications, lease->bundle->plan, root,
                                  lease->generation, lease->cancellation);
  if (!records.ok())
    return failure(records.status());
  auto weight =
      retained_weight(publications, *records.value(), lease->bundle->plan, root,
                      impl_->config.maximum_metadata_entries);
  if (!weight.ok())
    return failure(weight.status());
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return Result<DemandResult>(status);
  if (impl_->revision != lease->revision)
    return Result<DemandResult>(
        Status{ErrorCode::Stale,
               "demand publications changed during request publication"});
  impl_->records = records.take_value();
  impl_->publications.swap(publications);
  impl_->metadata_entries = weight.value();
  ++impl_->revision;
  return Result<DemandResult>(std::move(result));
}
Result<std::uint64_t> DemandHandle::generation() const {
  if (!impl_)
    return Result<std::uint64_t>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<std::uint64_t>(Status{ErrorCode::Cancelled, {}});
  std::lock_guard<std::mutex> lock(owner->mutex);
  if (owner->closing || impl_->cancellation.token().cancelled())
    return Result<std::uint64_t>(Status{ErrorCode::Cancelled, {}});
  return Result<std::uint64_t>(impl_->generation.load());
}
Status DemandHandle::release(const DemandQuery& query) const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  auto owner = impl_->owner.lock();
  if (!owner)
    return Status{ErrorCode::Cancelled, {}};
  auto begun = owner->acquire(impl_, {});
  if (!begun.ok())
    return begun.status();
  auto lease = begun.take_value();
  auto normalized = normalize_tile_demands(
      query, lease->bundle->plan, impl_->config.maximum_metadata_entries,
      lease->bundle->resources);
  if (!normalized.ok())
    return lease->stop(normalized.status());
  auto key = demand_key(normalized.value(), lease->bundle->plan,
                        impl_->config.maximum_metadata_entries,
                        lease->bundle->resources);
  if (!key.ok())
    return lease->stop(key.status());
  std::map<std::string, std::shared_ptr<const Impl::Publication>> publications;
  std::shared_ptr<const TileRecords> current_records;
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    auto status = lease->stop();
    if (!status.ok())
      return status;
    auto found = impl_->publications.find(key.value().value);
    if (found == impl_->publications.end() ||
        found->second->query != normalized.value())
      return Status{ErrorCode::NotFound, {}};
    if (impl_->revision == UINT64_MAX)
      return Status{ErrorCode::ResourceExhausted, {}};
    publications = impl_->publications;
    current_records = impl_->records;
    lease->revision = impl_->revision;
  }
  const auto& root = lease->bundle->resources;
  ResourceAllocationScope allocation(root);
  publications.erase(key.value().value);
  auto records =
      retained_records(*current_records, {}, publications, lease->bundle->plan,
                       root, lease->generation, lease->cancellation);
  if (!records.ok())
    return lease->stop(records.status());
  auto weight =
      retained_weight(publications, *records.value(), lease->bundle->plan, root,
                      impl_->config.maximum_metadata_entries);
  if (!weight.ok())
    return lease->stop(weight.status());
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return status;
  if (impl_->revision != lease->revision)
    return Status{ErrorCode::Stale,
                  "demand publications changed during release"};
  impl_->records = records.take_value();
  impl_->publications.swap(publications);
  impl_->metadata_entries = weight.value();
  ++impl_->revision;
  return Status::success();
}
bool DemandHandle::cancel() const noexcept {
  if (!impl_)
    return false;
  const bool first = impl_->cancellation.cancel();
  std::shared_ptr<const DemandHandle::Impl::Bundle> retired;
  if (auto owner = impl_->owner.lock()) {
    std::lock_guard<std::mutex> lock(owner->mutex);
    impl_->publications.clear();
    impl_->metadata_entries = 0;
    impl_->records.reset();
    retired = std::move(impl_->bundle);
  }
  return first;
}
Result<DemandUpdate> DemandHandle::replace_bindings(
    ExecutionBindings bindings, const SnapshotAccessOptions& options) const
    // NOLINTNEXTLINE(whitespace/indent_namespace)
    try {
  if (!impl_)
    return Result<DemandUpdate>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<DemandUpdate>(Status{ErrorCode::Cancelled, {}});
  // Snapshot an owning Root under the lifetime/publication lock. This keeps
  // cancellation setup charged to the same Root without borrowing context
  // before acquiring a call lease. close retires the bundle under this lock.
  std::optional<ResourceBudget> root;
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    if (impl_->bundle)
      root = impl_->bundle->resources;
  }
  std::optional<ResourceAllocationScope> root_scope;
  if (root)
    root_scope.emplace(*root);
  auto begun = owner->acquire(impl_, options.cancellation);
  if (!begun.ok())
    return Result<DemandUpdate>(begun.status());
  auto lease = begun.take_value();
  const auto failure = [&](Status status) {
    return Result<DemandUpdate>(lease->stop(std::move(status)));
  };
  std::map<std::string, std::shared_ptr<const Impl::Publication>> publications;
  std::shared_ptr<const TileRecords> current_records;
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    auto status = lease->stop();
    if (!status.ok())
      return Result<DemandUpdate>(status);
    publications = impl_->publications;
    current_records = impl_->records;
    lease->revision = impl_->revision;
  }
  auto validated = execution_internal::preflight_regional_bindings(
      lease->bundle->plan, bindings, lease->cancellation, *root);
  if (!validated.ok())
    return failure(validated.status());
  auto next = std::allocate_shared<Impl::Bundle>(
      ResourceAllocator<Impl::Bundle>(*root), *root);
  next->plan = lease->bundle->plan;
  next->bindings = std::move(bindings);
  auto support = current_records->workflow_dependencies();
  if (!support.ok())
    return failure(support.status());
  auto access = options;
  access.cancellation = lease->cancellation;
  auto changes = execution_internal::tile_binding_changes(
      lease->bundle->plan, lease->bundle->bindings, next->bindings,
      support.value(), *root, access);
  if (!changes.ok())
    return failure(changes.status());
  auto dirty = current_records->dirty(changes.value());
  if (!dirty.ok())
    return failure(dirty.status());
  DemandUpdate update;
  if (lease->generation == UINT64_MAX)
    return failure(Status{ErrorCode::ResourceExhausted, {}});
  update.generation = lease->generation + 1;
  for (auto& item : publications) {
    auto publication = std::allocate_shared<Impl::Publication>(
        ResourceAllocator<Impl::Publication>(*root), *item.second);
    std::map<std::string, TileRequest> coverage;
    for (const auto& named : publication->query) {
      const auto& step = next->plan.output(named.first);
      const auto index = step.record_index;
      auto units = query_units(named.second, step, *root,
                               impl_->config.maximum_metadata_entries);
      if (!units.ok())
        return failure(units.status());
      TileRequest changes;
      for (const auto& member : units.value().members) {
        ResourceVector<TileIndex> selected{ResourceAllocator<TileIndex>(*root)};
        for (const auto& unit : dirty.value()) {
          auto work = root->consume({1});
          if (!work.ok())
            return failure(work);
          if (unit.step == index && unit.output == 0 &&
              unit.tensor_slot == member.first &&
              member.second.contains(unit.tile))
            selected.push_back(unit.tile);
        }
        auto changed =
            TileSet::from_indices(*root, selected.data(), selected.size());
        if (!changed.ok())
          return failure(changed.status());
        if (!changed.value().empty())
          changes.members.emplace(member.first, changed.take_value());
      }
      auto merged = publication->dirty[named.first].unite(changes, *root);
      if (!merged.ok())
        return failure(merged.status());
      publication->dirty[named.first] = merged.take_value();
      coverage.emplace(named.first, units.take_value());
    }
    auto status = unite_named(&update.coverage, coverage, *root);
    if (status.ok())
      status = unite_named(&update.potential_dirty, publication->dirty, *root);
    if (!status.ok())
      return failure(status);
    item.second = std::move(publication);
  }
  auto records =
      retained_records(*current_records, {}, publications, next->plan, *root,
                       update.generation, lease->cancellation);
  if (!records.ok())
    return failure(records.status());
  auto weight = retained_weight(publications, *records.value(), next->plan,
                                *root, impl_->config.maximum_metadata_entries);
  if (!weight.ok())
    return failure(weight.status());
  std::shared_ptr<const DemandHandle::Impl::Bundle> retired;
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return Result<DemandUpdate>(status);
  if (impl_->revision != lease->revision)
    return Result<DemandUpdate>(Status{
        ErrorCode::Stale, "demand publications changed during replacement"});
  if (impl_->revision == UINT64_MAX)
    return Result<DemandUpdate>(Status{ErrorCode::ResourceExhausted, {}});
  retired = std::move(impl_->bundle);
  impl_->bundle = std::move(next);
  impl_->publications.swap(publications);
  impl_->metadata_entries = weight.value();
  impl_->records = records.take_value();
  ++impl_->revision;
  impl_->generation.store(update.generation, std::memory_order_release);
  return Result<DemandUpdate>(std::move(update));
} catch (const std::bad_alloc&) {
  return Result<DemandUpdate>(Status{ErrorCode::ResourceExhausted, {}});
}

}  // namespace ps
