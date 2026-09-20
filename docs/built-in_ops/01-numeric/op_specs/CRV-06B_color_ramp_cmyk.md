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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06B: color_ramp_cmyk

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

Validate per-channel amounts only. Do not derive a total-ink limit from profile
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
Output appends exactly four channels and carries the matching CMYK profile
description. Input rank is 1..7 and logical input/color/output counts are <=2^40.

The complete four-channel CMYK tuple is the observation/validation unit. Globally
validate stops, but read only requested positions and the selected one or two
complete colors. Direct hit/clamp/single-stop paths convert only that selected
color; unselected colors are not validated. Attached source color descriptions
must match the explicit static CMYK profile description.

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
is clamp/reject, default clamp. Direct nodes supply all three. If colors already
has a color description, model/profile/channel interpretation must match.

The host binds an immutable resource reference and retains its content for the
compiled plan and exported values. Encoded metadata alone must not leave an
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

The observation coordinate is the input position, excluding the four-channel
axis. Expand a request for any component to the complete CMYK tuple. Control is
all stops plus the requested input positions; Data/Validation is the union of
selected complete color rows. Empty Q reads no runtime payload. ICC descriptor/
resource validity is a compile/preflight obligation, not a per-query CMM pass.
Every selected row's four ranges are validated, even when an exact formula
simplification would not use one value. Other rows remain unread.

Stop changes invalidate all dependent observations, input changes affect their
position, and a selected color-row edit invalidates the complete colors using
that row. Retain exact control/data/typed witnesses and the profile content
identity. No bounding gaps or whole color-table prevalidation are introduced.
Upstream Whole support keeps its own failures independently of local demand.

Return immutable packed complete-color fragments at the correct global
Region/storage origins, carrying the CMYK color-array description and owned
profile reference. Source strides/offsets may be arbitrary valid immutable
layouts. The output shape is input.shape+[4], inferred statically. Profile and
data owners survive context teardown until final release; closing optional cache
does not invalidate active outputs or their color interpretation.

## Resources, errors and acceptance

For M requested color tuples, ordinary lookup work is O(K+M log K), plus exact
four-channel interpolation and typed validation. Output payload is 4*M*b bytes;
an optional packed stop index is 8K. Account immutable profile bytes and validator
work once under actual shared ownership, plus all source windows, descriptor/index
metadata, arithmetic limbs, output/scratch and simultaneous growth capacity.
Many tiny outputs may retain one substantial profile resource; payload-only
measurement does not capture that ancestry. No source profile or table is loaded
outside host admission. Use RGB's cancellation intervals and the common resource,
cache-off and publication/lifetime rules.

Missing profile, unsupported profile class/version/space or invalid profile data
fails authoring/compile/preflight with InvalidArgument/InvalidDomain; preserve
resource-parser detail. Port/descriptor mismatch uses TypeMismatch. Invalid
requested positions/stops, ink outside finite [0,1] or reject-domain lookup uses
OperationFailed/InvalidDomain at the complete color Atom. Valid convex numeric
interpolation has no expected output overflow. Resource/cancellation/backend,
stale and upstream/typed errors retain their original Status/source/scope.
No partial color or unowned profile reference is published on failure.

Fixture under a valid frozen CMYK printing profile: stops=[0,1],
colors=[[0,0,0,0],[1,0.5,0,0.25]], input=[0.5] ->
values=[[0.5,0.25,0,0.125]]. An independent rational interpolation oracle
verifies all three profiles by bits. Check K=1, exact stops, zeros, boundaries,
irregular spacing, invalid ink/NaN in selected versus remote rows and all
Float32/Float64 combinations. Sum of components may exceed a device's ink limit;
the ramp intentionally does not turn that into an inferred TAC validation.

Public WorkflowDocument fixtures must supply an actual validated profile
resource and inspect output profile identity as well as amounts. Check different
file paths with identical frozen content, changed content requiring recompilation,
missing/RGB/DeviceLink/ICCmax/malformed profiles, exact read/dirty sets, partial
channel requests expanding to full CMYK, strides, low resource budgets, cancellation,
cache-off and profile/value lifetime after context destruction. The maintained public commands and current runtime evidence are linked in the
umbrella contract; CMYK/RGB conversion requires its own profile transform.

Profile-format references are the
[ICC v2/v4 specification catalogue](https://www.color.org/icc_specs2/).
The selected supported profile subset is a Photospider contract and does not
claim a general ICC color-management engine or arbitrary profile-type support.

- [Ramp family](CRV-06_color_ramp.md).
- [Color-array description dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Color-management category](../../02-format-color/representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).

## Maintained implementation and validation

Public [`color_ramp_cmyk_node`](../../../../include/photospider/numeric/color_ramps.hpp)
constructs this primitive; [`color_ramps.cpp`](../../../../plugins/ops/01-numeric/color_ramps.cpp)
implements its staged complete-color execution.

All profiles use exact rational component interpolation and return strict
bits, with one destination rounding. Complete-color metadata and regional
validation remain attached to the result.
The output also retains the explicitly imported and bound ICC profile.

See the [family implementation](CRV-06_color_ramp.md#maintained-implementation-and-validation),
[mathematical machinery](../math-implementation.md#crv-06-colorarray-and-color-ramps)
and [public workflow commands](../../../../examples/numeric_workflow/README.md#color-ramps)
for resource limits and actual validation.
