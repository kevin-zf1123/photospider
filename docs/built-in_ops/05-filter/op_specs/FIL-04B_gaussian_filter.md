---
spec_schema_version: 1
id: FIL-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
parent_id: FIL-04
function: gaussian_filter
proposed_operation_keys:
- filter.gaussian_filter_strict
- filter.gaussian_filter_accelerated_apple_silicon
- filter.gaussian_filter_accelerated_x86_64
numeric_reference: B+E
oracle_scope: mathematical_reference
research_sources:
- S03
- S25
---
# FIL-04B: gaussian_filter

Apply the baked64 Gaussian profile.

Inherit [FIL-04](FIL-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric and precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata and straight-color contract](../../02-format-color/op_specs/FMT_common_contract.md). Together, the family and member specifications define the target. Do not override RN stages, underflow behavior, or accelerated error bounds.

## Ports, shape, and semantics

Input port `input` selects a Result containing one tensor and no fields. Output
port `output` publishes one Result tensor with the same complete sample shape,
including batch axes. Its tensor schema, facets, layout metadata, and bound
resources are copied from the input; supported typed ColorArray v1 and ICC
ownership are retained. Raw numeric inputs do not reject NaN or infinity under a
blanket finite-value rule.

## Explicit parameters and legal domain

Require `sigma_x/y`, `radius_x/y`, axes, boundary, and `cval`. Sigma controls weight decay; radius independently controls finite support. Zero sigma requires zero radius; positive sigma permits radius zero. Sigma and cval are finite; sigma and radii are nonnegative. The operation has no arbitrary bias and no constructor defaults.

`x_axis` and `y_axis` index the complete tensor sample shape, including the
batch-axis prefix, not only the cell descriptor. For an RGBA sample shape
`{N,L,H,W,4}`, use `y_axis=2` and `x_axis=3`.

NUM special-value, floating-point overflow, signed-zero, payload, rounding, and precision behavior follows the corresponding operation and [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf under a family-wide rule or convert ordinary floating-point overflow into failure. Parameter-domain, structural, integer-overflow, resource, cancellation, and upstream errors retain their own contracts. Every parameter is explicit in the node; constructors have no implicit defaults. Interpret floating-point parameters as their stored values.

## Mathematical reference and rounding

Treat `kx` and `ky` as exact binary64 constants. Return `RN_t(Σ ky*kx*I / ((Σky)(Σkx)))` with one final rounding. Boundary handling follows FIL-02A. This is not the result of filtering an already rounded intermediate Gaussian image. If both radii are zero, perform an exact bit copy and do not read or validate other samples.

The numeric class and E/B/S meanings are defined in [FILTER numeric reference](FILTER_numeric_reference.md). Retain every declared rounding stage; helper calls do not introduce extra intermediate rounding unless the member says so. Signed zero, discrete choices, gradual underflow, and output range follow the member and NUM contracts.

Whole and GPU demand the complete tensor domain and publish a complete output. CPU tiled uses IndependentChunks and computes only requested output regions. Its Data support is a compact exact Neighborhood relation for nonzero taps after boundary mapping. It unions a separate Validation relation that closes typed ColorArray channel tuples and atomic trailing axes; the first Need also carries Descriptor role. Later Needs request Data and Validation without repeating descriptor demand. Boundaries use the complete input sample domain, including batch axes, never an ROI or tile edge. Empty demand reads no payload and performs no coefficient or arithmetic payload allocation.

Use the Cartesian product of nonzero 1D taps; the kernel is derived static control data. Zero sigma only removes support on its own axis. There is no hidden UI maximum for large sigma, though indexing, work, and resource budgets still apply. Execution follows FIL-02A.

Complexity uses P=HW samples, C independent planes, A taps, and T iterations where applicable; arbitrary-precision limb cost is additional and is not a measured benchmark. Follow FILTER admission, work/memory/stage accounting, cancellation, failure, and owner-lifetime rules. Whole and GPU publish only a complete Result. A tiled execution may retain already certified IndependentChunks if a later stage fails. Do not spill implicitly, lower precision silently, or fall back to retired implementations.

## Registered execution forms

`filter.gaussian_baked64_v1_strict_cpu_whole` implements the mathematical profile
above using explicit Whole execution. It demands and computes the complete input
and output domains; any input change invalidates the complete output group.
Static parameters are `sigma_x`, `sigma_y`, `radius_x`, `radius_y`, `x_axis`,
`y_axis`, `boundary` and `cval`, all mandatory with the types and domains inherited
above. The tensor supports Float32/Float64, sample rank 2..8 and at most 2^40
samples across all batch and descriptor axes. Coefficients are generated under
runtime budgets and share one immutable owner across the invocation's host ranges.

`filter.gaussian_baked64_v1_strict_cpu_tiled` computes requested regions with
budgeted coefficient preparation, exact Data support and CPU tile stages.
IndependentChunks preserve certified output as it is published. A compact
Neighborhood relation describes nonzero x/y taps; a separate Validation
relation closes ColorArray tuple and atomic trailing axes. Dirty propagation
follows these relations, and the continuation reuses one coefficient owner across
its tile polls.

`filter.gaussian_baked64_v1_strict_gpu` uses Whole demand and the same baked64
mathematics. It selects MSL/Metal or SPIR-V/Vulkan from the active GPU service and
has no CPU fallback. Host execution certifies coefficients; the device evaluates
and rounds every output sample using exact integer arithmetic. A dispatch covers
up to 256 output lanes on Metal and 64 on Vulkan, with at most 16 taps per lane.
A submission may group two ordered dispatches, so each lane processes at most 32
taps between submission drains. Cancellation stops later submissions and
publication while retaining all owners until completion. The Result migration
passes the native Metal test and independent oracle. Earlier FreeBSD Intel UHD
770 Vulkan verification exercised the former Value path; the current Vulkan
Result path has not been revalidated. See the [current implementation](../gaussian-implementation.md)
and [public workflow](../../../../examples/gaussian_workflow/README.md).

## Oracle, fixtures, and acceptance

Verify exact preservation of constants and impulse reflection. Compare Float32 and Float64 results with a baked64 rational oracle. Do not assert that two truncated blurs equal one blur with sigma `sqrt(s1²+s2²)`.

The linked reference function is a mathematical oracle, not a production kernel or a complete Result-port/preflight simulator. ExactRational uses exact rational expressions followed by IEEE rounding. For transcendental functions, only a DirectedMPFR interval that uniquely determines the result is a strict golden; otherwise report Inconclusive. Fixture identifiers describe reference-case scope only. Current runtime behavior and finite execution evidence are described in the [implementation document](../gaussian-implementation.md). See the [runtime acceptance protocol](FILTER_oracle_protocol.md).

The reference helpers are `gaussian_kernel(sigma,radius)` in [transcend.py](../../../../oracle/ops/filter/oracles/transcend.py) and `separable(image,kx,ky,anchor=(0,0),**kwargs)` in [spatial.py](../../../../oracle/ops/filter/oracles/spatial.py). See also the [oracle README](../../../../oracle/ops/filter/README.md).

Fixture coverage identifiers: `gaussian_constant`, `straight_alpha_zero_hidden_color`, `straight_all_zero_alpha`, `straight_alpha_underflow_hidden_color`. This catalog records fixture scope only; it makes no current pass claim.

## Backend and registration

The proposed keys are `filter.gaussian_filter_strict`,
`filter.gaussian_filter_accelerated_apple_silicon`, and
`filter.gaussian_filter_accelerated_x86_64`. Those proposed base/accelerated
keys remain unregistered. The separately named strict CPU Whole, CPU tiled and
native GPU forms are registered as described above. Accelerated variants must
satisfy NUM's final FP32-scaled four-ULP requirement and fallback rules without
accumulating a separate budget per tap, axis, stage, or iteration. Threshold,
ordering, boundary, copy, and output-support choices that must be exact cannot
change approximately. Float64 accelerated paths retain Float64 input/output and
exponent range; they do not first convert to Float32. Performance and CPU/ISA
differential acceptance are not established here.

## Conceptual DAG (not an existing API)

`static parameters + descriptors -> preflight/demand -> declared data and control -> exact expression and declared rounding stages -> requested owned output/result`

This sketch introduces no implicit color conversion, alpha premultiplication, frequency correction, or zero-fill for missing data. Whole and staged-buffer requirements are those declared by the member and family.

## Sources and open items

[S03](../research-sources.md#s03), [S25](../research-sources.md#s25).
