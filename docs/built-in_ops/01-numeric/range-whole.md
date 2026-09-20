# NUM-06 Whole execution and measurements

The six formal `numeric.clamp_*` and `numeric.remap_range_*` profile keys now
use synchronous Whole callbacks. Unsuffixed legacy clamp is unchanged. The public
`ranges.cpp` workflow directly authors formal keys and composes them with the
existing `broadcast_node` helper; there is no separate range-specific node helper.
Output identity remains `values`, with the input dtype/shape and empty facets.

For every nonempty request, the executor collects and validates all 3/5 complete
inputs, invokes the callback once and projects its complete packed result. Any
input edit invalidates all observed outputs. Invalid bounds anywhere fail the
invocation with `InvalidArgument/InvalidDomain`, Domain origin and Run scope,
without an Atom key; diagnostics retain bound port/bits and global coordinate.
Empty reads no payload and invokes no callback. Input NaN never suppresses
invalid bounds; endpoint and constant-target rules retain all upstream inputs.

The mathematical kernel is unchanged: raw integer/IEEE comparison and bit
selection for clamp; exact dyadic numerator/denominator construction for remap,
with the existing accelerated enclosure gate and strict fallback. No NUM-14
certificate, narrowed Float64 path or weaker approximation is introduced.
Scalar/NEON/AVX2 numerical facilities and NUM-14 Scalar/Accelerate/SME targets
remain available. Per-value numeric counters are N/A on Whole callbacks.

Payload includes complete collected inputs, N × dtype-width output and one
fixed RangeMath workspace. Sparse requests may need substantially more memory
and work. Scratch/output admission and cancellation use the worker resource
scope. All unpublished owners are released on failure. Legal negative/zero
strides, unaligned addresses, nonzero origins and singleton strides are handled
by packed-address detection or checked logical reads.

## Executed validation

Local Apple M5 arm64, macOS 27.0 (26A5425a), Clang 21.1.3, RelWithDebInfo,
no fast math and disabled FP contraction for numerical code, 2026-09-21:

- Independent raw-bit/Fraction oracle: 2,826 cases for strict and 2,826 for Apple.
  The oracle uses each profile's specified exact or final FP32-scaled bound.
- Both public manual suites: broadcast → remap → clamp composition;
  invalid bounds outside sparse projection and priority over input NaN;
  Whole source support/dirty; Empty; required endpoint upstream failure;
  typed-invalid alpha; warm-cache bound edits; negative subnormal underflow/fenv;
  direct partial-output rejection and correct public global projection.
- Direct clamp/remap fixtures: all 3/5 inputs, 65-element Float32 tail,
  arbitrary singleton stride, unaligned shifted origin and reversed strides.
  Int64 clamp additionally uses zero/negative/unaligned layouts.
- Work, output and scratch capacity failures plus cancellation after arithmetic
  began release Payload. The watcher does not identify an exact inner-refinement
  interruption instruction. Escaped output storage is retained by public values.
- Five focused CTests passed: numeric operations, dependency sampling, execution
  demand, resources and compiler. Modified C++ passed ClangFormat 21/cpplint and
  scoped independent code/spec review.

No current x86/WSL execution, installed-consumer check, full suite or release
matrix was run. Historical pre-Whole platform checks are not current evidence.

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric \
  --target photospider_numeric_ranges -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_ranges _strict
python3 examples/numeric_workflow/range_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_ranges _strict
```

Repeat with `_accelerated_apple_silicon`. The editable public workflow returns
`mapped=[0,127.5,255,510]`, `clipped=[0,127.5,255,255]`.

## Timings and observed bottleneck

Float64 `[N]`, `x[i]=(i%16)/8`, source bounds `[0,1]`, remap target `[-2,2]`.
Clamp expects `min(x,1)` and remap expects `-2+4*x`; these dyadic results are
checked independently at every element after every measured execution.
One worker, cache off, 1 GiB public Payload limit, 2^40 dependency/run/Footprint
work limits and 512 MiB dependency-state limit; callback uses default managed
ResourceLimits. One warm-up and seven measured samples. Generation, compilation,
freeze and verification are outside public timing. Core times include prebuilt
input-word loads, arithmetic, output stores and a work counter, excluding public
allocation, collection and resource admission. Core N=1 is an exact endpoint at
clock resolution and does not measure ordinary rational arithmetic.

Before objects were compiled from `a762c41e` and linked against the same kernel;
after uses the current library. Milliseconds below are medians; raw files retain
all reported min/max ranges. Input generation and correctness checks are identical.

| Operation | N | Dependency public | Whole public | Numerical core |
|---|---:|---:|---:|---:|
| clamp | 1 | 0.097416 | 0.045417 | resolution limited |
| clamp | 16 | 0.123834 | 0.046042 | 0.000250 |
| clamp | 64 | 0.122750 | 0.043458 | 0.000958 |
| clamp | 256 | 0.183459 | 0.059875 | 0.003833 |
| clamp | 16384 | 5.007670 | 0.541291 | 0.245917 |
| remap | 1 | 0.128916 | 0.062083 | resolution limited |
| remap | 16 | 0.137334 | 0.065958 | 0.009417 |
| remap | 64 | 0.190000 | 0.096625 | 0.036750 |
| remap | 256 | 0.450833 | 0.223291 | 0.148417 |
| remap | 16384 | 16.151900 | 10.167300 | 9.518500 |

At N=16,384 Whole public ranges were 0.528–0.612833 ms for clamp and
10.1415–10.4244 ms for remap. Controlled Payload peaks changed from 134,208 bytes
to 527,416 / 789,560 bytes respectively. These figures are not RSS. No 4096² or
cross-platform timing claim is made.

A 12-second Instruments Time Profiler run of continuous Whole remap at N=16,384
produced 12,002 execution-chain samples: callback 98.58%, RangeMath evaluation
96.01%, RatioWorkspace 84.41%, collect 0.31% and ResourceBudget::consume 2.38%.
Inclusive percentages overlap. Dominant leaves were fixed-integer subtract
(3,839 samples) and set_product (3,103), followed by exact-ratio compare (785).
This identifies the remaining exact rational arithmetic cost. No inference from
matrix backend behavior or replacement numerical certificate is used.
Profiler output was checked on the first iteration; timings above came from
separate completed runs with full verification.

Local ignored artifacts: `build/range-whole/{scale.cpp,core.cpp,build_scale.py,
timings.csv,after.trace,after.xml,after-samples.json,after-summary.txt}`,
`oracle-{strict,apple}.log`, `ctest.log`. The existing public category benchmark
also provides smaller reproducible fixtures:

```sh
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple extended clamp
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple selected remap_range
```
