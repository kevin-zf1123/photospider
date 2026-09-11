#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/status.hpp"
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

/**
 * @brief One normalized typed node in semantic compiler IR.
 *
 * @note Traits are copied values; no callback or DSO pointer enters IR.
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
  /** @brief Statically inferred output Value descriptor. */
  ValueDescriptor output_descriptor;
  /** @brief Canonical inferred output facets, independent of runtime storage.
   */
  std::vector<ValueFacet> output_facets = {};
  /** @brief Local Atomic AND all declared input ancestors EffectiveAtomic. */
  bool effective_atomic = true;
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

  /** @brief Canonical copied input metadata, with no runtime owners. */
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

  /** @brief Canonical copied input metadata, with no runtime owners. */
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
/** @brief Explicit arithmetic and placement policy; CPU exact is the default.
 */
enum class ExecutionMode : std::uint32_t { CpuExact = 1, MetalFp32 = 2 };

struct PHOTOSPIDER_API PlanningOptions final {
  /** @brief MetalFp32 permits native FP32 implementations and typed CPU
   * fallback.
   * @note This changes physical identity, not source parameters or input bits.
   */
  ExecutionMode execution_mode = ExecutionMode::CpuExact;
  /**
   * @brief Optional bounded logical demand per named workflow output.
   *
   * Missing names default to whole-output demand. Unknown names, rank/shape
   * mismatch, empty/out-of-bounds Regions and partial-channel image demand
   * fail before plan publication. Image coverage uses inferred facets and the
   * full logical channel extent, including generic ports and Whole outputs.
   * Changed demand replans optimized IR.
   */
  std::map<std::string, Region> output_regions;
  /** @brief Positive spatial tile extents; changing them only replans optimized
   * IR. */
  std::uint64_t tile_height = 128;
  std::uint64_t tile_width = 128;
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

/** @brief Physical action on local shared storage; HostAccess need not copy. */
enum class PhysicalStepKind : std::uint32_t {
  Upload = 1,
  Operation = 2,
  HostAccess = 3
};

/** @brief Explicit access/operation step with checked packed Region bounds.
 * @note step_index identifies the operation consumer (producer for a named
 * output). input_index is meaningful for unnamed access steps. Native addresses
 * are excluded. Runtime fallback can require a reported additional upload.
 */
struct PHOTOSPIDER_API PhysicalStep final {
  /** @brief Upload, operation execution or completed shared host access. */
  PhysicalStepKind kind = PhysicalStepKind::Operation;
  /** @brief Index in steps(); consumer for input access, producer for output.
   */
  std::size_t step_index = 0;
  /** @brief Ordered consumer input port; unused for operation/named output. */
  std::size_t input_index = 0;
  /** @brief Typed producer; operation records refer to their own step. */
  PlanInput source = PlanStepInput{};
  /** @brief Expected producer implementation before runtime fallback. */
  Backend source_backend = Backend::Cpu;
  /** @brief Planned consumer access domain; host access selects Cpu. */
  Backend destination_backend = Backend::Cpu;
  /** @brief Full logical type/shape independent of packed storage. */
  ValueDescriptor descriptor;
  /** @brief Exact bounded logical demand, retaining complete image channels. */
  Region region;
  /** @brief Packed payload byte count; zero only for non-dense CPU metadata. */
  std::uint64_t packed_bytes = 0;
  /** @brief Upload capacity or operation output/workspace bound; access is
   * zero. */
  std::uint64_t allocation_bytes = 0;
  /** @brief Named result for a terminal host access; empty otherwise. */
  std::string output_name;
  /** @brief Canonical packed target view, with logical Region origin. */
  StridedLayout packed_layout = {};
};

/**
 * @brief One validated local physical plan step.
 *
 * @note `inputs` distinguishes earlier steps from external declarations.
 */
struct PHOTOSPIDER_API PlanStep final {
  /** @brief Stable source node id. */
  std::uint64_t node_id = 0;
  /** @brief Stable operation key. */
  std::string operation;
  /** @brief Tagged physical producers in exact input order. */
  std::vector<PlanInput> inputs;
  /** @brief Canonically ordered source parameters. */
  std::map<std::string, ParameterValue> parameters;
  /** @brief Copied semantic traits used for validation/fallback. */
  OperationTraits traits;
  /** @brief Statically validated output Value descriptor. */
  ValueDescriptor output_descriptor;
  /** @brief Canonical inferred output facets, independent of runtime storage.
   */
  std::vector<ValueFacet> output_facets = {};
  /** @brief Selected local physical backend. */
  Backend backend = Backend::Cpu;
  /** @brief Estimated peak bytes reserved before invocation. */
  std::uint64_t planned_bytes = 0;
  /** @brief Logical result coverage demanded by downstream plan consumers. */
  Region output_demand;
  /** @brief Per-input logical demands derived from the operation Region rule.
   */
  std::vector<Region> input_demands;
  /** @brief Requires one complete materialization, never per-tile
   * recomputation. */
  bool whole_boundary = false;
  /** @brief Compiler-proven observation kind over all input edges. */
  bool effective_atomic = true;
};

/**
 * @brief Immutable validated local physical execution plan.
 *
 * @note Plans contain no native handles, callback pointers, or daemon objects.
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
  /**
   * @brief Reports whether the captured graph revision remains current.
   * @return True only when the source context still has the captured revision.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

  /** @brief Returns each name's exact output Region, including default Whole.
   */
  const std::map<std::string, Region>& output_regions() const noexcept {
    return output_regions_;
  }
  /** @brief Returns positive spatial tile geometry fixed by planning. */
  std::uint64_t tile_height() const noexcept { return tile_height_; }
  std::uint64_t tile_width() const noexcept { return tile_width_; }
  /** @brief Ordered uploads, operations and host access for this exact demand.
   */
  const std::vector<PhysicalStep>& physical_steps() const noexcept {
    return physical_steps_;
  }
  /** @brief Explicit numeric policy retained when deriving tile plans. */
  ExecutionMode execution_mode() const noexcept { return execution_mode_; }
  /**
   * @brief Derives one demand-local plan without reanalyzing or enumerating
   * tiles.
   * @param output_name Existing named output.
   * @param region Nonempty subset of that name's requested Region; inferred
   * image outputs require every logical channel, with spatial ROI allowed.
   * @return A dependency-pruned tile plan or Stale/InvalidArgument/overflow.
   * @throws std::bad_alloc For graph metadata allocation.
   * @note Whole boundaries retain complete demand. No runtime Value is
   * retained; callers must preserve graph/registry currentness as for the
   * parent plan.
   */
  Result<ExecutionPlan> tile_plan(const std::string& output_name,
                                  const Region& region) const;

  /** @brief True when this plan requires staged/terminal dependency execution.
   * @note In this case input_demands are unresolved templates, not Whole or
   * Empty evidence. Runtime certificates carry the actual exact dependencies.
   */
  bool dependency_network() const noexcept;

 private:
  friend class Compiler;
  friend class ExecutionContext;
  friend class DemandHandle;

  std::map<std::string, Region> output_regions_;
  ExecutionMode execution_mode_ = ExecutionMode::CpuExact;
  std::vector<PhysicalStep> physical_steps_;
  std::uint64_t tile_height_ = 128;
  std::uint64_t tile_width_ = 128;
  /** @brief Canonical copied input metadata, with no runtime owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Validated dependency-ordered plan steps. */
  std::vector<PlanStep> steps_;
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
   * @note Registry callbacks are not invoked during analysis or planning.
   */
  explicit Compiler(std::shared_ptr<OperationRegistry> operations);

  /**
   * @brief Builds normalized typed semantic IR.
   * @param snapshot Coherent GraphContext document/revision snapshot.
   * @return Semantic IR or complete graph/operation validation failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note Failure publishes no partial IR.
   */
  [[nodiscard]] Result<SemanticGraphIR> analyze(
      const GraphSnapshot& snapshot) const;

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
   * @param options Caller local-capability choices.
   * @return Complete stage chain plus raw timings, or the first failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note A replacement racing the pipeline returns `Stale` before success.
   */
  [[nodiscard]] Result<CompiledWorkflow> compile(
      const GraphContext& context, const PlanningOptions& options = {}) const;

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
