---
spec_schema_version: 1
id: NUM-15A
parent_id: NUM-15
function: derivative_1d
proposed_operation_keys:
  - numeric.derivative_1d_strict
  - numeric.derivative_1d_accelerated_apple_silicon
  - numeric.derivative_1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-15A: derivative_1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Differentiate one-dimensional samples using two `Result` tensor inputs: samples
with `sample_shape()` [N] and step with `sample_shape()` [1]. Each input may use
any tensor member key. Both use Float32 or both Float64. Require 2<=N<=2^40 and
finite nonzero step, allowing negative step. The output port key is `values`;
its Result schema is `photospider.tensor` with tensor member `samples`, shape
[N], input dtype, and no attached domain description. The
starting coordinate does not affect this operation;
callers preserve or connect sampling-axis information separately. There are no
static numeric parameters or implicit casts/axis extraction.

## Difference formula and numeric semantics

Use first-order one-sided endpoints and central interior differences:

```text
D[0]   = (samples[1]-samples[0]) / step
D[N-1] = (samples[N-1]-samples[N-2]) / step
D[i]   = (samples[i+1]-samples[i-1]) / (2*step), 0<i<N-1
```

For N=2 both results use the same one-sided quotient. There is no edge-order
parameter or automatic higher-order stencil. For finite samples, evaluate the
whole rational quotient exactly and correctly round once to output dtype; accelerated uses the shared FP32-scaled bound. Neither an overflowing naive
subtraction nor 2*step intermediate changes the mathematical result.

Exact zero results are +0. Nonzero exact results that underflow retain their sign;
final overflow yields signed infinity successfully. Among required samples,
propagate the first NaN in ascending source index order, quieting it and preserving
payload/sign. Without source NaNs, same-sign infinities in the subtraction yield
fixed positive quiet NaN, otherwise subtraction/division determine the infinity
sign. Step is finite and nonzero as a caller precondition, so it cannot introduce a numeric NaN within the specified domain. Fixed NaN bits follow [NUM-04](NUM-04_unary_contract.md).

## Fixed dependencies and errors

Each selected output tile becomes one fixed CPU task. For interval `[a,b)`, its source support is the union of lower stencil image `[max(0,a-1),max(1,b-1))` and upper image `[min(a+1,N-1),min(b,N-1)+1)`, rounded to complete source tiles. The task also reads step tile zero. Source geometry is independent of output geometry; atomic source tiles can enlarge physical coverage. An interior center sample is not in its own stencil. With an ordinary source axis and tile extent one, its source tile is omitted; an atomic source tile can still cover that center. Empty selection emits no tasks or payload reads.

Active step must be finite and nonzero as an unchecked caller numerical precondition. No numerical diagnostic is guaranteed outside this domain. All IEEE sample values are valid. Static shape/dtype mismatches return TypeMismatch; source, resource, backend and cancellation failures retain their operational status. A task failure prevents successful publication of its IR call's outputs.

## Algorithm and resources

Each task computes its selected coordinates using preallocated `sizeof(ExactCalculus)` scratch and an independent dense output tile. Runtime borrows source views and performs no payload allocation. Prepare checks both input schemas, the ordinary output schema, canonical Host write, exact source/control reads, empty parameters and scratch. Root reserves maximum scratch per CPU lane, selected output bytes and executor metadata. Cancellation is checked during output iteration and exact arithmetic. Accepted work drains before failed storage is released; successful outputs can outlive the execution context.

## Acceptance

For samples `[0,1,4]`, step `[1]`, expect `[1,2,3]`. For `[-MAX,0,MAX]` and step `[MAX]`, expect three ones despite naive intermediate overflow. Compare independent Fraction references for negative/subnormal step, N=2, first-stencil NaN, an interior center NaN, infinity arithmetic, signed zero, underflow and final overflow.

Verify exact source closure and omitted center producers with an ordinary source axis and extent one, source atomic geometry that retains the shared center tile, sparse selections, negative/zero strides, unaligned input, concurrent Runs and artifact round trips. Include binding replacement, capacity rejection, cancellation, malformed preparation and escaped Result lifetime. The [fixed calculus execution contract](../calculus-execution.md) defines the resource and dependency boundaries. Temporary checks are removed after operation validation; the independent oracle is retained.
