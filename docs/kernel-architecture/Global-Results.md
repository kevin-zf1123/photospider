# Paged global results

The installed C++ registry supports `start_result` (dependency protocol 2).
`WorkflowDocument` edges carry a compiler-visible `SchemaTemplate` through
semantic IR and the execution plan. `OperationPortKind::Result` distinguishes
these edges from ordinary `Value` edges. A result has no fabricated dense
descriptor, zero-extent Value, or terminal RequestRecord adapter.

## Static and dynamic facts

A schema has an id/version, 1..16 named primitive fields, optional domain axes,
and bounded semantic metadata. Each field consists of packed records with
0..7 positive record dimensions. Row constraints are Fixed, InputAxis,
InputElements, FieldRows (an earlier field), or RuntimeCount. Input-derived
constraints resolve from metadata; runtime counts remain constraints until
publication. Ceil division and offsets are checked. Empty collections have
zero rows and no data backing. Consumers inspect `ResultDescriptor`; they do
not replace an existing Value descriptor or dynamically load operations.

`ResultBuilder` assigns ObjectId at construction. Semantic keys include the
canonical schema and a length-framed execution scope. The runtime scope
includes the operation contract, parameters, ordered inputs, and frozen input
identity. Page size, file offsets, descriptor revisions and resource limits do
not enter semantic identity. Separately executed mutable RegionalSources have
distinct scopes. Physical plan choices remain in the physical plan identity.

`CompleteBundle` is hidden until seal. `StablePrefix` and `IndependentChunks`
currently publish ordered field prefixes: every new range is irrevocable and
all its data, control, validation and descriptor obligations must be final.
IndependentChunks accepts this ordered subset; arbitrary out-of-order range
publication is not exposed. The supplied `ResultFinality` is the registered
algorithm author's obligation, not a proof synthesized by the kernel. Family
validators add numerical and cross-field checks separately. A descriptor is a
fixed-size immutable snapshot; an older descriptor never gains new coverage.

## Stages and explicit I/O

An operation supplies a host-allocated `ResultContinuation` and declares its
state bytes, workspace and stage limit. `ResultProgramPhase` exposes only
ready Value fragments, ready result objects and I/O replies. A poll returns
bounded `ResultProgramNeed`, `ResultPublication`, or `ResultValuePublication`.
Needs have at most 64 entries and must respect the selected input projection.

Read/write plans retain exact interval authorization and owning storage.
Temporary creation, read, write and extension are closed I/O actions. The
existing coordinator executes them after the callback yields; the existing
worker executes callbacks. Calling temporary I/O directly inside a structured
callback produces a sticky protocol error. Failed host reads, allocations and
work debits cannot be ignored into successful publication. Ordinary source
callbacks retain their explicit source-I/O contract.

Structured execution currently uses CPU stages and requires
`ExecutionContextConfig::managed_resources`. It supports ordinary synchronous
and dependency-protocol-1 Value ancestors. Terminal RequestRecord outputs in mixed networks
execute their entire query without splitting it into Atomic invocations.
Result outputs are retrieved from `ExecutionResult::results` or observed with
`ExecutionOptions::result_publication`. `execute_stream` requires that observer
when named outputs include ResultRefs; ordinary Value outputs use its normal
sink. Returned read windows own their authorization and bytes.

## Ownership, sharing and cancellation

Mandatory backing is independent of the optional completed cache. Equivalent
result producers within a Run share one actor. Calls on the same
`FrozenExecution` join context-local active producers. A completed lookup is
weak: it permits reuse while a result or owning read window still exists, and
does not itself keep completed data alive. Every waiter receives its own
publication notification. A wait occurs on a coordinator, never on a worker.

Runtime publication binds the ordered input ObjectIds and retains at most 16
direct input ResultRefs. Consequently a final derived result can retain its
entire input ancestry and mandatory backing. This is a conservative lifetime
policy, not minimal liveness. All those live owners remain subject to the root
budget. The final result/window owner releases the corresponding storage even
after the context has ended.

One waiter's cancellation or observer failure does not cancel another active
waiter's shared producer. Each call declares the Result keys reachable from its named outputs. Shared
producer interest is computed from these original calls and their cancellation
flags, never recursively from other producer tokens. The coordinator refreshes
that interest while draining callbacks. Before any exit,
including an exception, it finishes outstanding shared obligations for other
waiters, subject to the same finite work/resource limits. Last-waiter
cancellation stops the producer. Failed startup wakes joined waiters. A
retired producer cannot install a complete object, and an old epoch cannot
modify a replacement entry. Later operational failure preserves previously
certified prefixes. No detached producer thread is created.

## Dependency evidence and accounting

`ResultRelation` supports bounded Cartesian, identity, paged rows, union and
composition. Relations declare Exact, Conservative or Unknown. Conservative
support remains Conservative through composition; Unknown is Unresolved in
dirty queries. These relations do not enter the older Exact-only
`DependencyCertificate` interface. Data support and descriptor support are
separate: an empty collection still requires a count/basis/validation witness.
Constructing an Exact relation is a trusted operation-author proof obligation.

The root ledger admits payload, explicit object metadata, entries, callback
queue slots, encoded disk extents and I/O windows. New schema strings, nested
metadata, actor containers, semantic keys and shared-result tables use
`ResourceAllocator`; capacity is reserved before the allocation. Compiled
static templates are plan-owned and borrowed during execution. Returned
diagnostics and cancellation tokens retain their allocator root through copies
and after the context ends. Publications, relations and I/O plans must belong
to the executing root; foreign-root objects are rejected. Ordinary Value
publications admit external storage under Referenced and preserve the admitted
alias. Coordinator fragment/Whole tables use the root allocator; temporary
legacy invocation arrays and copied static metadata have explicit bridge leases.
Legacy Value/Footprint/session-internal allocations retain the exclusions below. Work, submitted stages and
temporary I/O are cumulative. Low limits return ResourceExhausted and release
ordinary ownership; unverified physical cleanup remains quarantined. The
[managed-resource scope](Managed-Resources.md) excludes allocator-internal
headers/control blocks, OS caches, drivers, thread stacks and legacy
uninstrumented metadata. This is a managed-capacity model, not an RSS bound.

## Executable validation

`tests/integration/test_result_execution.cpp` uses only installed public APIs:
RegionalSource → runtime-selected IDs → two sum consumers. Its independent
integer loop checks zero/37/8192 candidates and multiple page sizes, cache-off
sharing, frozen input reuse, concurrent cancellation, startup failure,
streaming notification, bounded failures and final ownership release.
`tests/unit/test_global_results.cpp` checks direct descriptor authorization,
publication policies, paged support and a result larger than its host budget.

```sh
cmake --build build/issue257-shared --target test_result_execution test_global_results -j 6
ctest --test-dir build/issue257-shared -R '^(test_result_execution|test_global_results)$' --output-on-failure
cmake --install build/issue257-shared --prefix out/phase-a-delivery/install
cmake -S tests/consumer -B out/phase-a-delivery/consumer -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/consumer --target photospider_result_consumer -j 6
out/phase-a-delivery/consumer/photospider_result_consumer
```

The three Phase A effect workflows and complex representation families have
separate implementation leaves. This foundation test is not their acceptance.
