# NUM-07 Whole execution

All 24 formal NUM-07 profile keys use synchronous Whole Result execution: six
ordinary predicates, `is_close`, and `select`, each with strict, Apple and x86
profiles. Each input is a single-tensor Result in slot 0, and all inputs must
have the same complete `sample_shape()`, including batch axes. Each predicate
compares inputs of the same dtype. The six ordinary predicates accept UInt8,
Int64, Float32 and Float64; `is_close` accepts Float32 and Float64 and checks
finite, nonnegative Float64 tolerances even when the runtime footprint is
Empty. For `select`, the condition is UInt8 and the two branches share one
dtype.

The output is a `photospider.tensor` v1 Result with tensor key `samples`, full
`sample_shape()` and no input facets or batch-axis metadata. Predicates return
UInt8 0/1; `select` retains the branch dtype and copies selected bits. The Whole
program requests every active input with Data, Validation and Descriptor roles
(13), then reads through authorized Result windows. It does not collect or copy
complete input payloads. The comparison kernel handles up to four samples per
batch; it uses the admitted `ComparisonMath` workspace and the shared Whole
program's bounded phase scratch.

The `ResultRelation` records each input's descriptor and full-domain Data
support. An input edit can therefore dirty every observed output sample, while
the requested footprint still scopes the dependency query. A sparse consumer
does not turn the published Result into a packed ROI: Whole execution publishes
the complete output in global coordinates. Published Result owners survive
context destruction; uncommitted output is released on error or cancellation.

`select` eagerly requires and typed-validates both full branches, even when a
condition selects only one. Source or typed failures can precede condition
evaluation. A condition byte other than 0 or 1 fails with
InvalidArgument/InvalidDomain/Run and a diagnostic containing the byte and
global linear index. Selected bits, including signaling NaN and signed zero,
are copied without arithmetic quieting. Predicate IEEE/integer rules and exact
binary-rational `is_close` threshold arithmetic remain unchanged. Scalar/NEON/AVX2
comparison facilities remain; no NUM-14 certificate is used, and existing
matrix Scalar/Accelerate/SME comparisons are unchanged.

An Empty request still runs the small Whole poll that seals a zero-coverage
Result and descriptor constant witness. It issues no tensor Need, starts no
computed input producer and performs no sample arithmetic; static schema and
tolerance validation still applies.

## Current validation entry points

Build and run the strict public example, focused CTest and independent oracle:

```sh
cmake --build build/kernel-dev --target photospider_numeric_comparisons -j 8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_comparisons _strict
ctest --test-dir build/kernel-dev -R '^test_numeric_comparisons_result$' --output-on-failure
python3 oracle/ops/numeric/comparison_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_comparisons _strict
```

Use `_accelerated_apple_silicon` on Apple Silicon or
`_accelerated_x86_64` on x86-64; an unsupported host reports
`BackendUnavailable`. The root registers `test_numeric_comparisons_result`, and
the installed consumer registers `installed_numeric_comparisons_result` from
the same example source against `Photospider::kernel`.

Current Result evidence: the strict root and installed-consumer CTests pass 1/1
each, strict and Apple default workflows exit successfully, and the unchanged
comparison oracle passes 3,760 cases in each profile.

The preceding validation record below belongs to the earlier Value-backed Whole
runtime. It is retained as historical evidence and does not validate the current
Result implementation.

### Historical Value-backed Whole validation

2026-09-21, Apple M5 arm64, macOS 27.0 (26A5425a), Clang 21.1.3,
RelWithDebInfo; numerical code disabled fast math and FP contraction:

- Independent IEEE/Fraction oracle: 3,760 cases each for strict and Apple.
- The earlier public manual suites covered six predicate fixtures; select
  `[10,2,30]`; exact MAX/-MAX tolerance; eager unselected source and typed
  failures; source-before-invalid-condition priority; invalid unprojected byte;
  Empty; full support/dirty; warm cache and unselected-edit invalidation;
  composition `less -> select` yielding `[1,2,2]`; and escaped output owners.
- Earlier direct fixtures covered four-dtype raw selection, sNaN/fenv,
  65-element four-lane tails, unaligned shifted origins, singleton strides,
  negative/zero strides and mixed UInt8-control/Float64-branch strides.
- Earlier resource checks covered complete output/scratch rejection and
  cancellation after arithmetic started, with Payload release. Select has no
  scratch allocation; its output budget used branch width, not UInt8 condition
  width.
- Five focused CTests, ClangFormat 21/cpplint and scoped code/spec review passed
  for that implementation. That run did not include x86, an installed consumer
  or the full release matrix.

## Historical Value-backed Whole measurements

The measurements below are from the earlier Value-backed Whole implementation;
none is a measurement of the current Result runtime.

One CPU worker, cache off, 1 GiB public Payload limit, 2^40 dependency/run/
Footprint work limits, 512 MiB dependency state, default managed ResourceLimits.
One warm-up and seven timed samples; generation, compile, freeze and full-output
verification are outside execution timing. Every timed result is independently
checked. The measured source objects come from baseline `634be487`, were created
before timing, and were linked to the same kernel. Public and core are separately
measured layers, not timing subtraction.

Float64 `[N]` predicate inputs are `a[i]=(i%16)/8`, `b[i]=1`; is_close uses
atol=.25, rtol=0, so equal is true at residue 8 and close at residues 6..10.
Select uses UInt8 `condition[i]=(i%3==0)`, Float64 branches `i%16` and `i%16+100`.
Its core measurement directly calls the raw word-selection primitive over
prebuilt arrays; it includes no validation, allocation, public dispatch or
managed work accounting. Predicate core calls the actual four-lane math engine.

Milliseconds, medians of seven measurements:

| Operation | N | Dependency public | Whole public | Core | Whole public min–max |
|---|---:|---:|---:|---:|---:|
| equal | 1 | .100458 | .046833 | resolution limited | .035875–.065042 |
| equal | 16 | .107083 | .047917 | .000083 | .036875–.060833 |
| equal | 64 | .099375 | .037250 | .000167 | .033250–.058459 |
| equal | 256 | .115167 | .043625 | .000625 | .038583–.064958 |
| equal | 16384 | 2.069040 | .179958 | .036458 | .166333–.190542 |
| is_close | 1 | .081833 | .035583 | .000209 | .034083–.043417 |
| is_close | 16 | .100500 | .044000 | .003625 | .038250–.058000 |
| is_close | 64 | .110042 | .054208 | .014250 | .048584–.068417 |
| is_close | 256 | .185083 | .105167 | .057958 | .103250–.124333 |
| is_close | 16384 | 5.854580 | 3.845960 | 3.703830 | 3.833460–3.874750 |
| select | 1 | .103750 | .050042 | resolution limited | .047125–.064459 |
| select | 16 | 1.486210 | .050750 | resolution limited | .042959–.069083 |
| select | 64 | 8.838710 | .047625 | resolution limited | .044000–.066292 |
| select | 256 | 72.585100 | .050833 | resolution limited | .040500–.056792 |
| select | 16384 | not sampled | .273084 | .003084 | .262666–.320333 |

At N=16,384 controlled Payload peaks for equal/is_close grow from 18,712 to
280,848 bytes. Select N=256 grows from 2,088 to 6,400 bytes; Whole N=16,384 is
409,600 bytes. These are not RSS. The large legacy select baseline and 4096²
were not sampled. No cross-platform or strict timing claim is made.

Two 12-second Instruments runs used continuous cache-off Whole execution,
checking outputs on the first iteration. Timing runs above were separate and
fully checked. Inclusive sample ratios overlap:

- is_close: 11,964 execution-chain samples; callback 95.91%, exact close 93.36%,
  collect .23%, work admission .34%. Fixed-integer subtract/set/set_product/add
  dominate leaf samples (2,826 / 2,258 / 2,139 / 2,050). The remaining numerical
  cost is directly observed exact threshold arithmetic.
- select: 11,730 execution-chain samples; callback 61.95%, work admission
  30.37%, collect 1.94%. This identifies resource accounting as a substantial
  measured cost of the now-small selector callback, not an assumed SIMD issue.

Local ignored files: `build/comparison-whole/{scale.cpp,core.cpp,build_scale.py,
timings.csv,after.trace,select.trace}`, exported XML/sample JSON/summary files,
oracle/manual logs and `ctest.log`. The public category benchmark also supports
`apple selected is_close` and `apple extended select` with its own documented
N=1/256 fixtures and N/A numerical counters.
