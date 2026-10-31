---
spec_schema_version: 1
id: GRD-02
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-02: Affine and lift/gamma/gain

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-02A](GRD-02A_offset_gain.md) | Per-component affine adjustment | `grade.offset_gain` |
| [GRD-02B](GRD-02B_lift_gamma_gain.md) | Lift, gamma and gain | `grade.lift_gamma_gain` |

## Current parameters

The implemented operation keys are `grade.offset_gain` and `grade.lift_gamma_gain`. The controls table remains the second dynamic input. The current parameter representation specializes the common draft as follows:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Raw mode: required component axis in the tensor cell descriptor, excluding the `batch_axes` prefix; `0 <= axis < cell_rank`. For a spatial tensor it is the declared channel axis. |
| `components` | String | Raw mode: required nonempty canonical comma-separated distinct indices, for example `"0"`, `"2,1,0"` or `"0,2,4,6"`. Indices are inside `axis`, and their written order determines controls-table row order. This String representation replaces the common draft's `Array[Int64]` notation for these members. |
| `input_description` | String | Semantic mode: canonical FMT tensor-description parameter encoding with an explicit component axis. |
| `group` | String | Semantic mode: required name of the complete selected color group. The current FMT color-point selector accepts at most four components in native encoding with straight or absent association. Group index order determines controls-table row order. |
| `max_work` | Int64 | Optional nonnegative work bound, default `INT64_MAX`; it does not alter the mathematical result. |

Use either raw `axis`/`components` or semantic `input_description`/`group`, never both. Raw selection has no four-component limit and does not infer color or alpha from channel count. Batch axes are preserved; the resolved component axis follows their prefix. Components outside the selection are independent bit-preserving bypasses.

## Shared mathematics and behavior

Affine gain may be negative. LGG has nonnegative gain and positive gamma; its signed power and operation order define an artistic curve.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
