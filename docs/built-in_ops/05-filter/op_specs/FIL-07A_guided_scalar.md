---
spec_schema_version: 1
id: FIL-07A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-07
function: guided_scalar
proposed_operation_keys:
- filter.guided_scalar_strict
- filter.guided_scalar_accelerated_apple_silicon
- filter.guided_scalar_accelerated_x86_64
numeric_reference: 'S: coefficients RN64; final E'
oracle_scope: mathematical_reference
---

# FIL-07A: guided_scalar

Scalar guided filter.

Inherits [FIL-07](FIL-07_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.guided_scalar_strict`, `filter.guided_scalar_accelerated_apple_silicon`, `filter.guided_scalar_accelerated_x86_64`.

`input,guide` → `output`; `input` has C independent planes, and both inputs match in H/W and batch dimensions. FIL-07A requires a one-component guide; FIL-07B requires a guide with 1..4 components and an explicit `component_axis` distinct from the spatial axes.

### Parameters

`radius_y` and `radius_x` are nonnegative Int64. `epsilon` is a positive finite Float64 in squared units of the metric-scaled guide. `guide_metric_scale` is an explicitly supplied positive finite Float64 scalar or vector. `component_axis=-1` denotes a scalar tensor without a separate component axis. The boundary profile is fixed to clipped/truncate two-window support.

## Mathematics

Numerical classification: **S: coefficients RN64; final E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

Let B_k be the image-clipped box centered at k, and let g be the exactly scaled guide. Evaluate exactly `mu_g,mu_p,Cgg=mean(ggᵀ)-mu_g mu_gᵀ,Cgp=mean(gp)-mu_g mu_p` .`a_k=RN64_vec((Cgg+epsilon I)^-1 Cgp)` after an exact matrix solve, rounding each component once to RN64; `b_k=RN64(mu_p-dot(a_k,mu_g))` (using the already rounded a). Output `RN_t(dot(mean_{k∈Bq}(a_k),g_q)+mean(b_k))`; both means are exact. The vector SPD solution is unique and uses no arbitrary epsilon jitter; the scalar case is the same 1×1 formula.

## Behavior

For output q, use the complete B_k for every k in B_q, with worst-case H(2r) support; include the guide at q. The a/b S stages do not imply complete-input demand: exact support is the union of the two clipped windows. The guide contributes to both the coefficients and the output term.

## Acceptance

With a constant guide, a=0 and the output is box(box(input)), not a single box. At radius zero, input values are copied bitwise, including NUM-defined exceptional values. Changing input samples at distances r..2r can affect q. Large offsets in covariance test cases must not create negative approximate variance.
