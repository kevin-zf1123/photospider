#include "plugin/result_plugin.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "execution/cpu_range_context.hpp"
#include "plugin/utf8_validation.hpp"

namespace ps::plugin_internal {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
Status outcome(int code) {
  switch (code) {
    case 0:
      return Status::success();
    case 2:
      return {ErrorCode::Cancelled, {}};
    case 3:
      return {ErrorCode::BackendUnavailable, {}};
    case 4:
      return {ErrorCode::ResourceExhausted, {}};
    case 5:
      return {ErrorCode::TypeMismatch, {}};
    case 6:
      return {ErrorCode::InvalidArgument, {}};
    default:
      return {ErrorCode::OperationFailed, {}};
  }
}
int code(const Status& status) {
  return status.ok()                                    ? 0
         : status.code == ErrorCode::Cancelled          ? 2
         : status.code == ErrorCode::BackendUnavailable ? 3
         : status.code == ErrorCode::ResourceExhausted  ? 4
         : status.code == ErrorCode::TypeMismatch       ? 5
         : status.code == ErrorCode::InvalidArgument ||
                 status.code == ErrorCode::Stale
             ? 6
             : 1;
}
template <class T>
bool array(const T* pointer, std::uint64_t count, std::uint64_t maximum) {
  return count <= maximum && ((count == 0) == (pointer == nullptr)) &&
         (!pointer ||
          reinterpret_cast<std::uintptr_t>(pointer) % alignof(T) == 0);
}
Result<std::string> text(const char* pointer, std::uint32_t count,
                         std::uint32_t maximum = 128) {
  if (!pointer || !count || count > maximum)
    return Result<std::string>(invalid("invalid Result plugin text"));
  std::string string(pointer, count);
  if (!valid_utf8_key(string))
    return Result<std::string>(invalid("invalid Result plugin key"));
  return Result<std::string>(std::move(string));
}
Status facets(const ps_operation_facet_view_v11* input, std::uint32_t count,
              std::vector<ValueFacet>* output) {
  if (!array(input, count, 64))
    return invalid("invalid Result facets");
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& facet = input[i];
    if (facet.struct_size != sizeof(facet) || !facet.version ||
        !array(facet.payload, facet.payload_size, 65536))
      return invalid("invalid Result facet record");
    auto key = text(facet.key, facet.key_size, 256);
    if (!key.ok())
      return key.status();
    ValueFacet target;
    target.key = key.take_value();
    target.version = facet.version;
    if (facet.payload_size)
      target.payload.assign(facet.payload, facet.payload + facet.payload_size);
    output->push_back(std::move(target));
  }
  return input_internal::canonicalize_facets(output);
}
ResultExtent extent(const ps_result_extent_v1& source) {
  return {static_cast<ResultExtentKind>(source.kind),
          source.value,
          source.input,
          source.axis,
          source.field,
          source.divisor,
          source.offset};
}
Result<SchemaTemplate> schema(const ps_result_schema_v1* source) {
  if (!source ||
      reinterpret_cast<std::uintptr_t>(source) % alignof(ps_result_schema_v1) ||
      source->struct_size != sizeof(*source) ||
      !array(source->fields, source->field_count, 16) ||
      !array(source->images, source->image_count, 16) ||
      !array(source->domain, source->domain_rank, 8))
    return Result<SchemaTemplate>(invalid("invalid Result schema table"));
  auto id = text(source->id, source->id_size);
  if (!id.ok())
    return Result<SchemaTemplate>(id.status());
  SchemaTemplate target;
  target.id = id.take_value();
  target.version = source->version;
  target.publication = static_cast<PublishPolicy>(source->publication);
  for (std::uint32_t i = 0; i < source->field_count; ++i) {
    const auto& field = source->fields[i];
    if (field.struct_size != sizeof(field) || field.record_rank > 7)
      return Result<SchemaTemplate>(invalid("invalid Result field"));
    auto key = text(field.key, field.key_size);
    if (!key.ok())
      return Result<SchemaTemplate>(key.status());
    ResultFieldSpec copied;
    copied.key = key.take_value();
    copied.element_type = static_cast<ElementType>(field.element_type);
    copied.rows = extent(field.rows);
    copied.record_shape.assign(field.record_shape,
                               field.record_shape + field.record_rank);
    target.fields.push_back(std::move(copied));
  }
  for (std::uint32_t i = 0; i < source->image_count; ++i) {
    const auto& image = source->images[i];
    if (image.struct_size != sizeof(image) || image.rank < 2 ||
        image.rank > 3 || !array(image.groups, image.group_count, 64))
      return Result<SchemaTemplate>(invalid("invalid typed image record"));
    auto key = text(image.key, image.key_size);
    if (!key.ok())
      return Result<SchemaTemplate>(key.status());
    ResultImageSpec copied;
    copied.key = key.take_value();
    copied.frames = image.frames;
    copied.layers = image.layers;
    copied.descriptor.element_type =
        static_cast<ElementType>(image.element_type);
    copied.descriptor.shape.assign(image.shape, image.shape + image.rank);
    if (image.storage_order > 1)
      return Result<SchemaTemplate>(invalid("invalid image storage order"));
    copied.layout.order = static_cast<ImagePlaneOrder>(image.storage_order);
    copied.layout.row_pitch_bytes = image.row_pitch_bytes;
    copied.layout.height_axis = image.height_axis;
    copied.layout.width_axis = image.width_axis;
    copied.layout.channel_axis = image.channel_axis == UINT32_MAX
                                     ? std::optional<std::uint32_t>{}
                                     : image.channel_axis;
    for (std::uint32_t g = 0; g < image.group_count; ++g) {
      const auto& group = image.groups[g];
      if (group.struct_size != sizeof(group))
        return Result<SchemaTemplate>(invalid("invalid image group"));
      auto role = text(group.role, group.role_size);
      if (!role.ok())
        return Result<SchemaTemplate>(role.status());
      copied.layout.groups.push_back(
          {role.take_value(), group.first_channel, group.channel_count});
    }
    auto status = facets(image.facets, image.facet_count, &copied.facets);
    if (!status.ok())
      return Result<SchemaTemplate>(status);
    target.images.push_back(std::move(copied));
  }
  for (std::uint32_t i = 0; i < source->domain_rank; ++i)
    target.domain.push_back(extent(source->domain[i]));
  std::vector<ValueFacet> metadata;
  auto checked = facets(source->metadata, source->metadata_count, &metadata);
  if (!checked.ok())
    return Result<SchemaTemplate>(checked);
  for (const auto& facet : metadata)
    target.metadata.push_back(
        {ResourceString(facet.key.begin(), facet.key.end()), facet.version,
         ResourceVector<std::uint8_t>(facet.payload.begin(),
                                      facet.payload.end())});
  checked = target.validate();
  return checked.ok() ? Result<SchemaTemplate>(std::move(target))
                      : Result<SchemaTemplate>(checked);
}
Result<OperationMetadata> port(const ps_result_port_v1& input) {
  if (input.struct_size != sizeof(input))
    return Result<OperationMetadata>(invalid("invalid Result port size"));
  OperationMetadata metadata;
  if (input.kind == PS_RESULT_OBJECT_V1) {
    if (input.rank || input.facet_count || input.element_type)
      return Result<OperationMetadata>(
          invalid("Result port has Value metadata"));
    auto copied = schema(input.schema);
    if (!copied.ok())
      return Result<OperationMetadata>(copied.status());
    metadata.result_schema =
        std::make_shared<const SchemaTemplate>(copied.take_value());
  } else {
    if ((input.kind != PS_RESULT_VALUE_V1 &&
         input.kind != PS_RESULT_SCALAR_V1 &&
         input.kind != PS_RESULT_TYPED_V1) ||
        input.schema || input.rank < 1 || input.rank > 8)
      return Result<OperationMetadata>(invalid("invalid Value port"));
    metadata.descriptor = {
        static_cast<ElementType>(input.element_type),
        std::vector<std::uint64_t>(input.shape, input.shape + input.rank)};
    auto copied = facets(input.facets, input.facet_count, &metadata.facets);
    if (!copied.ok())
      return Result<OperationMetadata>(copied);
    copied = input_internal::validate_port_metadata({}, metadata);
    if (!copied.ok() || input_internal::structural_image_metadata(metadata))
      return Result<OperationMetadata>(
          invalid("image ports require Result slots"));
  }
  return Result<OperationMetadata>(std::move(metadata));
}
// Views borrow immutable strings/payloads from owned C++ metadata. Container
// capacities are admitted before allocation whenever a runtime root is active.
struct MetadataView {
  struct Port {
    ps_result_schema_v1 schema{};
    ResourceVector<ps_result_field_spec_v1> fields;
    ResourceVector<ps_result_image_spec_v1> images;
    ResourceVector<ps_result_extent_v1> domain;
    ResourceVector<ps_operation_facet_view_v11> facets, metadata;
    ResourceVector<ResourceVector<ps_result_group_v1>> groups;
    ResourceVector<ResourceVector<ps_operation_facet_view_v11>> image_facets;
  };
  ResourceVector<Port> storage;
  ResourceVector<ps_result_port_v1> ports;
  static ps_result_extent_v1 encode(const ResultExtent& e) {
    return {static_cast<uint32_t>(e.kind),
            e.input,
            e.axis,
            e.field,
            e.value,
            e.divisor,
            e.offset};
  }
  template <class Facets>
  static void encode_facets(
      const Facets& source,
      ResourceVector<ps_operation_facet_view_v11>& target) {
    target.reserve(source.size());
    for (const auto& f : source) {
      ps_operation_facet_view_v11 view{};
      view.struct_size = sizeof(view);
      view.key = f.key.data();
      view.key_size = f.key.size();
      view.version = f.version;
      view.payload = f.payload.empty() ? nullptr : f.payload.data();
      view.payload_size = f.payload.size();
      target.push_back(view);
    }
  }
  explicit MetadataView(const std::vector<OperationMetadata>& source)
      : MetadataView(source.data(), source.size()) {}
  MetadataView(const OperationMetadata* source, size_t count) {
    storage.resize(count);
    ports.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& b = storage[i];
      auto& p = ports[i];
      const auto& m = source[i];
      p.struct_size = sizeof(p);
      if (!m.result_schema) {
        p.kind = PS_RESULT_VALUE_V1;
        p.element_type = static_cast<uint32_t>(m.descriptor.element_type);
        p.rank = m.descriptor.shape.size();
        std::copy(m.descriptor.shape.begin(), m.descriptor.shape.end(),
                  p.shape);
        encode_facets(m.facets, b.facets);
        p.facets = b.facets.empty() ? nullptr : b.facets.data();
        p.facet_count = b.facets.size();
        continue;
      }
      p.kind = PS_RESULT_OBJECT_V1;
      p.schema = &b.schema;
      const auto& schema = *m.result_schema;
      b.schema.struct_size = sizeof(b.schema);
      b.schema.id = schema.id.data();
      b.schema.id_size = schema.id.size();
      b.schema.version = schema.version;
      b.schema.publication = static_cast<uint32_t>(schema.publication);
      b.fields.reserve(schema.fields.size());
      for (const auto& f : schema.fields) {
        ps_result_field_spec_v1 field{};
        field.struct_size = sizeof(field);
        field.key = f.key.data();
        field.key_size = f.key.size();
        field.element_type = static_cast<uint32_t>(f.element_type);
        field.record_rank = f.record_shape.size();
        std::copy(f.record_shape.begin(), f.record_shape.end(),
                  field.record_shape);
        field.rows = encode(f.rows);
        b.fields.push_back(field);
      }
      b.schema.fields = b.fields.empty() ? nullptr : b.fields.data();
      b.schema.field_count = b.fields.size();
      b.images.resize(schema.images.size());
      b.groups.resize(schema.images.size());
      b.image_facets.resize(schema.images.size());
      for (size_t j = 0; j < schema.images.size(); ++j) {
        auto& image = b.images[j];
        const auto& f = schema.images[j];
        image.struct_size = sizeof(image);
        image.key = f.key.data();
        image.key_size = f.key.size();
        image.element_type = static_cast<uint32_t>(f.descriptor.element_type);
        image.rank = f.descriptor.shape.size();
        std::copy(f.descriptor.shape.begin(), f.descriptor.shape.end(),
                  image.shape);
        image.frames = f.frames;
        image.layers = f.layers;
        image.height_axis = f.layout.height_axis;
        image.width_axis = f.layout.width_axis;
        image.channel_axis = f.layout.channel_axis.value_or(UINT32_MAX);
        image.storage_order = static_cast<uint32_t>(f.layout.order);
        image.row_pitch_bytes = f.layout.row_pitch_bytes;
        for (const auto& g : f.layout.groups)
          b.groups[j].push_back({sizeof(ps_result_group_v1), g.role.data(),
                                 static_cast<uint32_t>(g.role.size()),
                                 g.first_channel, g.channel_count});
        image.groups = b.groups[j].empty() ? nullptr : b.groups[j].data();
        image.group_count = b.groups[j].size();
        encode_facets(f.facets, b.image_facets[j]);
        image.facets =
            b.image_facets[j].empty() ? nullptr : b.image_facets[j].data();
        image.facet_count = b.image_facets[j].size();
      }
      b.schema.images = b.images.empty() ? nullptr : b.images.data();
      b.schema.image_count = b.images.size();
      for (const auto& d : schema.domain)
        b.domain.push_back(encode(d));
      b.schema.domain = b.domain.empty() ? nullptr : b.domain.data();
      b.schema.domain_rank = b.domain.size();
      encode_facets(schema.metadata, b.metadata);
      b.schema.metadata = b.metadata.empty() ? nullptr : b.metadata.data();
      b.schema.metadata_count = b.metadata.size();
    }
  }
};
void apply_constraint_view(MetadataView& view,
                           const ps_result_port_v1* prototypes) {
  for (size_t i = 0; i < view.ports.size(); ++i) {
    auto& p = view.ports[i];
    p.kind = prototypes[i].kind;
    p.minimum = prototypes[i].minimum;
    p.maximum = prototypes[i].maximum;
    p.semantic_kind = prototypes[i].semantic_kind;
  }
}
ResourceVector<ps_operation_parameter_value_v11> parameters_view(
    const std::map<std::string, ParameterValue>& source) {
  ResourceVector<ps_operation_parameter_value_v11> result;
  result.reserve(source.size());
  for (const auto& parameter : source) {
    ps_operation_parameter_value_v11 p{};
    p.struct_size = sizeof(p);
    p.key = parameter.first.data();
    p.key_size = parameter.first.size();
    if (const auto* integer = std::get_if<std::int64_t>(&parameter.second)) {
      p.type = 1;
      p.int64_value = *integer;
    } else if (const auto* number = std::get_if<double>(&parameter.second)) {
      p.type = 2;
      p.float64_value = *number;
    } else if (const auto* boolean = std::get_if<bool>(&parameter.second)) {
      p.type = 3;
      p.bool_value = *boolean;
    } else {
      const auto& string = std::get<std::string>(parameter.second);
      p.type = 4;
      p.string_value = string.data();
      p.string_size = string.size();
    }
    result.push_back(p);
  }
  return result;
}
Result<OperationPortConstraint> constraint(const ps_result_port_v1& p) {
  if (p.struct_size != sizeof(p))
    return Result<OperationPortConstraint>(
        invalid("invalid port constraint size"));
  OperationPortConstraint c;
  c.kind = static_cast<OperationPortKind>(p.kind);
  c.element_type = p.element_type;
  c.rank = p.rank;
  c.minimum = p.minimum;
  c.maximum = p.maximum;
  c.element_type_mask = p.element_type_mask;
  c.semantic_kind = p.semantic_kind;
  auto status = facets(p.facets, p.facet_count, &c.facets);
  if (!status.ok())
    return Result<OperationPortConstraint>(status);
  if (p.kind == PS_RESULT_OBJECT_V1) {
    auto copied = port(p);
    if (!copied.ok())
      return Result<OperationPortConstraint>(copied.status());
    c.result_schema_id = std::string(copied.value().result_schema->id);
    c.result_schema_version = copied.value().result_schema->version;
  } else if (p.schema ||
             (p.kind != PS_RESULT_VALUE_V1 && p.kind != PS_RESULT_SCALAR_V1 &&
              p.kind != PS_RESULT_TYPED_V1)) {
    return Result<OperationPortConstraint>(
        invalid("image ports require Result slots"));
  }
  return Result<OperationPortConstraint>(std::move(c));
}
struct MetadataSink {
  const ps_result_operation_v1& operation;
  std::vector<OperationOutputSpecialization> outputs;
  std::array<bool, 64> supplied{};
  Status failure;
  const std::thread::id owner = std::this_thread::get_id();
  static int set(void* raw, uint32_t index,
                 const ps_result_port_v1* source) noexcept {
    auto& sink = *static_cast<MetadataSink*>(raw);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || !source ||
          reinterpret_cast<std::uintptr_t>(source) %
              alignof(ps_result_port_v1) ||
          source->struct_size != sizeof(*source) ||
          index >= sink.outputs.size() || sink.supplied[index] ||
          source->kind != sink.operation.outputs[index].port.kind) {
        sink.failure = invalid("invalid metadata sink output/kind");
      } else {
        auto translated = port(*source);
        if (!translated.ok()) {
          sink.failure = translated.status();
        } else {
          const auto& prototype = sink.operation.outputs[index].port;
          if (source->minimum != prototype.minimum ||
              source->maximum != prototype.maximum ||
              source->semantic_kind != prototype.semantic_kind ||
              source->element_type_mask != prototype.element_type_mask) {
            sink.failure =
                invalid("metadata resolver changed output constraints");
          } else {
            sink.outputs[index].metadata = translated.take_value();
            sink.supplied[index] = true;
          }
        }
      }
    } catch (const std::bad_alloc&) {
      sink.failure = {ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      sink.failure = {ErrorCode::OperationFailed, {}};
    }
    return code(sink.failure);
  }
};
Result<ResourceLease> bridge_capacity(uint64_t bytes) {
  if (const auto* root = resource_internal::metadata_budget())
    return root->reserve(ResourceCapacity::host(bytes, bytes));
  return Result<ResourceLease>(ResourceLease{});
}
template <class Function>
Status with_coordinate(const uint64_t* at, uint32_t rank, Function function) {
  auto lease = bridge_capacity(rank * sizeof(uint64_t));
  if (!lease.ok())
    return lease.status();
  return function(std::vector<uint64_t>(at, at + rank));
}
struct ManagedRegion {
  ResourceLease lease;
  Region region;
};
Result<ManagedRegion> region(const ps_result_region_v1* source,
                             const std::vector<std::uint64_t>& shape) {
  if (!source ||
      reinterpret_cast<std::uintptr_t>(source) % alignof(ps_result_region_v1) ||
      source->struct_size != sizeof(*source) || source->rank != shape.size())
    return Result<ManagedRegion>(invalid("invalid Result region"));
  auto admitted = bridge_capacity(source->rank * sizeof(RegionDimension));
  if (!admitted.ok())
    return Result<ManagedRegion>(admitted.status());
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(source->rank);
  for (std::uint32_t i = 0; i < source->rank; ++i)
    dimensions.push_back({source->offset[i], source->extent[i]});
  Region target(std::move(dimensions));
  auto valid = target.validate(shape);
  return valid.ok() ? Result<ManagedRegion>(ManagedRegion{admitted.take_value(),
                                                          std::move(target)})
                    : Result<ManagedRegion>(valid);
}
struct Definition {
  ps_result_operation_v1 api;
  std::shared_ptr<void> library;
  OperationTraits traits;
};
class State final {
 public:
  State(std::shared_ptr<const Definition> definition, MutableBuffer bytes)
      : definition_(std::move(definition)),
        bytes_(std::move(bytes)),
        retained_(std::less<std::uint64_t>{},
                  ResourceAllocator<Retained::value_type>{}) {}
  ~State() noexcept {
    if (entered_) {
      try {
        definition_->api.destroy(definition_->api.user_data, bytes_.data());
      } catch (...) {
      }
    }
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    phase_ = &phase;
    owner_ = std::this_thread::get_id();
    active_ = true;
    auto lease =
        std::allocate_shared<Lease>(ResourceAllocator<Lease>(phase.resources));
    lease->owner = this;
    lease->cancellation = phase.query.cancellation;
    leases_.push_back(lease);
    struct Exit {
      State& self;
      std::shared_ptr<Lease> lease;
      ~Exit() {
        lease->active = false;
        self.active_ = false;
        self.phase_ = nullptr;
        self.scratch_.clear();
      }
    } exit{*this, lease};
    need_ = {};
    published_ = {};
    published_complete_ = false;
    value_ = {};
    value_parts_.clear();
    value_relation_ = {};
    value_descriptor_ = {};
    value_provided_ = false;
    if (!entered_)
      retained_ =
          Retained(std::less<std::uint64_t>{},
                   ResourceAllocator<Retained::value_type>(phase.resources));
    auto services = services_for(lease.get(), phase);
    const auto requested_count =
        phase.query.image_outputs   ? phase.query.image_outputs->boxes().size()
        : phase.query.value_outputs ? phase.query.value_outputs->boxes().size()
                                    : 0;
    if (requested_count > 65536 || phase.query.parameters.size() > 128)
      return Result<ResultProgramPoll>(invalid("unbounded C Result query"));
    ResourceVector<ps_result_region_v1> requested{
        ResourceAllocator<ps_result_region_v1>(phase.resources)};
    const auto* samples =
        phase.query.image_outputs   ? &*phase.query.image_outputs
        : phase.query.value_outputs ? &*phase.query.value_outputs
                                    : nullptr;
    if (samples) {
      for (const auto& box : samples->boxes()) {
        ps_result_region_v1 r{};
        r.struct_size = sizeof(r);
        r.rank = box.rank();
        for (std::uint32_t i = 0; i < r.rank; ++i) {
          r.offset[i] = box.dimensions()[i].offset;
          r.extent[i] = box.dimensions()[i].extent;
        }
        requested.push_back(r);
      }
    }
    ps_result_query_v1 query{};
    query.struct_size = sizeof(query);
    query.output_index = phase.query.output_index;
    query.image_slot = phase.query.image_slot;
    query.requested_kind = phase.query.image_outputs   ? 2
                           : phase.query.value_outputs ? 1
                                                       : 0;
    MetadataView input_views(phase.query.inputs),
        output_view(&phase.query.output, 1);
    apply_constraint_view(input_views, definition_->api.inputs);
    apply_constraint_view(output_view,
                          &definition_->api.outputs[query.output_index].port);
    auto resolved_output = definition_->api.outputs[query.output_index];
    resolved_output.port = output_view.ports[0];
    query.backend = static_cast<std::uint32_t>(phase.query.backend);
    query.inputs =
        input_views.ports.empty() ? nullptr : input_views.ports.data();
    query.input_count = input_views.ports.size();
    query.output = &resolved_output;
    query.requested = requested.empty() ? nullptr : requested.data();
    query.requested_count = requested.size();
    query.tile_height = phase.query.tile_height;
    query.tile_width = phase.query.tile_width;
    auto parameters = parameters_view(phase.query.parameters);
    query.parameters = parameters.empty() ? nullptr : parameters.data();
    query.parameter_count = parameters.size();
    if (!entered_) {
      entered_ = true;
      auto status = outcome(definition_->api.start(
          definition_->api.user_data, bytes_.data(), &query, &services));
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      if (!failure_.ok())
        return Result<ResultProgramPoll>(failure_);
      lease->active = false;
      lease = std::allocate_shared<Lease>(
          ResourceAllocator<Lease>(phase.resources));
      lease->owner = this;
      lease->cancellation = phase.query.cancellation;
      leases_.push_back(lease);
      exit.lease = lease;
      services = services_for(lease.get(), phase);
    }
    const auto result = definition_->api.poll(definition_->api.user_data,
                                              bytes_.data(), &query, &services);
    if (violation_.load())
      return Result<ResultProgramPoll>(
          invalid("Result service thread/lease violation"));
    if (!failure_.ok())
      return Result<ResultProgramPoll>(failure_);
    if (result == PS_RESULT_NEED_V1) {
      if (published_.valid() || value_ || value_provided_)
        return Result<ResultProgramPoll>(invalid("Need after publication"));
      return Result<ResultProgramPoll>(std::move(need_));
    }
    if (result != PS_RESULT_PUBLISH_V1)
      return Result<ResultProgramPoll>(outcome(result ? result : 1));
    if (!need_.values.empty() || !need_.images.empty() ||
        !need_.results.empty() || !need_.io.empty())
      return Result<ResultProgramPoll>(
          invalid("publication with pending Need"));
    if (value_provided_) {
      auto fragments = ValueFragments::create_view(
          phase.query.output.descriptor, phase.query.output.facets,
          *phase.query.value_outputs, value_parts_.data(), value_parts_.size(),
          {}, {}, phase.query.resources);
      if (!fragments.ok())
        return Result<ResultProgramPoll>(fragments.status());
      return Result<ResultProgramPoll>(ResultValuePublication{
          fragments.take_value(), value_relation_, value_descriptor_});
    }
    if (value_)
      return Result<ResultProgramPoll>(std::move(*value_));
    if (!published_.valid())
      return Result<ResultProgramPoll>(invalid("missing Result publication"));
    return Result<ResultProgramPoll>(
        ResultPublication{published_, published_complete_});
  }

 private:
  struct Lease {
    State* owner;
    CancellationToken cancellation;
    std::atomic<bool> active{true};
    ps_cpu_parallel_service_v1 parallel{};
    ps_cpu_tiles_service_v1 tiles{};
    ps_gpu_service_v11 gpu{};
    const ps_cpu_parallel_service_v1* host_parallel = nullptr;
    const ps_cpu_tiles_service_v1* host_tiles = nullptr;
    const ps_gpu_service_v11* host_gpu = nullptr;
  };
  using Retained = std::map<
      std::uint64_t, ResultImageInput, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, ResultImageInput>>>;
  static ps_result_services_v1 services_for(Lease* lease,
                                            const ResultProgramPhase& phase) {
    auto table = service(lease);
    lease->host_parallel = phase.cpu_parallel;
    lease->host_tiles = phase.cpu_tiles;
    lease->host_gpu = phase.gpu;
    if (phase.cpu_parallel) {
      lease->parallel = *phase.cpu_parallel;
      lease->parallel.context = lease;
      lease->parallel.run = parallel;
      table.cpu_parallel = &lease->parallel;
    }
    if (phase.cpu_tiles) {
      lease->tiles = *phase.cpu_tiles;
      lease->tiles.context = lease;
      lease->tiles.run = tiles;
      table.cpu_tiles = &lease->tiles;
    }
    if (phase.gpu) {
      lease->gpu = *phase.gpu;
      lease->gpu.context = lease;
      lease->gpu.buffer = gpu_buffer;
      lease->gpu.execute = gpu_execute;
      lease->gpu.release = gpu_release;
      table.gpu = &lease->gpu;
    }
    return table;
  }
  static int parallel(void* raw, uint64_t count, uint64_t grain,
                      uint32_t workers, ps_cpu_range_callback_v1 callback,
                      void* user) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](State&) {
      return outcome(lease->host_parallel->run(lease->host_parallel->context,
                                               count, grain, workers, callback,
                                               user));
    });
  }
  static int tiles(void* raw, const ps_cpu_tile_stage_v1* stage,
                   ps_cpu_tile_callback_v1 callback, void* user) {
    auto* lease = static_cast<Lease*>(raw);
    return call(raw, [&](State&) {
      return outcome(lease->host_tiles->run(lease->host_tiles->context, stage,
                                            callback, user));
    });
  }
  static Status gpu_outcome(State& state, int result) {
    if (state.phase_->gpu_status) {
      auto status = state.phase_->gpu_status();
      if (!status.ok())
        return status;
    }
    return outcome(result);
  }
  static int gpu_code(int result) {
    return result == 0 || result == 2 || result == 3 ? result : 1;
  }
  static int gpu_buffer(void* raw, const uint8_t* bytes, uint64_t count,
                        uint32_t writable, uint64_t* token) {
    auto* lease = static_cast<Lease*>(raw);
    return gpu_code(call(raw, [&](State& state) {
      return gpu_outcome(
          state, lease->host_gpu->buffer(lease->host_gpu->context, bytes, count,
                                         writable, token));
    }));
  }
  static int gpu_execute(void* raw, const ps_gpu_dispatch_v11* commands,
                         uint32_t count) {
    auto* lease = static_cast<Lease*>(raw);
    return gpu_code(call(raw, [&](State& state) {
      return gpu_outcome(state, lease->host_gpu->execute(
                                    lease->host_gpu->context, commands, count));
    }));
  }
  static int gpu_release(void* raw, uint64_t token) {
    auto* lease = static_cast<Lease*>(raw);
    return gpu_code(call(raw, [&](State& state) {
      return gpu_outcome(
          state, lease->host_gpu->release(lease->host_gpu->context, token));
    }));
  }
  static bool ready(State& state) {
    if (!state.active_ || std::this_thread::get_id() != state.owner_ ||
        execution_internal::in_cpu_range) {
      state.violation_ = true;
      return false;
    }
    return true;
  }
  template <class Function>
  static int call(void* context, Function function) noexcept {
    auto& lease = *static_cast<Lease*>(context);
    auto& state = *lease.owner;
    if (!lease.active.load() || !ready(state)) {
      state.violation_ = true;
      return 6;
    }
    Status status;
    if (state.violation_.load())
      status = {ErrorCode::InvalidArgument,
                {},
                FailureReason::UnauthorizedRead,
                {FailureOrigin::Protocol, FailureScope::Group}};
    else if (!state.failure_.ok())
      return code(state.failure_);
    try {
      if (status.ok())
        status = function(state);
    } catch (const std::bad_alloc&) {
      status = {ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      status = {ErrorCode::OperationFailed, {}};
    }
    if (state.violation_.load())
      status = {ErrorCode::InvalidArgument,
                {},
                FailureReason::UnauthorizedRead,
                {FailureOrigin::Protocol, FailureScope::Group}};
    if (!status.ok()) {
      if (state.failure_.ok())
        state.failure_ = std::move(status);
      try {
        if (state.phase_->failure_observer)
          state.phase_->failure_observer(state.failure_);
      } catch (...) {
      }
      if (state.phase_->failure) {
        auto expected = ErrorCode::Ok;
        state.phase_->failure->compare_exchange_strong(expected,
                                                       state.failure_.code);
      }
    }
    return code(state.failure_.ok() ? status : state.failure_);
  }
  Result<Footprint> samples(std::uint32_t input, std::uint32_t slot, bool image,
                            const ps_result_region_v1* regions,
                            std::uint32_t count) {
    if (input >= phase_->query.inputs.size() || !array(regions, count, 64))
      return Result<Footprint>(invalid("invalid image/Value Need"));
    const auto& metadata = phase_->query.inputs[input];
    if (image && (!metadata.result_schema ||
                  slot >= metadata.result_schema->images.size()))
      return Result<Footprint>(invalid("invalid image Need slot"));
    auto shape_admission = bridge_capacity(8 * sizeof(uint64_t));
    if (!shape_admission.ok())
      return Result<Footprint>(shape_admission.status());
    const auto shape = image
                           ? metadata.result_schema->images[slot].sample_shape()
                           : metadata.descriptor.shape;
    auto admitted = bridge_capacity(
        count * (sizeof(Region) + shape.size() * sizeof(RegionDimension)));
    if (!admitted.ok())
      return Result<Footprint>(admitted.status());
    std::vector<Region> boxes;
    boxes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto box = region(regions + i, shape);
      if (!box.ok())
        return Result<Footprint>(box.status());
      boxes.push_back(std::move(box.value().region));
    }
    FootprintLimits limits;
    limits.consume_work = phase_->consume_work;
    limits.cancellation = phase_->query.cancellation;
    return Footprint::from_regions(shape, boxes, limits);
  }
  Result<ResultRelation> relation(std::uint64_t outputs,
                                  const ps_result_relation_row_v1* rows,
                                  std::uint32_t count,
                                  std::uint32_t guarantee) {
    if (!array(rows, count, 65536) || guarantee < 1 || guarantee > 3)
      return Result<ResultRelation>(invalid("invalid Result relation rows"));
    if (guarantee == PS_RESULT_UNKNOWN_V1)
      return count ? Result<ResultRelation>(invalid("Unknown has rows"))
                   : ResultRelation::unknown(phase_->resources, outputs);
    if (!count)
      return ResultRelation::cartesian(
          phase_->resources, outputs, {0, 1, 0, 0},
          static_cast<DependencyGuarantee>(guarantee));
    return ResultRelation::sample_rows(
        phase_->resources, outputs, count,
        [&](std::uint64_t i) {
          const auto& row = rows[i];
          return Result<ResultRelationRow>(
              {row.output,
               {row.input, row.roles, row.first, row.count,
                static_cast<ResultSupportTarget>(row.target), row.slot}});
        },
        static_cast<DependencyGuarantee>(guarantee));
  }
  static int need_value(void* c, std::uint32_t input, std::uint32_t roles,
                        const ps_result_region_v1* boxes, std::uint32_t count) {
    return call(c, [&](State& s) {
      if (!roles || (roles & ~15U))
        return invalid("invalid Value Need roles");
      auto samples = s.samples(input, 0, false, boxes, count);
      if (!samples.ok())
        return samples.status();
      s.need_.values.push_back({input, samples.take_value(), roles});
      return Status::success();
    });
  }
  static int need_image(void* c, std::uint32_t input, std::uint32_t slot,
                        std::uint32_t roles, const ps_result_region_v1* boxes,
                        std::uint32_t count) {
    return call(c, [&](State& s) {
      if (!roles || (roles & ~15U))
        return invalid("invalid image Need roles");
      auto samples = s.samples(input, slot, true, boxes, count);
      if (!samples.ok())
        return samples.status();
      s.need_.images.push_back({input, slot, samples.take_value(), roles});
      return Status::success();
    });
  }
  static int need_result(void* c, std::uint32_t input, std::uint32_t field,
                         std::uint32_t complete, std::uint64_t rows) {
    return call(c, [&](State& s) {
      if (complete > 1)
        return invalid("invalid Result completion enum");
      s.need_.results.push_back({input, field, complete != 0, rows});
      return Status::success();
    });
  }
  static int read_value(void* c, std::uint32_t input, const std::uint64_t* at,
                        std::uint32_t rank, void* bytes, std::uint64_t size) {
    return call(c, [&](State& s) {
      if (!array(at, rank, 8) || !bytes || size > SIZE_MAX)
        return invalid("invalid Value read pointers");
      return with_coordinate(at, rank, [&](const auto& coordinate) {
        return s.phase_->read(input, coordinate, bytes, size);
      });
    });
  }
  static int read_image(void* c, std::uint32_t input, std::uint32_t slot,
                        const std::uint64_t* at, std::uint32_t rank,
                        void* bytes, std::uint64_t size) {
    return call(c, [&](State& s) {
      if (!array(at, rank, 8) || !bytes || size > SIZE_MAX)
        return invalid("invalid image read pointers");
      return with_coordinate(at, rank, [&](const auto& coordinate) {
        return s.phase_->read_image(input, slot, coordinate, bytes, size);
      });
    });
  }
  static int retain_image(void* c, std::uint32_t input, std::uint32_t slot,
                          std::uint64_t* handle) {
    return call(c, [&](State& s) {
      if (!handle || !s.phase_->images || s.next_handle_ == UINT64_MAX ||
          s.retained_.size() >= 1024)
        return invalid("invalid retained image request");
      auto found = s.phase_->images->find({input, slot});
      if (found == s.phase_->images->end())
        return invalid("image input is not ready");
      auto token = s.next_handle_++;
      s.retained_.emplace(token, found->second);
      *handle = token;
      return Status::success();
    });
  }
  static int read_retained(void* c, std::uint64_t handle,
                           const std::uint64_t* at, std::uint32_t rank,
                           void* bytes, std::uint64_t size) {
    return call(c, [&](State& s) {
      auto found = s.retained_.find(handle);
      if (found == s.retained_.end() || !array(at, rank, 8) || !bytes ||
          size > SIZE_MAX)
        return invalid("invalid retained image handle/read");
      return with_coordinate(at, rank, [&](const auto& coordinate) {
        return found->second.read(coordinate, bytes, size,
                                  s.phase_->query.cancellation);
      });
    });
  }
  static int release_image(void* c, std::uint64_t handle) {
    return call(c, [&](State& s) {
      return s.retained_.erase(handle) ? Status::success()
                                       : invalid("expired image handle");
    });
  }
  static int allocate(void* c, std::uint64_t bytes,
                      std::uint8_t** destination) {
    return call(c, [&](State& s) {
      if (!destination || !bytes)
        return invalid("invalid scratch request");
      auto made = s.phase_->allocator.allocate(bytes);
      if (!made.ok())
        return made.status();
      auto buffer = made.take_value();
      *destination = buffer.data();
      s.scratch_.push_back(std::move(buffer));
      return Status::success();
    });
  }
  static int release_scratch(void* c, std::uint8_t* pointer) {
    return call(c, [&](State& s) {
      auto found = std::find_if(s.scratch_.begin(), s.scratch_.end(),
                                [&](auto& b) { return b.data() == pointer; });
      if (found == s.scratch_.end())
        return invalid("expired scratch pointer");
      s.scratch_.erase(found);
      return Status::success();
    });
  }
  static int work(void* c, std::uint64_t units) {
    return call(c, [&](State& s) { return s.phase_->consume_work(units); });
  }
  static int cancelled(void* c) {
    auto& lease = *static_cast<Lease*>(c);
    if (!lease.active.load()) {
      lease.owner->violation_ = true;
      return 1;
    }
    return lease.cancellation.cancelled();
  }

  static int begin(void* c) {
    return call(c, [&](State& s) {
      if (s.builder_.reference().valid() ||
          !s.phase_->query.output.result_schema)
        return invalid("invalid Result begin");
      auto admitted =
          bridge_capacity(s.phase_->association
                              ? s.phase_->association->size() * sizeof(uint64_t)
                              : 0);
      if (!admitted.ok())
        return admitted.status();
      std::vector<std::uint64_t> association;
      if (s.phase_->association)
        association.assign(s.phase_->association->begin(),
                           s.phase_->association->end());
      auto made = ResultBuilder::start(
          s.phase_->resources, *s.phase_->query.output.result_schema,
          s.phase_->query.semantic_key, {}, std::move(association),
          s.phase_->query.tile_height, s.phase_->query.tile_width,
          s.phase_->query.resources);
      if (!made.ok())
        return made.status();
      s.builder_ = made.take_value();
      return Status::success();
    });
  }
  static int descriptor(void* c, const ps_result_relation_row_v1* rows,
                        std::uint32_t count, std::uint32_t guarantee) {
    return call(c, [&](State& s) {
      auto relation = s.relation(1, rows, count, guarantee);
      if (!relation.ok())
        return relation.status();
      if (!s.phase_->query.output.result_schema) {
        s.value_descriptor_ = relation.take_value();
        return Status::success();
      }
      return s.builder_.bind_descriptor_relation(relation.take_value());
    });
  }
  static int publish_image(void* c, std::uint32_t slot,
                           const ps_result_region_v1* box,
                           const std::uint8_t* bytes, std::uint64_t size,
                           const ps_result_relation_row_v1* rows,
                           std::uint32_t count, std::uint32_t guarantee,
                           std::uint32_t finality) {
    return call(c, [&](State& s) {
      const auto& schema = s.phase_->query.output.result_schema;
      if (!schema || slot >= schema->images.size() || size > SIZE_MAX ||
          (!bytes && size) || finality != 15)
        return invalid("invalid image publication envelope");
      auto shape_admission = bridge_capacity(8 * sizeof(uint64_t));
      if (!shape_admission.ok())
        return shape_admission.status();
      auto coverage = region(box, schema->images[slot].sample_shape());
      if (!coverage.ok())
        return coverage.status();
      auto relation = s.relation(schema->images[slot].sample_count().value(),
                                 rows, count, guarantee);
      if (!relation.ok())
        return relation.status();
      return s.builder_.publish_image(
          slot, coverage.value().region, ByteView(bytes, size),
          relation.take_value(), {true, true, true, true},
          s.phase_->query.cancellation);
    });
  }
  static int append_field(void* c, std::uint32_t field, std::uint64_t rows,
                          const std::uint8_t* bytes, std::uint64_t size) {
    return call(c, [&](State& s) {
      if (!bytes || !size || size > SIZE_MAX)
        return invalid("invalid field payload");
      auto made = s.phase_->allocator.allocate(size);
      if (!made.ok())
        return made.status();
      auto copy = made.take_value();
      auto work = s.phase_->consume_work(size);
      if (!work.ok())
        return work;
      std::memcpy(copy.data(), bytes, size);
      auto write =
          s.builder_.prepare_append(field, rows, std::move(copy).freeze());
      if (!write.ok())
        return write.status();
      s.need_.io.push_back(write.take_value());
      return Status::success();
    });
  }
  static int publish_field(void* c, std::uint32_t field, std::uint64_t end,
                           const ps_result_relation_row_v1* rows,
                           std::uint32_t count, std::uint32_t guarantee,
                           std::uint32_t finality) {
    return call(c, [&](State& s) {
      if (finality != 15)
        return invalid("incomplete field finality");
      auto relation = s.relation(end, rows, count, guarantee);
      return relation.ok()
                 ? s.builder_.publish(field, end, relation.take_value(),
                                      {true, true, true, true})
                 : relation.status();
    });
  }
  static int need_field_read(void* c, std::uint32_t input, std::uint32_t field,
                             std::uint64_t first, std::uint64_t rows) {
    return call(c, [&](State& s) {
      auto found = s.phase_->results.find(input);
      if (found == s.phase_->results.end())
        return invalid("Result field input is not ready");
      auto facts = found->second.descriptor(false);
      if (!facts.ok())
        return facts.status();
      auto read = found->second.prepare_read(facts.value(), field, first, rows);
      if (!read.ok())
        return read.status();
      s.need_.io.push_back(read.take_value());
      return Status::success();
    });
  }
  static int read_io(void* c, std::uint32_t index, std::uint64_t offset,
                     std::uint8_t* bytes, std::uint64_t size) {
    return call(c, [&](State& s) {
      if (index >= s.phase_->io.size() || !bytes || !size || size > SIZE_MAX)
        return invalid("invalid I/O reply read");
      const auto* window =
          std::get_if<std::shared_ptr<const CpuStorage>>(&s.phase_->io[index]);
      if (!window || offset > (*window)->bytes().size() ||
          size > (*window)->bytes().size() - offset)
        return invalid("I/O reply bounds");
      auto work = s.phase_->consume_work(size);
      if (!work.ok())
        return work;
      std::memcpy(bytes, (*window)->bytes().data() + offset, size);
      return Status::success();
    });
  }
  static int publish_result(void* c, std::uint32_t complete) {
    return call(c, [&](State& s) {
      if (complete > 1 || s.published_.valid() || s.value_)
        return invalid("invalid Result publication");
      if (complete) {
        auto sealed = s.builder_.seal();
        if (!sealed.ok())
          return sealed.status();
        s.published_ = sealed.take_value();
      } else {
        s.published_ = s.builder_.reference();
        if (!s.published_.valid())
          return invalid("missing Result builder");
        auto facts = s.published_.descriptor(false);
        if (!facts.ok())
          return facts.status();
      }
      s.published_complete_ = complete != 0;
      return Status::success();
    });
  }
  static int result_descriptor(void* c, std::uint32_t input,
                               ps_result_descriptor_v1* output) {
    return call(c, [&](State& s) {
      if (!output ||
          reinterpret_cast<std::uintptr_t>(output) %
              alignof(ps_result_descriptor_v1) ||
          output->struct_size != sizeof(*output))
        return invalid("invalid Result descriptor destination");
      auto found = s.phase_->results.find(input);
      if (found == s.phase_->results.end())
        return invalid("Result descriptor Need is absent");
      auto facts = found->second.descriptor(false);
      if (!facts.ok())
        return facts.status();
      *output = {};
      output->struct_size = sizeof(*output);
      output->sealed = facts.value().sealed();
      output->field_count = facts.value().field_count();
      output->image_count = facts.value().image_count();
      output->object_id = facts.value().object_id();
      output->revision = facts.value().revision();
      for (std::uint32_t i = 0; i < output->field_count; ++i)
        output->rows[i] = facts.value().rows(i);
      return Status::success();
    });
  }

  static int publish_value(void* c, const ps_result_region_v1* box,
                           const std::uint8_t* bytes, std::uint64_t size,
                           const ps_result_relation_row_v1* rows,
                           std::uint32_t count, std::uint32_t guarantee) {
    return call(c, [&](State& s) {
      if (s.published_.valid() || !s.phase_->query.value_outputs ||
          (!bytes && size) || size > SIZE_MAX)
        return invalid("invalid Value publication");
      auto coverage = region(box, s.phase_->query.output.descriptor.shape);
      if (!coverage.ok())
        return coverage.status();
      std::optional<Value> fragment;
      if (!coverage.value().region.empty()) {
        auto made = MutableValue::allocate(s.phase_->query.output.descriptor,
                                           coverage.value().region,
                                           s.phase_->allocator);
        if (!made.ok())
          return made.status();
        auto writer = made.take_value();
        if (writer.size() != size)
          return invalid("Value payload size mismatch");
        auto work = s.phase_->consume_work(size);
        if (!work.ok())
          return work;
        std::memcpy(writer.data(), bytes, size);
        auto value = std::move(writer).publish(s.phase_->query.output.facets,
                                               s.phase_->query.resources);
        if (!value.ok())
          return value.status();
        fragment = value.take_value();
      } else if (size) {
        return invalid("Empty Value publication has bytes");
      }
      std::uint64_t outputs = 1;
      for (auto n : s.phase_->query.output.descriptor.shape) {
        if (outputs > UINT64_MAX / n)
          return invalid("Value relation overflow");
        outputs *= n;
      }
      auto relation = s.relation(outputs, rows, count, guarantee);
      if (!relation.ok())
        return relation.status();
      auto combined =
          s.value_relation_.valid()
              ? ResultRelation::unite(s.phase_->resources,
                                      {s.value_relation_, relation.value()})
              : relation;
      if (!combined.ok())
        return combined.status();
      s.value_relation_ = combined.take_value();
      if (fragment)
        s.value_parts_.push_back(std::move(*fragment));
      s.value_provided_ = true;
      return Status::success();
    });
  }
  static ps_result_services_v1 service(Lease* lease) {
    return {sizeof(ps_result_services_v1),
            1,
            lease,
            need_value,
            need_image,
            need_result,
            read_value,
            read_image,
            retain_image,
            read_retained,
            release_image,
            allocate,
            release_scratch,
            work,
            cancelled,
            begin,
            descriptor,
            publish_image,
            append_field,
            publish_field,
            need_field_read,
            read_io,
            publish_result,
            result_descriptor,
            publish_value,
            nullptr,
            nullptr,
            nullptr};
  }
  std::shared_ptr<const Definition> definition_;
  MutableBuffer bytes_;
  Retained retained_;
  ResourceVector<MutableBuffer> scratch_;
  ResultProgramNeed need_;
  ResultBuilder builder_;
  ResultRef published_;
  std::optional<ResultValuePublication> value_;
  ResourceVector<Value> value_parts_;
  ResultRelation value_relation_, value_descriptor_;
  bool value_provided_ = false;
  const ResultProgramPhase* phase_ = nullptr;
  Status failure_;
  std::thread::id owner_;
  std::atomic<bool> violation_{false};
  std::atomic<bool> active_{false};
  bool entered_ = false, published_complete_ = false;
  ResourceVector<std::shared_ptr<Lease>> leases_;
  std::uint64_t next_handle_ = 1;
};
}  // namespace
Result<std::vector<OperationDefinition>> import_result_plugin(
    const ps_result_operation_plugin_api_v1* api,
    std::shared_ptr<void> library) {
  if (!api ||
      reinterpret_cast<std::uintptr_t>(api) %
          alignof(ps_result_operation_plugin_api_v1) ||
      api->struct_size != sizeof(*api) || api->abi_version != 1 ||
      !array(api->operations, api->operation_count, 1024) ||
      !api->operation_count || !api->destroy)
    return Result<std::vector<OperationDefinition>>(
        invalid("invalid Result ABI table"));
  std::vector<OperationDefinition> definitions;
  for (std::uint32_t i = 0; i < api->operation_count; ++i) {
    const auto& op = api->operations[i];
    if (op.struct_size != sizeof(op) ||
        !array(op.inputs, op.input_count, 1024) ||
        !array(op.outputs, op.output_count, 64) || !op.output_count ||
        !op.start || !op.poll || !op.destroy || op.state_bytes > 1048576 ||
        !op.maximum_stages || op.maximum_stages > 1048576 ||
        op.cpu_staged_tiles > 1 ||
        (op.flags & ~(PS_OPERATION_FLAG_DETERMINISTIC |
                      PS_OPERATION_FLAG_SIDE_EFFECT_FREE |
                      PS_OPERATION_FLAG_CPU | PS_OPERATION_FLAG_GPU)))
      return Result<std::vector<OperationDefinition>>(
          invalid("invalid Result operation table"));
    auto key = text(op.key, op.key_size);
    if (!key.ok())
      return Result<std::vector<OperationDefinition>>(key.status());
    OperationDefinition definition;
    definition.key = key.take_value();
    auto& traits = definition.traits;
    traits.input_count = op.input_count;
    traits.supports_cpu = (op.flags & PS_OPERATION_FLAG_CPU) != 0;
    traits.supports_gpu = (op.flags & PS_OPERATION_FLAG_GPU) != 0;
    traits.deterministic = (op.flags & PS_OPERATION_FLAG_DETERMINISTIC) != 0;
    traits.side_effect_free =
        (op.flags & PS_OPERATION_FLAG_SIDE_EFFECT_FREE) != 0;
    traits.cpu_staged_tiles = op.cpu_staged_tiles != 0;
    traits.workspace_bytes = op.workspace_bytes;
    traits.input_schema.clear();
    traits.outputs.clear();
    for (std::uint32_t input = 0; input < op.input_count; ++input) {
      auto copied = constraint(op.inputs[input]);
      if (!copied.ok())
        return Result<std::vector<OperationDefinition>>(copied.status());
      traits.input_schema.push_back(copied.take_value());
    }
    for (std::uint32_t output = 0; output < op.output_count; ++output) {
      const auto& declared = op.outputs[output];
      if (declared.struct_size != sizeof(declared) ||
          (declared.execution != 1 && declared.execution != 2) ||
          (declared.input_count != UINT32_MAX &&
           !array(declared.input_indices, declared.input_count, 1024)) ||
          (declared.input_count == UINT32_MAX && declared.input_indices))
        return Result<std::vector<OperationDefinition>>(
            invalid("invalid named Result output"));
      auto name = text(declared.key, declared.key_size);
      if (!name.ok())
        return Result<std::vector<OperationDefinition>>(name.status());
      auto metadata = port(declared.port);
      if (!metadata.ok())
        return Result<std::vector<OperationDefinition>>(metadata.status());
      auto output_constraint = constraint(declared.port);
      if (!output_constraint.ok())
        return Result<std::vector<OperationDefinition>>(
            output_constraint.status());
      OperationOutputTraits contract;
      contract.output_schema = output_constraint.take_value();
      contract.key = name.take_value();
      contract.dependency_version = 2;
      contract.region_rule = declared.execution == 1
                                 ? OperationRegionRule::Whole
                                 : OperationRegionRule::Dependency;
      contract.continuation_bytes =
          sizeof(State) + std::max<std::uint64_t>(1, op.state_bytes);
      contract.maximum_dependency_stages = op.maximum_stages;
      if (declared.input_count != UINT32_MAX) {
        contract.input_indices = std::vector<std::uint32_t>{};
        if (declared.input_count)
          contract.input_indices->assign(
              declared.input_indices,
              declared.input_indices + declared.input_count);
      }
      if (metadata.value().result_schema) {
        contract.result_schema = *metadata.value().result_schema;
        contract.output_schema.kind = OperationPortKind::Result;
        contract.output_schema.result_schema_id =
            std::string(metadata.value().result_schema->id);
        contract.output_schema.result_schema_version =
            metadata.value().result_schema->version;
      } else {
        contract.output_element_type = metadata.value().descriptor.element_type;
        contract.shape_rule = OperationShapeRule::Fixed;
        contract.fixed_output_shape = metadata.value().descriptor.shape;
        contract.output_facets = metadata.value().facets;
        if (!contract.output_facets.empty())
          contract.output_semantic_rule = OperationSemanticRule::Establish;
      }
      traits.outputs.push_back(std::move(contract));
    }
    if (!array(op.parameters, op.parameter_count, 128))
      return Result<std::vector<OperationDefinition>>(
          invalid("invalid Result parameter table"));
    for (std::uint32_t p = 0; p < op.parameter_count; ++p) {
      const auto& parameter = op.parameters[p];
      if (parameter.struct_size != sizeof(parameter) ||
          parameter.required > 1 || parameter.bounded > 1)
        return Result<std::vector<OperationDefinition>>(
            invalid("invalid Result parameter"));
      auto name = text(parameter.key, parameter.key_size);
      if (!name.ok())
        return Result<std::vector<OperationDefinition>>(name.status());
      traits.parameter_schema.push_back(
          {name.take_value(),
           static_cast<OperationParameterType>(parameter.type),
           parameter.required != 0, parameter.bounded != 0, parameter.minimum,
           parameter.maximum});
    }
    traits.requires_metadata_specialization = op.resolve_metadata != nullptr;
    auto owner = std::make_shared<Definition>(Definition{op, library, traits});
    if (op.resolve_metadata) {
      definition.prepare_static =
          [owner](const std::vector<OperationMetadata>& inputs,
                  const std::map<std::string, ParameterValue>& parameters)
          -> Result<OperationPreparation> {
        MetadataView view(inputs);
        apply_constraint_view(view, owner->api.inputs);
        auto values = parameters_view(parameters);
        ResourceVector<ps_result_port_v1> prototypes;
        prototypes.reserve(owner->api.output_count);
        for (uint32_t i = 0; i < owner->api.output_count; ++i)
          prototypes.push_back(owner->api.outputs[i].port);
        MetadataSink sink{
            owner->api,
            std::vector<OperationOutputSpecialization>(owner->api.output_count),
            {},
            Status::success()};
        ps_result_metadata_sink_v1 service{sizeof(service), &sink,
                                           MetadataSink::set};
        auto status = outcome(owner->api.resolve_metadata(
            owner->api.user_data,
            view.ports.empty() ? nullptr : view.ports.data(), view.ports.size(),
            values.empty() ? nullptr : values.data(), values.size(),
            prototypes.data(), prototypes.size(), &service));
        if (!sink.failure.ok())
          return Result<OperationPreparation>(sink.failure);
        if (!status.ok())
          return Result<OperationPreparation>(status);
        for (uint32_t i = 0; i < owner->api.output_count; ++i)
          if (!sink.supplied[i])
            return Result<OperationPreparation>(
                invalid("metadata resolver omitted output"));
        OperationPreparation prepared;
        prepared.outputs = std::move(sink.outputs);
        return Result<OperationPreparation>(std::move(prepared));
      };
    }
    definition.validate_dependency =
        [owner](const std::vector<OperationMetadata>& inputs,
                const std::map<std::string, ParameterValue>&) {
          if (inputs.size() != owner->api.input_count)
            return invalid("C Result input count mismatch");
          if (owner->api.resolve_metadata)
            return Status::success();
          for (std::uint32_t i = 0; i < inputs.size(); ++i) {
            auto expected = port(owner->api.inputs[i]);
            if (!expected.ok())
              return expected.status();
            if (expected.value().result_schema) {
              if (!inputs[i].result_schema ||
                  !inputs[i].result_schema->same_schema(
                      *expected.value().result_schema))
                return Status{ErrorCode::TypeMismatch,
                              "C Result input schema mismatch"};
            } else if (inputs[i].descriptor.shape !=
                           expected.value().descriptor.shape ||
                       inputs[i].descriptor.element_type !=
                           expected.value().descriptor.element_type ||
                       !input_internal::same_facets(inputs[i].facets,
                                                    expected.value().facets)) {
              return Status{ErrorCode::TypeMismatch,
                            "C Result Value input metadata mismatch"};
            }
          }
          return Status::success();
        };
    definition.start_result = [owner](const ResultProgramQuery&,
                                      const BufferAllocator& allocator) {
      auto allocated = allocator.allocate(
          std::max<std::uint64_t>(1, owner->api.state_bytes));
      if (!allocated.ok())
        return Result<ResultContinuation>(allocated.status());
      auto buffer = allocated.take_value();
      if (buffer.size())
        std::memset(buffer.data(), 0, buffer.size());
      return ResultContinuation::make<State>(allocator, owner,
                                             std::move(buffer));
    };
    definitions.push_back(std::move(definition));
  }
  return Result<std::vector<OperationDefinition>>(std::move(definitions));
}
}  // namespace ps::plugin_internal
