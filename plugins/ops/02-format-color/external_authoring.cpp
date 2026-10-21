#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_authoring.hpp"
#include "02-format-color/external_common.hpp"
#include "photospider/ops/format/external_color.hpp"

namespace ps::format {
namespace basis_ops = plugin_internal::basis_ops;
using namespace plugin_internal::external_color;  // NOLINT(build/namespaces)
std::string color_resource_parameter(const ImmutableResource& resource) {
  return "resource-v1:" + identity_text(resource.identity());
}
std::string icc_profile_parameter(const IccProfile& profile) {
  const auto bytes = profile.bytes();
  return std::string("icc-v1:") +
         model_name(signature_model(signature(bytes, 16))) + ":" +
         model_name(signature_model(signature(bytes, 20))) + ":" +
         signature(bytes, 12) + ":" + identity_text(profile.identity());
}
Result<std::string> color_endpoint_parameter(
    const TensorInterpretation& interpretation,
    const std::vector<std::string>& units) try {
  const auto names = roles(model_id(interpretation.model));
  require(units.size() == names.size(), "endpoint units count disagrees");
  TensorDescription d;
  d.channel_axis = 0;
  d.model = interpretation.model;
  d.primaries = interpretation.primaries;
  d.transfer = interpretation.transfer;
  d.reference = interpretation.reference;
  d.association = interpretation.association;
  d.white = interpretation.white;
  d.primaries_xy = interpretation.primaries_xy;
  d.profile = interpretation.profile;
  d.configured = interpretation.configured;
  d.convention = interpretation.convention;
  d.analytic_binding = interpretation.analytic_binding;
  d.coordinates = interpretation.coordinates;
  TensorColorGroup g;
  g.name = "color";
  g.interpretation = interpretation;
  for (unsigned c = 0; c < names.size(); ++c) {
    g.indices.push_back(c);
    TensorChannelDescription component;
    component.name = names[c];
    component.role = names[c];
    component.unit = units[c];
    g.components.push_back(component);
  }
  d.groups.push_back(std::move(g));
  return tensor_description_parameter(d);
} catch (const basis_ops::CheckedFailure& f) {
  return Result<std::string>(f.status);
}
Result<ColorResourceBinding> icc_chain(const std::vector<IccChainStage>& stages,
                                       const ResourceBudget& budget) try {
  require(stages.size() >= 2 && stages.size() <= 255,
          "ICC chain requires 2..255 stages");
  ResourceVector<std::uint8_t> data{
      ResourceAllocator<std::uint8_t>(budget, ResourceAllocationKind::Payload)};
  const auto append = [&](std::uint64_t n, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
      data.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    }
  };
  for (char c : std::string("PSICC1")) {
    data.push_back(static_cast<std::uint8_t>(c));
  }
  const auto first = profile_ref(icc_profile_parameter(stages.front().profile));
  const auto last = profile_ref(icc_profile_parameter(stages.back().profile));
  const int in = first.input;
  const int out = last.cls == "link" ? last.output : last.input;
  append(in, 1);
  append(out, 1);
  append(stages.size(), 2);
  for (const auto& stage : stages) {
    const auto ref = profile_ref(icc_profile_parameter(stage.profile));
    const auto d = direction(stage.direction);
    require(ref.cls == "link" || ref.cls == "abst" ? d == 2 : d < 2,
            "stage direction/class mismatch");
    require(ref.cls != "abst" ||
                (&stage != &stages.front() && &stage != &stages.back()),
            "abstract endpoint forbidden");
    require(ref.cls != "link" || (!stage.bpc && stage.intent.empty()),
            "DeviceLink controls are fixed");
    require(d != 0 || !stage.bpc, "device_to_pcs BPC is not applicable");
    append(d, 1);
    append(ref.cls == "link"
               ? 255
               : intent(stage.intent.empty() ? "relative_colorimetric"
                                             : stage.intent),
           1);
    append(stage.bpc ? (*stage.bpc ? 1 : 0) : 2, 1);
    const auto bytes = stage.profile.bytes();
    append(bytes.size(), 8);
    checked(budget.consume({bytes.size()}));
    data.insert(data.end(), bytes.begin(), bytes.end());
  }
  auto resource = checked(ImmutableResource::import(
      ByteView(data.data(), data.size()), budget, {}, UINT64_MAX));
  std::string parameter = std::string("chain-v1:") + model_name(in) + ":" +
                          model_name(out) + ":" +
                          (first.cls == "link" ? "1" : "0") + ":" +
                          (last.cls == "link" ? "1" : "0") + ":" +
                          identity_text(resource.identity());
  return Result<ColorResourceBinding>(
      ColorResourceBinding{std::move(resource), std::move(parameter)});
} catch (const basis_ops::CheckedFailure& f) {
  return Result<ColorResourceBinding>(f.status);
} catch (const std::bad_alloc&) {
  return Result<ColorResourceBinding>(
      Status{ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
}
std::string external_color_engine_identity() {
  return ps_color_engine_identity_v1();
}
Result<OcioConfigResource> ocio_config(OcioConfigSnapshot snapshot,
                                       const ResourceBudget& budget) {
  snapshot.engine_version = "2.5.2";
  snapshot.build_identity = external_color_engine_identity();
  snapshot.settings = "closed-rgb-v1";
  return OcioConfigResource::import(snapshot, budget, {}, UINT64_MAX);
}
Result<ColorResourceBinding> ocio_file(const std::string& name,
                                       const ImmutableResource& resource) try {
  require(resource.valid() && !name.empty() && name.size() <= 128 &&
              name.find('\0') == name.npos && name.find('$') == name.npos,
          "invalid immutable OCIO file binding");
  constexpr char digits[] = "0123456789abcdef";
  std::string encoded;
  for (unsigned char c : name) {
    encoded += digits[c >> 4];
    encoded += digits[c & 15];
  }
  return Result<ColorResourceBinding>(ColorResourceBinding{
      resource,
      "file-v1:" + encoded + ":" + identity_text(resource.identity())});
} catch (const basis_ops::CheckedFailure& f) {
  return Result<ColorResourceBinding>(f.status);
}
Result<ColorResourceBinding> ocio_transform_tree(
    const std::string& yaml, const ResourceBudget& budget) try {
  require(!yaml.empty() && yaml.find('\0') == yaml.npos,
          "invalid OCIO transform fragment");
  auto resource = checked(ImmutableResource::import(
      ByteView(reinterpret_cast<const std::uint8_t*>(yaml.data()), yaml.size()),
      budget, {}, UINT64_MAX));
  auto parameter = color_resource_parameter(resource);
  return Result<ColorResourceBinding>(
      ColorResourceBinding{std::move(resource), std::move(parameter)});
} catch (const basis_ops::CheckedFailure& f) {
  return Result<ColorResourceBinding>(f.status);
}
Result<std::string> inspect_external_color(
    const std::string& operation, const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters,
    const ResourceSet& resources) {
  const std::array<const char*, 5> icc{
      "color.icc_transform_lcms2_19_1_cpu",
      "color.icc_apply_devicelink_lcms2_19_1_cpu",
      "color.icc_transform_chain_lcms2_19_1_cpu",
      "color.softproof_lcms2_19_1_cpu",
      "color.gamut_alarm_lcms2_19_1_source_dims_v1_cpu"};
  const std::array<const char*, 6> ocio{"color.ocio_convert_space_2_5_2_cpu",
                                        "color.ocio_display_view_2_5_2_cpu",
                                        "color.ocio_looks_2_5_2_cpu",
                                        "color.ocio_named_transform_2_5_2_cpu",
                                        "color.ocio_file_transform_2_5_2_cpu",
                                        "color.ocio_transform_tree_2_5_2_cpu"};
  for (unsigned i = 0; i < icc.size(); ++i) {
    if (operation == icc[i]) {
      return plugin_internal::icc_ops::construction_trace(input, parameters, i,
                                                          resources);
    }
  }
  for (unsigned i = 0; i < ocio.size(); ++i) {
    if (operation == ocio[i]) {
      return plugin_internal::ocio_ops::construction_trace(input, parameters, i,
                                                           resources);
    }
  }
  return Result<std::string>(
      Status{ErrorCode::InvalidArgument, "unknown external color operation"});
}
}  // namespace ps::format
