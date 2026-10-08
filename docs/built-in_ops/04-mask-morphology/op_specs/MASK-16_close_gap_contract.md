---
spec_schema_version: 1
id: MASK-16
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-16: Explicit temporary-boundary gap closing and fill

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-16A](MASK-16A_bridge_axis_gaps.md) | bridge_axis_gaps | Bridge bounded horizontal and vertical barrier gaps |
| [MASK-16B](MASK-16B_fill_axis_gaps.md) | fill_axis_gaps | Flood using a temporary axis-gap boundary |
| [MASK-16C](MASK-16C_fill_morphological_gaps.md) | fill_morphological_gaps | Flood using a temporary morphology-closed boundary |

## Normative shared mathematics and interface

The initial scope includes all three members: standalone axis-gap bridging,
axis-gap closure followed by flood, and morphological closure followed by flood.
Each keeps its named geometric rule. Axis-gap size counts missing cells on a
row or column; morphology radius defines a footprint and is not a universal
maximum gap width. Neither path selects connections using tangent estimation,
curve tracing, endpoint pairing or a seed-dependent closure objective.

No stroke-layer object, hidden reference compositing or UI interaction lives in
these primitives. Caller explicitly thresholds/combines desired reference layers
into a Binary barrier; the original line/reference data is immutable. These
members implement bounded reproducible gap heuristics, not commercial fill-tool
compatibility or intelligent stroke continuation. Manual references confirm
feature existence only. Seed-conditioned learned/curvature endpoint matching is
outside this draft's precise members.

A: bridge_axis_gaps takes barrier and required maximum_gap Int64>=0. On the
ORIGINAL barrier enumerate maximal horizontal and vertical runs of zero pixels
bounded at both ends by ones on that same row/column. Add the whole run iff its
number of zero pixels<=maximum_gap. Combine all additions simultaneously by OR
with the original barrier; do not cascade newly added pixels into another pass.
Canvas-edge open runs are never bridged. Gap is a count of missing grid cells,
not center distance, Euclidean diameter or twice a morphology radius. Diagonal
or curved gaps need other members. maximum_gap=0 is binary identity.
A can be local: for each output pixel inspect horizontal/vertical offsets up
to maximum_gap+1; exact admitted support is the union of these two axis stencils,
clipped to D, not the whole square. Reference O(N*(gap+1)); dense run enumeration
O(N), with exact dirty support still required. Huge statics need checked bounds.

B: fill_axis_gaps takes barrier,seeds (Binary HW), applies A, then MASK-11C with
required fill_connectivity 4|8. All four Binary outputs use the seeds floating dtype and input spatial basis.
Ignore seeds on the ORIGINAL barrier: S = seeds AND NOT original. Validate
all Binary samples before filtering. After preparing the base closure T, define
the effective barrier E = original OR (T AND NOT S).
This removes newly added barrier pixels exactly at seed locations, simultaneously
for all seeds; it never clears original barriers or an entire adjacent gap.
Do not recompute closure after this clearing. Flood uses E and the filtered
seed set S. The effective temporary barrier is excluded from the fill output and
never committed to the source. Cleared seed sites are traversable and included
in the reached fill. Empty S gives empty fill, including when every seed was on the original barrier. The seed override may reopen
a path through the closure; no containment guarantee overrides this rule.
Standalone A continues to expose the seed-independent base closure.
C: fill_morphological_gaps takes barrier,seeds and the MASK-05 footprint statics,
fill_connectivity. Compute `temporary = OR(original, MASK-06 closing(original))`
on the extended lattice, apply the same E = original OR (temporary AND NOT S)
rule, then flood E=0. Original-barrier seeds are filtered before override. The OR makes "do not erase
original barriers" explicit even if a future closing variant differs. Radius
has the morphology footprint meaning; there is NO universal max-gap-size
promise. This may close unwanted narrow passages or connect unrelated strokes.

B/C expose four named [H,W] outputs, all exact Binary:

| Output | Definition | Meaning |
| --- | --- | --- |
| fill | flood(S,E) | Reached region; no effective barrier pixels are included. |
| barrier | E | Effective temporary barrier after seed filtering and reopening. |
| ignored_seeds | seeds AND original | Seeds ignored because they hit original obstacles. |
| reopened_barrier | T AND S | Newly added obstacle pixels removed by surviving seeds. |

The original barrier is immutable. ignored_seeds never authorizes removing an
original obstacle. reopened_barrier is the exact set T minus E, not an entire
gap or all closure additions. Empty S makes fill and reopened_barrier zero;
barrier still reports the prepared closure and ignored_seeds still reports
filtered seeds. Outputs are named workflow exports, not logging side effects
or a new primitive registration. The authoring templates expose these named
values and preserve their explicit dependencies and host publication rules.
Diagnostic-only requests do not require flood; no hidden full-image count or
coordinate-list reduction is included. Consumers can explicitly reduce a mask
if they need counts. Any nonempty output conservatively validates both complete
Binary inputs; no diagnostic output bypasses input-domain validation.

B/C declare Whole support for all outputs; fill requires global flood, while
barrier preparation and diagnostic formulas do not themselves require flood. Reference scratch O(N) temporary, queue, visited; B work O(N*(gap+1)),
C naive O(N|B|²), plus flood. Return selection and diagnostic masks; painting/growing under
linework/anti-aliasing/feather are separate MASK-07/08/17 stages. A successful
whole fill cannot hide an exhausted gap-preparation work budget.

## Sources, independent evidence and review boundary

[S20](../research-sources.md#s20)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
