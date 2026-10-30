---
spec_schema_version: 1
id: FMT-12
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# FMT-12: ICC profile transforms

Three CPU adapters (A ordinary transform, B DeviceLink, C chain) run the pinned Little CMS 2.19.1 engine.

## Inherited contracts

Structural tensors and explicit configuration, planar storage, straight internal color, call-local interpretation and unchecked consumed-semantic premises follow [FMT-common](FMT_common_contract.md). NUM strict rounding or accelerated four-ULP guarantees do not apply to the engine. The [relative-coordinate scale](FMT_relative_coordinate_scale.md) requires native CIELAB lightness l=L*/100. Engine boundary units are specified below.

## Scope

- A is an ordinary source-profile to destination-profile color transform. Admitted paths cover RGB, Gray, CMYK and XYZ/CIELAB according to the selected engine, profile admission rules and available transform direction.
- Each invocation converts one explicitly selected color group. Internal alpha and unrelated components pass through without color conversion.
- A transform is not required to be invertible. Converting back need not recover source samples or an original CMYK ink separation.
- B applies one DeviceLink profile's already constructed mapping. It does not expose a new ordinary two-profile intent selection as if rebuilding that map.
- C constructs one engine transform from an explicitly ordered profile chain. Its intermediate computation is not equivalent to a sequence of A nodes with separately published and rounded tensors. B/C use admitted models/profiles; arbitrary third-party engine plugins are outside this family.
- Softproofing and gamut-warning products are separately specified in [FMT-18](FMT-18_softproof_contract.md) on this engine/resource foundation.

## Engine boundary

The first adapter targets the CPU implementation of Little CMS 2.19.1, fixed to its release source and explicitly specified build/transform settings. System ColorSync or Windows CMM is not an interchangeable execution backend. A later engine upgrade requires an explicit contract revision; runtime version substitution is not allowed. This selection does not promise iccMAX support. Numerical conformance is defined by the execution recipe below.

## Profile scope

Admit ICC v2/v4 only, with Gray, RGB, CMYK, XYZ or CIELAB color endpoints. A accepts input, display, output and color-space profile classes, subject to the actual required transform direction being available. B accepts DeviceLink. C additionally admits abstract profiles as intermediate PCS transforms. Named Color, spot/multichannel ink models and iccMAX are outside this first contract and fail admission explicitly. Profile header class alone does not prove that the requested transform is usable. Validate directions/tags and the actual constructed chain as specified below.

## Sample representation

Color inputs and outputs retain the same Float32 or Float64 dtype. Project-side RGB/Gray device coordinates and CMYK ink coordinates use nominal 0..1; CIELAB uses l=L*/100 with unchanged a*/b*; XYZ uses relative reference-white Y=1. The adapter explicitly translates these units to and from the selected engine formatters. Integers require prior FMT-06 conversion and absolute luminance requires explicit normalization before this adapter.

Float64 ports do not promise a Float64 internal pipeline. Little CMS 2.19.1 floating transforms include Float32 stages, including narrowing double input formatters. Output dtype preservation is distinct from computational precision. Use double formatters for both public dtypes. Widen a Float32 input exactly to Float64; CIELAB l and every CMYK ink then use RN64(100*x). Other coordinates have no adapter scale. After engine output, those same coordinates use RN64(x/100), then RN_input_dtype; other coordinates use only the final dtype rounding. Adapter operations inherit NUM round-to-nearest ties-to-even; the engine retains its separately defined internal arithmetic. These explicit rounding boundaries must not be fused or replaced with dtype-specific formatters.

## Ordinary intent and black-point handling

A exposes perceptual, relative_colorimetric, saturation and absolute_colorimetric, defaulting to relative_colorimetric. Requested bpc is off/on, default off. Reject absolute_colorimetric with requested bpc=on. For each v4 perceptual/saturation stage, the pinned engine forces its BPC array entry on even when requested bpc=off; this affects black-point handling where that stage applies a PCS connection. A device_to_pcs stage does not itself apply connection BPC. Do not infer effective BPC solely from the source being v4. Record requested settings and the effective engine rules separately. B applies its prebuilt DeviceLink rather than exposing these as map-rebuilding controls; C expands the same defaults/rules into its explicit per-stage settings.

Fix applicable engine adaptation state to 1.0 (full adaptation); no partial adaptation parameter is exposed in this revision. Media white/PCS processing follows the selected profile and engine. ICC absolute_colorimetric does not change endpoint units to cd/m² and does not insert a separate FMT-10C node.

The selected engine's fixed intent fallback and matrix/TRC paths are admitted. Absence of a dedicated intent table does not alone reject a profile; reject if the required direction cannot be constructed under the admitted engine path. Transform construction diagnostics must identify the actual selected tags, fallback or matrix/TRC path. A header or cmsIsIntentSupported result alone is not proof of the actual linked behavior.

## Profile-defined interpretation

Allow a complete profile-defined color space whose immutable profile and native endpoint coordinate units define its meaning. Arbitrary LUT-based RGB or Gray profiles need not decompose into analytic primaries/transfer or one of FMT-11's four Gray kinds. Do not invent those fields. Use an explicit conversion to an identified analytic endpoint before consumers that require those fields. Each operation receives its source profile and interpretation explicitly. Profile-defined endpoint configuration is supported by the SDK `TensorDescription` codec. It does not attach a descriptor to data. The registered adapters own LCMS execution; numeric tuple helpers do not execute the engine.

An explicit endpoint binding may associate a frozen profile identity with a complete analytic space. This is a caller assertion, not inferred from profile names, textual tags or a set of matching samples. Check model, units, component order and declared reference/white convention. A binding does not transform samples or prove equivalence of an arbitrary LUT with an analytic formula. If conventions differ, require explicit conversion or reinterpretation. Retain the binding in preparation and pass it explicitly to later consumers.

ICC PCS D50 is not automatically the analytic D50 xy preset. Keep the pinned engine's PCS convention, its D50 XYZ constants and profile encoding precision distinct from an analytic xy declaration. Native XYZ/Lab consumers require an explicitly compatible analytic binding or conversion or explicit reinterpretation. Engine-internal PCS conversion/media-white processing creates no hidden FMT-09/10/11 nodes. The pinned engine's native D50 constants are RN64(0.9642), 1 and RN64(0.8249); serialized profile header illuminants retain their actual fixed-point bytes. Neither is silently replaced by analytic D50 xy=(0.3457,0.3585). A profile's device-white tags also remain distinct from PCS reference-white interpretation.

## DeviceLink interpretation

B fixes engine LUT selection to the DeviceLink header intent; do not expose additional BPC or adaptation controls. Require explicit complete entry and exit space descriptions matching the link's endpoint model signatures and channel counts. Their association with this mapping is a caller declaration, not a fact proved by optional profile-sequence tags. Static preparation checks the explicit entry and exit configuration against endpoint structure. The plan owns required resources; output data does not carry an exit description.

Names and optional sequence tags do not establish byte identity or validate the caller's colorimetric assertion. Resource ownership and endpoint structural consistency remain mandatory for every explicitly supplied interpretation.

## Chain declaration

C explicitly declares each stage's direction and intent/BPC settings; whole-chain defaults may expand into those per-stage fields. Ordinary profiles declare device-to-PCS or PCS-to-device. Abstract and DeviceLink stages are forward only; DeviceLink uses its header intent. Compilation checks that these declarations match the pinned engine's actual connection directions and rejects mismatch. An ordered list is not permission to silently reverse a declared stage. If ordinary endpoint profiles do not establish the chain's complete entry/exit space, require explicit endpoint descriptions as for B.

## Semantic sample domain

Semantic RGB, Gray, XYZ and CIELAB admit finite signed/HDR coordinates. CMYK requires each consumed ink coordinate in [0,1]. Pass admitted finite extensions without an extra adapter clamp; profile curves/CLUTs may themselves clip or otherwise lose HDR information. No HDR-preservation guarantee follows from floating endpoint types. Consumed inputs and requested outputs must be finite; requested CMYK outputs must also lie in [0,1]. These are unchecked caller premises. Neither the adapter nor its private bridge scans inputs or completed outputs to validate them.

The proposed finite-only raw path uses explicit profiles and the same unit adapter, without the described CMYK nominal-range premise. Finite consumed inputs, unit adaptation, Float32 pipeline values and requested outputs are unchecked premises in both forms. Raw does not inherit NUM NaN-payload propagation through this engine. Neither path attaches a color description to its output.

## Output group placement

When source and target color counts match, assign target roles in model order to the source group's ordered slots, preserving positions. When they differ, replace the group at its smallest physical slot with consecutive target-model components and delete its other original slots. Preserve the relative order of all unrelated components and remap data slots, including same-tensor alpha. Callers update the explicit configuration supplied to downstream operations. Axis-free Gray that expands to multiple channels requires explicit output_axis. No resampling or change to non-channel dimensions occurs.

## Point support

Any requested transformed color component reads the complete selected source color group at that coordinate. Spatial demand is exact with no halo. The engine may compute all target color components and publishes the requested components without a result-validity scan. Actual shared engine execution failures still propagate; sample-domain violations have no required diagnostic. Bypass alpha/AOV requests read only their mapped source component and do not evaluate color. Requested hidden color is still transformed at alpha=0 without reading alpha.

## Execution profiles

Provide static execution=optimized (default) and execution=reference. Optimized uses stock Little CMS default optimization, without FORCE_CLUT, optional high/low-resolution precalculation flags or extra acceleration plugins. Reference sets NOOPTIMIZE; both set NOCACHE and permit the pinned engine's mandatory pre-optimization. NOOPTIMIZE does not preserve every internal stage or its pre-optimization rounding. Neither configuration inherits NUM strict or accelerated four-ULP semantics.

Within one identified build and selected configuration, repeated execution, ROI splitting, batching and worker scheduling must preserve output bits. Optimized may differ numerically from reference; compare fixed profile/color fixtures and report differences. It is not a transparent bit-equivalent rewrite, and the optimizer must not substitute it for a requested reference invocation.

## Layout and identity policy

layout=auto (default) or materialize produces independent dense allocations for selected complete output tiles. Reject forced view. Equal source/destination profile identities do not turn an invocation into a copy: retain the engine path, its PCS/curve processing and rounding. An alpha-only bypass may reuse a valid read window but does not make the complete node a view. Such reuse concerns source access. Every materialized output tile has its own allocation; bypass samples are copied into those tiles together with transformed samples. No source-owned plane is installed as a produced tile alias.

## Static interface and member identity

A, B and C are native external-engine adapters, not NUM primitives or compile-time compositions of each other. Internal implementation may be shared. Each takes an `input` tensor and produces one `values` tensor. Frozen profile resources are explicit compile-time bindings, never mutable pixel-valued ports. No GPU or platform-specific accelerated alias is specified.

| Member | Operation key | Profile parameters |
| --- | --- | --- |
| [A](FMT-12A_transform_icc_profiles.md) | color.icc_transform_lcms2_19_1_cpu | source_profile, destination_profile |
| [B](FMT-12B_apply_icc_devicelink.md) | color.icc_apply_devicelink_lcms2_19_1_cpu | link_profile, entry_space, exit_space |
| [C](FMT-12C_transform_icc_chain.md) | color.icc_transform_chain_lcms2_19_1_cpu | stages, applicable endpoint declarations |

Selectors, profile identities, stage order/directions, descriptions, execution, intent/BPC, output_axis and layout are static. Changes require re-inference and transform reconstruction. Pixel values may change under the same input schema. Source and destination profile/endpoint data are explicit static parameters. Preparation returns their resource identities for host resolution. No profile is selected from a Result or Tensor.

The description path selects one complete configured group; raw specifies ordered distinct source slots and explicit profile/endpoint parameters. Resolve axes using FMT common rules, never inferred HWC/CHW. Channel counts are Gray=1, RGB/XYZ/Lab=3, CMYK=4. Only native Float32/Float64 coordinates are admitted; non-native numeric code encodings require prior explicit decoding. Ordinary NUM checked-size/rank limits and the kernel's actual image-axis capabilities remain applicable.

C admits 2..255 stages; use B for one DeviceLink. Abstract profiles are intermediate PCS stages. Adjacent signatures must be compatible, and all stages remain within the admitted model/PCS set. Check ordinary declared directions against the actual pinned linker; record inserted XYZ/Lab PCS bridges in the construction trace. Matching channel counts alone do not establish a valid link. Ordinary/abstract stages inherit relative_colorimetric; applicable BPC defaults to off and is overridable under A's effective rules. DeviceLink fixes header intent and rejects user BPC/adaptation parameters. Unused API array entries are internal bookkeeping, not user-visible controls.

An ordinary device_to_pcs stage does not apply the linker's PCS-connection BPC. Its bpc field is absent; whole-chain BPC defaults apply only where the engine uses that control. Supply the API's internal placeholder and record effective not_applicable. Reject a user BPC override there rather than implying an extra black-point operation. Intent still selects the stage's profile path. Abstract PCS stages retain their applicable connection settings.

An existing channel axis remains present after conversion to one-channel Gray. Axis-free Gray-to-Gray remains axis-free; expansion uses explicit output_axis. Reject overlapping group/reference structures that cannot remain consistent after replacement, rather than silently changing unrelated groups.

## Output interpretation and request behavior

The target profile and endpoint configuration define how callers interpret the computed coordinates. Operations publish structure and numeric data only. Callers supply destination configuration to later consumers and adjust explicit channel mappings when component counts change.

Validate the full static interface and construct the transform even for empty spatial or bypass-only requests. This reads resources, not image samples. Bypass samples preserve all bits, including NaN payloads and signed zeros. Pixel computation follows the exact point/group support above; an upstream complete-input node retains its own support/failure scope. Unproduced samples remain missing.

## Execution recipe

The engine uses private contexts and double planar formatters for admitted endpoint models. No engine alpha/extra channels are added. Bypass planes are copied outside the CMM and unit rounding remains explicit.

- reference flags: NOOPTIMIZE | NOCACHE, with the applicable ordinary A BPC flag. C supplies BPC per stage.
- optimized flags: NOCACHE, with the same BPC setup and stock optimization. No FORCE_CLUT, HIGHRESPRECALC, LOWRESPRECALC, NONEGATIVES, COPY_ALPHA, NULLTRANSFORM, proof/gamut flags or white-fixup overrides. Stock optimized white handling is part of this configuration.
- No color algorithm/interpolation/formatter/acceleration plugins.
- Arithmetic is nearest-even with gradual underflow; fast-math, reassociation and implicit FMA contraction are disabled. The caller's floating environment is restored. Other numerical build options, including the profile black point tag option, retain pinned release defaults.

Normalized Float32 formatter values and requested outputs satisfy the documented finite/representability premises (unchecked). Finite underflow follows the selected arithmetic without epsilon replacement. Internal profile clipping is part of the engine, distinct from an adapter clamp.

## Numerical conformance

The normative result is the output of the identified engine release and execution configuration, enclosed by the explicit unit-adapter rounding. This is an external-engine contract, not a correctly rounded universal ICC formula. Profile measurement, quantization, interpolation and intent rendering may be lossy. No universal bound relates arbitrary profiles or the two configurations. Within one identified build and configuration, repeated execution, ROI splitting, batching and worker scheduling preserve output bits.

## Errors and publication

Admission/inference rejects unsupported dtype/shape/channel structure with TypeMismatch. Malformed/disallowed profiles, invalid parameters, metadata conflicts, missing endpoints, incompatible chains, forced view and construction failure use InvalidArgument/InvalidDomain with profile/stage detail. Numerical parameters, consumed domains, representability of adaptation/narrowing and requested output validity are unchecked premises; no InvalidDomain/ArithmeticOverflow diagnostic is promised for their violation. A genuine private-engine execution failure remains OperationFailed. A failed computation publishes no successful result and does not fall back to another engine/configuration, identity copy or invented profile. Diagnostics identify member, execution configuration, profile/stage identity and effective settings.

## Profile resources and engine identity

Profiles are explicitly supplied immutable byte resources, not runtime paths, system-monitor lookups or ambient application defaults. A filename or an ICC header Profile ID does not identify a profile; the content identity does. Resource bytes are frozen content.

Engine identity includes the exact Little CMS 2.19.1 release, numerically relevant settings, formatters and the chosen execution configuration. Compiled processor identity also includes all frozen profile identities, ordered stages, requested/effective settings, group mapping, mode, endpoint interpretation and dtype. No hidden system profile lookup occurs. Engine construction is synchronous and a failed construction yields no transform: an internal engine fallback never hides a failed helper and returns a non-null transform.

## Acceptance

1. Compact identity/constant/affine DeviceLink fixtures and ordinary matrix/TRC profiles with frozen bytes and expected coordinates. Include Lab l=0.5 <-> engine L*=50 and CMYK 0.5 <-> engine 50 percent; exact constants, black/white and branch/table boundary samples are checked against analytical values as well as a direct engine harness.
2. Real RGB/Gray/CMYK profiles, v2/v4, both PCS encodings, matrix/TRC and CLUT paths, intent fallback and a chain with an abstract stage.
3. Noncanonical roles and group replacement. For seven input slots with source RGB ordered as [5,1,3] and alpha at 2, conversion to CMYK produces [old0,C,M,Y,K,old2,old4,old6]; alpha moves to 5. Same-count XYZ instead uses X at 5, Y at 1, Z at 3. One-channel and axis-free Gray cases cover insertion.
4. Whole observation versus disjoint/offset/cross-tile ROIs, split batches and repeated execution match bits within each build and configuration. Unused peers in the source color group are still consumed; bypass planes are not.
5. Fixtures satisfying the finite unit/formatter/output premises and semantic CMYK range. Negative/HDR coordinates, engine clipping, underflow and signed zeros. Alpha-only output preserves arbitrary alpha bits.
6. Explicit endpoint/profile conflicts, finite-only raw, missing or malformed endpoint binding, named-space/profile identity distinction, incompatible topology and unsupported versions/classes/directions. A valid header without a usable transform fails admission. Equal profiles do not justify view.

Named Color, more-than-four-ink/spectral models, iccMAX, profile creation/export, black-preserving proprietary intent variants, partial viewing adaptation and additional engine/plugin backends are outside this contract. DeviceLink can carry an explicitly supplied separation map; B does not create that map. [FMT-18](FMT-18_softproof_contract.md) defines separate proofing and gamut-warning controls; ordinary FMT-12 paths do not enable gamut checking.

## References

- [ICC introduction to the profile format](https://www.color.org/getting-started/)
- [Little CMS 2.19.1 float formatters](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmspack.c), [transform linker](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmscnvrt.c), [profile paths](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsio1.c), [pipeline evaluator](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmslut.c), [transform adapter](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsxform.c) and [optimization](https://github.com/mm2/Little-CMS/blob/lcms2.19.1/src/cmsopt.c)
