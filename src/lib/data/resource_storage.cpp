#include "data/resource_storage.hpp"

#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "core/resource_state.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/resources.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/data/storage.hpp"

namespace ps {
// Each alias owns storage and a lease. Registration removal and lease release
// are serialized; physical storage retires after releasing the admission lock.
struct ResourceBudget::Impl::Reference {
  ResourceLease lease;
  std::shared_ptr<Impl> root;
  std::shared_ptr<const void> storage;
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
Result<std::shared_ptr<const CpuStorage>> ResourceBudget::reference(
    std::shared_ptr<const CpuStorage> storage) const {
  using Answer = Result<std::shared_ptr<const CpuStorage>>;
  if (!storage)
    return Answer({ErrorCode::InvalidArgument, "missing referenced storage"});
  const auto* identity = storage.get();
  auto retained = data_internal::ResourceStorageAccess::reference(
      *this, storage, storage->capacity(), allocator().owns(*storage));
  if (!retained.ok())
    return Answer(retained.status());
  return Answer(
      std::shared_ptr<const CpuStorage>(retained.take_value(), identity));
}
Result<std::shared_ptr<const void>>
data_internal::ResourceStorageAccess::reference(
    const ResourceBudget& budget, std::shared_ptr<const void> storage,
    std::uint64_t bytes, bool owned) {
  using Impl = ResourceBudget::Impl;
  const auto& impl_ = budget.impl_;
  using Answer = Result<std::shared_ptr<const void>>;
  if (!storage)
    return Answer(
        Status{ErrorCode::InvalidArgument, "missing referenced storage"});
  if (owned)
    return Answer(std::move(storage));
  // Lookup and first admission are one transaction. Reserving speculatively
  // can reject another reference to an already admitted full-capacity owner.
  std::unique_lock<std::mutex> admission(impl_->reference_admission);
  bool retiring = false;
  auto find = [&]() -> std::shared_ptr<const void> {
    for (auto* node = impl_->references; node; node = node->next)
      if (node->storage.get() == storage.get()) {
        auto owner = node->self.lock();
        if (owner)
          return std::shared_ptr<const void>(std::move(owner), storage.get());
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
  capacity[ResourceKind::Referenced] = bytes;
  capacity[ResourceKind::Entries] = 1;
  auto complete = capacity;
  complete[ResourceKind::Host] += ResourceBudget::lease_metadata_bytes();
  complete[ResourceKind::Metadata] += ResourceBudget::lease_metadata_bytes();
  bool fits = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    fits = impl_->fits(complete);
  }
  if (!fits) {
    admission.unlock();
    impl_->reclaim(complete);
    admission.lock();
    for (;;) {
      retiring = false;
      {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (auto existing = find())
          return Answer(std::move(existing));
      }
      if (!retiring)
        break;
      impl_->reference_changed.wait(admission);
    }
  }
  auto admitted = budget.reserve(capacity);
  if (!admitted.ok())
    return Answer(admitted.status());
  try {
    auto object = std::make_unique<Impl::Reference>();
    object->lease = admitted.take_value();
    core_internal::ResourceMetadataAccess::commit(object->lease,
                                                  sizeof(Impl::Reference));
    auto owner = std::shared_ptr<Impl::Reference>(std::move(object));
    owner->root = impl_;
    owner->storage = storage;
    owner->self = owner;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    owner->next = impl_->references;
    impl_->references = owner.get();
    owner->linked = true;
    return Answer(std::shared_ptr<const void>(std::move(owner), storage.get()));
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  }
}
BufferAllocator ResourceBudget::allocator() const {
  const auto reserve = [root = *this](bool shared) {
    return BufferAllocator::Reserve([root, shared](std::uint64_t bytes) {
      constexpr auto metadata = sizeof(CpuStorage);
      if (!core_internal::can_add(bytes, metadata))
        return Result<std::shared_ptr<void>>(
            core_internal::capacity_exhausted());
      auto capacity = ResourceCapacity::host(bytes + metadata, metadata);
      capacity[ResourceKind::Payload] = bytes;
      if (shared) {
        capacity[ResourceKind::Device] = bytes;
        capacity[ResourceKind::Shared] = bytes;
      }
      auto admitted = root.reserve(capacity);
      if (!admitted.ok())
        return Result<std::shared_ptr<void>>(admitted.status());
      return Result<std::shared_ptr<void>>(admitted.value().impl_);
    });
  };
  BufferAllocator result(reserve(false), impl_);
  result.metadata_budget_ = *this;
  result.native_shared_reserve_ = reserve(true);
  result.allocation_committed_ = [](const std::shared_ptr<void>& owner,
                                    std::uint64_t bytes,
                                    std::uint64_t metadata) {
    core_internal::ResourcePayloadAccess::commit_owner(owner, bytes);
    core_internal::ResourceMetadataAccess::commit_owner(owner, metadata);
  };
  return result;
}
}  // namespace ps
