#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "photospider/compiler/ir.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {

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

/** @brief Explicit placement permission; operation profiles define arithmetic.
 */
enum class ExecutionMode : std::uint32_t { CpuExact = 1, NativeGpu = 2 };

/** @brief Physical planning choices for one compilation, including the named
 * backend. Contains no plugin paths or runtime device handles.
 */
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
  /** @brief Explicit output selection; absent selects all, empty selects none.
   */
  std::optional<std::vector<std::string>> output_names = std::nullopt;
  /** @brief Algorithm used by Compiler::plan and Compiler::compile.
   * Resolved in the compiler's PlannerRegistry; unknown names return NotFound.
   * This choice
   * is compilation-only; saved plans contain the resulting instructions.
   */
  std::string planner = "static-tiles";
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
   * @brief Returns the actual input IR content digest.
   * @return Immutable borrowed parent-stage identity.
   * @throws Nothing.
   * @note This preserves typed identity across compiler stages.
   */
  [[nodiscard]] const IRDigest& input_ir_digest() const noexcept {
    return input_ir_digest_;
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
  friend class PlanBuilder;
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
  /** @brief Parent input IR digest. */
  IRDigest input_ir_digest_;
  /** @brief Canonical non-security plan digest. */
  ExecutionPlanDigest digest_;
  /** @brief Domain-separated derived-cache lookup key. */
  PlanCacheKey cache_key_;
  /** @brief Runtime-only currentness predicate excluded from identity. */
  std::function<bool()> current_check_;
  /** @brief Runtime-only frozen operation-set identity excluded from digest. */
  std::weak_ptr<OperationRegistry> operation_registry_;
};

}  // namespace ps
