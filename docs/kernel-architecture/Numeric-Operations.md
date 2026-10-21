# Numeric operations

All registered core scalar/COPY operations, unsuffixed arithmetic, reductions, scan, expression and LUT keys, and the numeric profile families documented below use physical tasks. See the [execution contract](Compiler-and-Execution.md).

The bundled sources define unsuffixed finite-domain arithmetic and clamp keys, fixed scalar reductions, an ordered scan, and profile-specific numeric families. Their parameter, execution and output contracts differ, so choose a key by its exact name. These are bundled operations in `plugins/ops/01-numeric/`, not kernel code; the kernel supplies only the generic Result execution, resource, cancellation, and failure contracts they use. The operations use module ABI 8 and fixed invocation ABI 8. Numeric work reports, LUT bake reports, and quality reports are operations-SDK types (`photospider/ops/numeric/`); see [Atom errors and quality](Atom-Errors-and-Quality.md).

## Unsuffixed Result numeric operations

| Key | Inputs and output | Parameters and region |
| --- | --- | --- |
| `math.add` | Two Float64 tensor Results with one sample each (`sample_shape() == [1]`); output port `value` is a `photospider.tensor` v1 Result with member `samples`, shape `[1]` | No parameters; fixed scalar task; IEEE addition |
| `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` | Two Float32/Float64 Result inputs with identical full `sample_shape()` and dtype; output port `value` is a `photospider.tensor` v1 Result with tensor member `samples`, preserving shape and dtype | No parameters; fixed output-tile tasks |
| `numeric.clamp` | One Float32/Float64 Result input; output port `value` uses the same Result schema and preserves shape and dtype | Required finite Float64 `min`, `max`, with `min <= max`; fixed output-tile tasks |
| `numeric.abs` | One Float32/Float64 tensor Result; output port `value` is `photospider.tensor` v1/member `samples`, same sample shape and dtype | No parameters; fixed output-tile tasks |
| `numeric.minimum`, `numeric.maximum` | Two Float32/Float64 tensor Results with identical sample shape and dtype; output port `value` is `photospider.tensor` v1/member `samples` | No parameters; fixed output-tile tasks |

Each pointwise input contains one tensor with a structurally valid schema and member key. Inputs have equal Float32/Float64 dtype and complete `sample_shape()`, of rank 1..8 including batch axes. The output is a `photospider.tensor` v1 Result with member `samples`, preserving dtype and the complete shape as ordinary axes.

The eight unsuffixed numeric keys have unchecked finite-domain semantics. Callers supply finite operands and representable results; division requires a nonzero divisor. Clamp requires finite static Float64 `min` and `max`, with `min <= max`, checked during metadata specialization. Its selected result must fit the destination dtype; an unused endpoint may exceed Float32 range. Runtime kernels do not scan samples or promise errors for violated numerical preconditions. Planning checks the byte capacity of each selected tile. A large logical domain can be selected sparsely without materializing or counting the full payload.

`math.add` requires two Float64 tensor inputs with complete `sample_shape()` `[1]` and produces a `photospider.tensor` v1 Result with member `samples`, shape `[1]`. A nonempty selection lowers to one ABI 8 CPU task with two fixed reads and one preallocated write. The kernel performs IEEE Float64 addition, including NaN and infinity propagation. Preparation validates the complete scalar invocation signature. Empty selection emits no task.

`core.constant` has no inputs and requires a Float64 `value` parameter, with no default or finite-domain restriction. It produces the same scalar schema through one fixed CPU task. Preparation owns the immutable parameter value through every accepted invocation.

`core.identity` accepts one Result containing one tensor and preserves its schema, dtype, shape and selected coverage. It lowers each requested tile to an explicit Host COPY with a separate output allocation. The copy preserves sample bytes, including signed zero and NaN payloads. Inputs use the Plan's fixed affine binding contract. Zero selected tiles emit no COPY task. These three operations need no runtime scratch; output storage and metadata enter block admission.

Each selected output tile becomes one CPU task. Lowering declares every input tile intersecting that output region, independently for each input grid. Input batch, spatial and atomic axes can therefore differ from the output grid without changing logical coordinates. Preparation checks complete read coverage and binds fixed invocation slots. Execution performs the selected arithmetic directly into separately allocated output tiles, with no scratch payload. Empty selection emits no task.

`abs(-0)` produces `+0`; minimum returns `-0` for every pair of zero operands, and maximum returns `+0`. Arithmetic uses nearest rounding with gradual underflow and restores the worker's floating environment. Cancellation is sampled at most every 1024 output samples. These unsuffixed finite-domain keys have their own semantics; suffixed profiles retain their separately specified dtype and IEEE rules.

## Unsuffixed expression and LUT

`numeric.sample_expression` accepts an unbatched Float64 coefficient vector and static start/positive-step/count/expression. Each selected Float32 output tile reads the complete coefficient vector and uses 4096 bytes of fixed scratch. Global indices drive FMA coordinates. The bounded AST is prepared once; numerical domains and finite intermediates/results are caller preconditions.

`lut.apply_1d` maps Float32 queries through an unbatched Float32 table with static origin/positive step and reject/clip policy. Each task reads the selected query region and the complete table as a conservative data-dependent address superset. Output preserves query cell shape and batch axes; scratch is zero. Index-conversion and compensated-interpolation precision guards remain checked. See [expression and LUT operations](Expression-and-LUT-Operations.md).

## Fixed ordered reductions and scan

| Key | Inputs and output | Parameters and execution |
| --- | --- | --- |
| `numeric.mean`, `numeric.variance` | One Float32/Float64 tensor Result, complete sample rank 1..8; output `value` is a Float64 `photospider.tensor` v1 Result/member `samples`, shape `[1]` | Optional Int64 `block_size` in `[1,65536]`, default 64; one complete-input CPU task |
| `numeric.ordered_scan` | One Float64 tensor Result with complete sample rank 1; output `value` has the same shape and schema `photospider.tensor` v1/member `samples` | Same `block_size`; one complete-input CPU task with the selected output tiles |

Mean converts Float32 samples exactly to Float64 and accumulates in logical row-major order, starting from positive zero. It divides the final sum by the sample count converted to Float64. Variance computes that mean in the first pass, then traverses the same input order to accumulate `(sample - mean) * (sample - mean)` in Float64 and divides by the count (`ddof=0`). Scan performs strict left-to-right inclusive Float64 addition from positive zero. Input tile boundaries and `block_size` do not regroup arithmetic.

A nonempty request lowers to one fixed task reading every input tile. Mean and variance write one preallocated scalar; scan writes only the selected preallocated output tiles while visiting the complete input. Preparation checks the complete ordered read set and every output view. The kernel keeps coordinates, sum and mean on the stack and allocates no payload or intermediate Result. Empty selection emits no task and returns the declared schema with empty coverage.

These kernels use the unchecked finite-domain contract: samples and all intermediate arithmetic must remain finite and representable. Static validation checks dtype, shape, representable sample count and the optional block size. `block_size` sets the cancellation query interval, at most that many samples; variance restarts the interval in its second pass. The CPU invocation uses nearest rounding with gradual underflow and restores the worker environment. Cancellation discards unpublished output and retains accepted input/output owners until the kernel returns.

## Fixed comparison and selection profiles

The six comparison predicates, `is_close` and `select` each have strict, Apple Silicon and x86-64 profile keys. They lower one CPU task per selected output tile. Prepare checks fixed reads using each port's own dtype and grid. Comparisons return UInt8; select preserves its branch dtype and raw bits. Empty selection emits no task.

Ordinary comparisons support UInt8, Int64, Float32 and Float64 without integer-to-float conversion. IEEE classification handles signed zero, infinities and NaNs directly. `is_close` accepts Float32/Float64, checks required finite nonnegative static Float64 `atol`/`rtol`, and compares its threshold with exact integer arithmetic in preallocated scratch. Select uses UInt8 conditions with a caller-supplied unchecked 0/1 precondition; both branches remain declared dependencies. Its loop uses bounded stack state and zero task scratch. See [NUM-07 fixed execution](../built-in_ops/01-numeric/comparison-execution.md).

## Fixed interpolation profiles

`numeric.mix_<profile>` and `numeric.smoothstep_<profile>` accept three same-shape Float32/Float64 tensors and produce `values`, a `photospider.tensor` Result with member `samples`. Each selected tile lowers to one CPU invocation with intersecting input tiles and preallocated exact-arithmetic scratch. Input batch and atomic axes may differ. Prepare validates fixed slot coverage; independent Runs share immutable prepared state and use separate invocation scratch.

Mix factors must be finite in `[0,1]`; smoothstep edges must be finite and strictly increasing. These are unchecked caller preconditions. Endpoint raw bits, NaN/infinity handling and strict exact rational rounding follow the [member specifications](../built-in_ops/01-numeric/interpolation-execution.md). Empty selection issues no task. CPU profile availability is checked before execution.

## Profile-specific pointwise operations

The numeric math families use three explicit suffixes:

| Profile suffix | Admission |
| --- | --- |
| `_strict` | Portable strict implementation |
| `_accelerated_apple_silicon` | Apple Silicon implementation when its runtime profile is available |
| `_accelerated_x86_64` | x86-64 accelerated implementation when its runtime profile is available |

`numeric_unary.cpp` and `numeric_binary.cpp` define 93 fixed CPU profile keys. Each requested output tile becomes one ABI 8 invocation; prepare checks per-port dtypes, input views, output and exact scratch capacity. Inputs may have different batch/atomic grids. Rational-pi preserves both Int64 input widths and produces the explicitly selected Float32/Float64 dtype. Fixed scratch aggregates arithmetic state, a 64-lane batch and Float32 SIMD buffers; independent Runs use separate mutable state. Empty selection emits no tasks.

Representable integer results and positive rational denominators are unchecked caller preconditions. Floating NaN/infinity, signed-zero, exact rounding and accelerated error bounds follow the member specifications. Arithmetic refinement does not change Plan placement; cancellation is queried per batch and at exact-arithmetic checkpoints. See [fixed mathematical execution](../built-in_ops/01-numeric/point-math-execution.md).

## Fixed dynamic range profiles

`numeric.clamp_<profile>` takes three same-shape/dtype inputs in value/lower/upper order; `numeric.remap_range_<profile>` takes five in value/source-lower/source-upper/target-lower/target-upper order. Each requested output tile lowers to a fixed CPU task with every intersecting input tile and preallocated `RangeMath` scratch. The kernel writes a separate dense output allocation. Empty selection emits no task.

Clamp supports UInt8, Int64 and Float32/64, preserving selected raw bits and quieting input NaNs. Its caller supplies non-NaN ordered bounds. Remap supports Float32/64, preserving endpoint bits and evaluating the full rational formula before final rounding; callers supply finite bounds with an increasing source interval. These bound rules are unchecked preconditions. See [fixed range execution](../built-in_ops/01-numeric/range-execution.md). The unsuffixed `numeric.clamp` has separate static-parameter and finite-domain semantics.

## Fixed matrix transform profiles

The three `numeric.matrix_transform_<profile>` keys accept same-dtype Float32/64 vectors `[...,Cin]`, matrix `[Cout,Cin]` and bias `[Cout]`, with Cin/Cout in 2..4. Each selected output tile declares matching vector prefixes with all Cin components, selected matrix rows and bias channels. Prepare validates the complete signature; tasks use preallocated output and `MatrixWorkspace` scratch. A task's local Cout can be 1..4, supporting partial channels and nonzero origins. Empty selection emits no tasks.

Finite arithmetic rounds the complete dot plus bias once; NaN priority, infinity and zero signs follow [NUM-14](../built-in_ops/01-numeric/op_specs/NUM-14_matrix_transform.md). Apple Float32 candidates pass an independent enclosure check; other cases use raw-word ExactDot. Fixed kernels process at most 64 vectors per batch. Prepared state is immutable and each invocation has private arithmetic state.

## Fixed reduction profiles

The suffixed reduction family registers `numeric.reduce_sum`, `numeric.reduce_minimum`, `numeric.reduce_maximum`, `numeric.reduce_mean`, `numeric.reduce_count`, `numeric.reduce_variance` and `numeric.reduce_std`, each with the same three suffixes. These use fixed CPU tasks. They require a comma-separated String `axes` list of unique canonical nonnegative axis indices; sum, mean, variance and standard deviation also require String `dtype` (`uint8`, `int64`, `float32` or `float64`), subject to source and operation-domain compatibility. Variance and standard deviation require Int64 `ddof` in `0 <= ddof < N`, where `N` is the product of the reduced extents. Reduced axes become length one; unreduced extents remain unchanged. The implementation rejects invalid axes, unsupported dtype combinations, oversized inputs and invalid degrees of freedom. These reductions have profile-specific exact arithmetic and output conversion. They are separate registered keys from the unsuffixed fixed ordered tasks `numeric.mean` and `numeric.variance` above.

Each selected output tile declares source tiles covering its complete reduction groups and uses private preallocated exact aggregate/moments scratch. Logical row-major order determines NaN priority; physical tile layout and completion order do not change it. Integer final representability is an unchecked caller precondition. Count declares only a schema reference and fills selected dense Int64 output tiles without source payload or arithmetic scratch. Empty selection emits no tasks. See [fixed reduction execution](../built-in_ops/01-numeric/reductions-execution.md).

## Fixed prefix and integral scan profiles

The six `numeric.prefix_sum_<profile>` and `numeric.integral_image_<profile>` keys use fixed CPU tasks. Prefix requires Int64 `axis`; integral requires a comma-separated String `axes` naming two distinct axes. Both require a same-domain String `dtype`. Each selected axis grows by one, with leading integer zero or floating +0. Output `values` uses `photospider.tensor`/`samples` and ordinary axes from the complete input sample shape.

Tiles with equal non-scan regions share one task. The task reads all input tiles intersecting its bounding source prefix and writes separate dense output tiles. Zero-only tiles do not enlarge the prefix; zero-only groups use no input payload or arithmetic scratch. Empty selection emits no task. Exact source carry preserves NaN priority and cancellation after floating overflow. Private scratch contains fixed exact state and, for integral, one carry per required source column. Integer representability is an unchecked precondition for every declared write. `select_outputs` retains complete tasks, including their reads/writes/scratch; fresh compilation can narrow the prefix. See [fixed scan execution](../built-in_ops/01-numeric/scans-execution.md).

## Fixed layout transform profiles

The nine `array.{reshape,transpose,slice}_<profile>` keys copy all seven numeric dtypes bit-for-bit through fixed CPU tasks, one independent dense allocation per selected output tile. Reshape maps global row-major ordinals, transpose permutes logical axes, and slice uses dynamic Int64 starts/steps with positive static counts. The old `layout` parameter and `TransformLayout` enum are removed. Output `values` uses ordinary target axes in `photospider.tensor`/`samples`.

Reshape and transpose declare exactly the input tiles touched by their static mappings. Slice declares the union of tiles reachable by any legal controls; each axis contributes at most two intervals. Runtime checks the full slice endpoints before copying. All-singleton slices omit steps payload; empty selection emits no task. Preparation verifies complete read/write slots. Source affine strides may be negative or zero; runtime uses bounded stack state and no scratch payload. See [fixed layout transforms](../built-in_ops/01-numeric/layouts-execution.md).

## Fixed indexing profiles

The eighteen `array.{concatenate,gather,scatter_replace,scatter_sum,scatter_minimum,scatter_maximum}_<profile>` keys use fixed CPU tasks with UInt8/Int64/Float32/Float64 values. Concatenate accepts 2..256 inputs, removes its layout policy and declares only source slabs intersecting each selected output tile. Gather/scatter validate the complete dynamic Int64 indices vector once per call in a shared task; scatter stably sorts target/position pairs. The result is a private dense Int64 `[M,2]` tile consumed by output tasks.

Each consumer declares its non-axis source/update region and all possible axis positions; scatter also reads its base output region. No-hit and Replace preserve raw bits. Aggregate variants process base then updates in original position order, with exact arithmetic and final conversion. Integer sum representability is an unchecked caller precondition. Private normalized positions are checked before source addressing. Every selected output tile owns an independent allocation; empty selection emits no task. Sorting uses `16*M` private payload plus `16*M` task scratch, subject to the executor's per-lane scratch reservation. See [fixed indexing execution](../built-in_ops/01-numeric/indexing-execution.md).

## Fixed constant and broadcast profiles

The six `numeric.{constant,broadcast}_<profile>` keys preserve all seven numeric dtypes through fixed CPU tasks. Constant requires source sample shape `[1]`; broadcast maps every complete source axis to a distinct target axis, with extent one or the matching target extent. Both have static target shape, ordinary output axes and independent dense allocations for selected tiles. The layout parameter and `ArrayLayout` enum are removed.

Each constant task reads its scalar once and fills a tile with disjoint copies of at most 64 KiB. Broadcast declares the exact mapped source rectangle, then copies raw samples with bounded stack state. Both use zero scratch; empty selection emits no tasks. Sparse huge targets reserve only selected output tiles. Prepare validates canonical output and complete source views before entry. See [fixed array execution](../built-in_ops/01-numeric/arrays-execution.md).

## Fixed sequence profiles

The six `numeric.{linspace,arange}_<profile>` keys accept two scalar Results, static count in `[1,1048576]` and dtype. Floating modes permit mixed Float32/Float64 inputs and produce the requested Float32/Float64 values; arange also supports two Int64 inputs with Int64 output. Each selected values tile has one fixed CPU task. Axis is a separately selected atomic `[3]` task using Float64 or Int64. Count one reads only start while still checking both schemas; larger counts read both scalar payloads.

Each sample uses its global index and exact arithmetic before direct rounding. Dynamically read inputs and selected outputs must satisfy finite/representability preconditions without runtime numerical validation. Unselected coordinates are not computed. Tasks share one IR call's publication outcome, private invocation scratch and independent output allocations. Empty selection emits no tasks. See [fixed sequence execution](../built-in_ops/01-numeric/sequences-execution.md).

## Fixed ordering profiles

The six `array.sort_<profile>` and `numeric.quantile_<profile>` keys accept UInt8/Int64/Float32/Float64 source tensors with a static axis. Sort groups selected values/indices tiles by their non-axis rectangle and sorts each complete source line once per task. Only selected output tiles are allocated. Quantile produces one sample per selected line, with exact binary rank and interpolation followed by final RN conversion. Source NaNs, infinities and signed zeros retain the specified ordering/conversion behavior; sort preserves raw bits.

Each task declares complete source-line tile support using the source grid. Quantile reads scalar q for axis length greater than one and records only its schema otherwise. Active q must be finite in `[0,1]`; a constant-time guard protects address and shift safety. Scratch uses two L-word buffers and fixed comparison/exact arithmetic state, reused across lines. Independent Runs own their scratch; tasks share their IR call's publication outcome. See [fixed ordering execution](../built-in_ops/01-numeric/ordering-execution.md).

## Fixed calculus profiles

The six `numeric.{derivative_1d,integrate_1d}_<profile>` keys use same-dtype Float32/Float64 `[N]` samples and scalar controls. Derivative tasks declare the union of lower/upper stencil source tiles and step for each selected output tile. With an ordinary source axis and extent one, an interior center has no dedicated source tile edge; atomic source tile rounding can still cover it. Integration uses one task for all selected positive-index tiles, accumulates an exact source prefix through the largest selected coordinate and converts only selected outputs. A tile containing only index zero has a separate initial-only task with zero scratch and schema-only samples/step references.

Active step is finite and nonzero as a caller precondition. IEEE source and initial values follow the discrete formulas. Prefix carry survives final conversion, and each invocation uses fixed ExactCalculus scratch without payload allocation. Concurrent Runs share only immutable preparation. Source/control closure, canonical writes and scratch are checked before invocation. See [fixed calculus execution](../built-in_ops/01-numeric/calculus-execution.md).

## Fixed expression profiles

The three `numeric.sample_expression_<profile>` keys parse one bounded immutable AST and accept mixed Float32/Float64 scalar endpoints and named coefficients. Each selected values tile executes one fixed CPU task with global endpoint-weighted RN64 coordinates. Axis is a separate atomic Float64 triple task, with no coefficient reads. Count one excludes end payload; all static schemas remain checked. Strict rounds each AST primitive before its parent; accelerated batches enclose the complete strict reference and replay uncertain samples.

Numerical domains, finite inputs/intermediates/results and sampling precision are unchecked caller preconditions. Preparation verifies canonical output, exact active scalar reads and fixed scratch. Sparse requests allocate only selected tiles, while independent Runs share the AST and own their arithmetic state. Selected tasks share one call's publication outcome. See [fixed expression execution](../built-in_ops/01-numeric/expression-execution.md).

## Fixed curve interpolation profiles

The twelve `curve.interpolate_{linear,pchip}[_multi]_<profile>` keys use fixed CPU tasks. Each selected output tile reads all x, its query rows and all y rows for its columns, closed through each source grid. The task searches input views directly, shares query classification across its columns and evaluates selected global outputs. Scratch contains one complete ExactCurve arithmetic state per invocation.

Finite consumed inputs, strictly increasing x, the reject domain and output representability are caller preconditions. Strict/Float64 rounds the complete formula once; accelerated Float32 accepts only uniquely rounded enclosures and otherwise evaluates exactly. The four interpolation baking and four resampling templates execute through the fixed executor, with query/axis/positions dependencies determined by their expanded nodes. See the [CRV-01 fixed contract](../built-in_ops/01-numeric/op_specs/CRV-01_interpolate.md#fixed-execution-resources-and-caller-preconditions).

## Fixed inverse-curve profiles

The six `curve.invert_{linear,pchip}_<profile>` keys share the fixed adapter with forward interpolation. Each selected output tile reads complete x/y and its query rows, searches y in its monotone direction and inverts the original mathematical curve. Each invocation owns preallocated ExactCurve scratch; only selected global outputs are evaluated and allocated, with no promoted x/y arrays.

The complete x/y arrays must be finite, x strictly increasing and y globally strictly increasing or decreasing without plateaus. Consumed queries must be finite and within the y domain under reject; selected outputs must be representable. These are caller preconditions. Exact lattice/midpoint rounding, zero signs, interval certification and internal capacity checks remain. See the [CRV-10 fixed contract](../built-in_ops/01-numeric/op_specs/CRV-10_invert.md#fixed-execution-resources-and-caller-preconditions).

## Fixed dynamic-axis LUT1D profiles

The six `curve.apply_lut1d[_channels]_<profile>` keys preserve the complete input shape with fixed CPU tasks. L>1 reads selected queries, all table rows for selected columns and axis endpoints through each source grid. Bounded binary search reconstructs the original endpoint-weighted RN64 knots on demand, using constant-size ExactSampling/ExactCurve scratch. Only selected output tiles are allocated.

L=1 converts the sole selected table row with zero scratch; input/axis retain static schema checks but have no payload dependency. The complete axis, step consistency, finite consumed values, reject domain and representable output remain unchecked caller preconditions, including singleton reject equality. See the [CRV-05 fixed contract](../built-in_ops/01-numeric/op_specs/CRV-05_apply_lut1d.md#fixed-execution-resources-and-caller-preconditions).

## Fixed parametric Bezier profiles

The three `curve.evaluate_bezier_<profile>` keys execute one fixed CPU task per selected output tile. Segment/t reads cover its query rows; anchors/handles cover all candidate segments for its selected columns, closed through each source grid. The segment guard protects dynamic control addresses. A task reuses row classification across columns and writes only selected global outputs, with fixed ExactPolynomial/ExactSampling scratch per invocation.

Finite t in [0,1], finite consumed and RN64-reconstructed controls, and representable finite outputs are unchecked caller preconditions. Endpoint selection and exact polynomial rounding preserve zero signs. All current profiles use the same exact numerical algorithm. See [CRV-03 fixed execution](../built-in_ops/01-numeric/op_specs/CRV-03_evaluate_bezier.md#fixed-execution-and-physical-demand).

## Fixed Bezier function sampling

The three `curve.sample_bezier_function_<profile>` keys execute each selected values tile with complete controls and active endpoints, direct anchor search and selected-segment RN64 reconstruction. Exact inverse/evaluation retains the original mathematical function. Axis is a separate scalar-only task; count one excludes end and uses zero axis scratch. All static schemas remain checked, and selected tasks share one call's ordered publication outcome.

Global x topology and selected numerical conditions are caller preconditions. Values require selected-coordinate neighbor separation; only axis selection requires a representable nonzero step. Each invocation owns fixed arithmetic scratch and only selected output tiles are allocated. The Bezier LUT baking template executes through this sampler. See [CRV-02 fixed execution](../built-in_ops/01-numeric/op_specs/CRV-02_sample_bezier_function.md#fixed-execution-and-dependency-projection).

## Fixed logarithmic shapers

The six `curve.log2_shaper[_inverse]_<profile>` keys execute one fixed CPU task per selected output tile, reading its input region and both same-dtype scalar bounds through the source grids. Finite 0<lower<upper is a caller precondition. Input NaN payloads, signed zeros, infinities, domain extensions and successful IEEE overflow/underflow follow the family contract; generic finite-input assumptions do not apply to queries.

Each invocation owns preallocated ExactShaper state. Exact/certified whole-formula arithmetic uses fixed retry/range/failure state and no exception allocation for those transitions. Only selected output tiles are allocated, and required tasks share ordered call publication. Linear shapers remain fixed remap/constant compositions. See [CRV-08 fixed execution](../built-in_ops/01-numeric/op_specs/CRV-08_shaper.md#fixed-log-execution).

## Fixed three-dimensional LUT application

The six trilinear/tetrahedral LUT3D keys emit one fixed CPU task per selected complete-color output tile. Reads close its query region, the complete table and six axis endpoints on independent source grids. Numerical axis consistency/order, original query legality, contributing table color legality, reject-domain membership and finite output are caller preconditions. Only positive-weight complete vertices enter exact arithmetic. Each invocation owns fixed knot/arithmetic scratch and writes a dense output tile with one atomic trailing component axis; Empty emits no tasks. See [CRV-07](../built-in_ops/01-numeric/op_specs/CRV-07_apply_lut3d.md).

## Fixed color ramps

The 45 color-ramp keys execute one fixed CPU task per selected complete-color output tile. Query reads follow that tile; stops/colors and optional rational hue arrays have complete conservative closures on their own grids. The kernel searches source stops directly and evaluates one or two complete rows with fixed exact/certified arithmetic scratch. Finite ordered stops, selected model/association legality, rational denominator positivity, reject-domain membership and output representability are caller preconditions. Empty emits no task; CMYK still validates its selected frozen ICC resource during operation preparation. See [CRV-06](../built-in_ops/01-numeric/op_specs/CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions).

## Fixed uniform low-pass filtering

The fifteen `curve.lowpass_uniform_*` keys emit one ABI 8 CPU task per selected output tile. Static nonzero taps and reflect/replicate/zero/wrap mapping determine source intervals, which close on the source's own tile grid. Extra physical samples do not become operands. Output preserves complete shape/dtype as ordinary axes, with zero atomic trailing axes; only selected tiles are allocated. Each invocation borrows affine source views and owns fixed arithmetic/sample/coefficient scratch. Empty emits no task.

NaN/Inf samples retain the specified logical tap-order aggregation. Strict rounds the exact normalized sum once; accelerated candidates pass the final FP32 bound or use strict arithmetic. Even constant/symmetry shortcuts require a positive-normalizer certificate. Checked arithmetic returns cancellation/resource failures directly before numerical retries. See the [uniform low-pass contract](../built-in_ops/01-numeric/op_specs/CRV-11_uniform_lowpass_contract.md).

## Fixed nonuniform low-pass filtering

The fifteen `curve.lowpass_nonuniform_*` keys emit one ABI 8 CPU task per selected output tile. Reads cover complete positions and the full filtered values axis for selected other-axis coordinates, closed on independent source grids. Positions are searched directly; each invocation streams exact positive-length segments and keeps two current pieces in fixed scratch. Polynomial arrays contain 513, 513 and 1025 intervals. Only selected outputs are allocated, with ordinary output axes and zero atomic trailing axes; Empty emits no task.

Finite strictly increasing positions, finite contributing endpoints and finite representable selected outputs are caller preconditions. Strict and accelerated keys currently use the same certified continuous integral and exact paired-affine shortcuts. Local constant raw bits, including -0, are preserved. Cancellation/resource failures propagate before shortcuts or numerical retries. Large support can traverse many periods, with cancellation checked in every piece and arithmetic loop. See the [nonuniform low-pass contract](../built-in_ops/01-numeric/op_specs/CRV-11_nonuniform_lowpass_contract.md).

## Fixed LUT3D baking geometry

The fifteen `curve.bake_lut3d_{axis,grid,points,points_extra,color}` profile keys emit one fixed CPU task per selected output tile. Axis copies selected cells. Grid and center tasks read six endpoints and reconstruct RN64 knots with fixed ExactSampling scratch; centers average the two actual neighboring knots. Extra-only tiles read selected extra rows without axis payload or scratch. Color copies same-dtype bits or converts once under RN, with zero scratch. Source closures follow each input grid; outputs preserve global coordinates and use one atomic trailing component axis except for axis. Empty emits no task.

Finite consistent axes, model-valid colors, in-range extras and finite representable conversion are caller preconditions. Direct invocation borrows signed/unaligned views, owns its scratch and writes only selected dense tiles. Pack/Measure/Unpack/Gate use fixed CPU invocations. Pack/Unpack/Gate copy selected samples into independent dense tiles. A nonempty Measure waits for the complete owned table, reads every validation point/reference/applied color, and writes all thirteen report members with fixed exact-integer scratch. Report and table projections retain this global measurement closure. The retirement observer can retain the report even if a later Gate fails. Report v3 stores the actual owned-table ObjectId from the invocation association array. Gate checks configuration identity (`TypeMismatch`), then table identity (`OperationFailed/InvalidAssociation`), then tolerance (`OperationFailed/InvalidDomain` with first-failure details). Well-formed report values are caller premises; the SDK reader independently validates them. See [CRV-09](../built-in_ops/01-numeric/op_specs/CRV-09_bake_lut3d.md#fixed-geometry-tasks).

## Backend selection

A profile that cannot be selected on the host fails during preparation; callers should use the strict suffix when portability is required. Profile names do not promise GPU execution.

## Inputs, ownership and failures

Numeric programs use Result inputs and outputs. Fixed kernels borrow the views declared by the physical Plan and write preallocated output tiles. Shared intermediate computation uses explicit tasks and private values. Preparation, shape rules and batch mapping belong to each operation family.

Invalid static parameters return `InvalidArgument`. Unsupported or incompatible dtype, shape, or schema metadata returns `TypeMismatch`. The eight unsuffixed numeric pointwise keys require valid finite-domain inputs. Fixed profile families follow their own numerical rules, including specified IEEE special values. The fixed ordered reductions and scan require the same valid finite-domain inputs. Fixed interpolation profiles preserve IEEE special values according to their specified rules and require valid factors or edges from the caller. Resource exhaustion and cancellation retain their operational status codes. The fixed executor exposes successful call Results in retirement order after all declared task writes succeed.

## Public workflows and checks

Operator arithmetic is checked with temporary independent numerical references
and public Result workflows during the operation change. Remove these checkers
after validation, as described in [Testing and Validation](../development/Testing-and-Validation.md).
The retained kernel [test_fixed_execution](../../tests/integration/kernel/test_fixed_execution.cpp) uses synthetic operations to check concurrent issue, ordered retirement, cancellation drain and observer failures. It does not establish the numerical correctness of bundled operations.

Generic finite arithmetic accepts compatible single-Tensor Float32/Float64 inputs with equal complete sample shape, including structurally spatial inputs. There is no separate planar capability declaration or automatic RGBA validator.
