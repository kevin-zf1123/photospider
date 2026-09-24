#include "execution/channel_assembly.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "photospider/format/channel_editing.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/utf8_validation.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Status invalid(const std::string& text) {
  return {ErrorCode::InvalidArgument,
          text,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
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
    return Answer(
        invalid(std::string(key) + ": expected canonical v1 records"));
  fields.erase(fields.begin());
  return Answer(std::move(fields));
}
Result<std::string> unhex(const std::string& text) {
  if (text.empty() || text.size() % 2 || text.size() > 256)
    return Result<std::string>(invalid("invalid selector hex length"));
  const auto digit = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
  };
  std::string out;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int a = digit(text[i]), b = digit(text[i + 1]);
    if (a < 0 || b < 0)
      return Result<std::string>(invalid("invalid selector hex"));
    out += static_cast<char>((a << 4) | b);
  }
  if (!valid_utf8_key(out))
    return Result<std::string>(invalid("invalid UTF-8 selector"));
  return Result<std::string>(std::move(out));
}
TensorInterpretation interpretation(const TensorDescription& d) {
  return {d.model,       d.primaries, d.transfer,     d.reference,
          d.association, d.white,     d.primaries_xy, d.profile};
}
bool empty(const TensorInterpretation& d) {
  return d.model.empty() && d.primaries.empty() && d.transfer.empty() &&
         d.reference.empty() && d.association.empty() && !d.white &&
         !d.primaries_xy && !d.profile;
}
Status overlay(TensorInterpretation* value, const TensorInterpretation& target,
               bool assertion) {
  if (!assertion && !target.model.empty() && target.model != value->model) {
    value->primaries.clear();
    value->transfer.clear();
    value->white.reset();
    value->primaries_xy.reset();
    value->profile.reset();
    value->association.clear();
  }
  const auto text = [assertion](std::string* a, const std::string& b) {
    if (b.empty())
      return true;
    if (assertion && !a->empty() && *a != b)
      return false;
    *a = b;
    return true;
  };
  if (!text(&value->model, target.model) ||
      !text(&value->primaries, target.primaries) ||
      !text(&value->transfer, target.transfer) ||
      !text(&value->reference, target.reference) ||
      !text(&value->association, target.association))
    return invalid("conflicting component interpretation");
  const auto field = [assertion](auto* a, const auto& b) {
    if (!b)
      return true;
    if (assertion && *a && !(**a == *b))
      return false;
    *a = b;
    return true;
  };
  if (!field(&value->white, target.white) ||
      !field(&value->primaries_xy, target.primaries_xy) ||
      !field(&value->profile, target.profile))
    return invalid("conflicting component reference");
  return Status::success();
}
Status overlay(TensorChannelDescription* value,
               const TensorChannelDescription& target, bool assertion) {
  for (auto pair : {std::make_pair(&value->name, &target.name),
                    std::make_pair(&value->role, &target.role),
                    std::make_pair(&value->unit, &target.unit)}) {
    if (pair.second->empty())
      continue;
    if (assertion && !pair.first->empty() && *pair.first != *pair.second)
      return invalid("conflicting destination component field");
    *pair.first = *pair.second;
  }
  if (target.interpretation) {
    if (!value->interpretation)
      value->interpretation.emplace();
    return overlay(&*value->interpretation, *target.interpretation, assertion);
  }
  return Status::success();
}
TensorChannelDescription component(const std::optional<TensorDescription>& d,
                                   std::uint64_t index, bool channels) {
  TensorChannelDescription out;
  if (!d)
    return out;
  if (channels && !d->channels.empty())
    out = d->channels[index];
  else if (!channels && d->component)
    out = *d->component;
  auto global = interpretation(*d);
  if (out.interpretation)
    overlay(&global, *out.interpretation, false);
  if (!empty(global))
    out.interpretation = global;
  if (channels) {
    for (const auto& group : d->groups)
      for (std::size_t j = 0; j < group.indices.size(); ++j)
        if (group.indices[j] == index) {
          overlay(&out, group.components[j], false);
          if (!out.interpretation)
            out.interpretation.emplace();
          overlay(&*out.interpretation, group.interpretation, false);
        }
  }
  return out;
}
// A bounded, lossless descriptor assertion. It is part of invocation identity
// and catches stale authoring hints on producer edges without evaluating them.
std::string input_assertion(const std::vector<OperationMetadata>& inputs) {
  std::string out = "v1";
  for (const auto& input : inputs) {
    out += ";" +
           std::to_string(static_cast<unsigned>(input.descriptor.element_type));
    for (auto extent : input.descriptor.shape)
      out += "," + std::to_string(extent);
    out += ":" + format::detail::assembly_hex(
                     format::detail::layout_assertion(input.planar_layout));
    for (const auto& f : input.facets) {
      out += ":" + format::detail::assembly_hex(f.key) + "," +
             std::to_string(f.version) + ",";
      out += format::detail::assembly_hex(
          std::string(f.payload.begin(), f.payload.end()));
    }
  }
  return out;
}
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs, const Params& p, int member,
    SequenceProfile profile) {
  using Answer = Result<OperationPreparation>;
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  if (p.count("expected_inputs") &&
      std::get<std::string>(p.at("expected_inputs")) != input_assertion(inputs))
    return Answer(
        invalid("FMT-03 authoring descriptors disagree with inference"));
  if (inputs.empty())
    return Answer(invalid("assembly requires at least one input"));
  const auto mode = std::get<std::string>(p.at("metadata_mode"));
  const auto layout = std::get<std::string>(p.at("layout"));
  if (mode != "respect" && mode != "raw" && mode != "override")
    return Answer(invalid("invalid metadata_mode"));
  if (layout != "auto" && layout != "view" && layout != "materialize")
    return Answer(invalid("invalid layout"));
  auto override_records = records(p, "input_overrides");
  if (!override_records.ok())
    return Answer(override_records.status());
  if ((mode == "override") != !override_records.value().empty())
    return Answer(
        invalid("input_overrides must occur exactly in override mode"));
  std::map<std::uint32_t, TensorDescription> overrides;
  std::uint64_t previous = 0;
  for (const auto& row : override_records.value()) {
    const auto fields = split(row, ':');
    std::uint64_t port = 0;
    if (fields.size() != 2 || !number(fields[0], &port) ||
        port >= inputs.size() || (!overrides.empty() && port <= previous))
      return Answer(
          invalid("input_overrides requires sorted unique input ordinals"));
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
    return Answer(
        invalid("axis/structure list length differs from input arity"));
  Assembly assembly;
  assembly.layout = layout;
  std::vector<std::optional<TensorDescription>> descriptions;
  std::vector<std::uint64_t> shape;
  std::optional<PlanarImageLayout> image_layout;
  bool any_image = false;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto& input = inputs[i];
    const auto& dimensions = input.descriptor.shape;
    const std::string where = "input " + std::to_string(i) + ": ";
    if (input.result_schema || dimensions.empty() || dimensions.size() > 8 ||
        input.descriptor.element_type != inputs[0].descriptor.element_type)
      return Answer(mismatch(where + "unsupported rank/type or mixed dtype"));
    auto extent = Region::whole(dimensions).element_count();
    if (!extent.ok())
      return Answer(extent.status());
    if (extent.value() == 0 || extent.value() > (1ULL << 40))
      return Answer(invalid(where + "invalid element count"));
    std::optional<TensorDescription> description;
    if (overrides.count(i)) {
      description = overrides.at(i);
    } else {
      for (const auto& facet : input.facets)
        if (facet.key == "photospider.tensor-description") {
          auto decoded = decode_tensor_description(facet);
          if (!decoded.ok()) {
            if (mode != "raw")
              return Answer(decoded.status());
          } else {
            description = decoded.take_value();
          }
        }
    }
    if (description) {
      auto status = validate_tensor_description(*description, input.descriptor);
      if (!status.ok()) {
        if (mode != "raw")
          return Answer(status);
        description.reset();
      }
    }
    bool scalar = member == 2 && axis_records.value()[i] == "s";
    if (scalar && (!p.count("authoring_member") || !p.count("expected_inputs")))
      return Answer(invalid("scalar fill is an internal FMT-03 lowering"));
    bool single = member == 0;
    std::optional<std::uint32_t> axis;
    if (member != 0) {
      std::string field =
          axis_records.value().empty() ? "_" : axis_records.value()[i];
      if (member == 2) {
        single = field == "c" || scalar;
        if (!single) {
          if (field.empty() || field[0] != 'h')
            return Answer(invalid(where + "structure must be c or h<axis>"));
          field.erase(field.begin());
        }
      }
      if (!single && field != "_") {
        std::uint64_t number_axis = 0;
        if (!number(field, &number_axis) || number_axis >= dimensions.size())
          return Answer(invalid(where + "channel axis out of range"));
        axis = number_axis;
      }
    }
    if (scalar) {
      if (dimensions != std::vector<std::uint64_t>{1} || input.planar_layout)
        return Answer(mismatch(where + "scalar requires generic shape [1]"));
    } else if (single) {
      if (dimensions.size() > 7 ||
          (mode != "raw" && description && description->channel_axis))
        return Answer(invalid(
            where + "component has an existing channel axis or rank eight"));
    } else {
      if (mode != "raw" && description && description->channel_axis) {
        if (axis && *axis != *description->channel_axis)
          return Answer(
              invalid(where + "axis assertion conflicts with description"));
        axis = description->channel_axis;
      }
      if (!axis)
        return Answer(
            invalid(where + "channel axis must be supplied or described"));
    }
    if (mode == "raw" && description && description->channel_axis != axis)
      description.reset();
    auto nonchannel = dimensions;
    if (axis)
      nonchannel.erase(nonchannel.begin() + *axis);
    if (i == 0) {
      if (scalar)
        return Answer(invalid("first source must establish the spatial grid"));
      shape = nonchannel;
    } else if (!scalar && shape != nonchannel) {
      return Answer(mismatch(where + "nonchannel extents differ"));
    }
    if (input.planar_layout) {
      any_image = true;
      auto structural = *input.planar_layout;
      if (structural.channel_axis != axis)
        return Answer(invalid(
            where + "interpretation cannot relabel physical image axes"));
      if (axis) {
        if (structural.height_axis > *axis)
          --structural.height_axis;
        if (structural.width_axis > *axis)
          --structural.width_axis;
      }
      structural.channel_axis.reset();
      structural.groups.clear();
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
    return Answer(invalid("output channel axis out of range"));
  assembly.axis = output_axis;
  std::map<std::uint64_t, TensorChannelDescription> mapped_targets;
  std::uint64_t channels = 0;
  if (member != 2) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      auto count =
          assembly.axes[i] ? inputs[i].descriptor.shape[*assembly.axes[i]] : 1;
      if (count > (1ULL << 40) - channels)
        return Answer(invalid("channel prefix overflow"));
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
      return Answer(invalid("mapping must be nonempty"));
    std::set<std::uint64_t> destinations;
    for (const auto& row : rows.value()) {
      auto fields = split(row, ',');
      std::uint64_t port = 0, dest = 0, selected = 0;
      if (fields.size() != 5 || !number(fields[0], &port) ||
          port >= inputs.size() || !number(fields[3], &dest) ||
          dest >= channels || !destinations.insert(dest).second)
        return Answer(invalid(
            "mapping has invalid source, duplicate destination or hole"));
      auto selector = unhex(fields[2]);
      if (!selector.ok())
        return Answer(selector.status());
      auto count = assembly.axes[port]
                       ? inputs[port].descriptor.shape[*assembly.axes[port]]
                       : 1;
      if (fields[1] == "index") {
        if (!number(selector.value(), &selected) || selected >= count)
          return Answer(invalid("source selector index out of range"));
      } else {
        if (mode == "raw" || (fields[1] != "name" && fields[1] != "role"))
          return Answer(invalid("raw permits only index selectors"));
        const auto& desc = descriptions[port];
        if (!desc || (assembly.axes[port] && desc->channels.size() != count) ||
            (!assembly.axes[port] && !desc->component))
          return Answer(
              invalid("named selector requires component descriptions"));
        bool found = false;
        for (std::uint64_t index = 0; index < count; ++index) {
          const auto& c =
              assembly.axes[port] ? desc->channels[index] : *desc->component;
          if ((fields[1] == "name" ? c.name : c.role) != selector.value())
            continue;
          if (found)
            return Answer(invalid("ambiguous named source selector"));
          selected = index;
          found = true;
        }
        if (!found)
          return Answer(invalid("source selector absent"));
      }
      if (fields[4] != "_") {
        auto target = tensor_description_from_parameter(fields[4]);
        if (!target.ok())
          return Answer(target.status());
        TensorDescription only;
        only.component = target.value().component;
        auto canonical = tensor_description_parameter(only);
        if (!only.component || !canonical.ok() ||
            canonical.value() != fields[4])
          return Answer(
              invalid("mapping destination must contain only a component"));
        mapped_targets.emplace(dest, *only.component);
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
  output.metadata.descriptor = {inputs[0].descriptor.element_type, shape};
  output.metadata.descriptor.shape.insert(
      output.metadata.descriptor.shape.begin() + output_axis, channels);
  auto output_count =
      Region::whole(output.metadata.descriptor.shape).element_count();
  if (!output_count.ok())
    return Answer(output_count.status());
  if (output_count.value() > (1ULL << 40))
    return Answer(invalid("output exceeds element limit"));
  TensorDescription result;
  result.channel_axis = assembly.axis;
  std::optional<TensorDescription> target;
  if (p.count("output_description")) {
    auto decoded = tensor_description_from_parameter(
        std::get<std::string>(p.at("output_description")));
    if (!decoded.ok())
      return Answer(decoded.status());
    target = decoded.take_value();
    auto status =
        validate_tensor_description(*target, output.metadata.descriptor);
    if (!status.ok())
      return Answer(status);
    if ((target->channel_axis && *target->channel_axis != assembly.axis) ||
        target->component)
      return Answer(
          invalid("output description disagrees with channel structure"));
    result.groups = target->groups;
  }
  // Coordinate assertions apply to every connected input, including unused
  // ones.
  std::optional<std::vector<TensorAxisDescription>> common_axes;
  bool all_axes = true;
  for (std::size_t i = 0; i < descriptions.size(); ++i) {
    if (assembly.scalars[i])
      continue;
    if (mode == "raw" || !descriptions[i] || descriptions[i]->axes.empty()) {
      all_axes = false;
      continue;
    }
    auto axes = descriptions[i]->axes;
    if (assembly.axes[i])
      axes.erase(axes.begin() + *assembly.axes[i]);
    if (common_axes) {
      for (std::size_t j = 0; j < axes.size(); ++j) {
        auto& old = (*common_axes)[j];
        const auto& fresh = axes[j];
        if ((!old.name.empty() && !fresh.name.empty() &&
             old.name != fresh.name) ||
            (!old.unit.empty() && !fresh.unit.empty() &&
             old.unit != fresh.unit) ||
            old.origin != fresh.origin || old.step != fresh.step)
          return Answer(invalid("input " + std::to_string(i) +
                                ": conflicting nonchannel grid"));
        if (old.name != fresh.name)
          old.name.clear();
        if (old.unit != fresh.unit)
          old.unit.clear();
      }
    } else {
      common_axes = std::move(axes);
    }
  }
  if (common_axes && all_axes) {
    result.axes = *common_axes;
    result.axes.insert(result.axes.begin() + assembly.axis,
                       TensorAxisDescription{});
  }
  if (target && !target->axes.empty()) {
    if (common_axes) {
      for (std::size_t j = 0; j < shape.size(); ++j) {
        const auto& a = (*common_axes)[j];
        const auto& b = target->axes[j < assembly.axis ? j : j + 1];
        if ((!a.name.empty() && !b.name.empty() && a.name != b.name) ||
            (!a.unit.empty() && !b.unit.empty() && a.unit != b.unit) ||
            a.origin != b.origin || a.step != b.step)
          return Answer(
              invalid("target cannot override nonchannel coordinate grid"));
      }
    }
    result.axes = target->axes;
  }
  const bool complete = p.count("output_description_complete") &&
                        std::get<bool>(p.at("output_description_complete"));
  if (complete && !target)
    return Answer(invalid("complete output description is absent"));
  bool semantic = target.has_value() || !mapped_targets.empty();
  for (const auto& d : descriptions)
    semantic =
        semantic || (d && (!d->channels.empty() || d->component ||
                           !empty(interpretation(*d)) || !d->groups.empty()));
  if (semantic) {
    if (channels > 1024)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "channel metadata capacity"});
    result.channels.resize(channels);
    TensorInterpretation common;
    for (const auto& span : assembly.spans) {
      for (std::uint64_t j = 0; j < span.count; ++j) {
        const auto k = span.destination + j;
        auto value = complete
                         ? TensorChannelDescription{}
                         : component(descriptions[span.port], span.source + j,
                                     assembly.axes[span.port].has_value());
        TensorChannelDescription assignment;
        if (target) {
          auto global = interpretation(*target);
          if (!empty(global))
            assignment.interpretation = global;
          if (!target->channels.empty()) {
            auto status = overlay(&assignment, target->channels[k], true);
            if (!status.ok())
              return Answer(status);
          }
          for (const auto& group : target->groups)
            for (std::size_t g = 0; g < group.indices.size(); ++g)
              if (group.indices[g] == k) {
                auto status = overlay(&assignment, group.components[g], true);
                if (!status.ok())
                  return Answer(status);
                if (!assignment.interpretation)
                  assignment.interpretation.emplace();
                status = overlay(&*assignment.interpretation,
                                 group.interpretation, true);
                if (!status.ok())
                  return Answer(status);
              }
        }
        if (mapped_targets.count(k)) {
          auto status = overlay(&assignment, mapped_targets.at(k), true);
          if (!status.ok())
            return Answer(status);
        }
        auto status = overlay(&value, assignment, false);
        if (!status.ok())
          return Answer(status);
        // Independent explicit target groups may intentionally differ. Without
        // a target interpretation, retain respect's common-field assertion.
        if (mode != "raw" && value.interpretation) {
          auto respected = *value.interpretation;
          if (assignment.interpretation) {
            const auto& assigned = *assignment.interpretation;
            if (!assigned.model.empty()) {
              respected.model.clear();
              respected.primaries.clear();
              respected.transfer.clear();
              respected.association.clear();
              respected.white.reset();
              respected.primaries_xy.reset();
              respected.profile.reset();
            }
            if (!assigned.primaries.empty())
              respected.primaries.clear();
            if (!assigned.transfer.empty())
              respected.transfer.clear();
            if (!assigned.reference.empty())
              respected.reference.clear();
            if (!assigned.association.empty())
              respected.association.clear();
            if (assigned.white)
              respected.white.reset();
            if (assigned.primaries_xy)
              respected.primaries_xy.reset();
            if (assigned.profile)
              respected.profile.reset();
          }
          status = overlay(&common, respected, true);
          if (!status.ok())
            return Answer(status);
        }
        result.channels[k] = std::move(value);
      }
    }
  }
  auto encoded = encode_tensor_description(result);
  if (!encoded.ok())
    return Answer(encoded.status());
  output.metadata.facets = {encoded.take_value()};
  if (any_image) {
    auto physical = *image_layout;
    if (physical.height_axis >= assembly.axis)
      ++physical.height_axis;
    if (physical.width_axis >= assembly.axis)
      ++physical.width_axis;
    physical.channel_axis = assembly.axis;
    output.metadata.planar_layout = std::move(physical);
  }
  std::vector<DependencyMapPiece> pieces;
  for (const auto& span : assembly.spans) {
    auto dims = Region::whole(output.metadata.descriptor.shape).dimensions();
    dims[assembly.axis] = {span.destination, span.count};
    auto coverage = Footprint::from_regions(output.metadata.descriptor.shape,
                                            {Region(dims)});
    if (!coverage.ok())
      return Answer(coverage.status());
    DependencyMappedNeed need;
    need.port = span.port;
    const auto source_axis = assembly.axes[span.port];
    const auto rank = inputs[span.port].descriptor.shape.size();
    for (std::size_t a = 0; a < rank; ++a) {
      DependencyAxis axis;
      if (assembly.scalars[span.port]) {
        axis.observation_axis = -1;
        axis.fixed = {0, 1};
      } else if (source_axis && a == *source_axis) {
        axis.observation_axis = assembly.axis;
        axis.translation = static_cast<std::int64_t>(span.source) -
                           static_cast<std::int64_t>(span.destination);
      } else {
        auto spatial = a - (source_axis && a > *source_axis ? 1 : 0);
        axis.observation_axis = spatial < assembly.axis ? spatial : spatial + 1;
      }
      need.axes.push_back(axis);
    }
    pieces.push_back({coverage.take_value(), {std::move(need)}});
  }
  output.static_dependency_pieces = std::move(pieces);
  output.regional_atomic = true;
  output.preserve_output_views = layout != "materialize";
  OperationPreparation prepared;
  prepared.outputs.push_back(std::move(output));
  prepared.state = std::make_shared<Assembly>(std::move(assembly));
  return Answer(std::move(prepared));
}

Result<ValueFragments> evaluate(const DependencyPhase& phase,
                                const Assembly& a) {
  using Answer = Result<ValueFragments>;
  const auto& descriptor = phase.query.output.descriptor;
  const auto& facets = phase.query.output.facets;
  const auto width = Value::element_size(descriptor.element_type);
  // Every projected fragment has an exact affine mapping and retains its
  // source.
  std::vector<Value> views;
  for (const auto& box : phase.query.outputs.boxes())
    for (const auto& span : a.spans) {
      const auto c = box.dimensions()[a.axis];
      auto first = std::max(c.offset, span.destination);
      auto last = std::min(c.offset + c.extent, span.destination + span.count);
      if (first >= last)
        continue;
      for (const auto& source : phase.inputs[span.port].fragments()) {
        auto charged = phase.consume_work(1 + descriptor.shape.size());
        if (!charged.ok())
          return Answer(charged);
        auto dims = box.dimensions();
        dims[a.axis] = {first, last - first};
        const auto source_axis = a.axes[span.port];
        std::vector<std::uint64_t> source_at(source.region().rank());
        std::vector<std::int64_t> strides(descriptor.shape.size(), 0);
        bool intersects = true;
        if (a.scalars[span.port])
          source_at[0] = 0;
        for (std::size_t i = 0; !a.scalars[span.port] && i < source_at.size();
             ++i) {
          const auto input = source.region().dimensions()[i];
          std::size_t target;
          std::int64_t shift = 0;
          if (source_axis && i == *source_axis) {
            target = a.axis;
            shift = static_cast<std::int64_t>(span.source) -
                    static_cast<std::int64_t>(span.destination);
          } else {
            auto j = i - (source_axis && i > *source_axis ? 1 : 0);
            target = j < a.axis ? j : j + 1;
          }
          const auto start = std::max<__int128>(
              dims[target].offset, static_cast<__int128>(input.offset) - shift);
          const auto end = std::min<__int128>(
              dims[target].offset + dims[target].extent,
              static_cast<__int128>(input.offset) + input.extent - shift);
          if (start >= end) {
            intersects = false;
            break;
          }
          dims[target] = {static_cast<std::uint64_t>(start),
                          static_cast<std::uint64_t>(end - start)};
          source_at[i] = static_cast<std::uint64_t>(start + shift);
          strides[target] = source.layout().byte_strides[i];
        }
        if (!intersects)
          continue;
        auto address = source.byte_address(source_at);
        if (!address.ok())
          return Answer(address.status());
        std::vector<std::uint64_t> origin;
        for (const auto& dim : dims)
          origin.push_back(dim.offset);
        auto view = Value::from_storage(
            descriptor, Region(dims), {address.value(), strides, origin},
            source.storage(), facets, phase.query.resources);
        if (!view.ok())
          return Answer(view.status());
        views.push_back(view.take_value());
      }
    }
  bool legal_view = !views.empty();
  // A generic result may have several authorized fragments, but a forced view
  // requires a common owner and one global affine address map.
  std::vector<std::int64_t> canonical;
  __int128 base = 0;
  if (legal_view) {
    canonical = views[0].layout().byte_strides;
    const auto& v = views[0];
    bool channel_stride_found = v.region().dimensions()[a.axis].extent > 1;
    for (const auto& next : views) {
      if (next.storage().get() != v.storage().get()) {
        legal_view = false;
        break;
      }
      for (std::size_t d = 0; d < canonical.size(); ++d)
        if (d != a.axis && canonical[d] != next.layout().byte_strides[d])
          legal_view = false;
      if (!channel_stride_found && next.region().dimensions()[a.axis].offset !=
                                       v.region().dimensions()[a.axis].offset) {
        __int128 delta = static_cast<__int128>(next.layout().byte_offset) -
                         v.layout().byte_offset;
        for (std::size_t d = 0; d < canonical.size(); ++d)
          if (d != a.axis)
            delta -= (static_cast<__int128>(next.layout().origin[d]) -
                      v.layout().origin[d]) *
                     canonical[d];
        const auto dc = static_cast<__int128>(next.layout().origin[a.axis]) -
                        v.layout().origin[a.axis];
        if (delta % dc || delta / dc < INT64_MIN || delta / dc > INT64_MAX) {
          legal_view = false;
        } else {
          canonical[a.axis] = static_cast<std::int64_t>(delta / dc);
          channel_stride_found = true;
        }
      }
    }
    base = v.layout().byte_offset;
    for (std::size_t d = 0; d < canonical.size(); ++d)
      base -= static_cast<__int128>(v.layout().origin[d]) * canonical[d];
    for (const auto& v2 : views) {
      __int128 expected = base;
      for (std::size_t d = 0; d < canonical.size(); ++d)
        expected += static_cast<__int128>(v2.layout().origin[d]) * canonical[d];
      if (expected != v2.layout().byte_offset ||
          (v2.region().dimensions()[a.axis].extent > 1 &&
           canonical[a.axis] != v2.layout().byte_strides[a.axis]))
        legal_view = false;
    }
  }
  if (a.layout == "view" && !legal_view)
    return Answer(invalid("ViewUnavailable: no common affine owner mapping"));
  numeric_ops::ArrayPublication publication(
      views.size() + phase.query.outputs.boxes().size(),
      descriptor.shape.size());
  std::vector<Value> values;
  if (a.layout != "materialize" && legal_view) {
    for (const auto& box : phase.query.outputs.boxes()) {
      std::vector<std::uint64_t> origin;
      __int128 address = base;
      for (std::size_t d = 0; d < canonical.size(); ++d) {
        origin.push_back(box.dimensions()[d].offset);
        address += static_cast<__int128>(origin.back()) * canonical[d];
      }
      if (address < 0 || address > UINT64_MAX)
        return Answer(invalid("view offset overflow"));
      auto view = Value::from_storage(
          descriptor, box,
          {static_cast<std::uint64_t>(address), canonical, origin},
          views[0].storage(), facets, phase.query.resources);
      if (!view.ok())
        return Answer(view.status());
      auto kept = publication.retain(view.take_value());
      if (!kept.ok())
        return Answer(kept.status());
      values.push_back(kept.take_value());
    }
  } else {
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      for (const auto& view : views) {
        auto intersection = Footprint::from_regions(
            descriptor.shape, {view.region()}, phase.sets);
        if (!intersection.ok())
          return Answer(intersection.status());
        auto region =
            Footprint::from_regions(descriptor.shape, {box}, phase.sets);
        if (!region.ok())
          return Answer(region.status());
        auto clipped =
            intersection.value().intersect(region.value(), phase.sets);
        if (!clipped.ok())
          return Answer(clipped.status());
        auto copied = clipped.value().visit(
            [&](const auto& at) {
              auto charged = phase.consume_work(at.size() + width);
              if (!charged.ok())
                return charged;
              auto address = view.byte_address(at);
              if (!address.ok())
                return address.status();
              std::uint64_t offset = 0;
              for (std::size_t d = 0; d < at.size(); ++d)
                offset = offset * box.dimensions()[d].extent + at[d] -
                         box.dimensions()[d].offset;
              std::memcpy(writer.data() + offset * width,
                          view.bytes().data() + address.value(), width);
              return Status::success();
            },
            phase.sets.maximum_work, phase.query.cancellation);
        if (!copied.ok())
          return Answer(copied);
      }
      auto value = std::move(writer).publish(facets, phase.query.resources);
      if (!value.ok())
        return Answer(value.status());
      auto kept = publication.retain(value.take_value());
      if (!kept.ok())
        return Answer(kept.status());
      values.push_back(kept.take_value());
    }
  }
  if (phase.query.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, "assembly cancelled"});
  return publication.finish(descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, facets,
                            phase.query.resources);
}
struct State final {
  const Assembly* assembly;
  bool requested = false;
  explicit State(const Assembly* value) : assembly(value) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    auto result = evaluate(phase, *assembly);
    return result.ok() ? Result<DependencyPoll>(result.take_value())
                       : Result<DependencyPoll>(result.status());
  }
};
OperationDefinition definition(const std::string& key, int member,
                               SequenceProfile profile) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  t.input_schema.resize(1);
  t.repeated_minimum = 1;
  t.repeated_maximum = 1024;
  t.repeated_match = false;
  t.planar_storage_capable = true;
  t.cacheable = false;
  t.requires_metadata_specialization = true;
  t.parameter_schema = {
      {"input_overrides", OperationParameterType::String, false},
      {"layout", OperationParameterType::String},
      {"metadata_mode", OperationParameterType::String},
      {"output_description", OperationParameterType::String, false},
      {"output_description_complete", OperationParameterType::Bool, false},
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
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 1;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 2;
  op.prepare_static = [member, profile](const auto& inputs,
                                        const auto& params) {
    return prepare(inputs, params, member, profile);
  };
  op.start_dependency = [](const DependencyQuery& query,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(
        allocator, static_cast<const Assembly*>(query.prepared->state()));
  };
  op.planar_callback = [member,
                        profile](const PlanarOperationInvocation& call) {
    if (std::get<std::string>(call.parameters.at("layout")) == "view")
      return invalid(
          "ViewUnavailable: direct planar output cannot replace its owner");
    std::vector<OperationMetadata> metadata;
    for (const auto& input : call.inputs) {
      const auto& c = input.config();
      metadata.push_back(
          {input.descriptor(),
           input.facets(),
           {},
           0,
           PlanarImageLayout{c.order, c.height_axis, c.width_axis,
                             c.channel_axis, c.row_pitch_bytes, c.groups}});
    }
    auto prepared = prepare(metadata, call.parameters, member, profile);
    if (!prepared.ok())
      return prepared.status();
    const auto& out = prepared.value().outputs[0];
    auto requested = Footprint::from_regions(out.metadata.descriptor.shape,
                                             {call.output_region});
    if (!requested.ok())
      return requested.status();
    for (const auto& piece : *out.static_dependency_pieces) {
      auto clipped = piece.coverage.intersect(requested.value());
      if (!clipped.ok())
        return clipped.status();
      const auto& map = piece.inputs[0];
      for (const auto& box : clipped.value().boxes()) {
        auto status = execution_internal::copy_channel_piece(
            box, map, &call.inputs[map.port], nullptr, call.output,
            *out.metadata.planar_layout,
            Value::element_size(out.metadata.descriptor.element_type),
            call.cancellation);
        if (!status.ok())
          return status;
      }
    }
    return Status::success();
  };
  return op;
}
Result<std::vector<std::uint8_t>> literal_bytes(const Params& params) {
  using Answer = Result<std::vector<std::uint8_t>>;
  const auto raw_type = std::get<std::int64_t>(params.at("dtype"));
  if (raw_type < 0 || raw_type > UINT32_MAX)
    return Answer(invalid("unsupported typed literal dtype"));
  const auto type = static_cast<ElementType>(raw_type);
  if (type != ElementType::UInt8 && type != ElementType::Int8 &&
      type != ElementType::UInt16 && type != ElementType::Int16 &&
      type != ElementType::Int64 && type != ElementType::Float32 &&
      type != ElementType::Float64)
    return Answer(invalid("unsupported typed literal dtype"));
  const auto width = Value::element_size(type);
  const auto& bits = std::get<std::string>(params.at("bits"));
  if (!width || bits.size() != width * 2)
    return Answer(invalid("typed literal dtype/bit width mismatch"));
  std::vector<std::uint8_t> result;
  for (std::size_t i = 0; i < bits.size(); i += 2) {
    unsigned byte = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const auto c = bits[i + j];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return Answer(invalid("typed literal requires lowercase hex bytes"));
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
  t.cacheable = false;
  t.parameter_schema = {{"dtype", OperationParameterType::Int64},
                        {"bits", OperationParameterType::String}};
  auto& out = t.outputs[0];
  out.key = "values";
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  out.region_rule = OperationRegionRule::Whole;
  op.specialize_metadata = [profile](const auto&, const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto status = numeric_ops::sequence_profile_available(profile);
    if (!status.ok())
      return Answer(status);
    auto bytes = literal_bytes(parameters);
    if (!bytes.ok())
      return Answer(bytes.status());
    OperationOutputSpecialization result;
    result.metadata.descriptor = {
        static_cast<ElementType>(
            std::get<std::int64_t>(parameters.at("dtype"))),
        {1}};
    return Answer(std::vector<OperationOutputSpecialization>{result});
  };
  op.callback = [](const OperationInvocation& call) {
    using Answer = Result<Value>;
    if (call.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, "literal cancelled"});
    auto bytes = literal_bytes(call.parameters);
    if (!bytes.ok())
      return Answer(bytes.status());
    const ValueDescriptor descriptor{
        static_cast<ElementType>(
            std::get<std::int64_t>(call.parameters.at("dtype"))),
        {1}};
    auto allocated =
        MutableValue::allocate(descriptor, call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto writer = allocated.take_value();
    std::memcpy(writer.data(), bytes.value().data(), bytes.value().size());
    return std::move(writer).publish();
  };
  return op;
}
Result<std::optional<TensorDescription>> edit_description(
    const OperationMetadata& metadata, std::uint32_t port,
    const format::ChannelAssemblyOptions& options) {
  using Answer = Result<std::optional<TensorDescription>>;
  std::optional<TensorDescription> result;
  if (options.input_overrides.count(port)) {
    result = options.input_overrides.at(port);
  } else {
    for (const auto& f : metadata.facets)
      if (f.key == "photospider.tensor-description") {
        auto decoded = decode_tensor_description(f);
        if (!decoded.ok()) {
          if (options.metadata_mode != "raw")
            return Answer(decoded.status());
        } else {
          result = decoded.take_value();
        }
      }
  }
  if (result) {
    auto valid = validate_tensor_description(*result, metadata.descriptor);
    if (!valid.ok()) {
      if (options.metadata_mode != "raw")
        return Answer(valid);
      result.reset();
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
      return Answer(invalid("FMT-03 index outside source/destination"));
    return Answer(selected);
  }
  if (raw || (selector.match != "name" && selector.match != "role") ||
      selector.value.empty() || selector.value.size() > 128 || !desc ||
      (channels ? desc->channels.size() != count : !desc->component))
    return Answer(invalid("FMT-03 invalid name/role selector"));
  bool found = false;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto& c = channels ? desc->channels[i] : *desc->component;
    if ((selector.match == "name" ? c.name : c.role) == selector.value) {
      if (found)
        return Answer(invalid("FMT-03 ambiguous name/role selector"));
      selected = i;
      found = true;
    }
  }
  return found ? Answer(selected) : Answer(invalid("FMT-03 selector absent"));
}
Result<WorkflowNodeOutput> edit_channels(
    WorkflowDocument& document, std::vector<format::ChannelEditInput> inputs,
    const std::vector<format::ChannelEditSource>& slots,
    const std::vector<format::ChannelReplacement>* replacements,
    const format::ChannelAssemblyOptions& options) {
  using Answer = Result<WorkflowNodeOutput>;
  if (inputs.empty() || inputs.size() > 1024 || inputs[0].structure.component ||
      inputs[0].structure.scalar ||
      (options.metadata_mode != "respect" && options.metadata_mode != "raw" &&
       options.metadata_mode != "override") ||
      (options.metadata_mode == "override") != !options.input_overrides.empty())
    return Answer(invalid("FMT-03 invalid base/metadata mode/arity"));
  if (!replacements) {
    for (std::size_t i = 1; i < inputs.size(); ++i)
      if (!inputs[i].structure.scalar)
        return Answer(
            invalid("FMT-03A external inputs must be explicit scalars"));
  }
  for (const auto& entry : options.input_overrides)
    if (entry.first >= inputs.size())
      return Answer(invalid("FMT-03 override input ordinal is absent"));
  const auto base = inputs[0].metadata;
  const auto dtype = base.descriptor.element_type;
  if (dtype != ElementType::UInt8 && dtype != ElementType::Int8 &&
      dtype != ElementType::UInt16 && dtype != ElementType::Int16 &&
      dtype != ElementType::Int64 && dtype != ElementType::Float32 &&
      dtype != ElementType::Float64)
    return Answer(mismatch("FMT-03 unsupported base dtype"));
  if (base.result_schema || base.descriptor.shape.empty() ||
      base.descriptor.shape.size() > 8)
    return Answer(mismatch("FMT-03 base requires positive rank 1..8"));
  auto base_description = edit_description(base, 0, options);
  if (!base_description.ok())
    return Answer(base_description.status());
  auto axis = inputs[0].structure.axis;
  auto desc = base_description.take_value();
  if (options.metadata_mode != "raw" && desc && desc->channel_axis) {
    if (axis && axis != desc->channel_axis)
      return Answer(invalid("FMT-03 base axis disagrees with metadata"));
    axis = desc->channel_axis;
  }
  if (!axis || *axis >= base.descriptor.shape.size())
    return Answer(invalid("FMT-03 base axis required/in range"));
  if (options.metadata_mode == "raw" && desc && desc->channel_axis != axis)
    desc.reset();
  inputs[0].structure.axis = axis;
  const auto count = base.descriptor.shape[*axis];
  const auto output_count = replacements ? count : slots.size();
  if (!output_count || output_count > 1024 || count > (1ULL << 40))
    return Answer(invalid("FMT-03 empty slots or mapping capacity exceeded"));
  std::vector<format::ChannelEditSource> effective = slots;
  std::set<std::uint64_t> changed;
  if (replacements) {
    effective.resize(count);
    for (std::uint64_t i = 0; i < count; ++i)
      effective[i].selector.value = std::to_string(i);
    for (const auto& r : *replacements) {
      auto destination = edit_select(r.destination, count, desc, true,
                                     options.metadata_mode == "raw");
      if (!destination.ok())
        return Answer(destination.status());
      if (!changed.insert(destination.value()).second)
        return Answer(invalid("FMT-03B duplicate destination"));
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
    const auto width = Value::element_size(literal.dtype);
    if (!width || literal.bytes.size() != width)
      return Answer(invalid("FMT-03 malformed typed literal"));
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
      input.metadata.descriptor = {literal.dtype, {1}};
      input.structure.scalar = true;
      literals[bits] = inputs.size();
      inputs.push_back(std::move(input));
    }
    e.input = literals.at(bits);
    e.selector = {};
  }
  if (inputs.size() > 1024)
    return Answer(invalid("FMT-03 expanded input capacity exceeded"));
  std::vector<OperationMetadata> metadata;
  std::vector<format::ChannelEditStructure> structures;
  references.clear();
  for (const auto& input : inputs) {
    if ((input.structure.component && input.structure.axis) ||
        (input.structure.scalar &&
         (input.structure.component || input.structure.axis)))
      return Answer(invalid("FMT-03 conflicting source structure"));
    if (input.metadata.result_schema)
      return Answer(mismatch("FMT-03 requires tensor inputs"));
    // Supplied declaration metadata is checked before committing the expansion.
    if (const auto* ref = std::get_if<WorkflowInputReference>(&input.input)) {
      bool found = false;
      for (const auto& declaration : document.inputs)
        if (declaration.id == ref->input_id) {
          OperationMetadata actual;
          actual.descriptor = declaration.descriptor;
          actual.facets = declaration.facets;
          actual.planar_layout = declaration.planar_layout;
          if (input_assertion({actual}) != input_assertion({input.metadata}))
            return Answer(
                invalid("FMT-03 descriptor disagrees with declaration"));
          found = true;
        }
      if (!found)
        return Answer(invalid("FMT-03 input declaration absent"));
    }
    metadata.push_back(input.metadata);
    references.push_back(input.input);
    structures.push_back(input.structure);
  }
  std::vector<format::ChannelMapping> mapping;
  std::vector<std::optional<std::uint64_t>> base_selection(output_count);
  TensorDescription target;
  target.channel_axis = axis;
  target.channels.resize(output_count);
  if (desc && !desc->axes.empty() && options.metadata_mode != "raw")
    target.axes = desc->axes;
  for (std::size_t k = 0; k < effective.size(); ++k) {
    const auto& e = effective[k];
    if (e.input >= inputs.size() ||
        (!replacements && e.input != 0 && !inputs[e.input].structure.scalar))
      return Answer(invalid("FMT-03A source must be base or scalar"));
    auto d = edit_description(metadata[e.input], e.input, options);
    if (!d.ok())
      return Answer(d.status());
    const auto& structure = structures[e.input];
    auto a = structure.axis;
    if (options.metadata_mode != "raw" && d.value() &&
        d.value()->channel_axis && !structure.scalar && !structure.component)
      a = d.value()->channel_axis;
    const auto& shape = metadata[e.input].descriptor.shape;
    if (shape.empty() || (a && *a >= shape.size()))
      return Answer(mismatch("FMT-03 source rank/axis mismatch"));
    const auto n = a ? shape[*a] : 1;
    auto selected = edit_select(e.selector, n, d.value(), a.has_value(),
                                options.metadata_mode == "raw");
    if (!selected.ok())
      return Answer(selected.status());
    mapping.push_back(
        {e.input, "index", std::to_string(selected.value()), k, {}});
    if (e.input == 0)
      base_selection[k] = selected.value();
    target.channels[k] = replacements ? component(desc, k, true)
                         : e.input == 0
                             ? component(desc, selected.value(), true)
                             : TensorChannelDescription{};
  }
  if (desc) {
    for (auto group : desc->groups) {
      bool keep = true;
      const auto remap = [&](std::uint64_t source, std::uint64_t* destination) {
        if (replacements) {
          *destination = source;
          return true;
        }
        unsigned matches = 0;
        for (std::size_t k = 0; k < base_selection.size(); ++k)
          if (base_selection[k] == source) {
            *destination = k;
            ++matches;
          }
        return matches == 1;
      };
      for (auto& index : group.indices)
        keep = remap(index, &index) && keep;
      if (group.alpha)
        keep = remap(*group.alpha, &*group.alpha) && keep;
      if (keep)
        target.groups.push_back(std::move(group));
    }
  }
  if (options.output_description) {
    const auto& explicit_target = *options.output_description;
    auto shape = base.descriptor;
    shape.shape[*axis] = output_count;
    auto valid = validate_tensor_description(explicit_target, shape);
    if (!valid.ok())
      return Answer(valid);
    if (explicit_target.component ||
        (explicit_target.channel_axis && explicit_target.channel_axis != axis))
      return Answer(invalid("FMT-03 output channel structure mismatch"));
    for (std::size_t k = 0; k < output_count; ++k) {
      auto assigned = target.channels[k];
      auto global = interpretation(explicit_target);
      if (!empty(global)) {
        if (!assigned.interpretation)
          assigned.interpretation.emplace();
        auto status = overlay(&*assigned.interpretation, global, false);
        if (!status.ok())
          return Answer(status);
      }
      if (!explicit_target.channels.empty()) {
        auto status = overlay(&assigned, explicit_target.channels[k], false);
        if (!status.ok())
          return Answer(status);
      }
      for (const auto& group : explicit_target.groups)
        for (std::size_t j = 0; j < group.indices.size(); ++j)
          if (group.indices[j] == k) {
            auto status = overlay(&assigned, group.components[j], false);
            if (!status.ok())
              return Answer(status);
            if (!assigned.interpretation)
              assigned.interpretation.emplace();
            status =
                overlay(&*assigned.interpretation, group.interpretation, false);
            if (!status.ok())
              return Answer(status);
          }
      TensorDescription before, after;
      before.component = target.channels[k];
      after.component = assigned;
      if (replacements && !changed.count(k) &&
          tensor_description_parameter(before).value() !=
              tensor_description_parameter(after).value())
        return Answer(invalid("FMT-03B target changes an unlisted component"));
      target.channels[k] = std::move(assigned);
    }
    if (!explicit_target.axes.empty())
      target.axes = explicit_target.axes;
    std::vector<TensorColorGroup> retained;
    for (const auto& group : target.groups) {
      TensorDescription trial = target;
      trial.groups = {group};
      if (encode_tensor_description(trial).ok())
        retained.push_back(group);
    }
    // Explicit groups replace overlapping/namesake groups only. Independent
    // complete groups retain their original interpretation.
    target.groups.clear();
    for (const auto& old : retained) {
      bool replaced = false;
      for (const auto& fresh : explicit_target.groups) {
        replaced = replaced || old.name == fresh.name;
        for (auto index : old.indices)
          replaced =
              replaced || std::find(fresh.indices.begin(), fresh.indices.end(),
                                    index) != fresh.indices.end();
      }
      if (!replaced)
        target.groups.push_back(old);
    }
    target.groups.insert(target.groups.end(), explicit_target.groups.begin(),
                         explicit_target.groups.end());
  }
  auto lowered_options = options;
  lowered_options.output_description = target;
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
  auto output = format::assemble_mapped_channels(staged, references, *axis,
                                                 primitive_structures, mapping,
                                                 lowered_options);
  if (!output.ok())
    return Answer(output.status());
  auto& node = staged.nodes.back();
  node.parameters["input_structure"] = fused_structure;
  node.parameters["output_description_complete"] = true;
  node.parameters["expected_inputs"] = input_assertion(metadata);
  node.parameters["authoring_member"] =
      std::string(replacements ? "FMT-03B" : "FMT-03A");
  for (const auto& n : staged.nodes)
    for (const auto& p : n.parameters)
      if (const auto* value = std::get_if<std::string>(&p.second);
          value && value->size() > 8192)
        return Answer(invalid("FMT-03 parameter capacity exceeded"));
  const auto profile = options.profile == "strict" ? SequenceProfile::Strict
                       : options.profile == "accelerated_apple_silicon"
                           ? SequenceProfile::AppleSilicon
                           : SequenceProfile::X86Avx2;
  auto checked = prepare(metadata, node.parameters, 2, profile);
  if (!checked.ok())
    return Answer(checked.status());
  document = std::move(staged);
  return output;
}
}  // namespace
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
