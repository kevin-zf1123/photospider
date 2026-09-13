---
spec_schema_version: 1
id: CRV-01
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: legacy_subset_only_target_not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-01: interpolation family

This family records the completed clarification of four independent interfaces.
Each selected operator has a separate primitive specification, rather than a
generic mode-dispatch operation. No new
registration, runtime behavior or acceptance result is claimed here.

## Confirmed purpose and split

Interpolate supplied known points to evaluate one-dimensional functions at
explicit dynamic query positions. The curve passes through its known points;
this input model differs from CRV-02's Bezier anchors and off-curve handles.
Queries can be supplied by a numeric sequence generator or an arbitrary
position array rather than being generated internally from a static domain.

The maintainer selected separate single-function and multiple-function
implementations/specifications:

- Single-function interface: x[K], y[K], query[N] -> values[N].
- Multiple-function interface: x[K], y[K,C], query[N] -> values[N,C],
  with multiple functions sharing the knot/query x coordinates. The independent
  multi-function specifications define column-local support and product limits.

Do not overload one operation with both ranks or silently squeeze/insert the
function axis. Linear and PCHIP interpolation also use independent operation
names, with their separate multiple-function counterparts. There is no static
method switch.

The single-function linear contract is
[CRV-01A](CRV-01A_interpolate_linear.md), whose clarification is complete.
The single-function PCHIP contract is
[CRV-01B](CRV-01B_interpolate_pchip.md), whose clarification is complete.
The independent multi-function variants are
[CRV-01C linear](CRV-01C_interpolate_linear_multi.md) and
[CRV-01D PCHIP](CRV-01D_interpolate_pchip_multi.md), both fully clarified.
All four target specifications remain Proposed and unimplemented.
No provisional recommendation in the category index becomes a confirmed rule
merely through this family record.

## Existing implementation facts

Source inspected at the front-matter commit:

- [curve.sample_linear](../../../../plugins/ops/01-numeric/curve_sample_linear.cpp)
  and [curve.sample_monotone](../../../../plugins/ops/01-numeric/curve_sample_monotone.cpp)
  each use one Float32/Float64 controls[K,2] input and Whole output execution.
- Static domain_min/domain_max and count generate an increasing uniform grid;
  count is 2..1048576 and out_of_domain is reject/clip. Output is a rank-1
  array with input dtype. These are not explicit-query input interfaces.
- The [shared implementation](../../../../plugins/ops/01-numeric/curve_common.hpp)
  validates every x/y control as finite and requires increasing x. The monotone
  path forms rounded Float64 slopes and clips computed values to local endpoint
  bounds. Its current rounding and demand behavior do not establish the future
  versioned CRV-01 contracts.

## Related specifications

- [Curve category](../curves.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
- [Existing implementation summary](../../../kernel-architecture/Basic-Operations.md).
- [CRV-02 sampled Bezier function](CRV-02_sample_bezier_function.md).
