---
spec_schema_version: 1
id: NUM-11G
parent_id: NUM-11
function: reduce_std
proposed_operation_keys:
  - numeric.reduce_std_strict
  - numeric.reduce_std_accelerated_apple_silicon
  - numeric.reduce_std_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-11G: reduce_std

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inherit [reduce_variance](NUM-11F_reduce_variance.md) for input/values ports,
all four input dtypes, Float32/Float64 output (default Float64), required axes,
nonnegative static ddof (default 0), N>ddof compile/preflight validation,
fixed keepdims, selected-group demand, nonfinite cases, NaN payload mapping,
resources, errors and ownership. Static dtype/ddof are explicit in direct nodes.

For finite data, define the exact mathematical variance V as in that contract,
then return RN_dtype(sqrt(V)). There is no intermediate rounded variance or
rounded mean. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound. Finite constant groups return +0. Any NaN or infinity
follows variance's exceptional-value rules before root evaluation.

Use exact moments and a certified correctly rounded rational-square-root method,
or directed enclosures with exact boundary handling. A rounded native sqrt of
an already rounded variance is not this operation. Exact positive results may
round to a subnormal, +0 or +Inf according to output dtype. Charge all moment,
root/refinement and limb storage/work and return ResourceExhausted if correct
rounding cannot be established within the budget.

Fixture: input=[1,2,3], axes="0", dtype="float64": ddof=0 yields
[RN_Float64(sqrt(2/3))], ddof=1 yields [1]. For Float64 [MAX,-MAX] with ddof=0,
standard deviation is exactly MAX even though rounding its variance to Float64
would overflow. This fixture detects an incorrect variance-then-sqrt composition.
Test tiny moments that would prematurely underflow, large common offsets, integer
sources, exact root boundaries, all NaN/Inf/zero cases, and inherited resource,
region, cancellation and owner-lifetime behavior using the independent root
oracle and public manual target. The current three profile keys use
`reduce_std_node` from `photospider/numeric/reductions.hpp` and compute exact
variance before the correctly rounded square root; they do not compose a
rounded variance with a native sqrt. The shared reduction contract records the
complete strict/Apple/WSL and installed-consumer evidence. Proposed status is
unchanged.
