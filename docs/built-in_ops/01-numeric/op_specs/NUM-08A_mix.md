---
spec_schema_version: 1
id: NUM-08A
parent_id: NUM-08
function: mix
proposed_operation_keys:
  - numeric.mix_strict
  - numeric.mix_accelerated_apple_silicon
  - numeric.mix_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-08A: mix

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Mix matching floating elements by (1-t)*a+t*b. Dynamic inputs in order are
`a`, `b`, `t`, with identical positive rank-1..8 shapes and Float32/Float64 dtype.
Output `values` preserves dtype/shape with empty facets. There are no static
numeric parameters or implicit casts/broadcasting. A common blend factor is
explicitly broadcast to shape. This generic numeric operation does not infer
color space, alpha association or image compositing behavior.

For every requested coordinate, t must be finite and satisfy 0<=t<=1. Negative
zero is valid zero. An invalid t fails with InvalidArgument, FailureReason::InvalidDomain and diagnostic tag InvalidMixFactor,
reporting coordinate and value bits. No linear extrapolation is provided.
Unrequested factors are not checked. All three CPU profiles follow the common
NUM version policy.

## Numeric and staged-demand contract

First read/validate t on requested Q as retained Control support, with recognized
typed-validation closure. Define A={q:t[q]<1}, B={q:t[q]>0}. Read a exactly on A
and b exactly on B, plus separately required typed-validation closure. Empty Q
reads nothing. At t=0, copy a bits without reading b; at t=1, copy b bits without
reading a. Endpoint copies preserve sNaN bits rather than quieting them. Input
metadata for both branches is always checked at compile/preflight.

For 0<t<1, both values are required. Propagate a sole NaN, or a if both are NaN,
preserving sign/payload and quieting it. One infinity paired with a finite value,
or two same-sign infinities, returns that infinity. Opposite infinities yield
the fixed positive quiet NaN. These are successful numeric results.

For finite a/b in the interior, correctly round the exact real expression
(1-t)*a+t*b directly to dtype once. Every profile is bitwise equivalent. Two
negative-zero endpoints yield -0; other exact zero interior results yield +0.
A nonzero exact result rounding to zero retains its mathematical sign. Since
t is in [0,1], finite endpoints cannot produce an infinite mathematical-range
result. Avoid naive subtraction or products that create spurious overflow.

Use exact dyadic arithmetic or a certified equivalent. Input changes invalidate
selected positions and retained typed-validation witnesses; t changes invalidate
and replan the affected coordinate. Inherit [select's staged support, mapping,
owner and resource requirements](NUM-07H_select.md), using the overlapping A/B
sets above rather than disjoint binary-condition sets. Interior positions read
both endpoints. Do not broaden irregular sets to bounding boxes silently.

## Resource, error and acceptance requirements

Work is O(|Q|) plus actual source/validation and exact arithmetic work. Account
factor decisions, index/run sets, both retained source owners, output fragments
and arithmetic scratch. Publish owned packed fragments for Q with correct global
origins. Budget exhaustion or cancellation fails without publishing a partial
observation; check at least every 64 simple samples and extended-arithmetic stage.
Follow inherited cache, floating-environment and final-owner release requirements.

The public `examples/numeric_workflow/interpolation.cpp` fixture uses
`WorkflowDocument`, `Compiler`, `ExecutionContext` and explicit broadcasts
of endpoint and edge scalars. It checks `a=[10,10,10]`, `b=[20,20,20]`,
`t=[0,0.25,1] -> [10,12.5,20]`, exact selected supports `{0,1}` and `{1,2}`,
typed-validation closure, cache branch replacement, layouts/ROI, empty demand,
caller floating-environment preservation and cleanup under WorkLimit and
cancellation. `interpolation_oracle.py` independently decodes IEEE values and
uses `Fraction` plus direct destination rounding; the local strict and Apple
profile runs passed 5242 cases per profile. Ubuntu WSL Clang strict/x86
also passed 5242 cases per profile; the installed consumer passed locally. These are manual targets without CTest or
integration-test registration, and no performance result is claimed.
