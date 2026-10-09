#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "02-format-color/result_mapping.hpp"
#include "core/status_helpers.hpp"
#include "core/utf8_validation.hpp"
#include "data/model_coordinates.hpp"
#include "photospider/data/region_runs.hpp"
#include "photospider/ops/format/channel_editing.hpp"
#include "plugin/builtin_operations.hpp"

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
TensorInterpretation interpretation(const TensorDescription& d) {
  return {d.model,       d.primaries,  d.transfer,         d.reference,
          d.association, d.white,      d.primaries_xy,     d.profile,
          d.convention,  d.configured, d.analytic_binding, d.coordinates};
}
bool empty(const TensorInterpretation& d) {
  return d.model.empty() && d.primaries.empty() && d.transfer.empty() &&
         d.reference.empty() && d.association.empty() && !d.white &&
         !d.primaries_xy && !d.profile && !d.configured &&
         !d.analytic_binding && !d.coordinates;
}
Status overlay(TensorInterpretation* value, const TensorInterpretation& target,
               bool assertion) {
  const bool described = !empty(*value);
  if (!assertion && !value->model.empty() && !target.model.empty() &&
      target.model != value->model) {
    value->primaries.clear();
    value->transfer.clear();
    value->white.reset();
    value->primaries_xy.reset();
    value->profile.reset();
    value->configured.reset();
    value->analytic_binding.reset();
    value->coordinates.reset();
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
    return core_internal::invalid_schema_domain(
        "conflicting component interpretation");
  if (!empty(target)) {
    if (assertion && described && value->convention != target.convention)
      return core_internal::invalid_schema_domain(
          "conflicting coordinate conventions");
    value->convention = target.convention;
  }
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
      !field(&value->profile, target.profile) ||
      !field(&value->configured, target.configured) ||
      !field(&value->analytic_binding, target.analytic_binding) ||
      !data_internal::overlay_model_coordinates(&value->coordinates,
                                                target.coordinates, assertion))
    return core_internal::invalid_schema_domain(
        "conflicting component reference");
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
      return core_internal::invalid_schema_domain(
          "conflicting destination component field");
    *pair.first = *pair.second;
  }
  if (target.encoding) {
    if (assertion && value->encoding && !(*value->encoding == *target.encoding))
      return core_internal::invalid_schema_domain(
          "conflicting component encoding");
    value->encoding = target.encoding;
  }
  if (target.sampling) {
    if (assertion && value->sampling && !(*value->sampling == *target.sampling))
      return core_internal::invalid_schema_domain(
          "conflicting component sampling");
    value->sampling = target.sampling;
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
  if (!out.encoding)
    out.encoding = d->encoding;
  if (!out.sampling)
    out.sampling = d->sampling;
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
  const auto mode = std::get<std::string>(p.at("metadata_mode"));
  const auto layout = std::get<std::string>(p.at("layout"));
  if (mode != "respect" && mode != "raw" && mode != "override")
    return Answer(
        core_internal::invalid_schema_domain("invalid metadata_mode"));
  if (layout != "auto" && layout != "view" && layout != "materialize")
    return Answer(core_internal::invalid_schema_domain("invalid layout"));
  auto override_records = records(p, "input_overrides");
  if (!override_records.ok())
    return Answer(override_records.status());
  if ((mode == "override") != !override_records.value().empty())
    return Answer(core_internal::invalid_schema_domain(
        "input_overrides must occur exactly in override mode"));
  std::map<std::uint32_t, TensorDescription> overrides;
  std::uint64_t previous = 0;
  for (const auto& row : override_records.value()) {
    const auto fields = split(row, ':');
    std::uint64_t port = 0;
    if (fields.size() != 2 || !number(fields[0], &port) ||
        port >= inputs.size() || (!overrides.empty() && port <= previous))
      return Answer(core_internal::invalid_schema_domain(
          "input_overrides requires sorted unique input ordinals"));
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
      if (dimensions.size() > 7 ||
          (mode != "raw" && description && description->channel_axis))
        return Answer(core_internal::invalid_schema_domain(
            where + "component has an existing channel axis or rank eight"));
    } else {
      if (mode != "raw" && description && description->channel_axis) {
        if (axis && *axis != *description->channel_axis)
          return Answer(core_internal::invalid_schema_domain(
              where + "axis assertion conflicts with description"));
        axis = description->channel_axis;
      }
      if (!axis)
        return Answer(core_internal::invalid_schema_domain(
            where + "channel axis must be supplied or described"));
    }
    if (mode == "raw" && description && description->channel_axis != axis)
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
    return Answer(core_internal::invalid_schema_domain(
        "output channel axis out of range"));
  assembly.axis = output_axis;
  std::map<std::uint64_t, TensorChannelDescription> mapped_targets;
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
      if (fields.size() != 5 || !number(fields[0], &port) ||
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
        if (mode == "raw" || (fields[1] != "name" && fields[1] != "role"))
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
      if (fields[4] != "_") {
        auto target = tensor_description_from_parameter(fields[4]);
        if (!target.ok())
          return Answer(target.status());
        TensorDescription only;
        only.component = target.value().component;
        auto canonical = tensor_description_parameter(only);
        if (!only.component || !canonical.ok() ||
            canonical.value() != fields[4])
          return Answer(core_internal::invalid_schema_domain(
              "mapping destination must contain only a component"));
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
        validate_tensor_description(*target, output_tensor.descriptor);
    if (!status.ok())
      return Answer(status);
    if ((target->channel_axis && *target->channel_axis != assembly.axis) ||
        target->component)
      return Answer(core_internal::invalid_schema_domain(
          "output description disagrees with channel structure"));
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
          return Answer(core_internal::invalid_schema_domain(
              "input " + std::to_string(i) + ": conflicting nonchannel grid"));
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
          return Answer(core_internal::invalid_schema_domain(
              "target cannot override nonchannel coordinate grid"));
      }
    }
    result.axes = target->axes;
  }
  const bool complete = p.count("output_description_complete") &&
                        std::get<bool>(p.at("output_description_complete"));
  if (complete && !target)
    return Answer(core_internal::invalid_schema_domain(
        "complete output description is absent"));
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
              respected.coordinates.reset();
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
            if (assigned.coordinates && respected.coordinates) {
              const auto& assigned_coordinates = *assigned.coordinates;
              auto& remaining = *respected.coordinates;
              if (!assigned_coordinates.scale.empty())
                remaining.scale.clear();
              if (!assigned_coordinates.observer.empty())
                remaining.observer.clear();
              if (!assigned_coordinates.gray_kind.empty())
                remaining.gray_kind.clear();
              if (assigned_coordinates.ncl_coefficients)
                remaining.ncl_coefficients.reset();
            }
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
  output_tensor.facets = {encoded.take_value()};
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
  auto valid = schema.validate(true);
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

using Poll = Result<ResultProgramPoll>;
ResultRelation joined(const ResourceBudget& budget,
                      ResourceVector<ResultRelation> relations) {
  while (relations.size() > 1) {
    ResourceVector<ResultRelation> next{
        ResourceAllocator<ResultRelation>(budget)};
    for (std::size_t i = 0; i < relations.size(); i += 16) {
      const auto end = std::min(relations.size(), i + 16);
      next.push_back(take(ResultRelation::unite(
          budget, {relations.begin() + i, relations.begin() + end})));
    }
    relations = std::move(next);
  }
  return relations.front();
}
ResultBuilder assembly_builder(const ResultProgramPhase& phase, bool empty) {
  auto result = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_height, phase.query.tile_width, phase.query.resources));
  ResourceVector<ResultRelation> descriptors{
      ResourceAllocator<ResultRelation>(phase.resources)};
  if (empty || phase.query.inputs.empty()) {
    descriptors.push_back(take(ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})));
  } else {
    for (std::uint32_t port = 0; port < phase.query.inputs.size(); ++port)
      descriptors.push_back(take(ResultRelation::cartesian(
          phase.resources, 1,
          {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
  }
  require(result.bind_descriptor_relation(
      joined(phase.resources, std::move(descriptors))));
  return result;
}
Poll empty_result(const ResultProgramPhase& phase) try {
  auto result = assembly_builder(phase, true);
  return Poll(ResultPublication{take(result.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}
Region span_region(const ResultTensorSpec& output, const Assembly& assembly,
                   const Span& span) {
  auto dims = Region::whole(output.sample_shape()).dimensions();
  dims[output.batch_axes.size() + assembly.axis] = {span.destination,
                                                    span.count};
  return Region(std::move(dims));
}
std::vector<ResultMappedAxis> mapping(const ResultProgramQuery& query,
                                      const Assembly& a, const Span& span) {
  const auto& source = query.inputs[span.port].result_schema->tensors[0];
  const auto batches = query.output.result_schema->tensors[0].batch_axes.size();
  const auto source_axis = a.axes[span.port];
  std::vector<ResultMappedAxis> axes(source.sample_shape().size());
  for (std::size_t i = 0; i < axes.size(); ++i) {
    if (a.scalars[span.port]) {
      axes[i].output_axis = -1;
    } else if (i < batches) {
      axes[i].output_axis = i;
    } else if (source_axis && i - batches == *source_axis) {
      axes[i].output_axis = batches + a.axis;
      axes[i].source_origin = span.source;
      axes[i].output_origin = span.destination;
    } else {
      const auto j =
          i - batches - (source_axis && i - batches > *source_axis ? 1 : 0);
      axes[i].output_axis = batches + (j < a.axis ? j : j + 1);
    }
  }
  return axes;
}
struct Projected final {
  const void* owner = nullptr;
  std::uint64_t offset = 0;
  std::array<RegionDimension, 8> dims{};
  std::array<std::int64_t, 8> strides{};
};
Status unavailable_view() {
  return core_internal::invalid_schema_domain(
      "ViewUnavailable: no common affine owner mapping");
}
// Retain the pre-Result whole-query view predicate, including cross-box owners.
Status generic_view(const ResultProgramPhase& phase, const Assembly& a,
                    const Footprint& outputs,
                    const ResourceVector<ResultRelation>& relations) {
  ResourceVector<Projected> views{
      ResourceAllocator<Projected>(phase.resources)};
  const auto& output = phase.query.output.result_schema->tensors[0];
  const auto rank = output.sample_shape().size();
  const auto channel_axis = output.batch_axes.size() + a.axis;
  for (std::size_t s = 0; s < a.spans.size(); ++s) {
    const auto& span = a.spans[s];
    auto clipped = take(outputs.intersect(take(Footprint::from_regions(
        output.sample_shape(), {span_region(output, a, span)}))));
    const auto axes = mapping(phase.query, a, span);
    std::function<Status(const Region&, const ResultTensorReadWindow&)> inspect;
    inspect = [&](const Region& box, const ResultTensorReadWindow& window) {
      require(phase.consume_work(rank + axes.size()));
      auto affine = execution_internal::ResultWindowAccess::affine(window);
      if (!affine.ok()) {
        if (affine.status().code != ErrorCode::NotFound)
          return affine.status();
        auto partitioned =
            execution_internal::ResultWindowAccess::visit_backing_regions(
                window, phase.resources, [&](const Region& region) {
                  auto pieces = take(relations[s].preimage(
                      take(Footprint::from_regions(output.sample_shape(),
                                                   {box})),
                      {span.port, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                      take(Footprint::from_regions(window.spec().sample_shape(),
                                                   {region}))));
                  for (const auto& part : pieces.boxes()) {
                    auto source = take(
                        phase.tensors->at({span.port, 0})
                            .acquire(format_result::mapped_region(part, axes),
                                     phase.query.cancellation));
                    auto status = inspect(part, source);
                    if (!status.ok())
                      return status;
                  }
                  return Status::success();
                });
        if (!partitioned.ok())
          return partitioned.status();
        return partitioned.value() ? Status::success() : unavailable_view();
      }
      Projected view;
      view.owner = affine.value().storage().get();
      std::copy(box.dimensions().begin(), box.dimensions().end(),
                view.dims.begin());
      std::vector<std::uint64_t> at;
      for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto& axis = axes[i];
        at.push_back(take(axis.source_coordinate(
            axis.output_axis < 0 ? 0
                                 : box.dimensions()[axis.output_axis].offset)));
        if (axis.output_axis >= 0)
          view.strides[axis.output_axis] =
              affine.value().layout().byte_strides[i];
      }
      view.offset = take(affine.value().byte_address(at));
      views.push_back(view);
      return Status::success();
    };
    for (const auto& box : clipped.boxes()) {
      auto window = take(phase.tensors->at({span.port, 0})
                             .acquire(format_result::mapped_region(box, axes),
                                      phase.query.cancellation));
      auto status = inspect(box, window);
      if (!status.ok())
        return status;
    }
  }
  if (views.empty())
    return unavailable_view();
  const auto& first = views.front();
  auto strides = first.strides;
  bool channel_stride = first.dims[channel_axis].extent > 1;
  for (const auto& view : views) {
    require(phase.consume_work(rank + 1));
    if (view.owner != first.owner)
      return unavailable_view();
    for (std::size_t d = 0; d < rank; ++d)
      if (d != channel_axis && strides[d] != view.strides[d])
        return unavailable_view();
    if (!channel_stride &&
        view.dims[channel_axis].offset != first.dims[channel_axis].offset) {
      __int128 delta = static_cast<__int128>(view.offset) - first.offset;
      for (std::size_t d = 0; d < rank; ++d)
        if (d != channel_axis)
          delta -= (static_cast<__int128>(view.dims[d].offset) -
                    first.dims[d].offset) *
                   strides[d];
      const auto dc = static_cast<__int128>(view.dims[channel_axis].offset) -
                      first.dims[channel_axis].offset;
      if (delta % dc || delta / dc < INT64_MIN || delta / dc > INT64_MAX)
        return unavailable_view();
      strides[channel_axis] = delta / dc;
      channel_stride = true;
    }
  }
  for (const auto& view : views) {
    require(phase.consume_work(rank + 1));
    __int128 expected = first.offset;
    for (std::size_t d = 0; d < rank; ++d)
      expected +=
          (static_cast<__int128>(view.dims[d].offset) - first.dims[d].offset) *
          strides[d];
    if (expected != view.offset ||
        (view.dims[channel_axis].extent > 1 &&
         strides[channel_axis] != view.strides[channel_axis]))
      return unavailable_view();
  }
  return Status::success();
}
struct State final {
  const Assembly* assembly;
  std::uint32_t next_input = 0;
  std::optional<ResultTensorInputs> ready;
  std::optional<ResourceVector<ResultRelation>> relations;
  Footprint output;
  explicit State(const Assembly* value) : assembly(value) {}
  Poll poll(const ResultProgramPhase& phase) try {
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(16384, 16384)));
    const auto& spec = phase.query.output.result_schema->tensors[0];
    if (!ready) {
      ready.emplace(
          std::less<std::pair<std::uint32_t, std::uint32_t>>{},
          ResourceAllocator<ResultTensorInputs::value_type>(phase.resources));
      relations.emplace(ResourceAllocator<ResultRelation>(phase.resources));
      output = phase.query.tensor_outputs
                   ? *phase.query.tensor_outputs
                   : take(Footprint::all(spec.sample_shape()));
      for (const auto& span : assembly->spans)
        relations->push_back(take(ResultRelation::mapped(
            phase.resources, spec.sample_shape(),
            span_region(spec, *assembly, span),
            phase.query.inputs[span.port]
                .result_schema->tensors[0]
                .sample_shape(),
            mapping(phase.query, *assembly, span),
            {span.port, 1, 0, 0, ResultSupportTarget::Tensor, 0})));
    }
    if (phase.tensors)
      for (const auto& input : *phase.tensors)
        ready->insert_or_assign(input.first, input.second);
    if (next_input < phase.query.inputs.size()) {
      ResultProgramNeed need;
      while (next_input < phase.query.inputs.size() &&
             need.tensors.size() < 64) {
        const auto port = next_input++;
        auto samples = take(Footprint::none(
            phase.query.inputs[port].result_schema->tensors[0].sample_shape()));
        for (auto s : assembly->by_port[port])
          require((*relations)[s].project(
              output, [&](auto, const Footprint* selected) {
                if (selected)
                  samples = take(samples.unite(*selected));
                return Status::success();
              }));
        const auto roles = samples.empty() ? 8U : 9U;
        need.tensors.push_back({port, 0, std::move(samples), roles});
      }
      return Poll(std::move(need));
    }
    auto supplied = phase;
    supplied.tensors = &*ready;
    auto result = assembly_builder(phase, false);
    std::string policy = assembly->layout;
    if (!spec.layout.spatial && policy != "materialize") {
      auto available = generic_view(supplied, *assembly, output, *relations);
      if (!available.ok()) {
        if (policy == "view" || !format_result::view_unavailable(available))
          return Poll(available);
        policy = "materialize";
      }
    }
    auto relation = joined(phase.resources, *relations);
    require(format_result::planes(spec, output, [&](const Region& box) {
      if (spec.layout.spatial && policy != "materialize") {
        ResourceVector<ResultTensorReadWindow> windows{
            ResourceAllocator<ResultTensorReadWindow>(phase.resources)};
        std::vector<const ResultTensorReadWindow*> sources;
        for (const auto& span : assembly->spans) {
          auto clipped =
              take(Footprint::from_regions(spec.sample_shape(), {box}));
          clipped = take(clipped.intersect(take(Footprint::from_regions(
              spec.sample_shape(), {span_region(spec, *assembly, span)}))));
          for (const auto& part : clipped.boxes())
            windows.push_back(take(
                ready->at({span.port, 0})
                    .acquire(format_result::mapped_region(
                                 part, mapping(phase.query, *assembly, span)),
                             phase.query.cancellation)));
        }
        for (const auto& window : windows)
          sources.push_back(&window);
        auto status = result.publish_tensor_view(0, box, sources, relation,
                                                 {true, true, true, true},
                                                 phase.query.cancellation);
        if (status.ok())
          return status;
        if (!format_result::view_unavailable(status))
          return status;
        if (policy == "view")
          return unavailable_view();
      }
      for (std::size_t s = 0; s < assembly->spans.size(); ++s) {
        const auto& span = assembly->spans[s];
        auto clipped =
            take(Footprint::from_regions(spec.sample_shape(), {box}));
        clipped = take(clipped.intersect(take(Footprint::from_regions(
            spec.sample_shape(), {span_region(spec, *assembly, span)}))));
        const auto axes = mapping(phase.query, *assembly, span);
        for (const auto& part : clipped.boxes()) {
          auto window =
              take(ready->at({span.port, 0})
                       .acquire(format_result::mapped_region(part, axes),
                                phase.query.cancellation));
          auto status = format_result::publish(
              supplied, &result, part, window, axes, relation,
              spec.layout.spatial ? "materialize" : policy, span.port);
          if (!status.ok())
            return status;
        }
      }
      return Status::success();
    }));
    return Poll(ResultPublication{take(result.seal()), true});
  } catch (const Status& status) {
    return Poll(status);
  }
};
struct ScalarState final {
  std::array<std::uint8_t, 8> bits{};
  std::size_t width = 0;
  Poll poll(const ResultProgramPhase& phase) try {
    auto result = assembly_builder(phase, true);
    auto relation = take(ResultRelation::cartesian(
        phase.resources, 1, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    require(phase.consume_work(width));
    require(result.publish_tensor_kernel(
        0, Region::whole({1}),
        [&](const auto& writers) {
          auto row = writers[0].row_run({0});
          if (!row.ok())
            return row.status();
          std::memcpy(row.value().data, bits.data(), width);
          return Status::success();
        },
        relation, {true, true, true, true}, phase.query.cancellation));
    return Poll(ResultPublication{take(result.seal()), true});
  } catch (const Status& status) {
    return Poll(status);
  }
};

OperationDefinition definition(const std::string& key, int member,
                               SequenceProfile profile) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.element_type_mask = 127;
  t.input_schema = {port};
  t.repeated_minimum = 1;
  t.repeated_maximum = 1024;
  t.repeated_match = false;
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
  out.output_schema = port;
  out.result_schema = tensor_ops::scalar_schema();
  out.region_rule = OperationRegionRule::Dependency;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 17;
  op.prepare_static = [member, profile](const auto& inputs,
                                        const auto& params) {
    return prepare(inputs, params, member, profile);
  };
  op.start_result = [](const ResultProgramQuery& query,
                       const BufferAllocator& allocator) {
    if (query.tensor_outputs && query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_result>();
    return ResultContinuation::make<State>(
        allocator, static_cast<const Assembly*>(query.prepared->state()));
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
  const auto width = Value::element_size(type);
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
  t.cacheable = false;
  t.parameter_schema = {{"dtype", OperationParameterType::Int64},
                        {"bits", OperationParameterType::String}};
  auto& out = t.outputs[0];
  out.key = "values";
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.element_type_mask = 127;
  out.result_schema = tensor_ops::scalar_schema();
  out.region_rule = OperationRegionRule::Whole;
  out.continuation_bytes = sizeof(ScalarState);
  out.maximum_dependency_stages = 1;
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
  op.start_result = [](const ResultProgramQuery& query,
                       const BufferAllocator& allocator) {
    if (query.tensor_outputs && query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_result>();
    return ResultContinuation::make<ScalarState>(
        allocator, *static_cast<const ScalarState*>(query.prepared->state()));
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
    for (const auto& f : tensor(metadata).facets)
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
    auto valid =
        validate_tensor_description(*result, tensor(metadata).descriptor);
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
      inputs[0].structure.scalar ||
      (options.metadata_mode != "respect" && options.metadata_mode != "raw" &&
       options.metadata_mode != "override") ||
      (options.metadata_mode == "override") != !options.input_overrides.empty())
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 invalid base/metadata mode/arity"));
  if (!replacements) {
    for (std::size_t i = 1; i < inputs.size(); ++i)
      if (!inputs[i].structure.scalar)
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03A external inputs must be explicit scalars"));
  }
  for (const auto& entry : options.input_overrides)
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
  if (options.metadata_mode != "raw" && desc && desc->channel_axis) {
    if (axis && axis != desc->channel_axis)
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 base axis disagrees with metadata"));
    axis = desc->channel_axis;
  }
  if (!axis || *axis >= base.descriptor.shape.size())
    return Answer(core_internal::invalid_schema_domain(
        "FMT-03 base axis required/in range"));
  if (options.metadata_mode == "raw" && desc && desc->channel_axis != axis)
    desc.reset();
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
      auto destination = edit_select(r.destination, count, desc, true,
                                     options.metadata_mode == "raw");
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
    const auto width = Value::element_size(literal.dtype);
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
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03A source must be base or scalar"));
    auto d = edit_description(metadata[e.input], e.input, options);
    if (!d.ok())
      return Answer(d.status());
    const auto& structure = structures[e.input];
    auto a = structure.axis;
    if (options.metadata_mode != "raw" && d.value() &&
        d.value()->channel_axis && !structure.scalar && !structure.component)
      a = d.value()->channel_axis;
    const auto& shape = tensor(metadata[e.input]).descriptor.shape;
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
      return Answer(core_internal::invalid_schema_domain(
          "FMT-03 output channel structure mismatch"));
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
        return Answer(core_internal::invalid_schema_domain(
            "FMT-03B target changes an unlisted component"));
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
