#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/execution_sync_fixture.hpp"
#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
void active_delay_cancel();
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
      if (!changed.wait_for(lock, std::chrono::seconds(15),
                            [&] { return resume; }))
        return Status{ErrorCode::OperationFailed, "publication gate timeout"};
    }
    return Status::success();
  };
  CancellationSource producer_stop;
  ps::test::SharedJoinEvent joined_event("core.gpu_fallback_probe", 7);
  std::future<Result<ExecutionResult>> producer, waiter;
  ps::test::OnExit cleanup([&] {
    producer_stop.cancel();
    {
      std::lock_guard<std::mutex> lock(mutex);
      resume = true;
      changed.notify_all();
    }
    if (producer.valid())
      producer.wait();
    if (waiter.valid())
      waiter.wait();
  });
  producer = std::async(std::launch::async, [&] {
    return driver.context->execute(shared_frozen, producer_stop.token(),
                                   options);
  });
  bool callback_entered;
  {
    std::unique_lock<std::mutex> lock(mutex);
    callback_entered = changed.wait_for(lock, std::chrono::seconds(5),
                                        [&] { return entered; });
  }
  waiter = std::async(std::launch::async,
                      [&] { return driver.context->execute(shared_frozen); });
  const bool joined = joined_event.wait();
  producer_stop.cancel();
  {
    std::lock_guard<std::mutex> lock(mutex);
    resume = true;
  }
  changed.notify_all();
  auto retired = producer.get();
  auto survived = take(waiter.get());
  require(
      callback_entered && joined &&
          retired.status().code == ErrorCode::Cancelled &&
          read_bits(survived.results.at("out"), {2}) == 9 &&
          survived.diagnostics.shared_computations >= 1 &&
          survived.diagnostics.selected_backends.at({7, 0}) == Backend::Cpu,
      "shared fallback waiter survives producer cancellation and reports CPU");
  auto delayed = driver.prepare("core.delay", {source},
                                {{"milliseconds", INT64_C(200)}}, "value");
  auto delay_frozen =
      take(driver.context->freeze(delayed.plan, delayed.bindings));
  active_delay_cancel();
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
void active_delay_cancel() {
  auto original = make_default_operation_registry();
  auto registry = std::make_shared<OperationRegistry>();
  auto preparations = std::make_shared<unsigned>(0);
  auto stop = std::make_shared<CancellationSource>();
  auto observed = std::make_shared<bool>(false);
  require(
      registry
          ->register_operation(observed_numeric(
              original, "core.delay", preparations, stop, observed, false, 1))
          .ok(),
      "observed delay registration");
  require(registry->freeze().ok(), "observed delay freeze");
  Driver driver(UINT64_MAX, 16 * 1048576, 0, registry);
  auto source = driver.source({ElementType::UInt8, {3}}, {3, 7, 9}, {1, {1}});
  auto prepared = driver.prepare("core.delay", {source},
                                 {{"milliseconds", INT64_C(200)}}, "value");
  auto result =
      driver.context->execute(prepared.plan, prepared.bindings, stop->token());
  require(*observed && result.status().code == ErrorCode::Cancelled,
          "Result delay cancellation during admitted callback work");
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
    element_bits_and_strides();
    whole_and_resources();
    typed_and_batches();
    certified_profiles();
    comparison_and_selection();
    finite_arithmetic_workflows();
    finite_arithmetic_boundaries();
    finite_arithmetic_preparation_and_cancel();
    empty_computed_input();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
