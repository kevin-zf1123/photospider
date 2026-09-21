#pragma once

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

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
// Independent dyadic oracle with a 65-element tail, arbitrary singleton stride,
// unaligned packed origin, zero stride, and reversed axes on every input port.
inline void layouts(const ps::WorkflowNode& node, unsigned ports) {
  auto registry = ps::make_default_operation_registry();
  for (unsigned variant = 0; variant < 3; ++variant) {
    std::vector<ps::Value> inputs;
    std::vector<ps::Region> demands;
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
      inputs.push_back(take(ps::Value::from_storage(
          {ps::ElementType::Float32, {1, 65}}, ps::Region::whole({1, 65}),
          layout, std::move(storage).freeze())));
      demands.push_back(inputs.back().region());
    }
    ps::OperationInvocation call(inputs, demands, node.parameters,
                                 ps::Backend::Cpu, {},
                                 ps::Region::whole({1, 65}));
    auto result = take(registry->invoke(node.operation, call));
    for (unsigned i = 0; i < 65; ++i) {
      float expected = (variant == 1   ? 65 - i
                        : variant == 2 ? 1
                                       : i + 1) *
                       ports;
      std::uint32_t expected_bits = 0, actual = 0;
      std::memcpy(&expected_bits, &expected, 4);
      std::memcpy(&actual,
                  result.bytes().data() + take(result.byte_address({0, i})), 4);
      require(actual == expected_bits,
              "Whole Float32 layout/tail analytic oracle");
    }
  }
}
// Direct calls exercise arbitrary layouts without the executor's packed
// collect. A managed scope supplies the same work/capacity ledger as a worker
// callback.
inline void resources(const ps::WorkflowNode& node,
                      const std::vector<ps::Value>& inputs,
                      std::uint64_t output_bytes = 0,
                      std::uint32_t output_index = 0) {
  auto registry = ps::make_default_operation_registry();
  std::vector<ps::OperationMetadata> metadata;
  std::vector<ps::Region> demands;
  for (const auto& value : inputs) {
    metadata.push_back({value.descriptor(), value.facets()});
    demands.push_back(value.region());
  }
  auto traits =
      take(registry->resolve_traits(node.operation, metadata, node.parameters));
  require(traits.outputs[output_index].region_rule ==
                  ps::OperationRegionRule::Whole &&
              traits.outputs[output_index].dependency_version == 0 &&
              traits.outputs[output_index].continuation_bytes == 0,
          "formal profile has one Whole callback and admitted workspace");
  const auto output_region =
      traits.outputs[output_index].shape_rule == ps::OperationShapeRule::Fixed
          ? ps::Region::whole(traits.outputs[output_index].fixed_output_shape)
          : inputs[0].region();
  for (unsigned mode = 0; mode < (traits.workspace_bytes ? 3U : 2U); ++mode) {
    ps::ResourceLimits limits;
    if (mode == 0)
      limits.maximum_work = 1024;
    if (mode == 1)
      limits.capacity[ps::ResourceKind::Payload] = 8;
    if (mode == 2)
      limits.capacity[ps::ResourceKind::Payload] =
          (output_bytes ? output_bytes : inputs[0].bytes().size()) +
          traits.workspace_bytes - 1;
    ps::ResourceBudget budget(limits);
    {
      ps::ResourceAllocationScope scope(budget);
      ps::OperationInvocation call(inputs, demands, node.parameters,
                                   ps::Backend::Cpu, {}, output_region,
                                   budget.allocator());
      call.output_index = output_index;
      auto result = registry->invoke(node.operation, call);
      require(!result.ok() &&
                  result.status().code == ps::ErrorCode::ResourceExhausted,
              "Whole work/output/scratch budget rejection");
    }
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "failure releases full output and workspace");
  }
  ps::ResourceBudget budget(ps::ResourceLimits{});
  ps::CancellationSource cancellation;
  std::atomic<bool> ready{false}, done{false};
  std::thread watcher([&] {
    ready.store(true);
    while (!done.load() && budget.statistics().issued.work < 10000)
      std::this_thread::yield();
    if (!done.load())
      cancellation.cancel();
  });
  while (!ready.load())
    std::this_thread::yield();
  ps::Status outcome;
  try {
    ps::ResourceAllocationScope scope(budget);
    ps::OperationInvocation call(inputs, demands, node.parameters,
                                 ps::Backend::Cpu, cancellation.token(),
                                 output_region, budget.allocator());
    call.output_index = output_index;
    outcome = registry->invoke(node.operation, call).status();
  } catch (...) {
    done.store(true);
    watcher.join();
    throw;
  }
  done.store(true);
  watcher.join();
  require(outcome.code == ps::ErrorCode::Cancelled &&
              budget.statistics().issued.work >= 10000 &&
              budget.statistics().live[ps::ResourceKind::Payload] == 0,
          "cancellation during arithmetic releases Whole storage");
}
}  // namespace point_math_checks
