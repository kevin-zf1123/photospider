#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "photospider/photospider.hpp"

void reductions_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  WorkflowDocument document;
  document.inputs = {{1,
                      "source",
                      {ElementType::Float64, {4096}},
                      Region::whole({4096}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1,
                     "numeric.mean",
                     {WorkflowInputReference{1}},
                     {{"block_size", INT64_C(64)}}},
                    {2,
                     "numeric.variance",
                     {WorkflowInputReference{1}},
                     {{"block_size", INT64_C(64)}}}};
  document.outputs = {{"mean", 1, "value"}, {"variance", 2, "value"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  if (!compiled.ok())
    throw std::runtime_error(compiled.status().message);
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = document.inputs[0].descriptor;
  std::uint64_t reads = 0;
  source->read = [&](const Region& region, std::uint8_t* destination,
                     std::uint64_t bytes, const BufferAllocator&,
                     const CancellationToken&) {
    if (bytes != 512 || region.dimensions()[0].extent != 64)
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "unbounded reduction read"});
    ++reads;
    for (std::uint64_t i = 0; i < 64; ++i) {
      const double number =
          static_cast<double>((region.dimensions()[0].offset + i) % 4);
      std::memcpy(destination + 8 * i, &number, 8);
    }
    return Result<Region>(region);
  };
  ExecutionContext context(registry, {1, false, 8, 1024});
  auto result =
      context.execute(compiled.value().plan, {{{"source", {}, source}}});
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  // Uniform repeats of [0,1,2,3] have mean 1.5 and population variance 1.25.
  const auto& output = result.value();
  if (output.values.at("mean").as_float64().value() != 1.5 ||
      output.values.at("variance").as_float64().value() != 1.25 ||
      reads != 192 || output.diagnostics.source_read_bytes != 98304 ||
      output.diagnostics.peak_live_bytes > 1024 ||
      output.dependencies.source_support().value().at("source") !=
          Footprint::all({4096}).take_value())
    throw std::runtime_error("ordered reduction independent oracle failed");
  std::cout
      << "reductions: mean=1.5, variance=1.25, block=64, source_bytes=98304, "
         "live_budget=1024, global_support=present\n";
}
