#include <algorithm>
#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "image_vertical/image_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using ps::Backend;
using ps::CancellationSource;
using ps::Compiler;
using ps::CorrectnessObservation;
using ps::DataDefinitionRegistry;
using ps::ElementType;
using ps::ErrorCode;
using ps::ExecutionBindings;
using ps::ExecutionContext;
using ps::ExecutionPlan;
using ps::ExecutionResult;
using ps::GraphContext;
using ps::make_default_operation_registry;
using ps::OperationDefinition;
using ps::OperationInvocation;
using ps::OperationPortKind;
using ps::OperationRegionRule;
using ps::OperationRegistry;
using ps::OperationShapeRule;
using ps::OperationTraits;
using ps::PlanningOptions;
using ps::PlanStepInput;
using ps::PlanWorkflowInput;
using ps::RawBenchmarkOptions;
using ps::RawBenchmarkRunner;
using ps::Region;
using ps::Result;
using ps::StridedLayout;
using ps::Value;
using ps::ValueDescriptor;
using ps::ValueFacet;
using ps::WorkflowDocument;
using ps::WorkflowInputReference;
using ps::WorkflowNodeOutput;

int dynamic_bindings() {
  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  GraphContext graph(s1_fixture::document());
  auto compiled = compiler.compile(graph, s1_fixture::demand());
  PS_CHECK(compiled.ok());
  const auto& workflow = compiled.value();
  PS_CHECK(workflow.semantic.input_declarations().size() == 3);
  PS_CHECK(workflow.optimized.input_declarations().size() == 3);
  PS_CHECK(workflow.plan.input_declarations().size() == 3);
  PS_CHECK(std::get<PlanWorkflowInput>(workflow.plan.steps()[0].inputs[0])
               .declaration_index == 0);
  PS_CHECK(
      std::get<PlanStepInput>(workflow.plan.steps()[1].inputs[0]).step_index ==
      0);
  ExecutionContext execution(operations, {2, false, 16, 1024});
  for (bool second : {false, true}) {
    auto result =
        execution.execute(workflow.plan, s1_fixture::bindings(second));
    PS_CHECK(result.ok());
    PS_CHECK(s1_fixture::oracle(result.value(), second));
    PS_CHECK(result.value().diagnostics.operation_timings.size() == 2);
    PS_CHECK(result.value().diagnostics.transfer_count == 0);
    PS_CHECK(result.value().diagnostics.peak_live_bytes == 48);
    PS_CHECK(result.value().diagnostics.plan_digest ==
             workflow.plan.digest().value);
  }
  for (const auto& step : workflow.plan.steps()) {
    PS_CHECK(step.planned_bytes == 16);
    PS_CHECK(step.input_demands[0].dimensions()[1].offset == 1);
    PS_CHECK(step.input_demands[0].dimensions()[0].extent == 1);
    PS_CHECK(step.input_demands[0].dimensions()[2].extent == 4);
    PS_CHECK(step.input_demands[1].rank() == 1);
    PS_CHECK(step.input_demands[1].dimensions()[0].extent == 1);
  }
  auto reordered = s1_fixture::bindings();
  std::reverse(reordered.inputs.begin(), reordered.inputs.end());
  auto reordered_result = execution.execute(workflow.plan, reordered);
  PS_CHECK(reordered_result.ok() &&
           s1_fixture::oracle(reordered_result.value()));
  for (std::size_t changed = 0; changed < 3; ++changed) {
    auto a = s1_fixture::bindings();
    a.inputs[changed] = s1_fixture::bindings(true).inputs[changed];
    auto result = execution.execute(workflow.plan, a);
    PS_CHECK(result.ok());
    auto original = execution.execute(workflow.plan, s1_fixture::bindings());
    PS_CHECK(original.ok() && s1_fixture::oracle(original.value()));
    PS_CHECK(result.value().values.at("result").bytes() !=
             original.value().values.at("result").bytes());
  }
  auto zero = s1_fixture::bindings();
  zero.inputs[2].value = s1_fixture::scalar(0);
  auto result = execution.execute(workflow.plan, zero);
  PS_CHECK(result.ok());
  for (auto byte : result.value().values.at("result").bytes())
    PS_CHECK(byte == 0);
  auto first = std::async(std::launch::async, [&] {
    return execution.execute(workflow.plan, s1_fixture::bindings());
  });
  auto second = std::async(std::launch::async, [&] {
    return execution.execute(workflow.plan, s1_fixture::bindings(true));
  });
  // A separate context gives both simultaneous Runs adequate modeled capacity.
  // This bounded context may reject one reservation; test concurrent
  // correctness below.
  auto r1 = first.get();
  auto r2 = second.get();
  PS_CHECK(r1.ok() || r1.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(r2.ok() || r2.status().code == ErrorCode::ResourceExhausted);
  if (r1.ok())
    PS_CHECK(s1_fixture::oracle(r1.value()));
  if (r2.ok())
    PS_CHECK(s1_fixture::oracle(r2.value(), true));
  RawBenchmarkOptions options;
  options.iterations = 2;
  options.bindings = s1_fixture::bindings(true);
  options.planning = s1_fixture::demand();
  options.oracle_name = "s1-rgba32f-exposure-opacity-v1";
  options.correctness_oracle = [](const ExecutionResult& r) {
    return CorrectnessObservation{s1_fixture::oracle(r, true),
                                  "binary32 exact"};
  };
  RawBenchmarkRunner runner(&compiler, &execution);
  auto report = runner.run(graph, options);
  PS_CHECK(report.ok() && report.value().samples.size() == 2);
  for (const auto& sample : report.value().samples)
    PS_CHECK(sample.correctness_accepted);
  ExecutionContext limited(operations, {1, false, 16, 47});
  PS_CHECK(
      limited.execute(workflow.plan, s1_fixture::bindings()).status().code ==
      ErrorCode::ResourceExhausted);
  return 0;
}

int binding_failures() {
  auto operations = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> callbacks{0};
  OperationTraits traits;
  traits.input_count = 2;
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Elementwise;
  traits.input_schema = {
      {OperationPortKind::LinearPremultipliedRgbaFloat32, 0, 0},
      {OperationPortKind::Float32Scalar, 0, 16}};
  traits.output_schema = traits.input_schema[0];
  PS_CHECK(
      operations
          ->register_operation({"image.exposure_gain", traits,
                                [&](const OperationInvocation& invocation) {
                                  ++callbacks;
                                  return Result<Value>(invocation.inputs[0]);
                                }})
          .ok());
  traits.input_schema[1].maximum = 1;
  PS_CHECK(
      operations
          ->register_operation({"image.opacity", traits,
                                [&](const OperationInvocation& invocation) {
                                  ++callbacks;
                                  return Result<Value>(invocation.inputs[0]);
                                }})
          .ok());
  operations->freeze();
  Compiler compiler(operations);
  GraphContext graph(s1_fixture::document());
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(operations);
  const auto fail = [&](ExecutionBindings bindings, ErrorCode expected) {
    callbacks = 0;
    auto result = execution.execute(compiled.value().plan, std::move(bindings));
    return !result.ok() && result.status().code == expected && callbacks == 0;
  };
  auto input = s1_fixture::bindings();
  input.inputs.pop_back();
  PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  input = s1_fixture::bindings();
  input.inputs.push_back({"extra", s1_fixture::scalar(1)});
  PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  input = s1_fixture::bindings();
  input.inputs.push_back(input.inputs[0]);
  PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  for (const std::string& name : std::vector<std::string>{
           "", "bad name", "\x7f", "\xc3\xa9", std::string(129, 'a')}) {
    input = s1_fixture::bindings();
    input.inputs[0].name = name;
    PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  }
  input = s1_fixture::bindings();
  input.inputs[0].value = Value{};
  PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  const Value original = s1_fixture::bindings().inputs[0].value;
  auto check_value = [&](ValueDescriptor descriptor, Region region,
                         StridedLayout layout, std::vector<std::uint8_t> bytes,
                         std::vector<ValueFacet> facets) {
    auto candidate =
        Value::create(std::move(descriptor), std::move(region),
                      std::move(layout), std::move(bytes), std::move(facets));
    if (!candidate.ok())
      return false;
    auto bindings = s1_fixture::bindings();
    bindings.inputs[0].value = candidate.take_value();
    return fail(std::move(bindings), ErrorCode::TypeMismatch);
  };
  PS_CHECK(check_value({ElementType::UInt8, {2, 2, 4}}, original.region(),
                       original.layout(), original.copy_bytes(),
                       original.facets()));
  PS_CHECK(check_value({ElementType::Float32, {1, 4, 4}},
                       Region::whole({1, 4, 4}), {0, {64, 16, 4}},
                       original.copy_bytes(), original.facets()));
  for (auto region :
       {Region({{0, 1}, {0, 2}, {0, 4}}), Region({{0, 0}, {0, 2}, {0, 4}})})
    PS_CHECK(check_value(original.descriptor(), region, original.layout(),
                         original.copy_bytes(), original.facets()));
  for (auto layout :
       {StridedLayout{32, {-32, 16, 4}}, StridedLayout{0, {0, 16, 4}},
        StridedLayout{0, {36, 16, 4}}, StridedLayout{4, {32, 16, 4}}}) {
    auto bytes = original.copy_bytes();
    bytes.resize(68);
    PS_CHECK(check_value(original.descriptor(), original.region(), layout,
                         bytes, original.facets()));
  }
  auto trailing = original.copy_bytes();
  trailing.push_back(0);
  PS_CHECK(check_value(original.descriptor(), original.region(),
                       original.layout(), trailing, original.facets()));
  for (int mode = 0; mode < 5; ++mode) {
    auto facets = original.facets();
    if (mode == 0)
      facets.clear();
    if (mode == 1)
      facets.push_back({"extra", 1, {}});
    if (mode == 2)
      facets[0].key = "different";
    if (mode == 3)
      ++facets[0].version;
    if (mode == 4)
      facets[0].payload.push_back(0);
    PS_CHECK(check_value(original.descriptor(), original.region(),
                         original.layout(), original.copy_bytes(), facets));
  }
  for (std::size_t port : {1U, 2U}) {
    for (float number : {-1.F, 17.F, std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::quiet_NaN()}) {
      input = s1_fixture::bindings();
      input.inputs[port].value = s1_fixture::scalar(number);
      PS_CHECK(fail(input, ErrorCode::InvalidArgument));
    }
    input = s1_fixture::bindings();
    input.inputs[port].value = Value::from_float64(1);
    PS_CHECK(fail(input, ErrorCode::TypeMismatch));
    input = s1_fixture::bindings();
    input.inputs[port].value = s1_fixture::value({1, 1}, {2}, false);
    PS_CHECK(fail(input, ErrorCode::TypeMismatch));
  }
  input = s1_fixture::bindings();
  input.inputs[2].value = s1_fixture::scalar(1.5F);
  PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  for (int mode = 0; mode < 6; ++mode) {
    std::vector<float> pixels(16, 0);
    pixels[3] = 1;
    if (mode == 0)
      pixels[0] = -1;
    if (mode == 1)
      pixels[1] = std::numeric_limits<float>::infinity();
    if (mode == 2)
      pixels[2] = std::numeric_limits<float>::quiet_NaN();
    if (mode == 3)
      pixels[3] = 1.1F;
    if (mode == 4)
      pixels[3] = -1;
    if (mode == 5) {
      pixels[3] = 0;
      pixels[0] = 1;
    }
    input = s1_fixture::bindings();
    input.inputs[0].value = s1_fixture::value(pixels);
    PS_CHECK(fail(input, ErrorCode::InvalidArgument));
  }
  CancellationSource cancellation;
  cancellation.cancel();
  PS_CHECK(execution.execute(compiled.value().plan, {}, cancellation.token())
               .status()
               .code == ErrorCode::Cancelled);
  PS_CHECK(execution.execute(ExecutionPlan{}, {}, cancellation.token())
               .status()
               .code == ErrorCode::Stale);
  ExecutionContext foreign(make_default_operation_registry());
  PS_CHECK(foreign.execute(compiled.value().plan, {}, cancellation.token())
               .status()
               .code == ErrorCode::Stale);
  graph.replace(s1_fixture::document());
  PS_CHECK(execution.execute(compiled.value().plan, {}, cancellation.token())
               .status()
               .code == ErrorCode::Stale);
  return 0;
}

int compile_failures_and_identity() {
  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  const auto analyze_code = [&](WorkflowDocument doc) {
    GraphContext graph(std::move(doc));
    return compiler.analyze(graph.snapshot()).status().code;
  };
  for (int mode = 0; mode < 12; ++mode) {
    auto doc = s1_fixture::document();
    if (mode == 0)
      doc.schema_version = 1;
    if (mode == 1)
      doc.inputs[0].id = 0;
    if (mode == 2)
      doc.inputs[1].id = doc.inputs[0].id;
    if (mode == 3)
      doc.inputs[1].name = doc.inputs[0].name;
    if (mode == 4)
      doc.inputs[0].name = "bad name";
    if (mode == 5)
      doc.inputs[0].layout.byte_offset = 4;
    if (mode == 6)
      doc.inputs[0].layout.byte_strides[0] = 0;
    if (mode == 7)
      doc.inputs[0].region = Region({{0, 1}, {0, 2}, {0, 4}});
    if (mode == 8)
      doc.inputs[0].facets.push_back(doc.inputs[0].facets[0]);
    if (mode == 9)
      doc.inputs[0].facets[0].version = 0;
    if (mode == 10)
      doc.inputs[0].descriptor.element_type = static_cast<ElementType>(999);
    if (mode == 11)
      doc.inputs.resize(4097);
    PS_CHECK(analyze_code(doc) == ErrorCode::InvalidArgument);
  }
  auto doc = s1_fixture::document();
  doc.inputs[0].descriptor.shape = {UINT64_MAX};
  PS_CHECK(analyze_code(doc) == ErrorCode::ResourceExhausted);
  doc = s1_fixture::document();
  doc.nodes[0].inputs[0] = WorkflowInputReference{999};
  PS_CHECK(analyze_code(doc) == ErrorCode::NotFound);
  doc = s1_fixture::document();
  doc.nodes[0].inputs[0] = WorkflowNodeOutput{999, "value"};
  PS_CHECK(analyze_code(doc) == ErrorCode::NotFound);
  doc = s1_fixture::document();
  doc.nodes[0].inputs[1] = WorkflowNodeOutput{20, "value"};
  PS_CHECK(analyze_code(doc) == ErrorCode::Cycle);
  doc = s1_fixture::document();
  doc.nodes[1].inputs[1] = WorkflowNodeOutput{10, "value"};
  PS_CHECK(analyze_code(doc) == ErrorCode::InvalidArgument);
  doc = s1_fixture::document();
  doc.inputs[0].facets.clear();
  PS_CHECK(analyze_code(doc) == ErrorCode::TypeMismatch);
  GraphContext graph(s1_fixture::document());
  auto first = compiler.compile(graph);
  PS_CHECK(first.ok());
  doc = s1_fixture::document();
  std::reverse(doc.inputs.begin(), doc.inputs.end());
  GraphContext reordered(doc);
  auto second = compiler.compile(reordered);
  PS_CHECK(second.ok());
  PS_CHECK(first.value().semantic.digest().value ==
           second.value().semantic.digest().value);
  PS_CHECK(first.value().optimized.digest().value ==
           second.value().optimized.digest().value);
  PS_CHECK(first.value().plan.digest().value ==
           second.value().plan.digest().value);
  doc = s1_fixture::document();
  doc.inputs[0].name = "renamed";
  GraphContext renamed(doc);
  auto changed = compiler.compile(renamed);
  PS_CHECK(changed.ok());
  PS_CHECK(first.value().semantic.digest().value !=
           changed.value().semantic.digest().value);
  PS_CHECK(first.value().plan.cache_key().value !=
           changed.value().plan.cache_key().value);
  // Input id changes preserve namespace separation but change static identity.
  doc = s1_fixture::document();
  doc.inputs[0].id = 10;
  doc.nodes[0].inputs[0] = WorkflowInputReference{10};
  GraphContext id_changed(doc);
  auto id_result = compiler.compile(id_changed);
  PS_CHECK(id_result.ok());
  PS_CHECK(id_result.value().plan.digest().value !=
           first.value().plan.digest().value);
  doc = s1_fixture::document();
  doc.outputs.push_back({"whole", 20, "value"});
  GraphContext aliased(doc);
  auto all = compiler.compile(aliased);
  PS_CHECK(all.ok());
  auto subregion = compiler.plan(all.value().optimized, s1_fixture::demand());
  PS_CHECK(subregion.ok());
  PS_CHECK(subregion.value().digest().value != all.value().plan.digest().value);
  PS_CHECK(subregion.value().cache_key().value !=
           all.value().plan.cache_key().value);
  // Metadata-only maximum dense input is accepted; the complete two-image
  // working-set sum overflows.
  doc = s1_fixture::document();
  doc.inputs[0].descriptor.shape = {UINT64_C(1) << 59, 1, 4};
  doc.inputs[0].region = Region::whole(doc.inputs[0].descriptor.shape);
  doc.inputs[0].layout = {0, {16, 16, 4}};
  GraphContext enormous(doc);
  auto analyzed = compiler.analyze(enormous.snapshot());
  PS_CHECK(analyzed.ok());
  auto optimized = compiler.optimize(analyzed.value());
  PS_CHECK(optimized.ok());
  PS_CHECK(compiler.plan(optimized.value()).status().code ==
           ErrorCode::ResourceExhausted);
  auto requested = compiler.plan(first.value().optimized, s1_fixture::demand());
  PS_CHECK(requested.ok());
  PS_CHECK(requested.value().digest().value !=
           first.value().plan.digest().value);
  for (auto region :
       {Region({{0, 0}, {0, 2}, {0, 4}}), Region({{0, 3}, {0, 2}, {0, 4}}),
        Region({{0, 1}, {0, 1}, {1, 3}})}) {
    PlanningOptions options;
    options.output_regions["result"] = region;
    PS_CHECK(compiler.plan(first.value().optimized, options).status().code ==
             ErrorCode::InvalidArgument);
  }
  PlanningOptions unknown;
  unknown.output_regions["missing"] = Region::whole({2, 2, 4});
  PS_CHECK(compiler.plan(first.value().optimized, unknown).status().code ==
           ErrorCode::InvalidArgument);
  return 0;
}

int static_constraint_identity() {
  std::vector<std::string> semantic, physical;
  for (int mode = 0; mode < 6; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    OperationTraits traits;
    traits.input_count = 1;
    traits.input_schema.resize(1);
    traits.shape_rule = OperationShapeRule::PreserveFirstInput;
    traits.output_element_type = ElementType::Float32;
    if (mode == 4)
      traits.input_schema[0] = {OperationPortKind::Float32Scalar, 0, 1};
    if (mode == 5)
      traits.input_schema[0] = {OperationPortKind::Float32Scalar, 0, 2};
    if (mode >= 4)
      traits.shape_rule = OperationShapeRule::Scalar;
    PS_CHECK(registry
                 ->register_operation({"source", traits,
                                       [](const OperationInvocation& call) {
                                         return Result<Value>(call.inputs[0]);
                                       }})
                 .ok());
    registry->freeze();
    Compiler compiler(registry);
    WorkflowDocument doc;
    doc.inputs = {s1_fixture::declaration(1, "input", s1_fixture::scalar(1)),
                  s1_fixture::declaration(2, "unused", s1_fixture::scalar(1))};
    if (mode == 1)
      doc.inputs[1].facets = {{"semantic", 1, {1}}};
    if (mode == 2)
      doc.inputs[1].facets = {{"semantic", 1, {2}}};
    doc.nodes = {
        {1, "source", {WorkflowInputReference{mode == 3 ? 2U : 1U}}, {}}};
    doc.outputs = {{"result", 1, "value"}};
    GraphContext graph(doc);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    semantic.push_back(compiled.value().semantic.digest().value);
    physical.push_back(compiled.value().plan.digest().value);
  }
  for (std::size_t i = 0; i < semantic.size(); ++i)
    for (std::size_t j = i + 1; j < semantic.size(); ++j) {
      PS_CHECK(semantic[i] != semantic[j]);
      PS_CHECK(physical[i] != physical[j]);
    }
  return 0;
}

int schemas_halo_and_snapshots() {
  const auto defaults = make_default_operation_registry();
  auto image_traits = defaults->find_traits("image.exposure_gain").value();
  for (int mode = 0; mode < 10; ++mode) {
    auto traits = image_traits;
    if (mode == 0)
      traits.input_schema.clear();
    if (mode == 1)
      traits.input_schema[1].kind = static_cast<OperationPortKind>(999);
    if (mode == 2)
      traits.input_schema[1].minimum = std::numeric_limits<float>::quiet_NaN();
    if (mode == 3)
      traits.input_schema[1].maximum = -1;
    if (mode == 4)
      traits.input_schema[0].minimum = -0.0F;
    if (mode == 5)
      traits.output_schema.kind = OperationPortKind::Float32Scalar;
    if (mode == 6)
      traits.shape_rule = OperationShapeRule::MatchAllInputs;
    if (mode == 7)
      traits.input_schema[0] = traits.input_schema[1];
    if (mode == 8)
      traits.output_schema = {};
    if (mode == 9)
      traits.output_element_type = ElementType::UInt8;
    OperationRegistry registry;
    PS_CHECK(registry
                 .register_operation({"invalid", traits,
                                      [](const OperationInvocation& call) {
                                        return Result<Value>(call.inputs[0]);
                                      }})
                 .code == ErrorCode::InvalidArgument);
    PS_CHECK(registry.keys().empty());
  }
  auto halo_registry = std::make_shared<OperationRegistry>();
  auto halo_traits = image_traits;
  halo_traits.region_rule = OperationRegionRule::Halo;
  halo_traits.halo_radius = 1;
  PS_CHECK(halo_registry
               ->register_operation({"halo", halo_traits,
                                     [](const OperationInvocation& call) {
                                       return Result<Value>(call.inputs[0]);
                                     }})
               .ok());
  auto high_traits = image_traits;
  high_traits.input_schema[1] = {OperationPortKind::Float32Scalar, 17, 18};
  PS_CHECK(halo_registry
               ->register_operation({"high", high_traits,
                                     [](const OperationInvocation& call) {
                                       return Result<Value>(call.inputs[0]);
                                     }})
               .ok());
  halo_registry->freeze();
  Compiler halo_compiler(halo_registry);
  auto doc = s1_fixture::document();
  doc.inputs[0] = s1_fixture::declaration(
      1, "image", s1_fixture::value(std::vector<float>(36, 0), {3, 3, 4}));
  doc.nodes = {
      {10, "halo", {WorkflowInputReference{1}, WorkflowInputReference{2}}, {}}};
  doc.outputs = {{"result", 10, "value"}};
  GraphContext graph(doc);
  PlanningOptions requested;
  requested.output_regions["result"] = Region({{0, 1}, {0, 1}, {0, 4}});
  auto halo = halo_compiler.compile(graph, requested);
  PS_CHECK(halo.ok());
  const auto& demand = halo.value().plan.steps()[0].input_demands;
  PS_CHECK(demand[0].dimensions()[0].extent == 2 &&
           demand[0].dimensions()[1].extent == 2);
  PS_CHECK(demand[0].dimensions()[2].extent == 4 && demand[1].rank() == 1);
  doc.nodes.push_back(
      {20,
       "high",
       {WorkflowNodeOutput{10, "value"}, WorkflowInputReference{2}},
       {}});
  GraphContext empty_interval(doc);
  PS_CHECK(halo_compiler.analyze(empty_interval.snapshot()).status().code ==
           ErrorCode::InvalidArgument);
  // The same scalar declaration is checked against every consuming interval.
  doc.nodes.back().operation = "halo";
  GraphContext compatible_interval(doc);
  PS_CHECK(halo_compiler.compile(compatible_interval).ok());

  for (int mode = 0; mode < 3; ++mode) {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered = 0;
    bool release = false;
    auto operations = std::make_shared<OperationRegistry>();
    PS_CHECK(operations
                 ->register_operation(
                     {"image.exposure_gain", image_traits,
                      [&](const OperationInvocation& call) {
                        {
                          std::unique_lock<std::mutex> lock(mutex);
                          ++entered;
                          changed.notify_all();
                          changed.wait(lock, [&] { return release; });
                        }
                        return defaults->invoke("image.exposure_gain", call);
                      }})
                 .ok());
    PS_CHECK(operations
                 ->register_operation(
                     {"image.opacity",
                      defaults->find_traits("image.opacity").value(),
                      [&](const OperationInvocation& call) {
                        return defaults->invoke("image.opacity", call);
                      }})
                 .ok());
    operations->freeze();
    Compiler compiler(operations);
    GraphContext source(s1_fixture::document());
    auto compiled = compiler.compile(source);
    PS_CHECK(compiled.ok());
    ExecutionContext execution(operations, {2, false, 16, 512});
    CancellationSource cancel;
    auto owners_a = s1_fixture::bindings();
    auto owners_b = s1_fixture::bindings(true);
    auto a = std::async(
        std::launch::async, [&, owned = std::move(owners_a)]() mutable {
          return execution.execute(compiled.value().plan, std::move(owned),
                                   cancel.token());
        });
    auto b = std::async(
        std::launch::async, [&, owned = std::move(owners_b)]() mutable {
          return execution.execute(compiled.value().plan, std::move(owned));
        });
    bool both_entered = false;
    {
      std::unique_lock<std::mutex> lock(mutex);
      both_entered = changed.wait_for(lock, std::chrono::seconds(3),
                                      [&] { return entered == 2; });
      if (mode == 1)
        cancel.cancel();
      if (mode == 2)
        source.replace(s1_fixture::document());
      release = true;
      changed.notify_all();
    }
    auto first = a.get();
    auto second = b.get();
    PS_CHECK(both_entered);
    if (mode == 0) {
      PS_CHECK(first.ok() && s1_fixture::oracle(first.value()));
      PS_CHECK(second.ok() && s1_fixture::oracle(second.value(), true));
    } else if (mode == 1) {
      PS_CHECK(first.status().code == ErrorCode::Cancelled);
      PS_CHECK(second.ok() && s1_fixture::oracle(second.value(), true));
    } else {
      PS_CHECK(first.status().code == ErrorCode::Stale &&
               second.status().code == ErrorCode::Stale);
    }
  }
  return 0;
}

int plugin_boundaries() {
  for (const char* path :
       {PS_BAD_PORT_1, PS_BAD_PORT_2, PS_BAD_PORT_3, PS_BAD_PORT_4,
        PS_BAD_PORT_5, PS_BAD_PORT_6, PS_BAD_PORT_7, PS_BAD_PORT_8}) {
    OperationRegistry registry;
    PS_CHECK(registry.load_plugin(path).code == ErrorCode::InvalidArgument);
    PS_CHECK(registry.keys().empty());
  }
  DataDefinitionRegistry providers;
  PS_CHECK(providers.load_provider(PS_FLOAT32_PROVIDER_PATH).ok());
  PS_CHECK(providers.find("fixture.uint8").value().element_type ==
           ElementType::UInt8);
  PS_CHECK(providers.find("fixture.int64").value().element_type ==
           ElementType::Int64);
  PS_CHECK(providers.find("fixture.float64").value().element_type ==
           ElementType::Float64);
  PS_CHECK(providers.find("fixture.float32").value().element_type ==
           ElementType::Float32);
  for (bool bad : {false, true}) {
    auto operations = std::make_shared<OperationRegistry>();
    PS_CHECK(operations
                 ->load_plugin(bad ? PS_BAD_IMAGE_FIXTURE_PATH
                                   : PS_IMAGE_FIXTURE_PATH)
                 .ok());
    operations->freeze();
    Compiler compiler(operations);
    GraphContext graph(s1_fixture::document());
    auto compiled = compiler.compile(graph, s1_fixture::demand());
    PS_CHECK(compiled.ok());
    ExecutionContext execution(operations);
    for (bool second : {false, true}) {
      auto result = execution.execute(compiled.value().plan,
                                      s1_fixture::bindings(second));
      if (bad) {
        PS_CHECK(result.status().code == ErrorCode::OperationFailed);
      } else {
        PS_CHECK(result.ok());
        PS_CHECK(s1_fixture::oracle(result.value(), second));
      }
    }
    if (bad) {
      auto input = s1_fixture::bindings();
      input.inputs[1].value = s1_fixture::scalar(16);
      const auto exhausted = execution.execute(compiled.value().plan, input);
      PS_CHECK(!exhausted.ok());
      PS_CHECK(exhausted.status().code == ErrorCode::ResourceExhausted);
    }
  }
  return 0;
}

int regional_operation_views() {
  const Region roi({{1, 1}, {0, 2}, {0, 4}});
  for (bool plugin : {false, true}) {
    auto registry = plugin ? std::make_shared<ps::OperationRegistry>()
                           : make_default_operation_registry();
    if (plugin) {
      PS_CHECK(registry->load_plugin(PS_IMAGE_FIXTURE_PATH).ok());
      PS_CHECK(registry->freeze().ok());
    }
    const auto binding = s1_fixture::bindings();
    auto input = binding.inputs[0].value.view(roi);
    PS_CHECK(input.ok());
    std::vector<Value> inputs{input.value(), s1_fixture::scalar(2)};
    std::vector<Region> demands{roi, Region::whole({1})};
    const std::map<std::string, ps::ParameterValue> parameters;
    auto output = registry->invoke("image.exposure_gain",
                                   ps::OperationInvocation{inputs,
                                                           demands,
                                                           parameters,
                                                           ps::Backend::Cpu,
                                                           {},
                                                           roi});
    PS_CHECK(output.ok());
    PS_CHECK(output.value().descriptor().shape ==
             std::vector<std::uint64_t>({2, 2, 4}));
    PS_CHECK(output.value().bytes().size() == 32);
    PS_CHECK(output.value().layout().origin ==
             std::vector<std::uint64_t>({1, 0, 0}));
    float channels[8];
    std::memcpy(channels, output.value().bytes().data(), sizeof(channels));
    PS_CHECK(channels[0] == 0 && channels[1] == .5F && channels[2] == 1 &&
             channels[3] == 1);
    PS_CHECK(channels[4] == 0 && channels[7] == 0);
  }
  return 0;
}

int floating_environment() {
  std::fenv_t original;
  PS_CHECK(std::fegetenv(&original) == 0);
  for (int mode = 0; mode < 2; ++mode) {
    if (mode == 0)
      std::fesetround(FE_UPWARD);
#if defined(__APPLE__) && defined(__aarch64__)
    if (mode == 1)
      std::fesetenv(FE_DFL_DISABLE_DENORMS_ENV);
#endif
    const int rounding = std::fegetround();
    auto operations = make_default_operation_registry();
    Compiler compiler(operations);
    GraphContext graph(s1_fixture::document());
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    ExecutionContext execution(operations);
    auto input = s1_fixture::bindings();
    std::vector<float> pixels(16, 0);
    pixels[0] = std::numeric_limits<float>::min();
    pixels[3] = 1;
    input.inputs[0].value = s1_fixture::value(pixels);
    input.inputs[1].value = s1_fixture::scalar(.5F);
    input.inputs[2].value = s1_fixture::scalar(1);
    auto result = execution.execute(compiled.value().plan, input);
    PS_CHECK(result.ok());
    std::uint32_t bits = 0;
    std::memcpy(&bits, result.value().values.at("result").bytes().data(),
                sizeof(bits));
    PS_CHECK(bits == 0x00400000U);
    PS_CHECK(std::fegetround() == rounding);
    std::fesetenv(&original);
  }
  return 0;
}

int output_failure() {
  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  GraphContext graph(s1_fixture::document());
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(operations);
  for (bool underflow : {false, true}) {
    auto input = s1_fixture::bindings();
    std::vector<float> pixels(16, 0);
    pixels[0] = underflow ? 1 : std::numeric_limits<float>::max();
    pixels[3] = underflow ? std::numeric_limits<float>::denorm_min() : 1;
    input.inputs[0].value = s1_fixture::value(pixels);
    input.inputs[1].value = s1_fixture::scalar(underflow ? 1 : 16);
    input.inputs[2].value = s1_fixture::scalar(.5F);
    auto result = execution.execute(compiled.value().plan, input);
    PS_CHECK(result.status().code == ErrorCode::OperationFailed);
  }
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(dynamic_bindings() == 0);
  PS_CHECK(binding_failures() == 0);
  PS_CHECK(compile_failures_and_identity() == 0);
  PS_CHECK(output_failure() == 0);
  PS_CHECK(schemas_halo_and_snapshots() == 0);
  PS_CHECK(plugin_boundaries() == 0);
  PS_CHECK(regional_operation_views() == 0);
  PS_CHECK(floating_environment() == 0);
  PS_CHECK(static_constraint_identity() == 0);
  std::cout << "s1-rgba32f-exposure-opacity-v1: A/B exact; binding failures "
               "and identities passed\n";
  return 0;
}
