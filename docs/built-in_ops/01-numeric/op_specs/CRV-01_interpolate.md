---
spec_schema_version: 1
id: CRV-01
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-01: interpolation family

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

This family comprises four independent interfaces. Each selected operator has a separate primitive specification, rather than a generic mode-dispatch operation.

## Purpose and split

Interpolate supplied known points to evaluate one-dimensional functions at explicit dynamic query positions. The curve passes through its known points; this input model differs from CRV-02's Bezier anchors and off-curve handles. Queries can be supplied by a numeric sequence generator or an arbitrary position array rather than being generated internally from a static domain.

Single-function and multiple-function forms are separate operations:

- Single-function interface: x[K], y[K], query[N] -> values[N].
- Multiple-function interface: x[K], y[K,C], query[N] -> values[N,C], with multiple functions sharing the knot/query x coordinates. The independent multi-function specifications define column-wise mathematical rules and product limits.

One operation never serves both ranks, and the function axis is never silently squeezed or inserted. Linear and PCHIP interpolation also use independent operation names, with their separate multiple-function counterparts. There is no static method switch.

- [CRV-01A](CRV-01A_interpolate_linear.md): single-function linear.
- [CRV-01B](CRV-01B_interpolate_pchip.md): single-function PCHIP.
- [CRV-01C](CRV-01C_interpolate_linear_multi.md): multi-function linear.
- [CRV-01D](CRV-01D_interpolate_pchip_multi.md): multi-function PCHIP.

## Interface

The four interfaces are registered as sixteen keys and authored through public constructors. Each workflow input and output is a Result. An input schema contains one tensor member under any schema id and member key; `sample_shape()` supplies x[K], y[K] or y[K,C], and query[N]. Each input can use Float32 or Float64 independently. The `values` output uses schema `photospider.tensor`, member `samples`, no attached domain description, and shape [N] or [N,C]; the multi-function column is an ordinary sample axis.

The registry also contains `curve.sample_linear` and `curve.sample_monotone`, which use a controls tensor and a generated static-domain grid. They remain separate operation contracts. The sixteen explicit-query keys in this family accept x, y and query tensors and do not alias those controls-based operations.

## Mathematics

Strict and Float64 output are the exact rational value of the whole formula converted once with round-to-nearest-even. Knots and selected clamps remain exact.

The mathematical stencil is local: an exact knot or clamp uses one y, linear interpolation uses two endpoints, and PCHIP uses up to four neighboring y values. Unused y samples are not scanned for finiteness.

## Behavior

Query order and repetitions are unrestricted. Each output row `r` depends only on `x[0:K]`, `query[r]` and `y` at the stencil knots (columns `c` independently in the multi form). Region and tile results equal the corresponding slice of the whole result.

Finite x with strictly increasing values, finite consumed query/y values, representable final outputs and queries within the knot interval under `reject` are unchecked caller preconditions (see [foundation contract](../../00-foundation/contracts.md)). No global numeric validation scan is performed. Clamp, endpoint selection, extrapolation and exact rounding retain their mathematical definitions. Violating numerical caller preconditions has no specified numeric diagnostic. Static errors use `InvalidArgument` or `TypeMismatch`.

## References

- [Curve category](../curves.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
- [CRV-02 sampled Bezier function](CRV-02_sample_bezier_function.md).

## Native fast numerical profile

The four additional operation keys are `curve.interpolate_linear_fast`, `curve.interpolate_pchip_fast`, `curve.interpolate_linear_multi_fast` and `curve.interpolate_pchip_multi_fast`. SDK constructors select them with `CpuNumericProfile::Fast`. They retain the same ports, static parameters and output shapes. This profile uses ordinary binary64 computation and a final output-dtype cast, instead of NUM exact-rational final rounding or its certified accelerated bound. Intermediate native overflow and underflow are not repaired.

For interval width `h_i=x[i+1]-x[i]`, secant `d_i=(y[i+1]-y[i])/h_i`, and `t=(q-x[i])/h_i`, linear evaluation uses `fma(q-x[i],d_i,y[i])`. PCHIP constructs the specified sign-limited endpoint slopes and weighted harmonic interior slopes. The harmonic calculation scales interval widths and the smaller absolute secant before division, avoiding reciprocal overflow for small secants. With `a=h_i*m_i`, `b=3*delta-2*a-h_i*m_(i+1)`, `c=a+h_i*m_(i+1)-2*delta`, its cubic is `y[i]+t*(a+t*(b+t*c))`, evaluated by Horner FMA.

Exact knots and clamps select their stored ordinate before polynomial evaluation. Extrapolation uses the corresponding retained endpoint slope. K=2 reduces to the secant. Outputs depend on the knot and ordinate tables supplied to the current Run.
