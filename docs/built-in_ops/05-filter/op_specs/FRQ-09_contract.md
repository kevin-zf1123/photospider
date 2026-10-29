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

# FRQ-09:DCT I-IV

A separable 2D mathematical transform; `ortho` must not be mistaken for a single universal scale factor.

Inherits [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-09A | [2D DCT](FRQ-09A_dct2.md) | primitive | F |
| FRQ-09B | [Inverse 2D DCT](FRQ-09B_idct2.md) | primitive | F |

Letter suffixes identify forward and inverse transforms. Each member has one registered, unsuffixed native key and uses the F profile below; CPU strategy selection does not create a separate accuracy tier.

### Exact One-Dimensional Matrix Definitions
k,n=0..N-1 .
- Type I: A_kn=cos(πkn/(N-1))*w_n, with w_0=w_(N-1)=1 and all other w_n=2, for N>1.
- II:A_kn=2cos(πk(2n+1)/(2N)).
- Type III: A=1 when n=0; otherwise A=2cos(π(2k+1)n/(2N)).
- IV:A_kn=2cos(π(2k+1)(2n+1)/(4N)).
For backward normalization, the 2D DCT is A_y X A_xᵀ, evaluated with ordinary native floating-point arithmetic under the F profile. Use the true inverse matrices on each axis: type I inverse=A/[2(N-1)]; type II inverse=type III/[2N]; type III inverse=type II/[2N]; type IV inverse=type IV/[2N].
Ortho matrices: type I is sqrt(2/(N-1))*c_k*c_n*cos(πkn/(N-1)), where endpoint c values are 1/sqrt2 and others are 1; type II is sqrt(2/N)*c_k*cos(πk(2n+1)/(2N)), where c_0=1/sqrt2; type III is the transpose of ortho type II; type IV is sqrt(2/N)*cos(π(2k+1)(2n+1)/(4N)). Each ortho inverse is the transpose of its selected matrix. Native phase and normalization tables approximate these mathematical constants in the working dtype; the selected mathematical type and endpoint normalization remain fixed.

## Shared Execution and Numeric Requirements

F is ordinary native floating-point computation, consistent with FFT/IFFT. Float32 uses binary32 input, output and working arithmetic; Float64 uses binary64 throughout and is never narrowed to Float32. Shape-dependent phase and normalization coefficients may be generated in binary64 and converted to the working dtype. Explicit FMA, FFT reductions, vectorization and arithmetic fusion are permitted. Correct rounding, a fixed ULP bound and bitwise reproducibility across algorithms or ISAs are not promised. NaN, infinity, signed zero, underflow and overflow follow the selected native arithmetic and established floating-point environment; numerical payload premises remain unchecked.

This family-local F profile replaces inherited E/S-strict accuracy requirements without changing NUM itself. Matrix definitions, inverse conventions, endpoint weights, axes, shapes, support, discrete choices, resource admission, cancellation, publication and owner lifetime remain normative.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics; do not apply one alpha-blurring rule to all of them. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

### FFT-based implementation constraints

The native implementation applies separable one-axis transforms with dtype-specific plans and CPU kernels. Types II and III use a reordered real FFT and its inverse; even lengths reduce to N/2 complex points. Type I uses an even extension of length 2(N-1), reduced to N-1 complex points, or a real even/odd split when N-1 is a power of two and 17<=N<=1025. The latter recursively combines DCT-I and DCT-III branches with expanded small leaves; input/output endpoint weights and the selected normalization remain the same. Even-length type IV uses an N/2 complex reduction with phase factors; odd lengths use the corresponding extended real transform. Type and normalization branches are selected outside the element loops. Scalar paths cover other supported CPU configurations and dimensions.

Reserve immutable tables, per-worker FFT workspace and unpublished output under the shared resource model. Every worker must drain on success, failure or cancellation before returning. No precision-refinement dispatcher or strict numerical fallback is required.

## Dependencies and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently; paged access does not remove a whole-image dependency. Follow each member's transform, native numerical profile and whole-plane dependencies. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members follow structural, resource, lifecycle and schema checks in the [common acceptance protocol](FILTER_oracle_protocol.md). Numerical checks use documented absolute/relative error criteria under the local F profile. Preserve the mathematical references in the [oracle usage guide](../../../../oracle/ops/filter/README.md). D1/D2 and Proposed describe specification maturity separately from implementation and acceptance.

Implementation and registration are complete; this does not establish complete numerical, runtime or performance acceptance.

## Sources and Review

[S13 · SciPy dct](../research-sources.md#s13)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and registered operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
