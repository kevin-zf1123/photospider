#include "photospider/data/result.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "data/affine_view.hpp"
#include "data/result_host_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"

namespace ps {
Result<ResourceVector<ResultRef::CacheStorage>> ResultRef::cache_storage(
    const std::function<Status(std::uint64_t)>& work,
    bool require_complete) const try {
  using Answer = Result<ResourceVector<CacheStorage>>;
  if (!impl_ || !work)
    return Answer(Status{ErrorCode::Stale, {}});
  ResourceAllocationScope scope(impl_->budget);
  ResourceVector<CacheStorage> answer{
      ResourceAllocator<CacheStorage>(impl_->budget)};
  ResourceVector<ResultRef> pending{
      ResourceAllocator<ResultRef>(impl_->budget)};
  std::set<const Impl*, std::less<const Impl*>, ResourceAllocator<const Impl*>>
      seen;
  std::set<const void*, std::less<const void*>, ResourceAllocator<const void*>>
      owners;
  pending.push_back(*this);
  auto add = [&](const std::shared_ptr<const CpuStorage>& storage,
                 std::uint64_t bytes, bool native = false) -> Status {
    auto charged = work(1);
    if (!charged.ok())
      return charged;
    if (bytes && owners.insert(storage.get()).second)
      answer.push_back(
          {storage.get(), bytes, native, storage->weak_from_this()});
    return Status::success();
  };
  while (!pending.empty()) {
    auto charged = work(1);
    if (!charged.ok())
      return Answer(charged);
    auto object = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(object.impl_.get()).second)
      continue;
    std::lock_guard<std::timed_mutex> lock(object.impl_->mutex);
    const auto& source = *object.impl_;
    if (require_complete && (!source.complete || !source.status().ok()))
      return Answer(Status{ErrorCode::NotFound, {}});

    for (std::size_t slot = 0; slot < source.schema.tensors.size(); ++slot) {
      const auto& tensor = source.tensors[slot];
      for (const auto& affine : tensor.tensor.fragments()) {
        const auto& storage = affine.storage();
        auto status = add(storage, storage->capacity(),
                          storage->native_owner_ != nullptr);
        if (!status.ok())
          return Answer(status);
      }
      auto owners_work = work(tensor.affine_owners.size());
      if (!owners_work.ok())
        return Answer(owners_work);
      pending.insert(pending.end(), tensor.affine_owners.begin(),
                     tensor.affine_owners.end());
    }
  }
  return Answer(std::move(answer));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<CacheStorage>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRef> ResultRef::rebind_cached(
    std::string_view scope, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& work) const try {
  if (!impl_ || !work || cancellation.cancelled())
    return Result<ResultRef>(Status{
        cancellation.cancelled() ? ErrorCode::Cancelled : ErrorCode::Stale,
        {}});
  ResourceAllocationScope allocation(impl_->budget);
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2))) {
    auto status = work(0);
    if (!status.ok())
      return Result<ResultRef>(status);
    if (cancellation.cancelled())
      return Result<ResultRef>(Status{ErrorCode::Cancelled, {}});
  }
  if (!impl_->complete || !impl_->status().ok())
    return Result<ResultRef>(Status{ErrorCode::NotFound, {}});
  auto charged = work(1 + impl_->schema.canonical_size());
  if (!charged.ok())
    return Result<ResultRef>(charged);
  auto started = ResultBuilder::start(impl_->budget, impl_->schema, scope,
                                      impl_->limits, 128);
  if (!started.ok())
    return Result<ResultRef>(started.status());
  auto builder = started.take_value();
  auto target = builder.reference().impl_;
  target->tensors = impl_->tensors;
  target->storage_owners = impl_->storage_owners;
  target->image_budget = impl_->image_budget;
  lock.unlock();
  auto status = work(0);
  if (!status.ok())
    return Result<ResultRef>(status);
  if (cancellation.cancelled())
    return Result<ResultRef>(Status{ErrorCode::Cancelled, {}});
  auto sealed = builder.seal();
  if (!sealed.ok())
    return sealed;
  auto result = sealed.take_value();
  return Result<ResultRef>(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRef>(Status{ErrorCode::ResourceExhausted, {}});
}
bool ResultRef::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->budget.same_owner(budget);
}
std::uint64_t ResultRef::object_id() const noexcept {
  return impl_ ? impl_->object : 0;
}
const SchemaTemplate& ResultRef::schema() const {
  if (!impl_)
    throw std::logic_error("invalid ResultRef");
  return impl_->schema;
}
bool ResultRef::matches_scope(std::string_view scope) const noexcept {
  return impl_ && std::string_view(impl_->scope) == scope;
}
void ResultRef::bind_producer(std::uint64_t node) const noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  impl_->failure.bind_producer(node);
}
void ResultRef::retire_producer(const Status& failure) const noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  impl_->failure.bind_producer(failure.detail.node_id);
  if (!impl_->complete) {
    if (impl_->status().ok())
      impl_->failure.record(failure);
    else
      impl_->failure.enrich(failure);
  }
}
Status ResultRef::production_status() const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (captured_ ? captured_->descriptor.sealed() : impl_->complete)
    return Status::success();
  return impl_->failure.ok() && !impl_->cancelled_publication.load()
             ? data_internal::unavailable()
             : impl_->status();
}
}  // namespace ps
