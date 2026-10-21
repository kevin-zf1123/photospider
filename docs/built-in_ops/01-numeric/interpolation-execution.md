# NUM-08 fixed interpolation execution

The six `numeric.mix_*` and `numeric.smoothstep_*` operations lower to fixed CPU tasks through invocation ABI 8. Each requested output tile has one task, one separate output allocation and declared input tile reads. The strict, Apple Silicon and x86-64 profiles retain their mathematical contracts and require an available implementation on the host.

## Data and preparation

Each of the three inputs contains one tensor member, whose key may vary. Metadata specialization checks equal Float32/Float64 dtype and complete `sample_shape()`, including batch axes, with rank 1 through 8, positive extents and at most 2^40 samples. The output port `values` uses schema `photospider.tensor` and member `samples`; its full shape uses ordinary axes.

Lowering maps each requested output region into each input's own tile grid. For example, a `[2,3,7]` output tile can read ordinary spatial tiles from one input and larger tiles with an atomic final axis from another. All three inputs cover the same logical coordinates. Prepare checks the exact ordered read slots, full tile regions, dtype, output view and scratch reservation. It stores immutable grids and tile-to-slot maps in the prepared kernel owner.

## Invocation and numerical preconditions

Each invocation reads its fixed views and computes only its selected output tile in logical row-major order. The output is dense and preallocated. Input views use the Plan's fixed affine layouts. Empty selection emits no tasks or payload reads.

The caller supplies finite mix factors in `[0,1]` and finite strictly increasing smoothstep edges. These numerical preconditions are unchecked. Mix copies endpoint bits at factors zero and one, including signaling NaNs and signed zero. Interior mix and smoothstep preserve the specified NaN/infinity rules. Strict finite arithmetic uses exact fixed-width rational computation followed by one destination rounding; accelerated profiles use the specified certified bound and exact arithmetic when a candidate cannot be accepted. This numerical algorithm choice does not alter Plan placement or task topology.

## Memory, cancellation and ownership

The Plan declares `sizeof(InterpolationMath)` Host scratch per task. Block admission reserves per-lane scratch before issue; concurrent invocations receive distinct scratch storage. Coordinates and cached read slots are bounded stack data. The prepared context is immutable across Runs. Output tiles have independent allocations; sparse selection allocates only those tiles and their declared input support.

The invocation establishes nearest rounding with gradual underflow and restores the worker environment. Cancellation is queried per sample and at exact-arithmetic checkpoints. These checkpoints do not charge a dynamic sample-work budget. Resource admission and allocation failures prevent issue; kernel failures discard unpublished writes after accepted work drains. Successful Results retain their output owners independently of the execution context.

## Validation scope

Temporary public execution checks should compare against independent arithmetic, including exact endpoint bits and extreme finite edge widths. Cover full, partial and empty selections, nonzero origins, Float32/Float64, distinct input grids and imported Plan preparation. Check malformed invocation signatures before issue. Run accelerated profiles only on supported hardware and state the actual platform separately from registration or source availability.
