---
spec_schema_version: 1
id: MASK-13
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-13: Binary hole filling and component-size cleanup

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-13A](MASK-13A_fill_holes.md) | fill_holes | Fill every enclosed binary hole |
| [MASK-13B](MASK-13B_remove_small.md) | remove_small | Remove small foreground components |
| [MASK-13C](MASK-13C_fill_small_holes.md) | fill_small_holes | Fill enclosed holes up to an inclusive size |

## Normative shared mathematics and interface

Input Binary, same dtype/shape output. Required foreground_connectivity is 4|8;
background connectivity is its complement 8|4, not an independently guessed
same-connectivity default. Foreground removal B uses its foreground connectivity.
The node explicitly serializes foreground_connectivity. There is no independent
background_connectivity parameter: foreground 4 always implies background 8,
and foreground 8 always implies background 4. Both complementary pairs are
supported; same-connectivity pairs (4,4) and (8,8) are outside this contract.
Holes A/C are connected components of zeros under the complementary background
connectivity that contain NO canvas-edge pixel. Every background component
touching y=0,H-1 or x=0,W-1 is exterior and is never filled. This is equivalent
to background flood from a one-pixel exterior ring with the declared adjacency.
A full foreground image is unchanged; an all-background image is exterior, not
one giant hole. Diagonal openings are distinguished by complementary connectivity.

A fills all holes. B removes a foreground component iff its exact pixel count
<=maximum_removed_area (Int64>=0); C fills a hole iff its exact pixel count
<=maximum_area (Int64>=0). Both operations include equality in the processed
range. A zero threshold preserves the input Binary values after validation.
Area is pixel count (numerically px² at
unit pixels), not physical area or sum of soft mask weights. Soft masks require
explicit threshold first; restoring weights is a separately authored selection,
not silently done by these binary members. Physical spacing-scaled filtering
would use exact n*sy*sx and a separately named threshold-unit contract.

The inclusive bounds are fixed semantics, with no comparison-mode parameter.
At maximum_removed_area=10, foreground sizes 9 and 10 are removed; size 11
survives. At maximum_area=10, enclosed holes of sizes 9 and 10 are filled;
size 11 remains a hole. Exterior-connected background is never filled.

All are Whole; no bounded halo identifies a hole or completes a connected
component. Reference BFS O(N), O(N) queue/visited scratch. A/C first classify
exterior, then enumerate enclosed background. B computes components and areas.
Do not apply repeated per-tile local filling/removal. Poll during flood/count
loops; no completed-looking partial image on cancelled cleanup.

## Sources, independent evidence and review boundary

[S17](../research-sources.md#s17); [S18](../research-sources.md#s18)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
