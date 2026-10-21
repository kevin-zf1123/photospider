#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "02-format-color/result_mapping.hpp"
#include "common/domain/model_coordinates.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/core/utf8_validation.hpp"
#include "photospider/data/region_runs.hpp"
#include "photospider/ops/format/channel_editing.hpp"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps::plugin_internal {
namespace {
using format_result::require;
using format_result::take;
using numeric_ops::SequenceProfile;
Status mismatch(const std::string& text) {
  return {ErrorCode::TypeMismatch,
          text,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
using Params = std::map<std::string, ParameterValue>;
struct Span final {
  std::uint32_t port = 0;
  std::uint64_t destination = 0, source = 0, count = 1;
};
struct Assembly final {
  std::uint32_t axis = 0;
  std::vector<std::optional<std::uint32_t>> axes;
  std::vector<Span> spans;
  std::vector<bool> scalars;
  std::string layout;
  std::vector<std::vector<std::uint32_t>> by_port;
};
bool number(const std::string& text, std::uint64_t* value) {
  if (text.empty() || (text.size() > 1 && text[0] == '0'))
    return false;
  auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
std::vector<std::string> split(const std::string& text, char delimiter) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  for (std::size_t i = 0; i <= text.size(); ++i)
    if (i == text.size() || text[i] == delimiter) {
      result.push_back(text.substr(begin, i - begin));
      begin = i + 1;
    }
  return result;
}
Result<std::vector<std::string>> records(const Params& p, const char* key) {
  using Answer = Result<std::vector<std::string>>;
  auto found = p.find(key);
  if (found == p.end())
    return Answer(std::vector<std::string>{});
  auto fields = split(std::get<std::string>(found->second), ';');
  if (fields.size() < 2 || fields[0] != "v1")
    return Answer(core_internal::invalid_schema_domain(
        std::string(key) + ": expected canonical v1 records"));
  fields.erase(fields.begin());
  return Answer(std::move(fields));
}
Result<std::string> unhex(const std::string& text) {
  if (text.empty() || text.size() % 2 || text.size() > 256)
    return Result<std::string>(
        core_internal::invalid_schema_domain("invalid selector hex length"));
  const auto digit = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
  };
  std::string out;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int a = digit(text[i]), b = digit(text[i + 1]);
    if (a < 0 || b < 0)
      return Result<std::string>(
          core_internal::invalid_schema_domain("invalid selector hex"));
    out += static_cast<char>((a << 4) | b);
  }
  if (!core_internal::valid_utf8_key(out))
    return Result<std::string>(
        core_internal::invalid_schema_domain("invalid UTF-8 selector"));
  return Result<std::string>(std::move(out));
}
const ResultTensorSpec& tensor(const OperationMetadata& input) {
  require(tensor_ops::check_tensor(input));
  return input.result_schema->tensors[0];
}
using format_result::assembly_source_assertion;
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Params& p, int member,
    SequenceProfile profile) try {
  using Answer = Result<OperationPreparation>;
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  if (p.count("expected_inputs") &&
      std::get<std::string>(p.at("expected_inputs")) !=
          assembly_source_assertion(inputs))
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 authoring descriptors disagree with inference"));
  if (inputs.empty())
    return Answer(core_internal::invalid_schema_domain(
        "assembly requires at least one input"));
  const auto layout = std::get<std::string>(p.at("layout"));
  if (layout != "auto" && layout != "view" && layout != "materialize")
    return Answer(core_internal::invalid_schema_domain("invalid layout"));
  auto override_records = records(p, "input_descriptions");
  if (!override_records.ok())
    return Answer(override_records.status());
  std::map<std::uint32_t, TensorDescription> overrides;
  std::uint64_t previous = 0;
  for (const auto& row : override_records.value()) {
    const auto fields = split(row, ':');
    std::uint64_t port = 0;
    if (fields.size() != 2 || !number(fields[0], &port) ||
        port >= inputs.size() || (!overrides.empty() && port <= previous))
      return Answer(core_internal::invalid_schema_domain(
          "input_descriptions requires sorted unique input ordinals"));
    auto value = tensor_description_from_parameter(fields[1]);
    if (!value.ok())
      return Answer(value.status());
    overrides.emplace(port, value.take_value());
    previous = port;
  }
  auto axis_records =
      records(p, member == 2 ? "input_structure" : "input_axes");
  if (!axis_records.ok())
    return Answer(axis_records.status());
  if ((member == 2 || !axis_records.value().empty()) &&
      axis_records.value().size() != inputs.size())
    return Answer(core_internal::invalid_schema_domain(
        "axis/structure list length differs from input arity"));
  Assembly assembly;
  assembly.layout = layout;
  std::vector<std::optional<TensorDescription>> descriptions;
  std::vector<std::uint64_t> shape;
  std::optional<ResultTensorLayout> image_layout;
  bool any_image = false;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = tensor(inputs[i]);
    const auto& dimensions = input.descriptor.shape;
    const std::string where = "input " + std::to_string(i) + ": ";
    if (dimensions.empty() || dimensions.size() > 8 ||
        input.descriptor.element_type !=
            tensor(inputs[0]).descriptor.element_type)
      return Answer(mismatch(where + "unsupported rank/type or mixed dtype"));
    auto extent = input.sample_count();
    if (!extent.ok())
      return Answer(extent.status());
    if (extent.value() == 0 || extent.value() > (1ULL << 40))
      return Answer(core_internal::invalid_schema_domain(
          where + "invalid element count"));
    std::optional<TensorDescription> description;
    if (overrides.count(i)) {
      description = overrides.at(i);
    }
    if (description) {
      auto status = validate_tensor_description(*description, input.descriptor);
      if (!status.ok()) {
        if (description)
          return Answer(status);
        description.reset();
      }
    }
    bool scalar = member == 2 && axis_records.value()[i] == "s";
    if (scalar && (!p.count("authoring_member") || !p.count("expected_inputs")))
      return Answer(core_internal::invalid_schema_domain(
          "scalar fill is an internal FMT-03 lowering"));
    bool single = member == 0;
    std::optional<std::uint32_t> axis;
    if (member != 0) {
      std::string field =
          axis_records.value().empty() ? "_" : axis_records.value()[i];
      if (member == 2) {
        single = field == "c" || scalar;
        if (!single) {
          if (field.empty() || field[0] != 'h')
            return Answer(core_internal::invalid_schema_domain(
                where + "structure must be c or h<axis>"));
          field.erase(field.begin());
        }
      }
      if (!single && field != "_") {
        std::uint64_t number_axis = 0;
        if (!number(field, &number_axis) || number_axis >= dimensions.size())
          return Answer(core_internal::invalid_schema_domain(
              where + "channel axis out of range"));
        axis = number_axis;
      }
    }
    if (scalar) {
      if (dimensions != std::vector<std::uint64_t>{1} || input.layout.spatial ||
          !input.batch_axes.empty())
        return Answer(mismatch(where + "scalar requires generic shape [1]"));
    } else if (single) {
      if (dimensions.size() > 7 || (description && description->channel_axis))
        return Answer(core_internal::invalid_schema_domain(
            where + "component has an existing channel axis or rank eight"));
    } else {
      if (description && description->channel_axis) {
        if (axis && *axis != *description->channel_axis)
          return Answer(core_internal::invalid_schema_domain(
              where + "axis assertion conflicts with description"));
        axis = description->channel_axis;
      }
      if (!axis)
        return Answer(core_internal::invalid_schema_domain(
            where + "channel axis must be supplied or described"));
    }
    if (description && description->channel_axis != axis)
      description.reset();
    auto nonchannel = dimensions;
    if (axis)
      nonchannel.erase(nonchannel.begin() + *axis);
    if (i == 0) {
      if (scalar)
        return Answer(core_internal::invalid_schema_domain(
            "first source must establish the spatial grid"));
      shape = nonchannel;
    } else if (!scalar && (shape != nonchannel ||
                           input.batch_axes != tensor(inputs[0]).batch_axes)) {
      return Answer(mismatch(where + "nonchannel extents differ"));
    }
    if (input.layout.spatial) {
      any_image = true;
      auto structural = input.layout;
      if (structural.channel_axis != axis)
        return Answer(core_internal::invalid_schema_domain(
            where + "interpretation cannot relabel physical image axes"));
      if (axis) {
        if (structural.height_axis > *axis)
          --structural.height_axis;
        if (structural.width_axis > *axis)
          --structural.width_axis;
      }
      structural.channel_axis.reset();
      if (image_layout &&
          (image_layout->height_axis != structural.height_axis ||
           image_layout->width_axis != structural.width_axis))
        return Answer(mismatch(where + "physical spatial axes differ"));
      if (!image_layout)
        image_layout = structural;
    }
    assembly.axes.push_back(axis);
    assembly.scalars.push_back(scalar);
    descriptions.push_back(std::move(description));
  }
  const auto output_axis =
      std::get<std::int64_t>(p.at(member == 0 ? "axis" : "output_axis"));
  if (output_axis < 0 ||
      static_cast<std::uint64_t>(output_axis) > shape.size() ||
      shape.size() >= 8)
    return Answer(core_internal::invalid_schema_domain(
        "output channel axis out of range"));
  assembly.axis = output_axis;
  std::uint64_t channels = 0;
  if (member != 2) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      auto count = assembly.axes[i]
                       ? tensor(inputs[i]).descriptor.shape[*assembly.axes[i]]
                       : 1;
      if (count > (1ULL << 40) - channels)
        return Answer(
            core_internal::invalid_schema_domain("channel prefix overflow"));
      assembly.spans.push_back(
          {static_cast<std::uint32_t>(i), channels, 0, count});
      channels += count;
    }
  } else {
    auto rows = records(p, "mapping");
    if (!rows.ok())
      return Answer(rows.status());
    channels = rows.value().size();
    if (!channels)
      return Answer(
          core_internal::invalid_schema_domain("mapping must be nonempty"));
    std::set<std::uint64_t> destinations;
    for (const auto& row : rows.value()) {
      auto fields = split(row, ',');
      std::uint64_t port = 0, dest = 0, selected = 0;
      if (fields.size() != 4 || !number(fields[0], &port) ||
          port >= inputs.size() || !number(fields[3], &dest) ||
          dest >= channels || !destinations.insert(dest).second)
        return Answer(core_internal::invalid_schema_domain(
            "mapping has invalid source, duplicate destination or hole"));
      auto selector = unhex(fields[2]);
      if (!selector.ok())
        return Answer(selector.status());
      auto count =
          assembly.axes[port]
              ? tensor(inputs[port]).descriptor.shape[*assembly.axes[port]]
              : 1;
      if (fields[1] == "index") {
        if (!number(selector.value(), &selected) || selected >= count)
          return Answer(core_internal::invalid_schema_domain(
              "source selector index out of range"));
      } else {
        if ((fields[1] != "name" && fields[1] != "role"))
          return Answer(core_internal::invalid_schema_domain(
              "raw permits only index selectors"));
        const auto& desc = descriptions[port];
        if (!desc || (assembly.axes[port] && desc->channels.size() != count) ||
            (!assembly.axes[port] && !desc->component))
          return Answer(core_internal::invalid_schema_domain(
              "named selector requires component descriptions"));
        bool found = false;
        for (std::uint64_t index = 0; index < count; ++index) {
          const auto& c =
              assembly.axes[port] ? desc->channels[index] : *desc->component;
          if ((fields[1] == "name" ? c.name : c.role) != selector.value())
            continue;
          if (found)
            return Answer(core_internal::invalid_schema_domain(
                "ambiguous named source selector"));
          selected = index;
          found = true;
        }
        if (!found)
          return Answer(
              core_internal::invalid_schema_domain("source selector absent"));
      }
      assembly.spans.push_back(
          {static_cast<std::uint32_t>(port), dest, selected, 1});
    }
    std::sort(assembly.spans.begin(), assembly.spans.end(),
              [](const auto& a, const auto& b) {
                return a.destination < b.destination;
              });
  }
  OperationOutputSpecialization output;
  auto schema = *inputs[0].result_schema;
  auto& output_tensor = schema.tensors[0];
  output_tensor.atomic_trailing_axes = 0;
  output_tensor.layout = {};
  output_tensor.descriptor = {tensor(inputs[0]).descriptor.element_type, shape};
  output_tensor.descriptor.shape.insert(
      output_tensor.descriptor.shape.begin() + output_axis, channels);
  auto output_count = output_tensor.sample_count();
  if (!output_count.ok())
    return Answer(output_count.status());
  if (output_count.value() > (1ULL << 40))
    return Answer(
        core_internal::invalid_schema_domain("output exceeds element limit"));
  if (any_image) {
    auto physical = *image_layout;
    if (physical.height_axis >= assembly.axis)
      ++physical.height_axis;
    if (physical.width_axis >= assembly.axis)
      ++physical.width_axis;
    physical.channel_axis = assembly.axis;
    output_tensor.layout = std::move(physical);
  }
  assembly.by_port.resize(inputs.size());
  for (std::size_t i = 0; i < assembly.spans.size(); ++i)
    assembly.by_port[assembly.spans[i].port].push_back(i);
  auto valid = schema.validate();
  if (!valid.ok())
    return Answer(valid);
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  OperationPreparation prepared;
  prepared.outputs.push_back(std::move(output));
  prepared.state = std::make_shared<Assembly>(std::move(assembly));
  return Answer(std::move(prepared));
} catch (const Status& status) {
  return Result<OperationPreparation>(status);
}

struct InputPort final {
  TileGrid grid;
  std::vector<std::pair<TileIndex, std::uint32_t>> slots;
  std::array<std::int8_t, 8> output_axes{};
  unsigned rank = 0;
  int channel_axis = -1;
  bool scalar = false;
};
InputPort input_port(const OperationMetadata& metadata, const Assembly& a,
                     unsigned port, unsigned batches) {
  InputPort result;
  result.rank = metadata.result_schema->tensors[0].sample_shape().size();
  result.scalar = a.scalars[port];
  if (a.axes[port])
    result.channel_axis = batches + *a.axes[port];
  for (unsigned axis = 0; axis < result.rank; ++axis) {
    if (result.scalar) {
      result.output_axes[axis] = -1;
    } else if (axis < batches) {
      result.output_axes[axis] = axis;
    } else if (static_cast<int>(axis) == result.channel_axis) {
      result.output_axes[axis] = batches + a.axis;
    } else {
      const auto cell = axis - batches -
                        (result.channel_axis >= 0 &&
                         static_cast<int>(axis) > result.channel_axis);
      result.output_axes[axis] = batches + cell + (cell >= a.axis);
    }
  }
  return result;
}
std::optional<Region> source_region(const Region& output, unsigned channel_axis,
                                    const InputPort& port, const Span& span) {
  const auto& channel = output.dimensions()[channel_axis];
  const auto begin = std::max(channel.offset, span.destination);
  const auto end =
      std::min(channel.offset + channel.extent, span.destination + span.count);
  if (begin >= end)
    return {};
  if (port.scalar)
    return Region({{0, 1}});
  std::vector<RegionDimension> dimensions;
  for (unsigned axis = 0; axis < port.rank; ++axis)
    dimensions.push_back(
        static_cast<int>(axis) == port.channel_axis
            ? RegionDimension{span.source + begin - span.destination,
                              end - begin}
            : output.dimensions()[port.output_axes[axis]]);
  return Region(std::move(dimensions));
}
Result<std::set<TileIndex>> required_tiles(
    const Region& output, unsigned channel_axis, const Assembly& assembly,
    const InputPort& port, unsigned port_index, const ResourceBudget& budget,
    std::uint64_t maximum_tiles) {
  using Answer = Result<std::set<TileIndex>>;
  auto admitted = budget.consume({1});
  if (!admitted.ok())
    return Answer(admitted);
  std::set<TileIndex> result;
  if (assembly.layout == "view")
    return Answer(std::move(result));
  for (auto index : assembly.by_port[port_index]) {
    auto charged = budget.consume({1});
    if (!charged.ok())
      return Answer(charged);
    auto region =
        source_region(output, channel_axis, port, assembly.spans[index]);
    if (!region)
      continue;
    // Bound the closure before constructing even its compressed intervals.
    // During prepare the bound is the number of supplied immutable read slots.
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < port.grid.rank(); ++axis) {
      const auto dimension = region->dimensions()[axis];
      std::uint64_t extent = 1;
      if (port.grid.batch_axis_mask() & (1U << axis))
        extent = dimension.extent;
      else if (axis == port.grid.height_axis() ||
               axis == port.grid.width_axis())
        extent = (dimension.offset + dimension.extent - 1) /
                     port.grid.tile_extent() -
                 dimension.offset / port.grid.tile_extent() + 1;
      if (extent > maximum_tiles / count)
        return Answer(
            {ErrorCode::ResourceExhausted, "assembly input tile limit"});
      count *= extent;
    }
    auto tiles = sdk::tiles_for_region(budget, port.grid, *region);
    if (!tiles.ok())
      return Answer(tiles.status());
    for (auto tile : tiles.value()) {
      auto status = budget.consume({1});
      if (!status.ok())
        return Answer(status);
      result.insert(tile);
      if (result.size() > maximum_tiles)
        return Answer(
            {ErrorCode::ResourceExhausted, "assembly input tile union limit"});
    }
  }
  return Answer(std::move(result));
}
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& assembly = *static_cast<const Assembly*>(query.static_state);
  const auto& spec = query.outputs[0].result_schema->tensors[0];
  const auto batches = spec.batch_axes.size();
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::vector<InputPort> inputs;
  for (unsigned i = 0; i < query.inputs.size(); ++i) {
    auto port = input_port(query.inputs[i], assembly, i, batches);
    auto grid = TileGrid::from_schema(*query.inputs[i].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    port.grid = grid.take_value();
    inputs.push_back(std::move(port));
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task("assemble", "channel-assembly-copy-v1");
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      for (unsigned i = 0; i < inputs.size(); ++i) {
        auto reads =
            required_tiles(region.value(), batches + assembly.axis, assembly,
                           inputs[i], i, budget, query.limits.maximum_records);
        if (!reads.ok())
          return Answer(reads.status());
        if (reads.value().empty()) {
          auto status = builder.read_input_schema(task.value(), i);
          if (!status.ok())
            return Answer(status);
        }
        for (auto source : reads.value()) {
          auto status = builder.read_input(task.value(), i, 0, source);
          if (!status.ok())
            return Answer(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct PreparedAssembly final {
  std::vector<InputPort> inputs;
  std::vector<Span> spans;
  unsigned axis = 0, width = 0;
  bool require_view = false;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto cancelled = [&] {
    return invocation->cancellation.is_cancelled &&
           invocation->cancellation.is_cancelled(
               invocation->cancellation.context);
  };
  if (cancelled()) {
    *status = kernel_failure(ErrorCode::Cancelled);
    return;
  }
  const auto& state =
      *static_cast<const PreparedAssembly*>(invocation->prepared_context);
  if (state.require_view) {
    *status = kernel_failure(ErrorCode::InvalidArgument,
                             "ViewUnavailable: fixed assembly requires copy",
                             FailureReason::ViewUnavailable);
    return;
  }
  const auto& output = invocation->outputs[0];
  std::array<std::uint64_t, 8> at{}, source{};
  std::uint64_t samples = 1;
  for (unsigned axis = 0; axis < output.layout->rank; ++axis) {
    at[axis] = output.layout->origin[axis];
    samples *= output.layout->shape[axis];
  }
  auto* destination =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  const ps_kernel_const_view_v8* cached = nullptr;
  unsigned cached_port = UINT32_MAX;
  for (std::uint64_t sample = 0; sample < samples; ++sample) {
    if (sample % 256 == 0 && cancelled()) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    const auto next =
        std::upper_bound(state.spans.begin(), state.spans.end(), at[state.axis],
                         [](std::uint64_t channel, const Span& span) {
                           return channel < span.destination;
                         });
    const auto& span = *std::prev(next);
    const auto& port = state.inputs[span.port];
    for (unsigned axis = 0; axis < port.rank; ++axis)
      source[axis] = port.scalar ? 0
                     : static_cast<int>(axis) == port.channel_axis
                         ? span.source + at[state.axis] - span.destination
                         : at[port.output_axes[axis]];
    bool contained = cached && cached_port == span.port;
    for (unsigned axis = 0; contained && axis < port.rank; ++axis)
      contained = source[axis] >= cached->layout->origin[axis] &&
                  source[axis] - cached->layout->origin[axis] <
                      cached->layout->shape[axis];
    if (!contained) {
      const auto tile = fixed_tensor::input_tile(port.grid, source);
      const auto slot =
          std::lower_bound(port.slots.begin(), port.slots.end(), tile,
                           [](const auto& entry, TileIndex index) {
                             return entry.first < index;
                           });
      cached = &invocation->inputs[slot->second];
      cached_port = span.port;
    }
    __int128 offset = cached->byte_offset;
    for (unsigned axis = 0; axis < port.rank; ++axis)
      offset +=
          static_cast<__int128>(source[axis] - cached->layout->origin[axis]) *
          cached->layout->strides[axis];
    std::memcpy(destination + sample * state.width,
                static_cast<const std::uint8_t*>(cached->allocation) +
                    static_cast<std::ptrdiff_t>(offset),
                state.width);
    for (unsigned axis = output.layout->rank; axis-- > 0;) {
      if (++at[axis] < output.layout->origin[axis] + output.layout->shape[axis])
        break;
      at[axis] = output.layout->origin[axis];
    }
  }
  *status = {};
}
Status output_signature(const KernelPreparation& query, Region* output) {
  const auto invalid = [] {
    return Status{ErrorCode::TypeMismatch, "invalid assembly output signature"};
  };
  if (!query.static_state || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{})
    return invalid();
  const auto& write = query.writes[0];
  const auto& schema = *query.outputs[0].result_schema;
  auto grid = TileGrid::from_schema(schema, query.tile_extent);
  if (!grid.ok())
    return grid.status();
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return region.status();
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type != schema.tensors[0].descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  *output = region.take_value();
  return Status::success();
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  Region output;
  auto valid = output_signature(query, &output);
  if (!valid.ok())
    return Answer(valid);
  const auto& assembly = *static_cast<const Assembly*>(query.static_state);
  if (query.inputs.size() != assembly.axes.size())
    return Answer({ErrorCode::TypeMismatch, "invalid assembly input arity"});
  const auto& spec = query.outputs[0].result_schema->tensors[0];
  const auto batches = spec.batch_axes.size();
  auto state = std::make_unique<PreparedAssembly>();
  state->axis = batches + assembly.axis;
  state->width = TensorSpan::element_size(spec.descriptor.element_type);
  state->require_view = assembly.layout == "view";
  const auto channel = output.dimensions()[state->axis];
  for (const auto& span : assembly.spans)
    if (span.destination < channel.offset + channel.extent &&
        channel.offset < span.destination + span.count)
      state->spans.push_back(span);
  std::size_t slot = 0;
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  for (unsigned i = 0; i < query.inputs.size(); ++i) {
    auto port = input_port(query.inputs[i], assembly, i, batches);
    auto grid = TileGrid::from_schema(*query.inputs[i].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    port.grid = grid.take_value();
    auto reads = required_tiles(output, state->axis, assembly, port, i, budget,
                                query.reads.size() - slot);
    if (!reads.ok())
      return Answer(reads.status());
    for (auto tile : reads.value()) {
      if (slot >= query.reads.size())
        return Answer({ErrorCode::TypeMismatch, "missing assembly input slot"});
      const auto& read = query.reads[slot];
      auto region = port.grid.region(tile);
      if (!region.ok())
        return Answer(region.status());
      if (read.port != i || read.member || read.tile_index != tile ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != spec.descriptor.element_type ||
          !fixed_tensor::matches_region(read.layout, region.value()))
        return Answer({ErrorCode::TypeMismatch, "invalid assembly input slot"});
      port.slots.push_back({tile, static_cast<std::uint32_t>(slot++)});
    }
    state->inputs.push_back(std::move(port));
  }
  if (slot != query.reads.size())
    return Answer({ErrorCode::TypeMismatch, "extra assembly input slots"});
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedAssembly*>(context);
  };
  return Answer(kernel);
}
struct ScalarState final {
  std::array<std::uint8_t, 8> bits{};
  std::size_t width = 0;
};

OperationDefinition definition(const std::string& key, int member,
                               SequenceProfile profile) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  t.input_schema = {port};
  t.repeated_minimum = 1;
  t.repeated_maximum = 1024;
  t.repeated_match = false;
  t.requires_metadata_specialization = true;
  t.parameter_schema = {
      {"input_descriptions", OperationParameterType::String, false},
      {"layout", OperationParameterType::String},
      {"expected_inputs", OperationParameterType::String, false},
      {"authoring_member", OperationParameterType::String, false}};
  if (member == 0)
    t.parameter_schema.push_back({"axis", OperationParameterType::Int64});
  else
    t.parameter_schema.push_back(
        {"output_axis", OperationParameterType::Int64});
  if (member == 1)
    t.parameter_schema.push_back(
        {"input_axes", OperationParameterType::String, false});
  if (member == 2) {
    t.parameter_schema.push_back(
        {"input_structure", OperationParameterType::String});
    t.parameter_schema.push_back({"mapping", OperationParameterType::String});
  }
  auto& out = t.outputs[0];
  out.key = "values";
  out.output_schema = port;
  out.result_schema = tensor_ops::scalar_schema();
  op.prepare_static = [member, profile](const auto& inputs,
                                        const auto& params) {
    return prepare(inputs, params, member, profile);
  };
  op.lower = lower;
  op.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "assemble" ||
        kernel.contract != "channel-assembly-copy-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "unknown assembly kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "assembly requires CPU Host"});
    return prepare_kernel(query);
  };
  return op;
}
Result<std::vector<std::uint8_t>> literal_bytes(const Params& params) {
  using Answer = Result<std::vector<std::uint8_t>>;
  const auto raw_type = std::get<std::int64_t>(params.at("dtype"));
  if (raw_type < 0 || raw_type > UINT32_MAX)
    return Answer(core_internal::invalid_schema_domain(
        "unsupported typed literal dtype"));
  const auto type = static_cast<ElementType>(raw_type);
  if (type != ElementType::UInt8 && type != ElementType::Int8 &&
      type != ElementType::UInt16 && type != ElementType::Int16 &&
      type != ElementType::Int64 && type != ElementType::Float32 &&
      type != ElementType::Float64)
    return Answer(core_internal::invalid_schema_domain(
        "unsupported typed literal dtype"));
  const auto width = TensorSpan::element_size(type);
  const auto& bits = std::get<std::string>(params.at("bits"));
  if (!width || bits.size() != width * 2)
    return Answer(core_internal::invalid_schema_domain(
        "typed literal dtype/bit width mismatch"));
  std::vector<std::uint8_t> result;
  for (std::size_t i = 0; i < bits.size(); i += 2) {
    unsigned byte = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const auto c = bits[i + j];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return Answer(core_internal::invalid_schema_domain(
            "typed literal requires lowercase hex bytes"));
      byte = byte * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
    }
    result.push_back(byte);
  }
  return Answer(std::move(result));
}
OperationDefinition scalar_literal(const std::string& key,
                                   SequenceProfile profile) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  t.input_count = 0;
  t.requires_metadata_specialization = true;
  t.parameter_schema = {{"dtype", OperationParameterType::Int64},
                        {"bits", OperationParameterType::String}};
  auto& out = t.outputs[0];
  out.key = "values";

  out.output_schema.element_type_mask = 127;
  out.result_schema = tensor_ops::scalar_schema();
  op.prepare_static =
      [profile](const auto&,
                const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    auto status = numeric_ops::sequence_profile_available(profile);
    if (!status.ok())
      return Answer(status);
    auto bytes = literal_bytes(parameters);
    if (!bytes.ok())
      return Answer(bytes.status());
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor.element_type = static_cast<ElementType>(
        std::get<std::int64_t>(parameters.at("dtype")));
    OperationOutputSpecialization output;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    OperationPreparation prepared;
    prepared.outputs.push_back(std::move(output));
    auto state = std::make_shared<ScalarState>();
    state->width = bytes.value().size();
    std::copy(bytes.value().begin(), bytes.value().end(), state->bits.begin());
    prepared.state = std::move(state);
    return Answer(std::move(prepared));
  };
  op.lower = [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task = builder.add_task("literal", "channel-scalar-bits-v1");
        if (!task.ok())
          return Result<OperationProgram>(task.status());
        auto status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
    return builder.finish();
  };
  op.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != "literal" ||
        kernel.contract != "channel-scalar-bits-v1" || !query.inputs.empty() ||
        !query.reads.empty())
      return Answer({ErrorCode::TypeMismatch, "invalid scalar literal kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer(
          {ErrorCode::BackendUnavailable, "scalar literal requires CPU Host"});
    Region region;
    auto valid = output_signature(query, &region);
    if (!valid.ok())
      return Answer(valid);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context =
        new ScalarState(*static_cast<const ScalarState*>(query.static_state));
    result.release = [](const void* context) {
      delete static_cast<const ScalarState*>(context);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      if (invocation->cancellation.is_cancelled &&
          invocation->cancellation.is_cancelled(
              invocation->cancellation.context)) {
        *status = kernel_failure(ErrorCode::Cancelled);
        return;
      }
      const auto& state =
          *static_cast<const ScalarState*>(invocation->prepared_context);
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          state.bits.data(), state.width);
      *status = {};
    };
    return Answer(result);
  };
  return op;
}
Result<std::optional<TensorDescription>> edit_description(
    const OperationMetadata& metadata, std::uint32_t port,
    const format::ChannelAssemblyOptions& options) {
  using Answer = Result<std::optional<TensorDescription>>;
  std::optional<TensorDescription> result;
  if (options.input_descriptions.count(port)) {
    result = options.input_descriptions.at(port);
  }
  if (result) {
    auto valid =
        validate_tensor_description(*result, tensor(metadata).descriptor);
    if (!valid.ok()) {
      return Answer(valid);
    }
  }
  return Answer(std::move(result));
}
Result<std::uint64_t> edit_select(const format::ChannelSelector& selector,
                                  std::uint64_t count,
                                  const std::optional<TensorDescription>& desc,
                                  bool channels, bool raw) {
  using Answer = Result<std::uint64_t>;
  std::uint64_t selected = 0;
  if (selector.match == "index") {
    if (!number(selector.value, &selected) || selected >= count)
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 index outside source/destination"));
    return Answer(selected);
  }
  if (raw || (selector.match != "name" && selector.match != "role") ||
      selector.value.empty() || selector.value.size() > 128 || !desc ||
      (channels ? desc->channels.size() != count : !desc->component))
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 invalid name/role selector"));
  bool found = false;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto& c = channels ? desc->channels[i] : *desc->component;
    if ((selector.match == "name" ? c.name : c.role) == selector.value) {
      if (found)
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03 ambiguous name/role selector"));
      selected = i;
      found = true;
    }
  }
  return found ? Answer(selected)
               : Answer(core_internal::invalid_schema_domain(
                     "FMT-03 selector absent"));
}
Result<WorkflowNodeOutput> edit_channels(
    WorkflowDocument& document, std::vector<format::ChannelEditInput> inputs,
    const std::vector<format::ChannelEditSource>& slots,
    const std::vector<format::ChannelReplacement>* replacements,
    const format::ChannelAssemblyOptions& options) try {
  using Answer = Result<WorkflowNodeOutput>;
  if (inputs.empty() || inputs.size() > 1024 || inputs[0].structure.component ||
      inputs[0].structure.scalar)
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 invalid base/metadata mode/arity"));
  if (!replacements) {
    for (std::size_t i = 1; i < inputs.size(); ++i)
      if (!inputs[i].structure.scalar)
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03A external inputs must be explicit scalars"));
  }
  for (const auto& entry : options.input_descriptions)
    if (entry.first >= inputs.size())
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 override input ordinal is absent"));
  const auto& base = tensor(inputs[0].metadata);
  const auto dtype = base.descriptor.element_type;
  if (dtype != ElementType::UInt8 && dtype != ElementType::Int8 &&
      dtype != ElementType::UInt16 && dtype != ElementType::Int16 &&
      dtype != ElementType::Int64 && dtype != ElementType::Float32 &&
      dtype != ElementType::Float64)
    return Answer(mismatch("FMT-03 unsupported base dtype"));
  if (base.descriptor.shape.empty() || base.descriptor.shape.size() > 8)
    return Answer(mismatch("FMT-03 base requires positive rank 1..8"));
  auto base_description = edit_description(inputs[0].metadata, 0, options);
  if (!base_description.ok())
    return Answer(base_description.status());
  auto axis = inputs[0].structure.axis;
  auto desc = base_description.take_value();
  if (desc && desc->channel_axis) {
    if (axis && axis != desc->channel_axis)
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 base axis disagrees with metadata"));
    axis = desc->channel_axis;
  }
  if (!axis || *axis >= base.descriptor.shape.size())
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 base axis required/in range"));
  inputs[0].structure.axis = axis;
  const auto count = base.descriptor.shape[*axis];
  const auto output_count = replacements ? count : slots.size();
  if (!output_count || output_count > 1024 || count > (1ULL << 40))
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 empty slots or mapping capacity exceeded"));
  std::vector<format::ChannelEditSource> effective = slots;
  std::set<std::uint64_t> changed;
  if (replacements) {
    effective.resize(count);
    for (std::uint64_t i = 0; i < count; ++i)
      effective[i].selector.value = std::to_string(i);
    for (const auto& r : *replacements) {
      auto destination = edit_select(r.destination, count, desc, true, false);
      if (!destination.ok())
        return Answer(destination.status());
      if (!changed.insert(destination.value()).second)
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03B duplicate destination"));
      effective[destination.value()] = r.source;
    }
  }
  WorkflowDocument staged = document;
  std::vector<WorkflowInput> references;
  for (const auto& input : inputs)
    references.push_back(input.input);
  std::set<std::vector<std::uint8_t>> unique_literals;
  for (const auto& e : effective)
    if (e.literal)
      unique_literals.insert(e.literal->bytes);
  auto ids = numeric::available_workflow_node_ids(
      document, static_cast<unsigned>(1 + unique_literals.size()), references);
  if (!ids.ok())
    return Answer(ids.status());
  unsigned next_id = 0;
  std::map<std::string, std::uint32_t> literals;
  for (auto& e : effective) {
    if (!e.literal)
      continue;
    const auto& literal = *e.literal;
    if (literal.dtype != base.descriptor.element_type)
      return Answer(mismatch("FMT-03 literal dtype differs from base"));
    const auto width = TensorSpan::element_size(literal.dtype);
    if (!width || literal.bytes.size() != width)
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 malformed typed literal"));
    const auto bits = format::detail::assembly_hex(
        std::string(literal.bytes.begin(), literal.bytes.end()));
    if (!literals.count(bits)) {
      const auto id = ids.value()[next_id++];
      staged.nodes.push_back(
          {id,
           "channel.scalar_literal_" + options.profile,
           {},
           {{"dtype", static_cast<std::int64_t>(literal.dtype)},
            {"bits", bits}}});
      format::ChannelEditInput input;
      input.input = WorkflowNodeOutput{id, "values"};
      auto scalar_schema = tensor_ops::scalar_schema();
      scalar_schema.tensors[0].descriptor = {literal.dtype, {1}};
      input.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(scalar_schema));
      input.structure.scalar = true;
      literals[bits] = inputs.size();
      inputs.push_back(std::move(input));
    }
    e.input = literals.at(bits);
    e.selector = {};
  }
  if (inputs.size() > 1024)
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 expanded input capacity exceeded"));
  std::vector<OperationMetadata> metadata;
  std::vector<format::ChannelEditStructure> structures;
  references.clear();
  for (const auto& input : inputs) {
    if ((input.structure.component && input.structure.axis) ||
        (input.structure.scalar &&
         (input.structure.component || input.structure.axis)))
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 conflicting source structure"));
    require(tensor_ops::check_tensor(input.metadata));
    // Supplied declaration metadata is checked before committing the expansion.
    if (const auto* ref = std::get_if<WorkflowInputReference>(&input.input)) {
      bool found = false;
      for (const auto& declaration : document.inputs)
        if (declaration.id == ref->input_id) {
          OperationMetadata actual;
          actual.result_schema = declaration.result_schema;
          if (assembly_source_assertion({actual}) !=
              assembly_source_assertion({input.metadata}))
            return Answer(core_internal::invalid_schema_domain(
                "FMT-03 descriptor disagrees with declaration"));
          found = true;
        }
      if (!found)
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03 input declaration absent"));
    }
    metadata.push_back(input.metadata);
    references.push_back(input.input);
    structures.push_back(input.structure);
  }
  std::vector<format::ChannelMapping> mapping;
  for (std::size_t k = 0; k < effective.size(); ++k) {
    const auto& e = effective[k];
    if (e.input >= inputs.size() ||
        (!replacements && e.input != 0 && !inputs[e.input].structure.scalar))
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03A source must be base or scalar"));
    auto d = edit_description(metadata[e.input], e.input, options);
    if (!d.ok())
      return Answer(d.status());
    const auto& structure = structures[e.input];
    auto a = structure.axis;
    if (d.value() && d.value()->channel_axis && !structure.scalar &&
        !structure.component)
      a = d.value()->channel_axis;
    const auto& shape = tensor(metadata[e.input]).descriptor.shape;
    if (shape.empty() || (a && *a >= shape.size()))
      return Answer(mismatch("FMT-03 source rank/axis mismatch"));
    const auto n = a ? shape[*a] : 1;
    auto selected = edit_select(e.selector, n, d.value(), a.has_value(), false);
    if (!selected.ok())
      return Answer(selected.status());
    mapping.push_back({e.input, "index", std::to_string(selected.value()), k});
  }
  std::vector<format::ChannelSourceStructure> primitive_structures;
  std::string fused_structure = "v1";
  for (const auto& structure : structures) {
    primitive_structures.push_back({structure.component, structure.axis});
    fused_structure +=
        structure.scalar ? ";s"
        : structure.component
            ? ";c"
            : ";h" + (structure.axis ? std::to_string(*structure.axis) : "_");
  }
  auto output = format::assemble_mapped_channels(
      staged, references, *axis, primitive_structures, mapping, options);
  if (!output.ok())
    return Answer(output.status());
  auto& node = staged.nodes.back();
  node.parameters["input_structure"] = fused_structure;
  node.parameters["expected_inputs"] = assembly_source_assertion(metadata);
  node.parameters["authoring_member"] =
      std::string(replacements ? "FMT-03B" : "FMT-03A");
  for (const auto& n : staged.nodes)
    for (const auto& p : n.parameters)
      if (const auto* value = std::get_if<std::string>(&p.second);
          value && value->size() > 8192)
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03 parameter capacity exceeded"));
  const auto profile = options.profile == "strict" ? SequenceProfile::Strict
                       : options.profile == "accelerated_apple_silicon"
                           ? SequenceProfile::AppleSilicon
                           : SequenceProfile::X86Avx2;
  auto checked = prepare(metadata, node.parameters, 2, profile);
  if (!checked.ok())
    return Answer(checked.status());
  document = std::move(staged);
  return output;
} catch (const Status& status) {
  return Result<WorkflowNodeOutput>(status);
} catch (const std::bad_alloc&) {
  return Result<WorkflowNodeOutput>(Status{
      ErrorCode::ResourceExhausted, "channel authoring allocation failed"});
}
}  // namespace
Result<OperationPreparation> prepare_alpha_channel_mapping(
    const std::vector<OperationMetadata>& inputs,
    const format_result::Params& p, numeric_ops::SequenceProfile profile) {
  return prepare(inputs, p, 2, profile);
}
Status register_channel_assembly(OperationRegistry* registry) {
  for (const auto& p :
       {std::make_pair("strict", SequenceProfile::Strict),
        std::make_pair("accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto literal_status = registry->register_operation(scalar_literal(
        std::string("channel.scalar_literal_") + p.first, p.second));
    if (!literal_status.ok())
      return literal_status;
    int member = 0;
    for (const auto* name : {"assemble", "concatenate", "assemble_mapped"}) {
      auto status = registry->register_operation(definition(
          std::string("channel.") + name + "_" + p.first, member++, p.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal

namespace ps::format {
Result<WorkflowNodeOutput> swizzle_channels(
    WorkflowDocument& document, const std::vector<ChannelEditInput>& inputs,
    const std::vector<ChannelEditSource>& slots,
    const ChannelAssemblyOptions& options) {
  return plugin_internal::edit_channels(document, inputs, slots, nullptr,
                                        options);
}
Result<WorkflowNodeOutput> replace_channels(
    WorkflowDocument& document, const std::vector<ChannelEditInput>& inputs,
    const std::vector<ChannelReplacement>& replacements,
    const ChannelAssemblyOptions& options) {
  return plugin_internal::edit_channels(document, inputs, {}, &replacements,
                                        options);
}
}  // namespace ps::format
