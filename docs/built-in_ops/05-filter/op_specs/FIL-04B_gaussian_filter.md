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

Inherit [FIL-04](FIL-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Input port `input` selects a Result containing one tensor. Output
port `output` publishes one Result tensor with the same complete sample shape,
including batch axes. Color interpretation is explicit static configuration
under the current FMT contract, not a facet copied through Result. The old
ColorArray v1 interface is removed. Downstream color consumers receive their
own TensorDescription and explicitly select any ICC resource during preparation.
Raw numeric inputs do not reject NaN or infinity under a blanket finite-value rule.

## Explicit parameters and legal domain

Require `sigma_x/y`, `radius_x/y`, axes, boundary, and `cval`. Sigma controls weight decay; radius independently controls finite support. Zero sigma requires zero radius; positive sigma permits radius zero. Sigma and cval are finite; sigma and radii are nonnegative. The operation has no arbitrary bias and no constructor defaults.

`x_axis` and `y_axis` index the complete tensor sample shape, including the
batch-axis prefix, not only the cell descriptor. For an RGBA sample shape
`{N,L,H,W,4}`, use `y_axis=2` and `x_axis=3`.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Treat `kx` and `ky` as exact binary64 constants. Return `RN_t(Σ ky*kx*I / ((Σky)(Σkx)))` with one final rounding. Boundary handling follows FIL-02A. This is not the result of filtering an already rounded intermediate Gaussian image. If both radii are zero, perform an exact bit copy and do not read or validate other samples.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Lowering expands each retained output request to complete tiles and declares a fixed Cartesian-product support bounded by the static radii after global boundary mapping. A coefficient bake that runs during invocation cannot shrink the compiled read set; lowering may use the exact nonzero support only when those coefficients are already known. Structural atomic axes remain complete. Shared coefficient or image stages are explicit tasks with private SSA outputs; output tiles have distinct allocations. A complete-input implementation declares its complete input dependency without changing ordered call retirement. Empty retained selections create no coefficient or arithmetic payload tasks.

Use the Cartesian product of nonzero 1D taps; the kernel is derived static control data. Zero sigma only removes support on its own axis. There is no hidden UI maximum for large sigma, though indexing, work, and resource budgets still apply. Execution follows FIL-02A.

Complexity uses P=HW samples, C independent planes and A taps; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation and owner-lifetime rules. Successful tasks enable internal dependents; a selected Result becomes externally visible at successful call retirement. A failed call publishes no successful export, while previously retired call exports remain valid. Resource exhaustion does not silently reduce precision, spill or change algorithms.

## Oracle, fixtures, and acceptance

Verify exact preservation of constants and impulse reflection. Compare Float32 and Float64 results with a baked64 rational oracle. Do not assert that two truncated blurs equal one blur with sigma `sqrt(s1²+s2²)`.

The reference helpers are `gaussian_kernel(sigma,radius)` in [transcend.py](../../../../oracle/ops/filter/fil04/transcend.py) and `separable(image,kx,ky,anchor=(0,0),**kwargs)` in [spatial.py](../../../../oracle/ops/filter/fil02/spatial.py). See also the [oracle README](../../../../oracle/ops/filter/README.md).

Fixture coverage identifier: `gaussian_constant`. The straight-alpha reference cases belong to FIL-01D's fused color-window profile.

## Backend and registration

The proposed keys are `filter.gaussian_filter_strict`,
`filter.gaussian_filter_accelerated_apple_silicon`, and
`filter.gaussian_filter_accelerated_x86_64`. These keys are registered CPU operations through fixed invocation ABI 8 in
`plugins/ops/05-filter/`. Certified separable enclosures retain the baked64
coefficients and use exact-expression fallback whenever final rounding is not
proved. The specification remains Proposed/D1; there is no native GPU implementation. Accelerated variants must
satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without
accumulating a separate budget per tap, axis, stage, or iteration. Threshold,
ordering, boundary, copy, and output-support choices that must be exact cannot
change approximately. Float64 accelerated paths retain Float64 input/output and
exponent range; they do not first convert to Float32. Implementation status records runtime availability; performance measurements and
validation evidence are maintained separately from this normative specification.

## Conceptual DAG (not an existing API)

`static parameters + schemas -> compile fixed reads/tasks -> prepare -> unchecked arithmetic with declared rounding stages -> complete tiles -> ordered call retirement`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. complete-input and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S03](../research-sources.md#s03), [S25](../research-sources.md#s25).
