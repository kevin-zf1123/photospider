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

#include "data/result_support.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/data/result.hpp"

namespace ps {
namespace {
bool key_valid(std::string_view key, std::size_t maximum) {
  return !key.empty() && key.size() <= maximum &&
         std::all_of(key.begin(), key.end(),
                     [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
}  // namespace
std::vector<std::uint64_t> ResultTensorSpec::sample_shape() const {
  std::vector<std::uint64_t> shape(batch_axes.begin(), batch_axes.end());
  shape.insert(shape.end(), descriptor.shape.begin(), descriptor.shape.end());
  return shape;
}
std::vector<std::uint64_t> ResultTensorSpec::observation_shape() const {
  auto shape = sample_shape();
  shape.resize(shape.size() - atomic_trailing_axes);
  if (shape.empty())
    shape.push_back(1);
  return shape;
}
Result<std::uint64_t> ResultTensorSpec::sample_count() const {
  auto valid = data_internal::tensor_topology(*this);
  if (!valid.ok())
    return Result<std::uint64_t>(valid);
  return Region::whole(sample_shape()).element_count();
}
Result<Footprint> ResultTensorSpec::close_samples(
    const Footprint& samples, const FootprintLimits& limits) const {
  auto valid = data_internal::tensor_topology(*this);
  if (!valid.ok())
    return Result<Footprint>(valid);
  if (!samples.valid() || samples.shape() != sample_shape())
    return Result<Footprint>(data_internal::invalid_schema());
  ResourceLease bridge;
  if (const auto* root = resource_internal::metadata_budget()) {
    const auto bytes =
        samples.boxes().size() *
        (sizeof(Region) + samples.shape().size() * sizeof(RegionDimension));
    auto admission = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admission.ok())
      return Result<Footprint>(admission.status());
    bridge = admission.take_value();
  }
  std::vector<Region> boxes;
  boxes.reserve(samples.boxes().size());
  for (const auto& box : samples.boxes()) {
    auto dimensions = box.dimensions();
    if (!box.empty()) {
      for (std::size_t axis = descriptor.shape.size() - atomic_trailing_axes;
           axis < descriptor.shape.size(); ++axis)
        dimensions[axis + batch_axes.size()] = {0, descriptor.shape[axis]};
    }
    boxes.emplace_back(std::move(dimensions));
  }
  return Footprint::from_regions(sample_shape(), boxes, limits);
}
const Footprint& ResultDescriptor::tensor_coverage(std::uint32_t slot) const {
  if (slot >= tensor_count_)
    throw std::out_of_range("invalid image slot");
  return tensors_[slot];
}
const ResultPublicationScope* SchemaTemplate::publication_scope(
    std::uint32_t slot) const noexcept {
  for (const auto& scope : publication_scopes)
    for (auto member : scope.members)
      if (member == slot)
        return &scope;
  return nullptr;
}
Status SchemaTemplate::validate() const {
  if (!key_valid(id, 128) || !version || tensors.empty() || tensors.size() > 16)
    return data_internal::invalid_schema();
  std::array<std::string_view, 64> keys;
  std::size_t key_count = 0;
  const auto add_key = [&](std::string_view key) {
    if (std::find(keys.begin(), keys.begin() + key_count, key) !=
        keys.begin() + key_count)
      return false;
    keys[key_count++] = key;
    return true;
  };

  for (const auto& image : tensors) {
    if (!key_valid(image.key, 128) || !add_key(image.key) ||
        !data_internal::tensor_topology(image).ok())
      return data_internal::invalid_schema();
    auto validated = input_internal::validate_value_structure(image.descriptor);
    if (!validated.ok())
      return validated;
  }
  std::uint32_t seen = 0, previous = 0;
  bool first_scope = true;
  if (publication_scopes.size() > 16)
    return data_internal::invalid_schema();
  for (const auto& scope : publication_scopes) {
    if (scope.members.empty() || scope.members.size() > tensors.size() ||
        (scope.policy != ResultPublicationPolicy::CompleteBundle &&
         scope.policy != ResultPublicationPolicy::IndependentChunks &&
         scope.policy != ResultPublicationPolicy::StablePrefix) ||
        (scope.policy == ResultPublicationPolicy::StablePrefix &&
         scope.members.size() != 1) ||
        (!first_scope && scope.members.front() <= previous))
      return data_internal::invalid_schema();
    previous = scope.members.front();
    first_scope = false;
    std::uint32_t last = 0;
    bool first_member = true;
    for (auto member : scope.members) {
      if (member >= tensors.size() || (seen & (1U << member)) ||
          (!first_member && member <= last))
        return data_internal::invalid_schema();
      seen |= 1U << member;
      last = member;
      first_member = false;
    }
  }
  return Status::success();
}
namespace {
template <class String>
void encode_schema(String* destination, const SchemaTemplate& schema) {
  auto& bytes = *destination;
  auto word = [&](std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
      bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
  };
  auto text = [&](std::string_view value) {
    word(value.size());
    bytes.append(value.data(), value.size());
  };
  text("photospider.result-schema.v6");
  text(schema.id);
  word(schema.version);

  word(schema.tensors.size());
  for (const auto& image : schema.tensors) {
    text(image.key);
    word(image.batch_axes.size());
    for (auto n : image.batch_axes)
      word(n);
    word(image.atomic_trailing_axes);
    word(image.layout.spatial);
    word(static_cast<std::uint32_t>(image.descriptor.element_type));
    word(image.descriptor.shape.size());
    for (auto n : image.descriptor.shape)
      word(n);
    word(image.layout.spatial ? image.layout.height_axis : 0);
    word(image.layout.spatial ? image.layout.width_axis : 0);
    word(image.layout.spatial && image.layout.channel_axis
             ? *image.layout.channel_axis + 1
             : 0);
  }
  word(schema.publication_scopes.size());
  for (const auto& scope : schema.publication_scopes) {
    word(static_cast<std::uint32_t>(scope.policy));
    word(scope.members.size());
    for (auto member : scope.members)
      word(member);
  }
}
}  // namespace
std::uint64_t SchemaTemplate::canonical_size() const noexcept {
  std::uint64_t size =
      8 + sizeof("photospider.result-schema.v6") - 1 + 8 + id.size() + 8 + 16;
  for (const auto& image : tensors) {
    size += 8 + image.key.size() + 64 +
            (image.descriptor.shape.size() + image.batch_axes.size()) * 8;
  }
  for (const auto& scope : publication_scopes)
    size += 16 + 8 * scope.members.size();
  return size;
}
ResourceString SchemaTemplate::canonical() const {
  if (!validate().ok())
    throw std::invalid_argument("invalid result schema canonicalization");
  if (auto* budget = resource_internal::metadata_budget()) {
    auto encoded = managed_canonical(*budget);
    if (!encoded.ok())
      throw std::bad_alloc();
    return encoded.take_value();
  }
  ResourceString bytes;
  bytes.reserve(canonical_size());
  encode_schema(&bytes, *this);
  return bytes;
}
Result<ResourceString> SchemaTemplate::managed_canonical(
    const ResourceBudget& budget) const {
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(budget))
    scope.emplace(budget);
  auto status = validate();
  if (!status.ok())
    return Result<ResourceString>(status);
  auto work = budget.consume({canonical_size()});
  if (!work.ok())
    return Result<ResourceString>(work);
  try {
    ResourceString bytes{ResourceAllocator<char>(budget)};
    bytes.reserve(canonical_size());
    encode_schema(&bytes, *this);
    return Result<ResourceString>(std::move(bytes));
  } catch (const std::bad_alloc&) {
    return Result<ResourceString>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<SchemaTemplate> SchemaTemplate::managed_copy(
    const ResourceBudget& budget) const {
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(budget))
    scope.emplace(budget);
  auto status = validate();
  if (!status.ok())
    return Result<SchemaTemplate>(status);
  auto work = budget.consume({canonical_size()});
  if (!work.ok())
    return Result<SchemaTemplate>(work);
  try {
    SchemaTemplate copy;
    copy.id =
        ResourceString(id.data(), id.size(), ResourceAllocator<char>(budget));
    copy.version = version;
    copy.tensors = ResourceVector<ResultTensorSpec>(
        ResourceAllocator<ResultTensorSpec>(budget));
    copy.tensors.reserve(tensors.size());
    for (const auto& image : tensors) {
      std::uint64_t bytes =
          (image.descriptor.shape.size() + image.batch_axes.size()) *
          sizeof(std::uint64_t);
      auto lease = budget.reserve(ResourceCapacity::host(bytes, bytes));
      if (!lease.ok())
        return Result<SchemaTemplate>(lease.status());
      auto target = image;
      target.key = ResourceString(image.key.data(), image.key.size(),
                                  ResourceAllocator<char>(budget));
      target.batch_axes = ResourceVector<std::uint64_t>(
          image.batch_axes.begin(), image.batch_axes.end(),
          ResourceAllocator<std::uint64_t>(budget));
      target.metadata_owner = lease.take_value();
      copy.tensors.push_back(std::move(target));
    }
    copy.publication_scopes = ResourceVector<ResultPublicationScope>(
        ResourceAllocator<ResultPublicationScope>(budget));
    for (const auto& scope : publication_scopes) {
      ResultPublicationScope owned;
      owned.policy = scope.policy;
      owned.members = ResourceVector<std::uint32_t>(
          scope.members.begin(), scope.members.end(),
          ResourceAllocator<std::uint32_t>(budget));
      copy.publication_scopes.push_back(std::move(owned));
    }
    return Result<SchemaTemplate>(std::move(copy));
  } catch (const std::bad_alloc&) {
    return Result<SchemaTemplate>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
bool SchemaTemplate::same_schema(const SchemaTemplate& other) const noexcept {
  if (id != other.id || version != other.version ||
      tensors.size() != other.tensors.size() ||
      publication_scopes.size() != other.publication_scopes.size())
    return false;
  for (std::size_t i = 0; i < tensors.size(); ++i) {
    const auto& a = tensors[i];
    const auto& b = other.tensors[i];
    if (a.key != b.key || a.batch_axes != b.batch_axes ||
        a.atomic_trailing_axes != b.atomic_trailing_axes ||
        a.layout.spatial != b.layout.spatial ||
        a.descriptor.shape != b.descriptor.shape ||
        a.descriptor.element_type != b.descriptor.element_type ||
        (a.layout.spatial && (a.layout.height_axis != b.layout.height_axis ||
                              a.layout.width_axis != b.layout.width_axis ||
                              a.layout.channel_axis != b.layout.channel_axis)))
      return false;
  }

  for (std::size_t i = 0; i < publication_scopes.size(); ++i)
    if (publication_scopes[i].policy != other.publication_scopes[i].policy ||
        publication_scopes[i].members != other.publication_scopes[i].members)
      return false;
  return true;
}
}  // namespace ps
