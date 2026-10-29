---
spec_schema_version: 1
id: FRQ-09A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-09
function: dct2
proposed_operation_keys:
- frequency.dct2
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S13
---

# FRQ-09A: dct2

2D DCT. Status: **Proposed**.

Inherits the [FRQ-09 family contract](FRQ-09_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. The family's local F profile defines arithmetic accuracy; structural, resource and lifecycle requirements remain applicable.

## Ports, Shape, and Semantics

`input` → `output` with the same shape and dtype. The DCT coefficient-grid tag is associated with its type, norm, and axes; it is not ordinary RGB data.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

Axes are explicit; type=I|II|III|IV; norm=backward|ortho. Both transformed dimensions for type I must be >=2; other types permit length 1. There is no ambiguous `orthogonalize` option.

Special values, underflow and overflow follow the selected native execution path and established floating-point environment. No finite-only payload scan or numerical-domain operation failure is introduced. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **F**, defined by the [FRQ-09 family contract](FRQ-09_contract.md).

Evaluate the family's selected 2D separable matrix with native floating-point arithmetic. Preserve the backward matrix definition or every ortho endpoint weight; ortho is not one universal scalar multiplier.

F is ordinary native floating-point computation, consistent with FFT/IFFT. Float32 uses binary32 input, output and working arithmetic; Float64 uses binary64 throughout and is never narrowed to Float32. Shape-dependent phase and normalization coefficients may be generated in binary64 and converted to the working dtype. Explicit FMA, FFT reductions, vectorization and arithmetic fusion are permitted. Correct rounding, a fixed ULP bound and bitwise reproducibility across algorithms or ISAs are not promised. NaN, infinity, signed zero, underflow and overflow follow the selected native arithmetic and established floating-point environment; numerical payload premises remain unchecked.

This family-local F profile replaces inherited E/S-strict accuracy requirements without changing NUM itself. Matrix definitions, inverse conventions, endpoint weights, axes, shapes, support, discrete choices, resource admission, cancellation, publication and owner lifetime remain normative.

## Fixed reads, algorithmic control and schemas

complete-input demand per plane. Block DCT requires an explicit external tile collection; this node does not reset block boundaries automatically.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

The native implementation applies separable one-axis transforms with dtype-specific plans and CPU kernels. Types II and III use a reordered real FFT and its inverse; even lengths reduce to N/2 complex points. Type I uses an even extension of length 2(N-1), reduced to N-1 complex points. Even-length type IV uses an N/2 complex reduction with phase factors; odd lengths use the corresponding extended real transform. Type and normalization branches are selected outside the element loops. Scalar paths cover other supported CPU configurations and dimensions.

Reserve immutable tables, per-worker FFT workspace and unpublished output under the shared resource model. Every worker must drain on success, failure or cancellation before returning. No precision-refinement dispatcher or strict numerical fallback is required.

For P=HW and C independent planes, FFT-derived execution costs O(P C (log H+log W)); the small-input mathematical oracle is not a production complexity estimate. A failed call publishes no successful Result export; previously retired exports remain valid, and final RET results require successful Run completion. No automatic spill or silent precision reduction is permitted.

## Oracle, Fixtures, and Acceptance

Fixtures: for N=2 and backward normalization, type I maps [a,b] to [a+b,a-b]. Type II has only DC for a constant input. Verify all four matrices, II/III pairing, ortho Parseval and type-I rejection of N=1.

The preserved reference evaluates the mathematical matrix expression on small inputs. It is not a production kernel or complete schema/preflight simulator. Directed high-precision calculations diagnose numerical error; their rounded bits are not a bitwise acceptance requirement for the F profile. State dtype, input range, error norm and tolerance for every numerical comparison.

`frq09.spectral.dct2(image, *, kind='II', norm='backward', inverse, dtype)` → [source](../../../../oracle/ops/filter/frq09/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

`frequency.dct2` is implemented and registered as a bundled native CPU operation. Scalar, SIMD and threaded execution share the unsuffixed key and F profile; Float64 input, output and arithmetic remain Float64.

Implementation and registration are complete; this does not establish complete numerical, runtime or performance acceptance.

## Execution Outline

`static parameters + schemas → compile fixed reads/tasks → prepare → native F-profile arithmetic → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S13 · SciPy dct](../research-sources.md#s13)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

Special-value behavior follows the native F profile and selected arithmetic. Resource admission and failure follow the shared FILTER contract.
