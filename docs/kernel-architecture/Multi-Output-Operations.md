# Multi-output operations

Package 0.9 / operation ABI 9 exposes named compile-time results. A workflow edge
selects a name through `WorkflowNodeOutput{node, port}`; a root uses
`WorkflowOutput{name, node, port}`. These are implemented public APIs. Pure
unrequested ports do not execute. `ExecutionOptions::enable_joint` controls the
optional CPU physical optimization; it does not change numeric semantics.

## `color.rgb_to_ycbcr420`

One Float32 HWC Image input, with linear sRGB D65 semantics and no alpha. Channel
roles may be reordered; scene/display reference is preserved. Every actually
read RGB sample must be finite and in [0,1]. There are no parameters or implicit
range/alpha conversions. The named Float32 results are:

| Port | Shape | Role | Nominal source-pixel center (Y,X) | Step (Y,X) |
| --- | --- | --- | --- | --- |
| `y` | `{H,W}` | luma Y′, [0,1] | `{0,0}` | `{1,1}` |
| `cb` | `{ceil(H/2),ceil(W/2)}` | signed blue difference, nominal [-0.5,0.5] | `{0.5,0.5}` | `{2,2}` |
| `cr` | `{ceil(H/2),ceil(W/2)}` | signed red difference, nominal [-0.5,0.5] | `{0.5,0.5}` | `{2,2}` |

For each linear sample L, transfer is `4.5 L` below 0.018 and
`1.099 L^0.45 - 0.099` otherwise. For the transferred RGB values:

```text
Y′ = 0.2126 R′ + 0.7152 G′ + 0.0722 B′
Cb = (B′ - Y′) / 1.8556
Cr = (R′ - Y′) / 1.5748
```

These coefficients and transfer follow [ITU-R BT.709-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/r-rec-bt.709-6-201506-i!!pdf-e.pdf).
The project's 420 contract averages each centered 2×2 chroma block after transfer
and matrix calculation, in row-major binary64 arithmetic. Odd right/bottom edges
average only their valid samples. Final samples are Float32. No studio-range
integer offsets or scaling are applied.

Y reads one complete source pixel; each chroma observation reads its own valid
2×2 source block. Data and Validation associations remain per output. Joint
execution reuses transformed pixels across ready outputs; each member still
performs its authorized reads and range checks. Named outputs can request
different Regions or be consumed independently.

`SemanticKind::ImagePlane` is a Float32 HW color plane. Its canonical
`photospider.semantic` v1 payload includes the plane role, BT.709 transfer, sRGB
primaries, reference white and reference, plus `plane_origin`/`plane_step` in Y,X
order. Sampling positions are nominal metadata, independent from exact source
support in dependency certificates. Existing image-v2 remains complete-pixel
Float32 HWC. Plane metadata does not turn a partial HWC image into a valid image.

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
regional staged requests and also consumes ImagePlane inputs. Output is a generic
HW field. `field.correlate` retains its existing Whole implementation.

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

## Current executable validation

```sh
cmake --build build/issue257-static --target test_multi_output_ops -j 8
ctest --test-dir build/issue257-static -R '^test_multi_output_ops$' --output-on-failure
```

The public-API integration test constructs a 3×5 RGB workflow, checks all three
ports against an independent long-double oracle, compares joint/singleton bits,
checks odd edges and dirty support, requests Y alone, rejects bad sample domains
and alpha, and verifies channel ordering and reference metadata. The installed
four-scenario example is delivered by M10 (#313).
