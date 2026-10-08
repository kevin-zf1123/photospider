#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "support/dependency_workflow_fixture.hpp"

void reductions_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = make_default_operation_registry(false);
  std::uint64_t blocks = 0, generated_bytes = 0;
  dependency_fixture::check(
      registry->register_operation(dependency_fixture::source(
          "example.reduction_source",
          dependency_fixture::schema(ElementType::Float64, 4096),
          [&](const ResultProgramQuery&, const Region& region,
              std::uint8_t* destination, std::uint64_t bytes) {
            if (bytes != 512 || region.dimensions()[0].extent != 64)
              return Status{ErrorCode::OperationFailed,
                            "unbounded reduction source"};
            ++blocks;
            generated_bytes += bytes;
            for (std::uint64_t i = 0; i < 64; ++i) {
              const double number =
                  static_cast<double>((region.dimensions()[0].offset + i) % 4);
              std::memcpy(destination + 8 * i, &number, 8);
            }
            return Status::success();
          })));
  dependency_fixture::check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "example.reduction_source", {}, {}},
                    {2,
                     "numeric.mean",
                     {WorkflowNodeOutput{1, "value"}},
                     {{"block_size", INT64_C(64)}}},
                    {3,
                     "numeric.variance",
                     {WorkflowNodeOutput{1, "value"}},
                     {{"block_size", INT64_C(64)}}}};
  document.outputs = {{"mean", 2, "value"}, {"variance", 3, "value"}};
  GraphContext graph(document);
  auto compiled = dependency_fixture::take(Compiler(registry).compile(graph));
  ExecutionContext context(registry, {1, false, 8, 1024});
  auto output = dependency_fixture::take(context.execute(compiled.plan));
  const auto peak = dependency_fixture::take(context.resource_budget())
                        .statistics()
                        .peak[ResourceKind::Payload];
  if (dependency_fixture::number(output.results.at("mean")) != 1.5 ||
      dependency_fixture::number(output.results.at("variance")) != 1.25)
    throw std::runtime_error("ordered reduction independent oracle failed");
  if (!blocks || blocks > 192 || generated_bytes != blocks * 512 || peak > 1024)
    throw std::runtime_error(
        "ordered reduction source work or payload bound failed");
  for (const auto* name : {"mean", "variance"}) {
    auto relation =
        dependency_fixture::take(output.results.at(name).tensor_relation(0));
    unsigned spans = 0;
    dependency_fixture::check(relation.visit(0, 8, [&](ResultSupport support) {
      if (support.input != 0 || support.roles != 5 || support.first != 0 ||
          support.count != 4096 ||
          support.target != ResultSupportTarget::Tensor)
        return Status{ErrorCode::OperationFailed, "reduction global support"};
      ++spans;
      return Status::success();
    }));
    if (spans != 1)
      throw std::runtime_error("missing reduction global support");
  }
  std::cout
      << "reductions: mean=1.5, variance=1.25, block=64, generated_blocks="
      << blocks << ", generated_bytes=" << generated_bytes
      << ", peak_payload=" << peak
      << ", live_budget=1024, global_support=present\n";
}
