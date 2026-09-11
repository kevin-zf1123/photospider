#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "photospider/photospider.hpp"

/** @brief Checks scan prefix reuse and the request-local nonfinite boundary. */
void scan_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  constexpr std::uint64_t n = 128;
  WorkflowDocument doc;
  doc.inputs = {
      {1, "x", {ElementType::Float64, {n}}, Region::whole({n}), {0, {8}}, {}}};
  doc.nodes = {{1,
                "numeric.ordered_scan",
                {WorkflowInputReference{1}},
                {{"block_size", INT64_C(16)}}}};
  doc.outputs = {{"y", 1, "value"}};
  auto registry = make_default_operation_registry();
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = doc.inputs[0].descriptor;
  std::uint64_t reads = 0;
  source->read = [&](const Region& region, std::uint8_t* bytes,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    if (region.dimensions()[0].offset != reads)
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "prefix reread"});
    for (std::uint64_t i = 0; i < size / 8; ++i) {
      const double value = static_cast<double>(++reads);
      std::memcpy(bytes + i * 8, &value, 8);
    }
    return Result<Region>(region);
  };
  ExecutionContext context(registry, {2, false, 8, 8192});
  auto result = context.execute(plan, {{{"x", {}, source}}});
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  for (std::uint64_t i = 0; i < n; ++i) {
    double actual = 0;
    std::memcpy(&actual, result.value().values.at("y").bytes().data() + i * 8,
                8);
    if (actual != static_cast<double>((i + 1) * (i + 2) / 2))
      throw std::runtime_error("scan triangular-number oracle failed");
  }
  if (reads != n)
    throw std::runtime_error("scan repeated source reads");
  std::vector<double> invalid(n, 0);
  invalid[0] = 1;
  invalid[1] = std::numeric_limits<double>::infinity();
  std::vector<std::uint8_t> bytes(n * 8);
  std::memcpy(bytes.data(), invalid.data(), bytes.size());
  auto value =
      Value::create(source->descriptor, Region::whole({n}), {0, {8}}, bytes)
          .take_value();
  auto demand = context.open_demand(plan, {{{"x", value}}}).take_value();
  auto short_q = Footprint::from_regions({n}, {Region({{0, 1}})}).take_value();
  auto joint_q = Footprint::from_regions({n}, {Region({{0, 2}})}).take_value();
  auto short_result = demand.request({{"y", short_q}});
  double actual = 0;
  auto joint_result = demand.request({{"y", joint_q}});
  if (!short_result.ok() ||
      !short_result.value().values.at("y").read({0}, &actual, 8).ok() ||
      actual != 1 || joint_result.status().code != ErrorCode::OperationFailed ||
      joint_result.status().message != "nonfinite scan input 1")
    throw std::runtime_error("scan per-request error oracle failed");
  std::cout << "scan: outputs=128, source_reads=128, last=8256, "
               "short_query=1, joint_query=nonfinite_input_1\n";
}
