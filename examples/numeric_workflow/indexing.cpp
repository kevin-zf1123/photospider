#include "photospider/numeric/indexing.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace {
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
ps::Value array(std::vector<std::uint64_t> shape,
                const std::vector<std::int64_t>& values) {
  auto buffer = take(ps::BufferAllocator{}.allocate(values.size() * 8));
  std::memcpy(buffer.data(), values.data(), buffer.size());
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = 8;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({ps::ElementType::Int64, shape},
                                      ps::Region::whole(shape), {0, strides},
                                      std::move(buffer).freeze()));
}
struct Fixture {
  std::uint64_t maximum_work = UINT64_C(1) << 40;
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto name = "input" + std::to_string(i);
      const auto& value = inputs[i];
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
    }
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& demand,
                                   const ps::ExecutionOptions& options = {}) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 65536;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->maximum_work = maximum_work;
    ps::ExecutionContext execution(registry, config);
    auto frozen = execution.freeze(plan.value().plan, bindings);
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    return execution.execute_fragments(frozen.value(), {{"values", demand}}, {},
                                       options);
  }
};
void concatenate(ps::CpuNumericProfile profile) {
  for (auto layout :
       {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
    auto node = take(ps::numeric::concatenate_node(
        1, {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}}, 1,
        layout, profile));
    Fixture fixture(node, {array({2, 2}, {1, 2, 3, 4}), array({2, 1}, {5, 6})});
    auto full = fixture.run(take(ps::Footprint::all({2, 3})));
    if (layout == ps::numeric::ArrayLayout::View) {
      require(!full.ok() && full.status().message.find("ViewUnavailable") !=
                                std::string::npos,
              "multi-owner concatenate View fails");
      fixture.document.nodes[0].parameters["layout"] = std::string("dense");
      full = fixture.run(take(ps::Footprint::all({2, 3})));
    }
    auto answer = take(std::move(full));
    const std::int64_t expected[] = {1, 2, 5, 3, 4, 6};
    for (std::uint64_t i = 0; i < 2; ++i)
      for (std::uint64_t j = 0; j < 3; ++j) {
        std::int64_t actual = 0;
        require(answer.values.at("values").read({i, j}, &actual, 8).ok() &&
                    actual == expected[i * 3 + j],
                "non-leading concatenation values");
      }
    unsigned reads = 0;
    fixture.registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition failed;
    failed.key = "manual.unhit_concat";
    failed.traits.input_count = 0;
    failed.traits.input_schema.clear();
    failed.traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
    failed.traits.outputs[0].fixed_output_shape = {2, 2};
    failed.traits.outputs[0].output_element_type = ps::ElementType::Int64;
    failed.callback = [&](const auto&) {
      ++reads;
      return ps::Result<ps::Value>(
          ps::Status{ps::ErrorCode::OperationFailed, "unhit A"});
    };
    require(fixture.registry->register_operation(std::move(failed)).ok(),
            "register failing producer");
    require(fixture.registry->freeze().ok(),
            "freeze failing producer registry");
    fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "value"};
    fixture.document.nodes.push_back({2, "manual.unhit_concat", {}, {}});
    auto requested = take(
        ps::Footprint::from_regions({2, 3}, {ps::Region({{0, 2}, {2, 1}})}));
    auto only_b = fixture.run(requested);
    require(!only_b.ok() && reads == 1,
            "Whole concatenate reads even unselected input");
  }
  // Two strided Values share one complete affine backing through public invoke.
  auto whole = array({2, 3}, {1, 2, 5, 3, 4, 6});
  auto left = take(ps::Value::from_storage({ps::ElementType::Int64, {2, 2}},
                                           ps::Region::whole({2, 2}),
                                           {0, {24, 8}}, whole.storage()));
  auto right = take(ps::Value::from_storage({ps::ElementType::Int64, {2, 1}},
                                            ps::Region::whole({2, 1}),
                                            {16, {24, 8}}, whole.storage()));
  auto node = take(ps::numeric::concatenate_node(
      1, {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}}, 1,
      ps::numeric::ArrayLayout::View, profile));
  const std::vector<ps::Value> inputs{left, right};
  const std::vector<ps::Region> demands{left.region(), right.region()};
  ps::OperationInvocation call(inputs, demands, node.parameters);
  auto result =
      take(ps::make_default_operation_registry()->invoke(node.operation, call));
  require(result.storage() == whole.storage() &&
              result.layout().byte_strides == whole.layout().byte_strides,
          "concatenate shared owner affine view");
  std::cout << "concatenate: [[1,2,5],[3,4,6]], Whole Dense, shared-owner View "
               "and unselected failure passed\n";
}
void gather(ps::CpuNumericProfile profile) {
  auto node =
      take(ps::numeric::gather_node(1, ps::WorkflowInputReference{1},
                                    ps::WorkflowInputReference{2}, 1, profile));
  Fixture fixture(
      node, {array({2, 3}, {10, 11, 12, 20, 21, 22}), array({3}, {2, 0, 2})});
  auto result = take(fixture.run(take(ps::Footprint::all({2, 3}))));
  const std::int64_t expected[] = {12, 10, 12, 22, 20, 22};
  for (std::uint64_t i = 0; i < 2; ++i)
    for (std::uint64_t j = 0; j < 3; ++j) {
      std::int64_t value = 0;
      require(result.values.at("values").read({i, j}, &value, 8).ok() &&
                  value == expected[3 * i + j],
              "gather values");
    }
  const auto wanted =
      take(ps::Footprint::from_regions({2, 3}, {ps::Region({{0, 2}, {0, 1}})}));
  fixture.bindings.inputs[1].value = array({3}, {2, -1, 2});
  auto partial = fixture.run(wanted);
  require(
      !partial.ok() && partial.status().detail.scope == ps::FailureScope::Run,
      "gather validates all indices even for partial output");
  auto invalid = fixture.run(take(ps::Footprint::all({2, 3})));
  require(invalid.status().code == ps::ErrorCode::InvalidArgument &&
              invalid.status().reason == ps::FailureReason::InvalidDomain &&
              !invalid.status().detail.atom &&
              invalid.status().detail.scope == ps::FailureScope::Run &&
              invalid.status().message.find("IndexOutOfBounds") !=
                  std::string::npos,
          "gather index error attribution");
  std::cout << "gather: [[12,10,12],[22,20,22]], repeated target support and "
               "observed-index validation passed\n";
}
void scatter(ps::CpuNumericProfile profile) {
  for (unsigned kind = 0; kind < 4; ++kind) {
    auto node = kind == 0   ? take(ps::numeric::scatter_replace_node(
                                1, ps::WorkflowInputReference{1},
                                ps::WorkflowInputReference{2},
                                ps::WorkflowInputReference{3}, 0, profile))
                : kind == 1 ? take(ps::numeric::scatter_sum_node(
                                  1, ps::WorkflowInputReference{1},
                                  ps::WorkflowInputReference{2},
                                  ps::WorkflowInputReference{3}, 0, profile))
                : kind == 2 ? take(ps::numeric::scatter_minimum_node(
                                  1, ps::WorkflowInputReference{1},
                                  ps::WorkflowInputReference{2},
                                  ps::WorkflowInputReference{3}, 0, profile))
                            : take(ps::numeric::scatter_maximum_node(
                                  1, ps::WorkflowInputReference{1},
                                  ps::WorkflowInputReference{2},
                                  ps::WorkflowInputReference{3}, 0, profile));
    Fixture fixture(
        node, {array({3}, {10, 20, 30}), array({3}, {1, 1, 2}),
               array({3}, kind == 3 ? std::vector<std::int64_t>{22, 23, 34}
                                    : std::vector<std::int64_t>{2, 3, 4})});
    auto result = take(fixture.run(take(ps::Footprint::all({3}))));
    const std::int64_t expected[][3] = {{10, 3, 4},
                                        {10, 25, 34},
                                        {10, 2, 4},
                                        {10, 23, 34}};
    for (std::uint64_t j = 0; j < 3; ++j) {
      std::int64_t value = 0;
      require(result.values.at("values").read({j}, &value, 8).ok() &&
                  value == expected[kind][j],
              "scatter fixture result");
    }
    const auto support = take(result.dependencies.source_support());
    require(support.at("input1") == take(ps::Footprint::all({3})),
            "scatter retains full index scan");
    require(support.at("input0") == take(ps::Footprint::all({3})) &&
                support.at("input2") == take(ps::Footprint::all({3})),
            "Whole scatter reads complete base and updates");
    fixture.bindings.inputs[1].value = array({3}, {1, 1, 3});
    auto invalid = fixture.run(
        take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})})));
    require(invalid.status().code == ps::ErrorCode::InvalidArgument &&
                invalid.status().message.find("IndexOutOfBounds") !=
                    std::string::npos,
            "scatter validates indices outside Q");
    if (kind == 1) {
      fixture.bindings.inputs[0].value = array({3}, {INT64_MAX, 0, 0});
      fixture.bindings.inputs[1].value = array({3}, {0, 0, 1});
      fixture.bindings.inputs[2].value = array({3}, {1, -1, 0});
      auto cancellation = take(fixture.run(take(ps::Footprint::all({3}))));
      std::int64_t value = 0;
      require(cancellation.values.at("values").read({0}, &value, 8).ok() &&
                  value == INT64_MAX,
              "integer aggregate does not reject intermediate overflow");
      fixture.bindings.inputs[2].value = array({3}, {1, 0, 0});
      auto overflow = fixture.run(take(ps::Footprint::all({3})));
      require(overflow.status().code == ps::ErrorCode::OperationFailed &&
                  overflow.status().reason ==
                      ps::FailureReason::ArithmeticOverflow &&
                  !overflow.status().detail.atom &&
                  overflow.status().detail.scope == ps::FailureScope::Run,
              "integer final overflow belongs to actual atom");
    }
  }
  std::cout << "scatter replace/sum/min/max: "
               "[10,3,4]/[10,25,34]/[10,2,4]/[10,23,34], exact contributors "
               "and final integer overflow passed\n";
}
ps::ValueFragments fragments(const ps::Value& value, bool full) {
  return take(ps::ValueFragments::create(
      value.descriptor(), value.facets(),
      take(full ? ps::Footprint::all(value.descriptor().shape)
                : ps::Footprint::none(value.descriptor().shape)),
      full ? std::vector<ps::Value>{value} : std::vector<ps::Value>{}));
}
void failure_diagnostics(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto node = take(ps::numeric::scatter_sum_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, 0, profile));
  std::vector<ps::Value> inputs{array({5}, {1, 2, 3, 4, INT64_MAX}),
                                array({1}, {4}), array({1}, {1})};
  std::vector<ps::Region> demands;
  for (const auto& value : inputs)
    demands.push_back(value.region());
  ps::ResourceBudget budget(ps::ResourceLimits{});
  {
    ps::ResourceAllocationScope scope(budget);
    ps::OperationInvocation call(inputs, demands, node.parameters,
                                 ps::Backend::Cpu, {}, ps::Region::whole({5}),
                                 budget.allocator());
    auto result = registry->invoke(node.operation, call);
    require(
        !result.ok() &&
            result.status().reason == ps::FailureReason::ArithmeticOverflow &&
            result.status().detail.scope == ps::FailureScope::Run,
        "late aggregate overflow fails complete output");
  }
  require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
              budget.statistics().live[ps::ResourceKind::Metadata] == 0,
          "failure releases output, scratch and dynamic index plan");
  std::vector<std::int64_t> base(16384, 1), indices(16384), updates(16384, 2);
  for (unsigned i = 0; i < indices.size(); ++i)
    indices[i] = i;
  const std::vector<ps::Value> large{
      array({16384}, base), array({16384}, indices), array({16384}, updates)};
  for (const auto* name : {"scatter_replace", "scatter_sum", "scatter_minimum",
                           "scatter_maximum", "gather", "concatenate"}) {
    auto checked_node = node;
    auto suffix =
        node.operation.substr(std::string("array.scatter_sum").size());
    checked_node.operation = std::string("array.") + name + suffix;
    auto checked_inputs = large;
    if (std::string(name) == "gather")
      checked_inputs.resize(2);
    if (std::string(name) == "concatenate") {
      checked_inputs = {large[0], large[2]};
      checked_node.parameters["layout"] = std::string("dense");
    }
    point_math_checks::resources(
        checked_node, checked_inputs,
        std::string(name) == "concatenate" ? 262144 : 131072);
  }
  ps::ResourceLimits limited;
  limited.capacity[ps::ResourceKind::Metadata] = 128;
  ps::ResourceBudget metadata_budget(limited);
  {
    ps::ResourceAllocationScope scope(metadata_budget);
    std::vector<ps::Region> regions;
    for (const auto& value : large)
      regions.push_back(value.region());
    ps::OperationInvocation call(large, regions, node.parameters,
                                 ps::Backend::Cpu, {}, large[0].region(),
                                 metadata_budget.allocator());
    auto failed = registry->invoke(node.operation, call);
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::ResourceExhausted,
            "index plan metadata capacity failure");
  }
  require(metadata_budget.statistics().live[ps::ResourceKind::Metadata] == 0 &&
              metadata_budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "index metadata failure releases ownership");
  std::cout << "Whole scatter overflow, work/output/scratch budgets and active "
               "cancellation passed; counters=N/A\n";
}
void layouts_environment(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto buffer = take(ps::BufferAllocator{}.allocate(33));
  const std::int64_t data[] = {0, 1, 2, 3};
  std::memcpy(buffer.data() + 1, data, sizeof(data));
  const auto owner = std::move(buffer).freeze();
  const auto indices = array({3}, {3, 0, 3});
  for (bool zero : {false, true}) {
    const auto source = take(ps::Value::from_storage(
        {ps::ElementType::Int64, {4}}, ps::Region::whole({4}),
        {25, {zero ? 0 : -8}}, owner));
    const std::vector<ps::Value> inputs{source, indices};
    const std::vector<ps::Region> demands{source.region(), indices.region()};
    auto node = take(ps::numeric::gather_node(1, ps::WorkflowInputReference{1},
                                              ps::WorkflowInputReference{2}, 0,
                                              profile));
    ps::OperationInvocation call(inputs, demands, node.parameters);
    auto output = take(registry->invoke(node.operation, call));
    const std::int64_t expected[] = {0, 3, 0};
    for (std::uint64_t j = 0; j < 3; ++j) {
      std::int64_t value = -1;
      std::memcpy(&value,
                  output.bytes().data() + take(output.byte_address({j})), 8);
      require(value == (zero ? 3 : expected[j]),
              "gather unaligned negative/zero source strides");
    }
  }
  fenv_t original;
  require(fegetenv(&original) == 0, "save fenv");
  const std::uint64_t raw[] = {UINT64_C(0x7ff0000000000001),
                               UINT64_C(0x8000000000000000),
                               UINT64_C(0x7ff0000000000000), 1};
  auto raw_buffer = take(ps::BufferAllocator{}.allocate(sizeof(raw)));
  std::memcpy(raw_buffer.data(), raw, sizeof(raw));
  const auto source = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {4}}, ps::Region::whole({4}), {0, {8}},
      std::move(raw_buffer).freeze()));
  for (int rounding : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    require(fesetround(rounding) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set caller fenv");
    for (const auto* operation : {"gather", "scatter_replace", "scatter_sum",
                                  "scatter_minimum", "scatter_maximum"}) {
      const std::string suffix =
          profile == ps::CpuNumericProfile::Strict ? "_strict"
          : profile == ps::CpuNumericProfile::AppleSiliconNeon
              ? "_accelerated_apple_silicon"
              : "_accelerated_x86_64";
      const auto index = array({4}, {0, 1, 2, 3});
      std::vector<ps::Value> inputs{source, index};
      if (std::string(operation) != "gather")
        inputs.push_back(source);
      std::vector<ps::Region> demands;
      for (const auto& value : inputs)
        demands.push_back(value.region());
      const std::map<std::string, ps::ParameterValue> parameters{
          {"axis", static_cast<std::int64_t>(0)}};
      ps::OperationInvocation call(inputs, demands, parameters);
      auto output = take(
          registry->invoke("array." + std::string(operation) + suffix, call));
      require(fegetround() == rounding &&
                  fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
              "index arithmetic preserves caller fenv");
      std::uint64_t nan = 0;
      std::memcpy(&nan, output.bytes().data() + take(output.byte_address({0})),
                  8);
      const auto expected = std::string(operation) == "gather" ||
                                    std::string(operation) == "scatter_replace"
                                ? raw[0]
                                : raw[0] | (UINT64_C(1) << 51);
      require(nan == expected, "copy versus arithmetic sNaN policy");
    }
  }
  require(fesetenv(&original) == 0, "restore fenv");
  std::cout << "index layouts and four fenv modes: strided reads, raw/quiet "
               "NaN policy passed\n";
}
void cache_and_limits(ps::CpuNumericProfile profile) {
  auto node =
      take(ps::numeric::gather_node(1, ps::WorkflowInputReference{1},
                                    ps::WorkflowInputReference{2}, 0, profile));
  Fixture fixture(node, {array({3}, {10, 20, 30}), array({1}, {0})});
  ps::InputSnapshotStore snapshots;
  for (auto& binding : fixture.bindings.inputs) {
    binding.snapshot = std::make_shared<const ps::InputSnapshot>(
        take(snapshots.import_value(binding.value)));
    binding.value = {};
  }
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(fixture.registry, config);
  auto demand = take(execution.open_demand(plan.plan, fixture.bindings));
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  take(demand.request(query));
  require(take(demand.request(query)).diagnostics.cache_hits > 0,
          "warm gather cache");
  fixture.bindings.inputs[1].snapshot =
      std::make_shared<const ps::InputSnapshot>(
          take(snapshots.import_value(array({1}, {2}))));
  require(demand.replace_bindings(fixture.bindings).ok(),
          "replace gather indices");
  auto changed = take(demand.request(query));
  std::int64_t value = 0;
  require(changed.values.at("values").read({0}, &value, 8).ok() && value == 30,
          "changed gather index replans result");
  require(take(changed.dependencies.source_support()).at("input0") ==
              take(ps::Footprint::all({3})),
          "new cache witness replaces old source support");
  Fixture limits(node, {array({3}, {10, 20, 30}), array({1}, {0})});
  ps::ExecutionOptions options;
  limits.maximum_work = 1;
  require(limits.run(query.at("values"), options).status().reason ==
              ps::FailureReason::WorkLimit,
          "gather work exhaustion");
  std::cout << "warm gather cache, changed-index replanning and work/state "
               "limits passed\n";
}
void typed_empty_cancel(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  const auto image = [&](bool invalid) {
    const std::uint32_t bits[] = {0x3f800000, 0, 0,
                                  invalid ? 0x40000000U : 0x3f800000U};
    auto buffer = take(ps::BufferAllocator{}.allocate(sizeof(bits)));
    std::memcpy(buffer.data(), bits, sizeof(bits));
    return take(ps::Value::from_storage(
        {ps::ElementType::Float32, {1, 1, 4}}, ps::Region::whole({1, 1, 4}),
        {0, {16, 16, 4}}, std::move(buffer).freeze(), {facet}));
  };
  const auto bad = image(true), good = image(false);
  const auto idx = array({4}, {0, 1, 2, 3});
  for (const auto* operation :
       {"concatenate", "gather", "scatter_replace", "scatter_sum",
        "scatter_minimum", "scatter_maximum"}) {
    const std::string name(operation);
    const std::string suffix =
        profile == ps::CpuNumericProfile::Strict ? "_strict"
        : profile == ps::CpuNumericProfile::AppleSiliconNeon
            ? "_accelerated_apple_silicon"
            : "_accelerated_x86_64";
    const auto key = "array." + name + suffix;
    std::vector<ps::Value> inputs =
        name == "concatenate" ? std::vector<ps::Value>{bad, good}
        : name == "gather"    ? std::vector<ps::Value>{bad, idx}
                              : std::vector<ps::Value>{bad, idx, good};
    std::vector<ps::Region> demands;
    for (const auto& value : inputs)
      demands.push_back(value.region());
    std::map<std::string, ps::ParameterValue> parameters{
        {"axis", static_cast<std::int64_t>(2)}};
    if (name == "concatenate")
      parameters["layout"] = std::string("view");
    const std::vector<std::uint64_t> shape =
        name == "concatenate" ? std::vector<std::uint64_t>{1, 1, 8}
                              : std::vector<std::uint64_t>{1, 1, 4};
    ps::OperationInvocation invocation(inputs, demands, parameters,
                                       ps::Backend::Cpu, {},
                                       ps::Region::whole(shape));
    require(!registry->invoke(key, invocation).ok(),
            "Whole validates every active typed input");
    ps::WorkflowNode authored{1, key, {}, parameters};
    for (unsigned port = 0; port < inputs.size(); ++port)
      authored.inputs.push_back(ps::WorkflowInputReference{port + 1});
    Fixture empty_fixture(authored, inputs);
    require(empty_fixture.run(take(ps::Footprint::none(shape))).ok(),
            "Whole Empty skips invalid typed payload");
    ps::CancellationSource cancellation;
    cancellation.cancel();
    ps::OperationInvocation cancelled(inputs, demands, parameters,
                                      ps::Backend::Cpu, cancellation.token(),
                                      ps::Region::whole(shape));
    require(registry->invoke(key, cancelled).status().code ==
                ps::ErrorCode::Cancelled,
            "pre-cancelled Whole indexing");
  }
  std::cout << "all six indexing operations: typed actual-read closure, Empty "
               "and cancellation cleanup passed\n";
}
std::vector<std::uint64_t> shape_list(const std::string& text) {
  std::vector<std::uint64_t> result;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(',', begin);
    result.push_back(std::stoull(text.substr(begin, end - begin)));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}
void oracle(ps::CpuNumericProfile profile) {
  std::string operation, layout;
  unsigned dtype = 0, axis = 0, count = 0;
  const auto suffix = profile == ps::CpuNumericProfile::Strict ? "_strict"
                      : profile == ps::CpuNumericProfile::AppleSiliconNeon
                          ? "_accelerated_apple_silicon"
                          : "_accelerated_x86_64";
  while (std::cin >> operation >> dtype >> axis >> layout >> count) {
    std::vector<ps::Value> values;
    ps::WorkflowNode node{1,
                          "array." + operation + suffix,
                          {},
                          {{"axis", static_cast<std::int64_t>(axis)}}};
    if (operation == "concatenate")
      node.parameters["layout"] = layout;
    for (unsigned port = 0; port < count; ++port) {
      std::string encoded;
      std::cin >> encoded;
      const auto shape = shape_list(encoded);
      std::uint64_t elements = 1;
      for (auto extent : shape)
        elements *= extent;
      const auto type = port == 1 && operation != "concatenate"
                            ? ps::ElementType::Int64
                            : static_cast<ps::ElementType>(dtype);
      const auto width = ps::Value::element_size(type);
      auto buffer = take(ps::BufferAllocator{}.allocate(elements * width));
      for (std::uint64_t j = 0; j < elements; ++j) {
        std::uint64_t bits = 0;
        std::cin >> std::hex >> bits >> std::dec;
        std::memcpy(buffer.data() + j * width, &bits, width);
      }
      std::vector<std::int64_t> strides(shape.size());
      std::int64_t stride = width;
      for (std::size_t j = shape.size(); j; --j) {
        strides[j - 1] = stride;
        stride *= shape[j - 1];
      }
      values.push_back(take(
          ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                  {0, strides}, std::move(buffer).freeze())));
      node.inputs.push_back(ps::WorkflowInputReference{port + 1});
    }
    auto output_shape = values[0].descriptor().shape;
    if (operation == "concatenate") {
      output_shape[axis] = 0;
      for (const auto& value : values)
        output_shape[axis] += value.descriptor().shape[axis];
    } else if (operation == "gather") {
      output_shape[axis] = values[1].descriptor().shape[0];
    }
    Fixture fixture(node, values);
    const auto all = take(ps::Footprint::all(output_shape));
    auto result = fixture.run(all);
    if (!result.ok()) {
      if (result.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else if (result.status().message.find("IndexOutOfBounds") !=
               std::string::npos)
        std::cout << "index\n";
      else if (result.status().message.find("ViewUnavailable") !=
               std::string::npos)
        std::cout << "error\n";
      else
        throw std::runtime_error(result.status().message);
      continue;
    }
    bool first = true;
    const auto width =
        ps::Value::element_size(static_cast<ps::ElementType>(dtype));
    require(all.visit(
                   [&](const auto& coordinate) {
                     std::uint64_t bits = 0;
                     auto status = result.value().values.at("values").read(
                         coordinate, &bits, width);
                     if (status.ok()) {
                       if (!first)
                         std::cout << ' ';
                       first = false;
                       std::cout << std::hex << bits << std::dec;
                     }
                     return status;
                   },
                   4096)
                .ok(),
            "oracle result read");
    std::cout << '\n';
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile must be strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      concatenate(profile);
      gather(profile);
      scatter(profile);
      failure_diagnostics(profile);
      layouts_environment(profile);
      cache_and_limits(profile);
      typed_empty_cancel(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
