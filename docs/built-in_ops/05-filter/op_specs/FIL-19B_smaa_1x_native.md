---
spec_schema_version: 1
id: FIL-19B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-19
function: smaa_1x_native
proposed_operation_keys:
- filter.smaa_1x_native_strict
numeric_reference: S
oracle_scope: native_reference_and_pinned_third_party_comparison
research_sources:
- S08
---

# FIL-19B: smaa_1x_native

Native CPU SMAA 1x with the explicit `smaa_1x_low_luma_v1` profile. The key is
registered through fixed invocation ABI 8. Specification status remains
**Proposed / D2_draft**. No external graphics engine is loaded by this operation.

Inherit [FIL-19](FIL-19_contract.md), [FILTER common](FILTER_common_contract.md),
[NUM](../../01-numeric/op_specs/NUM_common_contract.md), and
[FMT](../../02-format-color/op_specs/FMT_common_contract.md).
This member is independent of directional_post_aa_v1 and has no temporal state.

## Ports, shape and semantic domain

The repeated input port has one input image, followed by one scalar edge-guide
Result only when `guide=external_luma`. The sole output port is `output`.
Every Result has one `samples` Tensor member. Input/output dtype is Float32.
The output retains the complete input sample shape, including batch axes.

The required FMT description selects an opaque RGB or Gray color group.
RGB roles are explicitly red, green and blue, in any declared channel order,
with sRGB primaries. RGB and Gray both use the `srgb` transfer function.
The selected group has no alpha member and no alpha association. Other channels
are copied bit for bit. The caller supplies any required color conversion or
background compositing before this operation.

Selected color samples and the external guide are finite values in [0,1] as
unchecked numerical premises. No alpha, finite-value or range scan is introduced.
Unselected channels have no arithmetic-domain restriction. Each combination of
nonspatial, noncomponent axes is independent.

## Required static parameters

| Parameter | Type | Value/domain |
| --- | --- | --- |
| y_axis, x_axis | Int64 | Distinct axes of the complete sample shape |
| input_description | String | Canonical FMT TensorDescription parameter |
| group | String | Explicit RGB or Gray group name |
| profile | String | `smaa_1x_low_luma_v1` |
| resources | String | `iryoku-smaa-71c806a8` |
| guide | String | `internal_luma` or `external_luma` |

There are no defaults. The selected component axis cannot be a spatial axis.
The external guide has the same dtype and complete sample shape as the input,
except that its component extent is 1. It contains the already encoded scalar
luma used by edge detection; it is not inferred to be linear light.
Guide mode and input arity must agree.

Internal Gray luma is its selected channel. RGB luma is

```
RN32(RN32(RN32(r * 0.2126F) + RN32(g * 0.7152F))
     + RN32(b * 0.0722F)).
```

Here and below an F suffix denotes the nearest-even Float32 encoding of the
written constant. Color transfer is not decoded during this operation.

## Pinned algorithm and resources

The source algorithm is [iryoku/smaa](https://github.com/iryoku/smaa), revision
`71c806a838bdd7d517df19192a20f0c61b3ca29d`, configured as SMAA 1x LOW,
luma edges, no diagonal search, no corner detection, no predication, no
reprojection, and zero subsample indices. Search performs at most four
two-texel iterations in each direction.

The original lookup headers and MIT notice are retained byte for byte in
[smaa_resources](../../../../plugins/ops/05-filter/smaa_resources/pin.json).
The pinned shader and notice are also retained in the independent
[oracle source bundle](../../../../oracle/ops/filter/fil19/pinned/pin.json).

| File | SHA-256 |
| --- | --- |
| SMAA.hlsl | 4db53d92ef0b45661f8450303589ffabce93c352cae4282a6f32dd2b80efbeb9 |
| AreaTex.h | 1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2 |
| SearchTex.h | 1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a |
| LICENSE.txt | 2b848b67ba50ac40b53e11cf1ccdaee2a190ca83b4652cba695a5f1ce81f52ae |

AreaTex is 160×560 RG8 and SearchTex is 64×16 R8. Decode each byte as
RN32(byte/255). The 1x profile uses the first area subtexture.
Resources are immutable bundled algorithm constants, not mutable runtime inputs.

## Native sampling and precision

The native profile uses integer texel centers and binary64 image coordinates.
Adding a Float32 displacement to an image coordinate first widens the
displacement to binary64. It does not round the absolute image position to
Float32. Each fetch clamps its integer indices to the complete logical extent
of the texture for that stage: source/guide, edge, weight or lookup.

Bilinear sampling uses floor coordinates and Float32 fractional parts.
Interpolate horizontally first, then vertically. Each interpolation is
`RN32(RN32(a * RN32(1-t)) + RN32(b*t))`. All samples, including zero-weight
operands, follow this definition within the finite selected-color premise.
There is no hardware texture quantization in the native profile.

Color, edge, weight, distance and normalization arithmetic round to Float32 at
each written arithmetic node. Intermediate edge and weight textures retain
Float32 values; they are not quantized to UNORM8. Square roots are RN32;
round-to-integer uses nearest-even. Explicit shader MAD sites use a correctly
rounded Float32 fused multiply-add; other multiply/add nodes are not contracted.
No iteration, task grid or SIMD implementation may change these stages.

## Three observable stages

1. **Luma edges.** Let dx=abs(center-left), dy=abs(center-top), with each
   subtraction rounded to Float32. Initial west/north flags use
   dx>=0.15F and dy>=0.15F. Let contrast be the maximum of dx, dy,
   abs(center-right), abs(center-bottom), abs(left-left2), and abs(top-top2).
   Retain a flag only when its doubled difference is at least contrast.
   Edge equality passes the threshold and local-contrast test.

2. **Blending weights.** Apply the pinned LOW
   SMAABlendingWeightCalculationPS search and area equations in native texel
   coordinates. Horizontal starts are (x-0.25,y-0.125) and
   (x+1.25,y-0.125); vertical starts exchange the spatial roles.
   Continue while the along-edge sample exceeds 0.8281F and the crossing
   component equals zero, for at most four iterations. SearchTex coordinates
   are `fma(32,e.x,positive?33:0)` and `fma(-32,e.y,32)`; the terminal
   correction is `fma(-255F/127F,searchValue,3.25F)`.
   Sample crossing edges at the corrected endpoints, round absolute
   distances nearest-even, and use area coordinates
   `fma(16,roundEven(4*crossing),sqrt(distance))`.
   The two horizontal weights occupy components 0/1 and vertical weights
   components 2/3. No diagonal/corner refinement is performed.

3. **Neighborhood blend.** Assemble
   `a=(weight(x+1,y).w, weight(x,y+1).y, weight(x,y).z, weight(x,y).x)`.
   If the left-associated Float32 sum is below 1e-5F, copy the source bits.
   Otherwise select horizontal iff max(a.x,a.z)>max(a.y,a.w); ties select
   vertical. Normalize the selected positive/negative pair by its Float32
   sum. Bilinearly sample source at the two signed displacements and combine
   them with the normalized weights using separate RN32 products and addition.

The deterministic executable native definitions are
[smaa_math.hpp](../../../../plugins/ops/05-filter/smaa_math.hpp).
The independent scalar [smaa.py](../../../../oracle/ops/filter/fil19/smaa.py)
uses Fraction-based rounding at critical Float32 operations and separate
cached stage functions. It is not called by the production operation.

## Fixed demand, resources and cancellation

The source/guide read superset is a square halo of 14 around each retained tile,
with the internal luma group expanded across all selected color channels.
External-guide mode separately declares the source neighborhood needed by the
final blend. All boundaries refer to the full image; ROI/tile edges are not
new image boundaries.

Per-task scratch contains the guide rectangle at halo 14, Float32 edge pairs
at halo 12, and weight quads for the output rectangle plus its right/bottom
neighbor. It is reused serially across independent planes. Source/output
allocations, preparation metadata and scratch follow the inherited budgets.
Finite search limits bound work; cancellation polls cover guide, edge,
search and output visits within the common 1024-work-unit limit.

Empty output requests introduce no payload task. Failed tasks do not publish
a successful call result. The NEON edge implementation uses the same Float32
stages; scalar paths handle borders and other CPU architectures.

## Independent and pinned-third-party acceptance

Native acceptance compares outputs bit for bit against smaa.py. It includes
32×32, 128×128, 1024² and 4096² workloads, compiled tile/ROI boundaries,
RGB channel permutations, unselected-channel copies, batches and external guides.

Third-party comparison separately runs the unchanged pinned shader on an actual
OpenGL 4.1 context, with SMAA_GLSL_3, SMAA_PRESET_LOW, Float32 source/edge/weight/
output textures, RG8/R8 lookup textures, clamp-to-edge sampling, and blending
and dithering disabled. Source luma reads use nearest filtering; search and
neighborhood sampling use linear filtering. Edge, weight and final color
readbacks are all exposed; no internal comparison stage is unavailable.
The optional macOS [GPU reference helper](../../../../oracle/ops/filter/fil19/gpu_reference.hpp)
is an oracle dependency only.

The fixed comparison set consists of checker, diagonal, deterministic random,
and threshold-adjacent patterns at 32², 128² and 37×53, plus 1024² random and
4096² diagonal images. Pattern definitions are retained in
[fixtures.py](../../../../oracle/ops/filter/fil19/fixtures.py).
For this suite the project comparison criterion is exact edge masks and
weight/final-color RMSE <= 1/255 on unit-range values (PSNR >= 48.13 dB).
This is a suite-level quality criterion, not a per-pixel error bound or an
upstream guarantee.

Actual Apple M5 GPU comparison passed this criterion for all 14 images.
Native binary64 texel positions and deterministic bilinear RN32 differ from
normalized Float32 GPU coordinates and hardware interpolation. Small weight
differences can change a tied orientation decision; isolated final-color
differences can therefore be much larger than the RMSE (the measured random
1024² case includes an absolute difference of about 0.285). No bitwise GPU
compatibility or GPU-relative four-ULP claim is made. Native exact conformance
and the pinned GPU quality comparison remain distinct acceptance results.

## Source

[S08 · Jimenez et al.: SMAA](../research-sources.md#s08) supplies algorithmic
background. The selected arithmetic, coordinate and comparison profile above
is an explicit Photospider specification.
