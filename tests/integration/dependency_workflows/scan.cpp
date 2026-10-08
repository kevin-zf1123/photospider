#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "support/dependency_workflow_fixture.hpp"

/** @brief Checks scan prefix reuse and the request-local nonfinite boundary. */
void scan_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  constexpr std::uint64_t n = 128;
  const auto schema = dependency_fixture::schema(ElementType::Float64, n);
  auto registry = make_default_operation_registry(false);
  std::uint64_t reads = 0;
  dependency_fixture::check(
      registry->register_operation(dependency_fixture::source(
          "example.scan_source", schema,
          [&](const ResultProgramQuery&, const Region& region,
              std::uint8_t* bytes, std::uint64_t size) {
            if (region.dimensions()[0].offset != reads)
              return Status{ErrorCode::OperationFailed, "prefix reread"};
            for (std::uint64_t i = 0; i < size / 8; ++i) {
              const double value = static_cast<double>(++reads);
              std::memcpy(bytes + i * 8, &value, 8);
            }
            return Status::success();
          })));
  dependency_fixture::check(registry->freeze());
  WorkflowDocument doc;
  doc.nodes = {{1, "example.scan_source", {}, {}},
               {2,
                "numeric.ordered_scan",
                {WorkflowNodeOutput{1, "value"}},
                {{"block_size", INT64_C(16)}}}};
  doc.outputs = {{"y", 2, "value"}};
  GraphContext graph(doc);
  auto plan = dependency_fixture::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, {2, false, 8, 8192});
  auto result = context.execute(plan);
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  for (std::uint64_t i = 0; i < n; ++i) {
    const auto actual =
        dependency_fixture::number(result.value().results.at("y"), i);
    if (actual != static_cast<double>((i + 1) * (i + 2) / 2))
      throw std::runtime_error("scan triangular-number oracle failed");
  }
  if (reads != n)
    throw std::runtime_error("scan repeated source reads");
  std::vector<double> invalid(n, 0);
  invalid[0] = 1;
  invalid[1] = std::numeric_limits<double>::infinity();
  WorkflowDocument bound_doc;
  bound_doc.inputs = {dependency_fixture::declaration(1, "x", schema)};
  bound_doc.nodes = {{2,
                      "numeric.ordered_scan",
                      {WorkflowInputReference{1}},
                      {{"block_size", INT64_C(16)}}}};
  bound_doc.outputs = {{"y", 2, "value"}};
  GraphContext bound_graph(bound_doc);
  auto bound_plan =
      dependency_fixture::take(Compiler(registry).compile(bound_graph)).plan;
  ExecutionBinding binding;
  binding.name = "x";
  binding.result = dependency_fixture::numbers(
      dependency_fixture::take(context.resource_budget()), invalid);
  auto demand =
      dependency_fixture::take(context.open_demand(bound_plan, {{binding}}));
  auto short_q = Footprint::from_regions({n}, {Region({{0, 1}})}).take_value();
  auto joint_q = Footprint::from_regions({n}, {Region({{0, 2}})}).take_value();
  auto short_result = demand.request({{"y", short_q}});
  auto joint_result = demand.request({{"y", joint_q}});
  if (!short_result.ok() ||
      dependency_fixture::number(short_result.value().results.at("y")) != 1 ||
      joint_result.status().code != ErrorCode::OperationFailed ||
      joint_result.status().message != "nonfinite scan input 1")
    throw std::runtime_error("scan per-request error oracle failed");
  std::cout << "scan: outputs=128, generated_samples=128, last=8256, "
               "short_query=1, joint_query=nonfinite_input_1\n";
}
