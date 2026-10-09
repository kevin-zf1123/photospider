#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/alpha_lowering.hpp"
#include "02-format-color/alpha_numeric_metadata.hpp"
#include "photospider/ops/format/metadata.hpp"

namespace ps::plugin_internal::alpha_ops {
namespace {
using Answer = Result<WorkflowNodeOutput>;
Status source_parameters(Params* p, const format::AlphaSource& source,
                         const std::optional<WorkflowInput>& alpha) {
  if (source.kind != "internal" && source.kind != "external_plane" &&
      source.kind != "scalar") {
    return invalid("unknown alpha source");
  }
  if ((source.kind != "internal") != alpha.has_value()) {
    return invalid("external/scalar alpha requires exactly one extra edge");
  }
  (*p)["alpha_source"] = source.kind;
  if (source.kind == "internal") {
    if (source.channel.match != "index" && source.channel.match != "name" &&
        source.channel.match != "role") {
      return invalid("unknown internal alpha selector");
    }
    (*p)["alpha_match"] = source.channel.match;
    (*p)["alpha_selector"] = source.channel.value;
  }
  return Status::success();
}
Answer append(WorkflowDocument& document, WorkflowInput input,
              const std::optional<WorkflowInput>& alpha, const std::string& key,
              Params parameters) {
  std::vector<WorkflowInput> inputs{std::move(input)};
  if (alpha) {
    inputs.push_back(*alpha);
  }
  auto ids = numeric::available_workflow_node_ids(document, 1, inputs);
  if (!ids.ok()) {
    return Answer(ids.status());
  }
  // Copy the document first: neither vector growth nor node construction may
  // leave a partially-expanded graph visible to the caller.
  WorkflowDocument staged = document;
  staged.nodes.push_back(
      {ids.value()[0], key, std::move(inputs), std::move(parameters)});
  const WorkflowNodeOutput output{ids.value()[0], "values"};
  document = std::move(staged);
  return Answer(output);
}
Answer associate(WorkflowDocument& document, WorkflowInput input,
                 const format::AlphaAssociationOptions& options,
                 const std::optional<WorkflowInput>& alpha, bool inverse) {
  auto status = profile_valid(options.profile);
  if (!status.ok()) {
    return Answer(status);
  }
  if (options.metadata_mode != "respect" &&
      options.metadata_mode != "override" && options.metadata_mode != "raw") {
    return Answer(invalid("unknown metadata mode"));
  }
  if (options.algorithm != "auto" && options.algorithm != "scalar" &&
      options.algorithm != "simd" && options.algorithm != "reference") {
    return Answer(invalid("unknown arithmetic algorithm"));
  }
  Params p{{"metadata_mode", options.metadata_mode},
           {"algorithm", options.algorithm}};
  status = add_override(&p, options.metadata_mode, options.metadata_override);
  if (!status.ok()) {
    return Answer(status);
  }
  if (!options.group.empty()) {
    p["group"] = options.group;
  }
  if (options.axis) {
    p["axis"] = static_cast<std::int64_t>(*options.axis);
  }
  if (!options.input_structure.empty()) {
    p["input_structure"] = options.input_structure;
  }
  if (!options.components.empty()) {
    std::string list;
    for (auto c : options.components) {
      if (!list.empty()) {
        list += ',';
      }
      list += std::to_string(c);
    }
    p["components"] = std::move(list);
  }
  if (options.alpha_source) {
    status = source_parameters(&p, *options.alpha_source, alpha);
    if (!status.ok()) {
      return Answer(status);
    }
  } else if (alpha || options.metadata_mode == "raw") {
    return Answer(
        invalid("raw/external use requires an explicit alpha source"));
  }
  return append(
      document, std::move(input), alpha,
      std::string(inverse ? "alpha.unassociate_" : "alpha.associate_") +
          options.profile,
      std::move(p));
}
Result<SemanticSource> helper_source(const WorkflowDocument& doc,
                                     const WorkflowInput& input,
                                     const OperationMetadata& metadata,
                                     const format::AlphaEditOptions& options) {
  using R = Result<SemanticSource>;
  auto status = profile_valid(options.profile);
  if (!status.ok()) {
    return R(status);
  }
  if (options.layout != "auto" && options.layout != "view" &&
      options.layout != "materialize") {
    return R(invalid("unknown layout"));
  }
  status = shape_valid(metadata);
  if (!status.ok()) {
    return R(status);
  }
  status = declaration_matches(doc, input, metadata);
  if (!status.ok()) {
    return R(status);
  }
  auto p = edit_params(options);
  status = add_override(&p, options.metadata_mode, options.metadata_override);
  if (!status.ok()) {
    return R(status);
  }
  return semantic_source(metadata, p, false, true);
}
TensorEncoding default_encoding(ElementType type) {
  TensorEncoding result;
  std::int64_t lo = 0, hi = 1;
  switch (type) {
    case ElementType::UInt8:
      hi = 255;
      break;
    case ElementType::UInt16:
      hi = 65535;
      break;
    case ElementType::Int8:
      lo = INT8_MIN;
      hi = INT8_MAX;
      break;
    case ElementType::Int16:
      lo = INT16_MIN;
      hi = INT16_MAX;
      break;
    case ElementType::Int64:
      lo = INT64_MIN;
      hi = INT64_MAX;
      break;
    default:
      break;
  }
  result.stored = {lo, hi};
  return result;
}
TensorDescription alpha_description(
    const SemanticSource& source, ElementType type, bool keepdims,
    const std::optional<TensorEncoding>& opaque_encoding) {
  TensorDescription d;
  d.axes = source.description.axes;
  d.sampling = source.description.sampling;
  TensorChannelDescription channel;
  if (source.group.alpha &&
      *source.group.alpha < source.description.channels.size()) {
    channel = source.description.channels[*source.group.alpha];
  }
  if (channel.name.empty()) {
    channel.name = "alpha";
  }
  channel.role = "alpha";
  if (channel.unit.empty()) {
    channel.unit = "coverage";
  }
  channel.interpretation.reset();
  if (!source.group.alpha) {
    channel.encoding = opaque_encoding.value_or(default_encoding(type));
  } else if (!channel.encoding) {
    channel.encoding =
        source.description.encoding.value_or(default_encoding(type));
  }
  if (source.axis && keepdims) {
    d.channel_axis = source.axis;
    d.channels = {channel};
  } else {
    d.component = channel;
    if (source.axis && !d.axes.empty()) {
      d.axes.erase(d.axes.begin() + *source.axis);
    }
  }
  return d;
}
using data_internal::format_numeric::Natural;
using data_internal::format_numeric::Rational;
Result<std::string> opaque_bits(ElementType type,
                                const TensorEncoding& encoding) {
  using R = Result<std::string>;
  std::uint64_t fuel = 20000000;
  const std::function<Status(std::uint64_t)> consume =
      [&fuel](std::uint64_t n) {
        if (n > fuel) {
          return Status{ErrorCode::ResourceExhausted,
                        "opaque inverse-encoding work limit"};
        }
        fuel -= n;
        if (const auto* budget = resource_internal::metadata_budget()) {
          return budget->consume({n});
        }
        return Status::success();
      };
  data_internal::format_numeric::ExactWorkScope work(&consume, nullptr);
  try {
    const auto s0 = exact_endpoint(encoding.stored[0]),
               s1 = exact_endpoint(encoding.stored[1]);
    const auto d0 = exact_endpoint(encoding.decoded[0]),
               d1 = exact_endpoint(encoding.decoded[1]);
    const auto code = Rational::add(
        s0, Rational::divide(
                Rational::multiply(Rational::subtract(Rational::integer(1), d0),
                                   Rational::subtract(s1, s0)),
                Rational::subtract(d1, d0)));
    if (code.compare(s0) < 0 || code.compare(s1) > 0) {
      return R(invalid(
          "opaque coverage 1 is outside the legal stored encoding interval"));
    }
    std::uint64_t bits = 0;
    if (type == ElementType::Float32 || type == ElementType::Float64) {
      const bool narrow = type == ElementType::Float32;
      bits = code.floating_bits(narrow);
      const auto inf =
          narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
      if ((bits & inf) == inf ||
          Rational::binary(bits, narrow).compare(code) != 0) {
        return R(
            invalid("opaque code is not exactly representable in source "
                    "floating dtype"));
      }
    } else {
      const auto limits = default_encoding(type);
      if (code.compare(exact_endpoint(limits.stored[0])) < 0 ||
          code.compare(exact_endpoint(limits.stored[1])) > 0) {
        return R(invalid("opaque code is outside source integer dtype"));
      }
      auto divided = Natural::divide(code.n, code.d);
      if (!divided.second.zero()) {
        return R(invalid("opaque code is not an integer"));
      }
      bits = divided.first.low64();
      if (code.negative) {
        bits = UINT64_C(0) - bits;
      }
    }
    // The literal codec describes native stored bytes, like scalar_literal.
    return R(format::detail::assembly_hex(std::string(
        reinterpret_cast<const char*>(&bits), Value::element_size(type))));
  } catch (const data_internal::format_numeric::ExactWorkFailure& e) {
    return R(e.status);
  } catch (const std::overflow_error&) {
    return R(invalid("opaque encoding exceeds exact arithmetic capacity"));
  }
}
}  // namespace
}  // namespace ps::plugin_internal::alpha_ops

namespace ps::format {
using plugin_internal::alpha_ops::add_override;
using plugin_internal::alpha_ops::alpha_description;
using plugin_internal::alpha_ops::Answer;
using plugin_internal::alpha_ops::append;
using plugin_internal::alpha_ops::associate;
using plugin_internal::alpha_ops::edit_params;
using plugin_internal::alpha_ops::helper_source;
using plugin_internal::alpha_ops::invalid;
using plugin_internal::alpha_ops::opaque_bits;
using plugin_internal::alpha_ops::Params;
using plugin_internal::alpha_ops::profile_kind;
using plugin_internal::alpha_ops::profile_valid;
using plugin_internal::alpha_ops::referenced_elsewhere;
using plugin_internal::alpha_ops::remap_description;
using plugin_internal::alpha_ops::source_assertion;
using plugin_internal::alpha_ops::source_parameters;
using plugin_internal::alpha_ops::tensor;
Result<WorkflowNodeOutput> associate_alpha(WorkflowDocument& d,
                                           WorkflowInput input,
                                           const AlphaAssociationOptions& o,
                                           std::optional<WorkflowInput> a) try {
  return associate(d, std::move(input), o, a, false);
} catch (const Status& status) {
  return Answer(status);
} catch (const std::bad_alloc&) {
  return Result<WorkflowNodeOutput>(Status{
      ErrorCode::ResourceExhausted, "alpha authoring allocation failed"});
}
Result<WorkflowNodeOutput> unassociate_alpha(
    WorkflowDocument& d, WorkflowInput input, const AlphaAssociationOptions& o,
    std::optional<WorkflowInput> a) try {
  return associate(d, std::move(input), o, a, true);
} catch (const Status& status) {
  return Answer(status);
} catch (const std::bad_alloc&) {
  return Result<WorkflowNodeOutput>(Status{
      ErrorCode::ResourceExhausted, "alpha authoring allocation failed"});
}
Result<WorkflowNodeOutput> set_alpha(WorkflowDocument& d, WorkflowInput input,
                                     const SetAlphaOptions& o,
                                     std::optional<WorkflowInput> a) try {
  auto status = profile_valid(o.profile);
  if (!status.ok()) {
    return Answer(status);
  }
  Params p = edit_params(o);
  status = add_override(&p, o.metadata_mode, o.metadata_override);
  if (!status.ok()) {
    return Answer(status);
  }
  status = source_parameters(&p, o.alpha_source, a);
  if (!status.ok()) {
    return Answer(status);
  }
  p["placement"] = o.placement;
  if (o.channel_index) {
    if (*o.channel_index > INT64_MAX) {
      return Answer(invalid("channel_index exceeds Int64"));
    }
    p["channel_index"] = static_cast<std::int64_t>(*o.channel_index);
  }
  if (o.output_axis) {
    p["output_axis"] = static_cast<std::int64_t>(*o.output_axis);
  }
  return append(d, std::move(input), a, "alpha.set_" + o.profile, std::move(p));
} catch (const Status& status) {
  return Answer(status);
} catch (const std::bad_alloc&) {
  return Answer(Status{ErrorCode::ResourceExhausted,
                       "alpha authoring allocation failed"});
}

Result<WorkflowNodeOutput> extract_alpha(WorkflowDocument& document,
                                         WorkflowInput input,
                                         const OperationMetadata& metadata,
                                         const ExtractAlphaOptions& o) try {
  auto source = helper_source(document, input, metadata, o);
  if (!source.ok()) {
    return Answer(source.status());
  }
  const auto& s = source.value();
  const auto& input_spec = tensor(metadata);
  if (o.missing_alpha != "error" && o.missing_alpha != "opaque") {
    return Answer(invalid("unknown missing_alpha policy"));
  }
  if (!s.group.alpha && o.missing_alpha == "error") {
    return Answer(invalid("selected group has no alpha"));
  }
  if (o.alpha_encoding) {
    if (o.missing_alpha != "opaque") {
      return Answer(invalid("alpha_encoding requires missing_alpha=opaque"));
    }
    TensorDescription probe;
    probe.component = TensorChannelDescription{"alpha", "alpha", "coverage"};
    probe.component->encoding = *o.alpha_encoding;
    auto status = validate_tensor_description(
        probe, {input_spec.descriptor.element_type, {1}});
    if (!status.ok()) {
      return Answer(status);
    }
  }
  if (s.axis && !o.keepdims && input_spec.descriptor.shape.size() == 1) {
    return Answer(invalid("rank-one alpha extraction requires keepdims=true"));
  }
  auto target = alpha_description(s, input_spec.descriptor.element_type,
                                  o.keepdims, o.alpha_encoding);
  auto descriptor = input_spec.descriptor;
  if (s.axis) {
    if (o.keepdims) {
      descriptor.shape[*s.axis] = 1;
    } else {
      descriptor.shape.erase(descriptor.shape.begin() + *s.axis);
    }
  }
  auto checked = validate_tensor_description(target, descriptor);
  if (!checked.ok()) {
    return Answer(checked);
  }
  auto encoded = tensor_description_parameter(target);
  if (!encoded.ok()) {
    return Answer(encoded.status());
  }
  Params p{{"keepdims", o.keepdims},
           {"layout", o.layout},
           {"expected_inputs", source_assertion({metadata})},
           {"authoring_member", std::string("FMT-05B")},
           {"output_description", encoded.take_value()}};
  if (s.axis) {
    p["axis"] = static_cast<std::int64_t>(*s.axis);
  }
  std::string key;
  if (s.group.alpha) {
    p["index"] = static_cast<std::int64_t>(*s.group.alpha);
    p["metadata_mode"] = o.metadata_mode;
    checked = add_override(&p, o.metadata_mode, o.metadata_override);
    if (!checked.ok()) {
      return Answer(checked);
    }
    auto prepared = plugin_internal::prepare_alpha_extraction(
        {metadata}, p, profile_kind(o.profile));
    if (!prepared.ok()) {
      return Answer(prepared.status());
    }
    key = "channel.extract_index_" + o.profile;
  } else {
    const auto& encoding = s.axis && o.keepdims ? *target.channels[0].encoding
                                                : *target.component->encoding;
    auto bits = opaque_bits(input_spec.descriptor.element_type, encoding);
    if (!bits.ok()) {
      return Answer(bits.status());
    }
    p["bits"] = bits.take_value();
    auto prepared = plugin_internal::prepare_channel_literal_like(
        {metadata}, p, profile_kind(o.profile));
    if (!prepared.ok()) {
      return Answer(prepared.status());
    }
    key = "channel.literal_like_" + o.profile;
  }
  return append(document, std::move(input), {}, key, std::move(p));
} catch (const Status& status) {
  return Answer(status);
} catch (const std::bad_alloc&) {
  return Answer(Status{ErrorCode::ResourceExhausted,
                       "alpha extraction authoring allocation failed"});
}

Result<WorkflowNodeOutput> remove_alpha(WorkflowDocument& document,
                                        WorkflowInput input,
                                        const OperationMetadata& metadata,
                                        const RemoveAlphaOptions& o) try {
  auto source = helper_source(document, input, metadata, o);
  if (!source.ok()) {
    return Answer(source.status());
  }
  const auto& s = source.value();
  const auto& input_spec = tensor(metadata);
  if (o.missing_alpha != "error" && o.missing_alpha != "identity") {
    return Answer(invalid("unknown missing_alpha policy"));
  }
  if (!s.group.alpha && o.missing_alpha == "error") {
    return Answer(invalid("selected group has no alpha"));
  }
  WorkflowDocument staged = document;
  if (!s.axis) {
    MetadataOptions options;
    options.mode = "replace";
    options.description = s.description;
    options.profile = o.profile;
    options.layout = o.layout;
    auto node = assign_metadata(staged, input, options);
    if (!node.ok()) {
      return node;
    }
    staged.nodes.back().parameters["expected_source"] =
        source_assertion({metadata});
    document = std::move(staged);
    return node;
  }
  const bool erase = s.group.alpha && !referenced_elsewhere(s, *s.group.alpha);
  std::vector<std::int64_t> indices;
  std::vector<ChannelMapping> rows;
  for (std::uint64_t c = 0; c < input_spec.descriptor.shape[*s.axis]; ++c) {
    if (erase && c == *s.group.alpha) {
      continue;
    }
    rows.push_back({0, "index", std::to_string(c), rows.size(), {}});
    indices.push_back(static_cast<std::int64_t>(c));
  }
  auto target = remap_description(s, indices, *s.axis, {});
  if (!target.ok()) {
    return Answer(target.status());
  }
  ChannelAssemblyOptions options;
  options.metadata_mode = o.metadata_mode;
  options.profile = o.profile;
  options.layout = o.layout;
  options.output_description = target.take_value();
  if (o.metadata_override) {
    options.input_overrides[0] = *o.metadata_override;
  }
  auto node = assemble_mapped_channels(staged, {input}, *s.axis,
                                       {{false, s.axis}}, rows, options);
  if (!node.ok()) {
    return node;
  }
  auto& p = staged.nodes.back().parameters;
  p["output_description_complete"] = true;
  p["expected_inputs"] =
      plugin_internal::format_result::assembly_source_assertion({metadata});
  p["authoring_member"] = std::string("FMT-05C");
  auto checked = plugin_internal::prepare_alpha_channel_mapping(
      {metadata}, p, profile_kind(o.profile));
  if (!checked.ok()) {
    return Answer(checked.status());
  }
  document = std::move(staged);
  return node;
} catch (const Status& status) {
  return Answer(status);
} catch (const std::bad_alloc&) {
  return Answer(Status{ErrorCode::ResourceExhausted,
                       "alpha removal authoring allocation failed"});
}
}  // namespace ps::format
