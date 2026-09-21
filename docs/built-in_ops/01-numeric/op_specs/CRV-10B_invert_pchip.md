---
spec_schema_version: 1
id: CRV-10B
parent_id: CRV-10
function: invert_pchip
operation_family: curve.invert_pchip
proposed_operation_keys:
  - curve.invert_pchip_strict
  - curve.invert_pchip_accelerated_apple_silicon
  - curve.invert_pchip_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-10B: invert_pchip

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Invert the mathematical PCHIP curve defined by x/y, before
its forward output rounding. Do not invert a rounded lookup or construct a new
swapped-control interpolator. The [CRV-10 shared contract](CRV-10_invert.md) is
normative for every interface, error, numerical and execution requirement.

Ordered ports x[K],y[K],query[N] independently accept Float32/Float64.
x is finite strictly increasing; y is finite strictly increasing or decreasing,
without plateaus. K=2..65536,N=1..2^40. Output values[N] is generic
Float32/Float64, static dtype default Float64. The other required static String
parameter out_of_domain is reject/clamp, default reject. No extrapolated tail
is inverted. Endpoints correctly convert corresponding x, preserving zero sign.

Use exactly the CRV-01B rational PCHIP slopes/polynomial, bracket the unique root and correctly round final x in strict. Accelerated permits <=4 ULP x error, exact classification/zero/endpoint rules, segment bounds and monotonicity; strict fallback is used when needed. Residual alone is not the quality bound.
Other mathematical exact zeros are +0 and nonzero underflow retains sign.
Demanded queries and outputs are finite, with narrowing overflow failure.

All three formal profile keys use Whole. Every nonempty request collects all
x/y/query, validates global topology and all query controls, then computes a
complete dense output. Any input edit invalidates all outputs; dynamic numerical
failures are Domain/Run, including invalid or overflowing undelivered positions.
Empty requests read no payload. Inherit CRV-10's typed/upstream closure, arbitrary
strides, ownership, cache-off, complete input/output storage, work and cancellation
rules. Lookup work is O(K+N log K) plus exact arithmetic. Numerical stencils and
rounding rules remain unchanged; unused endpoint conversion cannot reject a
finite root. No partial output is published on failure. Whole fallback counters
are N/A; the actual strict fallback remains available.

Conceptual fixture: x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> values=[0.5,1.5].
Negating y and query retains the result. Apply CRV-10's independent exact oracle,
boundary/rounding/monotonicity, dirty/partial-read, stride, budget, cancellation
and owner-lifetime acceptance. The maintained public Compiler/ExecutionContext
workflow supplies x/y/query and static dtype/policy. See the inverse-curves example linked below for commands and
current validation evidence.

## Maintained implementation and validation

The public helper is `invert_pchip_node` from
[`inverse_curves.hpp`](../../../../include/photospider/numeric/inverse_curves.hpp).
Strict and general Float64 output use the exact polynomial/lattice path.
Accelerated Float32 output tries a bracketed solver with uniquely rounded
output, then strict scalar fallback when unresolved. Exact collinear stencils
use linear inversion; knot/clamp and K=2 paths remain direct. See the [inverse-curves workflow](../../../../examples/numeric_workflow/README.md#inverse-curves)
for the public fixture, command and shared validation evidence. Native Clang21
Strict/Apple passed all four manual groups and 407 independent Fraction cases per
profile, plus focused numeric/compiler tests. WSL and installed consumers have
not been rerun for this Whole revision.
