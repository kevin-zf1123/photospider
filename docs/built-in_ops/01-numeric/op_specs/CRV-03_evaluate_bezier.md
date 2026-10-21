---
spec_schema_version: 1
id: CRV-03
function: evaluate_bezier
operation_family: curve.evaluate_bezier
proposed_operation_keys:
  - curve.evaluate_bezier_strict
  - curve.evaluate_bezier_accelerated_apple_silicon
  - curve.evaluate_bezier_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-03: evaluate_bezier

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for status, registry/profile
identity, platform availability, floating environment, resource/error handling
and acceptance. The per-component curve rules below take precedence. Completed
clarification does not change the Proposed status or implementation boundary.

## Scope and interface

`curve.evaluate_bezier_<profile>` evaluates quadratic or cubic parametric Bezier curves at explicit local parameters. It returns D-component numeric values in query-row order. Components may turn back, repeat or remain constant; no geometry, color role, arc length, smoothing or monotonicity is inferred. [CRV-02](CRV-02_sample_bezier_function.md) separately samples a single-valued function by solving Bx(t)=x.

| Ordered input | Complete sample shape and dtype |
| --- | --- |
| 0 anchors | Float32/Float64 [K,D] |
| 1 handles | Float32/Float64 [K-1,degree-1,D] |
| 2 segment_indices | Int64 [N] |
| 3 t | Float32/Float64 [N] |

Each input Result has one tensor member under any schema id/version/key. Shapes use complete `sample_shape()`, including batch axes. Anchors, handles and t independently accept Float32/Float64. Require K=2..65536, D>=1 and N>=1, with K*D, (K-1)*(degree-1)*D and N*D each <=2^40. D=1 retains its component axis. The compiler checks shapes, dtypes and overflow-safe products before payload execution, including Empty requests.

Required static parameters are Int64 `degree` equal to 2 or 3, and String `dtype` equal to `float32` or `float64`. `evaluate_bezier_node` defaults dtype to Float64; direct nodes specify both parameters. Output `values` is a Result using `photospider.tensor` v1/member `samples`, ordinary shape [N,D], selected dtype and no domain description. Integer coordinates require explicit conversion. No output axis, automatic segment selection, squeeze or extrapolation mode is provided.

## Control reconstruction and mathematical value

For segment j, quadratic controls are anchors[j], RN64(anchors[j]+handles[j,0]), anchors[j+1]. Cubic controls are anchors[j], RN64(anchors[j]+handles[j,0]), RN64(anchors[j+1]+handles[j,1]), anchors[j+1]. Thus quadratic/outgoing offsets use the first anchor; cubic incoming offsets use the final anchor. Adjacent segments share an anchor but may have independent tangents and corners. Coincident or degenerate anchors are valid.

Widen each consumed Float32 component exactly to Float64. Round every anchor-plus-offset reconstruction separately to Float64, nearest/ties-to-even. These reconstructed binary64 controls define the curve; raw anchors retain their widened bits. For degree d, selected segment j and component c, the exact mathematical value is:

```text
B_c(t) = sum(r=0..d, binomial(d,r)*(1-t)^(d-r)*t^r*P[r])
```

Strict rounds the complete polynomial once directly to the destination dtype. Sequentially rounded de Casteljau blends do not define the strict reference. At t=+0 or -0 select the first anchor, and at t=1 select the final anchor. Endpoint conversion does not reconstruct handles and preserves the selected anchor's zero sign. For an interior exact zero, return -0 only when every reconstructed control of that component is -0; otherwise return +0. Nonzero underflow retains the mathematical sign.

The accelerated profiles follow the [final FP32 four-ULP contract](NUM_accelerated_contract.md), including exact endpoint/zero rules and control-range bounds. Each component stays between its minimum and maximum reconstructed controls correctly converted to output dtype. Conversion overflow used only for these bounds acts as extended infinity, allowing another finite result. Current profiles all use the same exact polynomial calculation with profile-specific integer helpers. Results are independent of request partition, neighboring queries and task order.

## Caller preconditions and segment safety

For each selected row require 0<=segment_indices[i]<K-1 and finite 0<=t[i]<=1. Queries may be repeated or unordered. Segment bounds and the t range/finiteness are unchecked caller premises. Control addresses are formed directly; no segment guard or payload validation runs.

Consumed control components must be finite, reconstructed absolute controls must remain finite Float64, and selected final outputs must be finite and representable in the chosen dtype. These are unchecked caller preconditions. Endpoint mathematics uses only the selected anchor component. Generic controls outside the selected mathematical stencil have no additional finite requirement. An interior cannot use an overflowing reconstructed control even if an abstract curve with unbounded controls could cancel. Numerical precondition violations have no promised diagnostic or numerical result. Static signatures and internal exact-capacity/resource checks remain operational checks.

## Fixed execution and physical demand

Each selected output tile lowers to one fixed CPU Host task. For output region rows [r,r+R) and columns [c,c+C), its immutable read regions are:

| Port | Logical support before source tile closure |
| --- | --- |
| anchors | [0,K) x [c,c+C) |
| handles | [0,K-1) x [0,degree-1) x [c,c+C) |
| segment_indices | [r,r+R) |
| t | [r,r+R) |

Each source's own tile grid closes its region. Batch/spatial/atomic axes can enlarge the physical support; an atomic component axis can include other columns. Complete candidate control rows are required because segment indices are dynamic. Static handles dependencies remain even when every runtime query selects an endpoint. Upstream producers must satisfy their own contracts for all required output tiles, including values mathematically unused by this consumer.

The task classifies each selected row once and reuses segment/t across its columns. It evaluates only selected global cells and writes preallocated output tiles. The mathematical stencil consists of one anchor at an endpoint or all degree+1 controls of one segment/component in the interior. Empty selection has no payload tasks or reads. Selection from an existing Plan retains the closure of its saved tasks; fresh compilation determines narrower task regions.

Preparation validates all input/output metadata, the kernel symbol/contract, exact scratch size, empty task parameters, canonical output geometry and the exact input tile slots. Prepared input views support signed/zero strides, offsets and unaligned storage. DirectTiles can retain those views; DenseHost imports use explicit Plan COPY tasks and their declared allocations.

## Resources, publication and lifetime

Each invocation owns preallocated `ExactPolynomial` plus `ExactSampling` scratch, independent of K, N and D. The kernel keeps one row classification and a degree+1 control array; it allocates no payload or full coefficient table. Metadata and source tile descriptions scale with the physical closure. Output storage covers selected tiles. Work is O(selected rows + selected cells*degree) plus exact arithmetic. Large logical N or D therefore does not alone require a complete dense output allocation.

The exact polynomial arena retains capacity guards and bounded degree. Coefficients use units 2^-1075; the supplied t is imported with 1074 fractional bits. Horner evaluation and its denominator use the polynomial's actual degree after trimming, so constant and reduced-degree curves retain their scale. For valid degree<=3 and t in [0,1], the numerator bound is 5329 bits. These arithmetic rules also apply to concurrent Runs, which share immutable preparation and own separate scratch/output allocations.

FloatingEnvironment establishes RN and gradual underflow before conversion/reconstruction and restores caller state. Cancellation is checked at rows, components and exact arithmetic steps. Resource admission, cancellation, stale/binding/layout/backend failures and internal capacity/divisor guards retain their operational categories. Declared tile writes become internal dependencies on success. The call's public Result appears only after all required tasks succeed and the call retires in order; a failed task cannot publish a successful partial call Result.

Compatible source replacements reuse preparation. Retained output Results own allocations independently of input backing and the execution context. Content caching and repeated Result identity are outside this contract. Per-value arithmetic counters are not exposed.

## Acceptance and implementation

`plugins/ops/01-numeric/parametric_bezier.cpp` registers the three fixed CPU keys. The public `evaluate_bezier_node` constructor authors the four explicit input edges and static parameters.

Quadratic fixture: anchors=[[0,0],[2,0]], handles=[[[1,2]]], segment_indices=[0,0,0], t=[0,0.5,1] gives [[0,0],[1,1],[2,0]]. Cubic fixture: anchors=[[0,0],[3,0]], handles=[[[1,3],[-1,3]]], segment_indices=[0], t=[0.5] gives [[1.5,2.25]]. Use the independent [Fraction Bernstein reference](../../../../oracle/ops/numeric/crv03/reference.py), with separate RN64 control reconstruction, for valid selected cells. Its complete-output request expansion must be replaced in a temporary checker by the actual selected tile cells. Remove temporary checkers after validation and retain the oracle.

Compare strict result bits and accelerated ULP/range/zero guarantees; current exact profile implementations can additionally be compared bitwise. Cover both degrees, constant/reduced-degree curves, loops, repeated rows, shared endpoints, D=1, mixed dtypes, reconstruction midpoints, huge cancellation, subnormal t/results and signed zero. Check endpoint selection with mathematically unused generic nonfinite controls and valid selected interiors separately. Numerical and index-premise violations are excluded from valid-input acceptance; static segment dtype/shape mismatches still fail preparation.

Public workflow checks must cover sparse/nonzero output tiles, source-grid closure and selected component reads, ordinary/batch/atomic axes, all-port reverse/unaligned/zero strides, Empty, malformed static/prepare descriptions, compatible rebinding, concurrent Runs, all caller rounding modes, cancellation, scratch/output admission, failed-call publication and retained output lifetime. Use separate valid extreme shapes, such as D=2^39,N=1 and D=1,N=2^40, respecting all product bounds. Verify selected execution under explicit budgets instead of inferring it from metadata acceptance. Report each CPU profile actually executed; local Apple Silicon evidence does not establish x86 or GPU execution.

- [Implementation notes](../math-implementation.md#crv-03-parametric-bezier-evaluation).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
