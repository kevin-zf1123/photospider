---
spec_schema_version: 1
id: CRV-02
function: sample_bezier_function
operation_family: curve.sample_bezier_function
proposed_operation_keys:
  - curve.sample_bezier_function_strict
  - curve.sample_bezier_function_accelerated_apple_silicon
  - curve.sample_bezier_function_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
---

# CRV-02: sample_bezier_function

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

This specification records the Bezier anchor/handle generator and its
numerical, execution and acceptance contracts. The registered implementation
uses Result inputs and outputs; the specification status remains Proposed.

## Interface and outputs

`curve.sample_bezier_function_<profile>` samples a piecewise quadratic/cubic single-valued function of x. It solves Bx(t)=q and then evaluates By(t); it does not substitute t=q. Static degree is 2 or 3 for every segment. Anchors are shared between adjacent segments; handles are relative offsets.

| Ordered input | Complete sample shape and dtype |
| --- | --- |
| 0 anchors | Float32/Float64 [K,2], K=2..65536 |
| 1 handles | Float32/Float64 [K-1,degree-1,2] |
| 2 start | Float32/Float64 [1] |
| 3 end | Float32/Float64 [1] |

The final control axis is x,y. Each Result has one tensor member under any schema id/version/key. Shapes use complete `sample_shape()` including batch axes; all four dtypes are independent. Input views may have signed/zero strides, offsets and unaligned storage. Metadata checks apply to every edge, including schema-only edges, without reading payload.

| Static parameter | Required type and domain | Authoring default |
| --- | --- | --- |
| degree | Int64, 2 or 3 | Required |
| count | Int64, 1..1048576 | Required |
| dtype | String float32/float64 | Float64 |
| out_of_domain | String reject/clamp | Reject |

`sample_bezier_function_node` authors these parameters and four explicit inputs. Direct nodes supply every parameter. Output `values` uses `photospider.tensor` v1/member `samples`, ordinary shape [count] and selected dtype. Output `axis` uses the same schema/member, Float64 [3] and one atomic trailing axis. Neither output carries a domain description. No sorting, automatic handle construction, tangent extrapolation, units or y clipping is implied.

## Sampling coordinates and numerical preconditions

For count N>1, start a and end b must be finite and numerically distinct; either direction is valid. Define the sampling grid and axis as:

```text
q[0] = a; q[N-1] = b
q[i] = RN64(((N-1-i)*exact(a)+i*exact(b))/(N-1)), 0<i<N-1
step = RN64((exact(b)-exact(a))/(N-1))
axis = [a,b,step]
```

Endpoints preserve their widened bits. Interior coordinates use exact weighted endpoints and one RN64 rounding, rather than accumulated step. Each selected values coordinate must differ numerically from its immediate valid neighbors. Only selected axis output requires finite nonzero step with the correct direction; values-only evaluation does not compute step or impose its representability. Axis-only evaluation does not require per-coordinate separation.

For N=1, q[0]=a and axis=[a,a,+0]. Start must be finite; end retains static schema checks but has no payload dependency or numerical precondition. Both outputs exclude its producer unless another selected graph path requires it.

All these numerical conditions are unchecked caller preconditions. Under reject, selected q must lie inside the closed first/last anchor x domain. Clamp selects the nearest endpoint y outside that domain and preserves the original sampling axis. A selected value must be finite and representable in its output dtype. Numerical precondition violations have no promised diagnostic or numerical result.

## Controls and function topology

For segment j, quadratic controls are anchors[j], RN64(anchors[j]+handles[j,0]), anchors[j+1]. Cubic controls are anchors[j], RN64(anchors[j]+handles[j,0]), RN64(anchors[j+1]+handles[j,1]), anchors[j+1]. Widen Float32 exactly and round each absolute handle coordinate separately to Float64, nearest/ties-to-even. These reconstructed binary64 controls define the mathematical curve.

Whenever values is selected, callers guarantee the complete x topology: every anchor x and x offset is finite, every reconstructed x control is finite, anchor x is strictly increasing, and every segment's x derivative is nonnegative throughout [0,1]. Endpoints are strictly increasing, so isolated stationary derivatives are valid and each interior query has one inverse. Backward folds violate the precondition; no epsilon or automatic handle repair is applied. Axis-only requests impose none of these control conditions.

For degree d in {2,3}, regard reconstructed coordinates as exact binary rationals:

```text
B(t) = sum(k=0..d, choose(d,k)*(1-t)^(d-k)*t^k*Pk)
```

The exact derivative conditions defining the valid domain are:

- Quadratic: both P1.x-P0.x and P2.x-P1.x are nonnegative.
- Cubic: set u=P1.x-P0.x, v=P2.x-P1.x, w=P3.x-P2.x. The derivative divided by three is g(t)=(u-2v+w)t^2+2(v-u)t+u. Its values at 0 and 1 and its minimum at an interior vertex must be nonnegative.

This is a polynomial condition, not sorted control-point order. Cubic x controls [0,3/4,1/4,1] are valid despite crossing handles; [0,2,-1,1] violates the condition. The kernel trusts this global topology and does not retain a validated copy or run a topology scan.

Only mathematically used y components require finite values and finite RN64 reconstruction. Anchor hits/clamps use one anchor y; interior evaluation uses the selected segment's two y anchors and relative y handles. Unused generic y components have no additional finite requirement. Shared anchors provide C0 continuity; sharp corners and independent tangents are valid. y may overshoot, be negative or exceed one. K=2 still uses the supplied quadratic/cubic handles and does not imply a linear curve.

## Exact evaluation and profile semantics

Strict finds the unique mathematical root of Bx(t)=q and correctly rounds By(t) once directly to Float32/Float64 after the separate RN64 control reconstruction. Exact anchor/clamp selection converts the chosen y and preserves its zero sign. Every mathematically exact interior zero is +0, including an identically zero ordinate with negative-zero controls. A nonzero value rounded to zero retains its mathematical sign. Gradual underflow applies.

The accelerated profiles follow the [final FP32 bound](NUM_accelerated_contract.md) relative to that exact reference, including exact endpoints, zero signs and range/fallback conditions. Current profiles use the same exact inverse/evaluation algorithm with scalar/NEON/AVX2 integer helpers. An x residual alone cannot establish y rounding near a stationary derivative. Unresolved y rounding must fail operationally rather than publish an unproved approximation.

The implementation bisects a dyadic root bracket, bounds the entire ordinate through interval Horner and resolves common zeros and output midpoint ties with exact polynomial decisions. Refinement is bounded at 8192 fractional bits. Exact arena capacity and divisor guards remain; finite capacity does not promise success for every valid algebraic input. No per-value solver/fallback counters are exposed. Unsupported CPU profiles fail BackendUnavailable; profile names do not imply GPU execution.

## Fixed execution and dependency projection

Each selected values tile is one fixed CPU Host task. Its reads cover complete anchors and handles plus start, and end only when N>1. The task searches anchor x directly through prepared views, once for each selected global output coordinate. It reconstructs only the selected segment's controls and evaluates only selected outputs. Bounded search and endpoint selection keep every control address within the static support.

Axis has a separate atomic task reading only start/end, or only start for N=1. Its controls are schema-only references. All four original port numbers and schemas remain available during preparation. Empty output requests emit no pure payload tasks. Each source grid closes its read regions independently; source atomic or batch axes determine physical coverage. Upstream producers must satisfy their contracts for every declared source tile, including unused y entries or handles that this consumer does not use mathematically.

Preparation checks static schemas/parameters, the values/axis symbol and contract, exact scratch size, one canonical write on the correct output port, and the exact input tile slots. Selected tasks share one IR call's publication outcome. Successful tile writes become internal dependencies; public values/axis Results appear only when all required tasks succeed and the call retires in order. An axis-only call excludes values work; a joint request does not promise independent axis success if values fails.

## Resources, cancellation and lifetime

Values scratch contains one preallocated ExactSampling plus ExactBezier state per invocation, independent of K and N. Axis uses only coordinate arithmetic state; N=1 axis uses zero scratch and accepts a null scratch array. The kernel allocates neither a full topology vector nor a complete values array. Values storage covers selected tiles; selected axis storage is 24 bytes. Physical input descriptors and retained source views scale with the closure. DirectTiles may retain affine storage; DenseHost imports use explicit Plan COPY allocations.

Selection costs O(M log K) for M selected values, plus RN64 reconstruction and actual exact inverse work. A selected segment has at most four x/y control pairs. The exact polynomial arena has 640 64-bit limbs per number and 96 slots; refinement/storage guards return ResourceExhausted when a bound is exhausted. Scratch reservation scales with simultaneously admitted lanes, not the logical sample count.

FloatingEnvironment establishes RN and gradual underflow before arithmetic and restores caller state. Cancellation is checked per selected value, search iteration and inside exact arithmetic. Preparation, binding/layout, backend, resource, cancellation and internal exact failures preserve their operational categories. Numerical caller preconditions are not runtime validation gates.

Independent Runs share immutable preparation and own separate scratch/output. Compatible control/scalar rebinding reuses preparation. Retained output owners survive source and execution-context destruction. Content caching and repeated Result identity are outside this contract.

## Acceptance

Use the retained [independent Fraction reference](../../../../oracle/ops/numeric/crv02/reference.py), which uses de Casteljau, rational Euclid/Sturm and exact rounding-boundary decisions. Temporary selected-tile checks must remove its complete-output index expansion, its final full-array index projection and its runner-specific maximum-count resource shortcut. Expand actual selected tile cells before comparing. Keep global topology checks for valid-input filtering. Treat domain/overflow labels as exclusions from numerical preconditions rather than required runtime errors. An unresolved oracle is not an expected value.

| ID | Required behavior or valid-domain fixture |
| --- | --- |
| B01 | Quadratic anchors [(0,0),(1,0)], offset (0,1): q=1/4 gives y=1/2 |
| B02 | Cubic controls [(0,0),(0,1/4),(0,3/4),(1,1)]: q=1/8 gives y=1/2, through inversion |
| B03 | Degree-2/3 y=x with descending start=1,end=0,N=3 gives [1,0.5,0] and axis [1,0,-0.5] |
| B04 | N=1 excludes an end producer; axis=[start,start,+0] has zero scratch and excludes controls |
| B05 | Mixed input dtypes and compatible source rebinding preserve mathematical results |
| B06 | Crossing-handle x=[0,3/4,1/4,1] and stationary derivatives succeed; backward folds are outside the caller domain |
| B07 | C0 joins preserve sharp corners; exact anchor hits ignore y handles mathematically |
| B08 | Quadratic offset (0.5,4) between [(0,0),(1,0)] gives y=2 at q=0.5, without clipping |
| B09 | Clamp selects endpoint y and preserves axis; reject requires valid selected q |
| B10 | Global x topology is a precondition; unused generic y values do not enter the numerical stencil |
| B11 | Static count/K/degree/shape/type limits, malformed fixed task signatures and source layouts |
| B12 | Nonzero/disjoint tile selections retain global coordinates and allocate only selected output |
| B13 | Axis-only excludes controls; N=1 excludes end; joint outputs share call publication |
| B14 | Flat x tangencies, steep y, exact interior +0, signed endpoint zero, midpoint ties and subnormal outputs |
| B15 | Sparse maximum count under explicit budgets, scratch rejection and cancellation during refinement |
| B16 | Concurrent Runs, multiple consumers and output owners surviving context destruction |
| B17 | Check actual available CPU profiles, unsupported-profile rejection and separate hardware evidence |

Public checks use WorkflowDocument input schemas, Result bindings, Compiler, ExecutionContext and Result tensor reads. Compare generated Bezier baking with explicit expansion and independently known values. A cubic with the B02 controls, start=0,end=1,count=9 gives selected values {0:0,1:0.5,8:1} and axis [0,1,0.125]. Sparse maximum count must be verified by actual execution, not metadata acceptance. No performance claim follows from bounded numerical checks.

- [Bezier baking template](CRV-04B_bake_lut1d_bezier.md).
- [Curve category](../curves.md).
- [NUM-01 sampling](NUM-01_sample_expression.md).
- [Common execution contracts](../../00-foundation/contracts.md).
- [Operator template](../../00-foundation/spec-template.md).
