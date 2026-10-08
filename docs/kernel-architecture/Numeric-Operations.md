# Numeric operations

The default registry contains unsuffixed finite-domain arithmetic and clamp keys, staged scalar reductions, an ordered scan, and profile-specific numeric families. Their parameter, execution and output contracts differ, so choose a key by its exact name. Operation plugins use the Result operation C ABI 2.

## Unsuffixed Result numeric operations

| Key | Inputs and output | Parameters and region |
| --- | --- | --- |
| `math.add` | Two Float64 tensor Results with one sample each (`sample_shape() == [1]`); output port `value` is a `photospider.tensor` v1 Result with member `samples`, shape `[1]` | No parameters; Dependency; IEEE addition |
| `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` | Two Float32/Float64 Result inputs with identical full `sample_shape()` and dtype; output port `value` is a `photospider.tensor` v1 Result with tensor member `samples`, preserving shape and dtype | No parameters; Whole |
| `numeric.clamp` | One Float32/Float64 Result input; output port `value` uses the same Result schema and preserves shape and dtype | Required finite Float64 `min`, `max`, with `min <= max`; Whole |
| `numeric.abs` | One Float32/Float64 tensor Result; output port `value` is `photospider.tensor` v1/member `samples`, same sample shape and dtype | No parameters; staged Dependency |
| `numeric.minimum`, `numeric.maximum` | Two Float32/Float64 tensor Results with identical sample shape and dtype; output port `value` is `photospider.tensor` v1/member `samples` | No parameters; staged Dependency |

Each input Result must contain exactly one tensor and no fields. Any structurally valid schema id, version and tensor member key is accepted. The rank is 1..8, and `sample_shape()` includes batch axes. Outputs use that complete shape as ordinary tensor axes, with no copied source facets; each output schema selects its own resources. Programs read input data through authorized windows and publish results transactionally according to their region rule.

The `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` and `numeric.clamp` keys calculate in the input dtype and reject nonfinite inputs or results. Division rejects either signed zero. Clamp requires finite static bounds and checks `min <= max` during immutable preparation. It checks the selected clamped value against the destination dtype before narrowing to Float32; an unused endpoint may exceed the Float32 range. The operations do not broadcast or cast. Immutable preparation also checks the dense output byte count; a count not representable in 64 bits fails with `ResourceExhausted` before execution.

`math.add` is a separate scalar Result operation. Metadata preparation requires each input to contain one Float64 tensor, no fields, and complete `sample_shape()` `[1]`; the output has the same scalar shape. A nonempty request reads both one-sample tensors through role-13 Needs. The callback performs ordinary IEEE Float64 addition, so NaN and infinity propagate according to that addition rather than the finite-domain `numeric.add` contract. Empty demand performs no input reads.

A nonempty Whole request for `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` and `numeric.clamp` declares Data, Validation and Descriptor needs (role 13) for every complete input tensor. The program reads from authorized Result windows rather than collecting input tensors, and publishes no partial result. Empty requests perform static preflight but read no sample payload. There is no separate 2^40 logical-element limit, so a large legal zero-stride source can remain representable when its dense output fits. The arithmetic callback restores the caller's floating-point environment.

The unsuffixed finite `abs`, `minimum` and `maximum` keys accept Result inputs containing exactly one tensor and no fields. They accept structurally valid schema ids, versions and member keys; all ports must have the same Float32/Float64 dtype and complete `sample_shape()` (rank 1..8 including batch axes). The output is a generic `photospider.tensor` v1 Result with member `samples`, the same complete shape as ordinary axes, and empty facets. The keys run on CPU with staged Dependency needs, not the Whole contract used by their suffixed profile keys.

For a nonempty output footprint Q, the operation validates each input over `close_samples(Q)`, which closes typed tuples and atomic sample groups, then requests Data over Q. If `close_samples(Q)` equals Q, the program combines Data and Validation roles (1 and 4) into one Need. If closure expands validation, it sends a Validation role-4 Need followed by a Data role-1 Need. Arithmetic reads are restricted to Q; Validation reads and checks `close_samples(Q)`, which can include typed tuple or atomic-group members outside Q. No Need extends beyond `close_samples(Q)`. Its relation records Data and Validation mappings separately plus the input descriptor basis. An Empty request runs static preflight and returns a sealed Result with no tensor coverage, without issuing input Needs.

The finite kernels reject nonfinite arithmetic inputs or intermediate values with `OperationFailed`. Typed Result source validation can instead fail before arithmetic with `ErrorCode::InvalidArgument` and `FailureReason::InvalidDomain`, preserving the source `input_id`. `abs(-0)` returns `+0`; `minimum(-0,+0)` returns `-0`, and `maximum(-0,+0)` returns `+0`. They restore the caller's floating-point environment. Their finite-only error and sparse Dependency contracts differ from the suffixed Whole profile keys; the suffixed keys follow their own IEEE rules, including per-operation signed-zero behavior.

## Staged Result reductions and ordered scan

| Key | Inputs and output | Parameters and region |
| --- | --- | --- |
| `numeric.mean`, `numeric.variance` | One Float32/Float64 tensor Result, rank 1..8; output port `value` is a Float64 scalar Result with member `samples` shape `[1]` | Optional Int64 `block_size` in `[1,65536]`, default 64; staged tensor Needs |
| `numeric.ordered_scan` | One rank-1 Float64 tensor Result; output port `value` is a same-shape Float64 Result with member `samples` | Optional Int64 `block_size` in `[1,65536]`, default 64; prefix tensor Needs and checkpoints |

Each input is a Result carrying one tensor. Mean and variance accept Float32 or Float64 samples over the complete logical `sample_shape()`, rank 1..8. They request staged row-major intervals and read only those Need-authorized samples. Typed-source validation completes before arithmetic. Mean accumulates in Float64 in logical row-major order. Variance uses two passes: it computes the mean first, then accumulates squared deviations with population denominator (`ddof=0`). Carried state preserves the exact stage boundary and does not reassociate the sum. Both publish one Float64 scalar tensor Result on the `value` port. `block_size` bounds the sample interval; it does not batch output observations. Nonfinite values and unrepresentable accumulated results fail.

`numeric.ordered_scan` starts from a positive-zero Float64 carry and computes inclusive prefixes with strict left-to-right addition, nearest-even rounding, and gradual underflow. A nonempty requested output reads input from index zero through the greatest requested index; an Empty request reads no sample payload. The output uses the `value` port. Each published prefix carries an exact prefix relation, and successful carries can be restored from checkpoints before continuing. The first nonfinite input or accumulated overflow fails with its global input index.

For all three operations, an Empty output demand runs static preflight and returns a sealed Result with no tensor coverage. It issues no input Need.

`ResultProgramPhase::block` advances one explicit state transition. The incoming and returned states are sealed same-schema, same-Root `CompleteBundle` Results, each with one fully covered tensor and no fields. The tensor carries every value needed by later transitions, including carried controls. The callback receives the transition kind, half-open range `[begin,end)`, mode, incoming state and compute callback. Optional internal retention may skip a state when proof or resource bounds do not admit it; the operation then computes normally. Block-cache keys, hits and transition reuse are implementation details, not performance guarantees.

## Profile-specific pointwise operations

The numeric math families use three explicit suffixes:

| Profile suffix | Admission |
| --- | --- |
| `_strict` | Portable strict implementation |
| `_accelerated_apple_silicon` | Apple Silicon implementation when its runtime profile is available |
| `_accelerated_x86_64` | x86-64 accelerated implementation when its runtime profile is available |

`numeric_binary.cpp` registers each suffix for `add`, `subtract`, `multiply`, `divide`, `minimum`, `maximum`, `pow`, `atan2` and `atan2pi`. The exact elementary kernels accept same-dtype UInt8, Int64, Float32 and Float64 inputs for supported operations; divide is Float32/Float64 only. The certified transcendental functions use Float32/Float64 and their own domain and representability checks. These keys are Whole operations and keep dtype and shape. NaNs propagate according to each operation's exact kernel; domain and range failures follow each family's typed contract. `numeric.clamp_<profile>` and `numeric.remap_range_<profile>` report invalid bounds as `InvalidArgument/InvalidDomain`. Use the strict key when the selected accelerated profile is unavailable or its backend policy is not appropriate.

The suffixed reduction family registers `numeric.reduce_sum`, `numeric.reduce_minimum`, `numeric.reduce_maximum`, `numeric.reduce_mean`, `numeric.reduce_count`, `numeric.reduce_variance` and `numeric.reduce_std`, each with the same three suffixes. These are Whole operations. They require a comma-separated String `axes` list of unique canonical nonnegative axis indices; sum, mean, variance and standard deviation also require String `dtype` (`uint8`, `int64`, `float32` or `float64`), subject to source and operation-domain compatibility. Variance and standard deviation require Int64 `ddof` in `0 <= ddof < N`, where `N` is the product of the reduced extents. Reduced axes become length one; unreduced extents remain unchanged. The implementation rejects invalid axes, unsupported dtype combinations, oversized inputs and invalid degrees of freedom. These reductions have profile-specific exact arithmetic and output conversion. They are separate registered keys from the unsuffixed staged `numeric.mean` and `numeric.variance` above.

`numeric.clamp_<profile>` and `numeric.remap_range_<profile>` use the same suffixes. Clamp takes three same-shape, same-dtype inputs in value, lower-bound, upper-bound order. Remap takes five in value, source-low, source-high, destination-low, destination-high order. Both preserve input shape and dtype, require rank 1..8, use Whole regions and reject invalid bounds during execution. This clamp family is distinct from the unsuffixed two-static-parameter `numeric.clamp`.

The continuation carries its reduction state in a complete tensor Result. Variance's second-pass state includes the first-pass mean. Block-state retention is optional implementation behavior; the operation remains correct when each transition is recomputed.

The profile-specific pointwise and reduction implementations account work and cancellation through the execution resource budget. A profile that cannot be selected on the host fails during preparation; callers should use the strict suffix when portability is required. Profile names do not promise GPU execution.

## Inputs, ownership and failures

The numeric programs described here use Result inputs and outputs. Tensor programs preserve their declared Result schema and read only Need-authorized windows; preparation, shape rules, batch mapping and publication requirements belong to each operation family. Structured Result operations expose typed Result inputs and outputs; `Value` may still serve as private typed backing.

Invalid static parameters return `InvalidArgument`. Unsupported or incompatible dtype, shape, or schema metadata returns `TypeMismatch`. Unsuffixed finite-domain arithmetic, staged reductions, and the scan report their documented nonfinite or unrepresentable cases as operation failures. Profile-specific exact kernels preserve or classify IEEE values according to the operation; typed domain failures can return `InvalidArgument/InvalidDomain`. Resource exhaustion and cancellation retain their own status codes. Whole Result operations publish only after the complete output succeeds; staged Result operations publish according to their declared prefix or dependency contract.

## Public workflows and checks

[`tests/integration/test_numeric_operations.cpp`](../../tests/integration/test_numeric_operations.cpp) covers registered arithmetic behavior. Split Result numeric oracles live under [`tests/integration/numeric`](../../tests/integration/numeric), with separate files for arithmetic, arrays, sequences, statistics, matrix, calculus, curves, Bezier, LUTs, baking, expressions, filters and color ramps. Ordered reduction and scan behavior is exercised by [`test_ordered_reduction.cpp`](../../tests/integration/test_ordered_reduction.cpp), [`test_ordered_scan.cpp`](../../tests/integration/test_ordered_scan.cpp) and [`test_scan_waiters.cpp`](../../tests/integration/test_scan_waiters.cpp). These tests use independent numerical expectations; block-cache observations do not establish performance. The installed Result numeric consumer is registered as `installed_result_numeric` in `tests/consumer/CMakeLists.txt`.

Generic arithmetic keys do not declare planar image capability. Their registration does not imply that a structural planar image can be passed as a generic numeric array.
