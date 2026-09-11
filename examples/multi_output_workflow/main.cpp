#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const std::string& reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
template <class T>
T take(Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}
Value samples(std::vector<std::uint64_t> shape, const std::vector<float>& data,
              bool image = false) {
  auto buffer = take(MutableValue::allocate(
      {ElementType::Float32, shape}, Region::whole(shape), BufferAllocator{}));
  require(buffer.size() == data.size() * sizeof(float), "fixture shape");
  std::memcpy(buffer.data(), data.data(), buffer.size());
  std::vector<ValueFacet> facets;
  if (image) {
    auto rgb = rgba_semantics();
    rgb.channels.pop_back();
    rgb.association = "none";
    facets = {take(encode_semantic(rgb))};
  }
  return take(std::move(buffer).publish(facets));
}
float sample(const Value& value, const std::vector<std::uint64_t>& coordinate) {
  float number = 0;
  const auto address = take(value.byte_address(coordinate));
  std::memcpy(&number, value.bytes().data() + address, sizeof(number));
  return number;
}
struct Reads {
  std::mutex mutex;
  std::map<std::string, std::set<std::string>> regions;
};
std::string region_text(const Region& region) {
  std::ostringstream text;
  for (const auto& axis : region.dimensions())
    text << '[' << axis.offset << ',' << axis.offset + axis.extent << ')';
  return text.str();
}
// Only public APIs: source callbacks record actual transport, independently of
// the dependency certificates returned by execution.
ExecutionResult run(WorkflowDocument doc, const std::vector<Value>& inputs,
                    bool joint, const std::string& label,
                    PlanningOptions planning = {}) {
  auto reads = std::make_shared<Reads>();
  ExecutionBindings bindings;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto name = "input" + std::to_string(i);
    const auto value = inputs[i];
    doc.inputs.push_back({i + 1, name, value.descriptor(), value.region(),
                          value.layout(), value.facets()});
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = value.descriptor();
    source->facets = value.facets();
    source->read = [reads, name, value](
                       const Region& region, std::uint8_t* output,
                       std::uint64_t size, const BufferAllocator&,
                       const CancellationToken&) -> Result<Region> {
      {
        std::lock_guard<std::mutex> lock(reads->mutex);
        reads->regions[name].insert(region_text(region));
      }
      auto count = take(region.element_count());
      if (size != count * sizeof(float))
        return Result<Region>(
            Status{ErrorCode::InvalidArgument, "source size"});
      const auto& axes = region.dimensions();
      std::vector<std::uint64_t> coordinate(axes.size());
      for (std::uint64_t i = 0; i < count; ++i) {
        auto remaining = i;
        for (std::size_t axis = axes.size(); axis-- > 0;) {
          coordinate[axis] = axes[axis].offset + remaining % axes[axis].extent;
          remaining /= axes[axis].extent;
        }
        auto address = value.byte_address(coordinate);
        if (!address.ok())
          return Result<Region>(address.status());
        std::memcpy(output + i * sizeof(float),
                    value.bytes().data() + address.value(), sizeof(float));
      }
      return Result<Region>(region);
    };
    bindings.inputs.push_back({name, {}, source});
  }
  auto registry = make_default_operation_registry();
  GraphContext graph(doc);
  planning.tile_height = 1;
  planning.tile_width = 2;
  auto compiled = take(Compiler(registry).compile(graph, planning));
  ExecutionContext execution(registry, {2, false, 64, 16777216, 0});
  ExecutionOptions options;
  options.enable_joint = joint;
  // The 129x129 case intentionally exceeds the conservative default fuel.
  // Bound this demonstration explicitly, including certificate normalization.
  options.maximum_dependency_work = 100000000;
  options.dependencies.maximum_work = 100000000;
  options.dependencies.sets.maximum_work = 100000000;
  auto result = take(execution.execute(compiled.plan, bindings, {}, options));
  std::cout << label << " joint=" << joint
            << " groups=" << result.diagnostics.joint_groups
            << " source_reads=" << result.diagnostics.source_read_count << '\n';
  for (const auto& entry : result.values) {
    std::cout << "  " << entry.first << " shape=";
    for (auto extent : entry.second.descriptor().shape)
      std::cout << extent << ' ';
    std::cout << '\n';
  }
  std::map<ValueRef, std::uint64_t> attempts;
  for (const auto& timing : result.diagnostics.operation_timings)
    attempts[timing.output] += timing.invocation_count;
  for (const auto& entry : attempts)
    std::cout << "  result=" << entry.first.node_id << ':'
              << entry.first.output_index << " attempts=" << entry.second
              << '\n';
  for (const auto& entry : reads->regions) {
    std::cout << "  " << entry.first << " read_set=";
    for (const auto& region : entry.second)
      std::cout << region << ' ';
    std::cout << '\n';
  }
  return result;
}
std::vector<float> pixels() {
  std::vector<float> data(3 * 5 * 3);
  for (std::size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<float>((i * 7) % 23) / 22;
  return data;
}
std::array<long double, 3> ycbcr(const float* rgb) {
  std::array<long double, 3> encoded;
  for (unsigned c = 0; c < 3; ++c) {
    const long double linear = rgb[c];
    encoded[c] = linear < .018L ? 4.5L * linear
                                : 1.099L * std::pow(linear, .45L) - .099L;
  }
  const auto y =
      .2126L * encoded[0] + .7152L * encoded[1] + .0722L * encoded[2];
  return {y, (encoded[2] - y) / 1.8556L, (encoded[0] - y) / 1.5748L};
}
void color(bool joint) {
  auto data = pixels();
  auto image = samples({3, 5, 3}, data, true);
  WorkflowDocument doc;
  doc.nodes = {{1, "color.rgb_to_ycbcr420", {WorkflowInputReference{1}}, {}}};
  doc.outputs = {{"y", 1, "y"}, {"cb", 1, "cb"}, {"cr", 1, "cr"}};
  auto result = run(doc, {image}, joint, "420");
  const std::array<std::string, 3> names{"y", "cb", "cr"};
  for (unsigned c = 0; c < 3; ++c) {
    const std::uint64_t stride = c ? 2 : 1;
    for (std::uint64_t y = 0; y < (c ? 2U : 3U); ++y)
      for (std::uint64_t x = 0; x < (c ? 3U : 5U); ++x) {
        long double expected = 0;
        unsigned count = 0;
        for (auto sy = y * stride;
             sy < std::min<std::uint64_t>(3, (y + 1) * stride); ++sy)
          for (auto sx = x * stride;
               sx < std::min<std::uint64_t>(5, (x + 1) * stride); ++sx) {
            expected += ycbcr(&data[(sy * 5 + sx) * 3])[c];
            ++count;
          }
        require(std::abs(sample(result.values.at(names[c]), {y, x}) -
                         expected / count) < 1e-7L,
                "420 oracle");
      }
  }
  doc.outputs = {{"y", 1, "y"}};
  auto only = run(doc, {image}, joint, "420-only-y");
  for (const auto& timing : only.diagnostics.operation_timings)
    require(timing.output.output_index == 0, "unrequested sibling executed");
}
void split(bool joint) {
  auto data = pixels();
  WorkflowDocument doc;
  doc.nodes = {{1,
                "image.split_horizontal",
                {WorkflowInputReference{1}},
                {{"split_x", std::int64_t{2}}}}};
  doc.outputs = {{"full", 1, "full"},
                 {"left", 1, "left"},
                 {"right", 1, "right"}};
  PlanningOptions planning;
  planning.output_regions = {{"full", Region({{0, 1}, {3, 1}, {0, 3}})},
                             {"left", Region({{1, 1}, {0, 1}, {0, 3}})},
                             {"right", Region({{2, 1}, {1, 1}, {0, 3}})}};
  auto result =
      run(doc, {samples({3, 5, 3}, data, true)}, joint, "split", planning);
  const std::array<std::string, 3> names{"full", "left", "right"};
  const std::array<std::uint64_t, 3> local_x{3, 0, 1}, source_x{3, 0, 3};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned c = 0; c < 3; ++c)
      require(sample(result.values.at(names[i]), {i, local_x[i], c}) ==
                  data[(i * 5 + source_x[i]) * 3 + c],
              "split oracle");
}
void channels(bool joint) {
  auto data = pixels();
  const std::vector<std::vector<float>> kernels{{.5F, -.25F, .75F, 1.F},
                                                {1, 2, -1},
                                                {.25F, .5F, .25F}};
  const std::array<int, 3> kh{2, 1, 3}, kw{2, 3, 1}, ay{0, 0, 2}, ax{1, 1, 0};
  std::vector<Value> inputs{samples({3, 5, 3}, data, true)};
  std::map<std::string, ParameterValue> parameters;
  const std::array<std::string, 3> names{"r", "g", "b"};
  for (unsigned c = 0; c < 3; ++c) {
    inputs.push_back(samples(
        {static_cast<std::uint64_t>(kh[c]), static_cast<std::uint64_t>(kw[c])},
        kernels[c]));
    parameters[names[c] + "_anchor_y"] = static_cast<std::int64_t>(ay[c]);
    parameters[names[c] + "_anchor_x"] = static_cast<std::int64_t>(ax[c]);
    parameters[names[c] + "_boundary"] = std::string(c == 1 ? "clamp" : "zero");
  }
  WorkflowDocument doc;
  doc.nodes = {{1,
                "image.convolve_channels",
                {WorkflowInputReference{1}, WorkflowInputReference{2},
                 WorkflowInputReference{3}, WorkflowInputReference{4}},
                parameters}};
  doc.outputs = {{"r", 1, "r"}, {"g", 1, "g"}, {"b", 1, "b"}};
  auto result = run(doc, inputs, joint, "channels");
  for (unsigned c = 0; c < 3; ++c)
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 5; ++x) {
        double expected = 0;
        for (int ky = 0; ky < kh[c]; ++ky)
          for (int kx = 0; kx < kw[c]; ++kx) {
            int sy = y + ay[c] - ky, sx = x + ax[c] - kx;
            if (c == 1) {
              sy = std::clamp(sy, 0, 2);
              sx = std::clamp(sx, 0, 4);
            }
            if (sy >= 0 && sy < 3 && sx >= 0 && sx < 5)
              expected += static_cast<double>(kernels[c][ky * kw[c] + kx]) *
                          data[(sy * 5 + sx) * 3 + c];
          }
        require(sample(result.values.at(names[c]),
                       {static_cast<std::uint64_t>(y),
                        static_cast<std::uint64_t>(x)}) ==
                    static_cast<float>(expected),
                "channel oracle");
      }
  doc.outputs = {{"g", 1, "g"}};
  auto only = run(doc, inputs, joint, "channels-only-g");
  for (const auto& timing : only.diagnostics.operation_timings)
    require(timing.output.output_index == 1, "unrequested channel executed");
  for (const auto& name : {"input1", "input3"}) {
    auto dirty = take(only.dependencies.potential_dirty(
        name, take(Footprint::all(inputs[name == std::string("input1") ? 1 : 3]
                                      .descriptor()
                                      .shape))));
    require(dirty.at("g").empty(), "sibling kernel dependency");
  }
}
void gaussian(bool joint, double radius, double sigma) {
  require(std::isfinite(radius) && radius >= 0 && radius <= 64 &&
              std::isfinite(sigma) && sigma >= 0 && sigma <= 64,
          "radius/sigma range");
  const auto extent = static_cast<std::int64_t>(std::ceil(radius));
  const auto side = static_cast<std::uint64_t>(2 * extent + 1);
  auto data = pixels();
  WorkflowDocument doc;
  doc.nodes = {
      {1,
       "image.gaussian_blur_with_kernel",
       {WorkflowInputReference{1}},
       {{"radius", radius}, {"sigma", sigma}}},
      {2,
       "channel.extract",
       {WorkflowInputReference{1}},
       {{"index", std::int64_t{0}}}},
      {3,
       "field.convolve",
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{1, "kernel"}},
       {{"anchor_y", extent},
        {"anchor_x", extent},
        {"boundary", std::string("clamp")}}}};
  doc.outputs = {{"image", 1, "image"},
                 {"kernel", 1, "kernel"},
                 {"recomputed", 3, "value"}};
  auto image = samples({3, 5, 3}, data, true);
  PlanningOptions planning;
  // Keep the maximum-kernel demonstration small while retaining its complete
  // kernel output and a real independently recomputed image observation.
  if (side > 17) {
    planning.output_regions = {{"image", Region({{1, 1}, {2, 1}, {0, 3}})},
                               {"recomputed", Region({{1, 1}, {2, 1}})}};
  }
  auto result = run(doc, {image}, joint, "gaussian", planning);
  require(result.values.at("kernel").descriptor().shape ==
              std::vector<std::uint64_t>({side, side}),
          "kernel shape");
  long double normalizer = 0;
  std::vector<long double> weights;
  for (auto y = -extent; y <= extent; ++y)
    for (auto x = -extent; x <= extent; ++x) {
      const auto a = [&](std::int64_t d) {
        return std::clamp(static_cast<long double>(radius) - (std::abs(d) - 1),
                          0.L, 1.L);
      };
      const long double s = sigma;
      const auto weight =
          sigma == 0
              ? (x == 0 && y == 0 ? 1.L : 0.L)
              : std::exp(-.5L * ((x / s) * (x / s) + (y / s) * (y / s))) *
                    a(x) * a(y);
      weights.push_back(weight);
      normalizer += weight;
    }
  for (std::uint64_t y = 0; y < side; ++y)
    for (std::uint64_t x = 0; x < side; ++x)
      require(std::abs(sample(result.values.at("kernel"), {y, x}) -
                       weights[y * side + x] / normalizer) < 1e-7L,
              "kernel oracle");
  for (std::uint64_t y = side > 17 ? 1 : 0; y < (side > 17 ? 2U : 3U); ++y)
    for (std::uint64_t x = side > 17 ? 2 : 0; x < (side > 17 ? 3U : 5U); ++x)
      require(sample(result.values.at("image"), {y, x, 0}) ==
                  sample(result.values.at("recomputed"), {y, x}),
              "public convolution recomputation");
  doc.outputs = {{"kernel", 1, "kernel"}};
  auto only = run(doc, {image}, joint, "gaussian-only-kernel");
  require(only.diagnostics.source_read_count == 0, "kernel read image samples");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    std::string scenario = "all";
    bool joint = true;
    double radius = 1.25, sigma = .9;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "--scenario all|420|split|channels|gaussian --joint "
                     "on|off --radius FLOAT --sigma FLOAT\n";
        return 0;
      }
      require(i + 1 < argc, "missing option value");
      const std::string value = argv[++i];
      if (option == "--scenario") {
        scenario = value;
      } else if (option == "--radius") {
        radius = std::stod(value);
      } else if (option == "--sigma") {
        sigma = std::stod(value);
      } else if (option == "--joint") {
        require(value == "on" || value == "off", "joint value");
        joint = value == "on";
      } else {
        require(false, "unknown option");
      }
    }
    require(scenario == "all" || scenario == "420" || scenario == "split" ||
                scenario == "channels" || scenario == "gaussian",
            "scenario");
    if (scenario == "all" || scenario == "420")
      color(joint);
    if (scenario == "all" || scenario == "split")
      split(joint);
    if (scenario == "all" || scenario == "channels")
      channels(joint);
    if (scenario == "all" || scenario == "gaussian")
      gaussian(joint, radius, sigma);
    std::cout << "multi-output oracle=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "multi-output failed: " << error.what() << '\n';
    return 1;
  }
}
