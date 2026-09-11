#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "data/content_digest.hpp"
#include "photospider/compiler/compiler.hpp"
#include "plugin/operation_identity.hpp"

namespace ps::execution_internal {
// Optional cache identity work is charged before hashing arbitrary metadata.
class DependencyTemplateDigest final {
 public:
  explicit DependencyTemplateDigest(std::uint64_t* work) : work_(work) {}
  void integer(std::uint64_t value) {
    if (charge(1))
      hash_.integer(value);
  }
  void text(const std::string& value) {
    if (charge(value.size()) && charge(1))
      hash_.text(value);
  }
  void bytes(const void* value, std::size_t size) {
    if (charge(size))
      hash_.bytes(value, size);
  }
  void metadata(const ValueDescriptor& descriptor,
                const std::vector<ValueFacet>& facets) {
    integer(static_cast<std::uint32_t>(descriptor.element_type));
    integer(descriptor.shape.size());
    for (auto extent : descriptor.shape)
      integer(extent);
    contract_internal::append_facets(this, facets);
  }
  void disable() { valid_ = false; }
  std::string finish() { return valid_ ? hash_.finish() : std::string{}; }

 private:
  bool charge(std::uint64_t count) {
    if (!valid_)
      return false;
    if (count > *work_) {
      *work_ = 0;
      valid_ = false;
      return false;
    }
    *work_ -= count;
    return true;
  }
  std::uint64_t* work_;
  bool valid_ = true;
  content_internal::Sha256 hash_;
};
/** @brief Static selected-result DAG contracts without graph/node/step IDs.
 * All observable input metadata and static parameters are included; runtime
 * samples are hashed separately from the actual dependency witness. Source
 * names identify public binding routes, independent of declaration numbering.
 */
inline std::vector<std::string> dependency_cache_templates(
    const ExecutionPlan& plan, const std::string& native_identity,
    std::uint64_t* work, const CancellationToken& cancellation) {
  std::vector<std::string> keys(plan.steps().size());
  for (std::size_t index = 0; index < plan.steps().size() && *work; ++index) {
    if (cancellation.cancelled())
      break;
    const auto& step = plan.steps()[index];
    DependencyTemplateDigest hash(work);
    hash.text("photospider.dependency-static.v1");
    hash.integer(static_cast<std::uint32_t>(plan.execution_mode()));
    hash.integer(static_cast<std::uint32_t>(step.backend));
    if (step.backend == Backend::Gpu)
      hash.text(native_identity);
    hash.text(step.operation);
    hash.integer(step.output_index);
    contract_internal::append_traits(&hash, step.traits);
    hash.metadata(step.output_descriptor, step.output_facets);
    hash.integer(step.parameters.size());
    for (const auto& parameter : step.parameters) {
      hash.text(parameter.first);
      hash.integer(parameter.second.index());
      if (const auto* value = std::get_if<std::int64_t>(&parameter.second)) {
        hash.integer(static_cast<std::uint64_t>(*value));
      } else if (const auto* value = std::get_if<double>(&parameter.second)) {
        std::uint64_t bits;
        std::memcpy(&bits, value, sizeof(bits));
        hash.integer(bits);
      } else if (const auto* value = std::get_if<bool>(&parameter.second)) {
        hash.integer(*value);
      } else {
        hash.text(std::get<std::string>(parameter.second));
      }
    }
    hash.integer(step.inputs.size());
    for (const auto& input : step.inputs) {
      if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
        hash.integer(1);
        if (producer->step_index >= index ||
            keys[producer->step_index].empty()) {
          hash.disable();
          break;
        }
        hash.text(keys[producer->step_index]);
      } else {
        hash.integer(0);
        const auto& declaration = plan.input_declarations().at(
            std::get<PlanWorkflowInput>(input).declaration_index);
        hash.text(declaration.name);
        hash.metadata(declaration.descriptor, declaration.facets);
      }
    }
    keys[index] = hash.finish();
  }
  return keys;
}
}  // namespace ps::execution_internal
