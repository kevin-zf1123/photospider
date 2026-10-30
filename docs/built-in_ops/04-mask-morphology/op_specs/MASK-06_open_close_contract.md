---
spec_schema_version: 1
id: MASK-06
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-06: Opening and closing without intermediate clipping

Inherits the [MASK baseline](MASK_common_contract.md).

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-06A](MASK-06A_opening.md) | opening | Open a soft or binary mask |
| [MASK-06B](MASK-06B_closing.md) | closing | Close a soft or binary mask |

## Mathematics

Same input domains and footprint statics as MASK-05. On the infinite zero-extended source define E and D as the family erosion/dilation. Opening=D(E(input)); closing=E(D(input)). All stages use the same symmetric B. The intermediate must be available outside the final canvas: at final p, compute every intermediate at p+b and its source stencil p+b+c, then crop only final Q. An ordinary same-canvas D then E with zero at the new intermediate border is NOT this closing.

Values are selections, not floating arithmetic: all min/max tie rules are stage observable. Opening is numerically anti-extensive and closing extensive for these footprints; comparisons treat ±0 as equal. Numerical idempotence holds; a sign-bit identity after repeated extrema is not a separate promise. Acceptance includes a full 1x1 canvas: both opening with nontrivial B and closing are well defined, with opening=0, closing=1. No silent boundary-neutral rewrite.

Support `(Q+B+B) intersect D` is exact as a static stencil union, not merely radius r. Original input outside D is +0; generated intermediate outside D is NOT assumed zero.

## References

[S05](../research-sources.md#s05); [S06](../research-sources.md#s06)
