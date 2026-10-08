#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/alpha_common.hpp"
#include "02-format-color/alpha_math.hpp"
#include "02-format-color/alpha_numeric_metadata.hpp"
#include "photospider/data/region_runs.hpp"

namespace ps::plugin_internal {
namespace alpha_ops {
struct Slot final {
  std::int64_t source = 0;
  bool color = false;
  bool alpha = false;
};
struct Map final {
  std::uint32_t port = 0, roles = 1;
  std::vector<ResultMappedAxis> axes;
};
struct Span final {
  std::uint64_t first = 0, count = 1;
  Map map;
};
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
  bool view_legal = false;
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
    ResultMappedAxis item;
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
bool same_axis(const ResultMappedAxis& a, const ResultMappedAxis& b) {
  return a.output_axis == b.output_axis && a.source_origin == b.source_origin &&
         a.step == b.step && a.extent == b.extent &&
         a.output_origin == b.output_origin;
}
std::optional<Map> merge_map(const Map& first, const Map& second,
                             std::uint32_t output_axis, std::uint64_t start) {
  if (first.port != second.port || first.roles != second.roles ||
      first.axes.size() != second.axes.size())
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
      if (delta < -1 || delta > 1 || a.extent != b.extent)
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
  if (merged.port != item.port || merged.roles != item.roles ||
      merged.axes.size() != item.axes.size())
    return false;
  for (std::size_t i = 0; i < item.axes.size(); ++i) {
    const auto& a = merged.axes[i];
    const auto& b = item.axes[i];
    if (a.output_axis == static_cast<std::int32_t>(output_axis) &&
        b.output_axis < 0) {
      auto coordinate = a.source_coordinate(channel);
      if (!coordinate.ok() || coordinate.value() != b.source_origin ||
          a.extent != b.extent)
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
  s.raw = text(p, "metadata_mode", "respect") == "raw";
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
      Params source_params{{"metadata_mode", std::string("raw")}};
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
      Params source_params{{"metadata_mode", std::string("raw")}};
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
  output.metadata.atomic_trailing_axes = 0;
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
    if (output_spec.layout.spatial) {
      output_spec.layout.groups.clear();
    }
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
    auto encoded = encode_tensor_description(*desc);
    if (!encoded.ok()) {
      return Answer(encoded.status());
    }
    if (!s.raw) {
      output_spec.facets = output_facets(inputs[0], *desc);
    }
  }
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  auto valid_shape = shape_valid(output.metadata);
  if (!valid_shape.ok()) {
    return Answer(valid_shape);
  }
  s.alpha_map = mapping(s, inputs, true, s.alpha_index);
  s.alpha_map.roles = static_cast<std::uint32_t>(
      s.raw ? DependencyRole::Data : DependencyRole::Validation);
  if (action != Action::Set) {
    s.alpha_map.roles |= static_cast<std::uint32_t>(DependencyRole::Data);
  }
  bool identity_view =
      action == Action::Set && !s.external && s.input_axis == s.output_axis;
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
    if (slot.alpha || (!s.raw && slot.color))
      data.roles |= 4;
    s.data_maps.push_back(std::move(data));
    const auto actual =
        slot.alpha ? s.alpha_index : static_cast<std::uint64_t>(original);
    identity_view = identity_view && actual == k;
  }
  s.view_legal = identity_view;
  prepare_spans(&s, main.batch_axes.size());
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::make_shared<Preparation>(std::move(s));
  return Answer(std::move(result));
} catch (const Status& status) {
  return Result<OperationPreparation>(status);
}
void source_at(const Map& map, const std::vector<std::uint64_t>& at,
               std::vector<std::uint64_t>* source) {
  source->resize(map.axes.size());
  for (std::size_t i = 0; i < map.axes.size(); ++i) {
    const auto& axis = map.axes[i];
    (*source)[i] = tensor_ops::take(axis.source_coordinate(
        axis.output_axis < 0 ? 0 : at[axis.output_axis]));
  }
}
Status sample_failure(const Preparation& s, const MathFailure& failure,
                      const std::vector<std::uint64_t>& at) {
  std::string message =
      "alpha." +
      std::string(s.action == Action::Set         ? "set"
                  : s.action == Action::Associate ? "associate"
                                                  : "unassociate") +
      " group=" + s.group + " " + failure.operand + " at [";
  for (std::size_t i = 0; i < at.size(); ++i) {
    message += (i ? "," : "") + std::to_string(at[i]);
  }
  message += "] alpha_source=";
  message += s.scalar ? "scalar" : s.external ? "external_plane" : "internal";
  message += " alpha_port=" + std::to_string(s.alpha_map.port);
  if (!s.external) {
    message += " alpha_channel=" + std::to_string(s.alpha_index);
  }
  std::vector<std::uint64_t> alpha_at;
  source_at(s.alpha_map, at, &alpha_at);
  message += " alpha_at=[";
  for (std::size_t i = 0; i < alpha_at.size(); ++i) {
    message += (i ? "," : "") + std::to_string(alpha_at[i]);
  }
  message += "]";
  return {ErrorCode::OperationFailed,
          std::move(message),
          failure.reason,
          {FailureOrigin::Domain, FailureScope::Atom}};
}
using tensor_ops::require;
using tensor_ops::take;
using Poll = Result<ResultProgramPoll>;
Status unavailable_view() {
  return invalid(
      "ViewUnavailable: full set-alpha map has no common affine owner");
}
std::optional<std::uint32_t> output_axis(const Preparation& s,
                                         const ResultTensorSpec& spec) {
  return s.output_axis
             ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                   spec.batch_axes.size() + *s.output_axis)}
             : std::nullopt;
}
ResultRelation joined(const ResultProgramPhase& phase,
                      ResourceVector<ResultRelation> relations) {
  const auto& root = phase.resources;
  while (relations.size() > 1) {
    ResourceVector<ResultRelation> next{
        ResourceAllocator<ResultRelation>(root)};
    for (std::size_t i = 0; i < relations.size(); i += 16) {
      require(phase.consume_work(17));
      std::vector<ResultRelation> group;
      const auto end = std::min(relations.size(), i + 16);
      for (auto j = i; j < end; ++j)
        group.push_back(relations[j]);
      next.push_back(take(ResultRelation::unite(root, std::move(group))));
    }
    relations = std::move(next);
  }
  return relations.front();
}
ResultBuilder builder(const ResultProgramPhase& phase, bool empty) {
  auto result = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_height, phase.query.tile_width, phase.query.resources));
  ResourceVector<ResultRelation> relations{
      ResourceAllocator<ResultRelation>(phase.resources)};
  for (std::uint32_t port = 0; port < phase.query.inputs.size(); ++port)
    relations.push_back(take(ResultRelation::cartesian(
        phase.resources, 1,
        {port, 8, 0, empty ? 0U : 1U, ResultSupportTarget::Descriptor, 0})));
  require(result.bind_descriptor_relation(joined(phase, std::move(relations))));
  return result;
}
Poll empty_result(const ResultProgramPhase& phase) try {
  auto result = builder(phase, true);
  return Poll(ResultPublication{take(result.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}
Region channel_region(const Region& box, std::optional<std::uint32_t> axis,
                      std::uint64_t channel) {
  auto dims = box.dimensions();
  if (axis)
    dims[*axis] = {channel, 1};
  return Region(std::move(dims));
}
ResultRelation relation(const ResultProgramPhase& phase, const Preparation& s) {
  const auto& spec = phase.query.output.result_schema->tensors[0];
  const auto shape = spec.sample_shape();
  const auto axis = output_axis(s, spec);
  ResourceVector<ResultRelation> relations{
      ResourceAllocator<ResultRelation>(phase.resources)};
  for (const auto& span : s.spans) {
    require(phase.consume_work(span.map.axes.size() + 1));
    auto dims = Region::whole(shape).dimensions();
    if (axis)
      dims[*axis] = {span.first, span.count};
    const auto& map = span.map;
    relations.push_back(take(ResultRelation::mapped(
        phase.resources, shape, Region(std::move(dims)),
        phase.query.inputs[map.port].result_schema->tensors[0].sample_shape(),
        map.axes,
        {map.port, map.roles, 0, 0, ResultSupportTarget::Tensor, 0})));
  }
  for (std::uint32_t port = 0; port < phase.query.inputs.size(); ++port)
    relations.push_back(take(ResultRelation::cartesian(
        phase.resources, take(spec.sample_count()),
        {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
  return joined(phase, std::move(relations));
}
// Inspect only authorized backing metadata. This proves the full declared map
// but does not grant any additional sample authorization or output coverage.
struct GenericView final {
  std::shared_ptr<const CpuStorage> owner;
  std::uint64_t base = 0;
  std::vector<std::int64_t> strides;
};
Status generic_view(const ResultProgramPhase& phase, const Preparation& s,
                    GenericView* proof) {
  const auto& spec = phase.query.output.result_schema->tensors[0];
  const auto shape = spec.sample_shape();
  const auto rank = shape.size();
  std::array<Value, 2> sources;
  std::array<__int128, 2> bases{};
  const CpuStorage* owner = nullptr;
  for (const auto& map : s.data_maps) {
    if (sources[map.port].valid())
      continue;
    const auto& input = phase.tensors->at({map.port, 0});
    std::function<Status(const Region&)> inspect;
    inspect = [&](const Region& region) {
      require(phase.consume_work(1 + rank));
      auto window = take(input.acquire(region, phase.query.cancellation));
      auto value = execution_internal::ResultWindowAccess::affine(window);
      if (!value.ok()) {
        if (value.status().code != ErrorCode::NotFound)
          return value.status();
        auto split =
            execution_internal::ResultWindowAccess::visit_backing_regions(
                window, phase.resources, inspect);
        if (!split.ok())
          return split.status();
        return split.value() ? Status::success() : unavailable_view();
      }
      const auto& v = value.value();
      auto base = static_cast<__int128>(v.layout().byte_offset);
      for (std::size_t i = 0; i < v.layout().origin.size(); ++i)
        base -= static_cast<__int128>(v.layout().origin[i]) *
                v.layout().byte_strides[i];
      if (owner && owner != v.storage().get())
        return unavailable_view();
      const auto& previous = sources[map.port];
      if (previous.valid() &&
          (base != bases[map.port] ||
           previous.layout().byte_strides != v.layout().byte_strides))
        return unavailable_view();
      owner = v.storage().get();
      sources[map.port] = value.take_value();
      bases[map.port] = base;
      return Status::success();
    };
    for (const auto& box : input.coverage().boxes()) {
      auto inspected = inspect(box);
      if (!inspected.ok())
        return inspected;
    }
    if (!sources[map.port].valid())
      return unavailable_view();
  }
  std::array<std::int64_t, 8> strides{};
  __int128 first = 0, channel_stride = 0;
  for (std::size_t k = 0; k < s.data_maps.size(); ++k) {
    require(phase.consume_work(rank + 1));
    const auto& map = s.data_maps[k];
    const auto& source = sources[map.port];
    auto base = bases[map.port];
    std::array<std::int64_t, 8> projected{};
    for (std::size_t i = 0; i < map.axes.size(); ++i) {
      const auto& axis = map.axes[i];
      const auto stride = source.layout().byte_strides[i];
      base += static_cast<__int128>(axis.source_origin) * stride;
      if (axis.output_axis >= 0)
        projected[axis.output_axis] = stride;
    }
    if (!k) {
      strides = projected;
      first = base;
    } else {
      if (strides != projected)
        return unavailable_view();
      if (k == 1)
        channel_stride = base - first;
      if (base != first + static_cast<__int128>(k) * channel_stride)
        return unavailable_view();
    }
  }
  const auto axis = output_axis(s, spec);
  if (!axis || channel_stride < INT64_MIN || channel_stride > INT64_MAX)
    return unavailable_view();
  strides[*axis] = static_cast<std::int64_t>(channel_stride);
  auto low = first, high = first;
  for (std::size_t i = 0; i < rank; ++i) {
    const auto delta = static_cast<__int128>(shape[i] - 1) * strides[i];
    (delta < 0 ? low : high) += delta;
  }
  if (low < 0 ||
      high + (s.narrow ? 4 : 8) > static_cast<__int128>(owner->bytes().size()))
    return unavailable_view();
  proof->owner = sources[s.data_maps.front().port].storage();
  proof->base = static_cast<std::uint64_t>(first);
  proof->strides.assign(strides.begin(), strides.begin() + rank);
  return Status::success();
}
struct ReadSpan final {
  const std::uint8_t* data;
  std::uint64_t count;
  std::int64_t stride;
};
ReadSpan read_span(const ResultTensorReadWindow& window, const Map& map,
                   const std::vector<std::uint64_t>& at,
                   std::vector<std::uint64_t>* source, std::uint32_t row_axis,
                   std::uint64_t limit) {
  source_at(map, at, source);
  const auto run = take(window.row_run(*source));
  if (map.axes[window.sample_axis()].output_axis ==
      static_cast<std::int32_t>(row_axis))
    return {run.data, std::min(limit, run.samples), run.sample_stride_bytes};
  const bool repeat =
      std::none_of(map.axes.begin(), map.axes.end(), [&](const auto& axis) {
        return axis.output_axis == static_cast<std::int32_t>(row_axis);
      });
  return {run.data, repeat ? limit : 1, 0};
}
Status evaluate(const ResultProgramPhase& phase, const Preparation& s,
                const Region& box, const ResultTensorWriteWindow* writer) {
  const auto& spec = phase.query.output.result_schema->tensors[0];
  const auto axis = output_axis(s, spec);
  const auto channels = axis ? box.dimensions()[*axis] : RegionDimension{0, 1};
  const std::size_t width = s.narrow ? 4 : 8;
  input_internal::Float32Environment environment;
  const std::function<Status(std::uint64_t)> consume = [&](std::uint64_t n) {
    if (phase.query.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "alpha exact arithmetic cancelled"};
    return phase.consume_work(n);
  };
  constexpr std::uint64_t block = 128;
  std::array<std::uint8_t, block * 8> x{}, a{}, y{};
  std::vector<std::uint64_t> at, source;
  for (auto k = channels.offset; k < channels.offset + channels.extent; ++k) {
    const auto selected = channel_region(box, axis, k);
    const auto& slot = s.slots[k];
    const auto& map = s.data_maps[k];
    auto color =
        take(phase.tensors->at({map.port, 0})
                 .acquire(format_result::mapped_region(selected, map.axes),
                          phase.query.cancellation));
    std::optional<ResultTensorReadWindow> alpha;
    if (slot.color)
      alpha.emplace(take(
          phase.tensors->at({s.alpha_map.port, 0})
              .acquire(format_result::mapped_region(selected, s.alpha_map.axes),
                       phase.query.cancellation)));
    const auto work =
        take(execution_internal::ResultWindowAccess::read_work(color)) +
        (alpha ? take(execution_internal::ResultWindowAccess::read_work(*alpha))
               : 0);
    const auto calculate = [&](const std::uint8_t* colors,
                               const std::uint8_t* weights, std::uint64_t n) {
      if (!slot.color && !slot.alpha) {
        std::memcpy(y.data(), colors, n * width);
        return MathFailure{};
      }
      return math_span(colors, weights, y.data(), n, s.narrow,
                       slot.alpha ? Action::Set : s.action, s.raw, s.simd,
                       s.reference, environment, consume);
    };
    if (!spec.layout.spatial) {
      const auto count = take(selected.element_count());
      for (std::uint64_t begin = 0; begin < count; begin += block) {
        const auto n = std::min(block, count - begin);
        require(consume(n * (64 + 2 * selected.rank() + work)));
        for (std::uint64_t lane = 0; lane < n; ++lane) {
          region_run_coordinate(selected, begin + lane, &at);
          source_at(map, at, &source);
          const auto run = take(color.row_run(source));
          std::memcpy(x.data() + lane * width, run.data, width);
          if (alpha) {
            source_at(s.alpha_map, at, &source);
            const auto weight = take(alpha->row_run(source));
            std::memcpy(a.data() + lane * width, weight.data, width);
          }
        }
        const auto failed =
            calculate(x.data(), slot.alpha ? x.data() : a.data(), n);
        if (!failed.status.ok())
          return failed.status;
        if (failed.reason != FailureReason::None) {
          region_run_coordinate(selected, begin + failed.lane, &at);
          auto failure = failed;
          if (slot.alpha)
            failure.operand = "alpha";
          return sample_failure(s, failure, at);
        }
        if (writer) {
          for (std::uint64_t lane = 0; lane < n; ++lane) {
            region_run_coordinate(selected, begin + lane, &at);
            const auto run = take(writer->row_run(at));
            std::memcpy(run.data, y.data() + lane * width, width);
          }
        }
      }
      continue;
    }
    const auto h = spec.batch_axes.size() + spec.layout.height_axis;
    const auto w = spec.batch_axes.size() + spec.layout.width_axis;
    const auto& dims = selected.dimensions();
    at.clear();
    for (const auto& d : dims)
      at.push_back(d.offset);
    for (auto row = dims[h].offset; row < dims[h].offset + dims[h].extent;
         ++row) {
      at[h] = row;
      for (auto col = dims[w].offset; col < dims[w].offset + dims[w].extent;) {
        at[w] = col;
        auto n = std::min(block, dims[w].offset + dims[w].extent - col);
        require(consume(n * (64 + 2 * selected.rank() + work)));
        auto colors = read_span(color, map, at, &source, w, n);
        n = std::min(n, colors.count);
        auto weights = slot.alpha ? colors : ReadSpan{nullptr, n, 0};
        if (alpha) {
          weights = read_span(*alpha, s.alpha_map, at, &source, w, n);
          n = std::min(n, weights.count);
        }
        std::optional<ResultTensorMutableRun> target;
        if (writer) {
          target = take(writer->row_run(at));
          n = std::min(n, target->samples);
        }
        if (!n)
          return {ErrorCode::Internal, "empty alpha row span"};
        const auto gather = [n, width](ReadSpan span, std::uint8_t* buffer) {
          if (!span.data || span.stride == static_cast<std::int64_t>(width))
            return span.data;
          for (std::uint64_t i = 0; i < n; ++i)
            std::memcpy(buffer + i * width,
                        span.data + static_cast<std::int64_t>(i) * span.stride,
                        width);
          return static_cast<const std::uint8_t*>(buffer);
        };
        auto failed =
            calculate(gather(colors, x.data()), gather(weights, a.data()), n);
        if (!failed.status.ok())
          return failed.status;
        if (failed.reason != FailureReason::None) {
          at[w] += failed.lane;
          if (slot.alpha)
            failed.operand = "alpha";
          return sample_failure(s, failed, at);
        }
        if (target) {
          for (std::uint64_t i = 0; i < n; ++i)
            std::memcpy(target->data + static_cast<std::int64_t>(i) *
                                           target->sample_stride_bytes,
                        y.data() + i * width, width);
        }
        col += n;
      }
    }
  }
  return Status::success();
}
struct State final {
  const Preparation* state;
  bool requested = false;
  Footprint output;
  ResultRelation support;
  explicit State(const Preparation* prepared) : state(prepared) {}
  Poll poll(const ResultProgramPhase& phase) try {
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(16384, 16384)));
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto& s = *state;
    if (!requested) {
      requested = true;
      output = phase.query.tensor_outputs
                   ? *phase.query.tensor_outputs
                   : take(Footprint::all(spec.sample_shape()));
      support = relation(phase, s);
      ResultProgramNeed need;
      FootprintLimits limits;
      limits.cancellation = phase.query.cancellation;
      limits.consume_work = [&](std::uint64_t work) {
        return phase.consume_work(work);
      };
      for (std::uint32_t port = 0; port < phase.query.inputs.size(); ++port) {
        auto samples = take(Footprint::none(
            phase.query.inputs[port].result_schema->tensors[0].sample_shape()));
        std::uint32_t roles = 8;
        require(support.project(
            output,
            [&](auto source, const Footprint* selected) {
              require(phase.consume_work(1));
              if (source.input == port && selected) {
                roles |= source.roles;
                samples = take(samples.unite(*selected, limits));
              }
              return Status::success();
            },
            limits));
        need.tensors.push_back({port, 0, std::move(samples), roles});
      }
      return Poll(std::move(need));
    }
    auto result = builder(phase, false);
    std::string policy = s.layout;
    GenericView proof;
    if (policy != "materialize") {
      const auto available =
          spec.layout.spatial
              ? (s.view_legal ? Status::success() : unavailable_view())
              : generic_view(phase, s, &proof);
      if (!available.ok()) {
        if (policy == "view" || !format_result::view_unavailable(available))
          return Poll(available);
        policy = "materialize";
      }
    }
    require(format_result::planes(spec, output, [&](const Region& box) {
      if (policy != "materialize") {
        require(evaluate(phase, s, box, nullptr));
        if (!spec.layout.spatial) {
          StridedLayout layout;
          layout.byte_strides = proof.strides;
          __int128 address = proof.base;
          for (std::size_t i = 0; i < box.rank(); ++i) {
            layout.origin.push_back(box.dimensions()[i].offset);
            address +=
                static_cast<__int128>(layout.origin.back()) * proof.strides[i];
          }
          if (address < 0 || address > UINT64_MAX)
            return invalid("view address overflow");
          layout.byte_offset = static_cast<std::uint64_t>(address);
          return result.publish_tensor(0, box, layout, proof.owner, support,
                                       {true, true, true, true},
                                       phase.query.cancellation);
        }
        const auto axis = output_axis(s, spec);
        const auto channels =
            axis ? box.dimensions()[*axis] : RegionDimension{0, 1};
        for (auto k = channels.offset; k < channels.offset + channels.extent;
             ++k) {
          const auto part = channel_region(box, axis, k);
          const auto& map = s.data_maps[k];
          auto source =
              take(phase.tensors->at({map.port, 0})
                       .acquire(format_result::mapped_region(part, map.axes),
                                phase.query.cancellation));
          require(format_result::publish(phase, &result, part, source, map.axes,
                                         support, policy, map.port));
        }
        return Status::success();
      }
      return result.publish_tensor_kernel(
          0, box,
          [&](const auto& writers) {
            try {
              for (const auto& writer : writers)
                require(evaluate(phase, s, writer.region(), &writer));
              return Status::success();
            } catch (const Status& status) {
              return status;
            }
          },
          support, {true, true, true, true}, phase.query.cancellation);
    }));
    require(phase.consume_work(1));
    if (phase.query.cancellation.cancelled())
      return Poll(
          Status{ErrorCode::Cancelled, "alpha cancelled before publication"});
    return Poll(ResultPublication{take(result.seal()), true});
  } catch (const Status& status) {
    return Poll(status);
  }
};

OperationDefinition alpha_definition(const std::string& key, Action action,
                                     numeric_ops::SequenceProfile profile) {
  OperationDefinition d;
  d.key = key;
  auto& t = d.traits;
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.element_type_mask = 127;
  t.input_schema = {port};
  t.repeated_minimum = 1;
  t.repeated_maximum = 2;
  t.repeated_match = false;
  t.cacheable = false;
  t.requires_metadata_specialization = true;
  t.workspace_bytes = 16384;
  t.parameter_schema = {
      {"metadata_mode", OperationParameterType::String, false},
      {"metadata_override", OperationParameterType::String, false},
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
  out.region_rule = OperationRegionRule::Dependency;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 2;
  d.prepare_static = [action, profile](const auto& inputs, const auto& params) {
    return prepare_alpha(inputs, params, action, profile);
  };
  d.start_result = [](const ResultProgramQuery& query,
                      const BufferAllocator& allocator) {
    if (query.tensor_outputs && query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_result>();
    return ResultContinuation::make<State>(
        allocator, static_cast<const Preparation*>(query.prepared->state()));
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
