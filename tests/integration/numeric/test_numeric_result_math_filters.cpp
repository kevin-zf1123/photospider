#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
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
}  // namespace
int main() {
  try {
    result_shaper_authoring();
    lowpass_workflows();
    lowpass_boundaries();
    lowpass_preparation_and_cancel();
    lowpass_period_cancel();
    shaper_workflows();
    shaper_boundaries();
    shaper_preparation_and_cancel();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
