---
spec_schema_version: 1
id: FMT-13
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# FMT-13: configured OpenColorIO transforms

Six CPU adapters (A color space, B display/view, C Looks, D NamedTransform, E file, F transform tree) run OpenColorIO v2.5.2. Structural tensors and explicit configuration, planar storage, straight internal color and explicit interpretation follow [FMT-common](FMT_common_contract.md). OCIO has its own external-engine contract; it does not inherit native NUM strict/four-ULP guarantees or the numerical recipe of [ICC](FMT-12_icc_transform_contract.md).

## Initial members

- A converts between explicitly selected source and destination color spaces in a configuration.
- B applies an explicitly selected source space, display and view.
- C applies an explicit ordered Look sequence between specified source and destination spaces.
- Processing prescribed by the selected config/transform is part of that operation. FMT-15 may explicitly compose these adapters, but must not silently perform a second native rendering/view transform.
- D applies a config's explicitly selected NamedTransform.
- E applies a standalone FileTransform, including admitted CLF/CTF/LUT resources. It does not replace CRV's native LUT mathematical contract.
- F applies an explicit declarative Transform/GroupTransform tree supported by the fixed engine version, not arbitrary executable code. D/E/F require explicit entry/exit interpretations.

## Engine boundary

Fix the first contract to OpenColorIO v2.5.2 CPU, with identified build, optimization settings and frozen configuration resources. Do not silently substitute a system library or a newer config. GPU requires a separate future contract. Rolling documentation for a later release does not expand this scope.

## Configuration resources

Admit v1/v2 configs readable by the fixed engine, including user configs and built-in ACES configs identified by their exact versioned names. Freeze config text, explicit context variables and all resources actually referenced by the selected processing path into owned resources. Required missing resources fail. Do not use ambient OCIO environment selection, system monitor configuration or latest/default builtin aliases as runtime sources; do not upgrade config versions implicitly. No filesystem/environment mutation after freezing may change results.

## Color and alpha scope

Each call transforms one explicitly selected three-component color group, preserving dtype, shape and ordered component slots. Alpha and other groups/AOVs are copied bit-for-bit outside the processor. Reject transforms that depend on real alpha or modify alpha; prove admission during construction, not from hasChannelCrosstalk alone. Gray first expands explicitly to three components; CMYK conversion remains FMT-12. All image results retain canonical straight form.

## Dtype boundary

Support Float32 and Float64 public tensors with matching output dtype. Float64 selected colors first round to Float32 under NUM rules, run an F32-to-F32 OCIO processor, then widen exactly back to Float64. Float32 colors run directly. Finite representable narrowing is an unchecked caller premise. This explicitly loses Float64 precision; neither public dtype promises a different internal engine. Bypass alpha/AOVs never pass through narrowing and retain their original bit patterns.

## Reference-space bridge

Default reference_bridge=reject. Implicit scene-reference/display-reference crossing by a color-space conversion requires explicit config_default, then records the actual default ViewTransform chosen by the fixed engine/config. This applies to Look process-space conversions and nested color-space nodes as well as A. B explicitly selects its display/view route. No extra native FMT-15 rendering is added before or after a configured route.

## Coordinate interpretation

Identify configured coordinates by frozen config resource identity and resolved canonical space name, or an explicit complete endpoint declaration where the transform does not define one. Permit explicit analytic-space bindings as in FMT-12, without guessing definitions/units from names such as ACEScg or sRGB. Execute config-native numeric coordinates; do not automatically normalize them to [0,1]. If a Lab transform expects L* while the project uses l=L*/100, the caller explicitly converts values and interpretation at the boundary.

## Execution configurations

execution=optimized is default and fixes the v2.5.2 OPTIMIZATION_DEFAULT set; reference selects OPTIMIZATION_NONE. Default includes fast log/exp/pow and inverse LUT approximations. The two paths need not agree bit-for-bit and neither claims NUM correct rounding or four-ULP conformance. Within an identified build/CPU path and configuration, batch/ROI/thread scheduling must not change result bits. Ambient environment variables must not override the selected optimization. NONE still permits required engine finalization/config construction behavior.

## Dynamic-property policy

Resolve exposure/contrast/gamma and other dynamic grading properties to an explicit immutable compile-time snapshot, then make the processor non-dynamic. Changing values requires rebuilding. No shared mutable property may change between workers/tiles of an execution. Runtime typed control ports are a future separate extension, not an implicit capability of this family.

## Directions

All members expose static direction=forward (default) or inverse. Endpoint parameters describe the forward route; inverse consumes its exit interpretation and publishes its entry interpretation. Use the engine's selected inverse construction, failing if unavailable. Inverse Look sequences reverse order and flip individual directions. Availability is not proof of lossless recovery; display rendering, clipping and LUT inversion may remain lossy/approximate.

## Look candidates

Support ordered candidate Look sequences (OCIO's | fallback), resolved once during compilation against frozen resources, with the selected candidate recorded. Only missing-file failures try the next candidate; a missing Look name or other construction failure is fatal under v2.5.2. File changes cannot select another candidate at execution. Within a candidate, ordered entries all apply and each has an explicit forward/inverse direction. All candidates failing to resolve is a missing-resource error, not a no-op.

## Display Looks

B executes its view's configured Looks by default; explicit looks_bypass=true skips them and is part of processor identity. Additional user Looks compose through C rather than being implicitly appended by B. Always select display and view explicitly; no current-screen or menu-default selection occurs.

## Configuration bypass

Retain and record engine construction skips for identical names/equalitygroup. A/B expose data_bypass=true by default, explicitly settable to false; nested transforms retain their own declared/default setting. Semantic entry/exit spaces marked isData fail; raw may deliberately select them. A no-op processor still passes selected Float64 samples through RN32 and exact widening and is not an automatic bit-preserving view/copy. CPU reference does not undo config skips.

## Finite numerical domain

Description-based and finite-only raw paths require consumed color inputs, narrowed Float32 values and requested outputs finite as unchecked caller premises. No adapter scan checks source, narrowed or output values. Use finite negative/HDR coordinates without an adapter clamp; a configured Range/LUT/rendering transform may itself clip. The proposed raw path omits analytic interpretation checks, but still requires explicit endpoint configuration and finite samples. No path attaches an output description.

## File interpolation

E exposes supported interpolation choices including default/nearest/linear/ tetrahedral/best. Record the selected reader and effective interpretation; default/best resolve under that fixed v2.5.2 reader. Reject an explicit concrete mode unsupported by an affected LUT instead of accepting a warning/fallback to a different mode. Non-LUT stages in compound files retain their file-defined processing. File options and selection are static processor identity. Apply the same strict concrete-request policy to public LUT/FileTransform nodes explicitly authored in F. Existing interpolation declarations inside imported configs/files retain the pinned reader's interpretation unless an applicable public override was explicitly supplied; record their actual behavior. A public concrete override that a reader cannot apply is rejected, not silently ignored.

## Public selectors

Public color-space selectors explicitly distinguish name, alias and role. Resolve case-sensitive exact unique matches at compilation; missing/ambiguous matches fail. Store canonical config identity/name in prepared configuration and processor identity, retaining the supplied selector for diagnostics. A cannot select a NamedTransform as a color space; D selects that separate object class.

## Demand and layout

Every requested transformed coordinate consumes the complete source triple at that point, even for a reported no-op. Mathematical support has no halo. Bypass alpha/AOV output tiles consume only their corresponding source component; alpha=0 does not suppress requested hidden-color processing. Requested components are published without a validation pass. Actual shared engine execution failures propagate; violated sample premises have no required diagnostic.

layout=auto (default) and materialize both allocate distinct dense output tiles. Reject view, including Float32 no-op cases. Bypass read-window reuse concerns source access only and does not alias produced storage. ## Static interface and member identity

All six members are native external-engine adapters. F's internal group is one processor, not separate project tensor nodes with intermediate rounding. Each takes one `input` tensor and returns same-dtype/same-shape `values` with the same channel axis and ordered source slots. Operation keys:

| Member | Key | Required forward-route parameters |
| --- | --- | --- |
| [A](FMT-13A_convert_ocio_space.md) | color.ocio_convert_space_2_5_2_cpu | config, source_space, destination_space |
| [B](FMT-13B_apply_ocio_display_view.md) | color.ocio_display_view_2_5_2_cpu | config, source_space, display, view |
| [C](FMT-13C_apply_ocio_looks.md) | color.ocio_looks_2_5_2_cpu | config, source_space, destination_space, looks |
| [D](FMT-13D_apply_ocio_named_transform.md) | color.ocio_named_transform_2_5_2_cpu | config, named_transform, entry_space, exit_space |
| [E](FMT-13E_apply_ocio_file.md) | color.ocio_file_transform_2_5_2_cpu | frozen file, entry_space, exit_space |
| [F](FMT-13F_apply_ocio_transform_tree.md) | color.ocio_transform_tree_2_5_2_cpu | transform tree, entry_space, exit_space |

Parameters are static: selectors, config/context/resources, tree values and directions, property snapshots, interpolation, execution, bypass/bridge, layout and interpretation. Structural changes require inference and reconstruction. Only pixel values vary within a compiled workflow. Preserve NUM checked-size and generic tensor rank rules subject to actual planar image-axis capabilities. Selectors resolve axes directly or from explicit input configuration; never guess HWC/CHW.

Semantic mode selects one complete three-component group; raw selects three distinct ordered slots and explicitly supplies all numerical endpoint/config parameters. Integer/F16 storage requires prior explicit conversion to admitted Float32/Float64. Public endpoint coordinates must match the declared config-native units; no implicit FMT-06 decoder or RGB transfer stage is inserted.

Top-level color-space selectors distinguish name/alias/role. Display/view and Look names use exact case-sensitive lookup in their object namespaces. D uses an explicit NamedTransform name/alias selector. Canonicalize resolved objects before passing names to OCIO. Native references inside an imported config or file retain the pinned reader's own resolution semantics and are recorded in the construction trace; they are not silently rewritten to a different config dialect. F's explicit symbolic nodes are resolved as part of its static tree.

E/F may omit config only if their transform is self-contained. In that case use a fixed versioned empty resolver context with no color spaces, Looks, defaults, system configuration or ambient variables. A symbolic dependency requires an explicit frozen config/context. E uses interpolation=default when omitted and an explicit optional ccc_id (empty means the pinned reader's default selection); record the actual selected record and fail unavailable selections. F requires a finite, acyclic typed tree with fixed supported node types and parameters. Empty GroupTransform is permitted as an engine no-op, with normal dtype adaptation/validation. No arbitrary pointers, callbacks, code or plugin loading.

C supplies an explicit sequence or ordered alternatives; an explicitly empty sequence performs just its declared source/destination conversion. Each item contains a Look selector and direction. Serialize only unambiguous representable Look syntax; do not invent escaping for a name the engine grammar cannot address. complete-input inverse reverses each sequence and flips directions while retaining alternative priority. A missing inverse is not silently replaced with identity.

## Explicit configuration and selected route

Input and output interpretations are explicit static configuration. Config identity, resolved context and canonical space identity determine configured coordinates. Analytic/endpoint bindings are caller assertions with required structural/unit/reference consistency, not proof that arbitrary LUTs equal analytic formulas. Config reference-space enums control engine routing; they do not establish nits, a physical scene reference or a [0,1] sample range.

A/C compute coordinates for the resolved destination space. B computes the resolved display/view output space. D/E/F use explicit exit configuration; inverse swaps entry/exit. Require complete endpoint declarations whenever the route alone is insufficient. Preparation retains canonical route information and resources. Outputs contain numeric data and structure; consumers receive the needed configuration explicitly. View/Look provenance does not imply that pre-view information can be recovered from pixels.

Check logical source/process/destination reference-space crossings before CPU optimization or config identity/equalitygroup elision. config_default authorizes only the recorded engine/config default bridge. An explicit DisplayViewTransform node is an explicit view route, as B is; implicit extra bridges inside its Look or color-space dependencies still require the policy above. When the config omits default_view_transform and the pinned engine selects its first eligible ViewTransform, record that concrete object. Never choose a view from the OS.

Construction trace includes resolved names/context, actual forward/inverse definitions, candidate choice, reference bridges, bypass/elisions, file reader, effective interpolation, frozen properties and optimized processor identity. OCIO cache IDs alone are not authoritative resource/content identity.

## Alpha admission and exact processing

Inspect the selected resolved operation graph before CPU optimization. Require a conservative per-operation proof of RGB independence from alpha and identity of its alpha mapping; reject unsupported/unknown alpha effects. Do not accept an alpha-changing operation merely because a later operation cancels it, the current alpha is constant, or finite probes appear unchanged. Unselected config branches need not pass sample-processing admission. hasChannelCrosstalk is not an alpha proof: a 3x3 RGB matrix has legitimate cross-component dependencies.

Use an authorized Tensor window with only the selected three planes and no real alpha plane. The fixed engine fills absent alpha with zero. Admission makes this irrelevant to selected color; it is not an opacity assumption about the image. Real alpha/AOV bypass preserves every bit including nonfinite and signed zeros. Missing pixels and row padding are never read as colors.

RN32 conversion, engine processing and requested output conversion run directly under the finite/representability premises. No source, narrowed-value or output scan is performed. Empty and bypass-only observations still perform static resource/processor construction, including structural alpha-effect checks. Static acceptance requires supported node types, valid hierarchy and LUT/curve dimensions, supported interpolation and the alpha constraints above. Numerical coefficient and LUT-sample domains remain unchecked caller premises. Processor construction and its engine failures remain part of preparation.

## Execution recipe and reproducibility

Both configurations run F32-to-F32 after explicit dtype adaptation. Dynamic properties are snapshotted in private resolved transform instances before CPU optimization and made non-dynamic; the final processor reports isDynamic()==false. Each transform instance's own property values are frozen, so repeated same-type properties with different values keep their own values; unresolved mutable state is rejected. The source config/resource is not mutated.

Optimization is OPTIMIZATION_NONE or the exact v2.5.2 OPTIMIZATION_DEFAULT mask, with no environment override and no choice of flags, inverse quality or precision based on resource pressure. Reference does not undo config-level skip or finalization. The explicitly selected OCIO fast-math operations in optimized remain part of that processor. Arithmetic is nearest-even with gradual underflow and the caller's FP environment is restored.

The normative point result is the selected CPUProcessor applied to one F32 pixel via a planar descriptor with absent alpha, enclosed by public dtype adaptation. Any batched evaluation preserves this point result, including tails, sparse requests and worker partitions. Native NUM error bounds are not substituted for this engine contract. Within one build and configuration, ROI, batch and worker scheduling preserve output bits; the two configurations may differ and no universal four-ULP, Delta-E or reversible-transform bound is claimed.

## Frozen resources

The immutable resource manifest owns config/tree bytes, context values, logical path mapping and resolved file bytes. The entire supplied mapping is part of the identity; lookup absence is frozen too. Config-declared context defaults and explicit caller overrides are resolved without loading the process environment. Missing required variables, recursion or unresolved required references fail; empty values are never substituted. A selected candidate's missing file is eligible for the Look fallback only if no host failure caused the lookup miss. Resources irrelevant to the selected processor need not be read, but no later lookup acquires bytes from outside the snapshot. A new resource mapping is a new identity. The engine does not read the ambient environment (including OCIO_OPTIMIZATION_FLAGS and active display/view overrides), host file-system fallback paths or global caches.

## Errors

Static unsupported dtype/group/layout or incompatible semantic model uses TypeMismatch; missing/conflicting selectors, endpoints, resources, invalid tree/config/inverse/bridge or alpha-effect admission uses InvalidArgument/InvalidDomain. Forced view and unsupported concrete interpolation are static InvalidArgument/InvalidDomain. Numeric parameter domains, finite consumed values, representable narrowing and finite requested results are unchecked caller premises. A genuine private-engine execution failure is OperationFailed with a bounded message. A failed execution publishes no successful result and does not substitute a different engine/config/optimization/identity copy. Host failures never trigger config or Look alternatives.

## Acceptance

Both dtypes and configurations; exact identity Float64 narrowing; simple matrices/ranges/gamma and independent LUT fixtures; valid negative/HDR cases; aliases/roles/case errors; both directions and lossy inverse cases; same-name/equalitygroup/isData skips; default bridges and explicit views; Look candidate selection; static dynamic-property values; alpha mixing/replacement rejection and valid RGB cross-talk; bypass/empty, noncanonical slots, disjoint/offset/cross-tile ROI and missing-file no-host-fallback.

GPU execution, runtime dynamic-property ports, config authoring/baking/export, heuristic cross-config matching and automatic display selection are outside this contract. Cross-config use requires explicit intermediate interpretation and graph composition. ACES support is through explicit versioned config/builtin transforms, not a promise that a space name adopts all ACES workflows. FMT-15/18 retain their own native rendering/proofing contracts.

## References

- [OCIO transforms](https://opencolorio.readthedocs.io/en/v2.4.2/guides/authoring/transforms.html): initial API terminology; not the eventual engine version selection.
- [OCIO releases](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases): v2.5.2 is listed as the latest release when inspected on 2026-09-23.
- [Pinned public types](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/include/OpenColorIO/OpenColorTypes.h) and [public API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/include/OpenColorIO/OpenColorIO.h): bit depth, optimization, resource and processor interfaces.
- [CPUProcessor](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/CPUProcessor.cpp) and [image packing](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/ImagePacking.cpp): F32 engine and absent-alpha behavior.
- [ColorSpaceTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/ColorSpaceTransform.cpp), [DisplayViewTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/DisplayViewTransform.cpp) and [LookTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/LookTransform.cpp): configured routes, skips and candidate behavior.
- [Processor construction](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/Processor.cpp), [YAML loader](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/OCIOYaml.cpp) and [Config](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/Config.cpp): environment and resolution boundaries.
- [FileTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/FileTransform.cpp) and [PathUtils](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/PathUtils.cpp): proxy fallback and global cache integration prerequisites.
- [Transform API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/include/OpenColorIO/OpenColorTransforms.h) and [op optimization](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/OpOptimizers.cpp): static property preparation and optimization behavior.
