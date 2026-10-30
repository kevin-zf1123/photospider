# PNT-05A: local_inpaint_navier_stokes

Fill an explicit binary hole mask using known neighboring image colors, preserving every unmasked sample. Intended uses are scratches, small holes and local defects. The profile fixes the OpenCV 4.12.0 single-channel Float32 `INPAINT_NS` behavior, applied independently in R, G, B order. This is a named Navier-Stokes-inspired discrete algorithm, not an arbitrary PDE solver.

PNT-05 is the existing family; PNT-05A is this concrete profile. The unsuffixed operation name is the semantic family, not a separate registration or alias. There is no GPU variant.

Telea, Poisson, PatchMatch, generated content, automatic defect detection, allowed-source masks, transparent reconstruction, time coherence and commercial pixel compatibility are outside scope. No implicit algorithm substitution.

## Interface

Output port: `image`. Required static Int64 parameter: `radius`.

### Inputs

| Order / name | dtype / shape | Semantics | Sample domain |
| --- | --- | --- | --- |
| 0 / image | Result with one `photospider.image` v1 tensor member `pixels`; Float32 `[H,W,4]` and batch axes `[N,L]` | ordered RGBA; linear sRGB primaries, D65; straight alpha; scene or display reference preserved | finite signed/HDR RGB; alpha exactly 1 |
| 1 / hole_mask | Result with one `photospider.image` v1 tensor member `pixels`; Float32 `[H,W]` and matching batch axes `[N,L]` | canonical typed coverage; non-color; same logical grid | exactly numeric 0 or 1; both signed zeros are known |

Both Results have exactly one tensor and use sample batch axes `[N,L]`. The image and mask batch extents and H/W match exactly. Their complete sample shapes are `[N,L,H,W,4]` and `[N,L,H,W]`. There is no broadcasting, quantization, implicit conversion, thresholding or unassociation. Required: `3 <= H,W <= 32768` and `H*W <= 2^31-1`. Mask value 0.5 is invalid for this operator even though it is valid coverage.

### Parameters

| Parameter | Exact type | Unit | Range | Required | Constructor suggestion |
| --- | --- | --- | --- | --- | --- |
| radius | Int64 | logical pixel spacing | inclusive `[1,32]` | yes, static | 3, explicitly supplied |

Unknown keys, absence, Float64 3.0, 0, negative values and 33 are rejected (InvalidArgument). There are no hidden defaults or clamping. There is no method, boundary, seed, iterations, dt, solver_tolerance or opacity parameter. Dtype, shape or semantic mismatches of the input Results are rejected as TypeMismatch.

### Output inference

The output clones input 0's full schema: sample shape `[N,L,H,W,4]`, with the same caller-supplied scene/display interpretation; no description is attached or propagated. Static inference reads no pixel payload.

For input I, mask M and result J, `M(n,l,y,x)=0` implies bitwise equality `J(n,l,y,x,c)=I(n,l,y,x,c)` for every channel c. All alpha samples are bitwise preserved at 1. Unmasked negative zero survives. All-zero M returns bitwise identity. Successful results are finite and are not clamped to `[0,1]`. Hole placeholders do not affect the result when known samples and M are unchanged.

## Mathematics

Indices are y,x,c; x increases rightward, y downward; pixel centers are `(x+0.5,y+0.5)`. Neighborhoods use integer pixel offsets and the inclusive disk `dx*dx+dy*dy <= radius*radius`. Only real canvas colors are candidates.

The normative discrete reference is OpenCV tag `4.12.0`, `modules/photo/src/inpaint.cpp`, single-channel Float32 `INPAINT_NS`. The pinned source's padded guard state, initial band construction, excluded exterior band, arrival time initialization `1.0e6f`, gradient indexing and edge branches are normative boundary rules, not a selectable clamp/reflect/wrap policy. Holes touching edges and corners are supported. For every non-full binary mask the result is the finite pinned-reference result; with only one known pixel, the pinned result may retain zero work values where its frontier never reaches.

Algorithm, per batch plane:

1. Validate both inputs completely and count K holes.
2. K=0 returns the identity; K=N fails.
3. Copy the original samples to the output so unedited bits and alpha are retained.
4. Convert M to an internal 0/255 UInt8 mask. For R, then G, then B, pack a Float32 work plane and set every hole sample to positive zero.
5. Initialize the pinned source's state, time and band. Use row-major initial band insertion, minimum arrival time first, insertion order for ties. Consider frontier neighbors in up, left, down, right order. Preserve `FastMarching_solve` and `icvNSInpaintFMM<float>` single-channel branches.
6. Preserve candidate loops, gradient expressions, mixed Float32/Float64 evaluation points and constants `1e-20f`, `0.01`, `1e-6f`. Conceptually, `u(p)=sum(w(p,q)*u(q))/(1e-20f+sum(w(p,q)))`; this summary does not replace the normative source expressions, state predicates or iteration ordering.
7. Nonfinite arithmetic or results fail. Write back only hole RGB.

OpenCV supports Float32 single-channel inpainting; its native three-channel input path is UInt8. This profile does not quantize Float32 color to that path.

Numerics use round-to-nearest and gradual underflow; no fast-math or FMA contraction alters the reference order. Same build, snapshot and CPU configuration repeat bitwise.

## Behavior

- Dependence is global: newly filled values propagate across the plane, so there is no regional algorithm or transitive halo. A downstream ROI may crop the complete output. Any input sample change affects the full output.
- Each batch plane solves independently; a nonempty request still validates all planes before processing any plane. Empty output demand yields an empty Result.
- For nonempty demand, nonfinite RGB, alpha other than 1 and nonbinary masks fail with OperationFailed even for all-zero masks. A full mask in any batch plane fails with OperationFailed. There is no unfillable-component rejection otherwise.
- Empty or too-small image shapes are rejected.

## Acceptance

Shape, unmasked samples and all alpha are exact. For each hole RGB sample against the pinned OpenCV 4.12.0 single-channel Float32 reference: `abs(actual-reference) <= 1e-6 + 1e-5*abs(reference)`. NaN/Inf cannot pass comparison. This profile defines the acceptance threshold; it is not an OpenCV guarantee.

| ID | Fixture | Required assertion |
| --- | --- | --- |
| T01 | zero mask with finite samples and signed-zero coverage | bit identity |
| T02 | 5x5 constant `(0.25,0.5,0.75,1)`, center hole, r=1 | constant hole within tolerance, exact outside/alpha |
| T03 | change finite placeholders only | bit-identical output |
| T04 | gradients, asymmetric lines, checkerboards, scratches, holes | per-sample pinned Float32 reference |
| T05 | four edges/corners, disjoint holes, one known pixel | pinned result, no out-of-bounds |
| T06 | full mask | OperationFailed |
| T07 | 0.5/out-of-range/NaN mask; nonopaque alpha; NaN/Inf RGB | OperationFailed |
| T08 | radius 1/32, 0/33, absent/wrong type/unknown parameter | exact closed schema |
| T09 | size 3, 1/2, 32768, 32769 | bounds accepted/rejected as stated |
| T10 | nonzero ROI and different tile geometry | crop equality of the complete output; distant invalid input still fails |
| T11 | signed/HDR constants and extreme values | no clamp; nonfinite computation fails |

## References

- OpenCV 4.12.0 [`modules/photo/src/inpaint.cpp`](https://github.com/opencv/opencv/blob/4.12.0/modules/photo/src/inpaint.cpp). Any port retains the source license and attribution.
