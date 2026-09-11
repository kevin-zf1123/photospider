# Dependency sampling operations

The default CPU registry provides `image.stmap`, `numeric.radius_gather` and
`numeric.radius_scatter` through the version-one dependency program protocol.
Each invocation computes one generic sample or one complete image-v2 pixel.
They return request-only failures; different output observations are not batched.
Input dependencies and control evidence use the exact fragment protocol in
[Dependency data and execution](Dependency-Data.md).

## STMap

`image.stmap(source, map)` takes canonical linear premultiplied Float32 RGBA
`source[Hs,Ws,4]` and generic Float64 `map[Ho,Wo,2]`. It returns Float32
`[Ho,Wo,4]` with the source's image-v2 facets. Source axes must be at most 2^40.
Map channel 0 is source x, channel 1 source y, in pixel coordinates with centers
at `i+0.5`. The required String parameter `boundary` has no implicit default:

| Value | Address rule for an integer tap i on an axis of length N |
| --- | --- |
| `constant` | Outside taps use transparent black and read no source pixel. |
| `clamp` | Clamp to [0,N-1]. |
| `wrap` | Nonnegative remainder modulo N. |
| `reflect` | Period 2N, repeated endpoints: p<N ? p : 2N-1-p. |
| `mirror` | Period 2N-2, unrepeated endpoints: p<N ? p : 2N-2-p. |

For N=1, all nonconstant modes select coordinate zero. These mappings act on
global source coordinates, never independently on individual fragments.

For each output pixel, the program first requests both map components as Control.
They must be finite and within [-2^40,2^40]. It forms `floor(x-0.5)` and
`floor(y-0.5)`, then visits taps in top-left, top-right, bottom-left, bottom-right
order. Each in-domain tap requests the complete RGBA pixel, even when its weight
is zero. Footprints deduplicate addresses; the arithmetic retains all four taps.
After reading/validating all taps, each channel uses a sequential Float64 weighted
sum and one final Float32 conversion under nearest rounding with gradual
underflow. Transparent constant taps retain their map and descriptor evidence.

Malformed metadata and unknown boundary choices fail during compilation and before
session state construction, including Empty direct queries. Invalid coordinates
or source pixels fail OperationFailed for their own output observation. Exact
set, state, discovery or allocation limits fail ResourceExhausted. Empty output
queries read no samples. Output Regions and source fragments always include full C.

## Dynamic radius sums

Both operations take generic Float64 `source[N]` and Int64 `radius[N]`, with
1 <= N <= 2^40. They return generic Float64 `[N]`, dropping input facets. They
have no parameters. Radius samples used by the operation must lie in [0,2^40].

For output o, `numeric.radius_gather` includes i when `abs(i-o) <= radius[o]`.
It reads one output-aligned radius and sums the clipped source interval.
`numeric.radius_scatter` includes i when `abs(i-o) <= radius[i]`. It scans every
source radius in chunks of at most 64 candidates, retaining the complete Control
witness, including excluded candidates. It then reads only positive Data hits.
A formerly excluded distant candidate can therefore dirty the output when its
radius changes. No optional spatial inverse index is required by this implementation.

Both accumulate source values in increasing index order with a Float64 left fold
from positive zero. Each poll establishes and restores nearest rounding and
gradual underflow; changing the caller's rounding mode cannot change the result.
Nonfinite included source values, invalid observed radii and intermediate sum
overflow fail OperationFailed. Chunking never replaces ordered addition by block
sums. A gather query does not validate radii outside its own output observations;
scatter's complete control scan is part of its declared dependency semantics.

The fixed 64-candidate state uses a host continuation lease. Candidate reads,
stage transitions, certificates and source data remain subject to execution
limits. Large scatter queries can fail explicitly when their exact scan cannot
fit these bounds; no empty certificate or approximate answer is substituted.

## Static validation and executable checks

`OperationDefinition::validate_dependency` is an optional pure metadata/parameter
validator. The compiler calls it after base inference; a direct session calls it
before deciding whether Q is Empty. It cannot read pixels, change inferred
metadata or inspect Q. The immutable definition owns it, and callback exceptions
are fenced. Empty requests can skip state construction while retaining the same
static validity rules as nonempty requests.

The [G4 workflow](../../examples/g4_workflow/README.md) runs a computed STMap
control with source pixels {0,1023}, verifies exactly 32 source bytes, and applies
a distant radius patch through InputSnapshotStore while a FrozenExecution retains
the prior result. `test_dependency_sampling` additionally checks literal finite
radius relations, changed edges with equal output bytes, control transpose,
negative coordinates/endpoints/N=1, zero-weight tap validation, Empty static
errors and rounding-mode/cross-chunk numerical invariance.
