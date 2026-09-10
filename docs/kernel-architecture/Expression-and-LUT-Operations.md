# Expression and 1D LUT operations

The default registry provides `numeric.sample_expression` and `lut.apply_1d`
through public WorkflowDocument, Compiler and ExecutionContext APIs. Both are
CPU Whole operations with packed Float32 outputs. Their accepted scope is
[ADR 0020](../adr/0020-composable-operation-foundations.md); the
[Chinese mirror](zh/Expression-and-LUT-Operations.zh.md) describes this implementation.

## Expression sampling

Input is one unfaceted generic Float64 `[K]`, `1 <= K <= 256`, containing dynamic
coefficients. All coefficients must be finite, including unused entries. Required
static parameters are String `expression`, Int64 `count` in `[1,1048576]`, finite
Float64 `start`, and finite positive Float64 `step`. Constructors explicitly write
defaults, for example `count=3, start=0, step=.5`; the registry supplies none.
The finite sampling endpoint `fma(count-1, step, start)` must exceed start when
count exceeds one. The output is Float32 `[count]`, with SampledSignal metadata:
channel name/role `value`, value unit `dimensionless`, and a `dimensionless`
sampling axis whose origin/step are the parameters. Shape remains fixed per plan.

Grammar permits decimal/scientific literals, `x`, `c[index]`, parentheses,
unary `+ -`, binary `+ - * / ^`, unary `abs sqrt exp log sin cos`, and binary
`min max`. Indices are nonnegative decimal integers checked against K. Hexadecimal
literals, NaN/infinity names and arbitrary identifiers are rejected. Power is
right-associative and binds above unary minus: `-2^2=-4`, `2^-2=.25`,
`2^3^2=512`; `0^0=1`. Source is at most 4096 bytes; the actual AST has at most
256 nodes and height 32. Parentheses do not add AST nodes. The iterative parser
is shared by compilation and execution; no script, loop, file I/O or Run binding
is stored in registry or plan state.

Each sample uses `x=fma(i,step,start)` and Float64 evaluation in a controlled
nearest/gradual-underflow environment. Every subexpression must be finite, so
`min(1e300*1e300,0)` fails. Division by zero and function domain errors fail;
Float32 output range is checked before narrowing. The invocation allocator owns
output and 4096 bytes of coefficient/evaluation scratch. Evaluation checks
cancellation within the AST and between samples; failures release unpublished
allocations and restore the caller's numeric environment.

## Linear LUT application

Inputs are a Float32 SampledSignal query and a Float32 `[N]` single-channel
SampledSignal or Lut table, `N >= 2`. The query's **sample value unit** must equal
the table's **sampling axis unit**. Query axis units and table value units can
be independent. Table domain is its finite origin, positive step and finite
increasing `fma(N-1,step,origin)` endpoint. Required String `out_of_domain` is
`reject` (constructor default) or explicit `clip`. Clip returns the nearest
endpoint outside that domain. The output has the query shape and empty facets;
this operation does not establish table-value semantic metadata on the result.

Endpoint samples are exact. Interior interpolation uses Float64 compensated
local distances and weighted products, normalized by the step's binary exponent.
It combines the numerator before division to retain cancellation and avoids
intermediate overflow for huge steps. Signed table values, HDR and opposite
Float32 extremes are supported without a gamut clamp. Interior indices at or
above `2^53`, nonrepresentable arithmetic, or discarded compensation exceeding
one eighth of a Float32 ULP fail with `OperationFailed`; they do not publish an
unreliable finite sample. This is linear 1D interpolation, not a color 3D LUT.

The shared closed metadata rules `SampleExpression` (13) and `ApplyLut1d` (14)
reuse `output_semantic_input` and `output_semantic_parameter`. They validate input
metadata, required parameters and resolved output shape/dtype before IR or direct
callback execution, including C plugin declarations. SampleExpression's named
String parameter is the expression, with required Float64 `start`/`step`; its
resolved output supplies count. ApplyLut1d uses two ordered inputs and its named
String parameter for the domain policy. The same parser supplies coefficient
index validation; no inference is selected by operation key. ABI/Traits stay 7.

Malformed expressions/parameters return `InvalidArgument`; incompatible metadata
returns `TypeMismatch`; computed numeric failures return `OperationFailed` with
the sample index. Direct invalid typed bindings retain `InvalidArgument`.
Cancellation, stale plans and resource exhaustion retain their existing codes.

## Public workflow and checkable results

[test_expression_operations.cpp](../../tests/integration/test_expression_operations.cpp)
provides runnable public workflows in `sample()`, `luts()` and `gain_scene()`.
`document()` declares immutable Values, `run()` compiles a GraphContext and calls
execute with per-run bindings. A generator node has this public representation:

```cpp
ps::WorkflowNode generator{
    1, "numeric.sample_expression", {ps::WorkflowInputReference{1}},
    {{"expression", std::string("c[0]*x^2")}, {"count", std::int64_t{3}},
     {"start", 0.0}, {"step", 0.5}}};
```

Bind input 1 as Float64 `[1]` containing `1`. Expected samples are `[0,.25,1]`.
Connect the output as input 2 of `lut.apply_1d`, with a dimensionless SampledSignal
query `.25` as input 1 and `out_of_domain="reject"`: the result is `.125`.
Change expression, coefficients, start/step or static count to compose variants;
changing static shape requires recompilation. For gain, choose `count=1` and
`c[0]*2`, then connect its output to `image.exposure_gain`'s scalar input. The
same plan accepts changing coefficient Values and checks each generated result
against `[0,16]` before entering gain, including cache hits.

```sh
cmake --build build/issue257-static --target test_expression_operations -j 8
ctest --test-dir build/issue257-static -R '^test_expression_operations$' --output-on-failure
```

Exit zero checks independent expression/LUT oracles, multiple counts, grammar and
numeric boundaries, dynamic sequential/concurrent bindings, warmed invalid gain,
shared cancellation and allocation release. It checks near-endpoint interpolation
against `222044608266240F`, exact weighted cancellation to zero, opposite maximum
Float32 midpoint zero and a DBL_MAX sampling step. The installed consumer builds
this same source and a pure C contract fixture using only installed public targets.

Regional/stream result caching accepts preflight-validated Whole dense direct
Value bindings up to 2048 bytes, covering all 256 Float64 coefficients. Category,
dtype, rank/shape, exact facets, byte length and every input bit enter the key.
Larger or partial direct inputs remain uncacheable. Pure generic/scalar ordinary
execute retains its existing fast path; this eligibility applies to regional
execution and execute_stream. The generator-to-image workflow exercises actual
ordinary execute cache hits. This does not add new snapshot or disk value types;
see the [cache model](Cache-Model.md).
