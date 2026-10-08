#include "plugin/result_c_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/utf8_validation.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::result_c {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
Status outcome(int code) {
  switch (code) {
    case PS_RESULT_STATUS_OK_V2:
      return Status::success();
    case PS_RESULT_STATUS_CANCELLED_V2:
      return {ErrorCode::Cancelled, {}};
    case PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2:
      return {ErrorCode::BackendUnavailable, {}};
    case PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V2:
      return {ErrorCode::ResourceExhausted, {}};
    case PS_RESULT_STATUS_TYPE_MISMATCH_V2:
      return {ErrorCode::TypeMismatch, {}};
    case PS_RESULT_STATUS_INVALID_ARGUMENT_V2:
      return {ErrorCode::InvalidArgument, {}};
    default:
      return {ErrorCode::OperationFailed, {}};
  }
}
int code(const Status& status) {
  return status.ok()                           ? PS_RESULT_STATUS_OK_V2
         : status.code == ErrorCode::Cancelled ? PS_RESULT_STATUS_CANCELLED_V2
         : status.code == ErrorCode::BackendUnavailable
             ? PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V2
         : status.code == ErrorCode::ResourceExhausted
             ? PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V2
         : status.code == ErrorCode::TypeMismatch
             ? PS_RESULT_STATUS_TYPE_MISMATCH_V2
         : status.code == ErrorCode::InvalidArgument ||
                 status.code == ErrorCode::Stale
             ? PS_RESULT_STATUS_INVALID_ARGUMENT_V2
             : PS_RESULT_STATUS_FAILURE_V2;
}
ps_result_atom_key_v2 atom_view(const AtomKey& key) {
  ps_result_atom_key_v2 result{};
  result.output_index = key.output_index;
  result.rank = key.rank;
  std::copy(key.coordinate.begin(), key.coordinate.end(), result.coordinate);
  return result;
}
AtomKey atom_key(const ps_result_atom_key_v2& key) {
  AtomKey result{key.output_index, key.rank, {}};
  std::copy(std::begin(key.coordinate), std::end(key.coordinate),
            result.coordinate.begin());
  return result;
}
bool empty_atom(const ps_result_atom_key_v2& key) {
  return !key.output_index && !key.rank &&
         std::all_of(std::begin(key.coordinate), std::end(key.coordinate),
                     [](auto at) { return at == 0; });
}
bool empty_failure(const ps_result_atom_failure_v2& failure) {
  return !failure.struct_size && !failure.code && !failure.reason &&
         !failure.origin && !failure.scope && !failure.reserved &&
         empty_atom(failure.atom) && empty_atom(failure.domain.first) &&
         std::all_of(std::begin(failure.domain.extent),
                     std::end(failure.domain.extent),
                     [](auto extent) { return extent == 0; }) &&
         !failure.message_size &&
         std::all_of(std::begin(failure.message), std::end(failure.message),
                     [](char byte) { return byte == 0; });
}
Result<std::string> text(const char* pointer, std::uint32_t count,
                         std::uint32_t maximum) {
  if (!pointer || !count || count > maximum)
    return Result<std::string>(invalid("invalid Result plugin text"));
  std::string string(pointer, count);
  if (!core_internal::valid_utf8_key(string))
    return Result<std::string>(invalid("invalid Result plugin key"));
  return Result<std::string>(std::move(string));
}
Status facets(const ps_result_facet_view_v2* input, std::uint32_t count,
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
ResultExtent extent(const ps_result_extent_v2& source) {
  return {static_cast<ResultExtentKind>(source.kind),
          source.value,
          source.input,
          source.axis,
          source.field,
          source.divisor,
          source.offset};
}
Result<SchemaTemplate> schema(const ps_result_schema_v2* source) {
  if (!source ||
      reinterpret_cast<std::uintptr_t>(source) % alignof(ps_result_schema_v2) ||
      source->struct_size != sizeof(*source) ||
      !array(source->fields, source->field_count, 16) ||
      !array(source->tensors, source->tensor_count, 16) ||
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
  for (std::uint32_t i = 0; i < source->tensor_count; ++i) {
    const auto& image = source->tensors[i];
    if (image.struct_size != sizeof(image) || image.rank < 1 ||
        image.rank > 8 || image.batch_rank > 8 - image.rank ||
        image.spatial > 1 || !array(image.groups, image.group_count, 64))
      return Result<SchemaTemplate>(invalid("invalid typed image record"));
    auto key = text(image.key, image.key_size);
    if (!key.ok())
      return Result<SchemaTemplate>(key.status());
    ResultTensorSpec copied;
    copied.key = key.take_value();
    copied.batch_axes.assign(image.batch_shape,
                             image.batch_shape + image.batch_rank);
    copied.atomic_trailing_axes = image.atomic_trailing_axes;
    copied.layout.spatial = image.spatial != 0;
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
    target.tensors.push_back(std::move(copied));
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
Result<OperationPortConstraint> constraint(const ps_result_port_v2& p);
Result<OperationMetadata> port(const ps_result_port_v2& input) {
  if (input.struct_size != sizeof(input))
    return Result<OperationMetadata>(invalid("invalid Result port size"));
  OperationMetadata metadata;
  if (input.kind != PS_RESULT_OBJECT_V2)
    return Result<OperationMetadata>(invalid("invalid Result port kind"));
  auto copied = schema(input.schema);
  if (!copied.ok())
    return Result<OperationMetadata>(copied.status());
  metadata.result_schema =
      std::make_shared<const SchemaTemplate>(copied.take_value());
  auto predicate = constraint(input);
  if (!predicate.ok())
    return Result<OperationMetadata>(predicate.status());
  auto valid =
      input_internal::validate_port_metadata(predicate.value(), metadata);
  if (!valid.ok())
    return Result<OperationMetadata>(valid);
  return Result<OperationMetadata>(std::move(metadata));
}
// Views borrow immutable strings/payloads from owned C++ metadata. Container
// capacities are admitted before allocation whenever a runtime root is active.
MetadataView::MetadataView(const std::vector<OperationMetadata>& source)
    : MetadataView(source.data(), source.size()) {}
MetadataView::MetadataView(const OperationMetadata* source, size_t count) {
  storage.resize(count);
  ports.resize(count);
  for (size_t i = 0; i < count; ++i) {
    auto& b = storage[i];
    auto& p = ports[i];
    const auto& m = source[i];
    p.struct_size = sizeof(p);
    if (!m.result_schema) {
      status = invalid("C Result metadata requires a schema");
      return;
    }
    p.kind = PS_RESULT_OBJECT_V2;
    p.schema = &b.schema;
    const auto& schema = *m.result_schema;
    b.schema.struct_size = sizeof(b.schema);
    b.schema.id = schema.id.data();
    b.schema.id_size = schema.id.size();
    b.schema.version = schema.version;
    b.schema.publication = static_cast<uint32_t>(schema.publication);
    b.fields.reserve(schema.fields.size());
    for (const auto& f : schema.fields) {
      ps_result_field_spec_v2 field{};
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
    b.tensors.resize(schema.tensors.size());
    b.groups.resize(schema.tensors.size());
    b.image_facets.resize(schema.tensors.size());
    for (size_t j = 0; j < schema.tensors.size(); ++j) {
      auto& image = b.tensors[j];
      const auto& f = schema.tensors[j];
      image.struct_size = sizeof(image);
      image.key = f.key.data();
      image.key_size = f.key.size();
      image.element_type = static_cast<uint32_t>(f.descriptor.element_type);
      image.rank = f.descriptor.shape.size();
      std::copy(f.descriptor.shape.begin(), f.descriptor.shape.end(),
                image.shape);
      image.batch_rank = f.batch_axes.size();
      std::copy(f.batch_axes.begin(), f.batch_axes.end(), image.batch_shape);
      image.atomic_trailing_axes = f.atomic_trailing_axes;
      image.spatial = f.layout.spatial;
      image.height_axis = f.layout.height_axis;
      image.width_axis = f.layout.width_axis;
      image.channel_axis = f.layout.channel_axis.value_or(UINT32_MAX);
      image.storage_order = static_cast<uint32_t>(f.layout.order);
      image.row_pitch_bytes = f.layout.row_pitch_bytes;
      for (const auto& g : f.layout.groups)
        b.groups[j].push_back({sizeof(ps_result_group_v2), g.role.data(),
                               static_cast<uint32_t>(g.role.size()),
                               g.first_channel, g.channel_count});
      image.groups = b.groups[j].empty() ? nullptr : b.groups[j].data();
      image.group_count = b.groups[j].size();
      encode_facets(f.facets, b.image_facets[j]);
      image.facets =
          b.image_facets[j].empty() ? nullptr : b.image_facets[j].data();
      image.facet_count = b.image_facets[j].size();
    }
    b.schema.tensors = b.tensors.empty() ? nullptr : b.tensors.data();
    b.schema.tensor_count = b.tensors.size();
    for (const auto& d : schema.domain)
      b.domain.push_back(encode(d));
    b.schema.domain = b.domain.empty() ? nullptr : b.domain.data();
    b.schema.domain_rank = b.domain.size();
    encode_facets(schema.metadata, b.metadata);
    b.schema.metadata = b.metadata.empty() ? nullptr : b.metadata.data();
    b.schema.metadata_count = b.metadata.size();
  }
}
void apply_constraint(MetadataView& view, size_t i,
                      const OperationPortConstraint& prototype) {
  auto& p = view.ports[i];
  p.kind = static_cast<uint32_t>(prototype.kind);
  p.minimum = prototype.minimum;
  p.maximum = prototype.maximum;
  p.semantic_kind = prototype.semantic_kind;
  p.tensor_key =
      prototype.tensor_key.empty() ? nullptr : prototype.tensor_key.data();
  p.tensor_key_size = prototype.tensor_key.size();
  p.requires_semantics = prototype.requires_semantics;
  p.scalar_bounds = prototype.scalar_bounds;
  if (p.kind == PS_RESULT_OBJECT_V2 &&
      input_internal::tensor_member_predicate(prototype)) {
    p.element_type = prototype.element_type;
    p.rank = prototype.rank;
    p.element_type_mask = prototype.element_type_mask;
    auto& facets = view.storage[i].facets;
    facets.clear();
    MetadataView::encode_facets(prototype.facets, facets);
    p.facets = facets.empty() ? nullptr : facets.data();
    p.facet_count = facets.size();
  }
}
void apply_constraint_view(MetadataView& view, const OperationTraits& traits) {
  for (size_t i = 0; i < view.ports.size(); ++i) {
    const auto prototype = traits.repeated_maximum &&
                                   !traits.repeated_resolved &&
                                   i >= traits.input_count
                               ? traits.input_count
                               : i;
    apply_constraint(view, i, traits.input_schema.at(prototype));
  }
}
uint32_t float_bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
ResourceVector<ps_result_parameter_value_v2> parameters_view(
    const std::map<std::string, ParameterValue>& source) {
  ResourceVector<ps_result_parameter_value_v2> result;
  result.reserve(source.size());
  for (const auto& parameter : source) {
    ps_result_parameter_value_v2 p{};
    p.struct_size = sizeof(p);
    p.key = parameter.first.data();
    p.key_size = parameter.first.size();
    if (const auto* integer = std::get_if<std::int64_t>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_INT64_V2;
      p.int64_value = *integer;
    } else if (const auto* number = std::get_if<double>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_FLOAT64_V2;
      p.float64_value = *number;
    } else if (const auto* boolean = std::get_if<bool>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_BOOL_V2;
      p.bool_value = *boolean;
    } else {
      const auto& string = std::get<std::string>(parameter.second);
      p.type = PS_RESULT_PARAMETER_STRING_V2;
      p.string_value = string.data();
      p.string_size = string.size();
    }
    result.push_back(p);
  }
  return result;
}
Result<OperationPortConstraint> constraint(const ps_result_port_v2& p) {
  if (p.struct_size != sizeof(p) || p.kind != PS_RESULT_OBJECT_V2)
    return Result<OperationPortConstraint>(
        invalid("invalid Result port constraint size/kind"));
  OperationPortConstraint c;
  c.kind = OperationPortKind::Result;
  c.element_type = p.element_type;
  c.rank = p.rank;
  c.minimum = p.minimum;
  c.maximum = p.maximum;
  c.element_type_mask = p.element_type_mask;
  c.semantic_kind = p.semantic_kind;
  auto status = facets(p.facets, p.facet_count, &c.facets);
  if (!status.ok())
    return Result<OperationPortConstraint>(status);
  if (p.requires_semantics > 1 || p.scalar_bounds > 1 ||
      (p.tensor_key == nullptr) != (p.tensor_key_size == 0))
    return Result<OperationPortConstraint>(
        invalid("invalid Result member predicate"));
  if (p.tensor_key) {
    auto key = text(p.tensor_key, p.tensor_key_size);
    if (!key.ok())
      return Result<OperationPortConstraint>(key.status());
    c.tensor_key = key.take_value();
  }
  c.requires_semantics = p.requires_semantics != 0;
  c.scalar_bounds = p.scalar_bounds != 0;
  if (p.schema) {
    auto copied = schema(p.schema);
    if (!copied.ok())
      return Result<OperationPortConstraint>(copied.status());
    c.result_schema_id = std::string(copied.value().id);
    c.result_schema_version = copied.value().version;
  }
  return Result<OperationPortConstraint>(std::move(c));
}
struct MetadataSink {
  const OperationTraits& traits;
  std::vector<OperationOutputSpecialization> outputs;
  std::array<bool, 64> supplied{};
  Status failure;
  const std::thread::id owner = std::this_thread::get_id();
  static int set(void* raw, uint32_t index,
                 const ps_result_port_v2* source) noexcept {
    auto& sink = *static_cast<MetadataSink*>(raw);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || !source ||
          reinterpret_cast<std::uintptr_t>(source) %
              alignof(ps_result_port_v2) ||
          source->struct_size != sizeof(*source) ||
          index >= sink.outputs.size() || sink.supplied[index] ||
          source->kind != static_cast<uint32_t>(
                              sink.traits.outputs[index].output_schema.kind)) {
        sink.failure = invalid("invalid metadata sink output/kind");
      } else {
        auto translated = port(*source);
        if (!translated.ok()) {
          sink.failure = translated.status();
        } else {
          const auto& prototype = sink.traits.outputs[index].output_schema;
          const bool key_equal =
              source->tensor_key_size == prototype.tensor_key.size() &&
              ((prototype.tensor_key.empty() && !source->tensor_key) ||
               (source->tensor_key &&
                std::string_view(source->tensor_key, source->tensor_key_size) ==
                    prototype.tensor_key));
          if (!key_equal ||
              source->requires_semantics != prototype.requires_semantics ||
              source->scalar_bounds != prototype.scalar_bounds ||
              float_bits(source->minimum) != float_bits(prototype.minimum) ||
              float_bits(source->maximum) != float_bits(prototype.maximum) ||
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
QueryFrame::QueryFrame(const ResultProgramQuery& source,
                       const Definition& definition,
                       const ResourceBudget& resources)
    : input_views(source.inputs),
      output_view(&source.output, 1),
      requested(ResourceAllocator<ps_result_region_v2>(resources)) {
  const auto requested_count =
      source.tensor_outputs ? source.tensor_outputs->boxes().size() : 0;
  if (requested_count > 65536 || source.parameters.size() > 128) {
    status = invalid("unbounded C Result query");
    return;
  }
  const auto* samples =
      source.tensor_outputs ? &*source.tensor_outputs : nullptr;
  if (samples) {
    for (const auto& box : samples->boxes()) {
      ps_result_region_v2 r{};
      r.struct_size = sizeof(r);
      r.rank = box.rank();
      for (std::uint32_t i = 0; i < r.rank; ++i) {
        r.offset[i] = box.dimensions()[i].offset;
        r.extent[i] = box.dimensions()[i].extent;
      }
      requested.push_back(r);
    }
  }
  query.struct_size = sizeof(query);
  query.output_index = source.output_index;
  query.tensor_slot = source.tensor_slot;
  query.requested_kind = source.tensor_outputs ? 2 : 0;
  if (!input_views.status.ok()) {
    status = input_views.status;
    return;
  }
  if (!output_view.status.ok()) {
    status = output_view.status;
    return;
  }
  apply_constraint_view(input_views, definition.traits);
  const auto& output_trait = definition.traits.outputs[query.output_index];
  apply_constraint(output_view, 0, output_trait.output_schema);
  resolved_output.struct_size = sizeof(resolved_output);
  resolved_output.key = output_trait.key.data();
  resolved_output.key_size = output_trait.key.size();
  resolved_output.input_count = output_trait.input_indices
                                    ? output_trait.input_indices->size()
                                    : UINT32_MAX;
  resolved_output.input_indices =
      output_trait.input_indices && !output_trait.input_indices->empty()
          ? output_trait.input_indices->data()
          : nullptr;
  resolved_output.execution =
      output_trait.region_rule == OperationRegionRule::Whole
          ? PS_RESULT_WHOLE_V2
          : PS_RESULT_REGIONAL_V2;
  resolved_output.observation_kind =
      static_cast<std::uint32_t>(output_trait.observation_kind);
  resolved_output.failure_delivery =
      static_cast<std::uint32_t>(output_trait.failure_delivery);
  resolved_output.flags =
      (output_trait.preserve_output_views ? PS_RESULT_OUTPUT_PRESERVE_VIEWS_V2
                                          : 0U) |
      (output_trait.requires_input_views
           ? PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V2
           : 0U) |
      (output_trait.maximum_output_payload_bytes
           ? PS_RESULT_OUTPUT_PAYLOAD_BOUND_V2
           : 0U);
  resolved_output.maximum_output_payload_bytes =
      output_trait.maximum_output_payload_bytes.value_or(0);
  resolved_output.port = output_view.ports[0];
  query.backend = static_cast<std::uint32_t>(source.backend);
  query.inputs = input_views.ports.empty() ? nullptr : input_views.ports.data();
  query.input_count = input_views.ports.size();
  query.output = &resolved_output;
  query.requested = requested.empty() ? nullptr : requested.data();
  query.requested_count = requested.size();
  query.tile_height = source.tile_height;
  query.tile_width = source.tile_width;
  parameters = parameters_view(source.parameters);
  query.parameters = parameters.empty() ? nullptr : parameters.data();
  query.parameter_count = parameters.size();
}
// The sink and its service table are one-call borrows and never escape.
Result<OperationPreparation> prepare_metadata(
    const Definition& owner, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  MetadataView view(inputs);
  if (!view.status.ok())
    return Result<OperationPreparation>(view.status);
  apply_constraint_view(view, owner.traits);
  auto values = parameters_view(parameters);
  MetadataView prototypes(owner.output_prototypes);
  if (!prototypes.status.ok())
    return Result<OperationPreparation>(prototypes.status);
  for (size_t i = 0; i < prototypes.ports.size(); ++i)
    apply_constraint(prototypes, i, owner.traits.outputs[i].output_schema);
  MetadataSink sink{
      owner.traits,
      std::vector<OperationOutputSpecialization>(owner.api.output_count),
      {},
      Status::success()};
  ps_result_metadata_sink_v2 service{sizeof(service), &sink, MetadataSink::set};
  auto status = outcome(owner.api.resolve_metadata(
      owner.api.user_data, view.ports.empty() ? nullptr : view.ports.data(),
      view.ports.size(), values.empty() ? nullptr : values.data(),
      values.size(), prototypes.ports.data(), prototypes.ports.size(),
      &service));
  if (!sink.failure.ok())
    return Result<OperationPreparation>(sink.failure);
  if (!status.ok())
    return Result<OperationPreparation>(status);
  for (uint32_t i = 0; i < owner.api.output_count; ++i)
    if (!sink.supplied[i])
      return Result<OperationPreparation>(
          invalid("metadata resolver omitted output"));
  OperationPreparation prepared;
  prepared.outputs = std::move(sink.outputs);
  return Result<OperationPreparation>(std::move(prepared));
}
}  // namespace ps::plugin_internal::result_c
