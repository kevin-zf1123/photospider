# Dependency data and execution

## Scope and ownership

The kernel records dependencies between static member tiles. The planner owns dependency discovery: it calls each operation's static mapper at compile time and stores the result as fixed read grants and block-local instruction dependencies in the `ExecutionPlan`. The Run owns the evidence it records while executing those instructions. `Footprint` remains an exact sample-geometry type for windows and views; `TileSet` stores normalized member-local half-open intervals for grants and evidence. Neither sample-role tags nor exact/conservative/unknown relation certificates are part of the execution protocol.

## Core data structures and memory

`Tensor` combines a descriptor, authorized Footprint and owned rectangular spans. It validates complete coverage, address mappings and overlaps; missing samples are errors. `restrict` narrows owners and authorization together. Explicit `collect` admits copying and checks work and cancellation. Retained bytes count distinct physical owners.

`TileUnit` identifies a producer record, output ordinal, member slot and tile index. `TileDependency` stores a producer and a compact set of member tiles; an empty set in a dependency record denotes descriptor evidence. This record representation must not be confused with an empty payload grant, which produces no evidence. `TileRecord` stores the dependencies of one executed tile. `TileEvidence` freezes one shared-stage commit's inputs and inherited evidence. See [Global Results](Global-Results.md#tile-records-and-frozen-evidence).

Record indexes `0 .. steps().size() - 1` name call outputs. Each named return also has a synthetic record whose index is `steps().size()` plus the return ordinal, with output ordinal 0; `ExecutionPlan::output(name).record_index` returns it. The synthetic record identifies the named return independently of the branch that produced its value, so a consumer compares one stable record across runs that took different paths.

## Static mapping contract

```cpp
namespace ps {
struct ResultStaticQuery {           // borrowed for one mapper call
  const ResultProgramMetadata& metadata;   // input/output schemas and grids
  const std::map<std::string, ParameterValue>& parameters;
  const PreparedOperation& prepared;       // traits and resource identities
  Backend backend;
  std::uint64_t tile_extent;
  std::uint32_t output_index;
  std::uint32_t shared_stage;              // UINT32_MAX for an output tile
  std::uint32_t tensor_slot;
  TileIndex tile_unit;
  TileRequest publication;                 // complete publication target
  std::optional<Footprint> tensor_outputs; // output samples of this tile
};
struct ResultStaticMapping {
  std::vector<TileNeed> inputs;            // {input, tensor_slot, descriptor_only, tiles}
  std::vector<ResultSharedNeed> shared_stages;  // {stage, optional tiles}
};
using OperationDependencyMapper =
    std::function<Result<ResultStaticMapping>(const ResultStaticQuery&)>;
Result<ResultStaticMapping> complete_input_dependencies(const ResultStaticQuery&);
}  // namespace ps
```

`OperationDefinition::map_dependencies` defaults to `complete_input_dependencies`, which grants every member tile of every input in the selected output's or stage's input projection. An operation that reads less supplies its own mapper; `ps::sdk::pointwise_dependencies` and `ps::sdk::tile_need_for_samples` cover the common pointwise and sample-region cases. A mapper must be deterministic, may not perform I/O or start continuations, and may run concurrently; the registry fences exceptions and floating environment changes. A C++ definition whose mapper was explicitly set to an empty `std::function` is not a valid mapper: planning fails with `InvalidArgument` when it reaches that operation. An empty dependency declaration is an explicit, successful empty `ResultStaticMapping`.

The input projection only limits which ports a mapper may name, and a shared-stage declaration only limits which stages exist. Neither one creates a read edge. A consumer of a shared stage must return that stage from its mapper, and an operation that reads nothing returns an empty mapping. The planner rejects reads outside the projection or naming a missing stage with `InvalidArgument`, merges overlapping entries, drops empty payload entries, extends reads to their publication and validation closures, and records each instruction's producer tiles as dependencies.

Grants must be finite and fixed. A data-dependent address pattern declares a static superset, for example the union of every address a bounded-offset gather can reach; at run time the CPU or GPU code chooses reads inside that superset. Selecting between whole computations at run time requires an explicit IR branch.

## Execution and state machine

A Run fixes its plan, bindings and generation and walks the plan's blocks in order; see [Compiler and local execution](Compiler-and-Execution.md#static-vm-execution). Before the first poll of an instruction, the VM binds every fixed grant: payload grants become `ResultTensorInput` capabilities whose samples must already be certified, and descriptor-only grants become `ResultDescriptorInputs` entries. A descriptor-only grant supplies only compile-time schema facts with object id 0 and exposes no payload through tensors, read windows or native atlases. Shared-stage grants become stage Result projections restricted to the granted tiles.

A poll that returns `ResultProgramNeed` asserts that its requests lie inside those grants. The VM charges one unit of Run work per request entry, rejects anything outside the grants with a protocol `InvalidArgument`, and polls again. The assertion never computes, waits, narrows a grant, clears a capability, or upgrades descriptor-only access to payload. C `need_tiles`, `need_shared_stage` and `need_shared_tiles` behave the same way.

Shared stages run as ordinary instructions. One shared-stage state exists per call and stage within one block activation; block entry clears it, so a loop iteration starts its shared stages again. A shared stage can commit multiple complete tiles over multiple polls, and each commit atomically exposes payload, coverage and frozen evidence after validation; failure installs none of that transaction. An ordinary tile publication must be complete and exactly cover its own member tile, with other members empty. Prepared publication scopes control bundle and prefix visibility independently of execution. Inherited evidence never grants upstream reads.

Each executed instruction records a `TileRecord` that contains the control evidence of the branch conditions taken so far in the activation, the records of the producer tiles it read, external input tiles, and the evidence carried by block parameters. When the same external source reaches a tile through several internal routes, the record merges those dependencies into one entry. Each Run rebuilds this evidence from the fixed grants; it is never copied from an earlier Run or from a cache witness.

There are no automatic input-view preparation policies. Operations explicitly use `to_whole`, `to_tile`, owning windows and, when required, charged collection or transfer. Because grants are bound before the first poll, `to_whole` can return its ready alternative on the first call. Callback query and service tables are borrowed only until that callback returns. Owners explicitly retained by a window or operation state follow their own release lifetime.

## C staged-program contract

Result ABI 7 is the C operation interface. The importer requires exact table and record sizes, copies validated metadata and retains the module while callbacks or operation state exist. The optional `map_dependencies` hook receives a `ps_result_static_query_v7` and a synchronous `ps_result_static_mapping_sink_v7`; a NULL hook selects the complete projected inputs. Run-time C services provide static schemas, grant assertions, owning window access and transactional publication. See [Plugin ABI](Plugin-ABI.md).

Service failures remain sticky even if ignored by a callback. Poll admission is nonblocking: concurrent or reentrant polls fail without entering the callback or altering the admitted invocation. Factories, polls and mappers are exception-fenced. `bad_alloc` maps to ResourceExhausted; other exceptions map to OperationFailed/HostException while an earlier service failure keeps precedence.

### C checkpoint services

Ordinary pure deterministic tile instructions can use `checkpoint_before`, `checkpoint_read` and `checkpoint_publish`. Shared-stage instructions expose no checkpoint or block callbacks, and an instruction with shared-stage reads is not pure for optional reuse. Eligibility additionally depends on upstream purity, fallback state, cache work and capacity; see [Global Results](Global-Results.md#checkpoints).

A checkpoint lookup returns the greatest sequence no greater than the bound, or handle zero on a miss. Its handle is borrowed for the current callback. State is a sealed ordinary Result whose schema has one fully covered, nonspatial, unbatched Tensor member and no publication scopes. `create_block_state` copies checked canonical bytes; the resulting operation-owned handle survives polls until release or state destruction. Publishing a checkpoint does not consume that handle.

`checkpoint_read` copies a positive in-bounds byte interval in logical order, including intervals crossing element boundaries. It checks work, capacity and cancellation and exposes no storage pointer. The checkpoint scope is a content identity, so equivalent calls at different physical steps share it. Restoring a checkpoint restores state bytes only: the current instruction keeps its own fixed grants and evidence, and no witness replaces them or grants a source read. Optional capacity refusal becomes a miss without changing the instruction.

## Exact demands and immutable binding replacement

`DemandQuery` maps names to `TileRequest`, which selects compact tile sets by member slot. `open_demand(plan, bindings)` captures the plan and immutable Results. Each request retains its generation and requested member tiles. `execute_fragments(plan, bindings, query)` accepts per-call bindings and filters the current plan with `select_outputs` before execution; the request must be a subset of the plan's compiled output requests. Descriptor access does not require sample production.

Replacement validates all bindings and static schemas before publication. Failure retains the old generation. Racing publication or replacement returns Stale where specified; cancellation takes priority. Graph replacement or destruction makes the retained borrowed-graph plan stale. Active requests also become stale when bindings advance to another generation. `release` removes a subscription without cancelling active requests; `cancel` requests cancellation and clears retained publications and the bundle without waiting for active calls. Those calls retire cooperatively; context close waits for them. Context shutdown drains active demand calls before workers retire. A handle owns neither workers nor a separate payload cache.

Changed content is compared at observed member tiles under the plan's grid. Additional committed coverage, descriptor revision and producer progress alone do not imply content edits. A changed schema requires preparation and compilation again. Evidence restoration does not restore read authority.

## Result block state transitions

The block service consumes an explicit sealed single-Tensor state and returns that or a new state. A block callback neither asserts grants nor publishes operation output. Block reuse requires a pure deterministic tile instruction; it may decline under capacity or work limits and does not replace shared stages as the way to compute one value once per activation.

### Cross-output pure block sharing

OperationTraits 29 retains optional `share_blocks_across_outputs` for pure deterministic operations. The block namespace binds prepared operation/output contracts, input metadata, parameters and resources. Each transition also binds its explicit range, mode and incoming state. Compatible completed transitions may be reused; active callbacks are not shared through this cache. Published output identities and checkpoint scopes remain separate.

## GPU fragment transport and execution

[Fragment Atlas](Fragment-Atlas.md) describes exact authorized transport and its separate physical directory geometry. [GPU Discovery](GPU-Discovery.md) describes bounded selection inside compiled grants. Native services require a device, obey synchronization and retain owners through completion. Service declarations and CPU mocks are not native execution evidence.

## Instruction execution and retirement

The coordinator executes one instruction at a time. It submits the instruction's start and poll callbacks to the CPU pool or GPU lane, waits for each to retire, and applies its result before the next instruction; a `cpu_staged_tiles` poll runs on the coordinator with the CPU tile service. Submitted callbacks keep their state and observations until the coordinator merges them. Cancellation or shutdown drains submitted callbacks before borrowed state is released.

Committed tiles and retained read windows have independent backing owners. Shared-stage state, records and payload remain Root-accounted until their owners retire. Poll timing includes queue waiting and is not compute-only timing.

## Limits and failure handling

Tile arithmetic, interval normalization, grant and evidence metadata, callback state, work and payload have explicit bounds. The planner applies `maximum_instructions` and `maximum_dependency_edges`; the VM applies `maximum_dependency_work`, per-instruction poll limits and grant-assertion limits. Exhaustion returns ResourceExhausted, cancellation returns Cancelled, and malformed or unauthorized requests fail validation. There is no dynamic End, implicit zero fill, hidden upstream fetch or automatic dense materialization.
