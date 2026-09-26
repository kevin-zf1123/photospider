---
spec_schema_version: 1
id: MASK-11
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-11: Deterministic seeded connectivity

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-11A](MASK-11A_flood_fixed.md) | flood_fixed | Fixed-seed-range region growing |
| [MASK-11B](MASK-11B_flood_neighbor.md) | flood_neighbor | Neighbor-relative region growing |
| [MASK-11C](MASK-11C_flood_barrier.md) | flood_barrier | Flood regions bounded by a binary barrier |

## Normative shared mathematics and interface

A/B consume image Field[C,H,W], seeds Binary[H,W], barrier Binary[H,W] in that
order. All seed/barrier storage dtypes may independently be Float32/64;
image determines numeric input coordinates. Output is Binary[H,W]. All
channels participate; extract/reorder first for a subset. Tolerance is a single
finite Float64>=0 in the image's explicitly chosen raw coordinate units, not
an implicit color-distance standard. Connectivity is required Int64 4 or 8.
A zero-seed mask is legal and yields an all-zero output, but the Whole admission
still validates the declared input domains for any nonempty Q. Seeds lying on
barrier=1 are invalid, not silently ignored or relocated.

A fixed-range: for EACH seed s, build the induced nonbarrier graph whose
vertices p satisfy abs(image[c,p]-image[c,s])<=tolerance for every c. Output the
union of the seed's reachable vertices over all seeds. A path must satisfy ONE
seed's predicate from start to finish; it cannot switch seed reference halfway.
B neighbor-range: include a nonbarrier edge p--q when every component difference
is <=tolerance. Output all vertices reachable from the union of seeds in this
single graph. Exact differences/comparisons avoid subtract-overflow. No adaptive
mean or scan-order-dependent update of the reference color is permitted.
C consumes only seeds,barrier and marks seeded connected components of barrier=0.
No input layer is painted/mutated; applying color is a separate MASK-17 workflow.

All members are Whole on every declared input for nonempty output demand,
including full finite validation of A/B image. Seed/barrier masks are Control
and topology Data; tolerance statics affect graph identity. Output ROI is merely
a projection after global closure, not a local flood from cropped seeds.
Neighbors have no off-canvas vertices. Deterministic BFS is a reference; traversal
order may vary only if the graph closure and exact result are unchanged. B/C
O(N*C) / O(N) time, O(N) queue+visited scratch. A naive reference O(S*N*C),
O(N) scratch reused per seed plus output union; budget seed multiplicity rather
than promising O(N). Poll within BFS and between seed passes. No successful
partial flood on cancellation/work exhaustion. Use barrier thresholding and
explicit color conversion before flood; do not advertise a color-managed fill.

## Sources, independent evidence and review boundary

[S02](../research-sources.md#s02); [S15](../research-sources.md#s15)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../examples/mask_morphology_oracle/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
