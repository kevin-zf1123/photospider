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

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherits the [family contract](CRV-06_color_ramp.md) for the Result tensor-port rules, output schema and caller preconditions.

## Model

C/M/Y/K ink-amount components are interpolated directly in one explicitly identified printing ICC profile. Output remains CMYK under that same profile; no RGB/PCS conversion or device-independent interpretation is implied. It is a separate operation from the RGB and other CRV-06 model ramps.

Only four channels C,M,Y,K are supported, each finite in [0,1]. Zero means no amount of that ink and one means the channel's full amount. No CMYKA or implicit alpha association is provided. Per-channel finite [0,1] amounts are caller preconditions. No total-ink limit is derived from profile tables, K is not redistributed, no black generation is performed and the supplied recipe is not altered. This ramp promises component interpolation, not printability of every valid [0,1]^4 combination on the selected device.

## Interface

Ordered inputs and their rules are those of the RGB ramp: input[S] (rank 1..7), stops[K] (K=1..65536, finite strictly increasing), colors[K,4]; independently mixed Float32/Float64 input/stops/colors; output dtype defaulting to colors dtype. Output appends exactly four channels interpreted by the explicit profile parameter and carries no description. Logical input/color/output counts are <=2^40. Scalar queries are finite. The complete four-channel CMYK tuple is the observation unit.

Required static parameters are color_description, dtype and out_of_domain. color_description is a canonical TensorDescription String referring to the profile resource, CMYK model, canonical C/M/Y/K tuple order, normalized ink units and no alpha. Its convention is `icc-native`, reference is `profile`, and association is straight. The logical tuple uses `channel_axis=0`. `color_ramp_cmyk_description(identity)` returns `Result<TensorDescription>`; callers check its result and supply the profile resource to compilation. It has no RGB primaries/gamma overrides or default printing profile. dtype is float32/float64, default colors dtype in constructors; out_of_domain is clamp/reject, default clamp. Direct nodes supply all three. Results carry no attached color description.

The ICC profile is an immutable resource fixed at compilation; its content identity is referenced by the color-array description. A changed profile requires recompilation; file paths and mutable external file contents are not semantic color identity. Runtime stop/color values may change normally.

Only valid ICC v2/v4 CMYK output-device profiles are accepted: class signature prtr and data color space CMYK. RGB profiles, DeviceLink and ICCmax are not supported. The standard header/tag structure, declared length and in-bounds referenced data are validated. Malformed or unsupported resources are rejected before color evaluation. A missing resource returns NotFound; unsupported or malformed ICC contents return InvalidArgument. No A2B/B2A transform is executed and no rendering intent is selected.

## Mathematics

For distinct enclosing stops s0,s1 and finite query q, w=(q-s0)/(s1-s0) exactly. Each channel c returns RN_dtype((1-w)*colors[j,c]+w*colors[j+1,c]). Endpoints, clamp and K=1 correctly convert the selected color directly. Strict correctly rounds the whole formula; accelerated follows the shared FP32-scaled final-result allowance. Values stay in [0,1] without component clipping because interpolation is inside the segment; no extrapolation is provided. No transfer decoding, RGB conversion, premultiplication or ICC table evaluation participates, and the profile is not numerically consulted. The RGB ramp's direct/mixed zero-sign rules apply where no alpha exists.

## Behavior

Hit/clamp/singleton paths convert one selected row; ordinary interpolation uses two rows. All stops and colors rows are dependencies; selected rows obey the caller numerical preconditions and unused generic rows may contain invalid ink or NaN. Partial channel requests expand to full CMYK tuples. Region and tile results equal the corresponding slice of the whole result. Numerical ink/query/stop conditions are caller preconditions.

## Acceptance

Fixture under a valid CMYK printing profile: stops=[0,1], colors=[[0,0,0,0],[1,0.5,0,0.25]], input=[0.5] -> values=[[0.5,0.25,0,0.125]]. An exact rational interpolation reference verifies all three profiles by bits. Check K=1, exact stops, zeros, boundaries, irregular spacing and all Float32/Float64 combinations. The sum of components may exceed a device's ink limit; the ramp intentionally does not turn that into an inferred total-area-coverage validation. Profile acceptance covers identical frozen content from different file paths, and missing/RGB/DeviceLink/ICCmax/malformed profiles.

## References

The [ICC v2/v4 specification catalogue](https://www.color.org/icc_specs2/) defines the profile format. The supported profile subset is a Photospider contract and does not claim a general ICC color-management engine or arbitrary profile-type support. CMYK/RGB conversion requires its own profile transform.

- [Ramp family](CRV-06_color_ramp.md).
- [Color-array description dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Color-management category](../../02-format-color/representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).
