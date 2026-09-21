# NUM-07 Whole execution

All 24 formal NUM-07 profile keys now use synchronous Whole execution: six
ordinary predicates, `is_close`, and `select`, each strict/Apple/x86. The public
workflow authors these keys directly. Output is still `values`: UInt8 0/1 for
predicates and the original branch dtype/shape for select. Legacy keys are not
used as substitutes.

Nonempty requests collect and typed-validate every complete input, compute a
complete packed owned output and project it to consumer coordinates. Empty
skips payload and callback. All source edits invalidate all observed outputs.
There are no dependency maps, continuations or per-value diagnostics (N/A).
Comparison scratch is one fixed admitted workspace, with four-element batches;
select uses fixed scalar locals and no dynamic arithmetic workspace. Capacity
includes full input collections and complete output even for sparse consumers.
Unpublished allocations are released on error/cancellation; published owners
survive context destruction.

The eager select decision was explicitly confirmed for this migration:
unselected source and typed failures are visible; collection/typed failure may
precede the callback's condition validation. A byte other than 0/1 anywhere
fails with InvalidArgument/InvalidDomain/Run, no Atom key, and a diagnostic byte
and global linear index. Selected bits, including signaling NaN and signed zero,
are copied without arithmetic quieting. Predicate IEEE/integer rules and exact
binary-rational `is_close` threshold arithmetic remain unchanged. Scalar/NEON/AVX2
comparison facilities remain; no NUM-14 certificate is used, and existing
matrix Scalar/Accelerate/SME comparisons are unchanged.

## Validation and public example

2026-09-21, Apple M5 arm64, macOS 27.0 (26A5425a), Clang 21.1.3,
RelWithDebInfo; numerical code disables fast math and FP contraction:

- Independent IEEE/Fraction oracle: 3,760 cases each for strict and Apple.
- Both public manual suites passed: six predicate fixtures; select `[10,2,30]`;
  exact MAX/-MAX tolerance; eager unselected source and typed failures;
  source-before-invalid-condition priority; invalid unprojected byte; Empty;
  full support/dirty; warm cache and unselected-edit invalidation; composition
  `less -> select` yielding `[1,2,2]`; escaped output owners.
- Direct fixtures passed four dtype raw selection, sNaN/fenv, 65-element
  four-lane tails, unaligned shifted origins, arbitrary singleton strides,
  negative/zero strides and mixed UInt8-control/Float64-branch strides.
- Resource tests passed complete output/scratch rejection and cancellation after
  arithmetic started, with Payload release. Select has no scratch allocation;
  its output budget is based on branch width, not UInt8 condition width.
- Five focused CTests passed: numeric operations, dependency sampling, execution
  demand, resources and compiler. ClangFormat 21/cpplint and independent scoped
  code/spec review passed. No x86, installed-consumer or full release matrix ran.

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric \
  --target photospider_numeric_comparisons -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_comparisons _strict
python3 examples/numeric_workflow/comparison_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_comparisons _strict
```

Repeat with `_accelerated_apple_silicon`. Historical WSL/installed-consumer
results in the operator specs describe the pre-Whole implementation.

## Measurements

One CPU worker, cache off, 1 GiB public Payload limit, 2^40 dependency/run/
Footprint work limits, 512 MiB dependency state, default managed ResourceLimits.
One warm-up and seven timed samples; generation, compile, freeze and full-output
verification are outside execution timing. Every timed result is independently
checked. Before registration objects come from `634be487`, linked to the same
kernel. Public and core are separately measured layers, not timing subtraction.

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
