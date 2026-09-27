# Basic operations

The default registry provides the remaining basic CPU operations,
using the existing public WorkflowDocument, Compiler and ExecutionContext APIs.
ABI/Traits remains 7. The [research](../built-in_ops/00-foundation/basic-operations-research.md)
records algorithm sources and the approved first-version boundaries. The
[Chinese mirror](zh/Basic-Operations.zh.md) follows this implementation contract.

## Inputs and parameters

Fields are Float32/Float64 `[H,W]`, with generic, ScalarField or canonical
coverage interpretation. Unless specified otherwise, output dtype matches the
first input. Numeric binary arrays and field/table/kernel pairs require equal
dtypes; binary numeric/mask/image operands also require matching shapes.
No broadcasting, casting or GPU execution is implicit. All listed parameters are
required; defaults below are explicit choices in workflow construction.

| Operation | Input/output and required parameters |
| --- | --- |
| `curve.sample_linear`, `curve.sample_monotone` | Generic controls `[K,2]`, K>=2, to generic `[count]`. Int64 `count` 2..1048576; finite Float64 `domain_min < domain_max`; String `out_of_domain=reject/clip`. Defaults: 256, 0, 1, reject. Finite controls have strictly increasing x; y may turn, be signed or HDR. |
| `field.apply_lut_1d` | Field plus generic same-dtype `[N]`, N>=2, to generic field. Explicit Float64 `domain_min/max` and String `out_of_domain=reject/clip`; defaults 0,1,reject. |
| `image.mix` | Equal canonical premul RGBA A/B plus same-HW coverage mask; no parameters. Output retains image interpretation. |
| `analysis.histogram` | Field to Int64 `[bins]`; Int64 `bins` 1..1048576, finite Float64 `range_min < range_max`; defaults 256,0,1. |
| `analysis.histogram_out_of_range` | Field to Int64 `[2]` ordered underflow,overflow; finite Float64 `range_min < range_max`. |
| `grade.levels` | Field to same-dtype generic field; finite Float64 `black < white`, `gamma > 0`, `out_min <= out_max`; defaults 0,1,1,0,1. |
| `numeric.minimum`, `numeric.maximum`, `numeric.abs` | Finite Float32/64 rank-1..8 arrays to same-dtype generic arrays; no parameters. |
| `field.smoothstep` | Field to Float32 canonical coverage; finite Float64 `edge0 < edge1`, defaults 0,1. |

## Numeric and image semantics

Curve queries include both endpoints, using endpoint-weighted uniform coordinates.
Collapsed sample coordinates fail. Linear interpolation hits control points;
PCHIP uses weighted harmonic interior slopes and limited one-sided endpoint
slopes. Two controls reduce to linear interpolation. No output clipping is
applied to linear curves; PCHIP constrains floating rounding to the segment's
control-value range. Queries outside the control domain reject or return its
nearest endpoint. LUT coordinates include both table-domain endpoints, with
linear interpolation and the same reject/clip policy. These ordinary tables do
not establish the SampledSignal metadata used by the separate `lut.apply_1d`.

Image mix interpolates all
RGBA channels as `(1-M)A+MB`. M=0/1 returns exact endpoint samples; identical
alpha remains unchanged. RGB grading should unassociate before extraction and
associate after merging, as shown in `basic-curves`.

Histogram edges use endpoint-exact, compensated uniform Float64 interpolation;
collapsed edges fail. Bins are left-closed/right-open, with the final bin
including the upper endpoint. Binary search compares these edges directly,
with O(HW*log(bins)+bins) work. Out-of-range values are excluded from the bins;
request the separate out-of-range node to account for them. Counts use checked
Int64. Levels computes `t=clamp((x-black)/(white-black),0,1)`, then
`out_min+(out_max-out_min)*pow(t,1/gamma)`; gamma=1 directly uses compensated
interpolation in the original input interval to preserve cancellation; gamma>1 lifts midtones.
Smoothstep computes `t*t*(3-2*t)` using the analogous clamped edge coordinate.
Min/max zero ties choose negative/positive zero; abs changes negative zero to
positive zero. The legacy `field.coordinate` and `field.constant` test operators
are retired; their implementations and registry entries are removed without aliases.
New generation specifications do not imply available replacement operators.

## Execution, errors and resources

Elementwise: numeric min/max/abs, levels, smoothstep and image mix.
Whole: curves, field LUT and histogram. Whole materialization must fit the execution budget. Static shape changes require
recompilation; control/table samples are execution bindings.

Inputs honor byte offsets, signed/zero strides and nonzero storage origins.
Outputs and scratch use the invocation allocator. PCHIP reserves three Float64
arrays per control. Traversal polls cancellation, and unpublished allocations
are released on error.
The caller's floating environment is restored.

Nonfinite samples and unrepresentable arithmetic/output fail with OperationFailed.
Metadata and shape incompatibility return TypeMismatch; invalid static parameters
return InvalidArgument. Cross-parameter relations and generic field/table shape
restrictions are checked in the callback when existing traits cannot express
those relations, without adding shared operation-key inference. Direct typed
binding errors retain the host's InvalidArgument convention. Cancellation and
ResourceExhausted retain their own codes. No partial successful Value is returned.

## Public workflows and validation

[The standalone example](../../examples/foundations_workflow) uses the installed
public kernel to execute maintained numeric and expression workflows.
`tests/integration/test_basic_operations.cpp` checks numerical fixtures,
parameter failures, ROI equivalence, views, execution bindings, cancellation,
resource limits and floating-environment restoration.

```sh
cmake --build build --target test_basic_operations photospider_foundations_workflow -j 8
ctest --test-dir build -R '^(test_basic_operations|test_workflow_numeric_reductions|test_workflow_expression_lut)$' --output-on-failure
```
