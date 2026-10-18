#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/ir.hpp"
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

/**
 * @brief Immutable normalized and typed semantic graph.
 *
 * @note Call-table order is deterministic metadata order; blocks own execution
 * order.
 */
class PHOTOSPIDER_API SemanticGraphIR final {
 public:
  /**
   * @brief Constructs an invalid semantic IR placeholder.
   * @throws Nothing.
   * @note Analysis and artifact loading produce IR that may be optimized.
   */
  SemanticGraphIR() noexcept = default;

  /**
   * @brief Returns a captured graph revision, or one for a loaded artifact.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note The value participates in stale-publication rejection.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns prepared call declarations in deterministic metadata order.
   * @return Immutable borrowed node sequence owned by this IR.
   * @throws Nothing.
   * @note The reference remains valid for this IR object's lifetime.
   */
  [[nodiscard]] const std::vector<SemanticNode>& nodes() const noexcept {
    return nodes_;
  }
  /**
   * @brief Returns the ordered function result signature.
   * @return Immutable borrowed output sequence owned by this IR.
   * @throws Nothing.
   * @note Every return terminator must satisfy this signature.
   */
  [[nodiscard]] const std::vector<IRReturnDeclaration>& outputs()
      const noexcept {
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
  /** @brief Frozen resources explicitly selected by operation preparation.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceSet& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether this stage remains valid for its owning context.
   * @return True for unchanged source context or a linked standalone artifact.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

  /** @brief Explicit control flow; call-table order is not execution order. */
  const std::vector<IRBlock>& blocks() const noexcept { return blocks_; }
  IRBlockId entry() const noexcept { return entry_; }
  const std::vector<IRInputBinding>& input_bindings() const noexcept {
    return input_bindings_;
  }
  /** @brief Owning editable copy for another frontend or transformation pass.
   */
  IRProgram program() const {
    return {input_declarations_,
            input_bindings_,
            nodes_,
            blocks_,
            entry_,
            outputs_};
  }

 private:
  friend class Compiler;
  friend class CompilerArtifact;

  std::vector<IRBlock> blocks_;
  IRBlockId entry_ = 0;
  std::vector<IRInputBinding> input_bindings_;
  /** @brief Immutable resources explicitly selected by operation preparation.
   */
  ResourceSet resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Prepared semantic calls; blocks define their execution order. */
  std::vector<SemanticNode> nodes_;
  /** @brief Named requested outputs. */
  std::vector<IRReturnDeclaration> outputs_;
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
   * @note Optimization and artifact loading produce IR that may be planned.
   */
  OptimizedGraphIR() noexcept = default;
  /**
   * @brief Returns a captured graph revision, or one for a loaded artifact.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note The revision is preserved from semantic analysis.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns optimized calls in deterministic metadata order.
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
  [[nodiscard]] const std::vector<IRReturnDeclaration>& outputs()
      const noexcept {
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
  /** @brief Frozen resources explicitly selected by operation preparation.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceSet& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether this stage remains valid for its owning context.
   * @return True for unchanged source context or a linked standalone artifact.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

  /** @brief Explicit control flow; call-table order is not execution order. */
  const std::vector<IRBlock>& blocks() const noexcept { return blocks_; }
  IRBlockId entry() const noexcept { return entry_; }
  const std::vector<IRInputBinding>& input_bindings() const noexcept {
    return input_bindings_;
  }
  /** @brief Owning editable copy for another frontend or transformation pass.
   */
  IRProgram program() const {
    return {input_declarations_,
            input_bindings_,
            nodes_,
            blocks_,
            entry_,
            outputs_};
  }

 private:
  friend class Compiler;
  friend class CompilerArtifact;

  std::vector<IRBlock> blocks_;
  IRBlockId entry_ = 0;
  std::vector<IRInputBinding> input_bindings_;
  /** @brief Immutable resources explicitly selected by operation preparation.
   */
  ResourceSet resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Optimized semantic nodes. */
  std::vector<SemanticNode> nodes_;
  /** @brief Named outputs after optimization. */
  std::vector<IRReturnDeclaration> outputs_;
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
  /** @brief Explicit member tile intervals for each named output.
   * Missing names select all members through compact intervals. Unknown names,
   * member indices and out-of-grid intervals fail before publication.
   * Empty requests remain empty. Coordinates are converted by the caller.
   */
  std::map<std::string, TileRequest> output_requests;
  /** @brief Plan-wide square tile extent, a positive power of two including 1.
   */
  std::uint64_t tile_extent = 128;
  /** @brief Bound on static tile instructions and dependency edges. */
  std::uint64_t maximum_instructions = 4194304;
  std::uint64_t maximum_dependency_edges = 16777216;
};

/** @brief Reference to a static call result; execution follows blocks. */
struct PHOTOSPIDER_API PlanStepInput final {
  /** @brief Call-result metadata index. Static instructions encode readiness.
   */
  std::size_t step_index = 0;
};
/** @brief Reference to canonical external declaration metadata. */
struct PHOTOSPIDER_API PlanWorkflowInput final {
  /** @brief In-bounds declaration index; contributes no task dependency. */
  std::size_t declaration_index = 0;
};
/** @brief Tagged physical producer reference in input-port order. */
/** @brief A block parameter populated by parallel edge transfer. */
struct PHOTOSPIDER_API PlanArgumentInput final {
  std::size_t parameter_index = 0;
};
// NOLINTBEGIN(whitespace/indent_namespace)
using PlanInput =
    std::variant<PlanStepInput, PlanWorkflowInput, PlanArgumentInput>;
// NOLINTEND

struct PHOTOSPIDER_API PlanParameter final {
  IRValueId value = 0;
  IRBlockId block = 0;
  std::shared_ptr<const SchemaTemplate> result_schema;
  std::vector<TileGrid> tile_grids;
};
struct PHOTOSPIDER_API PlanEdge final {
  std::size_t target = 0;
  std::vector<PlanInput> arguments;
};
struct PHOTOSPIDER_API PlanJump final {
  PlanEdge edge;
};
struct PHOTOSPIDER_API PlanConditionalJump final {
  PlanInput condition;
  PlanEdge if_true;
  PlanEdge if_false;
};
struct PHOTOSPIDER_API PlanReturn final {
  std::vector<PlanInput> values;
};
using PlanTerminator = std::variant<PlanJump, PlanConditionalJump, PlanReturn>;

/** @brief Fully compiled read capability. No runtime coordinate mapping.
 * Samples are the stored union of the complete tiles in tiles. Descriptor-only
 * grants carry no samples and authorize no payload access.
 */
struct PHOTOSPIDER_API PlanTensorRead final {
  std::uint32_t input = 0;
  std::uint32_t tensor_slot = 0;
  bool descriptor_only = false;
  TileSet tiles;
  Footprint samples;
};
struct PHOTOSPIDER_API PlanSharedRead final {
  std::uint32_t stage = 0;
  TileRequest tiles;
  std::vector<Footprint> samples;
};
/** @brief One static tile or shared-stage publication unit.
 * Dependencies are earlier instructions in this block. Inputs and shared
 * commits must be ready when this instruction executes. Poll requests can only
 * select the precompiled grants; they cannot create work or dependencies.
 */
struct PHOTOSPIDER_API PlanTileInstruction final {
  std::size_t step_index = 0;
  std::uint32_t shared_stage = UINT32_MAX;
  std::uint32_t tensor_slot = 0;
  TileIndex tile_unit = 0;
  /** @brief Compiled CPU retry using these same ready grants. The planner
   * proves CPU reads are covered before setting this flag; no remapping occurs
   * on retry. Shared-stage fallback requires an explicit control-flow path.
   */
  bool cpu_fallback = false;
  std::optional<Footprint> tensor_outputs;
  TileRequest publication;
  std::vector<Footprint> publication_samples;
  std::vector<PlanTensorRead> inputs;
  std::vector<PlanSharedRead> shared_inputs;
  std::vector<std::size_t> dependencies;
};
struct PHOTOSPIDER_API PlanBlock final {
  IRBlockId id = 0;
  std::vector<std::size_t> parameters;
  std::vector<std::size_t> calls;
  std::vector<PlanTileInstruction> instructions;
  PlanTerminator terminator = PlanReturn{};
};
struct PHOTOSPIDER_API PlanOutput final {
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema;
  std::vector<TileGrid> tile_grids;
  std::vector<Footprint> requested_samples;
  /** @brief Synthetic return unit index used by ExecutionDependencies.
   * This is steps().size() + the return-signature index, with output ordinal 0.
   * It identifies the named return independently of the branch producing it.
   */
  std::size_t record_index = 0;
};

/**
 * @brief One validated local physical plan step.
 *
 * @note `inputs` distinguishes call results, external declarations and block
 * parameters. Block instructions define execution order independently.
 */
struct PHOTOSPIDER_API PlanStep final {
  /** @brief Stable source node id. */
  std::uint64_t node_id = 0;
  IRBlockId block = 0;
  IRValueId value = 0;
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
  /** @brief Resolved fixed schema for a paged output; no TensorSpan shape is
   * invented. */
  std::shared_ptr<const SchemaTemplate> output_result_schema = {};
  /** @brief Immutable staged metadata owned by compilation, borrowed by Runs.
   */
  std::shared_ptr<const ResultProgramMetadata> structured_metadata = {};
  /** @brief Member-local fixed tile grids in schema order. */
  std::vector<TileGrid> tile_grids;
  /** @brief Prepared stage contracts shared by all outputs of the original
   * node. */
  std::vector<std::shared_ptr<const PlanStep>> shared_stages;
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
   * @note Planning and artifact loading produce executable plans.
   */
  ExecutionPlan() noexcept = default;
  /**
   * @brief Returns a captured graph revision, or one for a loaded artifact.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note Execution rechecks currentness before and after work.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns immutable call/output declarations used by tile
   * instructions.
   * @return Immutable borrowed step sequence owned by this plan.
   * @throws Nothing.
   * @note Step indexes name metadata, not execution order. blocks() owns the
   * executable order. Inputs may name block parameters or dominating calls.
   */
  [[nodiscard]] const std::vector<PlanStep>& steps() const noexcept {
    return steps_;
  }
  /** @brief Compiler-owned static result identities; no Run-time rehashing. */
  const std::vector<std::string>& structured_templates() const noexcept {
    return structured_templates_;
  }
  /**
   * @brief Returns named output to return-signature-index mapping.
   * @return Immutable borrowed sorted mapping owned by this plan.
   * @throws Nothing.
   * @note Every mapped index is validated against return_signature().
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
  /** @brief Frozen resources explicitly selected by operation preparation.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceSet& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether this stage remains valid for its owning context.
   * @return True for unchanged source context or a linked standalone artifact.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

  /** @brief Normalized requests, including all member tiles for omitted names.
   */
  const std::map<std::string, TileRequest>& output_requests() const noexcept {
    return output_requests_;
  }
  /** @brief Returns positive spatial tile geometry fixed by planning. */
  std::uint64_t tile_extent() const noexcept { return tile_extent_; }
  /** @brief Workflow input grids in declaration order; storage-independent. */
  const std::vector<std::vector<TileGrid>>& input_tile_grids() const noexcept {
    return input_tile_grids_;
  }
  /** @brief Explicit placement permission retained when deriving tile plans. */
  ExecutionMode execution_mode() const noexcept { return execution_mode_; }
  /**
   * @brief Derives one demand-local plan by filtering compiled tile
   * instructions.
   * @param output_name Existing named output.
   * @param request Tile subset of that name's normalized plan request.
   * @return A named-output plan or Stale/InvalidArgument for an invalid subset.
   * @throws std::bad_alloc For graph metadata allocation.
   * @note No coordinates are converted and no pixel storage is allocated.
   */
  Result<ExecutionPlan> tile_plan(const std::string& output_name,
                                  const TileRequest& request) const;

  /** @brief Selects named tile subsets from the already compiled program.
   * Filters existing instructions and preserves control/effect dependencies.
   * No operation mapper, preparation or optimizer callback is invoked.
   * Requests must be subsets of this plan; omitted names are not returned.
   * Returns Stale/InvalidArgument for an invalid parent or subset.
   */
  Result<ExecutionPlan> select_outputs(
      const std::map<std::string, TileRequest>& requests) const;

  /** @brief Serialized instruction blocks; execution never lowers these. */
  const std::vector<PlanBlock>& blocks() const noexcept { return blocks_; }
  std::size_t entry() const noexcept { return entry_; }
  const std::vector<PlanParameter>& parameters() const noexcept {
    return parameters_;
  }
  const std::vector<PlanOutput>& return_signature() const noexcept {
    return return_signature_;
  }
  const PlanOutput& output(const std::string& name) const {
    return return_signature_.at(outputs_.at(name));
  }
  /** @brief Metadata for a statically checked physical value reference. */
  const SchemaTemplate& input_schema(const PlanInput& input) const;
  const std::vector<TileGrid>& input_grids(const PlanInput& input) const;

 private:
  friend class Compiler;
  friend class CompilerArtifact;
  friend class ExecutionContext;
  friend class DemandHandle;

  IRProgram linked_program_;
  std::vector<PlanBlock> blocks_;
  std::size_t entry_ = 0;
  std::vector<PlanParameter> parameters_;
  std::vector<PlanOutput> return_signature_;
  std::map<std::string, TileRequest> output_requests_;
  ExecutionMode execution_mode_ = ExecutionMode::CpuExact;
  std::uint64_t tile_extent_ = 128;
  std::vector<std::vector<TileGrid>> input_tile_grids_;
  /** @brief Immutable resources explicitly selected by operation preparation.
   */
  ResourceSet resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Validated call-result metadata; blocks own execution order. */
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
   * @param resources Explicit immutable resources selected by operation
   * preparation. Missing requested identities fail analysis; selected resource
   * owners are retained by the compiled state.
   * @return Semantic IR or complete graph/operation validation failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note Failure publishes no partial IR.
   */
  [[nodiscard]] Result<SemanticGraphIR> analyze(
      const GraphSnapshot& snapshot, ResourceSet resources = {}) const;

  /** @brief Validate and link frontend-neutral SSA IR, including cyclic CFG.
   * No runtime callback runs here. All schemas, dominance, operation contracts
   * and edge transfers are validated before returning an immutable stage.
   * The caller retains its program; callback exceptions become typed failures.
   */
  [[nodiscard]] Result<SemanticGraphIR> bind_ir(
      IRProgram program, ResourceSet resources = {}) const;

  /**
   * @brief Produces the independently identified optimized IR stage.
   * The current optimizer preserves all calls, values and control flow.
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
   * @param resources Immutable resources available for explicit selection by
   * static operation preparation. Changing selected identities or content
   * requires preparation and compilation again.
   * @param options Caller local-capability choices.
   * @return Complete stage chain plus raw timings, or the first failure.
   * @throws std::bad_alloc If staging allocation fails.
   * @note A replacement racing the pipeline returns `Stale` before success.
   */
  [[nodiscard]] Result<CompiledWorkflow> compile(
      const GraphContext& context, const PlanningOptions& options = {},
      ResourceSet resources = {}) const;

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
  /** @brief Rebuild derived grids, metadata and cache identity from fixed
   * steps. Neither backend selection nor graph lowering occurs here.
   */
  Result<ExecutionPlan> link_plan(
      const OptimizedGraphIR& ir, const PlanningOptions& options,
      const std::vector<Backend>* placements = nullptr) const;
  Status select_plan(ExecutionPlan* plan) const;
  Status finalize_plan(ExecutionPlan* plan) const;
  Status expand_plan(ExecutionPlan* plan, const PlanningOptions& options) const;
  Status validate_plan(const ExecutionPlan& plan) const;
  /** @brief Frozen shared operation set. */
  std::shared_ptr<OperationRegistry> operations_;
};

}  // namespace ps
