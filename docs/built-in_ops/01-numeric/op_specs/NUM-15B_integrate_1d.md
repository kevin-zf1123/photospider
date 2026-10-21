---
spec_schema_version: 1
id: NUM-15B
parent_id: NUM-15
function: integrate_1d
proposed_operation_keys:
  - numeric.integrate_1d_strict
  - numeric.integrate_1d_accelerated_apple_silicon
  - numeric.integrate_1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-15B: integrate_1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute cumulative trapezoidal integration from three `Result` tensor inputs in
port order: samples with `sample_shape()` [N], step [1], and initial [1]. Each
input may use any tensor member key; all share Float32 or Float64 dtype. Require
1<=N<=2^40. The output port key is `values`; its Result schema is
`photospider.tensor` with tensor member `samples`, shape [N], input dtype, and
no attached domain description. There are no static numeric parameters or
implicit casts/axis extraction.

```text
values[0] = initial
values[i] = initial + step * sum_{k=0..i-1}(samples[k]+samples[k+1])/2
```

Output zero copies initial raw bits, including sNaN, Inf and signed zero. A canonical output tile containing only zero is a separate initial-only task: this occurs for N=1 and for the first tile at extent one. It records only samples/step schemas and excludes their payloads and producers. All input metadata remains checked. Positive outputs require a finite nonzero step as a caller numerical precondition; negative step is valid.

## Complete-formula quality and special values

At i>0, evaluate the entire initial-plus-area expression mathematically, rounding
only once to output dtype. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.
For finite sources, an equivalent exact expression is

```text
initial + step/2 * (samples[0] + 2*sum(samples[1:i]) + samples[i])
```

Do not round trapezoids, prefixes or the area before adding initial. Unrequested
intermediate output overflows do not fail a later representable requested value.
Exact zero at i>0 is +0; nonzero exact underflow retains its sign. Final overflow
yields correctly signed infinity as a successful numeric result.

For i>0, source NaN priority is initial first, then samples by increasing index
within the contributing prefix. Quiet it preserving payload/sign. With no NaN,
opposite signed sample infinities make the trapezoidal area undefined and yield
fixed positive quiet NaN. Otherwise the sample infinity sign is multiplied by
step's sign, then combined with initial: opposite infinities yield canonical NaN,
one infinity sign determines the result. Finite weighted sums and products stay
exact until final output classification. Output 0 retains its separate raw-copy
contract; a generated prior-output NaN is never treated as a new source operand.

Fixed NaN patterns and floating-environment preservation follow
[NUM-04](NUM-04_unary_contract.md); output 0 retains its explicit raw-copy exception.

## Fixed dependencies and publication

All selected tiles other than the zero-only tile share one fixed prefix task. Its source support is `[0,E)`, where E is the end of the largest selected output tile. It also reads step and initial. Source tile rounding uses the source's own geometry. A tile containing both zero and positive coordinates belongs to this task. Empty selections create no tasks or payload reads.

The separate zero-only task reads initial, has zero scratch and checks samples/step schemas. Selecting only this task from a saved Plan removes the positive prefix task and its producers. Retaining a prefix task retains every declared read and write, even when the final return exposes fewer tiles. Recompilation can shorten its prefix.

Step is an unchecked numerical precondition whenever a prefix task is active, and no out-of-domain diagnostic is guaranteed. Source and initial support all IEEE patterns. Operational failure prevents successful publication of the IR call, including the zero-only output when its call also contains a failing selected prefix task. Resource, source, backend and cancellation failures retain their categories. Successful Result owners can outlive ExecutionContext.

## Exact state and resources

The prefix task keeps the exact unweighted source sum, first/last values and special classifications. A separate ratio workspace computes `2*sum-first-last`, multiplies the exact step and adds aligned initial before final conversion. Source accumulation visits each prefix coordinate once, and only selected output coordinates are converted. The exact carry survives conversion; rounded results never become state.

Each prefix invocation uses preallocated `sizeof(ExactCalculus)` scratch; the initial-only task uses zero. Only selected output tiles are allocated, each with separate dense storage. There is no runtime payload/carry allocation. Root admits maximum scratch per CPU lane, selected output bytes and executor metadata. Cancellation is checked during source traversal and arithmetic. Prepare validates all schemas, canonical sorted Host writes, zero-only separation, exact source/control reads, empty parameters and scratch. Static schema incompatibility returns TypeMismatch. Accepted work drains before unpublished storage is released.

## Acceptance

For samples `[0,1,2]`, step `[1]`, initial `[0]`, expect `[0,0.5,2]`. For constant samples `[2,2,2]`, step `[0.5]`, initial `[3]`, expect `[3,4,5]`. Use independent weighted-prefix Fraction arithmetic and explicit IEEE tables for negative/subnormal step, raw zero-output NaNs, signed zero, infinity cancellation, NaN priority and representable outputs following large intermediate areas.

Connect failing samples/step producers to a zero-only selection at N=1 and at larger N with extent one; neither producer should run. Selecting positive tiles must retain the corresponding source prefix and controls. Verify both fresh compilation and saved Plan selection, source atomic geometry, disjoint writes, negative/zero strides, unaligned storage, concurrent Runs, artifact import, binding replacement, capacity rejection, cancellation, preparation rejection and Result lifetime. See [fixed calculus execution](../calculus-execution.md). Temporary numerical checkers are removed after validation; the oracle remains available.
