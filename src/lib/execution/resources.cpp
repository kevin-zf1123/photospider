#include "photospider/execution/resources.hpp"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include "photospider/execution/resource_allocator.hpp"

namespace ps {
namespace {
thread_local const ResourceBudget* metadata_root = nullptr;
thread_local ErrorCode* metadata_error = nullptr;
}  // namespace
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
Status exhausted() {
  return Status::failure(ErrorCode::ResourceExhausted,
                         "managed resource capacity exhausted");
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
  capacity[ResourceKind::Disk] = 1024ULL * 1024 * 1024;
  capacity[ResourceKind::Entries] = 1048576;
  capacity[ResourceKind::Files] = 256;
  capacity[ResourceKind::IoSlots] = 16;
  capacity[ResourceKind::Queue] = 1024;
}
struct ResourceBudget::Impl {
  explicit Impl(ResourceLimits value) : limits(std::move(value)) {
    stats.protected_cleanup = limits.cleanup;
  }
  struct Reference;
  Reference* references = nullptr;
  std::mutex reference_admission;
  std::condition_variable reference_changed;
  std::mutex mutex;
  ResourceLimits limits;
  ResourceStatistics stats;
  bool fits(const ResourceCapacity& c) const {
    for (std::size_t i = 0; i < c.values.size(); ++i)
      if (c.values[i] > limits.capacity.values[i] -
                            stats.protected_cleanup.values[i] -
                            stats.live.values[i])
        return false;
    return true;
  }
  void add(const ResourceCapacity& c) {
    for (std::size_t i = 0; i < c.values.size(); ++i) {
      stats.live.values[i] += c.values[i];
      stats.peak.values[i] =
          std::max(stats.peak.values[i], stats.live.values[i]);
    }
  }
};
struct ResourceLease::Impl {
  std::shared_ptr<ResourceBudget::Impl> root;
  ResourceCapacity amount;
  bool quarantined = false;
  ~Impl() {
    if (!root)
      return;
    std::lock_guard<std::mutex> lock(root->mutex);
    if (!quarantined) {
      for (std::size_t i = 0; i < amount.values.size(); ++i)
        root->stats.live.values[i] -= amount.values[i];
    }
  }
};
struct ResourceBudget::Impl::Reference {
  ResourceLease lease;
  std::shared_ptr<Impl> root;
  std::shared_ptr<const CpuStorage> storage;
  std::weak_ptr<Reference> self;
  Reference* next = nullptr;
  bool linked = false;
  ~Reference() {
    if (!linked)
      return;
    std::lock_guard<std::mutex> admission(root->reference_admission);
    {
      std::lock_guard<std::mutex> lock(root->mutex);
      auto** cursor = &root->references;
      while (*cursor && *cursor != this)
        cursor = &(*cursor)->next;
      if (*cursor)
        *cursor = next;
    }
    // A replacement registration must see both removal and returned capacity.
    // Release external storage after this lock, including cross-root aliases.
    lease = {};
    root->reference_changed.notify_all();
  }
};
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
Result<ResourceLease> ResourceBudget::reserve(ResourceCapacity capacity) const {
  if (!coherent(capacity)) {
    resource_internal::metadata_failure(*this, ErrorCode::InvalidArgument);
    return Result<ResourceLease>(Status::failure(
        ErrorCode::InvalidArgument, "inconsistent resource reservation"));
  }
  const auto failure = [&] {
    resource_internal::metadata_failure(*this, ErrorCode::ResourceExhausted);
    return Result<ResourceLease>(exhausted());
  };
  const auto overhead = lease_metadata_bytes();
  if (capacity[ResourceKind::Host] > UINT64_MAX - overhead ||
      capacity[ResourceKind::Metadata] > UINT64_MAX - overhead)
    return failure();
  capacity[ResourceKind::Host] += overhead;
  capacity[ResourceKind::Metadata] += overhead;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->fits(capacity))
    return failure();
  // Admission and fallible owner construction are one serialized transaction.
  // No resource can be published until construction succeeds.
  try {
    auto owner = std::make_shared<ResourceLease::Impl>();
    impl_->add(capacity);
    owner->root = impl_;
    owner->amount = capacity;
    ResourceLease lease;
    lease.impl_ = std::move(owner);
    return Result<ResourceLease>(std::move(lease));
  } catch (const std::bad_alloc&) {
    return failure();
  }
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
  std::lock_guard<std::mutex> lock(impl_->root->mutex);
  if (impl_->quarantined)
    return Status::failure(ErrorCode::Stale, "quarantined resource lease");
  if (!impl_->root->fits(additional))
    return exhausted();
  impl_->root->add(additional);
  for (std::size_t i = 0; i < additional.values.size(); ++i)
    impl_->amount.values[i] += additional.values[i];
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
      remaining[ResourceKind::Metadata] < sizeof(Impl))
    return Status::failure(ErrorCode::InvalidArgument,
                           "inconsistent lease shrink");
  for (std::size_t i = 0; i < released.values.size(); ++i)
    impl_->root->stats.live.values[i] -= released.values[i];
  impl_->amount = remaining;
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto& issued = impl_->stats.issued;
  const auto& limit = impl_->limits;
  if (work.work > limit.maximum_work - issued.work ||
      work.io_bytes > limit.maximum_io_bytes - issued.io_bytes ||
      work.io_requests > limit.maximum_io_requests - issued.io_requests ||
      work.stages > limit.maximum_stages - issued.stages) {
    resource_internal::metadata_failure(*this, ErrorCode::ResourceExhausted);
    return exhausted();
  }
  issued.work += work.work;
  issued.io_bytes += work.io_bytes;
  issued.io_requests += work.io_requests;
  issued.stages += work.stages;
  return Status::success();
}
ResourceStatistics ResourceBudget::statistics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stats;
}
Result<std::shared_ptr<const CpuStorage>> ResourceBudget::reference(
    std::shared_ptr<const CpuStorage> storage) const {
  using Answer = Result<std::shared_ptr<const CpuStorage>>;
  if (!storage)
    return Answer(
        Status{ErrorCode::InvalidArgument, "missing referenced storage"});
  if (allocator().owns(*storage))
    return Answer(std::move(storage));
  // Lookup and first admission are one transaction. Reserving speculatively
  // can reject another reference to an already admitted full-capacity owner.
  std::unique_lock<std::mutex> admission(impl_->reference_admission);
  bool retiring = false;
  auto find = [&]() -> std::shared_ptr<const CpuStorage> {
    for (auto* node = impl_->references; node; node = node->next)
      if (node->storage.get() == storage.get()) {
        auto owner = node->self.lock();
        if (owner)
          return std::shared_ptr<const CpuStorage>(std::move(owner),
                                                   storage.get());
        retiring = true;
      }
    return {};
  };
  for (;;) {
    retiring = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      auto existing = find();
      if (existing)
        return Answer(std::move(existing));
    }
    if (!retiring)
      break;
    // The last alias has begun destruction but has not returned its lease.
    impl_->reference_changed.wait(admission);
  }
  auto capacity =
      ResourceCapacity::host(sizeof(Impl::Reference), sizeof(Impl::Reference));
  capacity[ResourceKind::Referenced] = storage->capacity();
  capacity[ResourceKind::Entries] = 1;
  auto admitted = reserve(capacity);
  if (!admitted.ok())
    return Answer(admitted.status());
  try {
    auto owner = std::shared_ptr<Impl::Reference>(new Impl::Reference());
    owner->lease = admitted.take_value();
    owner->root = impl_;
    owner->storage = storage;
    owner->self = owner;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    owner->next = impl_->references;
    impl_->references = owner.get();
    owner->linked = true;
    return Answer(
        std::shared_ptr<const CpuStorage>(std::move(owner), storage.get()));
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  }
}
BufferAllocator ResourceBudget::allocator() const {
  return BufferAllocator(
      [root = *this](std::uint64_t bytes) {
        constexpr auto metadata = sizeof(CpuStorage);
        if (bytes > UINT64_MAX - metadata)
          return Result<std::shared_ptr<void>>(exhausted());
        auto capacity = ResourceCapacity::host(bytes + metadata, metadata);
        capacity[ResourceKind::Payload] = bytes;
        auto admitted = root.reserve(capacity);
        if (!admitted.ok())
          return Result<std::shared_ptr<void>>(admitted.status());
        return Result<std::shared_ptr<void>>(admitted.value().impl_);
      },
      impl_);
}
}  // namespace ps
