# Structured Results and tensor members

## Scope & Ownership

`ResultRef` is the publication, ownership, and input/output path for all workflow data, including image data. A Result is a static directory of 1..16 typed Tensor members. A member may declare a spatial layout; this does not create a separate image result path. `Tensor`, `TensorSpan`, and `TensorWriter` are storage carriers inside the kernel and SDK, not workflow or operation input/output forms.

A Result carries structure, data, and the owners its storage needs. It has no attached domain description, no semantic kind, and no facet list. The kernel does not propagate color, alpha, transfer, or sampling descriptions from inputs to outputs, and it does not match such labels on connections. Two Results with the same structure connect to the same port even if the caller assigns them different meanings. An operation that needs a domain interpretation receives it as an explicit static parameter or as explicit operation-domain configuration, and it can check sample legality through a bound validator (see [Port validators and diagnostic reports](#port-validators-and-diagnostic-reports)).

The current package is 0.35.0. WorkflowDocument schema is 7, OperationTraits version is 27, the Result C operation ABI is 5, the CPU parallel and CPU tiles services are version 2, and the native GPU service is version 1. These version numbers describe independent contracts. The C++ operation interface uses `start_result` and `ResultContinuation`. The Result C operation table is the sole C operation-plugin interface; consumers must rebuild against the installed package because the C++ API and the exact-size C declarations are the contract. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) (Proposed) records a prefix-negotiated C ABI for all operations; package 0.35.0 does not implement it.

The compiler copies immutable schema into the plan. The execution coordinator owns producer state and schedules callbacks and I/O. A `ResultRef` owns its schema, monotone descriptor facts, certified backing, and source-association metadata. Copies of a reference share this ownership. An external reference or read window keeps required backing alive after the execution context ends. Association metadata identifies consumed inputs; it does not own their payloads. Dependency facts are not part of a Result: the execution records the actual Needs of each computation unit in a `TileRecord` (see [Computation units and tile records](#tile-records-and-frozen-evidence)).

```cpp
enum class ResultExtentKind : std::uint32_t {
  Fixed = 1, InputAxis = 2, InputElements = 3
};
struct ResultExtent final {
  ResultExtentKind kind = ResultExtentKind::Fixed;
  std::uint64_t value = 1;
  std::uint32_t input = 0, axis = 0;
  std::uint64_t divisor = 1, offset = 0;
};

struct ResultTensorLayout final {
  bool spatial = false;
  TensorStorageOrder order = TensorStorageOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
};

struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  TensorDescriptor descriptor;
  ResultTensorLayout layout;
};

enum class ResultPublicationPolicy : std::uint32_t {
  CompleteBundle = 1, IndependentChunks = 2, StablePrefix = 3
};
struct ResultPublicationScope final {
  ResultPublicationPolicy policy = ResultPublicationPolicy::IndependentChunks;
  ResourceVector<std::uint32_t> members;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  ResourceVector<ResultTensorSpec> tensors;  // 1..16 members
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultPublicationScope> publication_scopes;
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
```

The excerpt omits member functions; the declarations are in `photospider/data/result.hpp` and `photospider/execution/execution.hpp`. A workflow input with `result_schema` binds an owning `ResultRef`; execution returns named Result outputs in `ExecutionResult::results`. Tensor sample coordinates concatenate `batch_axes` with the descriptor's cell axes. `ResultTensorSpec::metadata_owner` is a resource lease that pays for the spec's managed nested containers; it is budget ownership, not attached interpretation. `id` and `version` name an explicit structural contract and enter compile identity; the kernel does not dispatch on them to a domain meaning.

Complete structured execution may compute a non-security `result_digest` over named Result outputs in `ExecutionResult::results`: canonical schemas and certified tensor coverage and sample bytes in logical order. It excludes object identity, source association, and physical layout; it is not authentication. Fragment execution leaves this digest empty. `ExecutionOptions::maximum_result_digest_samples` defaults to 65,536 and limits the certified tensor samples admitted for the digest; zero disables digesting. Before scanning, the coordinator also checks remaining Run-work and tensor coverage-count representability. If the complete digest does not fit these estimates, it omits the digest and continues with the execution result. For example, a large certified broadcast view need not be traversed once per logical sample solely to create a digest. After scan admission, digest reads use bounded windows and consume Run-work; cancellation, stale state, Root/resource, or read failures during that scan remain execution failures. The coordinator rechecks cancellation and stale state before returning success.

## Data Layout & Memory

`SchemaTemplate` has 1..16 Tensor members and up to eight domain axes. Every member has a fixed positive shape known before execution. A `ResultExtent` of kind `InputAxis` or `InputElements` refers to the static domain of an input; `SchemaTemplate::resolve()` evaluates these expressions from immutable input domains during compilation, without reading samples. No member shape, member count, or row count depends on runtime data. The canonical schema encoding domain is v5; it covers members, domain expressions, and publication scopes, and excludes physical storage order, pitch, object generations, and descriptor revisions.

`ResultTensorSpec` describes one typed Tensor member. Its cell descriptor has rank 1..8 with positive extents; `batch_axes` adds a positive-extent prefix, and the combined sample rank is at most eight. `atomic_trailing_axes` closes that many trailing cell axes into one indivisible observation. Schema validation does not impose a dense allocation or a representable full-domain element product. `ResultTensorLayout` records an optional spatial layout, the physical storage order, and row pitch. The default order is `TensorStorageOrder::Tiled`: the builder stores each tile separately and arranges spatial members planar within each tile. Storage order and pitch do not change semantic schema identity. A spatial layout maps height, width, and optional channel axes within the cell descriptor, independently of the batch prefix. A spatial tensor may have any legal channel count; the kernel does not infer RGB, alpha, a color model, or a numeric range from the channel count or layout.

`atomic_trailing_axes` defines observation completeness: sample regions close over those trailing axes. Payload authorization additionally follows member tile geometry. A spatial tile covers every non-H/W cell axis completely, including the channel axis, even when `atomic_trailing_axes = 0`. Explicit batch axes select separate coordinates. This geometric coverage does not infer RGBA or alpha semantics from the schema id, shape, or spatial flag. Logical tuple completeness is independent of physical byte order: in the default tiled storage, one pixel's channels are complete in the logical tile but lie in separate planes of that tile. The canonical image target in [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) D9 uses explicit logical axis roles and does not mandate HWC.

`ResultBuilder` assigns an object id at construction. The semantic key includes canonical schema and a length-framed execution scope. In compiled execution the scope is the actor key described under [Computation units and tile records](#tile-records-and-frozen-evidence): the structured template, which binds the operation contract, parameters, and ordered inputs, plus the frozen snapshot identity and the computation unit. Physical page size, file offsets, descriptor revision, and resource limits do not change semantic identity. `ResultRef::capture()` snapshots the certified descriptor at one revision. The host actor publishes only this captured view, so observers cannot see a prefix from a later revision. `ResultRef::read_tensor()` requires that descriptor and reads only certified samples. `ResultDescriptor` stores one coverage `Footprint` for each of up to 16 members. `ResultTensorInput` carries the explicit tensor Need, member slot, descriptor, and authorized sample set into a callback; the phase is borrowed for one poll, while its owning capabilities follow their own lifetime rules.

For each Result successfully supplied to an actor, the host keeps a Root-accounted historical fact keyed by input port and ObjectId, with the descriptor revision. This history identifies consumed sources but retains no payload and grants no read permission. `ResultObjectInputs` separately keeps the current owning ResultRef per input port across polls; the host clears that map when the actor completes or retires. Stored `ResultTensorInput` capabilities own their ResultRef and captured descriptor, and acquired read windows own the backing authorized by that Need. A physical tensor view independently retains its source Results, schemas, and backing.

Before publication, the host builds the ordered association from the actor's `(input port, ObjectId)` facts. `ResultRef::retain_association()` checks that IDs are nonzero, do not name the output itself, and preserve every previously claimed ID. It compares through a temporary sorted copy, leaving the stored port/ObjectId order unchanged and preserving repeated ObjectIds supplied through different ports. Membership checks charge Root work and observe cancellation; a failed check leaves the existing association unchanged. There is no fixed association entry limit: Root metadata capacity and Run-work admission bound the list.

## Scheduling & Publication

### Computation units and tile grids

Each plan step computes one selected named output with its own resolved output contract. Every member of that output has its own fixed tile grid, and every tile of every requested member is one computation unit. Kernel execution has no whole-result mode: an algorithm that needs a complete input or output expresses that need through ordinary member tile requests or through a Run-shared stage (see [Run-shared stages](#run-shared-stages)).

`PlanningOptions::tile_extent` sets one square tile extent for the whole plan. It defaults to 128 and must be a positive power of two, including 1. `TileGrid::from_tensor()` derives one grid per member from the schema and that extent:

- A spatial member tiles its height and width axes. Its explicit batch axes become grid batches with one batch coordinate per tile, and every other cell axis, such as the channel axis, is complete in each tile.
- A nonspatial member tiles its last two free cell axes, where free axes are the axes before the atomic trailing axes. Earlier free axes, including explicit batch axes, become grid batches. A member with one free axis tiles only that axis. Atomic trailing axes are complete in each tile.

For a planar RGBA image with shape `{4, 1080, 1920}` and `atomic_trailing_axes = 0`, a spatial layout with `channel_axis = 0` gives tiles of `{4, 128, 128}`, so each tile holds all four channels of its pixels. A nonspatial Float32 table of shape `{2, 3, 1000}` tiles `{3, 1000}` into `{3, 128}` column blocks and uses the leading axis as two batches. Grid creation rejects tile-count overflow without requiring the full sample count to fit `uint64_t`. The grid is independent of physical storage.

```cpp
struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0, tensor_slot = 0;
  TileIndex tile = 0;
};

struct TileNeed final {
  std::uint32_t input = 0, tensor_slot = 0;
  bool descriptor_only = false;
  TileSet tiles;  // half-open intervals in the producer member's grid
};

struct ResultSharedNeed final {
  std::uint32_t stage;
  std::optional<TileRequest> tiles;  // absent: all stage members
};

struct ResultProgramNeed final {
  ResourceVector<TileNeed> tiles;
  ResourceVector<ResultSharedNeed> shared_stages;
};
```

The excerpt omits constructors and member functions; the declarations are in `photospider/data/tiles.hpp`, `photospider/execution/tile_dependencies.hpp`, and `photospider/plugin/result_program.hpp`. `TileSet` stores normalized half-open `[begin, end)` intervals; construction, union, and copying scale with the interval count, not the tile count. `TileRequest` maps member slots to tile sets; `TileRequest::all()` builds full member ranges from static grids.

The coordinator runs one actor per computation unit. The actor key concatenates the step's structured template, the frozen snapshot identity, and a digest of the step index, output index, member slot, tile index, and tile extent. The template is a static digest of the selected-output contract and its upstream templates or workflow-input names and schemas; it contains no graph, node, or step ID. Within a Run, a repeated request for the same unit reuses its actor. Steps with different indices keep separate actors even when their templates are equal; only the completed-result cache below can reuse equal templates. The actor's query carries `tensor_slot`, `tile_unit`, `tile_extent`, and a `tensor_outputs` footprint equal to the unit's tile region. Every ordinary tile actor's `ResultPublication` must set `complete = true`, cover exactly the selected member's query tile, and leave every other member empty. A partial or nonfinal publication fails validation. Needs may span multiple polls; progressive Result publications belong to shared-stage execution.

### Needs and grants

A callback returns `ResultProgramNeed` to request input data. Each `TileNeed` has exactly one form:

- A payload Need names one input, one member slot, and member-local tile intervals. The host validates the intervals against the compiled grid of that producer member. A payload Need also authorizes the input's static descriptor facts.
- A descriptor-only Need has `tensor_slot = 0` and no intervals. It grants the input's descriptor facts and no payload.
- A payload Need with zero intervals is valid and requests nothing. The host skips it: it creates no dependency, no descriptor grant, no sample grant, and starts no producer.

The host validates the complete Need envelope, including interval counts and member slots, before it starts any producer work. For each payload Need, the coordinator obtains the actor of each requested producer tile, assembles the unit Results into one Result of tensor views, and grants the union of the requested tile regions, closed over `atomic_trailing_axes`. When a unit completes, the coordinator installs the union of its admitted Needs as the unit's `TileRecord`. Intervals merge only within one producer and member; the record retains no payload. `ExecutionDependencies::tile_record()` returns the record of a completed unit, and `dirty_units()` follows reverse edges from changed producer tiles or descriptors to the recorded units that consumed them.

Full tile publication is atomic: a unit's tile either becomes visible with complete coverage or not at all. Completion of a producer unit alone does not establish complete coverage of its output; coverage is the union of the published tiles. The actual Needs of a unit are frozen with its publication, so later Needs cannot change the dependency record of a published tile.

### Publication scopes

`SchemaTemplate::publication_scopes` statically declares visibility rules over complete members. Member slots and scopes are strictly sorted and disjoint, and at most 16 slots are listed. Members not listed in any scope use `IndependentChunks`.

| Policy | Members | Visibility |
| --- | --- | --- |
| `IndependentChunks` | Any | Each completed tile becomes visible independently. |
| `CompleteBundle` | One or more | The listed members become visible together after every tile of every listed member is complete. |
| `StablePrefix` | Exactly one | Complete tiles become visible in local tile index order. |

Scopes control visibility, not read authority and not callback scheduling. When a request touches a member of a `CompleteBundle`, the visibility gate drives every tile of the whole bundle, including members that were not requested. Aggregate validation and dependency certification precede one atomic installation; no bundle member is visible early. All units of a bundle inherit one frozen union of the bundle's dependencies, and fallback provenance from any member applies to every member, so a sink that reads one member cannot treat as clean a result that was certified together with a fallback-tainted sibling. A `StablePrefix` gate keeps its producer actors after visibility, so a later request for an earlier tile reuses their certified state. The returned Result still contains only the requested selection.

### Run-shared stages

Some algorithms read a complete input, perform one global pass, or emit an externally visible effect once. An operation declares such work as an `OperationSharedStage`:

```cpp
struct OperationSharedStage final {
  std::string key;
  std::vector<ExtensionIdentity> validators;
  SchemaTemplate result_schema;
  std::vector<std::uint32_t> input_indices;
  Backend backend = Backend::Cpu;
  std::uint64_t continuation_bytes = 0, workspace_bytes = 0;
  std::uint64_t maximum_work = 0, maximum_output_payload_bytes = 0;
  std::uint32_t maximum_dependency_stages = 0;
  bool mandatory = false;
};
```

The stage contract is static. Preparation fixes its schema, input projection, backend, state, workspace, work, and output payload bounds, and these enter operation identity. Within one Run, the coordinator creates at most one producer per original node instance and stage, independently of how many tiles or outputs request it. The input projection declares potential dependencies; it does not grant reads. The stage issues ordinary member Needs before it reads. All stage bounds are cumulative for that stage in that Run, and work bounds include input and publication validators.

A consumer requests a stage through `ResultSharedNeed`. An absent `tiles` selection requests every member tile of the stage; an explicit empty selection grants no payload and does not start the producer. The coordinator first tries to grant the requested tiles from immutable committed stage coverage. Only missing coverage registers a wait edge and advances the producer. A continuation waiting on another stage therefore remains a valid source of tiles it has already committed; a wait that would require a cycle of uncommitted tiles returns `Cycle`. Each visible stage commit freezes its dependency evidence and fallback provenance at commit time. A consumer receives the stage Result and that frozen history; it does not receive authority to read the stage's original inputs. A later tainted commit does not retroactively taint an earlier clean commit.

A stage's success or failure is retained until the Run ends. A failed stage fails every consumer that requests the affected tiles, and cancellation of the Run retires the producer after admitted callbacks return. A stage is not spilled, not recomputed within the Run, and not shared across Runs. Ordinary execution runs every `mandatory` stage, even when the plan selects no output tiles. Execution with an explicit `DemandQuery` skips mandatory stages only when all its selections are empty. An operation that is not side-effect-free must declare at least one mandatory stage, and an operation that is not both deterministic and side-effect-free cannot be cacheable. An operation declares at most 64 stages.

When execution under `ExecutionMode::CpuExact` requests a GPU stage producer, the coordinator returns `BackendUnavailable` at that point. Plans that only read stage metadata are unaffected. When an ordinary step or a stage uses the GPU backend, the Run creates its native upload state. CPU fallback is restricted to admission before stage computation starts; stage arithmetic is not retried on another backend after its factory is entered.

A direct `ResultContinuation` for a stage applies the same static visibility rules. A hidden intermediate commit returns an empty Need so the host can poll again. A returned nonfinal publication is a `capture()` at one revision, so a delta rejected later cannot appear through an earlier `ResultRef`. If a validator rejects a later tile, the host retires the projected producer with that failure: `production_status()` of an escaped prefix reports the failure before and after continuation destruction, while previously certified tiles remain readable.

### SDK acquisition helpers

The C++ SDK headers `photospider/plugin/tensor_helpers.hpp`, `tile_helpers.hpp`, and `whole_stage.hpp` provide nonblocking helpers. Each helper returns either a Need for the callback to return from its poll, or a ready value; none executes a producer, waits, fills missing samples, or collects dense bytes implicitly.

| Helper | Ready value | Use |
| --- | --- | --- |
| `sdk::to_whole(phase, input)` | `WholeTensorInputs`, one complete capability per member | Acquire every member of one input. |
| `sdk::to_whole(phase, input, slot)` | `WholeTensorWindows` covering the member | Acquire one complete member as owning read windows. |
| `sdk::to_whole(phase, input, slot, samples)` | `WholeTensorWindows` clipped to each box of `samples` | Acquire a member ROI. An empty `samples` returns an empty ready vector and grants nothing. |
| `sdk::to_tile(phase, source, member)` | A Result of zero-copy views covering the query tile | Publish the selected output tile from an owned Result or stage Result. |
| `sdk::collect(phase, input, region)` | `TensorSpan` | Explicit dense copy of one authorized rectangle. |
| `sdk::publish_dense_tensor(...)` | Publication through `ResultBuilder` | Explicit dense output allocation for one rectangle. |

The ROI overload of `to_whole` rejects an already cancelled query at entry and returns a compact tile Need until every selected sample is authorized. Complete acquisition does not imply dense collection; dense copies are an explicit operation decision.

`sdk::with_whole_stage(definition, work_bound)` adapts a single-output CPU operation whose algorithm needs its complete output into one Run-shared stage. When the stage is requested, the original factory runs at most once within that Run, with the complete output schema and no selected tile; it remains responsible for its nonblocking input Needs, numeric policy, and resource bounds. Output tile callbacks then publish retained views of the stage Result through `sdk::shared_result_tile`. This is an SDK adapter chosen by the operation author, not a kernel execution mode. Operations with several independent outputs use separate stages with narrower input projections; operations whose outputs are computed jointly, such as a stable sort, use one joint stage.

### Tile records and frozen evidence

```cpp
struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0, tensor_slot = 0;
  TileSet tiles;  // empty: descriptor-only
};

struct TileEvidence final {
  std::uint64_t identity;
  TileUnit owner;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> inherited;
};

struct TileRecord final {
  TileUnit unit;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> shared;
};
```

Every payload dependency also observes descriptor changes. `TileEvidence` is immutable dependency evidence for one stage commit: it owns that commit's local Needs and references earlier frozen evidence. It is never a read capability. A consumer of a stage tile keeps one reference to that node. The execution table imports each distinct node into the target Root once, preserving its identity, indexes reverse edges from each node once, and enumerates workflow dependencies from one locked table state. Nodes retire by reference count. These rules avoid copying the evidence of P stage tiles into each of Q consumers.

### Mandatory effects and repeated inputs

Ordinary execution runs every mandatory shared stage, including when the plan's output selections are empty. An explicit `DemandQuery` runs mandatory stages when at least one selection is nonempty and skips them when all selections are empty. Mandatory stages reach a terminal state before named outputs are collected. Effectful work is not shared or cached across Runs. Failure or cancellation of a mandatory stage fails the Run.

Result C ABI operation definitions can declare a fixed input prefix and one repeated trailing group. `input_count` and `inputs` describe only the prefix; `repeated_input` supplies the prototype constraint applied to each actual trailing input during metadata resolution and `start`/`poll` query validation. A nonzero `repeated_maximum` enables the group and requires `1 <= repeated_minimum <= repeated_maximum`, with the prefix plus maximum no greater than 1,024 inputs. When disabled, all repeated fields are zero or null. `repeated_match` is 0 or 1: value 1 requires every repeated tensor to match the first repeated tensor's dtype and complete logical `sample_shape()`; value 0 requires a pure metadata resolver for the expanded input list. The C++ registry keeps the unexpanded template and resolves the actual input count during preparation. This declaration constrains metadata only; the operation still requests a tile Need for every actual input it reads.

### Native GPU input access

A native GPU callback can call `ResultProgramPhase::acquire_native_atlas(input, slot)` for a payload-authorized tensor Need. It returns an immutable `FragmentAtlas` containing exactly that Need's coverage; the operation still decides how to dispatch and publish. Calls for the same port and slot within one poll reuse one atlas. CPU access, a missing Need, or a descriptor-only Need produces sticky `InvalidArgument` and does not widen authorization. Root-backed payload and directory storage is separate from operation workspace, and the atlas owns those buffers without retaining the source Result or read windows. A caller-held shared atlas can remain alive after the poll, source, and execution context end.

Native GPU input transport reuses raw physical backing within a Run only while a weak proof still identifies the same live CPU storage and the complete view key matches. An optional byte-bounded context cache can reuse uploads across Runs by device instance/build and logical input bits, Region, dtype and shape; it excludes the Result schema and association, then rebinds a hit to the current schema and association. Cache hashing consumes the coordinator shared cache-work budget, and optional-work exhaustion skips the cache path. Capacity reclamation first clears retransmittable Run-local entries, then evicts retained context entries. `host_access_count` records actual CPU exposure of live native-backed storage once per storage in a callback scope; metadata inspection, native-address acquisition, view forwarding and hashing do not count. Native GPU service failures are latched when the service call returns. A later `consume_work` exhaustion or callback status cannot replace that earlier service error; if work exhaustion occurs first, a later service error does not replace it.

Result atlas packing accepts either a `Tensor` or a `ResultTensorInput` capability. The lower-level `FragmentAtlasPlan` can pack a `Tensor` with empty coverage without reading source samples. The Result overload reads through Need-authorized owning windows, so signed and broadcast strides, fragmented coverage, and batch-prefixed spatial tensors are handled without dense conversion. `preparation_work()` and `materialization_work()` report generic geometry and packing units. Result preparation precharges its generic work through `FootprintLimits::consume_work`; Result materialization additionally precharges window acquisition and read-bound work through that callback. `materialization_work()` is not the complete Result-overload charge.

### Cross-output block-state sharing

Block-state retention can reuse a completed state when the current request and proof match. A retained state is an execution implementation detail; cache-hit counts and block transitions are not a public performance guarantee.

Pure deterministic Result operations may opt into `share_blocks_across_outputs`; registration rejects the opt-in for an operation that is not both deterministic and side-effect-free. The C Result ABI 5 exposes the same opt-in as `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V5` (flag bit 5).

The shared namespace includes the operation and all resolved output contracts, backend, execution mode, selected resource identities, static parameters, and actual input metadata. Each block key also includes its kind, range, mode, incoming state schema/coverage/content, and the currently supplied tensor metadata, coverage, and bytes. The selected output index and output-specific metadata are not appended independently. A computation shared by different outputs must therefore be output-independent or encode the distinction in its state or mode. Public output Results, their associations and tile records, checkpoints, and completed-result caches remain output-scoped and independent.

Shared stages expose no checkpoint or block callbacks. For ordinary actors, block-state lookup and retention require a pure, deterministic, cacheable operation, a pure and deterministic selected-input producer closure, no fallback taint, and no direct shared Need. Handling a shared Need disables optional block reuse for that actor. Retention is optional and charged to bounded cache work and Root capacity. If lookup or retention is unavailable, the callback computes the block normally. A lookup hit whose cached state cannot be unpacked because of capacity is treated as a block miss: the coordinator releases the candidate and its key lease and computes normally. Work limits, cancellation, and protocol failures during unpacking remain fatal. Each output still completes its own Needs and records its own `TileRecord`. The flag does not deduplicate concurrent producers or guarantee one computation per Run.

### CPU fallback

When a GPU attempt qualifies for CPU fallback, the coordinator gives the retry fresh failure latches, drops the attempt's continuation, tensor and object capabilities, descriptor inputs, recorded Needs, input facts, and completed-cache replay log, and restarts the same computation unit on CPU. Producer units that already completed keep their published Results and records, so the retry obtains them again without recomputing them. Spent Root work and stages are not refunded. The retried unit is fallback-tainted, and a consumer becomes tainted when a supplied producer unit or stage commit is tainted or ran on a backend other than the planned one. Tainted units skip checkpoint lookup and publication, block-state reuse, and completed-result retention.

### Checkpoints

Ordinary actors of pure, deterministic Result programs can use the C++ `ResultProgramPhase::checkpoint_publish` and `checkpoint_before` callbacks to retain continuation state; either call from an operation without both purity traits is a protocol failure. Shared stages do not expose these callbacks. Lookup and retention also require the selected-input producer closure to be pure and deterministic, an actor that has handled no direct shared Need and is not fallback-tainted, and remaining optional cache work. Otherwise lookup misses and publication declines without failing the operation. A published checkpoint owns a sealed state Result and a payload-free witness: the unit's `TileRecord` with the Needs admitted so far, the recorded `TileRecord`s of its producer closure, inherited shared evidence counted once per distinct node, and the consumed input facts, each keyed by input port and ObjectId with the highest descriptor revision observed.

A checkpoint scope belongs to one computation unit: its key is the actor key with a `/checkpoint` suffix, so it binds the structured template, the frozen snapshot identity, the step and output indices, the member slot, the tile index, and the tile extent. `checkpoint_before(phase, before)` returns the greatest published sequence in that phase not greater than `before`. A restore requires the same computation unit. The coordinator first stages the witness Needs, producer records, inherited evidence, input facts, identity facts, and callback association under an optional allocation fence. Only after staging succeeds does it restore ancestry and commit the staged values by same-Root moves. A capacity refusal during staging returns an empty checkpoint and leaves the actor state and read authorization unchanged. Work limits, cancellation, and protocol failures remain errors. A successful restore merges the witness Needs into the current unit's `TileRecord` (records already present in this execution remain authoritative) and refreshes the current poll's `association` with the restored input history; references or iterators to its previous elements may be invalidated, while the borrowed vector remains valid until poll returns. It does not populate the current phase's `results` or `tensors`, recreate read windows, or grant Need authorization. The callback must request current input access again.

The context directory holds weak references to checkpoint scopes, while an active execution holds its scopes strongly; `ExecutionContextConfig::maximum_result_checkpoint_scopes` bounds the live scope count. Each scope retains at most 64 entries, and the summed witness weight of a scope stays within the dependency `maximum_boxes` limit; the oldest entries retire first. The runtime charges optional checkpoint work before constructing lookup keys, traversing the producer closure, or copying Need records and input facts. Zero or insufficient optional cache work skips lookup or declines publication; it does not copy unadmitted witness data. A copied `ResultCheckpoint` owns its state and witness even after the index or context is cleared. The C Result ABI exposes `checkpoint_before`, `checkpoint_read`, and `checkpoint_publish` through poll-scoped opaque handles and sealed generic state Results; see the [C checkpoint services](Dependency-Data.md#c-checkpoint-services) contract.

Checkpoint eligibility follows purity independently of completed-result caching; block retention additionally requires the operation to be cacheable. Both remain subject to the actor restrictions above. An operation that declares `deterministic = false`, for example one whose output contains a Run-local ObjectId, excludes every dependent consumer from checkpoint lookup and retention and from block-cache admission. A non-cacheable but deterministic operation keeps its own checkpoint eligibility.

### Completed-result cache

The completed Result cache reuses the sealed Result of one computation unit across frozen execution identities, including binding generations and Runs of an equivalent plan, when the workflow-input content that the unit actually reached is unchanged. A completed-cache key digests the structured template, the selected output index, the planned backend, the tile extent, the member slot, and the tile index. The key contains no step index or graph ID, so an equal template at another step can reuse the entry. Each key keeps up to eight candidates. A retained entry (manifest) contains:

- the sealed Result;
- a payload-free tile snapshot: the unit's `TileRecord`, the recorded `TileRecord`s of its producer closure, the shared evidence and source routes it inherited, and routes from each producer to input ports in contract terms;
- the workflow-input sources reached through that closure, each a named binding with member tile sets, and a digest of their current content;
- the replay log: each admitted Need of the unit, paired with a digest of the supplied facts after that Need (canonical schemas and granted tensor footprints, without sample bytes);
- the backend, the frozen fallback provenance, and the cache epoch.

The source digest covers, for every recorded source, the binding's canonical schema and the tensor samples of the reached member tiles. Bytes that no recorded Need reached do not affect the proof.

A lookup rebinds each candidate's tile snapshot and shared evidence to the current plan by contract. When one recorded producer would map to different current producer steps, or when context reordering changes the evidence routes, the candidate misses rather than issue a Need against an arbitrary step. The coordinator recomputes the source digest over the current bindings and compares it with the stored digest. It then replays each saved Need through the ordinary supply path, including input validators, and requires each supplied-facts digest to match. A candidate that passes is adopted: the coordinator rebinds the cached Result with a new ObjectId, the current execution scope, and the association built from the replayed input facts, merges the rebound `TileRecord` into the unit, and validates the publication like a computed output. The cache entry retains the physical backing owners it references; rebinding does not make the old Result wrapper or old execution scope the identity of the new output.

Structured Result entries share the bounded cache LRU and managed Root capacity. The LRU holds at most 4,096 entries, `ExecutionContextConfig::result_cache_bytes` bounds retained backing (zero disables retention), and `maximum_dependency_cache_metadata` bounds manifest metadata. Capacity accounting deduplicates physical backing owners, including backing reached through tensor views and affine owners, rather than summing logical tensor sizes. Cache candidates stage only accounting deltas; a candidate that would retain an old input owner outside the current allowance becomes a miss without changing the live ledger. Eviction releases the cache's owner references; callers holding a Result or read window continue to own its backing. `clear()` advances the cache epoch, so an in-flight producer from the previous epoch cannot refill the cleared cache. Cache proof construction, Need replay, and rebinding consume the Run's optional cache work (`ExecutionOptions::maximum_dependency_cache_work`). Insufficient optional work or Root capacity skips retention or produces a miss without changing an otherwise successful computation.

The `StructuredResultCache` owned by each Run constructs completed-result keys and manifests, builds source proofs, restores replay snapshots, and spends the optional cache-work quota. It receives borrowed context and actor views for each synchronous call. The Run-local Host supplies Needs through the coordinator and adopts a candidate after successful replay; a failed replay restores the actor's previous inputs and Need record. Once lookup acquires a strong candidate, it carries that owner through replay and adoption; adoption does not require a second proof of LRU residency. An optional promotion lookup moves the entry to the LRU tail only if the same manifest remains resident. Cache closing or an epoch change rejects adoption while the candidate owner remains alive until the attempt ends.

Retention requires a pure, cacheable operation and a pure, cacheable selected-input producer closure, a unit that is not fallback-tainted and ran on its planned backend, and remaining optional cache work. A unit whose replay log exceeded the metadata limit is not retained. Each entry covers exactly one computation unit, and entries for different tiles are not combined. Shared stages and ordinary actors that handle a direct shared Need bypass completed-result caching. Outputs installed through `CompleteBundle`, `StablePrefix`, or a complete output validator also bypass completed-result retention. The C Result ABI does not expose this cache; its implementation is private to the C++ execution runtime.

### Result builder

`ResultBuilder` owns publication. The producer writes tensor regions through `publish_tensor`, `publish_tensor_view`, or `publish_tensor_kernel`. Tensor regions are checked against the member's logical sample domain. Overlap, out-of-domain access, or failed resource admission retires the producer with an error. `ResultBuilder::start()` takes the query's tile extent; an invalid spatial tile extent returns `InvalidArgument` before allocation.

Publication keeps physical arrays and backing owners in the Result-derived storage owner, while publication state owns the Root, resource leases, and schema. Destruction retires array backing before the publication state releases its Root leases. Storage-owner indexing avoids rescanning previously published backing for each publication, and `acquire_tensor` locates pieces by an interval search when the member's partition permits it, otherwise by a bounded scan. `acquire_tensor` copies immutable backing handles while holding the publication mutex, then unlocks before constructing the owning read window. `publish_tensor_kernel` prepares callback work outside the mutex, then reacquires it to validate the owner, revision, and sticky failure before committing. A read capability retains its authorized Result and backing, but grants reads only within the captured descriptor and requested Need.

The publication validator receives a `PublicationValidationView` and a borrowed `PublicationValidationServices` interface for one synchronous validation. The coordinator owns cancellation, work admission, current limits, and the actor. Validation checks publication identity, revisions, and associations; it does not retain the services or perform scheduling, publication, retirement, or notification. The coordinator applies the returned status and retirement intent after validation completes.

`publish_tensor_kernel` lends writable windows for the requested region. The builder partitions the request at member tile and batch boundaries and gives spatial members tile-local planar bytes. It reserves no full-domain backing, so a sparse request over a huge logical domain is charged by its actual writable tiles. Callbacks must use each window's `spec()`, `region()`, `sample_axis()`, and the strides returned by `row_run()` or `rectangle_run()`. They must not assume planar backing or contiguous rows across windows.

```cpp
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& samples,
    const std::function<
        Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
    const CancellationToken& cancellation = {});
```

The Footprint overload publishes an exact set of disjoint canonical boxes in one transaction. The builder copies `samples` before it runs any producer code, so a callback that changes the caller's footprint object does not change the published set. A footprint with one box takes the Region path above. For several boxes, the footprint must already be closed over the tensor's observation grouping (`close_samples` returns the same set), and must not overlap existing coverage; a violation returns `TypeMismatch`. The callback receives one write window per box in `boxes()` order. Holes receive no storage and no coverage. An empty footprint skips the callback and payload work.

The callback runs outside the publication mutex, and same-producer mutation and sealing are rejected while it runs. After the callback returns, the builder rechecks the owner, the revision and the sticky failure, then cancellation. A returned error, a thrown exception (`ResourceExhausted` for allocation failure, `OperationFailed` otherwise), cancellation, or a revision change (`Stale`) records the producer failure, releases the buffer and certifies no new coverage; previously certified prefixes remain readable. Workers writing through the windows must finish before the callback returns, because the windows and raw pointers expire at that point. On success, the builder commits the boxes and the coverage together at one new revision.

`ResultTensorWriteWindow::borrow_dense(spec, writer)` lends an explicit row-major `TensorWriter` as a write window without copying. It first validates the member's spatial topology, then rejects a writer whose dtype or full sample shape differs with `TypeMismatch`.

The published Result records ordered source ObjectIds for lineage. That association is metadata, not a strong owner for input payload and not a read grant. `ResultObjectInputs`, `ResultTensorInput` capabilities, and retained windows provide the active owner and authorization paths described above. A physical tensor view also retains every source Result whose bytes it publishes. Immutable operation resources, such as ICC profile or OCIO configuration bytes, are not Result members. Static preparation returns the explicit `ResourceIdentity` list it needs; the compiler selects exactly those entries from the `ResourceSet` passed to `Compiler::compile`, fails if one is missing, and the plan and execution state retain the selected set. A Result therefore never keeps a color configuration alive on its own. Public `ResourceMap` and `ResourceVector` containers also carry their allocator and managed ownership; copied results, names, and observations retain root-accounted metadata until those containers and any contained `ResultRef` owners are released. Consumers must budget these copies along with payload. The policy is conservative, so live owners can retain more backing than the final output alone needs.

## Port validators and diagnostic reports

The kernel provides generic extension points for sample legality and bounded reports. It does not ship domain validators: the operations SDK registers its own in `plugins/ops/common/domain/contracts.cpp`, for example `ops.scalar-range`, the LUT table validator, and the LUT report validator declared in `photospider/ops/contracts.hpp`.

```cpp
struct ExtensionIdentity final {
  std::string key;
  std::uint32_t version = 1;
  std::string implementation;
};

struct ValidationContext final {
  const SchemaTemplate& schema;
  const ResourceBudget& resources;
  const CancellationToken& cancellation;
  bool complete = false;
  std::uint32_t tensor_slot = UINT32_MAX;
  const Footprint* samples = nullptr;
  std::function<Status(std::uint64_t)> consume;
  std::function<const Footprint*(std::uint32_t)> tensor_coverage;
  std::function<Result<ResultTensorReadWindow>(std::uint32_t, const Region&)>
      acquire_tensor;
};

struct ValidationDefinition final {
  std::shared_ptr<const void> owner;
  ExtensionIdentity identity;
  std::uint64_t scratch_bytes = 0;
  bool requires_complete = false;
  std::function<Result<std::shared_ptr<const void>>(
      const ValidationPreparation&)> prepare;
  std::function<Status(const void*, const ValidationContext&)> validate;
};

Status OperationRegistry::register_validator(ValidationDefinition);
Status OperationRegistry::register_diagnostic(DiagnosticDefinition);
```

A port binds validators explicitly through `OperationPortConstraint::validators`, and a shared stage binds them through `OperationSharedStage::validators`. Each entry names a registered key, version, and implementation; the registry rejects a missing identity, a version or implementation conflict, duplicate keys, malformed definitions, and any registration after `freeze()`, without partial changes. The bound identities, including version and implementation, enter operation identity and therefore the plan cache key. Addresses, callback pointers, and report contents do not.

Validation has two phases:

- `prepare` runs once during operation preparation, outside the registry lock. It receives the port or stage schema, the static parameters, and the `ResourceSet` restricted to the identities that the operation's preparation selected. It returns owned immutable state.
- `validate` runs on the entry thread for each new grant or publication. `ValidationContext` exposes only host-authorized reads: `acquire_tensor` rejects a member or region outside the current coverage with `InvalidArgument`/`UnauthorizedRead`. Work is charged through `consume`, and `scratch_bytes` is admitted before entry. The validator must not retain the context or any borrowed object.

The coordinator calls validators at these points:

| Point | Coverage visible to `validate` | `complete` | Failure category |
| --- | --- | --- | --- |
| Input tensor grants | The members granted to one input port by one Need envelope. | Every member of the input schema is fully granted. | Workflow input: `InvalidArgument`. Produced input: `OperationFailed`. Origin `Domain`. |
| Output publication | Coverage added since the previously validated revision. | The validated coverage equals every member's full domain. | `OperationFailed`, origin `Domain`. |
| Shared-stage commit | Each newly visible stage commit, before supply to consumers. | The visible stage coverage is complete. | `OperationFailed`, origin `Domain`. |

A validator with `requires_complete` sees only complete coverage; an incomplete context returns `TypeMismatch`. For a complete output validator, the coordinator admits work and visibility only when the producer's request is complete after applying the triggered static publication scopes. It decides completeness from the requests for that producer's specialized output that are merged before any output is collected: the union of the root output requests of all aliases of that output, and, within one consumer Need envelope, the union of payload grants to the same producer step. Workflow inputs, descriptor-only Needs, and empty payload Needs add no completeness, and requests to different producers or different specialized outputs are not combined. A `CompleteBundle` that covers the whole Result therefore permits a request for one member. The gate assembles the scope and calls the validator once. The validator never enlarges the caller's request or the producer's input Needs, and each alias or port still receives only its own selected coverage. A request that remains incomplete starts no producer and calls no validator. A complete stage validator delays visibility until full static stage coverage; a stage that ends with incomplete coverage fails.

A completed-result cache hit still runs the input validators on the replayed Needs without repeating the computation. An invalid workflow input therefore fails with `InvalidArgument` before the cached output is returned, and a changed static parameter that a validator prepares from produces a different plan cache key.

The first service failure inside one `validate` call takes priority: if a read fails and the callback ignores it, the call still returns that read failure. A failed validator service cannot publish a result. Exceptions thrown by `prepare` or `validate` become typed failures. `ValidationDefinition::owner` retains the callback code; the host destroys it only after the callback objects and the prepared validator state, so a prepared operation keeps working after the caller releases its registry handle.

`OperationTraits::diagnostics` lists the report types an operation may emit. Before callbacks run, the host initializes a Root-owned `DiagnosticAccumulator` for those types. A callback reports a fixed-size byte record through `ResultProgramPhase::report_diagnostic` (C: `report_diagnostic`) by declared ordinal; `DiagnosticDefinition::validate` checks it, and `merge` writes a complete new record without changing either input. A failed report leaves the accumulated bytes unchanged. `OperationTiming::reports` carries the owning accumulator; `find(identity)` returns the merged bytes, or `NotFound` if the type never reported. The kernel interprets none of these bytes. The numeric work report is one such type, defined in the SDK header `photospider/ops/numeric/diagnostics.hpp`.

An operation can attach an opaque domain code to a failure. `Status(ErrorCode, message, DomainErrorCode, FailureDetail)` stores it in `FailureDetail::domain_code`; C callbacks use `fail_domain(status_code, domain_code)` on the run or validation services. The kernel preserves the code without interpreting it, and the first recorded domain failure is not replaced by later callback or diagnostic failures.

## Limits & Failure Handling

Every `ExecutionContext` creates a managed `ResourceBudget`. If `ExecutionContextConfig::managed_resources` is unset, the context uses default-constructed `ResourceLimits`; an explicit value supplies the configured capacities. `maximum_live_bytes` also caps the Payload dimension. `ExecutionContext::resource_budget()` returns a shared handle for clients that need to account temporary storage, and its leases may outlive the context.

The execution root accounts for payload/backing, work, stages, continuations, queue entries, metadata, and retained owners under their configured resource dimensions. Bounds are finite. Oversized payload, or owner admission fails with a resource error rather than returning an incomplete witness. This managed-capacity model covers instrumented resources; it is not an RSS bound.

An output can declare `maximum_output_payload_bytes`, and a shared stage declares its own bound. The host counts newly owned payload cumulatively across the tile actors of one logical output in one Run, counting each unique physical owner once. It excludes borrowed authorized owners: source allocations that remain live through exposed Results, authorized tensor grants, or supplied stage Results. The check keeps only weak source references, so the cap does not keep input payload alive. The first eligible poll allocates the cap guard and shared control block from the Result Root before entering the callback; insufficient Metadata capacity rejects the poll before callback entry. Owner enumeration and comparison consume normal Run and Root work. Exceeding the cap returns `ResourceExhausted` with `CapacityLimit`. Workspace, metadata, and operation-resource admission remain separate. The check runs when a Result is published, so a trusted callback's earlier Root allocations are still charged and are not prevented by the cap.

Result publication errors are sticky. Cancellation and stale execution retire active state only after admitted callbacks have returned. A failed append or tensor publication cannot be repaired by a later successful callback; a previously certified prefix remains immutable and readable under its captured descriptor. The last owning Result/read-window release retires backing and retained associations.

Behavioral regression tests are registered with the kernel build. The registrations under `tests/unit/kernel` and `tests/integration/kernel` are the current source for executable coverage.
