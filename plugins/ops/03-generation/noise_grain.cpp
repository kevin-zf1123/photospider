#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/scalar_math.hpp"
#include "common/registry.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct GrainMix final : Program {
  ElementType type;
  bool monochrome;
  std::array<std::uint64_t, 3> rgb{};
  GrainMix(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& image = tensor(in[0]);
    const auto dimensions = image.sample_shape();
    type = image.descriptor.element_type;
    require(dimensions.size() == 3 && floating(type),
            "grain mix requires a floating HWC image");
    const auto mode = text(p, "mode", "monochrome");
    require(mode == "monochrome" || mode == "independent_rgb", "grain mode");
    monochrome = mode == "monochrome";
    shape(in[1], {dimensions[0], dimensions[1],
                  monochrome ? UINT64_C(1) : UINT64_C(3)});
    shape(in[2], {1});
    const auto description =
        take(tensor_description_from_parameter(text(p, "input_description")));
    check(validate_tensor_description(description, image.descriptor));
    require(description.channel_axis && *description.channel_axis == 2,
            "grain image requires an explicit HWC description");
    const auto name = text(p, "color_group");
    const auto group =
        std::find_if(description.groups.begin(), description.groups.end(),
                     [&](const auto& g) { return g.name == name; });
    require(group != description.groups.end() &&
                group->interpretation.model == "rgb" &&
                group->interpretation.transfer == "linear" &&
                group->interpretation.association == "straight" &&
                group->indices.size() == 3 && group->components.size() == 3 &&
                dimensions[2] == (group->alpha ? 4U : 3U),
            "grain requires a complete straight linear RGB image");
    const auto identity = [](const std::optional<TensorEncoding>& encoding) {
      return !encoding || *encoding == TensorEncoding{};
    };
    require(identity(description.encoding),
            "grain requires native linear samples");
    const std::array<std::string, 3> roles{"red", "green", "blue"};
    for (unsigned c = 0; c < 3; ++c) {
      const auto component =
          std::find_if(group->components.begin(), group->components.end(),
                       [&](const auto& v) { return v.role == roles[c]; });
      require(
          component != group->components.end() && identity(component->encoding),
          "grain requires native RGB components");
      rgb[c] = group->indices[component - group->components.begin()];
      if (!description.channels.empty())
        require(identity(description.channels[rgb[c]].encoding),
                "grain requires native RGB channels");
    }
    add_output(this, tensor_schema(type, dimensions, true, true));
    scratch_bytes = sizeof(ScalarMath<128>);
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    auto grain = region.dimensions();
    grain[2] = {0, monochrome ? UINT64_C(1) : UINT64_C(3)};
    return {{0, 0, region},
            {1, 0, Region(std::move(grain))},
            {2, 0, Region::whole({1})}};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    using M = ScalarMath<128>;
    auto* math = new (invocation.scratch[0].allocation) M(work);
    struct Destroy {
      M* p;
      ~Destroy() { p->~M(); }
    } destroy{math};
    const double strength = reader.number(2, 0, {0});
    const bool narrow = type == ElementType::Float32;
    samples(invocation.outputs[0], [&](const Coordinate& at, std::uint64_t) {
      work.poll(4);
      const auto component = std::find(rgb.begin(), rgb.end(), at[2]);
      if (strength == 0 || component == rgb.end()) {
        store(invocation.outputs[0], at, reader.bits(0, 0, at));
        return;
      }
      auto grain = at;
      grain[2] =
          monochrome ? 0 : static_cast<std::uint64_t>(component - rgb.begin());
      const double color = reader.number(0, 0, at),
                   noise = reader.number(1, 0, grain);
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
      const auto bound =
          Fast::point(color) + Fast::point(strength) * Fast::point(noise);
      if (const auto result = fast_result(bound, narrow, false)) {
        store(invocation.outputs[0], at, *result);
        return;
      }
#endif
      math->exact.reset();
      const auto exact = math->add(math->exact.binary(bits(color)),
                                   math->mul(math->exact.binary(bits(strength)),
                                             math->exact.binary(bits(noise))));
      store(invocation.outputs[0], at, math->rounded(exact, narrow));
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_grain_noise(OperationRegistry* registry) {
  using generation::check;
  using generation::GrainMix;
  using generation::operation;
  using generation::tensor_schema;
  try {
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (unsigned profile = 0; profile < 3; ++profile)
      check(registry->register_operation(operation(
          std::string("noise.artistic_grain_mix_v1_") + suffix[profile], 3,
          {{"mode", OperationParameterType::String, false},
           {"input_description", OperationParameterType::String},
           {"color_group", OperationParameterType::String}},
          {{"image",
            tensor_schema(ElementType::Float64, {1, 1, 3}, true, true)}},
          [profile](const auto& in, const auto& p) {
            check(numeric_ops::sequence_profile_available(
                static_cast<numeric_ops::SequenceProfile>(profile)));
            return std::make_shared<GrainMix>(in, p);
          })));
    return Status::success();
  } catch (const Status& status) {
    return status;
  }
}
}  // namespace ps::plugin_internal
