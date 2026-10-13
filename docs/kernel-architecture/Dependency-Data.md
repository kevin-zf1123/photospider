# Dependency data, execution and ownership

## Scope and ownership

```text
requested Result roots
        |
        v
Root-owned Actor registry -> ready frontier -> context worker queues
        ^                                     |
        |           Need or publication       v
        +------ coordinator satisfies Needs < callback
                       |
                 Root admission
```

The coordinator advances dependency-ready Result Actors without occupying a worker while it waits for an upstream producer. Ready callbacks compete for the configured worker capacity and Root resource limits.

Each callback borrows its phase and service references until return. The Run retains continuation state and admitted output/scratch leases; `ValueFragments` retains its supplied Value storage owners independently.

```cpp
class Footprint final {
 public:
  static Result<Footprint> from_regions(
      std::vector<std::uint64_t> shape, const std::vector<Region>& boxes,
      const FootprintLimits& limits = {},
      std::uint64_t* consumed_work = nullptr);
};

class ValueFragments final {
 public:
  static Result<ValueFragments> create(
      ValueDescriptor descriptor, std::vector<ValueFacet> facets,
      Footprint authorized, const std::vector<Value>& fragments,
      const FootprintLimits& limits = {}, ResourceBindings resources = {});
  Status read(const std::vector<std::uint64_t>& coordinate, void* destination,
              std::size_t size) const;
};

```

## Core data structures and memory

`Footprint` is an exact set in a nonzero rank-1..8 logical domain. Its canonical disjoint rectangles represent Empty, All and non-contiguous coverage. Recursive axis sweeps merge adjacent intervals only when their suffix sets are identical, so construction order, duplicate boxes and alternative tilings do not affect set equality. All remains compressed even when the dense element product would overflow. Union, intersection and difference require the same domain and retain holes. `tile_cover(geometry)` returns exactly the touched tile coordinates in a ceil-divided domain; it does not mark every sample inside those tiles valid. `visit` emits each sample once in row-major order under an explicit sample limit.

`FootprintLimits` bounds each operation's candidates and box counts, including duplicate construction work. Exceeding limits returns ResourceExhausted; cancellation returns Cancelled. Neither outcome is Empty or a bounding-box approximation. Compound users must additionally bound their total operation and metadata work across calls; a per-set limit is not a whole execution budget.

`ValueFragments` pairs complete descriptor/facets with an authorized Footprint and retained rectangular Values. Construction clips supplied coverage, rejects missing authorized samples and rejects inconsistent overlapping owners/mappings. Equivalent origin-relative mappings on the same owner deduplicate. Validated ColorArray tuples require complete channel coverage. Structural image values are rejected from this Value container; tensor data is carried by a typed Result tensor slot and read through `ResultTensorInput`. `read` uses Value's checked signed-stride addressing and copies the actual dtype width. A hole, unauthorized address or wrong width is an error; there is no synchronous fetch, implicit extension or zero-fill fallback. `restrict` restricts owners and coverage together. `collect` requires a complete rectangle before allocating through the supplied BufferAllocator. Retained capacity counts unique actual storage owners, independently of valid coverage.

## Execution and state machine

`OperationTraits` uses semantic version 25 and Result protocol 2. Result traits describe failure delivery, continuation/workspace bounds, and typed output ports. Staged Result programs require deterministic, side-effect-free behavior except for the declared Whole RequestFailureOnly case. Compilation checks executable edges reachable from requested outputs and side-effect roots. Excluded ports retain static metadata without running a producer. Result plans retain unresolved input demands rather than inventing a rectangular approximation. Ordinary execution runs every non-side-effect-free operation as a mandatory root, including operations without requested named outputs. Fragment execution do so only when the query contains a nonempty demand; a completely empty query skips unrelated effect roots. Within a Run, an effectful tensor output uses slot zero and full coverage for its internal actor identity, so later requests reuse that actor. This does not widen authorization for other slots: each slot still requires its own Need and valid publication coverage. Effectful work is not shared or cached across Runs. A mandatory root failure or cancellation fails the enclosing Run.

Each `ps_result_output_v3` also carries an immutable view policy and optional output payload bound. `PRESERVE_VIEWS` is valid for a CPU Atomic output; `REQUIRE_INPUT_VIEWS` implies it and requires CPU Whole execution. In compiled structured CPU Whole execution, the coordinator validates a payload Tensor Need, prepares its authorized boxes before the computation poll, and either proves affine views or applies Auto collection. A direct registry start does not synthesize this Need preparation. `PAYLOAD_BOUND` enables `maximum_output_payload_bytes`, where zero is an explicit limit; with the flag clear the byte field must be zero. The bound is supported for CPU Whole or staged output and is checked at publication. Root accounting for input backing, workspace, metadata and work remains separate. The importer rejects unknown or inconsistent flags, and the resolved `query.output` exposes the registered policy to both `start` and `poll`; metadata resolution can refine the output port/schema but cannot change the policy. These tail fields change the nested output-record layout while the C ABI remains version 3. Exact `struct_size` validation rejects old prefix records, so C modules must rebuild; there is no compatibility shim.

Result discovery, poll counts, continuation state, output/scratch and GPU tables have finite limits. Reads, explicit work charges and allocation failures are sticky even if a callback ignores their returned Status. Failure observers cannot prevent another observer from receiving the error. The first recorded failure remains the reported cause, while the host separately vetoes CPU fallback after any nonretryable typed failure; only an unqualified `BackendUnavailable` with permitted origin and scope can remain retryable. Accepted-call failures retire continuation state once and release input owners before cancellation is applied. Poll admission is nonblocking: overlapping or reentrant calls return `InvalidArgument/Protocol/Group` without entering the callback, notifying failure observers, recording a failure latch, or modifying the active phase, first failure, or work count. Borrowed phase/query/service references expire on callback return, concurrent move or destruction is forbidden, and continuations must use their granted allocators. `start_result` checks cancellation around its factory; cancellation observed after factory entry takes precedence over factory, allocation, and returned-state validation errors, and any returned state is destroyed before return while the definition lease remains alive.

Structured Result execution uses a Root-owned Actor registry and pending-task list. The coordinator submits ready callbacks through the existing CPU worker queue or selected native GPU queue. A worker does not wait for an upstream Result producer. Each stage uses nonblocking admission against the shared Root budget; sealing releases unused capacity while retained state, input and output leases remain charged. A request that cannot fit its minimum working set fails with `ResourceExhausted`. Result callbacks use `ResultTensorInput` for authorized reads. An allocation may escape a Result callback only after the host imports it through the controlled allocator.

Frozen execution retains its captured graph and input owners. Root Actors are submitted in bounded callback waves and the coordinator applies each wave after its submitted callbacks retire. See [Cache Model](Cache-Model.md) for active demand reuse and completed-result retention. C++ staged GPU fragment access uses the [Fragment Atlas](Fragment-Atlas.md), and bounded native discovery is described in [GPU Discovery](GPU-Discovery.md).

## C staged-program contract

The Result ABI 3 C operation table is the only C staged-operation interface. It exposes typed Result ports, callback-scoped services, staged Needs and Result publication. The loader validates and copies its bounded table before atomically publishing definitions, and retains the module owner while imported callbacks can run. The C++ API uses the same staged Result model through `ResultContinuation`; Result ABI 3 remains the sole C operation-plugin interface.

The phase services expose checked sample reads with actual dtype, origin, signed strides, byte span and authorized region. Missing samples never trigger hidden upstream reads. Invalid handles and ignored service failures are sticky.

| Resource | Validity and ownership |
| --- | --- |
| Scratch pointer | Valid through the current poll callback; expires when poll returns. |
| Output pointer | Valid until successful publication, or until poll returns if unpublished; publication revokes direct pointer access immediately. |

`tests/integration/kernel/test_result_c_block.cpp` covers the C block-state handle lifecycle and ordered Float64 fixture scan. Its finite-input and finite-sum checks belong to that fixture; the generic Float64 block service transports canonical bytes. The test also checks failure cleanup and Root ownership. Block-cache reuse and hit counts are implementation details, not a performance contract.

### C checkpoint services

The C Result services `checkpoint_before`, `checkpoint_read`, and `checkpoint_publish` expose the Result checkpoint path for pure staged operations. A lookup returns the greatest stored sequence no greater than the requested bound. A miss succeeds with handle zero; a hit returns an opaque handle, sequence, and canonical tensor byte size. The opaque handle is usable only during the current start or poll callback; its callback-epoch entry retains the state and witness.

Checkpoint state is a sealed generic `CompleteBundle` Result with one fully covered tensor and no fields, metadata, or domain; its tensor has no facets, groups, batch axes, or spatial layout. `create_block_state` copies a byte payload into that Result and checks the payload size against the tensor's logical samples. The caller can use the resulting operation-owned handle as block state or pass it to `checkpoint_publish`. Block-state handles remain valid across polls until released or the callback state is destroyed. When optional retention is admitted, publishing retains the Result and successful dependency witness. The call does not consume the handle; the caller must still release it with `release_block_state`.

`checkpoint_read` copies a positive in-bounds byte interval in canonical row-major logical sample order. The interval may start or end inside an element and may cross sample boundaries. The service exposes no pointer to retained storage, charges read work, and checks Root capacity and cancellation. A restored checkpoint reimports its successful dependency history, facts, ancestry and association; it does not restore Result input capabilities or authorize new input reads. The callback must request current inputs separately. Checkpoint APIs require a nonzero phase and a deterministic, side-effect-free operation. Block callbacks and GPU discovery cannot use them. Invalid handles, intervals, scope or prohibited use become sticky failures.

`test_result_c_checkpoint.cpp` also checks a finite-prefix fixture: a valid prefix is published, while later requests containing infinity, NaN, or an overflowing sum fail. Finiteness is enforced by this fixture's scan logic; the generic Float64 checkpoint service copies canonical bytes and does not reject non-finite values by itself.

Result C modules and C++ Result continuations use the structured input and publication model. Native plugins remain trusted in-process code; pointer metadata checks do not provide memory isolation. C++ Result continuations and Result C modules use the structured input and publication model.

`test_result_plugin.cpp` exercises Result ABI 3 C module import, callback execution, sticky service failures and plugin-owner lifetime. Its `c_whole_view_policies()` case checks that the selected output flags and payload bound are visible at both C callback stages, then covers Auto collection, strict view rejection, compatible/signed/zero-stride backing, output-cap enforcement and owner retirement. The fixture also registers malformed modules for unknown or inconsistent policy flags, bound-presence mismatches, unsupported view combinations, and a truncated output record. The Result checkpoint and block fixtures exercise their scoped services.

## Exact demands and immutable binding replacement

`ExecutionContext::open_demand(plan, bindings)` pins the current graph contract and immutable Result bindings. A `DemandHandle` shares that bundle and generation across copies. `request({name: Footprint})` returns named ResultRefs, diagnostics and structural evidence. Every request keeps its original Q. Result continuations receive selected tensor slots through typed Needs. Unrequested names are absent. `execute_fragments(frozen, Q)` provides the same exact query for an explicitly pinned bundle, with result generation zero.

Replacement validates all bindings before publication. Sample, metadata or validation failures retain the old generation. A racing request publication or replacement returns Stale for retry. Latest requests captured before a completed replacement cannot publish into the new generation; cancellation takes priority over Stale. `freeze()` pins the current immutable bundle independently of future edits. `release(Q)` drops one exact subscription without cancelling active requests, which may subsequently republish it; `cancel()` stops that handle and retires its publications and bundle. Context destruction cancels and drains active demand calls before retiring its existing workers. Direct context calls must not race context destruction, while existing handle calls may do so. Input owner destructors run outside the publication mutex.

`maximum_demands` bounds live uncancelled handles (1..65536, default 1024), and active demand calls are bounded by the existing queue plus CPU worker capacity and one coordinator slot. `DemandConfig::maximum_metadata_entries` bounds each handle's retained query/evidence/dirty metadata (1..1048576, default 65536). A handle owns no workers or pixel cache. Structured Result producers use the existing CPU/GPU pools, WaitingAdmission and accounted allocator. Completed-result content reuse uses the optional pixel LRU with bounded structural proofs, as described in [Cache Model](Cache-Model.md). See [Fragment Atlas](Fragment-Atlas.md) for exact GPU input transport and CPU fallback.

`ExecutionContextConfig::maximum_result_checkpoint_scopes` bounds only the Result checkpoint-scope directory (1..1,048,576, default 65,536).

Context shutdown drains demand calls and submitted callbacks before retiring workers.

## Result block state transitions

`ResultProgramPhase::block` and the C block service operate on an explicit sealed `CompleteBundle` tensor state. A block callback reads that state and returns it or a newly created state; it does not issue Needs or publish the operation output. The producer requests any later tensor samples from its poll callback. Checkpoint and block services remain separate: checkpoint restoration can import successful dependency evidence, while block state carries only transition data. Optional block-state retention may skip a transition or recompute it under resource limits. Its hit counts and transition reuse are implementation details, not performance promises.

[`tests/integration/kernel/test_result_c_block.cpp`](../../tests/integration/kernel/test_result_c_block.cpp) covers the C block-state handle and Float64 fixture scan, including state lifetime and failure cleanup. Finite-input and finite-sum rules belong to that fixture; the generic state service transports tensor bytes.

## GPU fragment transport and execution

[Fragment Atlas](Fragment-Atlas.md) documents the implemented exact atlas/mask directory and SDK MSL lookup helper, including native transport verification. C++ staged GPU execution is integrated. Result C ABI staged GPU execution is also integrated; bounded discovery is implemented in [GPU Discovery](GPU-Discovery.md).

`BufferAllocator::same_owner` compares only accounting-domain identity, not quotas or provenance, and grants no payload access. The callback exception fence preserves the first shared allocation, work, or I/O failure. C++ Result factories and polls map `std::bad_alloc` to `ResourceExhausted`, standard exceptions to `OperationFailed` with `HostException` and their diagnostic (empty when `what()` is null), and non-standard exceptions to `OperationFailed` with a fixed `HostException` diagnostic. A recorded phase failure takes precedence over a subsequent throw, and an exception from the host failure observer cannot replace the producer's selected failure.

## Ready Atomic execution groups

The coordinator registers explicitly requested named Result roots before advancing Actors. The Root-owned pending-task list retains each queued or submitted Actor until dispatch rejection or callback retirement. Once the dependency frontier has queued work, the coordinator submits at most `maximum_parallelism` callbacks, waits for the submitted wave to retire, then applies phase results serially before advancing the next frontier. CPU staged-tile coordination and GPU dispatch keep their existing paths.

Before a producer serves a Need, the coordinator validates the complete Need envelope. Each input keeps an independent supply cursor, and the coordinator rotates among inputs that can progress. It visits each Actor at most once per traversal, avoiding repeated recursive walks through deep chains as peers wait on upstream producers. Stage-local transfer, tile and native observations remain attached to their submitted task and merge when the driver retires it. Checkpoint and block-cache metadata locks protect bounded lookup or commit sections, not their computation or test hooks.

Shutdown and synchronous Run completion drain every submitted task before borrowed callback state is released.

Whole-result release remains conservative. Only a completed Whole Actor whose every tensor slot has full coverage can mark its aliased steps finished. Liveness counts only steps reachable from requested roots and follows each step's selected `input_indices`. The coordinator clears the published object only after all aliases have no remaining consumers or named-output pin and no pending work. A Dynamic Dependency, Prefix, Empty publication cannot mark a step complete from one partial publication. Published Results and retained read windows keep their own storage owners independently. Poll timing spans phase creation through driver retirement, including callback queue and wave wait; it is not a compute-only measure.

`test_result_dag_parallel` exercises the callback wave limit and cancellation, keeps source backing alive while the independent left/right consumers read it, then checks that the source owner expires before the join publishes. Its named-output case runs the left and right producers concurrently and returns their separately owned Results; its single-output case keeps the final value `7` readable through an escaped `ResultRef` after context teardown, with eight Payload bytes retained until that reference is released. A held-output admission failure releases its resources and a later Run recovers.

Completed sibling values remain accounted until their registered consumer takes them. Shared backing uses its existing storage domain and unique-owner accounting.

### Cross-output pure block sharing

`share_blocks_across_outputs` is an optional `OperationTraits` v25 field for pure deterministic Atomic Result operations. It is disabled by default. When enabled, the runtime may retain compatible block state transitions across outputs; incompatible state or denied retention causes recomputation. Block retention remains separate from complete Result caching, checkpoints and published outputs. Hit counts and reuse scope are implementation details, not performance guarantees.

## Limits and failure handling

Exact-set construction, certificate growth, Result discovery, callback stages and retained demand metadata have separate finite limits. Invalid domains or protocol envelopes fail as errors; limit exhaustion returns `ResourceExhausted`; cancellation returns `Cancelled`. The caller must retain the bundle or owners for borrowed inputs through each call and should retry only statuses documented as retryable by the owning execution API.
