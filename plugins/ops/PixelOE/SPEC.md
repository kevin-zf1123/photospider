---
spec_schema_version: 1
id: PixelOE-CPU-Slang
status: Proposed
implementation_status: implemented_cpu_slang
upstream_revision: 0239787b8bb3e0c0dac615a33c896311d40cb46e
---

# PixelOE CPU Slang operation specification

## Scope and source of truth

This package ports the complete public Slang pixelization pipeline at
[KohakuBlueleaf/PixelOE, revision 0239787](https://github.com/KohakuBlueleaf/PixelOE/tree/0239787b8bb3e0c0dac615a33c896311d40cb46e/src/pixeloe/slang).
The pinned Slang pipeline, rather than the legacy OpenCV implementation or README,
is the algorithm reference. Apache-2.0 notices and the license accompany adapted
sources. New upstream commits require explicit numerical and feature review.

The package is independently configured against an installed Photospider SDK.
It is a native shared operation plugin loaded by OperationRegistry::load_plugin.
Its reference arithmetic kernels are compiled from Slang to CPU C++ ahead of time.
Internal NEON/AVX2 specializations may replace selected CPU stages only after
bitwise differential validation against those kernels. The installed plugin requires neither Python, Torch, a GPU nor a Slang JIT. Python
and numerical libraries may be used by build-time table generation and tests.
No PixelOE code is added to the built-in operation registry.

The host gains a separately versioned planar C operation protocol, discovered by
the existing plugin loader. It exposes static descriptor inference, bounded
plane-row access, transactional output, accounted scratch and cancellation.
The existing non-planar operation ABI v9 remains independently versioned. C++
objects, exceptions, allocators, STL containers and native ownership do not cross
the new pure-C boundary. All callback pointers expire at return; a library lease
outlives copied callbacks and every active invocation.

## Public operations and data

`pixeloe.pixelize` produces the final image. Companion operations
`pixeloe.expanded` and `pixeloe.weight` expose the pipeline's intermediates.
An authoring example requests all three to reproduce `return_intermediate`;
no promise of fused evaluation across separate output requests is made.

Input is one structural planar Float32 RGB image with logical shape [H,W,3],
H,W positive. Both continuous and tiled physical storage are accepted. Channel
order is R,G,B, straight, sRGB transfer and sRGB/D65 primaries. A declared
incompatible color interpretation rejects; absent color interpretation means
explicit sRGB input under this operator's contract, not an inferred conversion.
All input samples must be finite and in [0,1]. Alpha, generic interleaved image
Values, batches, other dtypes and implicit transfer/profile conversion reject.
PixelOE's internal Lab uses L* in [0,100]; it is temporary algorithm storage,
not a public Photospider Lab tensor with different coordinate conventions.

Final/expanded output is planar Float32 [H',W',3]. Weight is planar Float32
[Hpad,Wpad,1]. Output semantics establish the actually produced interpretation;
source sample-validity guarantees and geometry-dependent metadata are not
blindly copied. The weight map is a scalar field, not RGB. When thickness=0,
weight output explicitly computes the same normalized expansion weight used by
weighted quantization; this makes the companion output total rather than an
optional/null graph edge.

All arithmetic dimensions, byte counts, scratch, per-stage dispatches and integer
reduction bounds are checked before access. ResourceExhausted reports capacity
failure; invalid parameters/descriptors fail before execution. Numeric input or
computed nonfinite failures abort publication. Cancellation aborts publication
and releases all invocation-owned scratch.

## Parameters

Parameters have the exact names below. Defaults are supplied by the public
authoring helper/example; registry descriptors declare required static values.
All options are validated even when a stage is disabled. Unknown strings reject.

| Parameter | Type | Default | Accepted values |
| --- | --- | --- | --- |
| pixel_size | Int64 | 6 | 2..64 |
| thickness | Int64 | 3 | 0..6, upstream structuring-element table |
| mode | String | contrast | contrast, k_centroid, nearest, nearest-exact, bilinear, bicubic, area, lanczos |
| sharpen_mode | String | none | none, unsharp, laplacian |
| sharpen_factor | Float64 | 0.5 | finite 0..16; converted once to FP32 |
| do_color_match | Bool | true | false, true |
| do_quant | Bool | false | false, true |
| num_colors | Int64 | 32 | 2..256 |
| quant_mode | String | kmeans | kmeans, weighted-kmeans, repeat-kmeans |
| dither_mode | String | ordered | none, ordered, error_diffusion |
| no_post_upscale | Bool | false | false, true |
| weight_mapping | String | current | current, polarity, contrast_ratio, contrast_gated |
| weight_normalize | String | global | none, global, per_image |
| colorfix_blur | String | exact | exact, separable |
| blur_impl | String | lowrank | lowrank, direct, sym, tiled |
| blur_rank | Int64 | 1 | 1..65, capped at each kernel size |
| local_stats | String | lattice | lattice, sliding |
| stat_padding | String | zero | zero, replicate (sliding only) |

`tiled` specifies the exact two-dimensional blur algorithm using its portable
CPU implementation; GPU shared-memory scheduling is not exposed as CPU behavior.
`polarity` is the upstream spelling of `current`. Batch size is one, so global
and per_image normalization agree. CLI pre-resize and image codecs remain
outside the pixelize operation; input dimensions are the actual test dimensions.

## Algorithm and spatial contract

1. Replicate-pad centrally to Hpad=ceil(H/p)*p, Wpad=ceil(W/p)*p;
   top=floor((Hpad-H)/2), left=floor((Wpad-W)/2).
2. If thickness>0, convert RGB to L*/100. Compute lower-median 2p windows and
   min/max p windows on the upstream stride floor(p/2) lattice, zero padding,
   followed by overlap-add averaging; or use the selected sliding statistics.
   Apply the selected sigmoid mapping and normalization, including epsilon 1e-8.
   Blend non-flat erosion and clamped dilation with that weight. Apply erosion,
   dilation, clamped dilation, erosion using the upstream cleanup element.
3. Apply the optional zero-padded unsharp or Laplacian sharpening.
4. If requested, transfer global Lab mean/unbiased standard deviation and apply
   the five-level color correction at radii 2,4,8,16,32. Preserve the chosen
   exact-table, low-rank or separable algorithm; do not silently substitute one.
5. Downsample with the selected upstream method. Contrast uses lower medians,
   row-major center index floor(p*p/2), ordered FP32 mean and exact branch/tie
   predicates in Lab. K-centroid uses two centroids and four maximum iterations.
6. Optionally quantize using deterministic initial centroids, weighted assignment
   or repeat counts, then the selected palette dither. Apply the upstream final
   color match of the quantized image to the downsampled image.
7. Return [Hpad/p,Wpad/p,3], or nearest-exact integer upscale to
   [Hpad,Wpad,3]. Padding is retained, matching upstream.

Weight normalization, moments, palettes and early stopping can depend on the
whole image. All three operations therefore declare Whole input/output behavior.
The current planar Whole executor requires a complete output request; partial
output requests reject with InvalidArgument. A caller needing a crop first
executes the complete output and reads the desired region from the published
image. No fabricated local halo or partial-input optimization is permitted.

## Numerical contract

Computation and stored image intermediates are IEEE binary32; integer keys,
indices and upstream 32.32 fixed-point quantization accumulators retain their
specified integer semantics. Shape-derived quantization gamma follows upstream: binary64 sqrt(Hout*Wout)/512,
then one binary32 conversion. Static table generation may use binary64/SVD and
round coefficients to fixed published FP32 bit patterns. The reference Gaussian
2-D table intentionally includes upstream FP16 coefficient quantization; runtime
images and arithmetic remain FP32. `exact` names that coefficient table, not
correctly rounded real Gaussian convolution. `lowrank` with rank 1 is an explicit
algorithm approximation in upstream; it must never be described as a four-ULP
approximation to full-rank convolution.

Use nearest/ties-to-even, gradual underflow, no fast-math, reassociation or
implicit FMA contraction. Save and restore caller floating environment including
exception flags on the host-assigned callback thread. The plugin executes
serially on that thread and must not create workers or use external thread pools;
thread allocation and scheduling belong to the kernel. SIMD lanes may process
independent pixels, retaining per-pixel operation and reduction order.
Slang compilation uses precise FP;
generated C++ is compiled with strict FP options. Explicit upstream operation
order, compensation, min/max and lower-median tie behavior are retained.
Input signed zero is permitted; selection/copy paths preserve source bits unless
a specified arithmetic step changes them. NaN/Inf inputs reject before kernels.
All published samples must be finite. Any mathematical algorithmic clamp is
explicit; there is no blanket input clipping or quantization to bytes.

This operation defines its own pinned FP32 algorithm. It does not claim the
existing NUM/CRV `strict` correctly-rounded transcendental contract merely by
using a strict compiler flag. The CPU libm implementation and Slang compiler
version are reported with validation; cross-libm bitwise equality is not promised.
There is no `_strict` or accelerated profile key without a separately proven
profile contract. Determinism means identical bits on the same supported build,
independent of SIMD selection, host worker count, cache setting and caller
rounding mode. Branch,
index, palette label and selected endpoint behavior requires exact oracle checks;
continuous comparisons use documented stage-specific tolerances and cannot mask
branch disagreements.

## Validation and performance acceptance

Additional real-image tests use selected `assets/codec` fixtures. Decode,
channel/color preparation, normalization and planar input construction finish
before timing. Any discarded alpha and color conversion must be stated.

Mandatory input sizes are 1920x1080, 4096x4096 and 128x128, all FP32 RGB.
Report actual padded/output dimensions for each parameter set. Exercise the
public installed-package workflow and dynamically loaded plugin, including tiled
inputs, dimensions not divisible by p, whole and ROI requests, invalid input,
all option families, constant/edge/tie cases, cancellation and insufficient budget.
Use an independent Python/Torch or scalar oracle, not a second invocation of the
same compiled shader, for correctness. Test restored rounding/flags, denormals,
and exact repeated/scalar-SIMD equality. Coverage must identify unsupported or
unverified combinations rather than implying an exhaustive Cartesian matrix.

Benchmarks use an explicit default configuration plus feature profiles. Record
CPU/compiler/Slang version, workers, warmups, repetitions, cache policy, workload,
allocation policy, median and tail latency. Report public workflow latency and
CPU kernel dispatch time separately, including layout transfer and allocation costs in
the former. Profile before optimizing; re-run the same correctness oracle after
changes. No CPU latency target is inferred from the GPU data below.

The user supplied a developer screenshot reporting RTX 4090, 1080p default:
CUDA ~4.6 -> ~1.4 ms, Vulkan ~4.9 -> ~1.5 ms; kernel time 4.17 -> 0.86 ms.
The screenshot's torch.compile comparison is explicitly FP16 (39.2 ms). The
Slang dtype, exact commit, timing synchronization, transfers and option set are
not fully established by that image. These are attributed external measurements,
not reproduced acceptance evidence for this CPU FP32 plugin.
