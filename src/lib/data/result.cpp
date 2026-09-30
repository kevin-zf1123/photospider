#include "photospider/data/result.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/stored_failure.hpp"
#include "data/input_validation.hpp"
#include "execution/dependency_records.hpp"
#include "photospider/data/representation.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
namespace {
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
std::vector<std::uint64_t> ResultImageSpec::sample_shape() const {
  std::vector<std::uint64_t> shape{frames, layers};
  shape.insert(shape.end(), descriptor.shape.begin(), descriptor.shape.end());
  return shape;
}
Result<std::uint64_t> ResultImageSpec::sample_count() const {
  if (!frames || !layers || frames > 4096 / layers ||
      (descriptor.shape.size() != 2 && descriptor.shape.size() != 3))
    return Result<std::uint64_t>(invalid_schema());
  std::uint64_t count = frames * layers;
  for (auto n : descriptor.shape) {
    if (!n || count > UINT64_MAX / n)
      return Result<std::uint64_t>(invalid_schema());
    count *= n;
  }
  return Result<std::uint64_t>(count);
}
Result<Footprint> ResultImageSpec::close_samples(
    const Footprint& samples, const FootprintLimits& limits) const {
  auto layout_status = PlanarImage::validate_layout(descriptor, layout);
  if (!layout_status.ok())
    return Result<Footprint>(layout_status);
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
    if (tuple && !box.empty())
      dimensions[*tuple + 2] = {0, descriptor.shape[*tuple]};
    boxes.emplace_back(std::move(dimensions));
  }
  return Footprint::from_regions(sample_shape(), boxes, limits);
}
const Footprint& ResultDescriptor::image_coverage(std::uint32_t slot) const {
  if (slot >= image_count_)
    throw std::out_of_range("invalid image slot");
  return images_[slot];
}
Status SchemaTemplate::validate(bool resolved) const {
  // These historical schemas pack image planes into result fields. Structural
  // images now require PlanarImage owners and cannot use ResultBuilder.
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
  if (!key_valid(id) || !version || (fields.empty() && images.empty()) ||
      fields.size() + images.size() > 16 || domain.size() > 8 ||
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
  for (const auto& image : images) {
    if (!key_valid(image.key) || !add_key(image.key) ||
        !image.sample_count().ok() ||
        !PlanarImage::validate_layout(image.descriptor, image.layout).ok())
      return invalid_schema();
    const auto tuple =
        input_internal::tuple_channel_axis(image.descriptor, image.facets);
    if (tuple &&
        (image.descriptor.shape.size() != 3 || !image.layout.channel_axis ||
         *tuple != *image.layout.channel_axis))
      return invalid_schema();
    for (const auto& facet : image.facets) {
      if (facet.key == "photospider.image" &&
          (image.layout.height_axis != 0 || image.layout.width_axis != 1))
        return invalid_schema();
      if (facet.key == "photospider.tensor-description") {
        auto tensor = decode_tensor_description(facet);
        if (!tensor.ok())
          return tensor.status();
        if (tensor.value().channel_axis != image.layout.channel_axis)
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
  for (const auto& image : images) {
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
  text("photospider.result-schema.v2");
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
  word(schema.images.size());
  for (const auto& image : schema.images) {
    text(image.key);
    word(image.frames);
    word(image.layers);
    word(static_cast<std::uint32_t>(image.descriptor.element_type));
    word(image.descriptor.shape.size());
    for (auto n : image.descriptor.shape)
      word(n);
    word(image.layout.height_axis);
    word(image.layout.width_axis);
    word(image.layout.channel_axis ? *image.layout.channel_axis + 1 : 0);
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
      8 + sizeof("photospider.result-schema.v2") - 1 + 8 + id.size() + 24 + 24;
  for (const auto& field : fields)
    size += 8 + field.key.size() + 8 + 56 + 8 + field.record_shape.size() * 8;
  for (const auto& image : images) {
    size += 8 + image.key.size() + 72 + image.descriptor.shape.size() * 8;
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
    copy.images = ResourceVector<ResultImageSpec>(
        ResourceAllocator<ResultImageSpec>(budget));
    copy.images.reserve(images.size());
    for (const auto& image : images) {
      std::uint64_t bytes =
          image.descriptor.shape.size() * sizeof(std::uint64_t);
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
      target.metadata_owner = lease.take_value();
      copy.images.push_back(std::move(target));
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
      images.size() != other.images.size() ||
      domain.size() != other.domain.size() ||
      metadata.size() != other.metadata.size())
    return false;
  for (std::size_t i = 0; i < images.size(); ++i) {
    const auto& a = images[i];
    const auto& b = other.images[i];
    if (a.key != b.key || a.frames != b.frames || a.layers != b.layers ||
        a.descriptor.shape != b.descriptor.shape ||
        a.descriptor.element_type != b.descriptor.element_type ||
        a.layout.height_axis != b.layout.height_axis ||
        a.layout.width_axis != b.layout.width_axis ||
        a.layout.channel_axis != b.layout.channel_axis ||
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
  struct Image {
    ResourceVector<PlanarImage> backing;
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
  ResourceLease lease;
  mutable std::mutex mutex;
  SchemaTemplate schema;
  ResourceString key;
  ResourceVector<std::uint64_t> association;
  ResourceVector<ResultRef> input_owners;
  std::array<Field, 16> fields;
  std::array<Image, 16> images;
  ResourceBindings resources;
  ResultRelation descriptor_relation;
  std::shared_ptr<const execution_internal::DependencyBundle> dependencies;
  ResultGrowthLimits limits;
  std::uint64_t object = 0, revision = 1, bytes = 0;
  core_internal::StoredFailure failure;
  bool complete = false, owners_bound = false;
};
struct ResultRef::Capture {
  ResourceLease lease;
  ResultDescriptor descriptor;
  std::array<ResultRelation, 16> fields, images;
  ResultRelation basis;
  std::shared_ptr<const execution_internal::DependencyBundle> dependencies;
};
Result<ResultRef> ResultRef::capture() const {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  if (captured_)
    return Result<ResultRef>(*this);
  std::lock_guard<std::mutex> lock(impl_->mutex);
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
    facts.image_count_ = impl_->schema.images.size();
    for (uint32_t i = 0; i < facts.field_count_; ++i) {
      facts.rows_[i] = impl_->fields[i].certified;
      capture->fields[i] = impl_->fields[i].relation;
    }
    for (uint32_t i = 0; i < facts.image_count_; ++i) {
      facts.images_[i] = impl_->images[i].coverage;
      capture->images[i] = impl_->images[i].relation;
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
  return weak;
}
ResultRef WeakResultRef::lock() const noexcept {
  ResultRef result;
  result.impl_ = impl_.lock();
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
    const ResourceVector<ResultRef>& inputs) const {
  if (!impl_ || inputs.size() > 1024)
    return invalid_schema();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto& input : inputs)
    if (!input.owned_by(impl_->budget) || input.object_id() == impl_->object)
      return invalid_schema();
  for (auto claimed : impl_->association)
    if (std::none_of(inputs.begin(), inputs.end(), [&](const auto& input) {
          return input.object_id() == claimed;
        }))
      return invalid_schema();
  try {
    ResourceVector<std::uint64_t> ids{
        ResourceAllocator<std::uint64_t>(impl_->budget)};
    for (const auto& input : inputs)
      ids.push_back(input.object_id());
    ResourceVector<ResultRef> owners(
        inputs.begin(), inputs.end(),
        ResourceAllocator<ResultRef>(impl_->budget));
    impl_->input_owners = std::move(owners);
    impl_->association = std::move(ids);
    impl_->owners_bound = true;
    return Status::success();
  } catch (const std::bad_alloc&) {
    return Status{ErrorCode::ResourceExhausted, {}};
  }
}

void ResultRef::bind_dependencies(
    std::shared_ptr<const execution_internal::DependencyBundle> bundle) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->dependencies = std::move(bundle);
}
std::shared_ptr<const execution_internal::DependencyBundle>
ResultRef::dependencies() const {
  if (captured_)
    return captured_->dependencies;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->dependencies;
}
ResourceVector<std::uint64_t> ResultRef::association() const {
  if (!impl_)
    throw std::logic_error("invalid ResultRef");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->association;
}
void ResultRef::bind_producer(std::uint64_t node) const noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->failure.bind_producer(node);
}
void ResultRef::retire_producer(const Status& failure) const noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->failure.bind_producer(failure.detail.node_id);
  if (!impl_->complete) {
    if (impl_->failure.ok())
      impl_->failure.record(failure);
    else
      impl_->failure.enrich(failure);
  }
}
Status ResultRef::production_status() const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (captured_ ? captured_->descriptor.sealed() : impl_->complete)
    return Status::success();
  return impl_->failure.ok() ? unavailable() : impl_->failure.status();
}
Result<ResultDescriptor> ResultRef::descriptor(bool require_complete) const {
  if (captured_)
    return require_complete && !captured_->descriptor.sealed()
               ? Result<ResultDescriptor>(unavailable())
               : Result<ResultDescriptor>(captured_->descriptor);
  if (!impl_)
    return Result<ResultDescriptor>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->complete &&
      (require_complete ||
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultDescriptor>(
        impl_->failure.ok() ? unavailable() : impl_->failure.status());
  ResultDescriptor facts;
  facts.object_ = impl_->object;
  facts.revision_ = impl_->revision;
  facts.sealed_ = impl_->complete;
  facts.field_count_ = static_cast<std::uint32_t>(impl_->schema.fields.size());
  for (std::uint32_t i = 0; i < facts.field_count_; ++i)
    facts.rows_[i] = impl_->fields[i].certified;
  facts.image_count_ = static_cast<std::uint32_t>(impl_->schema.images.size());
  for (std::uint32_t i = 0; i < facts.image_count_; ++i)
    facts.images_[i] = impl_->images[i].coverage;
  return Result<ResultDescriptor>(facts);
}
Status ResultRef::read_image(const ResultDescriptor& facts, std::uint32_t slot,
                             const std::vector<std::uint64_t>& at,
                             void* destination, std::size_t bytes,
                             const CancellationToken& cancellation) const {
  if (!impl_ ||
      (captured_ && facts.revision() > captured_->descriptor.revision()))
    return Status{ErrorCode::Stale, {}};
  if (!destination || slot >= facts.image_count_ || at.size() < 4)
    return Status{ErrorCode::InvalidArgument, "invalid image read"};
  PlanarImage backing;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (facts.object_ != impl_->object || !facts.revision_ ||
        facts.revision_ > impl_->revision ||
        slot >= impl_->schema.images.size())
      return Status{ErrorCode::Stale, "invalid image descriptor"};
    const auto& spec = impl_->schema.images[slot];
    if (at.size() != spec.descriptor.shape.size() + 2 ||
        bytes != Value::element_size(spec.descriptor.element_type) ||
        !facts.images_[slot].contains(at))
      return Status{ErrorCode::InvalidArgument, "unauthorized image sample"};
    backing = impl_->images[slot].backing[at[0] * spec.layers + at[1]];
  }
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget())
    scope.emplace(impl_->budget);
  auto admitted = impl_->budget.reserve(ResourceCapacity::host(
      at.size() * (sizeof(std::uint64_t) + sizeof(RegionDimension)),
      at.size() * (sizeof(std::uint64_t) + sizeof(RegionDimension))));
  if (!admitted.ok())
    return admitted.status();
  auto bridge = admitted.take_value();
  auto copied = impl_->budget.consume({1, bytes, 1});
  if (!copied.ok())
    return copied;
  std::vector<std::uint64_t> coordinate(at.begin() + 2, at.end());
  std::vector<RegionDimension> dimensions;
  for (auto n : coordinate)
    dimensions.push_back({n, 1});
  auto window = backing.acquire(Region(std::move(dimensions)), cancellation);
  if (!window.ok())
    return window.status();
  auto run = window.value().row_run(coordinate);
  if (!run.ok())
    return run.status();
  std::memcpy(destination, run.value().data, bytes);
  return Status::success();
}
Result<ResultRelation> ResultRef::image_relation(std::uint32_t slot) const {
  if (captured_)
    return slot < captured_->descriptor.image_count()
               ? Result<ResultRelation>(captured_->images[slot])
               : Result<ResultRelation>(unavailable());
  if (!impl_)
    return Result<ResultRelation>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (slot >= impl_->schema.images.size() ||
      !impl_->images[slot].relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(unavailable());
  return Result<ResultRelation>(impl_->images[slot].relation);
}
Result<ResultReadPlan> ResultRef::prepare_read(const ResultDescriptor& facts,
                                               std::uint32_t field,
                                               std::uint64_t first,
                                               std::uint64_t rows) const {
  if (!impl_ ||
      (captured_ && facts.revision() > captured_->descriptor.revision()))
    return Result<ResultReadPlan>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::mutex> lock(impl_->mutex);
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->descriptor_relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(unavailable());
  return Result<ResultRelation>(impl_->descriptor_relation);
}
Status ResultBuilder::bind_descriptor_relation(ResultRelation relation) {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::mutex> lock(impl_->mutex);
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
  if (semantic_key.empty() || semantic_key.size() > 4096 ||
      association.size() > 1024 ||
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
    for (std::size_t i = 0; i < impl->schema.images.size(); ++i) {
      const auto& spec = impl->schema.images[i];
      auto& image = impl->images[i];
      image.backing = ResourceVector<PlanarImage>(
          ResourceAllocator<PlanarImage>(impl->budget));
      image.backing.reserve(spec.frames * spec.layers);
      auto coverage = Footprint::none(spec.sample_shape());
      if (!coverage.ok())
        return Result<ResultBuilder>(coverage.status());
      image.coverage = coverage.take_value();
      auto empty_relation = ResultRelation::cartesian(
          impl->budget, spec.sample_count().value(), {0, 1, 0, 0});
      if (!empty_relation.ok())
        return Result<ResultBuilder>(empty_relation.status());
      image.relation = empty_relation.take_value();
      PlanarImageConfig config;
      config.order = spec.layout.order;
      config.height_axis = spec.layout.height_axis;
      config.width_axis = spec.layout.width_axis;
      config.channel_axis = spec.layout.channel_axis;
      config.row_pitch_bytes = spec.layout.row_pitch_bytes;
      config.groups = spec.layout.groups;
      config.tile_height = tile_height;
      config.tile_width = tile_width;
      config.maximum_backed_bytes = limits.maximum_bytes;
      config.aggregate_budget = page_budget;
      for (std::uint64_t n = 0; n < spec.frames * spec.layers; ++n) {
        auto backing = PlanarImage::create(spec.descriptor, config, spec.facets,
                                           impl->resources);
        if (!backing.ok())
          return Result<ResultBuilder>(backing.status());
        image.backing.push_back(backing.take_value());
      }
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->complete && impl_->failure.ok())
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
  std::lock_guard<std::mutex> lock(impl->mutex);
  if (impl->complete || !impl->failure.ok())
    return impl->failure.ok() ? Status{ErrorCode::Stale, {}}
                              : impl->failure.status();
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
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->complete || !impl_->failure.ok())
    return impl_->failure.ok() ? Status{ErrorCode::Stale, {}}
                               : impl_->failure.status();
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
Status ResultBuilder::publish_image(std::uint32_t slot, const Region& region,
                                    ByteView packed, ResultRelation relation,
                                    ResultFinality finality,
                                    const CancellationToken& cancellation) {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::optional<ResourceAllocationScope> metadata_scope;
  const auto* active_root = resource_internal::metadata_budget();
  if (!active_root || !active_root->same_owner(impl_->budget))
    metadata_scope.emplace(impl_->budget);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  if (impl_->complete || !impl_->failure.ok())
    return impl_->failure.ok() ? Status{ErrorCode::Stale, {}}
                               : impl_->failure.status();
  if (slot >= impl_->schema.images.size() || !finality.satisfied() ||
      !impl_->descriptor_relation.valid() || !relation.owned_by(impl_->budget))
    return reject(invalid_schema());
  const auto& spec = impl_->schema.images[slot];
  auto& target = impl_->images[slot];
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
  if (closed.value() != samples.value() ||
      relation.coverage() != spec.sample_count().value())
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
  if (!count.ok() || count.value() > UINT64_MAX / width ||
      count.value() * width != packed.size() || impl_->revision == UINT64_MAX)
    return reject(invalid_schema());
  auto copied_work = impl_->budget.consume(
      {count.value(), packed.size(), count.value() ? 1U : 0U});
  if (!copied_work.ok())
    return reject(copied_work);
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto complete = samples.value().visit(
        [&](const auto& at) {
          std::uint64_t index = 0;
          const auto shape = spec.sample_shape();
          for (std::size_t axis = 0; axis < at.size(); ++axis)
            index = index * shape[axis] + at[axis];
          return relation.visit(
              index, impl_->limits.maximum_rows,
              [](ResultSupport) { return Status::success(); });
        },
        impl_->limits.maximum_rows, cancellation);
    if (!complete.ok())
      return reject(complete);
  }
  auto combined =
      !target.coverage.empty() && target.relation.valid() &&
              !target.relation.same_owner(relation)
          ? ResultRelation::unite(impl_->budget, {target.relation, relation})
          : Result<ResultRelation>(relation);
  if (!combined.ok())
    return reject(combined.status());
  if (!samples.value().empty()) {
    std::vector<RegionDimension> spatial(region.dimensions().begin() + 2,
                                         region.dimensions().end());
    Region plane_region(std::move(spatial));
    const auto frame_count = region.dimensions()[0].extent;
    const auto layer_count = region.dimensions()[1].extent;
    const auto plane_bytes = packed.size() / frame_count / layer_count;
    ResourceVector<PlanarImageWriteWindow> writers{
        ResourceAllocator<PlanarImageWriteWindow>(impl_->budget)};
    writers.reserve(frame_count * layer_count);
    for (std::uint64_t n = region.dimensions()[0].offset;
         n < region.dimensions()[0].offset + frame_count; ++n)
      for (std::uint64_t l = region.dimensions()[1].offset;
           l < region.dimensions()[1].offset + layer_count; ++l) {
        auto prepared = target.backing[n * spec.layers + l].begin_write(
            plane_region, cancellation);
        if (!prepared.ok())
          return reject(prepared.status());
        writers.push_back(prepared.take_value());
      }
    auto plane =
        Footprint::from_regions(spec.descriptor.shape, {plane_region}, limits);
    if (!plane.ok())
      return reject(plane.status());
    for (std::size_t pair = 0; pair < writers.size(); ++pair) {
      std::uint64_t scalar = 0;
      auto status = plane.value().visit(
          [&](const auto& at) -> Status {
            auto run = writers[pair].row_run(at);
            if (!run.ok())
              return run.status();
            std::memcpy(run.value().data,
                        packed.data() + pair * plane_bytes + scalar * width,
                        width);
            ++scalar;
            return Status::success();
          },
          count.value(), cancellation);
      if (!status.ok())
        return reject(status);
    }
    if (cancellation.cancelled())
      return reject(Status{ErrorCode::Cancelled, {}});
    // All admission/copy/cancellation checks precede this allocation-free
    // commit barrier. A failed preparation retires every fresh page/window.
    for (auto& writer : writers) {
      auto status = writer.commit();
      if (!status.ok())
        return reject(status);
    }
  }
  target.coverage = coverage.take_value();
  target.relation = combined.take_value();
  ++impl_->revision;
  return Status::success();
}
Result<ResultRef> ResultBuilder::seal() {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->complete || !impl_->failure.ok())
    return Result<ResultRef>(impl_->failure.ok() ? Status{ErrorCode::Stale, {}}
                                                 : impl_->failure.status());
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return Result<ResultRef>(status);
  };
  if (!impl_->descriptor_relation.valid())
    return reject(invalid_schema());
  for (std::size_t i = 0; i < impl_->schema.images.size(); ++i)
    if (!impl_->images[i].relation.valid())
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
