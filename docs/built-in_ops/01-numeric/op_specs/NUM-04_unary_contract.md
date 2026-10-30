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

Numeric profile: strict is the exact reference defined by each operation. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and shared requirements; explicit rules below and in the named family contract take precedence.

This file contains decisions common to the independent unary operator specifications. It does not register a generic `numeric.unary` mode dispatcher. The operations are abs, neg, sqrt, exp, ln, sin, cos, tan, floor, ceil, round, sign, reciprocal, sinpi, cospi, tanpi, sinc and sincpi. Pow is a binary operation under NUM-05.

Each operation has independently named strict, Apple Silicon CPU accelerated and x86-64 CPU accelerated versions. Individual files state dtypes, mathematical behavior, rounding and any allowed approximation.

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

Radian functions and independent pi-multiple functions also inherit the [trigonometric contract](NUM-04_trigonometric_contract.md). The exact rational pi counterparts NUM-04S..V are defined in the [rational pi contract](NUM-04_rational_pi_contract.md); they take two Int64 ports and do not inherit this file's single floating input rule.

## Interface

Each node consumes numeric tensor data from a Result input and publishes a Result on output port `values`. Each input Result contains exactly one tensor member in slot 0; its schema ID may vary. The tensor sample shape has rank 1..8, positive extents and at most 2^40 elements. Each operation declares its accepted dtypes and output mapping. No implicit integer/float conversion, shape change, physical units or color/alpha inference is introduced. The output schema is `photospider.tensor`, with tensor key `samples`, the preserved shape, and no attached domain description.

Metadata specialization checks dtype, complete shape and static parameters. The caller supplies representable integer results and positive rational denominators; these are unchecked numerical preconditions. Floating special-value semantics are as specified.

## Nonfinite behavior

Numeric propagation is IEEE-like: NaN and infinity are allowed in inputs and outputs. This does not change the NUM-01/CRV-02 generator contracts. Domain errors and floating overflow are numeric results, not operation failures: sqrt(-1) and ln(-1) yield NaN, ln(0) yields -infinity, reciprocal of +0/-0 yields +infinity/-infinity, and exp overflow yields +infinity. These observations complete successfully. Type, resource, cancellation and upstream failures keep their Status behavior. Individual files enumerate exact cases.

Input NaNs retain their payload and are quieted if signaling. Newly generated NaN is positive quiet NaN `0x7fc00000` for Float32 or `0x7ff8000000000000` for Float64. Unless the operation changes sign (abs/neg), the input NaN sign is preserved. The quiet and sign bits are handled explicitly and deterministically in all profiles; native platform NaN propagation is not the specification.

## Rounding and profiles

Strict finite arithmetic uses the function's exact mathematical definition and stated destination rounding. Floating round-to-nearest/ties-even and gradual underflow are the default except for deliberately directed rounding functions such as floor/ceil. Semantic infinities/NaNs are formed as specified, independent of the host's floating trap state. The caller's floating environment, including existing status flags, is preserved; special values are communicated via output.

Simple exact transforms are bitwise identical across the three versions. Transcendental or approximate operations allow the stated tolerance for finite results; IEEE classifications, signed-zero rules and NaN payload handling remain exact. Platform-specific keys on unsupported hosts return BackendUnavailable.

## Acceptance

Each function has its own analytic/special-value cases and integer boundary cases where relevant. Check NaN results by bits, not numeric equality; check infinities and zero signs explicitly. Strict/accelerated identity or finite tolerance is tested separately from classification and payload rules.
