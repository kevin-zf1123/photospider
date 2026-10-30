---
spec_schema_version: 1
id: FRQ-09
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-09A
- FRQ-09B
research_sources:
- S13
---

# FRQ-09: DCT I-IV

A separable 2D mathematical transform; `ortho` must not be mistaken for a single universal scale factor.

Inherits [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-09A | [2D DCT](FRQ-09A_dct2.md) | primitive | F |
| FRQ-09B | [Inverse 2D DCT](FRQ-09B_idct2.md) | primitive | F |

Letter suffixes identify forward and inverse transforms. Each member has one unsuffixed native key and uses the F profile below.

## Mathematics

### Exact One-Dimensional Matrix Definitions
k,n=0..N-1 .
- Type I: A_kn=cos(πkn/(N-1))*w_n, with w_0=w_(N-1)=1 and all other w_n=2, for N>1.
- II:A_kn=2cos(πk(2n+1)/(2N)).
- Type III: A=1 when n=0; otherwise A=2cos(π(2k+1)n/(2N)).
- IV:A_kn=2cos(π(2k+1)(2n+1)/(4N)).
For backward normalization, the 2D DCT is A_y X A_xᵀ, evaluated with ordinary native floating-point arithmetic under the F profile. Use the true inverse matrices on each axis: type I inverse=A/[2(N-1)]; type II inverse=type III/[2N]; type III inverse=type II/[2N]; type IV inverse=type IV/[2N].
Ortho matrices: type I is sqrt(2/(N-1))*c_k*c_n*cos(πkn/(N-1)), where endpoint c values are 1/sqrt2 and others are 1; type II is sqrt(2/N)*c_k*cos(πk(2n+1)/(2N)), where c_0=1/sqrt2; type III is the transpose of ortho type II; type IV is sqrt(2/N)*cos(π(2k+1)(2n+1)/(4N)). Each ortho inverse is the transpose of its selected matrix. Native phase and normalization tables approximate these mathematical constants in the working dtype; the selected mathematical type and endpoint normalization remain fixed.

## Numeric Requirements

F is ordinary native floating-point computation, consistent with FFT/IFFT. Float32 uses binary32 input, output and working arithmetic; Float64 uses binary64 throughout and is never narrowed to Float32. Shape-dependent phase and normalization coefficients may be generated in binary64 and converted to the working dtype. Explicit FMA is permitted. Correct rounding, a fixed ULP bound and bitwise reproducibility across algorithms or ISAs are not promised. NaN, infinity, signed zero, underflow and overflow follow the selected native arithmetic and established floating-point environment; numerical payload premises remain unchecked.

This family-local F profile replaces inherited E/S-strict accuracy requirements without changing NUM itself. Matrix definitions, inverse conventions, endpoint weights, axes, shapes and support remain normative.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

The member support set is normative: each output depends on the whole plane of each transformed axis. Numerical checks use documented absolute/relative error criteria under the local F profile, per the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S13 · SciPy dct](../research-sources.md#s13)
