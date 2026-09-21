# NUM-04/05 Whole execution

Measured on 2026-09-21, based on `numeric-optimize` at `3ae7c068`.
The public unary/binary helpers author 66/27 formal profile keys respectively;
all retain the `values` output. Legacy unsuffixed operations are separate.

The shared adapter now registers Whole, dense output and one fixed arithmetic
workspace, with no dependency version, continuation, stages or static Need maps.
The execution path is helper → Compiler metadata specialization → full input
collect and typed validation → synchronous callback → full packed output → consumer
projection. Empty bypasses payload and callback. Any input change invalidates all
observed values. Integer overflow or a nonpositive rational denominator anywhere
fails the whole invocation with Run scope and no Atom key; partial output is not
published. Output descriptors, mathematical selections, NaN priority and profile
accuracy contracts remain unchanged. No NUM-14-specific certificate is used.

Capacity includes complete collected inputs, N × destination-width output bytes
and the fixed ExactElementary or CertifiedMath workspace. Sparse consumers pay
these full costs. Strides, offsets, storage origins, singleton axes and unaligned
words are honored; owned results survive their context. Callback work uses the
worker resource scope and cancellation token, including refinement and final
publication checks. Old per-value NumericDiagnostics are N/A, not zero work.
Elementary operations lend a callback-local floating-environment guard to the
arithmetic engine; standalone users still establish their own guard. Both paths
restore caller rounding, denormal controls and exception flags.

## Validation

Clang 21.1.3, Apple M5 arm64, macOS 27.0 (26A5425a), RelWithDebInfo `-O2 -g`,
no fast math and FP contraction disabled for numerical code. MPFR 4.2.2 through
native `/opt/homebrew/bin/python3.11` supplied the independent directed reference.

- Unary: 7,524 integer/exact/Fraction/MPFR cases for each of strict and Apple.
- Binary: 14,174 integer/Fraction/MPFR cases for each of strict and Apple.
- Both public manual suites passed both profiles: Whole support/dirty, errors
  outside consumer projections, rational denominator errors, Empty, typed-invalid
  channels, required upstream producers, cache mutation and escaped ownership.
- Direct fixtures passed negative/zero strides, unaligned shifted origins,
  arbitrary singleton strides, 65-element Float32 tails and four fenv modes with
  preexisting flags. Work, output/scratch capacity rejection and cancellation
  after arithmetic began released all unpublished Payload. Cancellation sampling
  does not locate the interruption within an individual transcendental refinement.
- Focused CTest: `test_numeric_operations`, `test_dependency_sampling`,
  `test_execution_demand`, `test_resources`, `test_compiler`, all five passed.
- Modified C++ passed ClangFormat 21 and cpplint; scoped independent code/spec
  review covered Whole registration, lifetimes, fenv borrowing and document sync.

The x86 profile retains the same registration and shared code but was not executed
on this arm64 host. Existing NUM-14 Scalar/Accelerate/SME implementations and
comparison targets remain available; these measurements do not rank those matrix
backends or introduce a point-math SME/Accelerate implementation.

Public entry points and independently checkable fixtures:

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric \
  --target photospider_numeric_unary photospider_numeric_binary -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_unary apple
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_binary apple
/opt/homebrew/bin/python3.11 examples/numeric_workflow/unary_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_unary apple
/opt/homebrew/bin/python3.11 examples/numeric_workflow/binary_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_binary apple
```

Repeat with `strict`. The editable binary composition `(a+b)*b` for
`a=[1,2,3], b=[2,2,2]` returns `[6,8,10]`. Rational unary `sinpi(p/q)` for
`p=[0,1,1], q=[1,6,2]` returns `[0,0.5,1]`.

## Public and core measurements

All measurements used one worker, cache off, one warm-up and seven measured
repetitions. Input generation, compilation, freeze and output verification were
outside timed execution. Every output was checked in completed timing runs.
Core measurements exclude public dispatch, collect, allocation and managed work
admission. They include a reusable arithmetic workspace, input loads, output
stores and a work-count callback. N=1 core measurements reach clock resolution;
no speed claim is made there.

The scale fixture uses Float64 `[N]`, `x[i]=(i%16)+1`, `abs(x)` and `add(x,x)`;
the independent analytic expectations are `x[i]` and `2*x[i]`. Public payload
limit is 1 GiB; dependency/run work limits are 2^40, state limit 512 MiB and
Footprint work limit 2^40. The callback has the default managed ResourceLimits.
The before adapter was compiled from `3ae7c068` into separate objects and linked
against the same current kernel; it does not change the working tree or branch.
Times below are milliseconds, median of seven samples.

| Operation | N | Dependency public | Whole public | Whole core | Whole public min–max |
|---|---:|---:|---:|---:|---:|
| abs | 1 | 0.071708 | 0.034459 | resolution limited | 0.029625–0.069000 |
| abs | 16 | 0.078583 | 0.036417 | 0.000167 | 0.029959–0.046375 |
| abs | 64 | 0.116000 | 0.038083 | 0.000708 | 0.034417–0.058958 |
| abs | 256 | 0.198125 | 0.052167 | 0.002542 | 0.048334–0.067917 |
| abs | 16384 | 6.589210 | 0.470750 | 0.159292 | 0.461417–0.482292 |
| add | 1 | 0.096500 | 0.047333 | resolution limited | 0.038375–0.142583 |
| add | 16 | 0.096958 | 0.041292 | 0.000250 | 0.036458–0.063458 |
| add | 64 | 0.125292 | 0.043125 | 0.000708 | 0.039417–0.070875 |
| add | 256 | 0.215042 | 0.059292 | 0.002708 | 0.050541–0.079792 |
| add | 16384 | 7.614630 | 0.499208 | 0.174792 | 0.482917–0.531625 |

At N=16,384, controlled peak Payload changed from 133,928 bytes to 264,936 for
abs and 396,008 for add. This is not RSS and does not imply memory savings.

The public family benchmarks additionally sampled all 31 functions at N=1/256,
Float64, 1 MiB payload, dependency/run work limits 2^30/2^31, cache off, one worker,
one warm-up/seven samples. Selected N=256 medians in microseconds:

| Function | Dependency public | Whole public | Numerical core |
|---|---:|---:|---:|
| exp | 194 | 61 | 18 |
| sin | 192 | 63 | 20 |
| pow | 232 | 72 | 27 |
| atan2 | 248 | 70 | 22 |

The family fixture uses constant inputs; the kernel benchmark uses documented
per-function ordinary arguments and a checksum. Core/public rows are separate
layers, not a subtraction-derived overhead estimate. No 4096², strict timing,
RSS, or cross-platform performance claim is made for this cluster.

## Profiler evidence and local raw files

Three 12-second Instruments Time Profiler recordings ran `add public 16384` on a
continuous cache-off loop. Profiling verified outputs on the first iteration;
reported timings above come from separate completed, fully checked runs.
Inclusive percentages below overlap and must not be summed.

- Dependency: 11,970 execution-chain samples; 99.48% included DependencySession,
  98.91% Footprint traversal and 8.81% ExactElementary evaluation.
- Initial Whole: 11,910 execution-chain samples; callback 92.70%, arithmetic
  75.54%, collect 0.55%. `fegetenv` and `fesetenv` accounted for 6,824 leaf samples.
  This directly motivated moving elementary fenv protection outside the loop.
- Final Whole: 11,833 execution-chain samples; callback 90.53%, arithmetic 46.56%,
  ResourceBudget::consume 22.84%, collect 1.66%. Neither fenv function appeared
  in sampled execution stacks. Mutex and memcpy leaf samples remain; their
  individual call-site attribution was not established, so no cause is inferred.

Local diagnostic files are under `build/point-math-profile/`: `scale.cpp`,
`build_scale.py`, `scale-timings.csv`, `final-timings.csv`,
`{unary,binary}-{before,final}.csv`, `transcendental-core.csv`,
`{before,after,final}.trace`, corresponding XML/sample JSON/summary files,
four oracle logs and `ctest-final.log`. They are ignored local artifacts.
Public reproducible family/core measurement commands:

```sh
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_unary apple benchmark
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_binary apple benchmark
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_math_benchmark apple
```
