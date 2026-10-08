#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "execution/accounted_regions.hpp"
#include "execution/demand_context.hpp"
#include "execution/demand_query.hpp"
#include "execution/dependency_records.hpp"
#include "execution/execution_context_state.hpp"
#include "execution/frozen_execution_state.hpp"
#include "photospider/execution/execution.hpp"
#include "plugin/port_validation.hpp"

namespace ps {
using execution_internal::demand_key;
namespace {
// Checked publication metadata weight; failure leaves the old generation
// intact.
Result<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
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
  struct Publication {
    DemandQuery query;
    ExecutionDependencies dependencies;
    DemandQuery dirty;
    std::uint64_t weight = 0;
  };
  std::weak_ptr<execution_internal::DemandCoordinator> owner;
  std::shared_ptr<const FrozenExecution> bundle;
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
    std::shared_ptr<const FrozenExecution> bundle;
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
      if (handle->generation.load(std::memory_order_acquire) != generation)
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
    if (!handle->bundle)
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
Result<DemandQuery> close_color_demands(const DemandQuery& query,
                                        const ExecutionPlan& plan,
                                        const FootprintLimits& limits) {
  auto checked = demand_key(query, plan, limits.maximum_boxes);
  if (!checked.ok())
    return Result<DemandQuery>(checked.status());
  DemandQuery result;
  for (const auto& item : query) {
    const auto found = plan.outputs().find(item.first);
    if (found == plan.outputs().end())
      return Result<DemandQuery>(
          Status{ErrorCode::InvalidArgument, "unknown color demand output"});
    const auto& step = plan.steps()[found->second];
    auto closed =
        step.output_result_schema && !step.output_result_schema->tensors.empty()
            ? step.output_result_schema->tensors[0].close_samples(item.second,
                                                                  limits)
            : input_internal::color_output_samples(step.output_descriptor,
                                                   step.output_facets,
                                                   item.second, limits);
    if (!closed.ok())
      return Result<DemandQuery>(closed.status());
    result.emplace(item.first, closed.take_value());
  }
  return Result<DemandQuery>(std::move(result));
}
template <class NamedValues>
Status unite_named(DemandQuery* target, const NamedValues& values,
                   const FootprintLimits& limits) {
  for (const auto& item : values) {
    auto found = target->find(std::string(item.first));
    auto next = found == target->end()
                    ? Footprint::from_regions(item.second.shape(),
                                              item.second.boxes(), limits)
                    : found->second.unite(item.second, limits);
    if (!next.ok())
      return next.status();
    target->insert_or_assign(std::string(item.first), next.take_value());
  }
  std::uint64_t entries = 0;
  for (const auto& item : *target) {
    const auto cost = 1 + item.second.boxes().size();
    if (cost > limits.maximum_boxes || entries > limits.maximum_boxes - cost)
      return Status{ErrorCode::ResourceExhausted, {}};
    entries += cost;
  }
  return Status::success();
}
Result<ResourceVector<SourceObservation>> changed_observations(
    const ExecutionBindings& before, const ExecutionBindings& after,
    const ResourceVector<SourceObservation>& observations,
    SnapshotAccessOptions access, const FootprintLimits& limits) {
  using Answer = Result<ResourceVector<SourceObservation>>;
  using Key = std::tuple<ResourceString, ResultSupportTarget, std::uint32_t>;
  std::map<Key, Footprint, std::less<Key>,
           ResourceAllocator<std::pair<const Key, Footprint>>>
      physical, changes;
  std::map<ResourceString, const ExecutionBinding*, ResourceStringLess,
           ResourceAllocator<
               std::pair<const ResourceString, const ExecutionBinding*>>>
      old, next;
  for (const auto& input : before.inputs)
    old.emplace(ResourceString(input.name.data(), input.name.size()), &input);
  for (const auto& input : after.inputs)
    next.emplace(ResourceString(input.name.data(), input.name.size()), &input);
  for (const auto& observation : observations) {
    Key key{observation.input, observation.target, observation.slot};
    auto found = physical.find(key);
    auto united = found == physical.end()
                      ? Result<Footprint>(observation.samples)
                      : found->second.unite(observation.samples, limits);
    if (!united.ok())
      return Answer(united.status());
    physical.insert_or_assign(key, united.take_value());
    if (physical.size() > limits.maximum_boxes)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
  }
  std::uint64_t remaining = access.maximum_samples;
  for (const auto& item : physical) {
    const auto& name = std::get<0>(item.first);
    const auto kind = std::get<1>(item.first);
    const auto slot = std::get<2>(item.first);
    const auto& samples = item.second;
    if (samples.empty())
      continue;
    const auto& a = *old.at(name);
    const auto& b = *next.at(name);
    auto count = samples.element_count();
    if (!count.ok())
      return Answer(count.status());
    if (count.value() > remaining)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "demand update sample limit"});
    if (!a.result.valid() || !b.result.valid() ||
        !a.result.schema().same_schema(b.result.schema()))
      return Answer(
          Status{ErrorCode::TypeMismatch, "Result source schema changed"});
    auto left = a.result.descriptor();
    auto right = b.result.descriptor();
    if (!left.ok())
      return Answer(left.status());
    if (!right.ok())
      return Answer(right.status());
    if (kind == ResultSupportTarget::Descriptor) {
      --remaining;
      bool dirty = left.value().sealed() != right.value().sealed();
      for (std::uint32_t i = 0; i < left.value().field_count(); ++i)
        dirty |= left.value().rows(i) != right.value().rows(i);
      for (std::uint32_t i = 0; i < left.value().tensor_count(); ++i)
        dirty |=
            left.value().tensor_coverage(i) != right.value().tensor_coverage(i);
      if (dirty)
        changes.emplace(item.first, samples);
      continue;
    }
    execution_internal::AccountedRegions boxes;
    auto status = samples.visit(
        [&](const auto& at) -> Status {
          if (!remaining)
            return Status{ErrorCode::ResourceExhausted, {}};
          --remaining;
          bool dirty = false;
          if (kind == ResultSupportTarget::Tensor) {
            if (slot >= a.result.schema().tensors.size())
              return Status{ErrorCode::TypeMismatch, {}};
            const auto width = Value::element_size(
                a.result.schema().tensors[slot].descriptor.element_type);
            std::uint8_t first[8]{}, second[8]{};
            if (!right.value().tensor_coverage(slot).contains(at)) {
              dirty = true;
            } else {
              auto read = a.result.read_tensor(left.value(), slot, at, first,
                                               width, access.cancellation);
              if (!read.ok())
                return read;
              read = b.result.read_tensor(right.value(), slot, at, second,
                                          width, access.cancellation);
              if (!read.ok())
                return read;
              dirty = std::memcmp(first, second, width) != 0;
            }
          } else {
            if (kind != ResultSupportTarget::Field ||
                slot >= a.result.schema().fields.size() || at.size() != 1)
              return Status{ErrorCode::TypeMismatch, {}};
            if (at[0] >= right.value().rows(slot)) {
              dirty = true;
            } else {
              auto first = a.result.prepare_read(left.value(), slot, at[0], 1);
              if (!first.ok())
                return first.status();
              auto second =
                  b.result.prepare_read(right.value(), slot, at[0], 1);
              if (!second.ok())
                return second.status();
              auto bytes = a.result.schema().row_bytes(slot);
              if (!bytes.ok())
                return bytes.status();
              auto first_bytes =
                  first.value().load(bytes.value(), access.cancellation);
              if (!first_bytes.ok())
                return first_bytes.status();
              auto second_bytes =
                  second.value().load(bytes.value(), access.cancellation);
              if (!second_bytes.ok())
                return second_bytes.status();
              dirty = std::memcmp(first_bytes.value()->bytes().data(),
                                  second_bytes.value()->bytes().data(),
                                  bytes.value()) != 0;
            }
          }
          if (dirty) {
            if (boxes.boxes.size() >= limits.maximum_boxes)
              return Status{ErrorCode::ResourceExhausted, {}};
            std::array<RegionDimension, 8> dimensions{};
            for (std::size_t i = 0; i < at.size(); ++i)
              dimensions[i] = {at[i], 1};
            auto admitted = boxes.append(dimensions.data(), at.size());
            if (!admitted.ok())
              return admitted;
          }
          return Status::success();
        },
        count.value(), access.cancellation);
    if (!status.ok())
      return Answer(status);
    auto dirty = Footprint::from_regions(samples.shape(), boxes.boxes, limits);
    if (!dirty.ok())
      return Answer(dirty.status());
    if (!dirty.value().empty())
      changes.emplace(item.first, dirty.take_value());
  }
  ResourceVector<SourceObservation> answer;
  for (const auto& observation : observations) {
    auto found =
        changes.find({observation.input, observation.target, observation.slot});
    if (found == changes.end())
      continue;
    auto dirty = found->second.intersect(observation.samples, limits);
    if (!dirty.ok())
      return Answer(dirty.status());
    if (!dirty.value().empty())
      answer.push_back({observation.input, observation.target, observation.slot,
                        observation.roles, dirty.take_value()});
  }
  return Answer(std::move(answer));
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
  auto frozen = freeze(plan, std::move(bindings));
  if (!frozen.ok())
    return Result<DemandHandle>(frozen.status());
  auto state = std::make_shared<DemandHandle::Impl>();
  state->owner = impl_->demands;
  state->config = config;
  state->bundle = std::make_shared<const FrozenExecution>(frozen.take_value());
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
  auto normalized = close_color_demands(query, lease->bundle->plan(),
                                        options.dependencies.sets);
  if (!normalized.ok())
    return failure(normalized.status());
  DemandQuery original = normalized.take_value();
  auto key = demand_key(original, lease->bundle->plan(),
                        std::min(impl_->config.maximum_metadata_entries,
                                 options.dependencies.sets.maximum_boxes));
  if (!key.ok())
    return failure(key.status());
  auto state = std::allocate_shared<FrozenExecution::State>(
      ResourceAllocator<FrozenExecution::State>(
          lease->bundle->state_->resources),
      *lease->bundle->state_);
  state->plan.current_check_ = [handle = impl_,
                                generation = lease->generation] {
    return handle->generation.load(std::memory_order_acquire) == generation;
  };
  FrozenExecution run;
  run.state_ = std::move(state);
  auto executed = owner->context->execute_fragments(
      run, original, lease->cancellation, options);
  if (!executed.ok())
    return failure(executed.status());
  auto result = executed.take_value();
  result.generation = lease->generation;
  auto publication = std::make_shared<Impl::Publication>();
  publication->query = std::move(original);
  publication->dependencies = result.dependencies;
  auto limits = options.dependencies.sets;
  limits.cancellation = lease->cancellation;
  for (const auto& item : publication->query) {
    auto empty = Footprint::none(item.second.shape(), limits);
    if (!empty.ok())
      return failure(empty.status());
    publication->dirty.emplace(item.first, empty.take_value());
  }
  const auto base = checked_add(
      key.value().entries, execution_internal::DependencyRecords::metadata_size(
                               result.dependencies));
  if (!base.ok())
    return failure(base.status());
  auto weight = checked_add(base.value(), publication->dirty.size());
  if (!weight.ok())
    return failure(weight.status());
  publication->weight = weight.value();
  std::lock_guard<std::mutex> lock(owner->mutex);
  auto status = lease->stop();
  if (!status.ok())
    return Result<DemandResult>(status);
  if (impl_->revision == UINT64_MAX)
    return Result<DemandResult>(Status{ErrorCode::ResourceExhausted, {}});
  auto old = impl_->publications.find(key.value().value);
  if (old != impl_->publications.end() &&
      old->second->query != publication->query)
    return Result<DemandResult>(
        Status{ErrorCode::Internal, "query identity collision"});
  const auto remainder =
      impl_->metadata_entries -
      (old == impl_->publications.end() ? 0 : old->second->weight);
  if (publication->weight > impl_->config.maximum_metadata_entries ||
      remainder > impl_->config.maximum_metadata_entries - publication->weight)
    return Result<DemandResult>(
        Status{ErrorCode::ResourceExhausted, "retained demand metadata limit"});
  impl_->publications.insert_or_assign(key.value().value, publication);
  impl_->metadata_entries = remainder + publication->weight;
  ++impl_->revision;
  return Result<DemandResult>(std::move(result));
}
Result<FrozenExecution> DemandHandle::freeze() const {
  if (!impl_)
    return Result<FrozenExecution>(Status{ErrorCode::Stale, {}});
  auto owner = impl_->owner.lock();
  if (!owner)
    return Result<FrozenExecution>(Status{ErrorCode::Cancelled, {}});
  std::lock_guard<std::mutex> lock(owner->mutex);
  if (owner->closing || impl_->cancellation.token().cancelled())
    return Result<FrozenExecution>(Status{ErrorCode::Cancelled, {}});
  return Result<FrozenExecution>(*impl_->bundle);
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
  FootprintLimits limits;
  limits.maximum_boxes = impl_->config.maximum_metadata_entries;
  auto normalized = close_color_demands(query, lease->bundle->plan(), limits);
  if (!normalized.ok())
    return lease->stop(normalized.status());
  auto key = demand_key(normalized.value(), lease->bundle->plan(),
                        impl_->config.maximum_metadata_entries);
  if (!key.ok())
    return lease->stop(key.status());
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
  impl_->metadata_entries -= found->second->weight;
  impl_->publications.erase(found);
  ++impl_->revision;
  return Status::success();
}
bool DemandHandle::cancel() const noexcept {
  if (!impl_)
    return false;
  const bool first = impl_->cancellation.cancel();
  std::shared_ptr<const FrozenExecution> retired;
  if (auto owner = impl_->owner.lock()) {
    std::lock_guard<std::mutex> lock(owner->mutex);
    impl_->publications.clear();
    impl_->metadata_entries = 0;
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
      root = impl_->bundle->state_->resources;
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
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    auto status = lease->stop();
    if (!status.ok())
      return Result<DemandUpdate>(status);
    publications = impl_->publications;
    lease->revision = impl_->revision;
  }
  auto frozen =
      owner->context->freeze(lease->bundle->state_->plan, std::move(bindings));
  if (!frozen.ok())
    return failure(frozen.status());
  auto next = std::make_shared<const FrozenExecution>(frozen.take_value());
  FootprintLimits limits{impl_->config.maximum_metadata_entries, 1048576,
                         lease->cancellation};
  ResourceVector<SourceObservation> support;
  for (const auto& item : publications) {
    auto source = item.second->dependencies.source_observations(limits);
    if (!source.ok())
      return failure(source.status());
    if (source.value().size() > limits.maximum_boxes - support.size())
      return failure(Status{ErrorCode::ResourceExhausted, {}});
    support.insert(support.end(), source.value().begin(), source.value().end());
  }
  auto access = options;
  access.cancellation = lease->cancellation;
  auto changes =
      changed_observations(lease->bundle->state_->bindings,
                           next->state_->bindings, support, access, limits);
  if (!changes.ok())
    return failure(changes.status());
  DemandUpdate update;
  if (lease->generation == UINT64_MAX)
    return failure(Status{ErrorCode::ResourceExhausted, {}});
  update.generation = lease->generation + 1;
  std::uint64_t entries = 0;
  for (auto& item : publications) {
    auto publication = std::make_shared<Impl::Publication>(*item.second);
    for (const auto& change : changes.value()) {
      auto dirty = publication->dependencies.potential_dirty(
          std::string(change.input), change.samples, change.roles, limits,
          change.target, change.slot);
      if (!dirty.ok())
        return failure(dirty.status());
      auto status = unite_named(&publication->dirty, dirty.value(), limits);
      if (!status.ok())
        return failure(status);
    }
    auto key = demand_key(publication->query, next->state_->plan,
                          impl_->config.maximum_metadata_entries);
    if (!key.ok())
      return failure(key.status());
    auto weight =
        checked_add(key.value().entries,
                    execution_internal::DependencyRecords::metadata_size(
                        publication->dependencies));
    if (!weight.ok())
      return failure(weight.status());
    std::uint64_t cost = weight.value();
    for (const auto& dirty : publication->dirty) {
      weight = checked_add(cost, 1 + dirty.second.boxes().size());
      if (!weight.ok())
        return failure(weight.status());
      cost = weight.value();
    }
    publication->weight = cost;
    if (cost > impl_->config.maximum_metadata_entries ||
        entries > impl_->config.maximum_metadata_entries - cost)
      return failure(Status{ErrorCode::ResourceExhausted,
                            "updated demand metadata limit"});
    entries += cost;
    auto status = unite_named(&update.coverage, publication->query, limits);
    if (!status.ok())
      return failure(status);
    status = unite_named(&update.potential_dirty, publication->dirty, limits);
    if (!status.ok())
      return failure(status);
    item.second = std::move(publication);
  }
  std::shared_ptr<const FrozenExecution> retired;
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
  impl_->metadata_entries = entries;
  ++impl_->revision;
  impl_->generation.store(update.generation, std::memory_order_release);
  return Result<DemandUpdate>(std::move(update));
} catch (const std::bad_alloc&) {
  return Result<DemandUpdate>(Status{ErrorCode::ResourceExhausted, {}});
}

}  // namespace ps
