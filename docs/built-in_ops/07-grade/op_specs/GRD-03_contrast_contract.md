---
spec_schema_version: 1
id: GRD-03
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-03: Linear and log-axis contrast

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-03A](GRD-03A_contrast_linear.md) | Linear contrast around a pivot | `grade.contrast_linear` |
| [GRD-03B](GRD-03B_contrast_log2.md) | Contrast on the exposure axis | `grade.contrast_log2` |

## Current parameters

Both operation keys use the existing scalar-parameter ABI. The current selector profile represents the common contract's ordered component array as `components:String`, containing unique, comma-separated canonical nonnegative decimal indices such as `"2,0,1"`. The order determines the row order in the required dynamic `Float64[Csel,2]` controls tensor. No image/control broadcasting or implicit control defaults are supplied.

| Static parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required with raw selectors; zero-based component axis within the tensor cell, excluding explicit batch axes. |
| `components` | String | Required with `axis`; ordered selected component indices in the encoding above. |
| `input_description` | String | Optional FMT tensor-description encoding; resolves component selection instead of raw `axis` and `components`. |
| `group` | String | Optional named color group, only with `input_description`. |

Raw selectors make the input coordinate meaning a caller assertion. A supplied description for `grade.contrast_log2` declares linear RGB or Gray with `gray_kind=linear_y`. Control rows contain `[contrast,pivot]`, use binary64, and are read from the current dynamic input each invocation. The native computation profile and continuous tolerances are inherited unchanged from the common contract.

## Shared mathematics and behavior

The pivot uses source units. The 0.18 suggested value applies naturally to relative linear-light data; encoded coordinates require a caller-selected pivot.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
