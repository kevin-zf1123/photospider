---
spec_schema_version: 1
id: MASK-12
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-12: Deterministic labels and associated component measurements

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
All seven members are registered in the bundled strict CPU set through fixed
invocation ABI 8. The Proposed specification status remains separate.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-12A](MASK-12A_label_compact.md) | label_compact | Four/eight-connected compact component labels |
| [MASK-12B](MASK-12B_label_min_pixel.md) | label_min_pixel | Four/eight-connected MinPixel labels |
| [MASK-12C](MASK-12C_component_count.md) | component_count | Count declared positive label IDs |
| [MASK-12D](MASK-12D_component_areas.md) | component_areas | Associated component area table |
| [MASK-12E](MASK-12E_component_bboxes.md) | component_bboxes | Associated component bounding-box table |
| [MASK-12F](MASK-12F_component_bundle.md) | component_bundle | MinPixel labels with complete component attributes |
| [MASK-12G](MASK-12G_filter_area_index.md) | filter_area_index | Filter labels through their associated area index |

## Normative shared mathematics and interface

A/B consume Binary[H,W] and connectivity Int64 4|8; output Int64 Labels[H,W].
A assigns compact IDs 1..K by first row-major foreground pixel. B assigns
ID=1+minimum row-major pixel index of the component. Background=0. Neither
union-find root address nor thread/tile encounter order is an output ID.
Both labeling members are provided with fixed, separately named semantics;
there is no implicit selection between compact and MinPixel numbering.
Compact IDs are contiguous 1..K. MinPixel IDs may have gaps, so consumers must
use explicit IDs and must not infer component count from the largest ID.
MinPixel IDs are deterministic for an unchanged snapshot, NOT persistent across
edits that split/merge/change the minimum. Binary input is an unchecked premise.
The required maximum_count Int64 in 0..N bounds the admitted output count; the
authoring recommendation is N. K=0 and maximum_count=0 are legal. Exhausting this
explicit algorithmic output capacity fails the call without publishing a prefix;
it is not an input-domain validation pass.

C/D/E consume nonnegative Int64 Labels[H,W]. They measure distinct positive IDs,
not max(label). They do not re-prove connectivity of imported labels: a repeated
ID is one declared object even if its pixels are disconnected. Descriptors carry
no requirement that imported IDs remain connected. Measurement neither splits
nor relabels objects and has no connectivity parameter. For labels [[7,0,7]],
count is 1, area row is [7,2], and bbox row is [7,0,0,1,3]. Connectivity-based
relabeling requires an explicit separate operation and a new source association.
The workflow retains the source Labels ObjectId and spatial basis alongside
each standalone measurement Result; these are caller-owned association facts,
not implicit Result attributes. C produces
count:Int64[1]. Let N=H*W, checked during preparation. D returns static Tensor
members rows:Int64[N,2] and count:Int64[1]; E returns rows:Int64[N,5] and
count:Int64[1]. The first count[0] rows contain one record per distinct positive
ID in ascending order. D columns are id,area; E columns are
id,y_min,x_min,y_max_exclusive,x_max_exclusive. Remaining rows are zero-filled.
No background or missing-ID record occurs in the effective prefix. All-background
input has count[0]=0 and unchanged positive Tensor shapes. Count and areas are
exact pixel counts, not summed soft coverage. Consumers read count through an
ordinary fixed payload read and use the effective prefix without scanning IDs,
padding or associations. Both labeling bases use this contract. Attribute tables
are paired by the workflow with the exact source Labels ObjectId and spatial
basis. That association, sorted unique positive IDs, correct areas/counts and
zero padding are caller premises, including for empty tables. Equal label bytes
from another object do not satisfy the premise. Consumers do not compare ObjectId
or recompute measurements to validate an imported table. Component IDs in pixels
are distinct from source ObjectId. Preparation checks the static table schema.

Bounding boxes use half-open integer index ranges [y_min,y_max_exclusive) and
[x_min,x_max_exclusive), in that column order after id. Height and width are
the corresponding maximum minus minimum, without adding one. Exclusive maxima
may equal H or W and are not sample indices. These are index ranges, not
continuous pixel-cell edge coordinates. One pixel at (2,3) has bbox [2,3,3,4].
Distinct disconnected regions with the
same input ID share one enclosing bbox by the declared-label measurement rule.

F consumes Binary and performs B plus complete measurements in one prepared
shared stage. Its static members are labels:Int64[H,W], rows:Int64[N,7] and
count:Int64[1], N=H*W. Effective rows have columns
id,area,min_pixel,y_min,x_min,y_max_exclusive,x_max_exclusive; unused rows are
zero-filled. Labels and table retain the same generated Labels ObjectId/basis.
The output Result contains all three fixed members; materializing the complete
Result requires all of their capacity tiles and corresponding work/storage.
The operations SDK provides `ps::mask::component_bundle_schema(H,W)`;
D/E use `component_area_index_schema(N)` and `component_bbox_index_schema(N)`.
All three factories declare CompleteBundle publication scopes.

G consumes labels:Labels Tensor plus area_index:complete D Result, with required
minimum_area Int64>=1. Callers provide the correctly paired table with exact
counts/areas and complete sorted unique positive IDs. G trusts those premises and
returns Binary in required output_dtype (float32|float64):
`[label!=0 && area[label]>=minimum_area]`. Background can skip the table lookup.
There is no runtime source-identity comparison, table remeasurement or payload
validation pass; malformed tables have no rejection guarantee.

All members have complete-input mathematical support for nonempty output
selection. Preparation derives static capacity and fixed tile reads. A producer
constructs counts, effective rows and zero padding in bounded prepared work,
using explicit intermediate state where shared. C measures distinct positive IDs
and cannot use maximum ID as count. Each retained output tile has its own backing;
publication follows call retirement, including multi-member Results. Reference
BFS is O(N) work with O(N) queue/labels; measurements are O(N+K log K) with O(K)
index scratch. A hash map does not define row ordering. F accounts for all labels
and table capacity. These members have no registered paged-components
factory or implicit disk spill.

## Native implementation

[`components.cpp`](../../../../plugins/ops/04-mask-morphology/components.cpp)
registers all seven member keys and uses the shared bounded row/BFS helpers in
[`components.hpp`](../../../../plugins/ops/04-mask-morphology/components.hpp).
A/B/F flood whole components in first-pixel order. F accumulates area, minimum
pixel and half-open bounds during that same traversal; it does not relabel a
second time. Each nonempty request lowers to one collective fixed invocation
with complete source support. D/E/F close every member to all capacity tiles,
including count-only selection. The call-retirement export contains the complete
bundle. A deliberately partial workflow RET remains a projected Result under
the frozen kernel's selection rules; pass a complete export or complete RET as
G's area_index input. Compiling an empty selection emits no runtime task.
Selecting fewer RET tiles from an existing Plan preserves its compiled call
exports under the kernel's `select_outputs` contract.

C/D/E aggregate exact Int64 IDs in a growing hash index. Only the K aggregate
records are sorted for D/E; C needs no ordering. The active index/records use
O(K) space and expected measurement work is O(N+K log K), including geometric
index growth. Hash placement never determines output order. Worst-case scratch
capacity is admitted statically from N before execution. Let P be the smallest
power of two at least 2N. Admitted scratch bytes are:

| Members | Scratch bytes |
| --- | ---: |
| A/B | 16N |
| C | 8P + 8N |
| D | 8P + 16N |
| E | 8P + 40N |
| F | 16N + 56 maximum_count |
| G | 9N |

This excludes input owners, per-tile outputs and host metadata, which are also
charged by the execution context. G reads the effective table prefix once into
an ID/selection lookup, uses exact integer binary search and reuses the previous
positive label's lookup. It neither scans padding nor validates payload premises.
Work is O(K + Q log(K+1)) in the worst case for Q emitted samples.

Contiguous binary source spans use exact AArch64 NEON zero comparisons when
available; other builds/layouts use scalar loads. All fixed views honor their
origins, offsets and strides. Generated binary output is canonical +0/1.
Cancellation and the optional max_work guard are polled in bounded spans,
hash probes, sorting and flood loops. Exceeding maximum_count returns
ResourceExhausted/CapacityLimit without publishing a partial call Result.
The frozen kernel, ABI and source association rules are unchanged.

## Sources, independent evidence and review boundary

[S16](../research-sources.md#s16)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
