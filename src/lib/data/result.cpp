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

#include "core/checked_math.hpp"
#include "data/affine_view.hpp"
#include "data/result_host_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/value_validation.hpp"
#include "photospider/data/representation.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
Result<ResourceVector<ResultRef::CacheStorage>> ResultRef::cache_storage(
    const std::function<Status(std::uint64_t)>& work, bool include_resources,
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
  auto add = [&](const void* owner, std::uint64_t bytes,
                 bool native = false) -> Status {
    auto charged = work(1);
    if (!charged.ok())
      return charged;
    if (bytes && owners.insert(owner).second)
      answer.push_back({owner, bytes, native});
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
    for (std::size_t field = 0; field < source.schema.fields.size(); ++field) {
      const auto& storage = source.fields[field].storage;
      if (!storage.valid())
        continue;
      const auto size = storage.size();
      std::uint64_t rounded = 0;
      if (!core_internal::checked_align_up(size, 4096, &rounded))
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      auto status = add(storage.impl_.get(), rounded);
      if (!status.ok())
        return Answer(status);
    }
    for (std::size_t slot = 0; slot < source.schema.tensors.size(); ++slot) {
      const auto& tensor = source.tensors[slot];
      for (const auto& backing : tensor.backing) {
        auto status =
            add(backing.image.owner_token(), backing.image.backed_bytes());
        if (!status.ok())
          return Answer(status);
      }
      for (const auto& view : tensor.views) {
        auto status =
            add(view.backing.owner_token(), view.backing.backed_bytes());
        if (!status.ok())
          return Answer(status);
        auto owners_work = work(view.owners.size());
        if (!owners_work.ok())
          return Answer(owners_work);
        pending.insert(pending.end(), view.owners.begin(), view.owners.end());
      }
      for (const auto& affine : tensor.affine) {
        const auto& storage = affine.storage();
        auto status = add(storage.get(), storage->capacity(),
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
    if (!include_resources)
      continue;
    for (std::size_t i = 0; i < source.resources.profile_count(); ++i) {
      auto profile = source.resources.profile_at(i);
      if (!profile.ok())
        return Answer(profile.status());
      const auto& storage = profile.value().storage();
      auto status = add(storage.get(), storage->capacity());
      if (!status.ok())
        return Answer(status);
    }
    for (std::size_t i = 0; i < source.resources.config_count(); ++i) {
      auto config = source.resources.config_at(i);
      if (!config.ok())
        return Answer(config.status());
      const auto& storage = config.value().storage();
      auto status = add(storage.get(), storage->capacity());
      if (!status.ok())
        return Answer(status);
    }
  }
  return Answer(std::move(answer));
} catch (const std::bad_alloc&) {
  return Result<ResourceVector<CacheStorage>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRef> ResultRef::rebind_cached(
    std::string_view scope, const ResourceVector<std::uint64_t>& association,
    const CancellationToken& cancellation,
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
  // Self-associated PrimitiveRef rows need explicit ID relocation, so the
  // content cache does not admit this family until it can relocate those rows.
  if (impl_->schema.id == "photospider.path_set")
    return Result<ResultRef>(Status{ErrorCode::NotFound, {}});
  auto charged = work(1 + impl_->schema.canonical_size() + association.size());
  if (!charged.ok())
    return Result<ResultRef>(charged);
  std::uint64_t nested = 0;
  for (const auto& image : impl_->tensors) {
    for (const auto& view : image.views)
      nested += view.region.rank() * sizeof(RegionDimension);
    for (const auto& affine : image.affine) {
      nested += affine.descriptor().shape.size() * sizeof(std::uint64_t) +
                affine.region().rank() * sizeof(RegionDimension) +
                affine.layout().byte_strides.size() * sizeof(std::int64_t);
    }
  }
  auto admitted = impl_->budget.reserve(ResourceCapacity::host(nested, nested));
  if (!admitted.ok())
    return Result<ResultRef>(admitted.status());
  auto started = ResultBuilder::start(
      impl_->budget, impl_->schema, scope, impl_->limits,
      std::vector<std::uint64_t>(association.begin(), association.end()), 128,
      128, impl_->resources);
  if (!started.ok())
    return Result<ResultRef>(started.status());
  auto builder = started.take_value();
  auto target = builder.reference().impl_;
  target->cache_metadata = admitted.take_value();
  target->fields = impl_->fields;
  target->tensors = impl_->tensors;
  target->descriptor_relation = impl_->descriptor_relation;
  target->image_budget = impl_->image_budget;
  target->bytes = impl_->bytes;
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
  result.request_record_ = request_record_;
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
const ResourceBindings& ResultRef::resources() const {
  if (!impl_)
    throw std::logic_error("invalid ResultRef");
  return impl_->resources;
}
bool ResultRef::matches_scope(std::string_view scope) const noexcept {
  if (!impl_ || scope.size() > 4096 || impl_->key.size() < scope.size() + 8)
    return false;
  const auto offset = impl_->key.size() - scope.size() - 8;
  if (offset != impl_->schema.canonical_size())
    return false;
  std::uint64_t size = 0;
  for (unsigned i = 0; i < 8; ++i)
    size |= static_cast<std::uint64_t>(
                static_cast<unsigned char>(impl_->key[offset + i]))
            << (8 * i);
  return size == scope.size() &&
         std::string_view(impl_->key.data() + offset + 8, scope.size()) ==
             scope;
}
std::string_view ResultRef::semantic_key() const {
  if (!impl_)
    throw std::logic_error("invalid ResultRef");
  return impl_->key;
}
Status ResultRef::retain_association(
    const ResourceVector<std::uint64_t>& inputs,
    const std::function<Status(std::uint64_t)>& consume_work) const {
  if (!impl_ || !consume_work ||
      !inputs.get_allocator().owned_by(impl_->budget))
    return data_internal::invalid_schema();
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  struct Stopped {
    Status status;
  };
  try {
    auto work = consume_work(inputs.size());
    if (!work.ok())
      return work;
    for (auto input : inputs)
      if (!input || input == impl_->object)
        return data_internal::invalid_schema();
    auto ids = inputs;
    if (!impl_->association.empty()) {
      // Successive publications usually insert facts in stable port/ID order.
      // Verify that extension in one pass; arbitrary initial claim order still
      // uses membership checks, including repeated claims for the same ID.
      work = consume_work(inputs.size());
      if (!work.ok())
        return work;
      auto claimed = impl_->association.begin();
      for (auto input : inputs) {
        if (claimed == impl_->association.end())
          break;
        if (*claimed == input)
          ++claimed;
      }
      if (claimed != impl_->association.end()) {
        auto sorted = inputs;
        const auto less = [&](auto a, auto b) {
          auto charged = consume_work(1);
          if (!charged.ok())
            throw Stopped{std::move(charged)};
          return a < b;
        };
        if (!std::is_sorted(sorted.begin(), sorted.end(), less))
          std::sort(sorted.begin(), sorted.end(), less);
        for (auto prior : impl_->association)
          if (!std::binary_search(sorted.begin(), sorted.end(), prior, less))
            return data_internal::invalid_schema();
      }
    }
    // Associations are immutable dependency facts, not physical storage edges.
    // View publication independently retains every source whose bytes it uses.
    impl_->association = std::move(ids);
    impl_->owners_bound = true;
    return Status::success();
  } catch (const Stopped& stopped) {
    return stopped.status;
  } catch (const std::bad_alloc&) {
    return Status{ErrorCode::ResourceExhausted, {}};
  }
}

ResourceVector<std::uint64_t> ResultRef::association() const {
  if (!impl_)
    throw std::logic_error("invalid ResultRef");
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  return impl_->association;
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
Result<ResultRelation> ResultRef::descriptor_relation() const {
  if (captured_)
    return Result<ResultRelation>(captured_->basis);
  if (!impl_)
    return Result<ResultRelation>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->descriptor_relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(data_internal::unavailable());
  return Result<ResultRelation>(impl_->descriptor_relation);
}
}  // namespace ps
