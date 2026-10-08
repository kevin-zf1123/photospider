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
`BackendUnavailable`.
