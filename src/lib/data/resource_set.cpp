#include "photospider/data/resource_set.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps {
namespace {

struct Stop {
  Status status;
};
void check(Status status) {
  if (!status.ok())
    throw Stop{std::move(status)};
}
template <class T, class Work>
void admit(const T* values, std::size_t count, ResourceVector<T>* out,
           const ResourceBudget& resources, Work work) {
  if (count && !values)
    throw Stop{core_internal::invalid_domain("null resource handles")};
  out->reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    work(1);
    if (!values[i].valid())
      throw Stop{core_internal::invalid_domain("invalid resource handle")};
    out->push_back(values[i]);
  }
  std::sort(out->begin(), out->end(), [&](const auto& a, const auto& b) {
    work(1);
    return a.identity() < b.identity();
  });
  std::size_t unique = 0;
  for (std::size_t i = 0; i < count; ++i) {
    work(1);
    const auto& incoming = (*out)[i];
    if (unique && incoming.identity() == (*out)[unique - 1].identity()) {
      const auto& prior = (*out)[unique - 1];
      if (prior.storage().get() != incoming.storage().get()) {
        auto a = prior.storage()->bytes(), b = incoming.storage()->bytes();
        if (a.size() != b.size())
          throw Stop{
              core_internal::invalid_domain("resource identity collision")};
        for (std::size_t offset = 0; offset < a.size();) {
          const auto n = std::min<std::size_t>(1024, a.size() - offset);
          work(n);
          if (std::memcmp(a.data() + offset, b.data() + offset, n))
            throw Stop{
                core_internal::invalid_domain("resource identity collision")};
          offset += n;
        }
      }
      continue;
    }
    if (unique != i)
      (*out)[unique] = incoming;
    ++unique;
  }
  out->resize(unique);
  for (auto& value : *out) {
    work(1);
    auto ref = value.reference(resources);
    if (!ref.ok())
      throw Stop{ref.status()};
    value = ref.take_value();
  }
}
template <class T>
Result<T> lookup(const ResourceVector<T>& values, const ResourceIdentity& id) {
  const auto i = std::lower_bound(values.begin(), values.end(), id,
                                  [](const auto& value, const auto& key) {
                                    return value.identity() < key;
                                  });
  if (i == values.end() || !(i->identity() == id))
    return Result<T>(
        Status{ErrorCode::NotFound, "unresolved immutable resource identity"});
  return Result<T>(*i);
}
}  // namespace
struct ResourceSet::Impl {
  ResourceBudget resources;
  ResourceVector<ImmutableResource> values;
  explicit Impl(const ResourceBudget& root)
      : resources(root), values(ResourceAllocator<ImmutableResource>(root)) {}
};
Result<ResourceSet> ResourceSet::create(
    const std::vector<ImmutableResource>& values,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    std::uint64_t maximum_work) {
  return create_view(values.data(), values.size(), resources, cancellation,
                     maximum_work);
}
Result<ResourceSet> ResourceSet::create_view(
    const ImmutableResource* values, std::size_t count,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    std::uint64_t maximum_work) try {
  const auto work = [&](std::uint64_t n) {
    if (cancellation.cancelled())
      throw Stop{{ErrorCode::Cancelled, {}}};
    if (n > maximum_work)
      throw Stop{{ErrorCode::ResourceExhausted, "resource set work limit",
                  FailureReason::WorkLimit}};
    maximum_work -= n;
    check(resources.consume({n}));
  };
  work(0);
  if (!count)
    return Result<ResourceSet>(ResourceSet{});
  auto impl =
      std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources), resources);
  admit(values, count, &impl->values, resources, work);
  work(1);
  return Result<ResourceSet>(ResourceSet(std::move(impl)));
} catch (const Stop& stop) {
  return Result<ResourceSet>(stop.status);
} catch (const std::bad_alloc&) {
  return Result<ResourceSet>(Status{ErrorCode::ResourceExhausted,
                                    "resource set metadata capacity",
                                    FailureReason::CapacityLimit});
}
std::size_t ResourceSet::size() const noexcept {
  return impl_ ? impl_->values.size() : 0;
}
Result<ImmutableResource> ResourceSet::find(
    const ResourceIdentity& identity) const {
  return impl_ ? lookup(impl_->values, identity)
               : Result<ImmutableResource>(
                     Status{ErrorCode::NotFound, "missing resource"});
}
Result<ImmutableResource> ResourceSet::at(std::size_t index) const {
  if (index >= size())
    return Result<ImmutableResource>(
        Status{ErrorCode::NotFound, "resource index outside set"});
  return Result<ImmutableResource>(impl_->values[index]);
}
Result<ResourceSet> ResourceSet::select(
    const std::vector<ResourceIdentity>& identities,
    const CancellationToken& cancellation) const try {
  if (cancellation.cancelled())
    return Result<ResourceSet>(Status{ErrorCode::Cancelled, {}});
  if (identities.empty())
    return Result<ResourceSet>(ResourceSet{});
  if (!impl_)
    return Result<ResourceSet>(Status{ErrorCode::NotFound, "missing resource"});
  check(impl_->resources.consume({identities.size()}));
  ResourceVector<ImmutableResource> selected{
      ResourceAllocator<ImmutableResource>(impl_->resources)};
  selected.reserve(identities.size());
  for (const auto& id : identities) {
    if (cancellation.cancelled())
      return Result<ResourceSet>(Status{ErrorCode::Cancelled, {}});
    auto value = find(id);
    if (!value.ok())
      return Result<ResourceSet>(value.status());
    selected.push_back(value.take_value());
  }
  return create_view(selected.data(), selected.size(), impl_->resources,
                     cancellation, UINT64_MAX);
} catch (const Stop& stop) {
  return Result<ResourceSet>(stop.status);
} catch (const std::bad_alloc&) {
  return Result<ResourceSet>(
      Status{ErrorCode::ResourceExhausted, "resource selection capacity"});
}
Result<ResourceSet> ResourceSet::reference(
    const ResourceBudget& resources) const {
  return impl_ ? create_view(impl_->values.data(), impl_->values.size(),
                             resources, {}, UINT64_MAX)
               : Result<ResourceSet>(ResourceSet{});
}
Result<ResourceSet> ResourceSet::unite(const ResourceSet& other) const try {
  if (!impl_)
    return Result<ResourceSet>(other);
  if (!other.impl_ || impl_ == other.impl_)
    return Result<ResourceSet>(*this);
  check(impl_->resources.consume({size() + other.size()}));
  ResourceVector<ImmutableResource> values{
      ResourceAllocator<ImmutableResource>(impl_->resources)};
  values.insert(values.end(), impl_->values.begin(), impl_->values.end());
  values.insert(values.end(), other.impl_->values.begin(),
                other.impl_->values.end());
  return create_view(values.data(), values.size(), impl_->resources, {},
                     UINT64_MAX);
} catch (const Stop& stop) {
  return Result<ResourceSet>(stop.status);
} catch (const std::bad_alloc&) {
  return Result<ResourceSet>(
      Status{ErrorCode::ResourceExhausted, "resource union capacity"});
}
}  // namespace ps
