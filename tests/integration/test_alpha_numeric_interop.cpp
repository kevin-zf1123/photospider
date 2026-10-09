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
#include "photospider/ops.hpp"
#include "photospider/photospider.hpp"

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
void represented_interval_to_numeric() {
  for (bool inherited : {false, true})
    for (bool moved : {false, true}) {
      Fixture fixture;
      TensorDescription d;
      d.channel_axis = 1;
      d.channels = {{"Y", "gray", "relative"}, {"A", "alpha", "coverage"}};
      TensorColorGroup group;
      group.name = "gray";
      group.indices = {0};
      group.components = {d.channels[0]};
      group.interpretation.model = "gray";
      group.interpretation.association = "straight";
      group.alpha = 1;
      d.groups = {group};
      TensorEncoding encoding;
      encoding.stored = {std::int64_t{-1}, std::int64_t{2}};
      encoding.decoded = {-1.0, 2.0};
      if (inherited) {
        d.encoding = encoding;
        d.channels[0].encoding = TensorEncoding{};
        d.groups[0].components[0].encoding = TensorEncoding{};
      } else {
        d.channels[1].encoding = encoding;
      }
      const auto source =
          input(&fixture, {ElementType::Float32, {1, 2}}, {.5F, .5F}, d);
      format::SetAlphaOptions options;
      options.group = "gray";
      options.alpha_source.channel.value = "1";
      if (moved) {
        options.placement = "channel";
        options.channel_index = 0;
      }
      const auto set =
          take(format::set_alpha(fixture.document, source, options));
      const auto id = fixture.document.nodes.back().id + 1;
      fixture.document.nodes.push_back(
          {id,
           "numeric.convert_format_strict",
           {set},
           {{"dtype", std::string("float32")},
            {"axis", std::int64_t{1}},
            {"source_range",
             std::string(moved ? "i:-1,i:2;i:0,i:1" : "i:0,i:1;i:-1,i:2")}}});
      const auto run = assembly_fixture::run(fixture, {id, "values"});
      require(
          assembly_fixture::spec(run.results.at("result")).descriptor.shape ==
              std::vector<std::uint64_t>({1, 2}),
          "preserved alpha interval remains acceptable to FMT-06");
    }
}
}  // namespace
int main() {
  try {
    represented_interval_to_numeric();
    std::cout << "alpha numeric interoperability passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
