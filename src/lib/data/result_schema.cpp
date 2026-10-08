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
#include "data/value_validation.hpp"
#include "photospider/data/representation.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
namespace {
bool key_valid(std::string_view key, std::size_t maximum) {
  return !key.empty() && key.size() <= maximum &&
         std::all_of(key.begin(), key.end(),
                     [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
bool extent_valid(const ResultExtent& extent, bool resolved,
                  std::size_t field_count) {
  if (!extent.divisor || extent.input >= 1024 || extent.axis >= 8)
    return false;
  switch (extent.kind) {
    case ResultExtentKind::Fixed:
      return extent.value <= UINT64_MAX - extent.offset;
    case ResultExtentKind::InputAxis:
    case ResultExtentKind::InputElements:
      return !resolved;
    case ResultExtentKind::FieldRows:
      return extent.field < field_count;
    case ResultExtentKind::RuntimeCount:
      return extent.divisor == 1 && extent.offset == 0;
  }
  return false;
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
  const auto tuple = input_internal::tuple_channel_axis(descriptor, facets);
  for (const auto& box : samples.boxes()) {
    auto dimensions = box.dimensions();
    if (!box.empty()) {
      if (tuple)
        dimensions[*tuple + batch_axes.size()] = {0, descriptor.shape[*tuple]};
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
Status SchemaTemplate::validate(bool resolved) const {
  // These historical schemas pack image planes into result fields. Structural
  // tensors now require PlanarImage owners and cannot use ResultBuilder.
  if (id == "photospider.layer" || id == "photospider.layer_response" ||
      id == "photospider.raw_rgba_sum" ||
      id == "photospider.layer_contributions" ||
      id == "photospider.weighted_layer_sum" ||
      id == "photospider.optional_layer" ||
      std::any_of(metadata.begin(), metadata.end(),
                  [](const ResultFacet& facet) {
                    return facet.key == "photospider.layer";
                  }))
    return Status::failure(ErrorCode::TypeMismatch,
                           "legacy layer schema requires planar storage");
  if (!key_valid(id, 128) || !version || (fields.empty() && tensors.empty()) ||
      fields.size() + tensors.size() > 16 || domain.size() > 8 ||
      (publication != PublishPolicy::CompleteBundle &&
       publication != PublishPolicy::IndependentChunks &&
       publication != PublishPolicy::StablePrefix) ||
      metadata.size() > 64)
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
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto& field = fields[i];
    const auto type = static_cast<std::uint32_t>(field.element_type);
    if (!key_valid(field.key, 128) || !add_key(field.key) || type < 1 ||
        type > 7 || field.record_shape.size() > 7 ||
        !extent_valid(field.rows, resolved, i) ||
        !row_bytes(static_cast<std::uint32_t>(i)).ok())
      return data_internal::invalid_schema();
  }
  for (const auto& image : tensors) {
    if (!key_valid(image.key, 128) || !add_key(image.key) ||
        !data_internal::tensor_topology(image).ok())
      return data_internal::invalid_schema();
    const auto tuple =
        input_internal::tuple_channel_axis(image.descriptor, image.facets);
    if (input_internal::color_array(image.facets) &&
        image.atomic_trailing_axes > 1)
      return {ErrorCode::TypeMismatch,
              "color observations group exactly one trailing axis",
              FailureReason::None,
              {FailureOrigin::Schema, FailureScope::Unspecified}};
    if (tuple && image.layout.spatial &&
        (image.descriptor.shape.size() != 3 || !image.layout.channel_axis ||
         *tuple != *image.layout.channel_axis))
      return data_internal::invalid_schema();
    for (const auto& facet : image.facets) {
      if (facet.key == "photospider.image" &&
          (!image.layout.spatial || image.layout.height_axis != 0 ||
           image.layout.width_axis != 1))
        return data_internal::invalid_schema();
      if (facet.key == "photospider.tensor-description") {
        auto tensor = decode_tensor_description(facet);
        if (!tensor.ok())
          return tensor.status();
        if (image.layout.spatial && tensor.value().channel_axis &&
            tensor.value().channel_axis != image.layout.channel_axis)
          return data_internal::invalid_schema();
      }
    }
    auto validated =
        input_internal::validate_value_metadata(image.descriptor, image.facets);
    if (!validated.ok())
      return validated;
  }
  for (const auto& extent : domain)
    if (!extent_valid(extent, resolved, fields.size()) ||
        extent.kind == ResultExtentKind::RuntimeCount)
      return data_internal::invalid_schema();
  key_count = 0;
  std::uint64_t payload = 0;
  for (const auto& facet : metadata) {
    if (!key_valid(facet.key, 256) || !facet.version || !add_key(facet.key) ||
        facet.payload.size() > 65536 ||
        payload > 1048576 - facet.payload.size())
      return data_internal::invalid_schema();
    payload += facet.payload.size();
  }
  return validate_representation_schema(*this);
}
Result<ResourceBindings> SchemaTemplate::select_resources(
    const ResourceBindings& supplied) const try {
  ResourceBindings accepted;
  for (const auto& image : tensors) {
    auto subset = supplied.select(image.facets);
    if (!subset.ok())
      return subset;
    auto joined = accepted.unite(subset.value());
    if (!joined.ok())
      return joined;
    accepted = joined.take_value();
  }
  std::uint64_t bytes = metadata.size() * sizeof(ValueFacet);
  for (const auto& facet : metadata)
    bytes += facet.key.size() + 1 + facet.payload.size();
  ResourceLease bridge;
  if (const auto* root = resource_internal::metadata_budget()) {
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Result<ResourceBindings>(admitted.status());
    bridge = admitted.take_value();
  }
  std::vector<ValueFacet> facets;
  facets.reserve(metadata.size());
  for (const auto& f : metadata)
    facets.push_back(
        {std::string(f.key), f.version,
         std::vector<uint8_t>(f.payload.begin(), f.payload.end())});
  auto subset = supplied.select(facets);
  if (!subset.ok())
    return subset;
  return accepted.unite(subset.value());
} catch (const std::bad_alloc&) {
  return Result<ResourceBindings>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<std::uint64_t> SchemaTemplate::row_bytes(std::uint32_t field) const {
  if (field >= fields.size())
    return Result<std::uint64_t>(data_internal::invalid_schema());
  const auto type = static_cast<std::uint32_t>(fields[field].element_type);
  if (type < 1 || type > 7)
    return Result<std::uint64_t>(data_internal::invalid_schema());
  std::uint64_t bytes = Value::element_size(fields[field].element_type);
  for (const auto extent : fields[field].record_shape) {
    if (!extent || bytes > static_cast<std::uint64_t>(INT64_MAX) / extent)
      return Result<std::uint64_t>(data_internal::invalid_schema());
    bytes *= extent;
  }
  return Result<std::uint64_t>(bytes);
}
Result<SchemaTemplate> SchemaTemplate::resolve(
    const std::vector<std::vector<std::uint64_t>>& input_domains) const {
  auto valid = validate();
  if (!valid.ok())
    return Result<SchemaTemplate>(valid);
  auto result = *this;
  auto resolve_extent = [&](ResultExtent* extent) -> Status {
    if (extent->kind != ResultExtentKind::InputAxis &&
        extent->kind != ResultExtentKind::InputElements)
      return Status::success();
    if (extent->input >= input_domains.size())
      return data_internal::invalid_schema();
    const auto& shape = input_domains[extent->input];
    std::uint64_t count = 1;
    if (extent->kind == ResultExtentKind::InputAxis) {
      if (extent->axis >= shape.size())
        return data_internal::invalid_schema();
      count = shape[extent->axis];
    } else {
      if (shape.empty())
        return data_internal::invalid_schema();
      for (auto n : shape) {
        if (n && count > UINT64_MAX / n)
          return Status{ErrorCode::ResourceExhausted,
                        "schema input domain overflow"};
        count *= n;
      }
    }
    auto resolved_extent = data_internal::scaled(*extent, count);
    if (!resolved_extent.ok())
      return resolved_extent.status();
    *extent = ResultExtent{ResultExtentKind::Fixed, resolved_extent.value()};
    return Status::success();
  };
  for (auto& field : result.fields) {
    auto status = resolve_extent(&field.rows);
    if (!status.ok())
      return Result<SchemaTemplate>(status);
  }
  for (auto& extent : result.domain) {
    auto status = resolve_extent(&extent);
    if (!status.ok())
      return Result<SchemaTemplate>(status);
  }
  return Result<SchemaTemplate>(std::move(result));
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
  auto extent = [&](const ResultExtent& e) {
    word(static_cast<std::uint32_t>(e.kind));
    word(e.value);
    word(e.input);
    word(e.axis);
    word(e.field);
    word(e.divisor);
    word(e.offset);
  };
  text("photospider.result-schema.v3");
  text(schema.id);
  word(schema.version);
  word(static_cast<std::uint32_t>(schema.publication));
  word(schema.fields.size());
  for (const auto& field : schema.fields) {
    text(field.key);
    word(static_cast<std::uint32_t>(field.element_type));
    extent(field.rows);
    word(field.record_shape.size());
    for (auto size : field.record_shape)
      word(size);
  }
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
    word(image.layout.groups.size());
    for (const auto& group : image.layout.groups) {
      text(group.role);
      word(group.first_channel);
      word(group.channel_count);
    }
    word(image.facets.size());
    for (const auto& facet : image.facets) {
      text(facet.key);
      word(facet.version);
      word(facet.payload.size());
      if (!facet.payload.empty())
        bytes.append(reinterpret_cast<const char*>(facet.payload.data()),
                     facet.payload.size());
    }
  }
  word(schema.domain.size());
  for (const auto& e : schema.domain)
    extent(e);
  std::array<std::size_t, 64> order{};
  for (std::size_t i = 0; i < schema.metadata.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.begin() + schema.metadata.size(),
            [&](auto a, auto b) {
              return schema.metadata[a].key < schema.metadata[b].key;
            });
  word(schema.metadata.size());
  for (std::size_t i = 0; i < schema.metadata.size(); ++i) {
    const auto& facet = schema.metadata[order[i]];
    text(facet.key);
    word(facet.version);
    word(facet.payload.size());
    if (!facet.payload.empty())
      bytes.append(reinterpret_cast<const char*>(facet.payload.data()),
                   facet.payload.size());
  }
}
bool same_extent(const ResultExtent& a, const ResultExtent& b) {
  return a.kind == b.kind && a.value == b.value && a.input == b.input &&
         a.axis == b.axis && a.field == b.field && a.divisor == b.divisor &&
         a.offset == b.offset;
}
}  // namespace
std::uint64_t SchemaTemplate::canonical_size() const noexcept {
  std::uint64_t size =
      8 + sizeof("photospider.result-schema.v3") - 1 + 8 + id.size() + 24 + 24;
  for (const auto& field : fields)
    size += 8 + field.key.size() + 8 + 56 + 8 + field.record_shape.size() * 8;
  for (const auto& image : tensors) {
    size += 8 + image.key.size() + 80 +
            (image.descriptor.shape.size() + image.batch_axes.size()) * 8;
    for (const auto& group : image.layout.groups)
      size += 24 + group.role.size();
    for (const auto& facet : image.facets)
      size += 24 + facet.key.size() + facet.payload.size();
  }
  size += domain.size() * 56;
  for (const auto& facet : metadata)
    size += 8 + facet.key.size() + 16 + facet.payload.size();
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
    copy.publication = publication;
    copy.fields = ResourceVector<ResultFieldSpec>(
        ResourceAllocator<ResultFieldSpec>(budget));
    copy.fields.reserve(fields.size());
    for (const auto& field : fields) {
      ResultFieldSpec target;
      target.key = ResourceString(field.key.data(), field.key.size(),
                                  ResourceAllocator<char>(budget));
      target.element_type = field.element_type;
      target.rows = field.rows;
      target.record_shape = ResourceVector<std::uint64_t>(
          field.record_shape.begin(), field.record_shape.end(),
          ResourceAllocator<std::uint64_t>(budget));
      copy.fields.push_back(std::move(target));
    }
    copy.tensors = ResourceVector<ResultTensorSpec>(
        ResourceAllocator<ResultTensorSpec>(budget));
    copy.tensors.reserve(tensors.size());
    for (const auto& image : tensors) {
      std::uint64_t bytes =
          (image.descriptor.shape.size() + image.batch_axes.size()) *
          sizeof(std::uint64_t);
      bytes += image.layout.groups.size() * sizeof(ImageComponentGroup);
      bytes += image.facets.size() * sizeof(ValueFacet);
      for (const auto& group : image.layout.groups)
        bytes += group.role.size() + 1;
      for (const auto& facet : image.facets)
        bytes += facet.key.size() + 1 + facet.payload.size();
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
    copy.domain = ResourceVector<ResultExtent>(
        domain.begin(), domain.end(), ResourceAllocator<ResultExtent>(budget));
    copy.metadata =
        ResourceVector<ResultFacet>(ResourceAllocator<ResultFacet>(budget));
    copy.metadata.reserve(metadata.size());
    for (const auto& facet : metadata) {
      ResultFacet target;
      target.key = ResourceString(facet.key.data(), facet.key.size(),
                                  ResourceAllocator<char>(budget));
      target.version = facet.version;
      target.payload = ResourceVector<std::uint8_t>(
          facet.payload.begin(), facet.payload.end(),
          ResourceAllocator<std::uint8_t>(budget));
      copy.metadata.push_back(std::move(target));
    }
    return Result<SchemaTemplate>(std::move(copy));
  } catch (const std::bad_alloc&) {
    return Result<SchemaTemplate>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
bool SchemaTemplate::same_schema(const SchemaTemplate& other) const noexcept {
  if (id != other.id || version != other.version ||
      publication != other.publication ||
      fields.size() != other.fields.size() ||
      tensors.size() != other.tensors.size() ||
      domain.size() != other.domain.size() ||
      metadata.size() != other.metadata.size())
    return false;
  for (std::size_t i = 0; i < tensors.size(); ++i) {
    const auto& a = tensors[i];
    const auto& b = other.tensors[i];
    if (a.key != b.key || a.batch_axes != b.batch_axes ||
        a.atomic_trailing_axes != b.atomic_trailing_axes ||
        a.layout.spatial != b.layout.spatial ||
        a.descriptor.shape != b.descriptor.shape ||
        a.descriptor.element_type != b.descriptor.element_type ||
        (a.layout.spatial &&
         (a.layout.height_axis != b.layout.height_axis ||
          a.layout.width_axis != b.layout.width_axis ||
          a.layout.channel_axis != b.layout.channel_axis)) ||
        a.layout.groups.size() != b.layout.groups.size() ||
        !input_internal::same_facets(a.facets, b.facets))
      return false;
    for (std::size_t j = 0; j < a.layout.groups.size(); ++j)
      if (a.layout.groups[j].role != b.layout.groups[j].role ||
          a.layout.groups[j].first_channel !=
              b.layout.groups[j].first_channel ||
          a.layout.groups[j].channel_count != b.layout.groups[j].channel_count)
        return false;
  }
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto& a = fields[i];
    const auto& b = other.fields[i];
    if (a.key != b.key || a.element_type != b.element_type ||
        !same_extent(a.rows, b.rows) || a.record_shape != b.record_shape)
      return false;
  }
  for (std::size_t i = 0; i < domain.size(); ++i)
    if (!same_extent(domain[i], other.domain[i]))
      return false;
  for (const auto& a : metadata) {
    auto b = std::find_if(other.metadata.begin(), other.metadata.end(),
                          [&](const auto& v) { return v.key == a.key; });
    if (b == other.metadata.end() || a.version != b->version ||
        a.payload != b->payload)
      return false;
  }
  return true;
}
}  // namespace ps
