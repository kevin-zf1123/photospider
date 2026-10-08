#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
void interpolation_relations_and_exact_range() {
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
    if (std::string(suffix) != "_strict") {
      auto mixed =
          take(d.run(std::string("numeric.mix") + suffix, {a, b, result}))
              .results.at("out");
      const double expected[] = {0, 0, .15625, .5, .84375, 1, 1};
      for (uint64_t i = 0; i < 7; ++i) {
        require(
            read_bits(result, {i}) == double_bits(expected[i]),
            "smoothstep exact dyadic polynomial through public Result path");
        require(read_bits(mixed, {i}) == double_bits(10 + 10 * expected[i]),
                "smoothstep to mix composition");
      }
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
void range_bits_and_bounds() {
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
    if (std::string(suffix) == "_strict")
      continue;
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
}  // namespace
int main() {
  try {
    interpolation_relations_and_exact_range();
    interpolation_bits_and_batches();
    interpolation_boundaries();
    range_bits_and_bounds();
    range_boundaries();
    sequence_workflows();
    sequence_authoring();
    sequence_boundaries();
    sequence_projection();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
