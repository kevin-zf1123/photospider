#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "core/resource_state.hpp"
#include "photospider/core/resources.hpp"
#include "photospider/data/storage.hpp"

namespace ps {
namespace {
Status exhausted() {
  return Status{ErrorCode::ResourceExhausted,
                "managed resource capacity exhausted",
                FailureReason::CapacityLimit};
}
}  // namespace
// Each alias owns storage and a lease. Registration removal and lease release
// are serialized; physical storage retires after releasing the admission lock.
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
  auto complete = capacity;
  complete[ResourceKind::Host] += lease_metadata_bytes();
  complete[ResourceKind::Metadata] += lease_metadata_bytes();
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
  const auto reserve = [root = *this](bool shared) {
    return BufferAllocator::Reserve([root, shared](std::uint64_t bytes) {
      constexpr auto metadata = sizeof(CpuStorage);
      if (bytes > UINT64_MAX - metadata)
        return Result<std::shared_ptr<void>>(exhausted());
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
  result.native_shared_reserve_ = reserve(true);
  result.allocation_committed_ =
      core_internal::ResourcePayloadAccess::commit_owner;
  return result;
}
}  // namespace ps
