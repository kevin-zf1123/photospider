---
spec_schema_version: 1
id: GRD-19
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-19: Finite bilateral base/detail local grade

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-19A](GRD-19A_bilateral_log_base.md) | Bilateral base of log luminance | `grade.bilateral_log_base` |
| [GRD-19B](GRD-19B_local_base_grade.md) | Adjust a supplied local tonal base | `grade.local_base_grade` |
| [GRD-19C](GRD-19C_shadow_highlight_local.md) | Local shadows, highlights and detail | `grade.shadow_highlight_local` |
| [GRD-19D](GRD-19D_local_detail.md) | Independent local detail strength | `grade.local_detail` |

## Shared mathematics and behavior

A generates the Float64 log base; B adjusts a supplied base; C exposes complete local shadow/highlight and detail controls; D exposes detail strength alone. Detail is z-base in log2 luminance. Detail gain 1 retains it, 0 removes it, and larger gains enhance it. The square support crops at the full-image edge. Source hidden colors participate in the luminance base.

## Execution-tile dependencies

For B/C/D, the common bypass-only dependency guarantee applies to complete execution tiles. Empty requests consume no payload. A tile containing only unselected components depends solely on the corresponding source coordinates.

Spatial tiles cover every component slot. An alpha-only ROI in an RGBA image with selected RGB therefore requires the inputs of its enclosing execution tile: B requires the supplied base, controls and edges; C/D require the selected source colors over the cropped halo and their dynamic controls. Alpha and unselected AOVs retain their original bits.

Identity controls may skip feature and grading arithmetic inside the operator. The Plan retains its declared dependencies and any scheduled upstream producers.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
