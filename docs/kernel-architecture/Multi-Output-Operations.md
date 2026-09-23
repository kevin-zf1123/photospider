# Multi-output operations

The multi-output API exposes named compile-time results. A workflow edge
selects a name through `WorkflowNodeOutput{node, port}`; a root uses
`WorkflowOutput{name, node, port}`. These are implemented public APIs. Pure
unrequested ports do not execute. `ExecutionOptions::enable_joint` controls the
optional CPU physical optimization; it does not change numeric semantics.

## Current support boundary

Package 0.20.0 removes `color.rgb_to_ycbcr420`. FMT-16 is retired; external
chroma subsampling/reconstruction belongs to the separate input/output codec boundary.
The image operations and `test_multi_output_ops.cpp` described below remain
migration source references. Their legacy typed image paths are subject to the
planar gate, and that test is not currently registered in CTest. These sections
do not claim new planar image execution support. Generic named-output APIs remain.

## `image.split_horizontal`

One Float32 HWC Image input and required Int64 `split_x`, with
`0 < split_x < W`. There is no default split. Outputs preserve the source color,
alpha and channel interpretation:

| Port | Shape | Source mapping |
| --- | --- | --- |
| `full` | `{H,W,C}` | `(y,x,c)` |
| `left` | `{H,split_x,C}` | `(y,x,c)` |
| `right` | `{H,W-split_x,C}` | `(y,x+split_x,c)` |

Each output accepts independent complete-pixel Regions in its own coordinates.
The staged reader declares exactly the mapped source pixels. Published Values
are immutable views with checked origin-relative layouts and the source storage
owner. Collecting a dense result may copy those views. Joint members share source
transport where equal and preserve their independent offset evidence. Parameters
and shape subtraction are validated before reading samples.

The integration test requests three different offset ROIs, checks exact source
values, facets and owner identity with joint enabled/disabled, verifies dirty
mapping and rejects negative, zero and out-of-width splits.

## `image.convolve_channels` and regional `field.convolve`

`image.convolve_channels` consumes a Float32 RGB Image followed by three generic
Float32 HW kernels in R,G,B order. RGB roles may be reordered in storage. The
stored RGB channels are filtered independently; alpha is not emitted or implicitly
unassociated. Outputs `r/g/b` are Float32 HW ScalarFields with the corresponding
relative channel role. Each channel has required Int64 `{r,g,b}_anchor_y` and
`{r,g,b}_anchor_x`, and required String `{r,g,b}_boundary` (`zero` or `clamp`).
There are no defaults. Anchors must lie inside their own kernel. Odd, even and
non-symmetric kernels are supported without normalization or bias.

`field.convolve` retains the public input order, same-dtype Float32/Float64 HW
kernel, and required `anchor_y`, `anchor_x`, `boundary` parameters. It now accepts
regional staged requests for generic fields. The old ImagePlane path needs
planar migration. Output is a generic HW field. `field.correlate` retains its existing Whole implementation.

Both convolution paths compute, in fixed kernel row-major order and binary64
accumulation, `sum K[ky,kx] * I[y+anchor_y-ky,x+anchor_x-kx]`. Zero extends with
zero; clamp repeats the nearest valid edge. Input samples, coefficients,
intermediates and final dtype conversion must be finite/representable. Each
output declares its full selected kernel and exact clipped source neighborhood;
image-v2 still requires complete source pixels. Data and Validation roles are
explicit. No sibling kernel samples are read, so changing G's kernel invalidates
G while preserving R/B cache entries. Unrelated invalid kernel samples do not
fail an independently requested channel.

The integration test uses independent odd/even asymmetric kernels and anchors,
compares both execution modes to a scalar oracle, changes only G's kernel and
checks 24 R/B cache hits for a 3×4 image, and verifies a finite field ROI succeeds
without reading a remote NaN sample. Existing basic convolution results are also
covered by `test_basic_operations`.

## `image.gaussian_blur_with_kernel`

One Float32 HWC Image input produces `image` (same descriptor/facets) and
`kernel` (generic Float32 HW). Required Float64 `radius` and `sigma` are finite
and in `[0,64]`. Optional String `boundary` is `clamp` by default, or `zero`.
Let `R = ceil(radius)`; the kernel shape is `{2R+1,2R+1}`, including when
`sigma=0`. At integer offsets `x,y` in `[-R,R]`, normalize
`exp(-(x*x+y*y)/(2*sigma*sigma)) * a(x) * a(y)`, where
`a(d)=clamp(radius+1-abs(d),0,1)`. Sigma zero produces a center impulse.
For example radius 1.25 produces 5×5, with outer per-axis coverage 0.25.
The implementation preserves positive edge weights immediately above integer
radii and handles the smallest positive sigma without a center division error.

Kernel coefficients are rounded to Float32 once. Image convolution uses these
same coefficients, row-major kernel order and binary64 accumulation, so a
separately bound reference plane can be passed to `field.convolve` for
independent recomputation. The example supplies R explicitly. Image samples and results must be finite/representable.
Each image observation reads its clipped radius neighborhood with Data and
Validation roles. Kernel observations use static parameters and descriptor
metadata only; requesting the kernel never reads image samples. Parameters are
currently included as a complete set in cache identity.

The singleton staged implementation checkpoints its host-owned coefficient
table before reading image data. Joint execution shares one coefficient table
and charges its generation once through the shared work service. Published
kernel views retain the backing owner independently. Shared work failures are
sticky, including through the C ABI; a failed upstream member is retired before
continuing the remaining members. Existing Gaussian operators retain their
original parameter semantics.

`test_multi_output_ops` checks complete matrices for radii 0, 0.25, 1, 1.25,
2 and 64, adjacent floating-point values around integers, zero/tiny sigma,
invalid parameters, kernel-only zero source reads, and exact public
`field.convolve` recomputation with both boundaries and joint modes.

## Migration examples

The [three remaining image scenarios](../../examples/multi_output_workflow/README.md)
are buildable migration sources, not active planar runtime acceptance. Removed
420 behavior is no longer exposed. Their future migration must add executable
planar coverage before restoring runtime acceptance claims.

Convolution and Gaussian coefficient generation establish the default
round-to-nearest/gradual-underflow environment and restore the caller's prior
floating environment. Direct generic field/kernel outputs therefore agree with
typed image and joint paths even when the caller selected upward/downward
rounding. Both public direct invocation paths have environment regressions.
