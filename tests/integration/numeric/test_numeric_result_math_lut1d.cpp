#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
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
}  // namespace
int main() {
  try {
    lut1d_workflows();
    lut1d_boundaries();
    lut1d_composition();
    lut1d_resources();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
