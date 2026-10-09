#include "photospider/ops/numeric/lut1d.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ps::Value array(ps::ElementType type, const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& bits) {
  const auto width = ps::Value::element_size(type);
  auto buffer = take(ps::BufferAllocator{}.allocate(bits.size() * width));
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(buffer.data() + i * width, &bits[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = width;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                      {0, strides},
                                      std::move(buffer).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs)
      : backing(inputs) {
    rf::declare_sources(&document, inputs);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bindings(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(const ps::DemandQuery& query,
                                   bool cache = true,
                                   std::uint64_t proof_work = UINT64_C(64) *
                                                              1024 * 1024,
                                   std::uint64_t cache_bytes = 65536,
                                   std::uint64_t payload = 8 * 1024 * 1024) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = payload;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto snapshot = context.freeze(plan.value().plan,
                                   bindings(take(context.resource_budget())));
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(64) * 1024 * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(32) * 1024 * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& values) {
  std::vector<std::uint64_t> bits;
  for (auto value : values)
    bits.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, bits);
}
ps::WorkflowNode node(
    bool channels, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64,
    ps::numeric::CurveDomain domain = ps::numeric::CurveDomain::Reject) {
  const auto helper = channels ? ps::numeric::apply_lut1d_channels_node
                               : ps::numeric::apply_lut1d_node;
  return take(helper(1, ps::WorkflowInputReference{1},
                     ps::WorkflowInputReference{2},
                     ps::WorkflowInputReference{3}, ps::ElementType::Float64,
                     dtype, domain, profile));
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void value(const ps::DemandResult& result, const std::vector<std::uint64_t>& at,
           std::uint64_t expected, unsigned width = 8) {
  std::uint64_t bits = 0;
  require(rf::read(result.results.at("values"), at, &bits, width).ok() &&
              bits == expected,
          "LUT1D expected bits");
}
void examples(ps::CpuNumericProfile profile) {
  for (bool reverse : {false, true}) {
    Fixture single(node(false, profile),
                   {doubles({4}, {0, .25, .5, 1}),
                    doubles({3}, reverse ? std::vector<double>{1, .25, 0}
                                         : std::vector<double>{0, .25, 1}),
                    doubles({3}, reverse ? std::vector<double>{1, 0, -.5}
                                         : std::vector<double>{0, 1, .5})});
    auto result =
        take(single.run({{"values", take(ps::Footprint::all({4}))}}, false));
    const double expected[] = {0, .125, .25, 1};
    for (unsigned i = 0; i < 4; ++i)
      value(result, {i}, raw(expected[i]));
    Fixture multi(node(true, profile),
                  {doubles({2, 2}, {0, 1, .25, .5}),
                   doubles({2, 2}, reverse ? std::vector<double>{2, 8, 0, 10}
                                           : std::vector<double>{0, 10, 2, 8}),
                   doubles({3}, reverse ? std::vector<double>{1, 0, -1}
                                        : std::vector<double>{0, 1, 1})});
    result =
        take(multi.run({{"values", take(ps::Footprint::all({2, 2}))}}, false));
    const double expected_multi[] = {0, 8, .5, 9};
    for (unsigned i = 0; i < 2; ++i)
      for (unsigned c = 0; c < 2; ++c)
        value(result, {i, c}, raw(expected_multi[2 * i + c]));
    for (auto policy : {ps::numeric::CurveDomain::Clamp,
                        ps::numeric::CurveDomain::LinearExtrapolate}) {
      Fixture outside(node(false, profile, ps::ElementType::Float64, policy),
                      {doubles({2}, {-1, 2}),
                       doubles({2}, reverse ? std::vector<double>{2, 0}
                                            : std::vector<double>{0, 2}),
                       doubles({3}, reverse ? std::vector<double>{1, 0, -1}
                                            : std::vector<double>{0, 1, 1})});
      result =
          take(outside.run({{"values", take(ps::Footprint::all({2}))}}, false));
      value(result, {0},
            raw(policy == ps::numeric::CurveDomain::Clamp ? 0 : -2));
      value(result, {1},
            raw(policy == ps::numeric::CurveDomain::Clamp ? 2 : 4));
    }
  }
  for (auto policy :
       {ps::numeric::CurveDomain::Reject, ps::numeric::CurveDomain::Clamp,
        ps::numeric::CurveDomain::LinearExtrapolate}) {
    Fixture one(
        node(false, profile, ps::ElementType::Float64, policy),
        {doubles({1},
                 {policy == ps::numeric::CurveDomain::Reject ? 2.0 : 99.0}),
         doubles({1}, {7}), doubles({3}, {2, 2, 0})});
    auto result =
        take(one.run({{"values", take(ps::Footprint::all({1}))}}, false));
    value(result, {0}, raw(7));
  }
  for (unsigned column = 0; column < 2; ++column) {
    Fixture scalar(node(false, profile),
                   {doubles({1}, {column ? .75 : .25}),
                    doubles({2}, column ? std::vector<double>{10, 8}
                                        : std::vector<double>{0, 2}),
                    doubles({3}, {0, 1, 1})});
    auto result =
        take(scalar.run({{"values", take(ps::Footprint::all({1}))}}, false));
    value(result, {0}, raw(column ? 8.5 : .5));
  }
  auto authored = take(ps::numeric::apply_lut1d_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::Float32, {},
      ps::numeric::CurveDomain::Reject, profile));
  Fixture mixed(authored, {array(ps::ElementType::Float32, {1}, {0x3e800000}),
                           doubles({2}, {0, 2}), doubles({3}, {0, 1, 1})});
  auto result =
      take(mixed.run({{"values", take(ps::Footprint::all({1}))}}, false));
  value(result, {0}, 0x3f000000, 4);
  require(
      result.results.at("values").schema().tensors[0].descriptor.element_type ==
          ps::ElementType::Float32,
      "constructor defaults to input dtype hint");
  std::cout << "scalar/channels analytic, descending, "
               "clamp/extrapolate/singleton and input-dtype default passed\n";
}
void axis_and_support(ps::CpuNumericProfile profile) {
  const auto nan = UINT64_C(0x7ff0000000000042), sign = UINT64_C(1) << 63;
  Fixture local(node(true, profile),
                {doubles({2, 2}, {.25, .75, .5, 1}),
                 array(ps::ElementType::Float64, {3, 2},
                       {0, nan, raw(1), nan, raw(2), raw(8)}),
                 doubles({3}, {0, 1, .5})});
  auto q = region({2, 2},
                  {ps::Region({{0, 1}, {0, 1}}), ps::Region({{1, 1}, {1, 1}})});
  auto bad = local.run({{"values", q}}, false);
  require(!bad.ok() && bad.status().detail.scope == ps::FailureScope::Run,
          "unrequested table channel fails Whole");
  local.backing[1] = doubles({3, 2}, {0, 10, 1, 9, 2, 8});
  auto result = take(local.run({{"values", q}}, false));
  value(result, {0, 0}, raw(.5));
  value(result, {1, 1}, raw(8));
  require(take(result.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({2, 2})),
          "sparse LUT Result retains full Whole coverage");
  auto support = take(result.dependencies.source_support());
  require(support.at("input0") == take(ps::Footprint::all({2, 2})) &&
              support.at("input1") == take(ps::Footprint::all({3, 2})) &&
              support.at("input2") == take(ps::Footprint::all({3})),
          "Whole LUT complete support");
  for (const auto& source : support)
    require(take(result.dependencies.potential_dirty(
                     std::string(source.first.begin(), source.first.end()),
                     source.second))
                    .at("values") == q,
            "complete input edits dirty recorded outputs");
  Fixture selected(node(false, profile),
                   {doubles({1}, {0}),
                    array(ps::ElementType::Float64, {3}, {raw(7), nan, nan}),
                    doubles({3}, {0, 1, .5})});
  value(take(selected.run({{"values", take(ps::Footprint::all({1}))}}, false)),
        {0}, raw(7));
  for (auto axis : std::vector<std::vector<std::uint64_t>>{
           {0, raw(1), raw(1)},
           {raw(1), raw(1) + 1, raw(0x1p-53)},
           {0, raw(1), nan},
           {raw(-0.0), 0, 0},
           {raw(-0.0), raw(-0.0), sign}}) {
    const unsigned l = (axis[0] == sign) ? 1 : 3;
    Fixture bad(node(false, profile),
                {doubles({1}, {0}), doubles({l}, std::vector<double>(l, 7)),
                 array(ps::ElementType::Float64, {3}, axis)});
    auto failed = bad.run({{"values", take(ps::Footprint::all({1}))}}, false);
    require(!failed.ok() &&
                failed.status().reason == ps::FailureReason::InvalidDomain,
            "inconsistent/collapsed/nonfinite/signed singleton axis rejected");
  }
  for (unsigned pattern = 0; pattern < 3; ++pattern) {
    Fixture zero(node(false, profile),
                 {doubles({3}, {0, .5, 1}),
                  array(ps::ElementType::Float64, {2},
                        {sign, pattern == 0   ? sign
                               : pattern == 1 ? 0
                                              : UINT64_C(1)}),
                  doubles({3}, {0, 1, 1})});
    result = take(zero.run({{"values", take(ps::Footprint::all({3}))}}, false));
    value(result, {0}, sign);
    value(result, {1}, pattern == 0 ? sign : 0);
  }
  Fixture singleton_nan(node(false, profile, ps::ElementType::Float64,
                             ps::numeric::CurveDomain::Clamp),
                        {array(ps::ElementType::Float64, {1}, {nan}),
                         doubles({1}, {7}), doubles({3}, {2, 2, 0})});
  auto failed =
      singleton_nan.run({{"values", take(ps::Footprint::all({1}))}}, false);
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain,
          "singleton still validates input");
  std::cout
      << "Whole support/dirty, numeric knot selection, invalid global axes and "
         "collapsed "
         "coordinates, signed-zero and singleton query validation passed\n";
}
ps::ResultRef execute_result(
    const ps::WorkflowNode& authored, const std::vector<ps::Value>& inputs,
    const std::shared_ptr<point_math_checks::Control>& control = {}) {
  point_math_checks::Workflow workflow(authored, inputs, {}, control);
  return take(workflow.run()).results.at("values");
}
ps::Value reversed_unaligned(const ps::Value& value) {
  const auto bytes = value.bytes();
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = bytes.size() / width;
  auto buffer = take(ps::BufferAllocator{}.allocate(bytes.size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(buffer.data() + 1 + (count - i - 1) * width,
                bytes.data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (auto i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(buffer).freeze()));
}
void layouts_and_resources(ps::CpuNumericProfile profile) {
  auto authored = node(true, profile);
  std::vector<ps::Value> dense{doubles({2, 2}, {0, 1, .25, .5}),
                               doubles({2, 2}, {0, 10, 2, 8}),
                               doubles({3}, {0, 1, 1})};
  for (unsigned mask = 0; mask < 8; ++mask) {
    auto inputs = dense;
    for (unsigned p = 0; p < 3; ++p)
      if (mask & (1U << p))
        inputs[p] = reversed_unaligned(inputs[p]);
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save LUT fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set LUT fenv");
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = mode;
      auto result = execute_result(authored, inputs, control);
      std::uint64_t bits = 0;
      require(rf::read(result, {1, 1}, &bits, 8).ok(), "LUT Result read");
      require(bits == raw(9), "LUT strided bits");
      auto narrow = execute_result(
          node(true, profile, ps::ElementType::Float32), inputs, control);
      std::uint32_t bits32 = 0;
      require(rf::read(narrow, {1, 1}, &bits32, 4).ok(), "LUT Float32 read");
      require(bits32 == UINT32_C(0x41100000), "LUT Float32 strided/fenv bits");
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "LUT preserves fenv");
    }
    require(fesetenv(&saved) == 0, "restore LUT fenv");
  }
  auto large = dense;
  large[0] = doubles({256, 2}, std::vector<double>(512, .5));
  point_math_checks::resources(authored, large, 512 * 8);
  auto grid = dense;
  grid[1] = doubles({4096, 2}, std::vector<double>(8192, 7));
  grid[2] = doubles({3}, {0, 4095, 1});
  point_math_checks::resources(authored, grid, 4 * 8);
  auto zero = doubles({1}, {0}), seven = doubles({1}, {7});
  std::vector<ps::Value> aliases{
      take(ps::Value::from_storage({ps::ElementType::Float64, {2, 2}},
                                   ps::Region::whole({2, 2}), {0, {0, 0}},
                                   zero.storage())),
      take(ps::Value::from_storage({ps::ElementType::Float64, {1, 2}},
                                   ps::Region::whole({1, 2}), {0, {0, 0}},
                                   seven.storage())),
      take(ps::Value::from_storage({ps::ElementType::Float64, {3}},
                                   ps::Region::whole({3}), {0, {0}},
                                   zero.storage()))};
  auto alias_result = execute_result(authored, aliases);
  std::uint64_t bits = 0;
  require(rf::read(alias_result, {1, 1}, &bits, 8).ok(), "aliased Result read");
  require(bits == raw(7), "zero-stride all-port singleton LUT");
  Fixture fixture(authored, dense);
  const auto empty =
      take(fixture.run({{"values", take(ps::Footprint::none({2, 2}))}}, false));
  require(
      take(empty.results.at("values").descriptor()).tensor_coverage(0).empty(),
      "Empty no payload");
  auto registry = ps::make_default_operation_registry();
  std::vector<ps::OperationMetadata> metadata;
  for (const auto& input : dense) {
    ps::OperationMetadata item;
    item.result_schema =
        std::make_shared<ps::SchemaTemplate>(rf::source_schema(input));
    metadata.push_back(std::move(item));
  }
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto bad = metadata;
    auto parameters = authored.parameters;
    const auto descriptor = [&](unsigned port, ps::ValueDescriptor next) {
      auto schema = *bad[port].result_schema;
      schema.tensors[0].descriptor = std::move(next);
      bad[port].result_schema =
          std::make_shared<ps::SchemaTemplate>(std::move(schema));
    };
    if (kind == 0)
      descriptor(1, {ps::ElementType::Float64, {2, 3}});
    if (kind == 1)
      descriptor(1, {ps::ElementType::Float64, {1048577, 2}});
    if (kind == 2)
      descriptor(2, {ps::ElementType::Float32, {3}});
    if (kind == 3)
      descriptor(2, {ps::ElementType::Float64, {2}});
    if (kind == 4)
      parameters.erase("dtype");
    if (kind == 5)
      parameters["out_of_domain"] = std::string("wrap");
    if (kind == 6)
      descriptor(0, {ps::ElementType::Int64, {2, 2}});
    if (kind == 7)
      descriptor(1, {ps::ElementType::Float64, {2, UINT64_C(1) << 40}});
    require(!registry->resolve_traits(authored.operation, bad, parameters).ok(),
            "LUT static Result schema bounds");
  }
  std::cout << "all-port negative/unaligned strides/fenv, "
               "axis/arithmetic work/cancel/Whole release, zero strides and "
               "Empty/schema/output/workspace budgets passed\n";
}
void baking_chains(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 6; ++kind) {
    const bool multi = kind >= 4;
    std::vector<ps::Value> inputs{
        doubles(multi ? std::vector<std::uint64_t>{1, 2}
                      : std::vector<std::uint64_t>{1},
                kind == 5 ? std::vector<double>{.75, 1.25}
                : multi   ? std::vector<double>{.25, .75}
                          : std::vector<double>{.25}),
        doubles({1}, {kind == 5 ? .5 : 0}),
        doubles({1}, {kind == 2 || kind == 3 ? 2.0
                      : kind == 5            ? 1.5
                                             : 1.0})};
    if (kind == 1) {
      inputs.push_back(doubles({2, 2}, {0, 0, 1, 1}));
      inputs.push_back(doubles({1, 1, 2}, {.5, 0}));
    }
    if (kind == 2) {
      inputs.push_back(doubles({3}, {0, 1, 2}));
      inputs.push_back(doubles({3}, {0, 2, 4}));
    }
    if (kind == 3) {
      inputs.push_back(doubles({3}, {0, 1, 2}));
      inputs.push_back(doubles({3}, {0, 1, 4}));
    }
    if (kind == 4) {
      inputs.push_back(doubles({2}, {0, 1}));
      inputs.push_back(doubles({2, 2}, {0, 10, 2, 8}));
    }
    if (kind == 5) {
      inputs.push_back(doubles({3}, {0, 1, 2}));
      inputs.push_back(doubles({3, 2}, {0, 4, 1, 3, 4, 0}));
    }
    Fixture fixture(node(multi, profile), inputs);
    fixture.document.nodes.clear();
    auto start = ps::numeric::sequence_input(fixture.document.inputs[1]),
         end = ps::numeric::sequence_input(fixture.document.inputs[2]);
    ps::numeric::BakedLut1d baked;
    if (kind == 0) {
      baked = take(ps::numeric::bake_lut1d_expression(
          fixture.document, "x^2", start, end, 3, {}, ps::ElementType::Float64,
          profile));
    } else if (kind == 1) {
      baked = take(ps::numeric::bake_lut1d_bezier(
          fixture.document, ps::WorkflowInputReference{4},
          ps::WorkflowInputReference{5}, start, end, 2, 3,
          ps::ElementType::Float64, ps::numeric::BezierDomain::Reject,
          profile));
    } else {
      const auto bake = kind == 2   ? ps::numeric::bake_lut1d_linear
                        : kind == 3 ? ps::numeric::bake_lut1d_pchip
                        : kind == 4 ? ps::numeric::bake_lut1d_linear_multi
                                    : ps::numeric::bake_lut1d_pchip_multi;
      baked = take(bake(fixture.document, ps::WorkflowInputReference{4},
                        ps::WorkflowInputReference{5}, start, end,
                        kind == 3   ? 5
                        : kind == 5 ? 2
                                    : 3,
                        ps::ElementType::Float64,
                        ps::numeric::CurveDomain::Reject, profile));
    }
    const auto apply = multi ? ps::numeric::apply_lut1d_channels_node
                             : ps::numeric::apply_lut1d_node;
    fixture.document.nodes.push_back(
        take(apply(100, ps::WorkflowInputReference{1}, baked.values, baked.axis,
                   ps::ElementType::Float64, {},
                   ps::numeric::CurveDomain::Reject, profile)));
    fixture.document.outputs = {{"values", 100, "values"}};
    const auto shape = multi ? std::vector<std::uint64_t>{1, 2}
                             : std::vector<std::uint64_t>{1};
    auto result =
        take(fixture.run({{"values", take(ps::Footprint::all(shape))}}, false));
    if (multi) {
      value(result, {0, 0}, raw(kind == 4 ? .5 : .78125));
      value(result, {0, 1}, raw(kind == 4 ? 8.5 : 2.28125));
    } else {
      value(result, {0}, raw(kind < 2 ? .125 : kind == 2 ? .5 : .15625));
    }
    if (kind >= 2) {
      fixture.backing[2] = inputs[1];
      auto invalid =
          fixture.run({{"values", take(ps::Footprint::all(shape))}}, false);
      require(!invalid.ok() &&
                  invalid.status().reason == ps::FailureReason::InvalidDomain,
              "consumer rejects valid constant interpolation bake axis");
    }
  }
  std::cout << "all six baking-to-LUT chains and separate discrete "
               "approximation/axis acceptance passed\n";
}
void cache_and_large(ps::CpuNumericProfile profile) {
  Fixture fixture(
      node(false, profile),
      {doubles({1}, {.25}), doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, .5})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 4 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  const auto root = take(context.resource_budget());
  auto bindings = fixture.bindings(root);
  auto demand = take(context.open_demand(plan.plan, bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(32) << 30;
  options.dependencies.maximum_work = UINT64_C(16) << 30;
  options.maximum_dependency_cache_work = 128 * 1024 * 1024;
  ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  const auto preparation = plan.plan.steps()[0].prepared;
  require(preparation != nullptr, "LUT static preparation");
  const auto cold = take(demand.request(query, {}, options));
  value(cold, {0}, raw(.5));
  const auto repeated = take(demand.request(query, {}, options));
  require(cold.results.at("values").object_id() ==
              repeated.results.at("values").object_id(),
          "same LUT demand retains completed Result");
  const auto fresh = fixture.bindings(root);
  const auto warm = take(context.execute_fragments(
      take(context.freeze(plan.plan, fresh)), query, {}, options));
  require(warm.diagnostics.cache_hits > 0 &&
              rf::bytes(cold.results.at("values")) ==
                  rf::bytes(warm.results.at("values")),
          "fresh LUT sources reuse verified content");
  const auto association = warm.results.at("values").association();
  for (unsigned port = 0; port < 3; ++port)
    require(
        std::find(association.begin(), association.end(),
                  fresh.inputs[port].result.object_id()) != association.end() &&
            std::find(association.begin(), association.end(),
                      bindings.inputs[port].result.object_id()) ==
                association.end(),
        "cached LUT refreshes source associations");
  bindings.inputs[0].result =
      point_math_checks::source(root, doubles({1}, {.75}),
                                fixture.document.inputs[0].result_schema.get());
  require(demand.replace_bindings(bindings).ok(), "LUT input replacement");
  auto changed = take(demand.request(query, {}, options));
  value(changed, {0}, raw(1.5));
  require(take(changed.dependencies.source_support()).at("input1") ==
              take(ps::Footprint::all({3})),
          "query replacement retains complete table dependency");
  bindings.inputs[1].result =
      point_math_checks::source(root, doubles({3}, {0, 1, 4}),
                                fixture.document.inputs[1].result_schema.get());
  require(demand.replace_bindings(bindings).ok(), "LUT table replacement");
  value(take(demand.request(query, {}, options)), {0}, raw(2.5));
  bindings.inputs[2].result =
      point_math_checks::source(root, doubles({3}, {0, 2, 1}),
                                fixture.document.inputs[2].result_schema.get());
  require(demand.replace_bindings(bindings).ok(), "LUT axis replacement");
  changed = take(demand.request(query, {}, options));
  value(changed, {0}, raw(.75));
  require(take(changed.dependencies.source_support()).at("input1") ==
              take(ps::Footprint::all({3})),
          "axis replacement retains complete table dependency");
  require(plan.plan.steps()[0].prepared == preparation,
          "LUT source replacement reuses static preparation");
  const auto nan = UINT64_C(0x7ff0000000000042);
  Fixture isolated(node(true, profile), {doubles({1, 2}, {.5, .5}),
                                         array(ps::ElementType::Float64, {2, 2},
                                               {0, nan, raw(2), nan}),
                                         doubles({3}, {0, 1, 1})});
  auto bad = isolated.run(
      {{"values", region({1, 2}, {ps::Region({{0, 1}, {0, 1}})})}}, false);
  require(!bad.ok() && bad.status().detail.scope == ps::FailureScope::Run,
          "remote table channel fails complete output");
  const std::vector<std::uint64_t> rank8{2, 1, 1, 1, 1, 1, 1, 2};
  Fixture high_rank(node(true, profile),
                    {doubles(rank8, {0, .5, 1, 1.5}),
                     doubles({2, 2}, {0, 10, 2, 8}), doubles({3}, {0, 1, 1})});
  auto rejected =
      high_rank.run({{"values", region(rank8, {ps::Region({{1, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {0, 1},
                                                           {1, 1}})})}},
                    false);
  require(!rejected.ok() &&
              rejected.status().detail.scope == ps::FailureScope::Run &&
              rejected.status().reason == ps::FailureReason::InvalidDomain,
          "rank8 Whole query rejection");
  const auto columns = UINT64_C(1) << 39;
  Fixture huge(node(true, profile), {doubles({1}, {.5}), doubles({1}, {7}),
                                     doubles({3}, {0, 1, 1})});
  huge.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
  huge.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "values"};
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{1}, {1, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(3, ps::WorkflowInputReference{2}, {2, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  auto rejected_large = huge.run(
      {{"values",
        region({1, columns}, {ps::Region({{0, 1}, {columns - 1, 1}})})}},
      false);
  require(
      !rejected_large.ok() &&
          rejected_large.status().code == ps::ErrorCode::ResourceExhausted &&
          rejected_large.status().reason == ps::FailureReason::CapacityLimit &&
          rejected_large.status().detail.node_id == 1,
      "giant channels require full output payload at LUT node");
  const unsigned length = 1048576;
  Fixture full_grid(node(false, profile),
                    {doubles({1}, {0}), doubles({1}, {7}),
                     doubles({3}, {0, 1, 1.0 / (length - 1)})});
  full_grid.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "values"};
  full_grid.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{2}, {length},
                                 ps::numeric::ArrayLayout::View, profile)));
  auto result =
      take(full_grid.run({{"values", take(ps::Footprint::all({1}))}}, false,
                         UINT64_C(64) * 1024 * 1024, 0, 16 * 1024 * 1024));
  value(result, {0}, raw(7));
  require(result.diagnostics.managed_resources &&
              result.diagnostics.managed_resources
                      ->peak[ps::ResourceKind::Metadata] >=
                  static_cast<std::uint64_t>(length) * 8 &&
              result.diagnostics.managed_resources
                      ->peak[ps::ResourceKind::Payload] <
                  static_cast<std::uint64_t>(length) * 8,
          "maximum LUT grid is Root-accounted without dense table Payload");
  require(take(result.dependencies.source_support()).at("input1") ==
              take(ps::Footprint::all({1})),
          "full grid validation keeps scalar backing for constant table");
  std::cout << "input/table/axis cache replacement, Whole failures, "
               "giant-channel budget and complete maximum-L grid passed\n";
}
struct FailedTableSource {
  unsigned* calls;
  explicit FailedTableSource(unsigned* count) : calls(count) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(ps::Status{
        ps::ErrorCode::OperationFailed, "required LUT table producer"});
  }
};
void typed_and_upstream(ps::CpuNumericProfile profile) {
  auto input = array(ps::ElementType::Float32, {1, 1, 4},
                     {0x3e800000, 0, 0, 0x40000000});
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  std::vector<ps::Value> inputs{input, doubles({2}, {0, 2}),
                                doubles({3}, {0, 1, 1})};
  auto registry = ps::make_default_operation_registry();
  auto authored = node(false, profile);
  Fixture typed(authored, inputs);
  auto typed_schema = *typed.document.inputs[0].result_schema;
  typed_schema.tensors[0].facets = {facet};
  typed_schema.tensors[0].layout.spatial = true;
  typed_schema.tensors[0].layout.channel_axis = 2;
  typed.document.inputs[0].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(typed_schema));
  auto failed_typed = typed.run(
      {{"values", region({1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {0, 1}})})}},
      false);
  require(!failed_typed.ok() &&
              failed_typed.status().code == ps::ErrorCode::InvalidArgument &&
              failed_typed.status().detail.input_id == 1,
          "full typed Image validation rejects unrequested alpha");
  registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition source;
  source.key = "manual.lut_table";
  source.traits.input_count = 0;
  source.traits.input_schema.clear();
  auto& output = source.traits.outputs[0];
  output.region_rule = ps::OperationRegionRule::Whole;
  output.output_schema.kind = ps::OperationPortKind::Result;
  auto schema = rf::source_schema(doubles({2}, {0, 2}));
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  output.continuation_bytes = sizeof(FailedTableSource);
  output.maximum_dependency_stages = 8;
  source.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<FailedTableSource>(allocator, &calls);
  };
  require(registry->register_operation(std::move(source)).ok() &&
              registry->freeze().ok(),
          "LUT source registry");
  Fixture fixture(
      node(false, profile),
      {doubles({1}, {2}), doubles({2}, {0, 2}), doubles({3}, {0, 1, 2})});
  fixture.registry = registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.backing.erase(fixture.backing.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.lut_table", {}, {}});
  auto q = ps::DemandQuery{{"values", take(ps::Footprint::all({1}))}};
  auto failed = fixture.run(q, false);
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().message == "required LUT table producer" &&
              calls == 1,
          "Whole table producer precedes invalid axis");
  fixture.backing[1] = doubles({3}, {0, 1, 1});
  failed = fixture.run(q, false);
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().message == "required LUT table producer" &&
              calls == 2,
          "Whole table producer precedes query reject");
  fixture.backing[0] = doubles({1}, {.5});
  failed = fixture.run(q, false);
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::OperationFailed &&
              failed.status().message == "required LUT table producer" &&
              calls == 3,
          "selected table upstream failure preserved");
  std::cout << "full typed Image validation and complete upstream table "
               "producer order passed\n";
}

void retained_output(ps::CpuNumericProfile profile) {
  for (bool channels : {false, true}) {
    ps::ResourceBudget root;
    ps::ResultRef output;
    ps::ResultTensorReadWindow window;
    std::vector<std::weak_ptr<const ps::CpuStorage>> input_owners;
    const auto shape = channels ? std::vector<std::uint64_t>{1, 2}
                                : std::vector<std::uint64_t>{2};
    {
      std::vector<ps::Value> inputs{
          doubles(shape, {.25, .75}),
          channels ? doubles({2, 2}, {0, 10, 2, 8}) : doubles({2}, {0, 2}),
          doubles({3}, {0, 1, 1})};
      for (const auto& input : inputs)
        input_owners.push_back(input.storage());
      point_math_checks::Workflow workflow(node(channels, profile), inputs);
      root = workflow.root;
      output = take(workflow.run()).results.at("values");
      window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                          ps::Region::whole(shape)));
    }
    for (const auto& owner : input_owners)
      require(owner.expired(), "LUT output retires every source backing");
    require(root.statistics().live[ps::ResourceKind::Payload] == 16,
            "escaped LUT Result/window share one output owner");
    double number = 0;
    require(rf::read(output,
                     channels ? std::vector<std::uint64_t>{0, 0}
                              : std::vector<std::uint64_t>{0},
                     &number, 8)
                    .ok() &&
                number == .5,
            "LUT Result survives source and context retirement");
    output = {};
    const auto row =
        take(window.row_run(channels ? std::vector<std::uint64_t>{0, 1}
                                     : std::vector<std::uint64_t>{1}));
    std::memcpy(&number, row.data, 8);
    require(number == (channels ? 8.5 : 1.5) &&
                root.statistics().live[ps::ResourceKind::Payload] == 16,
            "authorized LUT window survives Result release");
    window = {};
    point_math_checks::released(root);
  }
  std::cout << "source retirement, escaped Result/window, one Payload owner "
               "and final all-Root release passed\n";
}

void oracle(ps::CpuNumericProfile profile) {
  unsigned channels = 0, dtype = 0, policy = 0, rank = 0, l = 0, c = 0,
           requested = 0;
  while (std::cin >> channels >> dtype >> policy >> rank >> l >> c >>
         requested) {
    std::vector<std::uint64_t> shape(rank);
    std::uint64_t size = 1;
    for (auto& extent : shape) {
      std::cin >> extent;
      size *= extent;
    }
    std::vector<std::vector<std::uint64_t>> coords(
        requested, std::vector<std::uint64_t>(rank));
    for (auto& at : coords)
      for (auto& index : at)
        std::cin >> index;
    std::vector<ps::Value> inputs;
    for (unsigned p = 0; p < 3; ++p) {
      unsigned type = 0;
      std::cin >> type;
      std::vector<std::uint64_t> bits(p == 0 ? size : p == 1 ? l * c : 3);
      for (auto& b : bits)
        std::cin >> std::hex >> b >> std::dec;
      inputs.push_back(array(static_cast<ps::ElementType>(type),
                             p == 0 ? shape
                             : p == 1
                                 ? (channels ? std::vector<std::uint64_t>{l, c}
                                             : std::vector<std::uint64_t>{l})
                                 : std::vector<std::uint64_t>{3},
                             bits));
    }
    Fixture fixture(node(channels, profile, static_cast<ps::ElementType>(dtype),
                         static_cast<ps::numeric::CurveDomain>(policy)),
                    inputs);
    std::vector<ps::Region> boxes;
    for (const auto& at : coords) {
      std::vector<ps::RegionDimension> spans;
      for (auto index : at)
        spans.push_back({index, 1});
      boxes.emplace_back(std::move(spans));
    }
    auto result =
        fixture.run({{"values", region(shape, std::move(boxes))}}, false);
    if (!result.ok()) {
      std::cout << (result.status().reason ==
                            ps::FailureReason::ArithmeticOverflow
                        ? "overflow"
                    : result.status().reason == ps::FailureReason::InvalidDomain
                        ? "domain"
                        : "other")
                << '\n';
      if (result.status().reason != ps::FailureReason::ArithmeticOverflow &&
          result.status().reason != ps::FailureReason::InvalidDomain)
        std::cerr << result.status().message << '\n';
      continue;
    }
    for (const auto& at : coords) {
      std::uint64_t bits = 0;
      require(rf::read(result.value().results.at("values"), at, &bits,
                       dtype == 4 ? 4 : 8)
                  .ok(),
              "LUT oracle read");
      std::cout << std::hex << bits << ' ';
    }
    std::cout << std::dec << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      examples(profile);
      axis_and_support(profile);
      layouts_and_resources(profile);
      baking_chains(profile);
      cache_and_large(profile);
      typed_and_upstream(profile);
      retained_output(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
