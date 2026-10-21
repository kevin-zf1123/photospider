# Static Dependencies and Fixed Invocations

Status: Proposed breaking design. This page defines the operation-facing part of the [Plan contract](Compiler-and-Execution.md). [Chinese reader version](zh/Dependency-Data.zh.md).

## Static mapping contract

The compiler resolves all reads and task topology. A mapper reads fixed schemas, grids, static parameters, implementation profiles and selected outputs. It returns finite input tile sets and any internal stage graph. It is deterministic, bounded and free of runtime I/O. Payload never becomes an implicit mapper input.

Static preparation describes output schemas, input projections, internal SSA tiles/tasks, implementation symbols, parameter encoding, scratch bounds and effects. The compiler flattens and validates that graph. Shared algorithms may produce several independent output tiles in one task. The fixed invocation path cannot add runtime dependencies or tasks.

The execution ABI supplies a fixed invocation. This C++17 sketch is a logical interface, not the final C ABI 8 record layout:

```cpp
#include <cstdint>
namespace ps_invocation_design {
struct Layout {
  std::uint32_t rank, element_type;
  std::uint64_t shape[8], origin[8];
  std::int64_t strides[8];             // bytes; resolved during prepare
};
struct ConstView {
  const void* allocation;             // opaque native handle in a device domain
  std::uint64_t bytes, byte_offset;
  const Layout* layout;
  std::uint32_t domain, member;
  std::uint64_t tile_index;
};
struct MutableView {
  void* allocation;
  std::uint64_t bytes, byte_offset;
  const Layout* layout;
  std::uint32_t domain, member;
  std::uint64_t tile_index;
};
struct Invocation {
  const void* prepared_kernel;        // immutable context with resource owners
  const ConstView* inputs;
  std::uint32_t input_count;
  const MutableView* outputs;
  std::uint32_t output_count;
  const MutableView* scratch;
  std::uint32_t scratch_count;
  const void* parameters;
  std::uint64_t parameter_bytes;
  const void* cancellation;           // opaque borrowed service
};
using CpuInvoke = std::int32_t (*)(const Invocation*) noexcept;
}  // namespace ps_invocation_design
```

The Plan fixes port/member/tile ordering, views and parameter encoding. Prepare resolves layout IDs to immutable descriptors containing rank, shape, origin and byte strides, and binds a PreparedKernel context containing immutable resources and direct entry points. Invocation borrows these resolved descriptors and context; kernels need no registry lookup or access to a global layout-ID table.

Layout rank is 1..8; shape is the view extent, origin is its logical tile origin, strides are in bytes, byte_offset locates local coordinate zero within the allocation, and bytes is accessible allocation capacity. Prepare checks every reachable address for signed strides. PreparedKernel supports concurrent read-only invocations across Runs.

Schema-only inputs are prepared metadata without payload handles. Invocation arrays live through completion. Outputs are exclusive writes, inputs immutable, scratch private. Adapters catch C++ exceptions before a C ABI boundary. Status reports operational or explicitly operator-reported errors; the executor does not verify numerical results.

## Dependency construction

For task `t`, `D(t)` is the distinct set of producer tasks from payload reads and explicit internal-stage edges. Incoming immutable values are ready at activation and contribute no local predecessor. Build `pending[t] = |D(t)|` and transposed successor lists. Repeated reads from one producer yield one readiness edge; distinct tiles still require separate lifetime holds.

The [prototype](prototypes/ooo_executor.py) accepts `Task.dependencies`, a tuple of zero-based static task indices. Its predecessor set is the union of those explicit edges and producers inferred from `reads`; duplicate edges across both sources count once. An explicit edge can name a later table entry when call order and acyclicity permit it. Invalid indices, self-dependencies, cycles and dependencies on later calls are rejected before admission.

An ordering-only edge adds no read view and no consumer hold. For example, an effect can depend on a preparation task with no payload output. If that preparation has a temporary output that nobody reads or exports, its buffer can be released at completion before the effect starts. Explicit edges still require successful predecessor completion; failed or skipped predecessors never authorize issue.

Example: A produces `a0,a1`; B reads `a0` through two ports and `a1` through one. B has one predecessor, two holds and three read-view entries. A completes once, enabling B once. Both allocations survive through B's completion. Descriptor reads add no producer edge because descriptors are compile-time facts.

Task success makes its entire write set available. A consumer of one tile waits for its producer task, not every task in that call. Numerical certification and `CompleteBundle`/`StablePrefix` validation closures create no dependencies. Actual reductions and coupled algorithms declare their computational stages explicitly. Call retirement still waits for all selected call tasks.

Data-dependent reads declare a static superset. STMap with proven displacement bounds can use a finite neighborhood; an arbitrary map requires the full source grid. The kernel picks addresses within supplied views and owns boundary/NaN semantics. Payload-dependent producer discovery is outside this protocol.

## Effects, control and selection

Ordinary tasks mutate only declared output/scratch storage. External I/O or shared mutable state requires an effectful call with pure preparation and one terminal `Effect`. It starts at retirement head. Registered implementation contracts supply effect freedom; input schemas cannot prove it.

Conditions, edge arguments and live-through values are static roots. Runtime activates only the chosen block, mapping parameters to immutable incoming values and creating fresh state. [Parallel execution](Parallel-Execution-Model.md) defines failure and abort order.

Initial planning treats a missing output request as default full coverage; `select_outputs` treats an omitted name as unexported. Empty requests carry no payload, while mandatory control/effects remain. Selection closes over compiled work. New coverage requires compilation. The executor produces no Need records, content-comparison cache or dirty-propagation index.

## CPU/GPU ownership and ABI

The CPU adapter invokes one worker and completes after return. A kernel may poll cancellation but joins all work before returning. Parallel stages lower into Plan tasks; the baseline supplies no nested range service. Immutable preparation is shared; mutable invocation state is Run-local.

The GPU adapter uses fixed views/parameters to encode dispatches/transfers and returns an accepted ticket or rejection. [Submission rules](Parallel-Execution-Model.md#cpu-and-gpu-completion-boundary) cover immediate completion, exactly-once events, visibility and drain. Inputs, outputs, scratch, parameter bytes, modules and command objects remain owned until actual completion.

The implemented C module and fixed invocation ABI 8 use fixed-width counts, exact sized/versioned records, immutable inputs, exclusive outputs/scratch, opaque native handles, status and completion services. The public headers define ownership and borrowing for each record without exporting STL layouts. Old module getters are rejected. Native GPU service ABI 2 independently defines asynchronous Metal/Vulkan preparation and accepted-completion ownership. See [Plugin ABI](Plugin-ABI.md) for the concrete records and tests.

## Checked boundaries and unchecked execution

| Boundary | Required checks |
| --- | --- |
| Compilation/import | Types, grids, declared coverage, bounds, dominance, DAG, membership, resource vectors and effects |
| Module link/prepare | ABI/version/size, symbol contract, resources, kernel profile and device capability |
| Run preflight | Names, schemas, physical coverage/layout, owners, currentness and resource admission |
| Coordinator | State, completion identity, credit, cancellation and lifecycle transitions |
| Operator | Numerical preconditions, address choice, output correctness and truthful completion |

Native modules execute as trusted in-process code. Unchecked execution removes numerical certification and repeated grant assertions, while bounded decoding and state correctness remain. Duplicate/stale events are rejected by key/state checks and cannot decrement counters twice; they indicate a backend protocol error.
