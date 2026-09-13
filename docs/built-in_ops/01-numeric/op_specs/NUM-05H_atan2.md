---
spec_schema_version: 1
id: NUM-05H
parent_id: NUM-05
function: atan2
proposed_operation_keys:
  - numeric.atan2_strict
  - numeric.atan2_accelerated_apple_silicon
  - numeric.atan2_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-05H: atan2

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inputs are named `y` (first operand) and `x` (second operand), with identical
shape and dtype, Float32 or Float64. Output `values` preserves shape and dtype,
has empty facets, and expresses the oriented angle in radians.
No static angle-unit parameter or degree output exists. Inherit the
[binary contract](NUM-05_binary_contract.md), substituting y/x for a/b in
both-operand support and NaN priority. Integer inputs require explicit cast.

## Mathematical and special-value semantics

For finite inputs away from the origin, the principal angle theta is the polar
argument of x+i*y with range [-pi,pi], selecting the signed branch as below.
Strict rounds the mathematical `atan2(y,x)` directly to output dtype. The
normalized operation never divides an already-rounded radian angle by a rounded
pi. Both inputs are read and validated even when a special case fixes the result.

If either input is NaN, quiet and preserve it; if both are NaN, y's payload/sign
wins. Otherwise apply the following exact table. Here s is the sign of y,
including signed zero; s*0 means zero with that sign. Every listed result is a
successful numeric output. For atan2 return RN_dtype(theta); for atan2pi return
the listed exact theta/pi, without an intermediate rounded theta.

| Condition | theta/pi |
| --- | --- |
| y=±0 and x positive or +0 | s*0 |
| y=±0 and x negative or -0 | s*1 |
| x=±0 and y finite nonzero | s*1/2 |
| y=±Inf and x finite | s*1/2 |
| y=±Inf and x=+Inf | s*1/4 |
| y=±Inf and x=-Inf | s*3/4 |
| y finite and x=+Inf | s*0 |
| y finite and x=-Inf | s*1 |

The zero-y rows also cover infinite x according to its sign. In particular the
four signed-zero origin combinations retain directional information. Ordinary
finite nonzero inputs use their exact real ratio/direction; avoid an overflowing
or underflowing y/x intermediate that destroys quadrant or accuracy information.

## Numerical quality, implementation and resources

Accelerated finite nonzero outputs are within four output-dtype ULP of strict.
NaN/Inf/zero classification and signs match strict, and all entries of the table
match the strict rounded output exactly. Apply [exp's fallback, diagnostic and
refinement rules](NUM-04D_exp.md), including per-profile independence from vector
width/batching, budget accounting and ResourceExhausted on unresolved rounding.
An approximate atan2 followed by division does not by itself prove the atan2pi
bound. Validate the final operation, using exact special cases separately.

The mathematical range does not justify clamping rounded radians to a floating
approximation of pi: correctly rounded endpoints and neighboring values follow
the rounding contract. For atan2pi, outputs remain within [-1,1] in every profile.

LLVM libc's [current status table](https://libc.llvm.org/headers/math/index.html),
reviewed 2026-09-14, labels atan2f correctly rounded, atan2 double with a largest
recorded error of one ULP, and leaves atan2pi empty. These are candidate-selection
facts, not a verified full-range bound for a pinned Photospider backend. Float64
strict and normalized variants require separately established implementations.

## Acceptance and current status

Conceptual public fixture: y=[+0,-0,1,-1], x=[-0,-0,+0,+0]. The normalized
outputs are [1,-1,0.5,-0.5]; radian outputs are independently correctly rounded
[pi,-pi,pi/2,-pi/2]. Check all sign combinations at axes, origin and infinities,
NaN payload priority, huge and subnormal ratios, quadrant boundaries, diagonal
neighbors and hard rounding cases. Use an independent high-precision angle
oracle with direct destination rounding and separately explicit special bits.

Exercise shared disjoint demand, both upstream dependencies, typed-validation
closure, cache/lifetime, resource exhaustion, cancellation and actual fallback
through public WorkflowDocument execution when implemented. No versioned key,
runtime result or platform benchmark is claimed by this specification.
