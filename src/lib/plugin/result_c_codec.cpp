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

#include "photospider/core/checked_math.hpp"
#include "photospider/core/utf8_validation.hpp"
#include "plugin/operation_exception.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::result_c {
Status outcome(int code) {
  switch (code) {
    case PS_RESULT_STATUS_OK_V4:
      return Status::success();
    case PS_RESULT_STATUS_CANCELLED_V4:
      return {ErrorCode::Cancelled, {}};
    case PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V4:
      return {ErrorCode::BackendUnavailable, {}};
    case PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V4:
      return {ErrorCode::ResourceExhausted, {}};
    case PS_RESULT_STATUS_TYPE_MISMATCH_V4:
      return {ErrorCode::TypeMismatch, {}};
    case PS_RESULT_STATUS_INVALID_ARGUMENT_V4:
      return {ErrorCode::InvalidArgument, {}};
    default:
      return {ErrorCode::OperationFailed, {}};
  }
}
int code(const Status& status) {
  return status.ok()                           ? PS_RESULT_STATUS_OK_V4
         : status.code == ErrorCode::Cancelled ? PS_RESULT_STATUS_CANCELLED_V4
         : status.code == ErrorCode::BackendUnavailable
             ? PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V4
         : status.code == ErrorCode::ResourceExhausted
             ? PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V4
         : status.code == ErrorCode::TypeMismatch
             ? PS_RESULT_STATUS_TYPE_MISMATCH_V4
         : status.code == ErrorCode::InvalidArgument ||
                 status.code == ErrorCode::Stale
             ? PS_RESULT_STATUS_INVALID_ARGUMENT_V4
             : PS_RESULT_STATUS_FAILURE_V4;
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
ResultExtent extent(const ps_result_extent_v4& source) {
  return {static_cast<ResultExtentKind>(source.kind),
          source.value,
          source.input,
          source.axis,
          source.field,
          source.divisor,
          source.offset};
}
Result<SchemaTemplate> schema(const ps_result_schema_v4* source) {
  if (!source ||
      reinterpret_cast<std::uintptr_t>(source) % alignof(ps_result_schema_v4) ||
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
        image.rank > 8 ||
        !core_internal::can_add(image.rank, image.batch_rank, 8) ||
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
    target.tensors.push_back(std::move(copied));
  }
  for (std::uint32_t i = 0; i < source->domain_rank; ++i)
    target.domain.push_back(extent(source->domain[i]));
  auto checked = target.validate();
  return checked.ok() ? Result<SchemaTemplate>(std::move(target))
                      : Result<SchemaTemplate>(checked);
}
Result<OperationPortConstraint> constraint(const ps_result_port_v4& p);
Result<OperationMetadata> port(const ps_result_port_v4& input) {
  if (input.struct_size != sizeof(input))
    return Result<OperationMetadata>(invalid("invalid Result port size"));
  OperationMetadata metadata;
  if (input.kind != PS_RESULT_OBJECT_V4)
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
    p.kind = PS_RESULT_OBJECT_V4;
    p.schema = &b.schema;
    const auto& schema = *m.result_schema;
    b.schema.struct_size = sizeof(b.schema);
    b.schema.id = schema.id.data();
    b.schema.id_size = schema.id.size();
    b.schema.version = schema.version;
    b.schema.publication = static_cast<uint32_t>(schema.publication);
    b.fields.reserve(schema.fields.size());
    for (const auto& f : schema.fields) {
      ps_result_field_spec_v4 field{};
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
        b.groups[j].push_back({sizeof(ps_result_group_v4), g.role.data(),
                               static_cast<uint32_t>(g.role.size()),
                               g.first_channel, g.channel_count});
      image.groups = b.groups[j].empty() ? nullptr : b.groups[j].data();
      image.group_count = b.groups[j].size();
    }
    b.schema.tensors = b.tensors.empty() ? nullptr : b.tensors.data();
    b.schema.tensor_count = b.tensors.size();
    for (const auto& d : schema.domain)
      b.domain.push_back(encode(d));
    b.schema.domain = b.domain.empty() ? nullptr : b.domain.data();
    b.schema.domain_rank = b.domain.size();
  }
}
void apply_constraint(MetadataView& view, size_t i,
                      const OperationPortConstraint& prototype) {
  auto& p = view.ports[i];
  p.kind = static_cast<uint32_t>(prototype.kind);
  p.minimum = prototype.minimum;
  p.maximum = prototype.maximum;
  p.tensor_key =
      prototype.tensor_key.empty() ? nullptr : prototype.tensor_key.data();
  p.tensor_key_size = prototype.tensor_key.size();
  p.scalar_bounds = prototype.scalar_bounds;
  auto& validators = view.storage[i].validators;
  for (const auto& id : prototype.validators)
    validators.push_back({sizeof(ps_result_extension_identity_v4), id.version,
                          id.key.data(), static_cast<uint32_t>(id.key.size()),
                          id.implementation.data(),
                          static_cast<uint32_t>(id.implementation.size())});
  p.validators = validators.empty() ? nullptr : validators.data();
  p.validator_count = validators.size();
  if (p.kind == PS_RESULT_OBJECT_V4 &&
      input_internal::tensor_member_predicate(prototype)) {
    p.element_type = prototype.element_type;
    p.rank = prototype.rank;
    p.element_type_mask = prototype.element_type_mask;
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
ResourceVector<ps_result_parameter_value_v4> parameters_view(
    const std::map<std::string, ParameterValue>& source) {
  ResourceVector<ps_result_parameter_value_v4> result;
  result.reserve(source.size());
  for (const auto& parameter : source) {
    ps_result_parameter_value_v4 p{};
    p.struct_size = sizeof(p);
    p.key = parameter.first.data();
    p.key_size = parameter.first.size();
    if (const auto* integer = std::get_if<std::int64_t>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_INT64_V4;
      p.int64_value = *integer;
    } else if (const auto* number = std::get_if<double>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_FLOAT64_V4;
      p.float64_value = *number;
    } else if (const auto* boolean = std::get_if<bool>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_BOOL_V4;
      p.bool_value = *boolean;
    } else {
      const auto& string = std::get<std::string>(parameter.second);
      p.type = PS_RESULT_PARAMETER_STRING_V4;
      p.string_value = string.data();
      p.string_size = string.size();
    }
    result.push_back(p);
  }
  return result;
}
Result<OperationPortConstraint> constraint(const ps_result_port_v4& p) {
  if (p.struct_size != sizeof(p) || p.kind != PS_RESULT_OBJECT_V4)
    return Result<OperationPortConstraint>(
        invalid("invalid Result port constraint size/kind"));
  OperationPortConstraint c;
  c.kind = OperationPortKind::Result;
  auto ids = extension_ids(p.validators, p.validator_count);
  if (!ids.ok())
    return Result<OperationPortConstraint>(ids.status());
  c.validators = ids.take_value();
  c.element_type = p.element_type;
  c.rank = p.rank;
  c.minimum = p.minimum;
  c.maximum = p.maximum;
  c.element_type_mask = p.element_type_mask;
  if (p.scalar_bounds > 1 ||
      (p.tensor_key == nullptr) != (p.tensor_key_size == 0))
    return Result<OperationPortConstraint>(
        invalid("invalid Result member predicate"));
  if (p.tensor_key) {
    auto key = text(p.tensor_key, p.tensor_key_size);
    if (!key.ok())
      return Result<OperationPortConstraint>(key.status());
    c.tensor_key = key.take_value();
  }
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
  std::vector<ResourceIdentity> resource_ids = {};
  bool resources_supplied = false;
  static int set_resources(void* raw,
                           const ps_result_resource_identity_v4* values,
                           uint32_t count) noexcept {
    auto& sink = *static_cast<MetadataSink*>(raw);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || sink.resources_supplied ||
          !array(values, count, 1024)) {
        sink.failure = invalid("invalid resource identity declaration");
      } else {
        sink.resources_supplied = true;
        for (uint32_t i = 0; i < count; ++i) {
          ResourceIdentity id;
          id.byte_length = values[i].byte_length;
          std::copy(values[i].sha256, values[i].sha256 + 32, id.sha256.begin());
          sink.resource_ids.push_back(id);
        }
      }
    } catch (...) {
      sink.failure = {ErrorCode::ResourceExhausted, {}};
    }
    return code(sink.failure);
  }
  static int set(void* raw, uint32_t index,
                 const ps_result_port_v4* source) noexcept {
    auto& sink = *static_cast<MetadataSink*>(raw);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || !source ||
          reinterpret_cast<std::uintptr_t>(source) %
              alignof(ps_result_port_v4) ||
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
          auto ids = extension_ids(source->validators, source->validator_count);
          if (!ids.ok() || ids.value() != prototype.validators || !key_equal ||
              source->scalar_bounds != prototype.scalar_bounds ||
              float_bits(source->minimum) != float_bits(prototype.minimum) ||
              float_bits(source->maximum) != float_bits(prototype.maximum) ||
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
namespace {
bool same_grid(const std::optional<TileGrid>& first,
               const std::optional<TileGrid>& second) {
  if (first.has_value() != second.has_value())
    return false;
  return !first || (first->tile_extent() == second->tile_extent() &&
                    first->rank() == second->rank() &&
                    first->batch_rank() == second->batch_rank() &&
                    first->height_axis() == second->height_axis() &&
                    first->width_axis() == second->width_axis() &&
                    first->shape() == second->shape() &&
                    first->batch_count() == second->batch_count() &&
                    first->tile_rows() == second->tile_rows() &&
                    first->tile_columns() == second->tile_columns() &&
                    first->tile_count() == second->tile_count());
}
ps_result_tile_grid_v4 encode_grid(const std::optional<TileGrid>& grid) {
  ps_result_tile_grid_v4 view{};
  view.struct_size = sizeof(view);
  if (grid) {
    const auto& value = *grid;
    view.present = 1;
    view.rank = value.rank();
    view.batch_rank = value.batch_rank();
    view.height_axis = value.height_axis();
    view.width_axis = value.width_axis();
    view.tile_extent = value.tile_extent();
    view.tile_rows = value.tile_rows();
    view.tile_columns = value.tile_columns();
    view.tile_count = value.tile_count();
    std::copy(value.shape().begin(), value.shape().end(), view.shape);
  }
  return view;
}
}  // namespace
QueryFrame::QueryFrame(const ResultProgramQuery& source,
                       const Definition& definition,
                       const ResourceBudget& resources)
    : input_views(source.inputs),
      output_view(&source.output, 1),
      input_tile_grids(ResourceAllocator<ps_result_tile_grid_v4>(resources)) {
  if (source.parameters.size() > 128 ||
      source.output_index >= definition.traits.outputs.size() ||
      !source.output.result_schema) {
    status = invalid("invalid C Result query");
    return;
  }
  query.struct_size = sizeof(query);
  query.output_index = source.output_index;
  query.tile_unit = source.tile_unit;
  query.tile_extent = source.tile_extent;
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
          ? PS_RESULT_WHOLE_V4
          : PS_RESULT_REGIONAL_V4;
  resolved_output.failure_delivery =
      static_cast<std::uint32_t>(output_trait.failure_delivery);
  resolved_output.flags =
      (output_trait.preserve_output_views ? PS_RESULT_OUTPUT_PRESERVE_VIEWS_V4
                                          : 0U) |
      (output_trait.requires_input_views
           ? PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V4
           : 0U) |
      (output_trait.maximum_output_payload_bytes
           ? PS_RESULT_OUTPUT_PAYLOAD_BOUND_V4
           : 0U);
  resolved_output.maximum_output_payload_bytes =
      output_trait.maximum_output_payload_bytes.value_or(0);
  resolved_output.port = output_view.ports[0];
  query.backend = static_cast<std::uint32_t>(source.backend);
  query.inputs = input_views.ports.empty() ? nullptr : input_views.ports.data();
  query.input_count = input_views.ports.size();
  if (!source.input_tile_grids.empty() &&
      source.input_tile_grids.size() != source.inputs.size()) {
    status = invalid("incomplete compiled C input grids");
    return;
  }
  input_tile_grids.reserve(source.inputs.size());
  for (std::size_t i = 0; i < source.inputs.size(); ++i) {
    if (!source.inputs[i].result_schema) {
      status = invalid("C input grid has no schema");
      return;
    }
    auto grid = TileGrid::from_schema(*source.inputs[i].result_schema,
                                      source.tile_extent);
    if (!grid.ok()) {
      status = grid.status();
      return;
    }
    if (!source.input_tile_grids.empty() &&
        !same_grid(source.input_tile_grids[i], grid.value())) {
      status = invalid("C input grid differs from resolved schema");
      return;
    }
    const auto view = encode_grid(grid.value());
    input_tile_grids.push_back(view);
  }
  query.input_tile_grids =
      input_tile_grids.empty() ? nullptr : input_tile_grids.data();
  query.output = &resolved_output;
  auto grid =
      TileGrid::from_schema(*source.output.result_schema, source.tile_extent);
  if (!grid.ok()) {
    status = grid.status();
    return;
  }
  if (source.output_tile_grid &&
      !same_grid(source.output_tile_grid, grid.value())) {
    status = invalid("C output grid differs from resolved schema");
    return;
  }
  output_tile_grid = encode_grid(grid.value());
  query.output_tile_grid = &output_tile_grid;
  query.tiled = output_trait.region_rule == OperationRegionRule::Tiled &&
                grid.value().has_value();
  auto selection =
      source.output_request.resolve(grid.value(), query.tiled != 0);
  if (!selection.ok()) {
    status = selection.status();
    return;
  }
  const auto& selected = selection.value();
  if ((query.tiled && selected.whole) ||
      (query.tiled && !selected.tiles.empty() &&
       (selected.tiles.size() != 1 ||
        !selected.tiles.contains(source.tile_unit))) ||
      (!query.tiled && source.tile_unit != 0)) {
    status = invalid("C query must name one computation unit");
    return;
  }
  auto copied = selected.tiles.copy_to(resources);
  if (!copied.ok()) {
    status = copied.status();
    return;
  }
  requested = copied.take_value();
  query.requested = {
      sizeof(ps_result_tile_request_v4), selected.whole ? 1U : 0U,
      requested.empty() ? nullptr : requested.begin(), requested.size()};
  parameters = parameters_view(source.parameters);
  query.parameters = parameters.empty() ? nullptr : parameters.data();
  query.parameter_count = parameters.size();
  this->resources = resource_views(source.resources);
  query.resources = this->resources.empty() ? nullptr : this->resources.data();
  query.resource_count = this->resources.size();
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
  ps_result_metadata_sink_v4 service{sizeof(service), &sink, MetadataSink::set,
                                     MetadataSink::set_resources};
  Status status;
  try {
    status = outcome(owner.api.resolve_metadata(
        owner.api.user_data, view.ports.empty() ? nullptr : view.ports.data(),
        view.ports.size(), values.empty() ? nullptr : values.data(),
        values.size(), prototypes.ports.data(), prototypes.ports.size(),
        &service));
  } catch (...) {
    status = current_operation_exception();
  }
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
  prepared.resources = std::move(sink.resource_ids);
  return Result<OperationPreparation>(std::move(prepared));
}
}  // namespace ps::plugin_internal::result_c
