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

#include "01-numeric/array_publication.hpp"
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
struct Preparation final {
  Action action = Action::Associate;
  bool raw = false, narrow = true, simd = false, reference = false;
  bool scalar = false, external = false, component = false;
  std::optional<std::uint32_t> input_axis, output_axis;
  std::uint64_t alpha_index = 0;
  std::string group, layout;
  std::vector<Slot> slots;
  std::vector<DependencyMappedNeed> data_maps;
  DependencyMappedNeed alpha_map;
  bool view_legal = false;
};
DependencyMappedNeed mapping(const Preparation& state,
                             const std::vector<OperationMetadata>& inputs,
                             bool alpha, std::uint64_t source) {
  DependencyMappedNeed map;
  map.port = alpha && state.external ? 1 : 0;
  const auto source_axis =
      map.port == 0 ? state.input_axis : std::optional<std::uint32_t>{};
  for (std::size_t axis = 0; axis < inputs[map.port].descriptor.shape.size();
       ++axis) {
    DependencyAxis item;
    if (alpha && state.scalar) {
      item.fixed = {0, 1};
    } else if (source_axis && axis == *source_axis) {
      item.fixed = {source, 1};
    } else {
      const auto spatial = axis - (source_axis && axis > *source_axis ? 1 : 0);
      item.observation_axis = static_cast<std::int32_t>(
          spatial +
          (state.output_axis && spatial >= *state.output_axis ? 1 : 0));
    }
    map.axes.push_back(item);
  }
  return map;
}
Result<OperationPreparation> prepare_alpha(
    const std::vector<OperationMetadata>& inputs, const Params& p,
    Action action, numeric_ops::SequenceProfile profile) {
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
    if ((m.descriptor.element_type != ElementType::Float32 &&
         m.descriptor.element_type != ElementType::Float64) ||
        m.descriptor.element_type != inputs[0].descriptor.element_type) {
      return Answer(mismatch(
          "arithmetic/set require matching Float32 or Float64 inputs"));
    }
  }
  Preparation s;
  s.action = action;
  s.narrow = inputs[0].descriptor.element_type == ElementType::Float32;
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
      if (axis < 0 || static_cast<std::uint64_t>(axis) >=
                          inputs[0].descriptor.shape.size()) {
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
          index >=
              (s.input_axis ? inputs[0].descriptor.shape[*s.input_axis] : 1) ||
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
  const auto count =
      s.input_axis ? inputs[0].descriptor.shape[*s.input_axis] : 1;
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
    auto spatial = inputs[0].descriptor.shape;
    if (s.input_axis) {
      spatial.erase(spatial.begin() + *s.input_axis);
    }
    if (!s.scalar && spatial.empty()) {
      return Answer(invalid("rank-zero external alpha plane is forbidden"));
    }
    const auto expected = s.scalar ? std::vector<std::uint64_t>{1} : spatial;
    if (inputs[1].descriptor.shape != expected ||
        (s.scalar && inputs[1].planar_layout)) {
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
          static_cast<std::uint64_t>(axis) >
              inputs[0].descriptor.shape.size() ||
          inputs[0].descriptor.shape.size() == 8) {
        return Answer(invalid("inserted output axis out of rank"));
      }
      s.output_axis = static_cast<std::uint32_t>(axis);
      output.metadata.descriptor.shape.insert(
          output.metadata.descriptor.shape.begin() + *s.output_axis, 1);
      if (output.metadata.planar_layout) {
        auto& l = *output.metadata.planar_layout;
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
    output.metadata.descriptor.shape[*s.output_axis] = source_map.size();
    auto remapped =
        remap_description(*semantic, source_map, *s.output_axis, target);
    if (!remapped.ok()) {
      return Answer(remapped.status());
    }
    desc = remapped.take_value();
    if (output.metadata.planar_layout) {
      output.metadata.planar_layout->groups.clear();
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
    auto valid = validate_tensor_description(*desc, output.metadata.descriptor);
    if (!valid.ok()) {
      return Answer(valid);
    }
    auto encoded = encode_tensor_description(*desc);
    if (!encoded.ok()) {
      return Answer(encoded.status());
    }
    if (!s.raw) {
      output.metadata.facets = output_facets(inputs[0], *desc);
    }
  }
  auto valid_shape = shape_valid(output.metadata);
  if (!valid_shape.ok()) {
    return Answer(valid_shape);
  }
  s.alpha_map = mapping(s, inputs, true, s.alpha_index);
  s.alpha_map.roles = static_cast<std::uint32_t>(DependencyRole::Validation);
  if (action != Action::Set) {
    s.alpha_map.roles |= static_cast<std::uint32_t>(DependencyRole::Data);
  }
  std::vector<DependencyMapPiece> pieces;
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
    s.data_maps.push_back(data);
    auto dims = Region::whole(output.metadata.descriptor.shape).dimensions();
    if (s.output_axis) {
      dims[*s.output_axis] = {k, 1};
    }
    auto coverage = Footprint::from_regions(output.metadata.descriptor.shape,
                                            {Region(dims)});
    if (!coverage.ok()) {
      return Answer(coverage.status());
    }
    std::vector<DependencyMappedNeed> needs{data};
    if (slot.color) {
      needs.push_back(s.alpha_map);
    }
    if (slot.alpha) {
      needs[0].roles |= static_cast<std::uint32_t>(DependencyRole::Validation);
    }
    for (std::uint32_t port = 0; port < inputs.size(); ++port) {
      DependencyMappedNeed descriptor;
      descriptor.port = port;
      descriptor.roles = static_cast<std::uint32_t>(DependencyRole::Descriptor);
      descriptor.tags = {{1, 0}};
      needs.push_back(std::move(descriptor));
    }
    pieces.push_back({coverage.take_value(), std::move(needs)});
    const auto actual =
        slot.alpha ? s.alpha_index : static_cast<std::uint64_t>(original);
    identity_view = identity_view && actual == k;
  }
  s.view_legal = identity_view;
  output.static_dependency_pieces = std::move(pieces);
  output.regional_atomic = true;
  output.preserve_output_views =
      action == Action::Set && s.layout != "materialize" &&
      (!output.metadata.planar_layout || s.view_legal);
  if (output.metadata.planar_layout && action == Action::Set) {
    output.data_movement_view_policy =
        s.layout == "view"          ? DataMovementViewPolicy::RequireView
        : s.layout == "materialize" ? DataMovementViewPolicy::Materialize
                                    : DataMovementViewPolicy::Auto;
  }
  OperationPreparation result;
  result.outputs.push_back(std::move(output));
  result.state = std::make_shared<Preparation>(std::move(s));
  return Answer(std::move(result));
}
void source_at(const DependencyMappedNeed& map,
               const std::vector<std::uint64_t>& at,
               std::vector<std::uint64_t>* source) {
  source->resize(map.axes.size());
  for (std::size_t i = 0; i < map.axes.size(); ++i) {
    const auto& a = map.axes[i];
    (*source)[i] = a.observation_axis < 0
                       ? a.fixed.offset
                       : static_cast<std::uint64_t>(
                             static_cast<__int128>(at[a.observation_axis]) +
                             a.translation);
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
bool contains(const Region& region, const std::vector<std::uint64_t>& at) {
  if (region.rank() != at.size()) {
    return false;
  }
  for (std::size_t i = 0; i < at.size(); ++i) {
    const auto d = region.dimensions()[i];
    if (at[i] < d.offset || at[i] - d.offset >= d.extent) {
      return false;
    }
  }
  return true;
}
struct Cursor final {
  const ValueFragments* input;
  const Value* cached = nullptr;
  Status read(const std::vector<std::uint64_t>& at, void* destination,
              std::size_t width) {
    if (!cached || !contains(cached->region(), at)) {
      cached = nullptr;
      for (const auto& v : input->fragments()) {
        if (contains(v.region(), at)) {
          cached = &v;
          break;
        }
      }
    }
    if (!cached) {
      return Status{ErrorCode::NotFound, "alpha source coverage is missing"};
    }
    auto address = cached->byte_address(at);
    if (!address.ok()) {
      return address.status();
    }
    std::memcpy(destination, cached->bytes().data() + address.value(), width);
    return Status::success();
  }
};
struct GenericView final {
  const Value* owner = nullptr;
  __int128 base = 0;
  std::vector<std::int64_t> strides;
};
// Prove the COMPLETE declared mapping, not only the requested channels. Source
// fragments must agree on one affine map per port and share one immutable
// storage owner. Physical bounds are checked over the full output shape, but
// this proof grants no sample coverage: only the validated query is published.
Result<std::optional<GenericView>> generic_view(const DependencyPhase& phase,
                                                const Preparation& state) {
  using Answer = Result<std::optional<GenericView>>;
  const auto unavailable = [] { return Answer(std::optional<GenericView>{}); };
  if (state.action != Action::Set || state.layout == "materialize") {
    return unavailable();
  }
  GenericView result;
  const auto rank = phase.query.output.descriptor.shape.size();
  std::vector<__int128> bases(phase.inputs.size());
  std::vector<const Value*> sources(phase.inputs.size(), nullptr);
  for (const auto& map : state.data_maps) {
    if (sources[map.port]) {
      continue;
    }
    for (const auto& value : phase.inputs[map.port].fragments()) {
      auto work = phase.consume_work(1 + rank);
      if (!work.ok()) {
        return Answer(work);
      }
      __int128 base = value.layout().byte_offset;
      for (std::size_t i = 0; i < value.layout().origin.size(); ++i) {
        base -= static_cast<__int128>(value.layout().origin[i]) *
                value.layout().byte_strides[i];
      }
      if (result.owner &&
          result.owner->storage().get() != value.storage().get()) {
        return unavailable();
      }
      if (sources[map.port] && (base != bases[map.port] ||
                                value.layout().byte_strides !=
                                    sources[map.port]->layout().byte_strides)) {
        return unavailable();
      }
      result.owner = &value;
      sources[map.port] = &value;
      bases[map.port] = base;
    }
    if (!sources[map.port]) {
      return unavailable();
    }
  }
  std::vector<__int128> offsets;
  for (const auto& map : state.data_maps) {
    const auto& source = *sources[map.port];
    auto base = bases[map.port];
    std::vector<std::int64_t> strides(rank, 0);
    for (std::size_t i = 0; i < map.axes.size(); ++i) {
      const auto& axis = map.axes[i];
      const auto stride = source.layout().byte_strides[i];
      base += static_cast<__int128>(
                  axis.observation_axis < 0
                      ? static_cast<std::int64_t>(axis.fixed.offset)
                      : axis.translation) *
              stride;
      if (axis.observation_axis >= 0) {
        strides[axis.observation_axis] = stride;
      }
    }
    if (offsets.empty()) {
      result.strides = strides;
    } else if (result.strides != strides) {
      return unavailable();
    }
    offsets.push_back(base);
  }
  if (offsets.empty() || !state.output_axis) {
    return unavailable();
  }
  const __int128 channel_stride =
      offsets.size() > 1 ? offsets[1] - offsets[0] : 0;
  if (channel_stride < INT64_MIN || channel_stride > INT64_MAX) {
    return unavailable();
  }
  for (std::size_t k = 0; k < offsets.size(); ++k) {
    if (offsets[k] != offsets[0] + static_cast<__int128>(k) * channel_stride) {
      return unavailable();
    }
  }
  result.base = offsets[0];
  result.strides[*state.output_axis] =
      static_cast<std::int64_t>(channel_stride);
  auto low = result.base, high = result.base;
  for (std::size_t i = 0; i < rank; ++i) {
    const auto span =
        static_cast<__int128>(phase.query.output.descriptor.shape[i] - 1) *
        result.strides[i];
    if (span < 0) {
      low += span;
    } else {
      high += span;
    }
  }
  const auto width =
      Value::element_size(phase.query.output.descriptor.element_type);
  if (low < 0 ||
      high + width > static_cast<__int128>(result.owner->bytes().size())) {
    return unavailable();
  }
  return Answer(std::optional<GenericView>{std::move(result)});
}
Result<ValueFragments> evaluate(const DependencyPhase& phase,
                                const Preparation& s) {
  using Answer = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto width = Value::element_size(descriptor.element_type);
  auto proof = generic_view(phase, s);
  if (!proof.ok()) {
    return Answer(proof.status());
  }
  const bool viewed = proof.value().has_value();
  if (s.layout == "view" && !viewed) {
    return Answer(invalid(
        "ViewUnavailable: full set-alpha map has no common affine owner"));
  }
  numeric_ops::ArrayPublication publication(phase.query.outputs.boxes().size(),
                                            descriptor.shape.size());
  std::vector<Value> values;
  std::vector<Cursor> cursors;
  for (const auto& input : phase.inputs) {
    cursors.push_back({&input, nullptr});
  }
  input_internal::Float32Environment environment;
  const std::function<Status(std::uint64_t)> exact_work =
      [&phase](std::uint64_t work) { return phase.consume_work(work); };
  constexpr std::uint64_t block = 128;
  std::array<std::uint8_t, block * 8> colors{}, alpha{}, result{};
  std::vector<std::uint64_t> at(descriptor.shape.size()), source;
  for (const auto& box : phase.query.outputs.boxes()) {
    std::optional<MutableValue> writer;
    if (!viewed) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok()) {
        return Answer(allocated.status());
      }
      writer = allocated.take_value();
    }
    const auto channel = s.output_axis ? box.dimensions()[*s.output_axis]
                                       : RegionDimension{0, 1};
    for (std::uint64_t k = channel.offset; k < channel.offset + channel.extent;
         ++k) {
      auto dims = box.dimensions();
      if (s.output_axis) {
        dims[*s.output_axis] = {k, 1};
      }
      Region selected(dims);
      const auto size = selected.element_count().value();
      const auto& slot = s.slots[k];
      const auto& data = s.data_maps[k];
      for (std::uint64_t begin = 0; begin < size; begin += block) {
        const auto n = std::min(block, size - begin);
        auto charged =
            phase.consume_work(n * (descriptor.shape.size() * 2 + 64));
        if (!charged.ok()) {
          return Answer(charged);
        }
        if (phase.query.cancellation.cancelled()) {
          return Answer(Status{ErrorCode::Cancelled, "alpha cancelled"});
        }
        for (std::uint64_t lane = 0; lane < n; ++lane) {
          region_run_coordinate(selected, begin + lane, &at);
          source_at(data, at, &source);
          auto read = cursors[data.port].read(
              source, colors.data() + lane * width, width);
          if (!read.ok()) {
            return Answer(read);
          }
          if (slot.color) {
            source_at(s.alpha_map, at, &source);
            read = cursors[s.alpha_map.port].read(
                source, alpha.data() + lane * width, width);
            if (!read.ok()) {
              return Answer(read);
            }
          } else if (slot.alpha) {
            std::memcpy(alpha.data() + lane * width,
                        colors.data() + lane * width, width);
          }
        }
        if (slot.color || slot.alpha) {
          auto failed =
              math_span(colors.data(), alpha.data(), result.data(), n, s.narrow,
                        slot.alpha ? Action::Set : s.action, s.raw, s.simd,
                        s.reference, environment, exact_work);
          if (!failed.status.ok()) {
            return Answer(failed.status);
          }
          if (failed.reason != FailureReason::None) {
            region_run_coordinate(selected, begin + failed.lane, &at);
            if (slot.alpha) {
              failed.operand = "alpha";
            }
            return Answer(sample_failure(s, failed, at));
          }
        } else {
          std::memcpy(result.data(), colors.data(), n * width);
        }
        if (!viewed) {
          for (std::uint64_t lane = 0; lane < n; ++lane) {
            region_run_coordinate(selected, begin + lane, &at);
            std::uint64_t offset = 0;
            for (std::size_t axis = 0; axis < at.size(); ++axis) {
              offset = offset * box.dimensions()[axis].extent + at[axis] -
                       box.dimensions()[axis].offset;
            }
            std::memcpy(writer->data() + offset * width,
                        result.data() + lane * width, width);
          }
        }
      }
    }
    if (writer) {
      auto value = std::move(*writer).publish(facets, phase.query.resources);
      if (!value.ok()) {
        return Answer(value.status());
      }
      auto retained = publication.retain(value.take_value());
      if (!retained.ok()) {
        return Answer(retained.status());
      }
      values.push_back(retained.take_value());
    } else {
      std::vector<std::uint64_t> origin;
      auto address = proof.value()->base;
      for (std::size_t axis = 0; axis < box.rank(); ++axis) {
        origin.push_back(box.dimensions()[axis].offset);
        address +=
            static_cast<__int128>(origin.back()) * proof.value()->strides[axis];
      }
      if (address < 0 || address > UINT64_MAX) {
        return Answer(invalid("view address overflow"));
      }
      auto view = Value::from_storage(
          descriptor, box,
          {static_cast<std::uint64_t>(address), proof.value()->strides, origin},
          proof.value()->owner->storage(), facets, phase.query.resources);
      if (!view.ok()) {
        return Answer(view.status());
      }
      auto retained = publication.retain(view.take_value());
      if (!retained.ok()) {
        return Answer(retained.status());
      }
      values.push_back(retained.take_value());
    }
  }
  if (phase.query.cancellation.cancelled()) {
    return Answer(
        Status{ErrorCode::Cancelled, "alpha cancelled before publication"});
  }
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}
struct Continuation final {
  const Preparation* state;
  bool requested = false;
  explicit Continuation(const Preparation* s) : state(s) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    auto result = evaluate(phase, *state);
    return result.ok() ? Result<DependencyPoll>(result.take_value())
                       : Result<DependencyPoll>(result.status());
  }
};
struct ReadSpan final {
  const std::uint8_t* data;
  std::uint64_t count;
  std::int64_t stride;
};
Result<ReadSpan> planar_read(const PlanarOperationInvocation& call,
                             const DependencyMappedNeed& map,
                             const std::vector<std::uint64_t>& at,
                             std::vector<std::uint64_t>* source,
                             std::uint32_t width_axis, std::uint64_t limit,
                             std::size_t width) {
  source_at(map, at, source);
  if (!call.exact_inputs) {
    auto run = call.inputs[map.port].row_run(*source);
    if (!run.ok()) {
      return Result<ReadSpan>(run.status());
    }
    return Result<ReadSpan>(ReadSpan{run.value().data, run.value().samples,
                                     static_cast<std::int64_t>(width)});
  }
  for (const auto& input : *call.exact_inputs) {
    if (input.port != map.port || !contains(input.region, *source)) {
      continue;
    }
    if (input.image.valid()) {
      auto run = input.image.row_run(*source);
      if (!run.ok()) {
        return Result<ReadSpan>(run.status());
      }
      return Result<ReadSpan>(ReadSpan{run.value().data, run.value().samples,
                                       static_cast<std::int64_t>(width)});
    }
    auto address = input.value.byte_address(*source);
    if (!address.ok()) {
      return Result<ReadSpan>(address.status());
    }
    std::int64_t stride = 0;
    for (std::size_t i = 0; i < map.axes.size(); ++i) {
      if (map.axes[i].observation_axis ==
          static_cast<std::int32_t>(width_axis)) {
        stride = input.value.layout().byte_strides[i];
        const auto d = input.region.dimensions()[i];
        limit = std::min(limit, d.offset + d.extent - (*source)[i]);
      }
    }
    return Result<ReadSpan>(
        ReadSpan{input.value.bytes().data() + address.value(), limit, stride});
  }
  return Result<ReadSpan>(Status{
      ErrorCode::NotFound, "alpha exact planar source coverage is missing"});
}
Status planar_alpha(const PlanarOperationInvocation& call) {
  const auto& s = *static_cast<const Preparation*>(call.prepared->state());
  if (s.layout == "view" && (!s.view_legal || !call.validate_only)) {
    return invalid(
        "ViewUnavailable: set-alpha requires an identity same-owner view");
  }
  const auto& layout = *call.output_metadata.planar_layout;
  const auto& dims = call.output_region.dimensions();
  const auto h = layout.height_axis, w = layout.width_axis;
  const auto channels =
      layout.channel_axis ? dims[*layout.channel_axis] : RegionDimension{0, 1};
  const std::size_t width = s.narrow ? 4 : 8;
  std::vector<std::uint64_t> at(dims.size()), source;
  for (std::size_t i = 0; i < dims.size(); ++i) {
    at[i] = dims[i].offset;
  }
  input_internal::Float32Environment environment;
  const auto* budget = resource_internal::metadata_budget();
  const std::function<Status(std::uint64_t)> exact_work =
      [&call, budget](std::uint64_t work) {
        if (call.cancellation.cancelled()) {
          return Status{ErrorCode::Cancelled,
                        "alpha exact arithmetic cancelled"};
        }
        return budget ? budget->consume({work}) : Status::success();
      };
  constexpr std::uint64_t block = 128;
  std::array<std::uint8_t, block * 8> xb{}, ab{}, yb{};
  const auto advance = [](ReadSpan* span, std::uint64_t n) {
    span->count -= n;
    // Do not form an out-of-bounds pointer after the last negative-stride
    // sample, even when that pointer would not subsequently be dereferenced.
    if (span->count) {
      span->data += static_cast<std::int64_t>(n) * span->stride;
    }
  };
  for (std::uint64_t channel = channels.offset;
       channel < channels.offset + channels.extent; ++channel) {
    if (layout.channel_axis) {
      at[*layout.channel_axis] = channel;
    }
    const auto& slot = s.slots[channel];
    const auto& map = s.data_maps[channel];
    for (std::uint64_t y = dims[h].offset; y < dims[h].offset + dims[h].extent;
         ++y) {
      at[h] = y;
      // Retain a checked row span across bounded arithmetic blocks. A tile or
      // exact fragment boundary expires the span before another access. This
      // avoids repeated owner/coordinate lookup on long continuous rows.
      ReadSpan color_span{nullptr, 0, 0}, alpha_span{nullptr, 0, 0};
      std::uint8_t* output_data = nullptr;
      std::uint64_t output_count = 0;
      for (std::uint64_t x = dims[w].offset;
           x < dims[w].offset + dims[w].extent;) {
        at[w] = x;
        if (call.cancellation.cancelled()) {
          return Status{ErrorCode::Cancelled, "alpha planar cancelled"};
        }
        const auto left = dims[w].offset + dims[w].extent - x;
        auto n = std::min(block, left);
        if (!color_span.count) {
          auto color = planar_read(call, map, at, &source, w, left, width);
          if (!color.ok()) {
            return color.status();
          }
          color_span = color.take_value();
        }
        n = std::min(n, color_span.count);
        if (slot.color) {
          if (!alpha_span.count) {
            auto alpha =
                planar_read(call, s.alpha_map, at, &source, w, left, width);
            if (!alpha.ok()) {
              return alpha.status();
            }
            alpha_span = alpha.take_value();
          }
          n = std::min(n, alpha_span.count);
        }
        std::uint8_t* destination = yb.data();
        if (!call.validate_only) {
          if (!output_count) {
            auto out = call.output.row_run(at);
            if (!out.ok()) {
              return out.status();
            }
            output_count = out.value().samples;
            output_data = out.value().data;
          }
          n = std::min(n, output_count);
          destination = output_data;
        }
        if (!n) {
          return Status{ErrorCode::Internal, "empty alpha row span"};
        }
        if (budget) {
          auto charged = budget->consume({n * 64});
          if (!charged.ok()) {
            return charged;
          }
        }
        const auto gather = [n, width](ReadSpan span, std::uint8_t* buffer) {
          if (span.stride == static_cast<std::int64_t>(width)) {
            return span.data;
          }
          for (std::uint64_t i = 0; i < n; ++i) {
            std::memcpy(buffer + i * width,
                        span.data + static_cast<std::int64_t>(i) * span.stride,
                        width);
          }
          return static_cast<const std::uint8_t*>(buffer);
        };
        const auto* c = gather(color_span, xb.data());
        if (slot.color || slot.alpha) {
          const auto* a =
              gather(slot.color ? alpha_span : color_span, ab.data());
          auto failed = math_span(c, a, destination, n, s.narrow,
                                  slot.alpha ? Action::Set : s.action, s.raw,
                                  s.simd, s.reference, environment, exact_work);
          if (!failed.status.ok()) {
            return failed.status;
          }
          if (failed.reason != FailureReason::None) {
            at[w] += failed.lane;
            if (slot.alpha) {
              failed.operand = "alpha";
            }
            return sample_failure(s, failed, at);
          }
        } else if (!call.validate_only) {
          std::memcpy(destination, c, n * width);
        }
        advance(&color_span, n);
        if (slot.color) {
          advance(&alpha_span, n);
        }
        if (!call.validate_only) {
          output_count -= n;
          if (output_count) {
            output_data += n * width;
          }
        }
        x += n;
      }
    }
  }
  return Status::success();
}
OperationDefinition alpha_definition(const std::string& key, Action action,
                                     numeric_ops::SequenceProfile profile) {
  OperationDefinition d;
  d.key = key;
  auto& t = d.traits;
  t.input_schema.resize(1);
  t.repeated_minimum = 1;
  t.repeated_maximum = 2;
  t.repeated_match = false;
  t.planar_storage_capable = true;
  t.planar_exact_dependencies = true;
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
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 1;
  out.continuation_bytes = sizeof(Continuation);
  out.maximum_dependency_stages = 2;
  d.prepare_static = [action, profile](const auto& inputs, const auto& params) {
    return prepare_alpha(inputs, params, action, profile);
  };
  d.start_dependency = [](const DependencyQuery& q,
                          const BufferAllocator& allocator) {
    return DependencyContinuation::make<Continuation>(
        allocator, static_cast<const Preparation*>(q.prepared->state()));
  };
  d.planar_callback = planar_alpha;
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
