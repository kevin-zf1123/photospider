#include <utility>
#pragma once

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "02-format-color/result_mapping.hpp"
#include "photospider/ops/format/alpha.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal::alpha_ops {
using Params = std::map<std::string, ParameterValue>;
inline Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          "alpha: " + message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status mismatch(const std::string& message) {
  return {ErrorCode::TypeMismatch,
          "alpha: " + message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline std::string text(const Params& p, const char* key,
                        const std::string& fallback = {}) {
  auto found = p.find(key);
  return found == p.end() ? fallback : std::get<std::string>(found->second);
}
inline bool number(const std::string& s, std::uint64_t* value) {
  if (s.empty() || (s.size() > 1 && s[0] == '0')) {
    return false;
  }
  const auto result = std::from_chars(s.data(), s.data() + s.size(), *value);
  return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}
inline const ResultTensorSpec& tensor(const OperationMetadata& m) {
  tensor_ops::require(tensor_ops::check_tensor(m));
  return m.result_schema->tensors[0];
}
inline Status shape_valid(const OperationMetadata& m) {
  auto valid = tensor_ops::check_tensor(m);
  if (!valid.ok())
    return valid;
  const auto& spec = m.result_schema->tensors[0];
  auto count = spec.sample_count();
  if (!count.ok())
    return count.status();
  if (!count.value() || count.value() > (UINT64_C(1) << 40))
    return invalid("logical count is outside [1,2^40]");
  return Status::success();
}
inline Result<std::optional<TensorDescription>> effective_description(
    const OperationMetadata& m, const Params& p, bool allow_raw) {
  using Answer = Result<std::optional<TensorDescription>>;
  const auto mode = text(p, "metadata_mode", "respect");
  if (mode != "respect" && mode != "override" &&
      !(allow_raw && mode == "raw")) {
    return Answer(invalid("unsupported metadata_mode"));
  }
  if ((mode == "override") != (p.count("metadata_override") != 0)) {
    return Answer(
        invalid("metadata_override must occur exactly with override"));
  }
  std::optional<TensorDescription> d;
  if (mode == "override") {
    auto parsed =
        tensor_description_from_parameter(text(p, "metadata_override"));
    if (!parsed.ok()) {
      return Answer(parsed.status());
    }
    d = parsed.take_value();
  } else {
    for (const auto& facet : tensor(m).facets) {
      if (facet.key != "photospider.tensor-description") {
        continue;
      }
      auto parsed = decode_tensor_description(facet);
      if (!parsed.ok()) {
        return Answer(parsed.status());
      }
      d = parsed.take_value();
    }
  }
  if (d) {
    auto valid = validate_tensor_description(*d, tensor(m).descriptor);
    if (!valid.ok()) {
      return Answer(valid);
    }
  }
  if (!d && mode != "raw") {
    return Answer(mismatch("tensor description is required"));
  }
  return Answer(std::move(d));
}
inline Result<std::uint64_t> select(const format::ChannelSelector& s,
                                    std::uint64_t count,
                                    const std::optional<TensorDescription>& d) {
  using Answer = Result<std::uint64_t>;
  std::uint64_t index = 0;
  if (s.match == "index") {
    if (!number(s.value, &index) || index >= count) {
      return Answer(invalid("index selector is outside channel count"));
    }
    return Answer(index);
  }
  if ((s.match != "name" && s.match != "role") || s.value.empty() || !d) {
    return Answer(invalid("invalid name/role selector"));
  }
  if (!d->channels.empty() && d->channels.size() != count) {
    return Answer(invalid("named alpha selector axis differs from metadata"));
  }
  bool found = false;
  for (std::uint64_t i = 0; i < count; ++i) {
    std::string field;
    if (!d->channels.empty()) {
      const auto& c = d->channels[i];
      field = s.match == "name" ? c.name : c.role;
    } else if (!d->channel_axis && d->component) {
      field = s.match == "name" ? d->component->name : d->component->role;
    }
    for (const auto& g : d->groups) {
      for (std::size_t j = 0; j < g.indices.size(); ++j) {
        if (g.indices[j] == i && field.empty()) {
          field =
              s.match == "name" ? g.components[j].name : g.components[j].role;
        }
      }
    }
    if (field == s.value) {
      if (found) {
        return Answer(invalid("ambiguous selector: " + s.value));
      }
      found = true;
      index = i;
    }
  }
  return found ? Answer(index)
               : Answer(invalid("selector is absent: " + s.value));
}
inline TensorInterpretation global_interpretation(const TensorDescription& d) {
  return {d.model,       d.primaries,  d.transfer,         d.reference,
          d.association, d.white,      d.primaries_xy,     d.profile,
          d.convention,  d.configured, d.analytic_binding, d.coordinates};
}
struct SemanticSource final {
  TensorDescription description;
  TensorColorGroup group;
  std::optional<std::uint32_t> axis;
  std::size_t group_index = 0;
  bool component = false;
};
inline Result<SemanticSource> semantic_source(const OperationMetadata& m,
                                              const Params& p,
                                              bool allow_boundary,
                                              bool allow_component) {
  using Answer = Result<SemanticSource>;
  auto status = shape_valid(m);
  if (!status.ok()) {
    return Answer(status);
  }
  auto desc = effective_description(m, p, false);
  if (!desc.ok()) {
    return Answer(desc.status());
  }
  SemanticSource result;
  result.description = std::move(*desc.value());
  result.axis = result.description.channel_axis;
  if (p.count("axis")) {
    const auto axis = std::get<std::int64_t>(p.at("axis"));
    if (axis < 0 || !result.axis ||
        static_cast<std::uint64_t>(axis) != *result.axis) {
      return Answer(invalid("axis assertion disagrees with description"));
    }
  }
  if (!allow_boundary) {
    if (result.description.association == "premultiplied") {
      return Answer(
          mismatch("premultiplied boundary requires unassociate first"));
    }
    for (const auto& g : result.description.groups) {
      if (!g.interpretation.association.empty() &&
          g.interpretation.association != "straight") {
        return Answer(mismatch("all complete groups must be straight"));
      }
    }
  }
  const auto name = text(p, "group");
  if (name.empty()) {
    return Answer(invalid("explicit group is required"));
  }
  if (!result.axis) {
    const auto& d = result.description;
    auto interpretation = d.component && d.component->interpretation
                              ? *d.component->interpretation
                              : global_interpretation(d);
    if (!allow_component) {
      return Answer(
          mismatch("association requires a channel-bearing RGB/Gray group with "
                   "internal alpha"));
    }
    if (!d.component || d.component->name != name ||
        interpretation.model != "gray" || d.component->role != "gray" ||
        (!interpretation.association.empty() &&
         interpretation.association != "straight")) {
      return Answer(
          invalid("component input requires an explicitly named straight Gray "
                  "component"));
    }
    result.component = true;
    auto c = *d.component;
    c.interpretation.reset();
    if (!c.encoding) {
      c.encoding = d.encoding;
    }
    if (!c.sampling) {
      c.sampling = d.sampling;
    }
    result.group = {name, {0}, {c}, interpretation, {}};
    return Answer(std::move(result));
  }
  bool found = false;
  for (std::size_t i = 0; i < result.description.groups.size(); ++i) {
    if (result.description.groups[i].name != name) {
      continue;
    }
    if (found) {
      return Answer(invalid("ambiguous group"));
    }
    found = true;
    result.group_index = i;
    result.group = result.description.groups[i];
  }
  if (!found) {
    return Answer(invalid("group is absent: " + name));
  }
  return Answer(std::move(result));
}
inline bool referenced_elsewhere(const SemanticSource& s, std::uint64_t index) {
  for (std::size_t i = 0; i < s.description.groups.size(); ++i) {
    if (i == s.group_index) {
      continue;
    }
    const auto& g = s.description.groups[i];
    if (g.alpha == index || std::find(g.indices.begin(), g.indices.end(),
                                      index) != g.indices.end()) {
      return true;
    }
  }
  return false;
}
/** Complete inverse map: -1 denotes the newly supplied alpha. */
inline Result<TensorDescription> remap_description(
    const SemanticSource& s, const std::vector<std::int64_t>& map,
    std::uint32_t axis, std::optional<std::uint64_t> new_alpha) {
  using Answer = Result<TensorDescription>;
  auto result = s.description;
  std::map<std::uint64_t, std::uint64_t> inverse;
  for (std::size_t i = 0; i < map.size(); ++i) {
    if (map[i] >= 0) {
      inverse[static_cast<std::uint64_t>(map[i])] = i;
    }
  }
  auto original_channels = result.channels;
  if (s.component) {
    original_channels = {*result.component};
    original_channels[0].interpretation.reset();
    result.component.reset();
    result.groups = {s.group};
    result.channel_axis = axis;
    if (!result.axes.empty()) {
      result.axes.insert(result.axes.begin() + axis, TensorAxisDescription{});
    }
  }
  TensorChannelDescription alpha;
  const bool replaced_alpha =
      s.group.alpha &&
      std::find(map.begin(), map.end(),
                static_cast<std::int64_t>(*s.group.alpha)) == map.end();
  if (replaced_alpha && *s.group.alpha < original_channels.size()) {
    alpha = original_channels[*s.group.alpha];
  }
  alpha.role = "alpha";
  alpha.unit = "coverage";
  alpha.interpretation.reset();
  // Preserve the represented interval, not just the affine decode formula.
  // An old alpha with no local encoding still inherits the tensor encoding.
  // A genuinely new alpha must instead override any color-specific default.
  if (!replaced_alpha) {
    alpha.encoding = TensorEncoding{};
  }
  result.channels.clear();
  result.channels.reserve(map.size());
  for (auto source : map) {
    if (source < 0) {
      result.channels.push_back(alpha);
    } else if (static_cast<std::uint64_t>(source) < original_channels.size()) {
      result.channels.push_back(original_channels[source]);
    } else {
      result.channels.emplace_back();
    }
  }
  const auto selected = s.component ? 0 : s.group_index;
  for (std::size_t i = 0; i < result.groups.size(); ++i) {
    auto& g = result.groups[i];
    for (auto& channel : g.indices) {
      if (!inverse.count(channel)) {
        return Answer(invalid("removed channel still has a color reference"));
      }
      channel = inverse.at(channel);
    }
    if (i == selected) {
      g.alpha = new_alpha;
    } else if (g.alpha) {
      if (!inverse.count(*g.alpha)) {
        return Answer(invalid("removed channel still has an alpha reference"));
      }
      g.alpha = inverse.at(*g.alpha);
    }
  }
  return Answer(std::move(result));
}
using format_result::source_assertion;
inline Status declaration_matches(const WorkflowDocument& document,
                                  const WorkflowInput& input,
                                  const OperationMetadata& metadata) {
  const auto* ref = std::get_if<WorkflowInputReference>(&input);
  if (!ref) {
    return Status::success();
  }
  for (const auto& d : document.inputs) {
    if (d.id != ref->input_id) {
      continue;
    }
    OperationMetadata actual;
    actual.result_schema = d.result_schema;
    return source_assertion({actual}) == source_assertion({metadata})
               ? Status::success()
               : invalid("authoring metadata disagrees with input declaration");
  }
  return invalid("input declaration is absent");
}
inline Params edit_params(const format::AlphaEditOptions& o) {
  Params p{{"metadata_mode", o.metadata_mode},
           {"group", o.group},
           {"layout", o.layout}};
  if (o.axis) {
    p["axis"] = static_cast<std::int64_t>(*o.axis);
  }
  return p;
}
inline Status add_override(Params* p, const std::string& mode,
                           const std::optional<TensorDescription>& d) {
  if ((mode == "override") != d.has_value()) {
    return invalid("metadata override/mode mismatch");
  }
  if (!d) {
    return Status::success();
  }
  auto encoded = tensor_description_parameter(*d);
  if (!encoded.ok()) {
    return encoded.status();
  }
  (*p)["metadata_override"] = encoded.take_value();
  return Status::success();
}
inline Status profile_valid(const std::string& profile) {
  if (profile != "strict" && profile != "accelerated_apple_silicon" &&
      profile != "accelerated_x86_64") {
    return invalid("unknown CPU profile");
  }
  return Status::success();
}
inline numeric_ops::SequenceProfile profile_kind(const std::string& p) {
  return p == "strict" ? numeric_ops::SequenceProfile::Strict
         : p == "accelerated_apple_silicon"
             ? numeric_ops::SequenceProfile::AppleSilicon
             : numeric_ops::SequenceProfile::X86Avx2;
}
inline std::vector<ValueFacet> output_facets(const OperationMetadata& input,
                                             const TensorDescription& d) {
  auto facets = tensor(input).facets;
  facets.erase(std::remove_if(facets.begin(), facets.end(),
                              [](const auto& f) {
                                return f.key ==
                                           "photospider.tensor-description" ||
                                       f.key == "photospider.semantic" ||
                                       f.key == "photospider.image" ||
                                       f.key == "photospider.color-array";
                              }),
               facets.end());
  auto encoded = encode_tensor_description(d);
  if (encoded.ok()) {
    facets.push_back(encoded.take_value());
  }
  return facets;
}
}  // namespace ps::plugin_internal::alpha_ops
