#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11): C++17 floating-environment regression.
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/icc_fixture.hpp"
#include "data/affine_view.hpp"
#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/bezier.hpp"
#include "photospider/numeric/calculus.hpp"
#include "photospider/numeric/color_ramps.hpp"
#include "photospider/numeric/curves.hpp"
#include "photospider/numeric/expression.hpp"
#include "photospider/numeric/inverse_curves.hpp"
#include "photospider/numeric/lowpass.hpp"
#include "photospider/numeric/lut1d.hpp"
#include "photospider/numeric/lut3d.hpp"
#include "photospider/numeric/lut3d_baking.hpp"
#include "photospider/numeric/matrix.hpp"
#include "photospider/numeric/resampling.hpp"
#include "photospider/numeric/scans.hpp"
#include "photospider/numeric/sequences.hpp"
#include "photospider/numeric/shapers.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(
        result.status().message.empty()
            ? "status code=" +
                  std::to_string(static_cast<int>(result.status().code)) +
                  " reason=" +
                  std::to_string(static_cast<int>(result.status().reason))
            : result.status().message);
  return result.take_value();
}
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::vector<uint8_t> words(const std::vector<uint64_t>& bits, size_t width) {
  std::vector<uint8_t> data(bits.size() * width + 1);
  for (size_t i = 0; i < bits.size(); ++i)
    std::memcpy(data.data() + 1 + i * width, &bits[i], width);
  return data;
}
struct Driver {
  std::shared_ptr<OperationRegistry> registry =
      make_default_operation_registry();
  std::unique_ptr<ExecutionContext> context;
  ResourceBudget root;
  explicit Driver(uint64_t work = UINT64_MAX, uint64_t metadata = 16 * 1048576,
                  uint64_t cache_bytes = 0) {
    ExecutionContextConfig config;
    config.result_cache_bytes = cache_bytes;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = work;
    config.managed_resources->capacity[ResourceKind::Metadata] = metadata;
    context = std::make_unique<ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  ResultRef source(ValueDescriptor descriptor, std::vector<uint64_t> bits,
                   StridedLayout layout, std::vector<ValueFacet> facets = {},
                   std::vector<uint64_t> batches = {},
                   std::weak_ptr<const CpuStorage>* storage_weak = nullptr,
                   std::string schema_id = "test.numeric",
                   uint32_t schema_version = 1) {
    SchemaTemplate schema;
    schema.id = std::move(schema_id);
    schema.version = schema_version;
    ResultTensorSpec member;
    member.key = "arbitrary-member";
    member.descriptor = descriptor;
    member.facets = std::move(facets);
    member.batch_axes.assign(batches.begin(), batches.end());
    schema.tensors.push_back(std::move(member));
    auto data = words(bits, Value::element_size(descriptor.element_type));
    auto buffer = take(root.allocator().allocate(data.size()));
    std::memcpy(buffer.data(), data.data(), data.size());
    auto storage = std::move(buffer).freeze();
    if (storage_weak)
      *storage_weak = storage;
    auto builder = take(ResultBuilder::start(root, schema, "numeric.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "source descriptor");
    require(
        builder
            .publish_tensor(0, Region::whole(schema.tensors[0].sample_shape()),
                            layout, storage,
                            take(ResultRelation::cartesian(
                                root, take(schema.tensors[0].sample_count()),
                                {0, 1, 0, 0})),
                            {true, true, true, true})
            .ok(),
        "source storage");
    return take(builder.seal());
  }
  struct Prepared {
    std::shared_ptr<GraphContext> graph;
    ExecutionPlan plan;
    ExecutionBindings bindings;
  };
  Prepared prepare(const std::string& key, const std::vector<ResultRef>& inputs,
                   std::map<std::string, ParameterValue> parameters = {},
                   const std::string& selected = "values",
                   ResourceBindings resources = {}, bool gpu = false) {
    WorkflowDocument doc;
    WorkflowNode node;
    node.id = 7;
    node.operation = key;
    node.parameters = std::move(parameters);
    ExecutionBindings bindings;
    for (size_t i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration input;
      input.id = i + 11;
      input.name = "input" + std::to_string(i);
      input.result_schema =
          std::make_shared<SchemaTemplate>(inputs[i].schema());
      node.inputs.push_back(WorkflowInputReference{input.id});
      bindings.inputs.push_back({input.name, inputs[i]});
      doc.inputs.push_back(std::move(input));
    }
    doc.nodes.push_back(std::move(node));
    const auto output_key =
        selected == "values" &&
                (key == "numeric.mean" || key == "numeric.variance" ||
                 key == "numeric.ordered_scan")
            ? "value"
            : selected;
    doc.outputs = {{"out", 7, output_key}};
    auto graph = std::make_shared<GraphContext>(doc);
    PlanningOptions planning;
    if (gpu)
      planning.execution_mode = ExecutionMode::NativeGpu;
    auto compiled =
        take(Compiler(registry).compile(*graph, planning, resources));
    return {graph, std::move(compiled.plan), std::move(bindings)};
  }
  Result<DemandResult> run(
      const std::string& key, const std::vector<ResultRef>& inputs,
      std::optional<Footprint> query = {},
      std::map<std::string, ParameterValue> parameters = {},
      CancellationToken cancellation = {},
      const std::string& selected = "values") {
    auto prepared = prepare(key, inputs, std::move(parameters), selected);
    const auto shape = prepared.plan.steps()[0]
                           .output_result_schema->tensors[0]
                           .sample_shape();
    return context->execute_fragments(
        take(context->freeze(prepared.plan, prepared.bindings)),
        {{"out", query ? *query : take(Footprint::all(shape))}}, cancellation);
  }
};
uint64_t read_bits(const ResultRef& output, std::vector<uint64_t> at) {
  uint64_t bits = 0;
  require(
      output
          .read_tensor(take(output.descriptor()), 0, at, &bits,
                       Value::element_size(
                           output.schema().tensors[0].descriptor.element_type))
          .ok(),
      "numeric output read");
  return bits;
}
uint64_t double_bits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
void core_result_programs() {
  Driver driver;
  for (const auto& pair : std::vector<std::pair<double, double>>{
           {2, 3},
           {-0.0, -0.0},
           {std::numeric_limits<double>::infinity(), 2},
           {std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()},
           {std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max()}}) {
    auto a = driver.source({ElementType::Float64, {1}},
                           {double_bits(pair.first)}, {1, {8}});
    auto b = driver.source({ElementType::Float64, {1}},
                           {double_bits(pair.second)}, {1, {8}});
    auto result = take(driver.run("math.add", {a, b}, {}, {}, {}, "value"));
    const auto expected = pair.first + pair.second;
    const auto bits = read_bits(result.results.at("out"), {0});
    double actual;
    std::memcpy(&actual, &bits, 8);
    require(std::isnan(expected) ? std::isnan(actual)
                                 : bits == double_bits(expected),
            "math.add scalar IEEE behavior");
    auto empty = take(driver.run("math.add", {a, b},
                                 take(Footprint::from_regions({1}, {})), {}, {},
                                 "value"));
    require(
        take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
        "math.add Empty");
  }
  auto source = driver.source({ElementType::UInt8, {3}}, {3, 7, 9}, {1, {1}});
  auto delay =
      take(driver.run("core.delay", {source},
                      take(Footprint::from_regions({3}, {Region({{1, 1}})})),
                      {{"milliseconds", INT64_C(0)}}, {}, "value"));
  require(read_bits(delay.results.at("out"), {0}) == 3 &&
              read_bits(delay.results.at("out"), {2}) == 9,
          "delay Whole coverage");
  for (const auto invalid : {INT64_C(-1), INT64_C(5001)}) {
    bool rejected = false;
    try {
      driver.prepare("core.delay", {source}, {{"milliseconds", invalid}},
                     "value");
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected, "delay static bounds");
  }
  auto prepared = driver.prepare("core.gpu_fallback_probe", {source}, {},
                                 "value", {}, true);
  auto frozen = take(driver.context->freeze(prepared.plan, prepared.bindings));
  auto fallback = take(driver.context->execute(frozen));
  require(
      read_bits(fallback.results.at("out"), {2}) == 9 &&
          fallback.diagnostics.selected_backends.at({7, 0}) == Backend::Cpu &&
          fallback.diagnostics.fallback_reasons.size() == 1 &&
          fallback.diagnostics.transfer_count == 0 &&
          fallback.diagnostics.native_dispatch_count == 0,
      "Result startup fallback value/backend/no upload");
  auto identity_prepared =
      driver.prepare("core.identity", {source}, {}, "value");
  auto identity_full = take(driver.context->execute(
      identity_prepared.plan, identity_prepared.bindings));
  require(!fallback.diagnostics.result_digest.empty() &&
              fallback.diagnostics.result_digest ==
                  identity_full.diagnostics.result_digest,
          "Result digest uses logical payload/schema independent of operation "
          "lineage");
  auto changed_source =
      driver.source({ElementType::UInt8, {3}}, {3, 7, 10}, {1, {1}});
  auto changed_prepared =
      driver.prepare("core.identity", {changed_source}, {}, "value");
  auto changed_full = take(driver.context->execute(changed_prepared.plan,
                                                   changed_prepared.bindings));
  require(changed_full.diagnostics.result_digest !=
              identity_full.diagnostics.result_digest,
          "Result digest detects changed logical samples");
  bool gpu_failed = false, cpu_ok = false;
  for (const auto& timing : fallback.diagnostics.operation_timings) {
    gpu_failed |= timing.backend == Backend::Gpu &&
                  timing.outcome == ErrorCode::BackendUnavailable;
    cpu_ok |= timing.backend == Backend::Cpu && timing.outcome == ErrorCode::Ok;
  }
  require(gpu_failed && cpu_ok, "Result fallback attempt timings");
  WorkflowDocument shared_document;
  WorkflowInputDeclaration input;
  input.id = 11;
  input.name = "input0";
  input.result_schema = std::make_shared<SchemaTemplate>(source.schema());
  shared_document.inputs = {input};
  shared_document.nodes = {
      {6,
       "core.delay",
       {WorkflowInputReference{11}},
       {{"milliseconds", INT64_C(0)}}},
      {7, "core.gpu_fallback_probe", {WorkflowNodeOutput{6, "value"}}, {}}};
  shared_document.outputs = {{"out", 7, "value"}};
  GraphContext shared_graph(shared_document);
  PlanningOptions gpu;
  gpu.execution_mode = ExecutionMode::NativeGpu;
  auto shared_plan =
      take(Compiler(driver.registry).compile(shared_graph, gpu)).plan;
  auto shared_frozen =
      take(driver.context->freeze(shared_plan, {{{"input0", source}}}));
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, resume = false;
  ExecutionOptions options;
  options.result_publication = [&](ValueRef ref, const ResultRef&) {
    if (ref.node_id == 6) {
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return resume; });
    }
    return Status::success();
  };
  CancellationSource producer_stop;
  auto producer = std::async(std::launch::async, [&] {
    return driver.context->execute(shared_frozen, producer_stop.token(),
                                   options);
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(3),
                             [&] { return entered; }),
            "shared fallback producer reached dependency");
  }
  const auto entries = driver.root.statistics().live[ResourceKind::Entries];
  auto waiter = std::async(std::launch::async, [&] {
    return driver.context->execute(shared_frozen);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (driver.root.statistics().live[ResourceKind::Entries] < entries + 2 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool joined =
      driver.root.statistics().live[ResourceKind::Entries] >= entries + 2;
  const bool waiting = waiter.wait_for(std::chrono::milliseconds(20)) ==
                       std::future_status::timeout;
  producer_stop.cancel();
  {
    std::lock_guard<std::mutex> lock(mutex);
    resume = true;
  }
  changed.notify_all();
  auto retired = producer.get();
  auto survived = take(waiter.get());
  require(
      joined && waiting && retired.status().code == ErrorCode::Cancelled &&
          read_bits(survived.results.at("out"), {2}) == 9 &&
          survived.diagnostics.shared_computations >= 1 &&
          survived.diagnostics.selected_backends.at({7, 0}) == Backend::Cpu,
      "shared fallback waiter survives producer cancellation and reports CPU");
  auto delayed = driver.prepare("core.delay", {source},
                                {{"milliseconds", INT64_C(200)}}, "value");
  auto delay_frozen =
      take(driver.context->freeze(delayed.plan, delayed.bindings));
  CancellationSource stop;
  auto future = std::async(std::launch::async, [&] {
    return driver.context->execute(delay_frozen, stop.token());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop.cancel();
  require(future.get().status().code == ErrorCode::Cancelled,
          "Result delay active cancellation");
  auto cold = take(driver.context->execute(delay_frozen));
  auto warm = take(driver.context->execute(delay_frozen));
  require(cold.results.at("out").object_id() ==
                  warm.results.at("out").object_id() &&
              !cold.diagnostics.operation_timings.empty() &&
              warm.diagnostics.operation_timings.empty() &&
              warm.diagnostics.shared_computations >= 1,
          "non-cacheable delay shares an already owned Result in the same "
          "frozen scope");
}
void finite_elementwise_results() {
  Driver d;
  const auto query = take(Footprint::from_regions(
      {2, 3}, {Region({{0, 1}, {1, 1}}), Region({{1, 1}, {2, 1}})}));
  for (const auto type : {ElementType::Float32, ElementType::Float64}) {
    const bool narrow = type == ElementType::Float32;
    const auto bits = [&](double number) {
      if (!narrow)
        return double_bits(number);
      const float value = static_cast<float>(number);
      uint32_t word;
      std::memcpy(&word, &value, 4);
      return static_cast<uint64_t>(word);
    };
    const auto width = narrow ? 4 : 8;
    auto left =
        d.source({type, {2, 3}},
                 {bits(-0.), bits(-3), bits(4), bits(5), bits(-2), bits(6)},
                 {1, {3 * width, width}});
    auto right =
        d.source({type, {2, 3}},
                 {bits(0.), bits(2), bits(-4), bits(3), bits(-7), bits(9)},
                 {1, {3 * width, width}});
    for (const auto* key :
         {"numeric.abs", "numeric.minimum", "numeric.maximum"}) {
      const bool abs = std::string(key) == "numeric.abs";
      const bool minimum = std::string(key) == "numeric.minimum";
      auto inputs = abs ? std::vector<ResultRef>{left}
                        : std::vector<ResultRef>{left, right};
      auto whole = take(d.run(key, inputs, {}, {}, {}, "value"));
      const std::vector<double> expected =
          abs       ? std::vector<double>{0., 3, 4, 5, 2, 6}
          : minimum ? std::vector<double>{-0., -3, -4, 3, -7, 6}
                    : std::vector<double>{0., 2, 4, 5, -2, 9};
      for (uint64_t i = 0; i < expected.size(); ++i)
        require(read_bits(whole.results.at("out"), {i / 3, i % 3}) ==
                    bits(expected[i]),
                "finite elementwise dtype and signed-zero oracle");
      auto sparse = take(d.run(key, inputs, query, {}, {}, "value"));
      const auto& out = sparse.results.at("out");
      require(take(out.descriptor()).tensor_coverage(0) == query &&
                  out.schema().tensors[0].facets.empty(),
              "finite elementwise exact sparse publication and generic output");
      require(take(sparse.dependencies.source_support()).at("input0") == query,
              "finite elementwise Data and Validation remain local");
      require(read_bits(out, {0, 1}) == bits(expected[1]) &&
                  read_bits(out, {1, 2}) == bits(expected[5]),
              "finite elementwise sparse values");
      auto dirty = sparse.dependencies.potential_dirty(
          "input0",
          take(Footprint::from_regions({2, 3}, {Region({{0, 1}, {0, 1}})})), 1,
          {}, ResultSupportTarget::Tensor, 0);
      require(dirty.ok() && dirty.value().at("out").empty(),
              "unqueried finite sample is not dirty");
      auto empty = take(
          d.run(key, inputs, take(Footprint::none({2, 3})), {}, {}, "value"));
      require(take(empty.results.at("out").descriptor())
                      .tensor_coverage(0)
                      .empty() &&
                  take(empty.dependencies.source_support()).empty(),
              "Empty finite query has no input Need");
    }
  }
  auto reversed = d.source(
      {ElementType::Float32, {1, 3}},
      {UINT64_C(0x3f800000), UINT64_C(0xc0000000), UINT64_C(0x40400000)},
      {1, {0, -4}, {0, 2}});
  auto view = take(d.run("numeric.abs", {reversed}, {}, {}, {}, "value"));
  require(
      read_bits(view.results.at("out"), {0, 0}) == UINT64_C(0x40400000) &&
          read_bits(view.results.at("out"), {0, 1}) == UINT64_C(0x40000000) &&
          read_bits(view.results.at("out"), {0, 2}) == UINT64_C(0x3f800000),
      "finite elementwise signed stride and unaligned source");
  auto bad = d.source({ElementType::Float64, {3}},
                      {double_bits(-1), double_bits(2), double_bits(INFINITY)},
                      {1, {8}});
  auto point = take(Footprint::from_regions({3}, {Region({{1, 1}})}));
  require(d.run("numeric.abs", {bad}, point, {}, {}, "value").ok(),
          "remote generic nonfinite sample is not read");
  auto failed = d.run("numeric.abs", {bad}, {}, {}, {}, "value");
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().message ==
                  "nonfinite basic operation input or intermediate",
          "finite elementwise preserves nonfinite diagnostic");
  for (const auto* key : {"numeric.minimum", "numeric.maximum"})
    require(!d.run(key, {bad, bad}, {}, {}, {}, "value").ok(),
            "binary finite operators reject nonfinite inputs");
  CancellationSource stopped;
  stopped.cancel();
  require(d.run("numeric.abs", {bad}, point, {}, stopped.token(), "value")
                  .status()
                  .code == ErrorCode::Cancelled,
          "finite elementwise cancellation");
  const auto rounding = std::fegetround();
  require(std::fesetround(FE_UPWARD) == 0, "finite fenv fixture");
  auto rounded = d.run("numeric.abs", {bad}, point, {}, {}, "value");
  const bool restored = std::fegetround() == FE_UPWARD;
  std::fesetround(rounding);
  require(rounded.ok() && restored, "finite elementwise restores caller fenv");
  auto color = take(encode_color_array(ColorArrayDescriptor{}));
  auto colors = d.source({ElementType::Float64, {1, 3}},
                         {double_bits(1), double_bits(2), double_bits(3),
                          double_bits(4), double_bits(5), double_bits(6)},
                         {1, {24, 24, 8}}, {color}, {2});
  auto channel = take(
      Footprint::from_regions({2, 1, 3}, {Region({{1, 1}, {0, 1}, {1, 1}})}));
  auto typed = take(d.run("numeric.abs", {colors}, channel, {}, {}, "value"));
  require(read_bits(typed.results.at("out"), {1, 0, 1}) == double_bits(5) &&
              take(typed.dependencies.source_support()).at("input0") ==
                  take(Footprint::from_regions(
                      {2, 1, 3}, {Region({{1, 1}, {0, 1}, {0, 3}})})),
          "typed finite input validates exactly one batch tuple");
  auto other_channel = take(
      Footprint::from_regions({2, 1, 3}, {Region({{1, 1}, {0, 1}, {2, 1}})}));
  auto data_dirty = take(typed.dependencies.potential_dirty(
      "input0", other_channel, 1, {}, ResultSupportTarget::Tensor, 0));
  auto validation_dirty = take(typed.dependencies.potential_dirty(
      "input0", other_channel, 4, {}, ResultSupportTarget::Tensor, 0));
  require(data_dirty.at("out").empty() && validation_dirty.at("out") == channel,
          "finite Data and tuple Validation keep separate mappings");
  auto bad_colors =
      d.source({ElementType::Float64, {1, 3}},
               {double_bits(INFINITY), double_bits(2), double_bits(3)},
               {1, {24, 8}}, {color});
  auto bad_channel =
      take(Footprint::from_regions({1, 3}, {Region({{0, 1}, {1, 1}})}));
  auto invalid =
      d.run("numeric.abs", {bad_colors}, bad_channel, {}, {}, "value");
  require(
      !invalid.ok() && invalid.status().code == ErrorCode::InvalidArgument &&
          invalid.status().reason == FailureReason::InvalidDomain &&
          invalid.status().detail.input_id == 11,
      "finite typed validation catches unused tuple channel before arithmetic");
  require(d.run("numeric.abs", {bad_colors}, take(Footprint::none({1, 3})), {},
                {}, "value")
              .ok(),
          "Empty skips typed payload validation");
  auto rank8 = d.source({ElementType::Float64, {1, 1, 1, 1, 1, 1, 1, 2}},
                        {double_bits(-3), double_bits(4)},
                        {1, {16, 16, 16, 16, 16, 16, 16, 8}});
  auto eight = take(d.run("numeric.abs", {rank8}, {}, {}, {}, "value"));
  require(read_bits(eight.results.at("out"), {0, 0, 0, 0, 0, 0, 0, 0}) ==
              double_bits(3),
          "finite rank eight source and output");
  auto small = d.source({ElementType::Float32, {3}}, {0, 0, 0}, {1, {4}});
  OperationMetadata a, b;
  a.result_schema = std::make_shared<SchemaTemplate>(bad.schema());
  b.result_schema = std::make_shared<SchemaTemplate>(small.schema());
  auto mismatch = d.registry->resolve_traits("numeric.minimum", {a, b}, {});
  require(!mismatch.ok() && mismatch.status().code == ErrorCode::TypeMismatch,
          "finite dtype mismatch fails immutable preparation");
  auto zeros_a = d.source(
      {ElementType::Float64, {4}},
      {double_bits(-0.), double_bits(-0.), double_bits(0.), double_bits(0.)},
      {1, {8}});
  auto zeros_b = d.source(
      {ElementType::Float64, {4}},
      {double_bits(-0.), double_bits(0.), double_bits(-0.), double_bits(0.)},
      {1, {8}});
  for (const auto* key : {"numeric.minimum", "numeric.maximum"}) {
    auto zeros = take(d.run(key, {zeros_a, zeros_b}, {}, {}, {}, "value"));
    for (uint64_t i = 0; i < 4; ++i)
      require(read_bits(zeros.results.at("out"), {i}) ==
                  double_bits(std::string(key) == "numeric.minimum" ? -0. : 0.),
              "finite binary signed-zero rules cover all operand signs");
  }
  auto grouped_schema = bad.schema();
  grouped_schema.tensors[0].atomic_trailing_axes = 1;
  auto grouped_builder =
      take(ResultBuilder::start(d.root, grouped_schema, "finite.group"));
  require(grouped_builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {})))
              .ok(),
          "finite group basis");
  const double group_data[] = {1, 2, 3};
  require(
      grouped_builder
          .publish_tensor(0, Region::whole({3}),
                          ByteView(reinterpret_cast<const uint8_t*>(group_data),
                                   sizeof(group_data)),
                          take(ResultRelation::cartesian(d.root, 3, {})),
                          {true, true, true, true})
          .ok(),
      "finite group source");
  auto grouped_source = take(grouped_builder.seal());
  auto grouped =
      take(d.run("numeric.abs", {grouped_source}, point, {}, {}, "value"));
  auto group_edit = take(Footprint::from_regions({3}, {Region({{0, 1}})}));
  require(
      take(grouped.dependencies.source_support()).at("input0") ==
              take(Footprint::all({3})) &&
          take(grouped.dependencies.potential_dirty(
                   "input0", group_edit, 4, {}, ResultSupportTarget::Tensor, 0))
                  .at("out") == point &&
          take(grouped.dependencies.potential_dirty(
                   "input0", group_edit, 1, {}, ResultSupportTarget::Tensor, 0))
              .at("out")
              .empty(),
      "finite declared atomic tuple expands Validation independently of Data");
  SchemaTemplate image_schema;
  image_schema.id = "test.finite.image";
  ResultTensorSpec pixels;
  pixels.key = "pixels";
  pixels.descriptor = {ElementType::Float32, {1, 2, 4}};
  pixels.batch_axes = {2, 1};
  pixels.layout.spatial = true;
  pixels.layout.channel_axis = 2;
  pixels.facets = {take(encode_semantic(rgba_semantics()))};
  image_schema.tensors.push_back(std::move(pixels));
  auto image_builder =
      take(ResultBuilder::start(d.root, image_schema, "finite.image"));
  require(image_builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {})))
              .ok(),
          "finite image basis");
  float pixel_data[16];
  std::fill(std::begin(pixel_data), std::end(pixel_data), .25F);
  pixel_data[3] = INFINITY;  // A remote invalid pixel must remain unread.
  require(
      image_builder
          .publish_tensor(0, Region::whole({2, 1, 1, 2, 4}),
                          ByteView(reinterpret_cast<const uint8_t*>(pixel_data),
                                   sizeof(pixel_data)),
                          take(ResultRelation::cartesian(d.root, 16, {})),
                          {true, true, true, true})
          .ok(),
      "finite batched image publication");
  auto image = take(image_builder.seal());
  auto image_q = take(Footprint::from_regions(
      {2, 1, 1, 2, 4}, {Region({{1, 1}, {0, 1}, {0, 1}, {1, 1}, {1, 1}})}));
  auto image_run =
      take(d.run("numeric.abs", {image}, image_q, {}, {}, "value"));
  require(read_bits(image_run.results.at("out"), {1, 0, 0, 1, 1}) ==
                  UINT64_C(0x3e800000) &&
              take(image_run.dependencies.source_support()).at("input0") ==
                  take(Footprint::from_regions(
                      {2, 1, 1, 2, 4},
                      {Region({{1, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 4}})})),
          "finite batched image validation preserves complete local pixel");
  auto image_edit = take(Footprint::from_regions(
      {2, 1, 1, 2, 4}, {Region({{1, 1}, {0, 1}, {0, 1}, {1, 1}, {3, 1}})}));
  auto image_v_dirty = take(image_run.dependencies.potential_dirty(
      "input0", image_edit, 4, {}, ResultSupportTarget::Tensor, 0));
  auto image_d_dirty = take(image_run.dependencies.potential_dirty(
      "input0", image_edit, 1, {}, ResultSupportTarget::Tensor, 0));
  require(
      image_v_dirty.at("out") == image_q && image_d_dirty.at("out").empty(),
      "batched image finite Validation mapping includes unrequested channels");
  auto image_remote = take(Footprint::from_regions(
      {2, 1, 1, 2, 4}, {Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {3, 1}})}));
  require(
      take(image_run.dependencies.potential_dirty(
               "input0", image_remote, 4, {}, ResultSupportTarget::Tensor, 0))
          .at("out")
          .empty(),
      "batched image finite Validation excludes remote pixels");
  auto prepared = d.prepare("numeric.abs", {bad}, {}, "value");
  ExecutionOptions limited;
  limited.maximum_dependency_work = 1;
  auto refused = d.context->execute_fragments(
      take(d.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", point}}, {}, limited);
  require(
      !refused.ok() && refused.status().code == ErrorCode::ResourceExhausted,
      "finite elementwise mandatory work admission");
  ResultRef survivor;
  WeakResultRef retired_source;
  ResourceBudget retired_root;
  {
    Driver retiring;
    retired_root = retiring.root;
    auto owned = retiring.source({ElementType::Float64, {1}}, {double_bits(-7)},
                                 {1, {8}});
    retired_source = owned.weak();
    survivor = take(retiring.run("numeric.abs", {owned}, {}, {}, {}, "value"))
                   .results.at("out");
  }
  require(!retired_source.lock().valid() &&
              read_bits(survivor, {0}) == double_bits(7),
          "finite output owns backing after source and context retirement");
  survivor = {};
  require(retired_root.statistics().live[ResourceKind::Payload] == 0 &&
              retired_root.statistics().live[ResourceKind::Metadata] == 0 &&
              retired_root.statistics().live[ResourceKind::Host] == 0,
          "finite output final release returns Root leases");
}
uint64_t float_bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
void staged_ordered_numeric() {
  Driver d(UINT64_MAX, 16 * 1048576, 4096);
  const std::vector<double> values{1e16, 1, -1e16, 3, -2, 8};
  std::vector<uint64_t> bits;
  for (auto value : values)
    bits.push_back(double_bits(value));
  auto source = d.source({ElementType::Float64, {2, 3}}, bits, {1, {24, 8}});
  double sum = 0;
  for (auto value : values)
    sum += value;
  const auto mean = sum / values.size();
  double variance = 0;
  for (auto value : values) {
    const auto difference = value - mean;
    variance += difference * difference;
  }
  variance /= values.size();
  for (int64_t block : {1, 2, 4, 64}) {
    auto average =
        take(d.run("numeric.mean", {source}, {}, {{"block_size", block}}));
    auto spread =
        take(d.run("numeric.variance", {source}, {}, {{"block_size", block}}));
    require(read_bits(average.results.at("out"), {0}) == double_bits(mean),
            "ordered mean preserves exact row-major left fold across blocks");
    require(read_bits(spread.results.at("out"), {0}) == double_bits(variance),
            "ordered variance preserves exact two-pass arithmetic");
    auto support = take(average.dependencies.source_support());
    require(support.at("input0") == take(Footprint::all({2, 3})),
            "ordered reduction retains all source support");
  }
  auto single =
      d.source({ElementType::Float32, {3}},
               {float_bits(1), float_bits(2), float_bits(3)}, {1, {4}});
  require(read_bits(take(d.run("numeric.mean", {single})).results.at("out"),
                    {0}) == double_bits(2),
          "ordered Float32 mean");
  auto scan_source = d.source({ElementType::Float64, {6}}, bits, {1, {8}});
  auto sparse = take(Footprint::from_regions(
      {6}, {Region({{1, 1}}), Region({{3, 1}}), Region({{5, 1}})}));
  auto output = take(d.run("numeric.ordered_scan", {scan_source}, sparse,
                           {{"block_size", int64_t{2}}}));
  double carry = 0;
  for (uint64_t i = 0; i < values.size(); ++i) {
    carry += values[i];
    if (sparse.contains({i}))
      require(read_bits(output.results.at("out"), {i}) == double_bits(carry),
              "sparse ordered scan preserves strict carries");
  }
  auto dense = take(d.run("numeric.ordered_scan", {scan_source}));
  carry = 0;
  for (uint64_t i = 0; i < values.size(); ++i) {
    carry += values[i];
    require(read_bits(dense.results.at("out"), {i}) == double_bits(carry),
            "dense ordered scan publishes every requested sample");
  }
  for (const auto* name :
       {"numeric.mean", "numeric.variance", "numeric.ordered_scan"}) {
    const auto shape = std::string(name) == "numeric.ordered_scan"
                           ? std::vector<uint64_t>{6}
                           : std::vector<uint64_t>{1};
    auto empty = take(d.run(name, {scan_source}, take(Footprint::none(shape))));
    require(
        take(empty.results.at("out").descriptor()).tensor_coverage(0).empty() &&
            take(empty.dependencies.source_support()).empty(),
        "empty staged numeric request performs static preflight without input "
        "reads");
  }
  auto at_five = take(Footprint::from_regions({6}, {Region({{5, 1}})}));
  auto clean = d.source({ElementType::Float64, {6}},
                        {double_bits(1), double_bits(2), double_bits(3),
                         double_bits(4), double_bits(5), double_bits(6)},
                        {1, {8}});
  auto swapped = d.source({ElementType::Float64, {6}},
                          {double_bits(2), double_bits(1), double_bits(3),
                           double_bits(4), double_bits(5), double_bits(6)},
                          {1, {8}});
  auto changed = d.source({ElementType::Float64, {6}},
                          {double_bits(0), double_bits(0), double_bits(3),
                           double_bits(4), double_bits(5), double_bits(6)},
                          {1, {8}});
  d.context->clear_result_cache();
  auto first = take(d.run("numeric.ordered_scan", {clean}, at_five,
                          {{"block_size", int64_t{2}}}));
  auto second = take(d.run("numeric.ordered_scan", {swapped}, at_five,
                           {{"block_size", int64_t{2}}}));
  auto third = take(d.run("numeric.ordered_scan", {changed}, at_five,
                          {{"block_size", int64_t{2}}}));
  require(first.diagnostics.block_cache_misses == 3 &&
              second.diagnostics.block_cache_hits == 2 &&
              second.diagnostics.block_cache_misses == 1 &&
              third.diagnostics.block_cache_hits == 0 &&
              third.diagnostics.block_cache_misses == 3,
          "block cache keys actual incoming state and supplied bits across "
          "bindings");
  require(read_bits(second.results.at("out"), {5}) == double_bits(21) &&
              read_bits(third.results.at("out"), {5}) == double_bits(18),
          "block hit result and changed incoming state recomputation");
  auto edit = take(Footprint::from_regions({6}, {Region({{0, 1}})}));
  auto dirty = take(second.dependencies.potential_dirty(
      "input0", edit, 7, {}, ResultSupportTarget::Tensor, 0));
  require(dirty.at("out") == at_five &&
              take(second.dependencies.source_support()).at("input0") ==
                  take(Footprint::all({6})),
          "block hits retain current complete prefix evidence");
  auto tail_bad =
      d.source({ElementType::Float64, {6}},
               {double_bits(1), double_bits(2), double_bits(3), double_bits(4),
                double_bits(5), UINT64_C(0x7ff0000000000000)},
               {1, {8}});
  auto at_two = take(Footprint::from_regions({6}, {Region({{2, 1}})}));
  auto prefix = take(d.run("numeric.ordered_scan", {tail_bad}, at_two));
  require(read_bits(prefix.results.at("out"), {2}) == double_bits(6),
          "ordered scan does not read invalid future samples");
  auto bad_scan = d.run("numeric.ordered_scan", {tail_bad});
  require(!bad_scan.ok() &&
              bad_scan.status().code == ErrorCode::OperationFailed &&
              bad_scan.status().message.find("5") != std::string::npos,
          "ordered scan reports the global nonfinite index");
  for (const auto* name : {"numeric.mean", "numeric.variance"}) {
    auto failed = d.run(name, {tail_bad});
    require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed,
            "ordered reductions reject nonfinite inputs");
    bool rejected = false;
    try {
      static_cast<void>(
          d.prepare(name, {single}, {{"block_size", int64_t{0}}}));
    } catch (const std::runtime_error& failure) {
      rejected =
          std::string(failure.what()).find("block_size") != std::string::npos;
    }
    require(rejected,
            "ordered reduction validates block_size before execution");
  }
  ColorArrayDescriptor xyz;
  xyz.model = ColorModel::Xyz;
  auto typed =
      d.source({ElementType::Float64, {2, 3}},
               {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff), 0,
                UINT64_C(0x7ff8000000000000), 0, 0},
               {1, {24, 8}}, {take(encode_color_array(xyz))});
  auto validation =
      d.run("numeric.mean", {typed}, {}, {{"block_size", int64_t{1}}});
  require(!validation.ok() &&
              validation.status().message.find("reduction sum overflow") ==
                  std::string::npos,
          "typed validation completes before strict arithmetic can overflow");
  auto fresh = d.source({ElementType::Float64, {6}},
                        {double_bits(1), double_bits(2), double_bits(3),
                         double_bits(4), double_bits(5), double_bits(6)},
                        {1, {8}});
  auto prepared =
      d.prepare("numeric.ordered_scan", {fresh}, {{"block_size", int64_t{2}}});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  ExecutionOptions options;
  options.maximum_dependency_cache_work = 0;
  auto uncached = take(
      d.context->execute_fragments(frozen, {{"out", at_five}}, {}, options));
  require(read_bits(uncached.results.at("out"), {5}) == double_bits(21) &&
              uncached.diagnostics.block_cache_hits == 0,
          "optional zero cache budget keeps actual scan computation");
  uncached = {};
  options.maximum_dependency_work = 1;
  require(!d.context->execute_fragments(frozen, {{"out", at_five}}, {}, options)
               .ok(),
          "ordered scan observes finite mandatory work budget");
  CancellationSource cancellation;
  cancellation.cancel();
  auto cancelled =
      d.run("numeric.mean", {single}, {}, {}, cancellation.token());
  require(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
          "ordered reduction observes cancellation");
  d.context->clear_result_cache();
}
void element_bits_and_strides() {
  Driver d;
  auto a =
      d.source({ElementType::Float32, {5}},
               {0x80000000, 0x3f800000, 0xbf800000, 0xff800000, 0xff800123},
               {1 + 4 * 4, {-4}});
  auto output = take(d.run("numeric.abs_strict", {a})).results.at("out");
  const uint64_t expected[] = {0x7fc00123, 0x7f800000, 0x3f800000, 0x3f800000,
                               0};
  for (unsigned i = 0; i < 5; ++i)
    require(read_bits(output, {i}) == expected[i],
            "abs IEEE payload/negative stride");
  auto zero = d.source({ElementType::Float32, {5}}, {0x80000000}, {1, {0}});
  auto minimum =
      take(d.run("numeric.minimum_strict", {a, zero})).results.at("out");
  require(read_bits(minimum, {4}) == 0x80000000 &&
              read_bits(minimum, {0}) == 0xffc00123,
          "minimum zero sign and first NaN bits");
  auto integers = d.source({ElementType::Int64, {3}}, {2, 4, 9}, {17, {-8}});
  auto negated =
      take(d.run("numeric.neg_strict", {integers})).results.at("out");
  require(read_bits(negated, {0}) == static_cast<uint64_t>(-9) &&
              read_bits(negated, {2}) == static_cast<uint64_t>(-2),
          "Int64 exact reverse/unaligned reads");
  auto bytes = d.source({ElementType::UInt8, {2}}, {3, 9}, {1, {1}});
  auto sum =
      take(d.run("numeric.add_strict", {bytes, bytes})).results.at("out");
  require(read_bits(sum, {0}) == 6 && read_bits(sum, {1}) == 18,
          "UInt8 exact binary arithmetic");
  auto doubles =
      d.source({ElementType::Float64, {3}},
               {double_bits(4), double_bits(-0.0), double_bits(-1)}, {1, {8}});
  auto root = take(d.run("numeric.sqrt_strict", {doubles})).results.at("out");
  require(read_bits(root, {0}) == double_bits(2) &&
              read_bits(root, {1}) == double_bits(-0.0) &&
              read_bits(root, {2}) == UINT64_C(0x7ff8000000000000),
          "Float64 sqrt domain is an IEEE result");
}
void whole_and_resources() {
  Driver d;
  auto overflowing = d.source({ElementType::Int64, {3}},
                              {1, 2, UINT64_C(0x8000000000000000)}, {1, {8}});
  auto point = take(Footprint::from_regions({3}, {Region({{0, 1}})}));
  auto failed = d.run("numeric.abs_strict", {overflowing}, point);
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom,
          "Whole overflow outside Q fails the run");
  auto source =
      d.source({ElementType::Float32, {2048}}, {0xbf800000}, {1, {0}});
  const auto before = d.root.statistics().issued.work;
  auto computed = take(d.run("numeric.abs_strict", {source}));
  require(read_bits(computed.results.at("out"), {2047}) == 0x3f800000 &&
              d.root.statistics().issued.work - before > 2048 * 1024,
          "operation work is Root charged beyond discovery limit");
  bool validation = false, descriptor = false;
  for (const auto& observation :
       take(computed.dependencies.source_observations())) {
    validation |= (observation.roles & 4U) != 0;
    descriptor |= (observation.roles & 8U) != 0;
  }
  require(validation && descriptor,
          "Whole input Validation and Descriptor witnesses");
  auto changed = take(Footprint::from_regions({2048}, {Region({{2047, 1}})}));
  auto dirty =
      take(computed.dependencies.potential_dirty("input0", changed, 4));
  require(dirty.at("out") == take(Footprint::all({2048})),
          "Whole validation prerequisite dirties full output through singleton "
          "descriptor domain");
  auto prepared = d.prepare("numeric.abs_strict", {source});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto none = take(Footprint::none({2048}));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  const auto work = d.root.statistics().issued.work;
  auto empty = take(d.context->execute_fragments(frozen, {{"out", none}}));
  const auto executed_work = d.root.statistics().issued.work - work;
  // Compare equal-rank Empty requests against a scalar domain. Schema,
  // identity and dependency bookkeeping have a fixed cost independent of N.
  Driver scalar_empty;
  auto scalar_source =
      scalar_empty.source({ElementType::Float32, {1}}, {0xbf800000}, {1, {0}});
  auto scalar_prepared =
      scalar_empty.prepare("numeric.abs_strict", {scalar_source});
  auto scalar_frozen = take(scalar_empty.context->freeze(
      scalar_prepared.plan, scalar_prepared.bindings));
  auto scalar_none = take(Footprint::none({1}));
  const auto scalar_before = scalar_empty.root.statistics().issued.work;
  const auto scalar_payload =
      scalar_empty.root.statistics().live[ResourceKind::Payload];
  auto scalar_result = take(scalar_empty.context->execute_fragments(
      scalar_frozen, {{"out", scalar_none}}));
  const auto scalar_work =
      scalar_empty.root.statistics().issued.work - scalar_before;
  require(take(scalar_result.results.at("out").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              take(scalar_result.dependencies.source_support()).empty() &&
              scalar_empty.root.statistics().live[ResourceKind::Payload] ==
                  scalar_payload,
          "scalar Empty baseline consumes no source or output payload");
  require(
      take(empty.results.at("out").descriptor()).tensor_coverage(0).empty() &&
          d.root.statistics().live[ResourceKind::Payload] == payload &&
          executed_work <= scalar_work + 1024 &&
          std::all_of(empty.diagnostics.operation_timings.begin(),
                      empty.diagnostics.operation_timings.end(),
                      [](const auto& timing) {
                        return timing.computed_elements == 0;
                      }) &&
          take(empty.dependencies.source_support()).empty(),
      ("Empty Whole skips payload allocation and arithmetic: work=" +
       std::to_string(executed_work) + " scalar=" + std::to_string(scalar_work))
          .c_str());
  Driver limited(30000);
  auto input =
      limited.source({ElementType::Float32, {64}}, {0x3f800000}, {1, {0}});
  auto rejected = limited.run("numeric.abs_strict", {input});
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::ResourceExhausted &&
              rejected.status().reason == FailureReason::WorkLimit,
          "Root work exhaustion preserves typed cause");
  CancellationSource stop;
  stop.cancel();
  auto cancelled = d.run("numeric.abs_strict", {source}, {}, {}, stop.token());
  require(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
          "numeric pre-cancel");
}
void typed_and_batches() {
  Driver d;
  auto mask = take(encode_semantic(coverage_semantics()));
  auto valid = d.source({ElementType::Float32, {2, 2}},
                        {0x3e800000, 0x3f000000, 0, 0x3f800000},
                        {1, {16, 8, 4}}, {mask}, {1});
  auto result = take(d.run("numeric.neg_strict", {valid})).results.at("out");
  const auto& output = result.schema().tensors[0];
  require(output.batch_axes.empty() && output.facets.empty() &&
              output.descriptor.shape == std::vector<uint64_t>({1, 2, 2}) &&
              read_bits(result, {0, 0, 1}) == 0xbf000000,
          "generic numeric output flattens batch topology and drops facets");
  auto bad =
      d.source({ElementType::Float32, {2, 2}},
               {0x3e800000, 0x3f000000, 0, 0x40000000}, {1, {8, 4}}, {mask});
  auto point =
      take(Footprint::from_regions({2, 2}, {Region({{0, 1}, {0, 1}})}));
  auto refused = d.run("numeric.abs_strict", {bad}, point);
  require(!refused.ok() &&
              refused.status().code == ErrorCode::InvalidArgument &&
              refused.status().detail.input_id == 11 &&
              refused.status().detail.origin == FailureOrigin::Domain,
          "typed mask is validated outside requested projection");
  ColorArrayDescriptor xyz;
  auto color = take(encode_color_array(xyz));
  auto invalid_color = d.source({ElementType::Float64, {2, 3}},
                                {0, 0, 0, 0, UINT64_C(0x7ff0000000000000), 0},
                                {1, {24, 8}}, {color});
  auto color_failed = d.run("numeric.abs_strict", {invalid_color});
  require(!color_failed.ok() &&
              color_failed.status().reason == FailureReason::InvalidDomain,
          "ColorArray nonfinite fails shared tensor validator");
}
void certified_profiles() {
  Driver d;
  std::vector<uint64_t> zeros(70);
  auto input = d.source({ElementType::Float32, {70}}, zeros, {1, {4}});
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
  unsigned unavailable = 0;
  for (const auto* profile :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    const std::string key = std::string("numeric.exp") + profile;
    auto resolved = d.registry->resolve_traits(key, {metadata}, {});
    if (!resolved.ok()) {
      require(std::string(profile) != "_strict" &&
                  resolved.status().code == ErrorCode::BackendUnavailable,
              "incompatible profile reports BackendUnavailable");
      ++unavailable;
      continue;
    }
    auto result = take(d.run(key, {input}));
    for (uint64_t i = 0; i < 70; ++i)
      require(read_bits(result.results.at("out"), {i}) == 0x3f800000,
              "exp exact zero landmark across full SIMD blocks");
  }
  require(unavailable != 0, "incompatible CPU profile remains unavailable");
  auto numerator = d.source({ElementType::Int64, {3}}, {0, 1, 2}, {1, {8}});
  auto denominator = d.source({ElementType::Int64, {3}}, {2, 2, 2}, {1, {8}});
  auto rational =
      take(d.run("numeric.sinpi_rational_strict", {numerator, denominator}, {},
                 {{"dtype", std::string("float64")}}))
          .results.at("out");
  require(read_bits(rational, {0}) == 0 &&
              read_bits(rational, {1}) == double_bits(1) &&
              read_bits(rational, {2}) == 0,
          "rational pi exact landmarks");
}
void comparison_and_selection() {
  Driver d;
  auto a =
      d.source({ElementType::Float32, {6}},
               {0x80000000, 0x7f800123, 0x7f800000, 0xbf800000, 0x3f800000, 0},
               {1, {4}});
  auto b = d.source({ElementType::Float32, {6}},
                    {0, 0x3f800000, 0x7f800000, 0, 0x40000000, 0x80000000},
                    {1, {4}});
  const std::vector<std::pair<std::string, std::vector<uint64_t>>> expected = {
      {"equal", {1, 0, 1, 0, 0, 1}},   {"not_equal", {0, 1, 0, 1, 1, 0}},
      {"less", {0, 0, 0, 1, 1, 0}},    {"less_equal", {1, 0, 1, 1, 1, 1}},
      {"greater", {0, 0, 0, 0, 0, 0}}, {"greater_equal", {1, 0, 1, 0, 0, 1}}};
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(a.schema());
  for (const auto* profile :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    auto available = d.registry->resolve_traits(
        std::string("numeric.equal") + profile, {metadata, metadata}, {});
    if (!available.ok()) {
      require(available.status().code == ErrorCode::BackendUnavailable,
              "comparison unavailable profile remains explicit");
      continue;
    }
    for (const auto& item : expected) {
      auto result = take(d.run("numeric." + item.first + profile, {a, b}))
                        .results.at("out");
      for (uint64_t i = 0; i < 6; ++i)
        require(read_bits(result, {i}) == item.second[i],
                "comparison IEEE unordered/signed-zero table");
    }
  }
  auto integers =
      d.source({ElementType::Int64, {3}},
               {UINT64_C(0x8000000000000000), UINT64_C(9007199254740993),
                UINT64_C(0x7fffffffffffffff)},
               {1, {8}});
  auto other =
      d.source({ElementType::Int64, {3}},
               {UINT64_C(0x8000000000000001), UINT64_C(9007199254740992),
                UINT64_C(0x7ffffffffffffffe)},
               {1, {8}});
  auto greater = take(d.run("numeric.greater_strict", {integers, other}))
                     .results.at("out");
  require(read_bits(greater, {0}) == 0 && read_bits(greater, {1}) == 1 &&
              read_bits(greater, {2}) == 1,
          "integer predicates do not convert through Float64");
  const double maximum = std::numeric_limits<double>::max();
  auto left = d.source({ElementType::Float64, {3}},
                       {double_bits(maximum), double_bits(1), 1}, {1, {8}});
  auto right = d.source(
      {ElementType::Float64, {3}},
      {double_bits(-maximum), double_bits(std::nextafter(1.0, 2.0)), 0},
      {1, {8}});
  auto close = take(d.run("numeric.is_close_strict", {left, right}, {},
                          {{"atol", 0.0}, {"rtol", 2.0}}))
                   .results.at("out");
  require(read_bits(close, {0}) == 1 && read_bits(close, {1}) == 1 &&
              read_bits(close, {2}) == 1,
          "is_close compares exact thresholds beyond IEEE intermediate range");
  auto boundary = take(d.run("numeric.is_close_strict", {left, right}, {},
                             {{"atol", 0x1p-52}, {"rtol", 0.0}}))
                      .results.at("out");
  auto below =
      take(d.run("numeric.is_close_strict", {left, right}, {},
                 {{"atol", std::nextafter(0x1p-52, 0.0)}, {"rtol", 0.0}}))
          .results.at("out");
  require(read_bits(boundary, {1}) == 1 && read_bits(below, {1}) == 0,
          "is_close exact threshold endpoint and one binary64 predecessor");
  auto condition =
      d.source({ElementType::UInt8, {6}}, {1, 1, 1, 0, 0, 0}, {1, {1}});
  auto selected =
      take(d.run("numeric.select_strict", {condition, a, b})).results.at("out");
  require(
      read_bits(selected, {0}) == 0x80000000 &&
          read_bits(selected, {1}) == 0x7f800123 &&
          read_bits(selected, {4}) == 0x40000000,
      "select copies branch bits including signaling NaN without conversion");
  auto invalid =
      d.source({ElementType::UInt8, {6}}, {0, 0, 0, 0, 0, 2}, {1, {1}});
  auto point = take(Footprint::from_regions({6}, {Region({{0, 1}})}));
  auto failed = d.run("numeric.select_strict", {invalid, a, b}, point);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom,
          "select invalid condition outside Q fails Whole run");
  auto mask = take(encode_semantic(coverage_semantics()));
  auto bad_branch = d.source({ElementType::Float32, {1, 2}},
                             {0x3f800000, 0x40000000}, {1, {8, 4}}, {mask});
  auto good_branch =
      d.source({ElementType::Float32, {1, 2}}, {0, 0}, {1, {8, 4}});
  auto always_false = d.source({ElementType::UInt8, {1, 2}}, {0}, {1, {0, 0}});
  auto branch_failure =
      d.run("numeric.select_strict", {always_false, bad_branch, good_branch});
  require(!branch_failure.ok() &&
              branch_failure.status().code == ErrorCode::InvalidArgument &&
              branch_failure.status().detail.input_id == 12,
          "select validates the complete unselected typed branch");
  auto empty = take(d.run("numeric.select_strict", {invalid, a, b},
                          take(Footprint::none({6}))));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "select Empty does not read invalid conditions or branches");
}
ResultRef row_fragments(Driver& driver, bool related) {
  auto schema = SchemaTemplate{};
  schema.id = "test.fragments";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {2, 2}};
  schema.tensors.push_back(member);
  auto owner = take(driver.root.allocator().allocate(33));
  for (uint64_t i = 0; i < 4; ++i) {
    const auto bits = double_bits(static_cast<double>(i + 1));
    std::memcpy(owner.data() + 1 + i * 8, &bits, 8);
  }
  auto common = std::move(owner).freeze();
  auto builder =
      take(ResultBuilder::start(driver.root, schema, "fragmented.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(driver.root, 1, {0, 8, 0, 0})))
              .ok(),
          "fragment source descriptor");
  auto relation = take(ResultRelation::cartesian(driver.root, 4, {0, 1, 0, 0}));
  for (uint64_t row = 0; row < 2; ++row) {
    std::shared_ptr<const CpuStorage> storage = common;
    if (!related) {
      auto buffer = take(driver.root.allocator().allocate(33));
      std::memcpy(buffer.data(), common->bytes().data(), 33);
      storage = std::move(buffer).freeze();
    }
    require(builder
                .publish_tensor(0, Region({{row, 1}, {0, 2}}),
                                {1 + row * 16, {INT64_MIN, 8}, {row, 0}},
                                storage, relation, {true, true, true, true})
                .ok(),
            "same-owner singleton row publication");
  }
  return take(builder.seal());
}
void layout_views() {
  Driver d;
  auto input = d.source({ElementType::Float64, {2, 3}},
                        {double_bits(1), double_bits(2), double_bits(3),
                         double_bits(4), double_bits(5), double_bits(6)},
                        {41, {-24, -8}});
  auto source_window = take(
      input.acquire_tensor(take(input.descriptor()), 0, Region::whole({2, 3})));
  const auto source_pointer = take(source_window.row_run({0, 0})).data;
  const auto source_owner = source_window.storage_owner_token();
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto reshaped = take(d.run("array.reshape_strict", {input}, {},
                             {{"shape", std::string("3,2")},
                              {"layout", std::string("view")}}))
                      .results.at("out");
  auto reshape_window = take(reshaped.acquire_tensor(
      take(reshaped.descriptor()), 0, Region::whole({3, 2})));
  require(reshape_window.storage_owner_token() == source_owner &&
              take(reshape_window.row_run({0, 0})).data == source_pointer &&
              take(reshape_window.row_run({0, 0})).sample_stride_bytes == -8 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "reshape preserves negative contiguous chunks as zero-payload view");
  for (uint64_t i = 0; i < 6; ++i)
    require(read_bits(reshaped, {i / 2, i % 2}) == double_bits(6 - i),
            "reshape logical order follows complete source ordinal");
  auto transposed = take(d.run(
      "array.transpose_strict", {input}, {},
      {{"permutation", std::string("1,0")}, {"layout", std::string("view")}}));
  auto transpose = transposed.results.at("out");
  require(read_bits(transpose, {2, 1}) == double_bits(1) &&
              read_bits(transpose, {0, 1}) == double_bits(3),
          "transpose permutes signed strides without copying");
  auto changed =
      take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {2, 1}})}));
  require(take(transposed.dependencies.potential_dirty("input0", changed, 1))
                  .at("out") == take(Footprint::all({3, 2})),
          "layout preserves Whole data support despite physical mapping");
  auto starts = d.source({ElementType::Int64, {2}}, {1, 2}, {1, {8}});
  auto steps = d.source({ElementType::Int64, {2}},
                        {UINT64_C(0x8000000000000000), UINT64_MAX}, {1, {8}});
  auto sliced = take(d.run("array.slice_strict", {input, starts, steps}, {},
                           {{"counts", std::string("1,2")},
                            {"layout", std::string("view")}}))
                    .results.at("out");
  require(read_bits(sliced, {0, 0}) == double_bits(1) &&
              read_bits(sliced, {0, 1}) == double_bits(2),
          "slice ignores singleton steps and preserves reverse physical view");
  auto fragment = row_fragments(d, true);
  const auto join_payload = d.root.statistics().live[ResourceKind::Payload];
  auto joined = take(d.run("array.reshape_strict", {fragment}, {},
                           {{"shape", std::string("4")},
                            {"layout", std::string("view")}}))
                    .results.at("out");
  auto original = take(fragment.acquire_tensor(take(fragment.descriptor()), 0,
                                               Region::whole({2, 2})));
  auto joined_window = take(
      joined.acquire_tensor(take(joined.descriptor()), 0, Region::whole({4})));
  require(
      d.root.statistics().live[ResourceKind::Payload] == join_payload &&
          original.storage_owner_token() ==
              joined_window.storage_owner_token() &&
          take(original.row_run({0, 0})).data ==
              take(joined_window.row_run({0})).data,
      "same-owner singleton fragments infer the complete affine row stride");
  for (uint64_t i = 0; i < 4; ++i)
    require(read_bits(joined, {i}) == double_bits(i + 1),
            "fragment view retains all source bytes");
  auto fragmented = row_fragments(d, false);
  auto point = take(Footprint::from_regions({4}, {Region({{0, 1}})}));
  auto unavailable =
      d.run("array.reshape_strict", {fragmented}, point,
            {{"shape", std::string("4")}, {"layout", std::string("view")}});
  require(!unavailable.ok() &&
              unavailable.status().code == ErrorCode::InvalidArgument &&
              unavailable.status().reason == FailureReason::InvalidDomain &&
              unavailable.status().detail.scope == FailureScope::Run,
          "small Q cannot make a multi-owner source a globally affine view");
  for (const auto* layout : {"auto", "dense"}) {
    auto copied = take(d.run("array.reshape_strict", {fragmented}, {},
                             {{"shape", std::string("4")},
                              {"layout", std::string(layout)}}))
                      .results.at("out");
    for (uint64_t i = 0; i < 4; ++i)
      require(read_bits(copied, {i}) == double_bits(i + 1),
              "Auto and Dense preserve fragmented raw values");
  }
  auto zero = d.source({ElementType::Float64, {UINT64_C(1) << 40}},
                       {double_bits(7)}, {1, {0}});
  const auto before = d.root.statistics().live[ResourceKind::Payload];
  auto huge = take(d.run("array.reshape_strict", {zero}, {},
                         {{"shape", std::string("1048576,1048576")},
                          {"layout", std::string("view")}}))
                  .results.at("out");
  require(
      read_bits(huge, {1048575, 1048575}) == double_bits(7) &&
          d.root.statistics().live[ResourceKind::Payload] == before,
      "large zero-stride reshape proves chunks without materializing samples");
  source_window = {};
  original = {};
  input = {};
  fragment = {};
  d.context.reset();
  require(read_bits(joined, {3}) == double_bits(4) &&
              read_bits(reshaped, {2, 1}) == double_bits(1),
          "array views retain source owner after input and context retirement");
}
void layout_profiles() {
  Driver d;
  auto input = d.source({ElementType::Float64, {2, 3}},
                        {double_bits(1), double_bits(2), double_bits(3),
                         double_bits(4), double_bits(5), double_bits(6)},
                        {1, {24, 8}});
  auto starts = d.source({ElementType::Int64, {2}}, {0, 0}, {1, {8}});
  auto steps = d.source({ElementType::Int64, {2}}, {1, 1}, {1, {8}});
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    for (const auto* kind : {"reshape", "transpose", "slice"}) {
      std::vector<ResultRef> inputs{input};
      std::map<std::string, ParameterValue> parameters{
          {"layout", std::string("dense")}};
      if (std::string(kind) == "reshape") {
        parameters["shape"] = std::string("3,2");
      } else if (std::string(kind) == "transpose") {
        parameters["permutation"] = std::string("1,0");
      } else {
        inputs.push_back(starts);
        inputs.push_back(steps);
        parameters["counts"] = std::string("2,3");
      }
      std::vector<OperationMetadata> metadata;
      for (const auto& value : inputs) {
        OperationMetadata member;
        member.result_schema = std::make_shared<SchemaTemplate>(value.schema());
        metadata.push_back(std::move(member));
      }
      const auto key = std::string("array.") + kind + suffix;
      auto resolved = d.registry->resolve_traits(key, metadata, parameters);
      if (!resolved.ok()) {
        require(std::string(suffix) != "_strict" &&
                    resolved.status().code == ErrorCode::BackendUnavailable,
                "unavailable array profile reports BackendUnavailable");
        continue;
      }
      auto output = take(d.run(key, inputs, {}, parameters)).results.at("out");
      require(read_bits(output, std::string(kind) == "slice"
                                    ? std::vector<uint64_t>{1, 2}
                                    : std::vector<uint64_t>{2, 1}) ==
                  double_bits(6),
              "each available layout profile preserves raw values");
    }
  }
  auto bad = d.source({ElementType::Int64, {1}}, {1}, {1, {8}});
  std::vector<OperationMetadata> metadata;
  for (const auto& value : {input, starts, bad}) {
    OperationMetadata member;
    member.result_schema = std::make_shared<SchemaTemplate>(value.schema());
    metadata.push_back(std::move(member));
  }
  auto rejected = d.registry->resolve_traits(
      "array.slice_strict", metadata,
      {{"counts", std::string("1,1")}, {"layout", std::string("view")}});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "excluded singleton step still obeys complete static metadata "
          "validation");
}
void constant_and_broadcast() {
  Driver d;
  const auto word = UINT64_C(0x7ff0000000000123);
  std::vector<uint64_t> oversized(4096, 0);
  oversized[0] = word;
  auto scalar = d.source({ElementType::Float64, {1}}, oversized, {1, {8}});
  const auto scalar_weak = scalar.weak();
  const auto scalar_id = scalar.object_id();
  auto scalar_window = take(
      scalar.acquire_tensor(take(scalar.descriptor()), 0, Region::whole({1})));
  const auto before = d.root.statistics().live[ResourceKind::Payload];
  auto huge = take(d.run("numeric.constant_strict", {scalar}, {},
                         {{"shape", std::string("1048576,1048576")},
                          {"layout", std::string("view")}}))
                  .results.at("out");
  auto window = take(huge.acquire_tensor(take(huge.descriptor()), 0,
                                         Region::whole({1048576, 1048576})));
  require(
      d.root.statistics().live[ResourceKind::Payload] == before + 8 &&
          window.storage_owner_token() != scalar_window.storage_owner_token() &&
          read_bits(huge, {1048575, 1048575}) == word &&
          huge.schema().tensors[0].atomic_trailing_axes == 2,
      "constant view copies only one scalar and preserves full-array tuple "
      "identity");
  auto source = d.source({ElementType::Int64, {2, 1, 3}}, {1, 2, 3, 4, 5, 6},
                         {41, {-24, INT64_MIN, -8}});
  auto original = take(source.acquire_tensor(take(source.descriptor()), 0,
                                             Region::whole({2, 1, 3})));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto broadcast = take(d.run("numeric.broadcast_strict", {source}, {},
                              {{"shape", std::string("3,2,4")},
                               {"axis_map", std::string("1,2,0")},
                               {"layout", std::string("view")}}))
                       .results.at("out");
  auto borrowed = take(broadcast.acquire_tensor(take(broadcast.descriptor()), 0,
                                                Region::whole({3, 2, 4})));
  require(
      d.root.statistics().live[ResourceKind::Payload] == payload &&
          borrowed.storage_owner_token() == original.storage_owner_token() &&
          take(borrowed.row_run({0, 0, 0})).data ==
              take(original.row_run({0, 0, 0})).data,
      "broadcast view preserves source owner, negative strides and singleton "
      "expansion");
  for (uint64_t i = 0; i < 3; ++i)
    for (uint64_t j = 0; j < 2; ++j)
      for (uint64_t k = 0; k < 4; ++k)
        require(read_bits(broadcast, {i, j, k}) == 6 - j * 3 - i,
                "broadcast logical axis permutation remains exact");
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    for (bool constant : {true, false}) {
      const auto input = constant ? scalar : source;
      const auto key =
          std::string(constant ? "numeric.constant" : "numeric.broadcast") +
          suffix;
      std::map<std::string, ParameterValue> parameters{
          {"shape", std::string(constant ? "2,8" : "3,2,4")},
          {"layout", std::string("dense")}};
      if (!constant)
        parameters["axis_map"] = std::string("1,2,0");
      OperationMetadata metadata;
      metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
      auto traits = d.registry->resolve_traits(key, {metadata}, parameters);
      if (!traits.ok()) {
        require(std::string(suffix) != "_strict" &&
                    traits.status().code == ErrorCode::BackendUnavailable,
                "unavailable array profile returns BackendUnavailable");
        continue;
      }
      auto dense = take(d.run(key, {input}, {}, parameters)).results.at("out");
      require(constant ? read_bits(dense, {1, 7}) == word
                       : read_bits(dense, {2, 1, 3}) == 1,
              "available array dense profiles copy raw sNaN/integer bits");
    }
  }
  auto multiple = row_fragments(d, false);
  auto failed = d.run("numeric.broadcast_strict", {multiple}, {},
                      {{"shape", std::string("2,2,3")},
                       {"axis_map", std::string("0,1")},
                       {"layout", std::string("view")}});
  require(!failed.ok() &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().detail.scope == FailureScope::Run,
          "broadcast view requires one complete affine owner");
  original = {};
  scalar_window = {};
  source = {};
  scalar = {};
  d.context.reset();
  require(!scalar_weak.lock().valid() &&
              huge.association() == ResourceVector<uint64_t>{scalar_id} &&
              read_bits(broadcast, {2, 1, 3}) == 1 &&
              read_bits(huge, {1048575, 1048575}) == word,
          "scalar-copy releases oversized source while preserving association "
          "and borrowed views");
}
void array_association_ownership() {
  for (bool constant : {true, false}) {
    Driver d;
    std::weak_ptr<const CpuStorage> storage;
    std::vector<uint64_t> bits(constant ? 512 : 3, 0);
    if (constant)
      bits[125] = double_bits(7);
    else
      bits = {double_bits(1), double_bits(2), double_bits(3)};
    auto source =
        d.source({ElementType::Float64, {constant ? 1U : 3U}}, bits,
                 constant ? StridedLayout{1001, {8}} : StridedLayout{17, {-8}},
                 {}, {}, &storage);
    const auto source_weak = source.weak();
    const auto source_id = source.object_id();
    std::map<std::string, ParameterValue> parameters{
        {"shape", std::string(constant ? "8,8" : "4,3")},
        {"layout", std::string("view")}};
    if (!constant)
      parameters["axis_map"] = std::string("1");
    auto execution = take(
        d.run(constant ? "numeric.constant_strict" : "numeric.broadcast_strict",
              {source}, {}, parameters));
    auto output = execution.results.at("out");
    auto evidence = execution.dependencies;
    execution.results.clear();
    auto window = take(output.acquire_tensor(
        take(output.descriptor()), 0,
        Region::whole(output.schema().tensors[0].sample_shape())));
    source = {};
    d.context.reset();
    require(output.association() == ResourceVector<uint64_t>{source_id} &&
                read_bits(output, constant ? std::vector<uint64_t>{7, 7}
                                           : std::vector<uint64_t>{3, 2}) ==
                    double_bits(constant ? 7 : 1),
            "escaped array keeps exact source association and readable bytes");
    bool data = false, validation = false, descriptor = false;
    for (const auto& observation : take(evidence.source_observations())) {
      data |= (observation.roles & 1) != 0;
      validation |= (observation.roles & 4) != 0;
      descriptor |= (observation.roles & 8) != 0;
    }
    require(data && validation && descriptor,
            "escaped dependency bundle preserves data/validation/descriptor "
            "witnesses without payload");
    require(constant
                ? storage.expired() && !source_weak.lock().valid() &&
                      d.root.statistics().live[ResourceKind::Payload] == 8
                : !storage.expired() && source_weak.lock().valid() &&
                      d.root.statistics().live[ResourceKind::Payload] == 25,
            "constant releases oversized source while broadcast keeps its "
            "physical owner");
    output = {};
    require(!constant ? !storage.expired() : true,
            "last borrowed output window still pins broadcast backing");
    window = {};
    require(storage.expired() && !source_weak.lock().valid() &&
                d.root.statistics().live[ResourceKind::Payload] == 0,
            "last result/window releases payload despite escaped dependency "
            "evidence");
  }
}
void interpolation_workflows() {
  Driver d;
  auto input = d.source({ElementType::Float64, {7}},
                        {double_bits(-1), 0, double_bits(.25), double_bits(.5),
                         double_bits(.75), double_bits(1), double_bits(2)},
                        {1, {8}});
  auto zero = d.source({ElementType::Float64, {7}}, {0}, {1, {0}});
  auto one = d.source({ElementType::Float64, {7}}, {double_bits(1)}, {1, {0}});
  auto a = d.source({ElementType::Float64, {7}}, {double_bits(10)}, {1, {0}});
  auto b = d.source({ElementType::Float64, {7}}, {double_bits(20)}, {1, {0}});
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    auto traits = d.registry->resolve_traits(
        std::string("numeric.smoothstep") + suffix,
        std::vector<OperationMetadata>(3, metadata), {});
    if (!traits.ok()) {
      require(std::string(suffix) != "_strict" &&
                  traits.status().code == ErrorCode::BackendUnavailable,
              "unavailable interpolation profile reports BackendUnavailable");
      continue;
    }
    auto smooth = take(
        d.run(std::string("numeric.smoothstep") + suffix, {input, zero, one}));
    auto result = smooth.results.at("out");
    auto mixed =
        take(d.run(std::string("numeric.mix") + suffix, {a, b, result}))
            .results.at("out");
    const double expected[] = {0, 0, .15625, .5, .84375, 1, 1};
    for (uint64_t i = 0; i < 7; ++i) {
      require(read_bits(result, {i}) == double_bits(expected[i]),
              "smoothstep exact dyadic polynomial through public Result path");
      require(read_bits(mixed, {i}) == double_bits(10 + 10 * expected[i]),
              "smoothstep to mix composition");
    }
    auto changed = take(Footprint::from_regions({7}, {Region({{6, 1}})}));
    for (unsigned port = 0; port < 3; ++port)
      for (uint32_t role : {1U, 4U})
        require(take(smooth.dependencies.potential_dirty(
                         "input" + std::to_string(port), changed, role))
                        .at("out") == take(Footprint::all({7})),
                "all interpolation ports retain Whole input obligations");
    unsigned descriptors = 0;
    for (const auto& observation :
         take(smooth.dependencies.source_observations()))
      if (observation.roles & 8U)
        ++descriptors;
    require(descriptors == 3,
            "interpolation records descriptor witnesses for all three inputs");
  }
  auto negative_max = d.source({ElementType::Float64, {1}},
                               {UINT64_C(0xffefffffffffffff)}, {1, {8}});
  auto positive_max = d.source({ElementType::Float64, {1}},
                               {UINT64_C(0x7fefffffffffffff)}, {1, {8}});
  auto half =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {8}});
  auto midpoint = d.source({ElementType::Float64, {1}}, {0}, {1, {8}});
  require(read_bits(take(d.run("numeric.mix_strict",
                               {negative_max, positive_max, half}))
                        .results.at("out"),
                    {0}) == 0 &&
              read_bits(take(d.run("numeric.smoothstep_strict",
                                   {midpoint, negative_max, positive_max}))
                            .results.at("out"),
                        {0}) == double_bits(.5),
          "exact interpolation handles edge spans beyond floating range");
}
void interpolation_bits_and_batches() {
  Driver d;
  auto a = d.source(
      {ElementType::Float32, {2, 3}},
      {0x7f800123, 0x3f800000, 0xff800000, 0xff800123, 0x80000000, 0x80000000},
      {21, {24, -12, -4}}, {}, {1});
  auto b = d.source({ElementType::Float32, {1, 2, 3}},
                    {0, 0xff800456, 0x3f800000, 0x7f800000, 0x3f800000, 0},
                    {1, {24, 12, 4}});
  auto t =
      d.source({ElementType::Float32, {1, 2, 3}},
               {0x80000000, 0x3f800000, 0x3f000000, 0x3f000000, 0x3e800000, 0},
               {1, {24, 12, 4}});
  std::feclearexcept(FE_ALL_EXCEPT);
  auto mixed = take(d.run("numeric.mix_strict", {a, b, t})).results.at("out");
  require((std::fetestexcept(FE_INVALID) & FE_INVALID) == 0,
          "raw interpolation endpoints do not raise invalid for sNaN");
  const uint64_t expected[] = {0x80000000, 0xff800456, 0xffc00123,
                               0x7fc00000, 0x3f800000, 0x7f800123};
  for (uint64_t i = 0; i < 6; ++i)
    require(read_bits(mixed, {0, i / 3, i % 3}) == expected[i],
            "mix preserves endpoint bits and interior IEEE precedence");
  require(mixed.schema().tensors[0].batch_axes.empty() &&
              mixed.schema().tensors[0].facets.empty() &&
              mixed.schema().tensors[0].descriptor.shape ==
                  std::vector<uint64_t>({1, 2, 3}),
          "interpolation flattens batch axes into ordinary output axes");
  auto x = d.source({ElementType::Float32, {3}},
                    {0xff800000, 0x7f800000, 0xff800123}, {1, {4}});
  auto low = d.source({ElementType::Float32, {3}}, {0}, {1, {0}});
  auto high = d.source({ElementType::Float32, {3}}, {0x3f800000}, {1, {0}});
  auto smooth = take(d.run("numeric.smoothstep_strict", {x, low, high}))
                    .results.at("out");
  require(read_bits(smooth, {0}) == 0 && read_bits(smooth, {1}) == 0x3f800000 &&
              read_bits(smooth, {2}) == 0xffc00123,
          "smoothstep clamps infinity and quiets the input NaN payload");
  d.context.reset();
  require(read_bits(mixed, {0, 1, 2}) == 0x7f800123,
          "interpolation output remains readable after context retirement");
}
void interpolation_boundaries() {
  Driver d;
  auto zero = d.source({ElementType::Float32, {1, 2}}, {0}, {1, {0, 0}});
  auto one =
      d.source({ElementType::Float32, {1, 2}}, {0x3f800000}, {1, {0, 0}});
  auto invalid =
      d.source({ElementType::Float32, {1, 2}}, {0, 0x40000000}, {1, {8, 4}});
  auto point =
      take(Footprint::from_regions({1, 2}, {Region({{0, 1}, {0, 1}})}));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed = d.run("numeric.mix_strict", {zero, one, invalid}, point);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom &&
              failed.status().message.find("InvalidMixFactor: port=2") !=
                  std::string::npos &&
              failed.status().message.find("coordinate=[0,1]") !=
                  std::string::npos &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "invalid factor outside Q fails Whole with diagnostics and rollback");
  auto nan =
      d.source({ElementType::Float32, {1, 2}}, {0x7f800123}, {1, {0, 0}});
  auto edges =
      d.source({ElementType::Float32, {1, 2}}, {0x3f800000, 0}, {1, {8, 4}});
  auto bad_edge = d.run("numeric.smoothstep_strict", {nan, zero, edges}, point);
  require(!bad_edge.ok() &&
              bad_edge.status().message.find("InvalidEdges: port=1") !=
                  std::string::npos &&
              bad_edge.status().message.find("coordinate=[0,1]") !=
                  std::string::npos,
          "smoothstep validates Q-outside edges before NaN propagation");
  auto typed =
      d.source({ElementType::Float32, {1, 2}}, {0, 0x40000000}, {1, {8, 4}},
               {take(encode_semantic(coverage_semantics()))});
  auto unselected = d.run("numeric.mix_strict", {zero, typed, zero}, point);
  require(!unselected.ok() &&
              unselected.status().code == ErrorCode::InvalidArgument &&
              unselected.status().detail.origin == FailureOrigin::Domain &&
              unselected.status().detail.input_id == 12,
          "mix validates the unselected endpoint outside Q");
  auto empty = take(d.run("numeric.mix_strict", {zero, typed, invalid},
                          take(Footprint::none({1, 2}))));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty interpolation skips payload validation and arithmetic");
  Driver limited(30000);
  auto many =
      limited.source({ElementType::Float32, {1024}}, {0x3f000000}, {1, {0}});
  const auto live = limited.root.statistics().live[ResourceKind::Payload];
  auto exhausted = limited.run("numeric.mix_strict", {many, many, many});
  require(!exhausted.ok() &&
              exhausted.status().code == ErrorCode::ResourceExhausted &&
              exhausted.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] == live,
          "interpolation work exhaustion preserves cause and releases output");
  CancellationSource stop;
  stop.cancel();
  auto cancelled = d.run("numeric.smoothstep_strict", {zero, zero, one}, {}, {},
                         stop.token());
  require(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
          "interpolation pre-cancellation");
}
void range_workflows() {
  Driver d;
  auto input = d.source(
      {ElementType::Float64, {4}},
      {double_bits(0), double_bits(.5), double_bits(1), double_bits(2)},
      {1, {8}});
  auto zero = d.source({ElementType::Float64, {4}}, {double_bits(0)}, {1, {0}});
  auto one = d.source({ElementType::Float64, {4}}, {double_bits(1)}, {1, {0}});
  auto maximum =
      d.source({ElementType::Float64, {4}}, {double_bits(255)}, {1, {0}});
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    const auto key = std::string("numeric.remap_range") + suffix;
    auto traits = d.registry->resolve_traits(
        key, std::vector<OperationMetadata>(5, metadata), {});
    if (!traits.ok()) {
      require(std::string(suffix) != "_strict" &&
                  traits.status().code == ErrorCode::BackendUnavailable,
              "unavailable range profile returns BackendUnavailable");
      continue;
    }
    auto mapped = take(d.run(key, {input, zero, one, zero, maximum}));
    auto result = mapped.results.at("out");
    const double expected[] = {0, 127.5, 255, 510};
    for (unsigned i = 0; i < 4; ++i)
      require(read_bits(result, {i}) == double_bits(expected[i]),
              "range rational formula preserves exact dyadic landmarks");
    auto clipped = take(d.run(std::string("numeric.clamp") + suffix,
                              {result, zero, maximum}))
                       .results.at("out");
    require(
        read_bits(clipped, {1}) == double_bits(127.5) &&
            read_bits(clipped, {3}) == double_bits(255) &&
            clipped.schema().tensors[0].facets.empty(),
        "Result remap to clamp composition preserves matching logical shape");
    auto changed = take(Footprint::from_regions({4}, {Region({{3, 1}})}));
    require(take(mapped.dependencies.potential_dirty("input4", changed, 1))
                    .at("out") == take(Footprint::all({4})),
            "every range operand has Whole data support");
  }
  auto special =
      d.source({ElementType::Float64, {3}},
               {UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000123),
                UINT64_C(0x7ff0000000000000)},
               {1, {8}});
  auto lower = d.source({ElementType::Float64, {3}}, {0}, {1, {0}});
  auto upper = d.source({ElementType::Float64, {3}},
                        {UINT64_C(0x7ff0000000000000)}, {1, {0}});
  auto clamped = take(d.run("numeric.clamp_strict", {special, lower, upper}))
                     .results.at("out");
  require(read_bits(clamped, {0}) == UINT64_C(0x8000000000000000) &&
              read_bits(clamped, {1}) == UINT64_C(0x7ff8000000000123) &&
              read_bits(clamped, {2}) == UINT64_C(0x7ff0000000000000),
          "clamp retains in-range signed zero and quiets input NaN bits");
  auto invalid = d.source({ElementType::Float64, {4}},
                          {double_bits(1), double_bits(1), double_bits(1),
                           UINT64_C(0x7ff0000000000001)},
                          {1, {8}});
  auto failed = d.run("numeric.clamp_strict", {input, zero, invalid},
                      take(Footprint::from_regions({4}, {Region({{0, 1}})})));
  require(
      !failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
          failed.status().reason == FailureReason::InvalidDomain &&
          failed.status().detail.scope == FailureScope::Run &&
          !failed.status().detail.atom &&
          failed.status().message.find("coordinate=[3]") != std::string::npos,
      "bounds outside projected Q fail the whole range with global coordinate");
  auto midpoint = d.source({ElementType::Float64, {1}}, {0}, {1, {8}});
  auto negative_max = d.source({ElementType::Float64, {1}},
                               {UINT64_C(0xffefffffffffffff)}, {1, {8}});
  auto positive_max = d.source({ElementType::Float64, {1}},
                               {UINT64_C(0x7fefffffffffffff)}, {1, {8}});
  auto negative_one =
      d.source({ElementType::Float64, {1}}, {double_bits(-1)}, {1, {8}});
  auto positive_one =
      d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {8}});
  auto precise = take(d.run("numeric.remap_range_strict",
                            {midpoint, negative_max, positive_max, negative_one,
                             positive_one}))
                     .results.at("out");
  require(read_bits(precise, {0}) == 0,
          "exact range rational avoids overflowing intermediate source "
          "subtraction");
  auto s_nan = d.source({ElementType::Float64, {1}},
                        {UINT64_C(0x7ff0000000000011)}, {1, {8}});
  auto bad_bounds =
      d.run("numeric.remap_range_strict",
            {s_nan, positive_one, negative_one, negative_one, positive_one});
  require(!bad_bounds.ok() &&
              bad_bounds.status().reason == FailureReason::InvalidDomain &&
              bad_bounds.status().message.find("InvalidBounds") !=
                  std::string::npos,
          "input NaN does not hide invalid remap bounds");
  auto empty = take(
      d.run("numeric.remap_range_strict",
            {s_nan, positive_one, negative_one, negative_one, positive_one},
            take(Footprint::none({1}))));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty range skips invalid bounds and payload work");
}
void range_boundaries() {
  Driver d;
  auto integers = d.source(
      {ElementType::Int64, {4}},
      {UINT64_C(0x8000000000000000), static_cast<uint64_t>(-3), 2, INT64_MAX},
      {25, {-8}});
  auto integer_low = d.source({ElementType::Int64, {4}},
                              {static_cast<uint64_t>(-2)}, {1, {0}});
  auto integer_high = d.source({ElementType::Int64, {4}}, {1}, {1, {0}});
  auto clipped =
      take(d.run("numeric.clamp_strict", {integers, integer_low, integer_high}))
          .results.at("out");
  require(read_bits(clipped, {0}) == 1 &&
              read_bits(clipped, {2}) == static_cast<uint64_t>(-2) &&
              read_bits(clipped, {3}) == static_cast<uint64_t>(-2),
          "Int64 clamp uses exact signed order across extrema and reversed "
          "unaligned input");
  auto bytes = d.source({ElementType::UInt8, {3}}, {0, 255, 17}, {3, {-1}});
  auto byte_low = d.source({ElementType::UInt8, {3}}, {3}, {1, {0}});
  auto byte_high = d.source({ElementType::UInt8, {3}}, {9}, {1, {0}});
  clipped = take(d.run("numeric.clamp_strict", {bytes, byte_low, byte_high}))
                .results.at("out");
  require(read_bits(clipped, {0}) == 9 && read_bits(clipped, {1}) == 9 &&
              read_bits(clipped, {2}) == 3,
          "UInt8 clamp preserves raw selections with zero/negative strides");
  auto floats = d.source({ElementType::Float32, {4}},
                         {0, 0x3f000000, 0x3f800000, 0x40000000}, {13, {-4}});
  auto low = d.source({ElementType::Float32, {4}}, {0}, {1, {0}});
  auto high = d.source({ElementType::Float32, {4}}, {0x3f800000}, {1, {0}});
  clipped = take(d.run("numeric.clamp_strict", {floats, low, high}))
                .results.at("out");
  require(read_bits(clipped, {0}) == 0x3f800000 &&
              read_bits(clipped, {2}) == 0x3f000000 &&
              read_bits(clipped, {3}) == 0,
          "Float32 clamp retains exact selected source bits");
  const auto mask = take(encode_semantic(coverage_semantics()));
  auto typed = d.source({ElementType::Float32, {2, 2}},
                        {0x3e800000, 0x3f000000, 0, 0x3f800000},
                        {1, {16, 8, 4}}, {mask}, {1});
  low = d.source({ElementType::Float32, {1, 2, 2}}, {0}, {1, {0, 0, 0}});
  high =
      d.source({ElementType::Float32, {1, 2, 2}}, {0x3f800000}, {1, {0, 0, 0}});
  clipped =
      take(d.run("numeric.clamp_strict", {typed, low, high})).results.at("out");
  require(clipped.schema().tensors[0].batch_axes.empty() &&
              clipped.schema().tensors[0].facets.empty() &&
              clipped.schema().tensors[0].descriptor.shape ==
                  std::vector<uint64_t>{1, 2, 2} &&
              read_bits(clipped, {0, 1, 1}) == 0x3f800000,
          "range uses full logical batch shape and drops typed output facets");
  auto bad = d.source({ElementType::Float32, {2, 2}},
                      {0x3e800000, 0x3f000000, 0, 0x40000000}, {1, {16, 8, 4}},
                      {mask}, {1});
  auto failed = d.run("numeric.clamp_strict", {bad, low, high},
                      take(Footprint::from_regions(
                          {1, 2, 2}, {Region({{0, 1}, {0, 1}, {0, 1}})})));
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().reason == FailureReason::None &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.input_id == 11,
          "range validates complete typed source before numeric clipping");
  Driver bounded(30000);
  auto x =
      bounded.source({ElementType::Float64, {64}}, {double_bits(.5)}, {1, {0}});
  auto a =
      bounded.source({ElementType::Float64, {64}}, {double_bits(0)}, {1, {0}});
  auto b =
      bounded.source({ElementType::Float64, {64}}, {double_bits(1)}, {1, {0}});
  auto c =
      bounded.source({ElementType::Float64, {64}}, {double_bits(-1)}, {1, {0}});
  const auto payload = bounded.root.statistics().live[ResourceKind::Payload];
  auto exhausted = bounded.run("numeric.remap_range_strict", {x, a, b, c, b});
  require(!exhausted.ok() &&
              exhausted.status().code == ErrorCode::ResourceExhausted &&
              exhausted.status().reason == FailureReason::WorkLimit &&
              bounded.root.statistics().live[ResourceKind::Payload] == payload,
          "exact rational work exhaustion preserves first cause and releases "
          "scratch/output");
  CancellationSource stop;
  stop.cancel();
  auto cancelled =
      d.run("numeric.clamp_strict", {typed, low, high}, {}, {}, stop.token());
  require(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
          "range pre-cancellation does not enter sample arithmetic");
}
void ordering_workflows() {
  Driver d;
  auto input = d.source({ElementType::Int64, {4}}, {3, 1, 1, 2}, {1, {8}});
  auto probability =
      d.source({ElementType::Float64, {1}}, {double_bits(.25)}, {1, {0}});
  auto numbers = d.source({ElementType::Int64, {4}}, {0, 10, 20, 30}, {1, {8}});
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    const auto sort = std::string("array.sort") + suffix;
    auto traits = d.registry->resolve_traits(
        sort, {metadata}, {{"axis", static_cast<int64_t>(0)}});
    if (!traits.ok()) {
      require(std::string(suffix) != "_strict" &&
                  traits.status().code == ErrorCode::BackendUnavailable,
              "ordering incompatible profile reports BackendUnavailable");
      continue;
    }
    auto values =
        take(d.run(sort, {input}, {}, {{"axis", static_cast<int64_t>(0)}}));
    auto indices = take(d.run(
        sort, {input}, {}, {{"axis", static_cast<int64_t>(0)}}, {}, "indices"));
    const uint64_t expected_values[] = {1, 1, 2, 3},
                   expected_indices[] = {1, 2, 3, 0};
    for (unsigned i = 0; i < 4; ++i)
      require(
          read_bits(values.results.at("out"), {i}) == expected_values[i] &&
              read_bits(indices.results.at("out"), {i}) == expected_indices[i],
          "stable sort independently publishes raw values or original axis "
          "indices");
    auto quantile = take(d.run(std::string("numeric.quantile") + suffix,
                               {numbers, probability}, {},
                               {{"axis", static_cast<int64_t>(0)},
                                {"dtype", std::string("float64")}}))
                        .results.at("out");
    require(
        read_bits(quantile, {0}) == double_bits(7.5),
        "quantile position and exact interpolation preserve dyadic landmark");
    auto changed = take(Footprint::from_regions({4}, {Region({{3, 1}})}));
    require(take(indices.dependencies.potential_dirty("input0", changed, 1))
                    .at("out") == take(Footprint::all({4})),
            "sort indices retain Whole source data support");
  }
  auto prepared = d.prepare("array.sort_strict", {input},
                            {{"axis", static_cast<int64_t>(0)}});
  auto document = prepared.graph->snapshot().document();
  document.outputs.push_back({"indices", 7, "indices"});
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(d.registry).compile(*graph));
  auto both = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({4}))},
       {"indices", take(Footprint::all({4}))}}));
  require(
      both.results.size() == 2 &&
          read_bits(both.results.at("indices"), {3}) == 0 &&
          read_bits(both.results.at("out"), {3}) == 3,
      "sort selected outputs have independent Result owners in one workflow");
  auto ieee =
      d.source({ElementType::Float64, {7}},
               {UINT64_C(0xfff0000000000003), UINT64_C(0x7ff0000000000000),
                UINT64_C(0x8000000000000000), 0, UINT64_C(0xfff0000000000000),
                UINT64_C(0x7ff0000000000021), double_bits(1)},
               {1, {8}});
  auto sorted = take(d.run("array.sort_strict", {ieee}, {},
                           {{"axis", static_cast<int64_t>(0)}}))
                    .results.at("out");
  auto indices = take(d.run("array.sort_strict", {ieee}, {},
                            {{"axis", static_cast<int64_t>(0)}}, {}, "indices"))
                     .results.at("out");
  const uint64_t order[] = {4, 2, 3, 6, 1, 0, 5};
  for (unsigned i = 0; i < 7; ++i)
    require(read_bits(indices, {i}) == order[i],
            "stable ordering classifies zero ties and NaNs once");
  require(read_bits(sorted, {1}) == UINT64_C(0x8000000000000000) &&
              read_bits(sorted, {2}) == 0 &&
              read_bits(sorted, {5}) == UINT64_C(0xfff0000000000003) &&
              read_bits(sorted, {6}) == UINT64_C(0x7ff0000000000021),
          "sort copies signaling NaN and signed-zero bits without arithmetic "
          "conversion");
  auto qzero = d.source({ElementType::Float64, {1}},
                        {UINT64_C(0x8000000000000000)}, {1, {8}});
  auto qnan = take(d.run("numeric.quantile_strict", {ieee, qzero}, {},
                         {{"axis", static_cast<int64_t>(0)},
                          {"dtype", std::string("float64")}}))
                  .results.at("out");
  require(read_bits(qnan, {0}) == UINT64_C(0xfff8000000000003),
          "quantile chooses first original NaN even at a finite endpoint");
  auto zero_one =
      d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
  auto tiny = d.source({ElementType::Float64, {1}}, {1}, {1, {8}});
  auto minimum = take(d.run("numeric.quantile_strict", {zero_one, tiny}, {},
                            {{"axis", static_cast<int64_t>(0)},
                             {"dtype", std::string("float64")}}))
                     .results.at("out");
  require(
      read_bits(minimum, {0}) == 1,
      "quantile subnormal probability uses exact large-denominator position");
  auto invalid = d.source({ElementType::Float64, {1}},
                          {UINT64_C(0x7ff0000000000001)}, {1, {8}});
  auto rejected = d.run(
      "numeric.quantile_strict", {zero_one, invalid}, {},
      {{"axis", static_cast<int64_t>(0)}, {"dtype", std::string("float64")}});
  require(!rejected.ok() &&
              rejected.status().reason == FailureReason::InvalidDomain &&
              rejected.status().detail.scope == FailureScope::Run &&
              rejected.status().message.find("InvalidQuantileProbability") !=
                  std::string::npos,
          "invalid dynamic probability fails a nonempty quantile run");
}

void curve_workflows() {
  Driver d;
  auto x =
      d.source({ElementType::Float32, {4}},
               {float_bits(4), float_bits(3), float_bits(1), 0}, {13, {-4}});
  auto y = d.source({ElementType::Float64, {4}},
                    {0, double_bits(3), double_bits(2), 0}, {25, {-8}});
  auto columns = d.source({ElementType::Float64, {2}},
                          {0, double_bits(1), double_bits(3), double_bits(-5),
                           double_bits(2), double_bits(-3), 0, double_bits(1)},
                          {49, {-16, 8}}, {}, {4});
  auto query = d.source({ElementType::Float64, {10}},
                        {double_bits(3.5), double_bits(-1), double_bits(1),
                         double_bits(.5), double_bits(5), double_bits(2), 0,
                         double_bits(4), double_bits(3), double_bits(2)},
                        {1, {8}});
  // Independent Fraction evaluation: PCHIP slopes are 5/2, 6/7, 0, -25/6.
  // At queries 3.5, .5 and 2 the exact values are 97/48, 135/112, 19/7.
  // The second function is 1-2*y, evaluated before destination rounding.
  const uint64_t expected64[2][2][10] = {
      {{0x3ff8000000000000, 0xc000000000000000, 0x4000000000000000,
        0x3ff0000000000000, 0xc008000000000000, 0x4004000000000000, 0, 0,
        0x4008000000000000, 0x4004000000000000},
       {0xc000000000000000, 0x4014000000000000, 0xc008000000000000,
        0xbff0000000000000, 0x401c000000000000, 0xc010000000000000,
        0x3ff0000000000000, 0x3ff0000000000000, 0xc014000000000000,
        0xc010000000000000}},
      {{0x40002aaaaaaaaaab, 0xc004000000000000, 0x4000000000000000,
        0x3ff3492492492492, 0xc010aaaaaaaaaaab, 0x4005b6db6db6db6e, 0, 0,
        0x4008000000000000, 0x4005b6db6db6db6e},
       {0xc008555555555555, 0x4018000000000000, 0xc008000000000000,
        0xbff6924924924925, 0x4022aaaaaaaaaaab, 0xc011b6db6db6db6e,
        0x3ff0000000000000, 0x3ff0000000000000, 0xc014000000000000,
        0xc011b6db6db6db6e}}};
  const uint64_t expected32[2][2][10] = {
      {{0x3fc00000, 0xc0000000, 0x40000000, 0x3f800000, 0xc0400000, 0x40200000,
        0, 0, 0x40400000, 0x40200000},
       {0xc0000000, 0x40a00000, 0xc0400000, 0xbf800000, 0x40e00000, 0xc0800000,
        0x3f800000, 0x3f800000, 0xc0a00000, 0xc0800000}},
      {{0x40015555, 0xc0200000, 0x40000000, 0x3f9a4925, 0xc0855555, 0x402db6db,
        0, 0, 0x40400000, 0x402db6db},
       {0xc042aaab, 0x40c00000, 0xc0400000, 0xbfb49249, 0x41155555, 0xc08db6db,
        0x3f800000, 0x3f800000, 0xc0a00000, 0xc08db6db}}};
  ResultRef retained;
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
        CpuNumericProfile::X86Avx2}) {
    for (bool cubic : {false, true}) {
      for (bool multi : {false, true}) {
        for (auto dtype : {ElementType::Float32, ElementType::Float64}) {
          const auto helper =
              cubic ? (multi ? numeric::interpolate_pchip_multi_node
                             : numeric::interpolate_pchip_node)
                    : (multi ? numeric::interpolate_linear_multi_node
                             : numeric::interpolate_linear_node);
          auto node = take(
              helper(7, WorkflowInputReference{11}, WorkflowInputReference{12},
                     WorkflowInputReference{13}, dtype,
                     numeric::CurveDomain::LinearExtrapolate, profile));
          const std::vector<ResultRef> inputs{x, multi ? columns : y, query};
          std::vector<OperationMetadata> metadata;
          for (const auto& input : inputs) {
            OperationMetadata item;
            item.result_schema =
                std::make_shared<SchemaTemplate>(input.schema());
            metadata.push_back(std::move(item));
          }
          auto available = d.registry->resolve_traits(node.operation, metadata,
                                                      node.parameters);
          if (!available.ok()) {
            require(
                profile != CpuNumericProfile::Strict &&
                    available.status().code == ErrorCode::BackendUnavailable,
                "curve wrong platform preserves BackendUnavailable");
            continue;
          }
          const int original_round = std::fegetround();
          std::fesetround(FE_UPWARD);
          std::feclearexcept(FE_ALL_EXCEPT);
          std::feraiseexcept(FE_DIVBYZERO);
          auto answer = d.run(node.operation, inputs, {}, node.parameters);
          const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
          const auto round = std::fegetround();
          std::fesetround(original_round);
          std::feclearexcept(FE_ALL_EXCEPT);
          require(round == FE_UPWARD && flags == FE_DIVBYZERO,
                  "curve preserves caller fenv across Result execution");
          auto result = take(std::move(answer));
          retained = result.results.at("out");
          const auto shape =
              multi ? std::vector<uint64_t>{10, 2} : std::vector<uint64_t>{10};
          require(retained.schema().id == "photospider.tensor" &&
                      retained.schema().tensors[0].key == "samples" &&
                      retained.schema().tensors[0].sample_shape() == shape &&
                      retained.schema().tensors[0].batch_axes.empty() &&
                      retained.schema().tensors[0].facets.empty(),
                  "curve Result publishes ordinary sample axes and no facets");
          for (uint64_t i = 0; i < 10; ++i)
            for (uint64_t c = 0; c < (multi ? 2U : 1U); ++c)
              require(
                  read_bits(retained, multi ? std::vector<uint64_t>{i, c}
                                            : std::vector<uint64_t>{i}) ==
                      (dtype == ElementType::Float32 ? expected32[cubic][c][i]
                                                     : expected64[cubic][c][i]),
                  "curve matches independent complete-formula rational bits");
          require(retained.association().size() == 3,
                  "curve output associates all active sources");
          for (unsigned port = 0; port < 3; ++port) {
            auto changed = take(Footprint::all(
                inputs[port].schema().tensors[0].sample_shape()));
            require(take(result.dependencies.potential_dirty(
                             "input" + std::to_string(port), changed, 1))
                            .at("out") == take(Footprint::all(shape)),
                    "curve Whole relations retain complete input support");
          }
        }
      }
    }
  }
  d.context.reset();
  require(read_bits(retained, {0, 1}) == expected64[1][1][0],
          "curve output remains readable after context retirement");
}
void curve_boundaries() {
  Driver d;
  const auto params = [](std::string policy = "reject",
                         std::string dtype = "float64") {
    return std::map<std::string, ParameterValue>{
        {"dtype", std::move(dtype)},
        {"out_of_domain", std::move(policy)}};
  };
  auto x = d.source({ElementType::Float64, {3}},
                    {0, double_bits(1), double_bits(2)}, {1, {8}});
  auto y = d.source({ElementType::Float64, {3}},
                    {0x8000000000000000, double_bits(1), 0x7ff0000000000001},
                    {1, {8}});
  auto hit = d.source({ElementType::Float32, {1}}, {0}, {1, {0}});
  for (const auto* key :
       {"curve.interpolate_linear_strict", "curve.interpolate_pchip_strict"}) {
    auto selected =
        take(d.run(key, {x, y, hit}, {}, params())).results.at("out");
    require(
        read_bits(selected, {0}) == UINT64_C(0x8000000000000000),
        "curve exact knot retains negative zero and skips unused generic NaN");
    auto outside =
        d.source({ElementType::Float32, {1}}, {float_bits(-1)}, {1, {0}});
    auto clamped = take(d.run(key, {x, y, outside}, {}, params("clamp")))
                       .results.at("out");
    require(read_bits(clamped, {0}) == UINT64_C(0x8000000000000000),
            "curve clamp retains selected zero without scanning generic y");
    auto query =
        d.source({ElementType::Float64, {2}}, {0, double_bits(3)}, {1, {8}});
    auto point = take(Footprint::from_regions({2}, {Region({{0, 1}})}));
    auto failed = d.run(key, {x, y, query}, point, params());
    require(
        !failed.ok() &&
            failed.status().reason == FailureReason::InvalidDomain &&
            failed.status().detail.scope == FailureScope::Run &&
            !failed.status().detail.atom &&
            failed.status().message.find("port=2 index=1") != std::string::npos,
        "curve checks all query controls before ordinate arithmetic outside Q");
    auto empty =
        take(d.run(key, {x, y, query}, take(Footprint::none({2})), params()));
    require(
        take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
        "Empty curve does not evaluate invalid numerical payload");
    auto duplicate = d.source({ElementType::Float64, {3}},
                              {0, double_bits(1), double_bits(1)}, {1, {8}});
    failed = d.run(key, {duplicate, y, hit}, {}, params());
    require(!failed.ok() && failed.status().message.find("strict increase") !=
                                std::string::npos,
            "curve validates knots beyond selected endpoint");
    auto middle =
        d.source({ElementType::Float32, {1}}, {float_bits(.5)}, {1, {0}});
    if (std::string(key).find("pchip") != std::string::npos) {
      failed = d.run(key, {x, y, middle}, {}, params());
      require(!failed.ok() && failed.status().message.find("port=1 index=2") !=
                                  std::string::npos,
              "PCHIP retains full local stencil finite checks");
    }
    auto extremes =
        d.source({ElementType::Float64, {2}},
                 {0xffefffffffffffff, 0x7fefffffffffffff}, {1, {8}});
    auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
    require(read_bits(take(d.run(key, {extremes, extremes, zero}, {}, params()))
                          .results.at("out"),
                      {0}) == 0,
            "curve exact formula tolerates unrepresentable intermediate span");
    auto finite_max =
        d.source({ElementType::Float64, {2}}, {0x7fefffffffffffff}, {1, {0}});
    failed = d.run(key, {extremes, finite_max, zero}, {},
                   params("reject", "float32"));
    require(!failed.ok() &&
                failed.status().reason == FailureReason::ArithmeticOverflow,
            "curve final destination overflow is Run failure");
  }
  auto typed =
      d.source({ElementType::Float32, {3, 2}}, {0, 0, 0, 0, 0, float_bits(2)},
               {1, {8, 4}}, {take(encode_semantic(coverage_semantics()))});
  auto cell = take(Footprint::from_regions({1, 2}, {Region({{0, 1}, {0, 1}})}));
  auto failed = d.run("curve.interpolate_linear_multi_strict", {x, typed, hit},
                      cell, params());
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 12,
          "curve validates complete typed ordinate even beyond all stencils");
  auto bad_column = d.source({ElementType::Float64, {3, 2}},
                             {0, 0x7ff0000000000000, 0, 0, 0, 0}, {1, {16, 8}});
  failed = d.run("curve.interpolate_pchip_multi_strict", {x, bad_column, hit},
                 cell, params());
  require(
      !failed.ok() && failed.status().reason == FailureReason::InvalidDomain,
      "curve evaluates unrequested output columns");
  Driver limited(30000);
  auto lx = limited.source({ElementType::Float64, {2}}, {0, double_bits(1)},
                           {1, {8}});
  auto ly = limited.source({ElementType::Float64, {2}}, {0, double_bits(2)},
                           {1, {8}});
  auto lq =
      limited.source({ElementType::Float64, {65}}, {double_bits(.5)}, {1, {0}});
  const auto before = limited.root.statistics().live;
  failed =
      limited.run("curve.interpolate_pchip_strict", {lx, ly, lq}, {}, params());
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  before[ResourceKind::Payload],
          "curve WorkLimit releases unpublished output and exact workspace");
  limited.context.reset();
  require(limited.root.statistics().live.values == before.values,
          "curve context retirement releases retained failure metadata");
  CancellationSource stop;
  stop.cancel();
  failed = d.run("curve.interpolate_pchip_strict", {x, y, hit}, {}, params(),
                 stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "curve pre-cancel preserves cancellation");
}

void curve_metadata_and_identity() {
  Driver d;
  const std::map<std::string, ParameterValue> params{
      {"dtype", std::string("float64")},
      {"out_of_domain", std::string("reject")}};
  auto x = d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
  auto y =
      d.source({ElementType::Float32, {2, 1}}, {0, float_bits(2)}, {1, {4, 0}});
  auto q = d.source({ElementType::Float64, {2}},
                    {double_bits(.25), double_bits(.75)}, {1, {8}});
  auto result = take(
      d.run("curve.interpolate_linear_multi_strict", {x, y, q}, {}, params));
  require(result.results.at("out").schema().tensors[0].sample_shape() ==
                  std::vector<uint64_t>({2, 1}) &&
              read_bits(result.results.at("out"), {1, 0}) == double_bits(1.5),
          "C=1 curve retains its second output axis");
  auto point =
      take(Footprint::from_regions({2, 1}, {Region({{1, 1}, {0, 1}})}));
  auto partial = take(
      d.run("curve.interpolate_linear_multi_strict", {x, y, q}, point, params));
  require(take(partial.results.at("out").descriptor())
                  .tensor_coverage(0)
                  .contains({1, 0}) &&
              read_bits(partial.results.at("out"), {1, 0}) == double_bits(1.5),
          "curve projects complete computation without rebasing global "
          "coordinates");
  std::vector<OperationMetadata> inputs(3);
  for (unsigned i = 0; i < 3; ++i)
    inputs[i].result_schema = std::make_shared<SchemaTemplate>(
        std::vector<ResultRef>{x, y, q}[i].schema());
  auto invalid = inputs;
  invalid[0].result_schema.reset();
  invalid[0].descriptor = {ElementType::Float64, {2}};
  require(!d.registry
               ->resolve_traits("curve.interpolate_linear_multi_strict",
                                invalid, params)
               .ok(),
          "curve rejects legacy Value metadata before specialization");
  invalid = inputs;
  auto extra = std::make_shared<SchemaTemplate>(*inputs[0].result_schema);
  auto member = extra->tensors[0];
  member.key = "extra";
  extra->tensors.push_back(member);
  invalid[0].result_schema = extra;
  require(!d.registry
               ->resolve_traits("curve.interpolate_linear_multi_strict",
                                invalid, params)
               .ok(),
          "curve requires an unambiguous sole tensor");
  invalid = inputs;
  auto huge = std::make_shared<SchemaTemplate>(*inputs[1].result_schema);
  huge->tensors[0].descriptor.shape = {2, UINT64_C(1) << 40};
  invalid[1].result_schema = huge;
  auto refused = d.registry->resolve_traits(
      "curve.interpolate_pchip_multi_strict", invalid, params);
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "curve rejects products above 2^40 using logical metadata");

  auto source = d.source({ElementType::Int16, {2}}, {1, 0xffff, 0x8000, 7},
                         {7, {-4, -2}}, {}, {2}, nullptr, "test.identity", 2);
  auto original = take(source.acquire_tensor(take(source.descriptor()), 0,
                                             Region::whole({2, 2})));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto identity_run =
      take(d.run("core.identity", {source}, {}, {}, {}, "value"));
  auto identity = identity_run.results.at("out");
  auto changed =
      take(Footprint::from_regions({2, 2}, {Region({{1, 1}, {0, 1}})}));
  require(take(identity_run.dependencies.potential_dirty("input0", changed, 4))
                  .at("out") == changed,
          "identity retains exact mapped Validation dependencies");
  auto window = take(identity.acquire_tensor(take(identity.descriptor()), 0,
                                             Region::whole({2, 2})));
  require(identity.schema().id == source.schema().id &&
              identity.schema().version == 2 &&
              identity.schema().tensors[0].key == "arbitrary-member" &&
              identity.schema().tensors[0].batch_axes ==
                  source.schema().tensors[0].batch_axes &&
              window.storage_owner_token() == original.storage_owner_token() &&
              d.root.statistics().live[ResourceKind::Payload] == payload &&
              read_bits(identity, {0, 0}) == 7 &&
              read_bits(identity, {1, 0}) == 0xffff,
          "identity preserves arbitrary schema version, Int16, batch axes and "
          "source view owner");

  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(source.schema());
  auto resolved =
      take(d.registry->resolve_traits("core.identity", {metadata}, {}));
  require(
      resolved.outputs[0].output_schema.result_schema_id == "test.identity" &&
          resolved.outputs[0].output_schema.result_schema_version == 2,
      "generic output predicate resolves to concrete schema identity");
  auto typed =
      d.source({ElementType::Float32, {1, 2}}, {0, float_bits(2)}, {1, {8, 4}},
               {take(encode_semantic(coverage_semantics()))});
  auto failed = d.run("core.identity", {typed}, {}, {}, {}, "value");
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 11,
          "generic identity keeps typed validation of captured source samples");
  auto empty = take(d.run("core.identity", {typed},
                          take(Footprint::none({1, 2})), {}, {}, "value"));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty identity skips invalid typed sample payload");

  const auto base_traits = take(d.registry->find_traits("core.identity"));
  auto metadata_registry = std::make_shared<OperationRegistry>();
  const auto definition = [&](std::string key, OperationTraits traits) {
    OperationDefinition operation;
    operation.key = std::move(key);
    operation.traits = std::move(traits);
    operation.specialize_metadata = [](const auto& supplied, const auto&) {
      OperationOutputSpecialization output;
      output.metadata.result_schema = supplied[0].result_schema;
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    };
    operation.start_result = [](const auto&, const auto&) {
      return Result<ResultContinuation>(
          Status{ErrorCode::OperationFailed, "metadata-only fixture"});
    };
    return operation;
  };
  auto fixed = base_traits;
  fixed.outputs[0].output_schema.result_schema_id = "photospider.tensor";
  fixed.outputs[0].output_schema.result_schema_version = 1;
  require(metadata_registry
              ->register_operation(definition("test.fixed.identity", fixed))
              .ok(),
          "fixed output fixture registers");
  refused =
      metadata_registry->resolve_traits("test.fixed.identity", {metadata}, {});
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "fixed Result output contract rejects changed schema id/version");
  auto other_version = metadata;
  auto renamed = std::make_shared<SchemaTemplate>(*metadata.result_schema);
  renamed->id = "photospider.tensor";
  other_version.result_schema = renamed;
  refused = metadata_registry->resolve_traits("test.fixed.identity",
                                              {other_version}, {});
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "fixed Result output also rejects changing only schema version");
  auto constrained = base_traits;
  constrained.outputs[0].output_schema.element_type_mask = 4;
  require(
      metadata_registry
          ->register_operation(definition("test.float.identity", constrained))
          .ok(),
      "generic output fixture registers with a dtype predicate");
  refused =
      metadata_registry->resolve_traits("test.float.identity", {metadata}, {});
  require(
      !refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
      "metadata-derived schema still obeys its registered tensor predicate");
  constrained = base_traits;
  constrained.requires_metadata_specialization = false;
  auto invalid_definition =
      definition("test.unspecialized.identity", constrained);
  invalid_definition.specialize_metadata = {};
  require(!metadata_registry->register_operation(std::move(invalid_definition))
               .ok(),
          "generic output identity requires static metadata specialization");
  d.context.reset();
  require(read_bits(identity, {1, 1}) == 1,
          "identity view retains source storage past context retirement");
}

void curve_composition() {
  Driver d;
  auto x = d.source({ElementType::Float32, {3}},
                    {0, float_bits(1), float_bits(3)}, {1, {4}});
  auto y = d.source({ElementType::Float64, {3}},
                    {double_bits(1), double_bits(3), double_bits(7)}, {1, {8}});
  auto start = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(3)}, {1, {0}});
  auto base = d.prepare("curve.interpolate_linear_strict", {x, y, start},
                        {{"dtype", std::string("float64")},
                         {"out_of_domain", std::string("reject")}});
  auto document = base.graph->snapshot().document();
  document.nodes.clear();
  document.outputs.clear();
  auto last = document.inputs.back();
  last.id = 14;
  last.name = "end";
  document.inputs.push_back(last);
  base.bindings.inputs.push_back({"end", end});
  auto table = take(numeric::bake_lut1d_pchip(
      document, WorkflowInputReference{11}, WorkflowInputReference{12},
      numeric::sequence_input(document.inputs[2]),
      numeric::sequence_input(document.inputs[3]), 7));
  auto exports = table.outputs();
  document.outputs.assign(exports.begin(), exports.end());
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(d.registry).compile(*graph));
  auto result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, base.bindings)),
      {{"values", take(Footprint::all({7}))},
       {"axis", take(Footprint::all({3}))}}));
  for (uint64_t i = 0; i < 7; ++i)
    require(read_bits(result.results.at("values"), {i}) == double_bits(i + 1),
            "linspace to PCHIP LUT baking uses only Result workflow edges");
  require(read_bits(result.results.at("axis"), {2}) == double_bits(.5),
          "LUT baking exports independent Result axis");
  document.nodes.clear();
  document.outputs.clear();
  auto resampled = take(numeric::resample_linear(
      document, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}));
  exports = resampled.outputs();
  document.outputs.assign(exports.begin(), exports.end());
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(d.registry).compile(*graph));
  result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, base.bindings)),
      {{"samples", take(Footprint::all({1}))},
       {"positions", take(Footprint::all({1}))}}));
  require(read_bits(result.results.at("samples"), {0}) == double_bits(1) &&
              read_bits(result.results.at("positions"), {0}) == 0,
          "resampling helper joins Result interpolation and Result identity");
}

void inverse_workflows() {
  Driver d;
  auto x = d.source({ElementType::Float64, {3}},
                    {double_bits(2), double_bits(1), 0}, {17, {-8}});
  const double targets[] = {.3125, 2.1875, .5, 2, 3, 1, 0, 4, .5};
  // Independent exact polynomial/lattice oracle. On [0,1], p=(3*x^2-x^3)/2;
  // on [1,2], p=1+3*u/2+2*u^2-u^3/2 where u=x-1. Compare at exact
  // destination midpoints to select ties-to-even, without forward rounding.
  const uint64_t roots64[] = {0x3fe0000000000000,
                              0x3ff8000000000000,
                              0x3fe4e2f2c0fa463b,
                              0x3ff703e132a025f2,
                              0x3ffbd4204c1af313,
                              0x3ff0000000000000,
                              0,
                              0x4000000000000000,
                              0x3fe4e2f2c0fa463b};
  const uint64_t roots32[] = {0x3f000000, 0x3fc00000, 0x3f271796,
                              0x3fb81f0a, 0x3fdea102, 0x3f800000,
                              0,          0x40000000, 0x3f271796};
  const uint64_t linear64[] = {0x3fd4000000000000,
                               0x3ff6555555555555,
                               0x3fe0000000000000,
                               0x3ff5555555555555,
                               0x3ffaaaaaaaaaaaab,
                               0x3ff0000000000000,
                               0,
                               0x4000000000000000,
                               0x3fe0000000000000};
  const uint64_t linear32[] = {0x3ea00000, 0x3fb2aaab, 0x3f000000,
                               0x3faaaaab, 0x3fd55555, 0x3f800000,
                               0,          0x40000000, 0x3f000000};
  ResultRef retained;
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
        CpuNumericProfile::X86Avx2}) {
    for (bool cubic : {false, true}) {
      for (bool decreasing : {false, true}) {
        auto y = d.source({ElementType::Float32, {3}},
                          {float_bits(decreasing ? -4 : 4),
                           float_bits(decreasing ? -1 : 1), 0},
                          {9, {-4}});
        std::vector<uint64_t> query_bits;
        for (double target : targets)
          query_bits.push_back(double_bits(decreasing ? -target : target));
        auto query =
            d.source({ElementType::Float64, {9}}, query_bits, {1, {8}});
        for (auto dtype : {ElementType::Float32, ElementType::Float64}) {
          const auto helper =
              cubic ? numeric::invert_pchip_node : numeric::invert_linear_node;
          auto node = take(helper(7, WorkflowInputReference{11},
                                  WorkflowInputReference{12},
                                  WorkflowInputReference{13}, dtype,
                                  numeric::CurveDomain::Reject, profile));
          std::vector<OperationMetadata> metadata;
          for (const auto& input : {x, y, query}) {
            OperationMetadata item;
            item.result_schema =
                std::make_shared<SchemaTemplate>(input.schema());
            metadata.push_back(std::move(item));
          }
          auto traits = d.registry->resolve_traits(node.operation, metadata,
                                                   node.parameters);
          if (!traits.ok()) {
            require(
                profile != CpuNumericProfile::Strict &&
                    traits.status().code == ErrorCode::BackendUnavailable,
                "inverse wrong-platform profile reports BackendUnavailable");
            continue;
          }
          std::fenv_t environment;
          require(std::fegetenv(&environment) == 0 &&
                      std::fesetround(FE_DOWNWARD) == 0,
                  "inverse fenv setup");
          std::feclearexcept(FE_ALL_EXCEPT);
          std::feraiseexcept(FE_DIVBYZERO);
          auto answer =
              d.run(node.operation, {x, y, query}, {}, node.parameters);
          const bool restored =
              std::fegetround() == FE_DOWNWARD &&
              std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
          std::fesetenv(&environment);
          require(restored,
                  "inverse preserves caller rounding and exception flags");
          auto result = take(std::move(answer));
          retained = result.results.at("out");
          for (uint64_t i = 0; i < 9; ++i) {
            const auto expected =
                cubic
                    ? (dtype == ElementType::Float32 ? roots32[i] : roots64[i])
                    : (dtype == ElementType::Float32 ? linear32[i]
                                                     : linear64[i]);
            require(read_bits(retained, {i}) == expected,
                    "inverse matches independent roots in both monotone "
                    "directions");
          }
          require(
              retained.schema().id == "photospider.tensor" &&
                  retained.schema().tensors[0].key == "samples" &&
                  retained.schema().tensors[0].sample_shape() ==
                      std::vector<uint64_t>{9} &&
                  retained.schema().tensors[0].facets.empty() &&
                  retained.association().size() == 3,
              "inverse publishes generic Result with three-source association");
          for (unsigned port = 0; port < 3; ++port) {
            auto changed = take(Footprint::from_regions(
                port == 2 ? std::vector<uint64_t>{9} : std::vector<uint64_t>{3},
                {Region({{1, 1}})}));
            for (uint32_t role : {1U, 4U})
              require(take(result.dependencies.potential_dirty(
                               "input" + std::to_string(port), changed, role))
                              .at("out") == take(Footprint::all({9})),
                      "inverse retains complete data and validation support");
          }
        }
      }
    }
  }
  d.context.reset();
  require(read_bits(retained, {2}) == roots64[2],
          "inverse Result outlives execution context");
}
void inverse_boundaries() {
  Driver d;
  const auto params = [](std::string policy = "reject",
                         std::string dtype = "float64") {
    return std::map<std::string, ParameterValue>{
        {"dtype", std::move(dtype)},
        {"out_of_domain", std::move(policy)}};
  };
  auto x =
      d.source({ElementType::Float64, {3}},
               {0x8000000000000000, double_bits(1), double_bits(2)}, {1, {8}});
  auto y = d.source({ElementType::Float64, {3}},
                    {0, double_bits(1), double_bits(4)}, {1, {8}});
  auto hit = d.source({ElementType::Float32, {1}}, {0}, {1, {0}});
  for (const auto* key :
       {"curve.invert_linear_strict", "curve.invert_pchip_strict"}) {
    require(
        read_bits(take(d.run(key, {x, y, hit}, {}, params())).results.at("out"),
                  {0}) == UINT64_C(0x8000000000000000),
        "inverse exact knot preserves x negative zero");
    auto descending = d.source({ElementType::Float64, {3}},
                               {double_bits(4), double_bits(1), 0}, {1, {8}});
    auto exterior = d.source({ElementType::Float32, {2}},
                             {float_bits(5), float_bits(-1)}, {1, {4}});
    auto clamped =
        take(d.run(key, {x, descending, exterior}, {}, params("clamp")))
            .results.at("out");
    require(read_bits(clamped, {0}) == UINT64_C(0x8000000000000000) &&
                read_bits(clamped, {1}) == double_bits(2),
            "descending inverse clamp selects corresponding x endpoints");
    auto plateau = d.source({ElementType::Float64, {3}},
                            {0, double_bits(1), double_bits(1)}, {1, {8}});
    auto failed = d.run(key, {x, plateau, hit}, {}, params());
    require(
        !failed.ok() &&
            failed.status().reason == FailureReason::InvalidDomain &&
            failed.status().message.find("port=1 index=2") != std::string::npos,
        "inverse globally rejects remote plateau even for knot request");
    auto nonfinite =
        d.source({ElementType::Float64, {3}},
                 {0, double_bits(1), 0x7ff0000000000001}, {1, {8}});
    failed = d.run(key, {x, nonfinite, hit}, {}, params());
    require(!failed.ok() && failed.status().message.find(
                                "nonfinite inverse input") != std::string::npos,
            "inverse globally validates all ordinates");
    auto query =
        d.source({ElementType::Float64, {2}}, {0, double_bits(5)}, {1, {8}});
    auto point = take(Footprint::from_regions({2}, {Region({{0, 1}})}));
    failed = d.run(key, {x, y, query}, point, params());
    require(
        !failed.ok() && failed.status().detail.scope == FailureScope::Run &&
            !failed.status().detail.atom &&
            failed.status().message.find("port=2 index=1") != std::string::npos,
        "inverse rejects unrequested query with global port/index diagnostics");
    auto empty = take(
        d.run(key, {x, plateau, query}, take(Footprint::none({2})), params()));
    require(
        take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
        "Empty inverse skips topology and query payload");
    auto span = d.source({ElementType::Float64, {2}},
                         {0xffefffffffffffff, 0x7fefffffffffffff}, {1, {8}});
    auto values = d.source({ElementType::Float64, {2}},
                           {double_bits(-1), double_bits(1)}, {1, {8}});
    require(read_bits(take(d.run(key, {span, values, hit}, {},
                                 params("reject", "float32")))
                          .results.at("out"),
                      {0}) == 0,
            "inverse finite root survives overflowing endpoint conversion");
    auto endpoint =
        d.source({ElementType::Float32, {2}}, {0, float_bits(1)}, {1, {4}});
    failed = d.run(key, {span, values, endpoint}, point,
                   params("reject", "float32"));
    require(!failed.ok() &&
                failed.status().reason == FailureReason::ArithmeticOverflow &&
                failed.status().detail.scope == FailureScope::Run,
            "inverse output overflow outside Q fails complete publication");
    auto half_min = d.source({ElementType::Float64, {2}}, {0, 1}, {1, {8}});
    auto unit =
        d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
    auto half =
        d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
    require(read_bits(take(d.run(key, {half_min, unit, half}, {}, params()))
                          .results.at("out"),
                      {0}) == 0,
            "inverse final subnormal midpoint rounds ties to even");
    CancellationSource stop;
    stop.cancel();
    failed = d.run(key, {x, y, hit}, {}, params(), stop.token());
    require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
            "inverse pre-cancel");
  }
  SemanticDescriptor signal;
  signal.kind = SemanticKind::SampledSignal;
  signal.channels = {{"value", "value", "dimensionless"}};
  signal.sample_step = .5;
  signal.sample_axis_unit = "seconds";
  auto typed =
      d.source({ElementType::Float32, {3}}, {0, float_bits(1), 0x7f800000},
               {1, {4}}, {take(encode_semantic(signal))});
  auto typed_failure =
      d.run("curve.invert_pchip_strict", {x, typed, hit}, {}, params());
  require(!typed_failure.ok() &&
              typed_failure.status().code == ErrorCode::InvalidArgument &&
              typed_failure.status().detail.input_id == 12 &&
              typed_failure.status().detail.origin == FailureOrigin::Domain,
          "inverse validates complete typed signal and preserves source input "
          "identity");
  auto typed_empty = take(d.run("curve.invert_pchip_strict", {x, typed, hit},
                                take(Footprint::none({1})), params()));
  require(take(typed_empty.results.at("out").descriptor())
              .tensor_coverage(0)
              .empty(),
          "Empty inverse skips typed signal sample validation");
  auto node = take(numeric::invert_linear_node(7, WorkflowInputReference{11},
                                               WorkflowInputReference{12},
                                               WorkflowInputReference{13}));
  std::vector<OperationMetadata> metadata(3);
  for (unsigned i = 0; i < 3; ++i)
    metadata[i].result_schema = std::make_shared<SchemaTemplate>(
        std::vector<ResultRef>{x, y, hit}[i].schema());
  auto invalid = metadata;
  invalid[0].result_schema.reset();
  invalid[0].descriptor = {ElementType::Float64, {3}};
  require(!d.registry->resolve_traits(node.operation, invalid, node.parameters)
               .ok(),
          "inverse rejects Value edge metadata");
  auto wrong_shape =
      std::make_shared<SchemaTemplate>(*metadata[0].result_schema);
  wrong_shape->tensors[0].batch_axes = {1};
  invalid = metadata;
  invalid[0].result_schema = wrong_shape;
  require(!d.registry->resolve_traits(node.operation, invalid, node.parameters)
               .ok(),
          "inverse checks complete sample rank including batch axes");
  node.parameters["out_of_domain"] = std::string("linear_extrapolate");
  require(!d.registry->resolve_traits(node.operation, metadata, node.parameters)
               .ok(),
          "inverse has no extrapolation mode");
  Driver limited(30000);
  auto lx = limited.source({ElementType::Float64, {3}},
                           {0, double_bits(1), double_bits(2)}, {1, {8}});
  auto ly = limited.source({ElementType::Float64, {3}},
                           {0, double_bits(1), double_bits(4)}, {1, {8}});
  auto lq =
      limited.source({ElementType::Float64, {2}}, {double_bits(.5)}, {1, {0}});
  const auto before = limited.root.statistics().live;
  auto failed =
      limited.run("curve.invert_pchip_strict", {lx, ly, lq}, {}, params());
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  before[ResourceKind::Payload],
          "inverse work exhaustion releases unpublished output and promoted "
          "arrays");
  limited.context.reset();
  require(limited.root.statistics().live.values == before.values,
          "inverse failure metadata retires with context");
}

struct CancelCurvePhase {
  ResultContinuation inner;
  std::shared_ptr<CancellationSource> stop;
  std::shared_ptr<bool> triggered;
  CancelCurvePhase(ResultContinuation program,
                   std::shared_ptr<CancellationSource> cancellation,
                   std::shared_ptr<bool> observed)
      : inner(std::move(program)),
        stop(std::move(cancellation)),
        triggered(std::move(observed)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      // ExactCurve allocates a fixed 352-limb slot after input validation.
      if (amount == 352) {
        *triggered = true;
        stop->cancel();
      }
      return phase.consume_work(amount);
    };
    return inner.poll(bounded);
  }
};
void inverse_active_cancel() {
  Driver d;
  const auto original = d.registry;
  auto stop = std::make_shared<CancellationSource>();
  auto triggered = std::make_shared<bool>(false);
  OperationDefinition definition;
  definition.key = "curve.invert_pchip_strict";
  definition.traits = take(original->find_traits(definition.key));
  definition.traits.outputs[0].continuation_bytes += sizeof(CancelCurvePhase);
  definition.specialize_metadata = [original](const auto& inputs,
                                              const auto& parameters) {
    auto traits = original->resolve_traits("curve.invert_pchip_strict", inputs,
                                           parameters);
    if (!traits.ok())
      return Result<std::vector<OperationOutputSpecialization>>(
          traits.status());
    OperationOutputSpecialization result;
    result.metadata.result_schema = std::make_shared<SchemaTemplate>(
        *traits.value().outputs[0].result_schema);
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  definition.start_result = [original, stop, triggered](const auto& query,
                                                        const auto& allocator) {
    auto forwarded = query;
    forwarded.prepared.reset();
    auto inner = original->start_result("curve.invert_pchip_strict", forwarded,
                                        allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<CancelCurvePhase>(
        allocator, inner.take_value(), stop, triggered);
  };
  d.registry = std::make_shared<OperationRegistry>();
  require(d.registry->register_operation(std::move(definition)).ok(),
          "inverse cancellation registration");
  require(d.registry->freeze().ok(), "inverse cancellation freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.context = std::make_unique<ExecutionContext>(d.registry, config);
  d.root = take(d.context->resource_budget());
  auto x = d.source({ElementType::Float64, {3}},
                    {0, double_bits(1), double_bits(2)}, {1, {8}});
  auto y = d.source({ElementType::Float64, {3}},
                    {0, double_bits(1), double_bits(4)}, {1, {8}});
  auto q = d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  const auto before = d.root.statistics().live;
  auto failed = d.run("curve.invert_pchip_strict", {x, y, q}, {},
                      {{"dtype", std::string("float64")},
                       {"out_of_domain", std::string("reject")}},
                      stop->token());
  require(*triggered && !failed.ok() &&
              failed.status().code == ErrorCode::Cancelled &&
              d.root.statistics().live[ResourceKind::Payload] ==
                  before[ResourceKind::Payload],
          "inverse cancellation during exact root work discards output and "
          "scratch");
  d.context.reset();
  require(
      d.root.statistics().live.values == before.values,
      "inverse cancelled continuation releases all context-owned resources");
}

void inverse_composition() {
  Driver d;
  auto x = d.source({ElementType::Float64, {3}},
                    {0, double_bits(1), double_bits(2)}, {1, {8}});
  auto y = d.source({ElementType::Float32, {3}},
                    {0, float_bits(1), float_bits(4)}, {1, {4}});
  auto query =
      d.source({ElementType::Float64, {3}},
               {double_bits(.5), double_bits(1), double_bits(1.5)}, {1, {8}});
  auto forward = take(numeric::interpolate_pchip_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}));
  auto prepared =
      d.prepare(forward.operation, {x, y, query}, forward.parameters);
  auto document = prepared.graph->snapshot().document();
  document.nodes.push_back(take(numeric::invert_pchip_node(
      8, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowNodeOutput{7, "values"})));
  document.outputs = {{"restored", 8, "values"}};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(d.registry).compile(*graph));
  auto restored =
      take(d.context->execute_fragments(
               take(d.context->freeze(compiled.plan, prepared.bindings)),
               {{"restored", take(Footprint::all({3}))}}))
          .results.at("restored");
  for (uint64_t i = 0; i < 3; ++i)
    require(
        read_bits(restored, {i}) == double_bits(.5 + i * .5),
        "forward PCHIP Result composes with inverse for exact dyadic samples");
}

bool math_profile_available(const Driver& driver, const WorkflowNode& node,
                            const std::vector<ResultRef>& inputs) {
  std::vector<OperationMetadata> metadata(inputs.size());
  for (unsigned i = 0; i < inputs.size(); ++i)
    metadata[i].result_schema =
        std::make_shared<SchemaTemplate>(inputs[i].schema());
  auto resolved = driver.registry->resolve_traits(node.operation, metadata,
                                                  node.parameters);
  if (resolved.ok())
    return true;
  require(resolved.status().code == ErrorCode::BackendUnavailable &&
              node.operation.find("_strict") == std::string::npos,
          "only an unavailable accelerated profile may skip a workflow");
  return false;
}
void shaper_workflows() {
  Driver d;
  ResultRef retained;
  for (const auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon}) {
    for (const bool narrow : {false, true}) {
      const auto dtype = narrow ? ElementType::Float32 : ElementType::Float64;
      const auto width = narrow ? 4U : 8U;
      const auto bits = [&](double v) {
        return narrow ? float_bits(v) : double_bits(v);
      };
      auto lower = d.source({dtype, {1}}, {bits(1)}, {1, {-733}});
      auto upper = d.source({dtype, {1}}, {bits(16)}, {1, {0}});
      auto input = d.source(
          {dtype, {3}},
          {bits(32), bits(16), bits(4), bits(2), bits(1), bits(.5)},
          {1 + 5 * width,
           {-static_cast<int64_t>(3 * width), -static_cast<int64_t>(width)}},
          {}, {2});
      auto node = take(numeric::log2_shaper_node(
          7, WorkflowInputReference{11}, WorkflowInputReference{12},
          WorkflowInputReference{13}, profile));
      if (!math_profile_available(d, node, {input, lower, upper}))
        continue;
      auto prepared = d.prepare(node.operation, {input, lower, upper});
      auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
      auto whole = d.context->execute_fragments(
          frozen, {{"out", take(Footprint::all({2, 3}))}});
      if (!whole.ok() && whole.status().code == ErrorCode::BackendUnavailable)
        continue;
      auto result = take(std::move(whole));
      retained = result.results.at("out");
      require(
          retained.schema().id == "photospider.tensor" &&
              retained.schema().tensors[0].key == "samples" &&
              retained.schema().tensors[0].facets.empty() &&
              retained.schema().tensors[0].sample_shape() ==
                  std::vector<uint64_t>({2, 3}) &&
              retained.association() ==
                  ResourceVector<uint64_t>{input.object_id(), lower.object_id(),
                                           upper.object_id()},
          "log shaper preserves complete sample shape and owning sources");
      const std::array<double, 6> expected{-.25, 0, .25, .5, 1, 1.25};
      for (unsigned i = 0; i < expected.size(); ++i)
        require(read_bits(retained, {i / 3, i % 3}) == bits(expected[i]),
                "log shaper signed unaligned batched exact landmarks");
      auto inverse = take(numeric::log2_shaper_inverse_node(
          7, WorkflowInputReference{11}, WorkflowInputReference{12},
          WorkflowInputReference{13}, profile));
      auto back = take(d.run(inverse.operation, {retained, lower, upper}))
                      .results.at("out");
      const std::array<double, 6> original{.5, 1, 2, 4, 16, 32};
      for (unsigned i = 0; i < original.size(); ++i)
        require(read_bits(back, {i / 3, i % 3}) == bits(original[i]),
                "log inverse exact exponent landmarks");
      for (unsigned port = 0; port < 3; ++port) {
        const auto shape =
            port ? std::vector<uint64_t>{1} : std::vector<uint64_t>{2, 3};
        auto changed = take(Footprint::all(shape));
        for (const uint32_t role : {1U, 4U})
          require(take(result.dependencies.potential_dirty(
                           "input" + std::to_string(port), changed, role))
                          .at("out") == take(Footprint::all({2, 3})),
                  "log shaper complete data and validation dirty support");
      }
      auto roi =
          take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {1, 1}})}));
      auto partial = take(d.context->execute_fragments(frozen, {{"out", roi}}));
      require(
          read_bits(partial.results.at("out"), {1, 1}) == bits(1) &&
              take(partial.results.at("out").descriptor()).tensor_coverage(0) ==
                  take(Footprint::all({2, 3})),
          "log shaper partial delivery retains global coordinates");
    }
  }
  d.context.reset();
  require(read_bits(retained, {1, 2}) == float_bits(1.25),
          "shaper output survives context retirement");
}
void shaper_boundaries() {
  Driver d;
  SemanticDescriptor signal;
  signal.kind = SemanticKind::SampledSignal;
  signal.channels = {{"value", "value", "dimensionless"}};
  signal.sample_step = .5;
  signal.sample_axis_unit = "seconds";
  for (bool narrow : {false, true}) {
    const auto dtype = narrow ? ElementType::Float32 : ElementType::Float64;
    const auto width = narrow ? 4U : 8U;
    const auto bits = [&](double v) {
      return narrow ? float_bits(v) : double_bits(v);
    };
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    const auto nan = sign | infinity | 0x42;
    auto lower = d.source({dtype, {1}}, {bits(1)}, {1, {0}});
    auto upper = d.source({dtype, {1}}, {bits(16)}, {1, {0}});
    auto input = d.source(
        {dtype, {6}}, {0, sign, sign | bits(1), infinity, sign | infinity, nan},
        {1, {width}});
    for (bool inverse : {false, true}) {
      const std::string key = inverse ? "curve.log2_shaper_inverse_strict"
                                      : "curve.log2_shaper_strict";
      auto output = take(d.run(key, {input, lower, upper})).results.at("out");
      const std::array<uint64_t, 6> expected{
          inverse ? bits(1) : sign | infinity,
          inverse ? bits(1) : sign | infinity,
          inverse ? bits(1. / 16) : infinity | quiet,
          infinity,
          inverse ? 0 : infinity | quiet,
          nan | quiet};
      for (unsigned i = 0; i < expected.size(); ++i)
        require(read_bits(output, {i}) == expected[i],
                "shaper preserves IEEE special classifications and NaN bits");
      for (auto raw :
           {uint64_t{0}, sign, bits(-1), bits(16), infinity, infinity | 1}) {
        auto bad = d.source({dtype, {1}}, {raw}, {1, {0}});
        auto failed = d.run(key, {input, bad, upper});
        require(!failed.ok() &&
                    failed.status().code == ErrorCode::InvalidArgument &&
                    failed.status().reason == FailureReason::InvalidDomain &&
                    failed.status().detail.scope == FailureScope::Run &&
                    failed.status().message.find("port=1 bits=") !=
                        std::string::npos,
                "invalid log lower outranks special input and retains port");
        auto empty =
            take(d.run(key, {input, bad, upper}, take(Footprint::none({6}))));
        require(take(empty.results.at("out").descriptor())
                    .tensor_coverage(0)
                    .empty(),
                "Empty log shaper avoids all bound and sample payload");
      }
      auto typed = d.source({dtype, {3}}, {bits(.25), bits(.5), infinity},
                            {1, {width}}, {take(encode_semantic(signal))});
      auto point = take(Footprint::from_regions({3}, {Region({{0, 1}})}));
      auto failed = d.run(key, {typed, lower, upper}, point);
      require(!failed.ok() && failed.status().detail.input_id == 11 &&
                  failed.status().detail.origin == FailureOrigin::Domain,
              "log shaper validates typed remote samples before evaluation");
      require(
          d.run(key, {typed, lower, upper}, take(Footprint::none({3}))).ok(),
          "Empty shaper skips typed sample validation");
    }
  }
  auto value =
      d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto bounds =
      d.source({ElementType::Float64, {1}}, {double_bits(16)}, {1, {0}});
  std::vector<OperationMetadata> metadata(3);
  metadata[0].result_schema = metadata[1].result_schema =
      std::make_shared<SchemaTemplate>(value.schema());
  metadata[2].result_schema = std::make_shared<SchemaTemplate>(bounds.schema());
  auto batch = std::make_shared<SchemaTemplate>(value.schema());
  batch->tensors[0].batch_axes = {2};
  auto wrong = metadata;
  wrong[1].result_schema = batch;
  require(
      !d.registry->resolve_traits("curve.log2_shaper_strict", wrong, {}).ok(),
      "shaper checks complete scalar bound sample shape");
  wrong = metadata;
  wrong[0].result_schema.reset();
  wrong[0].descriptor = {ElementType::Float64, {1}};
  require(
      !d.registry->resolve_traits("curve.log2_shaper_strict", wrong, {}).ok(),
      "shaper ports require Result metadata");
  Driver limited(30000);
  auto x =
      limited.source({ElementType::Float64, {1}}, {double_bits(3)}, {1, {0}});
  auto l =
      limited.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto u =
      limited.source({ElementType::Float64, {1}}, {double_bits(16)}, {1, {0}});
  const auto before = limited.root.statistics().live;
  auto failed = limited.run("curve.log2_shaper_strict", {x, l, u});
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  before[ResourceKind::Payload],
          "shaper refinement work exhaustion rolls back unpublished output");
  limited.context.reset();
  require(limited.root.statistics().live.values == before.values,
          "shaper failed continuation releases context resources");
}
WorkflowNode ramp_node(ColorModel model, ColorHueUnit unit,
                       CpuNumericProfile profile = CpuNumericProfile::Strict,
                       bool reject = false) {
  const WorkflowInput q = WorkflowInputReference{11},
                      k = WorkflowInputReference{12},
                      c = WorkflowInputReference{13};
  numeric::ColorRampOptions options;
  options.dtype = ElementType::Float64;
  options.profile = profile;
  options.out_of_domain =
      reject ? numeric::CurveDomain::Reject : numeric::CurveDomain::Clamp;
  auto color = numeric::color_ramp_detail::description(model, unit);
  if (model == ColorModel::Rgb) {
    color = numeric::color_ramp_rgb_description();
    color.transfer = ColorTransfer{ColorTransferKind::Linear, {}};
    numeric::RgbRampOptions rgb;
    static_cast<numeric::ColorRampOptions&>(rgb) = options;
    return take(numeric::color_ramp_rgb_node(7, q, k, c, ElementType::Float64,
                                             color, rgb));
  }
  if (model == ColorModel::Ycbcr) {
    color = numeric::color_ramp_rgb_description();
    color.model = model;
    color.ncl_coefficients =
        take(color_ncl_coefficients(ColorNclPreset::Bt709));
  }
  if (model == ColorModel::Cielch || model == ColorModel::Oklch ||
      model == ColorModel::Hsl) {
    numeric::HueRampOptions hue;
    static_cast<numeric::ColorRampOptions&>(hue) = options;
    hue.output_hue_unit = ColorHueUnit::PiMultiple;
    if (unit == ColorHueUnit::RationalPi) {
      const auto helper = model == ColorModel::Cielch
                              ? numeric::color_ramp_cielch_rational_pi_node
                          : model == ColorModel::Oklch
                              ? numeric::color_ramp_oklch_rational_pi_node
                              : numeric::color_ramp_hsl_rational_pi_node;
      return take(helper(7, q, k, c, WorkflowInputReference{14},
                         WorkflowInputReference{15}, ElementType::Float64,
                         color, hue));
    }
    const auto helper =
        model == ColorModel::Cielch  ? (unit == ColorHueUnit::PiMultiple
                                            ? numeric::color_ramp_cielch_pi_node
                                            : numeric::color_ramp_cielch_node)
        : model == ColorModel::Oklch ? (unit == ColorHueUnit::PiMultiple
                                            ? numeric::color_ramp_oklch_pi_node
                                            : numeric::color_ramp_oklch_node)
                                     : (unit == ColorHueUnit::PiMultiple
                                            ? numeric::color_ramp_hsl_pi_node
                                            : numeric::color_ramp_hsl_node);
    return take(helper(7, q, k, c, ElementType::Float64, color, hue));
  }
  const auto helper =
      model == ColorModel::Xyz      ? numeric::color_ramp_xyz_node
      : model == ColorModel::Cielab ? numeric::color_ramp_cielab_node
      : model == ColorModel::Oklab  ? numeric::color_ramp_oklab_node
                                    : numeric::color_ramp_ycbcr_node;
  return take(helper(7, q, k, c, ElementType::Float64, color, options));
}
void color_ramp_workflows() {
  Driver d;
  ResultRef retained;
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon}) {
    for (auto model : {ColorModel::Rgb, ColorModel::Xyz, ColorModel::Cielab,
                       ColorModel::Oklab, ColorModel::Ycbcr, ColorModel::Cielch,
                       ColorModel::Oklch, ColorModel::Hsl}) {
      const bool polar = model == ColorModel::Cielch ||
                         model == ColorModel::Oklch || model == ColorModel::Hsl;
      for (auto unit : {ColorHueUnit::Radian, ColorHueUnit::PiMultiple,
                        ColorHueUnit::RationalPi}) {
        if (!polar && unit != ColorHueUnit::Radian)
          continue;
        auto node = ramp_node(model, unit, profile);
        const bool split = polar && unit == ColorHueUnit::RationalPi;
        const unsigned channels = split ? 2 : 3;
        auto query =
            d.source({ElementType::Float32, {2}},
                     {float_bits(2), float_bits(.5), 0, float_bits(-1)},
                     {13, {-8, -4}}, {}, {2});
        auto stops = d.source({ElementType::Float64, {2}}, {double_bits(1), 0},
                              {9, {-8}});
        std::vector<uint64_t> values;
        const std::array<double, 3> low =
            model == ColorModel::Hsl ? std::array<double, 3>{0, .25, .5}
                                     : std::array<double, 3>{.25, .25, 0};
        const std::array<double, 3> high =
            model == ColorModel::Hsl ? std::array<double, 3>{4, .75, 1}
                                     : std::array<double, 3>{.75, .75, 4};
        for (const auto& row : {low, high})
          for (unsigned channel = 0; channel < 3; ++channel) {
            if (split && channel == (model == ColorModel::Hsl ? 0U : 2U))
              continue;
            // Radian fixtures use zero hue; pi/rational fixtures retain 4
            // turns.
            const auto value =
                polar && unit == ColorHueUnit::Radian &&
                        channel == (model == ColorModel::Hsl ? 0U : 2U)
                    ? 0.
                    : row[channel];
            values.push_back(double_bits(value));
          }
        std::reverse(values.begin(), values.end());
        auto colors = d.source({ElementType::Float64, {2, channels}}, values,
                               {1 + (2 * channels - 1) * 8,
                                {-static_cast<int64_t>(channels * 8), -8}});
        std::vector<ResultRef> inputs{query, stops, colors};
        if (split) {
          inputs.push_back(
              d.source({ElementType::Int64, {2}}, {0, 8}, {1, {8}}));
          inputs.push_back(d.source({ElementType::Int64, {2}}, {2}, {1, {0}}));
        }
        if (!math_profile_available(d, node, inputs))
          continue;
        auto prepared = d.prepare(node.operation, inputs, node.parameters);
        auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
        auto roi = take(Footprint::from_regions(
            {2, 2, 3}, {Region({{1, 1}, {0, 1}, {1, 1}})}));
        auto executed = d.context->execute_fragments(frozen, {{"out", roi}});
        if (!executed.ok() &&
            executed.status().code == ErrorCode::BackendUnavailable)
          continue;
        auto result = take(std::move(executed));
        retained = result.results.at("out");
        const auto& tensor = retained.schema().tensors[0];
        auto description = take(decode_color_array(tensor.facets[0]));
        require(retained.schema().id == "photospider.tensor" &&
                    tensor.key == "samples" &&
                    tensor.atomic_trailing_axes == 1 &&
                    tensor.sample_shape() == std::vector<uint64_t>({2, 2, 3}) &&
                    description.model == model &&
                    (!polar || description.hue == ColorHueUnit::PiMultiple) &&
                    take(retained.descriptor()).tensor_coverage(0) ==
                        take(Footprint::all({2, 2, 3})),
                "ramp publishes full color Result with batched global shape");
        for (unsigned i = 0; i < 4; ++i) {
          for (unsigned c = 0; c < 3; ++c) {
            double expected = i < 2    ? low[c]
                              : i == 3 ? high[c]
                                       : (low[c] + high[c]) * .5;
            if (polar && unit == ColorHueUnit::Radian &&
                c == (model == ColorModel::Hsl ? 0U : 2U))
              expected = 0;
            require(
                read_bits(retained, {i / 2, i % 2, c}) == double_bits(expected),
                "ramp exact interpolation/clamp uses original coordinates");
          }
        }
        for (unsigned port = 0; port < inputs.size(); ++port) {
          auto shape = inputs[port].schema().tensors[0].sample_shape();
          for (uint32_t role : {1U, 4U})
            require(take(result.dependencies.potential_dirty(
                             "input" + std::to_string(port),
                             take(Footprint::all(shape)), role))
                            .at("out") == take(tensor.close_samples(roi)),
                    "ramp retains complete data and validation dirty support");
        }
      }
    }
  }
  d.context.reset();
  require(read_bits(retained, {1, 0, 0}) == double_bits(2),
          "ramp Result outlives execution context");
}
void color_ramp_boundaries() {
  Driver d;
  auto node = ramp_node(ColorModel::Xyz, ColorHueUnit::Radian);
  auto query = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto midpoint =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  auto stops =
      d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
  const std::vector<uint64_t> bad_rows{UINT64_C(0x8000000000000000),
                                       double_bits(1),
                                       double_bits(2),
                                       UINT64_C(0x7ff8000000000042),
                                       0,
                                       0};
  auto colors =
      d.source({ElementType::Float64, {2, 3}}, bad_rows, {1, {24, 8}});
  auto direct =
      take(d.run(node.operation, {query, stops, colors}, {}, node.parameters))
          .results.at("out");
  require(read_bits(direct, {0, 0}) == UINT64_C(0x8000000000000000),
          "ramp exact hit preserves -0 and skips unused generic NaN");
  auto typed = d.source({ElementType::Float64, {2, 3}}, bad_rows, {1, {24, 8}},
                        {take(encode_color_array(ColorArrayDescriptor{}))});
  auto failed =
      d.run(node.operation, {query, stops, typed}, {}, node.parameters);
  require(
      !failed.ok() && failed.status().detail.input_id == 13 &&
          failed.status().detail.origin == FailureOrigin::Domain,
      "ramp typed validation observes remote NaN before stencil arithmetic");
  auto empty = take(d.run(node.operation, {query, stops, typed},
                          take(Footprint::none({1, 3})), node.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty ramp avoids typed payload validation");
  const uint64_t nz = UINT64_C(0x8000000000000000);
  auto mixed = d.source(
      {ElementType::Float64, {2, 3}},
      {nz, double_bits(1), double_bits(2), nz, double_bits(3), double_bits(4)},
      {1, {24, 8}});
  auto blended =
      take(d.run(node.operation, {midpoint, stops, mixed}, {}, node.parameters))
          .results.at("out");
  require(read_bits(blended, {0, 0}) == 0 &&
              read_bits(blended, {0, 1}) == double_bits(2),
          "ramp uses whole-row identity and mixed exact zero is +0");
  auto identical = d.source({ElementType::Float64, {2, 3}},
                            {nz, double_bits(1), double_bits(2)}, {1, {0, 8}});
  auto kept = take(d.run(node.operation, {midpoint, stops, identical}, {},
                         node.parameters))
                  .results.at("out");
  require(read_bits(kept, {0, 0}) == nz,
          "ramp complete identical rows preserve direct negative zero");
  auto bad_stops =
      d.source({ElementType::Float64, {2}}, {double_bits(1), 0}, {1, {8}});
  auto nan_query = d.source({ElementType::Float64, {1}},
                            {UINT64_C(0x7ff8000000000042)}, {1, {0}});
  failed = d.run(node.operation, {nan_query, bad_stops, colors}, {},
                 node.parameters);
  require(!failed.ok() && failed.status().detail.scope == FailureScope::Run &&
              failed.status().message.find("port=1 row=1") != std::string::npos,
          "ramp validates all stops before query finiteness");
  auto rejecting = ramp_node(ColorModel::Xyz, ColorHueUnit::Radian,
                             CpuNumericProfile::Strict, true);
  auto queries =
      d.source({ElementType::Float64, {2}}, {0, double_bits(2)}, {1, {8}});
  auto point =
      take(Footprint::from_regions({2, 3}, {Region({{0, 1}, {0, 1}})}));
  failed = d.run(rejecting.operation, {queries, stops, colors}, point,
                 rejecting.parameters);
  require(!failed.ok() &&
              failed.status().message.find("port=0 row=1") != std::string::npos,
          "Whole ramp rejects an unrequested query before selected colors");
  for (auto model : {ColorModel::Cielch, ColorModel::Hsl}) {
    auto rational = ramp_node(model, ColorHueUnit::RationalPi);
    auto pairs = d.source(
        {ElementType::Float64, {2, 2}},
        {double_bits(.25), double_bits(.5), double_bits(.75), double_bits(-1)},
        {1, {16, 8}});
    auto p = d.source({ElementType::Int64, {2}},
                      {UINT64_C(0x8000000000000000), INT64_MAX}, {1, {8}});
    auto q = d.source({ElementType::Int64, {2}}, {1, 0}, {1, {8}});
    auto result = take(d.run(rational.operation, {query, stops, pairs, p, q},
                             {}, rational.parameters))
                      .results.at("out");
    require(read_bits(result, {0, model == ColorModel::Hsl ? 0U : 2U}) ==
                double_bits(-0x1p63),
            "rational ramp retains INT64_MIN bits and ignores unused q<=0");
    failed = d.run(rational.operation, {midpoint, stops, pairs, p, q}, {},
                   rational.parameters);
    require(!failed.ok() && failed.status().detail.scope == FailureScope::Run,
            "rational ramp validates selected chroma/denominator");
  }
}
void color_ramp_icc() {
  Driver d;
  ResultRef retained, empty, generic;
  ColorProfileIdentity identity;
  {
    auto bytes = numeric_fixture::fixture();
    auto profile =
        take(IccProfile::import({bytes.data(), bytes.size()}, d.root));
    identity = profile.identity();
    auto resources = take(ResourceBindings::create({profile}, d.root));
    auto query =
        d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
    auto stops =
        d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
    auto colors = d.source(
        {ElementType::Float64, {2, 4}},
        {0, 0, 0, 0, double_bits(1), double_bits(.5), 0, double_bits(.25)},
        {1, {32, 8}});
    auto node = take(numeric::color_ramp_cmyk_node(
        7, WorkflowInputReference{11}, WorkflowInputReference{12},
        WorkflowInputReference{13}, ElementType::Float64,
        numeric::color_ramp_cmyk_description(identity)));
    auto prepared = d.prepare(node.operation, {query, stops, colors},
                              node.parameters, "values", resources);
    auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
    retained = take(d.context->execute_fragments(
                        frozen, {{"out", take(Footprint::all({1, 4}))}}))
                   .results.at("out");
    empty = take(d.context->execute_fragments(
                     frozen, {{"out", take(Footprint::none({1, 4}))}}))
                .results.at("out");
    auto numeric =
        d.prepare("numeric.clamp_strict", {retained, retained, retained}, {},
                  "values", resources);
    generic = take(d.context->execute_fragments(
                       take(d.context->freeze(numeric.plan, numeric.bindings)),
                       {{"out", take(Footprint::all({1, 4}))}}))
                  .results.at("out");
  }
  d.context.reset();
  require(empty.resources().icc_profile(identity).ok() &&
              take(empty.descriptor()).tensor_coverage(0).empty() &&
              generic.resources().size() == 0 &&
              generic.schema().tensors[0].facets.empty() &&
              read_bits(generic, {0, 0}) == double_bits(.5),
          "Empty CMYK owns profile while generic math drops unreferenced ICC");
  require(retained.resources().icc_profile(identity).ok() &&
              read_bits(retained, {0, 0}) == double_bits(.5) &&
              read_bits(retained, {0, 1}) == double_bits(.25) &&
              read_bits(retained, {0, 3}) == double_bits(.125),
          "CMYK Result retains exact colors and ICC owner after all producers "
          "retire");
}
void expression_workflows() {
  Driver d;
  auto start = d.source({ElementType::Float32, {1}}, {0}, {1, {INT64_MIN}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto a = d.source({ElementType::Float64, {1}}, {double_bits(2)}, {1, {0}});
  auto b = d.source({ElementType::Float32, {1}}, {float_bits(1)}, {1, {0}});
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
        CpuNumericProfile::X86Avx2}) {
    auto node = take(numeric::sample_expression_node(
        7, "a*x+b", WorkflowInputReference{11}, WorkflowInputReference{12}, 5,
        {{"a", WorkflowInputReference{13}}, {"b", WorkflowInputReference{14}}},
        ElementType::Float64, profile));
    std::vector<OperationMetadata> metadata;
    for (const auto& input : {start, end, a, b}) {
      OperationMetadata item;
      item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
      metadata.push_back(std::move(item));
    }
    auto available =
        d.registry->resolve_traits(node.operation, metadata, node.parameters);
    if (!available.ok()) {
      require(profile != CpuNumericProfile::Strict &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "unavailable expression profile reports BackendUnavailable");
      continue;
    }
    auto prepared =
        d.prepare(node.operation, {start, end, a, b}, node.parameters);
    auto document = prepared.graph->snapshot().document();
    document.outputs.push_back({"axis", 7, "axis"});
    auto graph = std::make_shared<GraphContext>(document);
    auto compiled = take(Compiler(d.registry).compile(*graph));
    require(compiled.plan.steps().size() == 2 &&
                compiled.plan.steps()[0].prepared->state() &&
                compiled.plan.steps()[0].prepared->state() ==
                    compiled.plan.steps()[1].prepared->state(),
            "named expression outputs share one immutable prepared AST");
    std::fenv_t saved;
    require(std::fegetenv(&saved) == 0 && std::fesetround(FE_DOWNWARD) == 0,
            "expression floating-environment fixture setup");
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    auto result = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)),
        {{"out", take(Footprint::all({5}))},
         {"axis", take(Footprint::from_regions({3}, {Region({{1, 1}})}))}}));
    const bool restored = std::fegetround() == FE_DOWNWARD &&
                          std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
    std::fesetenv(&saved);
    require(restored, "expression restores caller floating environment");
    for (uint64_t i = 0; i < 5; ++i)
      require(
          read_bits(result.results.at("out"), {i}) == double_bits(1 + i * .5),
          "expression reads mixed-dtype named Result coefficients");
    const auto& axis = result.results.at("axis");
    require(axis.schema().tensors[0].atomic_trailing_axes == 1 &&
                take(axis.descriptor()).tensor_coverage(0) ==
                    take(Footprint::all({3})) &&
                read_bits(axis, {0}) == 0 &&
                read_bits(axis, {1}) == double_bits(1) &&
                read_bits(axis, {2}) == double_bits(.25),
            "axis projection retains a complete atomic Float64 sampling tuple");
    auto changed = take(result.dependencies.potential_dirty(
        "input2", take(Footprint::all({1})), 1));
    require(changed.at("out") == take(Footprint::all({5})) &&
                changed.at("axis").empty(),
            "coefficient edits invalidate values but never axis");
    prepared.bindings.inputs[2].result =
        d.source({ElementType::Float64, {1}}, {double_bits(3)}, {1, {0}});
    prepared.bindings.inputs[3].result =
        d.source({ElementType::Float32, {1}}, {float_bits(-1)}, {1, {0}});
    auto repeated = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)),
        {{"out", take(Footprint::all({5}))},
         {"axis", take(Footprint::all({3}))}}));
    for (uint64_t i = 0; i < 5; ++i)
      require(read_bits(repeated.results.at("out"), {i}) ==
                  double_bits(-1 + i * .75),
              "compiled expression reuses static preparation with new dynamic "
              "bindings");
    require(read_bits(repeated.results.at("axis"), {2}) == double_bits(.25),
            "coefficient rebinding preserves independently computed sampling "
            "metadata");
  }
  auto node = take(numeric::sample_expression_node(
      7, "x*x", WorkflowInputReference{11}, WorkflowInputReference{12}, 5, {},
      ElementType::Float32));
  auto descending =
      take(d.run(node.operation, {end, start}, {}, node.parameters))
          .results.at("out");
  for (uint64_t i = 0; i < 5; ++i) {
    const float x = 1 - i * .25F;
    require(read_bits(descending, {i}) == float_bits(x * x),
            "descending sampling preserves coordinates and Float32 output "
            "conversion");
  }
  d.context.reset();
  require(read_bits(descending, {2}) == float_bits(.25F),
          "expression Result storage survives execution context retirement");
}
void expression_many_coefficients() {
  Driver d;
  auto scalar =
      d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  std::vector<ResultRef> inputs{scalar, scalar};
  std::map<std::string, WorkflowInput> coefficients;
  std::vector<std::string> names;
  for (unsigned i = 0; i < 128; ++i) {
    auto name = std::string("c") + (i < 100 ? "0" : "") + (i < 10 ? "0" : "") +
                std::to_string(i);
    names.push_back(name);
    coefficients.emplace(name, WorkflowInputReference{13 + i});
    inputs.push_back(
        d.source({ElementType::Float64, {1}}, {double_bits(i + 1)}, {1, {0}}));
  }
  while (names.size() > 1) {
    std::vector<std::string> paired;
    for (size_t i = 0; i < names.size(); i += 2)
      paired.push_back("(" + names[i] + "+" + names[i + 1] + ")");
    names = std::move(paired);
  }
  auto node = take(numeric::sample_expression_node(
      7, names.front(), WorkflowInputReference{11}, WorkflowInputReference{12},
      1, coefficients));
  auto result = take(d.run(node.operation, inputs, {}, node.parameters));
  require(read_bits(result.results.at("out"), {0}) == double_bits(8256),
          "128 coefficients preserve original port numbers across bounded Need "
          "envelopes");
  auto changed = take(result.dependencies.potential_dirty(
      "input129", take(Footprint::all({1})), 1));
  require(
      changed.at("out") == take(Footprint::all({1})),
      "final coefficient beyond the second envelope retains source support");
}
void expression_boundaries() {
  Driver d;
  auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto one = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto node = take(numeric::sample_expression_node(
      7, "ln(x)", WorkflowInputReference{11}, WorkflowInputReference{12}, 3));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed = d.run(node.operation, {zero, one},
                      take(Footprint::from_regions({3}, {Region({{2, 1}})})),
                      node.parameters);
  require(
      !failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
          failed.status().reason == FailureReason::InvalidDomain &&
          failed.status().detail.scope == FailureScope::Run &&
          !failed.status().detail.atom &&
          failed.status().message.find("sample=0 x=0") != std::string::npos &&
          failed.status().message.find("span=") != std::string::npos &&
          d.root.statistics().live[ResourceKind::Payload] == payload,
      "Whole expression failure outside Q retains sample, coordinate and "
      "source span");
  auto axis =
      take(d.run(node.operation, {zero, one}, {}, node.parameters, {}, "axis"))
          .results.at("out");
  require(read_bits(axis, {2}) == double_bits(.5),
          "axis-only skips expression domain evaluation");
  auto close = d.source({ElementType::Float64, {1}},
                        {UINT64_C(0x3ff0000000000001)}, {1, {0}});
  node = take(numeric::sample_expression_node(
      7, "x", WorkflowInputReference{11}, WorkflowInputReference{12}, 3));
  failed = d.run(node.operation, {one, close}, {}, node.parameters);
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().message.find("duplicate adjacent") !=
                  std::string::npos,
          "values reject duplicate RN64 coordinates");
  axis =
      take(d.run(node.operation, {one, close}, {}, node.parameters, {}, "axis"))
          .results.at("out");
  require(read_bits(axis, {2}) == double_bits(0x1p-53),
          "axis-only does not scan coordinate distinctness");
  auto empty = take(d.run(node.operation, {one, one},
                          take(Footprint::none({3})), node.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty expression skips invalid equal endpoints");
  node = take(numeric::sample_expression_node(
      7, "1/x", WorkflowInputReference{11}, WorkflowInputReference{12}, 3));
  failed = d.run(node.operation, {zero, one}, {}, node.parameters);
  require(!failed.ok() && failed.status().reason == FailureReason::DivideByZero,
          "expression division retains its distinct diagnostic reason");
  OperationMetadata good, legacy;
  good.result_schema = std::make_shared<SchemaTemplate>(zero.schema());
  legacy.descriptor = {ElementType::Float64, {1}};
  auto rejected = d.registry->resolve_traits(node.operation, {legacy, good},
                                             node.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "expression rejects incompatible input metadata before static parser "
          "access");
  auto multiple = zero.schema();
  auto extra = multiple.tensors[0];
  extra.key = "extra";
  multiple.tensors.push_back(extra);
  OperationMetadata ambiguous;
  ambiguous.result_schema = std::make_shared<SchemaTemplate>(multiple);
  rejected = d.registry->resolve_traits(node.operation, {ambiguous, good},
                                        node.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "expression requires an unambiguous single tensor input");
  auto bad_parameters = node.parameters;
  bad_parameters["coefficient_names"] = std::string("unused");
  rejected =
      d.registry->resolve_traits(node.operation, {good, good}, bad_parameters);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              rejected.status().detail.origin == FailureOrigin::Schema,
          "static coefficient-name mismatch remains a schema failure");
  auto wide = d.source({ElementType::Float64, {1}},
                       {UINT64_C(0x7fefffffffffffff)}, {1, {0}});
  node = take(numeric::sample_expression_node(
      7, "a", WorkflowInputReference{11}, WorkflowInputReference{12}, 1,
      {{"a", WorkflowInputReference{13}}}, ElementType::Float32));
  failed = d.run(node.operation, {zero, zero, wide}, {}, node.parameters);
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().message.find("final Float32 overflow") !=
                  std::string::npos,
          "expression final narrowing overflow retains its diagnostic");
  axis = take(d.run(node.operation, {zero, zero, wide}, {}, node.parameters, {},
                    "axis"))
             .results.at("out");
  require(read_bits(axis, {0}) == 0 && read_bits(axis, {2}) == 0,
          "axis remains independent of values final-conversion failure");
  Driver limited(30000);
  auto begin = limited.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto end =
      limited.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  node = take(numeric::sample_expression_node(
      7, "x*x", WorkflowInputReference{11}, WorkflowInputReference{12}, 65));
  const auto live = limited.root.statistics().live.values;
  failed = limited.run(node.operation, {begin, end}, {}, node.parameters);
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  live[static_cast<size_t>(ResourceKind::Payload)],
          "expression work exhaustion releases unpublished dense payload");
  limited.context.reset();
  require(limited.root.statistics().live.values == live,
          "expression failure releases execution resources after context "
          "retirement");
  CancellationSource stop;
  stop.cancel();
  failed =
      d.run(node.operation, {zero, one}, {}, node.parameters, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "expression pre-cancellation preserves host error");
}
void matrix_workflows() {
  Driver d;
  for (unsigned cin = 2; cin <= 4; ++cin)
    for (unsigned cout = 2; cout <= 4; ++cout) {
      std::vector<uint64_t> vectors(130 * cin), matrix(cin * cout), bias(cout);
      for (unsigned r = 0; r < 130; ++r)
        for (unsigned j = 0; j < cin; ++j)
          vectors[r * cin + (cin - 1 - j)] =
              float_bits((r % 17 + j + 1) * .25F);
      for (unsigned o = 0; o < cout; ++o) {
        bias[o] = float_bits((o + 1) * .25F);
        for (unsigned j = 0; j < cin; ++j)
          matrix[o * cin + j] =
              float_bits((o + j + 1) * .5F * (j % 2 ? -1 : 1));
      }
      auto x = d.source({ElementType::Float32, {65, cin}}, vectors,
                        {1 + (cin - 1) * 4,
                         {static_cast<int64_t>(65 * cin * 4),
                          static_cast<int64_t>(cin * 4), -4}},
                        {}, {2});
      auto m = d.source({ElementType::Float32, {cout, cin}}, matrix,
                        {1, {static_cast<int64_t>(cin * 4), 4}});
      auto b = d.source({ElementType::Float32, {cout}}, bias, {1, {4}});
      for (auto profile :
           {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
            CpuNumericProfile::X86Avx2}) {
        auto node = take(numeric::matrix_transform_node(
            7, WorkflowInputReference{11}, WorkflowInputReference{12},
            WorkflowInputReference{13}, profile));
        std::vector<OperationMetadata> metadata;
        for (const auto& input : {x, m, b}) {
          OperationMetadata item;
          item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
          metadata.push_back(std::move(item));
        }
        auto available =
            d.registry->resolve_traits(node.operation, metadata, {});
        if (!available.ok()) {
          require(profile != CpuNumericProfile::Strict &&
                      available.status().code == ErrorCode::BackendUnavailable,
                  "unavailable matrix profile reports BackendUnavailable");
          continue;
        }
        std::fenv_t saved;
        require(std::fegetenv(&saved) == 0 && std::fesetround(FE_UPWARD) == 0,
                "matrix floating-environment fixture setup");
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        auto result = take(d.run(node.operation, {x, m, b}));
        const bool restored = std::fegetround() == FE_UPWARD &&
                              std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
        std::fesetenv(&saved);
        require(restored,
                "matrix execution restores caller rounding mode and exception "
                "flags");
        const auto& tensor = result.results.at("out").schema().tensors[0];
        require(
            tensor.descriptor.shape == std::vector<uint64_t>({2, 65, cout}) &&
                tensor.batch_axes.empty() && tensor.facets.empty(),
            "matrix Result includes batch dimensions and replaces only last "
            "extent");
        for (unsigned r = 0; r < 130; ++r)
          for (unsigned o = 0; o < cout; ++o) {
            double expected = (o + 1) * .25;
            for (unsigned j = 0; j < cin; ++j)
              expected +=
                  (r % 17 + j + 1) * .25 * (o + j + 1) * .5 * (j % 2 ? -1 : 1);
            require(read_bits(result.results.at("out"), {r / 65, r % 65, o}) ==
                        float_bits(static_cast<float>(expected)),
                    "rectangular matrix matches independent exact dyadic sum "
                    "including block tails");
          }
        for (unsigned port = 0; port < 3; ++port)
          for (uint32_t role : {1U, 4U})
            require(take(result.dependencies.potential_dirty(
                             "input" + std::to_string(port),
                             take(Footprint::all(metadata[port]
                                                     .result_schema->tensors[0]
                                                     .sample_shape())),
                             role))
                            .at("out") == take(Footprint::all({2, 65, cout})),
                    "every matrix operand retains full Data and Validation "
                    "dependence");
      }
    }
  auto x = d.source({ElementType::Float64, {2}},
                    {double_bits(2), double_bits(3)}, {1, {8}});
  auto m = d.source({ElementType::Float64, {2, 2}},
                    {double_bits(1), double_bits(2), double_bits(-1), 0},
                    {1, {16, 8}});
  auto b = d.source({ElementType::Float64, {2}},
                    {double_bits(4), double_bits(5)}, {1, {8}});
  auto result = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
                    .results.at("out");
  require(read_bits(result, {0}) == double_bits(12) &&
              read_bits(result, {1}) == double_bits(3),
          "rank-one Float64 affine transform preserves matrix row orientation");
  d.context.reset();
  require(read_bits(result, {0}) == double_bits(12),
          "matrix output owns backing after context retirement");
}
void matrix_numerics_and_boundaries() {
  Driver d;
  auto x = d.source(
      {ElementType::Float64, {2}},
      {double_bits(std::ldexp(1., 600)), double_bits(std::ldexp(1., 600))},
      {1, {8}});
  auto m = d.source({ElementType::Float64, {2, 2}},
                    {double_bits(std::ldexp(1., 600)),
                     double_bits(-std::ldexp(1., 600)), 0, 0},
                    {1, {16, 8}});
  auto b = d.source({ElementType::Float64, {2}},
                    {double_bits(3), double_bits(-0.)}, {1, {8}});
  auto out = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
                 .results.at("out");
  require(
      read_bits(out, {0}) == double_bits(3) && read_bits(out, {1}) == 0,
      "exact products cancel before overflow and singular matrices are legal");
  auto residual = d.source({ElementType::Float32, {4}},
                           {float_bits(std::ldexp(1.F, 120)), 0x3f800000,
                            float_bits(-std::ldexp(1.F, 120)), 0},
                           {1, {4}});
  auto ones =
      d.source({ElementType::Float32, {2, 4}}, {0x3f800000}, {1, {0, 0}});
  auto zero_bias = d.source({ElementType::Float32, {2}}, {0}, {1, {0}});
  std::vector<OperationMetadata> residual_metadata;
  for (const auto& input : {residual, ones, zero_bias}) {
    OperationMetadata item;
    item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    residual_metadata.push_back(std::move(item));
  }
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    const auto key = std::string("numeric.matrix_transform") + suffix;
    auto available = d.registry->resolve_traits(key, residual_metadata, {});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "unavailable exact-replay profile remains explicit");
      continue;
    }
    auto exact =
        take(d.run(key, {residual, ones, zero_bias})).results.at("out");
    require(read_bits(exact, {0}) == 0x3f800000 &&
                read_bits(exact, {1}) == 0x3f800000,
            "matrix candidate cannot erase a residual lost by binary64 "
            "sequential summation");
  }
  x = d.source({ElementType::Float32, {2}}, {0xff800123, 0x7f800000}, {1, {4}});
  m = d.source({ElementType::Float32, {2, 2}}, {0x7f800456, 0, 0, 0x3f800000},
               {1, {8, 4}});
  b = d.source({ElementType::Float32, {2}}, {0x7f800789}, {1, {0}});
  for (const auto* key :
       {"numeric.matrix_transform_strict",
        "numeric.matrix_transform_accelerated_apple_silicon"}) {
    if (std::string(key).find("apple") != std::string::npos) {
#if !defined(__aarch64__) || !defined(__APPLE__)
      continue;
#endif
    }
    out = take(d.run(key, {x, m, b})).results.at("out");
    require(
        read_bits(out, {0}) == 0xffc00123 && read_bits(out, {1}) == 0xffc00123,
        "vector source NaN wins over matrix, bias and generated invalid "
        "products");
  }
  x = d.source({ElementType::Float32, {2}}, {0, 0x7f800000}, {1, {4}});
  m = d.source({ElementType::Float32, {2, 2}}, {0x7f800000, 0, 0, 0x3f800000},
               {1, {8, 4}});
  b = d.source({ElementType::Float32, {2}}, {0}, {1, {0}});
  out = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
            .results.at("out");
  require(
      read_bits(out, {0}) == 0x7fc00000 && read_bits(out, {1}) == 0x7f800000,
      "matrix zero-times-infinity and infinity classification");
  x = d.source({ElementType::Float32, {2}}, {0x80000000, 0}, {1, {4}});
  m = d.source({ElementType::Float32, {2, 2}},
               {0x3f800000, 0xbf800000, 0x3f800000, 0xbf800000}, {1, {8, 4}});
  b = d.source({ElementType::Float32, {2}}, {0x80000000, 0}, {1, {4}});
  out = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
            .results.at("out");
  require(read_bits(out, {0}) == 0x80000000 && read_bits(out, {1}) == 0,
          "matrix exact zero is negative only when all products and bias are "
          "negative zero");
  std::vector<OperationMetadata> metadata;
  for (const auto& input : {x, m, b}) {
    OperationMetadata item;
    item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    metadata.push_back(std::move(item));
  }
  auto bad_bias = b.schema();
  bad_bias.tensors[0].descriptor.shape = {3};
  auto invalid_metadata = metadata;
  invalid_metadata[2].result_schema =
      std::make_shared<SchemaTemplate>(bad_bias);
  auto rejected = d.registry->resolve_traits("numeric.matrix_transform_strict",
                                             invalid_metadata, {});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "matrix rejects output-component and bias-length mismatch before "
          "execution");
  auto oversized = x.schema();
  oversized.tensors[0].descriptor.shape = {UINT64_C(1) << 40, 2};
  invalid_metadata = metadata;
  invalid_metadata[0].result_schema =
      std::make_shared<SchemaTemplate>(oversized);
  rejected = d.registry->resolve_traits("numeric.matrix_transform_strict",
                                        invalid_metadata, {});
  require(
      !rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
      "matrix input count above 2^40 is rejected without payload allocation");
  auto typed =
      d.source({ElementType::Float32, {2, 2}}, {0, 0, 0, 0x40000000},
               {1, {8, 4}}, {take(encode_semantic(coverage_semantics()))});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed =
      d.run("numeric.matrix_transform_strict", {typed, m, b},
            take(Footprint::from_regions({2, 2}, {Region({{0, 1}, {0, 1}})})));
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 11 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "matrix validates typed samples outside projected output and "
          "releases payload");
  auto empty = take(d.run("numeric.matrix_transform_strict", {typed, m, b},
                          take(Footprint::none({2, 2}))));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty matrix skips payload validation");
  Driver limited(30000);
  auto vectors = limited.source({ElementType::Float32, {65, 2}}, {0x3f800000},
                                {1, {0, 0}});
  auto matrix =
      limited.source({ElementType::Float32, {2, 2}}, {0x3f800000}, {1, {0, 0}});
  auto bias = limited.source({ElementType::Float32, {2}}, {0}, {1, {0}});
  const auto live = limited.root.statistics().live.values;
  failed =
      limited.run("numeric.matrix_transform_strict", {vectors, matrix, bias});
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  live[static_cast<size_t>(ResourceKind::Payload)],
          "matrix WorkLimit releases unpublished payload and exact workspace");
  limited.context.reset();
  require(limited.root.statistics().live.values == live,
          "retired matrix failure releases all execution metadata");
  CancellationSource stop;
  stop.cancel();
  failed =
      d.run("numeric.matrix_transform_strict", {x, m, b}, {}, {}, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "matrix pre-cancellation preserves host category");
}
void calculus_workflows() {
  Driver d;
  auto samples = d.source({ElementType::Float64, {3}},
                          {double_bits(4), double_bits(1), 0}, {17, {-8}});
  auto step =
      d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {INT64_MIN}});
  auto initial = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  OperationMetadata source_metadata, control_metadata;
  source_metadata.result_schema =
      std::make_shared<SchemaTemplate>(samples.schema());
  control_metadata.result_schema =
      std::make_shared<SchemaTemplate>(step.schema());
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
        CpuNumericProfile::X86Avx2}) {
    auto derivative_node = take(numeric::derivative_1d_node(
        7, WorkflowInputReference{11}, WorkflowInputReference{12}, profile));
    auto available = d.registry->resolve_traits(
        derivative_node.operation, {source_metadata, control_metadata}, {});
    if (!available.ok()) {
      require(profile != CpuNumericProfile::Strict &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "unavailable calculus profile reports BackendUnavailable");
      continue;
    }
    auto derivative = take(d.run(derivative_node.operation, {samples, step}));
    auto integral_node = take(numeric::integrate_1d_node(
        7, WorkflowInputReference{11}, WorkflowInputReference{12},
        WorkflowInputReference{13}, profile));
    auto integral =
        take(d.run(integral_node.operation, {samples, step, initial}));
    const double expected_derivative[] = {1, 2, 3},
                 expected_integral[] = {0, .5, 3};
    for (uint64_t i = 0; i < 3; ++i) {
      require(read_bits(derivative.results.at("out"), {i}) ==
                  double_bits(expected_derivative[i]),
              "public derivative helper preserves exact one-sided and central "
              "differences");
      require(read_bits(integral.results.at("out"), {i}) ==
                  double_bits(expected_integral[i]),
              "public integral helper preserves exact trapezoidal prefix");
    }
    require(
        integral.results.at("out").schema().id == "photospider.tensor" &&
            integral.results.at("out").schema().tensors[0].key == "samples" &&
            integral.results.at("out").schema().tensors[0].facets.empty(),
        "calculus publishes generic Result tensor schema");
    for (unsigned port = 0; port < 3; ++port)
      require(
          take(integral.dependencies.potential_dirty(
                   "input" + std::to_string(port),
                   take(Footprint::all(port == 0 ? std::vector<uint64_t>{3}
                                                 : std::vector<uint64_t>{1})),
                   1))
                  .at("out") == take(Footprint::all({3})),
          "non-singleton integral has complete active input support");
  }
  step = d.source({ElementType::Float64, {1}}, {double_bits(-1)}, {1, {0}});
  auto negative = take(d.run("numeric.derivative_1d_strict", {samples, step}))
                      .results.at("out");
  require(read_bits(negative, {1}) == double_bits(-2),
          "negative calculus step is valid");
  auto pair = d.source({ElementType::Float64, {2}},
                       {double_bits(1), double_bits(3)}, {1, {8}});
  auto pair_result = take(d.run("numeric.derivative_1d_strict", {pair, step}))
                         .results.at("out");
  require(
      read_bits(pair_result, {0}) == double_bits(-2) &&
          read_bits(pair_result, {1}) == double_bits(-2),
      "two-sample derivative uses the same one-sided quotient at both ends");
  auto extreme =
      d.source({ElementType::Float64, {3}},
               {UINT64_C(0xffefffffffffffff), 0, UINT64_C(0x7fefffffffffffff)},
               {1, {8}});
  step = d.source({ElementType::Float64, {1}}, {UINT64_C(0x7fefffffffffffff)},
                  {1, {0}});
  auto exact = take(d.run("numeric.derivative_1d_strict", {extreme, step}))
                   .results.at("out");
  for (uint64_t i = 0; i < 3; ++i)
    require(read_bits(exact, {i}) == double_bits(1),
            "exact derivative avoids overflow in source difference and doubled "
            "step");
  samples = d.source({ElementType::Float64, {3}},
                     {double_bits(std::ldexp(1., 100)), double_bits(1),
                      double_bits(-std::ldexp(1., 100))},
                     {1, {8}});
  step = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  exact = take(d.run("numeric.integrate_1d_strict", {samples, step, initial}))
              .results.at("out");
  require(read_bits(exact, {2}) == double_bits(1),
          "exact integral retains low terms through wide cancellation");
  d.context.reset();
  require(read_bits(exact, {2}) == double_bits(1),
          "calculus output outlives execution context");
}
void calculus_boundaries() {
  Driver d;
  auto samples = d.source({ElementType::Float32, {3}},
                          {0, 0x7f800123, 0x40800000}, {1, {4}});
  auto step = d.source({ElementType::Float32, {1}}, {0x3f800000}, {1, {0}});
  auto initial =
      d.source({ElementType::Float32, {1}}, {0xff800456}, {1, {INT64_MIN}});
  auto derivative = take(d.run("numeric.derivative_1d_strict", {samples, step}))
                        .results.at("out");
  require(
      read_bits(derivative, {0}) == 0x7fc00123 &&
          read_bits(derivative, {1}) == 0x40000000 &&
          read_bits(derivative, {2}) == 0x7fc00123,
      "derivative excludes the center NaN from the interior numerical stencil");
  auto integral =
      take(d.run("numeric.integrate_1d_strict", {samples, step, initial}))
          .results.at("out");
  require(read_bits(integral, {0}) == 0xff800456 &&
              read_bits(integral, {2}) == 0xffc00456,
          "integral output zero copies raw initial and later values quiet "
          "initial-first NaN");
  auto infinities = d.source({ElementType::Float32, {3}},
                             {0x7f800000, 0x7f800000, 0}, {1, {4}});
  derivative = take(d.run("numeric.derivative_1d_strict", {infinities, step}))
                   .results.at("out");
  require(read_bits(derivative, {0}) == 0x7fc00000 &&
              read_bits(derivative, {1}) == 0xff800000 &&
              read_bits(derivative, {2}) == 0xff800000,
          "derivative classifies same-sign and one-sided infinity differences");
  infinities = d.source({ElementType::Float32, {3}},
                        {0x7f800000, 0xff800000, 0}, {1, {4}});
  auto zero = d.source({ElementType::Float32, {1}}, {0}, {1, {0}});
  integral =
      take(d.run("numeric.integrate_1d_strict", {infinities, step, zero}))
          .results.at("out");
  require(read_bits(integral, {0}) == 0 &&
              read_bits(integral, {1}) == 0x7fc00000 &&
              read_bits(integral, {2}) == 0x7fc00000,
          "integral classifies opposite source infinities independently of "
          "output carry");
  for (uint64_t bits : {UINT64_C(0), UINT64_C(0x80000000), UINT64_C(0x7f800000),
                        UINT64_C(0x7f800123)}) {
    auto invalid = d.source({ElementType::Float32, {1}}, {bits}, {1, {0}});
    const auto payload = d.root.statistics().live[ResourceKind::Payload];
    auto failed =
        d.run("numeric.integrate_1d_strict", {samples, invalid, initial},
              take(Footprint::from_regions({3}, {Region({{0, 1}})})));
    require(!failed.ok() &&
                failed.status().code == ErrorCode::InvalidArgument &&
                failed.status().reason == FailureReason::InvalidDomain &&
                failed.status().detail.scope == FailureScope::Run &&
                failed.status().message.find("InvalidSampleStep: port=1") !=
                    std::string::npos &&
                d.root.statistics().live[ResourceKind::Payload] == payload,
            "non-singleton output-zero projection still validates step and "
            "rolls back");
    auto empty =
        take(d.run("numeric.integrate_1d_strict", {samples, invalid, initial},
                   take(Footprint::none({3}))));
    require(
        take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
        "Empty calculus skips invalid dynamic step");
  }
  auto scalar_sample =
      d.source({ElementType::Float32, {1}}, {0x7f800123}, {1, {0}});
  auto invalid_step = d.source({ElementType::Float32, {1}}, {0}, {1, {0}});
  auto singleton = take(d.run("numeric.integrate_1d_strict",
                              {scalar_sample, invalid_step, initial}));
  require(read_bits(singleton.results.at("out"), {0}) == 0xff800456,
          "singleton integral ignores invalid samples and step numerically");
  for (const auto& observation :
       take(singleton.dependencies.source_observations()))
    require(observation.input == "input2",
            "singleton integral observes only initial");
  Driver limited(30000);
  auto many =
      limited.source({ElementType::Float32, {64}}, {0x3f800000}, {1, {0}});
  auto unit =
      limited.source({ElementType::Float32, {1}}, {0x3f800000}, {1, {0}});
  const auto live = limited.root.statistics().live.values;
  auto failed = limited.run("numeric.integrate_1d_strict", {many, unit, unit});
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  live[static_cast<size_t>(ResourceKind::Payload)],
          "calculus WorkLimit releases unpublished payload");
  limited.context.reset();
  require(limited.root.statistics().live.values == live,
          "retiring failed calculus releases all execution resources");
  CancellationSource stop;
  stop.cancel();
  failed = d.run("numeric.derivative_1d_strict", {samples, step}, {}, {},
                 stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "calculus pre-cancellation preserves host status");
}
void scan_workflows() {
  Driver d;
  auto input =
      d.source({ElementType::UInt8, {2, 3}}, {1, 2, 3, 4, 5, 6}, {1, {3, 1}});
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    const auto key = std::string("numeric.prefix_sum") + suffix;
    const auto prefix_parameters =
        std::map<std::string, ParameterValue>{{"axis", int64_t{1}},
                                              {"dtype", std::string("int64")}};
    auto available =
        d.registry->resolve_traits(key, {metadata}, prefix_parameters);
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "unavailable scan profile reports BackendUnavailable");
      continue;
    }
    auto prefix = take(d.run(key, {input}, {}, prefix_parameters));
    auto integral = take(
        d.run(std::string("numeric.integral_image") + suffix, {input}, {},
              {{"axes", std::string("1,0")}, {"dtype", std::string("int64")}}));
    require(
        prefix.results.at("out").schema().tensors[0].descriptor.shape ==
                std::vector<uint64_t>({2, 4}) &&
            integral.results.at("out").schema().tensors[0].descriptor.shape ==
                std::vector<uint64_t>({3, 4}),
        "scan output adds leading zero boundary on selected axes");
    for (uint64_t y = 0; y <= 2; ++y)
      for (uint64_t x = 0; x <= 3; ++x) {
        uint64_t sum = 0;
        for (uint64_t row = 0; row < y; ++row)
          for (uint64_t column = 0; column < x; ++column)
            sum += row * 3 + column + 1;
        require(read_bits(integral.results.at("out"), {y, x}) == sum,
                "integral matches independent rectangle enumeration");
        if (y < 2) {
          sum = 0;
          for (uint64_t column = 0; column < x; ++column)
            sum += y * 3 + column + 1;
          require(read_bits(prefix.results.at("out"), {y, x}) == sum,
                  "prefix matches independent line enumeration");
        }
      }
    auto changed =
        take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {2, 1}})}));
    for (uint32_t role : {1U, 4U})
      require(take(prefix.dependencies.potential_dirty("input0", changed, role))
                      .at("out") == take(Footprint::all({2, 4})),
              "source data and validation changes invalidate all scan outputs");
  }
  std::vector<uint64_t> values(48);
  for (size_t i = 0; i < values.size(); ++i)
    values[i] = i + 1;
  auto batched = d.source({ElementType::Int64, {3, 2, 4}}, values,
                          {25, {192, 64, 32, -8}}, {}, {2});
  auto node = take(numeric::integral_image_node(7, WorkflowInputReference{11},
                                                {3, 1}, ElementType::Int64));
  auto result = take(d.run(node.operation, {batched}, {}, node.parameters))
                    .results.at("out");
  const auto& tensor = result.schema().tensors[0];
  require(tensor.descriptor.shape == std::vector<uint64_t>({2, 4, 2, 5}) &&
              tensor.batch_axes.empty() && tensor.facets.empty(),
          "integral uses complete sample shape and drops batch topology");
  for (uint64_t b = 0; b < 2; ++b)
    for (uint64_t y = 0; y <= 3; ++y)
      for (uint64_t channel = 0; channel < 2; ++channel)
        for (uint64_t x = 0; x <= 4; ++x) {
          uint64_t expected = 0;
          for (uint64_t row = 0; row < y; ++row)
            for (uint64_t column = 0; column < x; ++column)
              expected += ((b * 3 + row) * 2 + channel) * 4 + (3 - column) + 1;
          require(read_bits(result, {b, y, channel, x}) == expected,
                  "nonadjacent integral axes preserve negative stride and "
                  "plane resets");
        }
  auto prefix_node = take(numeric::prefix_sum_node(
      7, WorkflowInputReference{11}, 0, ElementType::Int64));
  auto batch_prefix =
      take(d.run(prefix_node.operation, {batched}, {}, prefix_node.parameters))
          .results.at("out");
  require(read_bits(batch_prefix, {2, 2, 1, 3}) == 21 + 45,
          "prefix can scan across a Result batch axis");
  d.context.reset();
  require(read_bits(result, {1, 3, 1, 4}) == 462,
          "scan output owns its storage after execution context retirement");
}
void scan_numerics() {
  Driver d;
  const auto prefix_parameters =
      std::map<std::string, ParameterValue>{{"axis", int64_t{0}},
                                            {"dtype", std::string("float64")}};
  auto source =
      d.source({ElementType::Float64, {3}},
               {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff),
                UINT64_C(0xffefffffffffffff)},
               {1, {8}});
  auto prefix =
      take(d.run("numeric.prefix_sum_strict", {source}, {}, prefix_parameters))
          .results.at("out");
  require(read_bits(prefix, {0}) == 0 &&
              read_bits(prefix, {1}) == UINT64_C(0x7fefffffffffffff) &&
              read_bits(prefix, {2}) == UINT64_C(0x7ff0000000000000) &&
              read_bits(prefix, {3}) == UINT64_C(0x7fefffffffffffff),
          "floating scan output overflow never contaminates exact carry");
  source = d.source({ElementType::Float64, {3}},
                    {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                     UINT64_C(0xfff0000000000123)},
                    {1, {8}});
  prefix =
      take(d.run("numeric.prefix_sum_strict", {source}, {}, prefix_parameters))
          .results.at("out");
  require(read_bits(prefix, {2}) == UINT64_C(0x7ff8000000000000) &&
              read_bits(prefix, {3}) == UINT64_C(0xfff8000000000123),
          "later source NaN has priority over a previously generated NaN");
  auto zeros =
      d.source({ElementType::Float32, {2, 2}}, {0x80000000}, {1, {0, 0}});
  auto integral = take(d.run("numeric.integral_image_strict", {zeros}, {},
                             {{"axes", std::string("0,1")},
                              {"dtype", std::string("float32")}}))
                      .results.at("out");
  require(read_bits(integral, {0, 2}) == 0 &&
              read_bits(integral, {2, 0}) == 0 &&
              read_bits(integral, {2, 2}) == 0x80000000,
          "integral +0 padding is not part of nonempty negative-zero sums");
  auto nan_order = d.source({ElementType::Float32, {2, 2}},
                            {0, 0xff800123, 0x7f800456, 0}, {1, {8, 4}});
  integral = take(d.run("numeric.integral_image_strict", {nan_order}, {},
                        {{"axes", std::string("1,0")},
                         {"dtype", std::string("float32")}}))
                 .results.at("out");
  require(read_bits(integral, {2, 1}) == 0x7fc00456 &&
              read_bits(integral, {2, 2}) == 0xffc00123,
          "integral NaN priority follows logical row-major source order");
  auto cancellation =
      d.source({ElementType::Float64, {2, 2}},
               {double_bits(std::ldexp(1.0, 100)), double_bits(1),
                double_bits(-std::ldexp(1.0, 100)), double_bits(2)},
               {1, {16, 8}});
  integral = take(d.run("numeric.integral_image_strict", {cancellation}, {},
                        {{"axes", std::string("0,1")},
                         {"dtype", std::string("float64")}}))
                 .results.at("out");
  require(read_bits(integral, {2, 2}) == double_bits(3),
          "integral combines exact rows rather than rounded row outputs");
}
void scan_boundaries() {
  Driver d;
  auto integers =
      d.source({ElementType::Int64, {3}}, {INT64_MAX, 1, UINT64_MAX}, {1, {8}});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed = d.run("numeric.prefix_sum_strict", {integers},
                      take(Footprint::from_regions({4}, {Region({{3, 1}})})),
                      {{"axis", int64_t{0}}, {"dtype", std::string("int64")}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().detail.scope == FailureScope::Run &&
              failed.status().message.find("output=[2,") != std::string::npos &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "unrequested overflowing prefix fails Whole and releases output");
  auto rectangle = d.source({ElementType::Int64, {2, 2}},
                            {INT64_MAX, 1, UINT64_MAX, 0}, {1, {16, 8}});
  failed =
      d.run("numeric.integral_image_strict", {rectangle},
            take(Footprint::from_regions({3, 3}, {Region({{2, 1}, {2, 1}})})),
            {{"axes", std::string("0,1")}, {"dtype", std::string("int64")}});
  require(
      !failed.ok() &&
          failed.status().reason == FailureReason::ArithmeticOverflow,
      "unrequested integral rectangle overflow fails even if full sum fits");
  auto typed =
      d.source({ElementType::Float32, {1, 2}}, {0, 0x40000000}, {1, {8, 4}},
               {take(encode_semantic(coverage_semantics()))});
  const auto parameters =
      std::map<std::string, ParameterValue>{{"axis", int64_t{1}},
                                            {"dtype", std::string("float32")}};
  failed =
      d.run("numeric.prefix_sum_strict", {typed},
            take(Footprint::from_regions({1, 3}, {Region({{0, 1}, {0, 1}})})),
            parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 11,
          "zero-boundary prefix demand still validates the complete input");
  auto empty = take(d.run("numeric.prefix_sum_strict", {typed},
                          take(Footprint::none({1, 3})), parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty scan skips source payload validation");
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(rectangle.schema());
  auto bad_axes = d.registry->resolve_traits(
      "numeric.integral_image_strict", {metadata},
      {{"axes", std::string("0,0")}, {"dtype", std::string("int64")}});
  require(
      !bad_axes.ok() && bad_axes.status().code == ErrorCode::InvalidArgument,
      "duplicate integral axes fail before execution");
  Driver limited(30000);
  auto many = limited.source({ElementType::Float32, {16, 16}}, {0x3f800000},
                             {1, {0, 0}});
  const auto live = limited.root.statistics().live.values;
  failed = limited.run(
      "numeric.integral_image_strict", {many}, {},
      {{"axes", std::string("0,1")}, {"dtype", std::string("float32")}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  live[static_cast<size_t>(ResourceKind::Payload)],
          "integral work failure releases unpublished output and exact state");
  limited.context.reset();
  require(limited.root.statistics().live.values == live,
          "retiring failed integral execution releases all metadata including "
          "columns");
  CancellationSource stop;
  stop.cancel();
  failed =
      d.run("numeric.prefix_sum_strict", {typed}, {}, parameters, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "scan pre-cancellation preserves host category");
}
void reduction_workflows() {
  Driver d;
  auto input =
      d.source({ElementType::Int64, {2, 3}}, {1, 2, 3, 4, 5, 6}, {1, {24, 8}});
  const char* names[] = {"sum",   "minimum",  "maximum", "mean",
                         "count", "variance", "std"};
  const uint64_t expected[][2] = {
      {6, 15},
      {1, 4},
      {3, 6},
      {UINT64_C(0x4000000000000000), UINT64_C(0x4014000000000000)},
      {3, 3},
      {UINT64_C(0x3fe5555555555555), UINT64_C(0x3fe5555555555555)},
      {UINT64_C(0x3fea20bd700c2c3e), UINT64_C(0x3fea20bd700c2c3e)}};
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    auto available = d.registry->resolve_traits(
        std::string("numeric.reduce_sum") + suffix, {metadata},
        {{"axes", std::string("1")}, {"dtype", std::string("int64")}});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "reducer profile availability");
      continue;
    }
    for (unsigned kind = 0; kind < 7; ++kind) {
      std::map<std::string, ParameterValue> parameters{
          {"axes", std::string("1")}};
      if (kind == 0 || kind == 3 || kind >= 5)
        parameters["dtype"] = std::string(kind == 0 ? "int64" : "float64");
      if (kind >= 5)
        parameters["ddof"] = static_cast<int64_t>(0);
      auto result =
          take(d.run(std::string("numeric.reduce_") + names[kind] + suffix,
                     {input}, {}, parameters));
      auto output = result.results.at("out");
      require(output.schema().tensors[0].descriptor.shape ==
                      std::vector<uint64_t>{2, 1} &&
                  output.schema().tensors[0].facets.empty() &&
                  read_bits(output, {0, 0}) == expected[kind][0] &&
                  read_bits(output, {1, 0}) == expected[kind][1],
              "reduction keeps rank and exact group formula in every available "
              "profile");
      auto edit =
          take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {2, 1}})}));
      auto dirty = take(result.dependencies.potential_dirty("input0", edit, 1));
      require(kind == 4 ? dirty.empty() || dirty.at("out").empty()
                        : dirty.at("out") == take(Footprint::all({2, 1})),
              "numeric reductions retain Whole input support while count "
              "excludes payload");
    }
  }
  auto widest =
      d.source({ElementType::Float64, {3}},
               {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff),
                UINT64_C(0xffefffffffffffff)},
               {1, {8}});
  auto summed = take(d.run("numeric.reduce_sum_strict", {widest}, {},
                           {{"axes", std::string("0")},
                            {"dtype", std::string("float64")}}))
                    .results.at("out");
  require(read_bits(summed, {0}) == UINT64_C(0x7fefffffffffffff),
          "sum rounds only the final exact total after overflowing floating "
          "prefixes");
  auto opposing = d.source(
      {ElementType::Float64, {2}},
      {UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff)}, {1, {8}});
  auto variance = take(d.run("numeric.reduce_variance_strict", {opposing}, {},
                             {{"axes", std::string("0")},
                              {"dtype", std::string("float64")},
                              {"ddof", static_cast<int64_t>(0)}}))
                      .results.at("out");
  auto deviation = take(d.run("numeric.reduce_std_strict", {opposing}, {},
                              {{"axes", std::string("0")},
                               {"dtype", std::string("float64")},
                               {"ddof", static_cast<int64_t>(0)}}))
                       .results.at("out");
  require(read_bits(variance, {0}) == UINT64_C(0x7ff0000000000000) &&
              read_bits(deviation, {0}) == UINT64_C(0x7fefffffffffffff),
          "standard deviation rounds the exact root rather than rounded "
          "infinite variance");
  auto integers = d.source(
      {ElementType::Int64, {2}},
      {UINT64_C(9007199254740993), UINT64_C(9007199254740994)}, {1, {8}});
  auto mean = take(d.run("numeric.reduce_mean_strict", {integers}, {},
                         {{"axes", std::string("0")},
                          {"dtype", std::string("float64")}}))
                  .results.at("out");
  require(read_bits(mean, {0}) == UINT64_C(0x4340000000000001),
          "integer mean divides the exact sum before binary64 rounding");
  auto nan = d.source({ElementType::Float64, {2, 2}},
                      {UINT64_C(0x7ff0000000000021), double_bits(2),
                       UINT64_C(0xfff0000000000003), double_bits(1)},
                      {25, {-16, -8}});
  auto prioritized = take(d.run("numeric.reduce_variance_strict", {nan}, {},
                                {{"axes", std::string("1,0")},
                                 {"dtype", std::string("float32")},
                                 {"ddof", static_cast<int64_t>(0)}}))
                         .results.at("out");
  require(read_bits(prioritized, {0, 0}) == 0xffc00001,
          "multiple-axis moments preserve first logical NaN sign/payload "
          "through narrowing");
  auto bytes = d.source({ElementType::UInt8, {2}}, {100, 155}, {1, {1}});
  auto byte_sum =
      take(d.run("numeric.reduce_sum_strict", {bytes}, {},
                 {{"axes", std::string("0")}, {"dtype", std::string("uint8")}}))
          .results.at("out");
  require(read_bits(byte_sum, {0}) == 255,
          "UInt8 reduction checks the final destination range");
}
void reduction_boundaries() {
  Driver d;
  auto integers = d.source({ElementType::Int64, {2, 2}}, {1, 2, INT64_MAX, 1},
                           {1, {16, 8}});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed =
      d.run("numeric.reduce_sum_strict", {integers},
            take(Footprint::from_regions({2, 1}, {Region({{0, 1}, {0, 1}})})),
            {{"axes", std::string("1")}, {"dtype", std::string("int64")}});
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.scope == FailureScope::Run &&
              failed.status().message.find("output linear=1") !=
                  std::string::npos &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "unrequested reduction group overflow fails the run and releases all "
          "output");
  auto facet = take(encode_semantic(coverage_semantics()));
  auto bad = d.source({ElementType::Float32, {2, 2}}, {0, 0, 0, 0x40000000},
                      {1, {8, 4}}, {facet});
  auto count = take(d.run("numeric.reduce_count_strict", {bad}, {},
                          {{"axes", std::string("1")}}))
                   .results.at("out");
  require(read_bits(count, {1, 0}) == 2,
          "count validates schema without validating typed payload");
  failed =
      d.run("numeric.reduce_mean_strict", {bad},
            take(Footprint::from_regions({2, 1}, {Region({{0, 1}, {0, 1}})})),
            {{"axes", std::string("1")}, {"dtype", std::string("float64")}});
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.input_id == 11,
          "numeric reduction performs complete typed validation outside Q");
  auto empty = take(d.run(
      "numeric.reduce_sum_strict", {integers}, take(Footprint::none({2, 1})),
      {{"axes", std::string("1")}, {"dtype", std::string("int64")}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty reduction bypasses overflowing arithmetic");
  for (const auto& axes :
       {std::string(""), std::string("1,1"), std::string("2")}) {
    OperationMetadata metadata;
    metadata.result_schema =
        std::make_shared<SchemaTemplate>(integers.schema());
    auto rejected = d.registry->resolve_traits("numeric.reduce_count_strict",
                                               {metadata}, {{"axes", axes}});
    require(
        !rejected.ok() && rejected.status().code == ErrorCode::InvalidArgument,
        "reducer axes must be nonempty distinct and within full rank");
  }
  OperationMetadata metadata;
  metadata.result_schema = std::make_shared<SchemaTemplate>(integers.schema());
  auto ddof =
      d.registry->resolve_traits("numeric.reduce_variance_strict", {metadata},
                                 {{"axes", std::string("1")},
                                  {"dtype", std::string("float64")},
                                  {"ddof", static_cast<int64_t>(2)}});
  require(!ddof.ok() && ddof.status().message.find("InvalidDegreesOfFreedom") !=
                            std::string::npos,
          "degrees of freedom is statically checked before special-value "
          "arithmetic");
  Driver limited(5000);
  auto data =
      limited.source({ElementType::Float64, {16}}, {double_bits(1)}, {1, {0}});
  const auto live = limited.root.statistics().live[ResourceKind::Payload];
  failed = limited.run("numeric.reduce_variance_strict", {data}, {},
                       {{"axes", std::string("0")},
                        {"dtype", std::string("float64")},
                        {"ddof", static_cast<int64_t>(0)}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] == live,
          "moments work rejection releases exact state and unpublished output");
  CancellationSource stop;
  stop.cancel();
  failed = d.run("numeric.reduce_count_strict", {bad}, {},
                 {{"axes", std::string("1")}}, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "metadata-only count still observes cancellation before publication");
}
void sequence_workflows() {
  Driver d;
  auto start = d.source({ElementType::Float32, {1}}, {0}, {1, {INT64_MIN}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto step =
      d.source({ElementType::Float64, {1}}, {double_bits(.25)}, {1, {0}});
  const auto parameters =
      std::map<std::string, ParameterValue>{{"count", static_cast<int64_t>(5)},
                                            {"dtype", std::string("float64")}};
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata first_metadata, second_metadata;
    first_metadata.result_schema =
        std::make_shared<SchemaTemplate>(start.schema());
    second_metadata.result_schema =
        std::make_shared<SchemaTemplate>(end.schema());
    auto available = d.registry->resolve_traits(
        std::string("numeric.linspace") + suffix,
        {first_metadata, second_metadata}, parameters);
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "sequence profile availability");
      continue;
    }
    for (bool range : {false, true}) {
      const auto key =
          std::string(range ? "numeric.arange" : "numeric.linspace") + suffix;
      auto inputs = std::vector<ResultRef>{start, range ? step : end};
      auto result = take(d.run(key, inputs, {}, parameters));
      for (uint64_t index = 0; index < 5; ++index)
        require(read_bits(result.results.at("out"), {index}) ==
                    double_bits(index * .25),
                "sequence computes each exact affine sample independently");
      auto axis = take(d.run(key, inputs, {}, parameters, {}, "axis"))
                      .results.at("out");
      require(axis.schema().tensors[0].atomic_trailing_axes == 1 &&
                  read_bits(axis, {0}) == 0 &&
                  read_bits(axis, {1}) == double_bits(1) &&
                  read_bits(axis, {2}) == double_bits(.25),
              "sequence independently publishes complete atomic axis tuple");
      auto edit = take(Footprint::from_regions({1}, {Region({{0, 1}})}));
      require(take(result.dependencies.potential_dirty("input1", edit, 1))
                      .at("out") == take(Footprint::all({5})),
              "sequence values retain active scalar Whole dependency");
    }
  }
  auto prepared =
      d.prepare("numeric.linspace_strict", {start, end}, parameters);
  auto document = prepared.graph->snapshot().document();
  document.outputs.push_back({"axis", 7, "axis"});
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(d.registry).compile(*graph));
  auto both = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({5}))},
       {"axis", take(Footprint::from_regions({3}, {Region({{2, 1}})}))}}));
  require(both.results.size() == 2 &&
              read_bits(both.results.at("axis"), {0}) == 0 &&
              read_bits(both.results.at("axis"), {2}) == double_bits(.25),
          "one-component axis demand closes its complete tuple independently "
          "of values");
  auto large = d.source({ElementType::Int64, {1}}, {UINT64_C(9007199254740993)},
                        {1, {0}});
  auto two = d.source({ElementType::Int64, {1}}, {2}, {1, {0}});
  auto integers = take(d.run("numeric.arange_strict", {large, two}, {},
                             {{"count", static_cast<int64_t>(3)},
                              {"dtype", std::string("int64")}}))
                      .results.at("out");
  for (uint64_t index = 0; index < 3; ++index)
    require(
        read_bits(integers, {index}) == UINT64_C(9007199254740993) + index * 2,
        "integer sequence never converts values above 2^53 through Float64");
  auto axis = take(d.run("numeric.arange_strict", {large, two}, {},
                         {{"count", static_cast<int64_t>(3)},
                          {"dtype", std::string("int64")}},
                         {}, "axis"))
                  .results.at("out");
  require(read_bits(axis, {0}) == UINT64_C(9007199254740993) &&
              read_bits(axis, {1}) == UINT64_C(9007199254740997) &&
              read_bits(axis, {2}) == 2,
          "integer axis retains exact endpoint and step bits");
  auto lo = d.source({ElementType::Float64, {1}},
                     {UINT64_C(0xffefffffffffffff)}, {1, {0}});
  auto hi = d.source({ElementType::Float64, {1}},
                     {UINT64_C(0x7fefffffffffffff)}, {1, {0}});
  auto extreme = take(d.run("numeric.linspace_strict", {lo, hi}, {},
                            {{"count", static_cast<int64_t>(3)},
                             {"dtype", std::string("float64")}}))
                     .results.at("out");
  require(read_bits(extreme, {0}) == UINT64_C(0xffefffffffffffff) &&
              read_bits(extreme, {1}) == 0 &&
              read_bits(extreme, {2}) == UINT64_C(0x7fefffffffffffff),
          "linspace extreme finite endpoints cancel without overflowing "
          "intermediates");
  auto midpoint_start =
      d.source({ElementType::Float64, {1}},
               {double_bits(1 + std::ldexp(1.0, -24))}, {1, {0}});
  auto midpoint_end =
      d.source({ElementType::Float64, {1}},
               {double_bits(1 + 3 * std::ldexp(1.0, -24))}, {1, {0}});
  auto rounded =
      take(d.run("numeric.linspace_strict", {midpoint_start, midpoint_end}, {},
                 {{"count", static_cast<int64_t>(3)},
                  {"dtype", std::string("float32")}}))
          .results.at("out");
  require(
      read_bits(rounded, {0}) == 0x3f800000 &&
          read_bits(rounded, {1}) == 0x3f800001 &&
          read_bits(rounded, {2}) == 0x3f800002,
      "linspace rounds exact rationals directly to binary32 with even ties");
  auto negative_zero = d.source({ElementType::Float64, {1}},
                                {UINT64_C(0x8000000000000000)}, {1, {0}});
  rounded =
      take(d.run("numeric.linspace_strict", {negative_zero, negative_zero}, {},
                 {{"count", static_cast<int64_t>(3)},
                  {"dtype", std::string("float32")}}))
          .results.at("out");
  require(read_bits(rounded, {0}) == 0x80000000 &&
              read_bits(rounded, {1}) == 0x80000000 &&
              read_bits(rounded, {2}) == 0x80000000,
          "constant negative-zero sequence retains zero sign");
  auto direct_start =
      d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto direct_end = d.source({ElementType::Float64, {1}},
                             {double_bits(0x1.0000020000001p0)}, {1, {0}});
  auto direct =
      take(d.run("numeric.linspace_strict", {direct_start, direct_end}, {},
                 {{"count", static_cast<int64_t>(3)},
                  {"dtype", std::string("float32")}}))
          .results.at("out");
  require(read_bits(direct, {1}) == 0x3f800001,
          "direct binary32 rounding avoids the distinct binary64 "
          "double-rounding result");
  auto min_integer = d.source({ElementType::Int64, {1}},
                              {UINT64_C(0x8000000000000000)}, {1, {0}});
  auto max_step = d.source({ElementType::Int64, {1}}, {INT64_MAX}, {1, {0}});
  auto cancelled_product =
      take(d.run("numeric.arange_strict", {min_integer, max_step}, {},
                 {{"count", static_cast<int64_t>(3)},
                  {"dtype", std::string("int64")}}))
          .results.at("out");
  require(read_bits(cancelled_product, {2}) == UINT64_C(0x7ffffffffffffffe),
          "integer sequence admits a wide product whose final exact sum fits "
          "Int64");
  const auto old_round = std::fegetround();
  const auto old_flags = std::fetestexcept(FE_ALL_EXCEPT);
  std::fesetround(FE_UPWARD);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::feraiseexcept(FE_INEXACT);
  auto environment = d.run(
      "numeric.linspace_strict", {direct_start, direct_end}, {},
      {{"count", static_cast<int64_t>(3)}, {"dtype", std::string("float32")}});
  const bool restored = std::fegetround() == FE_UPWARD &&
                        std::fetestexcept(FE_ALL_EXCEPT) == FE_INEXACT;
  std::fesetround(old_round);
  std::feclearexcept(FE_ALL_EXCEPT);
  if (old_flags)
    std::feraiseexcept(old_flags);
  require(
      environment.ok() && restored &&
          read_bits(environment.value().results.at("out"), {1}) == 0x3f800001,
      "sequence restores caller rounding mode and exception flags");
}
void sequence_authoring() {
  Driver d;
  auto integer = d.source({ElementType::Int64, {1}},
                          {UINT64_C(9007199254740993)}, {1, {0}});
  auto integer_step = d.source({ElementType::Int64, {1}}, {2}, {1, {0}});
  auto floating = d.source({ElementType::Float32, {1}}, {0}, {1, {0}});
  auto half =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  for (bool integers : {false, true}) {
    auto prepared =
        d.prepare("numeric.arange_strict",
                  integers ? std::vector<ResultRef>{integer, integer_step}
                           : std::vector<ResultRef>{floating, half},
                  {{"count", static_cast<int64_t>(3)},
                   {"dtype", std::string(integers ? "int64" : "float64")}});
    auto document = prepared.graph->snapshot().document();
    auto start = numeric::sequence_input(document.inputs[0]);
    auto other = numeric::sequence_input(document.inputs[1]);
    auto authored = take(numeric::arange_node(7, start, other, 3));
    require(std::get<std::string>(authored.parameters.at("dtype")) ==
                (integers ? "int64" : "float64"),
            "public arange helper infers numeric kind from sole Result tensor "
            "schema");
    document.nodes[0] = std::move(authored);
    auto graph = std::make_shared<GraphContext>(document);
    auto compiled = take(Compiler(d.registry).compile(*graph));
    auto result = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)),
        {{"out", take(Footprint::all({3}))}}));
    require(read_bits(result.results.at("out"), {2}) ==
                (integers ? UINT64_C(9007199254740997) : double_bits(1)),
            "public Result-only arange authoring helper builds a runnable "
            "workflow");
    if (!integers) {
      document.nodes[0] = take(
          numeric::linspace_node(7, start, other, 3, ElementType::Float32));
      graph = std::make_shared<GraphContext>(document);
      compiled = take(Compiler(d.registry).compile(*graph));
      result = take(d.context->execute_fragments(
          take(d.context->freeze(compiled.plan, prepared.bindings)),
          {{"out", take(Footprint::all({3}))}}));
      require(
          read_bits(result.results.at("out"), {1}) == 0x3e800000,
          "public Result-only linspace helper preserves selected output dtype");
    }
  }
  WorkflowInputDeclaration invalid;
  invalid.id = 99;
  auto rejected = numeric::linspace_node(7, numeric::sequence_input(invalid),
                                         numeric::sequence_input(invalid), 3);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "sequence authoring rejects missing Result schema hints");
}
void result_shaper_authoring() {
  Driver d;
  auto input = d.source({ElementType::Float32, {3}},
                        {0x3f800000, 0x40000000, 0x40400000}, {1, {4}});
  auto lower = d.source({ElementType::Float32, {1}}, {0x3f800000}, {1, {0}});
  auto upper = d.source({ElementType::Float32, {1}}, {0x40400000}, {1, {0}});
  auto prepared = d.prepare("numeric.clamp_strict", {input, input, input});
  auto document = prepared.graph->snapshot().document();
  prepared.bindings.inputs[1].result = lower;
  prepared.bindings.inputs[2].result = upper;
  document.inputs[1].result_schema =
      std::make_shared<SchemaTemplate>(lower.schema());
  document.inputs[2].result_schema =
      std::make_shared<SchemaTemplate>(upper.schema());
  document.nodes.clear();
  document.outputs.clear();
  auto output = take(numeric::linear_shaper(
      document, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, {ElementType::Float32, {3}}));
  document.outputs = {{"out", output.source_node, output.source_port}};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(d.registry).compile(*graph));
  auto result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({3}))}}));
  require(read_bits(result.results.at("out"), {0}) == 0 &&
              read_bits(result.results.at("out"), {1}) == 0x3f000000 &&
              read_bits(result.results.at("out"), {2}) == 0x3f800000,
          "Float32 shaper uses Result schema hints for generated sequence "
          "constants");
  WorkflowDocument baked;
  baked.inputs = {document.inputs[1], document.inputs[2]};
  auto start = numeric::sequence_input(baked.inputs[0]);
  auto end = numeric::sequence_input(baked.inputs[1]);
  auto table = numeric::bake_lut1d_expression(baked, "x", start, end, 3);
  require(table.ok() && baked.nodes.size() == 1,
          "LUT baking endpoint validation consumes scalar Result schema hints");
  end.result_schema.reset();
  auto failed = numeric::bake_lut1d_expression(baked, "x", start, end, 3);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              baked.nodes.size() == 1,
          "invalid LUT endpoint hint leaves authoring graph unchanged");
}
void sequence_boundaries() {
  Driver d;
  auto lo = d.source({ElementType::Float64, {1}},
                     {UINT64_C(0xffefffffffffffff)}, {1, {0}});
  auto hi = d.source({ElementType::Float64, {1}},
                     {UINT64_C(0x7fefffffffffffff)}, {1, {0}});
  const auto parameters =
      std::map<std::string, ParameterValue>{{"count", static_cast<int64_t>(2)},
                                            {"dtype", std::string("float64")}};
  auto values = take(d.run("numeric.linspace_strict", {lo, hi}, {}, parameters))
                    .results.at("out");
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed =
      d.run("numeric.linspace_strict", {lo, hi}, {}, parameters, {}, "axis");
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().detail.scope == FailureScope::Run &&
              failed.status().message == "axis step overflow" &&
              d.root.statistics().live[ResourceKind::Payload] == payload &&
              read_bits(values, {1}) == UINT64_C(0x7fefffffffffffff),
          "axis-only overflow does not invalidate separately successful "
          "sequence values");
  auto point = take(Footprint::from_regions({3}, {Region({{1, 1}})}));
  failed = d.run(
      "numeric.linspace_strict", {lo, hi}, point,
      {{"count", static_cast<int64_t>(3)}, {"dtype", std::string("float32")}});
  require(
      !failed.ok() &&
          failed.status().reason == FailureReason::ArithmeticOverflow &&
          failed.status().message == "sequence value overflow index=0",
      "overflowing unrequested sequence value fails a nonempty Whole query");
  auto nan = d.source({ElementType::Float64, {1}},
                      {UINT64_C(0x7ff0000000000001)}, {1, {0}});
  failed = d.run("numeric.linspace_strict", {nan, hi}, {}, parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().message == "nonfinite start",
          "generic nonfinite sequence input is checked after complete input "
          "preparation");
  auto empty = take(d.run("numeric.linspace_strict", {nan, hi},
                          take(Footprint::none({2})), parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty sequence bypasses nonfinite inputs");
  Driver limited(5000);
  auto start = limited.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto end =
      limited.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  const auto live = limited.root.statistics().live[ResourceKind::Payload];
  failed = limited.run("numeric.linspace_strict", {start, end}, {}, parameters);
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] == live,
          "exact sequence work admission failure releases output and "
          "arithmetic scratch");
  CancellationSource cancellation;
  cancellation.cancel();
  failed = d.run("numeric.linspace_strict", {lo, hi}, {}, parameters,
                 cancellation.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "pre-cancellation prevents sequence arithmetic");
}
void indexing_workflows() {
  Driver d;
  auto base = d.source({ElementType::Int64, {3}}, {10, 20, 30}, {1, {8}});
  auto indices = d.source({ElementType::Int64, {3}}, {2, 0, 2}, {17, {-8}});
  auto updates = d.source({ElementType::Int64, {3}}, {3, 2, 1}, {17, {-8}});
  const uint64_t expected[][3] = {{30, 10, 30},
                                  {2, 20, 3},
                                  {12, 20, 34},
                                  {2, 20, 1},
                                  {10, 20, 30}};
  const char* names[] = {"gather", "scatter_replace", "scatter_sum",
                         "scatter_minimum", "scatter_maximum"};
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata input_metadata, index_metadata;
    input_metadata.result_schema =
        std::make_shared<SchemaTemplate>(base.schema());
    index_metadata.result_schema =
        std::make_shared<SchemaTemplate>(indices.schema());
    auto available = d.registry->resolve_traits(
        std::string("array.gather") + suffix, {input_metadata, index_metadata},
        {{"axis", static_cast<int64_t>(0)}});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "indexing incompatible profile reports BackendUnavailable");
      continue;
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
      const auto key = std::string("array.") + names[kind] + suffix;
      auto inputs = std::vector<ResultRef>{base, indices};
      if (kind)
        inputs.push_back(updates);
      auto result =
          take(d.run(key, inputs, {}, {{"axis", static_cast<int64_t>(0)}}));
      for (uint64_t at = 0; at < 3; ++at)
        require(read_bits(result.results.at("out"), {at}) == expected[kind][at],
                "gather/scatter preserve index order and stable contributors");
      for (unsigned port = 0; port < inputs.size(); ++port) {
        auto edit = take(Footprint::from_regions({3}, {Region({{1, 1}})}));
        require(take(result.dependencies.potential_dirty(
                         "input" + std::to_string(port), edit, 1))
                        .at("out") == take(Footprint::all({3})),
                "every active indexing input has Whole source support");
      }
    }
  }
  auto matrix = d.source({ElementType::UInt8, {2, 3}}, {10, 11, 12, 20, 21, 22},
                         {1, {3, 1}});
  auto gathered = take(d.run("array.gather_strict", {matrix, indices}, {},
                             {{"axis", static_cast<int64_t>(1)}}))
                      .results.at("out");
  const uint64_t expected_matrix[] = {12, 10, 12, 22, 20, 22};
  for (uint64_t at = 0; at < 6; ++at)
    require(read_bits(gathered, {at / 3, at % 3}) == expected_matrix[at],
            "gather copies complete nonleading-axis slices");
  auto zero_indices = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  auto one = d.source({ElementType::Float64, {1}},
                      {double_bits(std::ldexp(1.0, 100))}, {1, {0}});
  auto cancellation =
      d.source({ElementType::Float64, {2}},
               {double_bits(-std::ldexp(1.0, 100)), double_bits(3)}, {1, {8}});
  auto exact =
      take(d.run("array.scatter_sum_strict", {one, zero_indices, cancellation},
                 {}, {{"axis", static_cast<int64_t>(0)}}))
          .results.at("out");
  require(read_bits(exact, {0}) == double_bits(3),
          "scatter sum retains exact residual through wide cancellation");
  auto nan_base = d.source(
      {ElementType::Float64, {2}},
      {UINT64_C(0xfff0000000000003), UINT64_C(0x7ff0000000000011)}, {1, {8}});
  auto nan_updates = d.source(
      {ElementType::Float64, {2}},
      {UINT64_C(0x7ff0000000000021), UINT64_C(0xfff0000000000007)}, {1, {8}});
  auto copied = take(d.run("array.scatter_replace_strict",
                           {nan_base, zero_indices, nan_updates}, {},
                           {{"axis", static_cast<int64_t>(0)}}))
                    .results.at("out");
  require(read_bits(copied, {0}) == UINT64_C(0xfff0000000000007) &&
              read_bits(copied, {1}) == UINT64_C(0x7ff0000000000011),
          "replace and no-hit scatter preserve raw signaling NaN bits");
  for (const auto* name : {"sum", "minimum", "maximum"}) {
    copied = take(d.run(std::string("array.scatter_") + name + "_strict",
                        {nan_base, zero_indices, nan_updates}, {},
                        {{"axis", static_cast<int64_t>(0)}}))
                 .results.at("out");
    require(read_bits(copied, {0}) == UINT64_C(0xfff8000000000003) &&
                read_bits(copied, {1}) == UINT64_C(0x7ff0000000000011),
            "aggregate hits quiet base-first NaN while no-hit remains raw");
  }
  auto largest = d.source({ElementType::Int64, {1}}, {INT64_MAX}, {1, {0}});
  auto offsets = d.source({ElementType::Int64, {2}}, {1, UINT64_MAX}, {1, {8}});
  copied =
      take(d.run("array.scatter_sum_strict", {largest, zero_indices, offsets},
                 {}, {{"axis", static_cast<int64_t>(0)}}))
          .results.at("out");
  require(read_bits(copied, {0}) == INT64_MAX,
          "integer scatter checks final exact sum rather than intermediate "
          "overflow");
}
void concatenate_views() {
  Driver d;
  auto buffer = take(d.root.allocator().allocate(65));
  for (uint64_t index = 0; index < 8; ++index) {
    const auto bits = double_bits(index + 1);
    std::memcpy(buffer.data() + 1 + index * 8, &bits, 8);
  }
  auto common = std::move(buffer).freeze();
  std::weak_ptr<const CpuStorage> weak = common;
  const auto source = [&](std::vector<uint64_t> shape, uint64_t offset,
                          std::vector<int64_t> strides, bool fragmented) {
    SchemaTemplate schema;
    schema.id = "test.concatenate";
    ResultTensorSpec member;
    member.key = "samples";
    member.descriptor = {ElementType::Float64, shape};
    schema.tensors.push_back(member);
    auto builder =
        take(ResultBuilder::start(d.root, schema, "concatenate.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
                .ok(),
            "concatenate source descriptor");
    auto relation = take(
        ResultRelation::cartesian(d.root, shape[0] * shape[1], {0, 1, 0, 0}));
    if (fragmented) {
      for (uint64_t row = 0; row < shape[0]; ++row)
        require(builder
                    .publish_tensor(
                        0, Region({{row, 1}, {0, shape[1]}}),
                        {offset + row * 16, {INT64_MIN, strides[1]}, {row, 0}},
                        common, relation, {true, true, true, true})
                    .ok(),
                "concatenate singleton row fragment");
    } else {
      require(builder
                  .publish_tensor(0, Region::whole(shape), {offset, strides},
                                  common, relation, {true, true, true, true})
                  .ok(),
              "concatenate affine source");
    }
    return take(builder.seal());
  };
  auto first = source({2, 2}, 1, {16, 8}, true);
  auto second = source({2, 2}, 33, {16, 8}, false);
  auto original = take(
      first.acquire_tensor(take(first.descriptor()), 0, Region::whole({2, 2})));
  const auto pointer = take(original.row_run({0, 0})).data;
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  ResultRef joined;
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(first.schema());
    const auto key = std::string("array.concatenate") + suffix;
    auto available = d.registry->resolve_traits(
        key, {metadata, metadata},
        {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "concatenate profile availability");
      continue;
    }
    auto result = take(d.run(
        key, {first, second}, {},
        {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}}));
    joined = result.results.at("out");
    auto window = take(joined.acquire_tensor(take(joined.descriptor()), 0,
                                             Region::whole({4, 2})));
    require(window.storage_owner_token() == common.get() &&
                take(window.row_run({0, 0})).data == pointer &&
                d.root.statistics().live[ResourceKind::Payload] == payload,
            "concatenate View joins complete singleton fragments without "
            "payload copying");
    for (uint64_t at = 0; at < 8; ++at)
      require(read_bits(joined, {at / 2, at % 2}) == double_bits(at + 1),
              "concatenate prefix mapping preserves all values");
    auto edit =
        take(Footprint::from_regions({2, 2}, {Region({{1, 1}, {1, 1}})}));
    require(take(result.dependencies.potential_dirty("input1", edit, 1))
                    .at("out") == take(Footprint::all({4, 2})),
            "concatenate View still retains Whole source support");
  }
  auto singleton_first = source({1, 2}, 1, {INT64_MIN, 8}, false);
  auto singleton_second = source({1, 2}, 17, {INT64_MIN, 8}, false);
  auto inferred = take(d.run("array.concatenate_strict",
                             {singleton_first, singleton_second}, {},
                             {{"axis", static_cast<int64_t>(0)},
                              {"layout", std::string("view")}}))
                      .results.at("out");
  require(read_bits(inferred, {1, 1}) == double_bits(4),
          "all singleton concat axes infer stride between port anchors");
  auto negative_first = source({2, 2}, 57, {-16, -8}, false);
  auto negative_second = source({2, 2}, 25, {-16, -8}, false);
  auto reverse = take(d.run("array.concatenate_strict",
                            {negative_first, negative_second}, {},
                            {{"axis", static_cast<int64_t>(0)},
                             {"layout", std::string("view")}}))
                     .results.at("out");
  require(read_bits(reverse, {0, 0}) == double_bits(8) &&
              read_bits(reverse, {3, 1}) == double_bits(1),
          "concatenate View preserves negative global strides");
  auto reordered = d.run(
      "array.concatenate_strict", {second, first},
      take(Footprint::from_regions({4, 2}, {Region({{0, 1}, {0, 1}})})),
      {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}});
  require(!reordered.ok() &&
              reordered.status().code == ErrorCode::InvalidArgument &&
              reordered.status().reason == FailureReason::InvalidDomain &&
              reordered.status().detail.scope == FailureScope::Run,
          "partial Q cannot relax global contiguous concat View proof");
  auto dense = take(d.run("array.concatenate_strict", {second, first}, {},
                          {{"axis", static_cast<int64_t>(0)},
                           {"layout", std::string("dense")}}))
                   .results.at("out");
  require(read_bits(dense, {0, 0}) == double_bits(5) &&
              read_bits(dense, {3, 1}) == double_bits(4),
          "Dense explicitly copies ordered unrelated-to-prefix inputs");
  auto unrelated = d.source({ElementType::Float64, {2, 2}}, {0}, {1, {0, 0}});
  auto failed = d.run(
      "array.concatenate_strict", {first, unrelated}, {},
      {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}});
  require(!failed.ok() && failed.status().message.find("ViewUnavailable") !=
                              std::string::npos,
          "concatenate View rejects independent physical owners");
  auto empty = take(d.run(
      "array.concatenate_strict", {first, unrelated},
      take(Footprint::none({4, 2})),
      {{"axis", static_cast<int64_t>(0)}, {"layout", std::string("view")}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty concat does not inspect physical viewability");
  original = {};
  first = {};
  second = {};
  singleton_first = {};
  singleton_second = {};
  inferred = {};
  negative_first = {};
  negative_second = {};
  reverse = {};
  dense = {};
  common.reset();
  d.context.reset();
  require(!weak.expired() && read_bits(joined, {3, 1}) == double_bits(8),
          "concat output owns source schema and storage after input/context "
          "retirement");
  joined = {};
  require(weak.expired(), "last concat view releases its source backing");
}
void concatenate_many_ports() {
  for (uint64_t ports : {65, 256}) {
    Driver d;
    auto scalar =
        d.source({ElementType::Float64, {1}}, {double_bits(7)}, {1, {0}});
    std::vector<ResultRef> inputs(ports, scalar);
    for (const auto* layout : {"dense", "view"}) {
      const auto payload = d.root.statistics().live[ResourceKind::Payload];
      auto result = take(d.run("array.concatenate_strict", inputs, {},
                               {{"axis", static_cast<int64_t>(0)},
                                {"layout", std::string(layout)}}));
      require(read_bits(result.results.at("out"), {0}) == double_bits(7) &&
                  read_bits(result.results.at("out"), {ports - 1}) ==
                      double_bits(7),
              "concat preserves every legal repeated port across bounded Need "
              "envelopes");
      if (std::string(layout) == "view") {
        auto source = take(scalar.acquire_tensor(take(scalar.descriptor()), 0,
                                                 Region::whole({1})));
        auto output = take(result.results.at("out").acquire_tensor(
            take(result.results.at("out").descriptor()), 0,
            Region::whole({ports})));
        require(
            source.storage_owner_token() == output.storage_owner_token() &&
                take(source.row_run({0})).data ==
                    take(output.row_run({ports - 1})).data &&
                d.root.statistics().live[ResourceKind::Payload] == payload,
            "many-port constant affine concatenation remains a zero-copy view");
      }
    }
  }
}
void indexing_boundaries() {
  Driver d;
  auto base = d.source({ElementType::Int64, {3}}, {10, 20, 30}, {1, {8}});
  auto invalid = d.source({ElementType::Int64, {2}}, {0, 3}, {1, {8}});
  auto failed = d.run("array.gather_strict", {base, invalid},
                      take(Footprint::from_regions({2}, {Region({{0, 1}})})),
                      {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom &&
              failed.status().message.find("position=1 index=3 extent=3") !=
                  std::string::npos,
          "an index outside the selected output fails the complete run");
  auto empty = take(d.run("array.gather_strict", {base, invalid},
                          take(Footprint::none({2})),
                          {{"axis", static_cast<int64_t>(0)}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty gather skips invalid index payload");
  auto targets = d.source({ElementType::Int64, {1}}, {1}, {1, {0}});
  auto overflows =
      d.source({ElementType::Int64, {2}}, {0, INT64_MAX}, {1, {8}});
  auto update = d.source({ElementType::Int64, {1}}, {1}, {1, {0}});
  const auto live = d.root.statistics().live[ResourceKind::Payload];
  failed = d.run("array.scatter_sum_strict", {overflows, targets, update},
                 take(Footprint::from_regions({2}, {Region({{0, 1}})})),
                 {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().detail.scope == FailureScope::Run &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().message.find("output=[1,") != std::string::npos &&
              d.root.statistics().live[ResourceKind::Payload] == live,
          "late exact scatter overflow outside Q rolls back output with Run "
          "provenance");
  auto facet = take(encode_semantic(coverage_semantics()));
  auto typed = d.source({ElementType::Float32, {2, 2}}, {0, 0, 0, 0},
                        {1, {8, 4}}, {facet});
  auto bad_updates = d.source({ElementType::Float32, {2, 2}},
                              {0, 0, 0, 0x40000000}, {1, {8, 4}}, {facet});
  auto repeated = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  failed = d.run("array.scatter_replace_strict", {typed, repeated, bad_updates},
                 {}, {{"axis", static_cast<int64_t>(1)}});
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.input_id == 13,
          "overwritten typed updates still undergo complete Whole validation");
  CancellationSource stop;
  stop.cancel();
  failed = d.run("array.gather_strict", {base, invalid}, {},
                 {{"axis", static_cast<int64_t>(0)}}, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "indexing pre-cancellation preserves cancellation before bad index");
  Driver limited(3000);
  auto large = limited.source({ElementType::Int64, {64}}, {1}, {1, {0}});
  auto many = limited.source({ElementType::Int64, {64}}, {0}, {1, {0}});
  auto payload = limited.root.statistics().live[ResourceKind::Payload];
  failed = limited.run("array.scatter_sum_strict", {large, many, large}, {},
                       {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] == payload,
          "radix/aggregate work failure releases indexing scratch and output");
}
void joined_resource_failure() {
  ResourceBudget root;
  OcioConfigSnapshot snapshot;
  snapshot.config = {'t'};
  snapshot.spaces = {{"linear", "scene"}};
  snapshot.build_identity = "test-pinned";
  snapshot.settings = "reference";
  auto config = take(OcioConfigResource::import(snapshot, root));
  auto resources = take(ResourceBindings::create({}, {config}, root));
  auto source =
      take(Value::create({ElementType::Float64, {2, 2}}, Region::whole({2, 2}),
                         {0, {16, 8}}, std::vector<uint8_t>(32)));
  std::vector<Value> parts{take(source.view(Region({{0, 1}, {0, 2}}))),
                           take(source.view(Region({{1, 1}, {0, 2}})))};
  SemanticDescriptor semantics;
  semantics.channels = {{"value", "value", "dimensionless"}};
  const auto facet = take(encode_semantic(semantics));
  auto scratch = take(root.reserve(ResourceCapacity::host(4096, 4096)));
  require(root.consume({UINT64_MAX - root.statistics().issued.work}).ok(),
          "exhaust retained resource root");
  const auto live = root.statistics().live.values;
  auto joined = input_internal::join_affine_view(
      source.descriptor(), source.region(), parts, FootprintLimits{}, {facet},
      resources);
  require(!joined.ok() &&
              joined.status().code == ErrorCode::ResourceExhausted &&
              joined.status().reason == FailureReason::WorkLimit &&
              root.statistics().live.values == live,
          "affine join preserves resource selection failure instead of view "
          "fallback");
}
void paged_layout_copy() {
  for (const auto* layout : {"auto", "dense"}) {
    Driver d;
    SchemaTemplate schema;
    schema.id = "test.paged.numeric";
    ResultTensorSpec slot;
    slot.key = "samples";
    slot.descriptor = {ElementType::Float64, {1, 1}};
    slot.batch_axes = {128};
    slot.layout.spatial = true;
    slot.layout.channel_axis.reset();
    schema.tensors.push_back(slot);
    auto builder = take(ResultBuilder::start(d.root, schema, "paged.layouts"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
                .ok(),
            "paged layout descriptor");
    double values[128];
    for (unsigned i = 0; i < 128; ++i)
      values[i] = static_cast<double>(i + 1);
    require(builder
                .publish_tensor(
                    0, Region::whole(slot.sample_shape()),
                    ByteView(reinterpret_cast<const uint8_t*>(values),
                             sizeof(values)),
                    take(ResultRelation::cartesian(d.root, 128, {0, 1, 0, 0})),
                    {true, true, true, true})
                .ok(),
            "paged layout input");
    auto source = take(builder.seal());
    const auto before = d.root.statistics().issued.work;
    auto output = take(d.run("array.reshape_strict", {source}, {},
                             {{"shape", std::string("16,8")},
                              {"layout", std::string(layout)}}))
                      .results.at("out");
    const auto work = d.root.statistics().issued.work - before;
    require(work < 20000,
            "paged Whole copy reuses one window instead of quadratic directory "
            "scans");
    for (uint64_t i = 0; i < 128; ++i)
      require(read_bits(output, {i / 8, i % 8}) == double_bits(i + 1),
              "paged Auto/Dense reshape preserves every batch value");
  }
}
struct BoundedLayoutPhase {
  ResultContinuation inner;
  unsigned samples = 0;
  explicit BoundedLayoutPhase(ResultContinuation continuation)
      : inner(std::move(continuation)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      auto status = phase.consume_work(amount);
      if (status.ok() && amount == 13 && ++samples == 2) {
        // The first sample has already acquired the full paged window. Leave
        // two units immediately before the second sample's owning row lookup.
        status = phase.resources.consume(
            {UINT64_MAX - phase.resources.statistics().issued.work - 2});
      }
      return status;
    };
    return inner.poll(bounded);
  }
};
void paged_layout_failure() {
  Driver d;
  const auto original_registry = d.registry;
  OperationDefinition definition;
  definition.key = "array.reshape_strict";
  definition.traits = take(original_registry->find_traits(definition.key));
  definition.traits.requires_metadata_specialization = false;
  auto& output_schema = *definition.traits.outputs[0].result_schema;
  output_schema.tensors[0].descriptor = {ElementType::Float64, {16, 8}};
  definition.traits.outputs[0].continuation_bytes += sizeof(BoundedLayoutPhase);
  definition.start_result = [original_registry](const auto& query,
                                                const auto& allocator) {
    auto forwarded = query;
    forwarded.prepared.reset();
    auto inner = original_registry->start_result("array.reshape_strict",
                                                 forwarded, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<BoundedLayoutPhase>(allocator,
                                                        inner.take_value());
  };
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->register_operation(std::move(definition)).ok(),
          "bounded layout registration");
  require(registry->freeze().ok(), "bounded layout registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  SchemaTemplate schema;
  schema.id = "test.paged.failure";
  ResultTensorSpec slot;
  slot.key = "samples";
  slot.descriptor = {ElementType::Float64, {1, 1}};
  slot.batch_axes = {128};
  slot.layout.spatial = true;
  slot.layout.channel_axis.reset();
  schema.tensors.push_back(slot);
  auto builder = take(ResultBuilder::start(d.root, schema, "paged.failure"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
              .ok(),
          "paged failure descriptor");
  double values[128]{};
  require(builder
              .publish_tensor(
                  0, Region::whole(slot.sample_shape()),
                  ByteView(reinterpret_cast<const uint8_t*>(values),
                           sizeof(values)),
                  take(ResultRelation::cartesian(d.root, 128, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "paged failure source");
  auto source = take(builder.seal());
  const auto live = d.root.statistics().live[ResourceKind::Payload];
  auto failed =
      d.run("array.reshape_strict", {source}, {},
            {{"shape", std::string("16,8")}, {"layout", std::string("dense")}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              failed.status().detail.scope == FailureScope::Group &&
              !failed.status().detail.atom &&
              d.root.statistics().live[ResourceKind::Payload] == live,
          "owning paged row failure preserves complete cause and rolls back "
          "dense output");
}
struct FailingProducer {
  Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed, "must stay lazy"});
  }
};
void ordering_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.quantile.q";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  auto required_source = producer;
  required_source.key = "test.quantile.source";
  required_source.traits.outputs[0].result_schema->tensors[0].descriptor.shape =
      {2};
  require(registry->register_operation(std::move(required_source)).ok(),
          "quantile required producer registration");
  require(registry->register_operation(std::move(producer)).ok(),
          "quantile lazy producer registration");
  require(registry->freeze().ok(), "quantile projection registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto input = d.source({ElementType::Int64, {2, 1}},
                        {INT64_MAX, static_cast<uint64_t>(-2)}, {1, {8, 0}});
  auto q = d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  auto prepared = d.prepare(
      "numeric.quantile_strict", {input, q},
      {{"axis", static_cast<int64_t>(1)}, {"dtype", std::string("float32")}});
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(), {3, "test.quantile.q", {}, {}});
  document.nodes.back().inputs[1] = WorkflowNodeOutput{3, "value"};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto singleton = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({2, 1}))}}));
  require(*starts == 0 &&
              read_bits(singleton.results.at("out"), {0, 0}) == 0x5f000000 &&
              read_bits(singleton.results.at("out"), {1, 0}) == 0xc0000000,
          "singleton quantile excludes failing q and exactly converts Int64");
  for (const auto& descriptor : {ValueDescriptor{ElementType::Int64, {1}},
                                 ValueDescriptor{ElementType::Float64, {2}}}) {
    auto bad = d.source(
        descriptor, {0, 0},
        {1,
         {static_cast<int64_t>(Value::element_size(descriptor.element_type))}});
    OperationMetadata source_metadata, q_metadata;
    source_metadata.result_schema =
        std::make_shared<SchemaTemplate>(input.schema());
    q_metadata.result_schema = std::make_shared<SchemaTemplate>(bad.schema());
    auto rejected = registry->resolve_traits(
        "numeric.quantile_strict", {source_metadata, q_metadata},
        {{"axis", static_cast<int64_t>(1)}, {"dtype", std::string("float32")}});
    require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
            "excluded probability still receives complete static validation");
  }
  auto two = d.source({ElementType::Float64, {2}},
                      {double_bits(1), double_bits(2)}, {1, {8}});
  prepared = d.prepare(
      "numeric.quantile_strict", {two, q},
      {{"axis", static_cast<int64_t>(0)}, {"dtype", std::string("float64")}});
  document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(), {3, "test.quantile.q", {}, {}});
  document.nodes.back().inputs[0] = WorkflowNodeOutput{3, "value"};
  // The computed source is scalar, so this remains a valid singleton plan.
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  auto before = d.root.statistics().live[ResourceKind::Payload];
  auto empty = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::none({1}))}}));
  require(
      *starts == 0 &&
          d.root.statistics().live[ResourceKind::Payload] == before &&
          take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
      "Empty quantile neither evaluates source nor allocates output payload");
  auto invalid = d.source({ElementType::Float64, {1}},
                          {UINT64_C(0x7ff0000000000001)}, {1, {8}});
  prepared = d.prepare(
      "numeric.quantile_strict", {two, invalid},
      {{"axis", static_cast<int64_t>(0)}, {"dtype", std::string("float64")}});
  document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.quantile.source", {}, {}});
  document.nodes.back().inputs[0] = WorkflowNodeOutput{3, "value"};
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  auto failed = d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1}))}});
  require(!failed.ok() && *starts == 1 &&
              failed.status().message == "must stay lazy",
          "required source producer failure preserves its cause before q "
          "validation");
}
void reduction_count_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.count.source";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {1048576, 1048576}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "count source registration");
  require(registry->freeze().ok(), "count source registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  WorkflowDocument document;
  document.nodes = {{1, "test.count.source", {}, {}},
                    {2,
                     "numeric.reduce_count_strict",
                     {WorkflowNodeOutput{1, "value"}},
                     {{"axes", std::string("1")}}}};
  document.outputs = {{"out", 2, "values"}};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan)),
      {{"out", take(Footprint::all({1048576, 1}))}}));
  auto count = result.results.at("out");
  require(*starts == 0 && count.association().empty() &&
              read_bits(count, {0, 0}) == 1048576 &&
              read_bits(count, {1048575, 0}) == 1048576 &&
              d.root.statistics().live[ResourceKind::Payload] == payload + 8 &&
              take(result.dependencies.source_observations()).empty(),
          "metadata-only count leaves huge failing producer unstarted and owns "
          "exactly eight bytes");
  auto window = take(count.acquire_tensor(take(count.descriptor()), 0,
                                          Region::whole({1048576, 1})));
  require(take(window.row_run({0, 0})).data ==
              take(window.row_run({1048575, 0})).data,
          "count publishes a complete zero-stride logical output");
  result = {};
  window = {};
  d.context.reset();
  require(read_bits(count, {1048575, 0}) == 1048576,
          "count backing survives source graph and context retirement");
  count = {};
  require(d.root.statistics().live[ResourceKind::Payload] == payload,
          "last count owner releases independent backing");
}
void bezier_workflows() {
  Driver d;
  // Logical [K,2] combines a batch axis with a cell, using reversed rows.
  auto anchors = d.source({ElementType::Float32, {2}}, {float_bits(1), 0, 0, 0},
                          {9, {-8, 4}}, {}, {2}, nullptr, "custom.bezier", 7);
  auto handles = d.source({ElementType::Float64, {1, 1, 2}},
                          {0, double_bits(1)}, {1, {0, 0, 8}});
  auto start = d.source({ElementType::Float32, {1}}, {float_bits(.125F)},
                        {1, {INT64_MIN}});
  auto end =
      d.source({ElementType::Float64, {1}}, {double_bits(.375)}, {1, {0}});
  const uint64_t golden[2][3] = {
      {0x3fdd413cccfe7799, 0x3fe0000000000000, 0x3fde6238502484ba},
      {0x3eea09e6, 0x3f000000, 0x3ef311c3}};
  ResultRef retained;
  for (auto dtype : {ElementType::Float64, ElementType::Float32})
    for (auto profile :
         {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
          CpuNumericProfile::X86Avx2}) {
      auto node = take(numeric::sample_bezier_function_node(
          7, WorkflowInputReference{11}, WorkflowInputReference{12},
          WorkflowInputReference{13}, WorkflowInputReference{14}, 2, 3, dtype,
          numeric::BezierDomain::Reject, profile));
      std::vector<OperationMetadata> metadata;
      for (const auto& input : {anchors, handles, start, end}) {
        OperationMetadata item;
        item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
        metadata.push_back(std::move(item));
      }
      auto available =
          d.registry->resolve_traits(node.operation, metadata, node.parameters);
      if (!available.ok()) {
        require(profile != CpuNumericProfile::Strict &&
                    available.status().code == ErrorCode::BackendUnavailable,
                "unavailable Bezier profile is explicit");
        continue;
      }
      auto result =
          take(d.run(node.operation, {anchors, handles, start, end},
                     take(Footprint::from_regions({3}, {Region({{1, 1}})})),
                     node.parameters));
      retained = result.results.at("out");
      for (unsigned i = 0; i < 3; ++i)
        require(read_bits(retained, {i}) ==
                    golden[dtype == ElementType::Float32][i],
                "Bezier Bx=t^2 inverts x and rounds 2*sqrt(x)*(1-sqrt(x))");
      require(retained.schema().id == "photospider.tensor" &&
                  retained.schema().tensors[0].key == "samples" &&
                  retained.schema().tensors[0].facets.empty(),
              "Bezier emits the generic Result tensor schema");
      auto changed =
          take(Footprint::from_regions({2, 2}, {Region({{1, 1}, {0, 1}})}));
      require(take(result.dependencies.potential_dirty("input0", changed, 4))
                      .at("out") ==
                  take(Footprint::from_regions({3}, {Region({{1, 1}})})),
              "Bezier Whole validation support covers anchors outside Q");
    }
  auto cubic =
      d.source({ElementType::Float64, {1, 2, 2}},
               {0, double_bits(.25), double_bits(-1), double_bits(-.25)},
               {1, {32, 16, 8}});
  auto cubic_anchors =
      d.source({ElementType::Float64, {2, 2}},
               {0, 0, double_bits(1), double_bits(1)}, {1, {16, 8}});
  auto node = take(numeric::sample_bezier_function_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, WorkflowInputReference{14}, 3, 1));
  auto sampled = take(d.run(node.operation, {cubic_anchors, cubic, start, end},
                            {}, node.parameters))
                     .results.at("out");
  // Bx=t^3, By controls 0,.25,.75,1: x=.125 -> t=.5 -> y=.5.
  require(read_bits(sampled, {0}) == double_bits(.5),
          "cubic function uses mathematical x inverse instead of t=x");
  const uint64_t parametric64[2][4] = {
      {0x3fa16872b020c49b, 0xbf8cac083126e985, 0x3fa9999999999995,
       0x3fe072b020c49ba6},
      {0x3f9f212d77318fc3, 0x3fc07c84b5dcc63f, 0x3fd999999999999a,
       0x3fe8a0902de00d1b}};
  const uint64_t parametric32[2][4] = {
      {0x3d0b4396, 0xbc656042, 0x3d4ccccd, 0x3f039581},
      {0x3cf9096c, 0x3e03e426, 0x3ecccccd, 0x3f450481}};
  auto scalar_anchors =
      d.source({ElementType::Float64, {1}}, {double_bits(.1), double_bits(.7)},
               {1, {8, 0}}, {}, {2});
  auto indices = d.source({ElementType::Int64, {4}}, {0}, {1, {0}});
  auto t = d.source(
      {ElementType::Float64, {4}},
      {double_bits(.9), double_bits(.5), double_bits(.3), double_bits(.1)},
      {25, {-8}});
  for (unsigned degree : {2U, 3U}) {
    auto offsets =
        d.source({ElementType::Float64, {1, degree - 1, 1}},
                 {double_bits(-.4), double_bits(.4)}, {1, {0, 8, 0}});
    for (auto dtype : {ElementType::Float64, ElementType::Float32})
      for (auto profile :
           {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
            CpuNumericProfile::X86Avx2}) {
        auto evaluate = take(numeric::evaluate_bezier_node(
            7, WorkflowInputReference{11}, WorkflowInputReference{12},
            WorkflowInputReference{13}, WorkflowInputReference{14}, degree,
            dtype, profile));
        std::vector<OperationMetadata> metadata;
        for (const auto& input : {scalar_anchors, offsets, indices, t}) {
          OperationMetadata item;
          item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
          metadata.push_back(std::move(item));
        }
        auto available = d.registry->resolve_traits(
            evaluate.operation, metadata, evaluate.parameters);
        if (!available.ok()) {
          require(profile != CpuNumericProfile::Strict &&
                      available.status().code == ErrorCode::BackendUnavailable,
                  "unavailable parametric profile is explicit");
          continue;
        }
        std::fenv_t environment;
        require(std::fegetenv(&environment) == 0, "save Bezier fenv");
        std::fesetround(FE_UPWARD);
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        auto result = d.run(
            evaluate.operation, {scalar_anchors, offsets, indices, t},
            take(Footprint::from_regions({4, 1}, {Region({{3, 1}, {0, 1}})})),
            evaluate.parameters);
        const bool preserved = std::fegetround() == FE_UPWARD &&
                               std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
        std::fesetenv(&environment);
        require(preserved, "parametric evaluation preserves caller fenv");
        auto output = take(std::move(result)).results.at("out");
        require(output.schema().tensors[0].sample_shape() ==
                    std::vector<uint64_t>{4, 1},
                "parametric logical batches preserve D=1 output axis");
        for (unsigned i = 0; i < 4; ++i)
          require(
              read_bits(output, {i, 0}) == (dtype == ElementType::Float64
                                                ? parametric64[degree - 2][i]
                                                : parametric32[degree - 2][i]),
              "parametric exact Fraction Bernstein oracle after RN64 "
              "reconstruction");
      }
  }
  d.context.reset();
  require(read_bits(sampled, {0}) == double_bits(.5) &&
              read_bits(retained, {1}) == float_bits(.5F),
          "Bezier results survive source context retirement");
}
void bezier_boundaries() {
  Driver d;
  auto anchors = d.source({ElementType::Float64, {2, 2}},
                          {0, 0, double_bits(1), double_bits(1)}, {1, {16, 8}});
  auto crossing = d.source({ElementType::Float64, {1, 2, 2}},
                           {double_bits(.75), double_bits(.75),
                            double_bits(-.75), double_bits(-.75)},
                           {1, {32, 16, 8}});
  auto half =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  auto node = take(numeric::sample_bezier_function_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, WorkflowInputReference{14}, 3, 1));
  auto output = take(d.run(node.operation, {anchors, crossing, half, half}, {},
                           node.parameters))
                    .results.at("out");
  require(read_bits(output, {0}) == double_bits(.5),
          "crossing controls with nonnegative derivative are legal");
  auto backward =
      d.source({ElementType::Float64, {1, 2, 2}},
               {double_bits(2), 0, double_bits(-2), 0}, {1, {32, 16, 8}});
  auto failed = d.run(node.operation, {anchors, backward, half, half}, {},
                      node.parameters);
  require(!failed.ok() &&
              failed.status().message.find("backward Bezier segment=0") !=
                  std::string::npos &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom,
          "backward x derivative is rejected with Run diagnostics");
  auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto nan_handles =
      d.source({ElementType::Float64, {1, 1, 2}},
               {double_bits(.5), 0x7ff8000000000001}, {1, {0, 0, 8}});
  node.parameters["degree"] = int64_t{2};
  output = take(d.run(node.operation, {anchors, nan_handles, zero, half}, {},
                      node.parameters))
               .results.at("out");
  require(read_bits(output, {0}) == 0,
          "Bezier knot selection ignores unused generic handle y NaN");
  auto outside =
      d.source({ElementType::Float64, {1}}, {double_bits(-1)}, {1, {0}});
  failed = d.run(node.operation, {anchors, nan_handles, outside, half}, {},
                 node.parameters);
  require(!failed.ok() && failed.status().message.find(
                              "outside anchor domain") != std::string::npos,
          "Bezier query rejection precedes unused y arithmetic");
  node.parameters["out_of_domain"] = std::string("clamp");
  output = take(d.run(node.operation, {anchors, nan_handles, outside, half}, {},
                      node.parameters))
               .results.at("out");
  require(read_bits(output, {0}) == 0, "Bezier clamps to anchor y");
  auto bad_typed = d.source({ElementType::Float32, {2, 2}},
                            {0, 0, float_bits(1), float_bits(2)}, {1, {8, 4}},
                            {take(encode_semantic(coverage_semantics()))});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  failed = d.run(node.operation, {bad_typed, nan_handles, zero, half}, {},
                 node.parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 11 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "typed Bezier validates unused anchor y and rolls back publication");
  auto empty = take(d.run(node.operation, {bad_typed, nan_handles, zero, half},
                          take(Footprint::none({1})), node.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty Bezier skips invalid typed payload");
  auto evaluate = take(numeric::evaluate_bezier_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, WorkflowInputReference{14}, 2));
  auto all_nan = d.source({ElementType::Float64, {1, 1, 2}},
                          {0x7ff8000000000001}, {1, {0, 0, 0}});
  auto index = d.source({ElementType::Int64, {1}}, {0}, {1, {INT64_MIN}});
  auto negative_zero =
      d.source({ElementType::Float32, {1}}, {0x80000000}, {1, {0}});
  output =
      take(d.run(evaluate.operation, {anchors, all_nan, index, negative_zero},
                 {}, evaluate.parameters))
          .results.at("out");
  require(read_bits(output, {0, 0}) == 0 && read_bits(output, {0, 1}) == 0,
          "parametric t=-0 selects anchor without reading handle numbers");
  failed = d.run(evaluate.operation, {bad_typed, all_nan, index, negative_zero},
                 {}, evaluate.parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 11,
          "parametric typed validation covers the unused opposite anchor");
  for (auto invalid :
       {UINT64_C(0xffffffffffffffff), UINT64_C(0x8000000000000000),
        UINT64_C(0x7fffffffffffffff)}) {
    index = d.source({ElementType::Int64, {2}}, {0, invalid}, {9, {-8}});
    auto parameters =
        d.source({ElementType::Float64, {2}}, {double_bits(.5)}, {1, {0}});
    failed =
        d.run(evaluate.operation, {anchors, all_nan, index, parameters},
              take(Footprint::from_regions({2, 2}, {Region({{1, 1}, {0, 1}})})),
              evaluate.parameters);
    require(!failed.ok() &&
                failed.status().code == ErrorCode::InvalidArgument &&
                failed.status().message.find("segment out of range") !=
                    std::string::npos,
            "negative-stride signed Int64 extremes reject before control NaN");
  }
  index = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  auto invalid_t = d.source({ElementType::Float64, {2}},
                            {double_bits(.5), double_bits(2)}, {1, {8}});
  failed = d.run(evaluate.operation, {anchors, all_nan, index, invalid_t}, {},
                 evaluate.parameters);
  require(!failed.ok() && failed.status().message.find("t outside [0,1]") !=
                              std::string::npos,
          "all parametric query rows validate before any control arithmetic");
  empty = take(d.run(evaluate.operation, {anchors, all_nan, index, invalid_t},
                     take(Footprint::none({2, 2})), evaluate.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty parametric skips invalid query payload");
  auto huge = d.source({ElementType::Float64, {2, 2}}, {0x7fefffffffffffff},
                       {1, {0, 0}});
  auto offsets = d.source({ElementType::Float64, {1, 1, 2}},
                          {0x7fefffffffffffff}, {1, {0, 0, 0}});
  index = d.source({ElementType::Int64, {1}}, {0}, {1, {0}});
  failed = d.run(evaluate.operation, {huge, offsets, index, half}, {},
                 evaluate.parameters);
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow &&
              failed.status().message.find("reconstruction overflow") !=
                  std::string::npos,
          "parametric RN64 reconstruction overflow is explicit");
  CancellationSource stop;
  stop.cancel();
  failed = d.run(evaluate.operation, {anchors, all_nan, index, half}, {},
                 evaluate.parameters, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "Bezier pre-cancellation preserves host category");
}
void bezier_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  auto anchors = d.source({ElementType::Float64, {2, 2}},
                          {0, 0, double_bits(1), double_bits(1)}, {1, {16, 8}});
  auto handles = d.source({ElementType::Float64, {1, 1, 2}},
                          {double_bits(.5), 0}, {1, {0, 0, 8}});
  auto start =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  for (unsigned i = 0; i < 2; ++i) {
    OperationDefinition producer;
    producer.key = "test.bezier.fail" + std::to_string(i);
    auto& output = producer.traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = "test.numeric";
    output.output_schema.result_schema_version = 1;
    output.result_schema = i ? start.schema() : anchors.schema();
    output.dependency_version = 2;
    output.continuation_bytes = sizeof(FailingProducer);
    output.maximum_dependency_stages = 1;
    producer.start_result = [starts](const auto&, const auto& allocator) {
      ++*starts;
      return ResultContinuation::make<FailingProducer>(allocator);
    };
    require(registry->register_operation(std::move(producer)).ok(),
            "Bezier failing producer registration");
  }
  require(registry->freeze().ok(), "Bezier registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  anchors = d.source({ElementType::Float64, {2, 2}},
                     {0, 0, double_bits(1), double_bits(1)}, {1, {16, 8}});
  handles = d.source({ElementType::Float64, {1, 1, 2}}, {double_bits(.5), 0},
                     {1, {0, 0, 8}});
  start = d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  end = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
  auto node = take(numeric::sample_bezier_function_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, WorkflowInputReference{14}, 2, 3));
  auto prepared = d.prepare(node.operation, {anchors, handles, start, end},
                            node.parameters);
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.bezier.fail0", {}, {}});
  document.nodes.back().inputs[0] = WorkflowNodeOutput{3, "value"};
  document.outputs.push_back({"axis", 7, "axis"});
  auto execute = [&](DemandQuery demands) {
    auto graph = std::make_shared<GraphContext>(document);
    auto compiled = take(Compiler(registry).compile(*graph));
    return d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)), demands);
  };
  auto result = take(execute({{"axis", take(Footprint::all({3}))}}));
  auto axis = result.results.at("axis");
  require(*starts == 0 && read_bits(axis, {2}) == double_bits(.25) &&
              axis.schema().tensors[0].atomic_trailing_axes == 1 &&
              axis.association() ==
                  ResourceVector<uint64_t>{start.object_id(), end.object_id()},
          "axis-only excludes failing controls and retains active endpoint "
          "owners");
  auto failed = execute({{"out", take(Footprint::all({3}))}});
  require(!failed.ok() && failed.status().message == "must stay lazy" &&
              *starts == 1,
          "values execute their required failing controls producer");
  *starts = 0;
  document.nodes[0] = {3, "test.bezier.fail1", {}, {}};
  document.nodes.back().inputs[0] = WorkflowInputReference{11};
  document.nodes.back().inputs[3] = WorkflowNodeOutput{3, "value"};
  document.nodes.back().parameters["count"] = int64_t{1};
  result = take(execute({{"out", take(Footprint::all({1}))},
                         {"axis", take(Footprint::all({3}))}}));
  require(*starts == 0 &&
              read_bits(result.results.at("out"), {0}) == double_bits(.25) &&
              read_bits(result.results.at("axis"), {2}) == 0 &&
              result.results.at("axis").association() ==
                  ResourceVector<uint64_t>{start.object_id()},
          "singleton Bezier values and axis exclude end producer");
  auto changed = take(Footprint::all({2, 2}));
  auto dirty = take(result.dependencies.potential_dirty("input0", changed, 1));
  require(dirty.at("out") == take(Footprint::all({1})) &&
              (dirty.count("axis") == 0 || dirty.at("axis").empty()),
          "Bezier controls dirty values while axis remains independent");
  document.nodes.back().parameters["count"] = int64_t{3};
  result = take(execute({{"out", take(Footprint::none({3}))},
                         {"axis", take(Footprint::none({3}))}}));
  require(*starts == 0 && take(result.results.at("out").descriptor())
                              .tensor_coverage(0)
                              .empty(),
          "Empty Bezier leaves failing active producers unstarted");
}
struct CancelBezierPhase {
  ResultContinuation inner;
  std::shared_ptr<CancellationSource> stop;
  std::shared_ptr<bool> triggered;
  CancelBezierPhase(ResultContinuation program,
                    std::shared_ptr<CancellationSource> cancellation,
                    std::shared_ptr<bool> observed)
      : inner(std::move(program)),
        stop(std::move(cancellation)),
        triggered(std::move(observed)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      // A 640-limb ExactPolynomial slot is first touched inside exact work.
      if (amount == 640) {
        *triggered = true;
        stop->cancel();
      }
      return phase.consume_work(amount);
    };
    return inner.poll(bounded);
  }
};
void bezier_resources() {
  for (bool parametric : {false, true}) {
    Driver d;
    const auto original = d.registry;
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    const auto key = parametric ? "curve.evaluate_bezier_strict"
                                : "curve.sample_bezier_function_strict";
    OperationDefinition definition;
    definition.key = key;
    definition.traits = take(original->find_traits(key));
    for (auto& output : definition.traits.outputs)
      output.continuation_bytes += sizeof(CancelBezierPhase);
    definition.specialize_metadata = [original, key](const auto& inputs,
                                                     const auto& parameters) {
      auto traits = original->resolve_traits(key, inputs, parameters);
      if (!traits.ok())
        return Result<std::vector<OperationOutputSpecialization>>(
            traits.status());
      std::vector<OperationOutputSpecialization> outputs;
      for (const auto& output : traits.value().outputs) {
        OperationOutputSpecialization item;
        item.metadata.result_schema =
            std::make_shared<SchemaTemplate>(*output.result_schema);
        item.input_indices = output.input_indices;
        outputs.push_back(std::move(item));
      }
      return Result<std::vector<OperationOutputSpecialization>>(
          std::move(outputs));
    };
    definition.start_result = [original, key, stop, triggered](
                                  const auto& query, const auto& allocator) {
      auto forwarded = query;
      forwarded.prepared.reset();
      auto inner = original->start_result(key, forwarded, allocator);
      if (!inner.ok())
        return inner;
      return ResultContinuation::make<CancelBezierPhase>(
          allocator, inner.take_value(), stop, triggered);
    };
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry->register_operation(std::move(definition)).ok(),
            "Bezier cancellation registration");
    require(d.registry->freeze().ok(), "Bezier cancellation freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto anchors = d.source({ElementType::Float64, {2, 2}},
                            {0, 0, double_bits(1), 0}, {1, {16, 8}});
    auto handles = d.source({ElementType::Float64, {1, 1, 2}},
                            {0, double_bits(1)}, {1, {0, 0, 8}});
    auto q =
        d.source({ElementType::Float64, {1}}, {double_bits(.25)}, {1, {0}});
    auto index = d.source({ElementType::Int64, {1}}, {0}, {1, {0}});
    auto node = take(
        parametric
            ? numeric::evaluate_bezier_node(
                  7, WorkflowInputReference{11}, WorkflowInputReference{12},
                  WorkflowInputReference{13}, WorkflowInputReference{14}, 2)
            : numeric::sample_bezier_function_node(
                  7, WorkflowInputReference{11}, WorkflowInputReference{12},
                  WorkflowInputReference{13}, WorkflowInputReference{14}, 2,
                  1));
    const auto before = d.root.statistics().live;
    auto failed = d.run(key, {anchors, handles, parametric ? index : q, q}, {},
                        node.parameters, stop->token());
    require(*triggered && !failed.ok() &&
                failed.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    before[ResourceKind::Payload],
            "cancellation inside Bezier exact arithmetic rolls back payload");
    d.context.reset();
    require(d.root.statistics().live.values == before.values,
            "cancelled Bezier releases continuation and admitted workspace");
    Driver limited(30000);
    anchors = limited.source({ElementType::Float64, {2, 2}},
                             {0, 0, double_bits(1), 0}, {1, {16, 8}});
    handles = limited.source({ElementType::Float64, {1, 1, 2}},
                             {0, double_bits(1)}, {1, {0, 0, 8}});
    q = limited.source({ElementType::Float64, {1}}, {double_bits(.25)},
                       {1, {0}});
    index = limited.source({ElementType::Int64, {1}}, {0}, {1, {0}});
    const auto baseline = limited.root.statistics().live;
    failed = limited.run(key, {anchors, handles, parametric ? index : q, q}, {},
                         node.parameters);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::WorkLimit &&
                limited.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "Bezier Root WorkLimit rolls back unpublished output");
    limited.context.reset();
    require(limited.root.statistics().live.values == baseline.values,
            "Bezier WorkLimit retires all execution resource charges");
  }
}
void lut1d_workflows() {
  Driver d;
  auto query = d.source(
      {ElementType::Float64, {4}},
      {double_bits(.9), double_bits(.5), double_bits(.3), double_bits(.1)},
      {25, {-8}}, {}, {}, nullptr, "custom.query", 9);
  const uint64_t expected64[] = {0x3f947ae147ae147c, 0xbfc1eb851eb851eb,
                                 0xbfd3333333333333, 0x3fe0000000000000};
  const uint64_t expected32[] = {0x3ca3d70a, 0xbe0f5c29, 0xbe99999a,
                                 0x3f000000};
  ResultRef retained;
  for (bool descending : {false, true}) {
    auto table =
        d.source({ElementType::Float64, {3}},
                 {double_bits(.1), double_bits(-.3), double_bits(.7)},
                 descending ? StridedLayout{17, {-8}} : StridedLayout{1, {8}});
    auto axis = d.source(
        {ElementType::Float64, {3}},
        {double_bits(descending ? 1 : 0), double_bits(descending ? 0 : 1),
         double_bits(descending ? -.5 : .5)},
        {1, {8}});
    for (auto dtype : {ElementType::Float64, ElementType::Float32})
      for (auto profile :
           {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
            CpuNumericProfile::X86Avx2}) {
        auto node = take(numeric::apply_lut1d_node(
            7, WorkflowInputReference{11}, WorkflowInputReference{12},
            WorkflowInputReference{13}, ElementType::Float64, dtype,
            numeric::CurveDomain::Reject, profile));
        std::vector<OperationMetadata> metadata;
        for (const auto& input : {query, table, axis}) {
          OperationMetadata item;
          item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
          metadata.push_back(std::move(item));
        }
        auto available = d.registry->resolve_traits(node.operation, metadata,
                                                    node.parameters);
        if (!available.ok()) {
          require(profile != CpuNumericProfile::Strict &&
                      available.status().code == ErrorCode::BackendUnavailable,
                  "unavailable LUT profile is explicit");
          continue;
        }
        std::fenv_t environment;
        require(std::fegetenv(&environment) == 0, "save LUT fenv");
        std::fesetround(FE_DOWNWARD);
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        auto answer =
            d.run(node.operation, {query, table, axis},
                  take(Footprint::from_regions({4}, {Region({{2, 1}})})),
                  node.parameters);
        const bool preserved = std::fegetround() == FE_DOWNWARD &&
                               std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
        std::fesetenv(&environment);
        require(preserved, "LUT grid and interpolation preserve caller fenv");
        auto result = take(std::move(answer));
        retained = result.results.at("out");
        for (unsigned i = 0; i < 4; ++i)
          require(read_bits(retained, {i}) == (dtype == ElementType::Float64
                                                   ? expected64[i]
                                                   : expected32[i]),
                  "LUT matches independent Fraction interpolation words in "
                  "either axis direction");
        require(retained.schema().id == "photospider.tensor" &&
                    retained.schema().tensors[0].key == "samples" &&
                    retained.schema().tensors[0].facets.empty() &&
                    retained.association().size() == 3,
                "LUT publishes generic tensor and all active source owners");
        for (unsigned port = 0; port < 3; ++port)
          for (uint32_t role : {1U, 4U})
            require(take(result.dependencies.potential_dirty(
                             "input" + std::to_string(port),
                             take(Footprint::all(metadata[port]
                                                     .result_schema->tensors[0]
                                                     .sample_shape())),
                             role))
                            .at("out") ==
                        take(Footprint::from_regions({4}, {Region({{2, 1}})})),
                    "LUT whole source support maps dirty to recorded Q");
      }
  }
  auto channels =
      d.source({ElementType::Float32, {2, 2}},
               {float_bits(1), 0, float_bits(.5F), float_bits(.25F),
                float_bits(.25F), float_bits(.75F), 0, float_bits(1)},
               {5, {16, 8, -4}}, {}, {2});
  auto tables = d.source({ElementType::Float64, {2}},
                         {0, double_bits(10), double_bits(2), double_bits(8),
                          double_bits(5), double_bits(4)},
                         {1, {16, 8}}, {}, {3});
  auto axis = d.source({ElementType::Float64, {3}},
                       {0, double_bits(1), double_bits(.5)}, {1, {8}});
  const double expected[] = {0, 4, 1, 8, 3.5, 9, 5, 10};
  for (auto dtype : {ElementType::Float64, ElementType::Float32})
    for (auto profile :
         {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
          CpuNumericProfile::X86Avx2}) {
      auto node = take(numeric::apply_lut1d_channels_node(
          7, WorkflowInputReference{11}, WorkflowInputReference{12},
          WorkflowInputReference{13}, ElementType::Float32, dtype,
          numeric::CurveDomain::Reject, profile));
      std::vector<OperationMetadata> metadata;
      for (const auto& input : {channels, tables, axis}) {
        OperationMetadata item;
        item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
        metadata.push_back(std::move(item));
      }
      auto available =
          d.registry->resolve_traits(node.operation, metadata, node.parameters);
      if (!available.ok()) {
        require(profile != CpuNumericProfile::Strict &&
                    available.status().code == ErrorCode::BackendUnavailable,
                "unavailable channel LUT profile is explicit");
        continue;
      }
      auto output = take(d.run(node.operation, {channels, tables, axis}, {},
                               node.parameters))
                        .results.at("out");
      require(output.schema().tensors[0].sample_shape() ==
                  std::vector<uint64_t>{2, 2, 2},
              "channel LUT preserves complete logical batch shape");
      for (unsigned i = 0; i < 8; ++i)
        require(read_bits(output, {i / 4, i / 2 % 2, i % 2}) ==
                    (dtype == ElementType::Float64 ? double_bits(expected[i])
                                                   : float_bits(expected[i])),
                "channel LUT uses each query and table column independently");
    }
  d.context.reset();
  require(read_bits(retained, {3}) == float_bits(.5F),
          "LUT output owner survives context retirement");
}
void lut1d_boundaries() {
  Driver d;
  auto node = take(numeric::apply_lut1d_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, ElementType::Float64));
  auto axis = d.source({ElementType::Float64, {3}},
                       {0, double_bits(1), double_bits(.5)}, {1, {8}});
  auto table = d.source({ElementType::Float64, {3}},
                        {0, double_bits(.25), double_bits(1)}, {1, {8}});
  auto outside = d.source({ElementType::Float64, {2}},
                          {double_bits(-.5), double_bits(1.5)}, {1, {8}});
  for (const auto* policy : {"clamp", "linear_extrapolate"}) {
    node.parameters["out_of_domain"] = std::string(policy);
    for (bool descending : {false, true}) {
      auto selected_table =
          descending
              ? d.source({ElementType::Float64, {3}},
                         {0, double_bits(.25), double_bits(1)}, {17, {-8}})
              : table;
      auto selected_axis =
          descending ? d.source({ElementType::Float64, {3}},
                                {double_bits(1), 0, double_bits(-.5)}, {1, {8}})
                     : axis;
      auto output =
          take(d.run(node.operation, {outside, selected_table, selected_axis},
                     {}, node.parameters))
              .results.at("out");
      const bool clamp = std::string(policy) == "clamp";
      require(read_bits(output, {0}) == double_bits(clamp ? 0 : -.25) &&
                  read_bits(output, {1}) == double_bits(clamp ? 1 : 1.75),
              "LUT descending clamp and extrapolation preserve domain ends");
    }
  }
  auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto unused_nan = d.source(
      {ElementType::Float64, {3}},
      {0x8000000000000000, 0x7ff8000000000001, 0x7ff0000000000000}, {1, {8}});
  node.parameters["out_of_domain"] = std::string("reject");
  auto output =
      take(d.run(node.operation, {zero, unused_nan, axis}, {}, node.parameters))
          .results.at("out");
  require(read_bits(output, {0}) == UINT64_C(0x8000000000000000),
          "LUT exact knot preserves -0 and ignores unused generic table NaN");
  auto nan_table =
      d.source({ElementType::Float64, {3}}, {0x7ff8000000000001}, {1, {0}});
  auto queries = d.source({ElementType::Float64, {2}},
                          {double_bits(.25), double_bits(2)}, {1, {8}});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed = d.run(node.operation, {queries, nan_table, axis},
                      take(Footprint::from_regions({2}, {Region({{0, 1}})})),
                      node.parameters);
  require(!failed.ok() &&
              failed.status().message.find("query outside axis domain") !=
                  std::string::npos &&
              failed.status().detail.scope == FailureScope::Run &&
              !failed.status().detail.atom &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "all LUT queries reject before table arithmetic outside Q and "
          "publication rolls back");
  auto bad_step = d.source({ElementType::Float64, {3}},
                           {0, double_bits(1), double_bits(.25)}, {1, {8}});
  failed = d.run(node.operation, {queries, nan_table, bad_step}, {},
                 node.parameters);
  require(!failed.ok() &&
              failed.status().message.find("inconsistent") != std::string::npos,
          "complete axis validation precedes invalid queries and table NaN");
  auto collapsed = d.source(
      {ElementType::Float64, {3}},
      {double_bits(1), 0x3ff0000000000001, double_bits(0x1p-53)}, {1, {8}});
  failed = d.run(node.operation, {zero, table, collapsed}, {}, node.parameters);
  require(!failed.ok() &&
              failed.status().message.find("non-strict reconstructed axis") !=
                  std::string::npos,
          "LUT rejects collapsed RN64 knots despite bit-correct step");
  auto singleton =
      d.source({ElementType::Float64, {1}}, {double_bits(7)}, {1, {INT64_MIN}});
  auto singleton_axis =
      d.source({ElementType::Float64, {3}},
               {0x8000000000000000, 0x8000000000000000, 0}, {1, {8}});
  output = take(d.run(node.operation, {zero, singleton, singleton_axis}, {},
                      node.parameters))
               .results.at("out");
  require(read_bits(output, {0}) == double_bits(7),
          "singleton -0 axis accepts numerically equal +0 query");
  for (const auto* policy : {"clamp", "linear_extrapolate"}) {
    node.parameters["out_of_domain"] = std::string(policy);
    output = take(d.run(node.operation, {outside, singleton, singleton_axis},
                        {}, node.parameters))
                 .results.at("out");
    require(read_bits(output, {0}) == double_bits(7) &&
                read_bits(output, {1}) == double_bits(7),
            "singleton clamp/extrapolate has zero slope for finite queries");
  }
  auto bad_singleton = d.source({ElementType::Float64, {3}},
                                {0x8000000000000000, 0, 0}, {1, {8}});
  failed = d.run(node.operation, {zero, singleton, bad_singleton}, {},
                 node.parameters);
  require(!failed.ok() && failed.status().message.find("bit-identical") !=
                              std::string::npos,
          "singleton requires raw endpoint equality rather than numeric zero");
  auto nan_query =
      d.source({ElementType::Float64, {1}}, {0x7ff8000000000001}, {1, {0}});
  failed = d.run(node.operation, {nan_query, singleton, singleton_axis}, {},
                 node.parameters);
  require(!failed.ok() && failed.status().message.find(
                              "nonfinite LUT1D port=0") != std::string::npos,
          "constant table cannot erase invalid query");
  auto typed_table = d.source({ElementType::Float32, {3, 1}},
                              {0, float_bits(.25F), float_bits(2)}, {1, {4, 0}},
                              {take(encode_semantic(coverage_semantics()))});
  auto channel_node = take(numeric::apply_lut1d_channels_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, ElementType::Float64));
  failed = d.run(channel_node.operation, {zero, typed_table, axis}, {},
                 channel_node.parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 12,
          "LUT typed validation covers the unused table row and preserves "
          "source id");
  auto empty = take(d.run(channel_node.operation, {zero, typed_table, axis},
                          take(Footprint::none({1})), channel_node.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty LUT skips typed table validation");
  auto half =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  auto wide = d.source({ElementType::Float64, {2}},
                       {0x7fefffffffffffff, 0xffefffffffffffff}, {1, {8}});
  auto two_axis = d.source({ElementType::Float64, {3}},
                           {0, double_bits(1), double_bits(1)}, {1, {8}});
  node.parameters["dtype"] = std::string("float32");
  output =
      take(d.run(node.operation, {half, wide, two_axis}, {}, node.parameters))
          .results.at("out");
  require(read_bits(output, {0}) == 0,
          "wide LUT products cancel before final Float32 narrowing");
  failed = d.run(node.operation, {zero, wide, two_axis}, {}, node.parameters);
  require(!failed.ok() &&
              failed.status().reason == FailureReason::ArithmeticOverflow,
          "LUT endpoint conversion can overflow while interior remains finite");
  auto tiny = d.source({ElementType::Float32, {2}}, {0, 0x80000001}, {1, {4}});
  auto quarter =
      d.source({ElementType::Float64, {1}}, {double_bits(.25)}, {1, {0}});
  output = take(d.run(node.operation, {quarter, tiny, two_axis}, {},
                      node.parameters))
               .results.at("out");
  require(read_bits(output, {0}) == 0x80000000,
          "nonzero LUT underflow preserves its negative sign");
  OperationMetadata in, tab, ax;
  in.result_schema = std::make_shared<SchemaTemplate>(zero.schema());
  tab.result_schema = std::make_shared<SchemaTemplate>(table.schema());
  auto wrong_axis = axis.schema();
  wrong_axis.tensors[0].descriptor.element_type = ElementType::Float32;
  ax.result_schema = std::make_shared<SchemaTemplate>(wrong_axis);
  auto rejected = d.registry->resolve_traits(node.operation, {in, tab, ax},
                                             node.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "LUT axis dtype is statically Float64");
  OperationMetadata legacy;
  legacy.descriptor = {ElementType::Float64, {1}};
  rejected = d.registry->resolve_traits(node.operation, {legacy, tab, ax},
                                        node.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "LUT requires Result metadata at every input");
}
void lut1d_composition() {
  Driver d;
  ResultRef retained;
  for (unsigned kind = 0; kind < 6; ++kind) {
    auto start = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
    auto end =
        d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
    auto x = d.source({ElementType::Float64, {3}},
                      {0, double_bits(1), double_bits(2)}, {1, {8}});
    auto y = d.source({ElementType::Float32, {3}},
                      {0, float_bits(1), float_bits(4)}, {1, {4}});
    auto multi = d.source({ElementType::Float64, {3, 2}},
                          {0, double_bits(10), double_bits(1), double_bits(9),
                           double_bits(4), double_bits(6)},
                          {1, {16, 8}});
    auto anchors =
        d.source({ElementType::Float64, {2, 2}},
                 {0, 0, double_bits(1), double_bits(1)}, {1, {16, 8}});
    auto handles = d.source({ElementType::Float64, {1, 1, 2}},
                            {double_bits(.5), 0}, {1, {0, 0, 8}});
    auto query =
        kind < 4 ? d.source({ElementType::Float64, {1}}, {double_bits(.25)},
                            {1, {0}})
                 : d.source({ElementType::Float64, {1, 2}},
                            {double_bits(.25), double_bits(.75)}, {1, {16, 8}});
    WorkflowDocument document;
    ExecutionBindings bindings;
    uint64_t id = 11;
    for (const auto& input :
         {start, end, x, y, multi, anchors, handles, query}) {
      WorkflowInputDeclaration declaration;
      declaration.id = id++;
      declaration.name = "input" + std::to_string(declaration.id);
      declaration.result_schema =
          std::make_shared<SchemaTemplate>(input.schema());
      bindings.inputs.push_back({declaration.name, input});
      document.inputs.push_back(std::move(declaration));
    }
    const auto a = numeric::sequence_input(document.inputs[0]);
    const auto b = numeric::sequence_input(document.inputs[1]);
    numeric::BakedLut1d baked;
    if (kind == 0) {
      baked = take(numeric::bake_lut1d_expression(document, "x^2", a, b, 3));
    } else if (kind == 1) {
      baked = take(
          numeric::bake_lut1d_bezier(document, WorkflowInputReference{16},
                                     WorkflowInputReference{17}, a, b, 2, 3));
    } else {
      const auto helper = kind == 2   ? numeric::bake_lut1d_linear
                          : kind == 3 ? numeric::bake_lut1d_pchip
                          : kind == 4 ? numeric::bake_lut1d_linear_multi
                                      : numeric::bake_lut1d_pchip_multi;
      baked = take(helper(document, WorkflowInputReference{13},
                          WorkflowInputReference{kind < 4 ? 14U : 15U}, a, b, 3,
                          ElementType::Float64, numeric::CurveDomain::Reject,
                          CpuNumericProfile::Strict));
    }
    document.nodes.push_back(
        take(kind < 4 ? numeric::apply_lut1d_node(7, WorkflowInputReference{18},
                                                  baked.values, baked.axis,
                                                  ElementType::Float64)
                      : numeric::apply_lut1d_channels_node(
                            7, WorkflowInputReference{18}, baked.values,
                            baked.axis, ElementType::Float64)));
    document.outputs = {{"out", 7, "values"}};
    auto graph = std::make_shared<GraphContext>(document);
    auto compiled = take(Compiler(d.registry).compile(*graph));
    const auto shape =
        kind < 4 ? std::vector<uint64_t>{1} : std::vector<uint64_t>{1, 2};
    auto result = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, bindings)),
        {{"out", take(Footprint::all(shape))}}));
    retained = result.results.at("out");
    const double first = kind < 2                 ? .125
                         : kind == 3 || kind == 5 ? .15625
                                                  : .25;
    require(read_bits(retained, kind < 4 ? std::vector<uint64_t>{0}
                                         : std::vector<uint64_t>{0, 0}) ==
                double_bits(first),
            "all six public baking templates connect Result table and axis "
            "to LUT consumer with independent discretization values");
    if (kind >= 4)
      require(read_bits(retained, {0, 1}) ==
                  double_bits(kind == 4 ? 9.25 : 9.34375),
              "multi-table baked LUT keeps independent channel queries");
  }
  d.context.reset();
  require(read_bits(retained, {0, 1}) == double_bits(9.34375),
          "baked LUT chain result survives all source owners and context");
}
void lut1d_resources() {
  for (bool channels : {false, true}) {
    Driver d;
    const auto original = d.registry;
    const auto key = channels ? "curve.apply_lut1d_channels_strict"
                              : "curve.apply_lut1d_strict";
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    OperationDefinition definition;
    definition.key = key;
    definition.traits = take(original->find_traits(key));
    definition.traits.outputs[0].continuation_bytes += sizeof(CancelCurvePhase);
    definition.specialize_metadata = [original, key](const auto& inputs,
                                                     const auto& parameters) {
      auto traits = original->resolve_traits(key, inputs, parameters);
      if (!traits.ok())
        return Result<std::vector<OperationOutputSpecialization>>(
            traits.status());
      OperationOutputSpecialization result;
      result.metadata.result_schema = std::make_shared<SchemaTemplate>(
          *traits.value().outputs[0].result_schema);
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(result)});
    };
    definition.start_result = [original, key, stop, triggered](
                                  const auto& query, const auto& allocator) {
      auto forwarded = query;
      forwarded.prepared.reset();
      auto inner = original->start_result(key, forwarded, allocator);
      if (!inner.ok())
        return inner;
      return ResultContinuation::make<CancelCurvePhase>(
          allocator, inner.take_value(), stop, triggered);
    };
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry->register_operation(std::move(definition)).ok(),
            "LUT cancellation registration");
    require(d.registry->freeze().ok(), "LUT cancellation registry freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    const auto shape =
        channels ? std::vector<uint64_t>{1, 2} : std::vector<uint64_t>{2};
    const auto table_shape =
        channels ? std::vector<uint64_t>{2, 2} : std::vector<uint64_t>{2};
    auto query = d.source(
        {ElementType::Float64, shape}, {double_bits(.25), double_bits(.75)},
        channels ? StridedLayout{1, {16, 8}} : StridedLayout{1, {8}});
    auto table =
        d.source({ElementType::Float64, table_shape},
                 {0, double_bits(1), double_bits(1), double_bits(2)},
                 channels ? StridedLayout{1, {16, 8}} : StridedLayout{1, {8}});
    auto axis = d.source({ElementType::Float64, {3}},
                         {0, double_bits(1), double_bits(1)}, {1, {8}});
    const std::map<std::string, ParameterValue> parameters{
        {"dtype", std::string("float64")},
        {"out_of_domain", std::string("reject")}};
    const auto before = d.root.statistics().live;
    auto failed =
        d.run(key, {query, table, axis}, {}, parameters, stop->token());
    require(*triggered && !failed.ok() &&
                failed.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    before[ResourceKind::Payload],
            "LUT cancellation inside ExactCurve rolls back output");
    d.context.reset();
    require(d.root.statistics().live.values == before.values,
            "LUT cancellation releases grid, scratch and continuation");
    Driver limited(30000);
    query = limited.source(
        {ElementType::Float64, shape}, {double_bits(.25), double_bits(.75)},
        channels ? StridedLayout{1, {16, 8}} : StridedLayout{1, {8}});
    table = limited.source(
        {ElementType::Float64, table_shape},
        {0, double_bits(1), double_bits(1), double_bits(2)},
        channels ? StridedLayout{1, {16, 8}} : StridedLayout{1, {8}});
    axis = limited.source({ElementType::Float64, {3}},
                          {0, double_bits(1), double_bits(1)}, {1, {8}});
    const auto baseline = limited.root.statistics().live;
    failed = limited.run(key, {query, table, axis}, {}, parameters);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::WorkLimit &&
                limited.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "LUT WorkLimit rolls back unpublished dense payload");
    limited.context.reset();
    require(limited.root.statistics().live.values == baseline.values,
            "LUT WorkLimit releases all execution resources");
  }
  Driver d;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Payload] = 1048576;
  d.context = std::make_unique<ExecutionContext>(d.registry, config);
  d.root = take(d.context->resource_budget());
  auto query =
      d.source({ElementType::Float64, {262144}}, {double_bits(.25)}, {1, {0}});
  auto table =
      d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
  auto axis = d.source({ElementType::Float64, {3}},
                       {0, double_bits(1), double_bits(1)}, {1, {8}});
  const auto baseline = d.root.statistics().live;
  auto node = take(numeric::apply_lut1d_node(
      7, WorkflowInputReference{11}, WorkflowInputReference{12},
      WorkflowInputReference{13}, ElementType::Float64));
  auto failed =
      d.run(node.operation, {query, table, axis},
            take(Footprint::from_regions({262144}, {Region({{0, 1}})})),
            node.parameters);
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              d.root.statistics().live[ResourceKind::Payload] ==
                  baseline[ResourceKind::Payload],
          "one-cell LUT demand still admits the complete output payload");
  {
    auto point =
        d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
    auto constant =
        d.source({ElementType::Float32, {262145}}, {float_bits(7)}, {1, {0}});
    auto grid = d.source({ElementType::Float64, {3}},
                         {0, double_bits(1), double_bits(0x1p-18)}, {1, {8}});
    const auto payload = d.root.statistics().live[ResourceKind::Payload];
    auto result = take(
        d.run(node.operation, {point, constant, grid}, {}, node.parameters));
    require(read_bits(result.results.at("out"), {0}) == double_bits(7) &&
                d.root.statistics().live[ResourceKind::Payload] == payload + 8,
            "LUT authorized zero-stride table window avoids a full packed "
            "copy under a 1 MiB payload budget");
  }
  d.context.reset();
  require(d.root.statistics().live.values == baseline.values,
          "LUT payload admission failure releases execution state");
}
ColorArrayDescriptor lut3d_description(ColorModel model = ColorModel::Rgb) {
  ColorArrayDescriptor description;
  description.model = model;
  if (model == ColorModel::Rgb || model == ColorModel::Hsl ||
      model == ColorModel::Ycbcr) {
    description.primaries =
        take(color_primary_coordinates(ColorPrimaryPreset::Srgb)).primaries;
    description.transfer = ColorTransfer{ColorTransferKind::Linear, {}};
  }
  if (model == ColorModel::Cielch || model == ColorModel::Oklch ||
      model == ColorModel::Hsl)
    description.hue = ColorHueUnit::PiMultiple;
  if (model == ColorModel::Ycbcr)
    description.ncl_coefficients =
        take(color_ncl_coefficients(ColorNclPreset::Bt709));
  return description;
}
WorkflowNode lut3d_node(
    bool tetrahedral, const ColorArrayDescriptor& input,
    const ColorArrayDescriptor& output,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict,
    numeric::CurveDomain domain = numeric::CurveDomain::Reject) {
  numeric::Lut3dOptions options;
  options.dtype = dtype;
  options.profile = profile;
  options.out_of_domain = domain;
  const auto helper = tetrahedral ? numeric::apply_lut3d_tetrahedral_node
                                  : numeric::apply_lut3d_trilinear_node;
  return take(helper(7, WorkflowInputReference{11}, WorkflowInputReference{12},
                     WorkflowInputReference{13}, ElementType::Float32, input,
                     output, options));
}
void lut3d_workflows() {
  Driver d;
  auto color = lut3d_description();
  auto target = color;
  target.primaries =
      take(color_primary_coordinates(ColorPrimaryPreset::DisplayP3)).primaries;
  target.transfer = ColorTransfer{ColorTransferKind::Srgb, {}};
  const auto source_facet = take(encode_color_array(color));
  const auto target_facet = take(encode_color_array(target));
  const unsigned quarters[12][3] = {{3, 2, 1}, {3, 1, 2}, {2, 3, 1}, {2, 1, 3},
                                    {1, 3, 2}, {1, 2, 3}, {2, 2, 2}, {3, 3, 1},
                                    {0, 0, 0}, {4, 4, 4}, {4, 1, 2}, {0, 2, 4}};
  // Independent integer barycentric weights on stored-index tetrahedra.
  const uint8_t tetra_quarters[8][36] = {
      {2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 2, 1, 1, 2, 1,
       2, 2, 2, 3, 1, 1, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0},
      {1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 0,
       0, 2, 0, 2, 1, 0, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0},
      {1, 0, 1, 0, 0, 2, 1, 0, 1, 0, 0, 2, 0, 1, 1, 0, 1, 1,
       0, 0, 2, 2, 0, 1, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {2, 0, 0, 1, 0, 1, 2, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0,
       2, 0, 0, 3, 0, 0, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {2, 0, 0, 1, 0, 1, 2, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0,
       2, 0, 0, 3, 0, 0, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {1, 0, 1, 0, 0, 2, 1, 0, 1, 0, 0, 2, 0, 1, 1, 0, 1, 1,
       0, 0, 2, 2, 0, 1, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 0,
       0, 2, 0, 2, 1, 0, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0},
      {2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 2, 1, 1, 2, 1,
       2, 2, 2, 3, 1, 1, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0}};
  std::vector<uint64_t> input_words;
  for (const auto& q : quarters)
    for (unsigned c = 3; c > 0; --c)
      input_words.push_back(float_bits(q[c - 1] * .25F));
  auto input =
      d.source({ElementType::Float32, {6, 3}}, input_words, {9, {72, 12, -4}},
               {source_facet}, {2}, nullptr, "custom.color", 5);
  ResultRef retained;
  for (unsigned direction = 0; direction < 8; ++direction) {
    std::vector<uint64_t> table_words, axis_words;
    for (unsigned r = 0; r < 2; ++r)
      for (unsigned g = 0; g < 2; ++g)
        for (unsigned b = 0; b < 2; ++b) {
          const unsigned x = direction & 1 ? 1 - r : r;
          const unsigned y = direction & 2 ? 1 - g : g;
          const unsigned z = direction & 4 ? 1 - b : b;
          table_words.insert(
              table_words.end(),
              {double_bits(z * x), double_bits(y * z), double_bits(x * y)});
        }
    for (unsigned axis = 0; axis < 3; ++axis) {
      const bool reverse = direction & (1U << axis);
      axis_words.insert(axis_words.end(), {double_bits(reverse ? 1 : 0),
                                           double_bits(reverse ? 0 : 1),
                                           double_bits(reverse ? -1 : 1)});
    }
    auto table = d.source({ElementType::Float64, {2, 2, 3}}, table_words,
                          {17, {96, 48, 24, -8}}, {target_facet}, {2});
    auto axes =
        d.source({ElementType::Float64, {3, 3}}, axis_words, {1, {24, 8}});
    for (bool tetra : {false, true})
      for (auto dtype : {ElementType::Float64, ElementType::Float32})
        for (auto profile :
             {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
              CpuNumericProfile::X86Avx2}) {
          auto node = lut3d_node(tetra, color, target, dtype, profile);
          std::vector<OperationMetadata> metadata;
          for (const auto& source : {input, table, axes}) {
            OperationMetadata item;
            item.result_schema =
                std::make_shared<SchemaTemplate>(source.schema());
            metadata.push_back(std::move(item));
          }
          auto available = d.registry->resolve_traits(node.operation, metadata,
                                                      node.parameters);
          if (!available.ok()) {
            require(
                profile != CpuNumericProfile::Strict &&
                    available.status().code == ErrorCode::BackendUnavailable,
                "unavailable LUT3D profile remains explicit");
            continue;
          }
          auto query = take(Footprint::from_regions(
              {2, 6, 3}, {Region({{0, 1}, {1, 1}, {0, 1}})}));
          auto result = take(d.run(node.operation, {input, table, axes}, query,
                                   node.parameters));
          retained = result.results.at("out");
          require(retained.schema().id == "photospider.tensor" &&
                      retained.schema().tensors[0].key == "samples" &&
                      retained.schema().tensors[0].atomic_trailing_axes == 1 &&
                      retained.schema().tensors[0].facets.size() == 1 &&
                      retained.schema().tensors[0].facets[0].payload ==
                          target_facet.payload &&
                      retained.association().size() == 3,
                  "LUT3D publishes target ColorArray v1 and tuple closure "
                  "without implicit color conversion");
          for (unsigned row = 0; row < 12; ++row)
            for (unsigned c = 0; c < 3; ++c) {
              const double expected =
                  tetra ? tetra_quarters[direction][3 * row + c] * .25
                        : quarters[row][c] * quarters[row][(c + 1) % 3] * .0625;
              require(
                  read_bits(retained, {row / 6, row % 6, c}) ==
                      (dtype == ElementType::Float64 ? double_bits(expected)
                                                     : float_bits(expected)),
                  "LUT3D all six tetrahedra/ties/directions and full typed "
                  "batch traversal match independent dyadic weights");
            }
          const auto closed =
              take(retained.schema().tensors[0].close_samples(query));
          for (unsigned port = 0; port < 3; ++port)
            for (uint32_t role : {1U, 4U})
              require(
                  take(result.dependencies.potential_dirty(
                           "input" + std::to_string(port),
                           take(Footprint::all(metadata[port]
                                                   .result_schema->tensors[0]
                                                   .sample_shape())),
                           role))
                          .at("out") == closed,
                  "LUT3D whole data/validation dirty closes the recorded "
                  "component demand to complete colors");
        }
  }
  const double table_values[24] = {-.8, -.5, .2, .7,  -.3,  .1,  -.1, .4,
                                   -.6, .9,  .8, -.9, .3,   -.7, .6,  -.4,
                                   1.1, -.2, .5, .2,  -1.3, 1.7, -.9, .4};
  const uint64_t golden64[2][12] = {
      {0x3fe2c710cb295e9e, 0x3fa6ae7d566cf423, 0xbfc563886594af4f,
       0x3fbeecbfb15b573f, 0xbfc67d566cf41f20, 0x3fcd4c985f06f694,
       0x3fc07c84b5dcc63f, 0x3fd025aee631f8a1, 0xbfe58a0902de00d2,
       0xbfa0ff9724745396, 0xbfc36ae7d566cf42, 0xbfad3c36113404e9},
      {0x3fe6147ae147ae15, 0xbfc47ae147ae147b, 0xbfaeb851eb851eb6,
       0x3fc851eb851eb853, 0xbfd5c28f5c28f5c2, 0x3fd851eb851eb852,
       0x3fc0a3d70a3d70a4, 0x3fc1eb851eb851ed, 0xbfe1eb851eb851eb,
       0xbfa99999999999a4, 0xbfe3d70a3d70a3d7, 0x3fd0a3d70a3d70a4}};
  const uint64_t golden32[2][12] = {
      {0x3f163886, 0x3d3573eb, 0xbe2b1c43, 0x3df765fe, 0xbe33eab3, 0x3e6a64c3,
       0x3e03e426, 0x3e812d77, 0xbf2c5048, 0xbd07fcb9, 0xbe1b573f, 0xbd69e1b1},
      {0x3f30a3d7, 0xbe23d70a, 0xbd75c28f, 0x3e428f5c, 0xbeae147b, 0x3ec28f5c,
       0x3e051eb8, 0x3e0f5c29, 0xbf0f5c29, 0xbd4ccccd, 0xbf1eb852, 0x3e851eb8}};
  std::vector<uint64_t> table_words, queries;
  for (double v : table_values)
    table_words.push_back(double_bits(v));
  for (double v : {.1, .3, .9, .9, .1, .3, .3, .9, .1, .3, .3, .3})
    queries.push_back(double_bits(v));
  auto query = d.source({ElementType::Float64, {4, 3}}, queries, {1, {24, 8}});
  auto table = d.source({ElementType::Float64, {2, 2, 2, 3}}, table_words,
                        {1, {96, 48, 24, 8}});
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1), 0, double_bits(1),
                        double_bits(1), 0, double_bits(1), double_bits(1)},
                       {1, {24, 8}});
  for (bool tetra : {false, true})
    for (auto dtype : {ElementType::Float64, ElementType::Float32}) {
      auto node = lut3d_node(tetra, color, target, dtype);
      std::fenv_t environment;
      require(std::fegetenv(&environment) == 0, "save LUT3D fenv");
      std::fesetround(FE_UPWARD);
      std::feclearexcept(FE_ALL_EXCEPT);
      std::feraiseexcept(FE_DIVBYZERO);
      auto result =
          d.run(node.operation, {query, table, axes}, {}, node.parameters);
      const bool preserved = std::fegetround() == FE_UPWARD &&
                             std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
      std::fesetenv(&environment);
      require(preserved, "LUT3D preserves caller floating environment");
      auto output = take(std::move(result)).results.at("out");
      for (unsigned i = 0; i < 12; ++i)
        require(read_bits(output, {i / 3, i % 3}) ==
                    (dtype == ElementType::Float64 ? golden64[tetra][i]
                                                   : golden32[tetra][i]),
                "LUT3D independent Fraction weight and whole-sum oracle");
    }
  for (auto model : {ColorModel::Rgb, ColorModel::Xyz, ColorModel::Cielab,
                     ColorModel::Oklab, ColorModel::Cielch, ColorModel::Oklch,
                     ColorModel::Hsl, ColorModel::Ycbcr}) {
    auto description = lut3d_description(model);
    auto point = d.source({ElementType::Float32, {1, 3}}, {float_bits(.5F)},
                          {1, {0, 0}});
    auto ones = d.source({ElementType::Float32, {2, 2, 2, 3}}, {float_bits(1)},
                         {1, {0, 0, 0, 0}});
    for (bool tetra : {false, true}) {
      auto node = lut3d_node(tetra, description, description);
      auto output =
          take(d.run(node.operation, {point, ones, axes}, {}, node.parameters))
              .results.at("out");
      require(read_bits(output, {0, 0}) == double_bits(1) &&
                  read_bits(output, {0, 1}) == double_bits(1) &&
                  read_bits(output, {0, 2}) == double_bits(1) &&
                  take(decode_color_array(output.schema().tensors[0].facets[0]))
                          .model == model,
              "LUT3D all eight current ColorArray v1 models remain explicit");
    }
  }
  for (auto model : {ColorModel::Cielch, ColorModel::Oklch, ColorModel::Hsl}) {
    auto description = lut3d_description(model);
    auto point = d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)},
                          {1, {0, 0}});
    const auto hue = model == ColorModel::Hsl ? 0U : 2U;
    std::vector<uint64_t> color_words(3, double_bits(1));
    color_words[hue] = double_bits(4);
    auto colors = d.source({ElementType::Float64, {2, 2, 2, 3}}, color_words,
                           {1, {0, 0, 0, 8}});
    std::vector<uint64_t> winding_words;
    for (unsigned r = 0; r < 2; ++r)
      for (unsigned g = 0; g < 2; ++g)
        for (unsigned b = 0; b < 2; ++b) {
          auto color = color_words;
          color[hue] = double_bits(4.0 * r);
          winding_words.insert(winding_words.end(), color.begin(), color.end());
        }
    auto winding = d.source({ElementType::Float64, {2, 2, 2, 3}}, winding_words,
                            {1, {96, 48, 24, 8}});
    auto winding_queries =
        d.source({ElementType::Float64, {3, 3}},
                 {double_bits(0), double_bits(0), double_bits(0),
                  double_bits(.5), double_bits(.5), double_bits(.5),
                  double_bits(1), double_bits(1), double_bits(1)},
                 {1, {24, 8}});
    for (bool tetra : {false, true}) {
      auto node = lut3d_node(tetra, description, description);
      auto output = take(d.run(node.operation, {point, colors, axes}, {},
                               node.parameters))
                        .results.at("out");
      require(
          read_bits(output, {0, hue}) == double_bits(4) &&
              take(decode_color_array(output.schema().tensors[0].facets[0]))
                      .hue == ColorHueUnit::PiMultiple,
          "LUT3D polar/HSL hue retains unnormalized winding in explicit units");
      auto interpolated =
          take(d.run(node.operation, {winding_queries, winding, axes}, {},
                     node.parameters))
              .results.at("out");
      for (unsigned row = 0; row < 3; ++row)
        require(read_bits(interpolated, {row, hue}) == double_bits(2.0 * row),
                "LUT3D hue 0-to-4 interpolation retains midpoint 2 and both "
                "endpoints without normalization");
    }
  }
  d.context.reset();
  require(read_bits(retained, {0, 1, 0}) == float_bits(.25F),
          "LUT3D typed output and backing survive context retirement");
}
void lut3d_boundaries() {
  Driver d;
  const auto color = lut3d_description();
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1), 0, double_bits(1),
                        double_bits(1), 0, double_bits(1), double_bits(1)},
                       {1, {24, 8}});
  auto center =
      d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)}, {1, {0, 0}});
  auto origin = d.source({ElementType::Float32, {1, 3}}, {0}, {1, {0, 0}});
  std::vector<uint64_t> diagonal(24, 0x7ff8000000000001);
  for (unsigned c = 0; c < 3; ++c) {
    diagonal[c] = 0;
    diagonal[21 + c] = double_bits(1);
  }
  auto generic = d.source({ElementType::Float64, {2, 2, 2, 3}}, diagonal,
                          {1, {96, 48, 24, 8}});
  auto tetra = lut3d_node(true, color, color);
  auto output = take(d.run(tetra.operation, {center, generic, axes}, {},
                           tetra.parameters))
                    .results.at("out");
  require(read_bits(output, {0, 0}) == double_bits(.5) &&
              read_bits(output, {0, 2}) == double_bits(.5),
          "tetrahedral diagonal skips zero-weight generic NaN vertices");
  auto tri = lut3d_node(false, color, color);
  auto failed =
      d.run(tri.operation, {center, generic, axes}, {}, tri.parameters);
  require(
      !failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
          failed.status().message.find("port=1") != std::string::npos &&
          failed.status().detail.scope == FailureScope::Run &&
          !failed.status().detail.atom,
      "trilinear center uses all eight vertices and fails the complete Run");
  auto typed =
      d.source({ElementType::Float64, {2, 2, 2, 3}}, diagonal,
               {1, {96, 48, 24, 8}}, {take(encode_color_array(color))});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  failed = d.run(tetra.operation, {center, typed, axes}, {}, tetra.parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 12 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "typed LUT3D validates unused vertices and preserves source id");
  auto empty = take(d.run(tetra.operation, {center, typed, axes},
                          take(Footprint::none({1, 3})), tetra.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty LUT3D skips typed table payload");
  auto queries = d.source(
      {ElementType::Float64, {2, 3}},
      {double_bits(.5), double_bits(.5), double_bits(.5), double_bits(2), 0, 0},
      {1, {24, 8}});
  failed =
      d.run(tri.operation, {queries, generic, axes},
            take(Footprint::from_regions({2, 3}, {Region({{0, 1}, {0, 1}})})),
            tri.parameters);
  require(
      !failed.ok() && failed.status().message.find("query outside axis=0") !=
                          std::string::npos,
      "LUT3D validates every original query before table arithmetic outside Q");
  auto bad_axes =
      d.source({ElementType::Float64, {3, 3}},
               {0, double_bits(1), double_bits(.5), 0, double_bits(1),
                double_bits(1), 0, double_bits(1), double_bits(1)},
               {1, {24, 8}});
  failed =
      d.run(tri.operation, {queries, generic, bad_axes}, {}, tri.parameters);
  require(!failed.ok() &&
              failed.status().message.find("inconsistent") != std::string::npos,
          "LUT3D global axis validation precedes query/domain errors");
  auto lch = lut3d_description(ColorModel::Cielch);
  auto clamped =
      lut3d_node(true, lch, lch, ElementType::Float64,
                 CpuNumericProfile::Strict, numeric::CurveDomain::Clamp);
  auto negative_chroma = d.source(
      {ElementType::Float64, {1, 3}},
      {double_bits(.5), double_bits(-1), double_bits(.5)}, {1, {24, 8}});
  auto ones = d.source({ElementType::Float32, {2, 2, 2, 3}}, {float_bits(1)},
                       {1, {0, 0, 0, 0}});
  failed = d.run(clamped.operation, {negative_chroma, ones, axes}, {},
                 clamped.parameters);
  require(!failed.ok() &&
              failed.status().message.find("negative LUT3D chroma; port=0") !=
                  std::string::npos,
          "LUT3D validates original chroma before a clamp can hide it");
  auto outside = d.source({ElementType::Float64, {1, 3}},
                          {double_bits(-1), double_bits(2), double_bits(.5)},
                          {1, {24, 8}});
  auto rgb_clamp =
      lut3d_node(false, color, color, ElementType::Float64,
                 CpuNumericProfile::Strict, numeric::CurveDomain::Clamp);
  output = take(d.run(rgb_clamp.operation, {outside, ones, axes}, {},
                      rgb_clamp.parameters))
               .results.at("out");
  require(read_bits(output, {0, 0}) == double_bits(1),
          "LUT3D component clamp remains explicit for finite HDR queries");
  auto other = color;
  other.primaries =
      take(color_primary_coordinates(ColorPrimaryPreset::DisplayP3)).primaries;
  auto mismatched = d.source({ElementType::Float64, {1, 3}}, {0}, {1, {0, 0}},
                             {take(encode_color_array(other))});
  std::vector<OperationMetadata> metadata;
  for (const auto& source : {mismatched, ones, axes}) {
    OperationMetadata item;
    item.result_schema = std::make_shared<SchemaTemplate>(source.schema());
    metadata.push_back(std::move(item));
  }
  auto rejected =
      d.registry->resolve_traits(tri.operation, metadata, tri.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "LUT3D attached input description mismatch is a preflight failure");
  metadata[0].result_schema = std::make_shared<SchemaTemplate>(origin.schema());
  auto wrong_table = ones.schema();
  wrong_table.tensors[0].facets = {take(encode_color_array(other))};
  metadata[1].result_schema = std::make_shared<SchemaTemplate>(wrong_table);
  rejected =
      d.registry->resolve_traits(tri.operation, metadata, tri.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "LUT3D attached table description must match output description");
  auto negative_zero = d.source({ElementType::Float32, {2, 2, 2, 3}},
                                {0x80000000}, {1, {0, 0, 0, 0}});
  for (bool tetrahedral : {false, true}) {
    auto node = lut3d_node(tetrahedral, color, color, ElementType::Float32);
    output = take(d.run(node.operation, {center, negative_zero, axes}, {},
                        node.parameters))
                 .results.at("out");
    require(read_bits(output, {0, 0}) == 0x80000000 &&
                read_bits(output, {0, 2}) == 0x80000000,
            "LUT3D preserves all-negative-zero positive-weight mixtures");
    std::vector<uint64_t> cancellation(24);
    for (unsigned i = 0; i < 24; ++i)
      cancellation[i] =
          i < 12 ? UINT64_C(0x7fefffffffffffff) : UINT64_C(0xffefffffffffffff);
    auto wide = d.source({ElementType::Float64, {2, 2, 2, 3}}, cancellation,
                         {1, {96, 48, 24, 8}});
    output =
        take(d.run(node.operation, {center, wide, axes}, {}, node.parameters))
            .results.at("out");
    require(read_bits(output, {0, 0}) == 0 && read_bits(output, {0, 2}) == 0,
            "LUT3D exact large opposite vertex sums cancel before narrowing");
    failed = d.run(node.operation, {origin, wide, axes}, {}, node.parameters);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::ArithmeticOverflow,
            "LUT3D selected-vertex narrowing overflow remains explicit");
    std::vector<uint64_t> tiny(24, 0);
    for (unsigned c = 0; c < 3; ++c)
      tiny[21 + c] = 0x80000001;
    auto subnormal = d.source({ElementType::Float32, {2, 2, 2, 3}}, tiny,
                              {1, {48, 24, 12, 4}});
    auto quarter = d.source({ElementType::Float64, {1, 3}}, {double_bits(.25)},
                            {1, {0, 0}});
    output = take(d.run(node.operation, {quarter, subnormal, axes}, {},
                        node.parameters))
                 .results.at("out");
    require(read_bits(output, {0, 0}) == 0x80000000,
            "LUT3D nonzero negative underflow preserves sign");
  }
  std::vector<uint64_t> affine;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 3; ++g)
      for (unsigned b = 0; b < 5; ++b) {
        const double x = r, y = g * .5, z = b * .25;
        affine.insert(affine.end(), {double_bits(x + 2 * y - 3 * z),
                                     double_bits(2 * x - y + .25 * z),
                                     double_bits(1 + x - y + z)});
      }
  auto unequal = d.source({ElementType::Float64, {2, 3, 5, 3}}, affine,
                          {1, {360, 120, 24, 8}});
  auto unequal_axes =
      d.source({ElementType::Float64, {3, 3}},
               {0, double_bits(1), double_bits(1), 0, double_bits(1),
                double_bits(.5), 0, double_bits(1), double_bits(.25)},
               {1, {24, 8}});
  auto point = d.source(
      {ElementType::Float32, {1, 3}},
      {float_bits(.25F), float_bits(.375F), float_bits(.625F)}, {1, {12, 4}});
  for (bool tetrahedral : {false, true}) {
    auto node = lut3d_node(tetrahedral, color, color);
    output = take(d.run(node.operation, {point, unequal, unequal_axes}, {},
                        node.parameters))
                 .results.at("out");
    require(read_bits(output, {0, 0}) == double_bits(-.875) &&
                read_bits(output, {0, 1}) == double_bits(.28125) &&
                read_bits(output, {0, 2}) == double_bits(1.5),
            "LUT3D unequal [2,3,5] axes preserve affine mapping without "
            "conversion");
  }
}
struct ObservedMathPreparation {
  std::shared_ptr<const PreparedOperation> original;
};
struct ObservedMathPhase {
  ResultContinuation inner;
  std::shared_ptr<CancellationSource> stop;
  std::shared_ptr<bool> triggered;
  bool refinement, entered = false;
  uint64_t cancel_words;
  ObservedMathPhase(ResultContinuation program,
                    std::shared_ptr<CancellationSource> cancellation,
                    std::shared_ptr<bool> observed, bool cancel_refinement,
                    uint64_t words)
      : inner(std::move(program)),
        stop(std::move(cancellation)),
        triggered(std::move(observed)),
        refinement(cancel_refinement),
        cancel_words(words) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!stop)
      return inner.poll(phase);
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      if (refinement && amount == 16)
        entered = true;
      if (refinement ? entered && amount == 1 : amount == cancel_words) {
        *triggered = true;
        stop->cancel();
      }
      return phase.consume_work(amount);
    };
    return inner.poll(bounded);
  }
};
OperationDefinition observed_numeric(
    const std::shared_ptr<OperationRegistry>& original, const std::string& key,
    const std::shared_ptr<unsigned>& preparations,
    std::shared_ptr<CancellationSource> stop = {},
    std::shared_ptr<bool> triggered = {}, bool refinement = false,
    uint64_t cancel_words = 640) {
  OperationDefinition definition;
  definition.key = key;
  definition.traits = take(original->find_traits(key));
  definition.traits.outputs[0].continuation_bytes += sizeof(ObservedMathPhase);
  definition.prepare_static = [original, key, preparations](
                                  const auto& inputs, const auto& parameters) {
    ++*preparations;
    auto prepared = original->prepare_operation(key, inputs, parameters);
    if (!prepared.ok())
      return Result<OperationPreparation>(prepared.status());
    OperationPreparation result;
    auto inner = prepared.take_value();
    result.state = std::make_shared<ObservedMathPreparation>(
        ObservedMathPreparation{inner});
    result.outputs.resize(1);
    result.outputs[0].metadata.result_schema = std::make_shared<SchemaTemplate>(
        *inner->traits().outputs[0].result_schema);
    return Result<OperationPreparation>(std::move(result));
  };
  definition.start_result = [original, key, stop, triggered, refinement,
                             cancel_words](const auto& query,
                                           const auto& allocator) {
    auto forwarded = query;
    const auto* observation =
        static_cast<const ObservedMathPreparation*>(query.prepared->state());
    forwarded.prepared = observation->original;
    auto inner = original->start_result(key, forwarded, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<ObservedMathPhase>(
        allocator, inner.take_value(), stop, triggered, refinement,
        cancel_words);
  };
  return definition;
}

WorkflowNode lowpass_node(
    bool continuous, unsigned kernel,
    CpuNumericProfile profile = CpuNumericProfile::Strict, int64_t axis = 1,
    numeric::LowpassBoundary boundary = numeric::LowpassBoundary::Reflect) {
  using namespace numeric;  // NOLINT(build/namespaces)
  if (continuous) {
    if (kernel == 3)
      return take(lowpass_nonuniform_kaiser_sinc_node(
          7, WorkflowInputReference{11}, WorkflowInputReference{12}, axis, .5,
          .25, 2, boundary, profile));
    auto helper = kernel == 0   ? lowpass_nonuniform_hann_sinc_node
                  : kernel == 1 ? lowpass_nonuniform_hamming_sinc_node
                  : kernel == 2 ? lowpass_nonuniform_blackman_sinc_node
                                : lowpass_nonuniform_gaussian_node;
    return take(helper(7, WorkflowInputReference{11},
                       WorkflowInputReference{12}, axis, .5,
                       kernel == 4 ? 1. : .25, boundary, profile));
  }
  if (kernel == 3)
    return take(lowpass_uniform_kaiser_sinc_node(
        7, WorkflowInputReference{11}, axis, 2, .25, 2, boundary, profile));
  auto helper = kernel == 0   ? lowpass_uniform_hann_sinc_node
                : kernel == 1 ? lowpass_uniform_hamming_sinc_node
                : kernel == 2 ? lowpass_uniform_blackman_sinc_node
                              : lowpass_uniform_gaussian_node;
  return take(helper(7, WorkflowInputReference{11}, axis, 2,
                     kernel == 4 ? 1. : .25, boundary, profile));
}

void finite_arithmetic_workflows() {
  const std::array<const char*, 5> keys{"numeric.add", "numeric.subtract",
                                        "numeric.multiply", "numeric.divide",
                                        "numeric.clamp"};
  const std::array<std::array<double, 3>, 5> expected{{{{7, 6, 5}},
                                                       {{-1, -2, -3}},
                                                       {{12, 8, 4}},
                                                       {{.75, .5, .25}},
                                                       {{2.5, 2, 1.5}}}};
  for (bool narrow : {false, true})
    for (unsigned k = 0; k < keys.size(); ++k) {
      Driver d;
      const auto dtype = narrow ? ElementType::Float32 : ElementType::Float64;
      const auto raw = [&](double value) {
        return narrow ? float_bits(static_cast<float>(value))
                      : double_bits(value);
      };
      auto a = d.source({dtype, {3}}, {raw(1), raw(2), raw(3)},
                        {1 + 2 * (narrow ? 4U : 8U), {0, narrow ? -4 : -8}}, {},
                        {2}, nullptr, "finite.input", 43);
      auto b = d.source({dtype, {3}}, {raw(4)}, {1, {0, 0}}, {}, {2});
      const auto roi =
          take(Footprint::from_regions({2, 3}, {Region({{1, 1}, {1, 1}})}));
      ResultRef output;
      {
        auto result = take(d.run(
            keys[k],
            k == 4 ? std::vector<ResultRef>{a} : std::vector<ResultRef>{a, b},
            roi,
            k == 4 ? std::map<std::string, ParameterValue>{{"min", 1.5},
                                                           {"max", 2.5}}
                   : std::map<std::string, ParameterValue>{},
            {}, "value"));
        output = result.results.at("out");
        require(output.schema().id == "photospider.tensor" &&
                    output.schema().tensors[0].key == "samples" &&
                    output.schema().tensors[0].sample_shape() ==
                        std::vector<uint64_t>({2, 3}) &&
                    output.schema().tensors[0].facets.empty(),
                "finite arithmetic Result full batch shape and generic output");
        require(take(output.descriptor()).tensor_coverage(0) ==
                        take(Footprint::all({2, 3})) &&
                    take(result.dependencies.source_support()).at("input0") ==
                        take(Footprint::all({2, 3})),
                "finite arithmetic Whole full result and source support");
        require(
            take(result.dependencies.potential_dirty(
                     "input0", take(Footprint::from_regions(
                                   {2, 3}, {Region({{0, 1}, {0, 1}})}))))
                    .at("out") == roi,
            "finite arithmetic remote input dirties recorded output demand");
      }
      a = {};
      b = {};
      d.context.reset();
      for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 3; ++j)
          require(read_bits(output, {i, j}) == raw(expected[k][j]),
                  "finite arithmetic signed unaligned layouts and lifetime");
      output = {};
      require(d.root.statistics().live[ResourceKind::Payload] == 0,
              "finite arithmetic final owner releases payload");
    }
}
void finite_arithmetic_boundaries() {
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save finite arithmetic floating environment");
  for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set finite arithmetic floating environment");
    for (bool narrow : {false, true}) {
      Driver rounding;
      const auto dtype = narrow ? ElementType::Float32 : ElementType::Float64;
      const uint64_t sign = UINT64_C(1) << (narrow ? 31 : 63);
      auto numerator = rounding.source({dtype, {4}}, {1, 3, sign | 1, sign | 3},
                                       {1, {narrow ? 4 : 8}});
      auto divisor = rounding.source(
          {dtype, {4}}, {narrow ? float_bits(2) : double_bits(2)}, {1, {0}});
      auto rounded = take(rounding.run("numeric.divide", {numerator, divisor},
                                       {}, {}, {}, "value"));
      for (unsigned i = 0; i < 4; ++i)
        require(read_bits(rounded.results.at("out"), {i}) ==
                    (i < 2 ? 0 : sign) + (i % 2 ? 2 : 0),
                "finite arithmetic nearest-even subnormal ties and zero signs");
    }
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "finite arithmetic preserves caller floating environment");
  }
  require(fesetenv(&saved) == 0,
          "restore finite arithmetic floating environment");

  Driver d;
  auto a =
      d.source({ElementType::Float32, {3}},
               {float_bits(1), float_bits(2), UINT32_C(0x7f800000)}, {1, {4}});
  auto b = d.source({ElementType::Float32, {3}}, {float_bits(2)}, {1, {0}});
  auto roi = take(Footprint::from_regions({3}, {Region({{0, 1}})}));
  for (const auto* key : {"numeric.add", "numeric.subtract", "numeric.multiply",
                          "numeric.divide"}) {
    auto failed = d.run(key, {a, b}, roi, {}, {}, "value");
    require(!failed.ok() &&
                failed.status().code == ErrorCode::OperationFailed &&
                failed.status().message ==
                    "arithmetic input is nonfinite at sample 2" &&
                failed.status().detail.scope == FailureScope::Run,
            "finite arithmetic rejects undelivered infinity with global row "
            "identity");
    auto empty =
        take(d.run(key, {a, b}, take(Footprint::none({3})), {}, {}, "value"));
    require(take(empty.dependencies.source_support()).empty() &&
                take(empty.results.at("out").descriptor())
                    .tensor_coverage(0)
                    .empty(),
            "Empty finite arithmetic skips dynamic finite validation");
  }
  a = d.source({ElementType::Float32, {1}}, {float_bits(1)}, {1, {0}});
  auto result = take(d.run("numeric.clamp", {a}, {},
                           {{"min", -1e100}, {"max", 1e100}}, {}, "value"));
  require(read_bits(result.results.at("out"), {0}) == float_bits(1),
          "unused static Float32 clamp endpoints retain Float64 range");
  auto failed = d.run("numeric.clamp", {a}, {},
                      {{"min", 1e100}, {"max", 1e100}}, {}, "value");
  require(!failed.ok() && failed.status().message ==
                              "clamp result outside dtype range at sample 0",
          "static clamp checks selected bounded result before narrow");
  b = d.source({ElementType::Float32, {1}}, {float_bits(-0.)}, {1, {0}});
  failed = d.run("numeric.divide", {a, b}, {}, {}, {}, "value");
  require(
      !failed.ok() && failed.status().message == "division by zero at sample 0",
      "finite divide rejects negative zero");
  a = d.source({ElementType::Float32, {1}}, {UINT32_C(0x7f7fffff)}, {1, {0}});
  b = d.source({ElementType::Float32, {1}}, {float_bits(2)}, {1, {0}});
  failed = d.run("numeric.multiply", {a, b}, {}, {}, {}, "value");
  require(!failed.ok() && failed.status().message ==
                              "arithmetic result is nonfinite at sample 0",
          "finite multiplication preserves overflow failure");
  auto coverage = take(encode_semantic(coverage_semantics()));
  a = d.source({ElementType::Float32, {3, 1}},
               {0, float_bits(.5), float_bits(2)}, {1, {4, 0}}, {coverage});
  b = d.source({ElementType::Float32, {3, 1}}, {float_bits(.5)}, {1, {0, 0}});
  failed =
      d.run("numeric.add", {a, b},
            take(Footprint::from_regions({3, 1}, {Region({{1, 1}, {0, 1}})})),
            {}, {}, "value");
  require(
      !failed.ok() && failed.status().detail.input_id == 11 &&
          failed.status().message == "sample violates typed semantic domain",
      "finite arithmetic retains remote typed validation");
}
void finite_arithmetic_preparation_and_cancel() {
  for (bool cancel : {false, true}) {
    Driver d;
    const auto original = d.registry;
    const std::string key = cancel ? "numeric.add" : "numeric.clamp";
    auto count = std::make_shared<unsigned>(0);
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                    ->register_operation(observed_numeric(
                        original, key, count, cancel ? stop : nullptr,
                        triggered, false, 3))
                    .ok() &&
                d.registry->freeze().ok(),
            "finite arithmetic observer registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto a = d.source({ElementType::Float32, {3}}, {float_bits(1)}, {1, {0}});
    auto b = d.source({ElementType::Float32, {3}}, {float_bits(2)}, {1, {0}});
    const auto baseline = d.root.statistics().live;
    {
      auto prepared = d.prepare(
          key,
          cancel ? std::vector<ResultRef>{a, b} : std::vector<ResultRef>{a},
          cancel
              ? std::map<std::string, ParameterValue>{}
              : std::map<std::string, ParameterValue>{{"min", 0.}, {"max", 2.}},
          "value");
      require(*count == 1, "finite arithmetic immutable preparation once");
      auto demand =
          take(d.context->open_demand(prepared.plan, prepared.bindings));
      auto result =
          demand.request({{"out", take(Footprint::all({3}))}}, stop->token());
      if (cancel) {
        require(*triggered && !result.ok() &&
                    result.status().code == ErrorCode::Cancelled &&
                    d.root.statistics().live[ResourceKind::Payload] ==
                        baseline[ResourceKind::Payload],
                "finite arithmetic kernel cancellation rolls back transaction");
      } else {
        require(read_bits(take(std::move(result)).results.at("out"), {0}) ==
                    float_bits(1),
                "prepared static clamp output");
        auto next =
            d.source({ElementType::Float32, {3}}, {float_bits(3)}, {1, {0}});
        prepared.bindings.inputs[0].result = next;
        require(demand.replace_bindings(prepared.bindings).ok(),
                "finite arithmetic rebind");
        auto changed =
            take(demand.request({{"out", take(Footprint::all({3}))}}));
        require(
            *count == 1 &&
                read_bits(changed.results.at("out"), {0}) == float_bits(2) &&
                changed.results.at("out").association()[0] == next.object_id(),
            "finite arithmetic reuses static bounds with fresh association");
      }
    }
    if (cancel) {
      d.context.reset();
      require(
          d.root.statistics().live.values == baseline.values,
          "finite arithmetic cancelled context releases all Root resources");
    }
  }
}

void lowpass_workflows() {
  for (bool continuous : {false, true})
    for (unsigned kernel = 0; kernel < 5; ++kernel)
      for (auto profile :
           {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon}) {
        Driver d;
        auto node = lowpass_node(continuous, kernel, profile, 0);
        auto x = d.source({ElementType::Float32, {3}},
                          {float_bits(2), float_bits(.75), 0}, {9, {-4}});
        auto y = d.source({ElementType::Float64, {2}},
                          {double_bits(-2), double_bits(1)}, {9, {0, -8}}, {},
                          {3}, nullptr, "custom.lowpass", 29);
        std::vector<ResultRef> inputs;
        if (continuous)
          inputs.push_back(x);
        inputs.push_back(y);
        std::vector<OperationMetadata> metadata;
        for (auto input : inputs) {
          OperationMetadata m;
          m.result_schema = std::make_shared<SchemaTemplate>(input.schema());
          metadata.push_back(std::move(m));
        }
        if (!math_profile_available(d, node, inputs))
          continue;
        auto roi =
            take(Footprint::from_regions({3, 2}, {Region({{1, 1}, {0, 1}})}));
        ResultRef output;
        {
          auto result = take(d.run(node.operation, inputs, roi, node.parameters,
                                   {}, continuous ? "samples" : "values"));
          output = result.results.at("out");
          const auto& spec = output.schema().tensors[0];
          require(output.schema().id == "photospider.tensor" &&
                      spec.key == "samples" &&
                      spec.sample_shape() == std::vector<uint64_t>({3, 2}) &&
                      spec.facets.empty() && output.resources().size() == 0,
                  "lowpass output preserves complete batch sample shape with "
                  "generic facets");
          require(take(output.descriptor()).tensor_coverage(0) ==
                      take(Footprint::all({3, 2})),
                  "lowpass sparse demand certifies complete Whole output");
          const auto support = take(result.dependencies.source_support());
          require(support.at(continuous ? "input1" : "input0") ==
                      take(Footprint::all({3, 2})),
                  "lowpass Whole complete source support");
          require(take(result.dependencies.potential_dirty(
                           continuous ? "input1" : "input0",
                           take(Footprint::from_regions(
                               {3, 2}, {Region({{2, 1}, {1, 1}})}))))
                          .at("out") == roi,
                  "lowpass remote edit invalidates recorded request");
        }
        inputs.clear();
        x = {};
        y = {};
        d.context.reset();
        for (unsigned i = 0; i < 3; ++i)
          for (unsigned j = 0; j < 2; ++j)
            require(
                read_bits(output, {i, j}) == double_bits(j ? -2 : 1),
                "lowpass independent columns and owning result after context");
        output = {};
        require(d.root.statistics().live[ResourceKind::Payload] == 0,
                "lowpass final escaped owner releases payload");
      }
}
void lowpass_boundaries() {
  const uint64_t snan = UINT64_C(0x7ff0000000000042);
  Driver d;
  auto node = lowpass_node(false, 0, CpuNumericProfile::Strict, 0);
  node.parameters["radius"] = int64_t{1};
  auto y = d.source({ElementType::Float64, {3}}, {snan, double_bits(7), snan},
                    {1, {8}});
  auto result = take(d.run(node.operation, {y}, {}, node.parameters));
  require(read_bits(result.results.at("out"), {1}) == double_bits(7) &&
              read_bits(result.results.at("out"), {0}) ==
                  (snan | UINT64_C(0x8000000000000)),
          "uniform exact-zero taps stay unused and sNaN quiets with payload");
  auto x = d.source({ElementType::Float32, {3}},
                    {0, float_bits(.75), float_bits(2)}, {1, {4}});
  node = lowpass_node(true, 4, CpuNumericProfile::Strict, 0);
  auto failed = d.run(node.operation, {x, y},
                      take(Footprint::from_regions({3}, {Region({{1, 1}})})),
                      node.parameters, {}, "samples");
  require(!failed.ok() &&
              failed.status().reason == FailureReason::InvalidDomain &&
              failed.status().detail.scope == FailureScope::Run,
          "nonuniform remote nonfinite endpoint fails Whole run");
  auto empty = take(d.run(node.operation, {x, y}, take(Footprint::none({3})),
                          node.parameters, {}, "samples"));
  require(
      take(empty.dependencies.source_support()).empty() &&
          take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
      "Empty lowpass skips all payload and arithmetic");
  for (unsigned k = 0; k < 5; ++k)
    for (bool narrow : {false, true}) {
      node = lowpass_node(true, k, CpuNumericProfile::Strict, 0,
                          numeric::LowpassBoundary::Zero);
      x = d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
      y = d.source({narrow ? ElementType::Float32 : ElementType::Float64, {2}},
                   {3}, {1, {0}});
      auto half = take(
          d.run(node.operation, {x, y}, {}, node.parameters, {}, "samples"));
      require(read_bits(half.results.at("out"), {0}) == 2 &&
                  read_bits(half.results.at("out"), {1}) == 2,
              "nonuniform zero boundary exact half-subnormal ties");
    }
  auto facet = take(encode_semantic(coverage_semantics()));
  y = d.source({ElementType::Float32, {3, 1}},
               {0, float_bits(.5), float_bits(2)}, {1, {4, 0}}, {facet});
  node = lowpass_node(false, 4, CpuNumericProfile::Strict, 0);
  failed =
      d.run(node.operation, {y},
            take(Footprint::from_regions({3, 1}, {Region({{1, 1}, {0, 1}})})),
            node.parameters);
  require(
      !failed.ok() && failed.status().detail.input_id == 11 &&
          failed.status().message == "sample violates typed semantic domain",
      "Whole lowpass remote typed input validation retains input id");
}
void lowpass_preparation_and_cancel() {
  for (bool continuous : {false, true})
    for (bool cancel : {false, true}) {
      Driver d;
      auto node = lowpass_node(continuous, 4, CpuNumericProfile::Strict, 0);
      const auto original = d.registry;
      auto count = std::make_shared<unsigned>(0);
      auto stop = std::make_shared<CancellationSource>();
      auto triggered = std::make_shared<bool>(false);
      d.registry = std::make_shared<OperationRegistry>();
      require(d.registry
                      ->register_operation(observed_numeric(
                          original, node.operation, count,
                          cancel ? stop : nullptr, triggered, false, 192))
                      .ok() &&
                  d.registry->freeze().ok(),
              "lowpass observer registration");
      ExecutionContextConfig config;
      config.managed_resources = ResourceLimits{};
      d.context = std::make_unique<ExecutionContext>(d.registry, config);
      d.root = take(d.context->resource_budget());
      auto x = d.source({ElementType::Float64, {3}},
                        {0, double_bits(.75), double_bits(2)}, {1, {8}});
      auto y =
          d.source({ElementType::Float64, {3}},
                   {double_bits(1), double_bits(-2), double_bits(4)}, {1, {8}});
      std::vector<ResultRef> inputs;
      if (continuous)
        inputs.push_back(x);
      inputs.push_back(y);
      const auto baseline = d.root.statistics().live;
      {
        auto prepared = d.prepare(node.operation, inputs, node.parameters,
                                  continuous ? "samples" : "values");
        require(*count == 1, "lowpass static preparation once");
        auto demand =
            take(d.context->open_demand(prepared.plan, prepared.bindings));
        auto result =
            demand.request({{"out", take(Footprint::all({3}))}}, stop->token());
        if (cancel) {
          require(*triggered && !result.ok() &&
                      result.status().code == ErrorCode::Cancelled &&
                      d.root.statistics().live[ResourceKind::Payload] ==
                          baseline[ResourceKind::Payload],
                  "lowpass cancellation in directed limb work rolls back "
                  "writer and vectors");
        } else {
          take(std::move(result));
          auto next = d.source({ElementType::Float64, {3}}, {double_bits(-0.)},
                               {1, {0}});
          prepared.bindings.inputs.back().result = next;
          require(demand.replace_bindings(prepared.bindings).ok(),
                  "lowpass replace values");
          auto changed =
              take(demand.request({{"out", take(Footprint::all({3}))}}));
          require(
              *count == 1 &&
                  read_bits(changed.results.at("out"), {1}) ==
                      double_bits(-0.) &&
                  changed.results.at("out").association().back() ==
                      next.object_id(),
              "lowpass preparation reused across dynamic value association");
        }
      }
      if (cancel) {
        d.context.reset();
        require(d.root.statistics().live.values == baseline.values,
                "lowpass cancelled context releases all Root resources");
      }
    }
}

void lowpass_period_cancel() {
  for (auto boundary :
       {numeric::LowpassBoundary::Wrap, numeric::LowpassBoundary::Reflect}) {
    Driver d;
    auto node = lowpass_node(true, 4, CpuNumericProfile::Strict, 0, boundary);
    node.parameters["support_radius"] = 1.;
    const auto original = d.registry;
    auto count = std::make_shared<unsigned>(0);
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                    ->register_operation(
                        observed_numeric(original, node.operation, count, stop,
                                         triggered, false, 8192))
                    .ok() &&
                d.registry->freeze().ok(),
            "lowpass period observer registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto positions = d.source({ElementType::Float64, {2}},
                              {double_bits(1), double_bits(1) + 1}, {1, {8}});
    auto values =
        d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
    const auto baseline = d.root.statistics().live;
    // 8192 is LowpassGeometry's period-piece charge, after exact coordinate
    // promotion and support construction, rather than scratch initialization.
    auto result = d.run(node.operation, {positions, values}, {},
                        node.parameters, stop->token(), "samples");
    require(*triggered && !result.ok() &&
                result.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "huge-period lowpass cancels during geometry partition and rolls "
            "back output");
    d.context.reset();
    require(d.root.statistics().live.values == baseline.values,
            "huge-period cancelled lowpass releases every Root resource");
  }
}

void shaper_preparation_and_cancel() {
  for (bool cancel : {false, true}) {
    Driver d;
    const auto original = d.registry;
    auto count = std::make_shared<unsigned>(0);
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    const std::string key = "curve.log2_shaper_strict";
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                ->register_operation(observed_numeric(original, key, count,
                                                      cancel ? stop : nullptr,
                                                      triggered, cancel))
                .ok(),
            "shaper preparation observer registration");
    require(d.registry->freeze().ok(), "shaper preparation observer freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto x = d.source({ElementType::Float64, {1}},
                      {double_bits(cancel ? 3 : 4)}, {1, {0}});
    auto l = d.source({ElementType::Float64, {1}}, {double_bits(1)}, {1, {0}});
    auto u = d.source({ElementType::Float64, {1}}, {double_bits(16)}, {1, {0}});
    const auto baseline = d.root.statistics().live;
    {
      auto prepared = d.prepare(key, {x, l, u});
      require(*count == 1, "shaper prepares immutable static state once");
      auto demand =
          take(d.context->open_demand(prepared.plan, prepared.bindings));
      auto result =
          demand.request({{"out", take(Footprint::all({1}))}}, stop->token());
      if (cancel) {
        require(*triggered && !result.ok() &&
                    result.status().code == ErrorCode::Cancelled &&
                    d.root.statistics().live[ResourceKind::Payload] ==
                        baseline[ResourceKind::Payload],
                "shaper refinement cancellation rolls back writer and exact "
                "scratch");
      } else {
        require(read_bits(take(std::move(result)).results.at("out"), {0}) ==
                    double_bits(.5),
                "prepared shaper reads dynamic input");
        auto next =
            d.source({ElementType::Float64, {1}}, {double_bits(2)}, {1, {0}});
        prepared.bindings.inputs[1].result = next;
        require(demand.replace_bindings(prepared.bindings).ok(),
                "shaper replaces bounds without static reparsing");
        auto changed =
            take(demand.request({{"out", take(Footprint::all({1}))}}));
        require(
            *count == 1 &&
                read_bits(changed.results.at("out"), {0}) ==
                    UINT64_C(0x3fd5555555555555) &&
                changed.results.at("out").association()[1] == next.object_id(),
            "prepared shaper reuses state while bound association changes");
      }
    }
    if (cancel) {
      d.context.reset();
      require(d.root.statistics().live.values == baseline.values,
              "cancelled shaper releases all context resources");
    }
  }
}
void color_ramp_active_cancel() {
  for (auto model : {ColorModel::Rgb, ColorModel::Xyz, ColorModel::Hsl}) {
    Driver d;
    const auto original = d.registry;
    auto node =
        ramp_node(model, model == ColorModel::Hsl ? ColorHueUnit::RationalPi
                                                  : ColorHueUnit::Radian);
    auto count = std::make_shared<unsigned>(0);
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                ->register_operation(observed_numeric(
                    original, node.operation, count, stop, triggered, false,
                    model == ColorModel::Rgb ? 640 : 144))
                .ok(),
            "ramp arithmetic cancellation observer registration");
    require(d.registry->freeze().ok(), "ramp cancellation observer freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto q = d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
    auto k =
        d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
    const unsigned channels = model == ColorModel::Hsl ? 2 : 3;
    std::vector<uint64_t> colors(channels * 2);
    std::fill(colors.begin() + channels, colors.end(), double_bits(1));
    auto c = d.source({ElementType::Float64, {2, channels}}, colors,
                      {1, {static_cast<int64_t>(channels * 8), 8}});
    std::vector<ResultRef> inputs{q, k, c};
    if (model == ColorModel::Hsl) {
      inputs.push_back(d.source({ElementType::Int64, {2}}, {0, 4}, {1, {8}}));
      inputs.push_back(d.source({ElementType::Int64, {2}}, {1}, {1, {0}}));
    }
    const auto baseline = d.root.statistics().live;
    auto failed =
        d.run(node.operation, inputs, {}, node.parameters, stop->token());
    require(*triggered && !failed.ok() &&
                failed.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "ramp active exact slot cancellation discards writer and Root stop "
            "index");
    d.context.reset();
    require(d.root.statistics().live.values == baseline.values,
            "ramp cancelled arithmetic releases all context resources");
  }
}
void lut3d_preparation() {
  Driver d;
  const auto original = d.registry;
  auto count = std::make_shared<unsigned>(0);
  const std::string key = "curve.apply_lut3d_trilinear_strict";
  d.registry = std::make_shared<OperationRegistry>();
  require(d.registry->register_operation(observed_numeric(original, key, count))
              .ok(),
          "LUT3D preparation observer registration");
  require(d.registry->freeze().ok(), "LUT3D preparation observer freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.context = std::make_unique<ExecutionContext>(d.registry, config);
  d.root = take(d.context->resource_budget());
  const auto color = lut3d_description();
  auto node = lut3d_node(false, color, color);
  auto input =
      d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)}, {1, {0, 0}});
  std::vector<uint64_t> identity;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 2; ++g)
      for (unsigned b = 0; b < 2; ++b)
        identity.insert(identity.end(),
                        {double_bits(r), double_bits(g), double_bits(b)});
  auto table = d.source({ElementType::Float64, {2, 2, 2, 3}}, identity,
                        {1, {96, 48, 24, 8}});
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
  auto prepared =
      d.prepare(node.operation, {input, table, axes}, node.parameters);
  const auto calls = *count;
  const auto owner = prepared.plan.steps()[0].prepared;
  require(calls && owner->state(), "LUT3D compilation owns static preparation");
  auto first = take(d.context->execute_fragments(
      take(d.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 3}))}}));
  require(*count == calls &&
              read_bits(first.results.at("out"), {0, 1}) == double_bits(.5),
          "LUT3D compiled start uses the owning preparation without reparsing");
  auto changed_input = d.source(
      {ElementType::Float64, {1, 3}},
      {double_bits(.25), double_bits(.75), double_bits(.5)}, {1, {24, 8}});
  for (auto& bits : identity) {
    double v;
    std::memcpy(&v, &bits, 8);
    bits = double_bits(v * 2);
  }
  auto changed_table = d.source({ElementType::Float64, {2, 2, 2, 3}}, identity,
                                {1, {96, 48, 24, 8}});
  prepared.bindings.inputs[0].result = changed_input;
  prepared.bindings.inputs[1].result = changed_table;
  auto second = take(d.context->execute_fragments(
      take(d.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 3}))}}));
  auto retained = second.results.at("out");
  require(*count == calls && prepared.plan.steps()[0].prepared == owner &&
              read_bits(retained, {0, 0}) == double_bits(.5) &&
              read_bits(retained, {0, 1}) == double_bits(1.5) &&
              read_bits(retained, {0, 2}) == double_bits(1) &&
              retained.association() ==
                  ResourceVector<uint64_t>{changed_input.object_id(),
                                           changed_table.object_id(),
                                           axes.object_id()},
          "LUT3D same-plan rebinding reuses preparation and observes new "
          "Result owners");
  d.context.reset();
  require(read_bits(retained, {0, 1}) == double_bits(1.5),
          "LUT3D rebinding output survives execution retirement");
}
void lut3d_resources() {
  for (bool tetra : {false, true}) {
    Driver d;
    const auto original = d.registry;
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    auto count = std::make_shared<unsigned>(0);
    const auto color = lut3d_description();
    const auto node = lut3d_node(tetra, color, color);
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                ->register_operation(observed_numeric(original, node.operation,
                                                      count, stop, triggered))
                .ok(),
            "LUT3D cancellation observer registration");
    require(d.registry->freeze().ok(), "LUT3D cancellation observer freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto point = d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)},
                          {1, {0, 0}});
    auto table = d.source({ElementType::Float32, {2, 2, 2, 3}}, {float_bits(1)},
                          {1, {0, 0, 0, 0}});
    auto axes = d.source({ElementType::Float64, {3, 3}},
                         {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
    const auto baseline = d.root.statistics().live;
    auto failed = d.run(node.operation, {point, table, axes}, {},
                        node.parameters, stop->token());
    require(*triggered && !failed.ok() &&
                failed.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "LUT3D cancellation after entering exact 640-limb work rolls back "
            "output");
    d.context.reset();
    require(d.root.statistics().live.values == baseline.values,
            "LUT3D cancelled continuation releases all Root resources");
    Driver limited(30000);
    point = limited.source({ElementType::Float64, {1, 3}}, {double_bits(.5)},
                           {1, {0, 0}});
    table = limited.source({ElementType::Float32, {2, 2, 2, 3}},
                           {float_bits(1)}, {1, {0, 0, 0, 0}});
    axes = limited.source({ElementType::Float64, {3, 3}},
                          {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
    const auto before = limited.root.statistics().live;
    failed =
        limited.run(node.operation, {point, table, axes}, {}, node.parameters);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::WorkLimit &&
                limited.root.statistics().live[ResourceKind::Payload] ==
                    before[ResourceKind::Payload],
            "LUT3D WorkLimit discards unpublished output");
    limited.context.reset();
    require(limited.root.statistics().live.values == before.values,
            "LUT3D WorkLimit retires all execution resources");
  }
  Driver d;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Payload] = 1048576;
  d.context = std::make_unique<ExecutionContext>(d.registry, config);
  d.root = take(d.context->resource_budget());
  auto point =
      d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)}, {1, {0, 0}});
  auto table = d.source({ElementType::Float32, {65, 65, 65, 3}},
                        {float_bits(7)}, {1, {0, 0, 0, 0}});
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(0x1p-6)}, {1, {0, 8}});
  auto node = lut3d_node(false, lut3d_description(), lut3d_description());
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto output =
      take(d.run(node.operation, {point, table, axes}, {}, node.parameters))
          .results.at("out");
  require(read_bits(output, {0, 0}) == double_bits(7) &&
              d.root.statistics().live[ResourceKind::Payload] == payload + 24,
          "LUT3D authorized zero-stride table avoids a >3 MiB packed copy "
          "under 1 MiB payload cap");
  auto many = d.source({ElementType::Float64, {65536, 3}}, {double_bits(.5)},
                       {1, {0, 0}});
  const auto baseline = d.root.statistics().live;
  auto failed = d.run(
      node.operation, {many, table, axes},
      take(Footprint::from_regions({65536, 3}, {Region({{0, 1}, {0, 1}})})),
      node.parameters);
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              d.root.statistics().live[ResourceKind::Payload] ==
                  baseline[ResourceKind::Payload],
          "one-component LUT3D request still requires complete 1.5 MiB output");
}
struct BakePlan {
  std::shared_ptr<GraphContext> graph;
  ExecutionPlan plan;
  ExecutionBindings bindings;
  numeric::BakedLut3d exports;
};
BakePlan bake_plan(Driver& d, ResultRef axis, ResultRef extras,
                   Lut3dInterpolation method, ElementType dtype,
                   bool square = false, double tolerance = 0,
                   CpuNumericProfile profile = CpuNumericProfile::Strict) {
  WorkflowDocument document;
  ExecutionBindings bindings;
  const auto bind = [&](uint64_t id, const char* name,
                        const ResultRef& result) {
    WorkflowInputDeclaration input;
    input.id = id;
    input.name = name;
    input.result_schema =
        std::make_shared<const SchemaTemplate>(result.schema());
    document.inputs.push_back(std::move(input));
    bindings.inputs.push_back({name, result});
  };
  bind(11, "axis", axis);
  auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {8}});
  bind(12, "shared", zero);
  if (extras.valid())
    bind(13, "extras", extras);
  numeric::Lut3dBakeOptions options;
  options.shape = {2, 2, 2};
  options.interpolation = method;
  options.atol = tolerance;
  options.rtol = 0;
  options.input_description = lut3d_description();
  options.output_description = options.input_description;
  options.source_pointwise = true;
  options.table_dtype = dtype;
  options.profile = profile;
  unsigned expansions = 0;
  const auto source = [&](WorkflowDocument& staged,
                          const numeric::Lut3dSourceInput& input)
      -> Result<WorkflowNodeOutput> {
    ++expansions;
    if (!square)
      return Result<WorkflowNodeOutput>(input.colors);
    auto ids = take(numeric::available_workflow_node_ids(
        staged, 2, {input.colors, WorkflowInputReference{12}}));
    staged.nodes.push_back(take(numeric::constant_node(
        ids[0], WorkflowInputReference{12}, input.descriptor.shape)));
    staged.nodes.push_back(
        {ids[1],
         "numeric.mix_strict",
         {WorkflowNodeOutput{ids[0], "values"}, input.colors, input.colors},
         {}});
    return Result<WorkflowNodeOutput>(WorkflowNodeOutput{ids[1], "values"});
  };
  auto exported = take(numeric::bake_lut3d(
      document, d.registry, WorkflowInputReference{11}, source, options,
      extras.valid()
          ? std::optional<
                numeric::Lut3dValidationPoints>{{WorkflowInputReference{13},
                                                 extras.schema()
                                                     .tensors[0]
                                                     .sample_shape()[0]}}
          : std::nullopt));
  require(expansions == 2, "bake source expands twice during authoring only");
  document.outputs = {
      {"table", exported.table.source_node, exported.table.source_port},
      {"axis", exported.axis.source_node, exported.axis.source_port},
      {"report", exported.report.source_node, exported.report.source_port}};
  for (const auto& node : document.nodes)
    if (node.operation == "curve.pack_lut3d" ||
        node.operation == "curve.unpack_lut3d")
      document.outputs.push_back(
          {node.operation == "curve.pack_lut3d" ? "owned" : "unpacked", node.id,
           node.operation == "curve.pack_lut3d" ? "table" : "values"});
  auto graph = std::make_shared<GraphContext>(std::move(document));
  auto compiled = take(Compiler(d.registry).compile(*graph));
  return {graph, std::move(compiled.plan), std::move(bindings), exported};
}
ExecutionOptions bake_execution() {
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(1) << 30;
  options.dependencies.maximum_work = UINT64_C(1) << 30;
  options.maximum_result_window_bytes = 72;
  return options;
}
void lut3d_baking_workflows() {
  for (auto method :
       {Lut3dInterpolation::Trilinear, Lut3dInterpolation::Tetrahedral})
    for (auto dtype : {ElementType::Float32, ElementType::Float64}) {
      Driver d(UINT64_MAX, 64 * 1048576);
      auto axes =
          d.source({ElementType::Float64, {3, 3}},
                   {double_bits(1), 0, double_bits(-1), 0, double_bits(1),
                    double_bits(1), 0, double_bits(1), double_bits(1)},
                   {1, {24, 8}});
      auto extras = d.source({ElementType::Float64, {2, 3}}, {double_bits(.5)},
                             {1, {0, 0}});
      auto prepared = bake_plan(d, axes, extras, method, dtype);
      auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
      const auto q = take(Footprint::from_regions(
          {2, 2, 2, 3}, {Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}})}));
      auto full_run = take(d.context->execute(prepared.plan, prepared.bindings,
                                              {}, bake_execution()));
      ResultRef linked_report;
      auto options = bake_execution();
      options.result_publication = [&](ValueRef source,
                                       const ResultRef& published) {
        if (source.node_id == prepared.exports.report.source_node)
          linked_report = published;
        return Status::success();
      };
      auto execution = take(d.context->execute_fragments(
          frozen,
          {{"table", q},
           {"axis", take(Footprint::all({3, 3}))},
           {"owned", take(Footprint::all({2, 2, 2, 3}))},
           {"unpacked", q}},
          {}, options));
      auto table = execution.results.at("table");
      auto owned = execution.results.at("owned");
      auto unpacked = execution.results.at("unpacked");
      auto report = linked_report;
      require(report.valid(),
              "gated report is observed in matching frozen execution");
      auto measured = take(read_lut3d_bake_report(report, 72));
      require(measured.passed && measured.validation_count == 3 &&
                  measured.failed_count == 0 &&
                  measured.first_failure_index == -1 &&
                  report.association().size() >= 6 &&
                  report.association()[2] == owned.object_id(),
              "identity bake passes centers and duplicate extras with owned "
              "table provenance");
      require(owned.schema().version == 2 && owned.schema().fields.empty() &&
                  owned.schema().tensors[0].key == "colors",
              "owned bake is canonical typed Result tensor");
      auto full = take(owned.acquire_tensor(take(owned.descriptor()), 0,
                                            Region::whole({2, 2, 2, 3})));
      const Region color({{1, 1}, {0, 1}, {1, 1}, {0, 3}});
      auto gated =
          take(table.acquire_tensor(take(table.descriptor()), 0, color));
      auto extracted =
          take(unpacked.acquire_tensor(take(unpacked.descriptor()), 0, color));
      require(
          full.storage_owner_token() == gated.storage_owner_token() &&
              full.storage_owner_token() == extracted.storage_owner_token(),
          "pack/unpack/quality gate preserve actual authorized table backing");
      require(read_bits(table, {1, 0, 1, 0}) == 0 &&
                  read_bits(table, {1, 0, 1, 2}) ==
                      (dtype == ElementType::Float32 ? float_bits(1)
                                                     : double_bits(1)),
              "gated Result coordinates preserve descending axes and local "
              "color closure");
      d.context.reset();
      require(read_bits(table, {1, 0, 1, 2}) == (dtype == ElementType::Float32
                                                     ? float_bits(1)
                                                     : double_bits(1)) &&
                  take(read_lut3d_bake_report(report, 72)).passed,
              "baked table view/report owners survive context retirement");
    }
}
BakePlan select_bake(Driver& d, const BakePlan& source,
                     const std::string& name) {
  auto document = source.graph->snapshot().document();
  const auto selected =
      std::find_if(document.outputs.begin(), document.outputs.end(),
                   [&](const auto& output) { return output.name == name; });
  require(selected != document.outputs.end(), "selected bake export exists");
  document.outputs = {*selected};
  auto graph = std::make_shared<GraphContext>(std::move(document));
  auto compiled = take(Compiler(d.registry).compile(*graph));
  return {graph, std::move(compiled.plan), source.bindings, source.exports};
}
ResultRef bake_report_copy(
    Driver& d, const ResultRef& source,
    std::optional<std::pair<unsigned, uint64_t>> corruption = {}) {
  auto associations = source.association();
  auto builder = take(ResultBuilder::start(
      d.root, source.schema(), "test.bake.report", {},
      std::vector<uint64_t>(associations.begin(), associations.end())));
  require(builder
              .bind_descriptor_relation(take(ResultRelation::cartesian(
                  d.root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})))
              .ok(),
          "bound report descriptor");
  auto descriptor = take(source.descriptor());
  for (unsigned field = 0; field < source.schema().fields.size(); ++field) {
    const auto count = descriptor.rows(field);
    auto bytes =
        take(take(source.prepare_read(descriptor, field, 0, count)).load(72));
    std::vector<uint8_t> changed(bytes->bytes().data(),
                                 bytes->bytes().data() + bytes->bytes().size());
    if (corruption && corruption->first == field)
      std::memcpy(changed.data(), &corruption->second,
                  std::min<size_t>(8, changed.size()));
    require(builder.append(field, count, {changed.data(), changed.size()}).ok(),
            "bound report field");
    require(builder
                .publish(field, count,
                         take(ResultRelation::cartesian(
                             d.root, count,
                             {0, 1, 0, 0, ResultSupportTarget::Field, field})),
                         {true, true, true, true})
                .ok(),
            "bound report field coverage");
  }
  return take(builder.seal());
}
void lut3d_baking_boundaries() {
  for (auto method :
       {Lut3dInterpolation::Trilinear, Lut3dInterpolation::Tetrahedral}) {
    Driver d(UINT64_MAX, 64 * 1048576);
    auto axes = d.source({ElementType::Float64, {3, 3}},
                         {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
    auto extras = d.source({ElementType::Float64, {2, 3}}, {double_bits(.5)},
                           {1, {0, 0}});
    auto square =
        bake_plan(d, axes, extras, method, ElementType::Float64, true, .1);
    auto report_only = select_bake(d, square, "report");
    auto report =
        take(d.context->execute(report_only.plan, report_only.bindings, {},
                                bake_execution()))
            .results.at("report");
    auto measured = take(read_lut3d_bake_report(report, 72));
    require(
        !measured.passed && measured.validation_count == 3 &&
            measured.failed_count == 3 && measured.first_failure_index == 0 &&
            measured.max_abs_error == std::array<double, 3>{.25, .25, .25} &&
            measured.max_error_index == std::array<int64_t, 3>{0, 0, 0} &&
            measured.first_failure_reference ==
                std::array<double, 3>{.25, .25, .25} &&
            measured.first_failure_lut == std::array<double, 3>{.5, .5, .5},
        "exact square bake errors retain earliest duplicate-point maxima and "
        "first failure");
    auto table_only = select_bake(d, square, "table");
    auto failed = d.context->execute(table_only.plan, table_only.bindings, {},
                                     bake_execution());
    require(!failed.ok() &&
                failed.status().reason == FailureReason::InvalidDomain &&
                failed.status().message.find(
                    "LutApproximationToleranceExceeded") != std::string::npos,
            "failed measured report gates table with original diagnostic");
    auto outside =
        d.source({ElementType::Float64, {1, 3}}, {double_bits(2)}, {1, {0, 0}});
    auto bad = bake_plan(d, axes, outside, method, ElementType::Float64);
    auto axis_only = select_bake(d, bad, "axis");
    require(
        d.context
            ->execute(axis_only.plan, axis_only.bindings, {}, bake_execution())
            .ok(),
        "axis-only skips invalid extra point and source");
    auto bad_report = select_bake(d, bad, "report");
    require(!d.context
                 ->execute(bad_report.plan, bad_report.bindings, {},
                           bake_execution())
                 .ok(),
            "report validates every extra point domain");
    auto identity = bake_plan(d, axes, {}, method, ElementType::Float64);
    std::vector<ResultRef> sampled_tables;
    auto options = bake_execution();
    const auto document = identity.graph->snapshot().document();
    uint64_t pack_node = 0;
    for (const auto& node : document.nodes)
      if (node.operation == "curve.pack_lut3d")
        pack_node = node.id;
    options.result_publication = [&](ValueRef source,
                                     const ResultRef& published) {
      if (source.node_id == pack_node)
        sampled_tables.push_back(published);
      return Status::success();
    };
    auto all =
        take(d.context->execute(identity.plan, identity.bindings, {}, options));
    auto valid = all.results.at("report");
    const auto associated = valid.association()[2];
    auto sampled = std::find_if(
        sampled_tables.begin(), sampled_tables.end(),
        [&](const auto& table) { return table.object_id() == associated; });
    require(sampled != sampled_tables.end(),
            "report retains its actual sampled table instance");
    auto table = *sampled;
    for (auto corruption :
         {std::make_pair(0U, uint64_t{2}), std::make_pair(3U, uint64_t{1}),
          std::make_pair(7U, uint64_t{0})}) {
      auto malformed = bake_report_copy(d, valid, corruption);
      auto gate = d.prepare("curve.gate_lut3d", {table, malformed});
      auto rejected = d.context->execute_fragments(
          take(d.context->freeze(gate.plan, gate.bindings)),
          {{"out", take(Footprint::all({2, 2, 2, 3}))}}, {}, bake_execution());
      require(!rejected.ok() &&
                  rejected.status().reason == FailureReason::InvalidDomain,
              "external report fields cannot bypass quality validation");
    }
    auto gate = d.prepare("curve.gate_lut3d", {table, valid});
    auto tiny = bake_execution();
    tiny.maximum_result_window_bytes = 24;
    auto rejected = d.context->execute_fragments(
        take(d.context->freeze(gate.plan, gate.bindings)),
        {{"out", take(Footprint::all({2, 2, 2, 3}))}}, {}, tiny);
    require(
        !rejected.ok() &&
            rejected.status().code == ErrorCode::ResourceExhausted &&
            rejected.status().reason == FailureReason::CapacityLimit,
        "bound report gate preserves capacity category for a 72-byte field");
    const Status sentinel{ErrorCode::ResourceExhausted,
                          "table validation work sentinel",
                          FailureReason::WorkLimit,
                          {FailureOrigin::Resource, FailureScope::Run}};
    auto status = validate_representation(table, d.root, 72, {},
                                          [&](uint64_t) { return sentinel; });
    require(status.code == sentinel.code && status.reason == sentinel.reason &&
                status.message == sentinel.message &&
                status.detail.origin == sentinel.detail.origin &&
                status.detail.scope == sentinel.detail.scope,
            "typed owned table validation preserves complete budget failure "
            "status");
  }
}
void lut3d_baking_authoring_and_resources() {
  Driver d(UINT64_MAX, 64 * 1048576);
  auto axis = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
  auto extras =
      d.source({ElementType::Float64, {70, 3}}, {double_bits(.5)}, {1, {0, 0}});
  auto prepared = bake_plan(d, axis, extras, Lut3dInterpolation::Tetrahedral,
                            ElementType::Float64);
  auto only = select_bake(d, prepared, "report");
  auto report =
      take(d.context->execute(only.plan, only.bindings, {}, bake_execution()))
          .results.at("report");
  require(take(read_lut3d_bake_report(report, 72)).validation_count == 71 &&
              take(read_lut3d_bake_report(report, 72)).passed,
          "bake measurements cross the 64-row Need boundary without losing "
          "duplicate points");
  WorkflowDocument document;
  WorkflowInputDeclaration declaration;
  declaration.id = 11;
  declaration.name = "axis";
  declaration.result_schema =
      std::make_shared<const SchemaTemplate>(axis.schema());
  document.inputs.push_back(declaration);
  numeric::Lut3dBakeOptions options;
  options.shape = {2, 2, 2};
  options.interpolation = Lut3dInterpolation::Trilinear;
  options.atol = options.rtol = 0;
  options.input_description = options.output_description = lut3d_description();
  options.source_pointwise = true;
  auto rejected = numeric::bake_lut3d(
      document, d.registry, WorkflowInputReference{11},
      [](WorkflowDocument& staged, const numeric::Lut3dSourceInput& input) {
        auto altered =
            std::make_shared<SchemaTemplate>(*staged.inputs[0].result_schema);
        altered->id = "altered.input";
        staged.inputs[0].result_schema = std::move(altered);
        return Result<WorkflowNodeOutput>(input.colors);
      },
      options);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              document.nodes.empty() &&
              document.inputs[0].result_schema->same_schema(axis.schema()),
          "source authoring cannot replace existing Result declarations and "
          "failure leaves document unchanged");
  auto identity = bake_plan(d, axis, {}, Lut3dInterpolation::Trilinear,
                            ElementType::Float64);
  SchemaTemplate split_schema;
  split_schema.id = "test.fragmented.colors";
  ResultTensorSpec split_tensor;
  split_tensor.key = "colors";
  split_tensor.descriptor = {ElementType::Float64, {2, 2, 2, 3}};
  split_tensor.facets = {take(encode_color_array(lut3d_description()))};
  split_tensor.atomic_trailing_axes = 1;
  split_schema.tensors.push_back(split_tensor);
  auto split_builder =
      take(ResultBuilder::start(d.root, split_schema, "test.fragmented"));
  require(split_builder
              .bind_descriptor_relation(take(ResultRelation::cartesian(
                  d.root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})))
              .ok(),
          "fragmented color descriptor");
  for (unsigned r = 0; r < 2; ++r) {
    auto bytes = take(d.root.allocator().allocate(96));
    const auto bits = double_bits(r + .25);
    for (unsigned j = 0; j < 12; ++j)
      std::memcpy(bytes.data() + 8 * j, &bits, 8);
    require(
        split_builder
            .publish_tensor(
                0, Region({{r, 1}, {0, 2}, {0, 2}, {0, 3}}),
                {0, {96, 48, 24, 8}, {r, 0, 0, 0}}, std::move(bytes).freeze(),
                take(ResultRelation::cartesian(
                    d.root, 24, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})),
                {true, true, true, true})
            .ok(),
        "fragmented color half");
  }
  auto split = take(split_builder.seal());
  std::map<std::string, ParameterValue> pack_parameters;
  for (const auto& node : identity.graph->snapshot().document().nodes)
    if (node.operation == "curve.pack_lut3d")
      pack_parameters = node.parameters;
  auto packed =
      take(d.run("curve.pack_lut3d", {split}, {}, pack_parameters, {}, "table"))
          .results.at("out");
  auto unpacked = take(d.run("curve.unpack_lut3d", {packed})).results.at("out");
  auto packed_window = take(packed.acquire_tensor(take(packed.descriptor()), 0,
                                                  Region::whole({2, 2, 2, 3})));
  auto unpacked_window = take(unpacked.acquire_tensor(
      take(unpacked.descriptor()), 0, Region::whole({2, 2, 2, 3})));
  auto source_half = take(split.acquire_tensor(
      take(split.descriptor()), 0, Region({{0, 1}, {0, 2}, {0, 2}, {0, 3}})));
  require(read_bits(packed, {1, 1, 1, 2}) == double_bits(1.25) &&
              packed_window.storage_owner_token() !=
                  source_half.storage_owner_token() &&
              packed_window.storage_owner_token() ==
                  unpacked_window.storage_owner_token(),
          "unavailable multi-owner packing materializes once and legal "
          "unpacking retains its actual view");
  auto table_only = select_bake(d, identity, "table");
  CancellationSource stop;
  bool triggered = false;
  auto execution = bake_execution();
  uint64_t pack_id = 0;
  for (const auto& node : identity.graph->snapshot().document().nodes)
    if (node.operation == "curve.pack_lut3d")
      pack_id = node.id;
  execution.result_publication = [&](ValueRef source, const ResultRef&) {
    if (source.node_id == pack_id) {
      triggered = true;
      stop.cancel();
    }
    return Status::success();
  };
  auto failed = d.context->execute(table_only.plan, table_only.bindings,
                                   stop.token(), execution);
  require(
      triggered && !failed.ok() && failed.status().code == ErrorCode::Cancelled,
      "active bake cancellation after sampled table publication rejects final "
      "gated output");
}
void expression_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.expression.fail";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "expression source registration");
  require(registry->freeze().ok(), "expression registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto start =
      d.source({ElementType::Float64, {1}}, {double_bits(3)}, {1, {0}});
  auto end = d.source({ElementType::Float64, {1}}, {double_bits(4)}, {1, {0}});
  auto coefficient =
      d.source({ElementType::Float64, {1}}, {double_bits(2)}, {1, {0}});
  auto node = take(numeric::sample_expression_node(
      7, "a*x", WorkflowInputReference{11}, WorkflowInputReference{12}, 5,
      {{"a", WorkflowInputReference{13}}}));
  auto prepared =
      d.prepare(node.operation, {start, end, coefficient}, node.parameters);
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.expression.fail", {}, {}});
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  document.outputs.push_back({"axis", 7, "axis"});
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto frozen = take(d.context->freeze(compiled.plan, prepared.bindings));
  auto result = take(d.context->execute_fragments(
      frozen, {{"axis", take(Footprint::all({3}))}}));
  require(*starts == 0 &&
              read_bits(result.results.at("axis"), {2}) == double_bits(.25),
          "axis-only never starts a failing coefficient producer");
  auto failed = d.context->execute_fragments(
      frozen, {{"out", take(Footprint::all({5}))}});
  require(!failed.ok() && failed.status().message == "must stay lazy" &&
              failed.status().detail.node_id == 3 && *starts == 1,
          "values propagate the required coefficient producer failure");
  *starts = 0;
  document.nodes.back().inputs[1] = WorkflowNodeOutput{3, "value"};
  document.nodes.back().inputs[2] = WorkflowInputReference{13};
  document.nodes.back().parameters["count"] = int64_t{1};
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1}))},
       {"axis", take(Footprint::all({3}))}}));
  require(*starts == 0 &&
              read_bits(result.results.at("out"), {0}) == double_bits(6) &&
              read_bits(result.results.at("axis"), {0}) == double_bits(3) &&
              read_bits(result.results.at("axis"), {1}) == double_bits(3) &&
              read_bits(result.results.at("axis"), {2}) == 0,
          "singleton values and axis exclude the failing end producer");
  auto association = result.results.at("axis").association();
  require(association == ResourceVector<uint64_t>{start.object_id()},
          "singleton axis association contains only active start owner");
  document.nodes.back().parameters["count"] = int64_t{5};
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::none({5}))},
       {"axis", take(Footprint::none({3}))}}));
  require(*starts == 0 &&
              take(result.results.at("out").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              take(result.results.at("axis").descriptor())
                  .tensor_coverage(0)
                  .empty(),
          "Empty expression outputs leave all failing producers unstarted");
}
void calculus_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  for (unsigned count : {1U, 2U}) {
    OperationDefinition producer;
    producer.key = "test.calculus.source" + std::to_string(count);
    auto& output = producer.traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = "photospider.tensor";
    output.output_schema.result_schema_version = 1;
    SchemaTemplate schema;
    schema.id = "photospider.tensor";
    ResultTensorSpec member;
    member.key = "samples";
    member.descriptor = {ElementType::Float32, {count}};
    schema.tensors.push_back(member);
    output.result_schema = schema;
    output.dependency_version = 2;
    output.continuation_bytes = sizeof(FailingProducer);
    output.maximum_dependency_stages = 1;
    producer.start_result = [starts](const auto&, const auto& allocator) {
      ++*starts;
      return ResultContinuation::make<FailingProducer>(allocator);
    };
    require(registry->register_operation(std::move(producer)).ok(),
            "calculus source registration");
  }
  require(registry->freeze().ok(), "calculus registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto scalar =
      d.source({ElementType::Float32, {1}}, {0xff800123}, {1, {INT64_MIN}});
  auto prepared =
      d.prepare("numeric.integrate_1d_strict", {scalar, scalar, scalar});
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.calculus.source1", {}, {}});
  document.nodes.back().inputs[0] = WorkflowNodeOutput{3, "value"};
  document.nodes.back().inputs[1] = WorkflowNodeOutput{3, "value"};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto result = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1}))}}));
  require(*starts == 0 &&
              read_bits(result.results.at("out"), {0}) == 0xff800123 &&
              result.results.at("out").association() ==
                  ResourceVector<uint64_t>{scalar.object_id()},
          "singleton integral leaves failing samples/step unstarted and "
          "associates only initial");
  for (const auto& observation :
       take(result.dependencies.source_observations()))
    require(observation.input == "input2",
            "singleton read witnesses name only initial");
  document.nodes.insert(document.nodes.begin(),
                        {4, "test.calculus.source2", {}, {}});
  document.nodes.back().inputs[0] = WorkflowNodeOutput{4, "value"};
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  auto frozen = take(d.context->freeze(compiled.plan, prepared.bindings));
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none({2}))}}));
  require(
      *starts == 0 &&
          take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
      "Empty nonsingleton integral also leaves producers unstarted");
  auto failed = d.context->execute_fragments(
      frozen,
      {{"out", take(Footprint::from_regions({2}, {Region({{0, 1}})}))}});
  require(
      !failed.ok() && failed.status().message == "must stay lazy" &&
          *starts != 0,
      "nonsingleton integral starts required producers even for output zero");
  OperationMetadata good, bad;
  good.result_schema = std::make_shared<SchemaTemplate>(scalar.schema());
  auto bad_schema = scalar.schema();
  bad_schema.tensors[0].descriptor.element_type = ElementType::UInt8;
  bad.result_schema = std::make_shared<SchemaTemplate>(bad_schema);
  auto rejected = registry->resolve_traits("numeric.integrate_1d_strict",
                                           {good, bad, good}, {});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "excluded singleton step still receives complete static dtype "
          "validation");
}
void sequence_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.sequence.other";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "sequence producer registration");
  require(registry->freeze().ok(), "sequence producer registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto start =
      d.source({ElementType::Float32, {1}}, {0x80000000}, {1, {INT64_MIN}});
  auto other = d.source({ElementType::Float64, {1}},
                        {UINT64_C(0x7ff0000000000001)}, {1, {0}});
  for (const auto* name :
       {"numeric.linspace_strict", "numeric.arange_strict"}) {
    auto prepared = d.prepare(name, {start, other},
                              {{"count", static_cast<int64_t>(1)},
                               {"dtype", std::string("float64")}});
    auto document = prepared.graph->snapshot().document();
    document.nodes.insert(document.nodes.begin(),
                          {3, "test.sequence.other", {}, {}});
    document.nodes.back().inputs[1] = WorkflowNodeOutput{3, "value"};
    document.outputs.push_back({"axis", 7, "axis"});
    auto graph = std::make_shared<GraphContext>(document);
    auto compiled = take(Compiler(registry).compile(*graph));
    auto result = take(d.context->execute_fragments(
        take(d.context->freeze(compiled.plan, prepared.bindings)),
        {{"out", take(Footprint::all({1}))},
         {"axis", take(Footprint::all({3}))}}));
    require(*starts == 0 &&
                read_bits(result.results.at("out"), {0}) ==
                    UINT64_C(0x8000000000000000) &&
                read_bits(result.results.at("axis"), {0}) ==
                    UINT64_C(0x8000000000000000) &&
                read_bits(result.results.at("axis"), {1}) ==
                    UINT64_C(0x8000000000000000) &&
                read_bits(result.results.at("axis"), {2}) == 0,
            "singleton values and axis read only start and never schedule "
            "failing end/step");
    document.nodes.back().parameters["count"] = static_cast<int64_t>(2);
    graph = std::make_shared<GraphContext>(document);
    compiled = take(Compiler(registry).compile(*graph));
    auto frozen = take(d.context->freeze(compiled.plan, prepared.bindings));
    const auto payload = d.root.statistics().live[ResourceKind::Payload];
    auto empty = take(d.context->execute_fragments(
        frozen, {{"out", take(Footprint::none({2}))},
                 {"axis", take(Footprint::none({3}))}}));
    require(*starts == 0 &&
                d.root.statistics().live[ResourceKind::Payload] == payload,
            "Empty sequence outputs leave every producer unstarted");
    auto failed = d.context->execute_fragments(
        frozen, {{"out", take(Footprint::all({2}))}});
    require(!failed.ok() && failed.status().message == "must stay lazy" &&
                *starts == 1 && failed.status().detail.node_id == 3,
            "nonsingleton sequence eagerly prepares its required end/step "
            "producer");
    *starts = 0;
    auto bad = d.source({ElementType::UInt8, {1}}, {0}, {1, {0}});
    OperationMetadata first_metadata, second_metadata;
    first_metadata.result_schema =
        std::make_shared<SchemaTemplate>(start.schema());
    second_metadata.result_schema =
        std::make_shared<SchemaTemplate>(bad.schema());
    auto rejected =
        registry->resolve_traits(name, {first_metadata, second_metadata},
                                 {{"count", static_cast<int64_t>(1)},
                                  {"dtype", std::string("float64")}});
    require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
            "excluded sequence input still receives complete static dtype "
            "validation");
  }
}
void indexing_upstream() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.index.updates";
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::Float64, {2}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(FailingProducer);
  output.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "indexing producer registration");
  require(registry->freeze().ok(), "indexing producer registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto base = d.source({ElementType::Float64, {2}}, {double_bits(1)}, {1, {0}});
  auto indices = d.source({ElementType::Int64, {2}}, {0}, {1, {0}});
  auto prepared =
      d.prepare("array.scatter_replace_strict", {base, indices, base},
                {{"axis", static_cast<int64_t>(0)}});
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(),
                        {3, "test.index.updates", {}, {}});
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto frozen = take(d.context->freeze(compiled.plan, prepared.bindings));
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none({2}))}}));
  require(*starts == 0 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "Empty scatter does not start computed updates");
  auto point = take(Footprint::from_regions({2}, {Region({{1, 1}})}));
  auto failed = d.context->execute_fragments(frozen, {{"out", point}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              failed.status().message == "must stay lazy" && *starts == 1 &&
              failed.status().detail.node_id == 3 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "Whole scatter evaluates updates even when Q only selects a no-hit "
          "base sample");
  auto batched = d.source({ElementType::Int64, {2}}, {0}, {1, {0, 0}}, {}, {1});
  OperationMetadata base_metadata, index_metadata;
  base_metadata.result_schema = std::make_shared<SchemaTemplate>(base.schema());
  index_metadata.result_schema =
      std::make_shared<SchemaTemplate>(batched.schema());
  auto rejected = registry->resolve_traits("array.gather_strict",
                                           {base_metadata, index_metadata},
                                           {{"axis", static_cast<int64_t>(0)}});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "index vector logical rank includes every batch axis");
}
struct BoundedMetadataPhase {
  ResultContinuation inner;
  uint64_t trigger, available, occurrence;
  explicit BoundedMetadataPhase(ResultContinuation value, uint64_t trigger = 80,
                                uint64_t available = 1024,
                                uint64_t occurrence = 1)
      : inner(std::move(value)),
        trigger(trigger),
        available(available),
        occurrence(occurrence) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    std::optional<ResourceLease> reservation;
    uint64_t hits = 0;
    bounded.consume_work = [&](uint64_t amount) {
      auto status = phase.consume_work(amount);
      if (status.ok() && amount == trigger && ++hits == occurrence &&
          !reservation) {
        const auto remaining =
            UINT64_C(1000000) -
            phase.resources.statistics().live[ResourceKind::Metadata];
        auto admitted = phase.resources.reserve(ResourceCapacity::host(
            remaining - available, remaining - available));
        if (!admitted.ok())
          return admitted.status();
        reservation = admitted.take_value();
      }
      return status;
    };
    return inner.poll(bounded);
  }
};
void ordering_boundaries() {
  Driver d;
  auto input = d.source({ElementType::Int64, {2, 3}}, {6, 5, 4, 3, 2, 1},
                        {41, {-24, -8}});
  auto sorted = take(d.run("array.sort_strict", {input}, {},
                           {{"axis", static_cast<int64_t>(0)}}))
                    .results.at("out");
  for (uint64_t j = 0; j < 3; ++j)
    require(read_bits(sorted, {0, j}) == j + 1 &&
                read_bits(sorted, {1, j}) == j + 4,
            "nonlast-axis sort traverses reversed logical lines");
  auto facet = take(encode_semantic(coverage_semantics()));
  auto bad =
      d.source({ElementType::Float32, {2, 2}},
               {0, 0x3f000000, 0x3f800000, 0x40000000}, {1, {8, 4}}, {facet});
  auto failed =
      d.run("array.sort_strict", {bad},
            take(Footprint::from_regions({2, 2}, {Region({{0, 1}, {0, 1}})})),
            {{"axis", static_cast<int64_t>(1)}});
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.origin == FailureOrigin::Domain &&
              failed.status().detail.input_id == 11,
          "sort validates typed samples outside the selected observation");
  CancellationSource cancellation;
  cancellation.cancel();
  failed = d.run("array.sort_strict", {input}, {},
                 {{"axis", static_cast<int64_t>(0)}}, cancellation.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "ordering pre-cancellation prevents sample work");
  Driver limited(3000);
  auto large = limited.source({ElementType::Int64, {64}}, {1}, {1, {0}});
  auto live = limited.root.statistics().live[ResourceKind::Payload];
  failed = limited.run("array.sort_strict", {large}, {},
                       {{"axis", static_cast<int64_t>(0)}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] == live,
          "ordering work exhaustion releases permutation, scratch and output");
  auto original_registry = d.registry;
  OperationDefinition definition;
  definition.key = "array.sort_strict";
  definition.traits = take(original_registry->find_traits(definition.key));
  definition.traits.requires_metadata_specialization = false;
  for (auto& output : definition.traits.outputs) {
    output.result_schema->tensors[0].descriptor.shape = {64};
    output.continuation_bytes += sizeof(BoundedMetadataPhase);
  }
  definition.traits.outputs[0]
      .result_schema->tensors[0]
      .descriptor.element_type = ElementType::Int64;
  definition.start_result = [original_registry](const auto& query,
                                                const auto& allocator) {
    auto forwarded = query;
    forwarded.prepared.reset();
    auto inner = original_registry->start_result("array.sort_strict", forwarded,
                                                 allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<BoundedMetadataPhase>(allocator,
                                                          inner.take_value());
  };
  auto registry = std::make_shared<OperationRegistry>();
  require(registry->register_operation(std::move(definition)).ok(),
          "bounded ordering registration");
  require(registry->freeze().ok(), "bounded ordering registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Metadata] = 1000000;
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto source = d.source({ElementType::Int64, {64}}, {1}, {1, {0}});
  auto prepared = d.prepare("array.sort_strict", {source},
                            {{"axis", static_cast<int64_t>(0)}});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto demand = take(Footprint::all({64}));
  // Populate the context's shared-call/producer directory before recording
  // the compute-stage rollback baseline. Empty executes no sample kernel.
  auto warm = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none({64}))}}));
  warm = {};
  live = d.root.statistics().live[ResourceKind::Payload];
  const auto metadata = d.root.statistics().live[ResourceKind::Metadata];
  failed = d.context->execute_fragments(frozen, {{"out", demand}});
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              failed.status().reason == FailureReason::CapacityLimit &&
              d.root.statistics().live[ResourceKind::Payload] == live &&
              d.root.statistics().live[ResourceKind::Metadata] == metadata,
          "two ordering metadata vectors require admission and fully retire on "
          "failure");
}
void indexing_metadata_failure() {
  for (bool second_vector : {false, true}) {
    const uint64_t count = second_vector ? 512 : 64;
    Driver d;
    const auto original_registry = d.registry;
    OperationDefinition definition;
    definition.key = "array.scatter_sum_strict";
    definition.traits = take(original_registry->find_traits(definition.key));
    definition.traits.requires_metadata_specialization = false;
    auto& output = definition.traits.outputs[0];
    output.result_schema->tensors[0].descriptor = {ElementType::Int64, {count}};
    output.continuation_bytes += sizeof(BoundedMetadataPhase);
    definition.start_result = [original_registry, second_vector](
                                  const auto& query, const auto& allocator) {
      auto forwarded = query;
      forwarded.prepared.reset();
      auto inner = original_registry->start_result("array.scatter_sum_strict",
                                                   forwarded, allocator);
      if (!inner.ok())
        return inner;
      return ResultContinuation::make<BoundedMetadataPhase>(
          allocator, inner.take_value(), second_vector ? 256 : 64,
          second_vector ? 8192 : 1024, second_vector ? 2 : 1);
    };
    auto registry = std::make_shared<OperationRegistry>();
    require(registry->register_operation(std::move(definition)).ok(),
            "bounded indexing registration");
    require(registry->freeze().ok(), "bounded indexing registry freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Metadata] = 1000000;
    d.registry = registry;
    d.context = std::make_unique<ExecutionContext>(registry, config);
    d.root = take(d.context->resource_budget());
    auto source = d.source({ElementType::Int64, {count}}, {1}, {1, {0}});
    auto indices = d.source({ElementType::Int64, {count}}, {0}, {1, {0}});
    auto prepared =
        d.prepare("array.scatter_sum_strict", {source, indices, source},
                  {{"axis", static_cast<int64_t>(0)}});
    auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
    auto demand = take(Footprint::all({count}));
    auto warm = take(d.context->execute_fragments(
        frozen, {{"out", take(Footprint::none({count}))}}));
    warm = {};
    const auto payload = d.root.statistics().live[ResourceKind::Payload];
    const auto metadata = d.root.statistics().live[ResourceKind::Metadata];
    auto failed = d.context->execute_fragments(frozen, {{"out", demand}});
    require(!failed.ok() &&
                failed.status().code == ErrorCode::ResourceExhausted &&
                failed.status().reason == FailureReason::CapacityLimit &&
                d.root.statistics().live[ResourceKind::Payload] == payload &&
                d.root.statistics().live[ResourceKind::Metadata] == metadata,
            "index-plan metadata rejection retires readers, radix state and "
            "output");
  }
}
void singleton_slice_controls() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.steps";
  auto& out = producer.traits.outputs[0];
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = "photospider.tensor";
  out.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec spec;
  spec.key = "samples";
  spec.descriptor = {ElementType::Int64, {2}};
  schema.tensors.push_back(spec);
  out.result_schema = schema;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(FailingProducer);
  out.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "step producer registration");
  require(registry->freeze().ok(), "step registry freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.registry = registry;
  d.context = std::make_unique<ExecutionContext>(registry, config);
  d.root = take(d.context->resource_budget());
  auto source = d.source({ElementType::Float64, {2, 3}},
                         {double_bits(1), double_bits(2), double_bits(3),
                          double_bits(4), double_bits(5), double_bits(6)},
                         {1, {24, 8}});
  auto starts_input = d.source({ElementType::Int64, {2}}, {1, 2}, {1, {8}});
  auto prepared = d.prepare(
      "array.slice_strict", {source, starts_input, starts_input},
      {{"counts", std::string("1,1")}, {"layout", std::string("view")}});
  auto document = prepared.graph->snapshot().document();
  document.nodes.insert(document.nodes.begin(), {3, "test.steps", {}, {}});
  document.nodes.back().inputs[2] = WorkflowNodeOutput{3, "value"};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(registry).compile(*graph));
  auto copied = take(d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 1}))}}));
  require(*starts == 0 &&
              read_bits(copied.results.at("out"), {0, 0}) == double_bits(6),
          "all singleton slice leaves the step producer unstarted");
  document.nodes.back().parameters["counts"] = std::string("1,2");
  graph = std::make_shared<GraphContext>(document);
  compiled = take(Compiler(registry).compile(*graph));
  auto failed = d.context->execute_fragments(
      take(d.context->freeze(compiled.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 2}))}});
  require(!failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
              *starts == 1,
          "mixed slice counts retain the whole step producer obligation");
}
void empty_computed_input() {
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<unsigned>(0);
  OperationDefinition producer;
  producer.key = "test.lazy";
  auto& out = producer.traits.outputs[0];
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = "photospider.tensor";
  out.output_schema.result_schema_version = 1;
  SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ResultTensorSpec spec;
  spec.key = "samples";
  spec.descriptor = {ElementType::Float32, {1}};
  schema.tensors.push_back(spec);
  out.result_schema = schema;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(FailingProducer);
  out.maximum_dependency_stages = 1;
  producer.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ResultContinuation::make<FailingProducer>(allocator);
  };
  require(registry->register_operation(std::move(producer)).ok(),
          "lazy registration");
  require(registry->freeze().ok(), "registry freeze");
  WorkflowDocument doc;
  doc.nodes = {{1, "test.lazy", {}, {}},
               {2, "numeric.abs_strict", {WorkflowNodeOutput{1, "value"}}, {}}};
  doc.outputs = {{"out", 2, "values"}};
  auto graph = std::make_shared<GraphContext>(doc);
  auto compiled = take(Compiler(registry).compile(*graph));
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto before = root.statistics().live[ResourceKind::Payload];
  auto result =
      take(context.execute_fragments(take(context.freeze(compiled.plan)),
                                     {{"out", take(Footprint::none({1}))}}));
  require(*starts == 0 &&
              root.statistics().live[ResourceKind::Payload] == before &&
              take(result.results.at("out").descriptor())
                  .tensor_coverage(0)
                  .empty(),
          "Empty Whole leaves failing computed producer unstarted");
}
}  // namespace
int main() {
  try {
    core_result_programs();
    finite_elementwise_results();
    staged_ordered_numeric();
    element_bits_and_strides();
    whole_and_resources();
    typed_and_batches();
    certified_profiles();
    comparison_and_selection();
    layout_views();
    layout_profiles();
    constant_and_broadcast();
    array_association_ownership();
    interpolation_workflows();
    interpolation_bits_and_batches();
    interpolation_boundaries();
    range_workflows();
    range_boundaries();
    ordering_workflows();
    scan_workflows();
    scan_numerics();
    scan_boundaries();
    calculus_workflows();
    calculus_boundaries();
    matrix_workflows();
    matrix_numerics_and_boundaries();
    curve_workflows();
    curve_boundaries();
    curve_metadata_and_identity();
    curve_composition();
    inverse_workflows();
    inverse_boundaries();
    inverse_composition();
    inverse_active_cancel();
    bezier_workflows();
    bezier_boundaries();
    bezier_projection();
    bezier_resources();
    lut1d_workflows();
    lut1d_boundaries();
    lut1d_composition();
    lut1d_resources();
    lut3d_workflows();
    lut3d_boundaries();
    lut3d_preparation();
    lut3d_resources();
    lut3d_baking_workflows();
    lut3d_baking_boundaries();
    lut3d_baking_authoring_and_resources();
    expression_workflows();
    expression_many_coefficients();
    expression_boundaries();
    reduction_workflows();
    reduction_boundaries();
    reduction_count_projection();
    sequence_workflows();
    sequence_authoring();
    result_shaper_authoring();
    finite_arithmetic_workflows();
    finite_arithmetic_boundaries();
    finite_arithmetic_preparation_and_cancel();
    lowpass_workflows();
    lowpass_boundaries();
    lowpass_preparation_and_cancel();
    lowpass_period_cancel();
    shaper_workflows();
    shaper_boundaries();
    color_ramp_workflows();
    color_ramp_icc();
    color_ramp_boundaries();
    color_ramp_active_cancel();
    shaper_preparation_and_cancel();
    sequence_boundaries();
    sequence_projection();
    calculus_projection();
    expression_projection();
    indexing_workflows();
    concatenate_views();
    concatenate_many_ports();
    indexing_boundaries();
    indexing_metadata_failure();
    indexing_upstream();
    ordering_projection();
    ordering_boundaries();
    paged_layout_copy();
    paged_layout_failure();
    joined_resource_failure();
    singleton_slice_controls();
    empty_computed_input();
    std::cout << "Result numeric math workflows passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
