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

This category defines FIL-01..20, FRQ-01..10 and RES-01..14: 44 families.
Member letters identify distinct mathematical/interface objects. Proposed keys,
specification maturity, runtime registration and oracle coverage are separate facts.
FIL-01..20 are implemented through fixed invocation ABI 8: 45 primitive members
provide 129 registered keys, and two authoring helpers compose existing operations.
FRQ and RES families retain their own implementation statuses. Proposed/D1 describes
specification acceptance and maturity separately from runtime availability.

## Inheritance and representation

Member rules refine family rules and this category contract. They do not override
the rounding/accuracy baseline in [NUM](../../01-numeric/op_specs/NUM_common_contract.md)
and [NUM acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md), or
straight color and storage in [FMT](../../02-format-color/op_specs/FMT_common_contract.md),
[relative coordinates](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
and [tensor storage](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The Gabor and Gaussian/Laplacian FilterBands forms have explicit SDK/runtime integration; other proposed collection forms require their own integration before registration.
FIL-01D explicitly admits NUM color-sample arithmetic as documented in [color composition](FILTER_color_composition.md), while retaining explicit FMT interpretation, straight representation and the unchecked finite-coverage premise.

All operation ports carry Results with fixed Tensor member schemas in member-table order; authoring helpers compose those ports explicitly. Data use generic tensors, with no new Image, Layer or complex dtype. Arithmetic inputs are Float32/Float64; related image inputs have matching
dtypes. Outputs retain the main input dtype unless explicitly specified otherwise.
Kernel/calibration coefficients declared Float64 retain that precision. Integer
images require explicit FMT conversion. Dtype never implies a 0..1 or 0..255 scale.

Image logical rank is 2..8. Static Int64 y_axis and x_axis are distinct nonnegative
axes; other axes are independent batch/channel planes. Axis indices address the complete sample shape, including the batch-axis prefix. An explicit component_axis
for vector guides, RGB or matrices cannot coincide with either spatial axis.
Examples written [H,W,C] do not require interleaved storage. Extents are positive;
the inherited 2^40 element limit and checked static shape/capacity arithmetic apply. Runtime indices and address-control payloads are unchecked caller premises.
Scalars use [1]; zero-length Values cannot represent empty valid convolutions.
Shapes depend only on descriptors and static parameters. A runtime radius field
cannot change output shape and has an explicit admitted max_radius.

## Static structure and unchecked execution

Compilation and static preparation check schema/version, member directories, dtype, rank, positive extents, axes and shape relations, parameter presence/types/syntax, fixed read/write/scratch signatures and static capacity arithmetic. Runtime counts, offsets, indices, masks, IDs, associations, padding/encoding, PSF/model constraints and numerical/geometric domains are unchecked caller premises. Static numerical parameters retain their finite/range/order/positivity premises; only constraints that determine static structure are checked.

Producers construct each declared output without a validation pass; consumers use valid payloads without rescanning or comparing them. No result, diagnostic or recovery is promised outside those premises. Algorithmic predicates, candidate/branch selection, correct-rounding refinement, explicit clipping and quality measurements remain required computations. Resource exhaustion, cancellation, backend failures and explicitly defined no-solution outcomes on valid inputs remain operational. Independent oracles may diagnose malformed reference inputs without imposing production validation. All implementations consume the frozen [physical execution contract](../../../kernel-architecture/Compiler-and-Execution.md) and fixed invocation ABI 8.

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
sample requirement and no category-wide floating-overflow failure. Parameter, mask, PSF, geometry and model domains are member-defined caller premises. Nonfinite arithmetic retains the formula's defined NUM behavior and does not suppress required upstream failures.

Physical units are mathematical units: spatial lengths, guide distances, noise
standard deviations, squared-unit variances/epsilon and model-specific solver
parameters. There is no universal intensity knob, implicit normalization, inferred
image range or unit-conversion helper. Lab uses FMT l=L*/100. Scaling relationships
do not imply bit identity through floating conversion and staged rounding.

Raw field operators consume explicit values, axes and parameters. Color/alpha/units/sampling interpretation is caller-owned configuration; Results carry no attached descriptions or sample-validity proofs. Output schemas establish structural axes and member layouts, and the caller supplies the next consumer's interpretation. Semantic color entry points obey [color composition](FILTER_color_composition.md).

## Fixed support and publication

Data and Control denote mathematical sample support; Descriptor denotes static schema requirements. There is no separate Validation read set. Empty Q introduces no payload tasks. Lowering emits finite, fixed member-tile reads for kernel/guide/radius/mask computation and uses a static superset for data-dependent addresses. Boundary extension uses the full logical domain. Support closes on source tile grids; runtime selection cannot request additional producers. Complete-input algorithms declare their complete input dependency group and shared intermediate tasks. Changed descriptors/statics require compilation; the executor has no dirty-propagation or content-cache path. A local halo does not establish locality of connectivity, transforms or iterative solvers.

Requests normalize to complete retained tiles on each Result's own grid. Every materialized tile has an independent allocation, one producer and fully initialized declared writes, including specified padding. Shared computation uses explicit tasks and private SSA values; persistent iteration state crosses CFG edges explicitly. A successful task enables dependents, and a call publishes selected Result exports only after its required tasks succeed and the call retires in IR order. A failed task prevents successful publication of that call; already retired calls remain valid. Missing coverage is not zero. Result/read-window owners can outlive ExecutionContext until final release. Output selection eliminates unrelated computation without promising failure isolation within one call. Collection representation and selected-band dependencies follow [collections](FILTER_collections_contract.md).

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
cost nor refinement work from actual accounting. CPU callbacks poll cancellation
every 1024 scalar work units or sooner, including refinement and long solves.
A GPU profile must declare a finite work bound per nonpreemptive native submission,
check cancellation before submission and after completion, and drain submitted
work before releasing any referenced owner. It must report measured cancellation
latency separately from its work bound. Cancellation prevents further submissions
and successful result publication; it does not imply device preemption. Resource failure
does not lower precision, change algorithms or spill to disk. Early-stop state
retention and cooperative termination follow [iteration](FILTER_iterative_contract.md).

Fixed-step strict profiles preserve reproducible state trajectories. Accelerated
profiles retain exact discrete decisions and NUM final-value bounds. Independent
asynchronous early-stop invocations may select different returned rounds; the frozen executor retains no cross-Run result cache. Operator-defined stopping and explicit state follow the iteration contract.

## Errors and acceptance

| Condition | Phase | Status obligation |
| --- | --- | --- |
| Missing/mistyped parameter, invalid syntax/enum or structural anchor/shape constraint | Compile/preparation | InvalidArgument / InvalidDomain |
| Unsupported dtype/rank/shape/semantic structure | Compile/preflight | TypeMismatch / None |
| Violated dynamic control, model, association or numerical/geometric premise | Outside the contract | No validation, repair or diagnostic guarantee |
| Floating invalid arithmetic, division by zero, overflow | Evaluation | Successful NUM NaN/Inf result unless a corresponding explicit NUM domain exception applies |
| Static shape/capacity arithmetic overflow | Compile/preparation/admission | Explicit failure |
| Unavailable backend | Capability | BackendUnavailable / None |
| Resource, cancellation, upstream failure | Original phase | Preserve original failure |

Operational diagnostics use existing Status/FailureReason fields; strings do not add enumerators. Completion status is attributed to the failing task/call, without an invented per-sample diagnostic or report-submission ABI. No success is published for a failed call. Numerical and association premise violations impose no diagnostic requirement. Production acceptance uses valid payloads; malformed oracle fixtures exercise only reference-tool diagnostics.
[Acceptance](FILTER_oracle_protocol.md) separates mathematical evidence, third-party
comparisons, runtime behavior and image-quality scores. Conceptual DAGs are not
executable public APIs.
