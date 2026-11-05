---
spec_schema_version: 1
id: GRD-27
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-27: ASC CDL mathematical styles

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-27A](GRD-27A_asc_cdl_fwd.md) | ASC CDL forward with clamping | `grade.asc_cdl_fwd` |
| [GRD-27B](GRD-27B_asc_cdl_rev.md) | ASC CDL reverse with clamping | `grade.asc_cdl_rev` |
| [GRD-27C](GRD-27C_asc_cdl_fwd_no_clamp.md) | ASC CDL forward without clamping | `grade.asc_cdl_fwd_no_clamp` |
| [GRD-27D](GRD-27D_asc_cdl_rev_no_clamp.md) | ASC CDL reverse without clamping | `grade.asc_cdl_rev_no_clamp` |

## Shared mathematics and behavior

The four ASC CDL styles follow [CLF 3.0 equations](../research-sources.md#clf). Constants are the CDL luma weights, not a working-basis Y row. No-clamp styles bypass power for a negative pre-power value. Forward/reverse are independently callable; clipping is information-losing.

## Current parameter encoding

Color selectors use `axis:Int64` with ordered, distinct `components:String` indices, or `input_description:String` with `group:String`. The axis is relative to the cell descriptor before any batch prefix. The shared work parameter is `max_work:Int64`, default 9223372036854775807. See [static selector encoding](GRD-21_saturation_contract.md#static-parameter-encoding).

Fixed static arrays use comma-separated String values in the declared row-major order, without brackets. Float64 entries use decimal numeric text; Int64 entries use signed decimal integers. Their declared lengths are checked during static preparation. Dynamic tensors in member signatures remain required ordered input ports.

Every member has three dynamic inputs in order: input, sop, saturation. The three rows of Float64[3,3] sop correspond to R/G/B; each row is slope,offset,power. Saturation is Float64[1]. Raw component order is R/G/B; semantic selection requires a complete RGB group. The fixed CDL luma weights do not depend on an RGB-basis parameter.

## Current native prototype

SOP and saturation arithmetic uses Float64. Saturation equal to one selects the pre-saturation tuple directly. No-clamp members bypass power for negative pre-power values, and neutral no-clamp controls copy source bits. Forward and reverse clamping stages retain their individual equations. Native power evaluation follows the common continuous floating acceptance profile, including the scalar path for values outside the bounded approximation range.

For a region with at least 512 pixels and effective powers `0 < p <= 4`, the current prototype can prepare a power table from the current SOP controls. Write a positive normal base as `x = 2^e m`, with `1 <= m < 2`, choose `i = floor(64(m-1))`, and let `c_i = 1 + (i+1/2)/64` and `r = (m-c_i)/c_i`. The table stores `c_i^p` and `2^(ep)` for `-16 <= e <= 16`; the residual factor `(1+r)^p` uses the degree-six binomial polynomial with coefficients `b_0=1`, `b_j=b_(j-1)(p-j+1)/j`. This table applies only to bases in `[2^-16, 2^17)`. It is rebuilt from each current control tuple rather than retained across later executions.

The prototype retains native division in inverse saturation and inverse slope. Values outside supported approximation ranges, ill-conditioned offset or saturation cancellation, and results close to finite-output limits use the scalar equations. This selection does not alter the member clamping, negative-base bypass, or continuous acceptance rules.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
