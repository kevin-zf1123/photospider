The subsequent
[trigonometric update](trig-performance.md) replaces eligible Float32 sin/cos
and pi/sinc calculations with certified polynomial kernels.

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

The lowpass execution
fixture covered ten families, negative/unaligned/zero strides, four host rounding
modes, Whole work/output/workspace limits, active cancellation and release.

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
python3 oracle/ops/numeric/unary_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_unary apple
python3 oracle/ops/numeric/binary_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_binary apple
python3 oracle/ops/numeric/inverse_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_inverse apple
python3 oracle/ops/numeric/lowpass_oracle.py \
  build/examples/numeric_workflow/photospider_numeric_lowpass apple
build/examples/numeric_workflow/photospider_numeric_lowpass_execution apple
```

Use `x86` on WSL.
