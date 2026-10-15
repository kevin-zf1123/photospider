# Structured Results and tensor slots

## Scope & Ownership

`ResultRef` is the publication, ownership, and input/output path for structured tensor data and image data. A Result contains typed tensor slots, packed primitive fields, or both. A tensor slot may declare a spatial layout and use internal planar typed backing; this does not create a separate image result path. `Value` is backing storage, not a workflow or operation input/output form.

A Result carries structure, data, and the owners its storage needs. It has no attached domain description, no semantic kind, and no facet list; the kernel does not propagate color, alpha, transfer, or sampling descriptions from inputs to outputs, and it does not match such labels on connections. Two Results with the same structure connect to the same port even if the caller assigns them different meanings. An operation that needs a domain interpretation receives it as an explicit static parameter, and it can prove sample legality through a bound validator (see [Port validators and diagnostic reports](#port-validators-and-diagnostic-reports)).

The current package is 0.34.0, WorkflowDocument schema is 6, OperationTraits version is 26, the Result C operation ABI is 4. These version numbers describe independent contracts. The C++ operation interface uses `start_result` and `ResultContinuation`. The Result C operation table is the sole C operation-plugin interface; consumers must rebuild against the installed package because the C++ API and current-sized C declarations are the contract. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) (Proposed) records a prefix-negotiated C ABI for all operations; package 0.34.0 does not implement it.

The compiler copies immutable schema into the plan. The execution coordinator owns producer state and schedules callbacks and I/O. A `ResultRef` owns its schema, monotone descriptor facts, certified backing, and source-association metadata. Copies of a reference share this ownership. An external reference or read window keeps required backing alive after the execution context ends. Association metadata identifies consumed inputs; it does not own their payloads. Dependency facts are not part of a Result: the execution records the actual Needs of each computation unit in a `TileRecord` (see [Computation units and tile records](#computation-units-and-tile-records)).

```cpp
struct ResultTensorLayout final {
  bool spatial = false;
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
  std::vector<ImageComponentGroup> groups;
};

struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultTensorSpec> tensors;
  ResourceVector<ResultExtent> domain;
};

struct WorkflowInputDeclaration final {
  std::uint64_t id;
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema;
};

struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
```

The excerpt omits default member values and unrelated declarations. A workflow input with `result_schema` binds an owning `ResultRef`; execution returns named Result outputs in `ExecutionResult::results`. Tensor sample coordinates concatenate `batch_axes` with the descriptor's cell axes. There is no separate image output map. `ResultTensorSpec::metadata_owner` is a resource lease that pays for the spec's managed nested containers; it is budget ownership, not attached interpretation. `id` and `version` name an explicit structural contract and enter compile identity; the kernel does not dispatch on them to a domain meaning.

Complete structured execution may compute a non-security `result_digest` over named Result outputs in `ExecutionResult::results`: canonical schemas, field row counts and bytes, and certified tensor coverage and sample bytes in logical order. It excludes object identity, source association, and physical layout; it is not authentication. Fragment execution leaves this digest empty. `ExecutionOptions::maximum_result_digest_samples` defaults to 65,536 and sets the combined admission limit for field elements and certified tensor samples; zero disables digesting. Before scanning, the coordinator also checks per-field row-window limits, remaining Run-work, and tensor coverage-count representability. If the complete digest does not fit these estimates, it omits the digest and continues with the execution result. For example, a large certified broadcast view need not be traversed once per logical sample solely to create a digest. After scan admission, digest reads use bounded windows and consume Run-work; cancellation, stale state, Root/resource, or read failures during that scan remain execution failures. The coordinator rechecks cancellation and stale state before returning success.

## Data Layout & Memory

`SchemaTemplate` has at most 16 combined field and tensor slots and up to eight domain axes. A `ResultFieldSpec` describes a primitive element type, a row-count rule, and a record shape of rank zero through seven with positive extents. Row counts can be fixed, input-derived, tied to earlier field rows, or discovered at runtime. Fields are packed primitive records and remain separate from tensor sample bytes.

`ResultTensorSpec` describes a typed tensor member. Its cell descriptor has rank 1..8 with positive extents; `batch_axes` adds a positive-extent prefix, and the combined sample rank is at most eight. `atomic_trailing_axes` closes that many trailing cell axes into one indivisible observation. Schema validation does not impose a dense allocation or a representable full-domain element product. `ResultTensorLayout` records optional spatial layout, physical plane order, row pitch, and channel groups; storage order and pitch do not change semantic schema identity. A spatial layout maps height, width, and optional channel axes within the cell descriptor, independently of the batch prefix. A spatial tensor may have any legal channel count; the kernel does not infer RGB, alpha, a color model, or a numeric range from the channel count or from layout groups.

`atomic_trailing_axes` is the only tuple-completeness rule. Sample authorization closes each request over those trailing axes, so a request that touches one sample of an observation receives the complete observation. For an HWC image with `atomic_trailing_axes = 1`, a spatial request includes every channel of each requested pixel, including alpha when alpha is stored on that axis. Layout groups, schema id, and the spatial flag add no peer-channel or alpha demand. A tensor with `atomic_trailing_axes = 0` follows its sample coordinates exactly. Logical tuple completeness is independent of whether channel bytes are interleaved; planar storage may keep channels in separate planes. The canonical image target in [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) D9 uses explicit logical axis roles and does not mandate HWC.

`ResultBuilder` assigns an object id at construction, before runtime counts are known. The semantic key includes canonical schema and a length-framed execution scope. In compiled execution the scope is the actor key described under [Computation units and tile records](#computation-units-and-tile-records): the structured template, which binds the operation contract, parameters, and ordered inputs, plus the frozen snapshot identity and the computation unit. Physical page size, file offsets, descriptor revision, and resource limits do not change semantic identity. `ResultRef::capture()` snapshots the certified descriptor at one revision. The host actor publishes only this captured view, so observers cannot see a prefix from a later revision. `ResultRef::read_tensor()` requires that descriptor and reads only certified samples. `ResultTensorInput` carries the explicit tensor Need, slot, descriptor, and authorized sample set into a callback; the phase is borrowed for one poll, while its owning capabilities follow their own lifetime rules.

For each Result successfully supplied to an actor, the host keeps a Root-accounted historical fact keyed by input port and ObjectId, with the descriptor revision. This history identifies consumed sources but retains no payload and grants no read permission. `ResultObjectInputs` separately keeps the current owning ResultRef per input port across polls; the host clears that map when the actor completes or retires. Stored `ResultTensorInput` capabilities own their ResultRef and captured descriptor, and acquired read windows own the backing authorized by that Need. A physical tensor view independently retains its source Results, schemas, and backing.

For compiled structured CPU Whole operations with `preserve_output_views`, the coordinator prepares each payload-authorized Tensor Need after validating it and before the next computation poll. For each authorized box, it first proves an affine view over compatible source backing. If that mapping is unavailable, Auto may collect the authorized samples into immutable Root-owned private backing retained by the `ResultTensorInput`; `requires_input_views` instead returns `ViewUnavailable` before the computation callback. Auto does not turn work, cancellation, or resource failures into collection. Copies of the capability share the prepared backing and its Root metadata lease. The private `Value` is only backing: workflow inputs and outputs remain Results, and a published view retains the source Result identity and association. Direct Result phases do not automatically fulfill Needs or prepare these views. C Result output flags expose the same policies; the compiled CPU Whole coordinator performs the view preparation, while direct C callback entry does not synthesize it. This preparation path does not extend to GPU view execution.

The coordinator prepares boxes independently. If a later `ResultTensorInput::acquire()` spans boxes that have no common affine mapping, it returns the original authorized compound window without copying. A completed-result cache replay validates the saved Need and its facts without repeating physical view preparation; a cache miss follows the ordinary callback Need path and prepares the view before computation.

Before publication, the host builds the ordered association from the actor's `(input port, ObjectId)` facts. `ResultRef::retain_association()` checks that IDs are nonzero, do not name the output itself, and preserve every previously claimed ID. It compares through a temporary sorted copy, leaving the stored port/ObjectId order unchanged and preserving repeated ObjectIds supplied through different ports. Membership checks charge Root work and observe cancellation; a failed check leaves the existing association unchanged. There is no fixed 1024-entry association limit: Root metadata capacity and Run-work admission bound the list.

## Scheduling & Publication

### Computation units and tile records

Each plan step computes one selected named output with its own resolved output contract. The output's `OperationRegionRule` fixes its granularity at compile time. A Whole step has one computation unit, tile 0, which publishes the complete output. A Tiled step whose resolved output has a spatial grid has one unit per tile of that grid. The compiler derives every grid from the Result schema and the plan's single tile extent: tiles are square over the height and width axes, batch coordinates come first, and every non-spatial cell axis is complete in each tile. The grid is independent of physical storage.

```cpp
struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0;
  TileIndex tile = 0;  // Whole outputs always use tile zero.
};

struct TileNeed final {      // exactly one of whole, descriptor_only, tiles
  std::uint32_t input = 0;
  bool whole = false, descriptor_only = false;
  TileSet tiles;             // indices in the producer's compiled grid
};

struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0;
  bool whole = false;
  TileSet tiles;             // empty with whole == false: descriptor only
};

struct TileRecord final {
  TileUnit unit;
  bool computed_descriptor = false;
  ResourceVector<TileDependency> inputs;
};
```

The excerpt omits constructors and member functions; the declarations are in `photospider/data/tiles.hpp` and `photospider/execution/tile_dependencies.hpp`. The coordinator runs one actor per computation unit. The actor key concatenates the step's structured template, the frozen snapshot identity, and a digest of the step index, output index, tile index, and tile extent. The template is a static digest of the selected-output contract and its upstream templates or workflow-input names and schemas; it contains no graph, node, or step ID. Within a Run, a repeated request for the same unit reuses its actor. Steps with different indices keep separate actors even when their templates are equal; only the completed-result cache below can reuse equal templates. The actor's query uses tensor slot 0, its `tile_unit`, and a `tensor_outputs` footprint equal to the unit's tile region or, for a Whole unit, the complete first tensor. A Tiled unit's publication must stay inside that footprint, and its complete publication must cover it exactly.

A consumer's tile Need names producer tiles and first validates the indices against the producer's compiled grid. For a Tiled producer, the coordinator grants the union of those tile regions for each tensor, closed over `atomic_trailing_axes`. For a Whole producer, valid tile indices normalize to a Whole Need with an empty tile set, granting every tensor and field of the Result. An explicit Whole Need has the same complete grant, and a descriptor-only Need grants descriptor facts without payload. For a Tiled producer, the coordinator obtains the actor of each requested tile, then assembles the unit Results into one Result of tensor views whose coverage must equal the request. A Whole producer always supplies its unit 0. When a unit completes, the coordinator installs the union of its admitted Needs as the unit's `TileRecord`; Whole dominates tile and descriptor requests for the same producer, and the record retains no payload. `ExecutionDependencies::tile_record()` returns the record of a completed unit, and `dirty_units()` follows reverse edges from changed producer tiles or descriptors to the recorded units that consumed them.

Ordinary execution runs every non-side-effect-free operation as a mandatory root, including operations without a named output, whenever the plan's output requests or the supplied demand query request at least one output unit; a completely empty request skips unrelated effect roots. A non-pure operation computes Whole, so its effect root is one unit-0 actor that later requests in the same Run reuse. Effectful work is not shared or cached across Runs. Failure or cancellation of a mandatory root fails the Run.

Result C ABI operation definitions can declare a fixed input prefix and one repeated trailing group. `input_count` and `inputs` describe only the prefix; `repeated_input` supplies the prototype constraint applied to each actual trailing input during metadata resolution and `start`/`poll` query validation. A nonzero `repeated_maximum` enables the group and requires `1 <= repeated_minimum <= repeated_maximum`, with the prefix plus maximum no greater than 1,024 inputs. When disabled, all repeated fields are zero or null. `repeated_match` is 0 or 1: value 1 requires every repeated tensor to match the first repeated tensor's dtype and complete logical `sample_shape()`; value 0 requires a pure metadata resolver for the expanded input list. The C++ registry keeps the unexpanded template and resolves the actual input count during preparation. This declaration constrains metadata only; the operation still requests a tile Need for every actual input it reads.

The repeated-input contract is covered by `tests/integration/kernel/test_result_repeated.cpp` and its C fixture. It validates expanded input constraints, metadata resolution, malformed DSO rejection, and typed reads.

A native GPU callback can call `ResultProgramPhase::acquire_native_atlas(input, slot)` for a payload-authorized tensor Need. It returns an immutable `FragmentAtlas` containing exactly that Need's coverage; the operation still decides how to dispatch and publish. Calls for the same port and slot within one poll reuse one atlas. CPU access, a missing Need, or a Descriptor-only Need produces sticky `InvalidArgument` and does not widen authorization. A compiled workflow cannot request an Empty payload Need: Whole, descriptor-only, and a nonempty tile set are the three valid forms. An empty output request does not start a computation unit for that output. Root-backed payload and directory storage is separate from operation workspace, and the atlas owns those buffers without retaining the source Result or read windows. A caller-held shared atlas can remain alive after the poll, source, and execution context end.

Native GPU input transport reuses raw physical backing within a Run only while a weak proof still identifies the same live CPU storage and the complete view key matches. An optional byte-bounded context cache can reuse uploads across Runs by device instance/build and logical input bits, Region, dtype and shape; it excludes the Result schema and association, then rebinds a hit to the current schema and association. Cache hashing consumes the coordinator shared cache-work budget, and optional-work exhaustion skips the cache path. Capacity reclamation first clears retransmittable Run-local entries, then evicts retained context entries. `host_access_count` records actual CPU exposure of live native-backed storage, including Whole prepared reads, once per storage in a callback scope; metadata inspection, native-address acquisition, view forwarding and hashing do not count. Native GPU service failures are latched when the service call returns. A later `consume_work` exhaustion or callback status cannot replace that earlier service error; if work exhaustion occurs first, a later service error does not replace it.

Result atlas packing accepts either a current `ValueFragments` view or a `ResultTensorInput` capability. The lower-level `FragmentAtlasPlan` can pack a `ValueFragments` view with Empty coverage without reading source samples; this packing behavior does not add an Empty payload Need form to compiled workflows. The Result overload reads through Need-authorized owning windows, so signed and broadcast strides, fragmented coverage, and batch-prefixed spatial tensors are handled without converting the Result to Value semantics. `preparation_work()` and `materialization_work()` report generic geometry and packing units. Result preparation precharges its generic work through `FootprintLimits::consume_work`; Result materialization additionally precharges window acquisition and read-bound work through that callback. `materialization_work()` is not the complete Result-overload charge.

Block-state retention can reuse a completed state when the current request and proof match. A retained state is an execution implementation detail; cache-hit counts and block transitions are not a public performance guarantee. Benchmark claims require a separate controlled performance study.

### Cross-output block-state sharing

Pure deterministic Result operations may opt into `share_blocks_across_outputs`; registration rejects the opt-in for an operation that is not both deterministic and side-effect-free. The C Result ABI 4 exposes the same opt-in as `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V4` (flag bit 5), which uses an existing operation flag and does not change the Result table layout.

The shared namespace includes the operation and all resolved output contracts, backend, execution mode, selected resource identities, static parameters, and actual input metadata. Each block key also includes its kind, range, mode, incoming state schema/coverage/content, and the currently supplied tensor metadata, coverage, and bytes. The selected output index and output-specific metadata are not appended independently. A computation shared by different outputs must therefore be output-independent or encode the distinction in its state or mode. Public output Results, their associations and tile records, checkpoints, and completed-result caches remain output-scoped and independent.

Block-state lookup and retention apply only to a cacheable operation whose selected-input producer closure is pure, whose unit is not fallback-tainted, and whose current inputs include no object input (a field-bearing or tensorless Result, or a Whole Need on a port bound to a `requires_complete` validator), whether admitted with payload or descriptor-only. Retention is optional and charged to bounded cache work and Root capacity. If lookup or retention is unavailable, the callback computes the block normally. Each output still completes its own Needs and records its own `TileRecord`. The flag does not deduplicate concurrent producers or guarantee one computation per Run. Block-cache behavior is an implementation detail; performance remains uncharacterized.

When a GPU attempt qualifies for CPU fallback, the coordinator gives the retry fresh failure latches, drops the attempt's continuation, tensor and object capabilities, descriptor inputs, recorded Needs, input facts, and completed-cache replay log, and restarts the same computation unit on CPU. Producer units that already completed keep their published Results and records, so the retry obtains them again without recomputing them. Spent Root work and stages are not refunded. The retried unit is fallback-tainted, and a consumer becomes tainted when a supplied producer unit is tainted or ran on a backend other than the planned one. Tainted units skip checkpoint lookup and publication, block-state reuse, and completed-result retention.

The C++ `tests/fixtures/operation_fixture.cpp` exports 16 Result ABI 4 operations, including `fixture.bad_bytes`. `test_plugin_registry.cpp` exercises Result workflow binding/readback, direct public start validation, and dynamic output metadata: the resolver copies the input schema into the inferred output, then the poll callback checks the input's atomic trailing axis, reads the sample and multiplies it by `scale`. The `fixture.bad_atomic_axis` case gives the metadata sink a rank-1 tensor with `atomic_trailing_axes = 2` and verifies rejection; `fixture.bad_bytes` supplies a 9-byte payload for an 8-byte tensor and expects `TypeMismatch`. Fallback modes 5 and 7 attempt to publish through tensor slot 99 and expect `InvalidArgument`. Ownership checks cover owner-allocation failure, plugin destruction, and native-library close accounting. `test_execution.cpp` covers opt-in GPU fallback and Result callback outcomes, duplicate publication, cancellation, and fallback accounting. A native-only invocation is registered separately and skips with code 77 when no GPU lane is available. Duplicate tests distinguish a valid repeated publication (`OperationFailed`) from an invalid first service call whose `InvalidArgument` remains sticky after a later valid attempt; actual host cancellation can override the former, while callback-returned `Cancelled` cannot. Result C callbacks report outcomes through typed status codes: tests observe typed status, timing, and invocation counts, while bounded reports use the separate `report_diagnostic` service. C++ operation execution uses the staged `ResultContinuation` contract. The Result ABI 4 C table is the only C operation-plugin interface.

Pure, deterministic Result programs can use the C++ `ResultProgramPhase::checkpoint_publish` and `checkpoint_before` callbacks to retain continuation state; either call from an operation without both purity traits is a protocol failure. Lookup and retention also require the selected-input producer closure to be pure and deterministic, a unit that is not fallback-tainted, and remaining optional cache work. Otherwise lookup misses and publication declines without failing the operation. A published checkpoint owns a sealed state Result and a payload-free witness: the unit's `TileRecord` with the Needs admitted so far, the recorded `TileRecord`s of its producer closure, and the consumed input facts, each keyed by input port and ObjectId with the highest descriptor revision observed.

A checkpoint scope belongs to one computation unit: its key is the actor key with a `/checkpoint` suffix, so it binds the structured template, the frozen snapshot identity, the step and output indices, the tile index, and the tile extent. `checkpoint_before(phase, before)` returns the greatest published sequence in that phase not greater than `before`. A restore requires the same computation unit. It merges the witness Needs into the current unit's `TileRecord`, imports the producer records into the execution's record table (records already present in this execution remain authoritative), and merges the input facts. A successful restore also refreshes the current poll's `association` with the restored input history; references or iterators to its previous elements may be invalidated, while the borrowed vector remains valid until poll returns. It does not populate the current phase's `results` or `tensors`, recreate read windows, or grant Need authorization. The callback must request current input access again.

The context directory holds weak references to checkpoint scopes, while an active execution holds its scopes strongly; `ExecutionContextConfig::maximum_result_checkpoint_scopes` bounds the live scope count. Each scope retains at most 64 entries, and the summed witness weight of a scope stays within the dependency `maximum_boxes` limit; the oldest entries retire first. The runtime charges optional checkpoint work before constructing lookup keys, traversing the producer closure, or copying Need records and input facts. Zero or insufficient optional cache work skips lookup or declines publication; it does not copy unadmitted witness data. A copied `ResultCheckpoint` owns its state and witness even after the index or context is cleared. Optional cache, metadata, or capacity exhaustion can make lookup miss or publication decline without invalidating a successful operation; cancellation, protocol failures, and admitted Run-work failures remain errors. The C Result ABI exposes `checkpoint_before`, `checkpoint_read`, and `checkpoint_publish` through poll-scoped opaque handles and sealed generic state Results; see the [C checkpoint services](Dependency-Data.md#c-checkpoint-services) contract.

The completed Result cache reuses the sealed Result of one computation unit across frozen execution identities, including binding generations and Runs of an equivalent plan, when the workflow-input content that the unit actually reached is unchanged. A completed-cache key digests the structured template, the selected output index, the planned backend, the tile extent, and the tile index. The key contains no step index or graph ID, so an equal template at another step can reuse the entry. Each key keeps up to eight candidates. A retained entry (manifest) contains:

- the sealed Result;
- a payload-free tile snapshot: the unit's `TileRecord`, the recorded `TileRecord`s of its producer closure, and routes from each producer to input ports in contract terms;
- the workflow-input sources reached through that closure, each a named binding with a Whole flag or a tile set, and a digest of their current content;
- the replay log: each admitted Need of the unit, paired with a digest of the supplied facts after that Need (canonical schemas, field row counts, and granted tensor footprints, without sample bytes);
- the backend and the cache epoch.

The source digest covers, for every recorded source, the binding's canonical schema and field row counts. When the source was reached Whole or through tiles, the digest also covers all field rows and the tensor samples of the reached tiles, or of the whole tensor for a Whole source. Bytes that no recorded Need reached do not affect the proof.

A lookup rebinds each candidate's tile snapshot to the current plan by contract; when one recorded producer would map to different current producer steps, the route is ambiguous and the candidate misses rather than issue a Need against an arbitrary step. The coordinator recomputes the source digest over the current bindings and compares it with the stored digest. It then replays each saved Need through the ordinary supply path, including input validators, and requires each supplied-facts digest to match. A candidate that passes is adopted: the coordinator rebinds the cached Result with a new ObjectId, the current execution scope, and the association built from the replayed input facts, merges the rebound `TileRecord` into the unit, and validates the publication like a computed output. The cache entry retains the physical backing owners it references; rebinding does not make the old Result wrapper or old execution scope the identity of the new output.

Structured Result entries share the bounded cache LRU and managed Root capacity. The LRU holds at most 4,096 entries, `ExecutionContextConfig::result_cache_bytes` bounds retained backing (zero disables retention), and `maximum_dependency_cache_metadata` bounds manifest metadata. Capacity accounting deduplicates physical backing owners, including backing reached through tensor views and affine owners, rather than summing logical tensor sizes. Eviction releases the cache's owner references; callers holding a Result or read window continue to own its backing. `clear()` advances the cache epoch, so an in-flight producer from the previous epoch cannot refill the cleared cache. Cache proof construction, Need replay, and rebinding consume the Run's optional cache work (`ExecutionOptions::maximum_dependency_cache_work`). Insufficient optional work or Root capacity skips retention or produces a miss without changing an otherwise successful computation.

The `StructuredResultCache` owned by each Run constructs completed-result keys and manifests, builds source proofs, restores replay snapshots, and spends the optional cache-work quota. It receives borrowed context and actor views for each synchronous call. The Run-local Host supplies Needs through the coordinator and adopts a candidate after successful replay; a failed replay restores the actor's previous inputs and Need record. Once lookup acquires a strong candidate, it carries that owner through replay and adoption; adoption does not require a second proof of LRU residency. An optional promotion lookup moves the entry to the LRU tail only if the same manifest remains resident. Cache closing or an epoch change rejects adoption while the candidate owner remains alive until the attempt ends.

Retention requires a pure, cacheable operation and a pure, cacheable selected-input producer closure, a unit that is not fallback-tainted and ran on its planned backend, and remaining optional cache work. A unit that received a descriptor-only Need for an object input, or whose replay log exceeded the metadata limit, is not retained. Each entry covers exactly one computation unit, and entries for different tiles are not combined. The C Result ABI does not expose this cache; its implementation is private to the C++ execution runtime.

`ResultBuilder` owns publication. The producer writes tensor regions through `publish_tensor`. Tensor regions are checked against the slot's flattened logical sample domain. Overlap, out-of-domain access, or failed resource admission retires the producer with an error.

Publication keeps physical arrays and backing owners in the Result-derived storage owner, while publication state owns the Root, resource leases, and schema. Destruction retires array backing before the publication state releases its Root leases. `acquire_tensor` copies immutable backing handles while holding the publication mutex, then unlocks before constructing the owning read window; it does not reacquire that mutex. `publish_tensor_kernel` prepares callback work outside the mutex, then reacquires it to validate the owner, revision, and sticky failure before committing. A read capability retains its authorized Result and backing, but grants reads only within the captured descriptor and requested Need.

The publication validator receives a `PublicationValidationView` and a borrowed `PublicationValidationServices` interface for one synchronous validation. The coordinator owns cancellation, work admission, current limits, and the actor. Validation checks publication identity, revisions, and associations; it does not retain the services or perform scheduling, publication, retirement, or notification. The coordinator applies the returned status and retirement intent after validation completes.

`tests/support/result_diagnostics_fixture.hpp::verify_cpp_fixed_broadcast` publishes full-coverage Float64 Results for shapes `{4}`, `{UINT64_MAX}`, and `{UINT64_MAX,2}` from one 8-byte zero-stride scalar backing. `tests/integration/kernel/test_result_diagnostics.cpp` exercises the helper. A per-output payload cap of 8 bytes admits two separate Results within a 16-byte Root Payload limit; the tests read the first and last logical samples and check that releasing both outputs returns live Payload to zero. The exact maximum sample count reports `computed_elements == UINT64_MAX` with `computed_elements_saturated == false`. The extra-tensor case has logical coverage of `UINT64_MAX + 1` across two tensor members sharing one 8-byte backing; it also publishes successfully and reports `UINT64_MAX` with saturation set.

`publish_tensor_kernel` lends writable windows for the requested region. For spatial tensors, the builder uses planar rectangles when the complete logical sample byte geometry is representable. If the layout is nonspatial, the sample count cannot be represented, or the full sample count times element width would overflow, the builder allocates affine backing for the requested region under Root limits. This fallback preserves the tensor spec, spatial layout, global sample coordinates and certified coverage; it changes only the physical writer backing. Callbacks must use each window's `spec()`, `region()`, `sample_axis()`, and the strides returned by `row_run()` or `rectangle_run()`. They must not assume planar backing or contiguous rows. A sparse request over a huge logical spatial domain is charged by its actual writable region, while a larger requested region must still fit Root payload and work limits.

```cpp
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& samples,
    const std::function<
        Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
    const CancellationToken& cancellation = {});
```

The Footprint overload publishes an exact set of disjoint canonical boxes in one transaction. The builder copies `samples` before it runs any producer code, so a callback that changes the caller's footprint object does not change the published set. A footprint with one box takes the Region path above. For several boxes, the footprint must already be closed over the tensor's observation grouping (`close_samples` returns the same set), and must not overlap existing coverage; a violation returns `TypeMismatch`. The callback receives one write window per box in `boxes()` order. Holes receive no storage and no coverage. An Empty footprint skips the callback and payload work.

The callback runs outside the publication mutex, and same-producer mutation and sealing are rejected while it runs. After the callback returns, the builder rechecks the owner, the revision and the sticky failure, then cancellation. A returned error, a thrown exception (`ResourceExhausted` for allocation failure, `OperationFailed` otherwise), cancellation, or a revision change (`Stale`) records the producer failure, releases the buffer and certifies no new coverage; previously certified prefixes remain readable. Workers writing through the windows must finish before the callback returns, because the windows and raw pointers expire at that point. On success, the builder commits the boxes and the coverage together at one new revision. The overload charges one Root work unit per published sample and Host metadata per box; the byte size must fit the builder's tensor storage growth limit.

When a slot's first publication is such a multi-box publication, its affine pieces correspond one-to-one with the canonical coverage boxes. `acquire_tensor` then finds the piece containing the request origin by binary search. A request inside one piece yields one view; a request spanning pieces is authorized against coverage as a whole and then clipped once against each piece. `row_run()` and `rectangle_run()` on the resulting window also locate the piece by binary search. Any later publication to that slot returns its windows to the ordinary piece-by-piece lookup. Callbacks and readers use the strides reported by `row_run()` and `rectangle_run()` in either case.

The published Result records ordered source ObjectIds for lineage. That association is metadata, not a strong owner for input payload and not a read grant. `ResultObjectInputs`, `ResultTensorInput` capabilities, and retained windows provide the active owner and authorization paths described above. A physical tensor view also retains every source Result whose bytes it publishes. Immutable operation resources, such as ICC profile or OCIO configuration bytes, are not Result members. Static preparation returns the explicit `ResourceIdentity` list it needs; the compiler selects exactly those entries from the `ResourceSet` passed to `Compiler::compile`, fails if one is missing, and the plan and execution state retain the selected set. A Result therefore never keeps a color configuration alive on its own. Public `ResourceMap` and `ResourceVector` containers also carry their allocator and managed ownership; copied results, names, and observations retain root-accounted metadata until those containers and any contained `ResultRef` owners are released. Consumers must budget these copies along with payload. The policy is conservative, so live owners can retain more backing than the final output alone needs.

## Port validators and diagnostic reports

The kernel provides generic extension points for sample legality and bounded reports. It does not ship domain validators: the operations SDK registers its own, for example the RGBA, LUT table, and LUT report validators in `plugins/ops/common/domain/contracts.cpp`.

```cpp
struct ExtensionIdentity final {
  std::string key;
  std::uint32_t version = 1;
  std::string implementation;
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

struct OperationPortConstraint final {
  std::vector<ExtensionIdentity> validators;  // ordered; enters identity
  // ... dtype, rank, schema id/version, tensor key, scalar bounds
};

Status OperationRegistry::register_validator(ValidationDefinition);
Status OperationRegistry::register_diagnostic(DiagnosticDefinition);
```

A port binds validators explicitly through `OperationPortConstraint::validators`. Each entry names a registered key, version, and implementation; the registry rejects a missing identity, a version or implementation conflict, duplicate keys, malformed definitions, and any registration after `freeze()`, without partial changes. The bound identities, including version and implementation, enter operation identity and therefore the plan cache key. Addresses, callback pointers, and report contents do not.

Validation has two phases:

- `prepare` runs once during operation preparation, outside the registry lock. It receives the port schema, the static parameters, and the `ResourceSet` restricted to the identities that the operation's preparation selected. It returns owned immutable state.
- `validate` runs on the entry thread for each new grant or publication. `ValidationContext` exposes only host-authorized reads: `acquire_tensor` and `read_field` reject a region or row range outside the current coverage with `InvalidArgument`/`UnauthorizedRead`. Work is charged through `consume`, and `scratch_bytes` is admitted before entry. The validator must not retain the context or any borrowed object.

The coordinator calls validators at three points:

| Point | Coverage visible to `validate` | Failure category |
| --- | --- | --- |
| Input tensor Need | The newly granted sample footprint of one slot; field reads are denied. `complete` is true only for a single-tensor, field-free schema whose grant covers the whole slot. | Workflow input: `InvalidArgument`. Produced input: `OperationFailed`. Origin `Domain`. |
| Whole input consumption (fields, no tensors, or a `requires_complete` validator) | The complete sealed Result. | Same as above. |
| Output publication | Only coverage and field rows added since the previously validated revision; `complete` equals the descriptor's sealed state. | `OperationFailed`, origin `Domain`. |

A validator with `requires_complete` needs the whole `CompleteBundle`. For payload consumption on an input port, the Need must resolve to Whole authorization. A valid tile request to a Whole producer normalizes to Whole and satisfies this requirement; a request that remains a partial tile grant fails as a protocol error. Descriptor-only requests grant no payload and do not satisfy complete-bundle consumption. The validator does not widen the normalized grant. The tensor path skips the per-slot check for an input that the Whole path already validated, so one consumption is not checked twice.

A completed-result cache hit still runs the input validators on the replayed Needs without repeating the computation. An invalid workflow input therefore fails with `InvalidArgument` before the cached output is returned, and a changed static parameter that a validator prepares from produces a different plan cache key.

The first service failure inside one `validate` call takes priority: if a read fails and the callback ignores it, the call still returns that read failure. Exceptions thrown by `prepare` or `validate` become typed failures. `ValidationDefinition::owner` retains the callback code; the host destroys it only after the callback objects and the prepared validator state, so a prepared operation keeps working after the caller releases its registry handle.

`OperationTraits::diagnostics` lists the report types an operation may emit. Before callbacks run, the host initializes a Root-owned `DiagnosticAccumulator` for those types. A callback reports a fixed-size byte record through `ResultProgramPhase::report_diagnostic` (C: `report_diagnostic`) by declared ordinal; `DiagnosticDefinition::validate` checks it, and `merge` writes a complete new record without changing either input. A failed report leaves the accumulated bytes unchanged. `OperationTiming::reports` carries the owning accumulator; `find(identity)` returns the merged bytes, or `NotFound` if the type never reported. The kernel interprets none of these bytes. The numeric work report is one such type, defined in the SDK header `photospider/ops/numeric/diagnostics.hpp`.

An operation can attach an opaque domain code to a failure. `Status(ErrorCode, message, DomainErrorCode, FailureDetail)` stores it in `FailureDetail::domain_code`; C callbacks use `fail_domain(status_code, domain_code)` on the run or validation services. The kernel preserves the code without interpreting it, and the first recorded domain failure is not replaced by later callback or diagnostic failures.

## Limits & Failure Handling

Every `ExecutionContext` creates a managed `ResourceBudget`. If `ExecutionContextConfig::managed_resources` is unset, the context uses default-constructed `ResourceLimits`; an explicit value supplies the configured capacities. `maximum_live_bytes` also caps the Payload dimension. `ExecutionContext::resource_budget()` returns a shared handle for clients that need to account temporary storage, and its leases may outlive the context.

The execution root accounts for payload/backing, work, stages, continuations, queue entries, metadata, and retained owners under their configured resource dimensions. Bounds are finite. Oversized payload, or owner admission fails with a resource error rather than returning an incomplete witness. This managed-capacity model covers instrumented resources; it is not an RSS bound.

An output can declare `maximum_output_payload_bytes`. For structured Result operations, the host checks each published Result's physical backing against that per-output cap, deduplicating shared owners and excluding source allocations that remain live through exposed Results or authorized tensor grants. This includes private Whole input backing retained by a `ResultTensorInput`. The check keeps only weak source references, so the cap does not keep input payload alive. Root still charges source and collected input backing; the output cap measures only newly owned output payload. Every prefix publication is checked using the complete Result, which makes the check cumulative as backing is added. A direct singleton, compiled execution use the same rule. The first eligible poll allocates the cap guard and shared control block from the Result Root before entering the callback; insufficient Metadata capacity rejects the poll before callback entry. Owner enumeration and comparison consume normal Run and Root work. Exceeding the cap returns `ResourceExhausted` with `CapacityLimit`. Workspace, metadata, and operation-resource admission remain separate. The check runs when a Result is published, so a trusted callback's earlier Root allocations are still charged and are not prevented by the cap.

Result publication errors are sticky. Cancellation and stale execution retire active state only after admitted callbacks have returned. A failed append or tensor publication cannot be repaired by a later successful callback; a previously certified prefix remains immutable and readable under its captured descriptor. The last owning Result/read-window release retires backing and retained associations.

Behavioral regression tests are registered with the kernel build. The registrations under `tests/unit/kernel` and `tests/integration/kernel` are the current source for executable coverage.
