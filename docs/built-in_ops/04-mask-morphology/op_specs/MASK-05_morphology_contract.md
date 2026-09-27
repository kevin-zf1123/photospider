---
spec_schema_version: 1
id: MASK-05
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-05: Flat finite-footprint extrema

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-05A](MASK-05A_dilate.md) | dilate | Flat dilation |
| [MASK-05B](MASK-05B_erode.md) | erode | Flat erosion |

## Normative shared mathematics and interface

Input is Coverage or Binary. Required static `footprint` is
square|diamond|disk|custom. For square/diamond/disk require radius Int64>=0;
for custom reject radius and require `offsets: String`. This new signed-pair
String grammar is `dy:dx;dy:dx;...`, each integer canonical ASCII (0 or optional
minus with no leading zeros, no +, no -0). Pairs are unique, lexicographically
sorted; (0,0) is present and B=-B. Reject empty/noncanonical/asymmetric sets.
Factories may canonicalize before serialization; direct nodes must be canonical.
The array ABI is not extended for this static encoding. Coordinate/demand
arithmetic is checked; a large radius is not silently limited to legacy 64.

B_square={(dy,dx):max(abs(dy),abs(dx))<=r}; B_diamond uses abs(dy)+abs(dx)<=r;
B_disk uses exact integer dy²+dx²<=r². These are pixel-grid footprints, NOT
physical-space isotropic disks under non-square spacing; MASK-07A owns that.
Dilation(p)=max_{b in B} extended_input(p+b); erosion uses min. Outside D is
+0. B is symmetric, so reflection is immaterial. At equal extrema select the
center when tied, otherwise the lexicographically first offset; selected bits
are copied exactly, including -0. The sample domain is validated for the entire
required footprint even when an early extremum is 0 or 1. Radius zero is an
identity with required local validation, not a validation bypass.

Reference time O(|Q|*|B|), scratch O(|B|) for admitted offsets or bounded
iteration. A rectangular separable min/max algorithm can use O(N) work
independent of window width on a dense image; that complexity does not transfer
to arbitrary disk/custom footprints. It must still preserve selected-zero ties,
exact required support and page legality. Scratch/radius preparation is budgeted.

## Sources, independent evidence and review boundary

[S05](../research-sources.md#s05); [S06](../research-sources.md#s06); [S07](../research-sources.md#s07)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
