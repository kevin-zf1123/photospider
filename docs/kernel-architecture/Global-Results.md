# Structured Results and tensor members

## Scope & Ownership

`ResultRef` carries workflow inputs and outputs, including images. Each Result owns a static directory of 1..16 typed tensor members, published coverage, and the owners required by its storage. `Tensor`, `TensorSpan`, and `TensorWriter` are storage carriers used by the data layer and SDK. Execution uses the fixed physical program described in [Compiler and execution](Compiler-and-Execution.md).

Results contain structural schema and bytes. Color, alpha, transfer and sampling descriptions are explicit operation configuration. The compiler validates structural connections; the caller supplies valid numerical inputs and the operator implements its declared numerical semantics. Execution does not scan samples for domain conformance.

```cpp
struct ExecutionBinding final {
  std::string name;
  ResultRef result;
};
struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};
```

A workflow input binds an owning `ResultRef` by exact name. Successful `RunHandle::join()` returns named owners in `ExecutionResult::results`. Call observers can independently retain `CallRecord::values`. These references and owning read windows keep storage alive after the Run and execution context end. Object identity identifies one logical Result; it is independent of physical allocation identity and sample content.

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

`ResultBuilder::start` accepts a scope of 1..4096 bytes. It copies the scope into Root-accounted storage, charges the copy work, and assigns a new ObjectId. `ResultRef::matches_scope` compares the stored bytes exactly with the supplied string. Execution preflight separately checks schema, captured coverage, physical layout and storage ownership against the Plan. The scope string alone does not establish that a Result satisfies a binding. `SchemaTemplate::publication_scopes` belongs to the static schema identity; runtime call exports follow the ordered retirement contract.

`ResultRef::capture()` snapshots published coverage at one revision. An earlier capture does not gain access when the producer publishes more samples. `ResultRef::read_tensor()` requires a matching descriptor and reads only covered samples. `ResultDescriptor` stores one coverage `Footprint` per member. An acquired read window owns its source schema and backing, so it remains valid after the producer and execution context are destroyed.

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

Root work and metadata admission bound the C++ vector overload's piece count. An empty piece vector, a NULL `source`, `reshape=true` in a piece, or a piece region that leaves `total_region`, overlaps another piece, or leaves part of `total_region` uncovered fails with sticky `TypeMismatch`.

The builder validates the entire logical partition, dtype, source permissions and each mapping before preparing physical spans. It then checks ownership cycles across all retained source Results, including Results from different Roots, and calls `publish_tensor_spans` once. Success certifies coverage and advances revision once while retaining the source backing after the borrowed windows retire; no intermediate piece is published.

The view path copies no payload. Its target Root charges metadata, geometry/work and publication, while source-backed bytes remain charged to their owning Roots; directly supplied external storage without Result source owners follows the distinct `reference(storage)` admission path.

The cycle check runs even after a piece mapping reported `ViewUnavailable`, so a source chain that reaches the target Result fails with sticky `TypeMismatch` instead of offering a copy retry. Logical authorization, overlap, owner cycles, resource failure and cancellation are errors and cannot become an automatic copy fallback. Only a physically unrepresentable but logically valid mapping reports nonsticky `InvalidArgument` with `FailureReason::ViewUnavailable`, leaving existing coverage/revision and earlier captures unchanged so the operation may explicitly materialize the requested region. Other failures are sticky; cancelled or failed transactions cannot expose a partly assembled view.

## Execution, publication and lifetime

`ExecutionContext::prepare(plan)` links immutable kernels and retains module, resource and device owners. `submit(prepared, bindings)` captures the input Results and validates their names, schema, coverage, memory domain and physical layout before executing tasks. `PlanningOptions::input_domains` fixes input storage domains. A Device binding must belong to the prepared device instance.

The Plan declares every task read and write. Direct tile bindings require compatible allocations for the declared tiles. Layout adaptation and transfers are explicit COPY tasks; DenseHost input lowering emits CPU COPY tasks to import dense host input. COPY outputs use independent allocations. A COPY can therefore preserve every numerical bit while producing a different Result and backing owner.

For each block activation, the executor admits output storage and per-lane scratch before issue. Kernels borrow fixed input views, exclusive output views, immutable parameter bytes and scratch through terminal completion. A synchronous CPU return ends borrowing. An accepted GPU submission ends borrowing only when its completion callback establishes that all accesses have ended and writes are visible. Output envelopes remain private while tasks can write their backing.

Dependencies determine task readiness. CallGroup retirement follows IR call order, and `on_call_retired` delivers records on a separate serial lane. A successful call exports its compiled result selection after its tasks complete; a failed or elided call exports no values. `join()` waits for accepted tasks and observer delivery to settle. See [Compiler and execution](Compiler-and-Execution.md) for failure cuts, effects, CFG boundaries and cancellation ordering.

Tile storage remains live for declared consumers, control-flow transfers and retained exports. A CFG alias preserves its logical input association; a newly produced Result receives a new ObjectId. Partial output projections are separate Result objects. Kernel input ObjectIds identify logical Results across tiles and members, rather than allocation addresses or content hashes.

### CPU and Device storage

Host and Shared storage support CPU read windows. Device storage uses an opaque native allocation and remains owned by the Result after execution ends. Reading a Device-backed sample through `read_tensor` or requesting a CPU window that intersects Device coverage returns `BackendUnavailable`. CPU-backed regions of the same member remain readable. To read Device output on the CPU, compile a COPY into Host or Shared storage.

COPY tasks support Host, Shared and Device endpoints. Host endpoints of GPU COPY use admitted Shared staging; the host gather precedes submission and the host download completes before terminal notification. Metal and Vulkan completion paths retain all borrowed storage until native work settles. The declared layouts support signed and zero strides, including broadcast input backed by fewer bytes than its logical sample count.

### Result builder

`ResultBuilder` owns publication. The producer writes tensor regions through `publish_tensor`, `publish_tensor_view`, or `publish_tensor_kernel`. Tensor regions are checked against the member's logical sample domain. Overlap, out-of-domain access, or failed resource admission retires the producer with an error. `ResultBuilder::start()` takes an explicit tile extent; an invalid spatial tile extent returns `InvalidArgument` before allocation.

Publication keeps physical arrays and backing owners in the Result-derived storage owner, while publication state owns the Root, resource leases, and schema. Destruction retires array backing before the publication state releases its Root leases. Storage-owner indexing avoids rescanning previously published backing for each publication, and `acquire_tensor` locates pieces by an interval search when the member's partition permits it, otherwise by a bounded scan. `acquire_tensor` copies immutable backing handles while holding the publication mutex, then unlocks before constructing the owning read window. `publish_tensor_kernel` prepares callback work outside the mutex, then reacquires it to validate the owner, revision, and sticky failure before committing. A read window retains its Result and backing and permits reads only within its captured coverage and requested rectangle.

`publish_tensor_kernel` lends writable windows for the requested region. The builder partitions the request at member tile and batch boundaries and gives spatial members tile-local planar bytes. It reserves no full-domain backing, so a sparse request over a huge logical domain is charged by its actual writable tiles. Callbacks must use each window's `spec()`, `region()`, `sample_axis()`, and the strides returned by `row_run()` or `rectangle_run()`. They must not assume planar backing or contiguous rows across windows.

```cpp
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& samples,
    const std::function<
        Status(const ResourceVector<ResultTensorWriteWindow>&)>& write,
    const CancellationToken& cancellation = {});
```

The Footprint overload publishes an exact set of disjoint canonical boxes in one transaction. The builder copies `samples` before it runs any producer code, so a callback that changes the caller's footprint object does not change the published set. The Region overload delegates to this path with one box. The footprint must already be closed over the tensor's observation grouping (`close_samples` returns the same set), and must not overlap existing coverage; a violation returns `TypeMismatch`. Each box is split at member tile and batch boundaries. The callback receives one window per chunk, in `boxes()` order and then logical chunk order within each box; all windows share one private allocation and publication transaction. Holes receive no storage and no coverage. An empty footprint skips the callback and payload work.

The callback runs outside the publication mutex, and same-producer mutation and sealing are rejected while it runs. After the callback returns, the builder rechecks the owner, the revision and the sticky failure, then cancellation. A returned error, a thrown exception (`ResourceExhausted` for allocation failure, `OperationFailed` otherwise), cancellation, or a revision change (`Stale`) records the producer failure, releases the buffer and certifies no new coverage; previously certified prefixes remain readable. Workers writing through the windows must finish before the callback returns, because the windows and raw pointers expire at that point. On success, the builder commits the boxes and the coverage together at one new revision.

`ResultTensorWriteWindow::borrow_dense(spec, writer)` lends an explicit row-major `TensorWriter` as a write window without copying. It first validates the member's spatial topology, then rejects a writer whose dtype or full sample shape differs with `TypeMismatch`.

A physical tensor view retains every source Result whose bytes it publishes. Immutable operation resources, such as ICC profile or OCIO configuration bytes, are supplied separately through `ResourceSet`. Static preparation declares their `ResourceIdentity` values; the registry selects those resources and invokes `validate_resources` before publishing the prepared operation. Prepared kernels retain the selected resources and defining module through completion. A Result retains its own schema, backing and accounting independently.

## Structural contracts and diagnostic records

`OperationPortConstraint` describes schema identity, member selection, dtype and rank. `infer_operation_outputs` checks every input, including inputs excluded from runtime reads, and homogeneous repeated groups compare complete logical shapes and dtypes. Static preparation also checks declared immutable resources through `OperationDefinition::validate_resources`. Numerical sample validity remains an operator and caller obligation.

```cpp
// Selected public fields; see fixed_invocation_api.h.
struct ps_kernel_status_v8 {
  uint32_t code, reason;
  uint64_t domain_code;
  char message[256];
};
```

Fixed kernels return or complete with a bounded operational status. The host preserves `domain_code` as opaque diagnostic information. Completion records use preallocated storage, and the coordinator converts them into owning `Status` objects. `ExecutionResult::diagnostics` reports activation, submission, completion and retirement counts and peak active tasks.

The independent `DiagnosticAccumulator` API provides root-accounted, fixed-capacity report merging. `OperationRegistry::register_diagnostic` registers codecs, and `OperationTraits::diagnostics` binds their identities for static resolution. `DiagnosticDefinition::validate` checks report bytes; `merge` produces a complete replacement record. Failure leaves the accumulator unchanged. A caller using an accumulator serializes its mutations and retains any returned byte view only while that record remains unchanged. Fixed invocation ABI 8 has no report-submission callback, and the Run does not expose an accumulated report stream.

## Limits and failure handling

Each execution context owns a managed `ResourceBudget`. `ExecutionContextConfig::managed_resources` supplies optional limits, and `maximum_live_bytes` caps payload capacity. `resource_budget()` returns a shared handle whose leases may outlive the context. Managed capacity accounts instrumented allocations and reservations; it is not an RSS bound.

The Plan's physical bounds drive block output and scratch reservations. Actual retained Result owners keep their storage charges alive after block retirement. Root-accounted metadata includes managed schema copies, coverage, directories and owning containers. Large logical shapes are valid when the selected physical tiles and metadata fit these limits; logical size alone does not require a dense allocation.

Result publication errors are sticky except for the explicit `ViewUnavailable` copy-retry case described above. A failed or cancelled publication exposes no new coverage, while earlier published regions remain readable through their captured descriptors. `seal()` closes the producer at its current coverage, including sparse coverage, and rejects subsequent writes. It does not inspect numerical samples or fill holes.

Cancellation stops new work and drains accepted tasks before releasing their borrowed owners. Context destruction requests cancellation and waits for active Runs and observers. The final owning Result or read-window release retires the backing it retains.
