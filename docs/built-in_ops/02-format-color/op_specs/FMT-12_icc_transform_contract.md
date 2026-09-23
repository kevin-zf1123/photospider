---
spec_schema_version: 1
id: FMT-12
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-12: ICC profile transforms

This document records the completed FMT-12 clarification on 2026-09-23.
It specifies a future external-engine adapter and registers no operation.
Engine integration, numerical corpus and public runtime acceptance remain
implementation prerequisites, not evidence supplied by this specification.

## Inherited contracts

Use the unified tensor/metadata, planar storage, straight internal color,
call-local interpretation and consumed-semantic validation rules from
[FMT-common](FMT_common_contract.md). External-engine conformance is separately
specified; NUM strict rounding or accelerated four-ULP guarantees do not follow
automatically. The [relative-coordinate scale](FMT_relative_coordinate_scale.md)
requires native CIELAB lightness l=L*/100. Engine boundary units are specified below.

## Confirmed scope

- A is an ordinary source-profile to destination-profile color transform.
  Admitted paths cover RGB, Gray, CMYK and XYZ/CIELAB according to the selected
  engine, profile admission rules and available transform direction.
- Each invocation converts one explicitly selected color group. Internal alpha
  and unrelated components pass through without color conversion.
- A transform is not required to be invertible. Converting back need not recover
  source samples or an original CMYK ink separation.
- B applies one DeviceLink profile's already constructed mapping. It does not
  expose a new ordinary two-profile intent selection as if rebuilding that map.
- C constructs one engine transform from an explicitly ordered profile chain.
  Its intermediate computation is not equivalent to a sequence of A nodes with
  separately published and rounded tensors. B/C use admitted models/profiles;
  arbitrary third-party engine plugins are outside this family.
- Softproofing and gamut-warning products are separately specified in
  [FMT-18](FMT-18_softproof_contract.md) on this engine/resource foundation.

## Current implementation boundary

[IccProfile](../../../../include/photospider/data/icc_profile.hpp) currently
admits immutable ICC v2/v4 CMYK output-device resources. It validates and owns
bytes; it does not execute a CMM. Its current admission surface does not imply
support for all profile classes or models listed above. Broader admission and
the transform adapter remain implementation dependencies.

## Confirmed engine boundary

The first adapter targets the CPU implementation of Little CMS 2.19.1, fixed
to its release source and explicitly specified build/transform settings. System
ColorSync or Windows CMM is not an interchangeable execution backend. A later
engine upgrade requires an explicit contract revision; runtime version
substitution is not allowed. This selection does not promise iccMAX support.
Numerical conformance is defined by the execution recipe below.

## Confirmed profile scope

Admit ICC v2/v4 only, with Gray, RGB, CMYK, XYZ or CIELAB color endpoints.
A accepts input, display, output and color-space profile classes, subject to
the actual required transform direction being available. B accepts DeviceLink.
C additionally admits abstract profiles as intermediate PCS transforms.
Named Color, spot/multichannel ink models and iccMAX are outside this first
contract and fail admission explicitly. Profile header class alone does not
prove that the requested transform is usable. Validate directions/tags and
the actual constructed chain as specified below.

## Confirmed sample representation

Color inputs and outputs retain the same Float32 or Float64 dtype. Project-side
RGB/Gray device coordinates and CMYK ink coordinates use nominal 0..1; CIELAB
uses l=L*/100 with unchanged a*/b*; XYZ uses relative reference-white Y=1.
The adapter explicitly translates these units to and from the selected engine
formatters. Integers require prior FMT-06 conversion and absolute luminance
requires explicit normalization before this adapter.

Float64 ports do not promise a Float64 internal pipeline. Little CMS 2.19.1
floating transforms include Float32 stages, including narrowing double input
formatters. Output dtype preservation is distinct from computational precision.
Use double formatters for both public dtypes. Widen a Float32 input exactly to
Float64; CIELAB l and every CMYK ink then use RN64(100*x). Other coordinates
have no adapter scale. After engine output, those same coordinates use
RN64(x/100), then RN_input_dtype; other coordinates use only the final dtype
rounding. Adapter operations inherit NUM round-to-nearest ties-to-even; the
engine retains its separately defined internal arithmetic. These explicit
rounding boundaries must not be fused or replaced with dtype-specific formatters.

## Confirmed ordinary intent and black-point handling

A exposes perceptual, relative_colorimetric, saturation and
absolute_colorimetric, defaulting to relative_colorimetric. Requested bpc is
off/on, default off. Reject absolute_colorimetric with requested bpc=on.
For each v4 perceptual/saturation stage, the pinned engine forces its BPC array
entry on even when requested bpc=off; this affects black-point handling where
that stage applies a PCS connection. A device_to_pcs stage does not itself apply
connection BPC. Do not infer effective BPC solely from the source being v4.
Record requested settings and the effective engine rules separately. B applies
its prebuilt DeviceLink rather than exposing these as map-rebuilding controls;
C expands the same defaults/rules into its explicit per-stage settings.

Fix applicable engine adaptation state to 1.0 (full adaptation); no partial
adaptation parameter is exposed in this revision. Media white/PCS processing
follows the selected profile and engine. ICC absolute_colorimetric does not
change endpoint units to cd/m² and does not insert a separate FMT-10C node.

The selected engine's fixed intent fallback and matrix/TRC paths are admitted.
Absence of a dedicated intent table does not alone reject a profile; reject if
the required direction cannot be constructed under the admitted engine path.
Transform construction diagnostics must identify the actual selected tags,
fallback or matrix/TRC path. A header or cmsIsIntentSupported result alone is
not proof of the actual linked behavior.

## Confirmed profile-defined interpretation

Allow a complete profile-defined color space whose immutable profile and
native endpoint coordinate units define its meaning. Arbitrary LUT-based RGB
or Gray profiles need not decompose into analytic primaries/transfer or one of
FMT-11's four Gray kinds. Do not invent those fields. Use an explicit conversion
to an identified analytic endpoint before consumers that require those fields.
An ordinary source-profile assertion must match the input's bound identity;
source reinterpretation requires explicit override. Adding this descriptor is
a generic metadata migration, not support already present in ColorArray v1.

An explicit endpoint binding may associate a frozen profile identity with a
complete analytic space. This is a caller assertion, not inferred from profile
names, textual tags or a set of matching samples. Check model, units, component
order and declared reference/white convention. A binding does not transform
samples or prove equivalence of an arbitrary LUT with an analytic formula.
If conventions differ, require explicit conversion or reinterpretation. Retain
the binding's identity/provenance with the output.

ICC PCS D50 is not automatically the analytic D50 xy preset. Keep the pinned
engine's PCS convention, its D50 XYZ constants and profile encoding precision
distinct from an analytic xy declaration. Native XYZ/Lab consumers require an
explicitly compatible analytic binding or conversion/override. Engine-internal
PCS conversion/media-white processing creates no hidden FMT-09/10/11 nodes.
The pinned engine's native D50 constants are RN64(0.9642), 1 and RN64(0.8249);
serialized profile header illuminants retain their actual fixed-point bytes.
Neither is silently replaced by analytic D50 xy=(0.3457,0.3585). A profile's
device-white tags also remain distinct from PCS reference-white interpretation.

## Confirmed DeviceLink interpretation

B fixes engine LUT selection to the DeviceLink header intent; do not expose
additional BPC or adaptation controls. Require explicit complete entry and exit
space descriptions matching the link's endpoint model signatures and channel
counts. Their association with this mapping is a caller declaration, not a
fact proved by optional profile-sequence tags. Respect checks the entry against
the input interpretation; override explicitly changes that interpretation.
The output publishes the declared complete exit space and owns its required
resources. A bare link-exit label is not the chosen complete-image interface.

Names and optional sequence tags do not establish byte identity or validate
the caller's colorimetric assertion. Resource ownership and endpoint structural
consistency remain mandatory even when interpretation is explicitly overridden.

## Confirmed chain declaration

C explicitly declares each stage's direction and intent/BPC settings; whole-chain
defaults may expand into those per-stage fields. Ordinary profiles declare
device-to-PCS or PCS-to-device. Abstract and DeviceLink stages are forward only;
DeviceLink uses its header intent. Compilation checks that these declarations
match the pinned engine's actual connection directions and rejects mismatch.
An ordered list is not permission to silently reverse a declared stage.
If ordinary endpoint profiles do not establish the chain's complete entry/exit
space, require explicit endpoint descriptions as for B.

## Confirmed semantic sample domain

Semantic RGB, Gray, XYZ and CIELAB admit finite signed/HDR coordinates. CMYK
requires each consumed ink coordinate in [0,1]. Pass admitted finite extensions
without an extra adapter clamp; profile curves/CLUTs may themselves clip or
otherwise lose HDR information. No HDR-preservation guarantee follows from
floating endpoint types. Consumed inputs and requested outputs must be finite;
requested CMYK outputs must also lie in [0,1]. Reject nonfinite source samples
before calling the engine.

Raw uses explicit profiles and the same unit adapter, while skipping semantic
metadata consistency and CMYK nominal-range checks. All modes reject nonfinite
consumed inputs/requested results and overflow during unit adaptation or entry
into the engine's Float32 pipeline. Raw does not inherit NUM NaN-payload
propagation through this engine. Retain only applicable unverified metadata;
raw does not automatically publish a valid destination color-space declaration.

## Confirmed output group placement

When source and target color counts match, assign target roles in model order
to the source group's ordered slots, preserving positions. When they differ,
replace the group at its smallest physical slot with consecutive target-model
components and delete its other original slots. Preserve the relative order
of all unrelated components and remap metadata, including internal alpha.
Axis-free Gray that expands to multiple channels requires explicit output_axis.
No resampling or change to non-channel dimensions occurs.

## Confirmed point support

Any requested transformed color component reads the complete selected source
color group at that coordinate. Spatial demand is exact with no halo. The engine
may compute all target color components into scratch; publish and validate only
the requested components. An unrequested target nonfinite sample alone does
not fail another component; an actual shared engine failure remains a failure.
Bypass alpha/AOV requests read only their mapped source component and do not
evaluate color. Requested hidden color is still transformed at alpha=0 without
reading alpha. Dirty propagation maps a changed source color component to every
target color component at the same coordinates; bypass mapping stays individual.

## Confirmed execution-profile scope

Provide static execution=optimized (default) and execution=reference. Optimized
uses stock Little CMS default optimization, without FORCE_CLUT, optional
high/low-resolution precalculation flags or extra acceleration plugins.
Reference sets NOOPTIMIZE; both set NOCACHE and permit the pinned engine's
mandatory pre-optimization. NOOPTIMIZE does not preserve every internal stage
or its pre-optimization rounding. Neither configuration inherits NUM strict
or accelerated four-ULP semantics.

Within one identified build and selected configuration, repeated execution,
ROI splitting, batching and worker scheduling must preserve output bits.
Optimized may differ numerically from reference; compare fixed profile/color
fixtures and report differences. It is not a transparent bit-equivalent rewrite,
and the optimizer must not substitute it for a requested reference invocation.

## Confirmed layout and identity policy

layout=auto (default) or materialize produces transformed samples into requested
regions of the output's full logical planar reservation. Reject forced view.
Equal source/destination profile identities do not turn an invocation into a
copy: retain the engine path, its PCS/curve processing and rounding. An alpha-only
bypass may reuse a valid read window but does not make the complete node a view.
Such reuse concerns source access; published materialized output still uses one
legal result owner/reservation and never stitches source-owned bypass planes
into a new cross-owner image. Forced materialize copies requested bypass samples
as well as transformed samples.

## Static interface and member identity

A, B and C are native external-engine adapters, not NUM primitives or
compile-time compositions of each other. Internal implementation may be shared.
Each takes an `input` tensor and produces one `values` tensor. Frozen profile
resources are explicit compile-time bindings, never mutable pixel-valued ports.
No GPU or platform-specific accelerated alias is specified.

| Member | Proposed operation key | Profile parameters |
| --- | --- | --- |
| [A](FMT-12A_transform_icc_profiles.md) | color.icc_transform_lcms2_19_1_cpu | source_profile, destination_profile |
| [B](FMT-12B_apply_icc_devicelink.md) | color.icc_apply_devicelink_lcms2_19_1_cpu | link_profile, entry_space, exit_space |
| [C](FMT-12C_transform_icc_chain.md) | color.icc_transform_chain_lcms2_19_1_cpu | stages, applicable endpoint declarations |

Selectors, profile identities, stage order/directions, descriptions, execution,
intent/BPC, output_axis and layout are static. Changes require re-inference and
transform reconstruction. Pixel values may change under the same input schema.
A may resolve an omitted source_profile from a complete attached profile-defined
description; explicit source assertions must agree in respect mode. Destination
profile is always explicit. Raw requires explicit source profile/endpoint data.

Semantic modes select one complete group; raw specifies ordered distinct source
slots and explicit profile/endpoint parameters. Resolve axes using FMT common
rules, never inferred HWC/CHW. Channel counts are Gray=1, RGB/XYZ/Lab=3, CMYK=4.
Only native Float32/Float64 coordinates are admitted; non-native numeric code
encodings require prior explicit decoding. Ordinary NUM checked-size/rank limits
and the kernel's actual image-axis capabilities remain applicable.

C admits 2..255 stages; use B for one DeviceLink. Abstract profiles are
intermediate PCS stages. Adjacent signatures must be compatible, and all stages
remain within the admitted model/PCS set. Check ordinary declared directions
against the actual pinned linker; record inserted XYZ/Lab PCS bridges in the
construction trace. Matching channel counts alone do not establish a valid link.
Ordinary/abstract stages inherit relative_colorimetric; applicable BPC defaults
to off and is overridable under A's effective rules. DeviceLink fixes header intent and
rejects user BPC/adaptation parameters. Unused API array entries are internal
bookkeeping, not user-visible controls.

An ordinary device_to_pcs stage does not apply the linker's PCS-connection BPC.
Its bpc field is absent; whole-chain BPC defaults apply only where the engine
uses that control. Supply the API's internal placeholder and record effective
not_applicable. Reject a user BPC override there rather than implying an extra
black-point operation. Intent still selects the stage's profile path. Abstract
PCS stages retain their applicable connection settings.

An existing channel axis remains present after conversion to one-channel Gray.
Axis-free Gray-to-Gray remains axis-free; expansion uses explicit output_axis.
Reject overlapping group/reference structures that cannot remain consistent
after replacement, rather than silently changing unrelated groups.

## Published interpretation and request behavior

Respect/override publish the actual target model/space, native units and
straight alpha structure. Remove source-only fields, rebuild selected-group
roles and remap surviving references. Other groups, applicable names/coordinates
and opaque annotations follow common propagation rules. Names are not roles or
profile identity. Retain output profile/binding owners after context destruction.
Raw does not publish the target declaration as a semantic guarantee; remove
structurally incompatible descriptions and retain applicable ones unverified.

Validate the full static interface and construct the transform even for empty
spatial or bypass-only requests. This reads resources, not image samples.
Bypass samples preserve all bits, including NaN payloads and signed zeros.
Pixel validation follows the exact point/group support above; an upstream Whole
node retains its own support/failure scope. Unproduced samples remain missing.
Physical page/row-padding access does not enlarge logical sample validation.

## Fixed engine execution recipe

Use private contexts and double planar formatters for admitted endpoint models.
Prepare bounded planar scratch for the engine's uniform strides when needed;
this does not introduce interleaved public images. Add no engine alpha/extra
channels. Copy bypass planes outside the CMM and retain explicit unit rounding.

- reference flags: NOOPTIMIZE | NOCACHE, with applicable ordinary A BPC flag.
  C supplies BPC per stage.
- optimized flags: NOCACHE, with the same BPC setup and stock optimization.
  No FORCE_CLUT, HIGHRESPRECALC, LOWRESPRECALC, NONEGATIVES, COPY_ALPHA,
  NULLTRANSFORM, proof/gamut flags or white-fixup overrides. Stock optimized
  white handling is part of this configuration.
- No color algorithm/interpolation/formatter/acceleration plugins. Required
  host accounting/error/cancellation integration is separately identified.
- Fix nearest-even with gradual underflow, disable fast-math/reassociation and
  implicit FMA contraction, and restore the caller floating environment.
  Record compiler/architecture/math runtime; libm is not thereby cross-build
  bit-identical. Other numerical build options, including the profile black
  point tag option, retain pinned release defaults.

Check normalized Float32 input-formatter results for finiteness before pipeline
evaluation. Public Float64 finiteness alone is insufficient. Adapter/formatter
overflow and requested output overflow fail in all modes. Finite underflow
follows the selected arithmetic without epsilon replacement. Internal profile
clipping remains part of the engine, distinct from an adapter clamp. Unrequested
scratch results establish neither published coverage nor validity guarantees.

## Numerical conformance and compatibility

The normative result is the identified release/integration/build and execution
configuration's output, enclosed by explicit unit-adapter rounding. This is an
external-engine contract, not a correctly rounded universal ICC formula.
Profile measurement, quantization, interpolation and intent rendering may be
lossy. No universal bound relates arbitrary profiles or the two configurations.

An independent direct-CMM harness with identical frozen bytes, formatters,
settings and build checks integration bit-for-bit. Independent analytic and
hand-constructed profile fixtures separately check units and simple mappings;
comparing two wrapper paths alone is insufficient. Each admitted platform/build
must record both configurations on a fixed profile/color corpus. Report native
coordinate differences and color differences only after an explicit common PCS
conversion. Corpus measurements do not establish a universal four-ULP/Delta-E
bound. Same-build repeat/ROI/batch/worker determinism remains exact. No unmeasured
cross-build, Adobe/OS CMM equivalence or photographic-quality claim is made.

## Errors and publication

Inherit NUM error delivery, required-upstream propagation, checked arithmetic
and transactional publication. Admission/inference rejects unsupported
dtype/shape/channel structure with TypeMismatch. Malformed/disallowed profiles,
invalid parameters, metadata conflicts, missing endpoints, incompatible chains,
forced view and construction failure use InvalidArgument/InvalidDomain with
profile/stage detail. Consumed nonfinite/out-of-domain values, adapter/formatter
overflow and requested invalid outputs use OperationFailed with sample
coordinate/component: InvalidDomain for invalid input or range, and
ArithmeticOverflow for adapter/formatter narrowing overflow or a requested
nonfinite result from finite input. Host resource failures use ResourceExhausted and
the applicable capacity/work/stage reason; cancellation/currentness retain their
inherited status and take precedence when latched by integration hooks.

An integration invariant failure is not an invalid-profile error. Diagnostics
identify member, execution configuration, profile/stage identity and effective
settings without dumping resource bytes. Never publish invalid requested
samples or fall back to another engine/configuration, identity copy or invented
profile after failure.

## Inherited resource obligations

Profiles are explicitly supplied immutable byte resources, not runtime paths,
system-monitor lookups or ambient application defaults. Keep frozen bytes and
their content identity alive for consumers that need them; a filename or an ICC
header Profile ID is not sufficient ownership. Profile loading, parser scratch,
engine state and image windows participate in the host's resource budgets and
cancellation contract. The selected external numerical contract does not waive
these kernel obligations. Concrete adapter enforcement must satisfy the
requirements below and be verified before runtime acceptance.

Engine identity includes the exact release/integration revision, compiler and
numerically relevant build options, platform/math runtime, formatters and chosen
execution configuration. The integer LCMS_VERSION value alone is insufficient
to distinguish releases. Cache/validation identity also includes all frozen
profile identities, ordered stages, requested/effective settings, group mapping,
mode, endpoint interpretation and dtype. No hidden system profile lookup occurs.

Per-context memory hooks require audited bootstrap/global-allocation handling;
do not assume all context-creation memory passes through the installed local
allocator. Reserve audited fixed overhead or correct and validate the integration
path, without mutating a process-global allocator to serve one DAG. Temporary
engine/formatter storage is private scratch; public images remain planar.

Upstream memory callbacks alone do not provide bounded cancellation/work probes
through all parse/link/BPC loops. A conforming integration must add and audit
necessary host work/cancellation hooks without changing successful numerical
behavior, and identify that integration revision with the engine build. Never
claim that checking only before/after cmsCreateTransform supplies kernel-level
bounded construction cancellation. Latch allocator/work/cancellation failure
independently and reject construction even if an internal engine fallback hides
a failed helper and returns a non-null transform. These are implementation
prerequisites, not features demonstrated by this documentation.

Use private engine contexts and execution-local transform instances for active
workers. Do not share mutable profile parsing/transform state concurrently or
change process-global intent, adaptation, alarms or plugin state. The immutable
input resource may be shared; each retaining root accounts for its reference.
Internal memory/work/error hooks are host integration, not optional color
algorithm plugins. Disabling caches must still permit correct construction and
execution. Reused compiled state requires explicit ownership and accounting.

Bound pixel batches and all profile/tag/curve/CLUT/link loops by checked work
counts. Check cancellation/currentness before and after engine calls and at
bounded internal hook intervals; no detached engine work may outlive cancellation.
Do not use asynchronous thread termination. Release failed construction, scratch
and unpublished writes; published results/read windows retain normal ownership.
No engine optimization or precision fallback may be triggered by budget failure.

## Acceptance and implementation prerequisites

These are future implementation gates, not results of this documentation turn.

1. Independently specify compact identity/constant/affine DeviceLink fixtures
   and ordinary matrix/TRC profiles, with frozen bytes and expected coordinates.
   Include Lab l=0.5 <-> engine L*=50 and CMYK 0.5 <-> engine 50 percent;
   exact constants, black/white and branch/table boundary samples must be
   checked against analytical values as well as a direct engine harness.
2. Add fixed real RGB/Gray/CMYK profiles, v2/v4, both PCS encodings, matrix/TRC
   and CLUT paths, intent fallback and a chain with an abstract stage. Record
   resource provenance and all settings. Separate engine integration correctness
   from measured profile/color quality and optimized/reference differences.
3. Test noncanonical roles and group replacement. For seven input slots with
   source RGB ordered as [5,1,3] and alpha at 2, conversion to CMYK produces
   [old0,C,M,Y,K,old2,old4,old6]; alpha moves to 5. Same-count XYZ instead uses
   X at 5, Y at 1, Z at 3. One-channel and axis-free Gray cases cover insertion.
4. Require whole observation versus disjoint/offset/cross-tile ROIs, split batches,
   thread schedules and repeated execution to match bits within each build and
   configuration. Test each requested color separately and jointly. Unused
   peers in the source color group are still consumed; bypass planes are not.
5. Reject NaN/Inf, finite unit/formatter overflow and semantic CMYK out-of-range.
   Exercise negative/HDR finite coordinates, engine clipping, underflow/signed
   zeros and invalid unrequested target values without introducing global sample
   scans. Alpha-only output preserves arbitrary alpha bits.
6. Exercise source/profile conflicts, explicit override/raw, missing or malformed
   endpoint binding, named-space/profile identity distinction, incompatible
   topology and unsupported versions/classes/directions. A valid header without
   a usable transform fails admission. Equal profiles do not justify view.
7. Inject root capacity/work/stage failure and cancellation during profile copy,
   parsing, curve/CLUT expansion, BPC construction and pixel batches, including
   an engine helper failure otherwise swallowed by fallback. Check context
   bootstrap accounting, no hidden allocation escape and complete cleanup.
8. Verify cache-off, concurrent independent contexts, caller FP environment
   restoration, immutable resource reuse, result/read-window survival after
   context teardown and final release. No successful engine state may survive a
   latched cancellation or resource refusal as an apparently valid transform.
9. Supply an actual minimal public workflow and commands after implementation,
   plus directly checkable expectations. Benchmark transform construction and
   steady evaluation separately for each configuration; report managed memory
   and platform/build identity, without extrapolating unmeasured GPU/OS support.

Required implementation work includes broadening ICC resource admission,
generic profile-defined/analytic endpoint metadata, canonical planar bindings,
all host accounting/cancellation hooks and actual-path diagnostics. The existing
CMYK ramp importer and legacy ColorArray v1 do not satisfy those requirements.
The engine can impose internal losses even with normalized public coordinates.
No compatibility alias restores the retired color operations.

Named Color, more-than-four-ink/spectral models, iccMAX, profile creation/export,
black-preserving proprietary intent variants, partial viewing adaptation and
additional engine/plugin backends are not part of this revision. DeviceLink can
carry an explicitly supplied separation map; B does not create that map. [FMT-18](FMT-18_softproof_contract.md)
defines separate proofing and gamut-warning controls, including its explicitly
identified source-dimensional alarm-table correction. Ordinary FMT-12 paths do
not enable gamut checking and retain this family's numerical recipe.

## Primary sources inspected

- [ICC introduction to the profile format](https://www.color.org/getting-started/):
  profile connection space and rendering-intent context.
- [Little CMS 2.19.1](https://github.com/mm2/Little-CMS/releases/tag/lcms2.19.1):
  selected CPU engine release, inspected on 2026-09-23.
- [Pinned float formatters](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmspack.c):
  external units and Float64-to-Float32 pipeline boundary.
- [Pinned transform linker](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmscnvrt.c):
  standard intents and effective BPC policy.
- [Pinned profile paths](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsio1.c):
  direction-specific tags, fallback and optional sequence metadata.
- [Pinned pipeline evaluator](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmslut.c)
  and [transform adapter](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsxform.c):
  Float32 stage boundaries and extended-transform setup.
- [Pinned optimization](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsopt.c):
  mandatory pre-optimization before NOOPTIMIZE and optional stock paths.
- [Memory-plugin API](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/include/lcms2_plugin.h)
  and [context implementation](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsplugin.c):
  accounting integration and bootstrap boundaries.
