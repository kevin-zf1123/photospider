---
spec_schema_version: 1
id: FILTER-common
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: partially_implemented
---

# FILTER common contract

This category defines FIL-01..20, FRQ-01..10 and RES-01..14: 44 families. Member letters identify distinct mathematical/interface objects.

## Inheritance and representation

Member rules refine family rules and this category contract. FRQ-01A/B and FRQ-02A/B use the explicit ordinary floating-point F exception in [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles). RES-01 through RES-12 use the scoped [RES native F contract](RES_native_contract.md). Other members do not override the rounding/accuracy baseline in [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [NUM acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md), or straight color and storage in [FMT](../../02-format-color/op_specs/FMT_common_contract.md), [relative coordinates](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) and [tensor storage](../../../kernel-specs/Tensor-Storage-and-Region-Access.md). FIL-01D explicitly admits NUM color-sample arithmetic as documented in [color composition](FILTER_color_composition.md), while retaining explicit FMT interpretation, straight representation and the unchecked finite-coverage premise.

Operation ports carry Results with fixed Tensor member schemas in member-table order. Data use generic tensors, with no new Image, Layer or complex dtype. Arithmetic inputs are Float32/Float64; related image inputs have matching dtypes. Outputs retain the main input dtype unless explicitly specified otherwise. Kernel/calibration coefficients declared Float64 retain that precision. Integer images require explicit FMT conversion. Dtype never implies a 0..1 or 0..255 scale.

Image logical rank is 2..8. Static Int64 y_axis and x_axis are distinct nonnegative axes; other axes are independent batch/channel planes. Axis indices address the complete sample shape, including the batch-axis prefix. An explicit component_axis for vector guides, RGB or matrices cannot coincide with either spatial axis. Examples written [H,W,C] do not require interleaved storage. Extents are positive; the inherited 2^40 element limit and checked static shape arithmetic apply. Runtime indices and address-control payloads are unchecked caller premises. Scalars use [1]; zero-length Values cannot represent empty valid convolutions. Shapes depend only on descriptors and static parameters. A runtime radius field cannot change output shape and has an explicit admitted max_radius.

## Static structure and unchecked inputs

Static checks cover schema/version, member directories, dtype, rank, positive extents, axes and shape relations, parameter presence/types/syntax and static capacity arithmetic. Runtime counts, offsets, indices, masks, IDs, associations, padding/encoding, PSF/model constraints and numerical/geometric domains are unchecked caller premises. Static numerical parameters retain their finite/range/order/positivity premises; only constraints that determine static structure are checked.

No result, diagnostic or recovery is promised outside those premises. Algorithmic predicates, candidate/branch selection, correct-rounding refinement, explicit clipping and quality measurements remain required computations. Explicitly defined no-solution outcomes on valid inputs are results, not failures.

## Parameters and numerical semantics

Every static parameter is explicitly supplied. Configurable boundary, Gaussian sigma_x/y and radius_x/y, custom kernel/footprint anchors (including odd shapes), and selectable empty-support policy have no constructor defaults. A fixed boundary or anchor defined by a named mathematical profile remains fixed. Members whose support is necessarily nonempty have no redundant empty policy. Unknown, missing or mistyped parameters are rejected. Integer sizes/radii/levels/steps are Int64; real static parameters are finite Float64 stored values unless the member explicitly specifies a different parameter domain.

[Numeric reference](FILTER_numeric_reference.md) defines exact expressions, baked constants, staged state and the F native arithmetic exceptions. Outside that exception, NaN/Inf, payload, signed zero, floating overflow and underflow follow corresponding NUM operations. There is no category-wide finite sample requirement and no category-wide floating-overflow failure. Parameter, mask, PSF, geometry and model domains are member-defined caller premises. Nonfinite arithmetic follows the member's F or NUM behavior.

Physical units are mathematical units: spatial lengths, guide distances, noise standard deviations, squared-unit variances/epsilon and model-specific solver parameters. There is no universal intensity knob, implicit normalization, inferred image range or unit-conversion helper. Lab uses FMT l=L*/100. Scaling relationships do not imply bit identity through floating conversion and staged rounding.

Raw field operators consume explicit values, axes and parameters. Color/alpha/units/sampling interpretation is caller-owned configuration; Results carry no attached descriptions or sample-validity proofs. Output schemas establish structural axes and member layouts, and the caller supplies the next consumer's interpretation. Semantic color entry points obey [color composition](FILTER_color_composition.md).

## Support

Data and Control denote mathematical sample support; Descriptor denotes static schema requirements. Boundary extension uses the full logical domain. For data-dependent addresses (runtime kernel, guide, radius or mask), the result depends on the complete statically bounded control table and on a static superset of the addressed samples. Complete-input algorithms depend on their complete input. A local halo does not establish locality of connectivity, transforms or iterative solvers. Region and tile results equal the corresponding region of the complete-input result. Missing coverage is not zero.

Output selection computes only the requested outputs of a call and their mathematical ancestors. Collection representation and selected-band dependencies follow [collections](FILTER_collections_contract.md).

## Numerical profiles

Fixed-step E/B/S strict profiles are reproducible state trajectories. Their accelerated profiles retain exact discrete decisions and NUM final-value bounds. Native RES fixed-step profiles retain their specified recurrence and dtype-aware numerical acceptance. Operator-defined stopping and explicit state follow the [iteration contract](FILTER_iterative_contract.md).

The F-profile transforms and RES01-12 have their scoped native arithmetic exceptions; E/B/S members retain their own whole-expression rounding obligation. A composition of ordinary FFT outputs does not establish an E-strict result. Mathematically valid sizes, radii, levels and iteration counts are limited only by representability and necessary profile constraints; resource exhaustion neither lowers precision nor changes the algorithm.

## Errors

| Condition | Status |
| --- | --- |
| Missing/mistyped parameter, invalid syntax/enum or structural anchor/shape constraint | InvalidArgument / InvalidDomain |
| Unsupported dtype/rank/shape/semantic structure | TypeMismatch / None |
| Violated dynamic control, model, association or numerical/geometric premise | Outside the contract; no validation, repair or diagnostic guarantee |
| Floating invalid arithmetic, division by zero, overflow | Successful F-profile or NUM NaN/Inf result according to the member |
| Static shape/capacity arithmetic overflow | Explicit failure |

[Acceptance](FILTER_oracle_protocol.md) separates mathematical evidence, third-party comparisons and image-quality scores. Conceptual DAGs are not executable public APIs.
