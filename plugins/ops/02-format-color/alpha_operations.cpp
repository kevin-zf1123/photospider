#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/alpha_common.hpp"
#include "02-format-color/alpha_math.hpp"
#include "02-format-color/alpha_numeric_metadata.hpp"
#include "common/fixed_point_inputs.hpp"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps::plugin_internal {
namespace alpha_ops {
struct Slot final {
  std::int64_t source = 0;
  bool color = false;
  bool alpha = false;
};
struct Map final {
  std::uint32_t port = 0;
  std::vector<ResultViewAxis> axes;
};
struct Span final {
  std::uint64_t first = 0, count = 1;
  Map map;
};
struct RuntimeMap final {
  unsigned port = 0, rank = 0;
  std::array<std::int8_t, 8> axes{};
  std::array<std::uint64_t, 8> origins{};
};
struct RuntimeSlot final {
  RuntimeMap data;
  bool color = false;
};
struct RuntimeMaps final {
  std::vector<RuntimeSlot> slots;
  RuntimeMap alpha;
};
RuntimeMap runtime_map(const Map& map) {
  RuntimeMap result;
  result.port = map.port;
  result.rank = map.axes.size();
  for (unsigned axis = 0; axis < result.rank; ++axis) {
    result.axes[axis] = map.axes[axis].output_axis;
    result.origins[axis] = map.axes[axis].source_origin;
  }
  return result;
}
struct Preparation final {
  Action action = Action::Associate;
  bool raw = false, narrow = true, simd = false, reference = false;
  bool scalar = false, external = false, component = false;
  std::optional<std::uint32_t> input_axis, output_axis;
  std::uint64_t alpha_index = 0;
  std::string group, layout;
  std::vector<Slot> slots;
  std::vector<Map> data_maps;
  Map alpha_map;
  std::vector<Span> spans;
  std::shared_ptr<const RuntimeMaps> runtime;
  std::shared_ptr<const SchemaTemplate> output_schema;
};
Map mapping(const Preparation& state,
            const std::vector<OperationMetadata>& inputs, bool alpha,
            std::uint64_t source) {
  Map map;
  map.port = alpha && state.external ? 1 : 0;
  const auto& input = inputs[map.port].result_schema->tensors[0];
  const auto batches = inputs[0].result_schema->tensors[0].batch_axes.size();
  const auto source_axis =
      map.port == 0 ? state.input_axis : std::optional<std::uint32_t>{};
  for (std::size_t axis = 0; axis < input.sample_shape().size(); ++axis) {
    ResultViewAxis item;
    if (alpha && state.scalar) {
      item.source_origin = 0;
    } else if (axis < batches) {
      item.output_axis = axis;
    } else if (source_axis && axis - batches == *source_axis) {
      item.source_origin = source;
    } else {
      const auto spatial =
          axis - batches -
          (source_axis && axis - batches > *source_axis ? 1 : 0);
      item.output_axis =
          batches + spatial +
          (state.output_axis && spatial >= *state.output_axis ? 1 : 0);
    }
    map.axes.push_back(item);
  }
  return map;
}
bool same_axis(const ResultViewAxis& a, const ResultViewAxis& b) {
  return a.output_axis == b.output_axis && a.source_origin == b.source_origin &&
         a.step == b.step && a.output_origin == b.output_origin;
}
std::optional<Map> merge_map(const Map& first, const Map& second,
                             std::uint32_t output_axis, std::uint64_t start) {
  if (first.port != second.port || first.axes.size() != second.axes.size())
    return {};
  auto result = first;
  for (std::size_t i = 0; i < first.axes.size(); ++i) {
    const auto& a = first.axes[i];
    const auto& b = second.axes[i];
    if (a.output_axis >= 0 || b.output_axis >= 0) {
      if (!same_axis(a, b))
        return {};
    } else {
      const auto delta =
          static_cast<__int128>(b.source_origin) - a.source_origin;
      if (delta < -1 || delta > 1)
        return {};
      if (delta) {
        result.axes[i].output_axis = output_axis;
        result.axes[i].output_origin = start;
        result.axes[i].step = static_cast<std::int64_t>(delta);
      }
    }
  }
  return result;
}
bool matches_map(const Map& merged, const Map& item, std::uint32_t output_axis,
                 std::uint64_t channel) {
  if (merged.port != item.port || merged.axes.size() != item.axes.size())
    return false;
  for (std::size_t i = 0; i < item.axes.size(); ++i) {
    const auto& a = merged.axes[i];
    const auto& b = item.axes[i];
    if (a.output_axis == static_cast<std::int32_t>(output_axis) &&
        b.output_axis < 0) {
      auto coordinate = a.source_coordinate(channel);
      if (!coordinate.ok() || coordinate.value() != b.source_origin)
        return false;
    } else if (!same_axis(a, b)) {
      return false;
    }
  }
  return true;
}
void prepare_spans(Preparation* s, std::size_t batches) {
  const auto axis =
      static_cast<std::uint32_t>(batches + s->output_axis.value_or(0));
  for (std::size_t first = 0; first < s->data_maps.size();) {
    std::size_t end = first + 1;
    Map map = s->data_maps[first];
    if (s->output_axis && end < s->data_maps.size()) {
      auto merged = merge_map(map, s->data_maps[end], axis, first);
      if (merged) {
        map = std::move(*merged);
        ++end;
        while (end < s->data_maps.size() &&
               matches_map(map, s->data_maps[end], axis, end))
          ++end;
      }
    }
    s->spans.push_back({first, end - first, std::move(map)});
    first = end;
  }
  if (s->action == Action::Set)
    return;
  for (std::size_t first = 0; first < s->slots.size();) {
    if (!s->slots[first].color) {
      ++first;
      continue;
    }
    auto end = first + 1;
    while (end < s->slots.size() && s->slots[end].color)
      ++end;
    s->spans.push_back({first, end - first, s->alpha_map});
    first = end;
  }
}
Result<OperationPreparation> prepare_alpha(
    const std::vector<OperationMetadata>& inputs, const Params& p,
    Action action, numeric_ops::SequenceProfile profile) try {
  using Answer = Result<OperationPreparation>;
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok()) {
    return Answer(available);
  }
  if (inputs.empty() || inputs.size() > 2) {
    return Answer(invalid("expected input and optional alpha"));
  }
  for (const auto& m : inputs) {
    auto status = shape_valid(m);
    if (!status.ok()) {
      return Answer(status);
    }
    if ((tensor(m).descriptor.element_type != ElementType::Float32 &&
         tensor(m).descriptor.element_type != ElementType::Float64) ||
        tensor(m).descriptor.element_type !=
            tensor(inputs[0]).descriptor.element_type) {
      return Answer(mismatch(
          "arithmetic/set require matching Float32 or Float64 inputs"));
    }
  }
  const auto& main = inputs[0].result_schema->tensors[0];
  Preparation s;
  s.action = action;
  s.narrow = main.descriptor.element_type == ElementType::Float32;
  s.raw = !p.count("input_description");
  s.group = text(p, "group");
  s.layout = text(p, "layout", action == Action::Set ? "auto" : "materialize");
  if (s.layout != "auto" && s.layout != "view" && s.layout != "materialize") {
    return Answer(invalid("unknown layout"));
  }
  const auto algorithm = text(p, "algorithm", "auto");
  if (algorithm != "auto" && algorithm != "scalar" && algorithm != "simd" &&
      algorithm != "reference") {
    return Answer(invalid("unknown arithmetic algorithm"));
  }
  if (algorithm == "simd" && !simd_available()) {
    return Answer(
        Status{ErrorCode::BackendUnavailable, "alpha SIMD is unavailable"});
  }
  s.simd =
      algorithm == "simd" ||
      (algorithm == "auto" && profile != numeric_ops::SequenceProfile::Strict);
  s.reference = algorithm == "reference";
  auto description = effective_description(inputs[0], p, action != Action::Set);
  if (!description.ok()) {
    return Answer(description.status());
  }
  auto desc = description.take_value();
  std::optional<SemanticSource> semantic;
  std::vector<std::uint64_t> colors;
  if (!s.raw) {
    auto resolved = semantic_source(inputs[0], p, action != Action::Set,
                                    action == Action::Set);
    if (!resolved.ok()) {
      return Answer(resolved.status());
    }
    semantic = resolved.take_value();
    s.input_axis = semantic->axis;
    s.component = semantic->component;
    colors = semantic->group.indices;
    if (action != Action::Set) {
      const auto& g = semantic->group;
      if ((g.interpretation.model != "rgb" &&
           g.interpretation.model != "gray") ||
          !g.alpha) {
        return Answer(mismatch(
            "association requires an RGB/Gray group with internal alpha"));
      }
      const auto state = g.interpretation.association.empty()
                             ? "straight"
                             : g.interpretation.association;
      if (state !=
          (action == Action::Associate ? "straight" : "premultiplied")) {
        return Answer(mismatch(
            "selected group is not in the required source association state"));
      }
      s.alpha_index = *g.alpha;
    }
    if (p.count("components")) {
      return Answer(invalid("components is raw-only"));
    }
    if (p.count("input_structure") &&
        text(p, "input_structure") !=
            (s.component ? "component" : "channels")) {
      return Answer(invalid("input_structure contradicts source description"));
    }
  } else {
    if (p.count("group")) {
      return Answer(invalid("group is not a raw selector"));
    }
    const auto structure = text(p, "input_structure");
    if (structure != "component" && structure != "channels") {
      return Answer(
          invalid("raw requires explicit component/channels structure"));
    }
    s.component = structure == "component";
    if (s.component) {
      if (p.count("axis")) {
        return Answer(invalid("raw component forbids axis"));
      }
    } else {
      if (!p.count("axis")) {
        return Answer(invalid("raw channels requires axis"));
      }
      const auto axis = std::get<std::int64_t>(p.at("axis"));
      if (axis < 0 ||
          static_cast<std::uint64_t>(axis) >= main.descriptor.shape.size()) {
        return Answer(invalid("raw axis out of rank"));
      }
      s.input_axis = static_cast<std::uint32_t>(axis);
    }
    const auto list = text(p, "components");
    std::set<std::uint64_t> seen;
    for (std::size_t begin = 0; begin <= list.size();) {
      const auto end = list.find(',', begin);
      std::uint64_t index = 0;
      if (!number(
              list.substr(begin, end == std::string::npos ? end : end - begin),
              &index) ||
          index >= (s.input_axis ? main.descriptor.shape[*s.input_axis] : 1) ||
          !seen.insert(index).second) {
        return Answer(invalid("raw components must be distinct valid indices"));
      }
      colors.push_back(index);
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    }
  }
  const auto count = s.input_axis ? main.descriptor.shape[*s.input_axis] : 1;
  if (count > 65536) {
    return Answer(invalid("channel mapping capacity exceeded"));
  }
  const auto kind = text(p, "alpha_source",
                         !s.raw && action != Action::Set ? "internal" : "");
  if (kind != "internal" && kind != "external_plane" && kind != "scalar") {
    return Answer(
        invalid("alpha_source must be internal, external_plane or scalar"));
  }
  s.external = kind != "internal";
  s.scalar = kind == "scalar";
  if (s.external != (inputs.size() == 2)) {
    return Answer(invalid("alpha source/port arity mismatch"));
  }
  if (!s.raw && action != Action::Set && s.external) {
    return Answer(
        mismatch("semantic association forbids an external alpha port"));
  }
  if (s.external) {
    if (p.count("alpha_match") || p.count("alpha_selector")) {
      return Answer(invalid("external alpha forbids internal selector"));
    }
    auto spatial = main.descriptor.shape;
    if (s.input_axis) {
      spatial.erase(spatial.begin() + *s.input_axis);
    }
    if (!s.scalar && spatial.empty()) {
      return Answer(invalid("rank-zero external alpha plane is forbidden"));
    }
    const auto expected = s.scalar ? std::vector<std::uint64_t>{1} : spatial;
    if ((s.scalar && !tensor(inputs[1]).batch_axes.empty()) ||
        (!s.scalar && tensor(inputs[1]).batch_axes != main.batch_axes) ||
        tensor(inputs[1]).descriptor.shape != expected ||
        (s.scalar && tensor(inputs[1]).layout.spatial)) {
      return Answer(mismatch("external alpha shape/structure mismatch"));
    }
    if (!s.scalar) {
      Params source_params;
      auto alpha_desc = effective_description(inputs[1], source_params, true);
      if (!alpha_desc.ok()) {
        return Answer(alpha_desc.status());
      }
      if (alpha_desc.value() && alpha_desc.value()->channel_axis) {
        return Answer(invalid("external alpha must be a component plane"));
      }
      if (desc && alpha_desc.value() && !desc->axes.empty() &&
          !alpha_desc.value()->axes.empty()) {
        auto axes = desc->axes;
        if (s.input_axis) {
          axes.erase(axes.begin() + *s.input_axis);
        }
        for (std::size_t i = 0; i < axes.size(); ++i) {
          const auto& a = axes[i];
          const auto& b = alpha_desc.value()->axes[i];
          if ((!a.name.empty() && !b.name.empty() && a.name != b.name) ||
              (!a.unit.empty() && !b.unit.empty() && a.unit != b.unit) ||
              a.origin != b.origin || a.step != b.step) {
            return Answer(invalid("external alpha coordinate grid conflicts"));
          }
        }
      }
      if (desc && alpha_desc.value()) {
        const auto grid = alpha_desc.value()->component &&
                                  alpha_desc.value()->component->sampling
                              ? alpha_desc.value()->component->sampling
                              : alpha_desc.value()->sampling;
        if (grid && desc->sampling && grid->grid != desc->sampling->grid) {
          return Answer(invalid("external alpha sampling grid conflicts"));
        }
      }
    }
  } else {
    const bool selector = p.count("alpha_match") || p.count("alpha_selector");
    if (selector && (!p.count("alpha_match") || !p.count("alpha_selector"))) {
      return Answer(
          invalid("internal selector requires both match and selector"));
    }
    if (!selector && (s.raw || action == Action::Set)) {
      return Answer(invalid("explicit internal alpha selector is required"));
    }
    if (selector) {
      if (s.raw && text(p, "alpha_match") != "index" && desc &&
          desc->channel_axis != s.input_axis) {
        return Answer(
            invalid("named raw alpha selector axis disagrees with metadata"));
      }
      auto selected = select(
          {text(p, "alpha_match"), text(p, "alpha_selector")}, count, desc);
      if (!selected.ok()) {
        return Answer(selected.status());
      }
      if (!s.raw && action != Action::Set &&
          selected.value() != s.alpha_index) {
        return Answer(
            invalid("alpha selector differs from selected group's alpha"));
      }
      s.alpha_index = selected.value();
    }
  }
  if (!s.raw) {
    std::optional<TensorDescription> alpha_description = desc;
    if (s.external) {
      Params source_params;
      auto resolved = effective_description(inputs[1], source_params, true);
      if (!resolved.ok()) {
        return Answer(resolved.status());
      }
      alpha_description = resolved.take_value();
      if (alpha_description && alpha_description->channel_axis) {
        return Answer(
            invalid("external alpha/scalar requires component metadata"));
      }
    }
    std::optional<TensorEncoding> encoding;
    std::optional<TensorSampling> sampling;
    if (alpha_description) {
      encoding = alpha_description->encoding;
      sampling = alpha_description->sampling;
      const TensorChannelDescription* channel = nullptr;
      if (s.external || s.component) {
        if (alpha_description->component) {
          channel = &*alpha_description->component;
        }
      } else if (s.alpha_index < alpha_description->channels.size()) {
        channel = &alpha_description->channels[s.alpha_index];
      }
      if (channel) {
        if (channel->encoding) {
          encoding = channel->encoding;
        }
        if (channel->sampling) {
          sampling = channel->sampling;
        }
      }
    }
    auto normalized = normalized_alpha_encoding(encoding);
    if (!normalized.ok()) {
      return Answer(normalized);
    }
    if (!s.scalar && sampling) {
      auto check = [&](const std::optional<TensorSampling>& grid) {
        return !grid || grid->grid == sampling->grid;
      };
      if (!check(desc->sampling)) {
        return Answer(invalid("alpha and color sampling grids conflict"));
      }
      for (std::size_t k = 0; k < semantic->group.indices.size(); ++k) {
        const auto index = semantic->group.indices[k];
        if (!check(semantic->group.components[k].sampling) ||
            (index < desc->channels.size() &&
             !check(desc->channels[index].sampling))) {
          return Answer(invalid("alpha and color sampling grids conflict"));
        }
      }
    }
  }
  OperationOutputSpecialization output;
  output.metadata = inputs[0];
  auto schema = *inputs[0].result_schema;
  auto& output_spec = schema.tensors[0];
  output_spec.atomic_trailing_axes = 0;
  s.output_axis = s.input_axis;
  std::vector<std::int64_t> source_map;
  if (action == Action::Set) {
    const auto placement = text(p, "placement", "preserve");
    if (placement != "preserve" && placement != "channel") {
      return Answer(invalid("invalid alpha placement"));
    }
    const auto old = semantic->group.alpha;
    const bool shared = old && referenced_elsewhere(*semantic, *old);
    if (placement == "preserve" &&
        (p.count("channel_index") || !old || shared)) {
      return Answer(
          invalid("preserve requires a private existing alpha and forbids "
                  "channel_index"));
    }
    if ((placement == "channel") != (p.count("channel_index") != 0)) {
      return Answer(invalid("channel placement requires channel_index"));
    }
    if (s.component) {
      if (!p.count("output_axis")) {
        return Answer(invalid("component Gray requires output_axis"));
      }
      const auto axis = std::get<std::int64_t>(p.at("output_axis"));
      if (axis < 0 ||
          static_cast<std::uint64_t>(axis) > main.descriptor.shape.size() ||
          main.sample_shape().size() == 8) {
        return Answer(invalid("inserted output axis out of rank"));
      }
      s.output_axis = static_cast<std::uint32_t>(axis);
      output_spec.descriptor.shape.insert(
          output_spec.descriptor.shape.begin() + *s.output_axis, 1);
      if (output_spec.layout.spatial) {
        auto& l = output_spec.layout;
        if (l.height_axis >= *s.output_axis) {
          ++l.height_axis;
        }
        if (l.width_axis >= *s.output_axis) {
          ++l.width_axis;
        }
        l.channel_axis = s.output_axis;
      }
    } else if (p.count("output_axis")) {
      return Answer(invalid("channel-bearing input forbids output_axis"));
    }
    for (std::uint64_t i = 0; i < count; ++i) {
      if (!old || i != *old || shared) {
        source_map.push_back(static_cast<std::int64_t>(i));
      }
    }
    std::uint64_t target = old.value_or(0);
    if (placement == "channel") {
      const auto position = std::get<std::int64_t>(p.at("channel_index"));
      if (position < 0 ||
          static_cast<std::uint64_t>(position) > source_map.size()) {
        return Answer(invalid("final alpha channel_index is outside output"));
      }
      target = static_cast<std::uint64_t>(position);
    }
    source_map.insert(source_map.begin() + target, -1);
    output_spec.descriptor.shape[*s.output_axis] = source_map.size();
    auto remapped =
        remap_description(*semantic, source_map, *s.output_axis, target);
    if (!remapped.ok()) {
      return Answer(remapped.status());
    }
    desc = remapped.take_value();
  } else {
    for (std::uint64_t i = 0; i < count; ++i) {
      source_map.push_back(static_cast<std::int64_t>(i));
    }
    if (desc && !s.raw) {
      auto& selected = desc->groups[semantic->group_index];
      selected.interpretation.association =
          action == Action::Associate ? "premultiplied" : "straight";
      for (auto c : selected.indices) {
        if (c < desc->channels.size() && desc->channels[c].interpretation) {
          desc->channels[c].interpretation->association =
              selected.interpretation.association;
        }
      }
      const bool boundary = std::any_of(
          desc->groups.begin(), desc->groups.end(), [](const auto& g) {
            return g.interpretation.association == "premultiplied";
          });
      desc->association = boundary ? "premultiplied" : "straight";
    }
  }
  if (desc) {
    auto valid = validate_tensor_description(*desc, output_spec.descriptor);
    if (!valid.ok()) {
      return Answer(valid);
    }
  }
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  s.output_schema = output.metadata.result_schema;
  auto valid_shape = shape_valid(output.metadata);
  if (!valid_shape.ok()) {
    return Answer(valid_shape);
  }
  s.alpha_map = mapping(s, inputs, true, s.alpha_index);
  for (std::size_t k = 0; k < source_map.size(); ++k) {
    const auto original = source_map[k];
    Slot slot{original,
              original >= 0 && std::find(colors.begin(), colors.end(),
                                         original) != colors.end(),
              original < 0};
    s.slots.push_back(slot);
    auto data = mapping(
        s, inputs, slot.alpha,
        slot.alpha ? s.alpha_index : static_cast<std::uint64_t>(original));
    s.data_maps.push_back(std::move(data));
  }
  auto runtime = std::make_shared<RuntimeMaps>();
  runtime->alpha = runtime_map(s.alpha_map);
  for (std::size_t i = 0; i < s.slots.size(); ++i)
    runtime->slots.push_back({runtime_map(s.data_maps[i]), s.slots[i].color});
  s.runtime = std::move(runtime);
  prepare_spans(&s, main.batch_axes.size());
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::make_shared<Preparation>(std::move(s));
  return Answer(std::move(result));
} catch (const Status& status) {
  return Result<OperationPreparation>(status);
}
std::optional<std::uint32_t> output_axis(const Preparation& s,
                                         const ResultTensorSpec& spec) {
  return s.output_axis
             ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                   spec.batch_axes.size() + *s.output_axis)}
             : std::nullopt;
}
struct SampleBlock final {
  std::array<std::uint64_t, 128> colors{}, alpha{}, output{}, offsets{};
};
struct ReferenceScratch final {
  numeric_ops::RatioWorkspace arithmetic{numeric_ops::SequenceProfile::Strict};
  SampleBlock block;
};
bool calculates(const Preparation& state, const Region& output,
                std::optional<std::uint32_t> axis) {
  if (state.action == Action::Set || state.layout == "view")
    return false;
  const auto channels =
      axis ? output.dimensions()[*axis] : RegionDimension{0, 1};
  for (auto k = channels.offset; k < channels.offset + channels.extent; ++k)
    if (state.slots[k].color)
      return true;
  return false;
}
std::uint64_t scratch_bytes(const Preparation& state, const Region& output,
                            std::optional<std::uint32_t> axis) {
  return !calculates(state, output, axis) ? 0
         : state.reference                ? sizeof(ReferenceScratch)
                                          : sizeof(SampleBlock);
}
Result<std::set<TileIndex>> required_tiles(
    const Preparation& state, const Region& output,
    std::optional<std::uint32_t> channel_axis, const TileGrid& grid,
    unsigned port, const ResourceBudget& budget, std::uint64_t maximum_tiles) {
  using Answer = Result<std::set<TileIndex>>;
  std::set<TileIndex> result;
  if (state.layout == "view")
    return Answer(std::move(result));
  for (const auto& span : state.spans) {
    auto charged = budget.consume({1});
    if (!charged.ok())
      return Answer(charged);
    if (span.map.port != port)
      continue;
    auto dimensions = output.dimensions();
    if (channel_axis) {
      auto& c = dimensions[*channel_axis];
      const auto first = std::max(c.offset, span.first);
      const auto end = std::min(c.offset + c.extent, span.first + span.count);
      if (first >= end)
        continue;
      c = {first, end - first};
    }
    auto region = format_result::mapped_region(Region(std::move(dimensions)),
                                               span.map.axes);
    // Bound each rectangular closure before constructing a TileSet, including
    // malformed imported tasks that supplied fewer read slots than required.
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < grid.rank(); ++axis) {
      const auto d = region.dimensions()[axis];
      std::uint64_t extent = 1;
      if (grid.batch_axis_mask() & (1U << axis))
        extent = d.extent;
      else if (axis == grid.height_axis() || axis == grid.width_axis())
        extent = (d.offset + d.extent - 1) / grid.tile_extent() -
                 d.offset / grid.tile_extent() + 1;
      if (extent > maximum_tiles / count)
        return Answer({ErrorCode::ResourceExhausted, "alpha read tile limit"});
      count *= extent;
    }
    auto tiles = sdk::tiles_for_region(budget, grid, region);
    if (!tiles.ok())
      return Answer(tiles.status());
    for (auto index : tiles.value()) {
      auto status = budget.consume({1});
      if (!status.ok())
        return Answer(status);
      result.insert(index);
      if (result.size() > maximum_tiles)
        return Answer({ErrorCode::ResourceExhausted, "alpha read union limit"});
    }
  }
  return Answer(std::move(result));
}
constexpr const char* alpha_contract = "alpha-fixed-v1";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const Preparation*>(query.static_state);
  const auto axis =
      output_axis(state, query.outputs[0].result_schema->tensors[0]);
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 2> grids;
  for (unsigned port = 0; port < query.inputs.size(); ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    grids[port] = grid.take_value();
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      auto task =
          builder.add_task("alpha", alpha_contract, {},
                           {scratch_bytes(state, region.value(), axis), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      for (unsigned port = 0; port < query.inputs.size(); ++port) {
        auto reads = required_tiles(state, region.value(), axis, grids[port],
                                    port, budget, query.limits.maximum_records);
        if (!reads.ok())
          return Answer(reads.status());
        if (reads.value().empty()) {
          auto status = builder.read_input_schema(task.value(), port);
          if (!status.ok())
            return Answer(status);
        }
        for (auto index : reads.value()) {
          auto status = builder.read_input(task.value(), port, 0, index);
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
struct PreparedAlpha final {
  Action action;
  bool narrow, raw, simd, reference, view, calculates;
  std::optional<std::uint32_t> axis;
  std::shared_ptr<const RuntimeMaps> maps;
  fixed_tensor::PointInputs<2> inputs;
};
std::array<std::uint64_t, 8> source_at(
    const RuntimeMap& map, const std::array<std::uint64_t, 8>& at) noexcept {
  std::array<std::uint64_t, 8> result{};
  for (unsigned axis = 0; axis < map.rank; ++axis)
    result[axis] = map.axes[axis] < 0 ? map.origins[axis] : at[map.axes[axis]];
  return result;
}
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const PreparedAlpha*>(invocation->prepared_context);
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& result) {
    if (!result.ok())
      *status =
          kernel_failure(result.code, result.message.c_str(), result.reason);
    return result.ok();
  };
  if (!accept(consume(0)))
    return;
  if (state.view) {
    *status = kernel_failure(ErrorCode::InvalidArgument, "ViewUnavailable",
                             FailureReason::ViewUnavailable);
    return;
  }
  FloatingEnvironment environment;
  if (state.calculates && !environment.active()) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "alpha numeric environment");
    return;
  }
  SampleBlock* block = nullptr;
  numeric_ops::RatioWorkspace* arithmetic = nullptr;
  if (state.calculates) {
    if (state.reference) {
      auto* scratch = new (invocation->scratch[0].allocation) ReferenceScratch;
      block = &scratch->block;
      arithmetic = &scratch->arithmetic;
    } else {
      block = new (invocation->scratch[0].allocation) SampleBlock;
    }
  }
  fixed_tensor::PointReader<2> reader{state.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  const unsigned width = state.narrow ? 4 : 8;
  auto* destination = static_cast<std::uint8_t*>(output.allocation);
  const auto first = state.axis ? layout.origin[*state.axis] : 0;
  const auto channels = state.axis ? layout.shape[*state.axis] : 1;
  std::uint64_t points = 1;
  for (unsigned a = 0; a < layout.rank; ++a)
    if (!state.axis || a != *state.axis)
      points *= layout.shape[a];
  for (auto channel = first; channel < first + channels; ++channel) {
    const auto& slot = state.maps->slots[channel];
    for (std::uint64_t begin = 0; begin < points; begin += 128) {
      if (!accept(consume(0)))
        return;
      const auto count = std::min<std::uint64_t>(128, points - begin);
      const bool calculate = state.action != Action::Set && slot.color;
      for (std::uint64_t lane = 0; lane < count; ++lane) {
        auto index = begin + lane;
        std::array<std::uint64_t, 8> at{};
        std::uint64_t offset = output.byte_offset;
        for (unsigned a = layout.rank; a-- > 0;) {
          if (state.axis && a == *state.axis) {
            at[a] = channel;
          } else {
            at[a] = layout.origin[a] + index % layout.shape[a];
            index /= layout.shape[a];
          }
          offset += (at[a] - layout.origin[a]) * layout.strides[a];
        }
        const auto bits =
            reader.bits(slot.data.port, source_at(slot.data, at), width);
        if (!calculate) {
          std::memcpy(destination + offset, &bits, width);
          continue;
        }
        std::memcpy(reinterpret_cast<std::uint8_t*>(block->colors.data()) +
                        lane * width,
                    &bits, width);
        const auto alpha = reader.bits(state.maps->alpha.port,
                                       source_at(state.maps->alpha, at), width);
        std::memcpy(
            reinterpret_cast<std::uint8_t*>(block->alpha.data()) + lane * width,
            &alpha, width);
        block->offsets[lane] = offset;
      }
      if (calculate) {
        auto converted = math_span(
            reinterpret_cast<const std::uint8_t*>(block->colors.data()),
            reinterpret_cast<const std::uint8_t*>(block->alpha.data()),
            reinterpret_cast<std::uint8_t*>(block->output.data()), count,
            state.narrow, state.action, state.raw, state.simd, state.reference,
            environment, consume, arithmetic);
        if (!accept(converted))
          return;
        for (std::uint64_t lane = 0; lane < count; ++lane)
          std::memcpy(
              destination + block->offsets[lane],
              reinterpret_cast<const std::uint8_t*>(block->output.data()) +
                  lane * width,
              width);
      }
    }
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid alpha signature"});
  };
  const auto* state = static_cast<const Preparation*>(query.static_state);
  if (!state || query.inputs.empty() || query.inputs.size() > 2 ||
      query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() || !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema))
    return invalid();
  const auto& write = query.writes[0];
  const auto axis =
      output_axis(*state, query.outputs[0].result_schema->tensors[0]);
  auto grid =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (query.scratch !=
          std::array<std::uint64_t, 3>{
              scratch_bytes(*state, region.value(), axis), 0, 0} ||
      write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          (state->narrow ? ElementType::Float32 : ElementType::Float64) ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto prepared = std::make_unique<PreparedAlpha>();
  prepared->action = state->action;
  prepared->narrow = state->narrow;
  prepared->raw = state->raw;
  prepared->simd = state->simd;
  prepared->reference = state->reference;
  prepared->view = state->layout == "view";
  prepared->calculates = calculates(*state, region.value(), axis);
  prepared->axis = axis;
  prepared->maps = state->runtime;
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  std::size_t slot = 0;
  for (unsigned port = 0; port < query.inputs.size(); ++port) {
    auto input = TileGrid::from_schema(*query.inputs[port].result_schema,
                                       query.tile_extent);
    if (!input.ok())
      return Answer(input.status());
    prepared->inputs.grids[port] = input.take_value();
    auto reads = required_tiles(*state, region.value(), axis,
                                prepared->inputs.grids[port], port, budget,
                                query.reads.size() - slot);
    if (!reads.ok())
      return Answer(reads.status());
    for (auto index : reads.value()) {
      const auto& read = query.reads[slot];
      auto expected = prepared->inputs.grids[port].region(index);
      if (!expected.ok())
        return Answer(expected.status());
      if (read.port != port || read.member || read.tile_index != index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != write.layout.element_type ||
          !fixed_tensor::matches_region(read.layout, expected.value()))
        return invalid();
      prepared->inputs.slots[port].push_back(
          {index, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedAlpha*>(context);
  };
  return Answer(kernel);
}

OperationDefinition alpha_definition(const std::string& key, Action action,
                                     numeric_ops::SequenceProfile profile) {
  OperationDefinition d;
  d.key = key;
  auto& t = d.traits;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  t.input_schema = {port};
  t.repeated_minimum = 1;
  t.repeated_maximum = 2;
  t.repeated_match = false;
  t.requires_metadata_specialization = true;
  t.parameter_schema = {
      {"input_description", OperationParameterType::String, false},
      {"group", OperationParameterType::String, false},
      {"axis", OperationParameterType::Int64, false},
      {"alpha_source", OperationParameterType::String, false},
      {"alpha_match", OperationParameterType::String, false},
      {"alpha_selector", OperationParameterType::String, false}};
  if (action == Action::Set) {
    t.parameter_schema.push_back(
        {"layout", OperationParameterType::String, false});
    t.parameter_schema.push_back(
        {"placement", OperationParameterType::String, false});
    t.parameter_schema.push_back(
        {"channel_index", OperationParameterType::Int64, false});
    t.parameter_schema.push_back(
        {"output_axis", OperationParameterType::Int64, false});
  } else {
    t.parameter_schema.push_back(
        {"input_structure", OperationParameterType::String, false});
    t.parameter_schema.push_back(
        {"components", OperationParameterType::String, false});
    t.parameter_schema.push_back(
        {"algorithm", OperationParameterType::String, false});
  }
  auto& out = t.outputs[0];
  out.key = "values";
  out.output_schema = port;
  out.result_schema = tensor_ops::scalar_schema();
  d.prepare_static = [action, profile](const auto& inputs, const auto& params) {
    return prepare_alpha(inputs, params, action, profile);
  };
  d.lower = lower;
  d.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "alpha" || kernel.contract != alpha_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid alpha kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "alpha requires CPU Host"});
    return prepare_kernel(query);
  };
  return d;
}
}  // namespace alpha_ops
Status register_alpha_operations(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("strict", numeric_ops::SequenceProfile::Strict),
        std::make_pair("accelerated_apple_silicon",
                       numeric_ops::SequenceProfile::AppleSilicon),
        std::make_pair("accelerated_x86_64",
                       numeric_ops::SequenceProfile::X86Avx2)}) {
    for (const auto& member :
         {std::make_pair("associate", alpha_ops::Action::Associate),
          std::make_pair("unassociate", alpha_ops::Action::Unassociate),
          std::make_pair("set", alpha_ops::Action::Set)}) {
      auto status = registry->register_operation(alpha_ops::alpha_definition(
          std::string("alpha.") + member.first + "_" + profile.first,
          member.second, profile.second));
      if (!status.ok()) {
        return status;
      }
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
