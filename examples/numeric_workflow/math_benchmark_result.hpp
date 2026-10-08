#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace math_benchmark_result {
// A retained public workflow supplies actual Result Needs. The optional
// observer times only the computation poll on its worker, including the
// checked consume_work observer and Result publication. It excludes factory,
// initial Need, coordinator, source admission, freezing and output readback.
class Workflow final {
 public:
  explicit Workflow(ps::OperationDefinition operation,
                    std::shared_ptr<point_math_checks::Control> control = {})
      : registry_(std::make_shared<ps::OperationRegistry>()),
        control_(std::move(control)) {
    node_.id = 1;
    node_.operation = operation.key;
    node_.inputs = {ps::WorkflowInputReference{1}};
    point_math_checks::require(
        registry_->register_operation(std::move(operation)).ok(),
        "register Result benchmark operation");
    if (control_)
      node_ = point_math_checks::checked_node(registry_, node_, control_);
    point_math_checks::require(registry_->freeze().ok(),
                               "freeze Result benchmark registry");
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = UINT64_C(1) << 30;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->maximum_work = UINT64_C(1) << 50;
    context_ = std::make_unique<ps::ExecutionContext>(registry_, config);
    root_ = point_math_checks::take(context_->resource_budget());
  }
  void bind(const ps::Value& backing) {
    ps::WorkflowDocument document;
    numeric_result_fixture::declare_sources(&document, {backing});
    document.nodes = {node_};
    document.outputs = {{"values", 1, "values"}};
    if (!schema_ || !schema_->same_schema(*document.inputs[0].result_schema)) {
      auto graph = std::make_shared<ps::GraphContext>(document);
      auto plan =
          point_math_checks::take(ps::Compiler(registry_).compile(*graph));
      plan_ = std::move(plan.plan);
      graph_ = std::move(graph);
      schema_ = document.inputs[0].result_schema;
    }
    auto bindings = point_math_checks::bindings(root_, {backing}, document);
    auto frozen = point_math_checks::take(context_->freeze(*plan_, bindings));
    frozen_ = std::move(frozen);
  }
  ps::Result<ps::ResultRef> run() {
    if (control_) {
      control_->work = 0;
      control_->polls = 0;
      control_->computation_polls = 0;
      control_->computation_us = 0;
    }
    ps::ExecutionOptions options;
    options.maximum_parallelism = 1;
    options.maximum_dependency_work = UINT64_C(1) << 50;
    options.dependencies.maximum_work = UINT64_C(1) << 50;
    options.dependencies.sets.maximum_work = UINT64_C(1) << 50;
    options.maximum_dependency_cache_work = 0;
    auto run = context_->execute(frozen_, {}, options);
    if (!run.ok())
      return ps::Result<ps::ResultRef>(run.status());
    if (control_ && control_->measure_computation)
      point_math_checks::require(
          control_->computation_polls == 1 && control_->computation_us > 0,
          "core timer observes one real computation poll");
    peak_payload_ = root_.statistics().peak[ps::ResourceKind::Payload];
    return ps::Result<ps::ResultRef>(run.value().results.at("values"));
  }
  std::uint64_t peak_payload() const { return peak_payload_; }
  const ps::ResourceBudget& root() const { return root_; }

 private:
  std::shared_ptr<ps::OperationRegistry> registry_;
  std::shared_ptr<point_math_checks::Control> control_;
  std::unique_ptr<ps::ExecutionContext> context_;
  ps::ResourceBudget root_;
  std::shared_ptr<ps::GraphContext> graph_;
  std::optional<ps::ExecutionPlan> plan_;
  std::shared_ptr<const ps::SchemaTemplate> schema_;
  ps::FrozenExecution frozen_;
  ps::WorkflowNode node_;
  std::uint64_t peak_payload_ = 0;
};
inline std::vector<std::uint32_t> words(const ps::ResultRef& output) {
  const auto facts = point_math_checks::take(output.descriptor());
  const auto& tensor = output.schema().tensors.at(0);
  const auto shape = tensor.sample_shape();
  point_math_checks::require(
      shape.size() == 1 &&
          tensor.descriptor.element_type == ps::ElementType::Float32,
      "benchmark expects Float32 rank-one Result");
  const auto window = point_math_checks::take(
      output.acquire_tensor(facts, 0, ps::Region::whole(shape)));
  const auto row = point_math_checks::take(window.row_run({0}));
  point_math_checks::require(
      row.samples == shape[0] && row.sample_stride_bytes == 4,
      "numeric benchmark output is packed");
  std::vector<std::uint32_t> result(shape[0]);
  std::memcpy(result.data(), row.data, result.size() * sizeof(std::uint32_t));
  return result;
}
}  // namespace math_benchmark_result
