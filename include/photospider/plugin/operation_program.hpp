#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "photospider/compiler/physical_program.hpp"
#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps {
/** @brief Static input to a pure bounded operation lowering callback.
 * Output requests are normalized member tiles. No payload or runtime device is
 * available. static_state borrows immutable operation preparation. A callback
 * may emit multiple tasks and private SSA values, but cannot invent IR calls.
 */
struct OperationLowering final {
  std::string operation;
  std::vector<OperationMetadata> inputs, outputs;
  std::map<std::string, ParameterValue> parameters;
  std::vector<TileRequest> requests;
  std::uint64_t tile_extent = 128;
  /** @brief Requested static lowering strategy. CPU emits only CPU tasks;
   * GPU may include CPU tasks when traits declare CPU support. Each non-COPY
   * task must use a supported backend. Execution never changes placement.
   */
  physical::Backend backend = physical::Backend::Cpu;
  const void* static_state = nullptr;
  physical::ValidationLimits limits;
};

/** @brief Payload-free description of one fixed input/output allocation view.
 * port is the original operation port, or none for a private stage output.
 * bytes bounds the compiled tile. A Device input may bind inside a larger
 * retained allocation; its invocation offset includes the checked base.
 */
struct KernelView final {
  physical::Id port = physical::none, member = 0;
  TileIndex tile_index = 0;
  physical::Domain domain = physical::Domain::Host;
  std::uint64_t bytes = 0;
  physical::Layout layout;
};
/** @brief Complete fixed invocation metadata available only during prepare.
 * The callback checks its symbol's arity, layouts, ports, encoded parameters
 * and scratch requirements here before returning an unchecked entry point.
 * All fields are borrowed for this callback. Retained state must own copies.
 * Copying this query preserves its references; it does not extend their life.
 */
struct KernelPreparation final {
  KernelPreparation(
      const std::string& operation_name,
      const std::vector<OperationMetadata>& input_metadata,
      const std::map<std::string, ParameterValue>& arguments,
      const std::vector<std::uint8_t>& encoded_parameters) noexcept
      : operation(operation_name),
        inputs(input_metadata),
        parameters(arguments),
        task_parameters(encoded_parameters) {}
  const std::string& operation;
  const std::vector<OperationMetadata>& inputs;
  std::vector<OperationMetadata> outputs;
  const std::map<std::string, ParameterValue>& parameters;
  std::uint64_t tile_extent = 128;
  const void* static_state = nullptr;
  std::vector<KernelView> reads, writes;
  const std::vector<std::uint8_t>& task_parameters;
  std::array<std::uint64_t, 3> scratch = {};
};

/** @brief One call's static subgraph, including private intermediate tiles.
 * program contains exactly one block and call. Inputs are declaration-order
 * values and outputs indexes the Result values in operation output order.
 * The compiler merges tables into the owning IR call, preserving task edges.
 */
struct OperationProgram final {
  physical::Program program;
  std::vector<physical::Id> outputs;
};
using OperationLowerer = std::function<Result<OperationProgram>(
    const OperationLowering&)>;  // NOLINT(whitespace/indent_namespace)

/** @brief Checked construction helpers for one fixed operation subgraph.
 * All mutation is callback-local. Task IDs and tile IDs refer to the returned
 * program, not global Plan tables. Allocation failures may throw. finish
 * checks topology, requested output coverage and membership before transfer.
 * Aggregate record and constructed-reference limits are enforced before each
 * append; shared tiles are counted once and repeated reads separately. Derived
 * graph edges are checked at finish. A successful finish consumes the builder;
 * subsequent mutations fail.
 */
class PHOTOSPIDER_API OperationProgramBuilder final {
 public:
  explicit OperationProgramBuilder(const OperationLowering& query);
  OperationProgramBuilder(const OperationProgramBuilder&) = delete;
  OperationProgramBuilder& operator=(const OperationProgramBuilder&) = delete;
  /** @brief Add one direct kernel or terminal effect. Scratch is by domain. */
  Result<physical::Id> add_task(const std::string& symbol,
                                const std::string& contract,
                                physical::Placement placement = {},
                                std::array<std::uint64_t, 3> scratch = {},
                                bool effect = false);
  /** @brief Add one explicit transfer with no operation kernel or parameters.
   * Declare exactly one read and one write with the same logical tile layout.
   * COPY chooses its own backend independently of the call's numerical kernel.
   * The destination domain comes from placement; a Device endpoint requires
   * a GPU placement. Compilation validates the complete transfer at finish.
   * finish declares one packed Shared scratch segment per Host endpoint of a
   * GPU COPY. This staging remains part of the serialized task contract.
   */
  Result<physical::Id> add_copy(physical::Placement placement = {});
  /** @brief Add a private SSA Result. Its tiles are produced by this call. */
  Result<physical::Id> add_temporary(
      std::shared_ptr<const SchemaTemplate> schema);
  /** @brief Declare a payload read from an original input's fixed member tile.
   * Repeated reads share one incoming tile identity; port entries remain
   * distinct for invocation ordering. Domain must match the prepared input.
   */
  Status read_input(physical::Id task, std::uint32_t port, std::uint32_t member,
                    TileIndex tile,
                    physical::Domain domain = physical::Domain::Host);
  /** @brief Reference an original input's static schema without payload reads.
   * Adds no incoming tile or producer dependency. Repeated declarations are
   * idempotent. Invalid task/port or a consumed builder fails InvalidArgument.
   */
  Status read_input_schema(physical::Id task, std::uint32_t port);
  /** @brief Declare one exclusive output tile. Repeated writers are rejected.
   */
  Status write_output(physical::Id task, std::uint32_t output,
                      std::uint32_t member, TileIndex tile);
  Result<physical::Id> write_temporary(physical::Id task, physical::Id value,
                                       std::uint32_t member, TileIndex tile);
  /** @brief Read a private tile through its complete immutable view. */
  Status read_tile(physical::Id task, physical::Id tile,
                   std::uint32_t port = 0);
  Status depends_on(physical::Id task, physical::Id predecessor);
  /** @brief Editable task-local parameter encoding, consumed through
   * completion. Throws std::out_of_range for an invalid task.
   */
  std::vector<std::uint8_t>& parameters(physical::Id task);
  Result<OperationProgram> finish();

 private:
  bool admit(std::uint64_t records, std::uint64_t references);
  std::uint64_t references_ = 0;
  Result<physical::Id> tile(physical::Id value, std::uint32_t member,
                            TileIndex index, physical::Domain domain,
                            physical::Id producer);
  const OperationLowering& query_;
  OperationProgram result_;
  std::vector<std::vector<TileGrid>> grids_;
  std::map<std::tuple<physical::Id, std::uint32_t, TileIndex>, physical::Id>
      tiles_;
  Status status_;
};
}  // namespace ps
