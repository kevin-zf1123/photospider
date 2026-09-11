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
existing CPU worker queue or the selected native GPU queue and shared waiting admission. Waiting records hold no
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
serial ready order. Legacy Whole records run at most once per Run when actually requested;
unconnected effects still execute once. Pure Whole ancestry is resolved lazily
so a valid descendant cache hit need not rematerialize its pixels. Atomic streams deliver configured tiles and release each
tile before advancing; terminal streams preserve full original Q. This respects the caller's maximum parallelism bound.

The public progressive workflow and `test_dependency_program` exercise real
source discovery, legacy-to-staged-to-legacy composition, full-Q terminal
behavior, one-worker progress, finite admission, cancellation and frozen input
ownership. Successful dependency Runs now publish immutable structural evidence as described
below. Live demand replacement, shared Flights and dependency result cache
reuse are implemented as described below and in [Cache Model](Cache-Model.md).
C++ staged GPU fragment access is integrated through [Fragment Atlas](Fragment-Atlas.md).
Bounded native discovery and the staged C GPU bridge remain G4 work.

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
C checkpoint services add `checkpoint_before`, `checkpoint_read` and
`checkpoint_publish`. A successful lookup miss returns handle zero; a hit imports
the complete successful witness and returns sequence/byte size. The nonzero
handle is valid only until this poll returns, shares the invocation's monotonic
ID space and never authorizes an input read. `checkpoint_read` copies a positive,
in-bounds byte interval without exposing storage pointers. Publication copies
positive-size opaque state bytes into a packed UInt8 Value from the current stage
allocator; the operation must declare enough workspace for that copy. State must
encode complete deterministic algorithm values, with no pointers, handles or
uninitialized padding. Mutating the caller's source bytes cannot change the
published state. Copy work is charged before allocation/access. Invalid handles,
intervals, scope or terminal use produce sticky failures.

Every image output fragment must cover full C before allocation is granted.
Native plugins remain trusted in-process code: pointer metadata checks are not
memory isolation.

`test_dependency_plugin` loads a real C11 module and checks four dtype widths,
negative strides, two distant samples with an excluded interior, retained owners,
failed start, ignored reads and duplicate publication, cancellation, library
lifetime, and full-Q RequestRecord values. Its public workflow reads only 16
source bytes, exercises the observed admission frontier and one byte below it,
and verifies cancellation cleanup followed by a successful new execution. The
C checkpoint tests also check partial byte reads, copied carry state, expired
phase handles, invalid addresses/intervals, terminal rejection, prefix witness
import, and a real five-output C scan with exactly five source reads. The same
source and C module run as an installed-package consumer for both library
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
The handle owns no workers or pixel cache. Exact demand calls use context-owned
Flights for overlapping active observations in the same immutable bundle, through
the existing CPU pool, WaitingAdmission and accounted allocator. Completed dependency content-cache reuse uses the existing pixel LRU with
bounded structural proofs, as described in [Cache Model](Cache-Model.md). GPU
fragment execution remains unfinished G4 integration.

`test_execution_demand` covers sparse results, typed snapshots, continuous dirty
accumulation, frozen isolation, stale publication, independent cancellation and
context drain. The real C terminal fixture checks one invocation for sparse Q
and no invocation for Empty. The public `g4_workflow` demand scenario checks
endpoint scatter sums before/after two replacements against a direct oracle.

The U2 sibling working-set counterexample is a real `test_dependency_program`
workflow: A and B each allocate a 1 MiB output and 3 MiB scratch from the context
allocator. The lazily resolved children also retain the parent's one-byte
continuation. Exactly 4 MiB rejects A before its callback; 4 MiB plus that state
lets A complete but rejects B, and A's storage owner expires on failure. The same
context can then execute A alone with an observed 4 MiB peak. With 5 MiB plus the
state, the parent receives both results and returns 3, with the corresponding
observed allocation peak. This verifies finite rejection and actual
lease retirement for that execution order, not an optimal scheduling guarantee.

## Shared exact-observation Flights

`request` and `execute_fragments` claim one Atomic sample/full image pixel or one
complete terminal Q. The key binds the captured bundle identity, plan/operation
contract, node, geometry, exact query and resource policy. Sharing requires
deterministic, side-effect-free implementations throughout the input ancestry.
A side-effectful/non-deterministic Whole ancestor therefore prevents downstream
sharing even when the local callback is pure. Dispatch never expands Q or
batches RequestFailureOnly observations.

The directory linearizes claims, gives each producer a unique FlightId and keeps
waiter cancellation/currentness separate from its producer token. Explicit and
auxiliary set cancellation both belong to the waiter. The caller's coordinator
drives stages and waits for dependencies; callback workers never wait for another
Flight. While waiting for a callback, the coordinator checks ancestor waiters.
An independently needed child can finish after the originating parent request
is cancelled. A successful frozen waiter can likewise receive old-bundle work
when a latest waiter becomes Stale after replacement.

Last-waiter cancellation prevents new joins; a later request claims a new
FlightId. Late completion removes the directory entry only if its ID still
matches, so retiring P0 cannot erase P1. Context shutdown cancels active producers
and drains demand calls before destroying workers. `clear_result_cache()` also
advances the dependency epoch and invalidates completed retention eligibility. `maximum_dependency_flights` separately bounds active Flights and their
total subscribers, each by the same configured count (1..1048576, default 65536).
Exhaustion returns ResourceExhausted without a wait for metadata capacity.

Successful shared values carry immutable direct records with exact certificates
or indivisible manifests and links to contributing upstream records. Import walks
these links in topological order, preserving the per-output relation in each
waiter's execution evidence. Structural records contain no pixel/snapshot owners.
Their destruction uses an allocation-free iterative retirement queue, including
when a long chain loses its last owner. Context `cache_statistics()` includes
active dependency Flights and shared subscriptions even with result retention
disabled; per-call shared diagnostics exclude duplicated callback timings.

The integration tests exercise shared legacy and staged ancestors, independent
explicit/auxiliary cancellation, impure-ancestor exclusion, old-P0/new-P1 overlap,
latest/frozen races and imported dirty evidence. The direct lifetime regression
checks epoch/limit behavior and retirement of 20000 linked structural records.
The gated terminal case proves identical sparse Q shares once while a smaller Q
executes separately; imported terminal evidence still rejects subset restriction.
An explicit joint request containing a nonfinite sample fails while a shared
normal-only waiter succeeds, and a later atom finishing first does not change the
joint request's canonical error. The separate `test_scan_waiters` now checks these boundaries with the real
`numeric.ordered_scan` implementation and successful shared prefix states.
The public workflow uses two exact waiters and a bounded callback barrier to
prove one callback, one cancelled waiter and the other waiter's value 7 with
complete identity dependency evidence.


## Completed internal checkpoints

A C++ `DependencyPhase` offers `checkpoint_before(phase, sequence)` and
`checkpoint_publish(phase, sequence, state)`. These are optional completed-state
services for pure Atomic programs. RequestRecord use is a sticky InvalidArgument,
including when the callback ignores the error. An immutable checkpoint state
must be allocated by the current stage allocator; sibling stage allocations are
not accepted. The host captures the already supplied canonical history, charging
metadata/work before copying it. Lookup validates operation/static contract,
backend, input bundle, host node scope and sequence, then imports its full witness.
A checkpoint does not authorize reading fragments omitted from the current stage.

Each active context scope retains at most 64 checkpoints within exact-set metadata
limits. The context directory is bounded by `maximum_dependency_flights` and holds
only weak scope owners. State payload remains in the existing live allocation
budget. Memory admission can clear optional states; result-cache clear detaches
old scopes. Borrowed states stay alive until their own leases retire. Lookup never
waits for a producer, and no error or cancelled outcome is published through this
interface. A query can therefore reuse a valid prefix of another still-active
query without inheriting that query's later error or cancellation.

Retention proof construction uses the bounded optional dependency-cache work
allowance; exhausted retention can recompute. Import uses ordinary dependency
work limits. These checkpoints provide active same-bundle carry reuse. Cross-bundle pure
block transforms use the separate keyed service below. The C staged bridge exposes these services
through phase-local opaque handles and copied byte states, as described above.


## Pure block transition cache

`DependencyPhase::block` evaluates a finite pure internal state transition. Its
compute callback may depend only on the supplied phase inputs, explicit incoming
state, static operation parameters/metadata, phase, half-open range and mode.
Every carried control and numerical value belongs in incoming state; the callback
cannot depend on original output Q, earlier unsupplied fragments, timing or hidden
mutable state. This is a trusted operation contract, not code analysis or a grant
to batch RequestFailureOnly output observations. RequestRecord use rejects.

The session key hashes the immutable registry/operation contract, input/state
metadata, exact canonical supplied sets and bits at their actual dtype width,
phase/range/mode and actual incoming state. Snapshot identity and original Q are
excluded because they are provenance rather than transition inputs. Distinct
registries already have unique runtime implementation identities. Failed blocks
are never retained. A hit must match the incoming state's descriptor, region and
facets, and is copied into the current stage allocator. A computed state must use
that allocator. The current successful supply/history remains the evidence; old
prefix certificates are never imported through this cache.

ExecutionContext supplies optional `DependencyBlockServices` backed by its existing
accounted result LRU. Only pure/cacheable ancestry participates. Optional key work
uses `maximum_dependency_cache_work`, charged before sample hashing; zero or
exhaustion runs the transition without lookup/retention. Epoch-checked lookup and
publication prevent old Runs from repopulating a cleared cache. Borrowed values
survive eviction through their existing allocation leases. Internal hit/miss
counters are separate from completed-output `cache_hits`. Direct hosts can provide
the same services; service exceptions and ignored failures are sticky.

The real scan regression changes incoming 0 to 1 before `[1,2^54]`: the changed
blocks recompute, their earlier output changes from 1 to 2, and only subsequent
matching transitions hit after complete carry reconvergence. Mean/variance tests
check unchanged first-pass reuse and second-pass misses when mean changes. Direct
protocol tests share one host cache across two registries with distinct pure
implementations, and reject wrong state metadata, foreign allocations and ignored
host errors. The C bridge also exposes `block` with a read-only block service
table (current input read, accounted scratch, work and cancellation). It provides
no association, checkpoint, retained-owner or output-publication operations.
Incoming is copied before compute and outgoing is copied back only on success,
so caller buffers may overlap. Compute receives host-owned zeroed outgoing bytes
and must write the complete state, returning SUCCESS or an error; NEED is invalid.
Two state copies and scratch must fit the declared stage workspace.

The C callback's `user` can carry only key-determined data or fixed registered
implementation constants. Extra immutable configuration and pointer addresses are
not automatically keyed. Each phase/mode names one fixed algorithm within the
registered implementation; another transition requires a distinct identity.
The C11 scan workflow observes six initial computations and three computations
plus three hits after the reconverging edit, with complete current source support.
Null callbacks, ignored invalid reads, NEED returns, scratch exhaustion and ordinary
compute failure reject; repeated failed requests invoke compute again.


## Sparse GPU transport

[Fragment Atlas](Fragment-Atlas.md) documents the implemented exact atlas/mask
directory and SDK MSL lookup helper, including native transport verification.
C++ staged GPU execution is integrated. Bounded discovery and the staged C GPU bridge remain in progress.
