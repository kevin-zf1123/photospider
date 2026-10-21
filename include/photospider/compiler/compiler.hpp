#pragma once

#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/compilation.hpp"
#include "photospider/compiler/frontend.hpp"
#include "photospider/compiler/optimizer.hpp"
#include "photospider/compiler/planner.hpp"

namespace ps {

/**
 * @brief Typed compiler, optimizer, and local physical planner facade.
 *
 * @note The referenced operation registry must be frozen for this object's
 * lifetime and is safe for concurrent compilation reads.
 */
class PHOTOSPIDER_API Compiler final {
 public:
  /**
   * @brief Constructs a compiler over a frozen operation set.
   * @param operations Shared registry retained by the compiler.
   * @param optimizers Optional frozen optimizer catalog retained by the
   * compiler.
   * @param planners Optional frozen planner catalog retained by the compiler.
   * @param frontends Optional frozen frontend catalog retained by the compiler.
   * @throws std::invalid_argument If operations is null or any registry is
   * mutable.
   * @note Analysis may invoke pure metadata/static preparation hooks. Runtime
   * execution callbacks are not invoked during analysis or planning.
   */
  explicit Compiler(std::shared_ptr<OperationRegistry> operations,
                    std::shared_ptr<const OptimizerRegistry> optimizers = {},
                    std::shared_ptr<const PlannerRegistry> planners = {},
                    std::shared_ptr<const FrontendRegistry> frontends = {});

  /**
   * @brief Builds normalized typed semantic IR.
   * @param snapshot Coherent GraphContext document/revision snapshot.
   * @param resources Explicit immutable resources selected by operation
   * preparation. Missing requested identities fail analysis; selected resource
   * owners are retained by the compiled state.
   * @param frontend Registered frontend name; defaults to dag.
   * @return Validated IR, NotFound for an unknown frontend, Stale for an
   * expired snapshot, or the frontend/binding failure. Callback exceptions are
   * Internal; allocation failure inside the callback is ResourceExhausted.
   * @note Binding errors retain their original status because frontend source
   * is unvalidated. Provenance uses the registered name and version.
   * @throws std::bad_alloc If staging allocation fails.
   * @note Failure publishes no partial IR.
   * The default DAG frontend preserves every path in input-ordered DFS
   * postorder, starting at named outputs in name order, then unreachable nodes
   * in ID order. Shared ancestors appear in every referencing path with the
   * same call and SSA IDs. Branch switches, multi-input joins and new roots
   * split chains. Cycles fail with ErrorCode::Cycle; expansion beyond 1,048,576
   * call occurrences or 65,536 chains fails with ErrorCode::ResourceExhausted.
   */
  [[nodiscard]] Result<ValidatedIR> analyze(
      const GraphSnapshot& snapshot, ResourceSet resources = {},
      const std::string& frontend = "dag") const;

  /** @brief Validate and link frontend-neutral SSA IR, including cyclic CFG.
   * No runtime callback runs here. All schemas, dominance, operation contracts
   * and edge transfers are validated before returning an immutable stage.
   * The caller retains its program; callback exceptions become typed failures.
   * DependencyPaths must be a linear, parameterless chain with a final return.
   * Repeated requests share one definition; each input must already be defined
   * on first use. Handwritten paths need not reproduce canonical DAG expansion.
   */
  [[nodiscard]] Result<ValidatedIR> bind_ir(IRProgram program,
                                            ResourceSet resources = {}) const;

  /**
   * @brief Run one named transformation and validate its new IR program.
   * @param semantic Current IR bound to this compiler's registry.
   * @param invocation Optimizer name and typed overrides of declared defaults.
   * @return Validated IR with provenance, or typed failure. Invalid transform
   * output and callback exceptions are Internal; prior versions remain valid.
   * @throws std::bad_alloc If staging allocation fails outside the callback.
   * @note Input is immutable. A no-op may retain the same content digest.
   */
  [[nodiscard]] Result<ValidatedIR> optimize(
      const ValidatedIR& semantic,
      const OptimizerInvocation& invocation = {"optimize", {}}) const;

  /**
   * @brief Lowers validated IR into a validated local physical plan.
   * @param ir Valid IR; raw dependency paths are lowered privately.
   * @param options Planner backend and caller local-capability choices.
   * @return Plan, NotFound for an unknown planner, InvalidArgument for an
   * unsupported IR form, Stale for foreign/expired IR, or planning failure.
   * Invalid backend output and callback exceptions are Internal; allocation
   * failure inside the callback is ResourceExhausted. No partial Plan escapes.
   * @throws std::bad_alloc If staging allocation fails.
   * @note GPU selection never introduces a remote/device handle into the plan.
   */
  [[nodiscard]] Result<ExecutionPlan> plan(
      const ValidatedIR& ir, const PlanningOptions& options = {}) const;

  /**
   * @brief Runs analyze, explicitly selected optimizers, and plan as one
   * fail-before-publication flow.
   * @param context Independently owned source graph context.
   * @param resources Immutable resources available for explicit selection by
   * static operation preparation. Changing selected identities or content
   * requires preparation and compilation again.
   * @param options Planner backend and caller local-capability choices.
   * @param optimizers Ordered invocations, including repetitions; empty skips
   * optimization.
   * @param frontend Registered frontend name used by analyze.
   * @return Complete stage chain plus raw timings, or the first failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note A replacement racing the pipeline returns `Stale` before success.
   */
  [[nodiscard]] Result<CompiledWorkflow> compile(
      const GraphContext& context, const PlanningOptions& options = {},
      ResourceSet resources = {},
      const std::vector<OptimizerInvocation>& optimizers = {},
      const std::string& frontend = "dag") const;

  /** @brief Frozen ordered optimizer catalog; the default is created lazily.
   * Operations-only compilation, binding and Plan loading do not create it.
   * First default-catalog access may throw on allocation failure.
   */
  const OptimizerRegistry& optimizers() const;

  /** @brief Frozen planner catalog retained by this compiler, or the lazy
   * default catalog. Binding, analysis and Plan import do not initialize it.
   * The borrowed reference is valid for the compiler lifetime. First default
   * access may throw on allocation failure; frozen reads are thread-safe.
   */
  const PlannerRegistry& planners() const;

  /** @brief Frozen frontend catalog retained by this compiler, or the lazy
   * default catalog. Binding and artifact import do not initialize it.
   * The borrowed reference is valid for the compiler lifetime. First default
   * access may throw on allocation failure; frozen reads are thread-safe.
   */
  const FrontendRegistry& frontends() const;

  /**
   * @brief Returns the frozen operation registry used by this compiler.
   * @return Shared registry retained by the compiler.
   * @throws Nothing.
   * @note Callers receive read access by convention; mutation is already
   * fenced.
   */
  [[nodiscard]] std::shared_ptr<OperationRegistry> operations() const noexcept {
    return operations_;
  }

 private:
  friend class CompilerArtifact;
  friend class ExecutionPlan;
  /** @brief Link declarations, derive grids and privately lower dependency
   * paths to control flow. Saved placements bypass backend selection; callers
   * validate the completed instructions before computing Plan/cache identities.
   */
  Result<ExecutionPlan> link_plan(const ValidatedIR& ir,
                                  const PlanningOptions& options) const;
  Status select_plan(ExecutionPlan* plan) const;
  Status finalize_plan(ExecutionPlan* plan) const;
  Status validate_plan(const ExecutionPlan& plan) const;
  /** @brief Frozen shared operation set. */
  std::shared_ptr<OperationRegistry> operations_;
  std::shared_ptr<const OptimizerRegistry> optimizers_;
  std::shared_ptr<const PlannerRegistry> planners_;
  std::shared_ptr<const FrontendRegistry> frontends_;
};

}  // namespace ps
