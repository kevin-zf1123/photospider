#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
  const std::string suffix = profile == ps::CpuNumericProfile::Strict
                                 ? "_strict"
                                 : "_accelerated_apple_silicon";
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
  {
    ps::GraphContext graph(c.document);
    auto compiled = take(ps::Compiler(c.registry).compile(graph));
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
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      retained = {};
      const auto start = std::chrono::steady_clock::now();
      auto result = take(
          context.execute_fragments(frozen, {{"result", wanted}}, {}, options));
      times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count());
      source_elements = evaluated = fallbacks = 0;
      source_support_available = result.dependencies.valid();
      if (source_support_available) {
        for (const auto& entry : take(result.dependencies.source_support()))
          source_elements += take(entry.second.element_count());
      } else {
        require(c.cluster == "CRV-09", "sample workflow dependency evidence");
      }
      for (const auto& timing : result.diagnostics.operation_timings) {
        evaluated += timing.numeric.evaluated_values;
        fallbacks += timing.numeric.strict_fallbacks;
      }
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
            << (c.dtype == ps::ElementType::UInt8 ? "UInt8" : "Float64")
            << ",Whole,1,off,3," << times[1] << ',' << times[2] << ','
            << c.expected.size() * ps::Value::element_size(c.dtype) << ','
            << stats.peak[ps::ResourceKind::Payload] << ','
            << stats.peak[ps::ResourceKind::Metadata] << ','
            << stats.live[ps::ResourceKind::Payload] << ','
            << stats.live[ps::ResourceKind::Metadata] << ','
            << (source_support_available ? std::to_string(source_elements)
                                         : "unavailable")
            << ',' << evaluated << ',' << fallbacks << '\n'
            << std::flush;
  retained = {};
  require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
              budget.statistics().live[ps::ResourceKind::Metadata] == 0,
          "benchmark owner release");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple",
            "select strict or apple");
    const auto profile = selected == "strict"
                             ? ps::CpuNumericProfile::Strict
                             : ps::CpuNumericProfile::AppleSiliconNeon;
    std::cout << "cluster,operation,profile,input_shapes,output_shape,dtype,"
                 "region,workers,cache,repetitions,median_us,max_us,output_"
                 "bytes,peak_payload,peak_metadata,retained_payload,retained_"
                 "metadata,source_elements,evaluated,fallbacks\n";
    for (unsigned operation = 0; operation < 18; ++operation)
      for (std::uint64_t n : {1, 256}) {
        auto c = fixture(operation, n, profile);
        std::cerr << c.cluster << ' ' << shape_text(c.shape) << '\n';
        measure(c, selected);
      }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
