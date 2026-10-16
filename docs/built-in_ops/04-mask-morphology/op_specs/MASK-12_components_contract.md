---
spec_schema_version: 1
id: MASK-12
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-12: Deterministic labels and associated component measurements

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members.

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
edits that split/merge/change the minimum. The whole input is validated before
publication. The domain cap is NOT implicit: required maximum_count Int64
0..N, authoring N. K=0 and maximum_count=0 are legal. Exceeding it is a domain
error, not resource exhaustion and not permission to publish first components.

C/D/E consume nonnegative Int64 Labels[H,W]. They measure distinct positive IDs,
not max(label). They do not re-prove connectivity of imported labels: a repeated
ID is one declared object even if its pixels are disconnected. Descriptors carry
no requirement that imported IDs remain connected. Measurement neither splits
nor relabels objects and has no connectivity parameter. For labels [[7,0,7]],
count is 1, area row is [7,2], and bbox row is [7,0,0,1,3]. Connectivity-based
relabeling requires an explicit separate operation and a new source association.
Descriptors carry
the label basis; outputs retain source ObjectId association. C produces
count:Int64[1]. Let N=H*W, checked during preparation. D returns static Tensor
members rows:Int64[N,2] and count:Int64[1]; E returns rows:Int64[N,5] and
count:Int64[1]. The first count[0] rows contain one record per distinct positive
ID in ascending order. D columns are id,area; E columns are
id,y_min,x_min,y_max_exclusive,x_max_exclusive. Remaining rows are zero-filled.
No background or missing-ID record occurs in the effective prefix. All-background
input has count[0]=0 and unchanged positive Tensor shapes. Count and areas are
exact pixel counts, not summed soft coverage. Consumers read count through an
ordinary payload Need and apply positive/sorted-ID validation to effective rows;
padding is validated separately as zero. Both labeling bases use this contract.
Attribute tables bind to the exact source Labels ObjectId and spatial basis.
Consumers must reject a different source object even when shape, IDs or all
label samples are equal. Shape checks, content hashes and caller assertions do
not replace this association. This object identity is distinct from component
IDs stored in pixels. Empty tables carry the same source association obligation.
Association checks do not waive table schema, ordering, uniqueness or value
validation required by the consuming member.
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
A CompleteBundle scope includes all three members and therefore all capacity
tiles; requesting it may require full-capacity work and storage. The precise
schema codec remains a registration gate. This Proposed contract is not an alias
for the existing components4.labels factory.

G consumes labels:Labels Tensor plus area_index:complete D Result, required
minimum_area Int64>=1. It verifies the exact Labels ObjectId association,
complete sorted unique effective positive IDs, exact payload count and correct area counts against
labels BEFORE publication; equal bytes from another object do not suffice.
Output Binary in required output_dtype (float32|float64): `[label!=0 && area[label]>=minimum_area]`. Missing/extra IDs,
wrong areas or foreign associations fail. A table is not trusted because its
shape looks right. G may skip a per-pixel lookup for background but still owes
its declared complete-input association/table validation closure. Independent outputs
from unrelated sources must never be attached by numerical coincidence.

All members declare complete-input input Data/Validation for nonempty output demand.
Static capacity is prepared before execution; effective count and contents are
computed once in a Run-shared stage and published under a fixed CompleteBundle scope. C cannot use a header's maximum ID as count. D/E/G
need complete source validation even for a requested subset of rows; no partial
or unvalidated prefix is promised. Dirty input -> whole affected labels/tables.
Reference BFS O(N) for A/B, O(N) queue/labels; measurements O(N+K log K), O(K)
index scratch. A hash map does not define row ordering. F includes all labels
and rows in capacity admission. Existing paged factory's disk support and
Conservative(All) traits do not automatically transfer to these proposed Values.

The separate paged-components factory retains its own published contract.
Its storage and paging capabilities are not implicitly provided by these members.

## Sources, independent evidence and review boundary

[S16](../research-sources.md#s16)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
