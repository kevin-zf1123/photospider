#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {

/**
 * @brief Typed non-security identity of canonical semantic graph bytes.
 *
 * @note This value is not substitutable for optimized, plan, or cache identity.
 */
struct PHOTOSPIDER_API SemanticGraphDigest final {
  /** @brief Fixed lowercase hexadecimal representation. */
  std::string value;
};

/**
 * @brief Typed non-security identity of canonical optimized graph bytes.
 *
 * @note Domain separation remains explicit even when payloads are equal.
 */
struct PHOTOSPIDER_API OptimizedGraphDigest final {
  /** @brief Fixed lowercase hexadecimal representation. */
  std::string value;
};

/**
 * @brief Typed non-security identity of one canonical physical plan.
 *
 * @note The digest is a reproducibility aid, not native-code admission proof.
 */
struct PHOTOSPIDER_API ExecutionPlanDigest final {
  /** @brief Fixed lowercase hexadecimal representation. */
  std::string value;
};

/**
 * @brief Typed key for a disposable derived physical-plan cache entry.
 *
 * @note A matching key never replaces full plan/currentness validation, and a
 * missing entry is rebuilt from source rather than recovered.
 */
struct PHOTOSPIDER_API PlanCacheKey final {
  /** @brief Fixed lowercase hexadecimal representation. */
  std::string value;
};

/** @brief One independently inferred result in declaration order. */
struct PHOTOSPIDER_API SemanticOutput final {
  std::string key;
  /** @brief Complete resolved Result schema. */
  std::shared_ptr<const SchemaTemplate> result_schema = {};
};

/**
 * @brief One normalized typed node in semantic compiler IR.
 *
 * @note Traits are copied values. Optional static preparation owns immutable
 * derived state and its definition lease; neither enters canonical identity.
 */
struct PHOTOSPIDER_API SemanticNode final {
  /** @brief Nonzero source node id. */
  std::uint64_t id = 0;
  /** @brief Stable registered operation key. */
  std::string operation;
  /** @brief Tagged producer references in exact input order. */
  std::vector<WorkflowInput> inputs;
  /** @brief Canonically ordered normalized parameters. */
  std::map<std::string, ParameterValue> parameters;
  /** @brief Copied compiler-visible operation traits. */
  OperationTraits traits;
  /** @brief Immutable static preparation shared across outputs and executions.
   * Retains its definition; pointer/state bytes are excluded from digests.
   */
  std::shared_ptr<const PreparedOperation> prepared = {};
  /** @brief Independent named result metadata. */
  std::vector<SemanticOutput> outputs;
};

/**
 * @brief Immutable normalized and typed semantic graph.
 *
 * @note Node order is deterministic topological order with node-id tie breaks.
 */
class PHOTOSPIDER_API SemanticGraphIR final {
 public:
  /**
   * @brief Constructs an invalid semantic IR placeholder.
   * @throws Nothing.
   * @note Only `Compiler::analyze` produces an IR that may be optimized.
   */
  SemanticGraphIR() noexcept = default;

  /**
   * @brief Returns the captured nonzero GraphContext revision.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note The value participates in stale-publication rejection.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns nodes in deterministic topological order.
   * @return Immutable borrowed node sequence owned by this IR.
   * @throws Nothing.
   * @note The reference remains valid for this IR object's lifetime.
   */
  [[nodiscard]] const std::vector<SemanticNode>& nodes() const noexcept {
    return nodes_;
  }
  /**
   * @brief Returns exact requested named source outputs.
   * @return Immutable borrowed output sequence owned by this IR.
   * @throws Nothing.
   * @note Names and node ids are normalized during semantic analysis.
   */
  [[nodiscard]] const std::vector<WorkflowOutput>& outputs() const noexcept {
    return outputs_;
  }
  /**
   * @brief Returns the non-security semantic digest.
   * @return Immutable borrowed digest owned by this IR.
   * @throws Nothing.
   * @note The digest supports reproducibility and is not a trust identity.
   */
  [[nodiscard]] const SemanticGraphDigest& digest() const noexcept {
    return digest_;
  }
  /**
   * @brief Returns canonical declaration metadata sorted by id.
   * @return Immutable vector owned by this stage, valid for its lifetime.
   * @throws Nothing.
   * @note Contains no runtime payload; concurrent immutable reads are safe.
   */
  [[nodiscard]] const std::vector<WorkflowInputDeclaration>&
  input_declarations() const noexcept {
    return input_declarations_;
  }
  /** @brief Frozen resources required by static input/output descriptions.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceBindings& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether the captured graph revision remains current.
   * @return True only for a compiler-produced IR whose context is unchanged.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

 private:
  friend class Compiler;

  /** @brief Immutable resources required to interpret static metadata. */
  ResourceBindings resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Deterministic topologically sorted semantic nodes. */
  std::vector<SemanticNode> nodes_;
  /** @brief Named requested outputs. */
  std::vector<WorkflowOutput> outputs_;
  /** @brief Canonical non-security stage digest. */
  SemanticGraphDigest digest_;
  /** @brief Runtime-only currentness predicate excluded from identity. */
  std::function<bool()> current_check_;
  /** @brief Runtime-only frozen operation-set identity excluded from digest. */
  std::weak_ptr<OperationRegistry> operation_registry_;
};

/**
 * @brief Immutable semantics-equivalent optimized graph IR.
 *
 * @note The current conservative optimizer is an explicit no-op stage that
 * preserves stable source ids while establishing independent stage identity.
 */
class PHOTOSPIDER_API OptimizedGraphIR final {
 public:
  /**
   * @brief Constructs an invalid optimized IR placeholder.
   * @throws Nothing.
   * @note Only `Compiler::optimize` produces an IR that may be planned.
   */
  OptimizedGraphIR() noexcept = default;
  /**
   * @brief Returns the captured GraphContext revision.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note The revision is preserved from semantic analysis.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns optimized nodes in deterministic topological order.
   * @return Immutable borrowed node sequence owned by this IR.
   * @throws Nothing.
   * @note Stable source node ids remain available for diagnostics.
   */
  [[nodiscard]] const std::vector<SemanticNode>& nodes() const noexcept {
    return nodes_;
  }
  /**
   * @brief Returns exact named outputs after optimization remapping.
   * @return Immutable borrowed output sequence owned by this IR.
   * @throws Nothing.
   * @note Output ordering is deterministic.
   */
  [[nodiscard]] const std::vector<WorkflowOutput>& outputs() const noexcept {
    return outputs_;
  }
  /**
   * @brief Returns the source semantic digest.
   * @return Immutable borrowed parent-stage identity.
   * @throws Nothing.
   * @note This preserves typed identity across compiler stages.
   */
  [[nodiscard]] const SemanticGraphDigest& semantic_digest() const noexcept {
    return semantic_digest_;
  }
  /**
   * @brief Returns the non-security optimized digest.
   * @return Immutable borrowed digest owned by this IR.
   * @throws Nothing.
   * @note The digest supports reproducibility and is not a trust identity.
   */
  [[nodiscard]] const OptimizedGraphDigest& digest() const noexcept {
    return digest_;
  }
  /**
   * @brief Returns canonical declaration metadata sorted by id.
   * @return Immutable vector owned by this stage, valid for its lifetime.
   * @throws Nothing.
   * @note Contains no runtime payload; concurrent immutable reads are safe.
   */
  [[nodiscard]] const std::vector<WorkflowInputDeclaration>&
  input_declarations() const noexcept {
    return input_declarations_;
  }
  /** @brief Frozen resources required by static input/output descriptions.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceBindings& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether the captured graph revision remains current.
   * @return True only when the source context still has the captured revision.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

 private:
  friend class Compiler;

  /** @brief Immutable resources required to interpret static metadata. */
  ResourceBindings resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Optimized semantic nodes. */
  std::vector<SemanticNode> nodes_;
  /** @brief Named outputs after optimization. */
  std::vector<WorkflowOutput> outputs_;
  /** @brief Parent semantic stage digest. */
  SemanticGraphDigest semantic_digest_;
  /** @brief Canonical non-security optimized digest. */
  OptimizedGraphDigest digest_;
  /** @brief Runtime-only currentness predicate excluded from identity. */
  std::function<bool()> current_check_;
  /** @brief Runtime-only frozen operation-set identity excluded from digest. */
  std::weak_ptr<OperationRegistry> operation_registry_;
};

/**
 * @brief Physical planning options supplied by one caller.
 *
 * @note Options select local capabilities only and contain no plugin paths.
 */
/** @brief Explicit placement permission; operation profiles define arithmetic.
 */
enum class ExecutionMode : std::uint32_t { CpuExact = 1, NativeGpu = 2 };

struct PHOTOSPIDER_API PlanningOptions final {
  /** @brief NativeGpu selects declared GPU implementations on the context
   * device, with CPU fallback only when permitted by the operation contract.
   * CpuExact selects CPU implementations.
   * @note Each operation/profile defines its numerical behavior. Placement
   * changes physical identity, not source parameters or input bits.
   */
  ExecutionMode execution_mode = ExecutionMode::CpuExact;
  /** @brief Explicit tile indices or complete output for each named output.
   * Missing names default to Whole. Unknown names, Whole plus indices,
   * indices without a grid and out-of-grid indices fail before publication.
   * Nonempty requests for Whole producers normalize to complete output;
   * empty tile sets remain empty. Coordinates are converted by the caller.
   */
  std::map<std::string, TileRequest> output_requests;
  /** @brief Plan-wide square tile extent, a positive power of two including 1.
   */
  std::uint64_t tile_extent = 128;
};

/** @brief Reference to an earlier physical step. */
struct PHOTOSPIDER_API PlanStepInput final {
  /** @brief Earlier step index; contributes a scheduling dependency. */
  std::size_t step_index = 0;
};
/** @brief Reference to canonical external declaration metadata. */
struct PHOTOSPIDER_API PlanWorkflowInput final {
  /** @brief In-bounds declaration index; contributes no task dependency. */
  std::size_t declaration_index = 0;
};
/** @brief Tagged physical producer reference in input-port order. */
using PlanInput = std::variant<PlanStepInput, PlanWorkflowInput>;

/**
 * @brief One validated local physical plan step.
 *
 * @note `inputs` distinguishes earlier steps from external declarations.
 */
struct PHOTOSPIDER_API PlanStep final {
  /** @brief Stable source node id. */
  std::uint64_t node_id = 0;
  /** @brief Selected declaration-order output. */
  std::uint32_t output_index = 0;
  /** @brief Result route independent of physical scheduling. */
  ValueRef result_ref() const noexcept { return {node_id, output_index}; }
  /** @brief Stable operation key. */
  std::string operation;
  /** @brief Tagged physical producers in exact input order. */
  std::vector<PlanInput> inputs;
  /** @brief Canonically ordered source parameters. */
  std::map<std::string, ParameterValue> parameters;
  /** @brief Copied semantic traits used for validation/fallback. */
  OperationTraits traits;
  /** @brief Immutable static preparation shared across outputs and executions.
   * Retains its definition; pointer/state bytes are excluded from digests.
   */
  std::shared_ptr<const PreparedOperation> prepared = {};
  /** @brief Selected local physical backend. */
  Backend backend = Backend::Cpu;
  /** @brief Resolved fixed schema for a paged output; no Value shape is
   * invented. */
  std::shared_ptr<const SchemaTemplate> output_result_schema = {};
  /** @brief Immutable staged metadata owned by compilation, borrowed by Runs.
   */
  std::shared_ptr<const ResultProgramMetadata> structured_metadata = {};
  /** @brief Spatial storage grid when this Result has one spatial tensor only.
   */
  std::optional<TileGrid> tile_grid;
  /** @brief True only for a declared Tiled output with a resolved tile grid.
   * Whole outputs use dependency/computation unit zero even when grid is
   * present.
   */
  bool tiled = false;
};

/**
 * @brief Immutable validated local physical execution plan.
 *
 * @note Steps may retain opaque immutable preparation and definition leases.
 * Native execution handles and daemon objects are not part of a plan.
 */
class PHOTOSPIDER_API ExecutionPlan final {
 public:
  /**
   * @brief Constructs an invalid execution-plan placeholder.
   * @throws Nothing.
   * @note Only `Compiler::plan` produces an executable plan.
   */
  ExecutionPlan() noexcept = default;
  /**
   * @brief Returns the captured GraphContext revision.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note Execution rechecks currentness before and after work.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns dependency-ordered local plan steps.
   * @return Immutable borrowed step sequence owned by this plan.
   * @throws Nothing.
   * @note Tagged step indexes refer only to earlier steps; declaration indexes
   * refer to the canonical input_declarations() table.
   */
  [[nodiscard]] const std::vector<PlanStep>& steps() const noexcept {
    return steps_;
  }
  /** @brief Compiler-owned static result identities; no Run-time rehashing. */
  const std::vector<std::string>& structured_templates() const noexcept {
    return structured_templates_;
  }
  /**
   * @brief Returns named output to step-index mapping.
   * @return Immutable borrowed sorted mapping owned by this plan.
   * @throws Nothing.
   * @note Every mapped index is validated against `steps()`.
   */
  [[nodiscard]] const std::map<std::string, std::size_t>& outputs()
      const noexcept {
    return outputs_;
  }
  /**
   * @brief Returns the parent optimized digest.
   * @return Immutable borrowed parent-stage identity.
   * @throws Nothing.
   * @note This preserves typed identity across compiler stages.
   */
  [[nodiscard]] const OptimizedGraphDigest& optimized_digest() const noexcept {
    return optimized_digest_;
  }
  /**
   * @brief Returns the non-security physical-plan digest.
   * @return Immutable borrowed digest owned by this plan.
   * @throws Nothing.
   * @note The digest supports reproducibility and is not a trust identity.
   */
  [[nodiscard]] const ExecutionPlanDigest& digest() const noexcept {
    return digest_;
  }
  /**
   * @brief Returns the typed disposable derived-cache lookup key.
   * @return Immutable borrowed cache key owned by this plan.
   * @throws Nothing.
   * @note A cache hit never replaces currentness and plan validation.
   */
  [[nodiscard]] const PlanCacheKey& cache_key() const noexcept {
    return cache_key_;
  }
  /**
   * @brief Returns canonical declaration metadata sorted by id.
   * @return Immutable vector owned by this stage, valid for its lifetime.
   * @throws Nothing.
   * @note Contains no runtime payload; concurrent immutable reads are safe.
   */
  [[nodiscard]] const std::vector<WorkflowInputDeclaration>&
  input_declarations() const noexcept {
    return input_declarations_;
  }
  /** @brief Frozen resources required by static input/output descriptions.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceBindings& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether the captured graph revision remains current.
   * @return True only when the source context still has the captured revision.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

  /** @brief Normalized requests, including default Whole for omitted names. */
  const std::map<std::string, TileRequest>& output_requests() const noexcept {
    return output_requests_;
  }
  /** @brief Returns positive spatial tile geometry fixed by planning. */
  std::uint64_t tile_extent() const noexcept { return tile_extent_; }
  /** @brief Workflow input grids in declaration order; storage-independent. */
  const std::vector<std::optional<TileGrid>>& input_tile_grids()
      const noexcept {
    return input_tile_grids_;
  }
  /** @brief Explicit placement permission retained when deriving tile plans. */
  ExecutionMode execution_mode() const noexcept { return execution_mode_; }
  /**
   * @brief Derives one demand-local plan without reanalyzing or enumerating
   * tiles.
   * @param output_name Existing named output.
   * @param request Tile subset of that name's normalized plan request.
   * @return A named-output plan or Stale/InvalidArgument for an invalid subset.
   * @throws std::bad_alloc For graph metadata allocation.
   * @note No coordinates are converted and no pixel storage is allocated.
   * Whole producers normalize nonempty requests before subset validation.
   */
  Result<ExecutionPlan> tile_plan(const std::string& output_name,
                                  const TileRequest& request) const;

 private:
  friend class Compiler;
  friend class ExecutionContext;
  friend class DemandHandle;

  std::map<std::string, TileRequest> output_requests_;
  ExecutionMode execution_mode_ = ExecutionMode::CpuExact;
  std::uint64_t tile_extent_ = 128;
  std::vector<std::optional<TileGrid>> input_tile_grids_;
  /** @brief Immutable resources required to interpret static metadata. */
  ResourceBindings resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Validated dependency-ordered plan steps. */
  std::vector<PlanStep> steps_;
  std::vector<std::string> structured_templates_;
  /** @brief Sorted named output mapping. */
  std::map<std::string, std::size_t> outputs_;
  /** @brief Parent optimized stage digest. */
  OptimizedGraphDigest optimized_digest_;
  /** @brief Canonical non-security plan digest. */
  ExecutionPlanDigest digest_;
  /** @brief Domain-separated derived-cache lookup key. */
  PlanCacheKey cache_key_;
  /** @brief Runtime-only currentness predicate excluded from identity. */
  std::function<bool()> current_check_;
  /** @brief Runtime-only frozen operation-set identity excluded from digest. */
  std::weak_ptr<OperationRegistry> operation_registry_;
};

/**
 * @brief Raw wall-clock compiler stage timings.
 *
 * @note Timings are diagnostics, not performance verdicts or release evidence.
 */
struct PHOTOSPIDER_API CompilationDiagnostics final {
  /** @brief Semantic analysis duration in microseconds. */
  std::uint64_t analyze_us = 0;
  /** @brief Optimization duration in microseconds. */
  std::uint64_t optimize_us = 0;
  /** @brief Physical planning duration in microseconds. */
  std::uint64_t plan_us = 0;
};

/**
 * @brief Complete immutable compiler output for one graph revision.
 *
 * @note Stage objects remain separately inspectable and separately identified.
 */
struct PHOTOSPIDER_API CompiledWorkflow final {
  /** @brief Typed normalized semantic stage. */
  SemanticGraphIR semantic;
  /** @brief Semantics-equivalent optimized stage. */
  OptimizedGraphIR optimized;
  /** @brief Local physical execution plan. */
  ExecutionPlan plan;
  /** @brief Raw compiler stage timings. */
  CompilationDiagnostics diagnostics;
};

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
   * @throws std::invalid_argument If registry is null or mutable.
   * @note Analysis may invoke pure metadata/static preparation hooks. Runtime
   * execution callbacks are not invoked during analysis or planning.
   */
  explicit Compiler(std::shared_ptr<OperationRegistry> operations);

  /**
   * @brief Builds normalized typed semantic IR.
   * @param snapshot Coherent GraphContext document/revision snapshot.
   * @param resources Explicit immutable profile bindings for static facets.
   * Missing CMYK profiles fail before publication; required owners are
   * retained.
   * @return Semantic IR or complete graph/operation validation failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note Failure publishes no partial IR.
   */
  [[nodiscard]] Result<SemanticGraphIR> analyze(
      const GraphSnapshot& snapshot, ResourceBindings resources = {}) const;

  /**
   * @brief Applies deterministic semantics-preserving optimizer rules.
   * @param semantic Valid semantic IR.
   * @return Optimized IR or stale/invalid-stage failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note The input object remains unchanged.
   */
  [[nodiscard]] Result<OptimizedGraphIR> optimize(
      const SemanticGraphIR& semantic) const;

  /**
   * @brief Lowers optimized IR into a validated local physical plan.
   * @param optimized Valid optimized IR.
   * @param options Caller local-capability choices.
   * @return Plan or stale/backend/overflow validation failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note GPU selection never introduces a remote/device handle into the plan.
   */
  [[nodiscard]] Result<ExecutionPlan> plan(
      const OptimizedGraphIR& optimized,
      const PlanningOptions& options = {}) const;

  /**
   * @brief Runs analyze, optimize, and plan as one fail-before-publication
   * flow.
   * @param context Independently owned source graph context.
   * @param resources Explicit immutable profile bindings, frozen at analysis.
   * Changing profile content requires matching metadata and recompilation.
   * @param options Caller local-capability choices.
   * @return Complete stage chain plus raw timings, or the first failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note A replacement racing the pipeline returns `Stale` before success.
   */
  [[nodiscard]] Result<CompiledWorkflow> compile(
      const GraphContext& context, const PlanningOptions& options = {},
      ResourceBindings resources = {}) const;

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
  /** @brief Frozen shared operation set. */
  std::shared_ptr<OperationRegistry> operations_;
};

}  // namespace ps
