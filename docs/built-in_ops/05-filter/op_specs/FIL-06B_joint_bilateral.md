---
spec_schema_version: 1
id: FIL-06B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-06
function: joint_bilateral
proposed_operation_keys:
- filter.joint_bilateral_strict
- filter.joint_bilateral_accelerated_apple_silicon
- filter.joint_bilateral_accelerated_x86_64
numeric_reference: B+E
oracle_scope: mathematical_reference
---

# FIL-06B: joint_bilateral

Joint bilateral filter.

Inherits [FIL-06](FIL-06_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.joint_bilateral_strict`, `filter.joint_bilateral_accelerated_apple_silicon`, `filter.joint_bilateral_accelerated_x86_64`.

`input,guide` → `output` with the same shape as `input`; `guide` matches input spatial and batch axes and has an explicit component dimension of 1..4.

### Parameters

`radius_y` and `radius_x` are nonnegative Int64; `sigma_space_y`, `sigma_space_x`, and `sigma_range` are positive finite Float64. `metric_scale` is an explicitly supplied positive finite Float64 vector. `axis` explicitly identifies the two spatial axes. `component_axis=-1` denotes a scalar guide; otherwise it identifies the nonspatial guide axis. `metric_scale` is a comma-separated Float64 list whose length equals the guide component count. Callers must explicitly select one boundary from `reflect_half`, `reflect_whole`, `clamp`, `wrap`, or `truncate`.

## Mathematics

Numerical classification: **B+E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S definitions.

For tap d, `ws=RN64(exp(-dy²/(2sy²)-dx²/(2sx²)))`; compute `Dg=Σ((g(q+d,c)-g(q,c))*metric_scale[c])²` exactly; `wr=RN64(exp(-Dg/(2sr²)))`; compute w=ws*wr exactly; output `RN_t(ΣwI/Σw)`. The center weight is 1, so support is nonempty. FIL-06A processes each plane independently and self-guides from that plane; FIL-06B reuses the explicit guide for every output component and does not infer RGBA semantics.

## Behavior

The output depends on the input and guide samples in the footprint and on the center guide. When ws=0, that tap's guide and input samples do not participate. Other guide samples participate in w even if wr underflows to zero; the input participates only when w>0. Radius zero is an explicit identity copy.

## Acceptance

A constant guide reduces to the same positive-weight spatial blur. Equal center and neighbor guides have zero range distance. Changing an unselected joint-guide component does not change output. Scaling the guide by s requires inverse scaling of range sigma or metric scale.
