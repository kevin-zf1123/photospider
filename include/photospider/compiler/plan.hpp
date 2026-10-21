#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/ir.hpp"
#include "photospider/compiler/physical_program.hpp"
#include "photospider/data/tiles.hpp"

namespace ps {
struct PHOTOSPIDER_API ExecutionPlanDigest final {
  std::string value;
};
/** @brief Static backend selection policy, without runtime retry.
 * CpuExact requires CPU tasks. NativeGpu selects GPU lowering for operations
 * advertising GPU support and CPU lowering for CPU-only operations. Such a
 * Plan may contain both backends and explicit COPY tasks. Each lowering fixes
 * its task placements; prepare cannot substitute another implementation.
 */
enum class ExecutionMode : std::uint32_t { CpuExact = 1, NativeGpu = 2 };
struct PHOTOSPIDER_API PlanningOptions final {
  ExecutionMode execution_mode = ExecutionMode::CpuExact;
  std::map<std::string, TileRequest> output_requests;
  /** @brief Per-declaration external allocation contracts. Omitted names use
   * DirectTiles. Unknown names or enum values fail InvalidArgument. DenseHost
   * fixes full-member row-major strides and explicit per-block import COPYs;
   * submit checks the binding and never changes the compiled topology.
   */
  std::map<std::string, physical::InputLayout> input_layouts;
  /** @brief Fixed memory domain of each external input, default Host.
   * DirectTiles may bind Host, Shared or Device owners. DenseHost requires
   * Host. Unknown names or enum values fail InvalidArgument.
   */
  std::map<std::string, physical::Domain> input_domains;
  std::uint64_t tile_extent = 128;
  /** @brief Maximum emitted tasks; independent structural record bounds apply
   * to metadata and incoming tiles, including programs with zero tasks.
   */
  std::uint64_t maximum_tasks = 4194304;
  /** @brief Aggregate physical table records, checked during static expansion
   * and independently at publication. Includes layouts and incoming tiles.
   */
  std::uint64_t maximum_records = 4194304;
  std::uint64_t maximum_dependency_edges = 16777216;
  std::uint64_t maximum_work = 268435456;
  /** @brief Absent selects all named outputs; an empty vector selects none. */
  std::optional<std::vector<std::string>> output_names;
  std::string planner = "static-tiles";
};

/** @brief Static call metadata, indexed exactly like physical::Program::calls.
 * Input/output IDs name physical values. prepared retains static operation and
 * module/resource ownership; device kernels are created only by prepare.
 * No callback state, payload, current Run or cache entry is part of this data.
 */
struct PHOTOSPIDER_API PlanCallBinding final {
  std::string operation;
  std::vector<physical::Id> inputs, outputs;
  std::map<std::string, ParameterValue> parameters;
  std::shared_ptr<const PreparedOperation> prepared;
};
struct PHOTOSPIDER_API PlanOutput final {
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema;
  std::vector<TileGrid> tile_grids;
};

/** @brief Immutable validated physical task program.
 * Compiler and artifact loading are the publication boundaries. Copies share
 * the complete immutable program and static preparation owners; mutable Run
 * state and concrete devices belong to PreparedPlan/RunHandle. All accessors
 * borrow for this object's lifetime and support concurrent reads.
 */
class PHOTOSPIDER_API ExecutionPlan final {
 public:
  ExecutionPlan() noexcept = default;
  const physical::Program& program() const noexcept { return data().program_; }
  const std::vector<PlanCallBinding>& calls() const noexcept {
    return data().calls_;
  }
  const std::map<std::string, std::size_t>& outputs() const noexcept {
    return data().outputs_;
  }
  const std::vector<PlanOutput>& return_signature() const noexcept {
    return data().returns_;
  }
  const PlanOutput& output(const std::string& name) const {
    return data().returns_.at(data().outputs_.at(name));
  }
  const std::vector<WorkflowInputDeclaration>& input_declarations()
      const noexcept {
    return data().inputs_;
  }
  const std::vector<std::vector<TileGrid>>& input_tile_grids() const noexcept {
    return data().input_grids_;
  }
  const std::map<std::string, TileRequest>& output_requests() const noexcept {
    return data().requests_;
  }
  std::uint64_t tile_extent() const noexcept {
    return data().program_.tile_extent;
  }
  ExecutionMode execution_mode() const noexcept { return data().mode_; }
  std::uint64_t revision() const noexcept { return data().revision_; }
  const IRDigest& input_ir_digest() const noexcept { return data().input_ir_; }
  const ExecutionPlanDigest& digest() const noexcept { return data().digest_; }
  const ResourceSet& resources() const noexcept { return data().resources_; }
  bool current() const noexcept {
    return data().current_check_ && data().current_check_();
  }
  /** @brief Select existing output tiles without mapper/optimizer/preparation.
   * Rebuilds physical closure, adjacency, liveness and resource bounds while
   * retaining every call and mandatory effect. Unknown names or additional
   * coverage fail InvalidArgument; expired source fails Stale. May allocate.
   */
  Result<ExecutionPlan> select_outputs(
      const std::map<std::string, TileRequest>& requests) const;

 private:
  friend class Compiler;
  friend class PlanBuilder;
  friend class CompilerArtifact;
  friend class ExecutionContext;
  struct Storage final {
    physical::Program program_;
    IRProgram source_program_;
    std::vector<PlanCallBinding> calls_;
    std::vector<WorkflowInputDeclaration> inputs_;
    std::vector<std::vector<TileGrid>> input_grids_;
    std::vector<PlanOutput> returns_;
    std::map<std::string, std::size_t> outputs_;
    std::map<std::string, TileRequest> requests_;
    ResourceSet resources_;
    ExecutionMode mode_ = ExecutionMode::CpuExact;
    std::uint64_t revision_ = 0;
    IRDigest input_ir_;
    ExecutionPlanDigest digest_;
    physical::ValidationLimits limits_;
    std::function<bool()> current_check_;
    std::weak_ptr<OperationRegistry> operation_registry_;
  };
  const Storage& data() const noexcept;
  Storage& edit();
  std::shared_ptr<Storage> storage_;
};
}  // namespace ps
