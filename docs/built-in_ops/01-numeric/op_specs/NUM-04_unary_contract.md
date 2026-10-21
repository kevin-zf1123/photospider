---
spec_schema_version: 1
id: NUM-04
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04: shared unary contract

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

This file contains common decisions inherited by the independent unary operator
specifications. It does not register a generic `numeric.unary` mode dispatcher.
The selected operations are abs, neg, sqrt, exp, ln, sin, cos, tan, floor, ceil,
round, sign, reciprocal, sinpi, cospi, tanpi, sinc and sincpi. Pow is a binary operation under NUM-05.

Each operation uses independently named strict, Apple Silicon CPU accelerated
and x86-64 CPU accelerated versions under the common NUM policy. Individual
files state dtypes, mathematical behavior, rounding and any allowed approximation.
All remain Proposed until their completed specifications are accepted.

| Spec | Supported dtypes | Finite accelerated quality |
| --- | --- | --- |
| [NUM-04A abs](NUM-04A_abs.md) | All four | Bitwise strict equivalence |
| [NUM-04B neg](NUM-04B_neg.md) | Int64, Float32/64 | Bitwise strict equivalence |
| [NUM-04C sqrt](NUM-04C_sqrt.md) | Float32/64 | Strict correct rounding; accelerated final FP32-scaled bound |
| [NUM-04D exp](NUM-04D_exp.md) | Float32/64 | <=4 FP32-scaled ULP for nonzero finite results |
| [NUM-04E ln](NUM-04E_ln.md) | Float32/64 | <=4 FP32-scaled ULP for nonzero finite results |
| [NUM-04F sin](NUM-04F_sin.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04G cos](NUM-04G_cos.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04H tan](NUM-04H_tan.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04I floor](NUM-04I_floor.md) | All four | Bitwise strict equivalence |
| [NUM-04J ceil](NUM-04J_ceil.md) | All four | Bitwise strict equivalence |
| [NUM-04K round](NUM-04K_round.md) | All four | Bitwise strict equivalence, ties-to-even |
| [NUM-04L sign](NUM-04L_sign.md) | All four | Bitwise strict equivalence |
| [NUM-04M reciprocal](NUM-04M_reciprocal.md) | Float32/64 | Strict correct rounding; accelerated final FP32-scaled bound |
| [NUM-04N sinpi](NUM-04N_sinpi.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04O cospi](NUM-04O_cospi.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04P tanpi](NUM-04P_tanpi.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04Q sinc](NUM-04Q_sinc.md) | Float32/64 | <=4 ULP for the whole mathematical function |
| [NUM-04R sincpi](NUM-04R_sincpi.md) | Float32/64 | <=4 ULP for the whole mathematical function |

For radian functions and independent pi-multiple functions, also inherit the
[trigonometric contract](NUM-04_trigonometric_contract.md). These files define
target behavior.

## Selected nonfinite behavior

The maintainer selected IEEE-like numeric propagation for this family, allowing
NaN and infinity in inputs and outputs. This supersedes the proposed finite-only
unary policy. It does not retroactively change NUM-01/CRV-02 generator contracts.
Domain errors and floating overflow are numeric results, not operation failures:
sqrt(-1) and ln(-1) yield NaN, ln(0) yields -infinity, reciprocal of +0/-0 yields
+infinity/-infinity, and exp overflow yields +infinity. These observations
complete successfully. Type, resource, cancellation and upstream execution
failures retain their Status behavior. Individual files enumerate exact cases
rather than claiming unspecified general IEEE compatibility.

Input NaNs retain their payload and are quieted if signaling; no floating
conversion is used to transport the payload. Newly generated NaN uses positive
quiet NaN `0x7fc00000` for Float32 or `0x7ff8000000000000` for Float64. Unless
the operation specifically changes sign (abs/neg), preserve the input NaN sign.
Bit-level classification avoids unintentionally triggering host floating traps.
The scalar's quiet/sign bits are handled explicitly and deterministically in
all profiles; native platform NaN propagation is not the specification.

## Common interface and execution

Each node consumes numeric tensor data from a Result input and publishes a
Result on output port `values`. Each input Result contains exactly one tensor
member in slot 0. Its schema ID may vary.
Structural input validation occurs before issue. The tensor sample shape has rank 1..8, positive extents, and at
most 2^40 elements. The
individual operation declares its accepted dtypes and output mapping. No
implicit integer/float conversion, shape change, physical units or color/alpha
inference is introduced. The output schema is `photospider.tensor`, with tensor
key `samples`, the preserved shape, and no attached domain description.

Metadata specialization checks dtype, complete shape and static parameters. Each requested output tile lowers to one fixed CPU task with its intersecting input tiles. Prepare checks the exact views and scratch contract. Empty selection emits no task and produces empty coverage. Sparse selection computes only selected tiles, using admitted affine inputs and separate preallocated dense outputs. Output owners may outlive the context; accepted callbacks retain every borrowed owner until return.

The caller supplies representable integer results and positive rational denominators. These are unchecked numerical preconditions, without a promised error outcome when violated. Floating special-value semantics remain as specified. See [fixed mathematical execution](../point-math-execution.md) for invocation and resource details.

## Rounding, versions and resources

Strict finite arithmetic uses the individual function's exact mathematical
definition and stated destination rounding. Floating round-to-nearest/ties-even
and gradual underflow are the default except for deliberately directed rounding
functions such as floor/ceil. Semantic infinities/NaNs are formed as specified,
without depending on the host's floating trap state or libm diagnostic strings.
Preserve and restore the caller/thread floating environment, including existing
status flags; the API communicates defined special values via output, not by
leaking newly raised flags. No global environment changes are allowed.

Simple exact transforms must be bitwise identical across the three versions.
Transcendental or approximate algorithms require separately confirmed tolerances;
IEEE classifications, signed-zero rules and NaN payload handling remain exact
even if finite numeric results permit a tolerance. Platform-specific keys on
unsupported hosts return BackendUnavailable, without silent key replacement.

For selected tiles, output payload is the sum of their dense byte capacities. A fixed aggregate contains exact/certified arithmetic state and bounded batch buffers, reserved as Host scratch per active worker lane. Inputs retain their original immutable owners. Work is proportional to selected samples and their numerical refinement. Cancellation is queried before each batch of at most 64 samples and inside long arithmetic; checkpoints do not charge a dynamic sample-work budget. Bounded unresolved refinement may fail with `ResourceExhausted`. Managed capacity is not an RSS bound.

## Common acceptance obligations

Each function has its own analytic/special-value oracle and integer boundary
cases where relevant. Its public WorkflowDocument test must run through Compiler
and ExecutionContext with actual bindings and named output, including nonzero/
disjoint requests, exact fixed reads, altered input bits, strided inputs, concurrent Runs, cancellation and allocation failure. Check NaN results
by bits, not numeric equality; check infinities and zero signs explicitly.
Strict/accelerated identity or finite tolerance must be tested separately from
classification/payload rules and exact dependency mapping.

The maintained implementation is provided by `numeric_unary.cpp` and the public
`photospider/ops/numeric/unary.hpp` entry points. The runnable workflow and current
validation boundary are documented below; this does not change the Proposed status.

## Exact rational pi input counterparts

[NUM-04S..V](NUM-04_rational_pi_contract.md) add independently named
sinpi_rational, cospi_rational, tanpi_rational and sincpi_rational. Their exact
p/q inputs use two Int64 ports and a separate interface/execution contract;
they do not inherit this file's one-floating-input shape/dtype rule. Existing
floating pi-multiple functions remain separate. Reduced common-angle denominator
1/2/3/4/6 paths are exact for the three new trigonometric functions in all profiles.

- [NUM category](../core.md).
- [Operator template](../../00-foundation/spec-template.md).
- [Current abs implementation](../../../../plugins/ops/01-numeric/numeric_abs.cpp).

## Maintained implementation and validation

The 66 maintained unary keys are registered in `plugins/ops/01-numeric/numeric_unary.cpp`
and exposed through `photospider/ops/numeric/unary.hpp`. The helpers author Result
workflows. Each input port accepts a Result with a supported numeric tensor in
slot 0; metadata specialization requires matching input dtype and complete
sample shape. It does not require a particular input schema ID.
Each requested output tile uses a fixed ABI 8 invocation with complete declared read views and preallocated output/scratch. Ordinary unary operations preserve dtype; rational-pi helpers use explicit Float32/Float64 output selection. Metadata specialization and prepare validate static contracts before issue. Numerical integer/rational preconditions belong to the caller. IEEE nonfinite results follow the member rules and succeed. Results retain immutable backing beyond context lifetime.

Bit-level special cases precede controlled elementary arithmetic. Strict transcendental results use directed Q128..Q4096 enclosures. Accelerated results use private SIMD kernels or conservative enclosures within the admitted ranges. Pi and rational-pi arguments undergo exact quadrant reduction before approximation. Rejected candidates use exact evaluation; unresolved rounding may return `ResourceExhausted`. See [fixed mathematical execution](../point-math-execution.md).
