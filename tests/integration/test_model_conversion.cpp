#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11): project requires C++17.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Params = std::map<std::string, ParameterValue>;
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
std::uint64_t bits(double x, bool narrow) {
  std::uint64_t result = 0;
  if (narrow) {
    const float f = static_cast<float>(x);
    std::memcpy(&result, &f, 4);
  } else {
    std::memcpy(&result, &x, 8);
  }
  return result;
}
double number(std::uint64_t raw, bool narrow) {
  if (narrow) {
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
  }
  double f;
  std::memcpy(&f, &raw, 8);
  return f;
}
StridedLayout dense(const ValueDescriptor& d) {
  StridedLayout result;
  result.byte_strides.resize(d.shape.size());
  std::int64_t stride = Value::element_size(d.element_type);
  for (std::size_t a = d.shape.size(); a-- > 0;) {
    result.byte_strides[a] = stride;
    stride *= d.shape[a];
  }
  return result;
}
struct Fixture {
  ValueDescriptor descriptor;
  std::vector<std::uint64_t> raw;
  std::optional<TensorDescription> description;
  bool planar = false;
  std::optional<std::uint32_t> channel_axis;
  std::vector<Region> published;
};
Result<ExecutionResult> execute(const Fixture& f, const std::string& key,
                                Params params, std::optional<Region> roi = {},
                                ExecutionOptions execution_options = {}) {
  using Answer = Result<ExecutionResult>;
  std::vector<ValueFacet> facets;
  if (f.description) {
    auto encoded = encode_tensor_description(*f.description);
    if (!encoded.ok())
      return Answer(encoded.status());
    facets.push_back(encoded.take_value());
  }
  const auto width = Value::element_size(f.descriptor.element_type);
  std::vector<std::uint8_t> bytes(f.raw.size() * width);
  for (std::size_t i = 0; i < f.raw.size(); ++i)
    std::memcpy(bytes.data() + i * width, &f.raw[i], width);
  WorkflowDocument doc;
  ExecutionBindings bindings;
  if (f.planar) {
    PlanarImageConfig config;
    config.order = ImagePlaneOrder::Tiled;
    config.tile_height = config.tile_width = 128;
    config.height_axis = 0;
    config.width_axis = 1;
    config.channel_axis = f.channel_axis;
    auto made = PlanarImage::create(f.descriptor, config, facets);
    if (!made.ok())
      return Answer(made.status());
    auto image = made.take_value();
    auto source =
        take(Value::create(f.descriptor, Region::whole(f.descriptor.shape),
                           dense(f.descriptor), bytes, facets));
    const auto regions =
        f.published.empty()
            ? std::vector<Region>{Region::whole(f.descriptor.shape)}
            : f.published;
    for (const auto& region : regions) {
      std::vector<std::uint8_t> packed;
      auto fp = take(Footprint::from_regions(f.descriptor.shape, {region}));
      auto visited = fp.visit(
          [&](const auto& at) {
            auto address = source.byte_address(at);
            if (!address.ok())
              return address.status();
            packed.insert(packed.end(), bytes.begin() + address.value(),
                          bytes.begin() + address.value() + width);
            return Status::success();
          },
          UINT64_MAX);
      if (!visited.ok())
        return Answer(visited);
      auto status = image.publish(region, packed.data(), packed.size());
      if (!status.ok())
        return Answer(status);
    }
    doc.inputs = {
        {1,
         "source",
         f.descriptor,
         Region::whole(f.descriptor.shape),
         {},
         facets,
         PlanarImageLayout{config.order, 0, 1, f.channel_axis, 0, {}}}};
    ExecutionBinding binding;
    binding.name = "source";
    binding.image = std::make_shared<const PlanarImage>(image);
    bindings.inputs.push_back(std::move(binding));
  } else {
    auto value = Value::create(f.descriptor, Region::whole(f.descriptor.shape),
                               dense(f.descriptor), bytes, facets);
    if (!value.ok())
      return Answer(value.status());
    doc.inputs = {{1, "source", f.descriptor, Region::whole(f.descriptor.shape),
                   dense(f.descriptor), facets}};
    bindings.inputs.push_back({"source", value.take_value()});
  }
  doc.nodes = {{1, key, {WorkflowInputReference{1}}, std::move(params)}};
  doc.outputs = {{"result", 1, "values"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(doc);
  Compiler compiler(registry);
  PlanningOptions planning;
  planning.tile_height = planning.tile_width = 128;
  if (roi)
    planning.output_regions = {{"result", *roi}};
  auto compiled = compiler.compile(graph, planning);
  if (!compiled.ok())
    return Answer(compiled.status());
  ExecutionContextConfig context_config;
  context_config.cpu_workers = 1;
  ExecutionContext context(registry, context_config);
  if (!f.planar && key.find("color.color_to_gray_") == 0 &&
      (!doc.nodes[0].parameters.count("layout") ||
       std::get<std::string>(doc.nodes[0].parameters.at("layout")) !=
           "materialize")) {
    auto frozen = context.freeze(compiled.value().plan, bindings);
    if (!frozen.ok())
      return Answer(frozen.status());
    const auto& shape =
        compiled.value().plan.steps().back().output_descriptor.shape;
    const auto region = roi.value_or(Region::whole(shape));
    auto footprint = Footprint::from_regions(shape, {region});
    if (!footprint.ok())
      return Answer(footprint.status());
    auto result = context.execute_fragments(
        frozen.value(), {{"result", footprint.take_value()}}, {},
        execution_options);
    if (!result.ok())
      return Answer(result.status());
    // Q views must retain the original owner rather than a silent copy.
    for (const auto& fragment : result.value().values.at("result").fragments())
      if (fragment.storage() != bindings.inputs[0].value.storage())
        return Answer(
            Status{ErrorCode::Internal, "Q did not preserve the source owner"});
    auto collected =
        result.value().values.at("result").collect(region, BufferAllocator{});
    if (!collected.ok())
      return Answer(collected.status());
    ExecutionResult output;
    output.values.emplace("result", collected.take_value());
    output.diagnostics = result.value().diagnostics;
    return Answer(std::move(output));
  }
  return context.execute(compiled.value().plan, bindings, {},
                         execution_options);
}
std::uint64_t read(const ExecutionResult& result,
                   const std::vector<std::uint64_t>& at) {
  std::uint64_t bits = 0;
  if (result.images.count("result")) {
    const auto& image = result.images.at("result");
    std::vector<RegionDimension> dimensions;
    for (const auto c : at)
      dimensions.push_back({c, 1});
    std::vector<std::uint8_t> data(
        Value::element_size(image.descriptor().element_type));
    auto status = image.read(Region(dimensions), data.data(), data.size());
    if (!status.ok())
      throw std::runtime_error(status.message);
    std::memcpy(&bits, data.data(), data.size());
  } else {
    const auto& value = result.values.at("result");
    const auto address = take(value.byte_address(at));
    std::memcpy(&bits, value.bytes().data() + address,
                Value::element_size(value.descriptor().element_type));
  }
  return bits;
}
TensorDescription described(const std::string& model, std::uint32_t axis,
                            std::vector<std::uint64_t> slots = {0, 1, 2},
                            unsigned channels = 3) {
  TensorDescription d;
  d.channel_axis = axis;
  d.channels.resize(channels);
  TensorColorGroup group;
  group.name = "color";
  group.indices = slots;
  auto& i = group.interpretation;
  i.model = model;
  i.reference = "scene";
  i.association = "straight";
  i.white = std::array<double, 2>{.25, .25};
  i.coordinates = TensorModelCoordinates{};
  i.coordinates->scale = "relative";
  std::vector<std::string> roles;
  if (model == "xyz")
    roles = {"x", "y", "z"};
  else if (model == "xyy")
    roles = {"x", "y", "luminance"};
  else if (model == "cielab" || model == "oklab")
    roles = {"l", "a", "b"};
  else if (model == "cielch" || model == "oklch")
    roles = {"l", "c", "h"};
  else if (model == "rgb")
    roles = {"red", "green", "blue"};
  else if (model == "hsl")
    roles = {"hue", "saturation", "lightness"};
  else if (model == "hsv")
    roles = {"hue", "saturation", "value"};
  else if (model == "ycbcr")
    roles = {"y", "cb", "cr"};
  else
    roles = {"gray"};
  if (model == "oklab" || model == "oklch")
    i.white = std::array<double, 2>{.3127, .3290};
  if (model == "rgb" || model == "hsl" || model == "hsv" || model == "ycbcr") {
    i.primaries = "srgb";
    i.transfer = "srgb";
  }
  if (model == "ycbcr")
    i.coordinates->ncl_coefficients = std::array<double, 2>{.25, .25};
  if (model == "gray" || model == "black_white")
    i.coordinates->gray_kind = "linear_y";
  for (const auto& role : roles) {
    TensorChannelDescription c;
    c.role = c.name = role;
    c.unit = (role == "h" || role == "hue") ? "pi_multiple" : "1";
    group.components.push_back(c);
  }
  d.groups.push_back(std::move(group));
  return d;
}
Params raw(std::uint32_t axis, const std::string& components = "0,1,2") {
  return {{"metadata_mode", std::string("raw")},
          {"axis", static_cast<std::int64_t>(axis)},
          {"components", components}};
}
}  // namespace
int main() {
  try {
    for (const bool narrow : {false, true}) {
      const auto dtype = narrow ? ElementType::Float32 : ElementType::Float64;
      const auto one = bits(1, narrow), sign = UINT64_C(1)
                                               << (narrow ? 31 : 63);
      const auto snan =
          narrow ? UINT64_C(0x7f800123) : UINT64_C(0x7ff0000000000123);
      const std::array<const char*, 16> names{
          {"xyz_to_cielab", "cielab_to_xyz", "cielab_to_cielch",
           "cielch_to_cielab", "xyz_to_oklab", "oklab_to_xyz", "oklab_to_oklch",
           "oklch_to_oklab", "rgb_to_hsl", "hsl_to_rgb", "rgb_to_hsv",
           "hsv_to_rgb", "rgb_to_ycbcr_ncl", "ycbcr_ncl_to_rgb", "xyz_to_xyy",
           "xyy_to_xyz"}};
      const std::array<const char*, 16> models{
          {"xyz", "cielab", "cielab", "cielch", "xyz", "oklab", "oklab",
           "oklch", "rgb", "hsl", "rgb", "hsv", "rgb", "ycbcr", "xyz", "xyy"}};
      const std::array<std::array<double, 3>, 16> inputs{{{1, 1, 2},
                                                          {1, 0, 0},
                                                          {1, 3, 4},
                                                          {1, 2, .5},
                                                          {0, 0, 0},
                                                          {0, 0, 0},
                                                          {1, 3, 4},
                                                          {1, 2, .5},
                                                          {1, 0, 0},
                                                          {0, 1, .5},
                                                          {1, 0, 0},
                                                          {0, 1, 1},
                                                          {1, 0, 0},
                                                          {.25, 0, 0},
                                                          {1, 1, 2},
                                                          {.25, .25, 1}}};
      const std::array<std::array<double, 3>, 16> expected{{{1, 0, 0},
                                                            {1, 1, 2},
                                                            {1, 5, 0},
                                                            {1, 0, 2},
                                                            {0, 0, 0},
                                                            {0, 0, 0},
                                                            {1, 5, 0},
                                                            {1, 0, 2},
                                                            {0, 1, .5},
                                                            {1, 0, 0},
                                                            {0, 1, 1},
                                                            {1, 0, 0},
                                                            {.25, -1. / 6, .5},
                                                            {.25, .25, .25},
                                                            {.25, .25, 1},
                                                            {1, 1, 2}}};
      for (unsigned k = 0; k < names.size(); ++k) {
        Fixture f;
        f.descriptor = {dtype, {1, 3}};
        for (double x : inputs[k])
          f.raw.push_back(bits(x, narrow));
        Params p = raw(1);
        if (k < 2) {
          p["white_x"] = .25;
          p["white_y"] = .25;
        }
        if (k == 2 || k == 6 || k == 8 || k == 10)
          p["output_hue_unit"] = std::string("pi_multiple");
        if (k == 3 || k == 7 || k == 9 || k == 11)
          p["input_hue_unit"] = std::string("pi_multiple");
        if (k == 12 || k == 13) {
          p["kr"] = .25;
          p["kb"] = .25;
        }
        const auto key = std::string("color.") + names[k] + "_strict";
        auto result = execute(f, key, p);
        if (!result.ok())
          std::cerr << key << ": " << result.status().message << '\n';
        PS_CHECK(result.ok());
        for (unsigned c = 0; c < 3; ++c) {
          if ((k == 2 || k == 6) && c == 2)
            continue;  // independent atan2 oracle is in the numerical test.
          PS_CHECK(read(result.value(), {0, c}) ==
                   bits(expected[k][c], narrow));
        }
        // Exercise native semantic inference independently of raw selectors.
        f.description = described(models[k], 1);
        if (k == 4)
          f.description->groups[0].interpretation.white =
              std::array<double, 2>{.3127, .3290};
        p.erase("axis");
        p.erase("components");
        p.erase("metadata_mode");
        p.erase("white_x");
        p.erase("white_y");
        result = execute(f, key, p);
        if (!result.ok())
          std::cerr << "semantic " << key << ": " << result.status().message
                    << '\n';
        PS_CHECK(result.ok());
      }
      // Nonconsecutive canonical role slots, alpha/AOV preservation and exact
      // support: selected X/Z are signaling NaNs, but l and Gray only need Y.
      Fixture f;
      f.descriptor = {dtype, {1, 7}};
      f.raw = {snan, snan, sign, snan, bits(9, narrow), one, snan};
      f.description = described("xyz", 1, {1, 5, 3}, 7);
      f.description->reference = "scene";
      f.description->groups[0].alpha = 6;
      auto l = execute(f, "color.xyz_to_cielab_strict", {},
                       Region({{0, 1}, {1, 1}}));
      PS_CHECK(l.ok() && read(l.value(), {0, 1}) == one);
      auto q = execute(f, "color.color_to_gray_strict", {});
      if (!q.ok())
        std::cerr << q.status().message << '\n';
      PS_CHECK(q.ok());
      const std::array<unsigned, 5> mapping{0, 5, 2, 4, 6};
      for (unsigned c = 0; c < 5; ++c)
        PS_CHECK(read(q.value(), {0, c}) == f.raw[mapping[c]]);
      auto qd = take(decode_tensor_description(
          q.value().values.at("result").facets().back()));
      PS_CHECK(qd.groups[0].indices == std::vector<std::uint64_t>{1});
      PS_CHECK(qd.groups[0].alpha == 4);
      PS_CHECK(qd.groups[0].interpretation.coordinates->gray_kind ==
               "linear_y");
      PS_CHECK(qd.channels[0].interpretation &&
               qd.channels[0].interpretation->reference == "scene");
      auto force = execute(f, "color.color_to_gray_strict",
                           {{"layout", std::string("view")}});
      PS_CHECK(force.ok());
      // Overlap is legal source metadata but cannot survive conversion of one
      // group.
      auto conflicting = f;
      auto group = conflicting.description->groups[0];
      group.name = "also_color";
      conflicting.description->groups.push_back(group);
      auto rejected =
          execute(conflicting, "color.xyz_to_cielab_strict",
                  {{"group", std::string("color")}}, Region({{0, 1}, {0, 1}}));
      PS_CHECK(!rejected.ok() &&
               rejected.status().code == ErrorCode::InvalidArgument);
      // Override changes only this invocation, not f or its other consumer.
      auto override = *f.description;
      override.groups[0].interpretation.white =
          std::array<double, 2>{.3127, .3290};
      auto overridden = execute(
          f, "color.xyz_to_cielab_strict",
          {{"metadata_mode", std::string("override")},
           {"metadata_override", take(tensor_description_parameter(override))}},
          Region({{0, 1}, {1, 1}}));
      PS_CHECK(overridden.ok());
      PS_CHECK((*f.description->groups[0].interpretation.white)[0] == .25);
      // Structural Gray expansion and typed threshold/select paths.
      Fixture gray;
      gray.descriptor = {dtype, {1, 3}};
      gray.raw = {snan, bits(.75, narrow), sign};
      gray.description = described("gray", 1, {1}, 3);
      auto r = execute(gray, "color.gray_to_color_strict", {});
      if (!r.ok())
        std::cerr << "R: " << r.status().message << '\n';
      PS_CHECK(r.ok());
      PS_CHECK(read(r.value(), {0, 0}) == snan &&
               read(r.value(), {0, 4}) == sign);
      PS_CHECK(read(r.value(), {0, 1}) == bits(.75, narrow));
      PS_CHECK(read(r.value(), {0, 2}) == bits(.75, narrow));
      PS_CHECK(read(r.value(), {0, 3}) == bits(1.5, narrow));
      auto threshold =
          execute(gray, "mask.threshold_channel_strict", {{"threshold", .5}});
      PS_CHECK(threshold.ok() && read(threshold.value(), {0, 1}) == one);
      Fixture binary = gray;
      binary.raw[1] = one;
      binary.description = described("black_white", 1, {1}, 3);
      Params levels{{"black_value", format::model_detail::constant_parameter(
                                        narrow ? format::ModelConstant(-0.f)
                                               : format::ModelConstant(-0.))},
                    {"white_value", format::model_detail::constant_parameter(
                                        narrow ? format::ModelConstant(.25f)
                                               : format::ModelConstant(.25))}};
      auto t = execute(binary, "color.black_white_to_gray_strict", levels);
      PS_CHECK(t.ok() && read(t.value(), {0, 1}) == bits(.25, narrow));
      // Binary samples are dimensionless even when their retained Gray origin
      // is absolute luminance. T restores the source-origin Gray unit.
      auto absolute = gray;
      absolute.description->groups[0].interpretation.coordinates->scale =
          "absolute";
      absolute.description->groups[0].components[0].unit = "cd/m2";
      auto absolute_binary = execute(absolute, "mask.threshold_channel_strict",
                                     {{"threshold", .5}});
      PS_CHECK(absolute_binary.ok());
      auto bd = take(decode_tensor_description(
          absolute_binary.value().values.at("result").facets().back()));
      PS_CHECK(bd.groups[0].components[0].unit == "1");
      PS_CHECK(bd.groups[0].interpretation.coordinates->scale == "absolute");
      absolute.description = bd;
      absolute.raw[1] = one;
      auto absolute_gray =
          execute(absolute, "color.black_white_to_gray_strict", levels);
      PS_CHECK(absolute_gray.ok());
      auto gd = take(decode_tensor_description(
          absolute_gray.value().values.at("result").facets().back()));
      PS_CHECK(gd.groups[0].components[0].unit == "cd/m2");
      binary.raw[1] = bits(.5, narrow);
      PS_CHECK(
          !execute(binary, "color.black_white_to_gray_strict", levels).ok());
      // Axis-free R inserts a real channel axis; zeros need no source samples.
      Fixture field;
      field.descriptor = {dtype, {2}};
      field.raw = {bits(.25, narrow), bits(.5, narrow)};
      Params expansion{{"metadata_mode", std::string("raw")},
                       {"axis_free", true},
                       {"output_axis", std::int64_t{1}},
                       {"gray_kind", std::string("linear_y")},
                       {"gray_white_x", .25},
                       {"gray_white_y", .25}};
      auto expanded = execute(field, "color.gray_to_color_strict", expansion);
      PS_CHECK(expanded.ok() && read(expanded.value(), {1, 2}) == one);
      expansion["gray_kind"] = std::string("cielab_l");
      expansion.erase("gray_white_x");
      expansion.erase("gray_white_y");
      field.raw = {snan, snan};
      auto zeros = execute(field, "color.gray_to_color_strict", expansion,
                           Region({{0, 2}, {1, 2}}));
      PS_CHECK(zeros.ok() && read(zeros.value(), {1, 2}) == 0);
      // Invalid statics are checked even for a bypass-only observation.
      auto bad = raw(1);
      bad["white_x"] = .75;
      bad["white_y"] = .25;
      PS_CHECK(!execute(f, "color.xyz_to_cielab_strict", bad,
                        Region({{0, 1}, {6, 1}}))
                    .ok());
      bad = raw(1);
      bad["output_hue_unit"] = std::string("degrees");
      PS_CHECK(
          !execute(f, "color.rgb_to_hsl_strict", bad, Region({{0, 1}, {6, 1}}))
               .ok());
      bad = raw(1);
      bad["layout"] = std::string("view");
      PS_CHECK(
          !execute(f, "color.xyz_to_xyy_strict", bad, Region({{0, 1}, {6, 1}}))
               .ok());
      // Cross-tile sparse image: only the mathematically required Y plane and
      // exact spatial ROI are published; X/Z/alpha pages are unavailable.
      Fixture image;
      image.planar = true;
      image.channel_axis = 2;
      image.descriptor = {dtype, {3, 133, 7}};
      image.raw.assign(3 * 133 * 7, snan);
      for (unsigned y = 1; y < 3; ++y)
        for (unsigned x = 126; x < 133; ++x)
          image.raw[(y * 133 + x) * 7 + 5] = one;
      image.description = described("xyz", 2, {1, 5, 3}, 7);
      image.published = {Region({{1, 2}, {126, 7}, {5, 1}})};
      auto sparse = execute(image, "color.xyz_to_cielab_strict", {},
                            Region({{1, 2}, {126, 7}, {1, 1}}));
      if (!sparse.ok())
        std::cerr << "sparse planar: " << sparse.status().message << '\n';
      PS_CHECK(sparse.ok() && read(sparse.value(), {2, 132, 1}) == one);
      // Forced planar Q views are not representable by the current image
      // contract. Fail explicitly rather than silently materializing a copy.
      auto image_view = execute(image, "color.color_to_gray_strict",
                                {{"layout", std::string("view")}},
                                Region({{1, 2}, {126, 7}, {1, 1}}));
      PS_CHECK(!image_view.ok() &&
               image_view.status().code == ErrorCode::InvalidArgument);
      // Planar matrix blocks include 64-lane boundaries, vector tails and tile
      // edges.
      image.description.reset();
      image.descriptor = {dtype, {2, 133, 3}};
      image.raw.resize(2 * 133 * 3);
      image.published.clear();
      for (std::size_t i = 0; i < image.raw.size(); i += 3) {
        image.raw[i] = one;
        image.raw[i + 1] = 0;
        image.raw[i + 2] = 0;
      }
      auto matrix = raw(2);
      matrix["kr"] = .25;
      matrix["kb"] = .25;
      for (const auto* profile :
           {"strict", "accelerated_x86_64", "accelerated_apple_silicon"}) {
        auto result = execute(
            image, std::string("color.rgb_to_ycbcr_ncl_") + profile, matrix);
        if (!result.ok() &&
            result.status().code == ErrorCode::BackendUnavailable)
          continue;
        if (!result.ok())
          std::cerr << profile << ": " << result.status().message << '\n';
        PS_CHECK(result.ok());
        PS_CHECK(read(result.value(), {1, 132, 0}) == bits(.25, narrow));
        PS_CHECK(std::abs(number(read(result.value(), {1, 132, 1}), narrow) +
                          1. / 6) < 1e-6);
      }
      // Codec v4 stays byte-compatible; v5 survives canonical hex round-trips.
      TensorDescription old;
      old.model = "gray";
      PS_CHECK(take(encode_tensor_description(old)).version == 4);
      auto facet = take(encode_tensor_description(*gray.description));
      PS_CHECK(facet.version == 5);
      auto roundtrip = take(tensor_description_from_parameter(
          take(tensor_description_parameter(*gray.description))));
      PS_CHECK(take(encode_tensor_description(roundtrip)).payload ==
               facet.payload);
    }
    // Disjoint demand, exact forward dirty relation, Empty and cancellation.
    {
      ValueDescriptor d{ElementType::Float32, {3, 3}};
      const std::array<float, 9> samples{NAN, 1, NAN, NAN, 2, NAN, NAN, 3, NAN};
      std::vector<std::uint8_t> data(sizeof(samples));
      std::memcpy(data.data(), samples.data(), data.size());
      auto source = take(
          Value::create(d, Region::whole(d.shape), dense(d), std::move(data)));
      auto p = raw(1);
      p["white_x"] = .25;
      p["white_y"] = .25;
      WorkflowDocument doc;
      doc.inputs = {{1, "source", d, Region::whole(d.shape), dense(d), {}}};
      doc.nodes = {
          {1, "color.xyz_to_cielab_strict", {WorkflowInputReference{1}}, p}};
      doc.outputs = {{"result", 1, "values"}};
      auto registry = make_default_operation_registry();
      GraphContext graph(doc);
      auto compiled = take(Compiler(registry).compile(graph));
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      ExecutionContext context(registry, config);
      auto frozen = take(context.freeze(compiled.plan, {{{"source", source}}}));
      auto q = take(Footprint::from_regions(
          d.shape, {Region({{0, 1}, {0, 1}}), Region({{2, 1}, {0, 1}})}));
      auto answer = context.execute_fragments(frozen, {{"result", q}});
      if (!answer.ok())
        std::cerr << "disjoint: " << answer.status().message << '\n';
      PS_CHECK(answer.ok() &&
               answer.value().values.at("result").coverage() == q);
      float value = 0;
      PS_CHECK(
          answer.value().values.at("result").read({0, 0}, &value, 4).ok() &&
          value == 1);
      PS_CHECK(
          !answer.value().values.at("result").read({1, 0}, &value, 4).ok());
      auto unrelated =
          take(Footprint::from_regions(d.shape, {Region({{0, 1}, {0, 1}})}));
      auto dirty = take(
          answer.value().dependencies.potential_dirty("source", unrelated));
      PS_CHECK(dirty.at("result").empty());
      auto luminance =
          take(Footprint::from_regions(d.shape, {Region({{0, 1}, {1, 1}})}));
      dirty = take(
          answer.value().dependencies.potential_dirty("source", luminance));
      PS_CHECK(dirty.at("result") == unrelated);
      auto empty = context.execute_fragments(
          frozen, {{"result", take(Footprint::none(d.shape))}});
      PS_CHECK(empty.ok() &&
               empty.value().values.at("result").coverage().empty());
      CancellationSource cancellation;
      PS_CHECK(cancellation.cancel());
      auto stopped = context.execute_fragments(frozen, {{"result", q}},
                                               cancellation.token());
      PS_CHECK(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled);
      ExecutionOptions limited;
      limited.maximum_dependency_work = 1;
      auto exhausted =
          context.execute_fragments(frozen, {{"result", q}}, {}, limited);
      PS_CHECK(!exhausted.ok() &&
               exhausted.status().code == ErrorCode::ResourceExhausted);
      // Compile-time rejection also applies when no samples are requested.
      doc.nodes[0].parameters["white_x"] = .75;
      GraphContext invalid(doc);
      PlanningOptions empty_plan;
      empty_plan.output_regions = {{"result", Region({{0, 0}, {0, 0}})}};
      PS_CHECK(!Compiler(registry).compile(invalid, empty_plan).ok());
    }
    // S is a graph authoring helper over an admitted MASK constituent, not a
    // new color key or the legacy Whole threshold operation.
    WorkflowDocument doc;
    format::ModelConversionOptions options;
    options.metadata_mode = "raw";
    options.axis = 1;
    options.components = {0};
    options.threshold = .5;
    auto edge =
        format::gray_to_black_white(doc, WorkflowInputReference{1}, options);
    PS_CHECK(edge.ok() && doc.nodes.size() == 1 &&
             doc.nodes[0].operation == "mask.threshold_channel_strict");
    std::cout << "FMT-11 graph, metadata, Gray topology, views, sparse planar "
                 "and profile tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << "integration exception: " << error.what() << '\n';
    return 1;
  }
}
