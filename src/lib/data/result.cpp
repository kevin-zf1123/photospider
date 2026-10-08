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

#include "core/stored_failure.hpp"
#include "data/affine_view.hpp"
#include "data/input_validation.hpp"
#include "data/result_host_access.hpp"
#include "data/result_window_access.hpp"
#include "execution/dependency_records.hpp"
#include "photospider/data/representation.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
namespace {
std::timed_mutex result_ownership_mutex;
struct ProducerWriteGuard final {
  std::atomic<bool>* active = nullptr;
  void release() noexcept {
    if (active) {
      active->store(false);
      active = nullptr;
    }
  }
  ~ProducerWriteGuard() { release(); }
};
Status invalid_schema() {
  return Status{ErrorCode::TypeMismatch,
                "invalid structured result schema or association"};
}
Status unavailable() {
  return Status{ErrorCode::NotFound, "structured result range is incomplete"};
}
bool key_valid(std::string_view key, std::size_t maximum = 128) {
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
Result<std::uint64_t> scaled(const ResultExtent& extent, std::uint64_t count) {
  count = count / extent.divisor + (count % extent.divisor != 0);
  if (count > UINT64_MAX - extent.offset)
    return Result<std::uint64_t>(
        Status{ErrorCode::ResourceExhausted, "result extent overflow"});
  return Result<std::uint64_t>(count + extent.offset);
}
}  // namespace
namespace {
PlanarImageLayout planar_layout(const ResultTensorLayout& layout) {
  return {layout.order,        layout.height_axis,     layout.width_axis,
          layout.channel_axis, layout.row_pitch_bytes, layout.groups};
}
bool singleton_batches(const ResultTensorSpec& spec, const Region& region) {
  for (std::size_t axis = 0; axis < spec.batch_axes.size(); ++axis)
    if (region.dimensions()[axis].extent != 1)
      return false;
  return true;
}
Status tensor_topology(const ResultTensorSpec& spec) {
  const auto rank = spec.descriptor.shape.size();
  if (!rank || rank > 8 || spec.batch_axes.size() > 8 - rank ||
      spec.atomic_trailing_axes > rank ||
      std::any_of(spec.descriptor.shape.begin(), spec.descriptor.shape.end(),
                  [](auto n) { return !n; }) ||
      std::any_of(spec.batch_axes.begin(), spec.batch_axes.end(),
                  [](auto n) { return !n; }))
    return invalid_schema();
  const auto dtype = static_cast<std::uint32_t>(spec.descriptor.element_type);
  if (dtype < 1 || dtype > 7)
    return invalid_schema();
  if (spec.layout.spatial) {
    return PlanarImage::validate_layout(spec.descriptor,
                                        planar_layout(spec.layout), false);
  }
  if (!spec.layout.groups.empty())
    return invalid_schema();
  return Status::success();
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
  auto valid = tensor_topology(*this);
  if (!valid.ok())
    return Result<std::uint64_t>(valid);
  return Region::whole(sample_shape()).element_count();
}
Result<Footprint> ResultTensorSpec::close_samples(
    const Footprint& samples, const FootprintLimits& limits) const {
  auto valid = tensor_topology(*this);
  if (!valid.ok())
    return Result<Footprint>(valid);
  if (!samples.valid() || samples.shape() != sample_shape())
    return Result<Footprint>(invalid_schema());
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
  if (!key_valid(id) || !version || (fields.empty() && tensors.empty()) ||
      fields.size() + tensors.size() > 16 || domain.size() > 8 ||
      (publication != PublishPolicy::CompleteBundle &&
       publication != PublishPolicy::IndependentChunks &&
       publication != PublishPolicy::StablePrefix) ||
      metadata.size() > 64)
    return invalid_schema();
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
    if (!key_valid(field.key) || !add_key(field.key) || type < 1 || type > 7 ||
        field.record_shape.size() > 7 ||
        !extent_valid(field.rows, resolved, i) ||
        !row_bytes(static_cast<std::uint32_t>(i)).ok())
      return invalid_schema();
  }
  for (const auto& image : tensors) {
    if (!key_valid(image.key) || !add_key(image.key) ||
        !tensor_topology(image).ok())
      return invalid_schema();
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
      return invalid_schema();
    for (const auto& facet : image.facets) {
      if (facet.key == "photospider.image" &&
          (!image.layout.spatial || image.layout.height_axis != 0 ||
           image.layout.width_axis != 1))
        return invalid_schema();
      if (facet.key == "photospider.tensor-description") {
        auto tensor = decode_tensor_description(facet);
        if (!tensor.ok())
          return tensor.status();
        if (image.layout.spatial && tensor.value().channel_axis &&
            tensor.value().channel_axis != image.layout.channel_axis)
          return invalid_schema();
      }
    }
    auto validated = input_internal::validate_port_metadata(
        OperationPortConstraint{}, image.descriptor, image.facets);
    if (!validated.ok())
      return validated;
  }
  for (const auto& extent : domain)
    if (!extent_valid(extent, resolved, fields.size()) ||
        extent.kind == ResultExtentKind::RuntimeCount)
      return invalid_schema();
  key_count = 0;
  std::uint64_t payload = 0;
  for (const auto& facet : metadata) {
    if (!key_valid(facet.key, 256) || !facet.version || !add_key(facet.key) ||
        facet.payload.size() > 65536 ||
        payload > 1048576 - facet.payload.size())
      return invalid_schema();
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
    return Result<std::uint64_t>(invalid_schema());
  const auto type = static_cast<std::uint32_t>(fields[field].element_type);
  if (type < 1 || type > 7)
    return Result<std::uint64_t>(invalid_schema());
  std::uint64_t bytes = Value::element_size(fields[field].element_type);
  for (const auto extent : fields[field].record_shape) {
    if (!extent || bytes > static_cast<std::uint64_t>(INT64_MAX) / extent)
      return Result<std::uint64_t>(invalid_schema());
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
      return invalid_schema();
    const auto& shape = input_domains[extent->input];
    std::uint64_t count = 1;
    if (extent->kind == ResultExtentKind::InputAxis) {
      if (extent->axis >= shape.size())
        return invalid_schema();
      count = shape[extent->axis];
    } else {
      if (shape.empty())
        return invalid_schema();
      for (auto n : shape) {
        if (n && count > UINT64_MAX / n)
          return Status{ErrorCode::ResourceExhausted,
                        "schema input domain overflow"};
        count *= n;
      }
    }
    auto resolved_extent = scaled(*extent, count);
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
struct ResultRef::Impl {
  struct ImageView {
    Region region;
    PlanarImage backing;
    ResourceVector<ResultRef> owners;
  };
  struct SpatialBacking {
    ResourceVector<std::uint64_t> batch;
    PlanarImage image;
  };
  struct Image {
    PlanarImageConfig config;
    ResourceVector<ImageView> views;
    ResourceVector<SpatialBacking> backing;
    ResourceVector<Value> affine;
    ResourceVector<ResultRef> affine_owners;
    ResourceVector<ResourceLease> affine_metadata;
    ResourceVector<std::shared_ptr<void>> affine_growth;
    Footprint coverage;
    ResultRelation relation;
  };
  struct Field {
    TemporaryStorage storage;
    ResultRelation relation;
    std::uint64_t written = 0, certified = 0, row_bytes = 0;
  };
  explicit Impl(ResourceBudget value) : budget(std::move(value)) {}
  ResourceBudget budget;
  ResourceLease lease, cache_metadata;
  mutable std::timed_mutex mutex;
  SchemaTemplate schema;
  ResourceString key;
  ResourceVector<std::uint64_t> association;
  std::array<Field, 16> fields;
  std::array<Image, 16> tensors;
  ResourceBindings resources;
  std::shared_ptr<PlanarPageBudget> image_budget;
  ResultRelation descriptor_relation;
  std::shared_ptr<const execution_internal::DependencyBundle> dependencies;
  ResultGrowthLimits limits;
  std::uint64_t object = 0, revision = 1, bytes = 0;
  core_internal::StoredFailure failure;
  std::atomic<bool> cancelled_publication{false};
  std::atomic<bool> kernel_active{false};
  Status reject_active_mutation() {
    if (!kernel_active.load())
      return Status::success();
    if (status().ok()) {
      auto rejected = Status{ErrorCode::InvalidArgument,
                             "producer mutation during active tensor write"};
      rejected.detail = {FailureOrigin::Protocol, FailureScope::Group};
      failure.record(rejected);
    }
    return status();
  }
  Status status() const {
    if (!failure.ok())
      return failure.status();
    return cancelled_publication.load() ? Status{ErrorCode::Cancelled, {}}
                                        : Status::success();
  }
  bool complete = false, owners_bound = false;
};
struct ResultRef::Capture {
  ResourceLease lease;
  ResultDescriptor descriptor;
  std::array<ResultRelation, 16> fields, tensors;
  ResultRelation basis;
  std::shared_ptr<const execution_internal::DependencyBundle> dependencies;
};
Result<ResultRef> ResultRef::capture() const {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  if (captured_)
    return Result<ResultRef>(*this);
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->complete &&
      impl_->schema.publication == PublishPolicy::CompleteBundle)
    return Result<ResultRef>(unavailable());
  auto admitted = impl_->budget.reserve(
      ResourceCapacity::host(sizeof(Capture), sizeof(Capture)));
  if (!admitted.ok())
    return Result<ResultRef>(admitted.status());
  try {
    auto capture = std::shared_ptr<Capture>(new Capture());
    capture->lease = admitted.take_value();
    auto& facts = capture->descriptor;
    facts.object_ = impl_->object;
    facts.revision_ = impl_->revision;
    facts.sealed_ = impl_->complete;
    facts.field_count_ = impl_->schema.fields.size();
    facts.tensor_count_ = impl_->schema.tensors.size();
    for (uint32_t i = 0; i < facts.field_count_; ++i) {
      facts.rows_[i] = impl_->fields[i].certified;
      capture->fields[i] = impl_->fields[i].relation;
    }
    for (uint32_t i = 0; i < facts.tensor_count_; ++i) {
      facts.tensors_[i] = impl_->tensors[i].coverage;
      capture->tensors[i] = impl_->tensors[i].relation;
    }
    capture->basis = impl_->descriptor_relation;
    capture->dependencies = impl_->dependencies;
    ResultRef result = *this;
    result.captured_ = std::move(capture);
    return Result<ResultRef>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultRef>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResultTensorReadWindow> execution_internal::ResultWindowAccess::compose(
    const ResourceBudget& budget, const Region& region,
    ResourceVector<ResultTensorReadWindow> windows,
    const CancellationToken& cancellation) try {
  using Answer = Result<ResultTensorReadWindow>;
  ResourceAllocationScope scope(budget);
  if (windows.empty() || windows.size() > 65536 || region.empty())
    return Answer(invalid_schema());
  auto& first = windows.front();
  if (!first.valid() || !first.owner_.owned_by(budget))
    return Answer(invalid_schema());
  const auto shape = first.spec().sample_shape();
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget](auto n) { return budget.consume({n}); };
  auto requested = Footprint::from_regions(shape, {region}, limits);
  auto covered = Footprint::none(shape, limits);
  if (!requested.ok() || !covered.ok())
    return Answer(requested.ok() ? covered.status() : requested.status());
  std::size_t affine = 0, planar = 0, owners = 0, leases = 0;
  for (const auto& window : windows) {
    if (!window.valid() || !window.owner_.owned_by(budget) ||
        window.slot_ != first.slot_)
      return Answer(invalid_schema());
    auto charged = budget.consume({1 + window.owner_.schema().canonical_size() +
                                   first.owner_.schema().canonical_size() +
                                   region.rank()});
    if (!charged.ok())
      return Answer(charged);
    if (!window.owner_.schema().same_schema(first.owner_.schema()))
      return Answer(invalid_schema());
    auto part = Footprint::from_regions(shape, {window.region_}, limits);
    if (!part.ok())
      return Answer(part.status());
    auto outside = part.value().subtract(requested.value(), limits);
    auto overlap = part.value().intersect(covered.value(), limits);
    if (!outside.ok() || !overlap.ok())
      return Answer(outside.ok() ? overlap.status() : outside.status());
    if (!outside.value().empty() || !overlap.value().empty())
      return Answer(invalid_schema());
    auto joined = covered.value().unite(part.value(), limits);
    if (!joined.ok())
      return Answer(joined.status());
    covered = std::move(joined);
    affine += window.affine_.size();
    planar += window.pieces_.size();
    owners += window.owners_.empty() ? 1 : window.owners_.size();
    leases += 1 + window.source_leases_.size();
  }
  if (covered.value() != requested.value())
    return Answer(unavailable());
  auto lease = budget.reserve(ResourceCapacity::host(
      sizeof(ResultTensorReadWindow) + region.rank() * sizeof(RegionDimension),
      sizeof(ResultTensorReadWindow) +
          region.rank() * sizeof(RegionDimension)));
  if (!lease.ok())
    return Answer(lease.status());
  ResultTensorReadWindow result;
  result.lease_ = lease.take_value();
  result.owner_ = first.owner_;
  result.slot_ = first.slot_;
  result.region_ = region;
  result.cancellation_ = cancellation;
  result.owners_ =
      ResourceVector<ResultRef>{ResourceAllocator<ResultRef>(budget)};
  result.source_leases_ =
      ResourceVector<ResourceLease>{ResourceAllocator<ResourceLease>(budget)};
  result.affine_ = ResourceVector<Value>{ResourceAllocator<Value>(budget)};
  result.pieces_ = ResourceVector<PlanarImageReadWindow>{
      ResourceAllocator<PlanarImageReadWindow>(budget)};
  result.piece_batches_ = ResourceVector<std::array<std::uint64_t, 8>>{
      ResourceAllocator<std::array<std::uint64_t, 8>>(budget)};
  result.piece_order_ =
      ResourceVector<std::size_t>{ResourceAllocator<std::size_t>(budget)};
  result.owners_.reserve(owners);
  result.source_leases_.reserve(leases);
  result.affine_.reserve(affine);
  result.pieces_.reserve(planar);
  result.piece_batches_.reserve(planar);
  result.piece_order_.reserve(planar);
  for (auto& window : windows) {
    if (window.owners_.empty())
      result.owners_.push_back(std::move(window.owner_));
    else
      for (auto& owner : window.owners_)
        result.owners_.push_back(std::move(owner));
    result.source_leases_.push_back(std::move(window.lease_));
    for (auto& prior : window.source_leases_)
      result.source_leases_.push_back(std::move(prior));
    for (auto& value : window.affine_)
      result.affine_.push_back(std::move(value));
    for (std::size_t i = 0; i < window.pieces_.size(); ++i) {
      result.pieces_.push_back(std::move(window.pieces_[i]));
      result.piece_batches_.push_back(window.piece_batches_[i]);
      result.piece_order_.push_back(result.piece_order_.size());
    }
  }
  std::uint64_t rounds = 1;
  for (auto n = planar; n > 1; n >>= 1)
    ++rounds;
  if (planar > UINT64_MAX / (rounds * (1 + result.spec().batch_axes.size())))
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  auto charged =
      budget.consume({planar * rounds * (1 + result.spec().batch_axes.size())});
  if (!charged.ok())
    return Answer(charged);
  std::sort(
      result.piece_order_.begin(), result.piece_order_.end(),
      [&](std::size_t a, std::size_t b) {
        for (std::size_t axis = 0; axis < result.spec().batch_axes.size();
             ++axis)
          if (result.piece_batches_[a][axis] != result.piece_batches_[b][axis])
            return result.piece_batches_[a][axis] <
                   result.piece_batches_[b][axis];
        return a < b;
      });
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultTensorReadWindow>(
      Status{ErrorCode::ResourceExhausted, {}});
} catch (...) {
  return Result<ResultTensorReadWindow>(Status{ErrorCode::OperationFailed, {}});
}
Result<std::uint64_t> execution_internal::ResultWindowAccess::read_work(
    const ResultTensorReadWindow& window) {
  if (!window.valid())
    return Result<std::uint64_t>(Status{ErrorCode::Stale, {}});
  const auto rank = window.region_.rank();
  const auto pieces =
      static_cast<std::uint64_t>(window.affine_.size()) + window.pieces_.size();
  const auto fixed = 4 * rank + 2;
  if (pieces == UINT64_MAX || pieces + 1 > (UINT64_MAX - fixed) / (rank + 1))
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  return Result<std::uint64_t>((pieces + 1) * (rank + 1) + fixed);
}
Result<bool> execution_internal::ResultWindowAccess::visit_backing_regions(
    const ResultTensorReadWindow& window, const ResourceBudget& budget,
    const std::function<Status(const Region&)>& visitor) {
  using Answer = Result<bool>;
  if (!window.valid())
    return Answer(Status{ErrorCode::Stale, {}});
  if (window.affine_.size() + window.pieces_.size() <= 1)
    return Answer(false);
  auto scratch = budget.reserve(ResourceCapacity::host(512, 512));
  if (!scratch.ok())
    return Answer(scratch.status());
  const auto visit = [&](const Region& region) {
    if (window.cancellation_.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    auto charged = budget.consume({region.rank() + 1});
    return charged.ok() ? visitor(region) : charged;
  };
  for (const auto& part : window.affine_) {
    auto status = visit(part.region());
    if (!status.ok())
      return Answer(status);
  }
  for (std::size_t i = 0; i < window.pieces_.size(); ++i) {
    std::vector<RegionDimension> dims;
    for (std::size_t axis = 0; axis < window.spec().batch_axes.size(); ++axis)
      dims.push_back({window.piece_batches_[i][axis], 1});
    const auto& cell = window.pieces_[i].region().dimensions();
    dims.insert(dims.end(), cell.begin(), cell.end());
    auto status = visit(Region(std::move(dims)));
    if (!status.ok())
      return Answer(status);
  }
  return Answer(true);
}
Result<ResultTensorReadWindow>
execution_internal::ResultWindowAccess::replace_backing(
    ResultTensorReadWindow window, Value backing, ResourceLease metadata) {
  using Answer = Result<ResultTensorReadWindow>;
  if (!window.valid() || !backing.valid() ||
      backing.descriptor().element_type !=
          window.spec().descriptor.element_type ||
      backing.descriptor().shape.size() !=
          window.spec().batch_axes.size() +
              window.spec().descriptor.shape.size() ||
      backing.region().rank() != window.region().rank())
    return Answer(
        Status{ErrorCode::InvalidArgument, "native tensor backing mismatch"});
  for (std::size_t i = 0; i < backing.descriptor().shape.size(); ++i) {
    const auto expected =
        i < window.spec().batch_axes.size()
            ? window.spec().batch_axes[i]
            : window.spec()
                  .descriptor.shape[i - window.spec().batch_axes.size()];
    if (backing.descriptor().shape[i] != expected)
      return Answer(
          Status{ErrorCode::InvalidArgument, "native tensor shape mismatch"});
  }
  for (std::size_t i = 0; i < window.region().rank(); ++i)
    if (backing.region().dimensions()[i].offset !=
            window.region().dimensions()[i].offset ||
        backing.region().dimensions()[i].extent !=
            window.region().dimensions()[i].extent)
      return Answer(
          Status{ErrorCode::InvalidArgument, "native tensor region mismatch"});
  window.affine_.clear();
  window.affine_.push_back(std::move(backing));
  if (metadata.valid())
    window.source_leases_.push_back(std::move(metadata));
  window.pieces_.clear();
  window.piece_batches_.clear();
  window.piece_order_.clear();
  return Answer(std::move(window));
}
Result<Value> execution_internal::ResultWindowAccess::affine(
    const ResultTensorReadWindow& window) {
  if (!window.valid())
    return Result<Value>(Status{ErrorCode::Stale, {}});
  if (window.cancellation_.cancelled())
    return Result<Value>(Status{ErrorCode::Cancelled, {}});
  if (window.affine_.size() != 1 || !window.pieces_.empty())
    return Result<Value>(Status{ErrorCode::NotFound, {}});
  return window.affine_[0].view(window.region_);
}
Result<std::optional<Value>> execution_internal::ResultWindowAccess::input_view(
    const ResultTensorReadWindow& window, const FootprintLimits& limits) {
  using Answer = Result<std::optional<Value>>;
  if (!window.valid())
    return Answer(Status{ErrorCode::Stale, {}});
  if (window.cancellation_.cancelled() || limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (!window.pieces_.empty() || window.affine_.empty())
    return Answer(std::optional<Value>{});
  return input_internal::join_affine_view(
      {window.spec().descriptor.element_type, window.spec().sample_shape()},
      window.region_, window.affine_, limits);
}
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
      if (size > UINT64_MAX - 4095)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      auto status = add(storage.impl_.get(), ((size + 4095) / 4096) * 4096);
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
struct ResultReadPlan::Impl {
  ResourceLease lease;
  std::shared_ptr<ResultRef::Impl> result;
  TemporaryStorage storage;
  std::uint64_t offset = 0, bytes = 0;
};
struct ResultWritePlan::Impl {
  ResourceLease lease;
  std::shared_ptr<ResultRef::Impl> result;
  std::shared_ptr<const CpuStorage> payload;
  std::uint32_t field = 0;
  std::uint64_t rows = 0;
  std::atomic<bool> applied{false};
};
bool ResultWritePlan::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->result->budget.same_owner(budget);
}
bool ResultReadPlan::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->result->budget.same_owner(budget);
}
bool ResultRef::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->budget.same_owner(budget);
}
std::uint64_t ResultWritePlan::byte_size() const noexcept {
  return impl_ && impl_->payload ? impl_->payload->capacity() : 0;
}
Status ResultWritePlan::apply(const CancellationToken& cancellation) const {
  if (!impl_ || impl_->applied.exchange(true))
    return Status{ErrorCode::Stale, "result write command already applied"};
  return ResultBuilder::append_to(impl_->result, impl_->field, impl_->rows,
                                  impl_->payload->bytes(), cancellation);
}
Result<ResultWritePlan> ResultBuilder::prepare_append(
    std::uint32_t field, std::uint64_t rows,
    std::shared_ptr<const CpuStorage> payload) const {
  if (!impl_ || !payload)
    return Result<ResultWritePlan>(Status{ErrorCode::Stale, {}});
  auto retained = impl_->budget.reference(std::move(payload));
  if (!retained.ok())
    return Result<ResultWritePlan>(retained.status());
  auto capacity = ResourceCapacity::host(sizeof(ResultWritePlan::Impl),
                                         sizeof(ResultWritePlan::Impl));
  capacity[ResourceKind::Queue] = 1;
  auto lease = impl_->budget.reserve(capacity);
  if (!lease.ok())
    return Result<ResultWritePlan>(lease.status());
  try {
    auto plan = std::make_shared<ResultWritePlan::Impl>();
    plan->lease = lease.take_value();
    plan->result = impl_;
    plan->payload = retained.take_value();
    plan->field = field;
    plan->rows = rows;
    ResultWritePlan result;
    result.impl_ = std::move(plan);
    return Result<ResultWritePlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultWritePlan>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
WeakResultRef ResultRef::weak() const noexcept {
  WeakResultRef weak;
  weak.impl_ = impl_;
  weak.captured_ = captured_;
  weak.captured_view_ = captured_ != nullptr;
  weak.request_record_ = request_record_;
  return weak;
}
ResultRef WeakResultRef::lock() const noexcept {
  ResultRef result;
  result.impl_ = impl_.lock();
  result.request_record_ = request_record_;
  if (captured_view_) {
    result.captured_ = captured_.lock();
    if (!result.captured_)
      result.impl_.reset();
  }
  return result;
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
    return invalid_schema();
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
        return invalid_schema();
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
            return invalid_schema();
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

void ResultRef::bind_dependencies(
    std::shared_ptr<const execution_internal::DependencyBundle> bundle) const {
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  impl_->dependencies = std::move(bundle);
}
std::shared_ptr<const execution_internal::DependencyBundle>
ResultRef::dependencies() const {
  if (captured_)
    return captured_->dependencies;
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  return impl_->dependencies;
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
             ? unavailable()
             : impl_->status();
}
Result<ResultDescriptor> ResultRef::descriptor(bool require_complete) const {
  if (captured_)
    return require_complete && !captured_->descriptor.sealed()
               ? Result<ResultDescriptor>(unavailable())
               : Result<ResultDescriptor>(captured_->descriptor);
  if (!impl_)
    return Result<ResultDescriptor>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->complete &&
      (require_complete ||
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultDescriptor>(impl_->status().ok() ? unavailable()
                                                         : impl_->status());
  ResultDescriptor facts;
  facts.object_ = impl_->object;
  facts.revision_ = impl_->revision;
  facts.sealed_ = impl_->complete;
  facts.field_count_ = static_cast<std::uint32_t>(impl_->schema.fields.size());
  for (std::uint32_t i = 0; i < facts.field_count_; ++i)
    facts.rows_[i] = impl_->fields[i].certified;
  facts.tensor_count_ =
      static_cast<std::uint32_t>(impl_->schema.tensors.size());
  for (std::uint32_t i = 0; i < facts.tensor_count_; ++i)
    facts.tensors_[i] = impl_->tensors[i].coverage;
  return Result<ResultDescriptor>(facts);
}
namespace {
bool inside(const Region& region, const std::vector<std::uint64_t>& at) {
  if (region.rank() != at.size())
    return false;
  for (std::size_t i = 0; i < at.size(); ++i) {
    const auto d = region.dimensions()[i];
    if (at[i] < d.offset || at[i] - d.offset >= d.extent)
      return false;
  }
  return true;
}
}  // namespace
Result<std::size_t> ResultTensorReadWindow::find_piece(
    const std::vector<std::uint64_t>& at) const {
  using Answer = Result<std::size_t>;
  const auto compare = [&](std::size_t index) -> Result<int> {
    if (cancellation_.cancelled())
      return Result<int>(Status{ErrorCode::Cancelled, {}});
    for (std::size_t axis = 0; axis < spec().batch_axes.size(); ++axis) {
      auto charged = owner_.impl_->budget.consume({1});
      if (!charged.ok())
        return Result<int>(charged);
      if (piece_batches_[index][axis] != at[axis])
        return Result<int>(piece_batches_[index][axis] < at[axis] ? -1 : 1);
    }
    return Result<int>(0);
  };
  std::size_t low = 0, high = piece_order_.size();
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    auto order = compare(piece_order_[middle]);
    if (!order.ok())
      return Answer(order.status());
    if (order.value() < 0)
      low = middle + 1;
    else
      high = middle;
  }
  for (; low < piece_order_.size(); ++low) {
    const auto index = piece_order_[low];
    auto order = compare(index);
    if (!order.ok())
      return Answer(order.status());
    if (order.value() != 0)
      break;
    auto charged =
        owner_.impl_->budget.consume({pieces_[index].region().rank() + 1});
    if (!charged.ok())
      return Answer(charged);
    bool covered = true;
    for (std::size_t axis = 0; axis < pieces_[index].region().rank(); ++axis) {
      const auto d = pieces_[index].region().dimensions()[axis];
      const auto coordinate = at[spec().batch_axes.size() + axis];
      covered =
          covered && coordinate >= d.offset && coordinate - d.offset < d.extent;
    }
    if (covered)
      return Answer(index);
  }
  return Answer(unavailable());
}
Result<ResultTensorRun> ResultTensorReadWindow::row_run(
    const std::vector<std::uint64_t>& at) const {
  if (cancellation_.cancelled())
    return Result<ResultTensorRun>(Status{ErrorCode::Cancelled, {}});
  if (!valid() || !inside(region_, at))
    return Result<ResultTensorRun>(Status{
        ErrorCode::InvalidArgument, "coordinate outside Result tensor window"});
  if (owners_.size() > 1) {
    auto charged = owner_.impl_->budget.consume(
        {(affine_.size() + 1) * (region_.rank() + 1)});
    if (!charged.ok())
      return Result<ResultTensorRun>(charged);
  }
  for (const auto& value : affine_) {
    if (!inside(value.region(), at))
      continue;
    auto address = value.byte_address(at);
    if (!address.ok())
      return Result<ResultTensorRun>(address.status());
    const auto axis = sample_axis();
    const auto d = value.region().dimensions()[axis];
    const auto samples = d.offset + d.extent - at[axis];
    const auto stride = value.layout().byte_strides[axis];
    const auto magnitude = stride < 0
                               ? static_cast<std::uint64_t>(-(stride + 1)) + 1
                               : static_cast<std::uint64_t>(stride);
    const auto width = Value::element_size(spec().descriptor.element_type);
    // The validated Value span guarantees this product, including broadcasts.
    const auto bytes = (samples - 1) * magnitude + width;
    auto observed =
        data_internal::ResultHostAccessScope::observe(value.storage());
    if (!observed.ok())
      return Result<ResultTensorRun>(observed);
    return Result<ResultTensorRun>(ResultTensorRun{
        value.bytes().data() + address.value(), samples, bytes, stride});
  }
  std::vector<std::uint64_t> spatial(at.begin() + spec().batch_axes.size(),
                                     at.end());
  auto selected = find_piece(at);
  if (!selected.ok())
    return Result<ResultTensorRun>(selected.status());
  {
    const auto& piece = pieces_[selected.value()];
    auto run = piece.row_run(spatial);
    if (!run.ok())
      return Result<ResultTensorRun>(run.status());
    return Result<ResultTensorRun>(ResultTensorRun{
        run.value().data, run.value().samples, run.value().bytes,
        static_cast<std::int64_t>(
            Value::element_size(spec().descriptor.element_type))});
  }
  return Result<ResultTensorRun>(unavailable());
}
Result<ResultTensorRectangle> ResultTensorReadWindow::rectangle_run(
    const std::vector<std::uint64_t>& at) const {
  auto row = row_run(at);
  if (!row.ok())
    return Result<ResultTensorRectangle>(row.status());
  if (owners_.size() > 1) {
    auto charged = owner_.impl_->budget.consume(
        {(affine_.size() + 1) * (region_.rank() + 1)});
    if (!charged.ok())
      return Result<ResultTensorRectangle>(charged);
  }
  for (const auto& value : affine_) {
    if (!inside(value.region(), at))
      continue;
    if (!row_axis())
      return Result<ResultTensorRectangle>(
          ResultTensorRectangle{row.take_value(), 1, 0});
    const auto axis = *row_axis();
    const auto d = value.region().dimensions()[axis];
    return Result<ResultTensorRectangle>(
        ResultTensorRectangle{row.take_value(), d.offset + d.extent - at[axis],
                              value.layout().byte_strides[axis]});
  }
  std::vector<std::uint64_t> spatial(at.begin() + spec().batch_axes.size(),
                                     at.end());
  auto selected = find_piece(at);
  if (!selected.ok())
    return Result<ResultTensorRectangle>(selected.status());
  {
    const auto& piece = pieces_[selected.value()];
    auto run = piece.rectangle_run(spatial);
    if (!run.ok())
      return Result<ResultTensorRectangle>(run.status());
    return Result<ResultTensorRectangle>(ResultTensorRectangle{
        row.take_value(), run.value().rows,
        static_cast<std::int64_t>(run.value().row_stride_bytes)});
  }
  return Result<ResultTensorRectangle>(unavailable());
}
std::uint32_t ResultTensorWriteWindow::sample_axis() const {
  return spec().layout.spatial
             ? spec().batch_axes.size() + spec().layout.width_axis
             : region_.rank() - 1;
}
Result<ResultTensorMutableRun> ResultTensorWriteWindow::row_run(
    const std::vector<std::uint64_t>& at) const {
  using Answer = Result<ResultTensorMutableRun>;
  if (!valid() || !inside(region_, at))
    return Answer({ErrorCode::InvalidArgument,
                   "coordinate outside Result tensor writer"});
  const auto width = Value::element_size(spec().descriptor.element_type);
  if (planar_) {
    auto run = planar_->row_run(std::vector<std::uint64_t>(
        at.begin() + spec().batch_axes.size(), at.end()));
    return run.ok() ? Answer(ResultTensorMutableRun{
                          run.value().data, run.value().samples,
                          run.value().bytes, static_cast<std::int64_t>(width)})
                    : Answer(run.status());
  }
  std::uint64_t offset = 0;
  for (std::size_t axis = 0; axis < at.size(); ++axis)
    offset +=
        (at[axis] - region_.dimensions()[axis].offset) * affine_strides_[axis];
  const auto axis = sample_axis();
  const auto d = region_.dimensions()[axis];
  const auto count = d.offset + d.extent - at[axis];
  return Answer(ResultTensorMutableRun{
      affine_data_ + offset, count, (count - 1) * affine_strides_[axis] + width,
      static_cast<std::int64_t>(affine_strides_[axis])});
}
Result<ResultTensorMutableRectangle> ResultTensorWriteWindow::rectangle_run(
    const std::vector<std::uint64_t>& at) const {
  using Answer = Result<ResultTensorMutableRectangle>;
  auto row = row_run(at);
  if (!row.ok())
    return Answer(row.status());
  if (planar_) {
    auto rectangle = planar_->rectangle_run(std::vector<std::uint64_t>(
        at.begin() + spec().batch_axes.size(), at.end()));
    return rectangle.ok() ? Answer(ResultTensorMutableRectangle{
                                row.take_value(), rectangle.value().rows,
                                static_cast<std::int64_t>(
                                    rectangle.value().row_stride_bytes)})
                          : Answer(rectangle.status());
  }
  if (at.size() == 1)
    return Answer(ResultTensorMutableRectangle{row.take_value(), 1, 0});
  const auto axis = spec().layout.spatial
                        ? spec().batch_axes.size() + spec().layout.height_axis
                        : at.size() - 2;
  const auto d = region_.dimensions()[axis];
  return Answer(ResultTensorMutableRectangle{
      row.take_value(), d.offset + d.extent - at[axis],
      static_cast<std::int64_t>(affine_strides_[axis])});
}
const void* ResultTensorReadWindow::storage_owner_token() const noexcept {
  const void* root = nullptr;
  for (const auto& value : affine_) {
    const auto token = value.storage().get();
    if (root && root != token)
      return nullptr;
    root = token;
  }
  for (const auto& piece : pieces_) {
    const auto token = piece.image_->owner_token();
    if (root && root != token)
      return nullptr;
    root = token;
  }
  return root;
}
Result<ResultTensorReadWindow> ResultRef::acquire_tensor(
    const ResultDescriptor& facts, std::uint32_t slot, const Region& region,
    const CancellationToken& cancellation) const try {
  using Answer = Result<ResultTensorReadWindow>;
  if (!impl_ ||
      (captured_ && facts.revision() > captured_->descriptor.revision()))
    return Answer(Status{ErrorCode::Stale, {}});
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(impl_->budget))
    scope.emplace(impl_->budget);
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (facts.object_ != impl_->object || !facts.revision_ ||
      facts.revision_ > impl_->revision || slot >= facts.tensor_count_ ||
      slot >= impl_->schema.tensors.size())
    return Answer(Status{ErrorCode::Stale, "invalid image descriptor"});
  const auto& spec = impl_->schema.tensors[slot];
  if (region.empty() || !region.validate(spec.sample_shape()).ok())
    return Answer(Status{ErrorCode::InvalidArgument,
                         "image window requires one frame/layer rectangle"});
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget = impl_->budget](auto n) {
    return budget.consume({n});
  };
  auto requested =
      Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!requested.ok())
    return Answer(requested.status());
  auto missing = requested.value().subtract(facts.tensors_[slot], limits);
  if (!missing.ok())
    return Answer(missing.status());
  if (!missing.value().empty())
    return Answer(unavailable());
  auto lease = impl_->budget.reserve(ResourceCapacity::host(
      sizeof(ResultTensorReadWindow) + region.rank() * sizeof(RegionDimension),
      sizeof(ResultTensorReadWindow) +
          region.rank() * sizeof(RegionDimension)));
  if (!lease.ok())
    return Answer(lease.status());
  ResultTensorReadWindow result;
  result.lease_ = lease.take_value();
  result.owner_ = *this;
  if (!captured_) {
    auto admitted = impl_->budget.reserve(
        ResourceCapacity::host(sizeof(Capture), sizeof(Capture)));
    if (!admitted.ok())
      return Answer(admitted.status());
    auto capture = std::make_shared<Capture>();
    capture->lease = admitted.take_value();
    capture->descriptor = facts;
    for (std::size_t i = 0; i < facts.field_count(); ++i)
      capture->fields[i] = impl_->fields[i].relation;
    for (std::size_t i = 0; i < facts.tensor_count(); ++i)
      capture->tensors[i] = impl_->tensors[i].relation;
    capture->basis = impl_->descriptor_relation;
    capture->dependencies = impl_->dependencies;
    result.owner_.captured_ = std::move(capture);
  }
  result.slot_ = slot;
  result.region_ = region;
  result.pieces_ = ResourceVector<PlanarImageReadWindow>(
      ResourceAllocator<PlanarImageReadWindow>(impl_->budget));
  result.affine_ =
      ResourceVector<Value>(ResourceAllocator<Value>(impl_->budget));
  result.piece_batches_ = ResourceVector<std::array<std::uint64_t, 8>>(
      ResourceAllocator<std::array<std::uint64_t, 8>>(impl_->budget));
  result.piece_order_ = ResourceVector<std::size_t>(
      ResourceAllocator<std::size_t>(impl_->budget));
  result.cancellation_ = cancellation;
  auto remaining = requested.take_value();
  if (!impl_->tensors[slot].affine.empty()) {
    for (const auto& value : impl_->tensors[slot].affine) {
      if (remaining.empty())
        break;
      if (cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      auto inspected = impl_->budget.consume({region.rank() + 1});
      if (!inspected.ok())
        return Answer(inspected);
      bool overlaps = true;
      for (std::size_t axis = 0; axis < region.rank(); ++axis) {
        const auto a = region.dimensions()[axis];
        const auto b = value.region().dimensions()[axis];
        if (a.offset >= b.offset + b.extent ||
            b.offset >= a.offset + a.extent) {
          overlaps = false;
          break;
        }
      }
      if (!overlaps)
        continue;

      auto available = Footprint::from_regions(spec.sample_shape(),
                                               {value.region()}, limits);
      if (!available.ok())
        return Answer(available.status());
      auto covered = remaining.intersect(available.value(), limits);
      if (!covered.ok())
        return Answer(covered.status());
      for (const auto& box : covered.value().boxes()) {
        auto part = value.view(box);
        if (!part.ok())
          return Answer(part.status());
        result.affine_.push_back(part.take_value());
      }
      auto next = remaining.subtract(covered.value(), limits);
      if (!next.ok())
        return Answer(next.status());
      remaining = next.take_value();
    }
    if (remaining.empty())
      return Answer(std::move(result));
  }
  if (!spec.layout.spatial)
    return Answer(unavailable());
  ResourceVector<Impl::SpatialBacking> backings(
      ResourceAllocator<Impl::SpatialBacking>(impl_->budget));
  for (const auto& entry : impl_->tensors[slot].backing) {
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto inspected = impl_->budget.consume({spec.batch_axes.size() + 1});
    if (!inspected.ok())
      return Answer(inspected);
    bool needed = true;
    for (std::size_t axis = 0; axis < spec.batch_axes.size(); ++axis) {
      const auto d = region.dimensions()[axis];
      needed = needed && entry.batch[axis] >= d.offset &&
               entry.batch[axis] - d.offset < d.extent;
    }
    if (needed)
      backings.push_back(entry);
  }
  ResourceVector<std::pair<Region, PlanarImage>> views(
      ResourceAllocator<std::pair<Region, PlanarImage>>(impl_->budget));
  auto region_lease = impl_->budget.reserve(ResourceCapacity::host(
      impl_->tensors[slot].views.size() * spec.sample_shape().size() *
          sizeof(RegionDimension),
      impl_->tensors[slot].views.size() * spec.sample_shape().size() *
          sizeof(RegionDimension)));
  if (!region_lease.ok())
    return Answer(region_lease.status());
  for (const auto& view : impl_->tensors[slot].views) {
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto inspected = impl_->budget.consume({view.region.rank() + 1});
    if (!inspected.ok())
      return Answer(inspected);
    views.emplace_back(view.region, view.backing);
  }
  lock.unlock();
  auto scratch = impl_->budget.reserve(ResourceCapacity::host(512, 512));
  if (!scratch.ok())
    return Answer(scratch.status());
  const auto compare_batch = [&](const auto& left, const auto& right) {
    for (std::size_t axis = 0; axis < spec.batch_axes.size(); ++axis) {
      if (cancellation.cancelled())
        throw Status{ErrorCode::Cancelled, {}};
      auto charged = impl_->budget.consume({1});
      if (!charged.ok())
        throw charged;
      if (left[axis] != right[axis])
        return left[axis] < right[axis] ? -1 : 1;
    }
    return 0;
  };
  std::sort(backings.begin(), backings.end(),
            [&](const auto& a, const auto& b) {
              return compare_batch(a.batch, b.batch) < 0;
            });
  const auto append = [&](const Region& box,
                          const PlanarImage* fixed) -> Status {
    auto dimensions = box.dimensions();
    const auto batches = spec.batch_axes.size();
    for (std::size_t axis = 0; axis < batches; ++axis)
      dimensions[axis].extent = 1;
    for (;;) {
      auto charged = impl_->budget.consume({1});
      if (!charged.ok())
        return charged;
      std::array<std::uint64_t, 8> key{};
      for (std::size_t axis = 0; axis < batches; ++axis)
        key[axis] = dimensions[axis].offset;
      const PlanarImage* backing = fixed;
      if (!backing) {
        auto entry =
            std::lower_bound(backings.begin(), backings.end(), key,
                             [&](const auto& value, const auto& at) {
                               return compare_batch(value.batch, at) < 0;
                             });
        if (entry != backings.end() && compare_batch(entry->batch, key) == 0)
          backing = &entry->image;
      }
      if (!backing)
        return unavailable();
      auto window = backing->certified_window(
          Region(std::vector<RegionDimension>(dimensions.begin() + batches,
                                              dimensions.end())),
          cancellation);
      if (!window.ok())
        return window.status();
      result.pieces_.push_back(window.take_value());
      result.piece_batches_.push_back(key);
      bool advanced = false;
      for (std::size_t axis = batches; axis-- > 0;) {
        if (++dimensions[axis].offset <
            box.dimensions()[axis].offset + box.dimensions()[axis].extent) {
          advanced = true;
          break;
        }
        dimensions[axis].offset = box.dimensions()[axis].offset;
      }
      if (!advanced)
        return Status::success();
    }
  };
  for (const auto& view : views) {
    auto mapped =
        Footprint::from_regions(spec.sample_shape(), {view.first}, limits);
    if (!mapped.ok())
      return Answer(mapped.status());
    auto covered = remaining.intersect(mapped.value(), limits);
    if (!covered.ok())
      return Answer(covered.status());
    for (const auto& box : covered.value().boxes()) {
      auto status = append(box, &view.second);
      if (!status.ok())
        return Answer(status);
    }
    auto next = remaining.subtract(mapped.value(), limits);
    if (!next.ok())
      return Answer(next.status());
    remaining = next.take_value();
  }
  for (const auto& box : remaining.boxes()) {
    auto status = append(box, nullptr);
    if (!status.ok())
      return Answer(status);
  }
  result.piece_order_.reserve(result.pieces_.size());
  for (std::size_t i = 0; i < result.pieces_.size(); ++i)
    result.piece_order_.push_back(i);
  std::sort(result.piece_order_.begin(), result.piece_order_.end(),
            [&](auto left, auto right) {
              return compare_batch(result.piece_batches_[left],
                                   result.piece_batches_[right]) < 0;
            });
  return Answer(std::move(result));
} catch (const Status& status) {
  return Result<ResultTensorReadWindow>(status);
} catch (const std::bad_alloc&) {
  return Result<ResultTensorReadWindow>(
      Status{ErrorCode::ResourceExhausted,
             {},
             FailureReason::CapacityLimit,
             {FailureOrigin::Resource, FailureScope::Group}});
}
Status ResultRef::read_tensor(const ResultDescriptor& facts, std::uint32_t slot,
                              const std::vector<std::uint64_t>& at,
                              void* destination, std::size_t bytes,
                              const CancellationToken& cancellation) const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  if (!destination || slot >= schema().tensors.size() ||
      bytes !=
          Value::element_size(schema().tensors[slot].descriptor.element_type))
    return Status{ErrorCode::InvalidArgument, "invalid image read"};
  if (slot >= facts.tensor_count() || !facts.tensor_coverage(slot).contains(at))
    return Status{ErrorCode::InvalidArgument, "unauthorized image sample"};
  std::vector<RegionDimension> dimensions;
  for (auto n : at)
    dimensions.push_back({n, 1});
  auto window =
      acquire_tensor(facts, slot, Region(std::move(dimensions)), cancellation);
  if (!window.ok())
    return window.status();
  auto work = impl_->budget.consume({1, bytes, 1});
  if (!work.ok())
    return work;
  auto run = window.value().row_run(at);
  if (!run.ok())
    return run.status();
  std::memcpy(destination, run.value().data, bytes);
  return Status::success();
}
Result<ResultRelation> ResultRef::tensor_relation(std::uint32_t slot) const {
  if (captured_)
    return slot < captured_->descriptor.tensor_count()
               ? Result<ResultRelation>(captured_->tensors[slot])
               : Result<ResultRelation>(unavailable());
  if (!impl_)
    return Result<ResultRelation>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (slot >= impl_->schema.tensors.size() ||
      !impl_->tensors[slot].relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(unavailable());
  return Result<ResultRelation>(impl_->tensors[slot].relation);
}
Result<ResultReadPlan> ResultRef::prepare_read(const ResultDescriptor& facts,
                                               std::uint32_t field,
                                               std::uint64_t first,
                                               std::uint64_t rows) const {
  if (!impl_ ||
      (captured_ && facts.revision() > captured_->descriptor.revision()))
    return Result<ResultReadPlan>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (facts.object_ != impl_->object || !facts.revision_ ||
      facts.revision_ > impl_->revision ||
      facts.field_count_ != impl_->schema.fields.size() ||
      field >= facts.field_count_ || first > facts.rows_[field] ||
      rows > facts.rows_[field] - first ||
      facts.rows_[field] > impl_->fields[field].certified)
    return Result<ResultReadPlan>(
        Status{ErrorCode::Stale, "invalid result descriptor/range"});
  auto capacity = ResourceCapacity::host(sizeof(ResultReadPlan::Impl),
                                         sizeof(ResultReadPlan::Impl));
  capacity[ResourceKind::Entries] = 1;
  auto lease = impl_->budget.reserve(capacity);
  if (!lease.ok())
    return Result<ResultReadPlan>(lease.status());
  try {
    auto plan = std::make_shared<ResultReadPlan::Impl>();
    plan->lease = lease.take_value();
    plan->result = impl_;
    plan->storage = impl_->fields[field].storage;
    plan->offset = first * impl_->fields[field].row_bytes;
    plan->bytes = rows * impl_->fields[field].row_bytes;
    ResultReadPlan result;
    result.impl_ = std::move(plan);
    return Result<ResultReadPlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultReadPlan>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResultRelation> ResultRef::relation(std::uint32_t field) const {
  if (captured_)
    return field < captured_->descriptor.field_count()
               ? Result<ResultRelation>(captured_->fields[field])
               : Result<ResultRelation>(unavailable());
  if (!impl_)
    return Result<ResultRelation>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (field >= impl_->schema.fields.size() ||
      !impl_->fields[field].relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(unavailable());
  return Result<ResultRelation>(impl_->fields[field].relation);
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
    return Result<ResultRelation>(unavailable());
  return Result<ResultRelation>(impl_->descriptor_relation);
}
Status ResultBuilder::bind_descriptor_relation(ResultRelation relation) {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl_->revision != 1 || impl_->descriptor_relation.valid() ||
      !relation.owned_by(impl_->budget) || relation.coverage() != 1)
    return invalid_schema();
  impl_->descriptor_relation = std::move(relation);
  return Status::success();
}
std::uint64_t ResultReadPlan::byte_size() const noexcept {
  return impl_ ? impl_->bytes : 0;
}
Result<std::shared_ptr<const CpuStorage>> ResultReadPlan::load(
    std::uint64_t maximum_window, const CancellationToken& cancellation) const {
  if (!impl_ || !impl_->bytes)
    return Result<std::shared_ptr<const CpuStorage>>(
        Status{ErrorCode::InvalidArgument, "empty/invalid result read"});
  auto loaded = impl_->storage.read(impl_->offset, impl_->bytes, maximum_window,
                                    cancellation);
  if (!loaded.ok())
    return loaded;
  struct Owner {
    ResourceLease lease;
    std::shared_ptr<const Impl> plan;
    std::shared_ptr<const CpuStorage> storage;
  };
  auto lease = impl_->result->budget.reserve(
      ResourceCapacity::host(sizeof(Owner), sizeof(Owner)));
  if (!lease.ok())
    return Result<std::shared_ptr<const CpuStorage>>(lease.status());
  try {
    auto owner = std::shared_ptr<Owner>(
        new Owner{lease.take_value(), impl_, loaded.take_value()});
    const auto* storage = owner->storage.get();
    return Result<std::shared_ptr<const CpuStorage>>(
        std::shared_ptr<const CpuStorage>(std::move(owner), storage));
  } catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<const CpuStorage>>(
        Status{ErrorCode::ResourceExhausted, {}});
  }
}
ResultBuilder::ResultBuilder(ResultBuilder&& other) noexcept
    : impl_(std::move(other.impl_)) {}
ResultBuilder& ResultBuilder::operator=(ResultBuilder&& other) noexcept {
  if (this != &other) {
    fail(Status{ErrorCode::Cancelled, {}});
    impl_ = std::move(other.impl_);
  }
  return *this;
}
ResultBuilder::~ResultBuilder() noexcept {
  fail(Status{ErrorCode::Cancelled, {}});
}
Result<ResultBuilder> ResultBuilder::start(
    ResourceBudget budget, const SchemaTemplate& schema,
    std::string_view semantic_key, ResultGrowthLimits limits,
    std::vector<std::uint64_t> association, std::uint64_t tile_height,
    std::uint64_t tile_width, ResourceBindings resources) {
  std::optional<ResourceAllocationScope> metadata_scope;
  const auto* active_root = resource_internal::metadata_budget();
  if (!active_root || !active_root->same_owner(budget))
    metadata_scope.emplace(budget);
  auto valid = schema.validate(true);
  if (!valid.ok())
    return Result<ResultBuilder>(valid);
  if (std::any_of(schema.tensors.begin(), schema.tensors.end(),
                  [](const auto& spec) { return spec.layout.spatial; }) &&
      (!tile_height || !tile_width || (tile_height & (tile_height - 1)) ||
       (tile_width & (tile_width - 1))))
    return Result<ResultBuilder>(invalid_schema());
  auto association_work = budget.consume({association.size()});
  if (!association_work.ok())
    return Result<ResultBuilder>(association_work);
  if (semantic_key.empty() || semantic_key.size() > 4096 ||
      std::any_of(association.begin(), association.end(),
                  [](auto id) { return id == 0; }))
    return Result<ResultBuilder>(invalid_schema());
  auto admitted_resources = resources.reference(budget);
  if (!admitted_resources.ok())
    return Result<ResultBuilder>(admitted_resources.status());
  resources = admitted_resources.take_value();
  for (const auto& field : schema.fields)
    if (field.rows.kind == ResultExtentKind::Fixed &&
        scaled(field.rows, field.rows.value).value() > limits.maximum_rows)
      return Result<ResultBuilder>(
          Status{ErrorCode::ResourceExhausted,
                 "fixed result count exceeds growth limit"});
  auto copied = schema.managed_copy(budget);
  if (!copied.ok())
    return Result<ResultBuilder>(copied.status());
  auto encoded = schema.managed_canonical(budget);
  if (!encoded.ok())
    return Result<ResultBuilder>(encoded.status());
  auto extra_work = budget.consume({semantic_key.size() + 8});
  if (!extra_work.ok())
    return Result<ResultBuilder>(extra_work);
  ResourceString schema_key = encoded.take_value();
  try {
    const auto key_size = static_cast<std::uint64_t>(semantic_key.size());
    schema_key.reserve(schema_key.size() + 8 + semantic_key.size());
    for (unsigned i = 0; i < 8; ++i)
      schema_key.push_back(static_cast<char>((key_size >> (8 * i)) & 255));
    if (!semantic_key.empty())
      schema_key.append(semantic_key.data(), semantic_key.size());
  } catch (const std::bad_alloc&) {
    return Result<ResultBuilder>(Status{ErrorCode::ResourceExhausted, {}});
  }
  const auto metadata = sizeof(ResultRef::Impl);
  auto capacity = ResourceCapacity::host(metadata, metadata);
  capacity[ResourceKind::Entries] = 1;
  auto lease = budget.reserve(capacity);
  if (!lease.ok())
    return Result<ResultBuilder>(lease.status());
  try {
    auto impl = std::shared_ptr<ResultRef::Impl>(new ResultRef::Impl(budget));
    impl->lease = lease.take_value();
    impl->schema = copied.take_value();
    auto selected_resources = impl->schema.select_resources(resources);
    if (!selected_resources.ok())
      return Result<ResultBuilder>(selected_resources.status());
    auto owned_resources = selected_resources.value().reference(impl->budget);
    if (!owned_resources.ok())
      return Result<ResultBuilder>(owned_resources.status());
    impl->resources = owned_resources.take_value();
    impl->key = std::move(schema_key);
    impl->association = ResourceVector<std::uint64_t>(
        association.begin(), association.end(),
        ResourceAllocator<std::uint64_t>(impl->budget));
    impl->limits = limits;
    for (std::uint32_t i = 0; i < impl->schema.fields.size(); ++i)
      impl->fields[i].row_bytes = impl->schema.row_bytes(i).value();
    auto page_budget = std::make_shared<PlanarPageBudget>(
        limits.maximum_bytes,
        [root = impl->budget](std::uint64_t bytes,
                              bool metadata) -> Result<std::shared_ptr<void>> {
          auto capacity = ResourceCapacity::host(bytes, metadata ? bytes : 0);
          capacity[ResourceKind::Payload] = metadata ? 0 : bytes;
          auto charged = root.reserve(capacity);
          if (!charged.ok())
            return Result<std::shared_ptr<void>>(charged.status());
          return Result<std::shared_ptr<void>>(
              std::make_shared<ResourceLease>(charged.take_value()));
        });
    page_budget->payload_committed_ = [](const std::shared_ptr<void>& owner,
                                         std::uint64_t bytes) {
      auto lease = std::static_pointer_cast<ResourceLease>(owner);
      if (lease)
        resource_internal::commit_payload(*lease, bytes);
    };
    impl->image_budget = page_budget;
    for (std::size_t i = 0; i < impl->schema.tensors.size(); ++i) {
      const auto& spec = impl->schema.tensors[i];
      auto& image = impl->tensors[i];
      image.views = ResourceVector<ResultRef::Impl::ImageView>(
          ResourceAllocator<ResultRef::Impl::ImageView>(impl->budget));
      image.backing = ResourceVector<ResultRef::Impl::SpatialBacking>(
          ResourceAllocator<ResultRef::Impl::SpatialBacking>(impl->budget));
      image.affine_owners =
          ResourceVector<ResultRef>(ResourceAllocator<ResultRef>(impl->budget));
      image.affine =
          ResourceVector<Value>(ResourceAllocator<Value>(impl->budget));
      image.affine_metadata = ResourceVector<ResourceLease>(
          ResourceAllocator<ResourceLease>(impl->budget));
      image.affine_growth = ResourceVector<std::shared_ptr<void>>(
          ResourceAllocator<std::shared_ptr<void>>(impl->budget));
      auto coverage = Footprint::none(spec.sample_shape());
      if (!coverage.ok())
        return Result<ResultBuilder>(coverage.status());
      image.coverage = coverage.take_value();
      auto empty_relation = ResultRelation::cartesian(
          impl->budget,
          spec.sample_count().ok() ? spec.sample_count().value() : UINT64_MAX,
          {0, 1, 0, 0});
      if (!empty_relation.ok())
        return Result<ResultBuilder>(empty_relation.status());
      image.relation = empty_relation.take_value();
      if (!spec.layout.spatial)
        continue;
      auto& config = image.config;
      config.order = spec.layout.order;
      config.height_axis = spec.layout.height_axis;
      config.width_axis = spec.layout.width_axis;
      config.channel_axis = spec.layout.channel_axis;
      config.row_pitch_bytes = spec.layout.row_pitch_bytes;
      config.tile_height = tile_height;
      config.tile_width = tile_width;
      config.maximum_backed_bytes = limits.maximum_bytes;
      config.aggregate_budget = page_budget;
    }
    static std::atomic<std::uint64_t> next{1};
    auto id = next.load();
    do {
      if (id == UINT64_MAX)
        return Result<ResultBuilder>(Status{ErrorCode::ResourceExhausted,
                                            "result object ids exhausted"});
    } while (!next.compare_exchange_weak(id, id + 1));
    impl->object = id;
    ResultBuilder builder;
    builder.impl_ = std::move(impl);
    return Result<ResultBuilder>(std::move(builder));
  } catch (const std::bad_alloc&) {
    return Result<ResultBuilder>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
ResultRef ResultBuilder::reference() const noexcept {
  ResultRef result;
  result.impl_ = impl_;
  return result;
}
void ResultBuilder::fail(Status status) noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->complete && impl_->status().ok())
    impl_->failure.record(status);
}
Status ResultBuilder::append(std::uint32_t field, std::uint64_t rows,
                             ByteView bytes,
                             const CancellationToken& cancellation) {
  return append_to(impl_, field, rows, bytes, cancellation);
}
Status ResultBuilder::append_to(const std::shared_ptr<ResultRef::Impl>& impl,
                                std::uint32_t field, std::uint64_t rows,
                                ByteView bytes,
                                const CancellationToken& cancellation) {
  if (!impl)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl->mutex);
  if (auto busy = impl->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl->complete || !impl->status().ok())
    return impl->status().ok() ? Status{ErrorCode::Stale, {}} : impl->status();
  auto reject = [&](Status status) {
    impl->failure.record(status);
    return status;
  };
  if (cancellation.cancelled())
    return reject(Status{ErrorCode::Cancelled, {}});
  if (field >= impl->schema.fields.size())
    return reject(invalid_schema());
  auto& target = impl->fields[field];
  if (rows > impl->limits.maximum_rows - target.written ||
      rows > UINT64_MAX / target.row_bytes ||
      rows * target.row_bytes != bytes.size() ||
      bytes.size() > impl->limits.maximum_bytes - impl->bytes)
    return reject(
        Status{ErrorCode::ResourceExhausted, "result append count/byte limit"});
  if (!rows)
    return Status::success();
  if (!target.storage.valid()) {
    auto file = TemporaryStorage::create(impl->budget);
    if (!file.ok())
      return reject(file.status());
    target.storage = file.take_value();
  }
  auto extended = target.storage.append_zeroed(bytes.size(), cancellation);
  if (!extended.ok())
    return reject(extended.status());
  auto written = target.storage.write(extended.value(), bytes, cancellation);
  if (!written.ok())
    return reject(written);
  target.written += rows;
  impl->bytes += bytes.size();
  return Status::success();
}
Status ResultBuilder::publish(std::uint32_t field, std::uint64_t end,
                              ResultRelation relation,
                              ResultFinality finality) {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  if (field >= impl_->schema.fields.size() || !finality.satisfied() ||
      !relation.owned_by(impl_->budget) || !impl_->descriptor_relation.valid())
    return reject(invalid_schema());
  auto& target = impl_->fields[field];
  if (end < target.certified || end > target.written ||
      relation.coverage() < end || impl_->revision == UINT64_MAX)
    return reject(invalid_schema());
  if (target.relation.valid() && !target.relation.same_owner(relation) &&
      target.certified) {
    if (target.relation.guarantee() != relation.guarantee())
      return reject(invalid_schema());
    if (relation.guarantee() != DependencyGuarantee::Unknown) {
      const auto ordered = [](const ResultSupport& a, const ResultSupport& b) {
        return std::tie(a.input, a.roles, a.target, a.slot, a.first, a.count) <
               std::tie(b.input, b.roles, b.target, b.slot, b.first, b.count);
      };
      for (uint64_t row = 0; row < target.certified; ++row) {
        auto work = impl_->budget.consume({1});
        if (!work.ok())
          return reject(work);
        ResourceVector<ResultSupport> prior{
            ResourceAllocator<ResultSupport>(impl_->budget)},
            next{ResourceAllocator<ResultSupport>(impl_->budget)};
        auto status = target.relation.visit(row, impl_->limits.maximum_rows,
                                            [&](auto support) {
                                              prior.push_back(support);
                                              return Status::success();
                                            });
        if (!status.ok())
          return reject(status);
        status =
            relation.visit(row, impl_->limits.maximum_rows, [&](auto support) {
              next.push_back(support);
              return Status::success();
            });
        if (!status.ok())
          return reject(status);
        std::sort(prior.begin(), prior.end(), ordered);
        std::sort(next.begin(), next.end(), ordered);
        if (prior.size() != next.size() ||
            !std::equal(prior.begin(), prior.end(), next.begin(),
                        [&](const auto& a, const auto& b) {
                          return !ordered(a, b) && !ordered(b, a);
                        }))
          return reject(invalid_schema());
      }
    }
  }
  if (target.storage.valid()) {
    auto frozen = target.storage.freeze_prefix(end * target.row_bytes);
    if (!frozen.ok())
      return reject(frozen);
  }
  target.relation = std::move(relation);
  target.certified = end;
  ++impl_->revision;
  return Status::success();
}
Status ResultBuilder::publish_tensor(std::uint32_t slot, const Region& region,
                                     StridedLayout layout,
                                     std::shared_ptr<const CpuStorage> storage,
                                     ResultRelation relation,
                                     ResultFinality finality,
                                     const CancellationToken& cancellation) {
  return publish_tensor_storage(slot, region, std::move(layout),
                                std::move(storage), std::move(relation),
                                finality, cancellation, {});
}
Status ResultBuilder::publish_tensor_storage(
    std::uint32_t slot, const Region& region, StridedLayout layout,
    std::shared_ptr<const CpuStorage> storage, ResultRelation relation,
    ResultFinality finality, const CancellationToken& cancellation,
    ResourceVector<ResultRef> sources) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  ResourceAllocationScope scope(impl_->budget);
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2))) {
    if (cancellation.cancelled()) {
      impl_->cancelled_publication.store(true);
      return Status{ErrorCode::Cancelled, {}};
    }
  }
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  const auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (slot >= impl_->schema.tensors.size() || !storage ||
      !finality.satisfied() || !impl_->descriptor_relation.valid() ||
      !relation.owned_by(impl_->budget))
    return reject(invalid_schema());
  const auto& spec = impl_->schema.tensors[slot];
  auto& target = impl_->tensors[slot];
  // A source view retains its independent root without admitting its payload
  // again. Importing external storage instead references it in this root
  // ledger.
  auto retained = sources.empty()
                      ? impl_->budget.reference(std::move(storage))
                      : Result<std::shared_ptr<const CpuStorage>>(storage);
  if (!retained.ok())
    return reject(retained.status());
  if (sources.empty() &&
      retained.value()->capacity() > impl_->limits.maximum_bytes)
    return reject(
        {ErrorCode::ResourceExhausted, "tensor storage growth limit"});
  auto value =
      Value::from_storage({spec.descriptor.element_type, spec.sample_shape()},
                          region, std::move(layout), retained.take_value());
  if (!value.ok())
    return reject(value.status());
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = impl_->budget](auto n) {
    return root.consume({n});
  };
  auto samples = Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  auto count = spec.sample_count();
  if (closed.value() != samples.value() ||
      relation.coverage() != (count.ok() ? count.value() : UINT64_MAX))
    return reject(invalid_schema());
  auto overlap = samples.value().intersect(target.coverage, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(invalid_schema());
  auto coverage = target.coverage.unite(samples.value(), limits);
  if (!coverage.ok())
    return reject(coverage.status());
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto certified = relation.certify(samples.value(), limits);
    if (!certified.ok())
      return reject(certified);
  }
  auto restricted = relation.restrict_to(spec.sample_shape(), region);
  if (!restricted.ok())
    return reject(restricted.status());
  relation = restricted.take_value();
  auto combined =
      !target.coverage.empty() && !target.relation.same_owner(relation)
          ? ResultRelation::unite(impl_->budget, {target.relation, relation})
          : Result<ResultRelation>(relation);
  if (!combined.ok())
    return reject(combined.status());
  auto admission = impl_->budget.reserve(ResourceCapacity::host(
      sizeof(Value) +
          region.rank() * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t)),
      sizeof(Value) + region.rank() * (sizeof(RegionDimension) +
                                       3 * sizeof(std::uint64_t))));
  if (!admission.ok())
    return reject(admission.status());
  const auto metadata_bytes =
      sizeof(Value) +
      region.rank() * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t));
  std::uint64_t growth_bytes =
      sources.empty() ? value.value().storage()->capacity() + metadata_bytes
                      : metadata_bytes;
  for (const auto& member : impl_->tensors)
    if (std::any_of(member.affine.begin(), member.affine.end(),
                    [&](const auto& prior) {
                      return prior.storage() == value.value().storage();
                    })) {
      growth_bytes = metadata_bytes;
      break;
    }
  auto growth = impl_->image_budget->charge(growth_bytes, false, true);
  if (!growth.ok())
    return reject(growth.status());
  std::shared_ptr<void> growth_owner;
  try {
    growth_owner = std::shared_ptr<void>(
        nullptr, [budget = impl_->image_budget, growth_bytes](void*) {
          budget->release(growth_bytes);
        });
  } catch (...) {
    impl_->image_budget->release(growth_bytes);
    throw;
  }
  target.affine_owners.reserve(target.affine_owners.size() + sources.size());
  target.affine_growth.reserve(target.affine_growth.size() + 1);
  target.affine.reserve(target.affine.size() + 1);
  target.affine_metadata.reserve(target.affine_metadata.size() + 1);
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (impl_->revision == UINT64_MAX)
    return reject(invalid_schema());
  for (auto& source : sources)
    target.affine_owners.push_back(std::move(source));
  target.affine.push_back(value.take_value());
  target.affine_metadata.push_back(admission.take_value());
  target.affine_growth.push_back(std::move(growth_owner));
  target.coverage = coverage.take_value();
  target.relation = combined.take_value();
  ++impl_->revision;
  return Status::success();
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor(
    std::uint32_t slot, const Region& region, ByteView packed,
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  if (slot >= impl_->schema.tensors.size()) {
    auto status = invalid_schema();
    fail(status);
    return status;
  }
  const auto width =
      Value::element_size(impl_->schema.tensors[slot].descriptor.element_type);
  auto count = region.element_count();
  if (!count.ok() || count.value() > UINT64_MAX / width ||
      count.value() * width != packed.size() ||
      (packed.size() && !packed.data())) {
    auto status = invalid_schema();
    fail(status);
    return status;
  }
  auto copy =
      impl_->budget.consume({0, packed.size(), packed.size() ? 1U : 0U});
  if (!copy.ok()) {
    fail(copy);
    return copy;
  }
  if (!impl_->schema.tensors[slot].layout.spatial && count.value()) {
    auto allocated = impl_->budget.allocator().allocate(packed.size());
    if (!allocated.ok()) {
      fail(allocated.status());
      return allocated.status();
    }
    auto buffer = allocated.take_value();
    if (packed.size())
      std::memcpy(buffer.data(), packed.data(), packed.size());
    StridedLayout layout;
    layout.byte_strides.resize(region.rank());
    std::uint64_t stride = width;
    for (std::size_t axis = region.rank(); axis; --axis) {
      layout.origin.push_back(0);
      if (stride > INT64_MAX) {
        auto failure = invalid_schema();
        fail(failure);
        return failure;
      }
      layout.byte_strides[axis - 1] = stride;
      stride *= region.dimensions()[axis - 1].extent;
    }
    for (std::size_t axis = 0; axis < region.rank(); ++axis)
      layout.origin[axis] = region.dimensions()[axis].offset;
    return publish_tensor(slot, region, std::move(layout),
                          std::move(buffer).freeze(), std::move(relation),
                          finality, cancellation);
  }
  return publish_tensor_kernel(
      slot, region,
      [&](const auto& writers) {
        std::uint64_t next = 0;
        for (const auto& writer : writers) {
          const auto& dimensions = writer.region().dimensions();
          std::vector<std::uint64_t> at;
          for (const auto d : dimensions)
            at.push_back(d.offset);
          for (;;) {
            if (cancellation.cancelled())
              return Status{ErrorCode::Cancelled, {}};
            auto run = writer.row_run(at);
            if (!run.ok())
              return run.status();
            // Packed publication is descriptor ordered. A last-axis physical
            // width run can be copied in one operation; channel-last input is
            // scattered.
            const auto last = at.size() - 1;
            const auto n =
                writer.sample_axis() == last ? run.value().samples : 1;
            std::memcpy(run.value().data, packed.data() + next, n * width);
            next += n * width;
            at[last] += n - 1;
            std::size_t axis = at.size();
            while (axis) {
              --axis;
              if (++at[axis] <
                  dimensions[axis].offset + dimensions[axis].extent)
                break;
              at[axis] = dimensions[axis].offset;
            }
            if (!axis && at[0] == dimensions[0].offset)
              break;
          }
        }
        return Status::success();
      },
      std::move(relation), finality, cancellation);
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Region& region,
    const std::function<Status(const ResourceVector<ResultTensorWriteWindow>&)>&
        write,
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation) {
  const auto owner = impl_;
  if (!owner)
    return Status{ErrorCode::Stale, {}};
  try {
    std::optional<ResourceAllocationScope> metadata_scope;
    const auto* active_root = resource_internal::metadata_budget();
    if (!active_root || !active_root->same_owner(owner->budget))
      metadata_scope.emplace(owner->budget);
    std::unique_lock<std::timed_mutex> lock(owner->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled()) {
        owner->cancelled_publication = true;
        return Status{ErrorCode::Cancelled, {}};
      }
    if (cancellation.cancelled()) {
      owner->cancelled_publication = true;
      return Status{ErrorCode::Cancelled, {}};
    }
    const auto reject = [&](Status status) {
      owner->failure.record(status);
      return status;
    };
    if (owner->complete || !owner->status().ok())
      return owner->status().ok() ? Status{ErrorCode::Stale, {}}
                                  : owner->status();
    if (auto busy = owner->reject_active_mutation(); !busy.ok())
      return busy;
    owner->kernel_active.store(true);
    ProducerWriteGuard producer_guard{&owner->kernel_active};
    const auto revision = owner->revision;
    if (slot >= owner->schema.tensors.size() || !finality.satisfied() ||
        !owner->descriptor_relation.valid() ||
        !relation.owned_by(owner->budget))
      return reject(invalid_schema());
    const auto& spec = owner->schema.tensors[slot];
    auto& target = owner->tensors[slot];
    FootprintLimits limits;
    limits.cancellation = cancellation;
    limits.consume_work = [root = owner->budget](auto n) {
      return root.consume({n});
    };
    auto samples =
        Footprint::from_regions(spec.sample_shape(), {region}, limits);
    if (!samples.ok())
      return reject(samples.status());
    auto closed = spec.close_samples(samples.value(), limits);
    if (!closed.ok())
      return reject(closed.status());
    const auto domain_count = spec.sample_count();
    if (closed.value() != samples.value() ||
        relation.coverage() !=
            (domain_count.ok() ? domain_count.value() : UINT64_MAX))
      return reject(invalid_schema());
    auto overlap = samples.value().intersect(target.coverage, limits);
    if (!overlap.ok())
      return reject(overlap.status());
    if (!overlap.value().empty())
      return reject(invalid_schema());
    auto coverage = target.coverage.unite(samples.value(), limits);
    if (!coverage.ok())
      return reject(coverage.status());
    auto count = samples.value().element_count();
    const auto width = Value::element_size(spec.descriptor.element_type);
    if (!count.ok() || count.value() > UINT64_MAX / width || !write ||
        owner->revision == UINT64_MAX)
      return reject(invalid_schema());
    auto copied_work = owner->budget.consume({count.value()});
    if (!copied_work.ok())
      return reject(copied_work);
    if (relation.guarantee() != DependencyGuarantee::Unknown) {
      auto certified = relation.certify(samples.value(), limits);
      if (!certified.ok())
        return reject(certified);
    }
    auto restricted = relation.restrict_to(spec.sample_shape(), region);
    if (!restricted.ok())
      return reject(restricted.status());
    relation = restricted.take_value();
    auto combined =
        !target.coverage.empty() && target.relation.valid() &&
                !target.relation.same_owner(relation)
            ? ResultRelation::unite(owner->budget, {target.relation, relation})
            : Result<ResultRelation>(relation);
    if (!combined.ok())
      return reject(combined.status());
    // A sparse logical domain need not have a representable full canvas.
    // Reuse the bounded affine transaction when planar byte geometry cannot
    // represent that domain; the typed schema and global coordinates persist.
    const bool affine = !spec.layout.spatial || !domain_count.ok() ||
                        domain_count.value() > UINT64_MAX / width;
    if (affine && !samples.value().empty()) {
      auto allocated =
          owner->budget.allocator().allocate(count.value() * width);
      if (!allocated.ok())
        return reject(allocated.status());
      auto buffer = allocated.take_value();
      ResourceVector<ResultTensorWriteWindow> windows{
          ResourceAllocator<ResultTensorWriteWindow>(owner->budget)};
      ResultTensorWriteWindow window;
      window.spec_ = &spec;
      window.region_ = region;
      window.affine_data_ = buffer.data();
      window.affine_strides_.resize(region.rank());
      std::uint64_t stride = width;
      StridedLayout layout;
      layout.byte_strides.resize(region.rank());
      layout.origin.resize(region.rank());
      for (std::size_t axis = region.rank(); axis; --axis) {
        if (stride > INT64_MAX)
          return reject(invalid_schema());
        window.affine_strides_[axis - 1] = stride;
        layout.byte_strides[axis - 1] = stride;
        layout.origin[axis - 1] = region.dimensions()[axis - 1].offset;
        stride *= region.dimensions()[axis - 1].extent;
      }
      windows.push_back(std::move(window));
      lock.unlock();
      auto written = write(windows);
      lock.lock();
      if (!owner->status().ok())
        return owner->status();
      if (impl_ != owner || owner->complete || owner->revision != revision)
        return reject(
            {ErrorCode::Stale, "tensor producer changed during callback"});
      if (!written.ok())
        return reject(written);
      producer_guard.release();
      lock.unlock();
      return publish_tensor(slot, region, std::move(layout),
                            std::move(buffer).freeze(), std::move(relation),
                            finality, cancellation);
    }
    if (!samples.value().empty()) {
      const auto batches = spec.batch_axes.size();
      Region plane_region(std::vector<RegionDimension>(
          region.dimensions().begin() + batches, region.dimensions().end()));
      ResourceVector<PlanarImageWriteWindow> writers{
          ResourceAllocator<PlanarImageWriteWindow>(owner->budget)};
      ResourceVector<ResultRef::Impl::SpatialBacking> candidates{
          ResourceAllocator<ResultRef::Impl::SpatialBacking>(owner->budget)};
      std::vector<std::uint64_t> at;
      for (std::size_t axis = 0; axis < batches; ++axis)
        at.push_back(region.dimensions()[axis].offset);
      for (;;) {
        PlanarImage backing;
        for (const auto& entry : target.backing)
          if (std::equal(entry.batch.begin(), entry.batch.end(), at.begin())) {
            backing = entry.image;
            break;
          }
        if (!backing.valid()) {
          auto created = [&]() -> Result<PlanarImage> {
            std::uint64_t bytes =
                spec.layout.groups.size() * sizeof(ImageComponentGroup);
            for (const auto& group : spec.layout.groups)
              bytes += group.role.size();
            auto lease =
                owner->budget.reserve(ResourceCapacity::host(bytes, bytes));
            if (!lease.ok())
              return Result<PlanarImage>(lease.status());
            auto config = target.config;
            config.groups = spec.layout.groups;
            return PlanarImage::create(spec.descriptor, config, spec.facets,
                                       owner->resources);
          }();
          if (!created.ok())
            return reject(created.status());
          backing = created.take_value();
          candidates.push_back(
              {ResourceVector<std::uint64_t>(
                   at.begin(), at.end(),
                   ResourceAllocator<std::uint64_t>(owner->budget)),
               backing});
        }
        auto prepared = backing.begin_write(plane_region, cancellation);
        if (!prepared.ok())
          return reject(prepared.status());
        writers.push_back(prepared.take_value());
        std::size_t axis = batches;
        while (axis) {
          --axis;
          const auto d = region.dimensions()[axis];
          if (++at[axis] < d.offset + d.extent)
            break;
          at[axis] = d.offset;
        }
        if (!axis && (at.empty() || at[0] == region.dimensions()[0].offset))
          break;
      }
      // New roots stay private until the producer transaction succeeds. Reserve
      // installation metadata before invoking user work so commit cannot
      // allocate.
      target.backing.reserve(target.backing.size() + candidates.size());
      // User work must not hold the Result state lock. Descriptor readers and
      // ownership publication on independent builders remain reentrant. Planar
      // writer locks protect the unpublished bytes until commit or rollback.
      ResourceVector<ResultTensorWriteWindow> windows{
          ResourceAllocator<ResultTensorWriteWindow>(owner->budget)};
      windows.reserve(writers.size());
      at.clear();
      for (std::size_t axis = 0; axis < batches; ++axis)
        at.push_back(region.dimensions()[axis].offset);
      for (const auto& writer : writers) {
        ResultTensorWriteWindow window;
        window.spec_ = &spec;
        auto dimensions = region.dimensions();
        for (std::size_t axis = 0; axis < batches; ++axis)
          dimensions[axis] = {at[axis], 1};
        window.region_ = Region(std::move(dimensions));
        window.planar_ = &writer;
        windows.push_back(std::move(window));
        for (std::size_t axis = batches; axis;) {
          --axis;
          const auto d = region.dimensions()[axis];
          if (++at[axis] < d.offset + d.extent)
            break;
          at[axis] = d.offset;
        }
      }
      lock.unlock();
      auto written = write(windows);
      lock.lock();
      if (!owner->status().ok())
        return owner->status();
      if (impl_ != owner || owner->complete || owner->revision != revision)
        return reject(
            {ErrorCode::Stale, "tensor producer changed during callback"});
      if (!written.ok())
        return reject(written);
      if (cancellation.cancelled())
        return reject(Status{ErrorCode::Cancelled, {}});
      // All admission/copy/cancellation checks precede this allocation-free
      // commit barrier. A failed preparation retires every fresh page/window.
      for (auto& writer : writers) {
        auto status = writer.commit();
        if (!status.ok())
          return reject(status);
      }
      for (auto& candidate : candidates)
        target.backing.push_back(std::move(candidate));
    }
    target.coverage = coverage.take_value();
    target.relation = combined.take_value();
    ++owner->revision;
    return Status::success();
  } catch (const std::bad_alloc&) {
    auto status = Status{ErrorCode::ResourceExhausted, {}};
    std::lock_guard<std::timed_mutex> lock(owner->mutex);
    if (!owner->complete && owner->status().ok())
      owner->failure.record(status);
    return owner->status().ok() ? status : owner->status();
  } catch (...) {
    auto status = Status{ErrorCode::OperationFailed, {}};
    std::lock_guard<std::timed_mutex> lock(owner->mutex);
    if (!owner->complete && owner->status().ok())
      owner->failure.record(status);
    return owner->status().ok() ? status : owner->status();
  }
}
Status ResultBuilder::publish_tensor_view(
    std::uint32_t slot, const Region& region,
    const ResultTensorReadWindow& source,
    const ResultTensorViewTransform& transform, ResultRelation relation,
    ResultFinality finality, const CancellationToken& cancellation) try {
  if (!impl_)
    return {ErrorCode::Stale, {}};
  ResourceAllocationScope scope(impl_->budget);
  Footprint prior;
  {
    std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled()) {
        impl_->cancelled_publication = true;
        return {ErrorCode::Cancelled, {}};
      }
    if (!impl_->status().ok())
      return impl_->status();
    if (impl_->complete)
      return {ErrorCode::Stale, {}};
    if (auto busy = impl_->reject_active_mutation(); !busy.ok())
      return busy;
    if (slot >= impl_->schema.tensors.size() || !finality.satisfied() ||
        !impl_->descriptor_relation.valid() ||
        !relation.owned_by(impl_->budget)) {
      impl_->failure.record(invalid_schema());
      return impl_->status();
    }
    prior = impl_->tensors[slot].coverage;
  }
  const auto unavailable = [] {
    return Status{ErrorCode::InvalidArgument,
                  "ViewUnavailable: source has no affine physical mapping"};
  };
  const auto reject = [&](Status status) {
    fail(status);
    return status;
  };
  if (!source.valid() || slot >= impl_->schema.tensors.size() ||
      region.empty() ||
      !region.validate(impl_->schema.tensors[slot].sample_shape()).ok())
    return reject(invalid_schema());
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (source.owner_.request_record_)
    return reject({ErrorCode::InvalidArgument,
                   "terminal Result cannot supply a tensor view",
                   FailureReason::None,
                   {FailureOrigin::Protocol, FailureScope::Group}});

  const auto& spec = impl_->schema.tensors[slot];
  if (spec.descriptor.element_type != source.spec().descriptor.element_type)
    return reject(invalid_schema());
  // Logical validity and exact source authorization precede physical strategy
  // selection. Invalid transforms cannot be laundered as Auto copy retries.
  if (transform.reshape) {
    if (!transform.source_axes.empty())
      return reject({ErrorCode::InvalidArgument,
                     "invalid tensor view transform",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
    std::array<std::uint64_t, 8> remaining{};
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis)
      remaining[axis] = source.region_.dimensions()[axis].extent;
    const auto cancel_factor = [&](std::uint64_t extent) {
      for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
        const auto divisor = std::gcd(remaining[axis], extent);
        remaining[axis] /= divisor;
        extent /= divisor;
      }
      return extent == 1;
    };
    bool equal = true;
    for (auto extent : spec.batch_axes)
      equal = cancel_factor(extent) && equal;
    for (auto extent : spec.descriptor.shape)
      equal = cancel_factor(extent) && equal;
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis)
      equal = remaining[axis] == 1 && equal;
    if (!equal)
      return reject({ErrorCode::InvalidArgument,
                     "reshape tensor view changes logical sample count",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
  } else {
    if (transform.source_axes.size() != source.region_.rank())
      return reject({ErrorCode::InvalidArgument,
                     "invalid tensor view transform",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
      const auto& map = transform.source_axes[axis];
      if (map.extent != 1 || map.output_axis < -1 ||
          map.output_axis >= static_cast<std::int32_t>(region.rank()))
        return reject({ErrorCode::InvalidArgument,
                       "invalid tensor view transform",
                       FailureReason::None,
                       {FailureOrigin::Protocol, FailureScope::Group}});
      const auto output = map.output_axis < 0
                              ? RegionDimension{0, 1}
                              : region.dimensions()[map.output_axis];
      auto start = map.source_coordinate(output.offset);
      auto end = map.source_coordinate(output.offset + output.extent - 1);
      const auto allowed = source.region_.dimensions()[axis];
      if (!start.ok() || !end.ok() ||
          std::min(start.value(), end.value()) < allowed.offset ||
          std::max(start.value(), end.value()) - allowed.offset >=
              allowed.extent)
        return reject({ErrorCode::InvalidArgument,
                       "tensor view exceeds source authorization",
                       FailureReason::UnauthorizedRead,
                       {FailureOrigin::Protocol, FailureScope::Group}});
    }
  }
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = impl_->budget](auto n) {
    return root.consume({n});
  };
  auto samples = Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  const auto count = spec.sample_count();
  if (closed.value() != samples.value() ||
      relation.coverage() != (count.ok() ? count.value() : UINT64_MAX) ||
      source.owner_.object_id() == impl_->object)
    return reject(invalid_schema());
  auto overlap = samples.value().intersect(prior, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(invalid_schema());
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto certified = relation.certify(samples.value(), limits);
    if (!certified.ok())
      return reject(certified);
  }
  // Metadata for temporary source layouts, rank-bounded coordinates and chunk
  // factors is admitted before constructing standard-library scratch vectors.
  auto scratch = impl_->budget.reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok()) {
    fail(scratch.status());
    return scratch.status();
  }

  if (source.affine_.empty() || !source.pieces_.empty())
    return unavailable();
  auto joined =
      input_internal::join_affine_view(source.affine_.front().descriptor(),
                                       source.region_, source.affine_, limits);
  if (!joined.ok())
    return reject(joined.status());
  if (!joined.value())
    return unavailable();
  Result<Value> common(std::move(*joined.value()));
  auto work = impl_->budget.consume(
      {source.affine_.size() * source.region_.rank() + region.rank()});
  if (!work.ok())
    return reject(work);
  StridedLayout layout;
  layout.byte_strides.resize(region.rank(), 0);
  layout.origin.resize(region.rank());
  std::vector<std::uint64_t> source_at;
  for (auto d : source.region_.dimensions())
    source_at.push_back(d.offset);
  if (transform.reshape) {
    if (!transform.source_axes.empty())
      return reject(invalid_schema());
    struct Chunk {
      std::vector<std::uint64_t> factors;
      std::int64_t stride = 0;
    };
    std::vector<Chunk> chunks;
    for (std::size_t axis = source.region_.rank(); axis;) {
      --axis;
      const auto extent = source.region_.dimensions()[axis].extent;
      if (extent == 1)
        continue;
      const auto stride = common.value().layout().byte_strides[axis];
      bool merge = !chunks.empty();
      if (merge && chunks.back().stride == 0) {
        merge = stride == 0;
      } else if (merge) {
        const auto inner = chunks.back().stride;
        const auto magnitude =
            inner < 0 ? -static_cast<__int128>(inner) : inner;
        std::uint64_t product = 1;
        for (auto factor : chunks.back().factors) {
          if (factor >
              static_cast<unsigned __int128>(INT64_MAX) / magnitude / product) {
            merge = false;
            break;
          }
          product *= factor;
        }
        merge = merge && static_cast<__int128>(product) * inner == stride;
      }
      if (merge)
        chunks.back().factors.push_back(extent);
      else
        chunks.push_back({{extent}, stride});
    }
    std::size_t axis = region.rank();
    const auto shape = spec.sample_shape();
    for (auto& chunk : chunks) {
      std::uint64_t assigned = 1;
      auto remaining = [&] {
        return std::any_of(chunk.factors.begin(), chunk.factors.end(),
                           [](auto n) { return n != 1; });
      };
      while (axis && (remaining() || shape[axis - 1] == 1)) {
        --axis;
        const auto extent = shape[axis];
        const __int128 stride =
            extent == 1 ? 0 : static_cast<__int128>(assigned) * chunk.stride;
        if (stride < INT64_MIN || stride > INT64_MAX)
          return unavailable();
        layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
        auto needed = extent;
        for (auto& factor : chunk.factors) {
          const auto divisor = std::gcd(factor, needed);
          factor /= divisor;
          needed /= divisor;
        }
        if (needed != 1)
          return unavailable();
        if (chunk.stride) {
          if (extent > UINT64_MAX / assigned)
            return unavailable();
          assigned *= extent;
        }
      }
      if (remaining())
        return unavailable();
    }
    while (axis)
      if (shape[--axis] != 1)
        return unavailable();
    auto offset = common.value().byte_address(source_at);
    if (!offset.ok())
      return reject(offset.status());
    layout.byte_offset = offset.value();
    // Chunk factor cancellation proves equal logical sizes without requiring
    // their full-domain product to fit uint64. Zero strides remain zero.
  } else {
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
      const auto& map = transform.source_axes[axis];
      const auto output = map.output_axis < 0
                              ? RegionDimension{0, 1}
                              : region.dimensions()[map.output_axis];
      auto start = map.source_coordinate(output.offset);
      source_at[axis] = start.value();
      if (map.output_axis >= 0 &&
          region.dimensions()[map.output_axis].extent > 1) {
        const __int128 stride =
            static_cast<__int128>(common.value().layout().byte_strides[axis]) *
                map.step +
            layout.byte_strides[map.output_axis];
        if (stride < INT64_MIN || stride > INT64_MAX)
          return unavailable();
        layout.byte_strides[map.output_axis] =
            static_cast<std::int64_t>(stride);
      }
    }
    auto offset = common.value().byte_address(source_at);
    if (!offset.ok())
      return reject(offset.status());
    layout.byte_offset = offset.value();
    for (std::size_t axis = 0; axis < region.rank(); ++axis)
      layout.origin[axis] = region.dimensions()[axis].offset;
  }
  std::unique_lock<std::timed_mutex> ownership(result_ownership_mutex,
                                               std::defer_lock);
  while (!ownership.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return reject({ErrorCode::Cancelled, {}});
  ResourceVector<ResultRef> pending{
      ResourceAllocator<ResultRef>(impl_->budget)};
  ResourceVector<std::uint64_t> visited{
      ResourceAllocator<std::uint64_t>(impl_->budget)};
  if (source.owners_.empty())
    pending.push_back(source.owner_);
  else
    pending.insert(pending.end(), source.owners_.begin(), source.owners_.end());
  while (!pending.empty()) {
    auto current = pending.back();
    pending.pop_back();
    if (current.object_id() == impl_->object)
      return reject(invalid_schema());
    if (std::find(visited.begin(), visited.end(), current.object_id()) !=
        visited.end())
      continue;
    visited.push_back(current.object_id());
    auto consumed = impl_->budget.consume({1});
    if (!consumed.ok())
      return reject(consumed);
    std::unique_lock<std::timed_mutex> source_lock(current.impl_->mutex,
                                                   std::defer_lock);
    while (!source_lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled())
        return reject({ErrorCode::Cancelled, {}});
    for (const auto& tensor : current.impl_->tensors) {
      for (const auto& owner : tensor.affine_owners)
        pending.push_back(owner);
      for (const auto& view : tensor.views)
        for (const auto& owner : view.owners)
          pending.push_back(owner);
    }
  }
  ResourceVector<ResultRef> owners{ResourceAllocator<ResultRef>(impl_->budget)};
  if (source.owners_.empty())
    owners.push_back(source.owner_);
  else
    owners.insert(owners.end(), source.owners_.begin(), source.owners_.end());
  return publish_tensor_storage(slot, region, std::move(layout),
                                common.value().storage(), std::move(relation),
                                finality, cancellation, std::move(owners));
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor_view(
    std::uint32_t slot, const Region& region,
    const std::vector<const ResultTensorReadWindow*>& sources,
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(impl_->budget))
    scope.emplace(impl_->budget);
  // All graph ownership mutations share one gate before taking Result locks.
  // Kernel callbacks never hold those locks, so snapshot traversal cannot form
  // a callback/ownership lock cycle. The gate makes indirect cycle checks and
  // edge publication atomic with competing builders and host associations.
  std::unique_lock<std::timed_mutex> ownership(result_ownership_mutex,
                                               std::defer_lock);
  while (!ownership.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled()) {
      impl_->cancelled_publication = true;
      return Status{ErrorCode::Cancelled, {}};
    }
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled()) {
      impl_->cancelled_publication = true;
      return Status{ErrorCode::Cancelled, {}};
    }
  if (cancellation.cancelled()) {
    impl_->cancelled_publication = true;
    return Status{ErrorCode::Cancelled, {}};
  }
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  auto unavailable = [] {
    return Status{
        ErrorCode::InvalidArgument,
        "ViewUnavailable: sources have no canonical common-owner mapping"};
  };
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  if (slot >= impl_->schema.tensors.size() || !finality.satisfied() ||
      !impl_->descriptor_relation.valid() || !relation.owned_by(impl_->budget))
    return reject(invalid_schema());
  const auto& spec = impl_->schema.tensors[slot];
  auto& target = impl_->tensors[slot];
  if (!spec.layout.spatial || region.empty() ||
      !region.validate(spec.sample_shape()).ok() ||
      !singleton_batches(spec, region) || sources.empty())
    return reject(invalid_schema());
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget = impl_->budget](auto n) {
    return budget.consume({n});
  };
  auto samples = Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  const auto domain_count = spec.sample_count();
  if (closed.value() != samples.value() ||
      relation.coverage() !=
          (domain_count.ok() ? domain_count.value() : UINT64_MAX))
    return reject(invalid_schema());
  auto overlap = samples.value().intersect(target.coverage, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(invalid_schema());
  auto next = target.coverage.unite(samples.value(), limits);
  if (!next.ok())
    return reject(next.status());
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto certified = relation.certify(samples.value(), limits);
    if (!certified.ok())
      return reject(certified);
  }
  std::vector<PlanarImage> tensors;
  std::vector<std::uint64_t> channels, counts;
  ResourceVector<ResultRef> owners(ResourceAllocator<ResultRef>(impl_->budget));
  std::vector<RegionDimension> spatial(
      region.dimensions().begin() + spec.batch_axes.size(),
      region.dimensions().end());
  for (const auto* source : sources) {
    if (source && source->valid() && source->owner_.request_record_)
      return reject({ErrorCode::InvalidArgument,
                     "terminal Result cannot supply a tensor view",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
    if (!source || !source->valid() || source->pieces_.empty() ||
        !source->affine_.empty())
      return unavailable();
    // A view may cross root budgets; retaining its independently accounted
    // source is mandatory. Only new output/view metadata is charged here.
    ResourceVector<ResultRef> pending(
        ResourceAllocator<ResultRef>(impl_->budget));
    if (source->owners_.empty())
      pending.push_back(source->owner_);
    else
      pending.insert(pending.end(), source->owners_.begin(),
                     source->owners_.end());
    std::vector<std::uint64_t> visited;
    while (!pending.empty()) {
      auto current = pending.back();
      pending.pop_back();
      if (current.object_id() == impl_->object)
        return reject(invalid_schema());
      if (std::find(visited.begin(), visited.end(), current.object_id()) !=
          visited.end())
        continue;
      if (visited.size() >= 4096)
        return reject(Status{ErrorCode::ResourceExhausted,
                             "view association work limit"});
      visited.push_back(current.object_id());
      auto work = impl_->budget.consume({1});
      if (!work.ok())
        return reject(work);
      std::unique_lock<std::timed_mutex> source_lock(current.impl_->mutex,
                                                     std::defer_lock);
      while (!source_lock.try_lock_for(std::chrono::milliseconds(2)))
        if (cancellation.cancelled())
          return reject(Status{ErrorCode::Cancelled, {}});
      for (const auto& image : current.impl_->tensors) {
        for (const auto& owner : image.affine_owners)
          pending.push_back(owner);
        for (const auto& view : image.views)
          for (const auto& owner : view.owners)
            pending.push_back(owner);
      }
    }
    const auto& source_spec = source->spec();
    const auto& input_region = source->region_.dimensions();
    if (input_region[source_spec.batch_axes.size() +
                     source_spec.layout.height_axis]
                .offset != spatial[spec.layout.height_axis].offset ||
        input_region[source_spec.batch_axes.size() +
                     source_spec.layout.height_axis]
                .extent != spatial[spec.layout.height_axis].extent ||
        input_region[source_spec.batch_axes.size() +
                     source_spec.layout.width_axis]
                .offset != spatial[spec.layout.width_axis].offset ||
        input_region[source_spec.batch_axes.size() +
                     source_spec.layout.width_axis]
                .extent != spatial[spec.layout.width_axis].extent)
      return unavailable();
    const auto& first = *source->pieces_[0].image_;
    const auto zero =
        std::vector<std::uint64_t>(source_spec.descriptor.shape.size(), 0);
    const auto origin = first.byte_offset(zero);
    if (!origin.ok())
      return reject(origin.status());
    for (const auto& piece : source->pieces_) {
      auto offset = piece.image_->byte_offset(zero);
      if (!offset.ok())
        return reject(offset.status());
      if (piece.image_->owner_token() != first.owner_token() ||
          offset.value() != origin.value())
        return unavailable();
    }
    const auto c = source_spec.layout.channel_axis
                       ? input_region[source_spec.batch_axes.size() +
                                      *source_spec.layout.channel_axis]
                       : RegionDimension{0, 1};
    tensors.push_back(first);
    channels.push_back(c.offset);
    counts.push_back(c.extent);
    if (source->owners_.empty())
      owners.push_back(source->owner_);
    else
      owners.insert(owners.end(), source->owners_.begin(),
                    source->owners_.end());
  }
  auto alias = PlanarImage::assemble_view(
      tensors, channels, counts, spec.descriptor, planar_layout(spec.layout),
      Region(std::move(spatial)), spec.facets, impl_->image_budget,
      cancellation, impl_->resources);
  if (!alias.ok()) {
    if (alias.status().code == ErrorCode::InvalidArgument &&
        alias.status().message.find("ViewUnavailable") != std::string::npos)
      return alias.status();
    return reject(alias.status());
  }
  auto restricted = relation.restrict_to(spec.sample_shape(), region);
  if (!restricted.ok())
    return reject(restricted.status());
  relation = restricted.take_value();
  auto combined =
      !target.coverage.empty() && !target.relation.same_owner(relation)
          ? ResultRelation::unite(impl_->budget, {target.relation, relation})
          : Result<ResultRelation>(relation);
  if (!combined.ok())
    return reject(combined.status());
  if (impl_->revision == UINT64_MAX)
    return reject(invalid_schema());
  target.views.push_back({region, alias.take_value(), std::move(owners)});
  target.coverage = next.take_value();
  target.relation = combined.take_value();
  ++impl_->revision;
  return Status::success();
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Result<ResultRef> ResultBuilder::seal() {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return Result<ResultRef>(busy);
  if (impl_->complete || !impl_->status().ok())
    return Result<ResultRef>(impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                                  : impl_->status());
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return Result<ResultRef>(status);
  };
  if (!impl_->descriptor_relation.valid())
    return reject(invalid_schema());
  for (std::size_t i = 0; i < impl_->schema.tensors.size(); ++i)
    if (!impl_->tensors[i].relation.valid())
      return reject(invalid_schema());
  for (std::uint32_t i = 0; i < impl_->schema.fields.size(); ++i) {
    const auto& field = impl_->schema.fields[i];
    const auto& state = impl_->fields[i];
    if (!state.relation.valid() || state.certified != state.written)
      return reject(invalid_schema());
    if (field.rows.kind != ResultExtentKind::RuntimeCount) {
      const auto count = field.rows.kind == ResultExtentKind::FieldRows
                             ? impl_->fields[field.rows.field].written
                             : field.rows.value;
      auto expected = scaled(field.rows, count);
      if (!expected.ok() || expected.value() != state.written)
        return reject(invalid_schema());
    }
  }
  if (impl_->revision == UINT64_MAX)
    return reject(invalid_schema());
  for (auto& field : impl_->fields)
    if (field.storage.valid()) {
      auto sealed = field.storage.seal();
      if (!sealed.ok())
        return reject(sealed);
    }
  impl_->complete = true;
  ++impl_->revision;
  return Result<ResultRef>(reference());
}
}  // namespace ps
