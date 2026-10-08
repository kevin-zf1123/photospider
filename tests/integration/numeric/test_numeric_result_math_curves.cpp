#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
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

}  // namespace
int main() {
  try {
    curve_workflows();
    curve_boundaries();
    curve_metadata_and_identity();
    curve_composition();
    inverse_workflows();
    inverse_boundaries();
    inverse_composition();
    inverse_active_cancel();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
