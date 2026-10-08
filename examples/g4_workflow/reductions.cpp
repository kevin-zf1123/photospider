#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "result_support.hpp"  // NOLINT(build/include_subdir)

void reductions_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = make_default_operation_registry(false);
  std::uint64_t blocks = 0, generated_bytes = 0;
  g4_result::check(registry->register_operation(g4_result::source(
      "example.reduction_source", g4_result::schema(ElementType::Float64, 4096),
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
  g4_result::check(registry->freeze());
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
  auto compiled = g4_result::take(Compiler(registry).compile(graph));
  ExecutionContext context(registry, {1, false, 8, 1024});
  auto output = g4_result::take(context.execute(compiled.plan));
  const auto peak = g4_result::take(context.resource_budget())
                        .statistics()
                        .peak[ResourceKind::Payload];
  if (g4_result::number(output.results.at("mean")) != 1.5 ||
      g4_result::number(output.results.at("variance")) != 1.25 ||
      blocks != 192 || generated_bytes != 98304 || peak > 1024)
    throw std::runtime_error("ordered reduction independent oracle failed");
  for (const auto* name : {"mean", "variance"}) {
    auto relation = g4_result::take(output.results.at(name).tensor_relation(0));
    unsigned spans = 0;
    g4_result::check(relation.visit(0, 8, [&](ResultSupport support) {
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
