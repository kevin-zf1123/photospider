# Numeric operations

The default registry contains unsuffixed finite-domain arithmetic and clamp keys, staged scalar reductions, an ordered scan, and profile-specific numeric families. Their parameter, execution and output contracts differ, so choose a key by its exact name. These are bundled operations in `plugins/ops/01-numeric/`, not kernel code; the kernel supplies only the generic Result execution, resource, cancellation, and failure contracts they use. Operation plugins use the Result operation C ABI 7. Numeric work reports, LUT bake reports, and quality reports are operations-SDK types (`photospider/ops/numeric/`); see [Atom errors and quality](Atom-Errors-and-Quality.md).

## Unsuffixed Result numeric operations

| Key | Inputs and output | Parameters and region |
| --- | --- | --- |
| `math.add` | Two Float64 tensor Results with one sample each (`sample_shape() == [1]`); output port `value` is a `photospider.tensor` v1 Result with member `samples`, shape `[1]` | No parameters; Dependency; IEEE addition |
| `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` | Two Float32/Float64 Result inputs with identical full `sample_shape()` and dtype; output port `value` is a `photospider.tensor` v1 Result with tensor member `samples`, preserving shape and dtype | No parameters; complete-input |
| `numeric.clamp` | One Float32/Float64 Result input; output port `value` uses the same Result schema and preserves shape and dtype | Required finite Float64 `min`, `max`, with `min <= max`; complete-input |
| `numeric.abs` | One Float32/Float64 tensor Result; output port `value` is `photospider.tensor` v1/member `samples`, same sample shape and dtype | No parameters; tile continuation |
| `numeric.minimum`, `numeric.maximum` | Two Float32/Float64 tensor Results with identical sample shape and dtype; output port `value` is `photospider.tensor` v1/member `samples` | No parameters; tile continuation |

Each input Result must contain exactly one tensor. Any structurally valid schema id, version and tensor member key is accepted. The rank is 1..8, and `sample_shape()` includes batch axes. Outputs use that complete shape as ordinary tensor axes; no input description or interpretation is carried to the output. Programs read input data through authorized windows and publish results transactionally under the member-tile publication contract.

The `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` and `numeric.clamp` keys calculate in the input dtype and reject nonfinite inputs or results. Division rejects either signed zero. Clamp requires finite static bounds and checks `min <= max` during immutable preparation. It checks the selected clamped value against the destination dtype before narrowing to Float32; an unused endpoint may exceed the Float32 range. The operations do not broadcast or cast. Immutable preparation also checks the dense output byte count; a count not representable in 64 bits fails with `ResourceExhausted` before execution.

`math.add` is a separate scalar Result operation. Metadata preparation requires each input to contain one Float64 tensor,  and complete `sample_shape()` `[1]`; the output has the same scalar shape. A nonempty request reads both one-sample tensors through ordinary tile Needs. The callback performs ordinary IEEE Float64 addition, so NaN and infinity propagate according to that addition rather than the finite-domain `numeric.add` contract. Empty demand performs no input reads.

A nonempty complete-input request for `numeric.add`, `numeric.subtract`, `numeric.multiply`, `numeric.divide` and `numeric.clamp` declares ordinary payload tile Needs for every complete input tensor. The program reads from authorized Result windows rather than collecting input tensors, and publishes no partial result. Empty requests perform static preflight but read no sample payload. There is no separate 2^40 logical-element limit, so a large legal zero-stride source can remain representable when its dense output fits. The arithmetic callback restores the caller's floating-point environment.

The unsuffixed finite `abs`, `minimum` and `maximum` keys accept Result inputs containing exactly one tensor. They accept structurally valid schema ids, versions and member keys; all ports must have the same Float32/Float64 dtype and complete `sample_shape()` (rank 1..8 including batch axes). The output is a generic `photospider.tensor` v1 Result with member `samples`, and the same complete shape as ordinary axes. The keys run on CPU with tile continuation needs, not the complete-input contract used by their suffixed profile keys.

For each nonempty output tile, the operation requests the input member tiles intersecting its closed sample support. Validation and arithmetic read only authorized coverage. Atomic closure and spatial tile geometry may enlarge that coverage; all admitted payload Needs contribute tile dependency evidence without role tags. Empty payload demand does not start a producer.

The finite kernels reject nonfinite arithmetic inputs or intermediate values with `OperationFailed`. `abs(-0)` returns `+0`; `minimum(-0,+0)` returns `-0`, and `maximum(-0,+0)` returns `+0`. They restore the caller's floating-point environment. Their finite-only error and sparse Dependency contracts differ from the suffixed complete-input profile keys; the suffixed keys follow their own IEEE rules, including per-operation signed-zero behavior.

## Staged Result reductions and ordered scan

| Key | Inputs and output | Parameters and region |
| --- | --- | --- |
| `numeric.mean`, `numeric.variance` | One Float32/Float64 tensor Result, rank 1..8; output port `value` is a Float64 scalar Result with member `samples` shape `[1]` | Optional Int64 `block_size` in `[1,65536]`, default 64; staged tensor Needs |
| `numeric.ordered_scan` | One rank-1 Float64 tensor Result; output port `value` is a same-shape Float64 Result with member `samples` | Optional Int64 `block_size` in `[1,65536]`, default 64; Run-shared complete-input stage |

Mean and variance consume one Float32/Float64 Tensor in logical row-major intervals through tile Needs. They check finite samples during the first arithmetic pass, not through a default typed-image validator. Mean accumulates Float64 in order; variance performs two passes with population denominator (ddof=0). Ordered state preserves accumulation order across polls. block_size selects the nominal interval; it rounds up to complete atomic tuples when necessary. Both publish one Float64 scalar. Nonfinite input or unrepresentable accumulation fails. These ordinary scalar actors may use the block service and are distinct from the shared ordered_scan below.

`numeric.ordered_scan` accepts rank-1 Float64 input and returns a same-shape `photospider.tensor` v1/member `samples` on output `value`. A nonempty tile request starts one Run-shared complete-input computation. After nonblocking tile Needs grant the full input, it performs inclusive addition from positive zero in logical index order, with strict left-to-right Float64 addition. Optional Int64 `block_size` is 1..65536, default 64, and only controls work charging and cancellation intervals, not addition grouping. A nonfinite input or accumulated overflow anywhere in the input fails with its global index, including positions beyond the requested output tile. Output actors reuse the shared result. This operation does not use prefix checkpoints or block-cache reuse.

For all three operations, an Empty output demand runs static preflight and returns a sealed Result with no tensor coverage. It issues no input Need.

The generic block service operates on sealed single-Tensor state with no publication scope. It is separate from the Run-shared scan above; shared stages expose no block or checkpoint service. Optional transition retention is not the unique-producer mechanism. See [Dependency Data](Dependency-Data.md#result-block-state-transitions).

## Profile-specific pointwise operations

The numeric math families use three explicit suffixes:

| Profile suffix | Admission |
| --- | --- |
| `_strict` | Portable strict implementation |
| `_accelerated_apple_silicon` | Apple Silicon implementation when its runtime profile is available |
| `_accelerated_x86_64` | x86-64 accelerated implementation when its runtime profile is available |

`numeric_binary.cpp` registers each suffix for `add`, `subtract`, `multiply`, `divide`, `minimum`, `maximum`, `pow`, `atan2` and `atan2pi`. The exact elementary kernels accept same-dtype UInt8, Int64, Float32 and Float64 inputs for supported operations; divide is Float32/Float64 only. The certified transcendental functions use Float32/Float64 and their own domain and representability checks. These keys are Complete-input shared stages and keep dtype and shape. NaNs propagate according to each operation's exact kernel; domain and range failures follow each family's typed contract. `numeric.clamp_<profile>` and `numeric.remap_range_<profile>` report invalid bounds as `InvalidArgument/InvalidDomain`. Use the strict key when the selected accelerated profile is unavailable or its backend policy is not appropriate.

The suffixed reduction family registers `numeric.reduce_sum`, `numeric.reduce_minimum`, `numeric.reduce_maximum`, `numeric.reduce_mean`, `numeric.reduce_count`, `numeric.reduce_variance` and `numeric.reduce_std`, each with the same three suffixes. These are Complete-input shared stages. They require a comma-separated String `axes` list of unique canonical nonnegative axis indices; sum, mean, variance and standard deviation also require String `dtype` (`uint8`, `int64`, `float32` or `float64`), subject to source and operation-domain compatibility. Variance and standard deviation require Int64 `ddof` in `0 <= ddof < N`, where `N` is the product of the reduced extents. Reduced axes become length one; unreduced extents remain unchanged. The implementation rejects invalid axes, unsupported dtype combinations, oversized inputs and invalid degrees of freedom. These reductions have profile-specific exact arithmetic and output conversion. They are separate registered keys from the unsuffixed staged `numeric.mean` and `numeric.variance` above.

`numeric.clamp_<profile>` and `numeric.remap_range_<profile>` use the same suffixes. Clamp takes three same-shape, same-dtype inputs in value, lower-bound, upper-bound order. Remap takes five in value, source-low, source-high, destination-low, destination-high order. Both preserve input shape and dtype, require rank 1..8, use complete-input regions and reject invalid bounds during execution. This clamp family is distinct from the unsuffixed two-static-parameter `numeric.clamp`.

The unsuffixed mean/variance operations described above carry ordered state in a sealed single-Tensor Result and may use optional block retention. Profile-specific reductions instead compute exact aggregate/moments inside a shared stage without block services. Their reduce_count variant uses an empty input projection and derives counts from static metadata without a payload Need.

The profile-specific pointwise and reduction implementations account work and cancellation through the execution resource budget. A profile that cannot be selected on the host fails during preparation; callers should use the strict suffix when portability is required. Profile names do not promise GPU execution.

## Inputs, ownership and failures

The numeric programs described here use Result inputs and outputs. Tensor programs preserve their declared Result schema and read only Need-authorized windows; preparation, shape rules, batch mapping and publication requirements belong to each operation family. Structured Result operations expose typed Result inputs and outputs; `Tensor` may still serve as private typed backing.

Invalid static parameters return `InvalidArgument`. Unsupported or incompatible dtype, shape, or schema metadata returns `TypeMismatch`. Unsuffixed finite-domain arithmetic, staged reductions, and the scan report their documented nonfinite or unrepresentable cases as operation failures. Profile-specific exact kernels preserve or classify IEEE values according to the operation; typed domain failures can return `InvalidArgument/InvalidDomain`. Resource exhaustion and cancellation retain their own status codes. complete-input Result operations publish only after the complete output succeeds; staged Result operations publish according to their declared fixed publication scope.

## Public workflows and checks

Operator arithmetic is checked with temporary independent numerical references
and public Result workflows during the operation change. Remove these checkers
after validation, as described in [Testing and Validation](../development/Testing-and-Validation.md).
The retained kernel [test_scan_waiters](../../tests/integration/kernel/test_scan_waiters.cpp)
uses a synthetic scan to check checkpoint sharing between concurrent runs,
cancellation isolation, error propagation, cache clearing and Queue resource release.
It does not establish the numerical correctness of bundled scan operations.

Generic finite arithmetic accepts compatible single-Tensor Float32/Float64 inputs with equal complete sample shape, including structurally spatial inputs. There is no separate planar capability declaration or automatic RGBA validator.