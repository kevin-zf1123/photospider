---
spec_schema_version: 1
id: NUM-04E
parent_id: NUM-04
function: ln
proposed_operation_keys:
  - numeric.ln_strict
  - numeric.ln_accelerated_apple_silicon
  - numeric.ln_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04E: ln

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute natural logarithm for Float32/Float64, preserving input shape/dtype.
Inherit the [NUM-04 common interface and execution contract](NUM-04_unary_contract.md)
and [exp's numerical-quality/fallback requirements](NUM-04D_exp.md), with exp's
mathematical formula and special-value table replaced by those below. There
is no parameter, arbitrary-base mode or `log` alias. Integer conversion is explicit.

## Numeric semantics

Strict positive finite results are RN_dtype(ln(exact(x))), directly rounded
to the output dtype. Accelerated nonzero finite results may differ by at most
four representable steps, but NaN/infinity/zero classification and sign match
strict. Use a verified argument-domain fast path or strict fallback, reporting
actual profile/platform and fallback work as for exp.

| Input | Output |
| --- | --- |
| Positive finite x other than 1 | Natural logarithm under the chosen quality profile |
| 1 | Exact +0 |
| +0 or -0 | -infinity, successful numeric observation |
| Negative finite x or -infinity | Fixed positive quiet NaN, successful observation |
| +infinity | +infinity |
| NaN | Quiet input NaN preserving payload and sign |

Special-value classification precedes any comparison/conversion that could
discard NaN bits. Do not use a domain epsilon around zero or one. Tiny positive
subnormals are valid inputs and are not flushed to zero. Host floating flags
and rounding environment are restored as specified by the common contract.

## Algorithm and acceptance differences

Strict implementation must establish destination correct rounding for the
mathematical natural log, with high-precision/directed-bound refinement or a
proved correctly rounded implementation. Near one, cancellation-sensitive
algorithms cannot replace a missing rounding proof with a small absolute error.
Accelerated argument ranges, constants/library versions and fallback predicates
must be explicit. Charge actual reduction/refinement work and scratch under the
inherited budgets, with cancellation points inside long work.

Test ln(1)=+0, both signed zeros -> -Inf, negative finite/-Inf -> canonical NaN,
+Inf unchanged, signed NaN payloads, smallest positive subnormal, powers of two,
neighbors immediately above/below one, and large positive finite inputs. Resolve
oracle rounding independently; strict compares bits and accelerated compares
ULP distance plus exact classification. Apply common ROI/read-witness/cache/
resource/ownership tests, including an unrequested domain-error sample.

Conceptual public fixture: bind `[1,+0,-0,-1,+Inf]`, compile one selected ln
key and request values -> `[+0,-Inf,-Inf,canonical_NaN,+Inf]`. This target is
not implemented; actual public run commands and platform measurements belong
to a separately authorized implementation delivery.
