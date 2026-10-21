#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "02-format-color/color_authoring.hpp"
#include "02-format-color/color_point.hpp"
#include "02-format-color/external_common.hpp"
#include "02-format-color/transfer_prepare.hpp"
#include "photospider/ops/format/rgb_basis.hpp"
#include "photospider/ops/format/view.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"
#include "photospider/ops/registry.hpp"

namespace ps::plugin_internal::view_ops {
using namespace color_point;  // NOLINT(build/namespaces)
enum class Member {
  Decode,
  Encode,
  RgbToXyz,
  XyzToRgb,
  Adapt,
  Convert,
  Clip,
  Reduce,
  Compress,
  Tone,
  Display,
  OcioSpace,
  OcioView,
  OcioLooks,
  OcioNamed,
  OcioFile,
  OcioTree
};
Member member(const std::string& key) {
  for (const auto& profile :
       {"strict", "accelerated_apple_silicon", "accelerated_x86_64"}) {
    for (const auto& item : {std::pair{"transfer_decode", Member::Decode},
                             {"transfer_encode", Member::Encode},
                             {"rgb_to_xyz", Member::RgbToXyz},
                             {"xyz_to_rgb", Member::XyzToRgb},
                             {"adapt_xyz_white", Member::Adapt}}) {
      if (key == std::string("color.") + item.first + "_" + profile) {
        return item.second;
      }
    }
  }
  for (const auto& item :
       {std::pair{"color.convert_linear_rgb", Member::Convert},
        {"color.clip_rgb_gamut_strict", Member::Clip},
        {"color.reduce_oklab_chroma_v1_strict", Member::Reduce},
        {"color.compress_rgb_chroma_v1_strict", Member::Compress},
        {"color.map_luminance_v1_strict", Member::Tone},
        {"color.convert_display_luminance_strict", Member::Display}}) {
    if (key == item.first) {
      return item.second;
    }
  }
  const std::array<const char*, 6> ocio{"color.ocio_convert_space_2_5_2_cpu",
                                        "color.ocio_display_view_2_5_2_cpu",
                                        "color.ocio_looks_2_5_2_cpu",
                                        "color.ocio_named_transform_2_5_2_cpu",
                                        "color.ocio_file_transform_2_5_2_cpu",
                                        "color.ocio_transform_tree_2_5_2_cpu"};
  for (unsigned i = 0; i < ocio.size(); ++i) {
    if (key == ocio[i]) {
      return static_cast<Member>(static_cast<unsigned>(Member::OcioSpace) + i);
    }
  }
  throw basis_ops::CheckedFailure{
      Status{ErrorCode::BackendUnavailable,
             "view stage is outside the explicit FMT members"}};
}
std::string unit(std::string value) {
  if (value == "1" || value == "relative_luminance") {
    return "relative";
  }
  if (value == "cd/m^2") {
    return "cd/m2";
  }
  return value;
}
void reference(TensorInterpretation* i) {
  const auto r = i->reference;
  if (r != "scene_relative" && r != "display_relative" &&
      r != "display_absolute" && r != "absolute_display_cd_m2") {
    return;
  }
  const bool absolute =
      r == "display_absolute" || r == "absolute_display_cd_m2";
  if (!i->coordinates) {
    i->coordinates.emplace();
  }
  require(i->coordinates->scale.empty() ||
              i->coordinates->scale == (absolute ? "absolute" : "relative"),
          "view reference and scale disagree");
  i->coordinates->scale = absolute ? "absolute" : "relative";
  i->reference = r == "scene_relative" ? "scene" : "display";
}
void clear_global(TensorDescription* d) {
  d->model.clear();
  d->primaries.clear();
  d->transfer.clear();
  d->reference.clear();
  d->association.clear();
  d->white.reset();
  d->primaries_xy.reset();
  d->profile.reset();
  d->configured.reset();
  d->analytic_binding.reset();
  d->coordinates.reset();
  d->convention = "relative-v1";
}
void set_global(TensorDescription* d, const TensorInterpretation& i) {
  d->model = i.model;
  d->primaries = i.primaries;
  d->transfer = i.transfer;
  d->reference = i.reference;
  d->association = i.association;
  d->white = i.white;
  d->primaries_xy = i.primaries_xy;
  d->profile = i.profile;
  d->configured = i.configured;
  d->analytic_binding = i.analytic_binding;
  d->coordinates = i.coordinates;
  d->convention = i.convention;
}
// Resolve descriptor inheritance before comparing two explicit configurations.
// The wire representation of equivalent global/group assertions need not match.
TensorDescription normalized(TensorDescription d) {
  const auto source = d;
  const auto inherited = global(source);
  if (!d.channel_axis) {
    auto effective = inherited;
    if (source.component && source.component->interpretation) {
      merge(&effective, *source.component->interpretation);
    }
    if (!source.groups.empty()) {
      merge(&effective, source.groups[0].interpretation);
    }
    reference(&effective);
    set_global(&d, effective);
    if (d.component) {
      d.component->interpretation.reset();
      d.component->unit = unit(d.component->unit);
    }
    if (!d.groups.empty()) {
      d.groups[0].interpretation = effective;
      auto& component = d.groups[0].components[0];
      if (component.unit.empty() && d.component) {
        component.unit = d.component->unit;
      }
      component.unit = unit(component.unit);
    }
    return d;
  }
  std::vector<bool> grouped(d.channels.size(), false);
  for (std::size_t n = 0; n < d.groups.size(); ++n) {
    auto& group = d.groups[n];
    auto effective = inherited;
    merge(&effective, source.groups[n].interpretation);
    for (unsigned k = 0; k < group.indices.size(); ++k) {
      auto& component = group.components[k];
      const auto c = group.indices[k];
      if (c < source.channels.size()) {
        const auto& original = source.channels[c];
        if (original.interpretation) {
          merge(&effective, *original.interpretation);
        }
        if (component.unit.empty()) {
          component.unit = original.unit;
        }
        if (component.role.empty()) {
          component.role = original.role;
        }
        auto& channel = d.channels[c];
        channel.unit = unit(component.unit);
        channel.role = component.role;
        channel.interpretation.reset();
        grouped[c] = true;
      }
      component.unit = unit(component.unit);
    }
    reference(&effective);
    group.interpretation = std::move(effective);
  }
  // Global color fields are resolved into complete color groups. Unselected
  // alpha/AOV channels keep their own explicit interpretations; the image's
  // RGB model does not turn an alpha channel into an RGB coordinate.
  for (std::size_t c = 0; c < source.channels.size(); ++c) {
    if (grouped[c]) {
      continue;
    }
    if (d.channels[c].interpretation) {
      reference(&*d.channels[c].interpretation);
    }
    d.channels[c].unit = unit(d.channels[c].unit);
  }
  if (d.component) {
    if (d.component->interpretation) {
      reference(&*d.component->interpretation);
    }
    d.component->unit = unit(d.component->unit);
  }
  clear_global(&d);
  return d;
}
bool same_description(const TensorDescription& a, const TensorDescription& b) {
  return checked(encode_tensor_description(normalized(a))) ==
         checked(encode_tensor_description(normalized(b)));
}
TensorDescription changed(const Selection& selection, const Parameters& p,
                          TensorInterpretation target,
                          const std::string& target_unit,
                          bool native_identity = true) {
  auto d = normalized(*selection.description);
  if (native_identity) {
    target.profile.reset();
    target.configured.reset();
    target.analytic_binding.reset();
  }
  if (!d.channel_axis) {
    set_global(&d, target);
    if (d.component) {
      d.component->interpretation.reset();
      if (!target_unit.empty()) {
        d.component->unit = target_unit;
      }
    }
    if (!d.groups.empty()) {
      d.groups[0].interpretation = target;
      if (!target_unit.empty()) {
        d.groups[0].components[0].unit = target_unit;
      }
    }
    return d;
  }
  const auto inherited = global(d);
  for (auto& group : d.groups) {
    merge(&group.interpretation, inherited);
    if (group.name != text(p, "group")) {
      continue;
    }
    group.interpretation = target;
    for (unsigned k = 0; k < group.indices.size(); ++k) {
      auto& component = group.components[k];
      component.interpretation.reset();
      if (!target_unit.empty()) {
        component.unit = target_unit;
      }
      const auto c = group.indices[k];
      if (c < d.channels.size()) {
        d.channels[c].interpretation.reset();
        if (!target_unit.empty()) {
          d.channels[c].unit = target_unit;
        }
      }
    }
  }
  clear_global(&d);
  return d;
}
TensorDescription native_output(const OperationMetadata& source,
                                const Parameters& p, Member m) {
  if (m == Member::RgbToXyz || m == Member::XyzToRgb || m == Member::Adapt) {
    return checked(basis_ops::output_description(source, p,
                                                 m == Member::RgbToXyz   ? 0
                                                 : m == Member::XyzToRgb ? 1
                                                                         : 2));
  }
  const auto selection = select(source, p);
  auto target = selection.interpretation;
  reference(&target);
  std::string units;
  if (m == Member::Decode || m == Member::Encode) {
    // Use our own private preparation; never cast a custom registry's state.
    checked(transfer_ops::prepare({source}, p, m == Member::Encode,
                                  numeric_ops::SequenceProfile::Strict));
    auto curve = p.count("curve")
                     ? checked(transfer_ops::definition(p))
                     : checked(decode_transfer_definition(target.transfer));
    target.transfer = m == Member::Encode
                          ? checked(encode_transfer_definition(curve))
                          : "linear";
    if (target.model == "gray") {
      if (!target.coordinates) {
        target.coordinates.emplace();
      }
      target.coordinates->gray_kind =
          m == Member::Encode && curve.curve != TransferCurve::Linear
              ? "encoded_luma"
              : "linear_y";
    }
  } else if (m == Member::Tone || m == Member::Display) {
    target.reference = "display";
    if (!target.coordinates) {
      target.coordinates.emplace();
    }
    const bool absolute =
        m == Member::Display && text(p, "direction") == "relative_to_nits";
    target.coordinates->scale = absolute ? "absolute" : "relative";
    units = absolute ? "cd/m2" : "relative";
  }
  return changed(selection, p, std::move(target), units);
}
bool ocio_member(Member m) {
  return static_cast<unsigned>(m) >= static_cast<unsigned>(Member::OcioSpace);
}
TensorDescription ocio_output(const OperationMetadata& source, Parameters* p,
                              Member m, const TensorDescription& claimed,
                              const ResourceSet& resources) {
  auto claim_parameters = *p;
  claim_parameters["input_description"] =
      checked(tensor_description_parameter(claimed));
  const auto target =
      select(source, claim_parameters, external_color::roles(1), false);
  const auto endpoint_parameter = checked(
      format::color_endpoint_parameter(target.interpretation, target.units));
  const char* effective_exit = text(*p, "direction", "forward") == "inverse"
                                   ? "entry_space"
                                   : "exit_space";
  const auto existing = p->find(effective_exit);
  if (existing == p->end()) {
    (*p)[effective_exit] = endpoint_parameter;
  }
  // The implementation validates any existing assertion; never overwrite it.
  auto resolved = external_color::endpoint(
      checked(ocio_ops::output_endpoint(
          source, *p,
          static_cast<unsigned>(m) - static_cast<unsigned>(Member::OcioSpace),
          resources)),
      1);
  auto selection = select(source, *p, external_color::roles(1), false);
  auto output = changed(selection, *p, resolved.interpretation, "", false);
  const auto names = external_color::roles(1);
  for (auto& group : output.groups) {
    if (group.name == text(*p, "group")) {
      for (unsigned k = 0; k < group.indices.size(); ++k) {
        auto at =
            std::find(names.begin(), names.end(), group.components[k].role);
        require(at != names.end(), "OCIO output role disagrees");
        const auto& units =
            resolved.units[static_cast<unsigned>(at - names.begin())];
        group.components[k].unit = units;
        if (group.indices[k] < output.channels.size()) {
          output.channels[group.indices[k]].unit = units;
        }
      }
    }
  }
  return output;
}
void conditions(const OperationMetadata& metadata,
                const format::ConfiguredDisplayConditions& display,
                const ResourceSet& resources) {
  Parameters p{{"input_description",
                checked(tensor_description_parameter(display.description))},
               {"group", display.group}};
  const auto selected = select(metadata, p, external_color::roles(1), false);
  require(selected.interpretation.configured &&
              selected.interpretation.configured->config == display.config,
          "configured display manifest identity disagrees");
  require(!display.display.empty() && !display.view.empty(),
          "explicit display/view required");
  p["config"] = "resource-v1:" + external_color::identity_text(display.config);
  p["source_space"] = "name:" + selected.interpretation.configured->space;
  p["display"] = display.display;
  p["view"] = display.view;
  p["reference_bridge"] = display.reference_bridge;
  p["exit_space"] = checked(format::color_endpoint_parameter(
      selected.interpretation, selected.units));
  (void)checked(ocio_ops::output_endpoint(metadata, p, 1, resources));
}
format::RgbBasisOptions conversion_options(const Parameters& p) {
  const std::set<std::string> allowed{"group",        "input_description",
                                      "source_basis", "target_basis",
                                      "source_white", "target_white",
                                      "method",       "white_handling",
                                      "profile",      "layout"};
  for (const auto& item : p) {
    require(allowed.count(item.first),
            "inapplicable linear RGB view parameter");
  }
  format::RgbBasisOptions o;
  require(p.count("profile"),
          "composite RGB view stage requires explicit profile");
  o.profile = text(p, "profile");
  o.layout = text(p, "layout", "auto");
  o.group = text(p, "group");
  o.input_description =
      checked(tensor_description_from_parameter(text(p, "input_description")));
  const auto basis = [](const std::string& value) {
    const auto resolved = basis_ops::parse_basis(value);
    return format::RgbBasis{resolved.preset, resolved.xy, resolved.white};
  };
  if (p.count("source_basis")) {
    o.source_basis = basis(text(p, "source_basis"));
  }
  if (p.count("target_basis")) {
    o.target_basis = basis(text(p, "target_basis"));
  }
  if (p.count("source_white")) {
    o.source_white = basis_ops::parse_white(text(p, "source_white"));
  }
  if (p.count("target_white")) {
    o.target_white = basis_ops::parse_white(text(p, "target_white"));
  }
  if (p.count("method")) {
    o.method = text(p, "method");
  }
  if (p.count("white_handling")) {
    o.white_handling = text(p, "white_handling");
  }
  return o;
}
void conditions(const OperationMetadata& metadata,
                const format::NativeDisplayConditions& display) {
  Parameters p{{"input_description",
                checked(tensor_description_parameter(display.description))}};
  if (!display.group.empty()) {
    p["group"] = display.group;
  }
  const auto selected = select(metadata, p);
  auto i = selected.interpretation;
  reference(&i);
  require(!i.profile && !i.configured && i.reference == "display",
          "native view endpoint must be explicit display-referred coordinates");
  typed(i.model == "rgb" || i.model == "gray",
        "view endpoint must be RGB or Gray");
  require(!i.transfer.empty(), "view endpoint requires transfer identity");
  const auto curve = checked(decode_transfer_definition(i.transfer));
  if (i.model == "rgb") {
    (void)basis_ops::resolve({i.primaries, i.primaries_xy, i.white});
  } else {
    require(i.white.has_value() && i.coordinates &&
                (i.coordinates->gray_kind == "linear_y" ||
                 i.coordinates->gray_kind == "encoded_luma"),
            "view Gray endpoint requires white and coordinate kind");
  }
  require(i.coordinates && (i.coordinates->scale == "relative" ||
                            i.coordinates->scale == "absolute"),
          "view endpoint requires explicit scale");
  for (const auto& u : selected.units) {
    require(
        unit(u) == (i.coordinates->scale == "absolute" ? "cd/m2" : "relative"),
        "view endpoint unit/scale conflict");
  }
  if (curve.curve == TransferCurve::Bt1886) {
    require(curve.black_luminance == display.black_nits &&
                curve.white_luminance == display.white_nits,
            "BT.1886 endpoint and display conditions disagree");
  }
}
void stage_conditions(const Parameters& p, Member m,
                      const format::NativeDisplayConditions& display) {
  if (m == Member::Display) {
    require(real(p, "black_nits", -1) == display.black_nits &&
                real(p, "white_nits", -1) == display.white_nits,
            "display-range stage and final display conditions disagree");
  }
  if ((m == Member::Decode || m == Member::Encode) &&
      text(p, "curve") == "bt1886") {
    require(real(p, "black_luminance", -1) == display.black_nits &&
                real(p, "white_luminance", -1) == display.white_nits,
            "BT.1886 stage and final display conditions disagree");
  }
}
}  // namespace ps::plugin_internal::view_ops

namespace ps::format {
namespace {
template <class Display>
Result<WorkflowNodeOutput> compose_impl(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const Display& display, const ResourceSet& resources,
    std::shared_ptr<OperationRegistry> registry) {
  using namespace plugin_internal::view_ops;  // NOLINT(build/namespaces)
  using Answer = Result<WorkflowNodeOutput>;
  try {
    require(!stages.empty() && stages.size() <= 65536,
            "view requires a bounded nonempty stage list");
    plugin_internal::FloatingEnvironment environment;
    typed(environment.active(), "floating environment unavailable");
    if (!registry) {
      registry = make_default_operation_registry();
    }
    auto metadata = checked(plugin_internal::basis_ops::workflow_metadata(
        document, input, *registry, resources));
    checked(plugin_internal::tensor_ops::check_tensor(metadata));
    checked(validate_tensor_description(
        entry, metadata.result_schema->tensors[0].descriptor));
    WorkflowDocument staged = document;
    auto description = entry;
    WorkflowNodeOutput output{};
    for (const auto& stage : stages) {
      const auto kind = member(stage.operation);
      require(!stage.parameters.count("input_description") &&
                  !stage.parameters.count("axis") &&
                  !stage.parameters.count("components"),
              "view stage requires explicit semantic configuration");
      checked(validate_tensor_description(
          stage.input_description,
          metadata.result_schema->tensors[0].descriptor));
      require(same_description(description, stage.input_description),
              "view input configuration disagrees with prior output");
      auto p = stage.parameters;
      p["input_description"] =
          checked(tensor_description_parameter(stage.input_description));
      if constexpr (std::is_same_v<Display, NativeDisplayConditions>) {
        stage_conditions(p, kind, display);
      }
      TensorDescription inferred;
      if (kind == Member::Convert) {
        const auto first = staged.nodes.size();
        output = checked(plugin_internal::basis_ops::author_view_conversion(
            staged, input, conversion_options(p), registry, resources));
        auto current = metadata;
        for (std::size_t n = first; n < staged.nodes.size(); ++n) {
          const auto& node = staged.nodes[n];
          const auto native = member(node.operation);
          inferred = native_output(current, node.parameters, native);
          auto prepared = checked(registry->prepare_operation(
              node.operation, {current}, node.parameters, resources));
          auto outputs = checked(infer_operation_outputs(
              prepared->traits(), {current}, node.parameters));
          current = outputs[0];
        }
        metadata = std::move(current);
      } else {
        if (ocio_member(kind)) {
          inferred = ocio_output(metadata, &p, kind, stage.output_description,
                                 resources);
        } else {
          inferred = native_output(metadata, p, kind);
        }
        auto prepared = checked(registry->prepare_operation(
            stage.operation, {metadata}, p, resources));
        auto outputs =
            checked(infer_operation_outputs(prepared->traits(), {metadata}, p));
        require(outputs.size() == 1 &&
                    prepared->traits().outputs[0].key == "values",
                "view stage requires one values output");
        const auto id = checked(
            numeric::available_workflow_node_ids(staged, 1, {input}))[0];
        staged.nodes.push_back({id, stage.operation, {input}, std::move(p)});
        output = {id, "values"};
        metadata = std::move(outputs[0]);
      }
      checked(validate_tensor_description(
          stage.output_description,
          metadata.result_schema->tensors[0].descriptor));
      require(same_description(inferred, stage.output_description),
              "view output configuration disagrees with stage semantics: " +
                  stage.operation);
      description = stage.output_description;
      input = output;
    }
    checked(validate_tensor_description(
        display.description, metadata.result_schema->tensors[0].descriptor));
    require(same_description(description, display.description),
            "final view description disagrees");
    if constexpr (std::is_same_v<Display, NativeDisplayConditions>) {
      conditions(metadata, display);
    } else {
      conditions(metadata, display, resources);
    }
    document = std::move(staged);
    return Answer(output);
  } catch (const plugin_internal::basis_ops::CheckedFailure& failure) {
    return Answer(failure.status);
  } catch (const plugin_internal::basis_ops::GeometryError&) {
    return Answer(plugin_internal::basis_ops::invalid("invalid view geometry"));
  } catch (const data_internal::format_numeric::ExactWorkFailure& failure) {
    return Answer(failure.status);
  } catch (const std::bad_variant_access&) {
    return Answer(
        plugin_internal::basis_ops::invalid("invalid view parameter type"));
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         "view authoring capacity exhausted",
                         FailureReason::CapacityLimit});
  }
}
}  // namespace
Result<WorkflowNodeOutput> compose_view(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const NativeDisplayConditions& display,
    std::shared_ptr<OperationRegistry> registry) {
  return compose_impl(document, std::move(input), entry, stages, display, {},
                      std::move(registry));
}
Result<WorkflowNodeOutput> compose_view(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const NativeDisplayConditions& display, const ResourceSet& resources,
    std::shared_ptr<OperationRegistry> registry) {
  return compose_impl(document, std::move(input), entry, stages, display,
                      resources, std::move(registry));
}
Result<WorkflowNodeOutput> compose_view(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const ConfiguredDisplayConditions& display, const ResourceSet& resources,
    std::shared_ptr<OperationRegistry> registry) {
  return compose_impl(document, std::move(input), entry, stages, display,
                      resources, std::move(registry));
}
}  // namespace ps::format
