# Basic operations

The default registry provides 21 additional CPU operations in twelve families,
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
| `mask.invert` | Canonical Float32 coverage to coverage; no parameters. |
| `mask.combine` | Two equal HW coverage inputs; String `operation=and/or/xor`, String `algebra=fuzzy/independent_coverage`. Example default fuzzy. |
| `image.mix` | Equal canonical premul RGBA A/B plus same-HW coverage mask; no parameters. Output retains image interpretation. |
| `field.box_mean` | Field to same-dtype/interpretation field; Int64 `radius` 1..64, example 1. |
| `field.gaussian_blur` | As box, plus finite Float64 `sigma` 0..64, example 1. Sigma zero is identity. |
| `mask.dilate`, `mask.erode` | Coverage to coverage; Int64 `radius` 0..64, String `footprint=square/disk`; examples 1,square. |
| `field.convolve`, `field.correlate` | Field plus same-dtype generic `[Kh,Kw]` kernel to generic field. Nonnegative Int64 `anchor_y/x` within kernel and <=2^53-1; String `boundary=clamp/zero`. Every anchor is explicit, including odd kernels. |
| `analysis.histogram` | Field to Int64 `[bins]`; Int64 `bins` 1..1048576, finite Float64 `range_min < range_max`; defaults 256,0,1. |
| `analysis.histogram_out_of_range` | Field to Int64 `[2]` ordered underflow,overflow; finite Float64 `range_min < range_max`. |
| `grade.levels` | Field to same-dtype generic field; finite Float64 `black < white`, `gamma > 0`, `out_min <= out_max`; defaults 0,1,1,0,1. |
| `numeric.minimum`, `numeric.maximum`, `numeric.abs` | Finite Float32/64 rank-1..8 arrays to same-dtype generic arrays; no parameters. |
| `field.smoothstep` | Field to Float32 canonical coverage; finite Float64 `edge0 < edge1`, defaults 0,1. |
| `field.coordinate` | No inputs, generic HW output. Positive Int64 `height/width` <=2^53-1; String `dtype=float32/float64`, `axis=x/y`, `space=pixel/normalized`. Examples float32,x,pixel. |
| `field.constant` | Same shape/dtype parameters; finite Float64 `value`, example zero. |

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

NOT is `1-A`. Fuzzy AND/OR/XOR are `min(A,B)`, `max(A,B)`, `abs(A-B)`;
independent-coverage uses `AB`, `A+B-AB`, `A+B-2AB`. Image mix interpolates all
RGBA channels as `(1-M)A+MB`. M=0/1 returns exact endpoint samples; identical
alpha remains unchanged. RGB grading should unassociate before extraction and
associate after merging, as shown in `basic-curves`.

Box/Gaussian are separable square-support filters with clamp canvas extension,
full-window normalization and Float64 intermediate values. Gaussian samples
`exp(-.5*(distance/sigma)^2)` and normalizes the finite support. Very small
sigma gives zero off-center weights. Morphology uses gray maximum/minimum,
zero canvas extension and unchanged output size. Disk includes exactly offsets
with `dx²+dy² <= radius²`; zero radius is identity. Opening/closing are two
explicit nodes. Direct morphology costs O(HW*r²); separable smoothing O(HW*r).

Correlation computes `sum(K[j]*I[p+j-anchor])`; convolution computes
`sum(K[j]*I[p+anchor-j])`. Both produce same-sized signed output, use fixed
kernel row-major Float64 accumulation and cost O(HW*Kh*Kw). There is no automatic
kernel normalization, bias, alpha operation or transfer conversion.

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
positive zero. Coordinates use pixel centers `i+.5`, or `(i+.5)/axis_length`;
x increases rightward and y downward. A single normalized pixel is .5.

## Execution, errors and resources

Elementwise: numeric min/max/abs, levels, smoothstep, mask Boolean and image mix.
Halo: box/Gaussian with the declared positive radius. Whole: curves, field LUT,
convolution/correlation, histogram, morphology and no-input generators. Whole
materialization must fit the execution budget. Static shape changes require
recompilation; control/table/kernel samples are execution bindings.

Inputs honor byte offsets, signed/zero strides and nonzero storage origins.
Outputs and scratch use the invocation allocator. PCHIP reserves three Float64
arrays per control; smoothing reserves 129 Float64 weights and a Float64
horizontal intermediate restricted to the declared input demand. Other new operations use no sample scratch allocation.
Traversal polls cancellation, and unpublished allocations are released on error.
The caller's floating environment is restored.

Nonfinite samples and unrepresentable arithmetic/output fail with OperationFailed.
Metadata and shape incompatibility return TypeMismatch; invalid static parameters
return InvalidArgument. Cross-parameter relations and generic field/table shape
restrictions are checked in the callback when existing traits cannot express
those relations, without adding shared operation-key inference. Direct typed
binding errors retain the host's InvalidArgument convention. Cancellation and
ResourceExhausted retain their own codes. No partial successful Value is returned.

## Public workflows and validation

[The standalone example](../../examples/foundations_workflow) builds using only
`find_package(Photospider CONFIG REQUIRED COMPONENTS kernel)`. Its `basic.cpp`
provides these compiled and executed graphs:

| Scenario | Oracle |
| --- | --- |
| `basic-curves` | Unassociate -> PCHIP -> extracted channel LUT -> merge -> associate -> mix; alpha remains .5. |
| `basic-masks` | Boolean -> dilation/erosion -> box feather; nine coverage samples equal 1/9. |
| `basic-filters` | Non-symmetric correlation/convolution -> absolute difference `[4,4,4]`; histogram `[0,3]`, out-of-range `[0,0]`. |
| `basic-fields` | Generated normalized coordinates/constant -> smoothstep -> levels -> local image mix; alpha remains 1. |

`tests/integration/test_basic_operations.cpp` checks independent numerical
fixtures, parameter/domain failures, ROI/Whole equivalence, unusual views,
mutable execution bindings, floating-environment restoration, cancellation,
scratch refusal and budget enforcement. The same source and all four example
scenarios are included in the isolated installed-consumer gate.

```sh
cmake --build build/issue257-static --target test_basic_operations photospider_foundations_workflow -j 8
ctest --test-dir build/issue257-static -R '^(test_basic_operations|test_foundations_basic.*)$' --output-on-failure
```

Source organization follows [the operation directory guide](../../plugins/ops/README.md).
Each registered C++ operation owns one implementation file; shared algorithms
and host adapters are private helpers. Existing operation keys and ABI records
are retained during this source migration.
