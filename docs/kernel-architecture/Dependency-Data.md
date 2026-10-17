# Dependency data and execution

## Scope and ownership

The kernel records dependencies between static member tiles. `Footprint` remains an exact sample-geometry type for windows and views; `TileSet` stores normalized member-local half-open intervals for requests and evidence. Neither sample-role tags nor exact/conservative/unknown relation certificates are part of the execution protocol.

## Core data structures and memory

`Tensor` combines a descriptor, authorized Footprint and owned rectangular spans. It validates complete coverage, address mappings and overlaps; missing samples are errors. `restrict` narrows owners and authorization together. Explicit `collect` admits copying and checks work and cancellation. Retained bytes count distinct physical owners.

`TileUnit` identifies a producer output, member slot and tile index. `TileDependency` stores a producer and a compact set of member tiles; an empty set in a dependency record denotes descriptor evidence. This record representation must not be confused with an empty payload Need, which produces no evidence. `TileEvidence` freezes one shared commit's local Needs and inherited evidence. `TileRecord` stores an ordinary tile's local and inherited dependencies. See [Global Results](Global-Results.md#tile-records-and-frozen-evidence).

A running actor holds its **current** `ResultObjectInputs` map and Need-specific `ResultTensorInput` capabilities. These owning handles are distinct from the payload-free TileRecords and shared evidence used for dirty propagation. Checkpoint witnesses retain TileRecords, immutable stage-evidence ancestry and the admitted Needs, while completed-cache replay still checks its supplied-facts digest against each saved Need. Restoring a witness or cache proof never grants a source read: only a currently supplied Need and its owning window provide that authority.

Full member coverage stays compact through wait registration, dependency storage, reverse indexing and checkpoint evidence. Actual tasks expand in bounded batches. Shared evidence is reused as immutable objects; it is not copied for every consuming output tile. Geometry and metadata are Root-accounted; actual reads, comparisons and computation consume work as performed.

## Execution and state machine

A Run fixes its plan, bindings and generation. Ordinary tile actors and Run-shared stages are continuations. They issue nonblocking Needs and yield; worker callbacks never synchronously wait for upstream production. Stage declarations, schema, input projection and resource bounds are prepared before execution. One stage producer exists per original node and declared stage in that Run, with success or failure retained until Run end.

Ordinary execution advances mandatory stages even if plan output selections are empty. Explicit `DemandQuery` skips them only when all selections are empty. Shared wait registration and completion checks are coordinated to avoid lost wakeups, and recursive or mixed shared/tile dependency cycles fail. Cancellation stops new work, wakes waiters and drains admitted callbacks.

An ordinary tile publication must be complete and exactly cover its selected member tile, with other members empty. Shared stages can commit multiple complete tiles over multiple polls. Each commit atomically exposes payload, coverage and frozen evidence after validation; failure installs none of that transaction. Prepared publication scopes control bundle and prefix visibility independently of execution. Inherited evidence never grants upstream reads.

There are no automatic input-view preparation policies. Operations explicitly use `to_whole`, `to_tile`, owning windows and, when required, charged collection or transfer. Callback query and service tables are borrowed only until that callback returns. Owners explicitly retained by a window or operation state follow their own release lifetime.

## C staged-program contract

Result ABI 6 is the C operation interface. The importer requires exact table and record sizes, copies validated metadata and retains the module while callbacks or operation state exist. C services provide static schemas, member tile Needs, explicit descriptor-only requests, shared-stage Needs, owning window access and transactional publication. See [Plugin ABI](Plugin-ABI.md).

Service failures remain sticky even if ignored by a callback. Poll admission is nonblocking: concurrent or reentrant polls fail without entering the callback or altering the admitted invocation. Factories and polls are exception-fenced. `bad_alloc` maps to ResourceExhausted; other exceptions map to OperationFailed/HostException while an earlier service failure keeps precedence.

### C checkpoint services

Ordinary pure deterministic actors can use `checkpoint_before`, `checkpoint_read` and `checkpoint_publish`. Shared-stage phases expose no checkpoint or block callbacks. Once an ordinary actor handles a direct shared Need, it skips optional checkpoint/block reuse and completed-result caching. Eligibility additionally depends on upstream purity, fallback state, cache work and capacity; see [Global Results](Global-Results.md#checkpoints).

A checkpoint lookup returns the greatest sequence no greater than the bound, or handle zero on a miss. Its handle is borrowed for the current callback. State is a sealed ordinary Result whose schema has one fully covered, nonspatial, unbatched Tensor member and no publication scopes. `create_block_state` copies checked canonical bytes; the resulting operation-owned handle survives polls until release or state destruction. Publishing a checkpoint does not consume that handle.

`checkpoint_read` copies a positive in-bounds byte interval in logical order, including intervals crossing element boundaries. It checks work, capacity and cancellation and exposes no storage pointer. Restore stages dependency evidence and ancestry transactionally. The callback reacquires its current phase's input capabilities through newly admitted Needs. Optional capacity refusal becomes a miss without changing the actor. Successful restore does not populate current input capabilities or grant sample access; current Needs must be issued again.

## Exact demands and immutable binding replacement

`DemandQuery` maps names to `TileRequest`, which selects compact tile sets by member slot. `open_demand(plan, bindings)` captures the plan and immutable Results. Each request retains its generation and requested member tiles. `execute_fragments` uses an explicitly frozen bundle. Descriptor access does not require sample production.

Replacement validates all bindings and static schemas before publication. Failure retains the old generation. Racing publication or replacement returns Stale where specified; cancellation takes priority. A frozen bundle remains independent of later edits. `release` removes a subscription without cancelling active requests; `cancel` requests cancellation and clears retained publications and the bundle without waiting for active calls. Those calls retire cooperatively; context close waits for them. Context shutdown drains active demand calls before workers retire. A handle owns neither workers nor a separate payload cache.

Changed content is compared at observed member tiles under the plan's grid. Additional committed coverage, descriptor revision and producer progress alone do not imply content edits. A changed schema requires preparation and compilation again. Evidence restoration does not restore read authority.

## Result block state transitions

The block service consumes an explicit sealed single-Tensor state and returns that or a new state. A block callback neither issues Needs nor publishes operation output. The poll callback requests later inputs. Block retention is optional, requires a cacheable operation as well as purity, and may decline under capacity/work limits. It does not guarantee one producer within a Run; shared stages provide that guarantee.

## GPU fragment transport and execution

[Fragment Atlas](Fragment-Atlas.md) describes exact authorized transport and its separate physical directory geometry. [GPU Discovery](GPU-Discovery.md) describes bounded member-interval discovery. Native services require a device, obey synchronization and retain owners through completion. Service declarations and CPU mocks are not native execution evidence.

## Ready Atomic execution groups

The coordinator maintains Root-owned actors and pending work. It advances eligible Need cursors, submits at most `maximum_parallelism` callbacks in a wave and applies their results after retirement. CPU staged coordination and the GPU lane retain their separate callback roles. Submitted tasks keep their state and observations until the coordinator merges them. Shutdown drains all submitted tasks before borrowed state is released.

Committed tiles and retained read windows have independent backing owners. Shared tasks, wait records, terminal states and payload remain Root-accounted until their owners retire. Poll timing includes queue/wave waiting and is not compute-only timing.

### Cross-output pure block sharing

OperationTraits 28 retains optional `share_blocks_across_outputs` for pure deterministic operations. The block namespace binds prepared operation/output contracts, input metadata, parameters and resources. Each transition also binds its explicit range, mode and incoming state. Compatible completed transitions may be reused; active callbacks are not shared through this cache. Published output identities and checkpoint scopes remain separate.

## Limits and failure handling

Tile arithmetic, interval normalization, wait/evidence metadata, callback state, work and payload have explicit bounds. Exhaustion returns ResourceExhausted, cancellation returns Cancelled, and malformed or unauthorized requests fail validation. There is no dynamic End, implicit zero fill, hidden upstream fetch or automatic dense materialization.
