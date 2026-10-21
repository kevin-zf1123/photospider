---
spec_schema_version: 1
id: CRV-06B
parent_id: CRV-06
function: color_ramp_cmyk
operation_family: curve.color_ramp_cmyk
proposed_operation_keys:
  - curve.color_ramp_cmyk_strict
  - curve.color_ramp_cmyk_accelerated_apple_silicon
  - curve.color_ramp_cmyk_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06B: color_ramp_cmyk

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed model and numeric interpretation

Interpolate C/M/Y/K ink-amount components directly in one explicitly identified
printing ICC profile. Output remains CMYK under that same profile; no RGB/PCS
conversion or device-independent interpretation is implied by this operation.
It is a separate implementation from RGB and the other CRV-06 model ramps.

Only four channels C,M,Y,K are supported in this version, each finite in [0,1].
Zero means no amount of that ink and one means the channel's full amount.
No CMYKA or implicit alpha association is provided; coverage may be handled by
separate explicit data/operations. This clarification is complete; the current
registry and shared ICC-resource infrastructure implement the public path below.

Per-channel finite [0,1] amounts are caller preconditions. Do not derive a total-ink limit from profile
tables, redistribute K, perform black generation or alter the supplied recipe.
Total-ink control and actual profile color conversion belong to separate explicit
color-management operations. This ramp promises component interpolation, not
printability of every valid [0,1]^4 combination on the selected device.

## Confirmed profile lifetime

ICC profile is frozen as an immutable resource at compilation. Its content
identity is referenced by the color-array description and remains fixed across
executions of that compiled workflow. A changed profile requires recompilation.
File paths and mutable external file contents are not semantic color identity.
Resource ownership/validation and the concrete authoring representation belong
to the shared color-management dependency.

## Confirmed shared execution and precision

Inherit RGB ramp's stops/input interface, K=1..65536, finite strictly increasing
stops, clamp/reject (default clamp), scalar query finiteness, independently mixed
Float32/Float64 input/stops/colors and output dtype defaulting to colors dtype.
Output appends exactly four channels interpreted by the explicit profile parameter; it carries no description. Input rank is 1..7 and logical input/color/output counts are <=2^40.

The complete four-channel CMYK tuple is the observation unit. Selected rows obey caller numerical preconditions. Hit/clamp/singleton paths convert one selected row; ordinary interpolation uses two rows. Static dependencies include all stops/colors tiles, so upstream failures remain visible even for numerically unused rows.

Each channel is interpolated by the exact mathematical linear formula, with one
final output-dtype rounding. Strict correctly rounds the whole formula; accelerated follows the shared
FP32-scaled final-result allowance. No transfer decoding,
RGB conversion, premultiplication or ICC table evaluation participates in this
formula. Inherit RGB ramp's direct/mixed zero-sign rules where no alpha exists.

## Profile validation and explicit parameters

Only valid ICC v2/v4 CMYK output-device profiles are accepted: class signature
prtr and data color space CMYK. RGB profiles, DeviceLink and ICCmax are outside
this version. Validate the frozen resource's standard header/tag structure,
declared length and in-bounds referenced data through the host's bounded ICC
resource validator. Reject malformed or unsupported resources before color
evaluation. This does not execute A2B/B2A transforms or select a rendering intent.

Required static parameters are color_description, dtype and out_of_domain.
color_description is the proposed canonical description-helper String referring
to the frozen profile resource, CMYK model, channel order C/M/Y/K, normalized ink
units and alpha=none. It has no RGB primaries/gamma overrides or default printing
profile. dtype is float32/float64, default colors dtype in constructors; policy
is clamp/reject, default clamp. Direct nodes supply all three. Results carry no attached color description.

The host binds an immutable resource reference and retains its content for the
prepared operation, compiled Plan and active kernels. Exported values retain their data backing independently. Encoded metadata alone must not leave an
unresolvable pointer after context destruction. Resource loading is a separate
authoring action; this callback does not read mutable profile paths or perform
filesystem I/O. Rebinding a different profile requires a new compiled identity.
Runtime stop/color values may still change normally.

## Formula, demand and returned mapping

For distinct enclosing stops s0,s1 and finite query q, w=(q-s0)/(s1-s0) exactly.
Each channel c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]). Endpoints,
clamp and K=1 correctly convert the selected color directly. Values stay in
[0,1] without component clipping because interpolation is inside the segment;
no out-of-domain extrapolation is provided. The unchanged profile labels the
four output amounts and is not numerically consulted by the interpolation.

All profiles use the [fixed execution contract](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions). Each selected complete-color output tile has one CPU task. Its reads cover the corresponding query region and all stops/colors/p/q tiles, each closed on its own source grid. Runtime selection uses one exact-hit/clamp/singleton row or two enclosing rows. Generic unselected colors and denominators are numerically unused; upstream failures still propagate through declared tile dependencies.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[C]`, and `atomic_trailing_axes=1`. It carries no color description or input batch-axis partition. The kernel writes only selected dense output tiles, borrowing affine source views and owning fixed arithmetic scratch. Empty emits no task or sample read. Successful call publication follows required task completion in retirement order, and output storage survives context teardown.

Finite ordered stops, finite queries, selected complete-color model/association validity, selected rational q>0, reject-domain membership and representable final output are unchecked caller preconditions. Lookup costs O(P log K) for P selected query points, plus exact/certified arithmetic; the kernel searches source stops directly without an O(K) copy. Static schemas, parameters, resources and kernel signatures remain checked. Cancellation and capacity failures stop execution without approximate substitution.

## Resources, errors and acceptance

CMYK uses the [family resource contract](CRV-06_color_ramp.md#cmyk-resources). The resource validator runs once per preparation, including Empty and artifact import, without copying the complete profile. Missing resources return NotFound; unsupported or malformed ICC contents return InvalidArgument with parser detail. Resource admission and parser work limits preserve ResourceExhausted. Numerical ink/query/stop conditions are caller preconditions. No ICC CMM or automatic resource attachment to output Results occurs.

Fixture under a valid frozen CMYK printing profile: stops=[0,1],
colors=[[0,0,0,0],[1,0.5,0,0.25]], input=[0.5] ->
values=[[0.5,0.25,0,0.125]]. An independent rational interpolation oracle
verifies all three profiles by bits. Check K=1, exact stops, zeros, boundaries,
irregular spacing, unused generic rows containing invalid ink/NaN and all
Float32/Float64 combinations. Sum of components may exceed a device's ink limit;
the ramp intentionally does not turn that into an inferred TAC validation.

Public WorkflowDocument fixtures must supply an actual validated profile
resource and inspect prepared resource identity as well as output amounts. Check different
file paths with identical frozen content, changed content requiring recompilation,
missing/RGB/DeviceLink/ICCmax/malformed profiles, source-grid read closure, partial
channel requests expanding to full CMYK, strides, low resource budgets, cancellation,
profile and value ownership after context destruction. Create the public checks for the current operation change. CMYK/RGB conversion
requires its own profile transform.

Profile-format references are the
[ICC v2/v4 specification catalogue](https://www.color.org/icc_specs2/).
The selected supported profile subset is a Photospider contract and does not
claim a general ICC color-management engine or arbitrary profile-type support.

- [Ramp family](CRV-06_color_ramp.md).
- [Color-array description dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Color-management category](../../02-format-color/representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).
