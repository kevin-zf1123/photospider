# Finite Gaussian runtime

The default registry provides `filter.gaussian_baked64_v1_strict_cpu_whole`,
`filter.gaussian_baked64_v1_strict_cpu_tiled`, and
`filter.gaussian_baked64_v1_strict_gpu`. Each operation accepts one Result with a
single tensor member and publishes one Result tensor on output port `output`.
The input schema has no fields and describes a Float32/Float64 tensor of
rank 2..8, positive extents, and at most 2^40 samples. Gaussian preserves the
complete sample shape, including batch axes, and clones the schema, facets, and
bound resources to its output. This supports generic numeric tensors and existing
typed image Results such as photospider.image, retaining their color facets and ICC
profile owners so the output can feed other image operations. Gaussian does not
add image semantics to a raw numeric tensor.

The operations evaluate FIL-04B's baked64 coefficient profile with one final
rounding of the complete normalized two-dimensional expression. FIL-04 remains
Proposed; runtime registration does not change that specification status. Whole
and GPU demand the complete input and publish a complete output. CPU tiled
publishes requested chunks under IndependentChunks. The GPU form selects Metal
MSL or Vulkan SPIR-V from the active native service and has no CPU fallback. The
Result migration passes the current focused GPU test and installed-consumer
checks on Metal.

## Ports and explicit parameters

Input port `input` and output port `output` each select one Result tensor member;
that member key is preserved in the output schema. The input Result has no fields,
and its sole Float32/Float64 tensor has rank 2..8, positive extents, and at most
2^40 samples across all descriptor and batch axes. The output retains its sample
shape, layout, facets, and Result resources. Raw numeric tensors may contain NaN
or infinity; parameter validation requires finite sigma and cval values. All
parameters below are mandatory static values; there are no defaults.

| Parameter | Type | Domain |
| --- | --- | --- |
| `sigma_x`, `sigma_y` | Float64 | Finite, nonnegative |
| `radius_x`, `radius_y` | Int64 | Nonnegative; a zero sigma requires zero radius |
| `x_axis`, `y_axis` | Int64 | Distinct nonnegative axes in the complete sample shape |
| `boundary` | String | `constant`, `clamp`, `wrap`, `reflect_half`, `reflect_whole` |
| `cval` | Float64 | Finite, including when the input is Float32 |

Other axes are independent planes. Channel counts do not trigger color or alpha
arithmetic. Valid input facets and their resource bindings are retained by Result
schema cloning. A typed ColorArray v1 input keeps its full batch, channel tuple,
and ICC profile contract. Result input bindings can expose validated affine or
paged storage; the kernel reads only the authorized windows supplied by its Need.
Axis indices address the complete sample shape, with batch axes before descriptor
axes. For RGBA sample shape {N,L,H,W,4}, y_axis=2 and x_axis=3.

Whole and GPU request the complete input tensor and publish the complete output.
An input edit invalidates the complete output. All boundary coordinates use the
complete logical input sample shape, including batch axes and singleton axes.
CPU indexing uses signed 128-bit intermediates. GPU static table-byte admission
bounds radii below 2^59, permitting signed 64-bit boundary arithmetic; validated
byte addresses use modulo-2^64 offset/stride arithmetic.

## Regional execution and dirty propagation

The tiled form uses Result Dependency-v2 support and the IndependentChunks
publication policy. It computes only requested output samples and respects
atomic trailing axes and ColorArray channel tuples when choosing tile boundaries.
The first tensor Need requests Data, Validation, and Descriptor roles (mask 13);
later Needs request Data and Validation (mask 5), while descriptor support remains
independent. The producer generates its coefficients once and retains one
immutable owner across tile polls. The host's cpu_tiles service runs the granted
stage; a tile callback does not create worker threads or schedule a concurrent
window of tiles.

The compact Neighborhood relation records active radii without a per-sample
support table. For each requested sample, the Data relation is the rectangular
product of nonzero x/y taps after boundary mapping; the symmetric support is
exact for the clipped or wrapped input coordinates. Coefficients that round to
zero contribute no read or dirty edge. A separate Validation relation closes
ColorArray channel tuples and atomic trailing axes, and is united with Data
support. Descriptor evidence remains independent of sample reads. Empty output
requests publish an empty Result through a stateless continuation; they skip
coefficient generation, arithmetic scratch, and input payload access.

## Numerical execution

For each axis, symmetric coefficients are `RN64(exp(-j*j/(2*sigma*sigma)))`.
The exponent is an exact rational formed from stored parameter bits. Directed
interval refinement accepts a coefficient only when both endpoints round to the
same binary64. Exact range comparisons handle certified zero and one results.
Failure to certify within the admitted arena/refinement limit returns
ResourceExhausted. No ordinary floating exp or lower-precision fallback is used.

Coefficient zero terminates the monotone nonzero support. Zero taps are excluded
before sample reads and classification. All contributing taps retain logical
kernel row-major convolution order and boundary multiplicity. The first input NaN
wins over generated invalid arithmetic; signs and payloads are quieted according
to NUM. Opposite infinities generate a positive quiet NaN. All-negative-zero sums
preserve -0; exact finite cancellation returns +0. Boundary constants retain their
Float64 value until the final output rounding. Both radii zero copy bits,
including signaling NaN payloads and signed zero.

Finite accumulation uses 68 uint64 limbs. For each finite tap, the implementation
multiplies the two coefficient significands and the sample significand in a
compact 159-bit product, then shifts that product into the exact accumulator.
This avoids multiplying long fixed-width integers whose lower words are zero.
In units of 2^-1074, coefficient integers are at most 2^1074 and finite input
magnitudes are below 2^2098. With at most UINT64_MAX taps, the numerator is below
2^4310 and the denominator below 2^2212. Final ratio alignment and its remainder
bit fit the same workspace. The implementation has no rounded horizontal
floating intermediate.

## Ownership, work and parallelism

Static preparation validates domains and computes checked storage sizes only.
Coefficients are generated during execution using the managed callback allocator,
work budget and cancellation token. Their identity derives from the exact static
parameter bits and operation version. One Result continuation retains the
immutable coefficient owner across polls and CPU ranges; separate executions
generate their own table.
There is no hidden global coefficient cache. Each invocation-local arithmetic
slot builds the exact normalization denominator once for its coefficient table,
then reuses the denominator while resetting the numerator between samples.
Different slots do not share mutable arithmetic state.

The kernel grants Whole work to its shared CPU pool. A one-worker configuration
uses the same calculation. Each live worker slot owns its integer scratch and
writes disjoint output samples. No private worker pool is created. The synchronous
range barrier retires every active block before scratch or coefficients release.
Whole publication occurs only after successful completion and host failure
checks. Result owners retain output backing and cloned resources beyond
ExecutionContext lifetime.

Declared workspace includes the coefficient arena, up to 64 arithmetic slots and
the static radius-derived table size. Tiled workspace uses an arithmetic slot for
each active callback and reuses the continuation's coefficient owner across
successive tile polls. Each allocation retains managed capacity. The table
reservation remains conservative when outer coefficients
underflow to zero. Arena construction admits 128 bounded slot initializations and
one rounding-workspace initialization; coefficient refinement and long multiply
rows poll cancellation.

Each arithmetic block prepays a checked conservative work bound. The core sample
bound is `19827 + 204*(nx+ny) + 24140*nx*ny` for active kernel lengths; address and
read work are added separately. A local credit check covers every subsequent math
charge and keeps cancellation polling. Unused credit is not refunded. Thus finite
work limits may reject earlier than actual-limb accounting, while arithmetic
never spends unadmitted work. Authorized-window lookup separately prepays
authorization scans, fragment candidates and logical addressing, and polls
cancellation before each candidate. Capacity, indexing, cancellation and
upstream failures do not produce a successful final Result. A tiled Result may
already expose certified chunks when a later tile fails; the execution reports
failure and drains admitted stages. Empty output requests use a stateless
continuation to seal an empty Result without allocating coefficient, arithmetic,
or input payload storage.

## Native GPU execution

The GPU key has Whole demand and CompleteBundle publication. The host generates
certified baked64 coefficients under the same runtime budgets. All sample loads,
boundary mapping, weighted accumulation, special-value selection and final IEEE
rounding run in the GPU integer kernel. Metal uses MSL; Vulkan uses SPIR-V generated
from `gaussian.slang`. Float64 is represented by raw bits and exact integer
arithmetic; no hardware floating-point approximation is involved. Both paths keep
the accepted CPU numerical contract unchanged.

Each lane uses ten 136-word uint32 integers and eight state words, 5472 bytes.
The numerator and denominator satisfy the same bounds as the CPU implementation.
Products trim exact low zero words without changing their absolute limb indices.
An imported finite binary64 significand spans at most three uint32 words; two
weight significands multiply to at most five words. Full destination clearing
and retained carry order preserve exactness. Final long division uses at most
53 quotient bits and nearest-even rounding.
The first NaN and infinity/zero state persist across consecutive tap chunks.
Both radii zero retain the exact-copy rule, including signaling NaNs.

A dispatch handles at most 256 output samples on Metal and 64 on Vulkan, with at
most 16 taps per sample. Partial integer state remains in one reusable managed
buffer. Static workspace admission reserves 1,400,832 bytes for 256 lanes on both
backends; Vulkan uses 64 lanes per dispatch but retains this conservative
reservation. The runtime groups up to two ordered dispatches in one synchronous
native submission; each lane therefore handles at most 32 taps between submission
drains.
Inter-dispatch barriers preserve the exact accumulated integer state. Per-lane work
admission is 100000 units per tap, plus 4096 initialization units and 131072
finalization units when those stages occur. Constants/command construction are
charged separately. Checked static workspace includes this scratch, coefficient
arena and full radius-derived table reservation. All buffer owners remain live
through native completion; the Result publishes only after the complete invocation
succeeds.

Vulkan uses 64 output lanes per dispatch and a 512-byte std140 constants buffer.
The complete coefficient table is bound at byte offset zero; constants provide
the original x/y coefficient offsets so cropping retains its tap coordinates.
The registry key and exact arithmetic are shared with Metal. The GPU entry has no CPU fallback.
`tools/compile_builtin_vulkan.py --operator gaussian` generates the SPIR-V with
Slang 2026.18.2 and checks it with `spirv-val --target-env vulkan1.2`, including
descriptor bindings, byte-storage types, the constants layout and local size.

Cancellation is checked before each submission and after native completion.
A submitted kernel runs to completion with the declared finite work bound; no
within-kernel host polling or device preemption is promised. Deterministic native
submission tests cancel immediately after commit, require Cancelled and no later
submission, and check final managed payload release. Native service tests verify
that submitted writes complete before the cancelled call returns. Observed drain
latencies belong in platform performance reports, not in the numerical contract. ## Execution and validation

The [public workflow example](../../../examples/gaussian_workflow/README.md)
binds a Result input, assembles published Result chunks, and compares output with
a separate one-worker Whole execution. The
[independent oracle runner](../../../oracle/ops/filter/README.md) uses directed
MPFR coefficients and exact Fraction output rounding.

```sh
cmake --build build/kernel-dev --target test_gaussian_coefficients test_gaussian_exact test_gaussian_workflow test_gaussian_tiled test_gaussian_gpu -j 8
ctest --test-dir build/kernel-dev -R '^test_gaussian_(coefficients|exact|workflow|tiled|gpu)$' --output-on-failure
python3 oracle/ops/filter/check_gaussian_coefficients_runtime.py --runner build/kernel-dev/test_gaussian_coefficients
python3 oracle/ops/filter/check_gaussian_runtime.py --runner build/kernel-dev/test_gaussian_workflow
python3 oracle/ops/filter/check_gaussian_runtime.py --runner build/kernel-dev/test_gaussian_workflow --tiled
python3 oracle/ops/filter/check_gaussian_runtime.py --runner build/kernel-dev/test_gaussian_gpu
```

The five focused Gaussian Result tests cover coefficient range/refinement,
arithmetic, Whole workflow, tiled support, and native GPU execution.

Typed Result coverage checks retained CMYK ColorArray v1 facets and ICC
resources, batch axes, a downstream image split, tuple closure, ROI support,
outside-batch NaN handling, Empty output without payload allocation, and Result
access after context retirement. These fixtures are finite evidence, not an
exhaustive all-input proof.
