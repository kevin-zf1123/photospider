#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/bezier.hpp"
#include "photospider/numeric/calculus.hpp"
#include "photospider/numeric/color_ramps.hpp"
#include "photospider/numeric/indexing.hpp"
#include "photospider/numeric/layouts.hpp"
#include "photospider/numeric/lut1d.hpp"
#include "photospider/numeric/lut3d_baking.hpp"
#include "photospider/numeric/matrix.hpp"
#include "photospider/numeric/ordering.hpp"
#include "photospider/numeric/scans.hpp"
#include "photospider/numeric/sequences.hpp"
#include "photospider/numeric/shapers.hpp"
#include "photospider/photospider.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
std::uint64_t raw(double value) {
  std::uint64_t word;
  std::memcpy(&word, &value, 8);
  return word;
}
ps::Value array(ps::ElementType type, const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& words) {
  const auto width = ps::Value::element_size(type);
  auto storage = take(ps::BufferAllocator{}.allocate(words.size() * width));
  for (std::size_t i = 0; i < words.size(); ++i)
    std::memcpy(storage.data() + i * width, &words[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = width;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                      {0, strides},
                                      std::move(storage).freeze()));
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& values) {
  std::vector<std::uint64_t> words;
  for (auto value : values)
    words.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, words);
}
struct Case {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  ps::ResourceBindings resources;
  std::string cluster, operation;
  std::vector<std::uint64_t> shape, expected;
  ps::ElementType dtype = ps::ElementType::Float64;
  ps::WorkflowInput add(const ps::Value& value) {
    const auto id = document.inputs.size() + 1;
    const auto name = "input" + std::to_string(id);
    document.inputs.push_back({id, name, value.descriptor(), value.region(),
                               value.layout(), value.facets()});
    bindings.inputs.push_back({name, value});
    return ps::WorkflowInputReference{id};
  }
  void node(ps::WorkflowNode value) {
    operation = value.operation;
    document.outputs = {{"result", value.id, "values"}};
    document.nodes.push_back(std::move(value));
  }
};
Case fixture(unsigned operation, std::uint64_t n,
             ps::CpuNumericProfile profile) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  Case c;
  const char* clusters[] = {"NUM-02", "NUM-03", "NUM-06", "NUM-07", "NUM-08",
                            "NUM-09", "NUM-10", "NUM-11", "NUM-12", "NUM-13",
                            "NUM-14", "NUM-15", "CRV-03", "CRV-05", "CRV-06",
                            "CRV-07", "CRV-08", "CRV-09"};
  c.cluster = clusters[operation];
  c.shape = {n};
  const auto values = [&](std::uint64_t count, double value) {
    return c.add(doubles({count}, std::vector<double>(count, value)));
  };
  const auto expect = [&](std::uint64_t count, double value) {
    c.expected.assign(count, raw(value));
  };
  const std::string suffix =
      profile == ps::CpuNumericProfile::Strict ? "_strict"
      : profile == ps::CpuNumericProfile::AppleSiliconNeon
          ? "_accelerated_apple_silicon"
          : "_accelerated_x86_64";
  switch (operation) {
    case 0: {
      auto start = values(1, 0), end = values(1, n - 1);
      c.node(take(linspace_node(1, {start, {ps::ElementType::Float64, {1}}},
                                {end, {ps::ElementType::Float64, {1}}}, n,
                                ps::ElementType::Float64, profile)));
      for (std::uint64_t i = 0; i < n; ++i)
        c.expected.push_back(raw(i));
      break;
    }
    case 1:
      c.node(take(
          constant_node(1, values(1, 1.5), {n}, ArrayLayout::Dense, profile)));
      expect(n, 1.5);
      break;
    case 2: {
      auto x = values(n, .25), lo = values(n, 0), hi = values(n, 1);
      auto out_lo = values(n, -2), out_hi = values(n, 2);
      c.node(
          {1, "numeric.remap_range" + suffix, {x, lo, hi, out_lo, out_hi}, {}});
      expect(n, -1);
      break;
    }
    case 3: {
      auto a = values(n, 1), b = values(n, 1.125);
      c.node({1,
              "numeric.is_close" + suffix,
              {a, b},
              {{"atol", .125}, {"rtol", 0.}}});
      c.dtype = ps::ElementType::UInt8;
      c.expected.assign(n, 1);
      break;
    }
    case 4: {
      auto x = values(n, .5), lo = values(n, 0), hi = values(n, 1);
      c.node({1, "numeric.smoothstep" + suffix, {x, lo, hi}, {}});
      expect(n, .5);
      break;
    }
    case 5: {
      std::vector<double> input;
      for (std::uint64_t i = 0; i < 2 * n; ++i)
        input.push_back(i);
      c.node(take(transpose_node(1, c.add(doubles({n, 2}, input)), {1, 0},
                                 TransformLayout::Dense, profile)));
      c.shape = {2, n};
      for (unsigned j = 0; j < 2; ++j)
        for (std::uint64_t i = 0; i < n; ++i)
          c.expected.push_back(raw(2 * i + j));
      break;
    }
    case 6: {
      std::vector<double> input;
      std::vector<std::uint64_t> indices;
      for (std::uint64_t i = 0; i < n; ++i) {
        input.push_back(i);
        indices.push_back(n - 1 - i);
        c.expected.push_back(raw(n - 1 - i));
      }
      auto data = c.add(doubles({n}, input));
      auto index = c.add(array(ps::ElementType::Int64, {n}, indices));
      c.node(take(gather_node(1, data, index, 0, profile)));
      break;
    }
    case 7:
      c.node(take(reduce_sum_node(1, values(n, 1), {0},
                                  ps::ElementType::Float64, {}, profile)));
      c.shape = {1};
      expect(1, n);
      break;
    case 8: {
      std::vector<double> input;
      for (std::uint64_t i = 0; i < n; ++i) {
        input.push_back(n - 1 - i);
        c.expected.push_back(raw(i));
      }
      c.node(take(sort_node(1, c.add(doubles({n}, input)), 0, profile)));
      break;
    }
    case 9:
      c.node(take(prefix_sum_node(1, values(n, 1), 0, ps::ElementType::Float64,
                                  {}, profile)));
      c.shape = {n + 1};
      for (std::uint64_t i = 0; i <= n; ++i)
        c.expected.push_back(raw(i));
      break;
    case 10: {
      auto input = c.add(doubles({n, 2}, std::vector<double>(n * 2, 2)));
      auto matrix = c.add(doubles({2, 2}, {1, 0, 0, 1}));
      auto bias = c.add(doubles({2}, {1, 1}));
      c.node(take(matrix_transform_node(1, input, matrix, bias, profile)));
      c.shape = {n, 2};
      expect(n * 2, 3);
      break;
    }
    case 11: {
      // Integrating unit samples on a unit grid is exactly the sample index.
      auto input = values(n + 1, 1), step = values(1, 1),
           initial = values(1, 0);
      c.node(take(integrate_1d_node(1, input, step, initial, profile)));
      c.shape = {n + 1};
      for (std::uint64_t i = 0; i <= n; ++i)
        c.expected.push_back(raw(i));
      break;
    }
    case 12: {
      auto anchors = c.add(doubles({2, 2}, {0, 0, 2, 0}));
      auto handles = c.add(doubles({1, 1, 2}, {1, 2}));
      auto indices = c.add(
          array(ps::ElementType::Int64, {n}, std::vector<std::uint64_t>(n, 0)));
      auto t = values(n, .5);
      c.node(take(evaluate_bezier_node(1, anchors, handles, indices, t, 2,
                                       ps::ElementType::Float64, profile)));
      c.shape = {n, 2};
      expect(n * 2, 1);
      break;
    }
    case 13: {
      auto input = values(n, .25), table = c.add(doubles({2}, {0, 2}));
      auto axis = c.add(doubles({3}, {0, 1, 1}));
      c.node(
          take(apply_lut1d_node(1, input, table, axis, ps::ElementType::Float64,
                                {}, CurveDomain::Reject, profile)));
      expect(n, .5);
      break;
    }
    case 14: {
      auto input = values(n, .5), stops = c.add(doubles({2}, {0, 1}));
      auto colors = c.add(doubles({2, 3}, {0, 0, 0, 1, 1, 1}));
      auto description = color_ramp_rgb_description();
      description.transfer =
          ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
      RgbRampOptions options;
      options.profile = profile;
      c.node(take(color_ramp_rgb_node(1, input, stops, colors,
                                      ps::ElementType::Float64, description,
                                      options)));
      c.shape = {n, 3};
      expect(n * 3, .5);
      break;
    }
    case 15: {
      std::vector<double> input, table;
      for (std::uint64_t i = 0; i < n; ++i)
        for (double value : {.25, .5, .75}) {
          input.push_back(value);
          c.expected.push_back(raw(value));
        }
      for (unsigned r = 0; r < 2; ++r)
        for (unsigned g = 0; g < 2; ++g)
          for (unsigned b = 0; b < 2; ++b)
            for (unsigned value : {r, g, b})
              table.push_back(value);
      auto colors = c.add(doubles({n, 3}, input));
      auto lut = c.add(doubles({2, 2, 2, 3}, table));
      auto axis = c.add(doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 1, 1}));
      Lut3dOptions options;
      options.profile = profile;
      c.node(take(apply_lut3d_trilinear_node(
          1, colors, lut, axis, ps::ElementType::Float64, {}, {}, options)));
      c.shape = {n, 3};
      break;
    }
    case 16: {
      auto input = values(n, 4), lo = values(1, 1), hi = values(1, 16);
      c.node(take(log2_shaper_node(1, input, lo, hi, profile)));
      expect(n, .5);
      break;
    }
    case 17: {
      const std::uint64_t side = n == 1 ? 2 : 5;
      const double step = 1. / (side - 1);
      auto axis = c.add(doubles({3, 3}, {0, 1, step, 0, 1, step, 0, 1, step}));
      Lut3dBakeOptions options;
      options.shape = {side, side, side};
      options.interpolation = ps::Lut3dInterpolation::Trilinear;
      options.atol = options.rtol = 0;
      options.source_pointwise = true;
      options.profile = profile;
      auto baked = take(bake_lut3d(
          c.document, c.registry, axis,
          [](ps::WorkflowDocument&, const Lut3dSourceInput& source) {
            return ps::Result<ps::WorkflowNodeOutput>(source.colors);
          },
          options));
      c.document.outputs = {
          {"result", baked.table.source_node, baked.table.source_port}};
      c.operation = "bake_lut3d_identity";
      c.shape = {side, side, side, 3};
      for (std::uint64_t r = 0; r < side; ++r)
        for (std::uint64_t g = 0; g < side; ++g)
          for (std::uint64_t b = 0; b < side; ++b)
            for (auto value : {r, g, b})
              c.expected.push_back(raw(value * step));
      break;
    }
  }
  return c;
}
Case extended_fixture(const std::string& key, std::uint64_t n,
                      ps::CpuNumericProfile profile) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  using T = ps::ElementType;
  Case c;
  c.cluster = "per-key";
  c.shape = {n};
  const std::string suffix =
      profile == ps::CpuNumericProfile::Strict ? "_strict"
      : profile == ps::CpuNumericProfile::AppleSiliconNeon
          ? "_accelerated_apple_silicon"
          : "_accelerated_x86_64";
  const auto values = [&](std::uint64_t count, double value) {
    return c.add(doubles({count}, std::vector<double>(count, value)));
  };
  const auto expect = [&](std::uint64_t count, double value) {
    c.expected.assign(count, raw(value));
  };
  if (key == "numeric.arange") {
    auto start = values(1, 0), step = values(1, 1);
    c.node(
        take(arange_node(1, {start, {T::Float64, {1}}},
                         {step, {T::Float64, {1}}}, n, T::Float64, profile)));
    for (std::uint64_t i = 0; i < n; ++i)
      c.expected.push_back(raw(i));
  } else if (key == "numeric.broadcast") {
    c.node(take(broadcast_node(1, values(1, 1.5), {n}, {0}, ArrayLayout::Dense,
                               profile)));
    expect(n, 1.5);
  } else if (key == "numeric.clamp") {
    auto x = values(n, 2), lo = values(n, 0), hi = values(n, 1);
    c.node({1, key + suffix, {x, lo, hi}, {}});
    expect(n, 1);
  } else if (key == "numeric.mix") {
    auto a = values(n, 10), b = values(n, 20), t = values(n, .25);
    c.node({1, key + suffix, {a, b, t}, {}});
    expect(n, 12.5);
  } else if (key == "numeric.select") {
    auto condition =
        c.add(array(T::UInt8, {n}, std::vector<std::uint64_t>(n, 1)));
    auto a = values(n, 2), b = values(n, 3);
    c.node({1, key + suffix, {condition, a, b}, {}});
    expect(n, 2);
  } else if (key == "numeric.equal" || key == "numeric.not_equal" ||
             key == "numeric.less" || key == "numeric.less_equal" ||
             key == "numeric.greater" || key == "numeric.greater_equal") {
    auto a = values(n, 1), b = values(n, 2);
    c.node({1, key + suffix, {a, b}, {}});
    c.dtype = T::UInt8;
    c.expected.assign(n, key == "numeric.not_equal" || key == "numeric.less" ||
                             key == "numeric.less_equal");
  } else if (key == "array.reshape") {
    auto data = c.add(doubles({n, 2}, std::vector<double>(n * 2, 1.5)));
    c.shape = {2 * n};
    c.node(
        take(reshape_node(1, data, c.shape, TransformLayout::Dense, profile)));
    expect(2 * n, 1.5);
  } else if (key == "array.slice") {
    auto data = values(n + 2, 1.5);
    auto start = c.add(array(T::Int64, {1}, {1})),
         step = c.add(array(T::Int64, {1}, {1}));
    c.node(take(slice_node(1, data, start, step, {n}, TransformLayout::Dense,
                           profile)));
    expect(n, 1.5);
  } else if (key == "array.concatenate") {
    auto a = values(n, 1), b = values(n, 2);
    c.node(take(concatenate_node(1, {a, b}, 0, ArrayLayout::Dense, profile)));
    c.shape = {2 * n};
    expect(n, 1);
    c.expected.insert(c.expected.end(), n, raw(2));
  } else if (key.find("array.scatter_") == 0) {
    auto base = values(n, 1);
    std::vector<std::uint64_t> indices;
    for (std::uint64_t i = 0; i < n; ++i) {
      indices.push_back(i);
      indices.push_back(i);
    }
    auto index = c.add(array(T::Int64, {2 * n}, indices)),
         updates = values(2 * n, 2);
    c.node({1,
            key + suffix,
            {base, index, updates},
            {{"axis", static_cast<std::int64_t>(0)}}});
    expect(n, key == "array.scatter_sum"       ? 5
              : key == "array.scatter_minimum" ? 1
                                               : 2);
  } else if (key.find("numeric.reduce_") == 0) {
    std::vector<double> data;
    for (std::uint64_t i = 0; i < n; ++i) {
      data.push_back(0);
      data.push_back(2);
    }
    ps::WorkflowNode node{1,
                          key + suffix,
                          {c.add(doubles({2 * n}, data))},
                          {{"axes", std::string("0")}}};
    if (key == "numeric.reduce_mean" || key == "numeric.reduce_variance" ||
        key == "numeric.reduce_std")
      node.parameters["dtype"] = std::string("float64");
    if (key == "numeric.reduce_variance" || key == "numeric.reduce_std")
      node.parameters["ddof"] = static_cast<std::int64_t>(0);
    c.node(std::move(node));
    c.shape = {1};
    if (key == "numeric.reduce_count") {
      c.dtype = T::Int64;
      c.expected = {2 * n};
    } else {
      expect(1, key == "numeric.reduce_minimum"   ? 0
                : key == "numeric.reduce_maximum" ? 2
                                                  : 1);
    }
  } else if (key == "numeric.quantile") {
    std::vector<double> data;
    for (std::uint64_t i = 0; i < n; ++i) {
      data.push_back(0);
      data.push_back(2);
    }
    auto input = c.add(doubles({2 * n}, data)), q = values(1, .5);
    c.node(take(quantile_node(1, input, q, 0, T::Float64, profile)));
    c.shape = {1};
    expect(1, 1);
  } else if (key == "numeric.integral_image") {
    auto data = c.add(doubles({n, 2}, std::vector<double>(n * 2, 1)));
    c.node(take(integral_image_node(1, data, {0, 1}, T::Float64, {}, profile)));
    c.shape = {n + 1, 3};
    for (std::uint64_t i = 0; i <= n; ++i)
      for (unsigned j = 0; j < 3; ++j)
        c.expected.push_back(raw(i * j));
  } else if (key == "numeric.derivative_1d") {
    std::vector<double> data;
    for (std::uint64_t i = 0; i < n + 1; ++i)
      data.push_back(i);
    auto input = c.add(doubles({n + 1}, data)), step = values(1, 1);
    c.node(take(derivative_1d_node(1, input, step, profile)));
    c.shape = {n + 1};
    expect(n + 1, 1);
  } else if (key.find("curve.interpolate_") == 0) {
    const bool multi = key.find("_multi") != std::string::npos;
    auto x = c.add(doubles({3}, {0, 1, 2}));
    auto y = c.add(doubles(multi ? std::vector<std::uint64_t>{3, 2}
                                 : std::vector<std::uint64_t>{3},
                           multi ? std::vector<double>{0, 0, 1, 2, 2, 4}
                                 : std::vector<double>{0, 1, 2}));
    auto q = values(n, .5);
    c.node({1,
            key + suffix,
            {x, y, q},
            {{"dtype", std::string("float64")},
             {"out_of_domain", std::string("reject")}}});
    if (multi) {
      c.shape = {n, 2};
      for (std::uint64_t i = 0; i < n; ++i) {
        c.expected.push_back(raw(.5));
        c.expected.push_back(raw(1));
      }
    } else {
      expect(n, .5);
    }
  } else if (key == "curve.sample_bezier_function") {
    auto a = c.add(doubles({2, 2}, {0, 0, 1, 1})),
         h = c.add(doubles({1, 1, 2}, {.5, .5}));
    auto start = values(1, 0), end = values(1, 1);
    c.node(take(sample_bezier_function_node(1, a, h, start, end, 2, n + 1,
                                            T::Float64, BezierDomain::Reject,
                                            profile)));
    c.shape = {n + 1};
    for (std::uint64_t i = 0; i <= n; ++i)
      c.expected.push_back(raw(static_cast<double>(i) / n));
  } else if (key == "curve.apply_lut1d_channels") {
    auto input = c.add(doubles({n, 2}, std::vector<double>(2 * n, .25))),
         table = c.add(doubles({2, 2}, {0, 0, 2, 4}));
    auto axis = c.add(doubles({3}, {0, 1, 1}));
    c.node(take(apply_lut1d_channels_node(1, input, table, axis, T::Float64, {},
                                          CurveDomain::Reject, profile)));
    c.shape = {n, 2};
    for (std::uint64_t i = 0; i < n; ++i) {
      c.expected.push_back(raw(.5));
      c.expected.push_back(raw(1));
    }
  } else if (key == "curve.apply_lut3d_tetrahedral") {
    c = fixture(15, n, profile);
    c.document.nodes[0].operation = key + suffix;
    c.operation = key + suffix;
    c.cluster = "per-key";
  } else if (key == "curve.log2_shaper_inverse") {
    auto input = values(n, .5), lo = values(1, 1), hi = values(1, 16);
    c.node(take(log2_shaper_inverse_node(1, input, lo, hi, profile)));
    expect(n, 4);
  } else if (key.find("curve.color_ramp_") == 0) {
    auto model = key.substr(17);
    unsigned unit = 0;
    if (model.find("_rational_pi") != std::string::npos) {
      unit = 2;
      model.resize(model.size() - 12);
    } else if (model.find("_pi") != std::string::npos) {
      unit = 1;
      model.resize(model.size() - 3);
    }
    const std::map<std::string, ps::ColorModel> models{
        {"cielab", ps::ColorModel::Cielab}, {"cielch", ps::ColorModel::Cielch},
        {"cmyk", ps::ColorModel::Cmyk},     {"hsl", ps::ColorModel::Hsl},
        {"oklab", ps::ColorModel::Oklab},   {"oklch", ps::ColorModel::Oklch},
        {"xyz", ps::ColorModel::Xyz},       {"ycbcr", ps::ColorModel::Ycbcr}};
    ps::ColorArrayDescriptor description;
    description.model = models.at(model);
    const bool polar = model == "cielch" || model == "oklch" || model == "hsl";
    if (model == "cielab" || model == "cielch")
      description.white = ps::color_white_d50();
    if (model == "hsl" || model == "ycbcr") {
      description = color_ramp_rgb_description();
      description.model = models.at(model);
    }
    if (model == "ycbcr")
      description.ncl_coefficients =
          take(ps::color_ncl_coefficients(ps::ColorNclPreset::Bt709));
    if (model == "cmyk") {
      ps::ResourceBudget root;
      auto bytes = numeric_fixture::fixture();
      auto icc =
          take(ps::IccProfile::import({bytes.data(), bytes.size()}, root));
      description = color_ramp_cmyk_description(icc.identity());
      c.resources = take(ps::ResourceBindings::create({icc}, root));
    }
    if (polar) {
      description.hue = static_cast<ps::ColorHueUnit>(unit);
      if (unit == 2)
        description.source_layout = ps::ColorSourceLayout::RationalHueSplit;
    }
    unsigned channels = model == "cmyk" ? 4 : unit == 2 ? 2 : 3;
    std::vector<double> colors, expected;
    if (!polar) {
      colors.assign(channels, 0);
      colors.insert(colors.end(), channels, 1);
      expected.assign(channels, .5);
    } else if (model == "hsl") {
      colors = unit == 2 ? std::vector<double>{1, .25, 1, .75}
                         : std::vector<double>{0, 1, .25, 0, 1, .75};
      expected = {0, 1, .5};
    } else {
      colors = unit == 2 ? std::vector<double>{.25, 1, .75, 1}
                         : std::vector<double>{.25, 1, 0, .75, 1, 0};
      expected = {.5, 1, 0};
    }
    auto input = values(n, .5), stops = c.add(doubles({2}, {0, 1})),
         data = c.add(doubles({2, channels}, colors));
    ps::WorkflowNode node{
        1,
        key + suffix,
        {input, stops, data},
        {{"color_description", take(ps::color_array_parameter(description))},
         {"dtype", std::string("float64")},
         {"out_of_domain", std::string("reject")}}};
    if (polar)
      node.parameters["output_hue_unit"] =
          std::string(unit ? "pi_multiple" : "radian");
    if (unit == 2) {
      node.inputs.push_back(c.add(array(T::Int64, {2}, {0, 0})));
      node.inputs.push_back(c.add(array(T::Int64, {2}, {1, 1})));
    }
    c.node(std::move(node));
    c.shape = {n, expected.size()};
    for (std::uint64_t i = 0; i < n; ++i)
      for (auto v : expected)
        c.expected.push_back(raw(v));
  } else {
    throw std::runtime_error("missing benchmark fixture: " + key);
  }
  return c;
}

Case legacy_fixture(const std::string& key, std::uint64_t n) {
  using T = ps::ElementType;
  Case c;
  c.cluster = "legacy";
  c.shape = {n};
  const auto values = [&](std::uint64_t count, double v) {
    return c.add(doubles({count}, std::vector<double>(count, v)));
  };
  const auto expect = [&](std::uint64_t count, double v) {
    c.expected.assign(count, raw(v));
  };
  const auto f32 = [](double v) {
    float f = v;
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
  };
  if (key == "numeric.cast" || key == "numeric.encode_range") {
    ps::WorkflowNode node{
        1,
        key,
        {values(n, key == "numeric.cast" ? 1.5 : .5)},
        {{"dtype", std::string(key == "numeric.cast" ? "float32" : "uint8")},
         {"rounding", std::string("ties_even")},
         {"overflow", std::string("reject")}}};
    if (key == "numeric.encode_range") {
      node.parameters["src_min"] = 0.;
      node.parameters["src_max"] = 1.;
      node.parameters["dst_min"] = 0.;
      node.parameters["dst_max"] = 254.;
    }
    c.node(std::move(node));
    c.dtype = key == "numeric.cast" ? T::Float32 : T::UInt8;
    c.expected.assign(n, key == "numeric.cast" ? f32(1.5) : 127);
  } else if (key == "numeric.mean" || key == "numeric.variance") {
    c.node({1, key, {values(n, 1)}, {}});
    c.shape = {1};
    expect(1, key == "numeric.mean" ? 1 : 0);
  } else if (key == "numeric.ordered_scan") {
    c.node({1, key, {values(n, 1)}, {}});
    for (std::uint64_t i = 0; i < n; ++i)
      c.expected.push_back(raw(i + 1));
  } else if (key == "numeric.sample_expression") {
    c.node({1,
            key,
            {values(1, 0)},
            {{"count", static_cast<std::int64_t>(n)},
             {"expression", std::string("2*x+1")},
             {"start", 0.},
             {"step", 1.}}});
    c.dtype = T::Float32;
    for (std::uint64_t i = 0; i < n; ++i)
      c.expected.push_back(f32(2 * i + 1));
  } else if (key == "curve.sample_linear" || key == "curve.sample_monotone") {
    c.node({1,
            key,
            {c.add(doubles({2, 2}, {0, 0, 1, 1}))},
            {{"count", static_cast<std::int64_t>(n + 1)},
             {"domain_min", 0.},
             {"domain_max", 1.},
             {"out_of_domain", std::string("reject")}}});
    c.shape = {n + 1};
    for (std::uint64_t i = 0; i <= n; ++i)
      c.expected.push_back(raw(static_cast<double>(i) / n));
  } else if (key == "field.smoothstep" || key == "field.apply_lut_1d") {
    auto input = c.add(doubles({1, n}, std::vector<double>(n, .5)));
    c.shape = {1, n};
    ps::WorkflowNode node{1, key, {input}, {}};
    if (key == "field.smoothstep") {
      node.parameters = {{"edge0", 0.}, {"edge1", 1.}};
      c.dtype = T::Float32;
      c.expected.assign(n, f32(.5));
    } else {
      node.inputs.push_back(c.add(doubles({2}, {0, 2})));
      node.parameters = {{"domain_min", 0.},
                         {"domain_max", 1.},
                         {"out_of_domain", std::string("reject")}};
      expect(n, 1);
    }
    c.node(std::move(node));
  } else if (key == "lut.apply_1d") {
    const auto typed = [&](ps::Value value, ps::SemanticKind kind) {
      ps::SemanticDescriptor d;
      d.kind = kind;
      d.channels = {{"value", "value", "dimensionless"}};
      d.sample_step = 1;
      d.sample_axis_unit = "dimensionless";
      return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                          value.layout(), value.storage(),
                                          {take(ps::encode_semantic(d))}));
    };
    auto q = c.add(
        typed(array(T::Float32, {n}, std::vector<std::uint64_t>(n, f32(.25))),
              ps::SemanticKind::SampledSignal));
    auto table = c.add(
        typed(array(T::Float32, {2}, {f32(0), f32(2)}), ps::SemanticKind::Lut));
    c.node({1, key, {q, table}, {{"out_of_domain", std::string("clip")}}});
    c.dtype = T::Float32;
    c.expected.assign(n, f32(.5));
  } else if (key == "numeric.clamp") {
    c.node({1, key, {values(n, 2)}, {{"min", 0.}, {"max", 1.}}});
    expect(n, 1);
  } else if (key == "numeric.abs") {
    c.node({1, key, {values(n, -2)}, {}});
    expect(n, 2);
  } else {
    if (key == "math.add") {
      n = 1;
      c.shape = {1};
    }
    auto a = values(n, 2), b = values(n, 4);
    c.node({1, key, {a, b}, {}});
    expect(n, key == "numeric.subtract"   ? -2
              : key == "numeric.multiply" ? 8
              : key == "numeric.divide"   ? .5
              : key == "numeric.minimum"  ? 2
              : key == "numeric.maximum"  ? 4
                                          : 6);
  }
  c.document.outputs[0].port = "value";
  return c;
}

std::string shape_text(const std::vector<std::uint64_t>& shape) {
  std::string text;
  for (auto extent : shape) {
    if (!text.empty())
      text += 'x';
    text += std::to_string(extent);
  }
  return text;
}
void measure(const Case& c, const std::string& profile) {
  auto wanted = take(ps::Footprint::all(c.shape));
  ps::ResourceBudget budget;
  ps::ValueFragments retained;
  std::vector<std::int64_t> times;
  std::uint64_t source_elements = 0, evaluated = 0, fallbacks = 0;
  bool source_support_available = false;
  std::map<std::string, std::vector<std::uint64_t>> callback_times;
  {
    ps::GraphContext graph(c.document);
    auto compiled =
        take(ps::Compiler(c.registry).compile(graph, {}, c.resources));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = 256 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->capacity[ps::ResourceKind::Metadata] =
        128 * 1024 * 1024;
    ps::ExecutionContext context(c.registry, config);
    budget = take(context.resource_budget());
    auto frozen = take(context.freeze(compiled.plan, c.bindings));
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(64) << 30;
    options.dependencies.maximum_work = UINT64_C(32) << 30;
    options.dependencies.maximum_state_bytes = 64 * 1024 * 1024;
    options.maximum_dependency_cache_work = 0;
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
      retained = {};
      const auto start = std::chrono::steady_clock::now();
      auto result = take(
          context.execute_fragments(frozen, {{"result", wanted}}, {}, options));
      if (repeat)
        times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
      source_elements = evaluated = fallbacks = 0;
      source_support_available = result.dependencies.valid();
      if (source_support_available) {
        for (const auto& entry : take(result.dependencies.source_support()))
          source_elements += take(entry.second.element_count());
      } else {
        require(c.cluster == "CRV-09" || c.cluster == "legacy",
                "sample workflow dependency evidence");
      }
      std::map<std::string, std::uint64_t> callbacks;
      for (const auto& timing : result.diagnostics.operation_timings) {
        if (c.cluster == "CRV-09")
          for (const auto& node : c.document.nodes)
            if (node.id == timing.output.node_id &&
                (node.operation == "curve.pack_lut3d" ||
                 node.operation == "curve.measure_lut3d" ||
                 node.operation == "curve.unpack_lut3d" ||
                 node.operation == "curve.gate_lut3d"))
              callbacks[node.operation] += timing.duration_us;
        evaluated += timing.numeric.evaluated_values;
        fallbacks += timing.numeric.strict_fallbacks;
      }
      if (repeat)
        for (const auto& entry : callbacks)
          callback_times[entry.first].push_back(entry.second);
      require(result.values.at("result").descriptor().element_type == c.dtype,
              "benchmark output dtype");
      std::vector<std::uint64_t> at(c.shape.size(), 0);
      const auto width = ps::Value::element_size(c.dtype);
      for (auto expected : c.expected) {
        std::uint64_t actual = 0;
        require(result.values.at("result").read(at, &actual, width).ok() &&
                    actual == expected,
                "benchmark analytic output bits");
        for (std::size_t axis = at.size(); axis; --axis) {
          if (++at[axis - 1] < c.shape[axis - 1])
            break;
          at[axis - 1] = 0;
        }
      }
      if (c.cluster == "NUM-11" || c.cluster == "NUM-13")
        require(evaluated == c.document.inputs[0].descriptor.shape[0],
                "streaming accumulator input accounting");
      retained = result.values.at("result");
    }
  }
  std::uint64_t escaped = 0;
  require(retained.read(std::vector<std::uint64_t>(c.shape.size(), 0), &escaped,
                        ps::Value::element_size(c.dtype))
                  .ok() &&
              escaped == c.expected.front(),
          "benchmark escaped output read");
  std::sort(times.begin(), times.end());
  const auto stats = budget.statistics();
  std::string inputs;
  for (const auto& input : c.document.inputs) {
    if (!inputs.empty())
      inputs += ';';
    inputs += shape_text(input.descriptor.shape);
  }
  std::cout << c.cluster << ',' << c.operation << ',' << profile << ','
            << inputs << ',' << shape_text(c.shape) << ','
            << (c.dtype == ps::ElementType::UInt8     ? "UInt8"
                : c.dtype == ps::ElementType::Int64   ? "Int64"
                : c.dtype == ps::ElementType::Float32 ? "Float32"
                                                      : "Float64")
            << ",Whole,1,off,7," << times[3] << ',' << times[6] << ','
            << c.expected.size() * ps::Value::element_size(c.dtype) << ','
            << stats.peak[ps::ResourceKind::Payload] << ','
            << stats.peak[ps::ResourceKind::Metadata] << ','
            << stats.live[ps::ResourceKind::Payload] << ','
            << stats.live[ps::ResourceKind::Metadata] << ','
            << (source_support_available ? std::to_string(source_elements)
                                         : "unavailable")
            << ',' << evaluated << ',' << fallbacks << '\n'
            << std::flush;
  for (auto& entry : callback_times) {
    auto& samples = entry.second;
    std::sort(samples.begin(), samples.end());
    require(samples.size() == 7, "per-Result callback sample count");
    std::cout << "callback," << entry.first << ',' << profile << ',' << inputs
              << ',' << shape_text(c.shape) << ",Result,Whole,1,off,7,"
              << samples[3] << ',' << samples[6]
              << ",unavailable,unavailable,unavailable,unavailable,unavailable,"
                 "unavailable,unavailable,unavailable\n";
  }
  retained = {};
  require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
              budget.statistics().live[ps::ResourceKind::Metadata] == 0,
          "benchmark owner release");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "select strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    std::cout << "cluster,operation,profile,input_shapes,output_shape,dtype,"
                 "region,workers,cache,repetitions,median_us,max_us,output_"
                 "bytes,peak_payload,peak_metadata,retained_payload,retained_"
                 "metadata,source_elements,evaluated,fallbacks\n";
    if (argc > 2 && std::string(argv[2]) == "legacy") {
      for (const std::string key : {"numeric.cast",
                                    "numeric.encode_range",
                                    "numeric.add",
                                    "numeric.subtract",
                                    "numeric.multiply",
                                    "numeric.divide",
                                    "numeric.clamp",
                                    "numeric.mean",
                                    "numeric.ordered_scan",
                                    "numeric.variance",
                                    "numeric.minimum",
                                    "numeric.maximum",
                                    "numeric.abs",
                                    "field.apply_lut_1d",
                                    "field.smoothstep",
                                    "math.add",
                                    "curve.sample_linear",
                                    "curve.sample_monotone",
                                    "numeric.sample_expression",
                                    "lut.apply_1d"})
        for (std::uint64_t n : {1, 256}) {
          std::cerr << key << " N=" << n << '\n';
          measure(legacy_fixture(key, n), "legacy");
        }
      for (std::uint64_t n : {1, 256})
        measure(fixture(17, n, profile), selected);
      return 0;
    }
    if (argc > 2 && std::string(argv[2]) == "extended") {
      for (const std::string key : {"array.concatenate",
                                    "array.reshape",
                                    "array.scatter_maximum",
                                    "array.scatter_minimum",
                                    "array.scatter_replace",
                                    "array.scatter_sum",
                                    "array.slice",
                                    "curve.apply_lut1d_channels",
                                    "curve.apply_lut3d_tetrahedral",
                                    "curve.color_ramp_cielab",
                                    "curve.color_ramp_cielch",
                                    "curve.color_ramp_cielch_pi",
                                    "curve.color_ramp_cielch_rational_pi",
                                    "curve.color_ramp_cmyk",
                                    "curve.color_ramp_hsl",
                                    "curve.color_ramp_hsl_pi",
                                    "curve.color_ramp_hsl_rational_pi",
                                    "curve.color_ramp_oklab",
                                    "curve.color_ramp_oklch",
                                    "curve.color_ramp_oklch_pi",
                                    "curve.color_ramp_oklch_rational_pi",
                                    "curve.color_ramp_xyz",
                                    "curve.color_ramp_ycbcr",
                                    "curve.interpolate_linear",
                                    "curve.interpolate_linear_multi",
                                    "curve.interpolate_pchip",
                                    "curve.interpolate_pchip_multi",
                                    "curve.log2_shaper_inverse",
                                    "curve.sample_bezier_function",
                                    "numeric.arange",
                                    "numeric.broadcast",
                                    "numeric.clamp",
                                    "numeric.derivative_1d",
                                    "numeric.equal",
                                    "numeric.greater",
                                    "numeric.greater_equal",
                                    "numeric.integral_image",
                                    "numeric.less",
                                    "numeric.less_equal",
                                    "numeric.mix",
                                    "numeric.not_equal",
                                    "numeric.quantile",
                                    "numeric.reduce_count",
                                    "numeric.reduce_maximum",
                                    "numeric.reduce_mean",
                                    "numeric.reduce_minimum",
                                    "numeric.reduce_std",
                                    "numeric.reduce_variance",
                                    "numeric.select"})
        for (std::uint64_t n : {1, 256}) {
          if (argc > 3 && key.find(argv[3]) == std::string::npos)
            continue;
          auto c = extended_fixture(key, n, profile);
          std::cerr << key << " N=" << n << '\n';
          measure(c, selected);
        }
      return 0;
    }
    for (unsigned operation = 0; operation < 18; ++operation)
      for (std::uint64_t n : {1, 256}) {
        auto c = fixture(operation, n, profile);
        if (argc > 3 && c.operation.find(argv[3]) == std::string::npos)
          continue;
        std::cerr << c.cluster << ' ' << shape_text(c.shape) << '\n';
        measure(c, selected);
      }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
