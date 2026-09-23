#include "photospider/data/resource_bindings.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/data/color_array.hpp"
#include "photospider/data/tensor_description.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message, FailureReason::InvalidDomain};
}
struct Stop {
  Status status;
};
void check(Status status) {
  if (!status.ok())
    throw Stop{std::move(status)};
}
}  // namespace
struct ResourceBindings::Impl {
  ResourceBudget resources;
  ResourceVector<IccProfile> profiles;
  explicit Impl(const ResourceBudget& root)
      : resources(root), profiles(ResourceAllocator<IccProfile>(root)) {}
};
Result<ResourceBindings> ResourceBindings::create(
    const std::vector<IccProfile>& profiles, const ResourceBudget& resources,
    const CancellationToken& cancellation, std::uint64_t maximum_work) {
  return create_view(profiles.data(), profiles.size(), resources, cancellation,
                     maximum_work);
}
Result<ResourceBindings> ResourceBindings::create_view(
    const IccProfile* profiles, std::size_t count,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    std::uint64_t maximum_work) {
  try {
    const auto work = [&](std::uint64_t amount) {
      if (cancellation.cancelled())
        throw Stop{{ErrorCode::Cancelled, {}}};
      if (amount > maximum_work)
        throw Stop{{ErrorCode::ResourceExhausted, "resource binding work limit",
                    FailureReason::WorkLimit}};
      maximum_work -= amount;
      check(resources.consume({amount}));
    };
    work(0);
    if (!count)
      return Result<ResourceBindings>(ResourceBindings{});
    if (!profiles)
      return Result<ResourceBindings>(invalid("null resource binding handles"));
    auto impl = std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources),
                                           resources);
    impl->profiles.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      work(1);
      if (!profiles[i].valid())
        return Result<ResourceBindings>(invalid("invalid ICC binding handle"));
      impl->profiles.push_back(profiles[i]);
    }
    std::sort(impl->profiles.begin(), impl->profiles.end(),
              [&](const auto& a, const auto& b) {
                work(1);
                return a.identity() < b.identity();
              });
    std::size_t unique = 0;
    for (std::size_t i = 0; i < count; ++i) {
      work(1);
      const auto& incoming = impl->profiles[i];
      if (unique &&
          incoming.identity() == impl->profiles[unique - 1].identity()) {
        const auto& prior = impl->profiles[unique - 1];
        if (prior.storage().get() != incoming.storage().get()) {
          for (std::size_t offset = 0; offset < incoming.bytes().size();) {
            const auto n =
                std::min<std::size_t>(65536, incoming.bytes().size() - offset);
            work(n);
            if (std::memcmp(prior.bytes().data() + offset,
                            incoming.bytes().data() + offset, n))
              return Result<ResourceBindings>(
                  invalid("conflicting ICC bytes for resource identity"));
            offset += n;
          }
        }
        continue;
      }
      if (unique != i)
        impl->profiles[unique] = incoming;
      ++unique;
    }
    impl->profiles.resize(unique);
    for (auto& profile : impl->profiles) {
      work(1);
      auto admitted = profile.reference(resources);
      if (!admitted.ok())
        return Result<ResourceBindings>(admitted.status());
      profile = admitted.take_value();
    }
    work(1);
    return Result<ResourceBindings>(ResourceBindings(std::move(impl)));
  } catch (const Stop& stop) {
    return Result<ResourceBindings>(stop.status);
  } catch (const std::bad_alloc&) {
    return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted,
                                           "resource binding metadata capacity",
                                           FailureReason::CapacityLimit});
  }
}
std::size_t ResourceBindings::size() const noexcept {
  return impl_ ? impl_->profiles.size() : 0;
}
Result<IccProfile> ResourceBindings::icc_profile(
    const ColorProfileIdentity& identity) const {
  if (impl_) {
    const auto found =
        std::lower_bound(impl_->profiles.begin(), impl_->profiles.end(),
                         identity, [](const auto& profile, const auto& key) {
                           return profile.identity() < key;
                         });
    if (found != impl_->profiles.end() && found->identity() == identity)
      return Result<IccProfile>(*found);
  }
  return Result<IccProfile>(invalid("unresolved ICC profile identity"));
}
Result<IccProfile> ResourceBindings::profile_at(std::size_t index) const {
  if (index >= size())
    return Result<IccProfile>(invalid("resource binding index outside set"));
  return Result<IccProfile>(impl_->profiles[index]);
}
Result<ResourceBindings> ResourceBindings::select(
    const std::vector<ValueFacet>& facets) const {
  try {
    if (!impl_) {
      for (const auto& facet : facets)
        if (facet.key == "photospider.color-array") {
          auto description = decode_color_array(facet);
          if (!description.ok())
            return Result<ResourceBindings>(description.status());
          if (description.value().profile)
            return Result<ResourceBindings>(
                invalid("unresolved ICC profile identity"));
        } else if (facet.key == "photospider.tensor-description") {
          auto description = decode_tensor_description(facet);
          if (!description.ok())
            return Result<ResourceBindings>(description.status());
          const auto& d = description.value();
          bool has_profile = d.profile.has_value();
          for (const auto& c : d.channels)
            has_profile =
                has_profile || (c.interpretation && c.interpretation->profile);
          if (d.component && d.component->interpretation)
            has_profile =
                has_profile || d.component->interpretation->profile.has_value();
          for (const auto& g : d.groups)
            has_profile = has_profile || g.interpretation.profile.has_value();
          if (has_profile)
            return Result<ResourceBindings>(
                invalid("unresolved ICC profile identity"));
        }
      return Result<ResourceBindings>(ResourceBindings{});
    }
    ResourceVector<IccProfile> selected{
        ResourceAllocator<IccProfile>(impl_->resources)};
    bool matched = false;
    for (const auto& facet : facets) {
      auto status = impl_->resources.consume({1 + facet.payload.size()});
      if (!status.ok())
        return Result<ResourceBindings>(status);
      std::vector<ColorProfileIdentity> identities;
      std::optional<ColorProfileIdentity> identity;
      if (facet.key == "photospider.color-array") {
        auto description = decode_color_array(facet);
        if (!description.ok())
          return Result<ResourceBindings>(description.status());
        identity = description.value().profile;
      } else if (facet.key == "photospider.tensor-description") {
        auto description = decode_tensor_description(facet);
        if (!description.ok())
          return Result<ResourceBindings>(description.status());
        identity = description.value().profile;
        const auto& d = description.value();
        for (const auto& c : d.channels)
          if (c.interpretation && c.interpretation->profile)
            identities.push_back(*c.interpretation->profile);
        if (d.component && d.component->interpretation &&
            d.component->interpretation->profile)
          identities.push_back(*d.component->interpretation->profile);
        for (const auto& g : d.groups)
          if (g.interpretation.profile)
            identities.push_back(*g.interpretation.profile);
      }
      if (identity)
        identities.push_back(*identity);
      for (const auto& id : identities) {
        identity = id;
        if (!identity)
          continue;
        auto profile = icc_profile(*identity);
        if (!profile.ok())
          return Result<ResourceBindings>(profile.status());
        matched = true;
        if (impl_->profiles.size() != 1)
          selected.push_back(profile.take_value());
      }
    }
    if (matched && impl_->profiles.size() == 1)
      return Result<ResourceBindings>(*this);
    return create_view(selected.data(), selected.size(), impl_->resources, {},
                       UINT64_MAX);
  } catch (const std::bad_alloc&) {
    return Result<ResourceBindings>(Status{
        ErrorCode::ResourceExhausted, "resource selection metadata capacity",
        FailureReason::CapacityLimit});
  }
}
Result<ResourceBindings> ResourceBindings::reference(
    const ResourceBudget& resources) const {
  if (!impl_)
    return Result<ResourceBindings>(ResourceBindings{});
  return create_view(impl_->profiles.data(), impl_->profiles.size(), resources,
                     {}, UINT64_MAX);
}
Result<ResourceBindings> ResourceBindings::unite(
    const ResourceBindings& other) const {
  if (!impl_)
    return Result<ResourceBindings>(other);
  if (!other.impl_ || impl_ == other.impl_)
    return Result<ResourceBindings>(*this);
  try {
    ResourceVector<IccProfile> profiles{
        ResourceAllocator<IccProfile>(impl_->resources)};
    if (other.size() > SIZE_MAX - size())
      return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted,
                                             "resource union size overflow",
                                             FailureReason::CapacityLimit});
    profiles.reserve(size() + other.size());
    auto status = impl_->resources.consume({size() + other.size()});
    if (!status.ok())
      return Result<ResourceBindings>(status);
    profiles.insert(profiles.end(), impl_->profiles.begin(),
                    impl_->profiles.end());
    profiles.insert(profiles.end(), other.impl_->profiles.begin(),
                    other.impl_->profiles.end());
    return create_view(profiles.data(), profiles.size(), impl_->resources, {},
                       UINT64_MAX);
  } catch (const std::bad_alloc&) {
    return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted,
                                           "resource union metadata capacity",
                                           FailureReason::CapacityLimit});
  }
}
}  // namespace ps
