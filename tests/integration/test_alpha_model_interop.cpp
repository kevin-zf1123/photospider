#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../support/channel_result_fixture.hpp"
#include "photospider/format/model_conversion.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using assembly_fixture::Fixture;
using assembly_fixture::require;
using channel_fixture::take;
WorkflowInput input(Fixture* fixture, ValueDescriptor descriptor,
                    const std::vector<float>& values,
                    const std::optional<TensorDescription>& description = {},
                    ResultTensorLayout layout = {}) {
  std::vector<ValueFacet> facets;
  if (description)
    facets.push_back(take(encode_tensor_description(*description)));
  auto source = channel_fixture::source(descriptor, facets, layout);
  require(source.bytes.size() == values.size() * sizeof(float),
          "fixture count");
  std::memcpy(source.bytes.data(), values.data(), source.bytes.size());
  return fixture->add_source(std::move(source));
}
TensorDescription description(const ExecutionResult& run) {
  for (const auto& facet :
       assembly_fixture::spec(run.results.at("result")).facets)
    if (facet.key == "photospider.tensor-description")
      return take(decode_tensor_description(facet));
  throw std::runtime_error("missing output description");
}
void gray_insertion_to_model() {
  Fixture fixture;
  TensorDescription gray;
  gray.component = TensorChannelDescription{"Y", "gray", "relative"};
  gray.model = "gray";
  gray.association = "straight";
  gray.reference = "scene";
  gray.white = std::array<double, 2>{.3127, .3290};
  gray.coordinates = TensorModelCoordinates{};
  gray.coordinates->scale = "relative";
  gray.coordinates->gray_kind = "linear_y";
  gray.coordinates->observer = "cie1931_2deg";
  ResultTensorLayout layout;
  layout.spatial = true;
  layout.channel_axis.reset();
  auto source = input(&fixture, {ElementType::Float32, {2, 3}},
                      std::vector<float>(6, 2), gray, layout);
  auto weight = input(&fixture, {ElementType::Float32, {1}}, {.25F});
  format::SetAlphaOptions set;
  set.group = "Y";
  set.alpha_source.kind = "scalar";
  set.placement = "channel";
  set.channel_index = 1;
  set.output_axis = 2;
  auto node = take(format::set_alpha(fixture.document, source, set, weight));
  format::ModelConversionOptions expand;
  expand.group = "Y";
  auto converted = take(format::gray_to_color(fixture.document, node, expand));
  const auto run = assembly_fixture::run(fixture, converted);
  require(assembly_fixture::spec(run.results.at("result")).descriptor.shape ==
              std::vector<std::uint64_t>({2, 3, 4}),
          "Gray alpha insertion feeds native model conversion");
  require(description(run).groups[0].interpretation.model == "xyz",
          "Gray conversion publishes its native XYZ model");
  require(description(run).groups[0].alpha == 3,
          "Gray expansion remaps inserted alpha");
  const auto bytes =
      channel_fixture::read(run.results.at("result"), Region::whole({2, 3, 4}));
  // Exact Fraction arithmetic from binary64 D65 xy, rounded once to Float32.
  const std::array<std::uint32_t, 4> expected{0x3ff35114, 0x40000000,
                                              0x400b663f, 0x3e800000};
  for (std::size_t i = 0; i < 24; ++i) {
    std::uint32_t observed;
    std::memcpy(&observed, bytes.data() + i * 4, 4);
    require(observed == expected[i % 4], "XYZ/alpha composition bits");
  }
}
}  // namespace
int main() {
  try {
    gray_insertion_to_model();
    std::cout << "alpha conversion interoperability passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
