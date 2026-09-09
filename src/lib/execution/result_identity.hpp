#pragma once

#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/** @brief Hashes canonical local DAG semantics and exact demanded input bits.
 * @note The context fixes the registry implementation. Empty keys disable
 * caching for unproven input ancestry; no global graph/node ids are hashed.
 */
inline std::vector<std::string> result_keys(
    const ExecutionPlan& plan, const std::vector<ExecutionBinding>& bindings) {
  std::vector<std::string> keys(plan.steps().size());
  std::map<std::pair<std::size_t, std::string>, std::string> memo;
  std::function<std::string(std::size_t, Region)> identify;
  identify = [&](std::size_t i, Region requested) -> std::string {
    const auto& step = plan.steps()[i];
    const auto& t = step.traits;
    if (!t.cacheable || !t.deterministic || !t.side_effect_free ||
        step.backend != Backend::Cpu || requested.empty())
      return {};
    if (step.whole_boundary)
      requested = Region::whole(step.output_descriptor.shape);
    content_internal::Sha256 region_hash;
    for (auto d : requested.dimensions()) {
      region_hash.integer(d.offset);
      region_hash.integer(d.extent);
    }
    const auto lookup = std::make_pair(i, region_hash.finish());
    auto prior = memo.find(lookup);
    if (prior != memo.end())
      return prior->second;
    content_internal::Sha256 hash;
    hash.text("photospider.result-region.v1");
    hash.text(step.operation);
    hash.integer(t.version);
    hash.integer(static_cast<std::uint32_t>(t.shape_rule));
    hash.integer(static_cast<std::uint32_t>(t.region_rule));
    hash.integer(t.halo_radius);
    hash.integer(t.spatial_factor);
    hash.integer(static_cast<std::uint32_t>(t.output_schema.kind));
    hash.integer(
        static_cast<std::uint32_t>(step.output_descriptor.element_type));
    hash.integer(step.output_descriptor.shape.size());
    for (auto n : step.output_descriptor.shape)
      hash.integer(n);
    for (auto d : requested.dimensions()) {
      hash.integer(d.offset);
      hash.integer(d.extent);
    }
    hash.integer(step.parameters.size());
    for (const auto& p : step.parameters) {
      hash.text(p.first);
      hash.integer(p.second.index());
      if (const auto* v = std::get_if<std::int64_t>(&p.second)) {
        hash.integer(static_cast<std::uint64_t>(*v));
      } else if (const auto* v = std::get_if<double>(&p.second)) {
        std::uint64_t bits;
        std::memcpy(&bits, v, 8);
        hash.integer(bits);
      } else if (const auto* v = std::get_if<bool>(&p.second)) {
        hash.integer(*v);
      } else {
        hash.text(std::get<std::string>(p.second));
      }
    }
    hash.integer(step.inputs.size());
    bool valid = true;
    for (std::size_t port = 0; port < step.inputs.size(); ++port) {
      hash.integer(static_cast<std::uint32_t>(t.input_schema[port].kind));
      const auto* producer = std::get_if<PlanStepInput>(&step.inputs[port]);
      const auto& input_shape =
          producer ? plan.steps()[producer->step_index].output_descriptor.shape
                   : plan.input_declarations()[std::get<PlanWorkflowInput>(
                                                   step.inputs[port])
                                                   .declaration_index]
                         .descriptor.shape;
      auto mapped = input_internal::derive_input_demand(
          t, requested, step.output_descriptor.shape, input_shape,
          t.input_schema[port].kind);
      if (!mapped.ok())
        return {};
      const auto& demand = mapped.value();
      hash.integer(demand.rank());
      for (auto d : demand.dimensions()) {
        hash.integer(d.offset);
        hash.integer(d.extent);
      }
      if (const auto* producer =
              std::get_if<PlanStepInput>(&step.inputs[port])) {
        const auto key = identify(producer->step_index, demand);
        if (key.empty()) {
          valid = false;
          break;
        }
        hash.text(key);
      } else {
        const auto& input =
            bindings[std::get<PlanWorkflowInput>(step.inputs[port])
                         .declaration_index];
        if (input.snapshot) {
          auto key = input.snapshot->content_identity(demand);
          if (!key.ok()) {
            valid = false;
            break;
          }
          hash.text(key.value());
        } else if (input.value.valid() &&
                   t.input_schema[port].kind ==
                       OperationPortKind::Float32Scalar) {
          std::uint32_t bits;
          std::memcpy(&bits, input.value.bytes().data(), 4);
          hash.integer(bits);
        } else {
          valid = false;
          break;
        }
      }
    }
    auto key = valid ? hash.finish() : std::string{};
    memo.emplace(lookup, key);
    return key;
  };
  for (std::size_t i = 0; i < plan.steps().size(); ++i)
    keys[i] = identify(i, plan.steps()[i].output_demand);
  return keys;
}
}  // namespace ps::execution_internal
