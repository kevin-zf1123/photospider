# ADR 0016: Bind Workflow Inputs to Each Execution

- Status: Accepted

## 1. Core Summary (TL;DR)

Workflow declarations describe input metadata, while each execution supplies immutable input values or regional sources by exact name. The compiler keeps payload bytes and addresses out of IR, plans, and plan identity, so one plan can run against independent input snapshots. Preflight validates binding names and metadata before source reads, validates directly consumed scalar `Value`s before operation callbacks, and validates each regional-source sample after its read.

## 2. Mental Model & Intuition

```text
WorkflowDocument                         each execute call
  declarations + graph                         bindings
          |                                       |
          v                                       v
       analyze -> optimize -> plan         name / metadata preflight
                                      +----------+-----------+
                                      |          |           |
                                   Value   RegionalSource  InputSnapshot
                                      |          |           |
                                      +----------+-----------+
                                                 v
                                     callbacks -> named results
```

The declaration is a socket specification: it fixes the name and logical type expected at that port. Each `execute` call supplies its own plug. A `Value` retains immutable bytes; a regional source fills host-owned requested storage synchronously; an `InputSnapshot` supplies immutable blocks. Concurrent calls can share a plan while retaining separate binding snapshots.

## 3. Formal Contracts & APIs

```cpp
struct PHOTOSPIDER_API WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  ValueDescriptor descriptor;
  Region region;
  StridedLayout layout;
  std::vector<ValueFacet> facets;
  std::optional<PlanarImageLayout> planar_layout = {};
};

using WorkflowInput =
    std::variant<WorkflowNodeOutput, WorkflowInputReference>;

struct PHOTOSPIDER_API ExecutionBinding final {
  std::string name;
  Value value;
  std::shared_ptr<const RegionalSource> source = {};
  std::shared_ptr<const InputSnapshot> snapshot = {};
  std::shared_ptr<const PlanarImage> image = {};
};
struct PHOTOSPIDER_API ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};

class PHOTOSPIDER_API ExecutionContext final {
 public:
  Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
};
```

The declarations reproduce the current public member and field forms; enclosing headers and unrelated declarations are omitted.

`WorkflowDocument` schema 3 stores declarations, operation nodes, and named outputs. A node input is tagged as either a producer output or an input declaration reference; node ids and input ids occupy separate namespaces. Declarations use unique nonzero ids and exact, case-sensitive printable ASCII names from 1 to 128 bytes, using bytes `0x21` through `0x7e` with no spaces. A document can declare at most 4096 inputs. Every declaration must be bound exactly once, even when the graph does not consume it.

For a dense `Value` binding, the element type, shape, whole logical `Region`, zero byte offset, canonical positive row-major strides, exact byte count, and closed facet set must match the declaration. The descriptor has rank 1 through 8 with nonzero extents. Checked byte sizing requires a positive size representable by both the address arithmetic and host allocation size. Facets are canonicalized by key and compared by key, version, and payload. General `Value` views may use other valid layouts; workflow bindings apply the stricter dense declaration contract.

For a regional source, the source descriptor and facets match the declaration, and `read` receives an exact nonempty logical `Region` plus host-owned writable bytes. It reports the coverage it filled. The pointers expire when `read` returns, and the source must support concurrent reads without mutation. The executor validates each successfully read sample before using that region; a later bad sample can fail after earlier regions have already been read. A snapshot binding is a kernel-owned immutable block store; `read` and content identity access are bounded by caller-supplied sample limits and cooperative cancellation. Structural image declarations and bindings use `PlanarImageLayout` and `PlanarImage`, whose storage is described in [ADR 0017](0017-cpu-regional-execution-and-storage.md).

`ExecutionBinding` selects exactly one representation. Names are validated as a multiset, so duplicate entries cannot be silently overwritten by map insertion. Bindings and metadata are copied before invocation; `Value` and image/snapshot storage retain their owners. A Run keeps its binding snapshot until admitted work retires. Source callbacks and output sinks receive borrowed pointers or views only for the duration documented by their call.

Compile-time operation parameters remain in `WorkflowNode::parameters`. Runtime scalars are ordinary bound `Value`s and are checked against operation port constraints: a `Float32Scalar` has one Float32 sample, and the finite sample must lie within the inclusive minimum and maximum published by its port. The executor preflights binding names and metadata and validates directly consumed `Float32Scalar` Values before callbacks. Such a port requires a dense `Value`; it rejects `RegionalSource` and `InputSnapshot` bindings for that port. Input payload bytes, pointers, and allocation addresses do not enter semantic, optimized, physical-plan, or plan-cache identity. Static declaration metadata, traits, image layout, and normalized output demand do. `InputSnapshot::content_identity` hashes canonical metadata and exact requested sample bits, excluding allocation, layout, and block geometry.

## 4. Non-Goals & Explicit Boundaries

- Declarations do not load files or retain caller payloads. The kernel exposes no hidden file-loading path.
- Bindings have no optional/default values and receive no implicit type conversion, resize, layout repacking, or facet coercion.
- Runtime values do not become compile-time parameters or arbitrary literal nodes.
- Direct input-to-output passthrough is not a workflow output form; named outputs select operation-node outputs.
- A `RegionalSource` is a synchronous kernel callback contract, not a provider ABI or codec interface.
- Structural planar image storage is a distinct declaration/binding representation; planar samples are not asserted to be interleaved dense bytes.

## 5. Consequences

Malformed declaration metadata and malformed, missing, extra, duplicate, or invalid binding names fail with `InvalidArgument`; dense-size arithmetic that cannot be represented fails with `ResourceExhausted`. A mismatch in declared type, shape, region, layout, facets, or source metadata fails with `TypeMismatch`. Scalar values outside a declared operation interval and bound-image samples rejected by the image-domain validator fail with `InvalidArgument`. The executor checks names, metadata, dense Values, and directly consumed scalar Values before the first operation callback; it validates regional-source samples after each successful regional read and before consuming that region.

The plan is reusable with independent immutable bindings. The caller must not mutate binding containers while `execute` copies them, and must synchronize any mutable state behind its own source implementation. The kernel may run a source concurrently. Cancellation is cooperative and observed during bounded source/snapshot work and execution. A stale plan is rejected before binding inspection; after valid entry, cancellation takes priority over stale state and ordinary execution failures.

Caller-owned input retention is outside `ExecutionContextConfig::maximum_live_bytes`. Kernel-managed output, scratch, intermediate, and transfer allocations remain accounted until their final owner retires. Returned immutable values may outlive the execution context. Input snapshot storage has its own aggregate store budget; replacing a snapshot creates a new version and leaves older readers valid.
