#include "photospider/core/resources.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include "core/resource_state.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps {
namespace {
thread_local const ResourceBudget* metadata_root = nullptr;
thread_local ErrorCode* metadata_error = nullptr;
thread_local core_internal::ResourcePayloadScope* payload_scope = nullptr;
thread_local core_internal::ResourceMetadataScope* metadata_scope = nullptr;
thread_local core_internal::ResourceAdmissionScope* admission_scope = nullptr;
}  // namespace
namespace core_internal {
ResourceAdmissionScope::ResourceAdmissionScope(
    const ResourceBudget& root, ResourceAdmissionFailure* failure,
    bool* recorded) noexcept
    : root_(root),
      failure_(failure),
      recorded_(recorded),
      previous_(admission_scope) {
  *recorded_ = false;
  admission_scope = this;
}
ResourceAdmissionScope::~ResourceAdmissionScope() noexcept {
  admission_scope = previous_;
}
void ResourceAdmissionScope::record(
    const ResourceBudget& root,
    const ResourceAdmissionFailure& failure) noexcept {
  for (auto* scope = admission_scope; scope; scope = scope->previous_)
    if (scope->root_.same_owner(root)) {
      if (!*scope->recorded_) {
        *scope->failure_ = failure;
        *scope->recorded_ = true;
      }
      return;
    }
}
ResourceMetadataStatistics MetadataObservation::snapshot() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return statistics_;
}
ResourceMetadataScope::ResourceMetadataScope(
    const ResourceBudget& root,
    std::weak_ptr<MetadataObservation> observation) noexcept
    : root_(root),
      observation_(std::move(observation)),
      previous_(metadata_scope) {
  metadata_scope = this;
}
ResourceMetadataScope::~ResourceMetadataScope() noexcept {
  metadata_scope = previous_;
}
std::weak_ptr<MetadataObservation> ResourceMetadataScope::capture(
    const ResourceBudget& root) noexcept {
  for (auto* scope = metadata_scope; scope; scope = scope->previous_)
    if (scope->root_.same_owner(root))
      return scope->observation_;
  return {};
}
void ResourceMetadataAccess::update(
    const std::weak_ptr<MetadataObservation>& weak, std::uint64_t reserved_add,
    std::uint64_t reserved_remove, std::uint64_t actual_add,
    std::uint64_t actual_remove) noexcept {
  auto observation = weak.lock();
  if (!observation)
    return;
  std::lock_guard<std::mutex> lock(observation->mutex_);
  auto& s = observation->statistics_;
  s.reserved_bytes += reserved_add;
  s.reserved_bytes -= reserved_remove;
  s.allocated_bytes += actual_add;
  s.allocated_bytes -= actual_remove;
  s.peak_reserved_bytes = std::max(s.peak_reserved_bytes, s.reserved_bytes);
  s.peak_allocated_bytes = std::max(s.peak_allocated_bytes, s.allocated_bytes);
}
void ResourceMetadataAccess::attribute(
    ResourceLease& lease,
    const std::shared_ptr<MetadataObservation>& observation) noexcept {
  if (!lease.impl_ || !observation)
    return;
  auto& impl = *lease.impl_;
  std::lock_guard<std::mutex> lock(impl.root->mutex);
  if (!impl.metadata_observation.expired())
    return;
  impl.metadata_observation = observation;
  update(impl.metadata_observation, impl.amount[ResourceKind::Metadata], 0,
         sizeof(ResourceLease::Impl) + impl.committed_metadata, 0);
}
PayloadObservation::Snapshot PayloadObservation::peaks() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return {peak_, peak_reserved_};
}
PayloadObservation::Snapshot PayloadObservation::live_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return {live_, reserved_};
}
ResourcePayloadScope::ResourcePayloadScope(const ResourceBudget& root,
                                           PayloadCapture capture) noexcept
    : root_(root), capture_(std::move(capture)), previous_(payload_scope) {
  payload_scope = this;
}
ResourcePayloadScope::~ResourcePayloadScope() noexcept {
  payload_scope = previous_;
}
PayloadCapture ResourcePayloadScope::capture(
    const ResourceBudget& root) noexcept {
  return capture_owner(root.impl_.get());
}
PayloadCapture ResourcePayloadScope::capture_owner(const void* root) noexcept {
  for (auto* scope = payload_scope; scope; scope = scope->previous_)
    if (scope->root_.impl_.get() == root)
      return scope->capture_;
  return {};
}
void ResourcePayloadAccess::update(const PayloadCapture& capture,
                                   std::uint64_t reserved_add,
                                   std::uint64_t reserved_remove,
                                   std::uint64_t actual_add,
                                   std::uint64_t actual_remove) noexcept {
  const auto apply =
      [&](const std::shared_ptr<PayloadObservation>& observation) {
        if (!observation)
          return;
        std::lock_guard<std::mutex> lock(observation->mutex_);
        observation->reserved_ += reserved_add;
        observation->reserved_ -= reserved_remove;
        observation->live_ += actual_add;
        observation->live_ -= actual_remove;
        observation->peak_ = std::max(observation->peak_, observation->live_);
        observation->peak_reserved_ =
            std::max(observation->peak_reserved_, observation->reserved_);
      };
  apply(capture.run);
}
}  // namespace core_internal
namespace resource_internal {
const ResourceBudget* metadata_budget() noexcept {
  return metadata_root;
}
void metadata_failure(const ResourceBudget& budget, ErrorCode code) noexcept {
  if (metadata_root && metadata_error && budget.same_owner(*metadata_root) &&
      *metadata_error == ErrorCode::Ok)
    *metadata_error = code;
}
}  // namespace resource_internal
ResourceAllocationScope::ResourceAllocationScope(const ResourceBudget& budget,
                                                 ErrorCode* failure) noexcept
    : previous_(metadata_root), previous_failure_(metadata_error) {
  metadata_root = &budget;
  metadata_error = failure;
}
ResourceAllocationScope::~ResourceAllocationScope() noexcept {
  metadata_root = previous_;
  metadata_error = previous_failure_;
}

namespace {
bool coherent(const ResourceCapacity& c) {
  return c[ResourceKind::Metadata] <= c[ResourceKind::Host] &&
         c[ResourceKind::Shared] <= c[ResourceKind::Host] &&
         c[ResourceKind::Shared] <= c[ResourceKind::Device];
}
}  // namespace

ResourceCapacity ResourceCapacity::host(std::uint64_t bytes,
                                        std::uint64_t metadata) {
  ResourceCapacity result;
  result[ResourceKind::Host] = bytes;
  result[ResourceKind::Metadata] = metadata;
  return result;
}
ResourceLimits::ResourceLimits() {
  capacity.values.fill(UINT64_MAX);
  capacity[ResourceKind::Host] = 256ULL * 1024 * 1024;
  capacity[ResourceKind::Shared] = capacity[ResourceKind::Host];
  capacity[ResourceKind::Metadata] = 16ULL * 1024 * 1024;
  capacity[ResourceKind::Entries] = 1048576;
  capacity[ResourceKind::Queue] = 1024;
}
namespace core_internal {
void ResourceMetadataAccess::commit(ResourceLease& lease,
                                    std::uint64_t bytes) noexcept {
  if (!lease.impl_ || !bytes)
    return;
  auto& impl = *lease.impl_;
  std::lock_guard<std::mutex> lock(impl.root->mutex);
  if (impl.quarantined)
    return;
  impl.committed_metadata += bytes;
  impl.root->commit_metadata(bytes);
  update(impl.metadata_observation, 0, 0, bytes, 0);
}
void ResourceMetadataAccess::withdraw(ResourceLease& lease,
                                      std::uint64_t bytes) noexcept {
  if (!lease.impl_ || !bytes)
    return;
  auto& impl = *lease.impl_;
  std::lock_guard<std::mutex> lock(impl.root->mutex);
  if (impl.quarantined || bytes > impl.committed_metadata)
    return;
  impl.committed_metadata -= bytes;
  impl.root->stats.allocated_metadata_bytes -= bytes;
  update(impl.metadata_observation, 0, 0, 0, bytes);
}
void ResourceMetadataAccess::commit_owner(const std::shared_ptr<void>& owner,
                                          std::uint64_t bytes) noexcept {
  ResourceLease lease;
  lease.impl_ = std::static_pointer_cast<ResourceLease::Impl>(owner);
  commit(lease, bytes);
}
void ResourcePayloadAccess::commit(ResourceLease& lease,
                                   std::uint64_t bytes) noexcept {
  if (!lease.impl_ || !bytes)
    return;
  auto& impl = *lease.impl_;
  std::lock_guard<std::mutex> lock(impl.root->mutex);
  if (impl.quarantined)
    return;
  // A native provider can allocate more than its preflight capacity and then
  // fail validation. Its real allocation still contributes to this peak.
  impl.committed_payload += bytes;
  update(impl.observation, 0, 0, bytes, 0);
}
void ResourcePayloadAccess::withdraw(ResourceLease& lease,
                                     std::uint64_t bytes) noexcept {
  if (!lease.impl_ || !bytes)
    return;
  auto& impl = *lease.impl_;
  std::lock_guard<std::mutex> lock(impl.root->mutex);
  if (impl.quarantined || bytes > impl.committed_payload)
    return;
  impl.committed_payload -= bytes;
  update(impl.observation, 0, 0, 0, bytes);
}
void ResourcePayloadAccess::commit_owner(const std::shared_ptr<void>& owner,
                                         std::uint64_t bytes) noexcept {
  ResourceLease lease;
  lease.impl_ = std::static_pointer_cast<ResourceLease::Impl>(owner);
  commit(lease, bytes);
}
}  // namespace core_internal
namespace resource_internal {
void commit_payload(ResourceLease& lease, std::uint64_t bytes) noexcept {
  core_internal::ResourcePayloadAccess::commit(lease, bytes);
}
void commit_metadata(ResourceLease& lease, std::uint64_t bytes) noexcept {
  core_internal::ResourceMetadataAccess::commit(lease, bytes);
}
}  // namespace resource_internal
ResourceBudget::ResourceBudget(ResourceLimits limits) {
  if (!coherent(limits.cleanup))
    throw std::invalid_argument("inconsistent managed capacity dimensions");
  for (std::size_t i = 0; i < limits.capacity.values.size(); ++i)
    if (limits.cleanup.values[i] > limits.capacity.values[i])
      throw std::invalid_argument("cleanup capacity exceeds root limit");
  impl_ = std::make_shared<Impl>(std::move(limits));
}
std::uint64_t ResourceBudget::lease_metadata_bytes() noexcept {
  return sizeof(ResourceLease::Impl);
}
Result<ResourceLease> ResourceBudget::reserve(
    ResourceCapacity capacity, ResourceAdmissionFailure* rejection) const {
  if (rejection) {
    *rejection = {};
    rejection->requested = capacity;
  }
  if (!coherent(capacity)) {
    resource_internal::metadata_failure(*this, ErrorCode::InvalidArgument);
    return Result<ResourceLease>(Status::failure(
        ErrorCode::InvalidArgument, "inconsistent resource reservation"));
  }
  const auto failure = [&] {
    resource_internal::metadata_failure(*this, ErrorCode::ResourceExhausted);
    return Result<ResourceLease>(core_internal::capacity_exhausted());
  };
  const auto overhead = lease_metadata_bytes();
  if (!core_internal::can_add(capacity[ResourceKind::Host], overhead) ||
      !core_internal::can_add(capacity[ResourceKind::Metadata], overhead))
    return failure();
  capacity[ResourceKind::Host] += overhead;
  capacity[ResourceKind::Metadata] += overhead;
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const auto describe_rejection = [&] {
    ResourceAdmissionFailure detail;
    detail.requested = capacity;
    for (std::size_t i = 0; i < capacity.values.size(); ++i) {
      auto available = impl_->limits.capacity.values[i] -
                       impl_->stats.protected_cleanup.values[i] -
                       impl_->stats.live.values[i];
      detail.available.values[i] = available;
      if (!detail.exhausted && capacity.values[i] > available)
        detail.exhausted = static_cast<ResourceKind>(i);
    }
    if (rejection)
      *rejection = detail;
    core_internal::ResourceAdmissionScope::record(*this, detail);
  };
  if (!impl_->fits(capacity)) {
    lock.unlock();
    impl_->reclaim(capacity);
    lock.lock();
    if (!impl_->fits(capacity)) {
      describe_rejection();
      return failure();
    }
  }
  // Admission and fallible owner construction are one serialized transaction.
  // No resource can be published until construction succeeds.
  try {
    auto owner = std::make_shared<ResourceLease::Impl>();
    impl_->add(capacity);
    owner->root = impl_;
    owner->amount = capacity;
    impl_->commit_metadata(sizeof(ResourceLease::Impl));
    owner->metadata_observation =
        core_internal::ResourceMetadataScope::capture(*this);
    core_internal::ResourceMetadataAccess::update(
        owner->metadata_observation, capacity[ResourceKind::Metadata], 0,
        sizeof(ResourceLease::Impl), 0);
    if (capacity[ResourceKind::Payload]) {
      owner->observation = core_internal::ResourcePayloadScope::capture(*this);
      core_internal::ResourcePayloadAccess::update(
          owner->observation, capacity[ResourceKind::Payload], 0, 0, 0);
    }
    ResourceLease lease;
    lease.impl_ = std::move(owner);
    return Result<ResourceLease>(std::move(lease));
  } catch (const std::bad_alloc&) {
    describe_rejection();
    return failure();
  }
}
void ResourceBudget::set_reclaimer(
    std::function<void(const ResourceCapacity&)> reclaim) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->reclaimer = std::move(reclaim);
}
ResourceCapacity ResourceLease::capacity() const {
  if (!impl_)
    return {};
  std::lock_guard<std::mutex> lock(impl_->root->mutex);
  auto amount = impl_->amount;
  amount[ResourceKind::Host] -= sizeof(Impl);
  amount[ResourceKind::Metadata] -= sizeof(Impl);
  return amount;
}
Status ResourceLease::grow(ResourceCapacity additional) {
  if (!impl_)
    return Status::failure(ErrorCode::Stale, "invalid resource lease");
  if (!coherent(additional))
    return Status::failure(ErrorCode::InvalidArgument, "inconsistent growth");
  std::unique_lock<std::mutex> lock(impl_->root->mutex);
  if (impl_->quarantined)
    return Status::failure(ErrorCode::Stale, "quarantined resource lease");
  if (!impl_->root->fits(additional)) {
    lock.unlock();
    impl_->root->reclaim(additional);
    lock.lock();
    if (impl_->quarantined)
      return Status::failure(ErrorCode::Stale, "quarantined resource lease");
    if (!impl_->root->fits(additional))
      return core_internal::capacity_exhausted();
  }
  impl_->root->add(additional);
  core_internal::ResourceMetadataAccess::update(
      impl_->metadata_observation, additional[ResourceKind::Metadata], 0, 0, 0);
  if (!impl_->amount[ResourceKind::Payload] &&
      additional[ResourceKind::Payload] && !impl_->observation.run)
    impl_->observation =
        core_internal::ResourcePayloadScope::capture_owner(impl_->root.get());
  for (std::size_t i = 0; i < additional.values.size(); ++i)
    impl_->amount.values[i] += additional.values[i];
  core_internal::ResourcePayloadAccess::update(
      impl_->observation, additional[ResourceKind::Payload], 0, 0, 0);
  return Status::success();
}
Status ResourceLease::shrink(ResourceCapacity released) {
  if (!impl_)
    return Status::failure(ErrorCode::Stale, "invalid resource lease");
  std::lock_guard<std::mutex> lock(impl_->root->mutex);
  if (impl_->quarantined)
    return Status::failure(ErrorCode::Stale, "quarantined resource lease");
  auto remaining = impl_->amount;
  for (std::size_t i = 0; i < released.values.size(); ++i) {
    if (released.values[i] > remaining.values[i])
      return Status::failure(ErrorCode::InvalidArgument,
                             "lease shrink exceeds owner");
    remaining.values[i] -= released.values[i];
  }
  if (!coherent(remaining) || remaining[ResourceKind::Host] < sizeof(Impl) ||
      remaining[ResourceKind::Metadata] < sizeof(Impl) ||
      remaining[ResourceKind::Metadata] - sizeof(Impl) <
          impl_->committed_metadata ||
      remaining[ResourceKind::Payload] < impl_->committed_payload)
    return Status::failure(ErrorCode::InvalidArgument,
                           "inconsistent lease shrink");
  for (std::size_t i = 0; i < released.values.size(); ++i)
    impl_->root->stats.live.values[i] -= released.values[i];
  impl_->amount = remaining;
  core_internal::ResourceMetadataAccess::update(
      impl_->metadata_observation, 0, released[ResourceKind::Metadata], 0, 0);
  core_internal::ResourcePayloadAccess::update(
      impl_->observation, 0, released[ResourceKind::Payload], 0, 0);
  return Status::success();
}
void ResourceLease::quarantine() noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->root->mutex);
  if (!impl_->quarantined) {
    impl_->quarantined = true;
    for (std::size_t i = 0; i < impl_->amount.values.size(); ++i)
      impl_->root->stats.quarantined.values[i] += impl_->amount.values[i];
  }
}
void ResourceLease::settle_quarantine() noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->root->mutex);
  if (impl_->quarantined) {
    for (std::size_t i = 0; i < impl_->amount.values.size(); ++i)
      impl_->root->stats.quarantined.values[i] -= impl_->amount.values[i];
    impl_->quarantined = false;
  }
}
Status ResourceBudget::consume(ResourceWork work) const {
  Status result;
  try_consume(work, result);
  return result;
}
bool ResourceBudget::try_consume(ResourceWork work, Status& failure) const {
  auto failed = [&](bool work_limit) {
    resource_internal::metadata_failure(*this, ErrorCode::ResourceExhausted);
    // Reuse caller-reserved message storage for allocation-free Run services.
    failure.code = ErrorCode::ResourceExhausted;
    failure.reason =
        work_limit ? FailureReason::WorkLimit : FailureReason::StageLimit;
    failure.detail = {};
    failure.message.assign(work_limit ? "managed resource work exhausted"
                                      : "managed resource stages exhausted");
    return false;
  };
  if (!work.stages)
    return impl_->admit_work(work.work) || failed(true);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto& issued = impl_->stats.issued;
  const auto& limit = impl_->limits;
  const bool work_limit = !core_internal::can_add(
      work.work, impl_->issued_work.load(std::memory_order_relaxed),
      limit.maximum_work);
  if (work_limit ||
      !core_internal::can_add(work.stages, issued.stages, limit.maximum_stages))
    return failed(work_limit);
  // All other dimensions are locked and validated. After this CAS no failure
  // or throwing operation occurs before committing their counters.
  if (!impl_->admit_work(work.work))
    return failed(true);
  issued.stages += work.stages;
  return true;
}

ResourceCapacity ResourceBudget::available_capacity() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ResourceCapacity result;
  for (std::size_t i = 0; i < result.values.size(); ++i)
    result.values[i] = impl_->limits.capacity.values[i] -
                       impl_->stats.protected_cleanup.values[i] -
                       impl_->stats.live.values[i];
  return result;
}
ResourceStatistics ResourceBudget::statistics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto result = impl_->stats;
  result.issued.work = impl_->issued_work.load(std::memory_order_relaxed);
  return result;
}
}  // namespace ps
