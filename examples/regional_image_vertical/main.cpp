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

#include "image_vertical/image_fixture.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, const std::string& detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
using s1_fixture::take;
ValueFacet profile() {
  return s1_fixture::profile();
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
  ExecutionBindings bindings(const ResourceBudget& root) const {
    const std::vector<std::uint64_t> shape{height, width, 4};
    return {{{"foreground", s1_fixture::tensor(root, foreground, shape)},
             {"background", s1_fixture::tensor(root, background, shape)},
             {"mask", s1_fixture::tensor(root, mask, {height, width}, false)},
             {"gain", s1_fixture::scalar(root, gain)}}};
  }
  WorkflowDocument document() const {
    WorkflowDocument document;
    document.inputs = {
        s1_fixture::declaration(1, "foreground",
                                s1_fixture::schema({height, width, 4})),
        s1_fixture::declaration(2, "background",
                                s1_fixture::schema({height, width, 4})),
        s1_fixture::declaration(3, "mask",
                                s1_fixture::schema({height, width}, false)),
        s1_fixture::declaration(4, "gain", s1_fixture::schema({1}, false))};
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
void check(const ResultRef& output, const std::vector<float>& expected,
           std::uint64_t width, bool exact) {
  const auto& tensor = output.schema().tensors[0];
  require(tensor.facets.size() == 1 &&
              tensor.facets[0].payload == profile().payload,
          "image profile changed");
  const auto descriptor = take(output.descriptor());
  s1_fixture::check(descriptor.tensor_coverage(0).visit(
      [&](const auto& at) {
        float actual = 0;
        auto status = output.read_tensor(descriptor, 0, at, &actual, 4);
        if (!status.ok())
          return status;
        const float reference = expected[(at[2] * width + at[3]) * 4 + at[4]];
        require(exact ? std::memcmp(&actual, &reference, 4) == 0
                      : std::abs(actual - reference) <=
                            1e-6 + 1e-5 * std::abs(reference),
                "regional image oracle mismatch");
        return Status::success();
      },
      UINT64_MAX));
}
std::vector<float> pixels(const ResultRef& output) {
  std::vector<float> values;
  auto descriptor = take(output.descriptor());
  s1_fixture::check(descriptor.tensor_coverage(0).visit(
      [&](const auto& at) {
        float value = 0;
        auto status = output.read_tensor(descriptor, 0, at, &value, 4);
        if (status.ok())
          values.push_back(value);
        return status;
      },
      UINT64_MAX));
  return values;
}
ResultRef affine(const ResourceBudget& root, const SchemaTemplate& schema,
                 const Region& region, StridedLayout layout,
                 const std::vector<float>& values) {
  auto storage = take(root.allocator().allocate(values.size() * 4));
  std::memcpy(storage.data(), values.data(), values.size() * 4);
  auto builder = take(ResultBuilder::start(root, schema, "example.affine"));
  s1_fixture::check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  const auto count = schema.tensors[0].sample_count();
  auto witness = take(ResultRelation::cartesian(
      root, count.ok() ? count.value() : UINT64_MAX, {}));
  s1_fixture::check(builder.publish_tensor(
      0, region, std::move(layout), std::move(storage).freeze(),
      std::move(witness), {true, true, true, true}));
  return take(builder.seal());
}
void input_windows(const std::shared_ptr<OperationRegistry>& operations) {
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(operations, config);
  const auto root = take(execution.resource_budget());
  const auto schema = s1_fixture::schema({3, 3, 4});
  auto run = [&](const ResultRef& source, const std::string& operation,
                 const Region& demand,
                 const std::map<std::string, ParameterValue>& parameters,
                 bool exposure) {
    WorkflowDocument document;
    document.inputs = {s1_fixture::declaration(1, "image", source)};
    WorkflowNode node{1, operation, {WorkflowInputReference{1}}, parameters};
    ExecutionBindings bindings{{{"image", source}}};
    if (exposure) {
      auto gain = s1_fixture::scalar(root, 2);
      document.inputs.push_back(s1_fixture::declaration(2, "gain", gain));
      bindings.inputs.push_back({"gain", gain});
      node.inputs.push_back(WorkflowInputReference{2});
    }
    document.nodes.push_back(std::move(node));
    document.outputs = {
        {"result", 1,
         operation == "image.split_horizontal" ? "full" : "value"}};
    GraphContext graph(document);
    PlanningOptions options;
    options.output_regions = {{"result", demand}};
    auto compiled = Compiler(operations).compile(graph, options);
    if (!compiled.ok())
      return Result<ExecutionResult>(compiled.status());
    return execution.execute(compiled.value().plan, std::move(bindings));
  };
  const auto whole = Region::whole(schema.tensors[0].sample_shape());
  for (bool broadcast : {true, false}) {
    auto source = affine(root, schema, whole,
                         broadcast ? StridedLayout{0, {0, 0, 0, 0, 4}}
                                   : StridedLayout{128, {0, 0, -48, -16, 4}},
                         std::vector<float>(broadcast ? 4 : 36, .5F));
    auto result = take(run(source, "image.exposure_gain", whole, {}, true));
    const auto samples = pixels(result.results.at("result"));
    for (std::size_t i = 0; i < samples.size(); ++i)
      require(samples[i] == (i % 4 == 3 ? .5F : 1.F),
              "broadcast/reversed Result input");
    auto closed =
        take(run(source, "image.exposure_gain",
                 Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}}), {}, true));
    require(pixels(closed.results.at("result")) ==
                std::vector<float>({1, 1, 1, .5F}),
            "Result image demand closes over the complete RGBA tuple");
  }
  const Region row({{0, 1}, {0, 1}, {1, 1}, {0, 3}, {0, 4}});
  const Region center({{0, 1}, {0, 1}, {1, 1}, {1, 1}, {0, 4}});
  const std::map<std::string, ParameterValue> parameters{{"radius", int64_t{1}},
                                                         {"sigma", 1.0}};
  auto short_source = affine(root, schema, row, {0, {0, 0, 0, 0, 4}},
                             std::vector<float>(4, .5F));
  require(
      !run(short_source, "image.gaussian_blur", center, parameters, false).ok(),
      "Gaussian must reject unpublished halo");
  auto source = affine(root, schema, whole, {0, {0, 0, 0, 0, 4}},
                       std::vector<float>(4, .5F));
  require(run(source, "image.gaussian_blur", center, parameters, false).ok(),
          "complete Gaussian halo");
  auto huge = s1_fixture::schema({UINT64_MAX, 1, 4});
  const Region last_rows({{0, 1}, {0, 1}, {UINT64_MAX - 4, 4}, {0, 1}, {0, 4}});
  auto far = affine(root, huge, last_rows, {0, {0, 0, 0, 0, 4}},
                    std::vector<float>(4, .5F));
  auto edge =
      take(run(far, "image.gaussian_blur",
               Region({{0, 1}, {0, 1}, {UINT64_MAX - 1, 1}, {0, 1}, {0, 4}}),
               {{"radius", int64_t{3}}, {"sigma", 1.0}}, false));
  require(pixels(edge.results.at("result")) == std::vector<float>(4, .5F),
          "uint64 edge clamp");
  auto last_box =
      affine(root, huge,
             Region({{0, 1}, {0, 1}, {UINT64_MAX - 15, 15}, {0, 1}, {0, 4}}),
             {0, {0, 0, 0, 0, 4}}, std::vector<float>(4, .5F));
  auto downsampled =
      take(run(last_box, "image.downsample_box",
               Region({{0, 1}, {0, 1}, {UINT64_MAX / 16, 1}, {0, 1}, {0, 4}}),
               {{"factor", int64_t{16}}}, false));
  require(
      pixels(downsampled.results.at("result")) == std::vector<float>(4, .5F),
      "downsample clips its final uint64 box without multiplication overflow");
  const auto tall = s1_fixture::schema({128, 129, 4});
  auto tall_source =
      affine(root, tall, Region::whole(tall.tensors[0].sample_shape()),
             {0, {0, 0, 0, 0, 4}}, std::vector<float>(4, .5F));
  auto split = take(run(tall_source, "image.split_horizontal",
                        Region::whole(tall.tensors[0].sample_shape()),
                        {{"split_x", int64_t{64}}}, false));
  require(pixels(split.results.at("result")) ==
              std::vector<float>(128 * 129 * 4, .5F),
          "retained CPU output can exceed the transient workspace limit");
}
void numerical(const std::shared_ptr<OperationRegistry>& operations) {
  Compiler compiler(operations);
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(operations, config);
  const auto root = take(execution.resource_budget());
  for (bool uniform : {true, false}) {
    Fixture fixture(uniform);
    GraphContext graph(fixture.document());
    auto compiled = compiler.compile(graph);
    require(compiled.ok(), compiled.status().message);
    auto full =
        execution.execute(compiled.value().plan, fixture.bindings(root));
    require(full.ok(), full.status().message);
    check(full.value().results.at("result"), fixture.oracle(), fixture.width,
          false);
    const auto reference = pixels(full.value().results.at("result"));
    if (uniform) {
      for (std::size_t i = 0; i < reference.size(); ++i)
        require(reference[i] == (i % 4 == 3 ? .625F : .3125F),
                "hand-computed uniform fixture");
    }
    for (const auto& geometry :
         {std::make_pair(1, 1), std::make_pair(2, 4), std::make_pair(4, 8),
          std::make_pair(128, 128)}) {
      for (bool roi : {false, true}) {
        PlanningOptions options;
        options.tile_height = geometry.first;
        options.tile_width = geometry.second;
        if (roi)
          options.output_regions = {
              {"result", Region({{0, 1}, {0, 1}, {1, 5}, {2, 7}, {0, 4}})}};
        auto planned = compiler.plan(compiled.value().optimized, options);
        require(planned.ok(), planned.status().message);
        auto output =
            execution.execute(planned.value(), fixture.bindings(root));
        require(output.ok(), output.status().message);
        check(output.value().results.at("result"), reference, fixture.width,
              true);
        check(output.value().results.at("result"), fixture.oracle(),
              fixture.width, false);
        unsigned publications = 0;
        ExecutionOptions observe;
        observe.result_publication = [&](const auto& name,
                                         const ResultRef& result) {
          if (name.node_id == 40) {
            check(result, reference, fixture.width, true);
            ++publications;
          }
          return Status::success();
        };
        auto observed = execution.execute(planned.value(),
                                          fixture.bindings(root), {}, observe);
        require(observed.ok(), observed.status().message);
        require(publications == 1 &&
                    observed.value().diagnostics.operation_timings.size() == 4,
                "Result publication observer and operation diagnostics");
      }
    }
    fixture.gain = .5F;
    auto second =
        execution.execute(compiled.value().plan, fixture.bindings(root));
    require(second.ok(), second.status().message);
    check(second.value().results.at("result"), fixture.oracle(), fixture.width,
          false);
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
    auto output =
        execution.execute(compiled.value().plan, fixture.bindings(root));
    require(!output.ok() && output.status().code == ErrorCode::InvalidArgument,
            "mask interval");
  }
  for (int axis = 0; axis < 2; ++axis) {
    auto mismatch = fixture.document();
    auto& input = mismatch.inputs[axis == 0 ? 1 : 2];
    auto schema = std::make_shared<SchemaTemplate>(*input.result_schema);
    --schema->tensors[0].descriptor.shape[0];
    input.result_schema = std::move(schema);
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
      execution.execute(impulse_plan.value().plan, impulse.bindings(root));
  require(impulse_result.ok(), impulse_result.status().message);
  check(impulse_result.value().results.at("result"), impulse.oracle(),
        impulse.width, false);
  // Radius can exceed both image axes and the tile; clamp remains image-local.
  Fixture wide_halo(true);
  wide_halo.radius = 64;
  wide_halo.sigma = 64;
  GraphContext wide_graph(wide_halo.document());
  PlanningOptions options;
  options.tile_height = options.tile_width = 1;
  options.output_regions = {
      {"result", Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})}};
  auto wide_plan = compiler.compile(wide_graph, options);
  require(wide_plan.ok(), wide_plan.status().message);
  auto wide =
      execution.execute(wide_plan.value().plan, wide_halo.bindings(root));
  require(wide.ok(), wide.status().message);
  const auto reference = pixels(wide.value().results.at("result"));
  require(reference == std::vector<float>({.3125F, .3125F, .3125F, .625F}),
          "radius 64 clamp");
}

void large_source(const std::shared_ptr<OperationRegistry>& operations) {
  auto run = [&](uint64_t limit) -> Result<uint64_t> {
    ExecutionContextConfig config;
    config.gpu_enabled = false;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Payload] = limit;
    ExecutionContext execution(operations, config);
    const auto root = take(execution.resource_budget());
    Fixture fixture(true);
    auto document = fixture.document();
    const uint64_t side = 65536;
    ExecutionBindings bindings;
    for (auto& input : document.inputs) {
      if (input.name == "gain") {
        bindings.inputs.push_back({input.name, s1_fixture::scalar(root, 2)});
        continue;
      }
      const bool mask = input.name == "mask", back = input.name == "background";
      const auto schema =
          s1_fixture::schema(mask ? std::vector<uint64_t>{side, side}
                                  : std::vector<uint64_t>{side, side, 4},
                             !mask);
      input.result_schema = std::make_shared<SchemaTemplate>(schema);
      auto source =
          affine(root, schema, Region::whole(schema.tensors[0].sample_shape()),
                 mask ? StridedLayout{0, {0, 0, 0, 0}}
                      : StridedLayout{0, {0, 0, 0, 0, 4}},
                 mask   ? std::vector<float>{.5F}
                 : back ? std::vector<float>{.25F, .25F, .25F, .5F}
                        : std::vector<float>{.125F, .125F, .125F, .5F});
      bindings.inputs.push_back({input.name, std::move(source)});
    }
    GraphContext graph(document);
    PlanningOptions options;
    options.tile_height = 2;
    options.tile_width = 4;
    const Region roi({{0, 1}, {0, 1}, {100, 5}, {200, 7}, {0, 4}});
    options.output_regions = {{"result", roi}};
    auto compiled = take(Compiler(operations).compile(graph, options));
    auto output = execution.execute(compiled.plan, bindings);
    if (!output.ok())
      return Result<uint64_t>(output.status());
    const auto samples = pixels(output.value().results.at("result"));
    require(samples.size() == 5 * 7 * 4, "large source ROI coverage");
    for (size_t i = 0; i < samples.size(); ++i)
      require(samples[i] == (i % 4 == 3 ? .625F : .3125F),
              "large source oracle");
    auto supports = take(output.value().dependencies.source_support());
    require(take(supports.at("foreground").element_count()) == 11 * 13 * 4,
            "Gaussian source support stays inside ROI halo");
    return Result<uint64_t>(root.statistics().peak[ResourceKind::Payload]);
  };
  const auto peak = take(run(1024 * 1024));
  require(peak < 16384,
          "large logical source must use bounded payload storage");
  require(run(peak).ok(), "exact observed payload budget");
  auto failed = run(peak - 1);
  require(!failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
          "one byte below observed payload budget");
  std::cout << "regional Result image ROI=5x7 logical_source=65536x65536 "
               "peak_payload="
            << peak << " oracle=passed\n";
}

}  // namespace
int main(int argc, char**) {
  try {
    require(argc == 1, "usage: photospider_regional_image_vertical");
    auto operations = ps::make_default_operation_registry();
    input_windows(operations);
    numerical(operations);
    large_source(operations);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
