---
spec_schema_version: 1
id: MASK-05
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-05: Flat finite-footprint extrema

Inherits the [MASK baseline](MASK_common_contract.md).

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-05A](MASK-05A_dilate.md) | dilate | Flat dilation |
| [MASK-05B](MASK-05B_erode.md) | erode | Flat erosion |

## Mathematics

Input is Coverage or Binary. Required static `footprint` is square|diamond|disk|custom. For square/diamond/disk require radius Int64>=0; for custom reject radius and require `offsets: String`. The signed-pair String grammar is `dy:dx;dy:dx;...`, each integer canonical ASCII (0 or optional minus with no leading zeros, no +, no -0). Pairs are unique, lexicographically sorted; (0,0) is present and B=-B. Reject empty/noncanonical/asymmetric sets. Factories may canonicalize before serialization; direct nodes must be canonical. The array ABI is not extended for this static encoding. Static support/capacity arithmetic is checked during preparation; runtime coordinate representability is an unchecked premise.

B_square={(dy,dx):max(abs(dy),abs(dx))<=r}; B_diamond uses abs(dy)+abs(dx)<=r; B_disk uses exact integer dy²+dx²<=r². These are pixel-grid footprints, NOT physical-space isotropic disks under non-square spacing; MASK-07A owns that. Dilation(p)=max_{b in B} extended_input(p+b); erosion uses min. Outside D is +0. B is symmetric, so reflection is immaterial. At equal extrema select the center when tied, otherwise the lexicographically first offset; selected bits are copied exactly, including -0. The complete footprint is the declared mathematical support; finite Coverage/Binary values are caller premises, with no scan on early extrema or identity cases. Radius zero copies the input bits.

## References

[S05](../research-sources.md#s05); [S06](../research-sources.md#s06); [S07](../research-sources.md#s07)
