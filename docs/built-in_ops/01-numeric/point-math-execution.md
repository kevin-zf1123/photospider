# NUM-04/05 fixed mathematical execution

The 66 unary and 27 binary profile definitions use fixed CPU tile invocations through `numeric_math_operation.hpp`. Their constructors in `photospider/ops/numeric/unary.hpp` and `binary.hpp` author workflows. The profile suffix fixes the arithmetic implementation; preparation checks host availability before issue.

## Metadata and fixed inputs

Each input Result contains one tensor member in slot 0, with any valid schema ID and member key. Metadata specialization checks supported dtype, equal complete sample shapes, rank 1..8, positive extents and at most 2^40 samples. Ordinary operations preserve dtype. Rational-pi functions accept two Int64 inputs and an explicit `dtype` parameter selecting Float32 or Float64 output. Constructors may provide their documented default. Output `values` uses schema `photospider.tensor`, member `samples`, and the complete shape as ordinary axes.

Each requested output tile lowers to one task. Lowering declares intersecting tiles independently for each input's batch, spatial and atomic grid. All declared operands remain dependencies even when a numerical identity determines the result. Prepare validates the kernel contract, port/member/tile slots, per-port dtypes, complete regions, output view, empty task parameters and exact scratch capacity. Input width is independent of output width, preserving all Int64 numerator/denominator bits for Float32 rational-pi output.

Empty selection emits no tasks or payload reads. Sparse selection computes only requested complete output tiles. Inputs use the Plan's fixed affine layouts; each output tile has a separate preallocated dense allocation. The runtime performs no sample validation. Integer results must be representable and rational denominators must be positive; these are unchecked caller preconditions. The specified floating NaN, infinity, signed-zero and domain-result rules remain numerical output semantics.

## Scratch and arithmetic

`PointMathScratch<Arithmetic>` contains the exact or certified arithmetic state, a 64-lane `MathBatchWorkspace` and a `FloatMathWorkspace`. The Plan declares the complete aggregate size, including padding, as Host scratch. Block admission reserves it per active worker lane before issue. The prepared owner contains only immutable kind, profile, dtype, width, grids and slot maps. Independent Runs receive separate mutable arithmetic state.

The invocation gathers at most 64 samples from fixed input views and writes directly into the selected output tile. Exact elementary arithmetic, directed transcendental refinement and certified accelerated kernels retain their numerical rules. Float32/Float64 transports preserve raw bits before classification; rational-pi inputs enter exact integer reduction without narrowing. A numerical algorithm's exact refinement does not change Plan placement or task topology.

Each invocation establishes nearest-even rounding with gradual underflow and restores the worker environment. Cancellation is queried before each batch and through exact-arithmetic checkpoints. Those checkpoints do not debit a dynamic sample-work budget. Unresolved bounded refinement may return `ResourceExhausted`; operational failures propagate through fixed kernel status. Accepted invocations retain input/output/scratch owners until return, and successful Results retain output storage beyond context lifetime.

## Validation scope

Temporary public checks should compare integer and elementary results against exact integer/rational references and transcendental results against independent high-precision references. Include every supported dtype, rational inputs beyond 2^53, Float32 and Float64 outputs, special IEEE bits, selected tiles, different input grids, imported Plan preparation, concurrent Runs and SIMD tails. Compare different tile extents to detect partition-dependent results. Validate each accelerated profile only on supported hardware; source availability is not platform execution evidence.
