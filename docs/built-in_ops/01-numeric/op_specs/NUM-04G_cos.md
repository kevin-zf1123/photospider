---
spec_schema_version: 1
id: NUM-04G
parent_id: NUM-04
function: cos
proposed_operation_keys:
  - numeric.cos_strict
  - numeric.cos_accelerated_apple_silicon
  - numeric.cos_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04G: cos

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute cos(x), interpreting x as an exact floating-point value in radians. Inherit all interface, rounding, error,
resource, demand, backend and acceptance requirements from the
[trigonometric contract](NUM-04_trigonometric_contract.md) and
[unary contract](NUM-04_unary_contract.md).

Input/output dtype is Float32 or Float64 and shape is unchanged. The named
output is values, with empty facets; there are no static numeric parameters.

## Function-specific values

| Input | Output |
| --- | --- |
| +0 / -0 | Exactly 1 |
| ±Inf | Fixed positive quiet NaN |
| NaN | Quiet input NaN, preserving payload/sign |

Other finite inputs use the shared correctly rounded strict or <=4-ULP
accelerated contract, with the shared special-value requirements.
Cosine is even numerically; input NaN sign preservation remains the common
payload rule rather than an arithmetic evenness claim. Output must stay in
[-1,1] as well as meeting its ULP bound. An unproved algorithm followed by
clamping is not a substitute for the error contract.

## Acceptance and current status

The conceptual public fixture is `[+0,-0]` -> `[1,1]`.
Build a WorkflowDocument with that floating array bound to input,
compile one selected key and inspect output bits
through ExecutionContext. Repeat in both dtypes and platform profiles, including
disjoint requests and exact source-read witnesses.

For a nontrivial finite fixture, input=[1] represents exactly one radian.
Strict cos(1) has Float32 bits `0x3f0a5140` and Float64 bits
`0x3fe14a280fb5068c`. Accelerated results obey the shared four-ULP bound;
this fixture does not turn 1 radian into a special-angle identity requirement.
Independent alternating-series rational enclosures for sin(1)/cos(1), with
positive-denominator interval division for tan(1), and direct destination
rounding establish these reference bits without using the production libm.

Also test ordinary small/large finite radian inputs, signed zero, NaN payloads,
infinity, neighbors of rounded pi multiples, independent correct
rounding and accelerated ULP/classification. Apply all shared resource, cache,
lifetime and cancellation cases. A real public executable, run commands and
platform measurements belong to implementation delivery. These new keys are
not currently registered, and no implementation tests are claimed here.
