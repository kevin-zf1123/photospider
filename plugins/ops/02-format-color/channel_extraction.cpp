#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "photospider/data/tensor_description.hpp"
#include "photospider/format/channel.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/utf8_validation.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
using Answer = Result<std::vector<OperationOutputSpecialization>>;

Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
enum class Layout { Auto, View, Materialize };
struct Selection final {
  std::uint32_t axis = 0;
  std::uint64_t index = 0;
  bool keepdims = false;
  Layout layout = Layout::Auto;
};
struct Preparation final {
  Selection selection;
};
bool supported(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
    case ElementType::UInt16:
    case ElementType::Int8:
    case ElementType::Int16:
    case ElementType::Int64:
    case ElementType::Float32:
    case ElementType::Float64:
      return true;
  }
  return false;
}
std::optional<TensorDescription> attached_description(
    const OperationMetadata& input, Status* failure) {
  for (const auto& facet : input.facets) {
    if (facet.key != "photospider.tensor-description")
      continue;
    auto decoded = decode_tensor_description(facet);
    if (!decoded.ok()) {
      *failure = decoded.status();
      return {};
    }
    *failure = validate_tensor_description(decoded.value(), input.descriptor);
    if (!failure->ok())
      return {};
    return decoded.take_value();
  }
  return {};
}
TensorDescription project_description(TensorDescription description,
                                      const Selection& selected) {
  const auto axis = selected.axis;
  const bool own_channel =
      description.channel_axis && *description.channel_axis == axis;
  if (own_channel) {
    if (!description.channels.empty()) {
      description.component = description.channels[selected.index];
      description.channels = {description.channels[selected.index]};
    }
    for (const auto& group : description.groups)
      for (std::size_t i = 0; i < group.indices.size(); ++i)
        if (group.indices[i] == selected.index) {
          if (!description.component)
            description.component = group.components[i];
          description.component->interpretation = group.interpretation;
          description.channels = {*description.component};
        }
    description.groups.clear();
    if (!selected.keepdims) {
      description.channel_axis.reset();
      description.channels.clear();
    }
  } else if (description.channel_axis && !selected.keepdims &&
             *description.channel_axis > axis) {
    --*description.channel_axis;
  }
  if (!description.axes.empty()) {
    if (selected.keepdims)
      description.axes[axis].origin +=
          selected.index * description.axes[axis].step;
    else
      description.axes.erase(description.axes.begin() + axis);
  }
  return description;
}

Result<OperationPreparation> prepare_extraction(
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters, bool named,
    SequenceProfile profile) {
  using Prepared = Result<OperationPreparation>;
  const auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Prepared(available);
  const auto& input = inputs[0];
  const auto& shape = input.descriptor.shape;
  if (!supported(input.descriptor.element_type) || shape.empty() ||
      shape.size() > 8)
    return Prepared(Status{ErrorCode::TypeMismatch,
                           "channel extraction requires a supported tensor"});
  const auto expected_dtype = parameters.find("expected_source_dtype");
  const auto expected_shape = parameters.find("expected_source_shape");
  const auto expected_tensor = parameters.find("expected_source_tensor");
  const auto expected_layout = parameters.find("expected_source_layout");
  const auto assertion_count =
      static_cast<unsigned>(expected_dtype != parameters.end()) +
      static_cast<unsigned>(expected_shape != parameters.end()) +
      static_cast<unsigned>(expected_tensor != parameters.end()) +
      static_cast<unsigned>(expected_layout != parameters.end());
  if (assertion_count && assertion_count != 4)
    return Prepared(invalid("source metadata assertions must be complete"));
  if (assertion_count) {
    std::string actual_shape;
    for (const auto extent : shape) {
      if (!actual_shape.empty())
        actual_shape.push_back(',');
      actual_shape += std::to_string(extent);
    }
    std::string actual_tensor = "none";
    for (const auto& facet : input.facets)
      if (facet.key == "photospider.tensor-description") {
        auto decoded = decode_tensor_description(facet);
        if (!decoded.ok())
          return Prepared(decoded.status());
        auto encoded = tensor_description_parameter(decoded.value());
        if (!encoded.ok())
          return Prepared(encoded.status());
        actual_tensor = encoded.take_value();
      }
    if (std::get<std::int64_t>(expected_dtype->second) !=
            static_cast<std::int64_t>(input.descriptor.element_type) ||
        std::get<std::string>(expected_shape->second) != actual_shape ||
        std::get<std::string>(expected_tensor->second) != actual_tensor ||
        std::get<std::string>(expected_layout->second) !=
            format::detail::layout_assertion(input.planar_layout))
      return Prepared(
          invalid("source descriptor assertion disagrees with input"));
  }
  const auto& mode = std::get<std::string>(parameters.at("metadata_mode"));
  if (mode != "respect" && mode != "raw" && mode != "override")
    return Prepared(invalid("metadata_mode must be respect, raw or override"));
  if (named && mode == "raw")
    return Prepared(invalid("named extraction requires interpreted metadata"));
  const auto override_it = parameters.find("metadata_override");
  if ((mode == "override") != (override_it != parameters.end()))
    return Prepared(
        invalid("metadata_override must occur exactly in override mode"));
  Status metadata_status = Status::success();
  std::optional<TensorDescription> description;
  if (mode == "override") {
    auto replacement = tensor_description_from_parameter(
        std::get<std::string>(override_it->second));
    if (!replacement.ok())
      return Prepared(replacement.status());
    metadata_status =
        validate_tensor_description(replacement.value(), input.descriptor);
    if (!metadata_status.ok())
      return Prepared(metadata_status);
    description = replacement.take_value();
  } else if (mode != "raw") {
    description = attached_description(input, &metadata_status);
    if (!metadata_status.ok())
      return Prepared(metadata_status);
  } else {
    description = attached_description(input, &metadata_status);
    if (!metadata_status.ok())
      description.reset();
  }
  Selection selected;
  const auto axis_it = parameters.find("axis");
  if (axis_it != parameters.end()) {
    const auto axis = std::get<std::int64_t>(axis_it->second);
    if (axis < 0 || static_cast<std::uint64_t>(axis) >= shape.size())
      return Prepared(invalid("channel axis is outside tensor rank"));
    selected.axis = static_cast<std::uint32_t>(axis);
    if (mode != "raw" && description && description->channel_axis &&
        selected.axis != *description->channel_axis)
      return Prepared(invalid("axis assertion disagrees with metadata"));
  } else if (mode != "raw" && description && description->channel_axis) {
    selected.axis = *description->channel_axis;
  } else {
    return Prepared(invalid("channel axis must be explicit or described"));
  }
  const auto expected = parameters.find("expected_channels");
  if (expected != parameters.end() &&
      (std::get<std::int64_t>(expected->second) <= 0 ||
       static_cast<std::uint64_t>(std::get<std::int64_t>(expected->second)) !=
           shape[selected.axis]))
    return Prepared(invalid("channel count assertion disagrees with input"));
  selected.keepdims = std::get<bool>(parameters.at("keepdims"));
  if (!selected.keepdims && shape.size() == 1)
    return Prepared(invalid("rank-one extraction requires keepdims=true"));
  const auto& layout = std::get<std::string>(parameters.at("layout"));
  if (layout == "auto")
    selected.layout = Layout::Auto;
  else if (layout == "view")
    selected.layout = Layout::View;
  else if (layout == "materialize")
    selected.layout = Layout::Materialize;
  else
    return Prepared(invalid("layout must be auto, view or materialize"));
  if (named) {
    const auto& match = std::get<std::string>(parameters.at("match"));
    const auto& selector = std::get<std::string>(parameters.at("selector"));
    if (match != "name" && match != "role")
      return Prepared(invalid("match must be name or role"));
    if (selector.empty() || selector.size() > 128 || !valid_utf8_key(selector))
      return Prepared(
          invalid("selector must be strict UTF-8 of at most 128 bytes"));
    if (!description || !description->channel_axis ||
        *description->channel_axis != selected.axis ||
        description->channels.size() != shape[selected.axis])
      return Prepared(
          invalid("named extraction requires a complete channel table"));
    bool found = false;
    for (std::uint64_t index = 0; index < description->channels.size();
         ++index) {
      const auto& entry = description->channels[index];
      const auto& field = match == "name" ? entry.name : entry.role;
      if (field != selector)
        continue;
      if (found)
        return Prepared(invalid("named channel selector is ambiguous"));
      selected.index = index;
      found = true;
    }
    if (!found)
      return Prepared(invalid("named channel selector is absent"));
  } else {
    const auto index = std::get<std::int64_t>(parameters.at("index"));
    if (index < 0 || static_cast<std::uint64_t>(index) >= shape[selected.axis])
      return Prepared(invalid("channel index is outside selected axis"));
    selected.index = static_cast<std::uint64_t>(index);
  }
  OperationOutputSpecialization output;
  output.metadata.descriptor.element_type = input.descriptor.element_type;
  output.metadata.descriptor.shape = shape;
  if (selected.keepdims)
    output.metadata.descriptor.shape[selected.axis] = 1;
  else
    output.metadata.descriptor.shape.erase(
        output.metadata.descriptor.shape.begin() + selected.axis);
  if (description) {
    auto projected = project_description(*description, selected);
    auto encoded = encode_tensor_description(projected);
    if (!encoded.ok())
      return Prepared(encoded.status());
    output.metadata.facets.push_back(encoded.take_value());
  }
  if (input.planar_layout) {
    const auto& original = *input.planar_layout;
    const bool structural =
        original.channel_axis && selected.axis == *original.channel_axis;
    if (structural || selected.keepdims) {
      auto layout = original;
      layout.groups.clear();
      if (!structural) {
        layout.groups = original.groups;
      } else if (selected.keepdims) {
        for (const auto& group : original.groups)
          if (selected.index >= group.first_channel &&
              selected.index - group.first_channel < group.channel_count)
            layout.groups.push_back({group.role, 0, 1});
      } else {
        if (layout.height_axis > selected.axis)
          --layout.height_axis;
        if (layout.width_axis > selected.axis)
          --layout.width_axis;
        layout.channel_axis.reset();
      }
      output.metadata.planar_layout = std::move(layout);
    }
  }
  auto all = Footprint::all(output.metadata.descriptor.shape);
  if (!all.ok())
    return Prepared(all.status());
  DependencyMappedNeed data;
  data.port = 0;
  data.roles = static_cast<std::uint32_t>(DependencyRole::Data);
  for (std::uint32_t source = 0; source < shape.size(); ++source) {
    DependencyAxis mapped;
    if (source == selected.axis) {
      mapped.observation_axis = -1;
      mapped.fixed = {selected.index, 1};
    } else {
      mapped.observation_axis = static_cast<std::int32_t>(
          source < selected.axis || selected.keepdims ? source : source - 1);
    }
    data.axes.push_back(mapped);
  }
  output.static_dependency_pieces =
      std::vector<DependencyMapPiece>{{all.take_value(), {std::move(data)}}};
  output.regional_atomic = true;
  output.preserve_output_views = selected.layout != Layout::Materialize;
  if (output.preserve_output_views)
    output.maximum_output_payload_bytes = 0;
  OperationPreparation prepared;
  prepared.outputs.push_back(std::move(output));
  prepared.state = std::make_shared<Preparation>(Preparation{selected});
  return Prepared(std::move(prepared));
}

std::vector<std::uint64_t> source_coordinate(
    const std::vector<std::uint64_t>& output, const Selection& selected) {
  std::vector<std::uint64_t> source(output.size() +
                                    (selected.keepdims ? 0 : 1));
  for (std::size_t axis = 0; axis < source.size(); ++axis)
    source[axis] =
        axis == selected.axis
            ? selected.index
            : output[axis < selected.axis || selected.keepdims ? axis
                                                               : axis - 1];
  return source;
}

Result<std::optional<Value>> mapped_view(
    const Value& input, const Region& request,
    const ValueDescriptor& output_descriptor,
    const std::vector<ValueFacet>& output_facets, const Selection& selected,
    const ResourceBindings& resources) {
  using View = Result<std::optional<Value>>;
  const auto& input_box = input.region().dimensions();
  const auto source_channel = input_box[selected.axis];
  if (selected.index < source_channel.offset ||
      selected.index - source_channel.offset >= source_channel.extent)
    return View(std::optional<Value>{});
  std::vector<RegionDimension> clipped = request.dimensions();
  for (std::size_t axis = 0; axis < input_box.size(); ++axis) {
    if (axis == selected.axis)
      continue;
    const auto output_axis =
        axis < selected.axis || selected.keepdims ? axis : axis - 1;
    const auto start =
        std::max(clipped[output_axis].offset, input_box[axis].offset);
    const auto end =
        std::min(clipped[output_axis].offset + clipped[output_axis].extent,
                 input_box[axis].offset + input_box[axis].extent);
    if (start >= end)
      return View(std::optional<Value>{});
    clipped[output_axis] = {start, end - start};
  }
  std::vector<std::uint64_t> origin;
  origin.reserve(clipped.size());
  for (const auto& dimension : clipped)
    origin.push_back(dimension.offset);
  auto source = source_coordinate(origin, selected);
  auto address = input.byte_address(source);
  if (!address.ok())
    return View(address.status());
  std::vector<std::int64_t> strides;
  strides.reserve(clipped.size());
  for (std::size_t axis = 0; axis < input_box.size(); ++axis) {
    if (axis == selected.axis) {
      if (selected.keepdims)
        strides.push_back(0);
    } else {
      strides.push_back(input.layout().byte_strides[axis]);
    }
  }
  auto value = Value::from_storage(
      output_descriptor, Region(clipped),
      {address.value(), std::move(strides), std::move(origin)}, input.storage(),
      output_facets, resources);
  if (!value.ok())
    return View(value.status());
  return View(std::optional<Value>{value.take_value()});
}

Result<ValueFragments> publish_views(const DependencyPhase& phase,
                                     const Selection& selected) {
  using Output = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto& fragments = phase.inputs[0].fragments();
  numeric_ops::ArrayPublication publication(
      phase.query.outputs.boxes().size() * fragments.size(),
      descriptor.shape.size());
  std::vector<Value> views;
  for (const auto& box : phase.query.outputs.boxes()) {
    for (const auto& fragment : fragments) {
      auto charged = phase.consume_work(descriptor.shape.size() + 1);
      if (!charged.ok())
        return Output(charged);
      auto mapped = mapped_view(fragment, box, descriptor, facets, selected,
                                phase.query.resources);
      if (!mapped.ok())
        return Output(mapped.status());
      if (!mapped.value())
        continue;
      auto retained = publication.retain(std::move(*mapped.value()));
      if (!retained.ok())
        return Output(retained.status());
      views.push_back(retained.take_value());
    }
  }
  return publication.finish(descriptor, phase.query.outputs, views.data(),
                            views.size(), phase.sets, facets,
                            phase.query.resources);
}

Result<ValueFragments> publish_copies(const DependencyPhase& phase,
                                      const Selection& selected) {
  using Output = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto width = Value::element_size(descriptor.element_type);
  auto mapped = publish_views(phase, selected);
  if (!mapped.ok())
    return Output(mapped.status());
  numeric_ops::ArrayPublication publication(phase.query.outputs.boxes().size(),
                                            descriptor.shape.size());
  std::vector<Value> values;
  values.reserve(phase.query.outputs.boxes().size());
  for (const auto& box : phase.query.outputs.boxes()) {
    auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
    if (!allocated.ok())
      return Output(allocated.status());
    auto writer = allocated.take_value();
    for (const auto& fragment : mapped.value().fragments()) {
      std::vector<RegionDimension> overlap;
      overlap.reserve(box.rank());
      bool intersects = true;
      for (std::size_t axis = 0; axis < box.rank(); ++axis) {
        const auto source = fragment.region().dimensions()[axis];
        const auto destination = box.dimensions()[axis];
        const auto start = std::max(source.offset, destination.offset);
        const auto end = std::min(source.offset + source.extent,
                                  destination.offset + destination.extent);
        if (start >= end) {
          intersects = false;
          break;
        }
        overlap.push_back({start, end - start});
      }
      if (!intersects)
        continue;
      auto points = Footprint::from_regions(
          descriptor.shape, {Region(std::move(overlap))}, phase.sets);
      if (!points.ok())
        return Output(points.status());
      auto status = points.value().visit(
          [&](const auto& at) {
            auto charged = phase.consume_work(at.size() + width);
            if (!charged.ok())
              return charged;
            std::uint64_t linear = 0;
            for (std::size_t axis = 0; axis < at.size(); ++axis)
              linear = linear * box.dimensions()[axis].extent + at[axis] -
                       box.dimensions()[axis].offset;
            auto address = fragment.byte_address(at);
            if (!address.ok())
              return address.status();
            std::memcpy(writer.data() + linear * width,
                        fragment.bytes().data() + address.value(), width);
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Output(status);
    }
    auto value = std::move(writer).publish(facets, phase.query.resources);
    if (!value.ok())
      return Output(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Output(retained.status());
    values.push_back(retained.take_value());
  }
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}

struct ChannelState final {
  Selection selected;
  bool requested = false;
  explicit ChannelState(Selection value) : selected(value) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    auto result = selected.layout == Layout::Materialize
                      ? publish_copies(phase, selected)
                      : publish_views(phase, selected);
    return result.ok() ? Result<DependencyPoll>(result.take_value())
                       : Result<DependencyPoll>(result.status());
  }
};

OperationDefinition extraction(const std::string& key, bool named,
                               SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.planar_storage_capable = true;
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"axis", OperationParameterType::Int64, false},
      {"expected_channels", OperationParameterType::Int64, false},
      {"expected_source_dtype", OperationParameterType::Int64, false},
      {"expected_source_layout", OperationParameterType::String, false},
      {"expected_source_shape", OperationParameterType::String, false},
      {"expected_source_tensor", OperationParameterType::String, false},
      {"keepdims", OperationParameterType::Bool},
      {"layout", OperationParameterType::String},
      {"metadata_mode", OperationParameterType::String},
      {"metadata_override", OperationParameterType::String, false}};
  if (named) {
    traits.parameter_schema.push_back(
        {"match", OperationParameterType::String});
    traits.parameter_schema.push_back(
        {"selector", OperationParameterType::String});
  } else {
    traits.parameter_schema.push_back({"index", OperationParameterType::Int64});
  }
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ChannelState);
  output.maximum_dependency_stages = 2;
  definition.prepare_static = [named, profile](const auto& inputs,
                                               const auto& parameters) {
    return prepare_extraction(inputs, parameters, named, profile);
  };
  definition.start_dependency = [](const DependencyQuery& query,
                                   const BufferAllocator& allocator) {
    const auto* prepared =
        static_cast<const Preparation*>(query.prepared->state());
    return DependencyContinuation::make<ChannelState>(allocator,
                                                      prepared->selection);
  };
  definition.planar_callback = [named, profile](
                                   const PlanarOperationInvocation& call) {
    OperationMetadata metadata;
    metadata.descriptor = call.inputs[0].descriptor();
    metadata.facets = call.inputs[0].facets();
    const auto& config = call.inputs[0].config();
    metadata.planar_layout = PlanarImageLayout{
        config.order,        config.height_axis,     config.width_axis,
        config.channel_axis, config.row_pitch_bytes, config.groups};
    auto prepared =
        prepare_extraction({metadata}, call.parameters, named, profile);
    if (!prepared.ok())
      return prepared.status();
    const auto& selected =
        static_cast<const Preparation*>(prepared.value().state.get())
            ->selection;
    const auto width = Value::element_size(metadata.descriptor.element_type);
    const auto& layout = *prepared.value().outputs[0].metadata.planar_layout;
    const auto y = call.output_region.dimensions()[layout.height_axis];
    const auto x = call.output_region.dimensions()[layout.width_axis];
    const auto channels =
        layout.channel_axis
            ? call.output_region.dimensions()[*layout.channel_axis]
            : RegionDimension{0, 1};
    std::vector<std::uint64_t> at(call.output_region.rank(), 0);
    std::vector<std::uint64_t> source(metadata.descriptor.shape.size());
    for (std::size_t axis = 0; axis < at.size(); ++axis)
      at[axis] = call.output_region.dimensions()[axis].offset;
    // A window certifies its ROI once. Reuse coordinates and copy contiguous
    // row spans, bounded by both physical tile edges and cancellation work.
    for (std::uint64_t channel = channels.offset;
         channel < channels.offset + channels.extent; ++channel) {
      if (layout.channel_axis)
        at[*layout.channel_axis] = channel;
      for (std::uint64_t row = y.offset; row < y.offset + y.extent;) {
        auto rows = y.offset + y.extent - row;
        at[layout.height_axis] = row;
        for (std::uint64_t column = x.offset; column < x.offset + x.extent;) {
          if (call.cancellation.cancelled())
            return Status{ErrorCode::Cancelled, "channel extraction cancelled"};
          at[layout.width_axis] = column;
          for (std::size_t axis = 0; axis < source.size(); ++axis)
            source[axis] =
                axis == selected.axis
                    ? selected.index
                    : at[axis < selected.axis || selected.keepdims ? axis
                                                                   : axis - 1];
          auto read = call.inputs[0].rectangle_run(source);
          if (!read.ok())
            return read.status();
          auto write = call.output.rectangle_run(at);
          if (!write.ok())
            return write.status();
          const auto& input = read.value();
          const auto& output = write.value();
          rows = std::min(rows, std::min(input.rows, output.rows));
          const auto samples = std::min(input.row.samples, output.row.samples);
          for (std::uint64_t dy = 0; dy < rows; ++dy) {
            for (std::uint64_t dx = 0; dx < samples;) {
              if (call.cancellation.cancelled())
                return Status{ErrorCode::Cancelled,
                              "channel extraction cancelled"};
              const auto count = std::min<std::uint64_t>(1024, samples - dx);
              std::memcpy(
                  output.row.data + dy * output.row_stride_bytes + dx * width,
                  input.row.data + dy * input.row_stride_bytes + dx * width,
                  count * width);
              dx += count;
            }
          }
          column += samples;
        }
        row += rows;
      }
    }
    return Status::success();
  };
  return definition;
}
}  // namespace

Status register_channel_extraction(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& member : {std::make_pair("channel.extract_index", false),
                               std::make_pair("channel.extract_named", true)}) {
      auto status = registry->register_operation(
          extraction(std::string(member.first) + profile.first, member.second,
                     profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
