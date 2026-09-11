#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Value samples(std::vector<std::uint64_t> shape,
              const std::vector<float>& numbers,
              const std::vector<ValueFacet>& facets = {}) {
  auto allocated = MutableValue::allocate(
      {ElementType::Float32, shape}, Region::whole(shape), BufferAllocator{});
  auto output = allocated.take_value();
  std::memcpy(output.data(), numbers.data(), numbers.size() * sizeof(float));
  return std::move(output).publish(facets).take_value();
}
SemanticDescriptor rgb() {
  auto semantic = rgba_semantics();
  semantic.channels.pop_back();
  semantic.association = "none";
  return semantic;
}
WorkflowInputDeclaration declaration(std::uint64_t id, const std::string& name,
                                     const Value& value) {
  return {id,
          name,
          value.descriptor(),
          value.region(),
          value.layout(),
          value.facets()};
}
std::array<long double, 3> oracle_pixel(const float* input) {
  std::array<long double, 3> encoded;
  for (unsigned c = 0; c < 3; ++c) {
    const long double linear = input[c];
    encoded[c] = linear < .018L ? linear * 4.5L
                                : 1.099L * std::pow(linear, .45L) - .099L;
  }
  const auto y =
      .2126L * encoded[0] + .7152L * encoded[1] + .0722L * encoded[2];
  return {y, (encoded[2] - y) / 1.8556L, (encoded[0] - y) / 1.5748L};
}
int ycbcr420() {
  auto registry = make_default_operation_registry();
  constexpr std::uint64_t h = 3, w = 5;
  std::vector<float> pixels(h * w * 3);
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = static_cast<float>((i * 7) % 23) / 22;
  pixels[0] = std::nextafter(.018F, 0.F);
  pixels[1] = .018F;
  pixels[2] = std::nextafter(.018F, 1.F);
  auto input =
      samples({h, w, 3}, pixels, {encode_semantic(rgb()).take_value()});
  WorkflowDocument document;
  document.inputs = {declaration(1, "rgb", input)};
  document.nodes = {
      {1, "color.rgb_to_ycbcr420", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"y", 1, "y"}, {"cb", 1, "cb"}, {"cr", 1, "cr"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_height = 1;
  planning.tile_width = 2;
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    std::cerr << compiled.status().message << '\n';
  PS_CHECK(compiled.ok());
  std::array<std::vector<float>, 3> reference;
  for (bool joint : {false, true}) {
    ExecutionContext execution(registry, {2, false, 64, 1048576, 0});
    auto frozen = execution.freeze(compiled.value().plan, {{{"rgb", input}}})
                      .take_value();
    DemandQuery query{{"y", Footprint::all({h, w}).take_value()},
                      {"cb", Footprint::all({2, 3}).take_value()},
                      {"cr", Footprint::all({2, 3}).take_value()}};
    ExecutionOptions options;
    options.enable_joint = joint;
    auto result = execution.execute_fragments(frozen, query, {}, options);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK((result.value().diagnostics.joint_groups != 0) == joint);
    const std::array<std::string, 3> names{"y", "cb", "cr"};
    for (unsigned plane = 0; plane < 3; ++plane) {
      const auto& value = result.value().values.at(names[plane]);
      auto semantic = decode_semantic(value.facets()[0]);
      PS_CHECK(semantic.ok() &&
               semantic.value().kind == SemanticKind::ImagePlane);
      const double factor = plane ? 2 : 1;
      PS_CHECK(semantic.value().plane_step ==
               (std::array<double, 2>{factor, factor}));
      PS_CHECK(semantic.value().plane_origin[0] == (plane ? .5 : 0));
      const auto rows = plane ? 2U : h, columns = plane ? 3U : w;
      for (std::uint64_t y = 0; y < rows; ++y)
        for (std::uint64_t x = 0; x < columns; ++x) {
          const auto stride = plane ? 2U : 1U;
          long double expected = 0;
          unsigned count = 0;
          for (auto sy = y * stride; sy < std::min(h, (y + 1) * stride); ++sy)
            for (auto sx = x * stride; sx < std::min(w, (x + 1) * stride);
                 ++sx) {
              expected += oracle_pixel(&pixels[(sy * w + sx) * 3])[plane];
              ++count;
            }
          float actual = 0;
          PS_CHECK(value.read({y, x}, &actual, sizeof(actual)).ok());
          PS_CHECK(std::abs(static_cast<long double>(actual) -
                            expected / count) < 1e-7L);
          if (joint)
            PS_CHECK(actual == reference[plane][y * columns + x]);
          else
            reference[plane].push_back(actual);
        }
    }
    auto only_y = execution.execute_fragments(
        frozen,
        {{"y", Footprint::from_regions({h, w}, {Region({{2, 1}, {4, 1}})})
                   .take_value()}});
    PS_CHECK(only_y.ok() && only_y.value().diagnostics.joint_groups == 0);
    for (const auto& timing : only_y.value().diagnostics.operation_timings)
      PS_CHECK(timing.output.output_index == 0);
    auto dirty = result.value().dependencies.potential_dirty(
        "rgb",
        Footprint::from_regions({h, w, 3}, {Region({{2, 1}, {4, 1}, {0, 3}})})
            .take_value());
    PS_CHECK(dirty.ok() && dirty.value().at("cb").contains({1, 2}) &&
             !dirty.value().at("cb").contains({0, 0}));
  }
  for (float invalid : {-1.F, 2.F, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
    auto bad_pixels = pixels;
    bad_pixels[0] = invalid;
    auto bad =
        samples({h, w, 3}, bad_pixels, {encode_semantic(rgb()).take_value()});
    ExecutionContext execution(registry);
    auto frozen = execution.freeze(compiled.value().plan, {{{"rgb", bad}}});
    if (frozen.ok())
      PS_CHECK(
          !execution
               .execute_fragments(frozen.value(),
                                  {{"y", Footprint::all({h, w}).take_value()}})
               .ok());
  }
  auto reversed = rgb();
  std::swap(reversed.channels[0], reversed.channels[2]);
  reversed.reference = "display";
  auto bgr =
      samples({1, 1, 3}, {0, 0, 1}, {encode_semantic(reversed).take_value()});
  std::vector<Value> direct_inputs{bgr};
  std::vector<Region> direct_demands{bgr.region()};
  std::map<std::string, ParameterValue> parameters;
  OperationInvocation invocation(direct_inputs, direct_demands, parameters);
  auto direct = registry->invoke("color.rgb_to_ycbcr420", invocation);
  PS_CHECK(direct.ok());
  float luma;
  std::memcpy(&luma, direct.value().bytes().data(), sizeof(luma));
  PS_CHECK(std::abs(luma - .2126F) < 1e-7F);
  PS_CHECK(decode_semantic(direct.value().facets()[0]).value().reference ==
           "display");
  auto plane =
      decode_semantic(compiled.value().semantic.nodes()[0].outputs[0].facets[0])
          .take_value();
  PS_CHECK(
      validate_semantic_descriptor(plane, {ElementType::Float32, {3, 5}}).ok());
  PS_CHECK(
      !validate_semantic_descriptor(plane, {ElementType::Float32, {3, 5, 1}})
           .ok());
  plane.plane_step[0] = 0;
  PS_CHECK(!encode_semantic(plane).ok());
  auto rgba = samples({1, 1, 4}, {0, 0, 0, 1},
                      {encode_semantic(rgba_semantics()).take_value()});
  document.inputs = {declaration(1, "rgb", rgba)};
  GraphContext alpha(document);
  PS_CHECK(!Compiler(registry).compile(alpha).ok());
  return 0;
}
int split_horizontal() {
  auto registry = make_default_operation_registry();
  std::vector<float> numbers(3 * 5 * 3);
  for (std::size_t i = 0; i < numbers.size(); ++i)
    numbers[i] = static_cast<float>(i);
  auto input =
      samples({3, 5, 3}, numbers, {encode_semantic(rgb()).take_value()});
  WorkflowDocument document;
  document.inputs = {declaration(1, "image", input)};
  document.nodes = {{1,
                     "image.split_horizontal",
                     {WorkflowInputReference{1}},
                     {{"split_x", std::int64_t{2}}}}};
  document.outputs = {{"full", 1, "full"},
                      {"left", 1, "left"},
                      {"right", 1, "right"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  for (bool joint : {false, true}) {
    ExecutionContext execution(registry);
    auto frozen = execution.freeze(compiled.value().plan, {{{"image", input}}})
                      .take_value();
    DemandQuery query{
        {"full",
         Footprint::from_regions({3, 5, 3}, {Region({{2, 1}, {4, 1}, {0, 3}})})
             .take_value()},
        {"left",
         Footprint::from_regions({3, 2, 3}, {Region({{1, 1}, {1, 1}, {0, 3}})})
             .take_value()},
        {"right",
         Footprint::from_regions({3, 3, 3}, {Region({{0, 1}, {2, 1}, {0, 3}})})
             .take_value()}};
    ExecutionOptions options;
    options.enable_joint = joint;
    auto result = execution.execute_fragments(frozen, query, {}, options);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    PS_CHECK((result.value().diagnostics.joint_groups == 1) == joint);
    for (const auto& entry : query) {
      const auto& output = result.value().values.at(entry.first);
      PS_CHECK(output.fragments()[0].storage() == input.storage());
      PS_CHECK(output.facets()[0].payload == input.facets()[0].payload);
      const auto& dimensions = entry.second.boxes()[0].dimensions();
      for (std::uint64_t c = 0; c < 3; ++c) {
        float actual;
        PS_CHECK(output
                     .read({dimensions[0].offset, dimensions[1].offset, c},
                           &actual, sizeof(actual))
                     .ok());
        const auto source_x =
            dimensions[1].offset + (entry.first == "right" ? 2 : 0);
        PS_CHECK(actual ==
                 numbers[(dimensions[0].offset * 5 + source_x) * 3 + c]);
      }
    }
    auto dirty = result.value().dependencies.potential_dirty(
        "image",
        Footprint::from_regions({3, 5, 3}, {Region({{0, 1}, {4, 1}, {0, 3}})})
            .take_value());
    PS_CHECK(dirty.ok() && dirty.value().at("right").contains({0, 2, 0}));
    PS_CHECK(dirty.value().at("left").empty() &&
             dirty.value().at("full").empty());
  }
  for (std::int64_t split : {-1, 0, 5, 6}) {
    document.nodes[0].parameters["split_x"] = split;
    GraphContext invalid(document);
    PS_CHECK(!Compiler(registry).compile(invalid).ok());
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(ycbcr420() == 0);
  PS_CHECK(split_horizontal() == 0);
  return 0;
}
