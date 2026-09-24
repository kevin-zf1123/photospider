# NUM/CRV batch execution update, 2026-09-24

NUM-04/05 accelerated ln, sin, cos, tan, pow and atan2 now gather up to 64
Float32/Float64 operands and call the existing binary64 SLEEF adapter once per
block. The original bit-level special cases, exact power/angle landmarks,
per-result enclosure and strict refinement remain in CertifiedMath. Rejected
lanes are sanitized before SIMD and retain their original bits for fallback.
The fixed workspace is 2,624 bytes, admitted together with CertifiedMath;
indexing and arithmetic admission are charged before batch arithmetic. Exact
work totals match the scalar path. Cancellation is checked at each batch and
within scalar refinement, and no partial output is published.

All NUM-04/05 certified point calculations now borrow one Whole callback
nearest/gradual-underflow environment, including reduced pi/rational-pi
calculations. CRV-10 inverse curves and CRV-11 uniform lowpass similarly borrow
one environment over the complete output loop. Independent scalar calls still
own a guard. These CRV changes amortize setup; they do not vectorize inverse
bisection or reorder lowpass taps. NUM-01 already batches AST evaluation and
forward CRV interpolation already borrows a Whole environment. Exact integer,
reduction, scan and nonuniform-integration arithmetic is unchanged by this
commit; a mechanical batch wrapper would not establish an arithmetic speedup.

## Measurements

Same hosts and build settings as [the exp report](exp-performance.md): Apple M5,
macOS 27.2, Clang 21.1.3; Intel i9-12900, Ubuntu WSL, Clang 18.1.3, AVX2/FMA.
The private SIMD adapter uses -O3 and explicit FMA; fast-math stays disabled.

The manual `photospider_numeric_math_batch time` driver uses 262,144 deterministic
operands in [0.125,0.875], seed 405, and binary second operand 0.3125. It runs one
warmup plus seven measured iterations, verifies every measured output outside
timing, and reports medians. Linux is pinned to CPU 2. There is no result cache,
worker pool or profiler in this direct measurement. The scalar column measures
only CertifiedMath calls, with per-call environment setup. The batch column
measures the complete direct callback including allocation, gather and store.
These are different boundaries, not public workflow A/B timings. No CRV latency
or new hardware-counter speedup is claimed here.

Initial Float32 medians, microseconds (before moving equivalent arithmetic
admission to the start of each batch):

| Operation | M5 scalar math | M5 batch callback | i9 scalar math | i9 batch callback |
| --- | ---: | ---: | ---: | ---: |
| ln | 21044.8 | 8300.88 | 54762.7 | 16739.5 |
| sin | 20324.5 | 8613.46 | 56070.3 | 16868.2 |
| cos | 20856.6 | 8157.75 | 55737.5 | 17346.9 |
| tan | 20903.5 | 8468.04 | 57461.9 | 17425.2 |
| pow | 27921.0 | 10768.2 | 64665.9 | 19639.0 |
| atan2 | 23565.4 | 9242.92 | 58397.7 | 18413.8 |

Float64 batch medians were 7.91–10.72 ms on M5 and 15.53–19.00 ms on i9.
Fixed backend order and a single run per host limit small-difference claims.
Managed Payload is checked separately; RSS is not measured in this update.

## Validation and reproduction

On both platforms, independent public workflow oracles passed 7,524 unary,
14,174 binary, 407 inverse and 474 uniform-lowpass cases. The lowpass execution
fixture covered ten families, negative/unaligned/zero strides, four host rounding
modes, Whole work/output/workspace limits, active cancellation and release.
`test_numeric_operations` and `test_expression_operations` passed.

The new batch fixture compares scalar and batch bits for both dtypes, mixed
special/ordinary inputs, chunks 1/3/7/64/65/257, unaligned/negative/zero strides,
all four rounding modes and preexisting exception flags. It checks exact work
admission, a one-unit deficit, actual managed output/workspace allocation, a
one-byte capacity deficit, and release after success/failure. This directly
covers batch mechanics; independent public oracles establish numerical bounds.

```sh
cmake -S . -B build
cmake --build build --target photospider_numeric_math_batch \
  photospider_numeric_unary photospider_numeric_binary \
  photospider_numeric_inverse photospider_numeric_lowpass \
  photospider_numeric_lowpass_execution -j 8
build/examples/numeric_workflow/photospider_numeric_math_batch
build/examples/numeric_workflow/photospider_numeric_math_batch time
python3 examples/numeric_workflow/unary_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_unary apple
python3 examples/numeric_workflow/binary_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_binary apple
python3 examples/numeric_workflow/inverse_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_inverse apple
python3 examples/numeric_workflow/lowpass_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_lowpass apple
build/examples/numeric_workflow/photospider_numeric_lowpass_execution apple
```

Use `x86` on WSL. Local raw logs are ignored under `build/num04-exp/batch-*`;
remote logs are under `/home/alex/photospider-num04-exp-20260924/batch-*`.
No full release, sanitizer or installed-consumer matrix was run.
