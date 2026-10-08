#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
Value data(ElementType type, std::vector<std::uint64_t> shape,
           const std::vector<T>& samples, std::vector<ValueFacet> facets = {}) {
  auto writer = MutableValue::allocate({type, shape}, Region::whole(shape),
                                       BufferAllocator{})
                    .take_value();
  if (writer.size() != samples.size() * sizeof(T))
    throw std::invalid_argument("fixture size");
  std::memcpy(writer.data(), samples.data(), writer.size());
  return std::move(writer).publish(std::move(facets)).take_value();
}
Footprint footprint(const std::vector<std::uint64_t>& shape,
                    const Region& region) {
  return Footprint::from_regions(shape, {region}).take_value();
}
struct RadiusDriver {
  std::shared_ptr<OperationRegistry> registry;
  ExecutionContext context;
  ResourceBudget root;
  explicit RadiusDriver(std::shared_ptr<OperationRegistry> operations)
      : registry(operations),
        context(operations),
        root(context.resource_budget().take_value()) {}
  Result<DemandResult> run(const std::string& key,
                           const std::vector<ResultRef>& inputs,
                           const Footprint& query,
                           const ExecutionOptions& options = {},
                           const CancellationToken& cancellation = {}) {
    WorkflowDocument document;
    ExecutionBindings bindings;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration input;
      input.id = i + 1;
      input.name = "input" + std::to_string(i);
      input.result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].schema());
      document.inputs.push_back(input);
      bindings.inputs.push_back({input.name, inputs[i]});
    }
    document.nodes = {
        {1, key, {WorkflowInputReference{1}, WorkflowInputReference{2}}, {}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(document);
    auto compiled = Compiler(registry).compile(graph);
    if (!compiled.ok())
      return Result<DemandResult>(compiled.status());
    auto frozen = context.freeze(compiled.value().plan, bindings);
    if (!frozen.ok())
      return Result<DemandResult>(frozen.status());
    return context.execute_fragments(frozen.value(), {{"result", query}},
                                     cancellation, options);
  }
  ResultRef input(const Value& value) {
    return numeric_result_fixture::source(root, value);
  }
};
Result<DemandResult> resolve(const std::shared_ptr<OperationRegistry>& registry,
                             const std::string& key,
                             const std::vector<Value>& inputs,
                             const Footprint& query) {
  RadiusDriver driver(registry);
  std::vector<ResultRef> results;
  for (const auto& value : inputs)
    results.push_back(driver.input(value));
  return driver.run(key, results, query);
}
Footprint support(const DemandResult& result, std::uint32_t port,
                  DependencyRole role) {
  auto found = result.dependencies.source_observations().take_value();
  const auto name = "input" + std::to_string(port);
  for (const auto& observation : found)
    if (std::string(observation.input.data(), observation.input.size()) ==
            name &&
        observation.target == ResultSupportTarget::Tensor &&
        (observation.roles & static_cast<std::uint32_t>(role)))
      return observation.samples;
  throw std::runtime_error("missing radius support");
}
int radius_oracles(const std::shared_ptr<OperationRegistry>& registry) {
  std::mt19937 random(20260911);
  for (unsigned trial = 0; trial < 80; ++trial) {
    const std::uint64_t n = 1 + random() % 8;
    std::vector<double> samples(n);
    std::vector<std::int64_t> radii(n);
    for (std::uint64_t i = 0; i < n; ++i) {
      samples[i] = static_cast<int>(random() % 9) - 4;
      radii[i] = random() % 4;
    }
    const std::vector<Value> inputs{data(ElementType::Float64, {n}, samples),
                                    data(ElementType::Int64, {n}, radii)};
    for (bool scatter : {false, true})
      for (std::uint64_t o = 0; o < n; ++o) {
        auto result = resolve(
            registry,
            scatter ? "numeric.radius_scatter" : "numeric.radius_gather",
            inputs, footprint({n}, Region({{o, 1}})));
        PS_CHECK(result.ok());
        double expected = 0, actual = 0;
        std::vector<Region> positive;
        for (std::uint64_t i = 0; i < n; ++i) {
          const auto distance = i > o ? i - o : o - i;
          if (distance <= static_cast<std::uint64_t>(radii[scatter ? i : o])) {
            expected += samples[i];
            positive.emplace_back(Region({{i, 1}}));
          }
        }
        PS_CHECK(numeric_result_fixture::read(
                     result.value().results.at("result"), {o}, &actual, 8)
                     .ok() &&
                 actual == expected);
        PS_CHECK(support(result.value(), 0, DependencyRole::Data) ==
                 Footprint::from_regions({n}, positive).take_value());
        const auto controls = scatter ? Region::whole({n}) : Region({{o, 1}});
        PS_CHECK(support(result.value(), 1, DependencyRole::Control) ==
                 footprint({n}, controls));
      }
  }
  // A remote radius edit adds a new edge, even if the output bytes stay equal.
  const auto source =
      data(ElementType::Float64, {4}, std::vector<double>{1, 2, 3, 0});
  const auto old_radius =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, 0, 0});
  const auto new_radius =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, 0, 3});
  const auto q = footprint({4}, Region({{0, 1}}));
  auto old =
      resolve(registry, "numeric.radius_scatter", {source, old_radius}, q)
          .take_value();
  auto changed =
      resolve(registry, "numeric.radius_scatter", {source, new_radius}, q)
          .take_value();
  double before = 0, after = 0;
  PS_CHECK(
      numeric_result_fixture::read(old.results.at("result"), {0}, &before, 8)
          .ok() &&
      numeric_result_fixture::read(changed.results.at("result"), {0}, &after, 8)
          .ok() &&
      before == after);
  PS_CHECK(support(old, 0, DependencyRole::Data) !=
           support(changed, 0, DependencyRole::Data));
  PS_CHECK(old.dependencies
               .potential_dirty("input1", footprint({4}, Region({{3, 1}})), 2,
                                {}, ResultSupportTarget::Tensor, 0)
               .take_value()
               .at("result") == q);
  auto bad =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, -1, 0});
  PS_CHECK(resolve(registry, "numeric.radius_scatter", {source, bad}, q)
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(resolve(registry, "numeric.radius_gather", {source, bad}, q).ok());
  // More than one control/data chunk preserves source order exactly.
  std::vector<double> long_data(130, 0);
  long_data[0] = 1e16;
  long_data[64] = -1e16;
  long_data[129] = 1;
  std::vector<std::int64_t> long_radius(130, 130);
  auto folded = resolve(registry, "numeric.radius_scatter",
                        {data(ElementType::Float64, {130}, long_data),
                         data(ElementType::Int64, {130}, long_radius)},
                        footprint({130}, Region({{0, 1}})))
                    .take_value();
  PS_CHECK(
      numeric_result_fixture::read(folded.results.at("result"), {0}, &after, 8)
          .ok() &&
      after == 1);
  return 0;
}
int static_and_rounding(const std::shared_ptr<OperationRegistry>& registry) {
  for (bool empty : {false, true}) {
    auto q = empty ? Footprint::none({3}).take_value()
                   : footprint({3}, Region({{0, 1}}));
    PS_CHECK(
        resolve(
            registry, "numeric.radius_gather",
            {data(ElementType::Float64, {3}, std::vector<double>{0, 0, 0}),
             data(ElementType::Int64, {2}, std::vector<std::int64_t>{0, 0})},
            q)
            .status()
            .code == ErrorCode::TypeMismatch);
  }
  struct Restore {
    int mode = std::fegetround();
    ~Restore() { std::fesetround(mode); }
  } restore;
  const auto input =
      data(ElementType::Float64, {3}, std::vector<double>{1e16, 1, -1e16});
  const auto radii =
      data(ElementType::Int64, {3}, std::vector<std::int64_t>{2, 2, 2});
  std::vector<double> long_samples(130, 0);
  long_samples[0] = 1e16;
  long_samples[1] = 1;
  long_samples[64] = -1e16;
  long_samples[129] = 1;
  const auto long_input = data(ElementType::Float64, {130}, long_samples);
  const auto long_radii =
      data(ElementType::Int64, {130}, std::vector<std::int64_t>(130, 130));
  for (const auto mode :
       {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(mode) == 0);
    for (const std::string key :
         {"numeric.radius_gather", "numeric.radius_scatter"}) {
      auto result = resolve(registry, key, {input, radii},
                            footprint({3}, Region({{1, 1}})))
                        .take_value();
      double value = 7;
      PS_CHECK(numeric_result_fixture::read(result.results.at("result"), {1},
                                            &value, 8)
                   .ok() &&
               value == 0 && !std::signbit(value));
      auto longer = resolve(registry, key, {long_input, long_radii},
                            footprint({130}, Region({{0, 1}})))
                        .take_value();
      PS_CHECK(numeric_result_fixture::read(longer.results.at("result"), {0},
                                            &value, 8)
                   .ok() &&
               value == 1);
      PS_CHECK(std::fegetround() == mode);
    }
  }
  return 0;
}

int radius_contracts(const std::shared_ptr<OperationRegistry>& registry) {
  RadiusDriver driver(registry);
  auto source = driver.input(
      data(ElementType::Float64, {3}, std::vector<double>{1, 2, NAN}));
  auto radius = driver.input(
      data(ElementType::Int64, {3}, std::vector<std::int64_t>{0, 0, 0}));
  const auto q = footprint({3}, Region({{0, 1}}));
  for (const std::string key :
       {"numeric.radius_gather", "numeric.radius_scatter"}) {
    auto selected = driver.run(key, {source, radius}, q);
    PS_CHECK(selected.ok());
    double value = 0;
    PS_CHECK(numeric_result_fixture::read(selected.value().results.at("result"),
                                          {0}, &value, 8)
                 .ok() &&
             value == 1);
    PS_CHECK(selected.value().results.at("result").association().size() == 2);
    auto bad =
        driver.run(key, {source, radius}, footprint({3}, Region({{2, 1}})));
    PS_CHECK(!bad.ok() && bad.status().code == ErrorCode::OperationFailed);
    const auto partial_baseline =
        driver.root.statistics().live[ResourceKind::Payload];
    auto partial =
        driver.run(key, {source, radius}, Footprint::all({3}).take_value());
    PS_CHECK(!partial.ok() &&
             partial.status().code == ErrorCode::OperationFailed &&
             driver.root.statistics().live[ResourceKind::Payload] ==
                 partial_baseline);
    ExecutionOptions options;
    options.maximum_dependency_work = 1;
    const auto baseline = driver.root.statistics().live[ResourceKind::Payload];
    auto exhausted = driver.run(key, {source, radius}, q, options);
    PS_CHECK(!exhausted.ok() &&
             exhausted.status().code == ErrorCode::ResourceExhausted &&
             driver.root.statistics().live[ResourceKind::Payload] == baseline);
    CancellationSource stop;
    stop.cancel();
    auto cancelled = driver.run(key, {source, radius}, q, {}, stop.token());
    PS_CHECK(!cancelled.ok() &&
             cancelled.status().code == ErrorCode::Cancelled);
  }
  SemanticDescriptor semantic;
  semantic.kind = SemanticKind::SampledSignal;
  semantic.sample_step = 1;
  semantic.channels = {{"value", "value", "dimensionless"}};
  semantic.sample_axis_unit = "seconds";
  auto typed = driver.input(data(ElementType::Float64, {3},
                                 std::vector<double>{1, 2, NAN},
                                 {encode_semantic(semantic).take_value()}));
  auto opaque =
      driver.input(data(ElementType::Float64, {3}, std::vector<double>{1, 2, 3},
                        {{"fixture.opaque", 1, {7}}}));
  for (const std::string key :
       {"numeric.radius_gather", "numeric.radius_scatter"}) {
    auto typed_good = driver.run(key, {typed, radius}, q);
    PS_CHECK(typed_good.ok());
    auto typed_bad =
        driver.run(key, {typed, radius}, footprint({3}, Region({{2, 1}})));
    PS_CHECK(!typed_bad.ok() &&
             typed_bad.status().code == ErrorCode::InvalidArgument &&
             typed_bad.status().detail.input_id == 1);
    auto empty =
        driver.run(key, {typed, radius}, Footprint::none({3}).take_value());
    PS_CHECK(
        empty.ok() &&
        empty.value().dependencies.source_observations().take_value().empty());
    auto metadata = driver.run(key, {opaque, radius}, q);
    PS_CHECK(metadata.ok() && metadata.value()
                                  .results.at("result")
                                  .schema()
                                  .tensors[0]
                                  .facets.empty());
  }
  auto overflow = driver.input(data(
      ElementType::Float64, {3},
      std::vector<double>{0x1.fffffffffffffp1023, 0x1.fffffffffffffp1023, 0}));
  auto full_radius = driver.input(
      data(ElementType::Int64, {3}, std::vector<std::int64_t>{3, 3, 3}));
  PS_CHECK(driver.run("numeric.radius_gather", {overflow, full_radius}, q)
               .status()
               .code == ErrorCode::OperationFailed);
  constexpr std::uint64_t extent = UINT64_C(1) << 40;
  auto broadcast = [&](ElementType type, const void* value) {
    SchemaTemplate schema;
    schema.id = "radius.broadcast";
    ResultTensorSpec tensor;
    tensor.key = "source";
    tensor.descriptor = {type, {extent}};
    schema.tensors.push_back(tensor);
    auto bytes = driver.root.allocator().allocate(8).take_value();
    std::memcpy(bytes.data(), value, 8);
    auto builder =
        ResultBuilder::start(driver.root, schema, "radius.huge").take_value();
    numeric_result_fixture::require(
        builder
            .bind_descriptor_relation(
                ResultRelation::cartesian(driver.root, 1, {}).take_value())
            .ok(),
        "broadcast descriptor");
    numeric_result_fixture::require(
        builder
            .publish_tensor(
                0, Region::whole({extent}), {0, {0}}, std::move(bytes).freeze(),
                ResultRelation::cartesian(driver.root, extent, {}).take_value(),
                {true, true, true, true})
            .ok(),
        "broadcast payload");
    return builder.seal().take_value();
  };
  const double seven = 7;
  const std::int64_t zero = 0;
  auto huge_source = broadcast(ElementType::Float64, &seven);
  auto huge_radius = broadcast(ElementType::Int64, &zero);
  auto huge = driver.run("numeric.radius_gather", {huge_source, huge_radius},
                         footprint({extent}, Region({{extent - 1, 1}})));
  PS_CHECK(huge.ok());
  double value = 0;
  PS_CHECK(numeric_result_fixture::read(huge.value().results.at("result"),
                                        {extent - 1}, &value, 8)
               .ok() &&
           value == 7);
  auto remote = footprint({extent}, Region({{0, 1}}));
  PS_CHECK(huge.value()
               .dependencies
               .potential_dirty("input1", remote, 2, {},
                                ResultSupportTarget::Tensor, 0)
               .take_value()
               .at("result")
               .empty());
  auto many = driver.input(
      data(ElementType::Float64, {100000}, std::vector<double>(100000, 1)));
  auto radii = driver.input(
      data(ElementType::Int64, {100000}, std::vector<std::int64_t>(100000, 0)));
  const auto baseline = driver.root.statistics().live[ResourceKind::Payload];
  const auto work = driver.root.statistics().issued.work;
  CancellationSource stop;
  auto active = std::async(std::launch::async, [&] {
    return driver.run("numeric.radius_scatter", {many, radii},
                      footprint({100000}, Region({{0, 1}})), {}, stop.token());
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (driver.root.statistics().issued.work < work + 10000 &&
         active.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool progressed = driver.root.statistics().issued.work >= work + 10000;
  stop.cancel();
  auto interrupted = active.get();
  PS_CHECK(progressed && !interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled &&
           driver.root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
}

SchemaTemplate image_schema(std::uint64_t height, std::uint64_t width,
                            std::uint64_t frames = 1) {
  SchemaTemplate schema;
  schema.id = "photospider.image";
  ResultTensorSpec pixels;
  pixels.key = "pixels";
  pixels.batch_axes = {frames, 1};
  pixels.descriptor = {ElementType::Float32, {height, width, 4}};
  pixels.layout.spatial = true;
  pixels.facets = {encode_semantic(rgba_semantics()).take_value()};
  schema.tensors.push_back(pixels);
  return schema;
}
struct StmapDriver {
  std::shared_ptr<OperationRegistry> registry;
  ExecutionContext context;
  ResourceBudget root;
  explicit StmapDriver(std::shared_ptr<OperationRegistry> r)
      : registry(r), context(r), root(context.resource_budget().take_value()) {}
  ResultRef source(const SchemaTemplate& schema,
                   const std::vector<float>& pixels) {
    auto builder = numeric_result_fixture::take(
        ResultBuilder::start(root, schema, "stmap.input"));
    numeric_result_fixture::require(
        builder
            .bind_descriptor_relation(numeric_result_fixture::take(
                ResultRelation::cartesian(root, 1, {})))
            .ok(),
        "image descriptor");
    numeric_result_fixture::require(
        builder
            .publish_tensor(
                0, Region::whole(schema.tensors[0].sample_shape()),
                ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                         pixels.size() * 4),
                numeric_result_fixture::take(
                    ResultRelation::cartesian(root, pixels.size(), {})),
                {true, true, true, true})
            .ok(),
        "source image");
    return numeric_result_fixture::take(builder.seal());
  }
  Result<DemandResult> run(ResultRef source, ResultRef map,
                           const std::string& boundary, const Footprint& q,
                           ExecutionOptions options = {},
                           const CancellationToken& cancellation = {}) {
    WorkflowDocument doc;
    doc.inputs.resize(2);
    const std::array<ResultRef, 2> inputs{source, map};
    ExecutionBindings bindings;
    for (std::size_t i = 0; i < 2; ++i) {
      doc.inputs[i].id = i + 1;
      doc.inputs[i].name = "input" + std::to_string(i);
      doc.inputs[i].result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].schema());
      bindings.inputs.push_back({doc.inputs[i].name, inputs[i]});
    }
    doc.nodes = {{1,
                  "image.stmap",
                  {WorkflowInputReference{1}, WorkflowInputReference{2}},
                  {{"boundary", boundary}}}};
    doc.outputs = {{"result", 1, "value"}};
    GraphContext graph(doc);
    auto compiled = Compiler(registry).compile(graph);
    if (!compiled.ok())
      return Result<DemandResult>(compiled.status());
    auto frozen = context.freeze(compiled.value().plan, bindings);
    if (!frozen.ok())
      return Result<DemandResult>(frozen.status());
    return context.execute_fragments(frozen.value(), {{"result", q}},
                                     cancellation, options);
  }
  ResultRef map(const std::vector<double>& values,
                std::vector<std::uint64_t> shape = {1, 1, 2}) {
    return numeric_result_fixture::source(
        root, data(ElementType::Float64, shape, values));
  }
};
int stmap_results(const std::shared_ptr<OperationRegistry>& registry) {
  StmapDriver driver(registry);
  auto image =
      driver.source(image_schema(1, 3), {1, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 1});
  auto q = Footprint::all({1, 1, 1, 1, 4}).take_value();
  auto point_map = driver.map({0, .5});
  for (const auto& test :
       std::vector<std::pair<std::string, float>>{{"constant", .5F},
                                                  {"clamp", 1},
                                                  {"wrap", 2.5F},
                                                  {"reflect", 1},
                                                  {"mirror", 1.5F}}) {
    auto result = driver.run(image, point_map, test.first, q);
    PS_CHECK(result.ok());
    float value = 0;
    auto& output = result.value().results.at("result");
    PS_CHECK(output
                 .read_tensor(output.descriptor().take_value(), 0,
                              {0, 0, 0, 0, 0}, &value, 4)
                 .ok() &&
             value == test.second);
  }
  auto selected = footprint({1, 1, 1, 1, 4},
                            Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}}));
  auto result = driver.run(image, driver.map({1.5, .5}), "clamp", selected);
  PS_CHECK(result.ok());
  auto supports = result.value().dependencies.source_support().take_value();
  PS_CHECK(supports.at("input1") == Footprint::all({1, 1, 2}).take_value());
  // The neighbor at x=2 has zero weight but remains a declared source tap.
  const auto tap = footprint({1, 1, 1, 3, 4},
                             Region({{0, 1}, {0, 1}, {0, 1}, {2, 1}, {0, 4}}));
  auto dirty = result.value().dependencies.potential_dirty(
      "input0", tap, 1, {}, ResultSupportTarget::Tensor, 0);
  PS_CHECK(dirty.ok() && dirty.value().at("result") == q);
  auto outside = driver.run(image, driver.map({-100, -100}), "constant", q);
  PS_CHECK(outside.ok());
  float transparent = 9;
  const auto& black = outside.value().results.at("result");
  PS_CHECK(black
               .read_tensor(black.descriptor().take_value(), 0, {0, 0, 0, 0, 3},
                            &transparent, 4)
               .ok() &&
           transparent == 0);
  const auto outside_support =
      outside.value().dependencies.source_support().take_value();
  PS_CHECK(!outside_support.count("input0") ||
           outside_support.at("input0").empty());
  PS_CHECK(driver.run(image, driver.map({NAN, .5}), "clamp", q).status().code ==
           ErrorCode::OperationFailed);
  PS_CHECK(
      driver.run(image, driver.map({0x1p41, .5}), "clamp", q).status().code ==
      ErrorCode::OperationFailed);
  PS_CHECK(driver.run(image, point_map, "bogus", q).status().code ==
           ErrorCode::InvalidArgument);
  auto empty = driver.run(image, driver.map({NAN, NAN}), "clamp",
                          Footprint::none(q.shape()).take_value());
  PS_CHECK(empty.ok() &&
           empty.value().dependencies.source_support().value().empty());
  auto frames = driver.source(image_schema(1, 1, 2), {1, 0, 0, 1, 3, 0, 0, 1});
  auto both = driver.run(frames, driver.map({.5, .5}), "clamp",
                         Footprint::all({2, 1, 1, 1, 4}).take_value());
  PS_CHECK(both.ok());
  float second = 0;
  const auto& batched = both.value().results.at("result");
  PS_CHECK(batched
               .read_tensor(batched.descriptor().take_value(), 0,
                            {1, 0, 0, 0, 0}, &second, 4)
               .ok() &&
           second == 3);
  auto grid = driver.map({.5, .5, 1.5, .5, 2.5, .5}, {1, 3, 2});
  auto copy = driver.run(image, grid, "clamp",
                         Footprint::all({1, 1, 1, 3, 4}).take_value());
  PS_CHECK(copy.ok());
  const auto& copied = copy.value().results.at("result");
  for (std::uint64_t x = 0; x < 3; ++x) {
    float value = 0;
    PS_CHECK(copied
                 .read_tensor(copied.descriptor().take_value(), 0,
                              {0, 0, 0, x, 0}, &value, 4)
                 .ok() &&
             value == std::vector<float>({1, 2, 4})[x]);
  }
  PS_CHECK(driver.run(image, driver.map({0, 0, 0}, {1, 1, 3}), "clamp", q)
               .status()
               .code == ErrorCode::TypeMismatch);
  PS_CHECK(driver
               .run(image, point_map, "bogus",
                    Footprint::none(q.shape()).take_value())
               .status()
               .code == ErrorCode::InvalidArgument);
  auto huge_schema = image_schema(UINT64_C(1) << 40, UINT64_C(1) << 40);
  auto owner =
      numeric_result_fixture::take(driver.root.allocator().allocate(16));
  const float color[] = {1, 2, 3, 1};
  std::memcpy(owner.data(), color, 16);
  auto builder = numeric_result_fixture::take(
      ResultBuilder::start(driver.root, huge_schema, "huge.image"));
  PS_CHECK(builder
               .bind_descriptor_relation(numeric_result_fixture::take(
                   ResultRelation::cartesian(driver.root, 1, {})))
               .ok());
  PS_CHECK(builder
               .publish_tensor(
                   0, Region::whole(huge_schema.tensors[0].sample_shape()),
                   {0, {0, 0, 0, 0, 4}}, std::move(owner).freeze(),
                   numeric_result_fixture::take(
                       ResultRelation::cartesian(driver.root, UINT64_MAX, {})),
                   {true, true, true, true})
               .ok());
  auto huge = numeric_result_fixture::take(builder.seal());
  auto large =
      driver.run(huge, driver.map({0x1p40 - .5, 0x1p40 - .5}), "clamp", q);
  PS_CHECK(large.ok());
  float blue = 0;
  const auto& sampled = large.value().results.at("result");
  PS_CHECK(sampled
               .read_tensor(sampled.descriptor().take_value(), 0,
                            {0, 0, 0, 0, 2}, &blue, 4)
               .ok() &&
           blue == 3);
  for (const auto exponent : {30U, 40U}) {
    const auto extent = UINT64_C(1) << exponent;
    auto map_schema = numeric_result_fixture::source_schema(
        data(ElementType::Float64, {1, 1, 2}, std::vector<double>{.5, .5}));
    map_schema.tensors[0].descriptor.shape = {extent, extent, 2};
    auto map_owner =
        numeric_result_fixture::take(driver.root.allocator().allocate(16));
    const double uv[] = {.5, .5};
    std::memcpy(map_owner.data(), uv, 16);
    auto map_builder = numeric_result_fixture::take(
        ResultBuilder::start(driver.root, map_schema, "huge.map"));
    PS_CHECK(map_builder
                 .bind_descriptor_relation(numeric_result_fixture::take(
                     ResultRelation::cartesian(driver.root, 1, {})))
                 .ok());
    auto map_count = map_schema.tensors[0].sample_count();
    PS_CHECK(map_builder
                 .publish_tensor(
                     0, Region::whole(map_schema.tensors[0].sample_shape()),
                     {0, {0, 0, 8}}, std::move(map_owner).freeze(),
                     numeric_result_fixture::take(ResultRelation::cartesian(
                         driver.root,
                         map_count.ok() ? map_count.value() : UINT64_MAX, {})),
                     {true, true, true, true})
                 .ok());
    auto huge_map = numeric_result_fixture::take(map_builder.seal());
    const auto far_q = footprint(
        {1, 1, extent, extent, 4},
        Region({{0, 1}, {0, 1}, {extent - 1, 1}, {extent - 1, 1}, {0, 4}}));
    auto far = driver.run(image, huge_map, "clamp", far_q);
    if (!far.ok())
      std::cerr << "large output: " << static_cast<unsigned>(far.status().code)
                << " " << far.status().message << '\n';
    PS_CHECK(far.ok());
    float far_value = 0;
    const auto& far_result = far.value().results.at("result");
    PS_CHECK(far_result
                 .read_tensor(far_result.descriptor().take_value(), 0,
                              {0, 0, extent - 1, extent - 1, 0}, &far_value, 4)
                 .ok() &&
             far_value == 1);
  }
  for (bool cancel_inside : {false, true}) {
    const auto before = driver.root.statistics().live[ResourceKind::Payload];
    {
      auto transaction = numeric_result_fixture::take(
          ResultBuilder::start(driver.root, huge_schema, "huge.transaction"));
      PS_CHECK(transaction
                   .bind_descriptor_relation(numeric_result_fixture::take(
                       ResultRelation::cartesian(driver.root, 1, {})))
                   .ok());
      CancellationSource cancel;
      bool entered = false;
      auto status = transaction.publish_tensor_kernel(
          0, Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}}),
          [&](const auto& windows) {
            entered = true;
            if (windows.size() != 1 || windows[0].sample_axis() != 3)
              return Status{ErrorCode::TypeMismatch,
                            "affine transaction window"};
            if (cancel_inside) {
              cancel.cancel();
              return Status::success();
            }
            return Status{ErrorCode::OperationFailed, "transaction abort"};
          },
          numeric_result_fixture::take(
              ResultRelation::cartesian(driver.root, UINT64_MAX, {})),
          {true, true, true, true}, cancel.token());
      PS_CHECK(entered && !status.ok() &&
               status.code == (cancel_inside ? ErrorCode::Cancelled
                                             : ErrorCode::OperationFailed));
    }
    PS_CHECK(driver.root.statistics().live[ResourceKind::Payload] == before);
  }
  auto full_map = driver.map(std::vector<double>(200, .5), {10, 10, 2});
  const auto payload_before =
      driver.root.statistics().live[ResourceKind::Payload];
  auto full = driver.run(image, full_map, "clamp",
                         Footprint::all({1, 1, 10, 10, 4}).take_value());
  PS_CHECK(full.ok() && driver.root.statistics().live[ResourceKind::Payload] -
                                payload_before <
                            UINT64_C(1048576));
  float last = 0;
  const auto& complete = full.value().results.at("result");
  PS_CHECK(complete
               .read_tensor(complete.descriptor().take_value(), 0,
                            {0, 0, 9, 9, 0}, &last, 4)
               .ok() &&
           last == 1);
  auto invalid =
      driver.source(image_schema(1, 3), {1, 0, 0, 1, 2, 0, 0, 1, NAN, 0, 0, 1});
  PS_CHECK(
      driver.run(invalid, driver.map({1.5, .5}), "clamp", q).status().code ==
      ErrorCode::InvalidArgument);
  PS_CHECK(
      driver.run(invalid, driver.map({NAN, .5}), "clamp", q).status().code ==
      ErrorCode::OperationFailed);
  auto grouped_schema = image_schema(1, 3);
  grouped_schema.tensors[0].atomic_trailing_axes = 2;
  auto grouped =
      driver.source(grouped_schema, {1, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 1});
  auto grouped_result = driver.run(grouped, driver.map({1.5, .5}), "clamp", q);
  PS_CHECK(grouped_result.ok());
  auto other = footprint({1, 1, 1, 3, 4},
                         Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}}));
  PS_CHECK(grouped_result.value()
               .dependencies
               .potential_dirty("input0", other, 1, {},
                                ResultSupportTarget::Tensor, 0)
               .value()
               .at("result")
               .empty());
  PS_CHECK(grouped_result.value()
               .dependencies
               .potential_dirty("input0", other, 4, {},
                                ResultSupportTarget::Tensor, 0)
               .value()
               .at("result") == q);
  auto tie = driver.source(image_schema(1, 2),
                           {1, 0, 0, 1, std::nextafter(1.F, 2.F), 0, 0, 1});
  auto tie_map = driver.map({1, .5});
  const auto rounding = std::fegetround();
  for (const auto mode :
       {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(mode) == 0);
    auto rounded = driver.run(tie, tie_map, "clamp", q);
    PS_CHECK(rounded.ok());
    float value = 0;
    const auto& tensor = rounded.value().results.at("result");
    PS_CHECK(tensor
                 .read_tensor(tensor.descriptor().take_value(), 0,
                              {0, 0, 0, 0, 0}, &value, 4)
                 .ok() &&
             value == 1);
    PS_CHECK(std::fegetround() == mode);
  }
  PS_CHECK(std::fesetround(rounding) == 0);
  ExecutionOptions exhausted;
  exhausted.maximum_dependency_work = 1;
  const auto baseline = driver.root.statistics().live[ResourceKind::Payload];
  auto failed = driver.run(image, point_map, "clamp", q, exhausted);
  PS_CHECK(!failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted &&
           driver.root.statistics().live[ResourceKind::Payload] == baseline);
  CancellationSource stop;
  stop.cancel();
  auto cancelled = driver.run(image, point_map, "clamp", q, {}, stop.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
  auto large_map = driver.map(std::vector<double>(5000, .5), {50, 50, 2});
  const auto cancel_baseline =
      driver.root.statistics().live[ResourceKind::Payload];
  const auto baseline_work = driver.root.statistics().issued.work;
  CancellationSource active_stop;
  auto active = std::async(std::launch::async, [&] {
    return driver.run(image, large_map, "clamp",
                      Footprint::all({1, 1, 50, 50, 4}).take_value(), {},
                      active_stop.token());
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (driver.root.statistics().issued.work < baseline_work + 10000 &&
         active.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool progressed =
      driver.root.statistics().issued.work >= baseline_work + 10000;
  active_stop.cancel();
  auto interrupted = active.get();
  PS_CHECK(progressed && !interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled &&
           driver.root.statistics().live[ResourceKind::Payload] ==
               cancel_baseline);
  ResultRef surviving;
  {
    StmapDriver temporary(registry);
    auto source = temporary.source(image_schema(1, 1), {7, 0, 0, 1});
    auto map = temporary.map({.5, .5});
    auto answer = temporary.run(source, map, "clamp", q);
    PS_CHECK(answer.ok());
    surviving = answer.value().results.at("result");
    PS_CHECK(surviving.association().size() == 2);
  }
  float retained = 0;
  PS_CHECK(surviving
               .read_tensor(surviving.descriptor().take_value(), 0,
                            {0, 0, 0, 0, 0}, &retained, 4)
               .ok() &&
           retained == 7);
  return 0;
}

}  // namespace
int main() {
  auto registry = ps::make_default_operation_registry();
  PS_CHECK(static_and_rounding(registry) == 0);
  PS_CHECK(radius_oracles(registry) == 0);
  PS_CHECK(radius_contracts(registry) == 0);
  PS_CHECK(stmap_results(registry) == 0);
  return 0;
}
