#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/plan.hpp"

namespace ps {
/** @brief Callback-local construction of a physical instruction program.
 * The compiler fixes the linked CFG, input/output signature, prepared operation
 * contracts, placement and normalized output requests before invoking a
 * backend. Backends read those declarations and edit only per-block
 * instructions. Borrowed references are valid for the callback duration and
 * must not escape. Each invocation has its own builder; concurrent reads of
 * linked declarations are safe, while instruction edits require caller
 * synchronization. Allocation and bounds-checked access may throw;
 * Compiler::plan catches callback exceptions. Only the compiler can validate
 * and publish an ExecutionPlan.
 */
class PHOTOSPIDER_API PlanBuilder final {
 public:
  PlanBuilder(const PlanBuilder&) = delete;
  PlanBuilder& operator=(const PlanBuilder&) = delete;
  const std::vector<PlanStep>& steps() const noexcept { return plan_.steps(); }
  const std::vector<PlanBlock>& blocks() const noexcept {
    return plan_.blocks();
  }
  const std::vector<PlanParameter>& parameters() const noexcept {
    return plan_.parameters();
  }
  std::size_t entry() const noexcept { return plan_.entry(); }
  const std::map<std::string, std::size_t>& outputs() const noexcept {
    return plan_.outputs();
  }
  const std::vector<PlanOutput>& return_signature() const noexcept {
    return plan_.return_signature();
  }
  const std::vector<WorkflowInputDeclaration>& input_declarations()
      const noexcept {
    return plan_.input_declarations();
  }
  const ResourceSet& resources() const noexcept { return plan_.resources(); }
  const std::map<std::string, TileRequest>& output_requests() const noexcept {
    return plan_.output_requests();
  }
  std::uint64_t tile_extent() const noexcept { return plan_.tile_extent(); }
  ExecutionMode execution_mode() const noexcept {
    return plan_.execution_mode();
  }
  const SchemaTemplate& input_schema(const PlanInput& input) const {
    return plan_.input_schema(input);
  }
  const std::vector<TileGrid>& input_grids(const PlanInput& input) const {
    return plan_.input_grids(input);
  }
  /** @brief Mutable instruction sequence for a linked block index.
   * Initially empty. Throws std::out_of_range for an invalid index. Instruction
   * references and dependencies are checked after successful callback return.
   */
  std::vector<PlanTileInstruction>& instructions(std::size_t block) {
    return plan_.blocks_.at(block).instructions;
  }

 private:
  friend class Compiler;
  explicit PlanBuilder(ExecutionPlan plan) : plan_(std::move(plan)) {}
  ExecutionPlan plan_;
};

/** @brief Named trusted in-process physical planning backend.
 * The callback reads the original immutable IR and linked builder declarations,
 * then emits instructions. Return a failure to discard the builder. Successful
 * output is checked for coverage, ordering, grants and instruction budgets;
 * invalid output is Internal. Structural checks do not prove semantic mapping
 * or CPU fallback equivalence, which remain the backend's responsibility.
 * Callbacks must support concurrent invocations, retain no builder references,
 * and invoke only pure static operation hooks, never runtime callbacks.
 */
struct PHOTOSPIDER_API PlannerDefinition final {
  std::string name, version, description;
  std::function<Status(const ValidatedIR&, PlanBuilder&,
                       const PlanningOptions&)>
      plan;
  std::vector<IRForm> supported_ir_forms = {IRForm::DependencyPaths,
                                            IRForm::ControlFlow};
};

/** @brief Registration-order catalog, immutable after freeze.
 * Register and freeze on one thread before sharing. Frozen reads are
 * concurrent. Returned definition pointers remain valid until mutation or
 * destruction.
 */
class PHOTOSPIDER_API PlannerRegistry final {
 public:
  /** @brief Stable catalog identity; share ownership instead of copying or
   * moving a registry, so published definitions remain valid after freeze.
   */
  PlannerRegistry() = default;
  PlannerRegistry(const PlannerRegistry&) = delete;
  PlannerRegistry& operator=(const PlannerRegistry&) = delete;
  PlannerRegistry(PlannerRegistry&&) = delete;
  PlannerRegistry& operator=(PlannerRegistry&&) = delete;

  /** @brief Add a unique backend before freeze. InvalidArgument rejects an
   * empty/oversized name or version, missing callback, duplicate name, invalid
   * or repeated IR form, or frozen registry. Allocation failures may throw.
   */
  Status add(PlannerDefinition definition);
  void freeze() noexcept { frozen_ = true; }
  bool frozen() const noexcept { return frozen_; }
  const std::vector<PlannerDefinition>& definitions() const noexcept {
    return definitions_;
  }
  const PlannerDefinition* find(const std::string& name) const noexcept;

 private:
  bool frozen_ = false;
  std::vector<PlannerDefinition> definitions_;
};

/** @brief Shared frozen catalog containing static-tiles@1.
 * Initialization is thread-safe and may throw on allocation failure.
 */
PHOTOSPIDER_API std::shared_ptr<const PlannerRegistry>
make_default_planner_registry();
}  // namespace ps
