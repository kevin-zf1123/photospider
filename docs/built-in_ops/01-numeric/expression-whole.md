# NUM-01 Whole execution

The three suffixed `numeric.sample_expression_*` operations expose independent
Result outputs. Port `values` uses schema `photospider.tensor` / member
`samples`, with the selected Float32 or Float64 dtype and shape `[N]`. Port
`axis` uses the same schema and member key, with Float64 shape `[3]` and one
atomic trailing tuple `[start,end,step]`, or `[start,start,+0]` when N=1.
Each input Result has one tensor member under any key and `sample_shape()` [1];
input schema IDs may differ, fields may coexist, and scalar dtypes may mix.

`prepare_static` parses and validates the bounded expression once. The resulting
immutable AST is shared by both output programs and reused across dynamic
bindings. It also establishes output schemas and static input projections. Every
input schema, member shape and dtype is checked, even when a runtime projection
excludes that input.

For nonempty demand, `WholeTensorProgram` sends authorized Data, Validation and
Descriptor Needs (role 13) in envelopes of at most 64 input ports. Registration
declares a conservative 258 input slots: start, end and a 256-coefficient
ceiling. Its stage budget allows five bounded input stages and publication as
stage six. The 256-node AST limit can reject expressions before all declared
coefficient slots are reachable. Current coverage uses a balanced expression
with 128 coefficients (255 AST nodes, height 8) and 130 declared input ports;
count=1 activates 129 inputs over three input Need stages. Values uses start
and all coefficients for N=1, and adds end for N>=2. Axis uses only start for
N=1 and start/end for N>=2; it never requests coefficients or evaluates the
expression. Empty demand
produces empty output coverage without reading payloads or doing sample work.

Strict execution preserves the sampling specification's exact endpoint
interpolation and RN64 `step`/coordinate calculations. It evaluates each AST
primitive in left-to-right postorder using its own specified RN64 operation;
the complete expression is not rounded just once. Every intermediate is checked
before its parent runs, and the final value is converted once to the selected
output dtype. Values checks adjacent coordinates across the full grid and
evaluates all N samples before projecting. Axis checks its interval and step
without evaluating coefficients or scanning coordinate distinctness.

Accelerated execution evaluates independent samples in four-lane batches and
propagates enclosures of the strict stepwise RN64 result. It uses strict replay
for samples whose final error or domain classification cannot be certified.
The shared final FP32-scaled four-ULP contract applies to both output dtypes.
Strict and accelerated keys retain separate cache identities.

Each output Result has its own ObjectId. Its association names the ObjectIds of
the inputs active for that output: values associates its active endpoints and
coefficients, while axis associates only active endpoints. No additional
identity pairs the values and axis Results; workflows connect their named ports.
Input edits invalidate the outputs that depend on those inputs. Numeric failures
have Domain/Run scope, no Atom, and preserve the global sample index, coordinate
and AST source span when available. A failed run publishes no partial output.

Values owns `N * sizeof(dtype)` output bytes and a bounded evaluation workspace;
axis owns 24 output bytes and its smaller coordinate workspace. The runtime
admits actual coordinate and exact-math scratch, work, stages and output storage.
Cancellation and resource failures release unpublished output and scratch.
Published Result storage remains valid after execution context retirement.

## Focused validation

Build and run the public Result workflow test from the configured build
directory:

```sh
cmake --build build/kernel-dev --target photospider_numeric_expression -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_expression_result$' --output-on-failure
```

The root CTest `test_numeric_expression_result` runs the strict example through
the public C++ API. Focused coverage
asserts that both output programs share one prepared AST. It also checks mixed
Float32/Float64 bindings, a balanced 128-coefficient expression (255 AST nodes,
height 8) over 130 declared ports, three Need polls for the 129 active inputs,
atomic axis tuple publication and exact source associations. Other cases check
whole-domain failure outside the requested values region, duplicate-coordinate
rejection with axis-only behavior, a failing end producer excluded for N=1,
invalid scalar input schemas, Float32 final overflow, a valid Empty values+axis
request with empty coverage and no failing-coefficient producer start,
floating-point environment restoration, WorkLimit, pre-cancellation, and Results
that remain readable after context retirement. Fraction/MPFR oracle and
benchmark commands remain in the workflow README and are separate from these
CTests.
