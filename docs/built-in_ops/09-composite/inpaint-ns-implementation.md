# Local Navier-Stokes inpainting

The registry provides two CPU Result operations:
`image.local_inpaint_navier_stokes_native_apple_silicon` and
`image.local_inpaint_navier_stokes_openCV`. Both fill binary holes with the
same single-channel Float32 Navier-Stokes reference profile applied separately
to R, G, and B. The native implementation does not link OpenCV. The OpenCV
adapter is optional and uses OpenCV 4.12.0. The operations have no GPU backend.

## Result contract

Both inputs use Result schema `photospider.image` version 1, with one tensor
member named `pixels` and no fields. The image tensor is Float32 `[H,W,4]`
with scene- or display-reference linear-sRGB, D65, premultiplied RGBA semantics.
The mask tensor is Float32 `[H,W]` with canonical coverage semantics. Both
tensors carry the same two positive batch axes `[N,L]`, so their complete sample
shapes are `[N,L,H,W,4]` and `[N,L,H,W]`. The output port `image` publishes
the image tensor's complete Result schema, facets, spatial metadata and resources.
An output may feed other typed image operations.

Static specialization checks the schemas and parameters without reading samples.
It requires matching batch axes, matching H/W, `3 <= H,W <= 32768`,
`H*W <= INT32_MAX`, and checked representable sizes for backing, strides and
allocation. The alpha semantic descriptor must be canonical linear-sRGB
premultiplied RGBA; its scene/display reference is preserved. The mask must use
canonical coverage semantics.

The only parameter is required static Int64 `radius` in `[1,32]`; there are no
defaults, aliases or method selection. Unknown keys, missing radius, other
numeric types and values outside that interval fail before computation.
`x_axis`/`y_axis` parameters do not exist: the algorithm always uses H and W
from the tensor descriptor after the batch prefix.

## Pixel and numerical behavior

Execution validates every batch plane over the complete input domain before
processing. Every RGB component must be finite, alpha must equal exactly 1, and
every mask sample must be numeric 0 or 1. The mask's two signed-zero encodings
both mean known. A finite RGB placeholder inside a hole is ignored by the
algorithm; NaN or infinity in any RGB sample is still invalid, including outside
a requested ROI. Static schema validation does not inspect sample values.

For each plane, the implementation copies all original RGBA bits to the output.
It converts mask values to an internal 0/255 UInt8 mask, copies known RGB into a
Float32 work plane, and sets hole RGB to positive zero before each channel solve.
It solves R, then G, then B and copies results back only at holes. Known pixels and
all alpha bits remain unchanged. The result is finite but is not clamped to
`[0,1]`. A validated all-zero mask returns the original image bits after the
complete input scan. A plane whose mask is all ones fails with OperationFailed
because no known pixel exists.

The numerical reference is OpenCV 4.12.0 `cv::inpaint(..., INPAINT_NS)` on
three Float32 planes. The native implementation retains the source's state
transitions and arithmetic order: padded guard state, initial band construction,
arrival time initialization to `1.0e6f`, gradient indexing and edge branches.
Initial frontier insertion is row-major, equal arrival times retain insertion
order, and frontier neighbors are considered up, left, down, right. Initially
queued known neighbors retain the source's KNOWN state. Constants and mixed
Float32/Float64 expressions retain source order, including `1e-20f`, `0.01`,
and `1e-6f`. This is a named discrete profile, not a general PDE solver.

For each hole RGB sample, the pinned reference tolerance is
`abs(actual-reference) <= 1e-6 + 1e-5*abs(reference)`. Unmasked samples and
alpha are bitwise exact. Changing finite RGB placeholders under the same mask
does not change the filled result.

## Demand, publication and lifecycle

Both operations use Whole dependency semantics. The first Result Need requests
the complete image and mask sample domains with Data, Validation and Descriptor
roles (mask 13). The runtime validates both inputs before invoking the numerical
kernel. The output dependency relation conservatively records Data and
Validation support for both complete inputs; each input's descriptor relation is
declared independently. Any input sample or relevant metadata change dirties the
whole output.
One changed input sample therefore invalidates the complete output.

Successful publication is one CompleteBundle Result containing the cloned image
schema and a dense, root-owned output backing. The output keeps the input's batch
axes and all image metadata, including the scene/display reference. No partial
result is published. An Empty Result request uses a stateless continuation that
seals an empty result without requesting image/mask payload or entering
validation, coefficient, or numerical work.

All batch planes are validated first and then processed sequentially. Each plane
uses fresh algorithm scratch; no state is shared between executions. Cancellation
is checked throughout validation, packing, frontier processing and copy-back.
OpenCV is non-interruptible inside a channel call; its adapter checks cancellation
before and after each channel. Concurrent invocations use independent state.

## Managed resources and work

Let `N=H*W` per batch plane and `P=(H+2)*(W+2)`. The native phase allocator
owns per-plane scratch of `21N+5P` bytes:

| Native scratch | Bytes |
| --- | ---: |
| UInt8 mask | `N` |
| Float32 work plane | `4N` |
| Padded state | `P` |
| Padded arrival times | `4P` |
| Fixed 16-byte heap entry storage | `16N` |

For the minimum 3x3 plane, `21N+5P < 35N`. Output storage is separately
allocated through the Result resource root at 16 bytes per pixel and batch plane.
The operation reserves workspace for twice the authorized image-plus-mask input
bytes, plus 64 KiB. This covers host-owned materialization/scratch bounds; actual
capacity admission remains authoritative and these values are not an RSS limit.
Only one plane's scratch is live at a time.

Before validation reads, the operation prepays a conservative bound based on
Result window read work and per-pixel address/classification costs. For each
plane's three channel solves it prepays
`3*(8P + N*(256 + 128*log2_bound(N)) + K*(2r+1)^2*512)`, where K is the hole
count and r is radius. The `log2_bound` term is the implementation's integer
bit length of N and bounds heap comparisons/moves. Arithmetic never runs beyond
the admitted credit; a finite work limit may reject before actual work would have
exhausted it.

The OpenCV adapter owns three host-managed arrays totaling `9N` bytes: the `N`
byte mask and separate `4N` source and target Float32 planes. OpenCV's internal
matrices and queue are external allocations and cannot be bounded by the kernel
root. The estimate `7P+32N+64 KiB` for the pinned adapter and libc++ vector
growth is not a hard allocation guarantee or measured peak.
OpenCV `StsNoMem` maps to ResourceExhausted. No global OpenCV allocator or thread
configuration is changed.

The Result output backing and per-plane hole-count vector are allocated through
the execution Root; the vector stores one count per batch plane. The phase
allocator owns the per-plane mask, work plane,
padded state and times, and native heap; it releases that scratch when the
plane finishes. Neither implementation has a private thread pool. The native
source retains the required license at
`share/licenses/Photospider/inpaint_ns_license.txt`. The OpenCV operation is
uncacheable because replacing a same-version OpenCV library can change numerical
behavior outside the kernel build identity. This does not make the native
operation's output cacheable across changed inputs or profiles.

## Errors and finite validation evidence

| Condition | Result |
| --- | --- |
| Wrong schema, tensor count, dtype, dimensions, batches, or color/coverage profile | TypeMismatch during static preparation |
| Missing, unknown, non-Int64, or out-of-range radius | InvalidArgument before callback entry |
| Invalid typed image or mask value detected by the host's Need Validation | InvalidArgument with host validation priority |
| Nonbinary coverage, nonopaque alpha, nonfinite RGB, or nonfinite arithmetic rejected by operator-specific checks | OperationFailed for direct and generated bindings |
| Full mask in any batch plane | OperationFailed |
| Unrepresentable size, work exhaustion, or allocation failure | ResourceExhausted |
| Cancellation | Cancelled; admitted buffers are released |
| Unsupported backend | BackendUnavailable; no GPU or alternative algorithm fallback |

The native Result suite passes 16 numerical cases, validation and rounding
checks, concurrency, ROI behavior, cache policy, strided layouts, batch planes,
and result ownership. It also passes six Root payload capacity boundaries,
work exhaustion, active cancellation, invalid dimensions through UINT64_MAX
metadata, and Empty-demand checks. A full-mask plane fails as specified.

The OpenCV adapter test passes a 288-case reference matrix: 144 cases per
backend across shapes, patterns, radii 1/3/8/32 and all mask families. Its
validation, rounding, concurrency, ROI, cache, layout and batch checks pass.
Both adapters passed the 5x5 constant-color public example. The strict OpenCV
4.12.0 library was built from the 4.12 tag with
`-fno-fast-math -frounding-math -ffp-contract=off`.

The standalone native consumer configures, builds, and runs against package
0.30.0. The installed adapter consumer passes three tests: the 288-case
consumer, native example, and OpenCV example. A native-only package also passes
its two installed checks, and its executable has no OpenCV undefined symbols.
The native suite also verifies complete-output invalidation after a one-sample
input change. Same-context results after changing image, mask, radius, or image
reference match fresh executions bitwise, with zero cache hits.
These are finite checks, not a proof over all inputs. Deterministic frontier
cancellation injection and host Stale-priority injection are not covered. OpenCV
allocations inside its library remain outside hard Root capacity.

## Public workflow

The [standalone workflow example](../../../examples/inpaint_ns_workflow/README.md)
constructs typed Result inputs, binds them to a public WorkflowDocument, runs the
native operation, and checks the named `image` output. Its 5x5 constant-color
fixture verifies the output and retained image semantic facet.

    cmake --build build/kernel-dev --target photospider_inpaint_ns_workflow -j 8
    build/kernel-dev/examples/inpaint_ns_workflow/photospider_inpaint_ns_workflow

For the optional adapter, use the strictly built OpenCV 4.12.0 package and enable
`PHOTOSPIDER_ENABLE_INPAINT_OPENCV`. Its standalone example selects the adapter
with the operation key `image.local_inpaint_navier_stokes_openCV`.

The package version is 0.30.0. The OpenCV operation is available only when the
kernel is built with the optional OpenCV 4.12.0 adapter.
