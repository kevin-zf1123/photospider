---
spec_schema_version: 1
id: FRQ-09
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
parent_id: 05-filter
members:
- FRQ-09A
- FRQ-09B
research_sources:
- S13
---

# FRQ-09:DCT I-IV

A separable 2D mathematical transform; `ortho` must not be mistaken for a single universal scale factor.

Inherits [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-09A | [2D DCT](FRQ-09A_dct2.md) | primitive | E |
| FRQ-09B | [Inverse 2D DCT](FRQ-09B_idct2.md) | primitive | E |

Letter suffixes identify distinct mathematical or interface objects, not quality tiers that a backend may choose arbitrarily. Primitives may propose separately named strict and accelerated variants. Helpers only orchestrate explicit stages. An external engine requires fixed resources and approval before it can have an executable configuration.

## Shared Mathematics, Units, and Profiles

### Exact One-Dimensional Matrix Definitions
k,n=0..N-1 .
- Type I: A_kn=cos(πkn/(N-1))*w_n, with w_0=w_(N-1)=1 and all other w_n=2, for N>1.
- II:A_kn=2cos(πk(2n+1)/(2N)).
- Type III: A=1 when n=0; otherwise A=2cos(π(2k+1)n/(2N)).
- IV:A_kn=2cos(π(2k+1)(2n+1)/(4N)).
For backward normalization, the 2D DCT is A_y X A_xᵀ, evaluated as one exact whole-image expression and rounded once with RN_t. Use the true inverse matrices on each axis: type I inverse=A/[2(N-1)]; type II inverse=type III/[2N]; type III inverse=type II/[2N]; type IV inverse=type IV/[2N].
Ortho matrices: type I is sqrt(2/(N-1))*c_k*c_n*cos(πkn/(N-1)), where endpoint c values are 1/sqrt2 and others are 1; type II is sqrt(2/N)*c_k*cos(πk(2n+1)/(2N)), where c_0=1/sqrt2; type III is the transpose of ortho type II; type IV is sqrt(2/N)*cos(π(2k+1)(2n+1)/(4N)). Each ortho inverse is the transpose of its selected matrix. Trigonometric and square-root values are not pre-baked as Float64 coefficients; recognize exact integer and half-integer mathematical phase cases exactly.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics; do not apply one alpha-blurring rule to all of them. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines actual dependencies and dirty propagation.

## Dependencies and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently; paged access does not remove a whole-image dependency. Follow each member definition for staged rounding, global connectivity, transforms, and iterative state. Independently requested outputs must not turn validation of an unrequested component payload into a hidden whole-image dependency.

All members must follow the [common acceptance protocol](FILTER_oracle_protocol.md). See the [oracle usage guide](../../../../oracle/ops/filter/README.md) for mathematical references and associated self-tests. D1/D2 indicate draft maturity, not acceptance rates.

## Sources and Review

[S13 · SciPy dct](../research-sources.md#s13)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
