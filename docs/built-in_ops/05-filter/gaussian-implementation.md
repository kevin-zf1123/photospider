# Finite Gaussian runtime

The default registry provides `filter.gaussian_baked64_v1_strict_cpu_whole`,
`filter.gaussian_baked64_v1_strict_cpu_tiled` and
`filter.gaussian_baked64_v1_strict_gpu`.
It evaluates FIL-04B's baked64 coefficient profile with one final rounding of the
complete normalized two-dimensional expression. FIL-04 remains Proposed; runtime
registration does not change that specification status. The GPU form uses the
native backend selected by the kernel build: Metal runs MSL and Vulkan runs SPIR-V.
Unavailable backends fail explicitly without CPU fallback. The Vulkan workflow has
passed on FreeBSD Intel UHD 770; the Gaussian Vulkan port remains untested on NVIDIA
and Linux.

## Ports and explicit parameters

Input `input` and output `output` have the same Float32/Float64 dtype and logical
shape. Rank is 2..8, extents are positive and the total element count is at most
2^40. All parameters below are mandatory static values; there are no defaults.

| Parameter | Type | Domain |
| --- | --- | --- |
| `sigma_x`, `sigma_y` | Float64 | Finite, nonnegative |
| `radius_x`, `radius_y` | Int64 | Nonnegative; a zero sigma requires zero radius |
| `x_axis`, `y_axis` | Int64 | Distinct nonnegative axes smaller than rank |
| `boundary` | String | `constant`, `clamp`, `wrap`, `reflect_half`, `reflect_whole` |
| `cval` | Float64 | Finite, including when the input is Float32 |

Other axes are independent planes. Channel counts do not trigger color or alpha
arithmetic. Structurally valid input facets and their resource bindings are
preserved. CPU Whole publishes a dense generic Value. Input producers may supply
validated negative/broadcast strides, unaligned offsets and logical origins;
workflow input bindings retain the kernel's dense external-binding requirements.

Whole demands the complete input and computes the complete output. Any input edit
invalidates that output group. It does not expose sparse Regional evaluation.
All boundary coordinates use the complete logical input shape, including singleton
axes. CPU indexing uses signed 128-bit intermediates. GPU static table-byte
admission bounds radii below 2^59, permitting signed 64-bit boundary arithmetic;
validated byte addresses use modulo-2^64 offset/stride arithmetic.

## Regional execution and dirty propagation

The tiled form computes only requested output observations, including closure of
atomic trailing axes and structural groups. Its first poll generates the baked
coefficients under runtime budgets, then declares per-observation Data support.
Its second poll reads authorized fragments and computes the requested samples.
It never computes a Whole image for subsequent slicing. Each CPU tile callback
is single-threaded; streamed execution schedules independent tiles on the shared
host pool. Collected execution computes bounded tiles serially.

Support is the Cartesian product of the active nonzero taps after boundary
mapping. Constant, clamp and reflection modes have clipped interval unions;
wrap can produce two intervals per axis. Other axes preserve their requested
sample ranges. Coefficients that round to zero contribute no read or dirty edge.
Retained association rows expose exact support and its transpose. Descriptor
evidence remains independent of payload reads. Empty output requests validate
static metadata without allocating coefficient or arithmetic payload.

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

Finite accumulation uses 68 uint64 limbs. In units of 2^-1074, coefficient
integers are at most 2^1074 and finite input magnitudes are below 2^2098.
With at most UINT64_MAX taps, the numerator is below 2^4310 and the denominator
below 2^2212. Final ratio alignment and its remainder bit fit the same workspace.
The implementation has no rounded horizontal floating intermediate.

## Ownership, work and parallelism

Static preparation validates domains and computes checked storage sizes only.
Coefficients are generated during execution using the managed callback allocator,
work budget and cancellation token. Their identity derives from the exact static
parameter bits and operation version. All host ranges in one invocation share
one immutable coefficient owner; separate invocations generate their own table.
There is no hidden global coefficient cache.

The kernel grants Whole work to its shared CPU pool. A one-worker configuration
uses the same calculation. Each live worker slot owns its integer scratch and
writes disjoint output samples. No private worker pool is created. The synchronous
range barrier retires every active block before scratch or coefficients release.
Publication occurs only after successful completion and host failure checks.
Returned Values retain their storage beyond ExecutionContext lifetime.

Declared workspace includes the coefficient arena, up to 64 arithmetic slots and
the static radius-derived table size. Tiled workspace uses one arithmetic slot
per callback and one coefficient owner per tile session. Each actual allocation retains managed
capacity. The table reservation remains conservative when outer coefficients
underflow to zero. Arena construction admits 128 bounded slot initializations and
one rounding-workspace initialization; coefficient refinement and long multiply
rows poll cancellation.

Each arithmetic block prepays a checked conservative work bound. The core sample
bound is `19827 + 204*(nx+ny) + 24140*nx*ny` for active kernel lengths; address and
read work are added separately. A local credit check covers every subsequent math
charge and keeps cancellation polling. Unused credit is not refunded. Thus finite
work limits may reject earlier than actual-limb accounting, while arithmetic
never spends unadmitted work. Bounded fragment lookup separately prepays authorization scans, fragment
candidates and logical addressing, and polls cancellation before each candidate.
Capacity, indexing, cancellation and upstream failures publish no partial Value.
A streaming sink may already have received earlier complete tiles before a later
tile fails; the Run then reports failure and drains active stages.

## Native GPU execution

The GPU key has Whole demand and transactional publication. The host generates
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

A dispatch handles at most 64 output samples and 16 taps per sample. Partial
integer state remains in one reusable managed buffer, at most 350208 bytes. The
runtime currently groups up to two ordered dispatches in one synchronous native
submission; each lane therefore handles at most 32 taps between submission drains.
Inter-dispatch barriers preserve the exact accumulated integer state. Per-lane work
admission is 100000 units per tap, plus 4096 initialization units and 131072
finalization units when those stages occur. Constants/command construction are
charged separately. Checked static workspace includes this scratch, coefficient
arena and full radius-derived table reservation. All buffer owners remain live
through native completion; only the final successful invocation publishes output.

Vulkan uses a 512-byte std140 constants buffer. The complete coefficient table is
bound at byte offset zero; constants provide the original x/y coefficient offsets
so cropping retains its tap coordinates. The registry key and exact arithmetic are
shared with Metal. Native Vulkan verification is currently limited to Intel UHD
770 on FreeBSD; NVIDIA and Linux remain untested. The current submission policy
groups two ordered dispatches at most, with a maximum of 32 taps per lane per
submission. Measured timing evidence and its platform limits are recorded below.
`tools/compile_builtin_vulkan.py --operator gaussian` generates the SPIR-V with
Slang 2026.18.2 and checks it with `spirv-val --target-env vulkan1.2`, including
descriptor bindings, byte-storage types, the constants layout and local size.

Cancellation is checked before each submission and after native completion.
A submitted kernel runs to completion with the declared finite work bound; no
within-kernel host polling or device preemption is promised. Deterministic native
submission tests cancel immediately after commit, require Cancelled and no later
submission, and check final managed payload release. Native service tests verify
that submitted writes complete before the cancelled call returns. Observed drain
latencies belong in platform performance reports, not in the numerical contract.
The focused cancellation fixtures measured 9095.35 us from cancel to return on
FreeBSD Intel Vulkan and 2028.5 us on macOS Metal. Each is one observation, not a
worst-case bound or latency guarantee. See the [Intel log](../../../out/gpu-whole-tiled/raw/gaussian-cohort-intel-final.log)
and [Metal log](../../../out/gpu-whole-tiled/raw/gaussian-cohort-metal-final.log).

## Dispatch-cohort timing evidence

The current cohort setting is two ordered dispatches per submission. Each
dispatch covers at most 16 taps per lane, so grouping lowers the submission count
by half when pairs are available. Outputs remained bitwise equal to the separately
executed Whole reference in the paired benchmark runs.

On FreeBSD Intel UHD 770, static cohort-1 and cohort-2 binaries ran in alternating
order for three rounds, with one warmup and eleven measured calls per size and
configuration. The public example leaves result caching disabled by default
(`result_cache_bytes=0`, no disk cache); each timed call still compares its result
with the Whole reference. Values below are the median of the three per-round
medians in milliseconds.

| Intel UHD 770 / FreeBSD | Cohort 1 | Cohort 2 | Change |
| --- | ---: | ---: | ---: |
| 8 × 8 | 15.746 | 15.2533 | −3.13% |
| 16 × 16 | 52.6219 | 49.7157 | −5.52% |
| 32 × 32 | 201.362 | 189.077 | −6.10% |

The 8 × 8 p95 values varied across rounds and do not establish a stable small-case
gain. At 32 × 32, cohort-1 p95 values of 212.868, 200.18 and 211.659 ms changed to
198.762, 198.904 and 198.904 ms. The cohort-2 `peak_host_bytes` was 1080 bytes
higher at every tested size. See [the paired Intel raw results](../../../out/gpu-whole-tiled/raw/gaussian-vulkan-cohort-paired.jsonl).

One macOS M5 before/after set recorded these medians:

| Apple M5 / macOS | Cohort 1 | Cohort 2 | Change |
| --- | ---: | ---: | ---: |
| 8 × 8 | 5.15671 | 3.24475 | −37.08% |
| 16 × 16 | 13.9919 | 11.7485 | −16.03% |
| 32 × 32 | 52.8873 | 46.3666 | −12.33% |

This single M5 before/after set does not establish a general gain across runs.
`peak_host_bytes` was unchanged on M5. Raw results are [cohort 1](../../../out/gpu-whole-tiled/raw/gaussian-metal-submit-before.jsonl)
and [cohort 2](../../../out/gpu-whole-tiled/raw/gaussian-metal-submit-after.jsonl).
Performance measurements ran with the Vulkan validation layer disabled; correctness
validation ran separately in [the Intel validation log](../../../out/gpu-whole-tiled/raw/gaussian-vulkan-oracle-validation.log).

## Execution and validation

The [public workflow example](../../../examples/gaussian_workflow/README.md)
constructs inputs, compiles a graph and compares multi-worker output with a
separate one-worker execution. The
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

Tests cover coefficient range/refinement, construction and arithmetic cancellation,
work/capacity failure, all boundary modes, exceptional values, zero-support poison,
independent planes, multiple host ranges and nontrivial producer storage layouts.
Regional tests additionally check exact producer reads, interior/edge ROI,
per-observation dependency rows and dirty transpose against independent tap
enumeration. GPU tests require actual native dispatches without fallback and
compare the same independent oracle, including special values across tap chunks.
The FreeBSD Intel Vulkan run compares 94 workflows and 707 output bit patterns
against DirectedMPFR coefficients plus Fraction and IEEE rounding; the focused
Vulkan test and Mac Metal focused test pass. These fixtures are finite evidence,
not an exhaustive all-input proof or validation of NVIDIA/Linux GPU support.
Performance effects of batching two dispatches are still under measurement.
Performance reports and raw platform evidence are kept in untracked
`out/gpu-whole-tiled/`; the source example defines reproducible timing boundaries.
