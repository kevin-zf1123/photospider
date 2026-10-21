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
---

# NUM-14: matrix_transform

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Apply a shared affine transform y=M*x+b to a collection of vectors. Inputs in
port order are `vectors`, `matrix`, `bias`; each input is a `Result` with one
tensor member under any member key. Their dtypes match and are Float32 or
Float64. The vectors tensor has `sample_shape()` [...,Cin], Cin in {2,3,4}; a
rank-1 vector is allowed. The matrix has `sample_shape()` [Cout,Cin] and the
bias [Cout], Cout in {2,3,4}. Matrix and bias are dynamic inputs shared by all
vector instances. A zero bias uses an explicit constant. There are no implicit
dtype casts, per-vector matrices, broadcasting or optional bias input. Output
port key is `values`; its `photospider.tensor` Result schema contains tensor
member `samples`, with shape [...,Cout], the same dtype, and no batch
topology. Input/output vector ranks are 1..8 and logical counts at most 2^40.

Components follow y[r]=sum_c matrix[r,c]*vectors[c]+bias[r], defining matrix
row/column orientation independently of actual storage strides. No numeric
parameters are static; Cin/Cout are inferred from validated input shapes.
For finite operands, evaluate the complete dot product plus bias as exact
mathematical arithmetic and correctly round once to output dtype. Strict defines the reference; accelerated follows the shared FP32-scaled bound; no separately rounded products or FMA chain define
the contract. Singular matrices are valid. This operator performs no inverse or
implicit homogeneous-coordinate division.

## Special values and zero signs

All floating inputs may contain NaN/Inf. For an output row, source NaN priority
is vectors[c] by increasing c, then the selected matrix row by increasing c,
then bias[r]. Quiet the first source NaN with its sign/payload preserved. This
priority precedes newly generated exceptional values and does not short-circuit
declared input dependencies.

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

## Fixed task dependencies

Each requested output tile lowers to one CPU task. Vectors contribute the matching prefix coordinates and every Cin component. Matrix contributes the selected output channel rows and all Cin columns; bias contributes the selected output channels. Each region is mapped through that input's own grid, so tile granularity may include additional samples. Zero coefficients preserve all declared vector dependencies. A partial channel request can omit matrix/bias tiles outside its selected rows.

Prepare validates the exact fixed read slots, output view, kernel contract and scratch size. Kernels borrow admitted affine inputs and write one separate preallocated dense output tile. Sparse selection computes only requested output tiles. Empty selection emits no tasks or payload reads. Failure discards unpublished writes after accepted callbacks return; successful Result owners survive the context.

## Algorithms, resources and errors

Use a bounded exact dot accumulator or certified equivalent for at most four
products plus bias, with one final rounding. SIMD/FMA implementations require
proof of this result, rather than substituting a different sequential rounding
order. Work is O(number of vector instances * Cout * Cin), plus exact arithmetic.
Block admission accounts selected dense output tiles and a fixed per-lane workspace containing batch buffers and the exact accumulator. Input owners remain retained through completion.

Read arbitrary valid immutable source strides/offsets. Use host workers/budgets,
check cancellation per processing block and arithmetic refinement, and release
unpublished state on failure. No hidden GPU/Metal version or private thread pool
is introduced. Platform-specific keys require their named CPU targets. Result lifetime after context destruction follows the common owner rules.

Compile/preflight rejects dtype mismatch, unsupported Cin/Cout, bad bias/matrix
shape and logical vector count above 2^40. Singular matrices do not cause an
error. Generic nonfinite results use the numeric table; structural/upstream/resource/cancellation failures retain Status categories, and a failed observation publishes
no partial output.

## Acceptance and implementation status

Conceptual fixture: vectors=[2,3], matrix=[[1,2],[-1,0]], bias=[4,5] produces
[12,3]. Independent exact rational dot products and source bit classification
are the oracle. Include rectangular 2->4 and 4->3 transforms, multiple batches,
identity and singular matrices, cancellation after huge products, subnormals,
all NaN priorities, 0*Inf, infinity cancellation and signed-zero terms.

A partial component request must show the selected vector-prefix, matrix-row and bias-channel tile sets. Check empty selection, admitted affine strides/offsets, structural metadata errors, low budgets, cancellation, concurrent Runs and owner lifetime through public execution.

## Implementation and executable acceptance

`numeric_matrix.cpp` registers the strict, Apple Silicon accelerated, and x86_64
accelerated keys. `matrix_transform_node` authors three Result input edges in
the order `vectors`, `matrix`, `bias`; each edge has one tensor member under any
member key. The output is the `values` Result with `photospider.tensor` schema
and tensor member `samples`.

Metadata specialization validates dtype, rank, component counts, shape agreement and the 2^40 logical-count limit. The fixed kernel preparation owns immutable profile, Cin and tile-to-slot maps, reusable by concurrent Runs with independent bindings.

`exact_dot.hpp` uses a 4352-bit accumulator in units of 2^-2148; at most four binary64 products plus bias require 4198 magnitude bits. No product is independently rounded. Each task caches its selected matrix rows and bias in invocation scratch, gathers matching vectors in blocks of at most 64, and writes directly into its preallocated output tile. A task's local output component count can be 1..4 even though the complete Cout is 2..4. Global row selection and dense output offsets preserve the nonzero channel origin.

Temporary acceptance should compare against independent exact dot and bit-classification references. Cover rank 1/8, all Cin/Cout pairs, one-channel output tails, nonzero tile origins, batch/atomic input grids, negative/zero/unaligned admitted strides, caller/worker floating state, 64-vector batch tails, imported Plans, rejected malformed signatures and concurrent Runs. Report actual profiles separately from Accelerate or SME instruction dispatch.

## Apple matrix candidate backends

The Apple profile reuses selected matrix rows and bias within each fixed invocation and reads each selected vector once. Blocks contain at most 64 vectors, including a tail;
arbitrary valid direct-input strides and offsets are supported.

For Float32, finite operands are promoted exactly to binary64. Candidate
backends are selected at build time without changing the public operation key:

| `PHOTOSPIDER_MATRIX_BACKEND` | Candidate calculation |
| --- | --- |
| `ACCELERATE` (default) | Row-major `cblas_dgemm(X, M^T)` plus bias; LP64 dimensions bounded by 64. Requires `PHOTOSPIDER_ENABLE_ACCELERATE=ON`, macOS 15+ and successful single-thread admission; otherwise uses the scalar candidate. |
| `SME` | Direct FP64 `FMOPA` using private ZA and local streaming mode. Requires `PHOTOSPIDER_ENABLE_MATRIX_SME=ON`; runtime admission checks both SME and SME_F64F64, otherwise uses scalar. |
| `SCALAR` | Fixed-order binary64 multiply/add candidate, with the same certificate. |

Every candidate is independently checked against a binary64 enclosure of the
exact dot plus bias. Finite Float32 products are exact in binary64. A separate
rounded sum `s` and rounded absolute-term sum `A` give endpoints
`RN64(s - A*2^-50)` and `RN64(s + A*2^-50)`. With `u=2^-53` and
`gamma=4u/(1-4u)`, the exact-sum error is bounded by `gamma*S`, where
`S=abs(bias)+sum(abs(products))`, and `A >= (1-gamma)*S`. The radius `8u*A`
also covers endpoint rounding because
`8u*(1-gamma) > gamma + u*(1+gamma)*(1+8u)`. All intermediates are far from
binary64 overflow/underflow for finite Float32 inputs. This matrix-specific
proof does not change the general interval arithmetic used by other operators.

The candidate is published only when both endpoints convert to the same normal
Float32 bits as the candidate. The existing real-range admission is retained.
Uncertain rounding, zero, extreme ranges and original nonfinite operands use
`ExactDot` with original raw words. Thus all three backends retain exact
Float32 results across channel subsets, block tails and Regions. Float64
retains `ExactDot`; its inputs and outputs are never narrowed.

`MatrixBlock` owns 8,640 bytes of fixed buffers inside preallocated invocation scratch, alongside the exact accumulator. All profiles use this workspace.
Cancellation is checked before each batch, inside exact arithmetic and before return. Arithmetic checkpoints query cancellation without dynamic sample-work charging. Resource/cancellation failures release unpublished output after callbacks drain. Per-value fallback/copy counters are unavailable. Build configuration selects the candidate implementation without changing task topology or placement.

Accelerate uses thread-local single-thread mode and restores the previous mode;
caller floating state is scoped across packing, calculation and certification.
The framework's private allocations have not been proven to have a fixed upper
bound or to use the host allocator. Managed scratch accounting here covers the
operation-owned buffers, not all private system-library allocation. Direct SME
uses caller-owned buffers and architectural register state, without a BLAS
call or private worker pool. Accelerate may itself choose SME internally; this
comparison does not identify its private instruction dispatch.

SME compilation is restricted to its own source with
`-march=armv8-a+sme+sme-f64f64`; enabling the full Armv9 baseline would also
permit non-streaming SVE, which current Apple SME systems do not expose.
The wrapper and runtime capability checks retain the ordinary host target.
See [Apple's SME platform notes](https://github.com/apple-oss-distributions/xnu/blob/main/doc/arm/sme.md)
and [Arm ACLE streaming/ZA rules](https://arm-software.github.io/acle/main/acle.html).
The implementation preserves `-fno-fast-math -frounding-math -ffp-contract=off`.

Create temporary public Result checks for shape, exact values, structural validation, empty selection, resource limits, cancellation, preparation reuse and escaped-owner lifetime. Compare matrix formulas against independent exact dot products. Benchmarking, when needed for a change, must report its measured scope separately from numerical acceptance.

The Fraction oracle independently checks exact dot products and source-NaN
priority. These checks are correctness samples, not
a performance campaign. Benchmark elapsed time includes Result coordination,
digest.

Specification status remains Proposed independently of implementation status.
Focused tests of the strict and locally available profiles do not prove that
Accelerate or SME hardware instructions ran individually, and no cross-platform
performance claim is made.
