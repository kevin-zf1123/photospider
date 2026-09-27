---
spec_schema_version: 1
id: FILTER-common
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# FILTER common contract

This category defines FIL-01..20, FRQ-01..10 and RES-01..14: 44 families.
Member letters identify distinct mathematical/interface objects. Proposed keys,
specification maturity, runtime registration and oracle coverage are separate facts.
The specifications do not claim that their proposed keys are registered.

## Inheritance and representation

Member rules refine family rules and this category contract. They do not override
the rounding/accuracy baseline in [NUM](../../01-numeric/op_specs/NUM_common_contract.md)
and [NUM acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md), or
straight color and storage in [FMT](../../02-format-color/op_specs/FMT_common_contract.md),
[relative coordinates](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
and [tensor storage](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The new logical collection schemas require runtime integration before registration.
FIL-01D explicitly overrides FMT's finite-color-sample validation as documented in
[color composition](FILTER_color_composition.md), while retaining FMT metadata,
straight representation and finite coverage validation.

Unless explicitly declared Result or authoring helper, ports are required Values in
member-table order. Data use generic tensors, with no new Image, Layer or complex
dtype. Arithmetic inputs are Float32/Float64; related image inputs have matching
dtypes. Outputs retain the main input dtype unless explicitly specified otherwise.
Kernel/calibration coefficients declared Float64 retain that precision. Integer
images require explicit FMT conversion. Dtype never implies a 0..1 or 0..255 scale.

Image logical rank is 2..8. Static Int64 y_axis and x_axis are distinct nonnegative
axes; other axes are independent batch/channel planes. An explicit component_axis
for vector guides, RGB or matrices cannot coincide with either spatial axis.
Examples written [H,W,C] do not require interleaved storage. Extents are positive;
the inherited 2^40 element limit and checked integer shape/index arithmetic apply.
Scalars use [1]; zero-length Values cannot represent empty valid convolutions.
Shapes depend only on descriptors and static parameters. A runtime radius field
cannot change output shape and has an explicit admitted max_radius.

## Parameters and numerical semantics

Every static parameter is explicitly supplied. Configurable boundary, Gaussian
sigma_x/y and radius_x/y, custom kernel/footprint anchors (including odd shapes),
and selectable empty-support policy have no constructor defaults. A fixed boundary
or anchor defined by a named mathematical profile remains fixed. Members whose
support is necessarily nonempty have no redundant empty policy. Unknown, missing
or mistyped parameters fail before sample reads. Integer sizes/radii/levels/steps
are Int64; real static parameters are finite Float64 stored values unless the
member explicitly specifies a different parameter domain.

[Numeric reference](FILTER_numeric_reference.md) defines exact expressions, baked
constants and staged state. NaN/Inf, payload, signed zero, floating overflow and
underflow follow corresponding NUM operations. There is no category-wide finite
sample requirement and no category-wide floating-overflow failure. Parameter,
mask, PSF, geometry and model constraints remain explicit member validations;
nonfinite arithmetic cannot suppress those validations or upstream failures.

Physical units are mathematical units: spatial lengths, guide distances, noise
standard deviations, squared-unit variances/epsilon and model-specific solver
parameters. There is no universal intensity knob, implicit normalization, inferred
image range or unit-conversion helper. Lab uses FMT l=L*/100. Scaling relationships
do not imply bit identity through floating conversion and staged rounding.

Raw field operators consume explicit values, axes and parameters; neither channel
count nor attached metadata makes them consume RGB or alpha. Preserve descriptors
that remain structurally valid but invalidate unsupported sample-validity proofs.
Derivatives, spectra and covariance establish their own units/axes. Semantic color
entry points obey [color composition](FILTER_color_composition.md).

## Demand, validation and publication

Data, Control, Validation and Descriptor demand are distinct. Empty requested
Region Q reads no payload. Dynamic kernel/guide/radius/mask decisions retain Control
witnesses even when output bits do not change. Kernel-table validation is not whole
image color validation. Boundary extension uses the full logical domain, never an
ROI/tile edge. Exact demand is the actual admitted sample set; a radius bounding
box is only Conservative. FFT/prefix-sum optimizations cannot silently enlarge
Exact-only reads. Whole outputs require their complete input dependency group.
Dirty propagation transposes retained Data/Control/Validation relations; descriptor
or static changes reinfer outputs. A local halo does not establish locality of
connectivity, transforms or iterative solvers.

Publish canonical planar owned storage with full-image virtual address reservation,
unified DAG tile geometry, explicitly prepared pages and valid edge pixels separate
from padding. A dense Value exposes only generated global Regions; missing data are
not zero. Owners/read windows can outlive ExecutionContext until their final release.
Independent outputs do not read unrelated inputs when requested jointly. Collection
association and band-lazy payloads follow [collections](FILTER_collections_contract.md).

## Resources, cancellation and capabilities

Mathematically valid sizes, radii, levels and iteration counts are governed by
inherited representability limits, necessary profile constraints and execution
budgets. Do not impose arbitrary fixed capability caps. An oracle's tiny-image
work cap is not a runtime input limit. CPU strict reference is the baseline;
Apple Silicon and x86_64 acceleration require named capabilities, actual ISA checks,
final-value NUM error bounds and strict fallback. GPU support is not implicit.

Charge outputs, kernels, scratch, iterative states, arbitrary-precision limbs,
retained ancestors, collection owners and comparison-resource snapshots to root
capacity/work/stage budgets. Mathematical operation counts exclude neither limb
cost nor refinement work from actual accounting. Poll cancellation every 1024
scalar work units or sooner, including refinement and long solves. Resource failure
does not lower precision, change algorithms or spill to disk. Early-stop state
retention and cooperative termination follow [iteration](FILTER_iterative_contract.md).

Fixed-step strict profiles preserve reproducible state trajectories. Accelerated
profiles retain exact discrete decisions and NUM final-value bounds. Independent
asynchronous early-stop invocations may select different returned rounds; their
cross-invocation result caching is prohibited as specified in the iteration contract.

## Errors and acceptance

| Condition | Phase | Status obligation |
| --- | --- | --- |
| Invalid static parameter/anchor/profile/domain | Compile/preflight | InvalidArgument / InvalidDomain |
| Unsupported dtype/rank/shape/semantic structure | Compile/preflight | TypeMismatch / None |
| Invalid dynamic control or explicit model constraint | Evaluation | Member's domain error; not a generic nonfinite arithmetic error |
| Floating invalid arithmetic, division by zero, overflow | Evaluation | Successful NUM NaN/Inf result unless a corresponding explicit NUM domain exception applies |
| Checked integer overflow | Owning phase | Explicit failure |
| Unavailable backend | Capability | BackendUnavailable / None |
| Resource, cancellation, upstream failure | Original phase | Preserve original failure |

Diagnostics identify operation, output, global coordinate and offending bits where
applicable; strings do not add FailureReason enumerators. Failure scope follows
actual dependency observations and association requirements, preserving unrelated
completed outputs. Do not publish partially successful Values or success certificates.
[Acceptance](FILTER_oracle_protocol.md) separates mathematical evidence, third-party
comparisons, runtime behavior and image-quality scores. Conceptual DAGs are not
executable public APIs.
