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
