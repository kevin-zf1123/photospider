---
spec_schema_version: 1
id: NUM-04
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04: shared unary contract

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
| [NUM-04C sqrt](NUM-04C_sqrt.md) | Float32/64 | Correct rounding, bitwise strict equivalence |
| [NUM-04D exp](NUM-04D_exp.md) | Float32/64 | <=4 output-dtype ULP for nonzero finite results |
| [NUM-04E ln](NUM-04E_ln.md) | Float32/64 | <=4 output-dtype ULP for nonzero finite results |
| [NUM-04F sin](NUM-04F_sin.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04G cos](NUM-04G_cos.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04H tan](NUM-04H_tan.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04I floor](NUM-04I_floor.md) | All four | Bitwise strict equivalence |
| [NUM-04J ceil](NUM-04J_ceil.md) | All four | Bitwise strict equivalence |
| [NUM-04K round](NUM-04K_round.md) | All four | Bitwise strict equivalence, ties-to-even |
| [NUM-04L sign](NUM-04L_sign.md) | All four | Bitwise strict equivalence |
| [NUM-04M reciprocal](NUM-04M_reciprocal.md) | Float32/64 | Correct rounding, bitwise strict equivalence |
| [NUM-04N sinpi](NUM-04N_sinpi.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04O cospi](NUM-04O_cospi.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04P tanpi](NUM-04P_tanpi.md) | Float32/64 | <=4 ULP plus exact named landmarks |
| [NUM-04Q sinc](NUM-04Q_sinc.md) | Float32/64 | <=4 ULP for the whole mathematical function |
| [NUM-04R sincpi](NUM-04R_sincpi.md) | Float32/64 | <=4 ULP for the whole mathematical function |

For radian functions and independent pi-multiple functions, also inherit the
[trigonometric contract](NUM-04_trigonometric_contract.md). These files define
target behavior; the legacy current registry remains a separate implementation
fact, including its different finite-only abs policy.

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

Each node has one generic numeric Value input `input` and one generic Value
output `values`. Shape is preserved exactly, with rank 1..8 and positive extents
under the current Value/Region representation. Dtype support/output mapping is
declared by the individual operator. No implicit integer/float conversion,
shape change, physical units or color/alpha inference is introduced. Output
facets are empty. Input facets retain existing metadata and actually observed
typed-semantic validation; an invalid typed NaN does not become valid just
because generic numeric NaNs are allowed by the arithmetic profile.

For requested output Q, Data support is the identical input-coordinate set Q.
Recognized input semantics may add required validation support, such as full
image channels. Declare that closure separately, retain it and do not silently
read a bounding gap for disjoint Q. Empty Q reads no input data. Unrequested
numeric exceptions do not run and have no effect on other observations.

Use regional dependency execution wherever necessary to express this exact
support and typed validation. Changed input Data invalidates the corresponding
output coordinates; changed validation inputs invalidate the observations that
retained them. No blanket Whole validation pass is added. Output descriptors
depend on input metadata and static numeric profile, not runtime numeric values.

Read legal immutable strided/offset/unaligned inputs at logical coordinates.
Publish packed owned output fragments covering requested global coordinates,
with correct Region/storage origins and element-size strides. Do not expose
writable aliases or implicit zero-filled gaps. Published owners may outlive
the context; release unpublished work on failure and published bytes at the
final owner. SIMD tails cannot read unrequested input coordinates.

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

For M requested elements and destination size b, output payload is M*b; requested
input fragments, validation closure and O(rank) coordinate state are additionally
accounted. Simple transforms cost O(M); the particular spec must account for
nonconstant math work or exact-rounding refinement. No full logical array
allocation is required for a small request. Use host workers, allocator,
admission/work/stage limits and actual capacity accounting, including temporary
growth overlap. Bound scalar/SIMD batches and poll cancellation before reads,
at least every 64 simple elements, within long math/refinement work and before
publication. Resource failures are sticky, not reasons to return a weaker value.

Cache identities include operation/profile version, input metadata and exact
observed bits, including NaN payload/sign. Required validation and upstream
identities survive reuse; changed inputs with numerically equal outputs do not
erase witnesses. Cache-off preserves active ownership and execution semantics.
No private worker pool, implicit disk backing or persistent mutable numeric state
is required. Managed payload/scratch limits are not an RSS guarantee.

## Common acceptance obligations

Each function has its own analytic/special-value oracle and integer boundary
cases where relevant. Its public WorkflowDocument test must run through Compiler
and ExecutionContext with actual bindings and named output, including nonzero/
disjoint requests, input read witnesses, cache-off/warm runs, altered input bits,
strided inputs, cancellation and allocation/work/stage failure. Check NaN results
by bits, not numeric equality; check infinities and zero signs explicitly.
Strict/accelerated identity or finite tolerance must be tested separately from
classification/payload rules and exact dependency mapping.

No new keys in this family are currently claimed as implemented or runnable.
Implementation delivery supplies actual executable targets and run commands;
specification-only mathematical/bit-pattern experiments are not product acceptance.

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
