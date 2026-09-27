# Multi-output operations

The multi-output API exposes named compile-time results. A workflow edge
selects a name through `WorkflowNodeOutput{node, port}`; a root uses
`WorkflowOutput{name, node, port}`. These are implemented public APIs. Pure
unrequested ports do not execute. `ExecutionOptions::enable_joint` controls the
optional CPU physical optimization; it does not change numeric semantics.

## Current support boundary

The default registry currently provides `image.split_horizontal` among these
image multi-output operations. Its typed image path is subject to the planar
storage gate. The named-output host API is available independently of this
operation's image storage support.

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

The maintained host contract/execution tests validate named-output infrastructure.
The split example is not current planar runtime acceptance.

## Filter status

The former `image.convolve_channels` and `image.gaussian_blur_with_kernel`
registrations and their `field.convolve` dependency have been removed from the
default registry. Proposed replacement behavior is documented in
[05-filter](../built-in_ops/05-filter/spatial.md). The named-output host API and
`image.split_horizontal` remain available. The
[multi-output example](../../examples/multi_output_workflow/README.md) retains
only the split scenario and is not current planar runtime acceptance.
