---
spec_schema_version: 1
id: PixelOE-Slang
status: Proposed
implementation_status: implemented_cpu_slang_metal_and_vulkan_intel
upstream_revision: 0239787b8bb3e0c0dac615a33c896311d40cb46e
---

# PixelOE Slang operation specification

## Scope and source of truth

This package ports the complete public Slang pixelization pipeline at [KohakuBlueleaf/PixelOE, revision 0239787](https://github.com/KohakuBlueleaf/PixelOE/tree/0239787b8bb3e0c0dac615a33c896311d40cb46e/src/pixeloe/slang). The pinned Slang pipeline, rather than the legacy OpenCV implementation or README, is the algorithm reference. Apache-2.0 notices and the license accompany adapted sources. New upstream commits require explicit numerical and feature review.

The package minimum is Photospider 0.32.0. Its CMake requirement is `find_package(Photospider 0.32 CONFIG REQUIRED COMPONENTS operation_sdk kernel)`; the installed package version file uses `SameMinorVersion` compatibility, so accepted package releases remain within the 0.32 minor line. It is a native shared operation plugin loaded by `OperationRegistry::load_plugin`. Its reference arithmetic kernels are compiled from Slang to CPU C++ ahead of time. The CPU table contains 77 generated entries. Each GPU target compiles those entries plus the GPU-only `kc_iter_max` and `copy_words` entries, for 79 target-reflected entries per target. Metal produces MSL records; Vulkan produces SPIR-V records and std140 uniform-buffer layouts. The combined GPU table therefore has 158 records across the two backends. Internal NEON/AVX2 specializations may replace selected CPU stages only after bitwise differential validation against those kernels.

The installed plugin requires neither Python, Torch nor a Slang JIT. CPU operations run without a GPU. Metal operations require a native Metal device and compile the packaged MSL through the host service. Python and numerical libraries support build-time table generation and tests. No PixelOE code is added to the built-in operation registry.

The FreeBSD Intel UHD Graphics 770 Vulkan workflow passes the 612-dispatch stage gate and all 40 Torch fixtures, with maximum absolute error 3.4570693969726562e-6 against the 1e-5 threshold. The Vulkan profile’s fixed threshold witnesses also pass; their finite coverage is not a universal bound on device arithmetic. macOS Metal retains its own profile and stage validation. PixelOE Vulkan remains untested on NVIDIA hardware and Linux; see [the native Vulkan evidence and platform limits](../../../out/gpu-whole-tiled/VULKAN_MODEL.md). These records are from the former planar integration path; Vulkan was not exercised on the current Result workflow in this validation.

The plugin exports the Result operation C table ABI 2 and declares twelve keys: three operations (`pixelize`, `expanded`, `weight`) for CPU Whole, CPU tiled, Metal native FP32 and Vulkan native FP32 profiles. Each key has one Result image input and one Result image output on the `values` port. The plugin uses the C Result services for descriptor queries, image windows, relation construction, scratch, cancellation, CPU work and native GPU dispatch. GPU keys require their named backend; the host checks this before invocation work begins. C++ objects, exceptions, allocators, STL containers and native ownership do not cross the pure-C boundary. Callback pointers expire when callbacks return; a library lease outlives copied definitions and every active invocation.

Current Result validation rebuilt the plugin and workflow against the final installed Photospider 0.32 package. CPU Whole and CPU tiled workflow tests passed 2/2 in 1.02 s (0.50 s and 0.51 s); the CPU stage check passed with zero native dispatches. The Metal stage check passed with 598 actual native dispatches, including the profile's underflow-flushed case. The Metal contract checker passed its existing native/no-fallback, worker/SIMD, FP-state, Empty, ROI, semantic-error, work-limit, cancellation-cleanup and copy-bit checks. The final package includes frozen binding-generation isolation, protocol-first error handling, and active cache-candidate survival across LRU eviction; cache clearing and epoch changes still reject pending reuse. Vulkan, a new Torch oracle comparison, and performance measurements were not run for this Result validation. The Vulkan evidence above is from the former planar integration path.

## Public operations and data

`pixeloe.pixelize` produces the final image. Companion operations `pixeloe.expanded` and `pixeloe.weight` expose the pipeline’s intermediates. Metal keys append `_metal_native_fp32`, Vulkan keys append `_vulkan_native_fp32`, and CPU-staged keys append `_cpu_tiled` to each of the three base operation names. All twelve entries publish `values`; GPU entries require their named native backend and `_cpu_tiled` entries use CPU staged execution.

The public workflow declares an input by name and binds an owning `ResultRef` under the same name. For example, a declaration named `image` with `photospider.image` schema is supplied by `ExecutionBinding{"image", image_result}`; the declaration's numeric id is used by the workflow graph. The executable example constructs this declaration and binding directly. An authoring example requests the three outputs to reproduce `return_intermediate`; no promise of fused evaluation across separate output requests is made.

Input is one Result with schema `photospider.image`, one spatial Float32 tensor named `pixels`, logical HWC shape [H,W,3], exactly two batch axes for frames and layers, and no fields. H and W are positive. Channel order is R,G,B, straight, sRGB transfer and sRGB/D65 primaries. A declared incompatible color interpretation rejects; absent color interpretation means explicit sRGB input under this operator's contract, not an inferred conversion. All input samples must be finite and in [0,1]. Alpha, non-RGB layouts, other dtypes and implicit transfer/profile conversion reject. Each frame/layer pair is processed independently. PixelOE's internal Lab uses L* in [0,100]; it is temporary algorithm storage, not a public Photospider Lab tensor with different coordinate conventions.

Final/expanded output is Float32 HWC [H',W',3]; weight is Float32 HWC [Hpad,Wpad,1]. Each output is one Result image in schema `photospider.image`, preserving the input frame/layer batch axes. Output semantics establish the produced interpretation; source sample-validity guarantees and geometry-dependent metadata are not blindly copied. The weight map is a scalar field, not RGB. When thickness=0, weight output explicitly computes the normalized expansion weight used by weighted quantization; this keeps the companion output total.

All arithmetic dimensions, byte counts, scratch, per-stage dispatches and integer reduction bounds are checked before access. ResourceExhausted reports capacity failure; invalid parameters/descriptors fail before execution. Numeric input or computed nonfinite failures abort publication. Cancellation aborts publication and releases all invocation-owned scratch.

## Parameters

Parameters have the exact names below. Defaults are supplied by the public authoring helper/example; registry descriptors declare required static values. All options are validated even when a stage is disabled. Unknown strings reject.

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

`tiled` specifies the exact two-dimensional blur algorithm using its portable stage implementation on CPU or GPU. It selects the full two-dimensional algorithm; its name is independent of the operation's Whole/Region contract. `polarity` is the upstream spelling of `current`. Batch size is one, so global and per_image normalization agree. CLI pre-resize and image codecs remain outside the pixelize operation; input dimensions are the actual test dimensions.

## Algorithm and spatial contract

1. Replicate-pad centrally to Hpad=ceil(H/p)*p, Wpad=ceil(W/p)*p; top=floor((Hpad-H)/2), left=floor((Wpad-W)/2).
2. If thickness>0, convert RGB to L*/100. Compute lower-median 2p windows and min/max p windows on the upstream stride floor(p/2) lattice, zero padding, followed by overlap-add averaging; or use the selected sliding statistics. Apply the selected sigmoid mapping and normalization, including epsilon 1e-8. Blend non-flat erosion and clamped dilation with that weight. Apply erosion, dilation, clamped dilation, erosion using the upstream cleanup element.
3. Apply the optional zero-padded unsharp or Laplacian sharpening.
4. If requested, transfer global Lab mean/unbiased standard deviation and apply the five-level color correction at radii 2,4,8,16,32. Preserve the chosen exact-table, low-rank or separable algorithm; do not silently substitute one.
5. Downsample with the selected upstream method. Contrast uses lower medians, row-major center index floor(p*p/2), ordered FP32 mean and exact branch/tie predicates in Lab. K-centroid uses two centroids and up to four iterations.
6. Optionally quantize using deterministic initial centroids, weighted assignment or repeat counts, then the selected palette dither. Apply the upstream final color match of the quantized image to the downsampled image. Error diffusion reflects a one-column image to its sole column; a downsampled height below three skips the row-pair updates and performs the final palette assignment.
7. Return [Hpad/p,Wpad/p,3], or nearest-exact integer upscale to [Hpad,Wpad,3]. Padding is retained, matching upstream.

Weight normalization, moments, palettes and early stopping can depend on the whole image. All three operations declare Whole input/output behavior. Any nonempty requested output ROI expands to the complete output; the operation requests the complete input using Conservative support. Empty demand requests no samples and creates no payload, CPU stages or native dispatch. The C relation service records a Cartesian relation from every output tensor sample to the same flattened span of the input tensor. This relation describes dependency support; it does not authorize sample reads. PixelOE separately requests and reads the required input tensor window. The relation uses the full flattened input span across frame/layer and HWC axes, with Data and Validation roles and a Conservative guarantee. Before processing, the callback acquires each frame/layer input window and traverses its signed-stride row runs. It batches at most 1024 spans per Host staging copy, validates input samples in grain-32 work, and joins before computation. Output packing uses rectangular blocks of at most 1024 pixels in a Root-accounted temporary buffer; `parallel_for` validates and packs each block with grain 64, then joins before the callback entry thread publishes it. CPU Whole uses the host range service, while CPU tiled routes both input and output transfer stages through the tile service. Native GPU profiles do not use a CPU tile service for host transfers; their host packing executes locally around native device stages. Result payload, relation and association resources are charged to the execution Root.

## Numerical contract

Computation and stored image intermediates are IEEE binary32; integer keys, indices and upstream 32.32 fixed-point quantization accumulators retain their specified integer semantics.

Shape-derived quantization gamma follows upstream: binary64 sqrt(Hout*Wout)/512, then one binary32 conversion.

Static table generation may use binary64/SVD and round coefficients to fixed published FP32 bit patterns. The reference Gaussian 2-D table intentionally includes upstream FP16 coefficient quantization; runtime images and arithmetic remain FP32. `exact` names that coefficient table, not correctly rounded real Gaussian convolution. `lowrank` with rank 1 is an explicit algorithm approximation in upstream; it must never be described as a four-ULP approximation to full-rank convolution.

### CPU profile

CPU operations use nearest/ties-to-even, gradual underflow, and strict operation order. Fast-math, reassociation and implicit FMA contraction are disabled. Save and restore caller floating environment including exception flags on the callback and every range worker. The plugin must not create workers or use external pools.

CPU Whole stages submit fixed workgroup or row ranges to the host; all ranges join before the next stage starts. CPU staged entries run from a coordinator callback on the calling thread. The coordinator allocates and clears buffers, acquires the signed-stride input tensor windows, and allocates a bounded Root-accounted output packing buffer. It submits fixed-geometry input-transfer, compute and output-pack stages through `ps_cpu_tiles_service_v1`; shared workers execute single-threaded tile callbacks, and a stage barrier preserves the pipeline and reduction order. Input-transfer callbacks validate finiteness and range while copying row spans into the working image. Output-pack callbacks validate finite values and pack rectangular blocks into the temporary buffer; after the stage joins, the callback entry thread publishes those blocks to the Result image. CPU Whole uses the host range service for these transfers; GPU host-side packing runs locally without a CPU tile service around native dispatch. The existing SIMD tile kernels are reused inside computation callbacks. Internal work-item tiling remains independent of the Whole `RegionRule`: global weights, moments, palettes and stopping conditions still require the complete image.

Integer/reduction topology remains independent of the grant. Thread allocation and scheduling belong to the kernel. SIMD lanes may process independent pixels, retaining per-pixel operation and reduction order. Whole and CPU staged invocations charge parameter construction, buffer clearing, row I/O and a conservative source-work upper bound before dispatch; the same bound covers scalar and SIMD execution.

Slang compilation uses precise FP; generated C++ is compiled with strict FP options. Explicit upstream operation order, compensation, min/max and lower-median tie behavior are retained.

Input signed zero is permitted; selection/copy paths preserve source bits unless a specified arithmetic step changes them. NaN/Inf inputs reject before kernels. All published samples must be finite. Any mathematical algorithmic clamp is explicit; there is no blanket input clipping or quantization to bytes.

This operation defines its own pinned FP32 algorithm. It does not claim the existing NUM/CRV `strict` correctly-rounded transcendental contract merely by using a strict compiler flag. The CPU libm implementation and Slang compiler version are reported with validation; cross-libm bitwise equality is not promised. There is no `_strict` or accelerated profile key without a separately proven profile contract.

Determinism means identical bits on the same supported build, independent of SIMD selection, host worker count, cache setting and caller rounding mode.

Branch, index, palette label and selected endpoint behavior requires exact oracle checks; continuous comparisons use documented stage-specific tolerances and cannot mask branch disagreements.

### Metal native FP32 profile

The `_metal_native_fp32` suffix selects the device arithmetic contract below. Image-processing compute stages run on the GPU. The host prepares shape-derived tables and input transfers, checks completed global statistics, and copies/validates output data into host-backed Result publication storage; the native path does not return GPU output as a zero-copy Result. Intermediate image buffers retain native owners across stages.

| Property | CPU profile | Metal native FP32 profile | Vulkan native FP32 profile |
| --- | --- | --- | --- |
| Image and intermediate storage | binary32 | binary32 buffers | binary32 buffers |
| Basic arithmetic | nearest/ties-to-even with gradual underflow | device Metal arithmetic, with fast math and FMA contraction disabled | Slang precise FP32; add/subtract/multiply carry SPIR-V `NoContraction`; Vulkan-native rounding and underflow apply |
| Subnormal arithmetic operands/results | preserved according to IEEE rounding | Metal may preserve or flush them; the flushed zero's sign follows the device implementation | device may preserve or flush arithmetic subnormals; exact copy and transfer preserve their source bits |
| Selection/copy | source bits retained | source bits retained; tested separately from arithmetic | source bits retained; tested separately from arithmetic |
| Transcendental functions | target CPU libm | Metal precise math library | Slang precise math lowered to SPIR-V; device-native function error applies |
| Floating environment | caller state saved/restored on callback and range workers | host callback state saved/restored; device arithmetic is independent of host rounding and exception flags | host callback state saved/restored; device arithmetic is independent of host rounding and exception flags |
| Reproducibility identity | plugin/compiler/tables and CPU math implementation | plugin/Slang/tables, generated MSL, GPU, OS and Metal compiler | operation key and frozen plugin implementation within the context, plus native GPU device/build identity |

For non-fast Metal functions, the applicable local error limits include four ULP for `exp`, `log`, `log2` and `sin`, and sixteen ULP for `pow`. Basic arithmetic and `sqrt` use the device's supported rounding mode. Denormal behavior and conversion rules are governed by sections 8.1–8.6 of the [Metal Shading Language Specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf). These are local function guarantees, not a bound on the final pixelization pipeline: global normalization and discrete decisions can amplify local changes.

The same source predicates operate on each profile's own intermediate values. The following discrete rules remain exact:

```text
palette nearest tie: lowest palette index
kmeans continue: previous iteration ran AND maximum change >= 1/256
lower median: sorted[(count - 1) / 2]
repeat remainder tie: lowest original sample index
k-centroid populated-cluster tie: centroid 0
ordered dither: Bayer value > ratio selects nearest; equality selects second
```

A device's continuous arithmetic can place an intermediate on a different side of a predicate from the CPU profile. The GPU profile therefore has its own operation identity and conformance results.

Within one device/build, scheduling, CPU worker count and caller rounding preserve repeatability.

The GPU entry `kc_iter_max` performs one k-centroid iteration and atomically reduces the maximum per-block centroid change. `Context::empty(4)` creates four zeroed control slots before the loop. Each block writes its nonnegative binary32 change to `diff_bits[it]` with `InterlockedMax(diff_bits[it], asuint(change))`; for nonnegative finite binary32 values, the unsigned bit order matches numeric order. Each Metal or Vulkan dispatch completes synchronously before the next iteration reads its control slot, and the next iteration skips its block update when the change is below `1/256`. The four GPU iterations use four `kc_iter_max` dispatches. CPU performs each iteration through `kc_iter`, `diff_partial` and `diff_final`, for twelve iteration/reduction dispatches over four rounds. `kc_init` and `kc_final` remain in both paths. The GPU path allocates no per-block `item_diff` or partial-reduction buffer; the CPU dispatch path and its SIMD selection remain unchanged.

The stage witnesses allow a narrow difference in the global change value: Metal's float reduction may flush a value in `[0, min_normal)` while the unsigned atomic reduction preserves its bits. For the tested fixtures, the witness confirms bitwise-equal centroid state and the same strict comparison with `1/256`. This finite witness set does not establish universal internal bitwise equality or cover every possible image and parameter combination.

Continuous comparisons and exact branch/label witnesses are reported independently. A final RGB tolerance is never evidence for an unobserved discrete decision.

For example, `check_stages` supplies these raw binary32 words to a bilinear 2×2→1×1 stage:

```text
input, each channel:  00000004 00000008
                     0000000c 00000010
CPU output:          0000000a
Apple M5 Metal:      00000000
nearest/copy paths:  preserve the selected source word on both profiles
```

### Vulkan native FP32 profile

The `_vulkan_native_fp32` suffix selects the independent Vulkan arithmetic and cache identity. The plugin contains 79 Vulkan SPIR-V entrypoints and registers `pixeloe.pixelize_vulkan_native_fp32`, `pixeloe.expanded_vulkan_native_fp32` and `pixeloe.weight_vulkan_native_fp32`. The workflow selects them with `backend=vulkan`; execution checks that the active service is Vulkan before allocating invocation resources. This profile has passed the listed FreeBSD Intel checks, not NVIDIA or Linux validation.

Vulkan arithmetic uses Slang precise FP32 compilation, with SPIR-V `NoContraction` on each generated basic add, subtract and multiply. The profile permits the target device’s native handling of underflow and math-function error; tiny arithmetic results may flush to zero. The Vulkan SPIR-V environment specifies that correctly rounded operations without an explicit supported rounding mode use one of the adjacent representable values. For `OpFDiv`, the core 2.5-ULP bound applies when the divisor is zero or has magnitude in `[2^-126, 2^126]`; it is not a universal bound for all device functions or denominator values. See the [Khronos SPIR-V environment specification](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html). The Vulkan profile does not promise CPU bitwise equivalence. The CPU profile remains nearest/ties-to-even with gradual underflow, strict operation order, disabled implicit FMA contraction, and the existing validated NEON/AVX2 paths.

Copy and transfer stages preserve the exact source bits, including signed zero and subnormal words. The pipeline's global phases, reduction and iteration topology, and discrete predicates remain the same as the pinned algorithm. Each profile evaluates those predicates from its own arithmetic results, so branch decisions, palette labels, ties and stopping results require exact checks against that profile's expected behavior.

The ordered-threshold witness brackets the denominator quotient over the permitted adjacent-value interval and checks 17 neighboring threshold values for one monotone transition. Separate zero-ratio witnesses use negative minimum normal, zero and positive minimum normal values to check strict `>` and equality behavior. Intel measured a transition offset of +0.66666 ULP against the CPU-RNE denominator reference in that fixed witness. This is an observed witness result, not a universal device error bound.

The operation key distinguishes `vulkan_native_fp32` from CPU and Metal, and the GPU result-cache key includes the selected backend and native device/build identity. During a context lifetime, the frozen registry and loaded DSO fix the plugin implementation used by that key. External plugin DSOs have no persistent cache identity, so their results do not receive a reusable disk-cache namespace. Slang/SPIR-V compiler identity is not a separately hashed PixelOE cache field. The FreeBSD Intel UHD Graphics 770 run passed all 40 Torch fixtures with maximum absolute error 3.4570693969726562e-6 against the `1e-5` output gate, plus the exact discrete-decision checks. These finite fixtures do not prove all inputs, devices or drivers.

### Native work, capacity and lifetime

The host admits scratch before allocating it and retains each token until its last use. Native capacities include allocation rounding. GPU calls use the callback's serial service scope; each synchronous dispatch completes before a dependent stage or host statistics read. Color correction alternates two destination owners and reuses one row intermediate. Every first component writes the complete destination before later components accumulate.

Before each native dispatch, the host charges 16,384 source-model units for kernel lookup and dispatch metadata. It then calls `gpu_work_bound`, which charges all padded threads. For grid extents \(g_i\) and required workgroup sizes \(b_i\):

\[
T=\prod_{i=0}^{2}\left(\left\lfloor g_i/b_i\right\rfloor+[g_i\bmod b_i\ne0]\right)b_i,
\qquad Q=H+T\,C.
\]

Here \(H\) covers source bytes, bounded registry and reflected-argument lookup, and command construction. The host formula uses \(M=|\texttt{gpu\_kernels()}|\), currently 158 across both backends, and reads this count from the generated GPU table. With source byte count \(S\), argument count \(A\), reflected parameter count \(R\), constant byte count \(D\), and the 64-byte name bound \(L\), it computes:

\[
H=4096+4S+64\left(M(L+1)+AR(L+1)+A^2+31+\left\lceil D/4\right\rceil\right),\qquad L=64.
\]

The implementation builds \(T\) and \(H\) with checked helpers from `gpu_work.cpp`:

```cpp
std::uint64_t threads = 1;
for (unsigned i = 0; i < 3; ++i)
  threads = mul(threads, mul(ceil_div(grid[i], group[i]), group[i]));

const auto names = add(mul(M, L + 1), mul(mul(A, R), L + 1));
const auto fields = add(mul(A, A), add(31, ceil_div(D, 4)));
const auto host = add(4096, add(mul(4, S), mul(64, add(names, fields))));
const auto work = add(host, mul(threads, C));
```

Here `add`, `mul` and `ceil_div` reject overflow or a zero divisor before returning. \(C\) is the checked source-operation bound for the selected stage in `src/gpu_work.cpp`; its loop extents come from validated dimensions and parameters. For both `kc_iter` and `kc_iter_max`, \(C=1024+256p^2\). Transcendental calls cost 64 source-model units. Integer selection accounts for every radix pass, histogram and tie scan. A stopped stage is charged conservatively before it is submitted. \(Q\) is the work returned by `gpu_work_bound`; the fixed lookup charge is admitted separately.

All cumulative additions and products use the following preconditions before evaluating the expression:

\[
 a+b:\quad a\le U-b,\qquad
 a\,b:\quad b=0\ \lor\ a\le\left\lfloor U/b\right\rfloor,
 \qquad U=2^{64}-1.
\]

Parameter-map construction, buffer clearing, host input/output validation and static Lanczos parameter construction are admitted separately before their work. The model counts bounded source work; driver compilation, hardware instruction expansion and wall-clock duration are measured separately.

Cancellation checkpoints precede allocation, argument construction and dispatch, and follow synchronous stage completion. Cancellation during a native submission drains that submission before owners retire. Publication requires final success and currentness.

## Validation and performance acceptance

Additional real-image tests use selected `assets/codec` fixtures. Decode, channel/color preparation, normalization and planar input construction finish before timing. Any discarded alpha and color conversion must be stated.

Mandatory input sizes are 1920x1080, 4096x4096 and 128x128, all FP32 RGB. Report actual padded/output dimensions for each parameter set. Exercise the public installed-package workflow and dynamically loaded plugin, including tiled inputs, dimensions not divisible by p, whole and ROI requests, invalid input, all option families, constant/edge/tie cases, cancellation and insufficient budget. Use an independent Python/Torch or scalar oracle, not a second invocation of the same compiled shader, for correctness. Test restored rounding/flags, denormals, and exact repeated/scalar-SIMD equality. Coverage must identify unsupported or unverified combinations rather than implying an exhaustive Cartesian matrix.

Benchmarks use an explicit default configuration plus feature profiles. Record CPU/compiler/Slang version, workers, warmups, repetitions, cache policy, workload, allocation policy, median and tail latency. Report public workflow latency and CPU kernel dispatch time separately, including layout transfer and allocation costs in the former. Profile before optimizing; re-run the same correctness oracle after changes. CPU and GPU measurements use their explicitly selected profiles.
