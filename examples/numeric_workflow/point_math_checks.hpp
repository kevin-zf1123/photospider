#pragma once

#include <fenv.h>  // NOLINT(build/c++11)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "result_fixture.hpp"  // NOLINT(build/include_subdir)

namespace point_math_checks {
inline void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
// Publish caller-owned immutable backing under the execution Root. Its full
// allocation is admitted as Referenced; metadata, grants and output stay
// Root-owned.
inline ps::ResultRef source(const ps::ResourceBudget& root,
                            const ps::Value& value,
                            const ps::SchemaTemplate* declared = nullptr) {
  auto schema =
      declared ? *declared : numeric_result_fixture::source_schema(value);
  auto builder =
      take(ps::ResultBuilder::start(root, schema, "manual.point.source", {}, {},
                                    128, 128, value.resources()));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "point source descriptor");
  require(builder
              .publish_tensor(
                  0, value.region(), value.layout(), value.storage(),
                  take(ps::ResultRelation::cartesian(
                      root, take(schema.tensors[0].sample_count()), {})),
                  {true, true, true, true})
              .ok(),
          "point source Result publication");
  return take(builder.seal());
}
inline ps::ExecutionBindings bindings(const ps::ResourceBudget& root,
                                      const std::vector<ps::Value>& backing,
                                      const ps::WorkflowDocument& document) {
  ps::ExecutionBindings result;
  for (std::size_t i = 0; i < backing.size(); ++i)
    result.inputs.push_back(
        {document.inputs[i].name,
         source(root, backing[i], document.inputs[i].result_schema.get())});
  return result;
}
// The test adapter observes real continuation work, excluding source admission,
// freeze and coordinator work. Its mutable counters are not compiled state.
struct Control final {
  std::optional<int> rounding;
  std::uint64_t maximum_work = UINT64_MAX, cancel_after = UINT64_MAX;
  ps::CancellationSource cancellation;
  std::uint64_t work = 0, polls = 0, computation_polls = 0;
  bool measure_computation = false;
  double computation_us = 0;
};
struct CheckedProgram final {
  ps::ResultContinuation original;
  std::shared_ptr<Control> control;
  CheckedProgram(ps::ResultContinuation program, std::shared_ptr<Control> state)
      : original(std::move(program)), control(std::move(state)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++control->polls;
    if (phase.tensors && !phase.tensors->empty())
      ++control->computation_polls;
    auto checked = phase;
    checked.consume_work = [&](std::uint64_t count) {
      if (count > control->maximum_work - control->work)
        return ps::Status{ps::ErrorCode::ResourceExhausted,
                          "checked Result callback work limit",
                          ps::FailureReason::WorkLimit,
                          {ps::FailureOrigin::Resource, ps::FailureScope::Run}};
      auto status = phase.consume_work(count);
      if (!status.ok())
        return status;
      control->work += count;
      if (control->work >= control->cancel_after)
        control->cancellation.cancel();
      return status;
    };
    const auto invoke = [&] {
      if (!control->measure_computation || !phase.tensors ||
          phase.tensors->empty())
        return original.poll(checked);
      const auto start = std::chrono::steady_clock::now();
      auto result = original.poll(checked);
      control->computation_us += std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
      return result;
    };
    if (!control->rounding)
      return invoke();
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save checked worker fenv");
    require(fesetround(*control->rounding) == 0 &&
                feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set checked worker fenv");
    auto result = invoke();
    const bool restored = fegetround() == *control->rounding &&
                          fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
    require(fesetenv(&saved) == 0, "restore checked worker fenv");
    require(restored, "Result continuation preserves worker fenv");
    return result;
  }
};
inline ps::WorkflowNode checked_node(
    const std::shared_ptr<ps::OperationRegistry>& registry,
    ps::WorkflowNode node, const std::shared_ptr<Control>& control) {
  const auto original_key = node.operation;
  const std::weak_ptr<ps::OperationRegistry> owner = registry;
  ps::OperationDefinition adapter;
  adapter.key = "manual.checked." + original_key;
  adapter.traits = take(registry->find_traits(original_key));
  require(
      adapter.traits.outputs[0].region_rule == ps::OperationRegionRule::Whole,
      "checked adapter requires Whole Result continuation");
  adapter.traits.cacheable = false;
  adapter.traits.requires_metadata_specialization = true;
  for (auto& output : adapter.traits.outputs) {
    require(output.continuation_bytes <= UINT64_MAX - sizeof(CheckedProgram),
            "checked continuation capacity");
    output.continuation_bytes += sizeof(CheckedProgram);
  }
  adapter.prepare_static =
      [owner, original_key](
          const auto& inputs,
          const auto& parameters) -> ps::Result<ps::OperationPreparation> {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto original =
        registry->prepare_operation(original_key, inputs, parameters);
    if (!original.ok())
      return ps::Result<ps::OperationPreparation>(original.status());
    ps::OperationPreparation prepared;
    const auto declared =
        registry->find_traits(original_key).value().workspace_bytes;
    prepared.additional_workspace_bytes =
        original.value()->traits().workspace_bytes - declared;
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
  adapter.start_result = [owner, original_key, control](const auto& query,
                                                        const auto& allocator) {
    auto original_query = query;
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    original_query.prepared = std::shared_ptr<const ps::PreparedOperation>(
        query.prepared,
        static_cast<const ps::PreparedOperation*>(query.prepared->state()));
    auto original =
        registry->start_result(original_key, original_query, allocator);
    if (!original.ok())
      return ps::Result<ps::ResultContinuation>(original.status());
    return ps::ResultContinuation::make<CheckedProgram>(
        allocator, original.take_value(), control);
  };
  node.operation = adapter.key;
  const auto registered = registry->register_operation(std::move(adapter));
  if (!registered.ok())
    throw std::runtime_error("register checked Result continuation: " +
                             registered.message);
  return node;
}
struct Workflow final {
  std::unique_ptr<ps::ExecutionContext> context;
  ps::ResourceBudget root;
  std::shared_ptr<ps::GraphContext> graph;
  ps::FrozenExecution frozen;
  Workflow(ps::WorkflowNode node, const std::vector<ps::Value>& backing,
           ps::ResourceLimits limits = {},
           std::shared_ptr<Control> control = {},
           std::uint32_t output_index = 0) {
    auto registry = ps::make_default_operation_registry(false);
    if (control)
      node = checked_node(registry, std::move(node), control);
    require(registry->freeze().ok(), "freeze Result check registry");
    const auto traits = take(registry->find_traits(node.operation));
    ps::WorkflowDocument document;
    numeric_result_fixture::declare_sources(&document, backing);
    node.inputs.clear();
    for (std::size_t i = 0; i < backing.size(); ++i)
      node.inputs.push_back(ps::WorkflowInputReference{i + 1});
    document.outputs = {
        {"values", node.id, traits.outputs.at(output_index).key}};
    document.nodes = {std::move(node)};
    graph = std::make_shared<ps::GraphContext>(document);
    auto plan = take(ps::Compiler(registry).compile(*graph)).plan;
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = limits;
    context = std::make_unique<ps::ExecutionContext>(registry, config);
    root = take(context->resource_budget());
    frozen = take(context->freeze(
        plan, point_math_checks::bindings(root, backing, document)));
  }
  ps::Result<ps::ExecutionResult> run(const ps::CancellationToken& stop = {}) {
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = UINT64_C(512) * 1024 * 1024;
    options.maximum_dependency_work = UINT64_C(1024) * 1024 * 1024;
    options.maximum_dependency_cache_work = 0;
    return context->execute(frozen, stop, options);
  }
};
inline void released(const ps::ResourceBudget& root) {
  for (auto live : root.statistics().live.values)
    require(live == 0, "Result check releases all Root resources");
}
// Independent dyadic oracle with a 65-element tail, arbitrary singleton stride,
// unaligned packed origin, zero stride, and reversed axes on every input port.
inline void layouts(const ps::WorkflowNode& node, unsigned ports) {
  for (unsigned variant = 0; variant < 3; ++variant) {
    std::vector<ps::Value> backing;
    for (unsigned port = 0; port < ports; ++port) {
      auto storage = take(ps::BufferAllocator{}.allocate(65 * 4 + 1));
      for (unsigned i = 0; i < 65; ++i) {
        const float value = i + 1;
        std::memcpy(storage.data() + 1 + 4 * i, &value, 4);
      }
      ps::StridedLayout layout{variant == 1 ? UINT64_C(257) : UINT64_C(5),
                               {777, variant == 1   ? -4
                                     : variant == 2 ? 0
                                                    : 4}};
      if (variant != 1)
        layout.origin = {0, 1};
      if (variant == 2)
        layout.byte_offset = 1;
      backing.push_back(take(ps::Value::from_storage(
          {ps::ElementType::Float32, {1, 65}}, ps::Region::whole({1, 65}),
          layout, std::move(storage).freeze())));
    }
    Workflow workflow(node, backing);
    const auto result = take(workflow.run()).results.at("values");
    for (unsigned i = 0; i < 65; ++i) {
      const float expected = (variant == 1   ? 65 - i
                              : variant == 2 ? 1
                                             : i + 1) *
                             ports;
      std::uint32_t expected_bits = 0, actual = 0;
      std::memcpy(&expected_bits, &expected, 4);
      require(numeric_result_fixture::read(result, {0, i}, &actual, 4).ok(),
              "layout Result read");
      require(actual == expected_bits,
              "Whole Float32 layout/tail analytic oracle");
    }
  }
}
inline void resources(const ps::WorkflowNode& node,
                      const std::vector<ps::Value>& backing,
                      std::uint64_t output_bytes = 0,
                      std::uint32_t output_index = 0) {
  auto registry = ps::make_default_operation_registry();
  std::vector<ps::OperationMetadata> metadata;
  for (const auto& value : backing) {
    ps::OperationMetadata input;
    input.result_schema = std::make_shared<ps::SchemaTemplate>(
        numeric_result_fixture::source_schema(value));
    metadata.push_back(std::move(input));
  }
  const auto traits =
      take(registry->resolve_traits(node.operation, metadata, node.parameters));
  const auto& output = traits.outputs.at(output_index);
  if (!output_bytes) {
    const auto count = take(output.result_schema->tensors.at(0).sample_count());
    const auto width = ps::Value::element_size(
        output.result_schema->tensors[0].descriptor.element_type);
    require(count <= UINT64_MAX / width, "resource output capacity");
    output_bytes = count * width;
  }
  require(output_bytes > 0, "resource fixture output bytes");
  auto capacity = output_bytes;
  for (auto extra : {traits.workspace_bytes, output.continuation_bytes,
                     static_cast<std::uint64_t>(sizeof(CheckedProgram))}) {
    require(extra <= UINT64_MAX - capacity, "resource fixture capacity sum");
    capacity += extra;
  }
  ps::ResourceBudget root;
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto control = std::make_shared<Control>();
    ps::ResourceLimits limits;
    if (mode == 0)
      control->maximum_work = 1024;
    if (mode == 1)
      limits.capacity[ps::ResourceKind::Payload] = 8;
    if (mode == 2)
      limits.capacity[ps::ResourceKind::Payload] = capacity - 1;
    {
      Workflow workflow(node, backing, limits, control, output_index);
      root = workflow.root;
      auto result = workflow.run();
      require(!result.ok() &&
                  result.status().code == ps::ErrorCode::ResourceExhausted,
              "Whole Result callback-work/output/scratch budget rejection");
      if (mode == 0)
        require(control->computation_polls > 0 &&
                    result.status().reason == ps::FailureReason::WorkLimit,
                "callback work bound fails in computation poll");
    }
    released(root);
  }
  auto control = std::make_shared<Control>();
  control->cancel_after = 10000;
  {
    Workflow workflow(node, backing, {}, control, output_index);
    root = workflow.root;
    auto result = workflow.run(control->cancellation.token());
    require(!result.ok() && result.status().code == ps::ErrorCode::Cancelled &&
                control->computation_polls > 0 &&
                control->work >= control->cancel_after,
            "cancel during Result computation poll after callback work");
  }
  released(root);
}
}  // namespace point_math_checks
