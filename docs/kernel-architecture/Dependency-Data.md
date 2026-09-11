# Exact dependency data

The G4 data API provides exact sets, sparse immutable fragments and resolved
per-observation certificates. Execution integration is a separate implementation
layer: these types alone do not authorize an operation callback to batch errors,
consume a RequestRecord, or bypass producer/snapshot provenance.

`Footprint` is an exact set in a nonzero rank-1..8 logical domain. Its canonical
disjoint rectangles represent Empty, All and non-contiguous coverage. Recursive
axis sweeps merge adjacent intervals only when their suffix sets are identical,
so construction order, duplicate boxes and alternative tilings do not affect
set equality. All remains compressed even when the dense element product would
overflow. Union, intersection and difference require the same domain and retain
holes. `tile_cover(geometry)` returns exactly the touched tile coordinates in a
ceil-divided domain; it does not mark every sample inside those tiles valid.
`visit` emits each sample once in row-major order under an explicit sample limit.

`FootprintLimits` bounds each operation's candidates and box counts, including
duplicate construction work. Exceeding limits returns ResourceExhausted;
cancellation returns Cancelled. Neither outcome is Empty or a bounding-box
approximation. Compound users must additionally bound their total operation and
metadata work across calls; a per-set limit is not a whole execution budget.

`ValueFragments` pairs complete descriptor/facets with an authorized Footprint
and retained rectangular Values. Construction clips supplied coverage, rejects
missing authorized samples and rejects inconsistent overlapping owners/mappings.
Equivalent origin-relative mappings on the same owner deduplicate. Every typed
image fragment and authorized box covers full C; two partial-channel Values
cannot be assembled to evade that requirement. Generic arrays permit arbitrary
sample subsets. `read` uses Value's checked signed-stride addressing and copies
the actual dtype width. A hole, unauthorized address or wrong width is an error;
there is no synchronous fetch, implicit extension or zero-fill fallback.
`restrict` restricts owners and coverage together. `collect` requires a complete
rectangle before allocating through the supplied BufferAllocator. Retained
capacity counts unique actual storage owners, independently of valid coverage.

`DependencyCertificate` stores an identity, exact observation coverage, declared
input domains and one `AtomCertificate` per covered observation. Generic
observations are logical samples; image observations use an HW domain with one
complete-pixel observation. Each row retains input port, individual dependency
roles, exact sample Footprints and non-spatial tagged atoms. Multi-role inputs
are normalized and their expanded metadata is bounded before publication.
An explicitly present empty row is known empty; an omitted row is unknown and
cannot be published as resolved coverage. Identity binds the caller's fixed
access/numeric/error contract and snapshot; these data types cannot prove that
an arbitrary callback actually followed its declared reads.

`restrict(P)` rejects any P outside coverage. `backward(P)` unions just those rows
for transport, grouped by port/role. This fetch projection is not a certificate.
`transpose(dirty)` returns exactly covered observations whose same-port,
matching-role sample or tagged support intersects the edit. `merge` requires
equal identities/domains and equal canonical rows on overlapping observations.
A request-level failure witness or terminal RequestRecord is not an atomic
success certificate.

The internal `DirtyDeltaQueue` keeps accumulated and propagated Footprints for
each record in one certificate generation. Under one mutex, taking an item
computes accumulated minus propagated, records propagation and clears queued.
A later new atom can enqueue the record again. Dependency structure belongs to
the demand coordinator independently of pixel cache ownership; the queue itself
owns no pixels or workers. A failed downstream enqueue must fail its generation.

Focused checks are `test_footprint`, `test_value_fragments`, `test_dependency`,
`test_dependency_dirty` and `test_input_snapshot`. They exercise independent
finite membership/reachability oracles, unknown rows, identity/swap, role/tag
isolation, late dirty waves, dtype/stride/owner bounds and snapshot COW.
See [Cache Model](Cache-Model.md) for generic snapshot identity and ownership.

## Staged C++ programs and current Run integration

OperationTraits 8 distinguishes local `Atomic` and terminal `RequestRecord`,
request-only failure delivery, dependency protocol version, continuation byte
bound and finite stage bound. Exactly one synchronous callback or staged start
function is registered. Staged programs require deterministic, side-effect-free
behavior. Version 1 requires RegionRule::Dependency; it permits
static Typed/Axes/repeated inference without imposing Whole demand. Compilation
rejects every declared edge out of RequestRecord, including unused paths, and
computes EffectiveAtomic across all input ancestors. Dependency plans retain
unresolved input demands instead of inventing a rectangular approximation.

`start_dependency` copies validated metadata, parameters, original Q and the
immutable input-bundle identity. Generic Atomic starts accept at most one sample;
image-v2 starts accept at most one complete pixel. RequestRecord starts preserve
the complete original query. PerAtomOutcome is reserved and rejected until an
actual per-observation outcome protocol is implemented. Changing its flag cannot
make a request-only callback batch-safe.

A continuation is placement-constructed in its host allocation. `poll` consumes
only supplied fragments and either returns exact associated Needs or a complete
result. `supply` must match the transport union on every port and the captured
bundle identity. Descriptor evidence remains in every atomic row even if no
source pixels are requested. Terminal dependencies and original Q are retained
separately and never published as an atomic certificate. The certificate identity
includes the registry definition instance, backend, full traits, parameters,
input/output metadata and bundle identity; different atomic queries can restrict
or merge only under the same contract.

Discovery work, poll count, continuation bytes and phase output/scratch have
finite limits. Reads, explicit work charges and allocation failures are sticky
even if the callback ignores their returned Status. Parent failure observers
cannot prevent another observer from receiving the error. Accepted-call failures
retire continuation state exactly once, release input owners and prioritize
cancellation after cleanup. Rejected concurrent or reentrant calls leave the
active call undisturbed. Borrowed phase/query/service references expire at return;
concurrent destruction is forbidden. State must use the granted allocator.

`ExecutionContext` currently drives dependency templates through an explicit
record stack in ExecutionRun. Each start, poll and source callback uses the
existing CPU worker queue and shared waiting admission. Waiting records hold no
worker and no unused active reservation. Every stage performs nonblocking byte
admission against the same MemoryBudget; sealing releases unused capacity while
retained state/input/output leases stay charged. Requests beyond the realizable
minimum working set fail ResourceExhausted. Source callbacks fill exact requested
rectangles; sparse input ports remain ValueFragments. An unowned successful
callback allocation is imported through the controlled allocator before escape.

The default record path executes one Atomic observation at a time. Existing
synchronous Whole implementations keep their complete global observation,
including validation; this does not authorize broadening staged per-sample
queries. A terminal request executes once at full original Q. The direct
`OperationRegistry::invoke` convenience entry follows the same observation rule
with already supplied immutable input Values. Frozen execution retains its
captured graph and input owners. CPU dependency execution currently uses a
serial ready order. Legacy Whole/effect boundaries run once per Run, including
unconnected effects. Atomic streams deliver configured tiles and release each
tile before advancing; terminal streams preserve full original Q. This respects the caller's maximum parallelism bound.

The public progressive workflow and `test_dependency_program` exercise real
source discovery, legacy-to-staged-to-legacy composition, full-Q terminal
behavior, one-worker progress, finite admission, cancellation and frozen input
ownership. Successful dependency Runs now publish immutable structural evidence as described
below. Context-owned live demand replacement, shared Flights, dependency result
cache reuse and native GPU fragment access remain part of the ongoing G4
implementation. The direct certificate API is implemented independently
of those pending cache/scheduler integrations.

The [dependency sampling operations](Dependency-Sampling.md) implement STMap and
dynamic radius gather/scatter. Optional pure static validators run during
compilation and before direct Empty-query state decisions.


## C staged programs

`dependency_plugin_api.h` supplies the ABI-8 C equivalent of the staged
protocol. A descriptor supplies exactly one `execute` or `dependency_program`.
The loader copies and validates the bounded program table, retaining its library
through every active state and callback. Host-owned state bytes are zeroed before
`start`; `destroy` runs exactly once whenever start was entered, even when start
fails. Empty queries run pure metadata validation but skip all state callbacks.

A poll submits per-output associations with exact runs, ports, roles and tagged
evidence. Atomic coordinates name the current sample or HW pixel; terminal
associations use no atomic coordinate and retain the complete original Q.
Returning Need after allocating output, or completing with unresolved needs or
unpublished output, is an error. Descriptor runs cannot encode holes as data.

The phase services expose checked sample reads and borrowed fragment views with
actual dtype, origin, signed strides, byte span and authorized region. Missing
samples never trigger hidden upstream reads. Cross-poll input ownership requires
`retain_input`; its handle preserves exactly that fragment's authorization and
underlying storage lease. Handles are monotonic per invocation, never reused,
and become invalid at release or state destruction. Invalid handles and ignored
service failures are sticky. Scratch pointers expire at poll return. Output
pointers expire immediately on
successful publication, or at poll return if unpublished; they cannot be used
after publishing through the host-owned handle.
Every image output fragment must cover full C before allocation is granted.
Native plugins remain trusted in-process code: pointer metadata checks are not
memory isolation.

`test_dependency_plugin` loads a real C11 module and checks four dtype widths,
negative strides, two distant samples with an excluded interior, retained owners,
failed start, ignored reads and duplicate publication, cancellation, library
lifetime, and full-Q RequestRecord values. Its public workflow reads only 16
source bytes, exercises the observed admission frontier and one byte below it,
and verifies cancellation cleanup followed by a successful new execution. The
same source and C module run as an installed-package consumer for both library
forms.

To run that installed C workflow and its independent checks separately:

```sh
cmake --build build/issue257-static/consumer-build --target photospider_dependency_consumer -j 8
build/issue257-static/consumer-build/photospider_dependency_consumer
```

It exits zero only after checking Atomic output 9, terminal output 11, exact
source endpoints, rejected holes and the ownership/resource failure cases.
The consumer build is created by `test_installed_consumer`; replace `static`
with `shared` for the shared-library installation.


## Runtime structural evidence

`ExecutionResult::dependencies` owns immutable direct records for a successful
G4 dependency-network Run. Records preserve merged Atomic certificate rows and
their exact coverage; each Whole or terminal RequestRecord keeps an indivisible
manifest. Legacy regional steps record the actual per-port demands used for their
callback and validation. The builder merges only matching node/contract/snapshot
rows. Source declarations, records, subscriptions and named roots share a bounded
metadata count, separate from controlled pixel bytes. Evidence owns no Values,
source callbacks, snapshot blocks or workers and remains usable after pixel
results, the graph, registry and ExecutionContext have retired.

`coverage()` states the named output samples for which the evidence is complete.
`certificate(node)` returns observed Atomic rows; an absent node, Whole record or
terminal request returns NotFound. None of those outcomes means unknown rows are
clean. `potential_dirty(input, samples)` is exact for the captured relation and
those named output subsets. It routes only direct subscriptions using the real
`DirtyDeltaQueue`; a record receiving a later disjoint delta propagates again in
the same query generation. Source Footprint copying, traversal and answer growth
are bounded, with cancellation and explicit ResourceExhausted failures. A failed
query never returns a partial clean/dirty map.

This query concerns payload changes under fixed declarations. Descriptor/schema
replacement follows recompilation; the sample record graph does not model
metadata-output atoms and rejects Descriptor-role edits. Direct nonspatial tags
remain present in each certificate and may be transposed through that single
node's certificate. This distinction avoids claiming an absent metadata-only
upstream sample record proves a clean result.

`restrict({name: subset})` walks the stored direct associations backward and
restricts all contributing Atomic rows and subscriptions together, discarding
unused records and roots. Unknown coverage is rejected. Whole retains its
complete global manifest; terminal RequestRecord permits only its identical full
Q. Empty Atomic coverage is known empty, while an omitted output name is absent.
This operation reads no pixels and does not invoke callbacks.

`test_execution_dependencies` runs a short/long diamond whose B and T records
must receive `{0}` and later `{1}`, checks an independently constructed per-port
oracle, and retains evidence after releasing pixel/context owners. Dynamic
scatter checks excluded control evidence, equal-output dependency changes and
old evidence isolation. Whole and real C terminal tests check indivisible
manifests. The same tests run against installed static and shared packages. The
public G4 workflow also checks live runtime evidence for radius edits and frozen
old relations. Evidence itself is immutable; subscriptions are owned by the
context-managed demand API below.

## Exact demands and immutable binding replacement

`ExecutionContext::open_demand(plan, bindings)` pins the current graph contract
and immutable Value/snapshot bindings. A `DemandHandle` shares that bundle and
generation across copies. `request({name: Footprint})` returns exact
`ValueFragments`, diagnostics and structural evidence. Every request keeps its
original Q. Atomic callbacks still run per sample/full image pixel; a staged
terminal RequestRecord receives the complete sparse Q once. A legacy synchronous
terminal accepts rectangular Q only. Empty outputs validate static metadata and
skip source reads, admission and continuation callbacks. Unrequested names are
absent. `execute_fragments(frozen, Q)` provides the same exact query for an
explicitly pinned bundle, with result generation zero.

Successful requests retain only structural publications under their exact named
query. `replace_bindings` compares immutable bytes from the old recorded source
support, computes potential dirty through the recorded associations, and commits
the new bundle, generation and accumulated dirty together. No caller dirty hint
is trusted. Changed control evidence remains dirty until that exact query is
successfully republished, even if the numeric output happens to stay equal.
`source_support()` is the bounded fetch union used for these byte comparisons;
it does not replace per-output certificate relations. Typed image comparisons
include complete C, and generic comparisons preserve all dtype bit patterns.
Static descriptor/schema changes require a newly compiled plan.

Replacement validates all bindings before publication. Sample, metadata or
validation failures retain the old generation. A racing request publication or
replacement returns Stale for retry. Latest requests captured before a completed
replacement cannot publish into the new generation; cancellation takes priority
over Stale. `freeze()` pins the current immutable bundle independently of future
edits. `release(Q)` drops one exact subscription without cancelling active
requests, which may subsequently republish it; `cancel()` stops that handle
and retires its publications and bundle. Context destruction cancels and drains
active demand calls before retiring its existing workers. Direct context calls
must not race context destruction, while existing handle calls may do so.
Input owner destructors run outside the publication mutex.

`maximum_demands` bounds live uncancelled handles (1..65536, default 1024), and
active demand calls are bounded by the existing queue plus CPU worker capacity
and one coordinator slot. `DemandConfig::maximum_metadata_entries` bounds each
handle's retained query/evidence/dirty metadata (1..1048576, default 65536).
The handle owns no workers or pixel cache. Current calls execute independently
through the existing CPU pool, WaitingAdmission and accounted allocator.
Cross-Run dependency Flights, content-cache reuse and GPU fragment execution
remain unfinished G4 integration.

`test_execution_demand` covers sparse results, typed snapshots, continuous dirty
accumulation, frozen isolation, stale publication, independent cancellation and
context drain. The real C terminal fixture checks one invocation for sparse Q
and no invocation for Empty. The public `g4_workflow` demand scenario checks
endpoint scatter sums before/after two replacements against a direct oracle.
