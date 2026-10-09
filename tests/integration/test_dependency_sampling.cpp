#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "data/value_validation.hpp"
#include "execution/execution_test_hooks.hpp"
#include "photospider/photospider.hpp"
#include "support/execution_sync_fixture.hpp"
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
struct CallbackGate final {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false, timed_out = false;
  void hold() noexcept {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(20),
                          [&] { return released; }))
      timed_out = true;
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(10),
                            [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
CallbackGate* cancellation_gate = nullptr;
void hold_callback_retirement() noexcept {
  cancellation_gate->hold();
}
struct HookScope final {
  explicit HookScope(const execution_testing::ExecutionTestHooks& hooks) {
    execution_testing::install_execution_test_hooks(&hooks);
  }
  ~HookScope() { execution_testing::install_execution_test_hooks(nullptr); }
};
template <class Run>
int cancel_at_retirement(const ResourceBudget& root, Run run,
                         bool staged = false) {
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  const auto work = root.statistics().issued.work;
  CallbackGate gate;
  cancellation_gate = &gate;
  execution_testing::ExecutionTestHooks hooks;
  if (staged)
    hooks.final_result_ready = hold_callback_retirement;
  else
    hooks.callback_body_finished = hold_callback_retirement;
  HookScope installed(hooks);
  CancellationSource stop;
  std::future<decltype(run(stop.token()))> active;
  ps::test::OnExit cleanup([&] {
    stop.cancel();
    gate.release();
    if (active.valid())
      active.wait();
    cancellation_gate = nullptr;
  });
  active = std::async(std::launch::async, [&] { return run(stop.token()); });
  const bool entered = gate.wait();
  stop.cancel();
  gate.release();
  auto interrupted = active.get();
  PS_CHECK(entered && !gate.timed_out);
  PS_CHECK(root.statistics().issued.work > work);
  PS_CHECK(!interrupted.ok());
  if (interrupted.status().code != ErrorCode::Cancelled) {
    ps::test::require_ok(interrupted, "cancelled sampling callback", __FILE__,
                         __LINE__);
    return 1;
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
}
template <class Run>
int cancel_in_loop(const ResourceBudget& root, std::string_view operation,
                   Run run) {
  const auto baseline = root.statistics().live[ResourceKind::Payload];
  ps::test::PhaseWorkGate gate(operation, 8);
  CancellationSource stop;
  std::future<decltype(run(stop.token()))> active;
  ps::test::OnExit cleanup([&] {
    stop.cancel();
    gate.release();
    if (active.valid())
      active.wait();
  });
  active = std::async(std::launch::async, [&] { return run(stop.token()); });
  const bool entered = gate.wait();
  stop.cancel();
  gate.release();
  auto interrupted = active.get();
  PS_CHECK(entered && gate.cancelled_in_service());
  PS_CHECK(!interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == baseline);
  return 0;
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
  PS_CHECK(
      cancel_at_retirement(driver.root, [&](const CancellationToken& stop) {
        return driver.run("numeric.radius_scatter", {many, radii},
                          footprint({100000}, Region({{0, 1}})), {}, stop);
      }) == 0);
  PS_CHECK(cancel_in_loop(driver.root, "numeric.radius_scatter",
                          [&](const CancellationToken& stop) {
                            return driver.run(
                                "numeric.radius_scatter", {many, radii},
                                footprint({100000}, Region({{0, 1}})), {},
                                stop);
                          }) == 0);
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
  explicit StmapDriver(std::shared_ptr<OperationRegistry> r,
                       ExecutionContextConfig config = {})
      : registry(r),
        context(r, config),
        root(context.resource_budget().take_value()) {}
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
                           const CancellationToken& cancellation = {},
                           const PlanningOptions& planning = {}) {
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
    auto compiled = Compiler(registry).compile(graph, planning);
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
  PS_CHECK(cancel_at_retirement(
               driver.root,
               [&](const CancellationToken& token) {
                 return driver.run(
                     image, large_map, "clamp",
                     Footprint::all({1, 1, 50, 50, 4}).take_value(), {}, token);
               },
               true) == 0);
  PS_CHECK(cancel_in_loop(
               driver.root, "image.stmap", [&](const CancellationToken& token) {
                 return driver.run(
                     image, large_map, "clamp",
                     Footprint::all({1, 1, 50, 50, 4}).take_value(), {}, token);
               }) == 0);
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

std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
std::int64_t stmap_reference_index(std::int64_t i, std::int64_t n,
                                   const std::string& mode) {
  if (i >= 0 && i < n)
    return i;
  if (mode == "constant")
    return -1;
  if (mode == "clamp")
    return std::max<std::int64_t>(0, std::min(n - 1, i));
  if (n == 1)
    return 0;
  const auto period = mode == "wrap"      ? n
                      : mode == "reflect" ? 2 * n
                                          : 2 * (n - 1);
  const auto r = (i % period + period) % period;
  return mode == "wrap" ? r
                        : std::min(r, period - (mode == "reflect" ? 1 : 0) - r);
}
std::array<float, 4> stmap_reference(const std::vector<float>& source,
                                     std::uint64_t height, std::uint64_t width,
                                     double u, double v,
                                     const std::string& mode) {
  // An independent scalar oracle with explicit rounding after every operation;
  // it cannot inherit the production loop's vectorization or FMA decisions.
  volatile double sx = u - .5, sy = v - .5;
  const auto left = static_cast<std::int64_t>(std::floor(sx));
  const auto top = static_cast<std::int64_t>(std::floor(sy));
  volatile double fx = sx - static_cast<double>(left),
                  fy = sy - static_cast<double>(top);
  std::array<double, 4> weights{};
  std::array<std::array<float, 4>, 4> values{};
  for (unsigned dy = 0; dy < 2; ++dy)
    for (unsigned dx = 0; dx < 2; ++dx) {
      const auto k = dy * 2 + dx;
      volatile double weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
      weights[k] = weight;
      const auto y = stmap_reference_index(top + dy, height, mode);
      const auto x = stmap_reference_index(left + dx, width, mode);
      if (x >= 0 && y >= 0) {
        for (std::size_t c = 0; c < 4; ++c)
          values[k][c] = source[(y * width + x) * 4 + c];
      }
    }
  std::array<float, 4> result{};
  for (std::size_t c = 0; c < 4; ++c) {
    double sum = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      volatile double product = static_cast<double>(values[k][c]) * weights[k];
      volatile double next = sum + product;
      sum = next;
    }
    volatile float narrowed = static_cast<float>(sum);
    result[c] = narrowed;
  }
  return result;
}
int stmap_bitwise_oracle(const std::shared_ptr<OperationRegistry>& registry) {
  struct Environment {
    std::fenv_t saved{};
    Environment() {
      std::fegetenv(&saved);
      std::fesetenv(FE_DFL_ENV);
    }
    ~Environment() { std::fesetenv(&saved); }
  } environment;
  const auto tiny = std::numeric_limits<float>::denorm_min();
  const auto normal = std::numeric_limits<float>::min();
  const std::vector<float> original{
      -1,     -0.F,  tiny,      1,        std::nextafter(1.F, 2.F),
      normal, -tiny, 1,         0x1p80F,  -0x1p80F,
      .25F,   1,     -0x1p80F,  0x1p80F,  -.25F,
      1,      1,     -1,        3 * tiny, 1,
      -1,     1,     -3 * tiny, 1};
  const std::vector<double> uv{.5,   .5,      1,      .5,  1.75,    1.25, -3.25,
                               4.75, 0x1p-56, .5,     3.5, 2.5,     -.5,  -.5,
                               1.5,  1.5,     0x1p40, .5,  -0x1p40, .5};
  PlanningOptions tiles;
  tiles.tile_height = 1;
  tiles.tile_width = 2;
  for (const auto size : {std::array<std::uint64_t, 2>{2, 3}, {1, 1}, {1, 2}}) {
    const std::vector<float> pixels(original.begin(),
                                    original.begin() + size[0] * size[1] * 4);
    for (const std::string mode :
         {"constant", "clamp", "wrap", "reflect", "mirror"}) {
      StmapDriver driver(registry);
      const auto q = Footprint::all({1, 1, 2, 5, 4}).take_value();
      auto answer =
          driver.run(driver.source(image_schema(size[0], size[1]), pixels),
                     driver.map(uv, {2, 5, 2}), mode, q, {}, {}, tiles);
      PS_REQUIRE_OK(answer);
      for (std::uint64_t i = 0; i < 10; ++i) {
        const auto expected = stmap_reference(pixels, size[0], size[1],
                                              uv[i * 2], uv[i * 2 + 1], mode);
        for (std::uint64_t c = 0; c < 4; ++c) {
          std::uint32_t actual = 0;
          PS_REQUIRE_OK(numeric_result_fixture::read(
              answer.value().results.at("result"), {0, 0, i / 5, i % 5, c},
              &actual, 4));
          PS_CHECK(actual == float_bits(expected[c]));
        }
      }
    }
  }
  for (const int rounding :
       {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(rounding) == 0);
    StmapDriver driver(registry);
    auto q = Footprint::all({1, 1, 1, 1, 4}).take_value();
    auto folded = driver.run(
        driver.source(image_schema(2, 2), {0x1p100F, 0, 0, 1, 1, 0, 0, 1,
                                           -0x1p100F, 0, 0, 1, 1, 0, 0, 1}),
        driver.map({1, 1}), "clamp", q);
    PS_REQUIRE_OK(folded);
    std::uint32_t bits = 0;
    PS_REQUIRE_OK(numeric_result_fixture::read(
        folded.value().results.at("result"), {0, 0, 0, 0, 0}, &bits, 4));
    PS_CHECK(bits == float_bits(.25F) && std::fegetround() == rounding);
    auto shifted =
        driver.run(driver.source(image_schema(1, 2), {1, 0, 0, 1, -1, 0, 0, 1}),
                   driver.map({0x1p-56, .5}), "wrap", q);
    PS_REQUIRE_OK(shifted);
    PS_REQUIRE_OK(numeric_result_fixture::read(
        shifted.value().results.at("result"), {0, 0, 0, 0, 0}, &bits, 4));
    PS_CHECK(bits == 0 && std::fegetround() == rounding);
  }
  return 0;
}
ResultRef stmap_map(StmapDriver& driver, const std::vector<double>& uv,
                    std::uint64_t h, std::uint64_t w,
                    const std::vector<std::uint64_t>& batches = {},
                    std::uint32_t atomic = 0) {
  auto shape = batches;
  shape.insert(shape.end(), {h, w, 2});
  auto value = data(ElementType::Float64, shape, uv);
  auto schema = numeric_result_fixture::source_schema(value);
  schema.tensors[0].batch_axes.assign(batches.begin(), batches.end());
  schema.tensors[0].descriptor.shape = {h, w, 2};
  schema.tensors[0].atomic_trailing_axes = atomic;
  return numeric_result_fixture::source(driver.root, value, &schema);
}
Footprint stmap_dirty(const DemandResult& answer, const std::string& input,
                      const Footprint& edit, std::uint32_t roles) {
  return numeric_result_fixture::take(
             answer.dependencies.potential_dirty(
                 input, edit, roles, {}, ResultSupportTarget::Tensor, 0))
      .at("result");
}
int stmap_sparse_dependencies(
    const std::shared_ptr<OperationRegistry>& registry) {
  using numeric_result_fixture::take;
  StmapDriver driver(registry);
  const std::vector<std::uint64_t> output_shape{1, 1, 1, 3, 4},
      source_shape{1, 1, 1, 8, 4};
  const auto pixel = [](std::uint64_t x, std::uint64_t c = 4) {
    return Region({{0, 1}, {0, 1}, {0, 1}, {x, 1}, {0, c}});
  };
  const auto full = take(Footprint::all(output_shape));
  const auto sparse =
      take(Footprint::from_regions(output_shape, {pixel(0, 1), pixel(2, 1)}));
  const auto closed =
      take(Footprint::from_regions(output_shape, {pixel(0), pixel(2)}));
  std::vector<float> pixels(8 * 4);
  for (unsigned x = 0; x < 8; ++x) {
    pixels[x * 4] = x;
    pixels[x * 4 + 3] = 1;
  }
  const auto map = stmap_map(driver, {.5, .5, 3.5, .5, 6.5, .5}, 1, 3);
  auto answer = take(driver.run(driver.source(image_schema(1, 8), pixels), map,
                                "clamp", full));
  PS_CHECK(stmap_dirty(answer, "input0", footprint(source_shape, pixel(1)),
                       1) == footprint(output_shape, pixel(0)));
  PS_CHECK(stmap_dirty(answer, "input0", footprint(source_shape, pixel(4)),
                       1) == footprint(output_shape, pixel(1)));
  PS_CHECK(stmap_dirty(answer, "input0", footprint(source_shape, pixel(2)), 1)
               .empty());
  const auto map_middle =
      footprint({1, 3, 2}, Region({{0, 1}, {1, 1}, {0, 1}}));
  PS_CHECK(stmap_dirty(answer, "input1", map_middle, 2) ==
           footprint(output_shape, pixel(1)));
  auto relation = take(answer.results.at("result").tensor_relation(0));
  auto projected = take(Footprint::none(source_shape));
  PS_REQUIRE_OK(relation.project(
      footprint(output_shape, pixel(0, 1)),
      [&](ResultSupport support, const Footprint* samples) {
        if (support.input == 0 && (support.roles & 1U) && samples)
          projected = take(projected.unite(*samples));
        return Status::success();
      }));
  PS_CHECK(projected ==
           footprint(source_shape,
                     Region({{0, 1}, {0, 1}, {0, 1}, {0, 2}, {0, 4}})));
  auto finite = pixels;
  pixels[2 * 4] = std::numeric_limits<float>::quiet_NaN();
  auto sparse_answer = take(driver.run(
      driver.source(image_schema(1, 8), pixels),
      stmap_map(driver,
                {.5, .5, std::numeric_limits<double>::quiet_NaN(), .5, 6.5, .5},
                1, 3),
      "clamp", sparse));
  PS_CHECK(take(sparse_answer.results.at("result").descriptor())
               .tensor_coverage(0) == closed);
  PS_CHECK(
      stmap_dirty(sparse_answer, "input0", footprint(source_shape, pixel(4)), 1)
          .empty());
  PS_CHECK(stmap_dirty(sparse_answer, "input1", map_middle, 2).empty());
  auto supports = take(sparse_answer.dependencies.source_support());
  PS_CHECK(
      supports.at("input0") ==
      take(Footprint::from_regions(
          source_shape, {Region({{0, 1}, {0, 1}, {0, 1}, {0, 2}, {0, 4}}),
                         Region({{0, 1}, {0, 1}, {0, 1}, {6, 2}, {0, 4}})})));
  auto grouped_source = image_schema(1, 8);
  grouped_source.tensors[0].atomic_trailing_axes = 2;
  auto validation = take(
      driver.run(driver.source(grouped_source, finite), map, "clamp", sparse));
  auto unrelated = footprint(source_shape, pixel(2));
  PS_CHECK(stmap_dirty(validation, "input0", unrelated, 1).empty());
  PS_CHECK(stmap_dirty(validation, "input0", unrelated, 4) == closed);
  auto map_validation = take(
      driver.run(driver.source(image_schema(1, 8), finite),
                 stmap_map(driver, {.5, .5, 3.5, .5, 6.5, .5}, 1, 3, {}, 2),
                 "clamp", sparse));
  PS_CHECK(stmap_dirty(map_validation, "input1", map_middle, 2).empty());
  PS_CHECK(stmap_dirty(map_validation, "input1", map_middle, 4) == closed);
  return 0;
}
int stmap_batches_and_strides(
    const std::shared_ptr<OperationRegistry>& registry) {
  using numeric_result_fixture::take;
  StmapDriver driver(registry);
  auto schema = image_schema(1, 8, 2);
  schema.tensors[0].batch_axes = {2, 2};
  std::vector<float> pixels(4 * 8 * 4);
  std::vector<double> batched_uv;
  for (unsigned f = 0; f < 2; ++f)
    for (unsigned l = 0; l < 2; ++l) {
      for (unsigned x = 0; x < 8; ++x) {
        const auto i = ((f * 2 + l) * 8 + x) * 4;
        pixels[i] = 10 * f + 3 * l + x;
        pixels[i + 3] = 1;
      }
      batched_uv.insert(batched_uv.end(), {.5, .5, 3.5, .5, 6.5, .5});
    }
  auto source = driver.source(schema, pixels);
  const auto full = take(Footprint::all({2, 2, 1, 3, 4}));
  for (const bool batched : {false, true}) {
    auto map = batched ? stmap_map(driver, batched_uv, 1, 3, {2, 2})
                       : stmap_map(driver, {.5, .5, 3.5, .5, 6.5, .5}, 1, 3);
    auto answer = take(driver.run(source, map, "clamp", full));
    for (std::uint64_t f = 0; f < 2; ++f)
      for (std::uint64_t l = 0; l < 2; ++l)
        for (std::uint64_t x = 0; x < 3; ++x) {
          float value = 0;
          PS_REQUIRE_OK(numeric_result_fixture::read(
              answer.results.at("result"), {f, l, 0, x, 0}, &value, 4));
          PS_CHECK(value == 10 * f + 3 * l + 3 * x);
        }
    const auto map_edit =
        batched ? footprint({2, 2, 1, 3, 2},
                            Region({{1, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}}))
                : footprint({1, 3, 2}, Region({{0, 1}, {1, 1}, {0, 1}}));
    const auto expected =
        batched ? Region({{1, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 4}})
                : Region({{0, 2}, {0, 2}, {0, 1}, {1, 1}, {0, 4}});
    PS_CHECK(stmap_dirty(answer, "input1", map_edit, 2) ==
             footprint(full.shape(), expected));
    auto source_edit =
        footprint(schema.tensors[0].sample_shape(),
                  Region({{1, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 1}}));
    PS_CHECK(stmap_dirty(answer, "input0", source_edit, 1) ==
             footprint(full.shape(),
                       Region({{1, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})));
  }
  const std::vector<double> physical{.5, .5, 3.5, .5, 6.5, .5};
  auto map_schema = numeric_result_fixture::source_schema(
      data(ElementType::Float64, {1, 3, 2}, physical));
  for (const bool spatial : {false, true}) {
    map_schema.tensors[0].layout.spatial = spatial;
    auto builder =
        take(ResultBuilder::start(driver.root, map_schema, "signed.map"));
    PS_REQUIRE_OK(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(driver.root, 1, {}))));
    auto buffer = take(driver.root.allocator().allocate(48));
    std::memcpy(buffer.data(), physical.data(), 48);
    PS_REQUIRE_OK(builder.publish_tensor(
        0, Region::whole({1, 3, 2}), StridedLayout{32, {48, -16, 8}},
        std::move(buffer).freeze(),
        take(ResultRelation::cartesian(driver.root, 6, {})),
        {true, true, true, true}));
    auto signed_answer =
        take(driver.run(source, take(builder.seal()), "clamp", full));
    for (std::uint64_t x = 0; x < 3; ++x) {
      float value = 0;
      PS_REQUIRE_OK(numeric_result_fixture::read(
          signed_answer.results.at("result"), {1, 1, 0, x, 0}, &value, 4));
      PS_CHECK(value == 13 + (2 - x) * 3);
    }
  }
  return 0;
}

int typed_validation_work_batches(
    const std::shared_ptr<OperationRegistry>& registry) {
  using numeric_result_fixture::take;
  StmapDriver driver(registry);
  const auto facet = take(encode_semantic(coverage_semantics()));
  auto schema = image_schema(1, 513);
  schema.tensors[0].descriptor.shape = {1, 513};
  schema.tensors[0].layout.channel_axis.reset();
  schema.tensors[0].facets = {facet};
  const auto shape = schema.tensors[0].sample_shape();
  const auto samples = take(Footprint::all(shape));
  const auto make = [&](bool invalid) {
    std::vector<float> values(513, .5F);
    if (invalid)
      values[256] = 2.F;
    auto builder = take(ResultBuilder::start(
        driver.root, schema, "validation.input", {}, {}, 1, 1024));
    numeric_result_fixture::require(
        builder
            .bind_descriptor_relation(
                take(ResultRelation::cartesian(driver.root, 1, {})))
            .ok(),
        "validation descriptor");
    numeric_result_fixture::require(
        builder
            .publish_tensor(
                0, Region::whole(shape),
                ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                         values.size() * sizeof(float)),
                take(ResultRelation::cartesian(driver.root, values.size(), {})),
                {true, true, true, true})
            .ok(),
        "validation payload");
    return take(builder.seal());
  };
  const auto valid = make(false), invalid = make(true);
  std::vector<std::uint64_t> charges;
  const auto validate = [&](const ResultRef& input,
                            const CancellationToken& token,
                            const std::function<Status(std::uint64_t)>& consume,
                            const std::function<ErrorCode()>& stop = {}) {
    return input_internal::validate_tensor_samples(
        input, take(input.descriptor()), 0, samples, driver.root,
        ErrorCode::TypeMismatch, token, stop, consume);
  };
  auto status = validate(valid, {}, [&](auto n) {
    charges.push_back(n);
    return Status::success();
  });
  PS_REQUIRE_OK(status);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256, 1}));
  charges.clear();
  status = validate(invalid, {}, [&](auto n) {
    charges.push_back(n);
    return charges.size() == 2 ? Status{ErrorCode::ResourceExhausted, {}}
                               : Status::success();
  });
  PS_CHECK(status.code == ErrorCode::ResourceExhausted);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256}));
  charges.clear();
  status = validate(invalid, {}, [&](auto n) {
    charges.push_back(n);
    return Status::success();
  });
  PS_CHECK(status.code == ErrorCode::TypeMismatch);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256}));
  charges.clear();
  CancellationSource cancellation;
  status = validate(invalid, cancellation.token(), [&](auto n) {
    charges.push_back(n);
    if (charges.size() == 2)
      cancellation.cancel();
    return Status::success();
  });
  PS_CHECK(status.code == ErrorCode::Cancelled);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256}));
  charges.clear();
  status = validate(
      valid, {},
      [&](auto n) {
        charges.push_back(n);
        return Status::success();
      },
      [&] { return charges.size() == 3 ? ErrorCode::Stale : ErrorCode::Ok; });
  PS_CHECK(status.code == ErrorCode::Stale);
  PS_CHECK(charges == std::vector<std::uint64_t>({256, 256, 1}));
  return 0;
}
int stmap_cpu_tiles(const std::shared_ptr<OperationRegistry>& registry) {
  using numeric_result_fixture::take;
  constexpr std::uint64_t side = 256;
  const std::vector<float> pixels{1,  -2,  3,  1, -4,  5,  6,  1,
                                  7,  8,   -9, 1, -10, 11, 12, 1,
                                  13, -14, 15, 1, 16,  17, 18, 1};
  const std::array<std::array<double, 2>, 8> points{
      {{{.875, .75}},
       {{-.25, -.75}},
       {{2.5, 1.5}},
       {{3.875, 2.25}},
       {{.5, .5}},
       {{std::ldexp(1., -56), .5}},
       {{-1099511627776., .5}},
       {{2.125, 1.125}}}};
  std::vector<double> uv(2 * side * side);
  for (std::size_t i = 0; i < side * side; ++i) {
    uv[2 * i] = points[i % points.size()][0];
    uv[2 * i + 1] = points[i % points.size()][1];
  }
  const auto query = take(Footprint::all({1, 1, side, side, 4}));
  for (const std::string mode :
       {"constant", "clamp", "wrap", "reflect", "mirror"}) {
    std::vector<float> reference;
    std::uint64_t reference_work = 0, reference_tiles = 0;
    for (const unsigned workers : {1U, 4U}) {
      ExecutionContextConfig config;
      config.cpu_workers = workers;
      StmapDriver driver(registry, config);
      const auto source = driver.source(image_schema(2, 3), pixels);
      const auto map = driver.map(uv, {side, side, 2});
      ExecutionOptions options;
      options.maximum_parallelism = workers;
      options.maximum_dependency_work = UINT64_C(1) << 32;
      options.dependencies.maximum_work = UINT64_C(1) << 32;
      options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
      const auto before = driver.root.statistics().issued.work;
      const auto rounding = std::fegetround();
      PS_CHECK(std::fesetround(FE_UPWARD) == 0);
      const auto answer = driver.run(source, map, mode, query, options);
      const auto restored = std::fegetround();
      PS_CHECK(std::fesetround(rounding) == 0);
      PS_REQUIRE_OK(answer);
      PS_CHECK(restored == FE_UPWARD);
      const auto work = driver.root.statistics().issued.work - before;
      const auto& diagnostics = answer.value().diagnostics;
      PS_CHECK(diagnostics.cpu_stage_count == 1);
      PS_CHECK(diagnostics.cpu_tile_callback_count > 0 &&
               diagnostics.cpu_tile_callback_count <= 64);
      const auto& result = answer.value().results.at("result");
      const auto descriptor = take(result.descriptor());
      auto window = take(
          result.acquire_tensor(descriptor, 0, Region::whole(query.shape())));
      PS_CHECK(window.sample_axis() == 3);
      std::vector<float> actual(side * side * 4);
      for (std::uint64_t y = 0; y < side; ++y)
        for (std::uint64_t c = 0; c < 4; ++c)
          for (std::uint64_t x = 0; x < side;) {
            const auto run = take(window.row_run({0, 0, y, x, c}));
            const auto n = std::min(side - x, run.samples);
            for (std::uint64_t i = 0; i < n; ++i)
              std::memcpy(&actual[((y * side + x + i) * 4) + c],
                          run.data + static_cast<std::int64_t>(i) *
                                         run.sample_stride_bytes,
                          4);
            x += n;
          }
      for (std::size_t i = 0; i < points.size(); ++i) {
        const auto expected =
            stmap_reference(pixels, 2, 3, points[i][0], points[i][1], mode);
        PS_CHECK(std::memcmp(actual.data() + i * 4, expected.data(), 16) == 0);
      }
      auto support = take(answer.value().dependencies.source_support());
      PS_CHECK(support.at("input1") == take(Footprint::all({side, side, 2})));
      PS_CHECK(support.at("input0") == take(Footprint::all({1, 1, 2, 3, 4})));
      if (workers == 1) {
        reference = std::move(actual);
        reference_work = work;
        reference_tiles = diagnostics.cpu_tile_callback_count;
      } else {
        PS_CHECK(std::memcmp(actual.data(), reference.data(),
                             actual.size() * sizeof(float)) == 0);
        PS_CHECK(work == reference_work);
        PS_CHECK(diagnostics.cpu_tile_callback_count == reference_tiles);
      }
    }
  }
  return 0;
}
int stmap_sparse_scaling() {
  std::uint64_t previous = 0;
  for (const std::uint64_t side : {16, 32, 64}) {
    StmapDriver driver(ps::make_default_operation_registry());
    auto image = driver.source(image_schema(1, 2), {1, 0, 0, 1, 2, 0, 0, 1});
    auto map =
        driver.map(std::vector<double>(2 * side * side, .5), {side, side, 2});
    std::vector<Region> boxes;
    for (std::uint64_t y = 0; y < side; ++y)
      for (std::uint64_t x = y % 2; x < side; x += 2)
        boxes.emplace_back(std::vector<RegionDimension>{{0, 1},
                                                        {0, 1},
                                                        {y, 1},
                                                        {x, 1},
                                                        {0, 4}});
    auto query =
        Footprint::from_regions({1, 1, side, side, 4}, boxes).take_value();
    const auto before = driver.root.statistics().issued.work;
    ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 32;
    options.dependencies.maximum_work = UINT64_C(1) << 32;
    options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
    auto answer = driver.run(image, map, "clamp", query, options);
    PS_REQUIRE_OK(answer);
    const auto work = driver.root.statistics().issued.work - before;
    std::uint64_t callbacks = 0;
    for (const auto& op : answer.value().diagnostics.operation_timings)
      callbacks += op.invocation_count;
    std::cout << "STMap sparse side=" << side << " boxes=" << boxes.size()
              << " work=" << work << '\n';
    PS_CHECK(callbacks == 3);
    PS_CHECK(work < 15000 * boxes.size());
    if (previous)
      PS_CHECK(work < 6 * previous);
    previous = work;
    const auto& result = answer.value().results.at("result");
    PS_CHECK(result.descriptor().take_value().tensor_coverage(0) == query);
    for (const auto& region : {boxes.front(), boxes.back()}) {
      float value = 0;
      const auto& d = region.dimensions();
      PS_REQUIRE_OK(numeric_result_fixture::read(
          result, {0, 0, d[2].offset, d[3].offset, 0}, &value, 4));
      PS_CHECK(value == 1.F);
    }
  }
  return 0;
}
int stmap_scaling() {
  std::uint64_t previous_work = 0;
  for (const std::uint64_t side : {8, 16, 32, 64}) {
    StmapDriver driver(ps::make_default_operation_registry());
    auto image =
        driver.source(image_schema(1, 3), {1, 0, 0, 1, 2, 0, 0, 1, 4, 0, 0, 1});
    auto map =
        driver.map(std::vector<double>(2 * side * side, .5), {side, side, 2});
    auto query = Footprint::all({1, 1, side, side, 4}).take_value();
    const auto before = driver.root.statistics();
    {
      ExecutionOptions options;
      options.maximum_dependency_work = UINT64_C(1) << 32;
      options.dependencies.maximum_work = UINT64_C(1) << 32;
      options.dependencies.sets.maximum_work = UINT64_C(1) << 32;
      auto answer = driver.run(image, map, "clamp", query, options);
      PS_REQUIRE_OK(answer);
      const auto work =
          driver.root.statistics().issued.work - before.issued.work;
      // The exact witness table and batched execution grow with requested
      // pixels. This gate fails on the former P^2 publication path.
      const auto pixels = side * side;
      std::cout << "STMap side=" << side << " work=" << work << '\n';
      PS_CHECK(work < 1600 * pixels);
      if (previous_work)
        PS_CHECK(work < 5 * previous_work);
      previous_work = work;
      PS_CHECK(driver.root.statistics().live[ResourceKind::Payload] -
                   before.live[ResourceKind::Payload] <
               UINT64_C(1048576));
      const auto& result = answer.value().results.at("result");
      for (std::uint64_t y = 0; y < side; ++y) {
        for (std::uint64_t x = 0; x < side; ++x) {
          for (std::uint64_t c = 0; c < 4; ++c) {
            float value = -1;
            PS_CHECK(result
                         .read_tensor(result.descriptor().take_value(), 0,
                                      {0, 0, y, x, c}, &value, 4)
                         .ok());
            PS_CHECK(value == (c == 0 || c == 3 ? 1.F : 0.F));
          }
        }
      }
      auto support = answer.value().dependencies.source_support();
      PS_REQUIRE_OK(support);
      PS_CHECK(support.value().at("input1") ==
               Footprint::all({side, side, 2}).take_value());
      PS_CHECK(support.value().at("input0") ==
               footprint({1, 1, 1, 3, 4},
                         Region({{0, 1}, {0, 1}, {0, 1}, {0, 2}, {0, 4}})));
      auto tap = footprint({1, 1, 1, 3, 4},
                           Region({{0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 4}}));
      auto dirty = answer.value().dependencies.potential_dirty(
          "input0", tap, 1, {}, ResultSupportTarget::Tensor, 0);
      PS_REQUIRE_OK(dirty);
      PS_CHECK(dirty.value().at("result") == query);
      auto absent = footprint({1, 1, 1, 3, 4},
                              Region({{0, 1}, {0, 1}, {0, 1}, {2, 1}, {0, 4}}));
      auto clean = answer.value().dependencies.potential_dirty(
          "input0", absent, 1, {}, ResultSupportTarget::Tensor, 0);
      PS_REQUIRE_OK(clean);
      PS_CHECK(clean.value().at("result").empty());
    }
    PS_CHECK(driver.root.statistics().live[ResourceKind::Payload] ==
             before.live[ResourceKind::Payload]);
  }
  return 0;
}

}  // namespace
int main() {
  auto registry = ps::make_default_operation_registry();
  PS_CHECK(static_and_rounding(registry) == 0);
  PS_CHECK(radius_oracles(registry) == 0);
  PS_CHECK(radius_contracts(registry) == 0);
  PS_CHECK(stmap_results(registry) == 0);
  PS_CHECK(stmap_bitwise_oracle(registry) == 0);
  PS_CHECK(stmap_sparse_dependencies(registry) == 0);
  PS_CHECK(stmap_batches_and_strides(registry) == 0);
  PS_CHECK(typed_validation_work_batches(registry) == 0);
  PS_CHECK(stmap_cpu_tiles(registry) == 0);
  PS_CHECK(stmap_scaling() == 0);
  PS_CHECK(stmap_sparse_scaling() == 0);
  return 0;
}
