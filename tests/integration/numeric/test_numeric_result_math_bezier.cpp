#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
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
}  // namespace
int main() {
  try {
    bezier_workflows();
    bezier_boundaries();
    bezier_projection();
    bezier_resources();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
