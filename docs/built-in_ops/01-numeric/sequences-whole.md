# NUM-02 Whole execution

The public `linspace_node` and `arange_node` helpers select six formal profile
keys in `numeric_sequences.cpp`. Both outputs now use CPU Whole callbacks with
bounded `SequenceMath` workspace. The exact dyadic arithmetic, direct Float32
rounding, Int64 range checks and Scalar/NEON/AVX2 limb engines are preserved.
NUM-14's finite-Float32 four-term certificate is not used.

The selected values output computes all count elements; axis independently
computes its three-component tuple. Static count=1 excludes end/step entirely
from runtime input collection; otherwise both scalar inputs are required. A
numeric failure anywhere in values fails that Whole output, including for a
single-element consumer. Active input changes invalidate the full selected
output. Empty performs no callback. Output owner size is count*4/8 bytes or
24 axis bytes, plus 1112 bytes of arithmetic workspace on this ARM64 build.
Per-atom numeric diagnostics are unavailable for Whole.

## Runnable public checks

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric --target photospider_numeric_sequences -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_sequences strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_sequences apple_silicon
python3 examples/numeric_workflow/sequence_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_sequences strict
python3 examples/numeric_workflow/sequence_oracle.py build/clang21-numeric/examples/numeric_workflow/photospider_numeric_sequences apple_silicon
```

Both native profiles passed the public workflows and 960 independent
Fraction/IEEE cases per profile. Checks include values `[0,.25,.5,.75,1]`, axis
`[0,1,.25]`, Int64 above 2^53, extreme cancellation, direct Float32 midpoint
rounding, signed zero, count=1 failing-source exclusion, count>1 eager failure,
axis-only overflow, tuple projection, unaligned negative-stride scalar layout,
fenv restoration, cache invalidation, escaped owners, pre-cancel and cancellation
after admitted arithmetic. Independent work/output/scratch limits reject and
release all unpublished storage. The oracle accounts for whole-array overflow
using exact affine extrema. No x86 runtime result is claimed for this change.

Focused `test_numeric_operations`, `test_dependency_sampling`,
`test_execution_demand`, `test_resources` and `test_compiler` passed.
ClangFormat 21 and cpplint passed for changed C++.

## Measured performance

Local Apple M5, macOS 27.0 (26A5425a), Clang 21.1.3, RelWithDebInfo/O2,
`-fno-fast-math -ffp-contract=off`, native Apple Silicon profile, Float64.
Kernel package 0.17/traits15; before uses the unchanged sequence adapter from
8996f526 linked against the same current kernel, after uses Whole. This isolates
the adapter; it is not a comparison of two full historic kernel builds.

Public timing starts after graph compilation, registry creation and input
freeze. One CPU worker, result/dependency caches off, 1 GiB payload limit,
2^40 dependency work and 512 MiB dependency-state limit. Each row has one warmup
and seven samples; every output is checked against exact dyadic `i/8` outside
timing. Core timing calls the actual `SequenceMath::rounded` without execution,
allocation or collection. Times are milliseconds, median [min,max].

| Operation/count | Before public | Whole public | Numerical core |
| --- | --- | --- | --- |
| linspace/1 | .0803 [.0734,.1078] | .0381 [.0338,.0605] | .000375 [.000333,.000583] |
| linspace/256 | 67.81 [65.49,72.96] | .1998 [.1938,.2165] | .1546 [.1537,.1853] |
| linspace/16384 | failed before obtaining samples | 11.039 [10.493,11.059] | 10.643 [10.626,10.679] |
| arange/1 | .0806 [.0723,.0975] | .0343 [.0329,.0463] | .000375 [.000333,.000459] |
| arange/256 | 65.19 [63.85,66.14] | .2070 [.2038,.2457] | .1546 [.1533,.1580] |
| arange/16384 | not sampled | 10.553 [10.520,11.046] | 10.021 [9.925,10.088] |

N=16/64 and complete ranges are in the raw CSV. Full-request peak owned payload
at N=256 changes from 3160 to 3168 bytes; Whole N=16384 is 132192 bytes.
Small consumer requests now own full output capacity even when returned coverage
is smaller. Initial timings overlapped an independent oracle process and are
local observations, not statistically isolated hardware comparisons.

A 12-second Instruments Time Profiler capture of Whole linspace N=16384 contains
11994 execution-chain CPU samples: 9869 (82.28%) include
`ExactSequence::rounded_bits`, 831 (6.93%) include `sequence_multiply`, and only
3 (0.025%) include collect. Leaf samples are 6145 in rounded_bits and 3724 in
its bit accessor. These observed stacks identify exact rounding as the remaining
hot path; inclusive percentages overlap and inline callback names are incomplete.
No claim assigns all memmove samples to collection.

Raw local files: `build/num-whole-remaining/timings.csv`, `scale.cpp`, `core.cpp`,
`build_scale.py`, `sequences.trace`, `sequences.xml`, `sequences-samples.json`,
`profile-summary.txt`, and strict/Apple oracle logs. Drivers use public workflow
execution for latency and a separately checked numerical core for arithmetic.
