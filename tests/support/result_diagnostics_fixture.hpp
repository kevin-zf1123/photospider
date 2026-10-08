#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace result_diagnostics {
struct BroadcastResult final {
  static ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    const auto& schema = *phase.query.output.result_schema;
    auto storage = take(phase.resources.allocator().allocate(sizeof(double)));
    const double scalar = 19;
    std::memcpy(storage.data(), &scalar, sizeof(scalar));
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
    auto backing = std::move(storage).freeze();
    for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
      const auto shape = schema.tensors[slot].sample_shape();
      auto count = schema.tensors[slot].sample_count();
      check(builder.publish_tensor(
          slot, ps::Region::whole(shape),
          {0, std::vector<std::int64_t>(shape.size(), 0)}, backing,
          take(ps::ResultRelation::cartesian(
              phase.resources, count.ok() ? count.value() : UINT64_MAX, {})),
          {true, true, true, true}));
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};

inline int verify_cpp_fixed_broadcast(const std::vector<std::uint64_t>& shape,
                                      bool extra_tensor = false) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::OperationDefinition operation;
  operation.key = "fixture.fixed_broadcast";
  operation.traits.estimated_bytes = sizeof(double);
  auto schema = multi_result::schema(ps::ElementType::Float64, shape);
  if (extra_tensor) {
    auto extra = multi_result::schema().tensors[0];
    extra.key = "extra";
    schema.tensors.push_back(std::move(extra));
  }
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].region_rule = ps::OperationRegionRule::Whole;
  operation.traits.outputs[0].maximum_output_payload_bytes = sizeof(double);
  operation.start_result = [](const auto&, const auto&) {
    return ps::ResultContinuation::stateless<BroadcastResult::poll>();
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  auto copied_traits = registry->find_traits("fixture.fixed_broadcast");
  PS_CHECK(copied_traits.ok());
  PS_CHECK(copied_traits.value().estimated_bytes == sizeof(double));
  PS_CHECK(copied_traits.value()
               .outputs[0]
               .result_schema->tensors[0]
               .sample_shape() == shape);
  PS_CHECK(registry->freeze().ok());
  ps::WorkflowDocument document;
  document.nodes = {{1, "fixture.fixed_broadcast", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  ps::GraphContext graph(std::move(document));
  ps::Compiler compiler(registry);
  auto first_compilation = compiler.compile(graph);
  auto second_compilation = compiler.compile(graph);
  PS_CHECK(first_compilation.ok() && second_compilation.ok());
  PS_CHECK(first_compilation.value().semantic.digest().value ==
           second_compilation.value().semantic.digest().value);
  PS_CHECK(first_compilation.value().optimized.digest().value ==
           second_compilation.value().optimized.digest().value);
  PS_CHECK(first_compilation.value().plan.digest().value ==
           second_compilation.value().plan.digest().value);
  PS_CHECK(first_compilation.value().plan.cache_key().value ==
           second_compilation.value().plan.cache_key().value);
  PS_CHECK(first_compilation.value().plan.steps().size() == 1);
  PS_CHECK(first_compilation.value()
               .plan.steps()[0]
               .output_result_schema->tensors[0]
               .sample_shape() == shape);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 0;
  config.managed_resources = ps::ResourceLimits{};
  config.managed_resources->capacity[ps::ResourceKind::Payload] =
      2 * sizeof(double);
  ps::ExecutionContext execution(registry, config);
  auto root = execution.resource_budget();
  PS_CHECK(root.ok());
  auto first_result = execution.execute(first_compilation.value().plan);
  auto second_result = execution.execute(second_compilation.value().plan);
  if (!first_result.ok())
    std::cerr << "broadcast first execution: code="
              << static_cast<unsigned>(first_result.status().code) << " reason="
              << static_cast<unsigned>(first_result.status().reason) << " "
              << first_result.status().message << '\n';
  if (!second_result.ok())
    std::cerr << "broadcast second execution: code="
              << static_cast<unsigned>(second_result.status().code)
              << " reason="
              << static_cast<unsigned>(second_result.status().reason) << " "
              << second_result.status().message << '\n';
  PS_CHECK(first_result.ok() && second_result.ok());
  PS_CHECK(root.value().statistics().live[ps::ResourceKind::Payload] ==
           2 * sizeof(double));
  for (const auto* completed :
       {&first_result.value(), &second_result.value()}) {
    PS_CHECK(completed->diagnostics.operation_timings.size() == 1);
    const auto& timing = completed->diagnostics.operation_timings[0];
    const auto count = ps::Region::whole(shape).element_count();
    const bool saturated =
        !count.ok() || (extra_tensor && count.value() == UINT64_MAX);
    PS_CHECK(timing.computed_elements ==
             (saturated ? UINT64_MAX : count.value() + (extra_tensor ? 1 : 0)));
    PS_CHECK(timing.computed_elements_saturated == saturated);
    const auto& result = completed->results.at("value");
    PS_CHECK(result.schema().tensors[0].sample_shape() == shape);
    const auto facts = result.descriptor().take_value();
    if (extra_tensor) {
      double extra = 0;
      PS_CHECK(result.read_tensor(facts, 1, {0}, &extra, sizeof(extra)).ok() &&
               extra == 19);
    }
    PS_CHECK(facts.tensor_coverage(0) ==
             ps::Footprint::all(shape).take_value());
    std::vector<std::uint64_t> first(shape.size(), 0), last;
    for (auto extent : shape)
      last.push_back(extent - 1);
    double observed = 0;
    PS_CHECK(
        result.read_tensor(facts, 0, first, &observed, sizeof(observed)).ok() &&
        observed == 19);
    PS_CHECK(
        result.read_tensor(facts, 0, last, &observed, sizeof(observed)).ok() &&
        observed == 19);
  }
  first_result =
      ps::Result<ps::ExecutionResult>(ps::Status{ps::ErrorCode::Cancelled, {}});
  second_result =
      ps::Result<ps::ExecutionResult>(ps::Status{ps::ErrorCode::Cancelled, {}});
  PS_CHECK(root.value().statistics().live[ps::ResourceKind::Payload] == 0);
  return 0;
}

}  // namespace result_diagnostics
