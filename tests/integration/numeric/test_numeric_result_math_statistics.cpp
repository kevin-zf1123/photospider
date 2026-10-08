#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
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
  require(read_bits(uncached.results.at("out"), {5}) == double_bits(21),
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
void ordering_ieee_edges() {
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
    if (std::string(suffix) == "_strict")
      continue;
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
void reduction_exact_edges() {
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
    if (std::string(suffix) == "_strict")
      continue;
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
}  // namespace
int main() {
  try {
    staged_ordered_numeric();
    ordering_ieee_edges();
    scan_workflows();
    scan_numerics();
    scan_boundaries();
    reduction_exact_edges();
    reduction_boundaries();
    reduction_count_projection();
    ordering_projection();
    ordering_boundaries();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
