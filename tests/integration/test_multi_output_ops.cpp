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
float convolution_oracle(const std::vector<float>& image, std::uint64_t h,
                         std::uint64_t w, unsigned channel,
                         const std::vector<float>& kernel, std::uint64_t kh,
                         std::uint64_t kw, std::int64_t ay, std::int64_t ax,
                         bool clamp, std::uint64_t y, std::uint64_t x) {
  double sum = 0;
  for (std::uint64_t ky = 0; ky < kh; ++ky)
    for (std::uint64_t kx = 0; kx < kw; ++kx) {
      auto sy =
          static_cast<std::int64_t>(y) + ay - static_cast<std::int64_t>(ky);
      auto sx =
          static_cast<std::int64_t>(x) + ax - static_cast<std::int64_t>(kx);
      if (clamp) {
        sy = std::clamp<std::int64_t>(sy, 0, h - 1);
        sx = std::clamp<std::int64_t>(sx, 0, w - 1);
      }
      const double value =
          sy < 0 || sx < 0 || sy >= static_cast<std::int64_t>(h) ||
                  sx >= static_cast<std::int64_t>(w)
              ? 0
              : image[(static_cast<std::uint64_t>(sy) * w + sx) * 3 + channel];
      sum += value * static_cast<double>(kernel[ky * kw + kx]);
    }
  return static_cast<float>(sum);
}
int channel_convolution() {
  auto registry = make_default_operation_registry();
  constexpr std::uint64_t h = 3, w = 4;
  std::vector<float> image_data(h * w * 3);
  for (std::size_t i = 0; i < image_data.size(); ++i)
    image_data[i] = static_cast<float>(i) / 7;
  auto image =
      samples({h, w, 3}, image_data, {encode_semantic(rgb()).take_value()});
  const std::array<std::vector<float>, 3> coefficients{
      {{1, 2, -1, .5F}, {.25F, .5F, .25F}, {1, -2, 1}}};
  const std::array<std::array<std::uint64_t, 2>, 3> shapes{
      {{2, 2}, {1, 3}, {3, 1}}};
  const std::array<std::array<std::int64_t, 2>, 3> anchors{
      {{0, 1}, {0, 1}, {1, 0}}};
  const std::array<std::string, 3> names{"r", "g", "b"};
  std::array<Value, 3> kernels;
  WorkflowDocument doc;
  doc.inputs = {declaration(1, "image", image)};
  std::map<std::string, ParameterValue> parameters;
  for (unsigned i = 0; i < 3; ++i) {
    kernels[i] = samples({shapes[i][0], shapes[i][1]}, coefficients[i]);
    doc.inputs.push_back(declaration(i + 2, "k" + names[i], kernels[i]));
    parameters[names[i] + "_anchor_y"] = anchors[i][0];
    parameters[names[i] + "_anchor_x"] = anchors[i][1];
    parameters[names[i] + "_boundary"] = std::string(i == 1 ? "clamp" : "zero");
    doc.outputs.push_back({names[i], 1, names[i]});
  }
  doc.nodes = {{1,
                "image.convolve_channels",
                {WorkflowInputReference{1}, WorkflowInputReference{2},
                 WorkflowInputReference{3}, WorkflowInputReference{4}},
                parameters}};
  GraphContext graph(doc);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionBindings bindings{{{"image", image},
                              {"kr", kernels[0]},
                              {"kg", kernels[1]},
                              {"kb", kernels[2]}}};
  DemandQuery query;
  for (const auto& name : names)
    query[name] = Footprint::all({h, w}).take_value();
  for (bool joint : {false, true}) {
    ExecutionContext execution(registry, {2, false, 64, 1048576, 65536});
    auto frozen =
        execution.freeze(compiled.value().plan, bindings).take_value();
    ExecutionOptions options;
    options.enable_joint = joint;
    auto result = execution.execute_fragments(frozen, query, {}, options);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    for (unsigned channel = 0; channel < 3; ++channel)
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < w; ++x) {
          float actual;
          PS_CHECK(result.value()
                       .values.at(names[channel])
                       .read({y, x}, &actual, sizeof(actual))
                       .ok());
          PS_CHECK(actual ==
                   convolution_oracle(image_data, h, w, channel,
                                      coefficients[channel], shapes[channel][0],
                                      shapes[channel][1], anchors[channel][0],
                                      anchors[channel][1], channel == 1, y, x));
        }
    auto dirty = result.value().dependencies.potential_dirty(
        "kg", Footprint::all({1, 3}).take_value());
    PS_CHECK(dirty.ok() && dirty.value().at("r").empty() &&
             dirty.value().at("b").empty());
    PS_CHECK(dirty.value().at("g") == query.at("g"));
    auto changed = bindings;
    changed.inputs[2].value = samples({1, 3}, {.25F, 1.F, .25F});
    auto next = execution.freeze(compiled.value().plan, changed).take_value();
    auto rerun = execution.execute_fragments(next, query, {}, options);
    if (!rerun.ok())
      std::cerr << rerun.status().message << '\n';
    PS_CHECK(rerun.ok() && rerun.value().diagnostics.cache_hits == 2 * h * w);
    for (const auto& timing : rerun.value().diagnostics.operation_timings)
      PS_CHECK(timing.output.output_index == 1);
    changed.inputs[2].value =
        samples({1, 3}, {0, std::numeric_limits<float>::quiet_NaN(), 0});
    auto unrelated =
        execution.freeze(compiled.value().plan, changed).take_value();
    PS_CHECK(
        execution.execute_fragments(unrelated, {{"r", query.at("r")}}).ok());
  }
  // A remote invalid field sample must not be read by a finite ROI.
  std::vector<float> field_data(h * w);
  for (std::size_t i = 0; i < field_data.size(); ++i)
    field_data[i] = image_data[i * 3];
  field_data.back() = std::numeric_limits<float>::quiet_NaN();
  auto field = samples({h, w}, field_data);
  WorkflowDocument field_doc;
  field_doc.inputs = {declaration(1, "field", field),
                      declaration(2, "kernel", kernels[0])};
  field_doc.nodes = {{1,
                      "field.convolve",
                      {WorkflowInputReference{1}, WorkflowInputReference{2}},
                      {{"anchor_y", std::int64_t{0}},
                       {"anchor_x", std::int64_t{1}},
                       {"boundary", std::string("zero")}}}};
  field_doc.outputs = {{"value", 1, "value"}};
  GraphContext field_graph(field_doc);
  auto field_plan = Compiler(registry).compile(field_graph).take_value().plan;
  ExecutionContext execution(registry);
  auto frozen =
      execution.freeze(field_plan, {{{"field", field}, {"kernel", kernels[0]}}})
          .take_value();
  auto roi =
      Footprint::from_regions({h, w}, {Region({{1, 1}, {1, 1}})}).take_value();
  auto result = execution.execute_fragments(frozen, {{"value", roi}});
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok());
  float actual;
  PS_CHECK(result.value()
               .values.at("value")
               .read({1, 1}, &actual, sizeof(actual))
               .ok());
  PS_CHECK(actual == convolution_oracle(image_data, h, w, 0, coefficients[0], 2,
                                        2, 0, 1, false, 1, 1));
  auto dirty = result.value().dependencies.potential_dirty(
      "field",
      Footprint::from_regions({h, w}, {Region({{2, 1}, {3, 1}})}).take_value());
  PS_CHECK(dirty.ok() && dirty.value().at("value").empty());
  return 0;
}
Result<Value> gaussian_kernel(
    const std::shared_ptr<OperationRegistry>& registry, double radius,
    double sigma) {
  const std::vector<Value> inputs;
  const std::vector<Region> demands;
  const std::map<std::string, ParameterValue> parameters{{"radius", radius},
                                                         {"sigma", sigma}};
  OperationInvocation invocation(inputs, demands, parameters);
  invocation.input_metadata = {{{ElementType::Float32, {1, 1, 3}},
                                {encode_semantic(rgb()).take_value()}}};
  invocation.output_index = 1;
  return registry->invoke("image.gaussian_blur_with_kernel", invocation);
}
int gaussian_parameters() {
  auto registry = make_default_operation_registry();
  const std::vector<double> radii{0,
                                  .25,
                                  1,
                                  1.25,
                                  2,
                                  64,
                                  std::nextafter(1., 0.),
                                  std::nextafter(1., 2.),
                                  std::nextafter(2., 1.),
                                  std::nextafter(2., 3.)};
  for (auto radius : radii) {
    const int extent = static_cast<int>(std::ceil(radius));
    const std::uint64_t side = 2 * extent + 1;
    auto result = gaussian_kernel(registry, radius, 1.3);
    if (!result.ok())
      std::cerr << "radius=" << radius << ": " << result.status().message
                << '\n';
    PS_CHECK(result.ok() && result.value().descriptor().shape ==
                                (std::vector<std::uint64_t>{side, side}));
    std::vector<long double> weights;
    long double normalizer = 0;
    for (int y = -extent; y <= extent; ++y)
      for (int x = -extent; x <= extent; ++x) {
        const auto factor = [&](int d) {
          return std::min(1.L, std::max(0.L, static_cast<long double>(radius) -
                                                 (std::abs(d) - 1)));
        };
        const long double sigma = 1.3;
        const auto weight =
            std::exp(-(static_cast<long double>(x * x + y * y)) /
                     (2 * sigma * sigma)) *
            factor(x) * factor(y);
        weights.push_back(weight);
        normalizer += weight;
      }
    double sum = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
      float coefficient;
      std::memcpy(&coefficient, result.value().bytes().data() + i * 4, 4);
      PS_CHECK(std::abs(static_cast<long double>(coefficient) -
                        weights[i] / normalizer) < 1e-7L);
      sum += coefficient;
    }
    PS_CHECK(std::abs(sum - 1) < 1e-6);
    if (radius == std::nextafter(1., 2.)) {
      float boundary;
      std::memcpy(&boundary, result.value().bytes().data() + 2 * 4, 4);
      PS_CHECK(boundary > 0);
    }
  }
  for (double sigma : {0., std::numeric_limits<double>::denorm_min()}) {
    auto impulse = gaussian_kernel(registry, 1.25, sigma);
    PS_CHECK(impulse.ok() && impulse.value().descriptor().shape ==
                                 (std::vector<std::uint64_t>{5, 5}));
    for (std::size_t i = 0; i < 25; ++i) {
      float coefficient;
      std::memcpy(&coefficient, impulse.value().bytes().data() + i * 4, 4);
      PS_CHECK(coefficient == (i == 12 ? 1 : 0));
    }
  }
  for (double invalid :
       {-1., std::nextafter(64., 65.), std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()}) {
    PS_CHECK(!gaussian_kernel(registry, invalid, 1).ok());
    PS_CHECK(!gaussian_kernel(registry, 1, invalid).ok());
  }
  return 0;
}
int gaussian_workflow() {
  auto registry = make_default_operation_registry();
  std::vector<float> pixels(2 * 3 * 3);
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = static_cast<float>(i) / 13;
  auto input =
      samples({2, 3, 3}, pixels, {encode_semantic(rgb()).take_value()});
  for (const std::string boundary : {"zero", "clamp"}) {
    WorkflowDocument doc;
    doc.inputs = {declaration(1, "image", input)};
    doc.nodes = {
        {1,
         "image.gaussian_blur_with_kernel",
         {WorkflowInputReference{1}},
         {{"radius", 1.25}, {"sigma", .9}, {"boundary", boundary}}},
        {2,
         "channel.extract",
         {WorkflowInputReference{1}},
         {{"index", std::int64_t{0}}}},
        {3,
         "field.convolve",
         {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{1, "kernel"}},
         {{"anchor_y", std::int64_t{2}},
          {"anchor_x", std::int64_t{2}},
          {"boundary", boundary}}}};
    doc.outputs = {{"image", 1, "image"},
                   {"kernel", 1, "kernel"},
                   {"recomputed", 3, "value"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    if (!compiled.ok())
      std::cerr << compiled.status().message << '\n';
    PS_CHECK(compiled.ok());
    for (bool joint : {false, true}) {
      ExecutionContext execution(registry, {2, false, 64, 4194304, 1048576});
      auto frozen =
          execution.freeze(compiled.value().plan, {{{"image", input}}})
              .take_value();
      ExecutionOptions options;
      options.enable_joint = joint;
      auto result = execution.execute_fragments(
          frozen,
          {{"image", Footprint::all({2, 3, 3}).take_value()},
           {"kernel", Footprint::all({5, 5}).take_value()},
           {"recomputed", Footprint::all({2, 3}).take_value()}},
          {}, options);
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      PS_CHECK((result.value().diagnostics.joint_groups > 0) == joint);
      for (std::uint64_t y = 0; y < 2; ++y)
        for (std::uint64_t x = 0; x < 3; ++x) {
          float image, field;
          PS_CHECK(result.value()
                       .values.at("image")
                       .read({y, x, 0}, &image, 4)
                       .ok());
          PS_CHECK(result.value()
                       .values.at("recomputed")
                       .read({y, x}, &field, 4)
                       .ok());
          PS_CHECK(image == field);
        }
      auto dirty = result.value().dependencies.potential_dirty(
          "image", Footprint::all({2, 3, 3}).take_value());
      PS_CHECK(dirty.ok() && dirty.value().at("kernel").empty());
    }
    doc.outputs = {{"kernel", 1, "kernel"}};
    GraphContext kernel_graph(doc);
    auto kernel_plan =
        Compiler(registry).compile(kernel_graph).take_value().plan;
    auto reads = std::make_shared<unsigned>(0);
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = input.descriptor();
    source->facets = input.facets();
    source->read = [reads](const Region&, std::uint8_t*, std::uint64_t,
                           const BufferAllocator&,
                           const CancellationToken&) -> Result<Region> {
      ++*reads;
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "image must not be read"});
    };
    ExecutionContext execution(registry);
    auto kernel = execution.execute(kernel_plan, {{{"image", {}, source}}});
    if (!kernel.ok())
      std::cerr << kernel.status().message << '\n';
    PS_CHECK(kernel.ok() && *reads == 0 &&
             kernel.value().values.at("kernel").descriptor().shape ==
                 (std::vector<std::uint64_t>{5, 5}));
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(ycbcr420() == 0);
  PS_CHECK(split_horizontal() == 0);
  PS_CHECK(channel_convolution() == 0);
  PS_CHECK(gaussian_parameters() == 0);
  PS_CHECK(gaussian_workflow() == 0);
  return 0;
}
