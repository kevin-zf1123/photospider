# Structured Results and tensor members

## Scope & Ownership

`ResultRef` is the publication, ownership, and input/output path for all workflow data, including image data. A Result is a static directory of 1..16 typed Tensor members. A member may declare a spatial layout; this does not create a separate image result path. `Tensor`, `TensorSpan`, and `TensorWriter` are storage carriers inside the kernel and SDK, not workflow or operation input/output forms.

A Result carries structure, data, and the owners its storage needs. It has no attached domain description, no semantic kind, and no facet list. The kernel does not propagate color, alpha, transfer, or sampling descriptions from inputs to outputs, and it does not match such labels on connections. Two Results with the same structure connect to the same port even if the caller assigns them different meanings. An operation that needs a domain interpretation receives it as an explicit static parameter or as explicit operation-domain configuration, and it can check sample legality through a bound validator (see [Port validators and diagnostic reports](#port-validators-and-diagnostic-reports)).

The current package is 0.38.0. WorkflowDocument schema is 7, OperationTraits version is 29, the Result C operation ABI is 7, the CPU parallel and CPU tiles services are version 2, and the native GPU service is version 1. These version numbers describe independent contracts. The C++ operation interface uses `start_result`, `ResultContinuation` and a static `map_dependencies` mapper. The Result C operation table is the sole C operation-plugin interface; consumers must rebuild against the installed package because the C++ API and the exact-size C declarations are the contract. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) (Proposed) records a prefix-negotiated C ABI for all operations; package 0.38.0 does not implement it.

The compiler copies immutable schema into the plan and compiles every tile instruction with its fixed read grants. The execution coordinator executes those instructions in plan order, owns the Run's values and state, and performs callbacks and I/O. A `ResultRef` owns its schema, monotone descriptor facts, and certified backing. Copies of a reference share this ownership. An external reference or read window keeps required backing alive after the execution context ends. Dependency facts are not part of a Result: the execution records the compiled reads of each executed tile in a `TileRecord` (see [Tile records and frozen evidence](#tile-records-and-frozen-evidence)).

```cpp
struct ResultTensorLayout final {
  bool spatial = false;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
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
  ResourceVector<ResultPublicationScope> publication_scopes;
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
```

The excerpt omits member functions; the declarations are in `photospider/data/result.hpp` and `photospider/execution/execution.hpp`. A workflow input with `result_schema` binds an owning `ResultRef`; execution returns named Result outputs in `ExecutionResult::results`. Tensor sample coordinates concatenate `batch_axes` with the descriptor's cell axes. `ResultTensorSpec::metadata_owner` is a resource lease that pays for the spec's managed nested containers; it is budget ownership, not attached interpretation. `id` and `version` name an explicit structural contract and enter compile identity; the kernel does not dispatch on them to a domain meaning.

Complete structured execution may compute a non-security `result_digest` over named Result outputs in `ExecutionResult::results`: canonical schemas and certified tensor coverage and sample bytes in logical order. It excludes object identity and physical layout; it is not authentication. Fragment execution leaves this digest empty. `ExecutionOptions::maximum_result_digest_samples` defaults to 65,536 and limits the certified tensor samples admitted for the digest; zero disables digesting. Before scanning, the coordinator also checks remaining Run-work and tensor coverage-count representability. If the complete digest does not fit these estimates, it omits the digest and continues with the execution result. For example, a large certified broadcast view need not be traversed once per logical sample solely to create a digest. After scan admission, digest reads use bounded windows and consume Run-work; cancellation, stale state, Root/resource, or read failures during that scan remain execution failures. The coordinator rechecks cancellation and stale state before returning success.

## Data Layout & Memory

`SchemaTemplate` has 1..16 Tensor members. Every member has a fixed positive shape known before execution. No member shape, member count, or row count depends on runtime data. The canonical schema encoding domain is v6; it covers members and publication scopes, and excludes object generations and descriptor revisions. Its structural identity encodes schema id/version, member keys, dtype, cell extents, batch/atomic axes, optional spatial axis indices, and publication scopes. `SchemaTemplate` validates fixed shape and scope declarations without interpreting domains or prescribing storage order and row pitch. `ResultRef` captures actual coverage, descriptor revision, physical strides, and backing owners.

`ResultTensorSpec` describes one typed Tensor member. Its cell descriptor has rank 1..8 with positive extents; `batch_axes` adds a positive-extent prefix, and the combined sample rank is at most eight. `atomic_trailing_axes` closes that many trailing cell axes into one indivisible observation. Schema validation does not impose a dense allocation or a representable full-domain element product. `ResultTensorLayout` records an optional spatial layout. A spatial layout maps height, width, and optional channel axes within the cell descriptor, independently of the batch prefix. A spatial tensor may have any legal channel count; the kernel does not infer RGB, alpha, a color model, or a numeric range from the channel count or layout.

`atomic_trailing_axes` defines observation completeness: sample regions close over those trailing axes. Payload authorization additionally follows member tile geometry. A spatial tile covers every non-H/W cell axis completely, including the channel axis, even when `atomic_trailing_axes = 0`. Explicit batch axes select separate coordinates. This geometric coverage does not infer RGBA or alpha semantics from the schema id, shape, or spatial flag. Logical tuple completeness is independent of physical byte order: in the default tiled storage, one pixel's channels are complete in the logical tile but lie in separate planes of that tile. The canonical image target in [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) D9 uses explicit logical axis roles and does not mandate HWC.

```cpp
// Selected public methods from photospider/data/result.hpp.
class ResultBuilder {
 public:
  static Result<ResultBuilder> start(ResourceBudget budget,
                                     const SchemaTemplate& schema,
                                     std::string_view scope,
                                     ResultGrowthLimits limits = {},
                                     std::uint64_t tile_extent = 128);
};
class ResultRef {
 public:
  bool matches_scope(std::string_view scope) const noexcept;
};
```

`ResultBuilder::start` accepts a scope of 1..4096 bytes. It copies the scope into Root-accounted storage, charges the copy work, and assigns a new ObjectId. `ResultRef::matches_scope` compares the stored bytes exactly with the supplied string. The execution host separately checks Root ownership, structural schema, producer scope, revision progression and publication coverage; a matching scope alone is insufficient. This producer scope is distinct from `SchemaTemplate::publication_scopes`, which controls tile visibility.

In compiled execution, the producer scope is a Run-local invocation key derived from the plan digest, the Run, the block activation, and the instruction's call, stage, output, member and tile. It identifies one publication attempt; content reuse uses the separate cache identities below. Physical page size, file offsets, descriptor revision and resource limits do not change semantic identity. `ResultRef::capture()` snapshots the certified descriptor at one revision, so an earlier capture does not widen as production advances. `ResultRef::read_tensor()` requires that descriptor and reads only certified samples. `ResultDescriptor` stores one coverage `Footprint` per member. `ResultTensorInput` carries the compiled grant, member slot, descriptor and authorized samples into the callback; the phase is borrowed for one poll, while retained capabilities follow their own lifetime rules.

`ResultObjectInputs` separately keeps the current owning ResultRef per input port across polls; the host clears that map when the instruction completes or retires. Stored `ResultTensorInput` capabilities own their ResultRef and captured descriptor, and acquired read windows own the backing authorized by that grant. A physical tensor view independently retains its source Results, schemas, and backing.

### Explicit tensor views and publication ownership

```cpp
// Public Result view records (abridged; see photospider/data/result.hpp).
struct ResultViewAxis {
  std::int32_t output_axis = -1;
  std::uint64_t source_origin = 0;
  std::int64_t step = 1;
  std::uint64_t output_origin = 0;
};
struct ResultTensorViewTransform {
  std::vector<ResultViewAxis> source_axes;
  bool reshape = false;
};
struct ResultTensorViewPiece {
  const ResultTensorReadWindow* source = nullptr;
  Region region;  // Target coordinates.
  ResultTensorViewTransform transform;
};
```

`ResultBuilder::publish_tensor_view(slot, region, source, transform)` takes a borrowed, grant-authorized `ResultTensorReadWindow` and an explicit `ResultTensorViewTransform`. With `reshape=false`, `source_axes` contains one `ResultViewAxis` per complete source sample axis, including batch axes. For absolute target coordinate `output`, the anchored mapping is `source = source_origin + step * (output - output_origin)`, with checked arithmetic. `output_axis=-1` or `step=0` fixes the source coordinate at `source_origin`. Target coordinates include nonzero region offsets: with `source_origin=10`, `step=-2` and `output_origin=5`, target positions 5 and 6 read source coordinates 10 and 8. Identity, channel permutation and broadcast use this same explicit mapping.

With `reshape=true`, `source_axes` is empty. The complete source window and target have equal logical sample counts, and physical signed/zero-stride chunks must permit an affine join. A logically valid reshape may return `ViewUnavailable` when it requires copying.

For multiple sources, each `ResultTensorViewPiece` supplies a borrowed window, a nonempty region in target coordinates, and an explicit affine transform. `publish_tensor_view(slot, total_region, pieces)` requires at least one piece. All piece regions must be disjoint, contained within `total_region`, and cover it exactly.

**Atomic closure applies to the complete `total_region` and the union of its pieces, not to individual pieces.** For example, a target of shape `[1,2]` with the last axis atomic can publish one two-channel tuple from pieces covering channels `[0,1)` and `[1,2)`. Each piece covers half a tuple, but their union covers the complete tuple. Each piece still requires valid source authorization and affine mapping; the multi-piece overload does not support reshape.

The C++ vector overload has no fixed piece-count limit, so Root work and metadata admission bound its size; the C ABI 7 bridge accepts 1..16 pieces. An empty piece vector, a NULL `source`, `reshape=true` in a piece, or a piece region that leaves `total_region`, overlaps another piece, or leaves part of `total_region` uncovered fails with sticky `TypeMismatch`.

The builder validates the entire logical partition, dtype, source permissions and each mapping before preparing physical spans. It then checks ownership cycles across all retained source Results, including Results from different Roots, and calls `publish_tensor_spans` once. Success certifies coverage and advances revision once while retaining the source backing after the borrowed windows retire; no intermediate piece is published.

The view path copies no payload. Its target Root charges metadata, geometry/work and publication, while source-backed bytes remain charged to their owning Roots; directly supplied external storage without Result source owners follows the distinct `reference(storage)` admission path.

The cycle check runs even after a piece mapping reported `ViewUnavailable`, so a source chain that reaches the target Result fails with sticky `TypeMismatch` instead of offering a copy retry. Logical authorization, overlap, owner cycles, resource failure and cancellation are errors and cannot become an automatic copy fallback. Only a physically unrepresentable but logically valid mapping reports nonsticky `InvalidArgument` with `FailureReason::ViewUnavailable`, leaving existing coverage/revision and earlier captures unchanged so the operation may explicitly materialize the requested region. Other failures are sticky; cancelled or failed transactions cannot expose a partly assembled view.

## Scheduling & Publication

### Computation units and tile grids

Each plan step declares one selected named output with its own resolved output contract. Every member of that output has its own fixed tile grid, and every compiled tile of a member is one plan instruction. Kernel execution has no whole-result mode: an algorithm that needs a complete input or output expresses that need through mapped member tiles or through a Run-shared stage (see [Run-shared stages](#run-shared-stages)).

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

The planner emits one instruction per reachable unit `(step, shared stage, member, tile)` and deduplicates repeated requests for the same unit, so a tile read by several consumers runs once per block activation. The instruction's query carries `tensor_slot`, `tile_unit`, `tile_extent`, and a `tensor_outputs` footprint equal to the unit's tile region. Every ordinary tile instruction's `ResultPublication` must set `complete = true`, cover exactly the selected member's query tile, and leave every other member empty. A partial or nonfinal publication fails validation. A continuation may use several polls; progressive Result publications belong to shared-stage execution.

`ExecutionPlan::structured_templates()` holds one static template per step, computed once by the compiler. A template digests the tile extent, execution mode, backend, operation key, output index, resolved traits, selected resource identities, parameters and input schemas. It contains no graph, node, step or plan identifier, so two equivalent calls at different steps share one template.

### Needs and grants

The operation's static mapper returns `TileNeed` records at compile time, and the planner turns them into the instruction's `PlanTensorRead` grants. Each `TileNeed` has exactly one form:

- A payload record names one input, one member slot, and member-local tile intervals. The planner validates the intervals against the compiled grid of that producer member. A payload grant also authorizes the input's static descriptor facts.
- A descriptor-only record has `tensor_slot = 0` and no intervals. It grants the input's compile-time descriptor facts with object id 0 and no payload.
- A payload record with zero intervals is valid and requests nothing. The planner drops it: it creates no dependency, no descriptor grant and no sample grant.

The planner merges records for the same input and member, closes each grant over publication scopes and complete-validation requirements, and records the producer tiles as instruction dependencies. At run time, the coordinator assembles the granted producer tiles into one Result of tensor views and grants the union of their regions, closed over `atomic_trailing_axes`. A callback may still return `ResultProgramNeed`, but the coordinator only checks that its records lie inside these grants; it never starts a producer. When an instruction completes, the coordinator installs the union of its grants, the control evidence of the activation and the evidence of any block parameters it read as the tile's `TileRecord`. Intervals merge only within one producer and member; the record retains no payload. `ExecutionDependencies::tile_record()` returns the record of a completed tile, and `dirty_units()` follows reverse edges from changed producer tiles or descriptors to the recorded tiles that consumed them. Each named return also has a synthetic record at index `steps().size()` plus its return ordinal.

Full tile publication is atomic: a tile either becomes visible with complete coverage or not at all. Completion of one producer tile alone does not establish complete coverage of its output; coverage is the union of the published tiles. A published tile's dependency record is fixed by its compiled grants and cannot change afterward.

### Publication scopes

`SchemaTemplate::publication_scopes` statically declares visibility rules over complete members. Member slots and scopes are strictly sorted and disjoint, and at most 16 slots are listed. Members not listed in any scope use `IndependentChunks`.

| Policy | Members | Visibility |
| --- | --- | --- |
| `IndependentChunks` | Any | Each completed tile becomes visible independently. |
| `CompleteBundle` | One or more | The listed members become visible together after every tile of every listed member is complete. |
| `StablePrefix` | Exactly one | Complete tiles become visible in local tile index order. |

Scopes control visibility, not read authority. The planner turns them into compiled closures: a read that touches a member of a `CompleteBundle` depends on every tile of the whole bundle, including members that were not requested, and a read of a `StablePrefix` tile depends on every earlier tile. During publication the coordinator also verifies the expected schema, Root ownership, producer scope, revision progression, and output tile coverage; matching a producer scope by itself never admits a Result. Aggregate validation and dependency certification precede consumer visibility; no bundle member is visible early. The returned Result still contains only the requested selection.

When `ExecutionOptions::result_publication` is set, an output whose scopes are all `IndependentChunks` and that has no complete-coverage output validator is validated and announced tile by tile as each instruction is certified, including tiles adopted from the completed-result cache. Validation or observer failure stops the Run before the next instruction. `CompleteBundle`, `StablePrefix` and complete-validation outputs are announced once, after the call's compiled aggregate has been certified. The notified object and the projected object later returned under the output name need not share an `object_id`; consumers compare coverage, content and owner semantics.

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

The stage contract is static. Preparation fixes its schema, input projection, backend, state, workspace, work, and output payload bounds, and these enter operation identity. The planner canonicalizes stage units to the call's first step, so all outputs and tiles of one call reach the same stage instructions. Within one block activation the coordinator keeps one stage state per call and stage; entering the block again clears it, so each loop iteration runs its stages again. The input projection limits which inputs the stage's mapper may name; it does not grant reads. The stage reads only the grants its own mapper produced. All stage bounds are cumulative for that stage in one activation, and work bounds include input and publication validators.

A consumer reads a stage only when its mapper returns a `ResultSharedNeed` for it. An absent `tiles` selection requests every member tile of the stage; an explicit empty selection grants no payload. The planner closes the selection over the stage's publication scopes and complete-validation requirements and makes the consumer depend on those stage instructions, which run earlier in the same block. A dependency cycle between stage and tile units fails planning with `Cycle`. Each visible stage commit freezes its dependency evidence at commit time. A consumer receives the stage Result restricted to its grant and that frozen history; it does not receive authority to read the stage's original inputs.

A stage failure fails the instruction that executes it and therefore the Run, and cancellation retires the stage after admitted callbacks return. A stage is not spilled and not shared across Runs. The planner compiles every `mandatory` stage of every call as an effect root, even when the plan selects no output tiles, and `select_outputs` keeps them; a mandatory stage runs whenever the block that contains its call executes. An operation that is not side-effect-free must declare at least one mandatory stage, and an operation that is not both deterministic and side-effect-free cannot be cacheable. An operation declares at most 64 stages.

Under `ExecutionMode::CpuExact`, planning fails with `BackendUnavailable` when it emits an instruction for a GPU stage. Plans that only read stage metadata are unaffected. When an ordinary step or a stage uses the GPU backend, the Run creates its native upload state. A GPU stage has no compiled CPU fallback; stage arithmetic is never retried on another backend.

A direct `ResultContinuation` for a stage applies the same static visibility rules. A hidden intermediate commit returns an empty Need so the host can poll again. A returned nonfinal publication is a `capture()` at one revision, so a delta rejected later cannot appear through an earlier `ResultRef`. If a validator rejects a later tile, the host retires the projected producer with that failure: `production_status()` of an escaped prefix reports the failure before and after continuation destruction, while previously certified tiles remain readable.

### SDK acquisition helpers

The C++ SDK headers `photospider/plugin/tensor_helpers.hpp`, `tile_helpers.hpp`, and `whole_stage.hpp` provide nonblocking helpers. Each helper returns either a Need for the callback to return from its poll, or a ready value; none executes a producer, waits, fills missing samples, or collects dense bytes implicitly. Under the static VM, grants are bound before the first poll, so a helper usually returns its ready value on the first call; a returned Need only asserts the compiled grants. `tile_helpers.hpp` also provides the static mapper helpers `sdk::static_query_view`, `sdk::tile_need_for_samples(const ResultStaticQuery&, ...)` and `sdk::pointwise_dependencies`.

| Helper | Ready value | Use |
| --- | --- | --- |
| `sdk::to_whole(phase, input)` | `WholeTensorInputs`, one complete capability per member | Acquire every member of one input. |
| `sdk::to_whole(phase, input, slot)` | `WholeTensorWindows` covering the member | Acquire one complete member as owning read windows. |
| `sdk::to_whole(phase, input, slot, samples)` | `WholeTensorWindows` clipped to each box of `samples` | Acquire a member ROI. An empty `samples` returns an empty ready vector and grants nothing. |
| `sdk::to_tile(phase, source, member)` | A Result of zero-copy views covering the query tile | Publish the selected output tile from an owned Result or stage Result. |
| `sdk::collect(phase, input, region)` | `TensorSpan` | Explicit dense copy of one authorized rectangle. |
| `sdk::publish_dense_tensor(...)` | Publication through `ResultBuilder` | Explicit dense output allocation for one rectangle. |

The ROI overload of `to_whole` rejects an already cancelled query at entry and returns a compact tile Need while any selected sample lies outside the bound grants; with a sufficient compiled grant it is ready immediately. Complete acquisition does not imply dense collection; dense copies are an explicit operation decision.

`sdk::with_whole_stage(definition, work_bound)` adapts a single-output CPU operation whose algorithm needs its complete output into one Run-shared stage. It wraps the definition's mapper so the stage receives the original input reads and each output tile reads the complete stage. When a block activation executes the stage, the original factory runs once with the complete output schema and no selected tile; it remains responsible for its numeric policy and resource bounds. Output tile callbacks then publish retained views of the stage Result through `sdk::shared_result_tile`. This is an SDK adapter chosen by the operation author, not a kernel execution mode. Operations with several independent outputs use separate stages with narrower input projections; operations whose outputs are computed jointly, such as a stable sort, use one joint stage.

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

Every payload dependency also observes descriptor changes. `TileEvidence` is immutable dependency evidence for one stage commit: it owns that commit's input grants and references earlier frozen evidence. It is never a read capability. A consumer of a stage tile keeps one reference to that node. The execution table imports each distinct node into the target Root once, preserving its identity, indexes reverse edges from each node once, and enumerates workflow dependencies from one locked table state. Nodes retire by reference count. These rules avoid copying the evidence of P stage tiles into each of Q consumers.

### Mandatory effects and repeated inputs

The planner compiles every call that is not side-effect-free and every mandatory shared stage as an effect root, including when the plan's output selections are empty. `select_outputs` and an explicit `DemandQuery` keep those roots, together with every branch condition and edge argument, so an empty selection omits pure output work while control flow and mandatory effects still run on the path that executes. An effect in a branch that is not taken does not run. Mandatory stages in a block complete before the block's terminator is evaluated. Effectful work is not shared or cached across Runs. Failure or cancellation of an executed mandatory stage fails the Run.

Result C ABI operation definitions can declare a fixed input prefix and one repeated trailing group. `input_count` and `inputs` describe only the prefix; `repeated_input` supplies the prototype constraint applied to each actual trailing input during metadata resolution and `start`/`poll` query validation. A nonzero `repeated_maximum` enables the group and requires `1 <= repeated_minimum <= repeated_maximum`, with the prefix plus maximum no greater than 1,024 inputs. When disabled, all repeated fields are zero or null. `repeated_match` is 0 or 1: value 1 requires every repeated tensor to match the first repeated tensor's dtype and complete logical `sample_shape()`; value 0 requires a pure metadata resolver for the expanded input list. The C++ registry keeps the unexpanded template and resolves the actual input count during preparation. This declaration constrains metadata only; the operation's mapper still grants every actual input it reads.

### Native GPU input access

A native GPU callback can call `ResultProgramPhase::acquire_native_atlas(input, slot)` for a compiled payload grant. It returns an immutable `FragmentAtlas` containing exactly that grant's coverage; the operation still decides how to dispatch and publish. Calls for the same port and slot within one poll reuse one atlas. CPU access, a missing grant, or a descriptor-only grant produces sticky `InvalidArgument` and does not widen authorization. Root-backed payload and directory storage is separate from operation workspace, and the atlas owns those buffers without retaining the source Result or read windows. A caller-held shared atlas can remain alive after the poll, source, and execution context end.

Native GPU input transport reuses raw physical backing within a Run only while a weak proof still identifies the same live CPU storage and the complete view key matches. An optional byte-bounded context cache can reuse uploads across Runs by device instance/build and logical input bits, Region, dtype and shape; it excludes the Result schema, then rebinds a hit to the current schema. Cache hashing consumes the coordinator shared cache-work budget, and optional-work exhaustion skips the cache path. Capacity reclamation first clears retransmittable Run-local entries, then evicts retained context entries. `host_access_count` records actual CPU exposure of live native-backed storage once per storage in a callback scope; metadata inspection, native-address acquisition, view forwarding and hashing do not count. Native GPU service failures are latched when the service call returns. A later `consume_work` exhaustion or callback status cannot replace that earlier service error; if work exhaustion occurs first, a later service error does not replace it.

Result atlas packing accepts either a `Tensor` or a `ResultTensorInput` capability. The lower-level `FragmentAtlasPlan` can pack a `Tensor` with empty coverage without reading source samples. The Result overload reads through grant-authorized owning windows, so signed and broadcast strides, fragmented coverage, and batch-prefixed spatial tensors are handled without dense conversion. `preparation_work()` and `materialization_work()` report generic geometry and packing units. Result preparation precharges its generic work through `FootprintLimits::consume_work`; Result materialization additionally precharges window acquisition and read-bound work through that callback. `materialization_work()` is not the complete Result-overload charge.

### Cross-output block-state sharing

Block-state retention can reuse a completed state when the current request and proof match. A retained state is an execution implementation detail; cache-hit counts and block transitions are not a public performance guarantee.

Pure deterministic Result operations may opt into `share_blocks_across_outputs`; registration rejects the opt-in for an operation that is not both deterministic and side-effect-free. The C Result ABI 7 exposes the same opt-in as `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V7` (flag bit 5).

The shared namespace includes the operation and all resolved output contracts, backend, execution mode, selected resource identities, static parameters, and actual input metadata. Each block key also includes its kind, range, mode, incoming state schema/coverage/content, and the currently supplied tensor metadata, coverage, and bytes. The selected output index and output-specific metadata are not appended independently. A computation shared by different outputs must therefore be output-independent or encode the distinction in its state or mode. Public output Results, tile records, checkpoints, and completed-result caches remain output-scoped and independent.

Shared stages expose no checkpoint or block callbacks. For ordinary tile instructions, block-state lookup and retention require a pure instruction: a deterministic, side-effect-free operation, a pure producer closure over the granted payload inputs, pure control evidence, no fallback taint, no shared-stage reads, and no payload read of a block parameter. Block reuse does not require the operation or its ancestors to be cacheable; that requirement applies only to completed-result retention. The block key starts from a block contract (operation, execution mode, backend, tile extent, output index unless shared, traits, resources, parameters and input schemas) and adds the instruction's fixed-grant content proof (`photospider.fixed-block-inputs.v2`), the block kind, range, mode and incoming state. Retention is optional and charged to bounded cache work and Root capacity. A GPU block is retained only when the compute callback actually dispatched native work during that block, so a GPU path without a native computation proof is never stored. If lookup or retention is unavailable, the callback computes the block normally. A lookup hit whose cached state cannot be unpacked because of capacity is treated as a block miss and computes normally. Work limits, cancellation, and protocol failures during unpacking remain fatal. Each output still executes its own instructions and records its own `TileRecord`. The flag does not guarantee one computation per Run.

### CPU fallback

CPU fallback exists only where the planner compiled it: the instruction's `cpu_fallback` flag is set when the GPU-placed operation permits fallback, supports CPU, is deterministic and side-effect-free, has no shared-stage reads, and its CPU mapping fits inside the GPU grants. When a GPU start or poll then fails with a retryable `BackendUnavailable` before any publication, input read, native dispatch, checkpoint, block or GPU selection, the coordinator gives the retry fresh failure latches, drops the attempt's continuation, and restarts the same instruction on CPU with the same bound grants. Producer tiles that already completed keep their published Results and records. Spent Root work is not refunded. The rejected GPU attempt keeps its `OperationTiming` record, and the CPU attempt adds its own records. The retried tile is fallback-tainted and impure for optional reuse, so its value and every consumer that reads it skip checkpoint lookup and publication, block-state reuse, and completed-result retention.

### Checkpoints

Ordinary tile instructions of pure, deterministic Result programs can use the C++ `ResultProgramPhase::checkpoint_publish` and `checkpoint_before` callbacks to retain continuation state; either call from a shared stage or from an operation without both purity traits is a protocol failure. Lookup and retention also require a pure instruction (pure producer closure and control evidence, no fallback taint, no shared-stage reads, no payload read of a block parameter) and remaining optional cache work. Otherwise lookup misses and publication declines without failing the operation. Checkpoints do not require the operation or its ancestors to be cacheable.

A checkpoint scope is a content identity. Its key is the instruction's fixed-grant content key (`photospider.fixed-grant-cache.v2` over the step's structured template, backend, member, tile, publication samples, descriptor-grant ports and the granted input schemas and sample bytes) with a `/checkpoint` suffix. It contains no physical step, node or plan identifier, so two equivalent calls at different physical steps, or the same call in a renumbered or reloaded plan, share one scope. `checkpoint_before(phase, before)` returns the greatest published sequence in that phase not greater than `before`. A published checkpoint owns a sealed state Result and a payload-free witness holding the publishing tile's `TileRecord`.

Restoring a checkpoint restores state content only. The current instruction keeps its own fixed grants, dependency evidence and `TileRecord`; an earlier witness never replaces them and never imports another call's physical tile identity. Restoring does not populate the current phase's `results` or `tensors`, recreate read windows, or grant read authority; the callback reads its current inputs through its bound grants. A capacity refusal returns an empty checkpoint and leaves the instruction unchanged. Work limits, cancellation, and protocol failures remain errors.

The context directory holds weak references to checkpoint scopes, while an active execution holds its scopes strongly; `ExecutionContextConfig::maximum_result_checkpoint_scopes` bounds the live scope count. Each scope retains at most 64 entries, and the summed witness weight of a scope stays within the dependency `maximum_boxes` limit; the oldest entries retire first. The runtime charges optional checkpoint work before constructing lookup keys or copying witness records. Zero or insufficient optional cache work skips lookup or declines publication; it does not copy unadmitted witness data. A copied `ResultCheckpoint` owns its state and witness even after the index or context is cleared. The C Result ABI exposes `checkpoint_before`, `checkpoint_read`, and `checkpoint_publish` through poll-scoped opaque handles and sealed generic state Results; see the [C checkpoint services](Dependency-Data.md#c-checkpoint-services) contract.

Checkpoint and block eligibility follow purity independently of completed-result caching: a pure instruction can use both services even when the operation or one of its ancestors disables completed-result retention with `cacheable = false`. Both remain subject to the instruction restrictions above. An operation that declares `deterministic = false`, for example one whose output contains a Run-local ObjectId, makes every dependent consumer impure and excludes it from checkpoint lookup and retention and from block-cache admission.

### Completed-result cache

The completed Result cache reuses the sealed Result of one tile instruction across Runs, binding generations and equivalent plans when the instruction's fixed inputs have the same content. The lookup key is the fixed-grant content key (`photospider.fixed-grant-cache.v2`). It digests the step's structured template, the instruction's backend, member slot, tile index and publication samples, the ports of descriptor-only grants, and for each payload grant its port, member, canonical schema and the granted sample bytes. It contains no physical node, step or plan identifier, so a renamed, renumbered or reloaded plan, or an equivalent call at another step, can reuse an entry. A plan whose edited descriptor grant names a different port produces a different key and cannot collide with another plan's entry. Each key keeps up to eight candidates, and a candidate is considered only for the same backend.

A candidate that matches is rebound with a new ObjectId and the current producer scope and validated like a computed publication, including the output payload bound. Input validators have already run on the instruction's bound grants, so an invalid workflow input still fails with `InvalidArgument` before the cached output is used. The Run then installs the tile with dependency evidence rebuilt from the current plan's fixed grants; the cached entry contributes content only. Cache hits produce no `OperationTiming` record and add zero `computed_elements`. The cache entry retains the physical backing owners it references; rebinding does not make the old Result wrapper or old execution scope the identity of the new output. Content identity does not make stage digests stable: semantic, optimized and plan digests include program identifiers and change when calls or values are renumbered.

Structured Result entries share the bounded cache LRU and managed Root capacity. The LRU holds at most 4,096 entries, `ExecutionContextConfig::result_cache_bytes` bounds retained backing (zero disables retention and block reuse), and `maximum_dependency_cache_metadata` bounds manifest metadata. Capacity accounting deduplicates physical backing owners, including backing reached through tensor views and affine owners, rather than summing logical tensor sizes. `ResultCacheStatistics::native_retained_bytes` counts unique native allocations held by the cache, covering both retained GPU outputs and retained GPU block states; it is not a block-only capacity. Eviction releases the cache's owner references; callers holding a Result or read window continue to own its backing. `clear()` advances the cache epoch, so an in-flight producer from the previous epoch cannot refill the cleared cache. Key construction, sample hashing and rebinding consume the Run's optional cache work (`ExecutionOptions::maximum_dependency_cache_work`). Insufficient optional work or Root capacity skips retention or produces a miss without changing an otherwise successful computation.

Retention and lookup require a cacheable instruction: a pure instruction (see block-state sharing above) whose operation is `cacheable`, whose payload inputs come from workflow inputs or cacheable producers, whose control evidence is cacheable, and that reads no block parameter payload. The coordinator retains a tile after its call's value is certified, or immediately for an individually certified tile. Shared stages, instructions with shared-stage reads, fallback-tainted tiles and values carried through block parameters bypass completed-result caching. The C Result ABI does not expose this cache; its implementation is private to the C++ execution runtime.

### Result builder

`ResultBuilder` owns publication. The producer writes tensor regions through `publish_tensor`, `publish_tensor_view`, or `publish_tensor_kernel`. Tensor regions are checked against the member's logical sample domain. Overlap, out-of-domain access, or failed resource admission retires the producer with an error. `ResultBuilder::start()` takes the query's tile extent; an invalid spatial tile extent returns `InvalidArgument` before allocation.

Publication keeps physical arrays and backing owners in the Result-derived storage owner, while publication state owns the Root, resource leases, and schema. Destruction retires array backing before the publication state releases its Root leases. Storage-owner indexing avoids rescanning previously published backing for each publication, and `acquire_tensor` locates pieces by an interval search when the member's partition permits it, otherwise by a bounded scan. `acquire_tensor` copies immutable backing handles while holding the publication mutex, then unlocks before constructing the owning read window. `publish_tensor_kernel` prepares callback work outside the mutex, then reacquires it to validate the owner, revision, and sticky failure before committing. A read capability retains its authorized Result and backing, but grants reads only within the captured descriptor and compiled grant.

The publication validator receives a `PublicationValidationView` and a borrowed `PublicationValidationServices` interface for one synchronous validation. The coordinator owns cancellation, work admission, current limits, and the instruction state. Validation checks publication identity and revisions; it does not retain the services or perform scheduling, publication, retirement, or notification. The coordinator applies the returned status and retirement intent after validation completes.

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

`ResultObjectInputs`, `ResultTensorInput` capabilities, and retained windows provide the active owner and authorization paths described above. A physical tensor view also retains every source Result whose bytes it publishes. Immutable operation resources, such as ICC profile or OCIO configuration bytes, are not Result members. Static preparation returns the explicit `ResourceIdentity` list it needs; the compiler selects exactly those entries from the `ResourceSet` passed to `Compiler::compile`, fails if one is missing, and the plan and execution state retain the selected set. A Result therefore never keeps a color configuration alive on its own. Public `ResourceMap` and `ResourceVector` containers also carry their allocator and managed ownership; copied results, names, and observations retain root-accounted metadata until those containers and any contained `ResultRef` owners are released. Consumers must budget these copies along with payload. The policy is conservative, so live owners can retain more backing than the final output alone needs.

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
| Input tensor grants | The payload grants of one input port bound for one instruction, before its first poll. | Every member of the input schema is fully granted. | Workflow input: `InvalidArgument` with the external `input_id`. Produced input: `OperationFailed` with the upstream `node_id`. Origin `Domain`. |
| Output publication | The call's compiled aggregate when its value is first assembled, or each certified tile when the output is announced tile by tile. | The validated coverage equals every member's full domain. | `OperationFailed`, origin `Domain`. |
| Shared-stage commit | Each newly visible stage commit, before supply to consumers. | The visible stage coverage is complete. | `OperationFailed`, origin `Domain`. |

A validator with `requires_complete` sees only complete coverage; an incomplete context returns `TypeMismatch`. Completeness is compiled. When a consumer port has a complete input validator, the planner grants every tile of every member of that input to any instruction that reads it. When a producer output has a complete output validator, the planner makes any nonempty read of that output depend on every producer tile, so the validator observes the actual compiled aggregate of all executed tiles and never infers additional producer work from an incomplete publication. Descriptor-only and empty payload grants add no completeness. A complete stage validator delays visibility until full static stage coverage; a stage that ends with incomplete coverage fails. A plan whose stored grants omit required complete coverage fails import validation.

A completed-result cache hit still runs the input validators on the instruction's bound grants without repeating the computation. An invalid workflow input therefore fails with `InvalidArgument` before the cached output is returned, and a changed static parameter that a validator prepares from produces a different plan cache key.

The first service failure inside one `validate` call takes priority: if a read fails and the callback ignores it, the call still returns that read failure. A failed validator service cannot publish a result. Exceptions thrown by `prepare` or `validate` become typed failures. `ValidationDefinition::owner` retains the callback code; the host destroys it only after the callback objects and the prepared validator state, so a prepared operation keeps working after the caller releases its registry handle.

`OperationTraits::diagnostics` lists the report types an operation may emit. Before callbacks run, the host initializes a Root-owned `DiagnosticAccumulator` for those types. A callback reports a fixed-size byte record through `ResultProgramPhase::report_diagnostic` (C: `report_diagnostic`) by declared ordinal; `DiagnosticDefinition::validate` checks it, and `merge` writes a complete new record without changing either input. A failed report leaves the accumulated bytes unchanged. `OperationTiming::reports` carries the owning accumulator; `find(identity)` returns the merged bytes, or `NotFound` if the type never reported. The kernel interprets none of these bytes. The numeric work report is one such type, defined in the SDK header `photospider/ops/numeric/diagnostics.hpp`.

An operation can attach an opaque domain code to a failure. `Status(ErrorCode, message, DomainErrorCode, FailureDetail)` stores it in `FailureDetail::domain_code`; C callbacks use `fail_domain(status_code, domain_code)` on the run or validation services. The kernel preserves the code without interpreting it, and the first recorded domain failure is not replaced by later callback or diagnostic failures.

## Limits & Failure Handling

Every `ExecutionContext` creates a managed `ResourceBudget`. If `ExecutionContextConfig::managed_resources` is unset, the context uses default-constructed `ResourceLimits`; an explicit value supplies the configured capacities. `maximum_live_bytes` also caps the Payload dimension. `ExecutionContext::resource_budget()` returns a shared handle for clients that need to account temporary storage, and its leases may outlive the context.

The execution root accounts for payload/backing, work, stages, continuations, queue entries, metadata, and retained owners under their configured resource dimensions. Bounds are finite. Oversized payload, or owner admission fails with a resource error rather than returning an incomplete witness. This managed-capacity model covers instrumented resources; it is not an RSS bound.

An output can declare `maximum_output_payload_bytes`, and a shared stage declares its own bound. The host counts newly owned payload cumulatively across the tile instructions of one logical output in one Run, counting each unique physical owner once. It excludes borrowed authorized owners: source allocations that remain live through exposed Results, authorized tensor grants, or supplied stage Results. The check keeps only weak source references, so the cap does not keep input payload alive. The first eligible poll allocates the cap guard and shared control block from the Result Root before entering the callback; insufficient Metadata capacity rejects the poll before callback entry. Owner enumeration and comparison consume normal Run and Root work. Exceeding the cap returns `ResourceExhausted` with `CapacityLimit`. Workspace, metadata, and operation-resource admission remain separate. The check runs when a Result is published, so a trusted callback's earlier Root allocations are still charged and are not prevented by the cap.

Result publication errors are sticky. Cancellation and stale execution retire active state only after admitted callbacks have returned. A failed append or tensor publication cannot be repaired by a later successful callback; a previously certified prefix remains immutable and readable under its captured descriptor. The last owning Result/read-window release retires backing.

Behavioral regression tests are registered with the kernel build. The registrations under `tests/unit/kernel` and `tests/integration/kernel` are the current source for executable coverage.
