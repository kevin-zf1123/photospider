# Paged four-connected components and area index

Package 0.10 exposes `make_component_operation`, `component_area_schema` and
`component_filter_schema` through the installed C++ API. C operation ABI 9 and
the existing Float32 `mask.components` compact-label behavior are unchanged.

## Public profile

`ComponentsSpec` requires positive HW, `H*W <= (INT64_MAX-4095)/32`,
`maximum_count <= INT64_MAX` and MinPixel IDs for these factories. Input masks
are facet-free UInt8 HW; any nonzero byte is foreground. The named
`components_min_pixel_v1` basis uses background zero and component ID
`1+minimum(row-major pixel index)`. IDs are deterministic within a snapshot;
edits that split or merge components need not preserve them.

| Stage | Input and output |
| --- | --- |
| `components4.labels` | UInt8 HW → complete Components: N Int64 labels and K Int64[id,area,min] rows |
| `components4.area` | Complete Components → complete sorted Int64[id,area] index with RuntimeCount K |
| `components4.filter` | Components plus its area index → complete N-row UInt8 0/1 mask |

The index schema is `photospider.component_area_index`, with field `rows` and
`component_area_basis_v1` metadata retaining the Components specification.
The filter schema is `photospider.component_filter`, field `mask`, and
`component_filter_basis_v1` metadata. Both retain HW domain and the full basis;
the filter's required positive Int64 `minimum_area` belongs to the operation
semantic key. It supports the complete positive Int64 range without floating
conversion of the threshold. Output is exactly
`label!=0 && associated_area[label]>=minimum_area`.

`maximum_count` constrains final K only. K=0, maximum_count=0 and N>0 are legal:
labels contains N zeros, Components and area tables have zero rows, and filter
produces N zeros. The limit cannot substitute for capacity admission of the N
fixed labels or N private union records. Exceeding the count limit is an
explicit OperationFailed/InvalidDomain failure with Domain/Group scope before
publication. Page, disk and work exhaustion remain ResourceExhausted failures.

## Rank-union recipe and basis proof

1. Create one private temporary file and extend it once by checked 32*N bytes.
   Read source strips into bounded callbacks and write contiguous records:
   background `[0,0,0,0]`; foreground `[i+1,0,i,1]` for parent, rank, minimum,
   area. Parent addresses are one-based; minimum is zero-based.
2. Traverse pixels in row-major order. For foreground pixels, union left when
   x>0 and top when y>0. No page boundary changes this adjacency. Every current
   vertex is still a self-root before its first edge: previous vertices only
   visited smaller indices. Retain the current component root across its two
   unions, updating both address and record when another root wins.
3. Find neighbour roots by bounded paged parent reads. Same-root union is a
   no-op. Otherwise link by rank, sum disjoint areas and take the minimum
   position. Apply both cached record updates before the next neighbour. Non-root
   area/minimum fields may be stale and are never used as root facts.
4. Once every edge is processed, scan pixels again and find roots. Emit
   `minimum+1` as the label. Append `[id,area,minimum]` only when the scanned
   pixel equals that minimum, producing unique sorted rows without an in-RAM
   sorting table. Publish labels and table together only after all writes finish.

Each grid edge is visited once through left/top orientation. Union never joins
distinct true connected components; every path's edges are included, so the
result is exactly the four-connected partition. Root minimum and area are
preserved by minimum and disjoint-set addition. The root address can differ
from the published ID. Rank-only parent paths have O(log N) depth; the
implementation checks addresses and a 64-hop bound. It does not claim inverse
Ackermann complexity because it does not perform path compression.

In the 2×127 comb, N=254, foreground=191 and private UF payload=8128 bytes.
This two-pass initialization begins union with 191 roots. The design's 65
temporary row components describes another illustrative schedule, not the
actual provisional count of this implementation. The final K is one.

## Index validation, support and ownership

Area waits for a complete validated Components object and copies ID/area pairs
through bounded pages. Its association names exactly that Labels ObjectId.
Filter first requires this exact association and equal K; same shape, same K
or even equal numeric contents cannot replace object association. Before any
filtering it compares every index pair against the complete Components table.
This also validates order, positivity and completeness of externally supplied
index rows. Failures use InvalidAssociation, Association scope and the index's
ObjectId. Lookup uses a sorted paged lower-bound search and one retained cache
window. Background skips lookup. A missing nonzero label fails; it never
silently becomes area zero.

All three stages use CompleteBundle. Relations remain Conservative(All), with
separate reserved descriptor support even for K=0. Count/basis validation of an
imported Components object is distinct from proving its connectivity; the new
labels recipe and independent BFS provide that latter evidence for generated
results. Input associations and read windows retain predecessors and mandatory
backing after ExecutionContext destruction until the last owner releases them.

## Actual resource envelope

UF occupies 32*N logical disk bytes, admitted in 4096-byte encoded extents.
Create, Extend and dependent writes are separate coordinator stages. Source,
union and output windows are at most min(user page,1024) bytes. Labels requires
at least a 32-byte window; area/filter require at least 24 bytes. Two output
slabs and a possible smaller final table copy have bounded overlapping
capacity. Labels retains four mutable LRU pages across edge processing, parent
finds, both union updates and final emission. A dirty victim is written before
its replacement is read; cache hits continue within the current poll. Both
union records are resident before either cached update is applied. Emission
uses the same cache, so it sees the latest root facts. After the final append,
remaining private dirty pages can be discarded with UF: no later stage reads
that private tree. Association validation consumes only the published fields.

Labels declares 8192 bytes of callback workspace, including at most 4096 bytes
of cached tree payload and the bounded output slabs/tail copy. Area and filter
retain their 4096-byte workspace. All read-window owners, metadata and
persistent fields remain separately charged to the root.

Union and final root discovery cost O(N log N), index copying/verification O(K),
and filtering O(N log K). The existing Components validator additionally costs
O(NK+N log K) work and can repeatedly reload its single page while alternating
label and table fields. ResultBuilder's small appends also rewrite alignment
padding. Neither validation I/O nor padding writes can be budgeted merely from
logical final payload sizes. These operations consume actual cumulative
work/I/O; exhausted limits fail and release partial private storage. The public
example uses a finite one-million-stage envelope and explicit root limits.

[components_workflow](../../examples/components_workflow/README.md) supplies
public execution and installation commands, exact BFS references, dynamic/empty
cases, a paged index and data exceeding Host capacity. Measurements are the
product's managed-capacity ledger, not process RSS hard bounds.
