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

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/bezier.hpp"
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
                                   std::uint64_t cache_bytes = 65536) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 8 * 1024 * 1024;
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
ps::WorkflowNode node(unsigned degree, ps::CpuNumericProfile profile,
                      ps::ElementType dtype = ps::ElementType::Float64) {
  return take(ps::numeric::evaluate_bezier_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, degree,
      dtype, profile));
}
ps::Value indices(const std::vector<std::int64_t>& values) {
  std::vector<std::uint64_t> bits;
  for (auto value : values) {
    std::uint64_t b = 0;
    std::memcpy(&b, &value, 8);
    bits.push_back(b);
  }
  return array(ps::ElementType::Int64, {values.size()}, bits);
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
void value(const ps::DemandResult& result, std::uint64_t row, std::uint64_t col,
           std::uint64_t expected, unsigned width = 8) {
  std::uint64_t bits = 0;
  require(
      rf::read(result.results.at("values"), {row, col}, &bits, width).ok() &&
          bits == expected,
      "parametric expected bits");
}
void examples(ps::CpuNumericProfile profile) {
  Fixture quadratic(node(2, profile),
                    {doubles({2, 2}, {0, 0, 2, 0}), doubles({1, 1, 2}, {1, 2}),
                     indices({0, 0, 0}), doubles({3}, {0, .5, 1})});
  auto result = take(
      quadratic.run({{"values", take(ps::Footprint::all({3, 2}))}}, false));
  const double expected[] = {0, 0, 1, 1, 2, 0};
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned c = 0; c < 2; ++c)
      value(result, i, c, raw(expected[2 * i + c]));
  Fixture cubic(node(3, profile), {doubles({2, 2}, {0, 0, 3, 0}),
                                   doubles({1, 2, 2}, {1, 3, -1, 3}),
                                   indices({0}), doubles({1}, {.5})});
  result =
      take(cubic.run({{"values", take(ps::Footprint::all({1, 2}))}}, false));
  value(result, 0, 0, raw(1.5));
  value(result, 0, 1, raw(2.25));
  Fixture reconstructed(
      node(2, profile),
      {array(ps::ElementType::Float64, {2, 1}, {raw(1), raw(1) + 2}),
       doubles({1, 1, 1}, {0x1p-53}), indices({0}), doubles({1}, {.5})});
  result = take(
      reconstructed.run({{"values", take(ps::Footprint::all({1, 1}))}}, false));
  value(result, 0, 0, raw(1));
  for (auto dtype : {ps::ElementType::Float32, ps::ElementType::Float64}) {
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
      const auto sign = UINT64_C(1) << 63;
      Fixture zeros(node(2, profile, dtype),
                    {array(ps::ElementType::Float64, {2, 1},
                           {pattern == 3 ? 0 : sign, sign}),
                     array(ps::ElementType::Float64, {1, 1, 1},
                           {pattern == 0   ? sign
                            : pattern == 1 ? UINT64_C(1)
                            : pattern == 2 ? sign | 1
                                           : 0}),
                     indices({0, 0, 0}), doubles({3}, {-0.0, .5, 1})});
      result = take(
          zeros.run({{"values", take(ps::Footprint::all({3, 1}))}}, false));
      const auto target_sign = UINT64_C(1)
                               << (dtype == ps::ElementType::Float32 ? 31 : 63);
      value(result, 0, 0, pattern == 3 ? 0 : target_sign,
            dtype == ps::ElementType::Float32 ? 4 : 8);
      value(result, 1, 0, pattern == 0 || pattern == 2 ? target_sign : 0,
            dtype == ps::ElementType::Float32 ? 4 : 8);
      value(result, 2, 0, target_sign,
            dtype == ps::ElementType::Float32 ? 4 : 8);
    }
  }
  const auto maximum = UINT64_C(0x7fefffffffffffff);
  Fixture cancel(node(2, profile, ps::ElementType::Float32),
                 {array(ps::ElementType::Float64, {2, 1},
                        {maximum, maximum | (UINT64_C(1) << 63)}),
                  array(ps::ElementType::Float64, {1, 1, 1},
                        {maximum | (UINT64_C(1) << 63)}),
                  indices({0}), doubles({1}, {.5})});
  result =
      take(cancel.run({{"values", take(ps::Footprint::all({1, 1}))}}, false));
  value(result, 0, 0, 0, 4);
  Fixture overflow(node(2, profile, ps::ElementType::Float32),
                   {doubles({2, 1}, {0, 0x1p128}),
                    array(ps::ElementType::Float64, {1, 1, 1},
                          {UINT64_C(0x7ff0000000000042)}),
                    indices({0}), doubles({1}, {1})});
  auto failed =
      overflow.run({{"values", take(ps::Footprint::all({1, 1}))}}, false);
  require(
      !failed.ok() && failed.status().code == ps::ErrorCode::OperationFailed &&
          failed.status().reason == ps::FailureReason::ArithmeticOverflow &&
          failed.status().message.find("port=0 coordinate=1,0,") !=
              std::string::npos,
      "t1 overflow identifies actually selected endpoint and skips handles");
  std::cout << "quadratic/cubic public fixtures, RN64 reconstruction, signed "
               "zeros and finite extreme cancellation passed\n";
}
void support_and_failures(ps::CpuNumericProfile profile) {
  const auto nan = UINT64_C(0x7ff0000000000042);
  Fixture fixture(
      node(2, profile),
      {array(ps::ElementType::Float64, {3, 2},
             {raw(0), nan, raw(2), nan, nan, raw(4)}),
       array(ps::ElementType::Float64, {2, 1, 2}, {raw(1), nan, nan, nan}),
       indices({0, 0, 99, 1}),
       array(ps::ElementType::Float64, {4}, {raw(.5), 0, nan, raw(1)})});
  auto roi = region(
      {4, 2}, {ps::Region({{0, 2}, {0, 1}}), ps::Region({{3, 1}, {1, 1}})});
  auto bad = fixture.run({{"values", roi}}, false);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::InvalidArgument &&
              bad.status().detail.scope == ps::FailureScope::Run,
          "unrequested query rejects Whole before controls");
  fixture.backing[2] = indices({0, 0, 0, 1});
  fixture.backing[3] = doubles({4}, {.5, 0, .5, 1});
  bad = fixture.run({{"values", roi}}, false);
  require(!bad.ok() && bad.status().code == ps::ErrorCode::OperationFailed &&
              bad.status().detail.scope == ps::FailureScope::Run,
          "unrequested component failure covers Whole");
  fixture.backing[0] = doubles({3, 2}, {0, 0, 2, 2, 4, 4});
  fixture.backing[1] = doubles({2, 1, 2}, {1, 1, 1, 1});
  auto result = take(fixture.run({{"values", roi}}, false));
  value(result, 0, 0, raw(1));
  value(result, 1, 0, 0);
  value(result, 3, 1, raw(4));
  require(take(result.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({4, 2})),
          "sparse parametric Result retains full Whole coverage");
  auto support = take(result.dependencies.source_support());
  require(support.at("input0") == take(ps::Footprint::all({3, 2})) &&
              support.at("input1") == take(ps::Footprint::all({2, 1, 2})) &&
              support.at("input2") == take(ps::Footprint::all({4})),
          "parametric complete input support");
  for (const auto& name : {"input0", "input1"}) {
    auto dirty =
        take(result.dependencies.potential_dirty(name, support.at(name)));
    require(dirty.at("values") == roi, "all input edits dirty recorded demand");
  }
  Fixture endpoint(node(2, profile),
                   {array(ps::ElementType::Float64, {2, 1}, {raw(7), nan}),
                    array(ps::ElementType::Float64, {1, 1, 1}, {nan}),
                    indices({0}), doubles({1}, {0})});
  value(
      take(endpoint.run({{"values", take(ps::Footprint::all({1, 1}))}}, false)),
      0, 0, raw(7));
  std::cout << "Whole query/component failures, full support/dirty and "
               "mathematical endpoint selection passed\n";
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
  auto registry = ps::make_default_operation_registry();
  auto authored = node(3, profile);
  std::vector<ps::Value> dense{doubles({2, 2}, {0, 0, 3, 0}),
                               doubles({1, 2, 2}, {1, 3, -1, 3}),
                               indices({0, 0}), doubles({2}, {.5, .5})};
  for (unsigned mask = 0; mask < 16; ++mask) {
    auto inputs = dense;
    for (unsigned p = 0; p < 4; ++p)
      if (mask & (1U << p))
        inputs[p] = reversed_unaligned(inputs[p]);
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save parametric fenv");
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set fenv");
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = mode;
      auto result = execute_result(authored, inputs, control);
      std::uint64_t bits = 0;
      require(rf::read(result, {1, 1}, &bits, 8).ok(),
              "parametric Result read");
      require(bits == raw(2.25), "parametric strided result");
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "parametric fenv unchanged");
    }
    require(fesetenv(&saved) == 0, "restore parametric fenv");
  }
  auto large = dense;
  large[2] = indices(std::vector<std::int64_t>(256, 0));
  large[3] = doubles({256}, std::vector<double>(256, .5));
  point_math_checks::resources(authored, large, 256 * 2 * 8);
  Fixture fixture(authored, dense);
  const auto empty =
      take(fixture.run({{"values", take(ps::Footprint::none({2, 2}))}}, false));
  require(
      take(empty.results.at("values").descriptor()).tensor_coverage(0).empty(),
      "Empty reads no payload");
  std::vector<ps::OperationMetadata> metadata;
  for (const auto& input : dense) {
    ps::OperationMetadata item;
    item.result_schema =
        std::make_shared<ps::SchemaTemplate>(rf::source_schema(input));
    metadata.push_back(std::move(item));
  }
  for (unsigned kind = 0; kind < 9; ++kind) {
    auto inputs = metadata;
    auto parameters = authored.parameters;
    const auto descriptor = [&](unsigned port, ps::ValueDescriptor next) {
      auto schema = *inputs[port].result_schema;
      schema.tensors[0].descriptor = std::move(next);
      inputs[port].result_schema =
          std::make_shared<ps::SchemaTemplate>(std::move(schema));
    };
    if (kind == 0)
      parameters.erase("degree");
    if (kind == 1)
      parameters["degree"] = std::int64_t{4};
    if (kind == 2)
      parameters["dtype"] = std::string("int64");
    if (kind == 3)
      descriptor(0, {ps::ElementType::Float64, {65537, 2}});
    if (kind == 4)
      descriptor(0, {ps::ElementType::Float64, {2, UINT64_C(1) << 40}});
    if (kind == 5)
      descriptor(1, {ps::ElementType::Float64, {1, 1, 2}});
    if (kind == 6)
      descriptor(2, {ps::ElementType::Float64, {2}});
    if (kind == 7)
      descriptor(3, {ps::ElementType::Float64, {1}});
    if (kind == 8)
      parameters["unknown"] = std::int64_t{0};
    auto invalid =
        registry->resolve_traits(authored.operation, inputs, parameters);
    require(!invalid.ok() &&
                (invalid.status().code == ps::ErrorCode::TypeMismatch ||
                 invalid.status().code == ps::ErrorCode::InvalidArgument),
            "parametric malformed static Result schema");
  }
  std::cout << "all-port signed/unaligned strides/fenv, polynomial work/cancel "
               "release, Empty/schema and Whole budgets passed\n";
}
void cache_composition_and_typed(ps::CpuNumericProfile profile) {
  Fixture fixture(node(2, profile),
                  {doubles({3, 1}, {0, 2, 10}), doubles({2, 1, 1}, {1, 4}),
                   indices({0}), doubles({1}, {.5})});
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 1048576;
  config.maximum_live_bytes = 4 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  const auto root = take(context.resource_budget());
  auto bindings = fixture.bindings(root);
  auto demand = take(context.open_demand(plan.plan, bindings));
  ps::ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(8) << 30;
  options.dependencies.maximum_work = UINT64_C(4) << 30;
  options.maximum_dependency_cache_work = 128 * 1024 * 1024;
  ps::DemandQuery query{{"values", take(ps::Footprint::all({1, 1}))}};
  const auto preparation = plan.plan.steps()[0].prepared;
  require(preparation != nullptr, "parametric static preparation");
  const auto cold = take(demand.request(query, {}, options));
  value(cold, 0, 0, raw(1));
  const auto repeated = take(demand.request(query, {}, options));
  require(cold.results.at("values").object_id() ==
              repeated.results.at("values").object_id(),
          "same parametric demand retains completed Result");
  const auto fresh = fixture.bindings(root);
  const auto warm = take(context.execute_fragments(
      take(context.freeze(plan.plan, fresh)), query, {}, options));
  require(warm.diagnostics.cache_hits == 1 &&
              rf::bytes(cold.results.at("values")) ==
                  rf::bytes(warm.results.at("values")),
          "fresh parametric sources reuse cached content");
  const auto association = warm.results.at("values").association();
  for (unsigned port = 0; port < 4; ++port)
    require(
        std::find(association.begin(), association.end(),
                  fresh.inputs[port].result.object_id()) != association.end() &&
            std::find(association.begin(), association.end(),
                      bindings.inputs[port].result.object_id()) ==
                association.end(),
        "cached parametric output refreshes all source associations");
  bindings.inputs[2].result = point_math_checks::source(
      root, indices({1}), fixture.document.inputs[2].result_schema.get());
  require(demand.replace_bindings(bindings).ok(), "segment replacement");
  auto changed = take(demand.request(query, {}, options));
  value(changed, 0, 0, raw(6));
  require(take(changed.dependencies.source_support()).at("input1") ==
              take(ps::Footprint::all({2, 1, 1})),
          "segment replacement retains complete handles support");
  bindings.inputs[3].result = point_math_checks::source(
      root, doubles({1}, {1}), fixture.document.inputs[3].result_schema.get());
  require(demand.replace_bindings(bindings).ok(), "t replacement");
  require(plan.plan.steps()[0].prepared == preparation,
          "segment/t replacement reuses parametric preparation");
  changed = take(demand.request(query, {}, options));
  value(changed, 0, 0, raw(10));
  auto support = take(changed.dependencies.source_support());
  require(support.at("input1") == take(ps::Footprint::all({2, 1, 1})),
          "endpoint retains complete handle dependency");
  const auto columns = UINT64_C(1) << 39;
  Fixture huge(node(2, profile), {doubles({1}, {7}), doubles({1}, {0}),
                                  indices({0}), doubles({1}, {.5})});
  huge.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
  huge.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "values"};
  huge.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{1}, {2, columns},
                                 ps::numeric::ArrayLayout::View, profile)));
  huge.document.nodes.push_back(take(ps::numeric::constant_node(
      3, ps::WorkflowInputReference{2}, {1, 1, columns},
      ps::numeric::ArrayLayout::View, profile)));
  auto result = huge.run(
      {{"values", region({1, columns}, {ps::Region({{0, 1}, {2, 1}})})}},
      false);
  require(!result.ok() &&
              result.status().code == ps::ErrorCode::ResourceExhausted &&
              result.status().reason == ps::FailureReason::CapacityLimit &&
              result.status().detail.node_id == 1,
          "giant D complete output exceeds payload at parametric node");
  const auto count = UINT64_C(1) << 40;
  Fixture many(node(2, profile),
               {doubles({2, 1}, {0, 1}), doubles({1, 1, 1}, {.5}), indices({0}),
                doubles({1}, {.5})});
  many.document.nodes[0].inputs[2] = ps::WorkflowNodeOutput{2, "values"};
  many.document.nodes[0].inputs[3] = ps::WorkflowNodeOutput{3, "values"};
  many.document.nodes.push_back(take(
      ps::numeric::constant_node(2, ps::WorkflowInputReference{3}, {count},
                                 ps::numeric::ArrayLayout::View, profile)));
  many.document.nodes.push_back(take(
      ps::numeric::constant_node(3, ps::WorkflowInputReference{4}, {count},
                                 ps::numeric::ArrayLayout::View, profile)));
  auto last = many.run(
      {{"values", region({count, 1}, {ps::Region({{count - 1, 1}, {0, 1}})})}},
      false);
  require(!last.ok() &&
              last.status().code == ps::ErrorCode::ResourceExhausted &&
              last.status().reason == ps::FailureReason::CapacityLimit &&
              last.status().detail.node_id == 1,
          "giant N complete output exceeds payload at parametric node");
  auto authored = node(2, profile);
  auto handles =
      array(ps::ElementType::Float32, {1, 1, 4}, {0, 0, 0, 0x40000000});
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  std::vector<ps::Value> inputs{doubles({2, 4}, {0, 0, 0, 0, 1, 1, 1, 1}),
                                handles, indices({0}), doubles({1}, {.5})};
  Fixture typed(authored, inputs);
  auto typed_schema = *typed.document.inputs[1].result_schema;
  typed_schema.tensors[0].facets = {facet};
  typed_schema.tensors[0].layout.spatial = true;
  typed_schema.tensors[0].layout.channel_axis = 2;
  typed.document.inputs[1].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(typed_schema));
  auto invalid = typed.run(
      {{"values", region({1, 4}, {ps::Region({{0, 1}, {0, 1}})})}}, false);
  require(!invalid.ok() &&
              invalid.status().code == ps::ErrorCode::InvalidArgument &&
              invalid.status().detail.input_id == 2,
          "full typed Image alpha validation");
  typed.backing[3] = doubles({1}, {0});
  invalid = typed.run(
      {{"values", region({1, 4}, {ps::Region({{0, 1}, {0, 1}})})}}, false);
  require(!invalid.ok() &&
              invalid.status().code == ps::ErrorCode::InvalidArgument &&
              invalid.status().detail.input_id == 2,
          "endpoint still collects typed handles");
  auto zero = doubles({1}, {0}), one = doubles({1}, {1});
  inputs = {take(ps::Value::from_storage({ps::ElementType::Float64, {2, 2}},
                                         ps::Region::whole({2, 2}), {0, {0, 0}},
                                         zero.storage())),
            take(ps::Value::from_storage({ps::ElementType::Float64, {1, 1, 2}},
                                         ps::Region::whole({1, 1, 2}),
                                         {0, {0, 0, 0}}, one.storage())),
            indices({0}), doubles({1}, {.5})};
  auto broadcast = execute_result(authored, inputs);
  for (unsigned column = 0; column < 2; ++column) {
    std::uint64_t bits = 0;
    require(rf::read(broadcast, {0, column}, &bits, 8).ok(),
            "broadcast Result read");
    require(bits == raw(.5), "zero-stride parametric value");
  }
  std::cout << "cache replacement, giant D/N full-budget failure, typed Image "
               "and zero strides passed\n";
}

struct FailedHandlesSource final {
  unsigned* calls;
  explicit FailedHandlesSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(ps::Status{
        ps::ErrorCode::OperationFailed, "required parametric handle producer"});
  }
};
void upstream_order(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition source;
  source.key = "manual.parametric_handles";
  source.traits.input_count = 0;
  source.traits.input_schema.clear();
  auto& output = source.traits.outputs[0];
  output.region_rule = ps::OperationRegionRule::Whole;
  output.output_schema.kind = ps::OperationPortKind::Result;
  auto schema = rf::source_schema(doubles({1, 1, 1}, {0}));
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  output.continuation_bytes = sizeof(FailedHandlesSource);
  output.maximum_dependency_stages = 8;
  source.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<FailedHandlesSource>(allocator, &calls);
  };
  require(registry->register_operation(std::move(source)).ok() &&
              registry->freeze().ok(),
          "register failing handle producer");
  Fixture fixture(node(2, profile),
                  {doubles({2, 1}, {0, 1}), doubles({1, 1, 1}, {0}),
                   indices({0, 0}), doubles({2}, {0, .5})});
  fixture.registry = registry;
  fixture.document.inputs.erase(fixture.document.inputs.begin() + 1);
  fixture.backing.erase(fixture.backing.begin() + 1);
  fixture.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
  fixture.document.nodes.push_back({2, "manual.parametric_handles", {}, {}});
  const auto empty =
      take(fixture.run({{"values", take(ps::Footprint::none({2, 1}))}}, false));
  require(calls == 0 && take(empty.results.at("values").descriptor())
                            .tensor_coverage(0)
                            .empty(),
          "Empty parametric request does not poll failed handles");
  for (auto parameters :
       {std::vector<double>{0, .5}, std::vector<double>{0, 2}}) {
    fixture.backing.back() = doubles({2}, parameters);
    auto failed = fixture.run(
        {{"values", region({2, 1}, {ps::Region({{0, 1}, {0, 1}})})}}, false);
    require(
        !failed.ok() &&
            failed.status().code == ps::ErrorCode::OperationFailed &&
            failed.status().message == "required parametric handle producer",
        "Whole preserves failing handles even for endpoint or invalid query");
  }
  require(calls == 2, "complete upstream collection");
  std::cout
      << "Whole full handle producer failure before numeric controls passed\n";
}

void retained_output(ps::CpuNumericProfile profile) {
  for (const auto dtype :
       {ps::ElementType::Float32, ps::ElementType::Float64}) {
    ps::ResourceBudget root;
    ps::ResultRef output;
    ps::ResultTensorReadWindow window;
    std::vector<std::weak_ptr<const ps::CpuStorage>> input_owners;
    const auto width = ps::Value::element_size(dtype);
    const auto one =
        dtype == ps::ElementType::Float32 ? UINT64_C(0x3f800000) : raw(1);
    {
      std::vector<ps::Value> inputs{
          doubles({2, 2}, {0, 0, 2, 0}), doubles({1, 1, 2}, {1, 2}),
          indices({0, 0, 0}), doubles({3}, {0, .5, 1})};
      for (const auto& input : inputs)
        input_owners.push_back(input.storage());
      point_math_checks::Workflow workflow(node(2, profile, dtype), inputs);
      root = workflow.root;
      output = take(workflow.run()).results.at("values");
      window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                          ps::Region::whole({3, 2})));
    }
    for (const auto& owner : input_owners)
      require(owner.expired(),
              "parametric output retires every source backing");
    require(root.statistics().live[ps::ResourceKind::Payload] == 6 * width,
            "escaped parametric Result/window share one packed owner");
    std::uint64_t bits = 0;
    require(rf::read(output, {1, 0}, &bits, width).ok() && bits == one,
            "parametric Result survives source and context retirement");
    output = {};
    const auto row = take(window.row_run({1, 1}));
    bits = 0;
    std::memcpy(&bits, row.data, width);
    require(bits == one &&
                root.statistics().live[ps::ResourceKind::Payload] == 6 * width,
            "authorized parametric window survives Result release");
    window = {};
    point_math_checks::released(root);
  }
  std::cout << "source retirement, Float32/64 Result/window owners "
               "and final all-Root release passed\n";
}

void oracle(ps::CpuNumericProfile profile) {
  unsigned degree = 0, k = 0, n = 0, d = 0, dtype = 0, requested = 0;
  while (std::cin >> degree >> k >> n >> d >> dtype >> requested) {
    std::vector<std::pair<unsigned, unsigned>> coordinates(requested);
    for (auto& at : coordinates)
      std::cin >> at.first >> at.second;
    std::vector<ps::Value> inputs;
    for (unsigned p = 0; p < 4; ++p) {
      unsigned type = 0;
      std::cin >> type;
      const auto size = p == 0   ? k * d
                        : p == 1 ? (k - 1) * (degree - 1) * d
                                 : n;
      std::vector<std::uint64_t> bits(size);
      for (auto& b : bits)
        std::cin >> std::hex >> b >> std::dec;
      inputs.push_back(
          array(static_cast<ps::ElementType>(type),
                p == 0   ? std::vector<std::uint64_t>{k, d}
                : p == 1 ? std::vector<std::uint64_t>{k - 1, degree - 1, d}
                         : std::vector<std::uint64_t>{n},
                bits));
    }
    Fixture fixture(node(degree, profile, static_cast<ps::ElementType>(dtype)),
                    inputs);
    std::vector<ps::Region> boxes;
    for (auto at : coordinates)
      boxes.emplace_back(
          std::vector<ps::RegionDimension>{{at.first, 1}, {at.second, 1}});
    auto result =
        fixture.run({{"values", region({n, d}, std::move(boxes))}}, false);
    if (!result.ok()) {
      std::cout
          << (result.status().reason == ps::FailureReason::ArithmeticOverflow
                  ? "overflow"
              : result.status().reason == ps::FailureReason::InvalidDomain
                  ? (result.status().code == ps::ErrorCode::InvalidArgument
                         ? "query"
                         : "domain")
                  : "other")
          << '\n';
      if (result.status().reason != ps::FailureReason::ArithmeticOverflow &&
          result.status().reason != ps::FailureReason::InvalidDomain)
        std::cerr << result.status().message << '\n';
      continue;
    }
    for (auto at : coordinates) {
      std::uint64_t bits = 0;
      require(rf::read(result.value().results.at("values"),
                       {at.first, at.second}, &bits, dtype == 4 ? 4 : 8)
                  .ok(),
              "oracle read");
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
      support_and_failures(profile);
      layouts_and_resources(profile);
      cache_composition_and_typed(profile);
      upstream_order(profile);
      retained_output(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
