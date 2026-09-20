---
spec_schema_version: 1
id: NUM-14
function: matrix_transform
proposed_operation_keys:
  - numeric.matrix_transform_strict
  - numeric.matrix_transform_accelerated_apple_silicon
  - numeric.matrix_transform_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-14: matrix_transform

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Apply a shared affine transform y=M*x+b to a collection of vectors. Inputs in
order are `vectors`, `matrix`, `bias`, all with identical Float32/Float64 dtype.
Vectors has shape [...,Cin], Cin in {2,3,4}; a rank-1 vector is allowed. Matrix
has shape [Cout,Cin] and bias [Cout], Cout in {2,3,4}. Matrix and bias are dynamic
inputs shared by all vector instances. A zero bias uses an explicit constant.
There are no implicit dtype casts, per-vector matrices, broadcasting or optional
bias input. Output values has shape [...,Cout] and the same dtype, with empty
facets. Input/output vector ranks are 1..8 and logical counts at most 2^40.

Components follow y[r]=sum_c matrix[r,c]*vectors[c]+bias[r], defining matrix
row/column orientation independently of actual storage strides. No numeric
parameters are static; Cin/Cout are inferred from validated input shapes.
For finite operands, evaluate the complete dot product plus bias as exact
mathematical arithmetic and correctly round once to output dtype. Every CPU
profile is bitwise equivalent; no separately rounded products or FMA chain define
the contract. Singular matrices are valid. This operator performs no inverse or
implicit homogeneous-coordinate division.

## Special values and zero signs

All floating inputs may contain NaN/Inf. For an output row, source NaN priority
is vectors[c] by increasing c, then the selected matrix row by increasing c,
then bias[r]. Quiet the first source NaN with its sign/payload preserved. This
priority precedes newly generated exceptional values and does not short-circuit
required reads or typed validation.

With no source NaN, any zero-times-infinity product yields the fixed positive
quiet NaN. Otherwise classify each product using operand sign XOR and exact
finite multiplication, and include bias. Opposite signed infinities among terms
yield canonical NaN; a sole infinity sign determines that infinity result. Finite
products are never rounded individually, including overflow/underflow decisions.

For a finite exact-zero total, return -0 only if every product term is signed
negative zero and bias is -0; otherwise return +0. A zero product's sign is the
XOR of its input signs. Nonzero exact totals that underflow preserve their sign.
Final floating overflow is the correctly signed infinity, a successful result.
Fixed NaN patterns and floating-environment restoration follow
[NUM-04](NUM-04_unary_contract.md).

## Per-component demand and invalidation

For requested vector instances and output components r, read each complete input
vector, only the matrix rows corresponding to requested components, and the
corresponding bias elements. Deduplicate shared row/bias reads. Zero coefficients
do not remove vector dependencies: 0*Inf remains an observable invalid product.
Other vector instances, matrix rows and bias components have no Data demand.
Add recognized typed Validation closure separately. Empty Q reads nothing.

Vector component changes invalidate all output components for that vector instance;
selected matrix-row or bias changes invalidate that output component across all
instances. Retain Data/Validation and metadata witnesses. The output shape and
dtype are static; partial component requests do not imply reading unrelated
matrix rows merely to form a dense temporary matrix. Return owned packed output
fragments with correct global logical and storage origins.

## Algorithms, resources and errors

Use a bounded exact dot accumulator or certified equivalent for at most four
products plus bias, with one final rounding. SIMD/FMA implementations require
proof of this result, rather than substituting a different sequential rounding
order. Work is O(number of requested components * Cin), plus exact arithmetic;
account vector/matrix/bias owners, selected-row metadata, accumulators, output
bytes and scratch. Output does not require the full vector batch allocation.

Read arbitrary valid immutable source strides/offsets. Use host workers/budgets,
check cancellation per processing block and arithmetic refinement, and release
unpublished state on failure. No hidden GPU/Metal version or private thread pool
is introduced. Platform-specific keys require their named CPU targets. Cache-off
and result lifetime after context destruction follow the common owner rules.

Compile/preflight rejects dtype mismatch, unsupported Cin/Cout, bad bias/matrix
shape and logical vector count above 2^40. Singular matrices do not cause an
error. Generic nonfinite results use the numeric table; typed/upstream/resource/
cancellation failures retain Status categories, and a failed observation publishes
no partial output.

## Acceptance and implementation status

Conceptual fixture: vectors=[2,3], matrix=[[1,2],[-1,0]], bias=[4,5] produces
[12,3]. Independent exact rational dot products and source bit classification
are the oracle. Include rectangular 2->4 and 4->3 transforms, multiple batches,
identity and singular matrices, cancellation after huge products, subnormals,
all NaN priorities, 0*Inf, infinity cancellation and signed-zero terms.

Source-read logs for only output component r must show the full selected vector,
matrix[r,:], bias[r] and no other numeric rows. Test matrix/bias sharing,
strides, typed-validation closure, invalidation, low budgets, cancellation and
owner lifetime through public WorkflowDocument execution when implemented.
The implementation evidence below records checks actually run.

## Implementation and executable acceptance

`numeric_matrix.cpp` registers all three keys. The public
`photospider/numeric/matrix.hpp` helper `matrix_transform_node` authors the three
explicit input edges. `exact_dot.hpp` uses a host-owned 4352-bit accumulator in
units of 2^-2148; at most four binary64 products plus bias require 4198 magnitude
bits. No product is independently rounded. Source NaN classification precedes
generated invalid products; all required inputs and Validation have already
arrived before numeric evaluation.

One regional Need associates each requested output with its complete vector,
selected matrix row and bias. Transport unions deduplicate overlapping vectors
and shared rows/bias; source/result metadata and actual retained payload are
charged. The result is packed only over requested rectangles. This does not
promise sharing across separate executions or a once-per-Run transition.

The manual `photospider_numeric_matrix` executable and `matrix_oracle.py` live
in [the numeric workflow example](../../../../examples/numeric_workflow/README.md).
Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 1,110
independent Fraction/raw-bit cases per profile on 2026-09-19, plus editable
public fixtures, cache on/off, sparse support/dirty and deduplicated row/bias
transport, typed/Empty inputs, metadata errors, all-port negative strides,
fenv modes/flags, exact-work WorkLimit/cancellation and release checks, and
required upstream failure after vector NaN. Installed strict/Apple consumers,
the focused compiler unit, formatting/lint and scoped math/entry reviews passed.
No integration test or CTest registration is added. Specification status
remains Proposed independently of this implementation evidence.
