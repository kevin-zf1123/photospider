#include "photospider/numeric/indexing.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  Fixture(ps::WorkflowNode node, std::vector<ps::Value> inputs)
      : backing(std::move(inputs)) {
    rf::declare_sources(&document, backing);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bind(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& demand,
                                   const ps::ExecutionOptions& options = {},
                                   const ps::CancellationToken& stop = {}) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = 65536;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    auto frozen = execution.freeze(plan.value().plan,
                                   bind(take(execution.resource_budget())));
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    return execution.execute_fragments(frozen.value(), {{"values", demand}},
                                       stop, options);
  }
};
ps::ResultTensorReadWindow window(const ps::ResultRef& result) {
  return take(result.acquire_tensor(
      take(result.descriptor()), 0,
      ps::Region::whole(result.schema().tensors[0].sample_shape())));
}
void support_and_publication(const Fixture& fixture,
                             const ps::DemandResult& result,
                             const ps::Footprint& query) {
  const auto& output = result.results.at("values");
  require(
      take(output.descriptor()).tensor_coverage(0) ==
          take(ps::Footprint::all(output.schema().tensors[0].sample_shape())),
      "Whole indexing publishes complete output in global coordinates");
  const auto support = take(result.dependencies.source_support());
  for (const auto& input : fixture.document.inputs) {
    const auto shape = input.result_schema->tensors[0].sample_shape();
    require(support.at(input.name.c_str()) == take(ps::Footprint::all(shape)),
            "every active indexing port has complete sample support");
    auto box = ps::Region::whole(shape).dimensions();
    for (auto& axis : box)
      axis.extent = 1;
    const auto edit =
        take(ps::Footprint::from_regions(shape, {ps::Region(box)}));
    require(take(result.dependencies.potential_dirty(input.name, edit, 1))
                    .at("values") == query,
            "every active input edit invalidates observed indexing query");
  }
}
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
    support_and_publication(fixture, answer, take(ps::Footprint::all({2, 3})));
    const std::int64_t expected[] = {1, 2, 5, 3, 4, 6};
    for (std::uint64_t i = 0; i < 2; ++i)
      for (std::uint64_t j = 0; j < 3; ++j) {
        std::int64_t actual = 0;
        require(
            rf::read(answer.results.at("values"), {i, j}, &actual, 8).ok() &&
                actual == expected[i * 3 + j],
            "non-leading concatenation values");
      }
    unsigned reads = 0;
    fixture.registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition failed;
    failed.key = "manual.unhit_concat";
    failed.traits.input_count = 0;
    failed.traits.input_schema.clear();
    auto& output = failed.traits.outputs[0];
    output.key = "values";
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.result_schema = rf::source_schema(fixture.backing[0]);
    output.output_schema.result_schema_id = output.result_schema->id;
    output.output_schema.result_schema_version = output.result_schema->version;
    output.maximum_dependency_stages = 1;
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.dependency_version = 2;
    output.regional_atomic = true;
    output.continuation_bytes = 1;
    failed.start_result = [&](const auto& query, const auto&) {
      ++reads;
      require(query.tensor_outputs &&
                  *query.tensor_outputs == take(ps::Footprint::all({2, 2})),
              "unselected concatenation producer has full Whole Need");
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::OperationFailed, "unhit A"});
    };
    require(fixture.registry->register_operation(std::move(failed)).ok(),
            "register failing producer");
    require(fixture.registry->freeze().ok(),
            "freeze failing producer registry");
    fixture.document.nodes[0].inputs[0] = ps::WorkflowNodeOutput{2, "values"};
    fixture.document.nodes.push_back({2, "manual.unhit_concat", {}, {}});
    auto requested = take(
        ps::Footprint::from_regions({2, 3}, {ps::Region({{0, 2}, {2, 1}})}));
    auto only_b = fixture.run(requested);
    require(!only_b.ok() && reads == 1 && only_b.status().message == "unhit A",
            "Whole concatenate reads even unselected input");
  }
  // Both source Results publish the original shared immutable backing.
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
  Fixture joined(node, {left, right});
  ps::ResultRef held;
  ps::ResultTensorReadWindow held_window;
  ps::ResourceBudget root;
  {
    ps::GraphContext graph(joined.document);
    auto plan = take(ps::Compiler(joined.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(joined.registry, config);
    root = take(context.resource_budget());
    auto bindings = joined.bind(root);
    auto source = window(bindings.inputs[0].result);
    auto result = take(context.execute(plan.plan, bindings));
    held = result.results.at("values");
    held_window = window(held);
    require(held_window.storage_owner_token() == source.storage_owner_token() &&
                take(held_window.row_run({0, 0})).data ==
                    take(source.row_run({0, 0})).data &&
                held.association() ==
                    ps::ResourceVector<std::uint64_t>{
                        bindings.inputs[0].result.object_id(),
                        bindings.inputs[1].result.object_id()},
            "concatenate shared owner affine view and actual associations");
    for (std::uint64_t i = 0; i < 2; ++i)
      for (std::uint64_t j = 0; j < 3; ++j) {
        std::int64_t value = 0;
        const std::int64_t expected[] = {1, 2, 5, 3, 4, 6};
        require(rf::read(held, {i, j}, &value, 8).ok() &&
                    value == expected[i * 3 + j],
                "concatenate shared view logical coordinates");
      }
  }
  std::int64_t last = 0;
  require(rf::read(held, {1, 2}, &last, 8).ok() && last == 6,
          "concatenate view outlives its context");
  held = {};
  std::memcpy(&last, take(held_window.row_run({1, 2})).data, 8);
  require(
      last == 6 && root.statistics().live[ps::ResourceKind::Referenced] >= 48,
      "escaped concatenate read window retains original backing");
  held_window = {};
  point_math_checks::released(root);
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
  support_and_publication(fixture, result, take(ps::Footprint::all({2, 3})));
  const std::int64_t expected[] = {12, 10, 12, 22, 20, 22};
  for (std::uint64_t i = 0; i < 2; ++i)
    for (std::uint64_t j = 0; j < 3; ++j) {
      std::int64_t value = 0;
      require(rf::read(result.results.at("values"), {i, j}, &value, 8).ok() &&
                  value == expected[3 * i + j],
              "gather values");
    }
  const auto wanted =
      take(ps::Footprint::from_regions({2, 3}, {ps::Region({{0, 2}, {0, 1}})}));
  auto selected = take(fixture.run(wanted));
  support_and_publication(fixture, selected, wanted);
  std::int64_t selected_last = 0;
  require(
      rf::read(selected.results.at("values"), {1, 2}, &selected_last, 8).ok() &&
          selected_last == 22,
      "sparse gather still publishes full global output");
  fixture.backing[1] = array({3}, {2, -1, 2});
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
    support_and_publication(fixture, result, take(ps::Footprint::all({3})));
    const std::int64_t expected[][3] = {{10, 3, 4},
                                        {10, 25, 34},
                                        {10, 2, 4},
                                        {10, 23, 34}};
    for (std::uint64_t j = 0; j < 3; ++j) {
      std::int64_t value = 0;
      require(rf::read(result.results.at("values"), {j}, &value, 8).ok() &&
                  value == expected[kind][j],
              "scatter fixture result");
    }
    const auto support = take(result.dependencies.source_support());
    require(support.at("input1") == take(ps::Footprint::all({3})),
            "scatter retains full index scan");
    require(support.at("input0") == take(ps::Footprint::all({3})) &&
                support.at("input2") == take(ps::Footprint::all({3})),
            "Whole scatter reads complete base and updates");
    fixture.backing[1] = array({3}, {1, 1, 3});
    auto invalid = fixture.run(
        take(ps::Footprint::from_regions({3}, {ps::Region({{0, 1}})})));
    require(invalid.status().code == ps::ErrorCode::InvalidArgument &&
                invalid.status().message.find("IndexOutOfBounds") !=
                    std::string::npos,
            "scatter validates indices outside Q");
    if (kind == 1) {
      fixture.backing[0] = array({3}, {INT64_MAX, 0, 0});
      fixture.backing[1] = array({3}, {0, 0, 1});
      fixture.backing[2] = array({3}, {1, -1, 0});
      auto cancellation = take(fixture.run(take(ps::Footprint::all({3}))));
      std::int64_t value = 0;
      require(
          rf::read(cancellation.results.at("values"), {0}, &value, 8).ok() &&
              value == INT64_MAX,
          "integer aggregate does not reject intermediate overflow");
      fixture.backing[2] = array({3}, {1, 0, 0});
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
struct MetadataControl {
  bool admitted = false;
};
struct MetadataProgram {
  ps::ResultContinuation inner;
  std::shared_ptr<MetadataControl> control;
  MetadataProgram(ps::ResultContinuation original,
                  std::shared_ptr<MetadataControl> state)
      : inner(std::move(original)), control(std::move(state)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto bounded = phase;
    std::optional<ps::ResourceLease> reservation;
    bounded.consume_work = [&](std::uint64_t amount) {
      auto status = phase.consume_work(amount);
      // resolve_indices admits this scan immediately before reserving its
      // first 16*M-byte vector. Source admission and builder setup stay intact.
      if (status.ok() && phase.tensors && !phase.tensors->empty() &&
          amount == 16384 && !reservation) {
        const auto remaining =
            UINT64_C(1000000) -
            phase.resources.statistics().live[ps::ResourceKind::Metadata];
        require(remaining > 128, "metadata fixture leaves room for setup");
        auto admitted = phase.resources.reserve(
            ps::ResourceCapacity::host(remaining - 128, remaining - 128));
        if (!admitted.ok())
          return admitted.status();
        reservation = admitted.take_value();
        control->admitted = true;
      }
      return status;
    };
    return inner.poll(bounded);
  }
};
void metadata_limit(const ps::WorkflowNode& node,
                    const std::vector<ps::Value>& backing) {
  Fixture fixture(node, backing);
  fixture.registry = ps::make_default_operation_registry(false);
  const std::weak_ptr<ps::OperationRegistry> owner = fixture.registry;
  auto control = std::make_shared<MetadataControl>();
  ps::OperationDefinition adapter;
  adapter.key = "manual.metadata." + node.operation;
  adapter.traits = take(fixture.registry->find_traits(node.operation));
  adapter.traits.cacheable = false;
  for (auto& output : adapter.traits.outputs)
    output.continuation_bytes += sizeof(MetadataProgram);
  const auto key = node.operation;
  adapter.prepare_static =
      [owner, key](
          const auto& inputs,
          const auto& parameters) -> ps::Result<ps::OperationPreparation> {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto original = registry->prepare_operation(key, inputs, parameters);
    if (!original.ok())
      return ps::Result<ps::OperationPreparation>(original.status());
    ps::OperationPreparation prepared;
    for (const auto& output : original.value()->traits().outputs) {
      ps::OperationOutputSpecialization item;
      item.metadata.result_schema =
          std::make_shared<ps::SchemaTemplate>(*output.result_schema);
      item.input_indices = output.input_indices;
      prepared.outputs.push_back(std::move(item));
    }
    prepared.state = original.take_value();
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  adapter.start_result = [owner, key, control](const auto& query,
                                               const auto& allocator) {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto forwarded = query;
    forwarded.prepared = std::shared_ptr<const ps::PreparedOperation>(
        query.prepared,
        static_cast<const ps::PreparedOperation*>(query.prepared->state()));
    auto original = registry->start_result(key, forwarded, allocator);
    if (!original.ok())
      return original;
    return ps::ResultContinuation::make<MetadataProgram>(
        allocator, original.take_value(), control);
  };
  fixture.document.nodes[0].operation = adapter.key;
  require(fixture.registry->register_operation(std::move(adapter)).ok(),
          "register metadata-limited indexing");
  require(fixture.registry->freeze().ok(), "freeze metadata-limited indexing");
  ps::ResourceBudget root;
  {
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->capacity[ps::ResourceKind::Metadata] = 1000000;
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto frozen = take(execution.freeze(plan.plan, fixture.bind(root)));
    auto result = execution.execute(frozen);
    require(!result.ok() &&
                result.status().code == ps::ErrorCode::ResourceExhausted &&
                result.status().reason == ps::FailureReason::CapacityLimit &&
                control->admitted,
            "index metadata plan fails after actual scan admission");
  }
  point_math_checks::released(root);
}
void failure_diagnostics(ps::CpuNumericProfile profile) {
  auto node = take(ps::numeric::scatter_sum_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, 0, profile));
  std::vector<ps::Value> inputs{array({5}, {1, 2, 3, 4, INT64_MAX}),
                                array({1}, {4}), array({1}, {1})};
  ps::ResourceBudget root;
  {
    point_math_checks::Workflow workflow(node, inputs);
    root = workflow.root;
    auto result = workflow.run();
    require(
        !result.ok() &&
            result.status().reason == ps::FailureReason::ArithmeticOverflow &&
            result.status().detail.scope == ps::FailureScope::Run &&
            result.status().message.find("output=[4,") != std::string::npos,
        "late aggregate overflow fails complete output at global coordinate");
  }
  point_math_checks::released(root);
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
  metadata_limit(node, large);
  std::cout << "Whole scatter overflow, work/output/scratch budgets and active "
               "cancellation passed; counters=N/A\n";
}
void layouts_environment(ps::CpuNumericProfile profile) {
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
    auto node = take(ps::numeric::gather_node(1, ps::WorkflowInputReference{1},
                                              ps::WorkflowInputReference{2}, 0,
                                              profile));
    point_math_checks::Workflow workflow(node, inputs);
    auto output = take(workflow.run()).results.at("values");
    const std::int64_t expected[] = {0, 3, 0};
    for (std::uint64_t j = 0; j < 3; ++j) {
      std::int64_t value = -1;
      require(rf::read(output, {j}, &value, 8).ok(), "strided Result read");
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
      ps::WorkflowNode authored{1,
                                "array." + std::string(operation) + suffix,
                                {},
                                {{"axis", static_cast<std::int64_t>(0)}}};
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = rounding;
      point_math_checks::Workflow workflow(authored, inputs, {}, control);
      auto output = take(workflow.run()).results.at("values");
      require(control->computation_polls > 0,
              "fenv checks enter actual indexing worker callback");
      require(fegetround() == rounding &&
                  fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
              "index arithmetic preserves caller fenv");
      std::uint64_t nan = 0;
      require(rf::read(output, {0}, &nan, 8).ok(), "fenv Result read");
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
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ResourceBudget root;
  ps::ResultRef retained;
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
  {
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto bindings = fixture.bind(root);
    auto demand = take(execution.open_demand(plan.plan, bindings));
    retained = take(demand.request(query)).results.at("values");
    auto shared = take(demand.request(query));
    require(shared.results.at("values").object_id() == retained.object_id(),
            "same Frozen shares gather Result identity");
    auto fresh_bindings = fixture.bind(root);
    auto fresh = take(execution.freeze(plan.plan, fresh_bindings));
    auto warm = take(execution.execute_fragments(fresh, query));
    require(warm.diagnostics.cache_hits > 0, "completed gather cache hit");
    require(warm.results.at("values").association() ==
                ps::ResourceVector<std::uint64_t>{
                    fresh_bindings.inputs[0].result.object_id(),
                    fresh_bindings.inputs[1].result.object_id()},
            "cache replay associates current gather sources");
    bindings.inputs[1].result =
        point_math_checks::source(root, array({1}, {2}));
    require(demand.replace_bindings(bindings).ok(),
            "replace gather Result index");
    auto changed = take(demand.request(query));
    std::int64_t value = 0;
    std::uint64_t computed = 0;
    for (const auto& timing : changed.diagnostics.operation_timings)
      computed += timing.computed_elements;
    require(changed.diagnostics.cache_hits == 0 && computed == 1 &&
                rf::read(changed.results.at("values"), {0}, &value, 8).ok() &&
                value == 30,
            "changed gather index invalidates and recomputes output");
    require(take(changed.dependencies.source_support()).at("input0") ==
                take(ps::Footprint::all({3})),
            "new gather witness retains full source support");
  }
  std::int64_t value = 0;
  require(rf::read(retained, {0}, &value, 8).ok() && value == 10 &&
              root.statistics().live[ps::ResourceKind::Payload] >= 8,
          "dense gather Result remains readable after context retirement");
  retained = {};
  point_math_checks::released(root);
  auto control = std::make_shared<point_math_checks::Control>();
  control->maximum_work = 1;
  {
    point_math_checks::Workflow limits(node, fixture.backing, {}, control);
    root = limits.root;
    auto failed = limits.run();
    require(!failed.ok() &&
                failed.status().reason == ps::FailureReason::WorkLimit &&
                control->computation_polls > 0,
            "gather work limit fails in actual callback");
  }
  point_math_checks::released(root);
  std::cout
      << "gather completed cache/current associations, index invalidation "
         "and escaped dense owner passed\n";
}
void typed_empty_cancel(ps::CpuNumericProfile profile) {
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  const auto image = [&](bool invalid) {
    const float rgba[] = {1, 0, 0, invalid ? 2.0F : 1.0F};
    auto buffer = take(ps::BufferAllocator{}.allocate(sizeof(rgba)));
    std::memcpy(buffer.data(), rgba, sizeof(rgba));
    return take(ps::Value::from_storage(
        {ps::ElementType::Float32, {1, 1, 4}}, ps::Region::whole({1, 1, 4}),
        {0, {16, 16, 4}}, std::move(buffer).freeze()));
  };
  const auto bad = image(true), good = image(false);
  const auto idx = array({4}, {0, 1, 2, 3});
  for (const auto* operation :
       {"concatenate", "gather", "scatter_replace", "scatter_sum",
        "scatter_minimum", "scatter_maximum"}) {
    const std::string name(operation);
    const auto suffix = profile == ps::CpuNumericProfile::Strict ? "_strict"
                        : profile == ps::CpuNumericProfile::AppleSiliconNeon
                            ? "_accelerated_apple_silicon"
                            : "_accelerated_x86_64";
    ps::WorkflowNode node{1,
                          "array." + name + suffix,
                          {},
                          {{"axis", static_cast<std::int64_t>(2)}}};
    if (name == "concatenate")
      node.parameters["layout"] = std::string("dense");
    std::vector<ps::Value> inputs =
        name == "concatenate" ? std::vector<ps::Value>{good, good}
        : name == "gather"    ? std::vector<ps::Value>{good, idx}
                              : std::vector<ps::Value>{good, idx, good};
    for (unsigned port = 0; port < inputs.size(); ++port)
      node.inputs.push_back(ps::WorkflowInputReference{port + 1});
    const auto shape = name == "concatenate"
                           ? std::vector<std::uint64_t>{1, 1, 8}
                           : std::vector<std::uint64_t>{1, 1, 4};
    for (unsigned port : name == "concatenate" ? std::vector<unsigned>{0, 1}
                         : name == "gather"    ? std::vector<unsigned>{0}
                                               : std::vector<unsigned>{0, 2}) {
      Fixture fixture(node, inputs);
      for (unsigned color_port :
           name == "concatenate" ? std::vector<unsigned>{0, 1}
           : name == "gather"    ? std::vector<unsigned>{0}
                                 : std::vector<unsigned>{0, 2}) {
        auto schema = rf::source_schema(good);
        schema.tensors[0].facets = {take(ps::encode_color_array(color))};
        schema.tensors[0].atomic_trailing_axes = 1;
        fixture.document.inputs[color_port].result_schema =
            std::make_shared<ps::SchemaTemplate>(schema);
      }
      fixture.backing[port] = bad;
      const auto green = take(ps::Footprint::from_regions(
          shape, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
      auto failed = fixture.run(green);
      require(!failed.ok() &&
                  failed.status().code == ps::ErrorCode::InvalidArgument &&
                  failed.status().reason == ps::FailureReason::InvalidDomain &&
                  failed.status().detail.input_id == port + 1,
              "Whole indexing validates typed alpha outside requested green");
      auto empty = take(fixture.run(take(ps::Footprint::none(shape))));
      require(take(empty.results.at("values").descriptor())
                  .tensor_coverage(0)
                  .empty(),
              "Empty indexing skips invalid typed alpha");
      for (const auto& timing : empty.diagnostics.operation_timings)
        require(timing.computed_elements == 0,
                "Empty indexing performs no computation");
      for (const auto& support : take(empty.dependencies.source_support()))
        require(support.second.empty(), "Empty indexing has no sample support");
      ps::CancellationSource cancellation;
      cancellation.cancel();
      require(fixture.run(green, {}, cancellation.token()).status().code ==
                  ps::ErrorCode::Cancelled,
              "pre-cancelled indexing precedes bad typed payload");
      fixture.backing[port] = good;
      auto valid = take(fixture.run(green));
      support_and_publication(fixture, valid, green);
      require(
          take(valid.results.at("values").descriptor()).tensor_coverage(0) ==
                  take(ps::Footprint::all(shape)) &&
              valid.results.at("values").schema().tensors[0].facets.empty(),
          "typed indexing positive control has complete untyped publication");
      for (unsigned channel = 0; channel < shape[2]; ++channel) {
        float value = 0;
        const float expected =
            name == "scatter_sum"
                ? (channel == 0 || channel == 3 ? 2 : 0)
                : (channel % 4 == 0 || channel % 4 == 3 ? 1 : 0);
        require(rf::read(valid.results.at("values"), {0, 0, channel}, &value, 4)
                        .ok() &&
                    value == expected,
                "typed same-schema positive control output");
      }
    }
  }
  std::cout
      << "all six indexing operations: typed every active color port, Empty "
         "and pre-cancellation passed\n";
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
                     auto status = rf::read(result.value().results.at("values"),
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
