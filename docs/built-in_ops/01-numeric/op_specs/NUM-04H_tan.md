---
spec_schema_version: 1
id: NUM-04H
parent_id: NUM-04
function: tan
proposed_operation_keys:
  - numeric.tan_strict
  - numeric.tan_accelerated_apple_silicon
  - numeric.tan_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04H: tan

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute tan(x), interpreting x as an exact floating-point value in radians. Inherit all interface, rounding, error,
resource, demand, backend and acceptance requirements from the
[trigonometric contract](NUM-04_trigonometric_contract.md) and
[unary contract](NUM-04_unary_contract.md).

Input/output dtype is Float32 or Float64 and shape is unchanged. The named
output is values, with empty facets; there are no static numeric parameters.

## Function-specific values

| Input | Output |
| --- | --- |
| +0 / -0 | Same signed zero |
| ±Inf | Fixed positive quiet NaN |
| NaN | Quiet input NaN, preserving payload/sign |

Other finite inputs use the shared correctly rounded strict or <=4-ULP
accelerated contract, with the shared special-value requirements.
Do not infer a pole from proximity to a rounded radian pi/2. This function
uses the actual finite input; the separate tanpi operation can express an exact pole.
The final tan error includes range reduction and any division/polynomial error;
separate four-ULP allowances for a sine and cosine followed by division do not
establish four ULP for tangent. Strict reference overflow, if present, produces
the correctly signed infinity as a successful numeric output; accelerated must
match the reference classification.

## Acceptance and current status

The conceptual public fixture is `[+0,-0]` -> `[+0,-0]`.
Build a WorkflowDocument with that floating array bound to input,
compile one selected key and inspect output bits
through ExecutionContext. Repeat in both dtypes and platform profiles, including
disjoint requests and exact source-read witnesses.

For a nontrivial finite fixture, input=[1] represents exactly one radian.
Strict tan(1) has Float32 bits `0x3fc75923` and Float64 bits
`0x3ff8eb245cbee3a6`. Accelerated results obey the shared four-ULP bound;
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
