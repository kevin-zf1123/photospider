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
template <class T, class Work>
void admit(const T* values, std::size_t count, ResourceVector<T>* out,
           const ResourceBudget& resources, Work work) {
  if (count && !values)
    throw Stop{invalid("null resource handles")};
  out->reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    work(1);
    if (!values[i].valid())
      throw Stop{invalid("invalid resource handle")};
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
          throw Stop{invalid("resource identity collision")};
        for (std::size_t offset = 0; offset < a.size();) {
          const auto n = std::min<std::size_t>(1024, a.size() - offset);
          work(n);
          if (std::memcmp(a.data() + offset, b.data() + offset, n))
            throw Stop{invalid("resource identity collision")};
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
Result<T> lookup(const ResourceVector<T>& values,
                 const ColorProfileIdentity& id) {
  const auto i = std::lower_bound(values.begin(), values.end(), id,
                                  [](const auto& value, const auto& key) {
                                    return value.identity() < key;
                                  });
  if (i == values.end() || !(i->identity() == id))
    return Result<T>(invalid("unresolved immutable resource identity"));
  return Result<T>(*i);
}
}  // namespace
struct ResourceBindings::Impl {
  ResourceBudget resources;
  ResourceVector<IccProfile> profiles;
  ResourceVector<OcioConfigResource> configs;
  explicit Impl(const ResourceBudget& root)
      : resources(root),
        profiles(ResourceAllocator<IccProfile>(root)),
        configs(ResourceAllocator<OcioConfigResource>(root)) {}
};
Result<ResourceBindings> ResourceBindings::create(
    const std::vector<IccProfile>& profiles, const ResourceBudget& resources,
    const CancellationToken& cancellation, std::uint64_t maximum_work) {
  return create_view(profiles.data(), profiles.size(), resources, cancellation,
                     maximum_work);
}
Result<ResourceBindings> ResourceBindings::create(
    const std::vector<IccProfile>& profiles,
    const std::vector<OcioConfigResource>& configs,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    std::uint64_t maximum_work) {
  return create_view(profiles.data(), profiles.size(), resources, cancellation,
                     maximum_work, configs.data(), configs.size());
}
Result<ResourceBindings> ResourceBindings::create_view(
    const IccProfile* profiles, std::size_t count,
    const ResourceBudget& resources, const CancellationToken& cancellation,
    std::uint64_t maximum_work, const OcioConfigResource* configs,
    std::size_t config_count) try {
  const auto work = [&](std::uint64_t n) {
    if (cancellation.cancelled())
      throw Stop{{ErrorCode::Cancelled, {}}};
    if (n > maximum_work)
      throw Stop{{ErrorCode::ResourceExhausted, "resource binding work limit",
                  FailureReason::WorkLimit}};
    maximum_work -= n;
    check(resources.consume({n}));
  };
  work(0);
  if (!count && !config_count)
    return Result<ResourceBindings>(ResourceBindings{});
  auto impl =
      std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources), resources);
  admit(profiles, count, &impl->profiles, resources, work);
  admit(configs, config_count, &impl->configs, resources, work);
  work(1);
  return Result<ResourceBindings>(ResourceBindings(std::move(impl)));
} catch (const Stop& stop) {
  return Result<ResourceBindings>(stop.status);
} catch (const std::bad_alloc&) {
  return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted,
                                         "resource binding metadata capacity",
                                         FailureReason::CapacityLimit});
}
std::size_t ResourceBindings::size() const noexcept {
  return profile_count() + config_count();
}
std::size_t ResourceBindings::profile_count() const noexcept {
  return impl_ ? impl_->profiles.size() : 0;
}
std::size_t ResourceBindings::config_count() const noexcept {
  return impl_ ? impl_->configs.size() : 0;
}
Result<IccProfile> ResourceBindings::icc_profile(
    const ColorProfileIdentity& id) const {
  return impl_ ? lookup(impl_->profiles, id)
               : Result<IccProfile>(invalid("unresolved ICC profile identity"));
}
Result<OcioConfigResource> ResourceBindings::ocio_config(
    const ColorProfileIdentity& id) const {
  return impl_ ? lookup(impl_->configs, id)
               : Result<OcioConfigResource>(
                     invalid("unresolved OCIO config identity"));
}
Result<IccProfile> ResourceBindings::profile_at(std::size_t index) const {
  if (index >= profile_count())
    return Result<IccProfile>(invalid("profile index outside set"));
  return Result<IccProfile>(impl_->profiles[index]);
}
Result<OcioConfigResource> ResourceBindings::config_at(
    std::size_t index) const {
  if (index >= config_count())
    return Result<OcioConfigResource>(invalid("config index outside set"));
  return Result<OcioConfigResource>(impl_->configs[index]);
}
Result<ResourceBindings> ResourceBindings::select(
    const std::vector<ValueFacet>& facets) const try {
  // No global/default resource root is created by an empty lookup.
  ResourceVector<IccProfile> selected{
      impl_ ? ResourceAllocator<IccProfile>(impl_->resources)
            : ResourceAllocator<IccProfile>{}};
  ResourceVector<OcioConfigResource> configs{
      impl_ ? ResourceAllocator<OcioConfigResource>(impl_->resources)
            : ResourceAllocator<OcioConfigResource>{}};
  const auto profile = [&](const std::optional<ColorProfileIdentity>& id) {
    if (!id)
      return;
    auto value = icc_profile(*id);
    if (!value.ok())
      throw Stop{value.status()};
    selected.push_back(value.take_value());
  };
  const auto configured =
      [&](const std::optional<TensorConfiguredSpace>& description) {
        if (!description)
          return;
        auto value = ocio_config(description->config);
        if (!value.ok())
          throw Stop{value.status()};
        check(value.value().validate_space(description->space,
                                           description->reference_space));
        configs.push_back(value.take_value());
      };
  const auto interpretation = [&](const TensorInterpretation& d) {
    profile(d.profile);
    configured(d.configured);
    if (d.profile && !d.model.empty() && selected.back().model() != d.model)
      throw Stop{invalid("ICC header model disagrees with interpretation")};
  };
  for (const auto& facet : facets) {
    if (impl_)
      check(impl_->resources.consume({1 + facet.payload.size()}));
    if (facet.key == "photospider.color-array") {
      auto d = decode_color_array(facet);
      if (!d.ok())
        throw Stop{d.status()};
      profile(d.value().profile);
      if (d.value().profile && selected.back().model() != "cmyk")
        throw Stop{invalid("legacy CMYK facet requires a CMYK profile")};
    } else if (facet.key == "photospider.tensor-description") {
      auto result = decode_tensor_description(facet);
      if (!result.ok())
        throw Stop{result.status()};
      const auto& d = result.value();
      interpretation({d.model, d.primaries, d.transfer, d.reference,
                      d.association, d.white, d.primaries_xy, d.profile,
                      d.convention, d.configured, d.analytic_binding});
      for (const auto& c : d.channels)
        if (c.interpretation)
          interpretation(*c.interpretation);
      if (d.component && d.component->interpretation)
        interpretation(*d.component->interpretation);
      for (const auto& g : d.groups)
        interpretation(g.interpretation);
    }
  }
  if (!impl_)
    return Result<ResourceBindings>(ResourceBindings{});
  if (selected.empty() && configs.empty())
    return Result<ResourceBindings>(ResourceBindings{});
  // Common one-resource result keeps the sealed set and its charge.
  if (size() == 1)
    return Result<ResourceBindings>(*this);
  return create_view(selected.data(), selected.size(), impl_->resources, {},
                     UINT64_MAX, configs.data(), configs.size());
} catch (const Stop& stop) {
  return Result<ResourceBindings>(stop.status);
} catch (const std::bad_alloc&) {
  return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted,
                                         "resource selection metadata capacity",
                                         FailureReason::CapacityLimit});
}
Result<ResourceBindings> ResourceBindings::reference(
    const ResourceBudget& resources) const {
  if (!impl_)
    return Result<ResourceBindings>(ResourceBindings{});
  return create_view(impl_->profiles.data(), impl_->profiles.size(), resources,
                     {}, UINT64_MAX, impl_->configs.data(),
                     impl_->configs.size());
}
Result<ResourceBindings> ResourceBindings::unite(
    const ResourceBindings& other) const try {
  if (!impl_)
    return Result<ResourceBindings>(other);
  if (!other.impl_ || impl_ == other.impl_)
    return Result<ResourceBindings>(*this);
  ResourceVector<IccProfile> profiles{
      ResourceAllocator<IccProfile>(impl_->resources)};
  ResourceVector<OcioConfigResource> configs{
      ResourceAllocator<OcioConfigResource>(impl_->resources)};
  check(impl_->resources.consume({size() + other.size()}));
  profiles.insert(profiles.end(), impl_->profiles.begin(),
                  impl_->profiles.end());
  profiles.insert(profiles.end(), other.impl_->profiles.begin(),
                  other.impl_->profiles.end());
  configs.insert(configs.end(), impl_->configs.begin(), impl_->configs.end());
  configs.insert(configs.end(), other.impl_->configs.begin(),
                 other.impl_->configs.end());
  return create_view(profiles.data(), profiles.size(), impl_->resources, {},
                     UINT64_MAX, configs.data(), configs.size());
} catch (const Stop& stop) {
  return Result<ResourceBindings>(stop.status);
} catch (const std::bad_alloc&) {
  return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted,
                                         "resource union metadata capacity",
                                         FailureReason::CapacityLimit});
}
}  // namespace ps
