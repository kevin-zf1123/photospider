#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/data/result.hpp"

namespace ps::physical {
using Id = std::uint32_t;
inline constexpr Id none = std::numeric_limits<Id>::max();

/** @brief Fixed address space and execution class, resolved at prepare. */
enum class Domain : std::uint32_t { Host, Device, Shared };
enum class Backend : std::uint32_t { Cpu, Gpu };
enum class TaskKind : std::uint32_t { Kernel, Copy, Effect };
enum class EndKind : std::uint32_t { Jump, Branch, Return };
enum class ValueKind : std::uint32_t { Input, Argument, Produced, Imported };

/** @brief External allocation contract selected before lowering.
 * DirectTiles borrows distinct tile allocations. DenseHost borrows a dense
 * complete member range and materializes selected tiles through COPY. The
 * range may be embedded in a larger owner; bytes are relative to its bound
 * base.
 */
enum class InputLayout : std::uint32_t { DirectTiles, DenseHost };

/** @brief Immutable affine view. Strides and offset are in bytes.
 * rank is 1..8. Shape and origin describe logical coordinates; negative and
 * zero strides are legal for reads. Every address is checked before prepare.
 */
struct Layout final {
  ElementType element_type = ElementType::UInt8;
  std::uint32_t rank = 1;
  std::array<std::uint64_t, 8> shape = {1};
  std::array<std::uint64_t, 8> origin = {};
  std::array<std::int64_t, 8> strides = {1};
  std::uint64_t byte_offset = 0;
};

/** @brief One physical tile view, with one producer for materialized tiles.
 * Each produced/imported tile has a distinct allocation per block activation.
 * producer is none for an input/argument tile. DenseHost input views may share
 * external storage. A COPY creates a new allocation; fan-out keeps its
 * identity.
 */
struct TileDesc final {
  Id value = none, member = 0, layout = none, producer = none;
  Domain domain = Domain::Host;
  std::uint64_t tile_index = 0, bytes = 0, alignment = 1;
};

/** @brief One SSA Result and its ordered (member, tile_index) allocation IDs.
 * Inputs identify a declaration; arguments identify their block; produced
 * values identify their defining call. Private stage values use the same form.
 * Imported values identify their block and canonical external Input source.
 * Values may select zero tiles without changing their fixed schema.
 */
struct ValueDesc final {
  std::uint64_t source_value = 0;
  Id schema = none, owner = none;
  ValueKind kind = ValueKind::Produced;
  /** @brief Imported values belong to a block and copy this canonical Input.
   * All other kinds use none. Imports have source_value zero.
   */
  Id import_source = none;
  /** @brief Meaningful only for Input; other kinds use DirectTiles. */
  InputLayout input_layout = InputLayout::DirectTiles;
  std::vector<Id> tiles;
};

struct ReadDesc final {
  Id port = 0, tile = none, layout = none;
};

/** @brief Static placement; prepare binds concrete devices without replanning.
 * profile and queue identify the required arithmetic and queue capability.
 */
struct Placement final {
  Backend backend = Backend::Cpu;
  Domain domain = Domain::Host;
  std::string profile = "cpu";
  std::string queue;
};

/** @brief Symbolic kernel identity. No process pointers enter a physical plan.
 * The operation and symbol contracts are checked against the frozen registry.
 */
struct KernelDesc final {
  std::string operation, symbol, contract;
  std::uint32_t abi_version = 8;
};

/** @brief One fixed invocation, with all reads and writes known statically.
 * explicit_dependencies add ordering without a payload hold. predecessors
 * deduplicate those edges with local read producers; successors are the exact
 * transpose. Array position imposes no execution order. schema_reads name
 * values whose static metadata is required, without payload dependencies.
 * call may be none only for an external import COPY used at the block boundary.
 * Such copies issue after all real calls retire and produce no call record.
 * A GPU COPY declares Shared scratch of at least its output byte count times
 * its number of Host endpoints; each endpoint uses a separate packed segment.
 */
struct TaskDesc final {
  TaskKind kind = TaskKind::Kernel;
  Id call = none, kernel = none, placement = none;
  std::vector<ReadDesc> reads;
  std::vector<Id> writes, schema_reads;
  std::vector<std::uint8_t> parameters;
  std::vector<Id> explicit_dependencies, predecessors, successors;
  std::array<std::uint64_t, 3> scratch = {};
};

/** @brief One IR call, including calls elided by output selection.
 * ordinal is local to block and defines retirement order. returns selects
 * independently owning call exports; final RET owns its own value references.
 */
struct CallDesc final {
  std::uint64_t source_call = 0;
  Id block = none, ordinal = 0, effect_task = none;
  std::vector<Id> tasks, returns;
};

struct EdgeDesc final {
  Id target = none;
  std::vector<Id> arguments, live_values;
};

/** @brief Admission bound before a block issues any task.
 * outputs are the checked sum of separate allocation capacities by domain.
 * scratch stores each backend's maximum task demand by domain; runtime
 * multiplies it by granted lanes and separately admits its metadata records.
 */
struct Reservation final {
  std::array<std::uint64_t, 3> outputs = {};
  std::array<std::array<std::uint64_t, 3>, 2> scratch = {};
};

struct BlockDesc final {
  std::uint64_t source_block = 0;
  std::vector<Id> tasks, calls, arguments, live_ins;
  EndKind end = EndKind::Return;
  Id condition = none;
  std::vector<EdgeDesc> edges;
  std::vector<Id> return_values;
  Reservation reservation;
};

/** @brief Editable physical tables used privately by builders and decoders.
 * Index zero is valid; none is reserved. Published plans own an immutable
 * instance. Schemas are structural metadata and never hold runtime payload.
 * Concurrent reads are safe after validation; mutation requires exclusivity.
 */
struct Program final {
  Id entry = 0;
  std::uint64_t tile_extent = 128;
  std::vector<std::shared_ptr<const SchemaTemplate>> schemas;
  std::vector<Layout> layouts;
  std::vector<TileDesc> tiles;
  std::vector<ValueDesc> values;
  std::vector<Placement> placements;
  std::vector<KernelDesc> kernels;
  std::vector<TaskDesc> tasks;
  std::vector<CallDesc> calls;
  std::vector<BlockDesc> blocks;
  std::vector<Id> inputs, return_schemas;
};

struct ValidationLimits final {
  std::uint64_t maximum_records = 4194304;
  std::uint64_t maximum_edges = 16777216;
  std::uint64_t maximum_work = 268435456;
};

/** @brief Check affine reachability without sample access or allocation.
 * Returns InvalidArgument for malformed shape/type/extent or address overflow.
 * Writable views must be dense, positive-stride row-major tile allocations.
 */
PHOTOSPIDER_API Status validate_layout(const Layout& layout,
                                       std::uint64_t bytes, bool writable);

/** @brief Build adjacency, liveness and resource bounds from fixed tables.
 * Compiler-only normalization; does not call a mapper, kernel or backend.
 * On failure the editable program may be partially normalized and must not
 * be published. Allocation failure is ResourceExhausted.
 */
PHOTOSPIDER_API Status finalize_program(Program* program,
                                        const ValidationLimits& limits = {});

/** @brief Independently validate saved tables before immutable publication.
 * Checks coverage, membership, CFG/dominance, dependencies, terminal effects,
 * affine bounds and reservations. Does not repair imported data or invoke
 * operation code. Invalid structure is InvalidArgument; bounded-work or
 * allocation exhaustion is ResourceExhausted. Numerical samples are unchecked.
 */
PHOTOSPIDER_API Status validate_program(const Program& program,
                                        const ValidationLimits& limits = {});
}  // namespace ps::physical
