---
spec_schema_version: 1
id: GRD-14
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-14: Hard and local-mean threshold

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-14A](GRD-14A_threshold.md) | Fixed threshold | `grade.threshold` |
| [GRD-14B](GRD-14B_threshold_local_mean.md) | Local-mean threshold | `grade.threshold_local_mean` |
| [GRD-14C](GRD-14C_threshold_local_gaussian.md) | Gaussian-weighted local threshold | `grade.threshold_local_gaussian` |
| [GRD-14D](GRD-14D_threshold_sauvola.md) | Sauvola local threshold | `grade.threshold_sauvola` |

## Current parameters

The registered keys are the member-table keys. A uses the selector and `keepdims` parameters in its member page. B/C/D accept one rank-two [H,W] image with no implicit batch repetition. Their `radius` is Int64; Gaussian `sigma` and Sauvola `k`/`dynamic_range` are Float64. Dynamic thresholds and offsets retain their required Float64[1] ports.

### Current native numerical profile

Local members build one numerical grid over the full declared image; output requests select observations from that grid. Cropped boundaries always refer to the declared image extent. This fixes the arithmetic grid independently of ROI and tile partition.

For prefix-based decisions, use the recomputation guard

`B = 64*eps64*H*W*max(1,max_abs_input)*max(1,(H+W)/N)`,

where N is the cropped neighborhood count. The separable Gaussian uses the same base expression without the final shape factor. The prefix factor accounts for cumulative path length before rectangle cancellation. On the normalized Sauvola domain, propagate the shifted-moment bounds through variance with `E_v = B*(4+3*B)`. The threshold guard is `B*(1+abs(k)) + abs(k/R)*(sqrt(E_v)+B*sqrt(v))`; the standard-deviation error term uses the exact mean bound of one on [0,1]. This includes the quadratic variance-error term and avoids relying on an uncertain computed mean.

A candidate with nonfinite arithmetic or a comparison gap within the guard is recomputed from the same local neighborhood in row-major order. Centered accumulation preserves constant-image means. The recomputation publishes the Float64 mean before the offset subtraction or Sauvola threshold expression, and uses the ordinary `>=` comparison. The guard selects recomputation; it is never added to the threshold or used as a label tolerance. Native and strict decisions may differ when a specified Float64 stage rounds a value at a discrete boundary.

## Shared mathematics and behavior

A is a point comparison. B/C/D use cropped square neighborhoods. A mask value of one means the source is at least the local threshold; callers invert the binary mask if dark foreground is desired. Gaussian and Sauvola definitions use explicit finite radius.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
