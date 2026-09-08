#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const std::string& detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
ValueFacet profile() {
  const std::string payload = "rgba;linear-srgb;premultiplied;hwc";
  return {"photospider.image", 1, {payload.begin(), payload.end()}};
}
Value value(const std::vector<float>& pixels,
            const std::vector<std::uint64_t>& shape, bool image) {
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = 4;
  for (std::size_t i = shape.size(); i > 0; --i) {
    strides[i - 1] = stride;
    stride *= static_cast<std::int64_t>(shape[i - 1]);
  }
  std::vector<std::uint8_t> bytes(pixels.size() * 4);
  std::memcpy(bytes.data(), pixels.data(), bytes.size());
  auto made = Value::create(
      {ElementType::Float32, shape}, Region::whole(shape), {0, strides},
      std::move(bytes),
      image ? std::vector<ValueFacet>{profile()} : std::vector<ValueFacet>{});
  require(made.ok(), made.status().message);
  return made.take_value();
}
struct Fixture {
  std::uint64_t height = 7, width = 11;
  int radius = 3;
  double sigma = 1.5;
  float gain = 2;
  std::vector<float> foreground, background, mask;
  explicit Fixture(bool uniform = false) {
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        const bool transparent = (x + y) % 5 == 0;
        const float alpha = uniform ? .5F : transparent ? 0 : .75F;
        foreground.insert(
            foreground.end(),
            {uniform ? .125F : alpha * static_cast<float>(x % 4),
             uniform ? .125F : alpha * static_cast<float>(y % 3) / 2,
             uniform ? .125F : alpha / 4, alpha});
        background.insert(background.end(), {.25F, .25F, .25F, .5F});
        mask.push_back(uniform ? .5F : static_cast<float>((x + 2 * y) % 3) / 2);
      }
  }
  ExecutionBindings bindings() const {
    const std::vector<std::uint64_t> shape{height, width, 4};
    return {{{"foreground", value(foreground, shape, true)},
             {"background", value(background, shape, true)},
             {"mask", value(mask, {height, width}, false)},
             {"gain", value({gain}, {1}, false)}}};
  }
  WorkflowDocument document() const {
    WorkflowDocument document;
    const auto bound = bindings();
    for (std::size_t i = 0; i < bound.inputs.size(); ++i) {
      const auto& input = bound.inputs[i];
      document.inputs.push_back({i + 1, input.name, input.value.descriptor(),
                                 input.value.region(), input.value.layout(),
                                 input.value.facets()});
    }
    document.nodes = {
        {10,
         "image.gaussian_blur",
         {WorkflowInputReference{1}},
         {{"radius", static_cast<std::int64_t>(radius)}, {"sigma", sigma}}},
        {20,
         "image.exposure_gain",
         {WorkflowNodeOutput{10, "value"}, WorkflowInputReference{4}},
         {}},
        {30,
         "image.mask",
         {WorkflowNodeOutput{20, "value"}, WorkflowInputReference{3}},
         {}},
        {40,
         "image.source_over",
         {WorkflowNodeOutput{30, "value"}, WorkflowInputReference{2}},
         {}}};
    document.outputs = {{"result", 40, "value"}};
    return document;
  }
  // Independent full-image two-dimensional convolution. It does not reuse
  // separable passes, kernel callbacks, region mapping or host buffers.
  std::vector<float> oracle() const {
    std::vector<float> output(foreground.size());
    std::vector<double> kernel;
    double denominator = 0;
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx) {
        const auto weight =
            std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
        kernel.push_back(weight);
        denominator += weight;
      }
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        float front[4]{};
        for (std::size_t c = 0; c < 4; ++c) {
          double sum = 0;
          std::size_t k = 0;
          for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
              const auto row = std::clamp<std::int64_t>(
                  static_cast<std::int64_t>(y) + dy, 0, height - 1);
              const auto col = std::clamp<std::int64_t>(
                  static_cast<std::int64_t>(x) + dx, 0, width - 1);
              sum += foreground[(row * width + col) * 4 + c] * kernel[k++] /
                     denominator;
            }
          const float blurred = static_cast<float>(sum);
          const float exposed = c == 3 ? blurred : blurred * gain;
          front[c] = exposed * mask[y * width + x];
        }
        const float remaining = 1 - front[3];
        for (std::size_t c = 0; c < 4; ++c) {
          const auto index = (y * width + x) * 4 + c;
          const float back = background[index] * remaining;
          output[index] = front[c] + back;
        }
      }
    return output;
  }
};
void check(ValueView output, const std::vector<float>& expected,
           std::uint64_t width, bool exact) {
  const auto yd = output.region().dimensions()[0],
             xd = output.region().dimensions()[1];
  require(output.facets().size() == 1 &&
              output.facets()[0].payload == profile().payload,
          "image profile changed");
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y)
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < 4; ++c) {
        auto address = output.byte_address({y, x, c});
        require(address.ok(), "invalid output view");
        float actual = 0;
        std::memcpy(&actual, output.bytes().data() + address.value(), 4);
        const float reference = expected[(y * width + x) * 4 + c];
        require(exact ? std::memcmp(&actual, &reference, 4) == 0
                      : std::abs(actual - reference) <=
                            1e-6 + 1e-5 * std::abs(reference),
                "S2Image.RegionAndTiles oracle mismatch at " +
                    std::to_string(y) + "," + std::to_string(x));
      }
}
std::vector<float> pixels(const Value& output) {
  std::vector<float> result(output.bytes().size() / 4);
  std::memcpy(result.data(), output.bytes().data(), output.bytes().size());
  return result;
}
void direct_invocation(const std::shared_ptr<OperationRegistry>& operations) {
  const auto image = value(std::vector<float>(36, .5F), {3, 3, 4}, true);
  const auto gain = value({2}, {1}, false);
  const std::map<std::string, ParameterValue> none;
  std::vector<Value> inputs{image, gain};
  std::vector<Region> demands{image.region(), gain.region()};
  unsigned allocations = 0;
  BufferAllocator allocator([&](std::uint64_t) {
    ++allocations;
    return Result<std::shared_ptr<void>>(std::make_shared<int>(0));
  });
  OperationInvocation channels(inputs, demands, none, Backend::Cpu, {},
                               Region({{0, 1}, {0, 1}, {0, 1}}), allocator);
  require(operations->invoke("image.exposure_gain", channels).status().code ==
              ErrorCode::InvalidArgument,
          "direct partial-channel output must fail before allocation");
  require(allocations == 0, "partial output entered callback");
  for (bool broadcast : {true, false}) {
    auto unusual = Value::create(
        image.descriptor(), image.region(),
        broadcast
            ? StridedLayout{0, {0, 0, 4}, {UINT64_C(1) << 63, UINT64_MAX, 0}}
            : StridedLayout{0, {-48, -16, 4}, {2, 2, 0}},
        broadcast ? std::vector<std::uint8_t>(image.bytes().begin(),
                                              image.bytes().begin() + 16)
                  : image.copy_bytes(),
        image.facets());
    require(unusual.ok(), unusual.status().message);
    inputs = {unusual.take_value(), gain};
    OperationInvocation strided(inputs, demands, none);
    auto exposed = operations->invoke("image.exposure_gain", strided);
    require(exposed.ok(), exposed.status().message);
    const auto output = pixels(exposed.value());
    for (std::size_t index = 0; index < output.size(); ++index)
      require(output[index] == (index % 4 == 3 ? .5F : 1.F),
              "broadcast/reversed C view");
  }
  inputs = {image};
  demands = {Region({{1, 1}, {0, 3}, {0, 4}})};
  const std::map<std::string, ParameterValue> parameters{
      {"radius", static_cast<std::int64_t>(1)},
      {"sigma", 1.0}};
  OperationInvocation halo(inputs, demands, parameters, Backend::Cpu, {},
                           Region({{1, 1}, {1, 1}, {0, 4}}), allocator);
  require(operations->invoke("image.gaussian_blur", halo).status().code ==
              ErrorCode::InvalidArgument,
          "direct Gaussian must reject insufficient halo");
  require(allocations == 0, "insufficient halo entered callback");
  inputs[0] = image.view(demands[0]).take_value();
  demands[0] = image.region();
  require(operations->invoke("image.gaussian_blur", halo).status().code ==
              ErrorCode::TypeMismatch,
          "input Value must cover its claimed demand");
  inputs[0] = image;
  require(operations->invoke("image.gaussian_blur", halo).ok(),
          "direct valid halo");
  auto far_edge = Value::create(
      {ElementType::Float32, {UINT64_MAX, 1, 4}},
      Region({{UINT64_MAX - 4, 4}, {0, 1}, {0, 4}}), {0, {0, 0, 4}},
      std::vector<std::uint8_t>(image.bytes().begin(),
                                image.bytes().begin() + 16),
      {profile()});
  require(far_edge.ok(), far_edge.status().message);
  inputs = {far_edge.take_value()};
  demands = {inputs[0].region()};
  const std::map<std::string, ParameterValue> far_parameters{
      {"radius", static_cast<std::int64_t>(3)},
      {"sigma", 1.0}};
  OperationInvocation far(inputs, demands, far_parameters, Backend::Cpu, {},
                          Region({{UINT64_MAX - 1, 1}, {0, 1}, {0, 4}}));
  auto far_result = operations->invoke("image.gaussian_blur", far);
  require(far_result.ok(), far_result.status().message);
  require(pixels(far_result.value()) == std::vector<float>(4, .5F),
          "large logical edge clamp");
  inputs = {image, value(std::vector<float>(6, 1), {2, 3}, false)};
  demands = {image.region(), inputs[1].region()};
  OperationInvocation mask(inputs, demands, none);
  require(operations->invoke("image.mask", mask).status().code ==
              ErrorCode::TypeMismatch,
          "direct mask shape mismatch");
}
void numerical(const std::shared_ptr<OperationRegistry>& operations) {
  Compiler compiler(operations);
  ExecutionContext execution(operations, {2, false, 16, 1024 * 1024});
  for (bool uniform : {true, false}) {
    Fixture fixture(uniform);
    GraphContext graph(fixture.document());
    auto compiled = compiler.compile(graph);
    require(compiled.ok(), compiled.status().message);
    auto full = execution.execute(compiled.value().plan, fixture.bindings());
    require(full.ok(), full.status().message);
    check(ValueView(full.value().values.at("result")), fixture.oracle(),
          fixture.width, false);
    const auto reference = pixels(full.value().values.at("result"));
    if (uniform) {
      for (std::size_t i = 0; i < reference.size(); ++i)
        require(reference[i] == (i % 4 == 3 ? .625F : .3125F),
                "hand-computed uniform fixture");
    }
    for (const auto& geometry :
         {std::make_pair(1, 1), std::make_pair(2, 3), std::make_pair(5, 7),
          std::make_pair(128, 128)}) {
      for (bool roi : {false, true}) {
        PlanningOptions options;
        options.tile_height = geometry.first;
        options.tile_width = geometry.second;
        if (roi)
          options.output_regions = {
              {"result", Region({{1, 5}, {2, 7}, {0, 4}})}};
        auto planned = compiler.plan(compiled.value().optimized, options);
        require(planned.ok(), planned.status().message);
        auto output = execution.execute(planned.value(), fixture.bindings());
        require(output.ok(), output.status().message);
        check(ValueView(output.value().values.at("result")), reference,
              fixture.width, true);
        check(ValueView(output.value().values.at("result")), fixture.oracle(),
              fixture.width, false);
        auto streamed = execution.execute_stream(
            planned.value(), fixture.bindings(),
            [&](const std::string& name, ValueView tile) {
              require(name == "result", "output name");
              check(tile, reference, fixture.width, true);
              return Status::success();
            });
        require(streamed.ok(), streamed.status().message);
        require(streamed.value().operation_timings.size() == 4 &&
                    streamed.value().peak_active_tasks <= 2,
                "bounded task diagnostics");
      }
    }
    fixture.gain = .5F;
    auto second = execution.execute(compiled.value().plan, fixture.bindings());
    require(second.ok(), second.status().message);
    check(ValueView(second.value().values.at("result")), fixture.oracle(),
          fixture.width, false);
  }
  Fixture fixture;
  for (const auto& parameter :
       {ParameterValue(static_cast<std::int64_t>(0)),
        ParameterValue(static_cast<std::int64_t>(65)), ParameterValue(3.0)}) {
    auto document = fixture.document();
    document.nodes[0].parameters["radius"] = parameter;
    GraphContext graph(document);
    require(!compiler.compile(graph).ok(), "radius validation");
  }
  for (double sigma : {0.0, 65.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    auto document = fixture.document();
    document.nodes[0].parameters["sigma"] = sigma;
    GraphContext graph(document);
    require(!compiler.compile(graph).ok(), "sigma validation");
  }
  auto missing = fixture.document();
  missing.nodes[0].parameters.erase("sigma");
  GraphContext missing_graph(missing);
  require(!compiler.compile(missing_graph).ok(), "required sigma");
  GraphContext graph(fixture.document());
  auto compiled = compiler.compile(graph);
  require(compiled.ok(), compiled.status().message);
  for (float bad : {-1.F, 1.01F, std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN()}) {
    fixture.mask[0] = bad;
    auto output = execution.execute(compiled.value().plan, fixture.bindings());
    require(!output.ok() && output.status().code == ErrorCode::InvalidArgument,
            "mask interval");
  }
  for (int axis = 0; axis < 2; ++axis) {
    auto mismatch = fixture.document();
    auto& input = mismatch.inputs[axis == 0 ? 1 : 2];
    --input.descriptor.shape[0];
    input.region = Region::whole(input.descriptor.shape);
    GraphContext invalid_graph(mismatch);
    require(!compiler.compile(invalid_graph).ok(),
            "background/mask shape mismatch");
  }
  // Isolated impulse at an edge and sigma's lower bound exercise clamp and
  // underflowed weights separately from the uniform/HDR scene.
  Fixture impulse;
  std::fill(impulse.foreground.begin(), impulse.foreground.end(), 0);
  impulse.foreground[0] = impulse.foreground[3] = 1;
  impulse.radius = 1;
  impulse.sigma = .1;
  impulse.mask.assign(impulse.mask.size(), 1);
  GraphContext impulse_graph(impulse.document());
  PlanningOptions impulse_options;
  impulse_options.tile_height = 1;
  impulse_options.tile_width = 2;
  auto impulse_plan = compiler.compile(impulse_graph, impulse_options);
  require(impulse_plan.ok(), impulse_plan.status().message);
  auto impulse_result =
      execution.execute(impulse_plan.value().plan, impulse.bindings());
  require(impulse_result.ok(), impulse_result.status().message);
  check(ValueView(impulse_result.value().values.at("result")), impulse.oracle(),
        impulse.width, false);
  // Radius can exceed both image axes and the tile; clamp remains image-local.
  Fixture wide_halo(true);
  wide_halo.radius = 64;
  wide_halo.sigma = 64;
  GraphContext wide_graph(wide_halo.document());
  PlanningOptions options;
  options.tile_height = options.tile_width = 1;
  options.output_regions = {{"result", Region({{0, 1}, {0, 1}, {0, 4}})}};
  auto wide_plan = compiler.compile(wide_graph, options);
  require(wide_plan.ok(), wide_plan.status().message);
  auto wide = execution.execute(wide_plan.value().plan, wide_halo.bindings());
  require(wide.ok(), wide.status().message);
  const auto reference = pixels(wide.value().values.at("result"));
  require(reference == std::vector<float>({.3125F, .3125F, .3125F, .625F}),
          "radius 64 clamp");
}

void large_source(const std::shared_ptr<OperationRegistry>& operations) {
  Fixture fixture(true);
  auto document = fixture.document();
  const std::uint64_t side = 65536;
  ExecutionBindings bindings;
  for (auto& input : document.inputs) {
    if (input.name == "gain") {
      bindings.inputs.push_back({input.name, value({2}, {1}, false)});
      continue;
    }
    const bool mask = input.name == "mask";
    input.descriptor.shape = mask ? std::vector<std::uint64_t>{side, side}
                                  : std::vector<std::uint64_t>{side, side, 4};
    input.region = Region::whole(input.descriptor.shape);
    input.layout =
        mask ? StridedLayout{0, {static_cast<std::int64_t>(side * 4), 4}}
             : StridedLayout{0, {static_cast<std::int64_t>(side * 16), 16, 4}};
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = input.descriptor;
    source->facets = input.facets;
    const bool background = input.name == "background";
    source->read = [mask, background](
                       const Region& region, std::uint8_t* destination,
                       std::uint64_t bytes, const BufferAllocator&,
                       const CancellationToken&) {
      for (std::uint64_t offset = 0; offset < bytes; offset += 4) {
        const float sample = mask || offset % 16 == 12 ? .5F
                             : background              ? .25F
                                                       : .125F;
        std::memcpy(destination + offset, &sample, 4);
      }
      return Result<Region>(region);
    };
    bindings.inputs.push_back({input.name, {}, source});
  }
  GraphContext graph(document);
  Compiler compiler(operations);
  PlanningOptions options;
  options.tile_height = 2;
  options.tile_width = 3;
  options.output_regions = {{"result", Region({{100, 5}, {200, 7}, {0, 4}})}};
  auto compiled = compiler.compile(graph, options);
  require(compiled.ok(), compiled.status().message);
  auto sink = [](const std::string&, ValueView tile) {
    for (std::size_t offset = 0; offset < tile.bytes().size(); offset += 4) {
      float number = 0;
      std::memcpy(&number, tile.bytes().data() + offset, 4);
      require(number == (offset % 16 == 12 ? .625F : .3125F),
              "large source oracle");
    }
    return Status::success();
  };
  ExecutionContext execution(operations, {2, false, 16, 16384});
  auto stream = execution.execute_stream(compiled.value().plan, bindings, sink);
  require(stream.ok(), stream.status().message);
  const auto& stats = stream.value();
  require(stats.tile_count == 9 && stats.source_read_count == 27 &&
              stats.source_read_bytes < 12000 && stats.peak_live_bytes < 8192,
          "ROI must bound source reads and live storage");
  ExecutionContext exact(operations, {2, false, 16, stats.planned_peak_bytes});
  require(exact.execute_stream(compiled.value().plan, bindings, sink).ok(),
          "exact scene reservation");
  ExecutionContext short_budget(operations,
                                {2, false, 16, stats.planned_peak_bytes - 1});
  auto failed =
      short_budget.execute_stream(compiled.value().plan, bindings, sink);
  require(!failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
          "one byte short scene reservation");
  std::cout << "S2Image.RegionAndTiles tiles=" << stats.tile_count
            << " source_bytes=" << stats.source_read_bytes
            << " planned_peak=" << stats.planned_peak_bytes
            << " actual_peak=" << stats.peak_live_bytes << " oracle=passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    require(argc <= 2,
            "usage: photospider_regional_image_vertical [trusted-module]");
    auto operations = argc == 2 ? std::make_shared<ps::OperationRegistry>()
                                : ps::make_default_operation_registry();
    if (argc == 2) {
      const auto status = operations->load_plugin(argv[1]);
      require(status.ok(), status.message);
      operations->freeze();
    }
    direct_invocation(operations);
    numerical(operations);
    large_source(operations);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
