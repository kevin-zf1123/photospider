---
spec_schema_version: 1
id: FIL-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-04
function: gaussian_filter
proposed_operation_keys:
- filter.gaussian_filter_strict
- filter.gaussian_filter_accelerated_apple_silicon
- filter.gaussian_filter_accelerated_x86_64
numeric_reference: B+E
oracle_scope: mathematical_reference
research_sources:
- S03
- S25
---
# FIL-04B: gaussian_filter

Apply the baked64 Gaussian profile.

Inherits [FIL-04](FIL-04_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gaussian_filter_strict`, `filter.gaussian_filter_accelerated_apple_silicon`, `filter.gaussian_filter_accelerated_x86_64`.

Input port `input` selects a Result containing one tensor. Output port `output` publishes one Result tensor with the same complete sample shape, including batch axes. Color interpretation is explicit static configuration under the FMT contract. Raw numeric inputs do not reject NaN or infinity under a blanket finite-value rule.

### Parameters

Require `sigma_x/y`, `radius_x/y`, axes, boundary, and `cval`. Sigma controls weight decay; radius independently controls finite support. Zero sigma requires zero radius; positive sigma permits radius zero. Sigma and cval are finite; sigma and radii are nonnegative. The operation has no arbitrary bias and no constructor defaults.

`x_axis` and `y_axis` index the complete tensor sample shape, including the batch-axis prefix. For an RGBA sample shape `{N,L,H,W,4}`, use `y_axis=2` and `x_axis=3`.

## Mathematics

Treat `kx` and `ky` as exact binary64 constants. Return `RN_t(Σ ky*kx*I / ((Σky)(Σkx)))` with one final rounding. Boundary handling follows FIL-02A. This is not the result of filtering an already rounded intermediate Gaussian image. If both radii are zero, the output is an exact bit copy of the input.

The support is the Cartesian product of the nonzero 1D taps within the static radii after global boundary mapping; the kernel is derived static control data. Zero sigma only removes support on its own axis. There is no maximum sigma. Arithmetic follows FIL-02A.

## Acceptance

Verify exact preservation of constants and impulse reflection. Compare Float32 and Float64 results with a baked64 rational reference. Do not assert that two truncated blurs equal one blur with sigma `sqrt(s1²+s2²)`.

The straight-alpha reference cases belong to FIL-01D's fused color-window profile.

## References

[S03](../research-sources.md#s03), [S25](../research-sources.md#s25).
