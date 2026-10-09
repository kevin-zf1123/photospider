#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
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
void calculus_projection() {
  Driver d;
  auto registry = make_default_operation_registry(false);
  auto starts = std::make_shared<std::atomic<unsigned>>(0);
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
    output.continuation_bytes = sizeof(FailingProducer);
    output.maximum_dependency_stages = 1;
    producer.start_result = [starts](const auto&, const auto& allocator) {
      starts->fetch_add(1, std::memory_order_relaxed);
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
  require(starts->load(std::memory_order_relaxed) == 0 &&
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
      starts->load(std::memory_order_relaxed) == 0 &&
          take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
      "Empty nonsingleton integral also leaves producers unstarted");
  auto failed = d.context->execute_fragments(
      frozen,
      {{"out", take(Footprint::from_regions({2}, {Region({{0, 1}})}))}});
  require(
      !failed.ok() && failed.status().message == "must stay lazy" &&
          starts->load(std::memory_order_relaxed) != 0,
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
}  // namespace
int main() {
  try {
    calculus_workflows();
    calculus_boundaries();
    calculus_projection();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
