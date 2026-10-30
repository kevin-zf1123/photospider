---
spec_schema_version: 1
id: FMT-09A
parent_id: FMT-09
function: decode_transfer
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.transfer_decode_strict
  - color.transfer_decode_accelerated_apple_silicon
  - color.transfer_decode_accelerated_x86_64
---

# FMT-09A: decode transfer

Inherits the [FMT-09 contract](FMT-09_transfer_contract.md) and [scalar definitions](FMT-09_transfer_math.md). This direction has strict, accelerated Apple Silicon and accelerated x86-64 CPU keys.

## Interface

One input Result with one tensor member produces one `values` output Result containing one tensor. The member uses Float32 or Float64, retains its dtype, shape and axis positions, and preserves Result schema identity, tensor key and batch axes. The output carries no description and `atomic_trailing_axes` is zero. Complete sample rank, including batch and cell axes, is at most 8; sample count is at most 2^40. Axis indices refer to cell axes and exclude the Result batch prefix. Explicit `input_description` and `group` select one RGB/Gray group; raw uses explicit component selection. A may resolve the complete curve and parameters from input_description; explicit assertions must match that description. Raw always specifies them. No source interpretation is inferred from primaries, channel count or dtype.

## Mathematics

For every participating coordinate q, evaluate RN_dtype(D(input[q])). Semantic callers establish the family input-domain and finite-output preconditions. Raw uses the full IEEE formula. Linear and gamma=1 copy bits; nonparticipating coordinates also copy exactly. There is no alpha-dependent hidden-color shortcut.

Decode produces the corresponding linear quantity: PQ/1886 return absolute display-linear cd/m^2; OETF and ACES paths return their specified scene-relative quantity. No output description is attached; subsequent operations are configured explicitly with the resulting units, transfer, basis and white.

## Behavior

Static incoherence can fail even for bypass-only or Empty requests. Semantic sample-domain violations have no promised runtime diagnostic. Auto/materialize write independent dense output tiles. Nonidentity forced view fails statically; nonempty identity view returns ViewUnavailable. Empty requests retain static checks and emit no payload.

## Acceptance

1. Gamma=2: [-2,-0,0.5,2] -> [-4,-0,0.25,4], exact in both dtypes. Finite positive gamma is an unchecked premise in every mode.
2. sRGB: compare threshold neighbors around 0.04045 against the independently evaluated rational/power formula. Negative magnitudes mirror signs. B(A(x)) is not a branch oracle.
3. 709/2020: test both sides and ownership of 4.5*beta for every variant. rounded_10bit and 709 produce equal values but different transfer identity. Smooth coefficients are checked against independent root isolation.
4. BT.1886 with Lb=1,Lw=100: D(0)=1 and D(1)=100 exactly; finite values outside [0,1] are outside the semantic input precondition. Raw sufficiently negative V reaches the exact zero plateau. The condition 0<=Lb<Lw is an unchecked caller premise.
5. PQ: D(0)=0 and D(1)=10000; values on the exact zero plateau yield +0. Negative or above-one values are outside the semantic precondition; raw evaluates the formula. Check a high-precision oracle around c1^m2.
6. HLG: D(0)=0; D(0.5)=RN_dtype(1/12). Float64 D(1) is approximately 1.0000000269348073; feeding that into semantic B violates its domain precondition. Float32 D(1) rounds to 1. This dtype difference is deliberate.
7. ACEScc/cct: code 2 decodes to exactly 65504. Check neighbors of h and toe breakpoints, below-floor encoded values, and negative cct toe values. The cap is not a Float16 output restriction.
8. Output tiles separating participating and bypass components: bypass-only tiles copy NaN alpha/AOV bits. Alpha=0 does not waive semantic color preconditions.
9. Raw identity copies NaN payloads and signed zeros under auto/materialize; semantic identity requires finite participating samples. Forced PQ view fails even for bypass-only requests. Linear/gamma=1 identity view fails at invocation.

The curve's scalar definition is separate from RGB basis conversion and scene/display reference conversion. An explicit D -> basis/reference stage -> E workflow composes these operations.
