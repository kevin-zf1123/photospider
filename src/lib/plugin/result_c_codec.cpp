#include "plugin/result_c_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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
#include "plugin/result_c_member.hpp"

namespace ps::plugin_internal::result_c {
Status outcome(int code) {
  switch (code) {
    case PS_RESULT_STATUS_OK_V7:
      return Status::success();
    case PS_RESULT_STATUS_CANCELLED_V7:
      return {ErrorCode::Cancelled, {}};
    case PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V7:
      return {ErrorCode::BackendUnavailable, {}};
    case PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V7:
      return {ErrorCode::ResourceExhausted, {}};
    case PS_RESULT_STATUS_TYPE_MISMATCH_V7:
      return {ErrorCode::TypeMismatch, {}};
    case PS_RESULT_STATUS_INVALID_ARGUMENT_V7:
      return {ErrorCode::InvalidArgument, {}};
    default:
      return {ErrorCode::OperationFailed, {}};
  }
}
int code(const Status& status) {
  return status.ok()                           ? PS_RESULT_STATUS_OK_V7
         : status.code == ErrorCode::Cancelled ? PS_RESULT_STATUS_CANCELLED_V7
         : status.code == ErrorCode::BackendUnavailable
             ? PS_RESULT_STATUS_BACKEND_UNAVAILABLE_V7
         : status.code == ErrorCode::ResourceExhausted
             ? PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V7
         : status.code == ErrorCode::TypeMismatch
             ? PS_RESULT_STATUS_TYPE_MISMATCH_V7
         : status.code == ErrorCode::InvalidArgument ||
                 status.code == ErrorCode::Stale
             ? PS_RESULT_STATUS_INVALID_ARGUMENT_V7
             : PS_RESULT_STATUS_FAILURE_V7;
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
Result<SchemaTemplate> schema(const ps_result_schema_v7* source) {
  if (!source ||
      reinterpret_cast<std::uintptr_t>(source) % alignof(ps_result_schema_v7) ||
      source->struct_size != sizeof(*source) ||
      !array(source->tensors, source->tensor_count, 16) ||
      !array(source->publication_scopes, source->publication_scope_count, 16))
    return Result<SchemaTemplate>(invalid("invalid Result schema table"));
  auto id = text(source->id, source->id_size);
  if (!id.ok())
    return Result<SchemaTemplate>(id.status());
  SchemaTemplate target;
  target.id = id.take_value();
  target.version = source->version;

  for (std::uint32_t i = 0; i < source->tensor_count; ++i) {
    const auto& image = source->tensors[i];
    if (image.struct_size != sizeof(image) || image.rank < 1 ||
        image.rank > 8 ||
        !core_internal::can_add(image.rank, image.batch_rank, 8) ||
        image.spatial > 1)
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
    copied.layout.height_axis = image.height_axis;
    copied.layout.width_axis = image.width_axis;
    copied.layout.channel_axis = image.channel_axis == UINT32_MAX
                                     ? std::optional<std::uint32_t>{}
                                     : image.channel_axis;

    target.tensors.push_back(std::move(copied));
  }
  for (std::uint32_t i = 0; i < source->publication_scope_count; ++i) {
    const auto& scope = source->publication_scopes[i];
    if (scope.struct_size != sizeof(scope) ||
        !array(scope.members, scope.member_count, 16) || !scope.member_count)
      return Result<SchemaTemplate>(invalid("invalid publication scope"));
    ResultPublicationScope copied;
    copied.policy = static_cast<ResultPublicationPolicy>(scope.policy);
    copied.members.assign(scope.members, scope.members + scope.member_count);
    target.publication_scopes.push_back(std::move(copied));
  }
  auto checked = target.validate();
  return checked.ok() ? Result<SchemaTemplate>(std::move(target))
                      : Result<SchemaTemplate>(checked);
}
Result<OperationPortConstraint> constraint(const ps_result_port_v7& p);
Result<OperationMetadata> port(const ps_result_port_v7& input) {
  if (input.struct_size != sizeof(input))
    return Result<OperationMetadata>(invalid("invalid Result port size"));
  OperationMetadata metadata;
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
  for (size_t i = 0; i < count && status.ok(); ++i)
    initialize(i, source[i].result_schema.get());
}
MetadataView::MetadataView(const std::vector<OperationSharedStage>& stages) {
  storage.resize(stages.size());
  ports.resize(stages.size());
  for (size_t i = 0; i < stages.size() && status.ok(); ++i)
    initialize(i, &stages[i].result_schema);
}
void MetadataView::initialize(size_t i, const SchemaTemplate* source) {
  auto& b = storage[i];
  auto& p = ports[i];
  p.struct_size = sizeof(p);
  if (!source) {
    status = invalid("C Result metadata requires a schema");
    return;
  }
  p.schema = &b.schema;
  const auto& schema = *source;
  b.schema.struct_size = sizeof(b.schema);
  b.schema.id = schema.id.data();
  b.schema.id_size = schema.id.size();
  b.schema.version = schema.version;
  b.publication_scopes.resize(schema.publication_scopes.size());
  for (std::size_t j = 0; j < schema.publication_scopes.size(); ++j) {
    const auto& scope = schema.publication_scopes[j];
    b.publication_scopes[j] = {
        sizeof(ps_result_publication_scope_v7),
        static_cast<std::uint32_t>(scope.policy), scope.members.data(),
        static_cast<std::uint32_t>(scope.members.size())};
  }
  b.schema.publication_scopes =
      b.publication_scopes.empty() ? nullptr : b.publication_scopes.data();
  b.schema.publication_scope_count = b.publication_scopes.size();
  b.tensors.resize(schema.tensors.size());
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
  }
  b.schema.tensors = b.tensors.empty() ? nullptr : b.tensors.data();
  b.schema.tensor_count = b.tensors.size();
}

void apply_validators(MetadataView& view, size_t i,
                      const std::vector<ExtensionIdentity>& source) {
  auto& p = view.ports[i];
  auto& validators = view.storage[i].validators;
  for (const auto& id : source)
    validators.push_back({sizeof(ps_result_extension_identity_v7), id.version,
                          id.key.data(), static_cast<uint32_t>(id.key.size()),
                          id.implementation.data(),
                          static_cast<uint32_t>(id.implementation.size())});
  p.validators = validators.empty() ? nullptr : validators.data();
  p.validator_count = validators.size();
}
void apply_constraint(MetadataView& view, size_t i,
                      const OperationPortConstraint& prototype) {
  auto& p = view.ports[i];
  p.tensor_key =
      prototype.tensor_key.empty() ? nullptr : prototype.tensor_key.data();
  p.tensor_key_size = prototype.tensor_key.size();
  apply_validators(view, i, prototype.validators);
  if (input_internal::tensor_member_predicate(prototype)) {
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
ResourceVector<ps_result_parameter_value_v7> parameters_view(
    const std::map<std::string, ParameterValue>& source) {
  ResourceVector<ps_result_parameter_value_v7> result;
  result.reserve(source.size());
  for (const auto& parameter : source) {
    ps_result_parameter_value_v7 p{};
    p.struct_size = sizeof(p);
    p.key = parameter.first.data();
    p.key_size = parameter.first.size();
    if (const auto* integer = std::get_if<std::int64_t>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_INT64_V7;
      p.int64_value = *integer;
    } else if (const auto* number = std::get_if<double>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_FLOAT64_V7;
      p.float64_value = *number;
    } else if (const auto* boolean = std::get_if<bool>(&parameter.second)) {
      p.type = PS_RESULT_PARAMETER_BOOL_V7;
      p.bool_value = *boolean;
    } else {
      const auto& string = std::get<std::string>(parameter.second);
      p.type = PS_RESULT_PARAMETER_STRING_V7;
      p.string_value = string.data();
      p.string_size = string.size();
    }
    result.push_back(p);
  }
  return result;
}
Result<OperationPortConstraint> constraint(const ps_result_port_v7& p) {
  if (p.struct_size != sizeof(p))
    return Result<OperationPortConstraint>(
        invalid("invalid Result port constraint size/kind"));
  OperationPortConstraint c;

  auto ids = extension_ids(p.validators, p.validator_count);
  if (!ids.ok())
    return Result<OperationPortConstraint>(ids.status());
  c.validators = ids.take_value();
  c.element_type = p.element_type;
  c.rank = p.rank;
  c.element_type_mask = p.element_type_mask;
  if ((p.tensor_key == nullptr) != (p.tensor_key_size == 0))
    return Result<OperationPortConstraint>(
        invalid("invalid Result member predicate"));
  if (p.tensor_key) {
    auto key = text(p.tensor_key, p.tensor_key_size);
    if (!key.ok())
      return Result<OperationPortConstraint>(key.status());
    c.tensor_key = key.take_value();
  }
  if (p.schema) {
    auto copied = schema(p.schema);
    if (!copied.ok())
      return Result<OperationPortConstraint>(copied.status());
    c.result_schema_id = std::string(copied.value().id);
    c.result_schema_version = copied.value().version;
  }
  return Result<OperationPortConstraint>(std::move(c));
}
Result<OperationSharedStage> shared_stage(
    const ps_result_shared_stage_v7& source) {
  using Answer = Result<OperationSharedStage>;
  if (source.struct_size != sizeof(source) || source.mandatory > 1 ||
      source.state_bytes > 1048576 ||
      !array(source.input_indices, source.input_count, 1024))
    return Answer(invalid("invalid shared stage declaration"));
  auto name = text(source.key, source.key_size);
  auto result_schema = schema(source.schema);
  if (!name.ok() || !result_schema.ok())
    return Answer(!name.ok() ? name.status() : result_schema.status());
  auto ids = extension_ids(source.validators, source.validator_count);
  if (!ids.ok())
    return Answer(ids.status());
  OperationSharedStage result;
  result.validators = ids.take_value();
  result.key = name.take_value();
  result.result_schema = result_schema.take_value();
  if (source.input_count)
    result.input_indices.assign(source.input_indices,
                                source.input_indices + source.input_count);
  result.backend = static_cast<Backend>(source.backend);
  result.mandatory = source.mandatory != 0;
  result.continuation_bytes =
      c_member_storage_bytes() + std::max<std::uint64_t>(1, source.state_bytes);
  result.workspace_bytes = source.workspace_bytes;
  result.maximum_work = source.maximum_work;
  result.maximum_output_payload_bytes = source.maximum_output_payload_bytes;
  result.maximum_dependency_stages = source.maximum_stages;
  return Answer(std::move(result));
}
struct MetadataSink {
  const OperationTraits& traits;
  std::vector<OperationOutputSpecialization> outputs;
  std::array<bool, 64> supplied{};
  Status failure;
  const std::thread::id owner = std::this_thread::get_id();
  std::vector<ResourceIdentity> resource_ids = {};
  bool resources_supplied = false;
  std::optional<std::vector<OperationSharedStage>> stages = {};
  static int set_shared_stages(void* raw,
                               const ps_result_shared_stage_v7* values,
                               uint32_t count) noexcept {
    auto& sink = *static_cast<MetadataSink*>(raw);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || sink.stages ||
          !array(values, count, 64)) {
        sink.failure = invalid("invalid shared stage preparation");
      } else {
        sink.stages.emplace();
        for (uint32_t i = 0; i < count; ++i) {
          auto stage = shared_stage(values[i]);
          if (!stage.ok()) {
            sink.failure = stage.status();
            break;
          }
          sink.stages->push_back(stage.take_value());
        }
      }
    } catch (...) {
      sink.failure = {ErrorCode::ResourceExhausted, {}};
    }
    return code(sink.failure);
  }
  static int set_resources(void* raw,
                           const ps_result_resource_identity_v7* values,
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
                 const ps_result_port_v7* source) noexcept {
    auto& sink = *static_cast<MetadataSink*>(raw);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || !source ||
          reinterpret_cast<std::uintptr_t>(source) %
              alignof(ps_result_port_v7) ||
          source->struct_size != sizeof(*source) ||
          index >= sink.outputs.size() || sink.supplied[index]) {
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
bool same_grid(const TileGrid& a, const TileGrid& b) {
  return a.tile_extent() == b.tile_extent() && a.shape() == b.shape() &&
         a.rank() == b.rank() && a.batch_rank() == b.batch_rank() &&
         a.batch_axis_mask() == b.batch_axis_mask() &&
         a.height_axis() == b.height_axis() && a.width_axis() == b.width_axis();
}
bool same_grids(const std::vector<TileGrid>& a,
                const std::vector<TileGrid>& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), same_grid);
}
ps_result_tile_grid_v7 encode_grid(const TileGrid& value) {
  ps_result_tile_grid_v7 view{};
  view.struct_size = sizeof(view);
  view.rank = value.rank();
  view.batch_rank = value.batch_rank();
  view.batch_axis_mask = value.batch_axis_mask();
  view.height_axis = value.height_axis();
  view.width_axis = value.width_axis();
  view.tile_extent = value.tile_extent();
  view.tile_rows = value.tile_rows();
  view.tile_columns = value.tile_columns();
  view.tile_count = value.tile_count();
  std::copy(value.shape().begin(), value.shape().end(), view.shape);
  return view;
}
}  // namespace
namespace {
struct StaticQueryFrame {
  MetadataView inputs, output, stages;
  ResourceVector<ps_result_shared_stage_v7> stage_contracts;
  ResourceVector<ps_result_parameter_value_v7> parameters;
  ResourceVector<ps_result_resource_identity_v7> resources;
  ResourceVector<ResourceVector<ps_result_tile_grid_v7>> input_members;
  ResourceVector<ps_result_tile_grids_v7> input_grids;
  ResourceVector<ps_result_tile_grid_v7> output_members;
  ps_result_tile_grids_v7 output_grids{};
  ps_result_output_v7 selected{};
  ResourceVector<ResourceVector<ps_result_tile_interval_v7>> ranges;
  ResourceVector<ps_result_tile_selection_v7> publication;
  ps_result_static_query_v7 query{};
  Status status;
  explicit StaticQueryFrame(const ResultStaticQuery& source)
      : inputs(source.metadata.inputs),
        output(&source.metadata.output, 1),
        stages(source.prepared.traits().shared_stages) {
    if (!inputs.status.ok() || !output.status.ok() || !stages.status.ok()) {
      status = !inputs.status.ok()   ? inputs.status
               : !output.status.ok() ? output.status
                                     : stages.status;
      return;
    }
    const auto& traits = source.prepared.traits();
    apply_constraint_view(inputs, traits);
    selected.struct_size = sizeof(selected);
    if (source.shared_stage == UINT32_MAX) {
      const auto& declared = traits.outputs.at(source.output_index);
      apply_constraint(output, 0, declared.output_schema);
      selected.key = declared.key.data();
      selected.key_size = declared.key.size();
      selected.input_indices =
          declared.input_indices && !declared.input_indices->empty()
              ? declared.input_indices->data()
              : nullptr;
      selected.input_count =
          declared.input_indices ? declared.input_indices->size() : UINT32_MAX;
      selected.flags = declared.maximum_output_payload_bytes
                           ? PS_RESULT_OUTPUT_PAYLOAD_BOUND_V7
                           : 0;
      selected.maximum_output_payload_bytes =
          declared.maximum_output_payload_bytes.value_or(0);
    } else {
      const auto& declared = traits.shared_stages.at(source.shared_stage);
      apply_validators(output, 0, declared.validators);
      selected.key = declared.key.data();
      selected.key_size = declared.key.size();
      selected.input_indices = declared.input_indices.empty()
                                   ? nullptr
                                   : declared.input_indices.data();
      selected.input_count = declared.input_indices.size();
      selected.flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V7;
      selected.maximum_output_payload_bytes =
          declared.maximum_output_payload_bytes;
    }
    selected.port = output.ports[0];
    for (std::size_t i = 0; i < traits.shared_stages.size(); ++i) {
      const auto& stage = traits.shared_stages[i];
      apply_validators(stages, i, stage.validators);
      stage_contracts.push_back(
          {sizeof(ps_result_shared_stage_v7), stage.key.data(),
           static_cast<std::uint32_t>(stage.key.size()), stages.ports[i].schema,
           stage.input_indices.empty() ? nullptr : stage.input_indices.data(),
           static_cast<std::uint32_t>(stage.input_indices.size()),
           static_cast<std::uint32_t>(stage.backend), stage.mandatory ? 1U : 0U,
           stage.continuation_bytes - c_member_storage_bytes(),
           stage.workspace_bytes, stage.maximum_work,
           stage.maximum_output_payload_bytes, stage.maximum_dependency_stages,
           stages.ports[i].validators, stages.ports[i].validator_count});
    }
    input_members.resize(source.metadata.input_tile_grids.size());
    input_grids.resize(input_members.size());
    for (std::size_t i = 0; i < input_members.size(); ++i) {
      for (const auto& grid : source.metadata.input_tile_grids[i])
        input_members[i].push_back(encode_grid(grid));
      input_grids[i] = {
          sizeof(ps_result_tile_grids_v7),
          static_cast<std::uint32_t>(input_members[i].size()),
          input_members[i].empty() ? nullptr : input_members[i].data()};
    }
    for (const auto& grid : source.metadata.output_tile_grids)
      output_members.push_back(encode_grid(grid));
    output_grids = {sizeof(output_grids),
                    static_cast<std::uint32_t>(output_members.size()),
                    output_members.empty() ? nullptr : output_members.data()};
    ranges.resize(source.publication.members.size());
    std::size_t member_index = 0;
    for (const auto& member : source.publication.members) {
      auto& intervals = ranges[member_index++];
      for (const auto& range : member.second.intervals())
        intervals.push_back({range.begin, range.end});
      publication.push_back({sizeof(ps_result_tile_selection_v7), member.first,
                             intervals.empty() ? nullptr : intervals.data(),
                             intervals.size()});
    }
    parameters = parameters_view(source.parameters);
    for (const auto& identity : source.prepared.resource_ids()) {
      ps_result_resource_identity_v7 record{};
      record.byte_length = identity.byte_length;
      std::copy(identity.sha256.begin(), identity.sha256.end(), record.sha256);
      resources.push_back(record);
    }
    query.struct_size = sizeof(query);
    query.output_index = source.output_index;
    query.backend = static_cast<std::uint32_t>(source.backend);
    query.tensor_slot = source.tensor_slot;
    query.shared_stage = source.shared_stage;
    query.tile_unit = source.tile_unit;
    query.tile_extent = source.tile_extent;
    query.inputs = inputs.ports.empty() ? nullptr : inputs.ports.data();
    query.input_count = inputs.ports.size();
    query.input_tile_grids = input_grids.empty() ? nullptr : input_grids.data();
    query.output = &selected;
    query.output_tile_grids = &output_grids;
    query.shared_stages =
        stage_contracts.empty() ? nullptr : stage_contracts.data();
    query.shared_stage_count = stage_contracts.size();
    query.parameters = parameters.empty() ? nullptr : parameters.data();
    query.parameter_count = parameters.size();
    query.publication = publication.empty() ? nullptr : publication.data();
    query.publication_count = publication.size();
    query.resource_ids = resources.empty() ? nullptr : resources.data();
    query.resource_count = resources.size();
  }
};
struct StaticMappingSink {
  explicit StaticMappingSink(const ResultStaticQuery& source)
      : source(source) {}
  const ResultStaticQuery& source;
  ResultStaticMapping mapping;
  Status failure;
  std::mutex mutex;
  const std::thread::id owner = std::this_thread::get_id();
  std::uint64_t entries = 0, intervals = 0;
  Result<TileSet> copy(const ps_result_tile_interval_v7* ranges,
                       std::uint64_t count) {
    if (!array(ranges, count, 65536 - intervals))
      return Result<TileSet>(invalid("static interval limit"));
    intervals += count;
    std::vector<TileInterval> copied;
    copied.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i)
      copied.push_back({ranges[i].begin, ranges[i].end});
    return TileSet::from_intervals(ResourceBudget{}, copied.data(),
                                   copied.size());
  }
  static int add_input(void* raw, const ps_result_tile_need_v7* read) noexcept {
    auto& sink = *static_cast<StaticMappingSink*>(raw);
    std::lock_guard<std::mutex> lock(sink.mutex);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || ++sink.entries > 65536 ||
          !array(read, 1, 1) || read->struct_size != sizeof(*read) ||
          read->descriptor_only > 1 ||
          read->input >= sink.source.metadata.inputs.size() ||
          read->tensor_slot >=
              sink.source.metadata.input_tile_grids[read->input].size() ||
          (read->descriptor_only &&
           (read->tensor_slot || read->interval_count || read->intervals))) {
        sink.failure = invalid("invalid static C input mapping");
      } else {
        auto tiles = sink.copy(read->intervals, read->interval_count);
        if (!tiles.ok())
          sink.failure = tiles.status();
        else
          sink.mapping.inputs.push_back({read->input, read->tensor_slot,
                                         read->descriptor_only != 0,
                                         tiles.take_value()});
      }
    } catch (...) {
      sink.failure = current_operation_exception();
    }
    return code(sink.failure);
  }
  static int add_shared(void* raw, std::uint32_t stage, std::uint32_t all,
                        const ps_result_tile_selection_v7* members,
                        std::uint32_t count) noexcept {
    auto& sink = *static_cast<StaticMappingSink*>(raw);
    std::lock_guard<std::mutex> lock(sink.mutex);
    if (!sink.failure.ok())
      return code(sink.failure);
    try {
      if (sink.owner != std::this_thread::get_id() || ++sink.entries > 65536 ||
          all > 1 ||
          stage >= sink.source.prepared.traits().shared_stages.size() ||
          !array(members, count, 16) || (all && count)) {
        sink.failure = invalid("invalid static C shared mapping");
      } else if (all) {
        sink.mapping.shared_stages.emplace_back(stage);
      } else {
        TileRequest request;
        for (std::uint32_t i = 0; i < count && sink.failure.ok(); ++i) {
          if (members[i].struct_size != sizeof(members[i]) ||
              members[i].tensor_slot >= sink.source.prepared.traits()
                                            .shared_stages[stage]
                                            .result_schema.tensors.size()) {
            sink.failure = invalid("invalid static C shared member");
            break;
          }
          auto tiles =
              sink.copy(members[i].intervals, members[i].interval_count);
          if (!tiles.ok()) {
            sink.failure = tiles.status();
            break;
          }
          auto& selected = request.members[members[i].tensor_slot];
          auto joined = selected.unite(tiles.value(), ResourceBudget{});
          if (!joined.ok()) {
            sink.failure = joined.status();
            break;
          }
          selected = joined.take_value();
        }
        if (sink.failure.ok())
          sink.mapping.shared_stages.emplace_back(stage, std::move(request));
      }
    } catch (...) {
      sink.failure = current_operation_exception();
    }
    return code(sink.failure);
  }
};
}  // namespace
Result<ResultStaticMapping> map_static_dependencies(
    const Definition& definition, const ResultStaticQuery& query) {
  StaticQueryFrame frame(query);
  if (!frame.status.ok())
    return Result<ResultStaticMapping>(frame.status);
  StaticMappingSink sink(query);
  ps_result_static_mapping_sink_v7 service{sizeof(service), &sink,
                                           StaticMappingSink::add_input,
                                           StaticMappingSink::add_shared};
  Status status;
  try {
    status = outcome(definition.api.map_dependencies(definition.api.user_data,
                                                     &frame.query, &service));
  } catch (...) {
    status = current_operation_exception();
  }
  std::lock_guard<std::mutex> lock(sink.mutex);
  if (!sink.failure.ok())
    return Result<ResultStaticMapping>(sink.failure);
  if (!status.ok())
    return Result<ResultStaticMapping>(status);
  return Result<ResultStaticMapping>(std::move(sink.mapping));
}
QueryFrame::QueryFrame(const ResultProgramQuery& source,
                       const Definition& definition,
                       const ResourceBudget& resources)
    : input_views(source.inputs),
      output_view(&source.output, 1),
      input_tile_grids(ResourceAllocator<ps_result_tile_grids_v7>(resources)) {
  if (source.parameters.size() > 128 ||
      source.output_index >= definition.traits.outputs.size() ||
      !source.output.result_schema) {
    status = invalid("invalid C Result query");
    return;
  }
  query.struct_size = sizeof(query);
  query.output_index = source.output_index;
  query.shared_stage = source.shared_stage;
  query.tensor_slot = source.tensor_slot;
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
  if (source.shared_stage == UINT32_MAX)
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
  resolved_output.flags = (output_trait.maximum_output_payload_bytes
                               ? PS_RESULT_OUTPUT_PAYLOAD_BOUND_V7
                               : 0U);
  resolved_output.maximum_output_payload_bytes =
      output_trait.maximum_output_payload_bytes.value_or(0);
  resolved_output.port = output_view.ports[0];
  if (source.shared_stage != UINT32_MAX) {
    if (!source.prepared ||
        source.shared_stage >= source.prepared->traits().shared_stages.size()) {
      status = invalid("unknown C shared stage");
      return;
    }
    const auto& stage =
        source.prepared->traits().shared_stages[source.shared_stage];
    resolved_output.key = stage.key.data();
    resolved_output.key_size = stage.key.size();
    resolved_output.input_count = stage.input_indices.size();
    resolved_output.input_indices =
        stage.input_indices.empty() ? nullptr : stage.input_indices.data();
    resolved_output.flags = PS_RESULT_OUTPUT_PAYLOAD_BOUND_V7;
    resolved_output.maximum_output_payload_bytes =
        stage.maximum_output_payload_bytes;
  }
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
    auto grid =
        TileGrid::members(*source.inputs[i].result_schema, source.tile_extent);
    if (!grid.ok()) {
      status = grid.status();
      return;
    }
    if (!source.input_tile_grids.empty() &&
        !same_grids(source.input_tile_grids[i], grid.value())) {
      status = invalid("C input grid differs from resolved schema");
      return;
    }
    ResourceVector<ps_result_tile_grid_v7> views{
        ResourceAllocator<ps_result_tile_grid_v7>(resources)};
    for (const auto& member : grid.value())
      views.push_back(encode_grid(member));
    input_grid_members.push_back(std::move(views));
    const auto& members = input_grid_members.back();
    input_tile_grids.push_back({sizeof(ps_result_tile_grids_v7),
                                static_cast<std::uint32_t>(members.size()),
                                members.data()});
  }
  query.input_tile_grids =
      input_tile_grids.empty() ? nullptr : input_tile_grids.data();
  query.output = &resolved_output;
  auto grids =
      TileGrid::members(*source.output.result_schema, source.tile_extent);
  if (!grids.ok()) {
    status = grids.status();
    return;
  }
  if (!source.output_tile_grids.empty() &&
      !same_grids(source.output_tile_grids, grids.value())) {
    status = invalid("C output grids differ from resolved schema");
    return;
  }
  if (source.shared_stage == UINT32_MAX &&
      (source.tensor_slot >= grids.value().size() ||
       source.tile_unit >= grids.value()[source.tensor_slot].tile_count())) {
    status = invalid("C query has invalid member tile");
    return;
  }
  if (source.shared_stage == UINT32_MAX && source.tensor_outputs) {
    auto region = grids.value()[source.tensor_slot].region(source.tile_unit);
    if (!region.ok()) {
      status = region.status();
      return;
    }
    FootprintLimits limits;
    limits.consume_work = [&](std::uint64_t work) {
      return resources.consume({work});
    };
    limits.cancellation = source.cancellation;
    auto expected = Footprint::from_regions(
        source.output.result_schema->tensors[source.tensor_slot].sample_shape(),
        {region.take_value()}, limits);
    if (!expected.ok()) {
      status = expected.status();
      return;
    }
    if (*source.tensor_outputs != expected.value()) {
      status = invalid("C query coverage differs from selected member tile");
      return;
    }
  }
  output_grid_members = ResourceVector<ps_result_tile_grid_v7>(
      ResourceAllocator<ps_result_tile_grid_v7>(resources));
  for (const auto& member : grids.value())
    output_grid_members.push_back(encode_grid(member));
  output_tile_grids = {sizeof(ps_result_tile_grids_v7),
                       static_cast<std::uint32_t>(output_grid_members.size()),
                       output_grid_members.data()};
  query.output_tile_grids = &output_tile_grids;
  if (source.prepared) {
    const auto& stages = source.prepared->traits().shared_stages;
    for (const auto& stage : stages)
      stage_metadata.push_back({std::shared_ptr<const SchemaTemplate>(
          source.prepared, &stage.result_schema)});
    stage_views = std::make_unique<MetadataView>(stage_metadata);
    if (!stage_views->status.ok()) {
      status = stage_views->status;
      return;
    }
    stage_contracts = ResourceVector<ps_result_shared_stage_v7>(
        ResourceAllocator<ps_result_shared_stage_v7>(resources));
    for (std::size_t i = 0; i < stages.size(); ++i) {
      const auto& stage = stages[i];
      apply_validators(*stage_views, i, stage.validators);
      stage_contracts.push_back(
          {sizeof(ps_result_shared_stage_v7), stage.key.data(),
           static_cast<uint32_t>(stage.key.size()),
           stage_views->ports[i].schema,
           stage.input_indices.empty() ? nullptr : stage.input_indices.data(),
           static_cast<uint32_t>(stage.input_indices.size()),
           static_cast<uint32_t>(stage.backend), stage.mandatory ? 1U : 0U,
           stage.continuation_bytes - c_member_storage_bytes(),
           stage.workspace_bytes, stage.maximum_work,
           stage.maximum_output_payload_bytes, stage.maximum_dependency_stages,
           stage_views->ports[i].validators,
           stage_views->ports[i].validator_count});
    }
    query.shared_stages =
        stage_contracts.empty() ? nullptr : stage_contracts.data();
    query.shared_stage_count = stage_contracts.size();
  }
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
  ps_result_metadata_sink_v7 service{sizeof(service), &sink, MetadataSink::set,
                                     MetadataSink::set_resources,
                                     MetadataSink::set_shared_stages};
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
  prepared.shared_stages = std::move(sink.stages);
  prepared.resources = std::move(sink.resource_ids);
  return Result<OperationPreparation>(std::move(prepared));
}
}  // namespace ps::plugin_internal::result_c
