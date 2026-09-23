---
spec_schema_version: 1
id: FMT-13
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13: configured OpenColorIO transforms

This document records completed clarification on 2026-09-23 directly in operator
specs. It registers no runtime operation. Engine hooks, numerical corpus and
public workflow validation remain implementation prerequisites. Inherit the unified tensor/metadata, planar
storage, straight internal color and explicit interpretation rules from
[FMT-common](FMT_common_contract.md). OCIO requires its own external-engine
contract; it does not inherit native NUM strict/four-ULP guarantees or the
specific numerical recipe chosen for [ICC](FMT-12_icc_transform_contract.md).

## Confirmed initial members

- A converts between explicitly selected source and destination color spaces
  in a configuration.
- B applies an explicitly selected source space, display and view.
- C applies an explicit ordered Look sequence between specified source and
  destination spaces.
- Processing prescribed by the selected config/transform is part of that
  operation. FMT-15 may explicitly compose these adapters, but must not silently
  perform a second native rendering/view transform.
- D applies a config's explicitly selected NamedTransform.
- E applies a standalone FileTransform, including admitted CLF/CTF/LUT resources.
  It does not replace CRV's native LUT mathematical contract.
- F applies an explicit declarative Transform/GroupTransform tree supported by
  the fixed engine version, not arbitrary executable code. D/E/F require
  explicit entry/exit interpretations.

## Current implementation boundary

The inspected CMake, public headers, kernel sources and built-in operator sources
contain no OCIO adapter implementation. Existing NUM LUT operations do not
implement config resolution, Looks or display/view processors. This family
requires engine/resource and metadata integration before runtime acceptance.

## Confirmed engine boundary

Fix the first contract to OpenColorIO v2.5.2 CPU, with identified build,
optimization settings and frozen configuration resources. Do not silently
substitute a system library or a newer config. GPU requires a separate future
contract. Rolling documentation for a later release does not expand this scope.

## Confirmed configuration resources

Admit v1/v2 configs readable by the fixed engine, including user configs and
built-in ACES configs identified by their exact versioned names. Freeze config
text, explicit context variables and all resources actually referenced by the
selected processing path into owned resources. Required missing resources fail.
Do not use ambient OCIO environment selection, system monitor configuration or
latest/default builtin aliases as runtime sources; do not upgrade config versions
implicitly. No filesystem/environment mutation after freezing may change results.

## Confirmed color and alpha scope

Each call transforms one explicitly selected three-component color group,
preserving dtype, shape and ordered component slots. Alpha and other groups/AOVs
are copied bit-for-bit outside the processor. Reject transforms that depend on
real alpha or modify alpha; prove admission during construction, not from
hasChannelCrosstalk alone. Gray first expands explicitly to three components;
CMYK conversion remains FMT-12. All image results retain canonical straight form.

## Confirmed dtype boundary

Support Float32 and Float64 public tensors with matching output dtype. Float64
selected colors first round to Float32 under NUM rules, run an F32-to-F32 OCIO
processor, then widen exactly back to Float64. Float32 colors run directly.
Finite-to-infinity narrowing fails. This explicitly loses Float64 precision;
neither public dtype promises a different internal engine. Bypass alpha/AOVs
never pass through narrowing and retain their original bit patterns.

## Confirmed reference-space bridge

Default reference_bridge=reject. Implicit scene-reference/display-reference
crossing by a color-space conversion requires explicit config_default, then
records the actual default ViewTransform chosen by the fixed engine/config.
This applies to Look process-space conversions and nested color-space nodes as
well as A. B explicitly selects its display/view route. No extra native FMT-15
rendering is added before or after a configured route.

## Confirmed coordinate interpretation

Identify configured coordinates by frozen config resource identity and resolved
canonical space name, or an explicit complete endpoint declaration where the
transform does not define one. Permit explicit analytic-space bindings as in
FMT-12, without guessing definitions/units from names such as ACEScg or sRGB.
Execute config-native numeric coordinates; do not automatically normalize them
to [0,1]. If a Lab transform expects L* while the project uses l=L*/100, the
caller explicitly converts values and interpretation at the boundary.

## Confirmed execution configurations

execution=optimized is default and fixes the v2.5.2 OPTIMIZATION_DEFAULT set;
reference selects OPTIMIZATION_NONE. Default includes fast log/exp/pow and inverse
LUT approximations. The two paths need not agree bit-for-bit and neither claims
NUM correct rounding or four-ULP conformance. Within an identified build/CPU path
and configuration, batch/ROI/thread scheduling must not change result bits.
Ambient environment variables must not override the selected optimization.
NONE still permits required engine finalization/config construction behavior.

## Confirmed dynamic-property policy

Resolve exposure/contrast/gamma and other dynamic grading properties to an
explicit immutable compile-time snapshot, then make the processor non-dynamic.
Changing values requires rebuilding. No shared mutable property may change
between workers/tiles of an execution. Runtime typed control ports are a future
separate extension, not an implicit capability of this family.

## Confirmed directions

All members expose static direction=forward (default) or inverse. Endpoint
parameters describe the forward route; inverse consumes its exit interpretation
and publishes its entry interpretation. Use the engine's selected inverse
construction, failing if unavailable. Inverse Look sequences reverse order and
flip individual directions. Availability is not proof of lossless recovery;
display rendering, clipping and LUT inversion may remain lossy/approximate.

## Confirmed Look candidates

Support ordered candidate Look sequences (OCIO's | fallback), resolved once
during compilation against frozen resources, with the selected candidate
recorded. Only missing-file failures try the next candidate; a missing Look name
or other construction failure is fatal under v2.5.2. File changes cannot select
another candidate at execution. Within a candidate, ordered entries all apply
and each has an explicit forward/inverse direction. All candidates failing to
resolve is a missing-resource error, not a no-op.

## Confirmed display Looks

B executes its view's configured Looks by default; explicit looks_bypass=true
skips them and is part of processor identity. Additional user Looks compose
through C rather than being implicitly appended by B. Always select display
and view explicitly; no current-screen or menu-default selection occurs.

## Confirmed configuration bypass

Retain and record engine construction skips for identical names/equalitygroup.
A/B expose data_bypass=true by default, explicitly settable to false; nested
transforms retain their own declared/default setting. Semantic entry/exit spaces
marked isData fail; raw may deliberately select them. A no-op processor still
passes selected Float64 samples through RN32 and exact widening and is not an
automatic bit-preserving view/copy. CPU reference does not undo config skips.

## Confirmed finite numerical domain

Respect/override/raw all require consumed color inputs, narrowed Float32 values
and requested outputs finite. Admit finite negative/HDR coordinates without an
adapter clamp; a configured Range/LUT/rendering transform may itself clip.
Raw skips semantic consistency and retains applicable unverified descriptions,
but does not feed NaN/Inf to the engine or automatically declare a target space.

## Confirmed file interpolation

E exposes supported interpolation choices including default/nearest/linear/
tetrahedral/best. Record the selected reader and effective interpretation;
default/best resolve under that fixed v2.5.2 reader. Reject an explicit concrete
mode unsupported by an affected LUT instead of accepting a warning/fallback to
a different mode. Non-LUT stages in compound files retain their file-defined
processing. File options and selection are static processor identity.
Apply the same strict concrete-request policy to public LUT/FileTransform nodes
explicitly authored in F. Existing interpolation declarations inside imported
configs/files retain the pinned reader's interpretation unless an applicable
public override was explicitly supplied; record their actual behavior. A public
concrete override that a reader cannot apply is rejected, not silently ignored.

## Confirmed public selectors

Public color-space selectors explicitly distinguish name, alias and role.
Resolve case-sensitive exact unique matches at compilation; missing/ambiguous
matches fail. Store canonical config identity/name in metadata and processor
identity, retaining the supplied selector for diagnostics. A cannot select a
NamedTransform as a color space; D selects that separate object class.

## Confirmed demand and layout

Every requested transformed coordinate consumes the complete source triple at
that point, even for a reported no-op. Spatial demand is exact, without halo or
unrequested pixels. Bypass alpha/AOV requests consume only their corresponding
source component; alpha=0 does not suppress requested hidden-color processing.
Evaluate full target triples in scratch if needed, but validate/publish only
requested components. Unrequested target nonfinite values alone do not fail an
otherwise valid requested coordinate; an actual shared engine failure propagates.

layout=auto (default) and materialize both use requested regions of one legal
full-shape planar output reservation. Reject view, including Float32 no-op cases.
Bypass read-window reuse does not create a cross-owner output image. Changed
source color samples invalidate all target color components at the same points;
bypass dirty propagation is individual. Required upstream Whole failure/support
rules remain intact, and physical page/padding access is not logical sample demand.

## Static interface and member identity

All six members are native external-engine adapters. F's internal group is one
processor, not separate project tensor nodes with intermediate rounding. Each
takes one `input` tensor and returns same-dtype/same-shape `values` with the same
channel axis and ordered source slots. Proposed keys are:

| Member | Key | Required forward-route parameters |
| --- | --- | --- |
| [A](FMT-13A_convert_ocio_space.md) | color.ocio_convert_space_2_5_2_cpu | config, source_space, destination_space |
| [B](FMT-13B_apply_ocio_display_view.md) | color.ocio_display_view_2_5_2_cpu | config, source_space, display, view |
| [C](FMT-13C_apply_ocio_looks.md) | color.ocio_looks_2_5_2_cpu | config, source_space, destination_space, looks |
| [D](FMT-13D_apply_ocio_named_transform.md) | color.ocio_named_transform_2_5_2_cpu | config, named_transform, entry_space, exit_space |
| [E](FMT-13E_apply_ocio_file.md) | color.ocio_file_transform_2_5_2_cpu | frozen file, entry_space, exit_space |
| [F](FMT-13F_apply_ocio_transform_tree.md) | color.ocio_transform_tree_2_5_2_cpu | transform tree, entry_space, exit_space |

Parameters are static: selectors, config/context/resources, tree values and
directions, property snapshots, interpolation, execution, bypass/bridge, layout
and interpretation. Structural changes require inference and reconstruction.
Only pixel values vary within a compiled workflow. Preserve NUM checked-size
and generic tensor rank rules subject to actual planar image-axis capabilities.
Selectors resolve axes explicitly or from metadata; never guess HWC/CHW.

Semantic mode selects one complete three-component group; raw selects three
distinct ordered slots and explicitly supplies all numerical endpoint/config
parameters. Integer/F16 storage requires prior explicit conversion to admitted
Float32/Float64. Public endpoint coordinates must match the declared config-native
units; no implicit FMT-06 decoder or RGB transfer stage is inserted.

Top-level color-space selectors distinguish name/alias/role. Display/view and
Look names use exact case-sensitive lookup in their object namespaces. D uses
an explicit NamedTransform name/alias selector. Canonicalize resolved objects
before passing names to OCIO. Native references inside an imported config or
file retain the pinned reader's own resolution semantics and are recorded in
the construction trace; they are not silently rewritten to a different config
dialect. F's explicit symbolic nodes are resolved as part of its static tree.

E/F may omit config only if their transform is self-contained. In that case use
a fixed versioned empty resolver context with no color spaces, Looks, defaults,
system configuration or ambient variables. A symbolic dependency requires an
explicit frozen config/context. E uses interpolation=default when omitted and
an explicit optional ccc_id (empty means the pinned reader's default selection);
record the actual selected record and fail unavailable selections. F requires
a finite, acyclic typed tree with fixed supported node types and parameters.
Empty GroupTransform is permitted as an engine no-op, with normal dtype
adaptation/validation. No arbitrary pointers, callbacks, code or plugin loading.

C supplies an explicit sequence or ordered alternatives; an explicitly empty
sequence performs just its declared source/destination conversion. Each item
contains a Look selector and direction. Serialize only unambiguous representable
Look syntax; do not invent escaping for a name the engine grammar cannot address.
Whole inverse reverses each sequence and flips directions while retaining
alternative priority. A missing inverse is not silently replaced with identity.

## Metadata and selected route

Effective metadata is authoritative in respect; explicit assertions must match.
Override replaces interpretation for this invocation only. Config identity,
resolved context and canonical space identity determine a configured coordinate
system. Explicit analytic/endpoint bindings are caller assertions with required
structural/unit/reference consistency, not proof that arbitrary LUTs equal
analytic color mathematics. Config reference-space enums control engine routing;
they do not by themselves establish nits, a physical scene reference or [0,1].

Forward A/C publish the resolved destination space; B publishes the resolved
display/view output color space and records view/Look provenance separately.
That provenance is not a claim that the pixels retain recoverable pre-view
information. D/E/F publish their declared exit interpretation. Inverse swaps
effective entry/exit. Require explicit declarations whenever a configured route
does not establish a complete endpoint. Raw retains applicable original
unverified metadata and does not claim the semantic destination.

Rebuild selected-group roles/fields as required by the target interpretation,
keeping its three slots. Preserve unrelated fields/coordinates/opaque annotations
under common propagation rules and reject incompatible overlapping group
references. Retain all required output resource owners; no external alpha link.

Check logical source/process/destination reference-space crossings before CPU
optimization or config identity/equalitygroup elision. config_default authorizes
only the recorded engine/config default bridge. An explicit DisplayViewTransform
node is an explicit view route, as B is; implicit extra bridges inside its Look
or color-space dependencies still require the policy above. When the config
omits default_view_transform and the pinned engine selects its first eligible
ViewTransform, record that concrete object. Never choose a view from the OS.

Construction trace includes resolved names/context, actual forward/inverse
definitions, candidate choice, reference bridges, bypass/elisions, file reader,
effective interpolation, frozen properties and optimized processor identity.
OCIO cache IDs alone are not authoritative resource/content identity.

## Alpha admission and exact processing

Inspect the selected resolved operation graph before CPU optimization. Require
a conservative per-operation proof of RGB independence from alpha and identity
of its alpha mapping; reject unsupported/unknown alpha effects. Do not accept
an alpha-changing operation merely because a later operation cancels it, the
current alpha is constant, or finite probes appear unchanged. Unselected config
branches need not pass sample-processing admission. hasChannelCrosstalk is not
an alpha proof: a 3x3 RGB matrix has legitimate cross-component dependencies.

Use PlanarImageDesc with only the selected three planes and no real alpha plane.
The fixed engine fills absent scratch alpha with zero. Admission makes this
irrelevant to selected color; it is not an opacity assumption about the image.
Real alpha/AOV bypass preserves every bit including nonfinite and signed zeros.
Use prepared same-grid windows, gather into bounded planar scratch where tile
address maps cannot be described by uniform strides, and never read missing
pixels or row padding as colors. Do not publish temporary engine planes.

Validate all consumed source colors before RN32 and the narrowed values before
the processor. Validate requested output coordinates after processing. Shared
engine failures remain observable even if caused while generating the full
scratch triple; an unused numerical result alone is not a failure. Empty and
bypass-only observations still perform static resource/processor admission,
including alpha-effect checks, without scanning unrequested image samples.

## Execution recipe and reproducibility

Both configurations run F32-to-F32 after explicit adaptation. Snapshot properties
in private resolved transform instances before CPU optimization, make each
non-dynamic, then require final isDynamic()==false. Avoid a flag that silently
changes reference's intended optimization set; perform staticization as a
separate identified preparation step. Do not mutate the source config/resource.
Freeze each transform instance's own property values; a getter returning only
the first property of a given type cannot supply every instance's snapshot.
Test repeated same-type properties with different values and reject unresolved
mutable state rather than silently sharing one value across them.

Pin the engine release plus host-integration revision, compiler options,
dependency versions, architecture/ISA dispatch and math runtime. Fix nearest-even
and gradual underflow; prohibit fast-math/reassociation or compiler contraction
that changes the selected build recipe, and restore the caller's FP environment.
The explicitly selected OCIO fast-math operations in optimized remain part of
that processor; compiler policy does not turn them into NUM strict operations.
Use OPTIMIZATION_NONE or the exact v2.5.2 OPTIMIZATION_DEFAULT mask, with no
environment override. Do not choose flags, inverse quality or precision based
on memory pressure. Reference does not undo config-level skip or finalization.

The normative point oracle uses the selected CPUProcessor on one F32 pixel via
a planar descriptor with absent alpha, enclosed by public dtype adaptation.
Batched/SIMD evaluation is admitted only if it preserves this point result for
the supported build/path, including tails, alignment, sparse requests and worker
partitions. Otherwise retain the conforming point path; no throughput claim is
made here. Native NUM error bounds are not substituted for this engine contract.

Independent direct-OCIO integration checks use the same frozen resources,
resolved definitions, staticization, flags and build. Separately use analytic
small transforms and independently specified LUTs to check coordinate/slot and
rounding boundaries. Record reference/optimized and cross-build corpus errors;
they do not establish a universal four-ULP, Delta-E or reversible-transform bound.
Each configuration's within-build/path scheduling invariance is exact.

## Frozen resources, memory and cancellation

The immutable resource manifest owns config/tree bytes, context values, logical
path mapping and resolved file bytes. Include the entire supplied mapping in
identity; lookup absence is frozen too. Resolve config-declared context defaults
and explicit caller overrides without loading process environment. Missing
required variables, recursion or unresolved required references fail; do not
silently substitute empty values. A selected candidate's missing file is eligible
for the specified Look fallback only if no host failure caused the lookup miss.
Resources irrelevant to the selected processor need not be read, but no later
lookup may acquire new bytes from outside the snapshot. A new resource mapping
is a new identity and requires reconstruction. This does not promise every other
route in that config is executable with the selected resource package.

Retain immutable owners through config parsing, compilation, execution and
resource-bearing output/read-window lifetime. Charge copied streams, parsed
curves/LUTs, inverse lookup structures, snapshots, compiled renderers, staging,
diagnostics, output pages and all concurrently live ancestors. No arbitrary
profile-size-to-memory estimate replaces audited actual accounting or a proven
reservation bound. Validate dimensions/counts before products and allocations.
Stages/curves/tree depth/resources must obey root capacity/work/stage limits.

Stock v2.5.2 needs audited integration changes to satisfy this contract:

- YAML/config construction may read ambient environment; CPU creation may read
  OCIO_OPTIMIZATION_FLAGS. Active display/view/inactive-space overrides and cache
  controls must also be isolated. Never temporarily mutate process environment
  or global current config as a per-DAG strategy.
- ConfigIOProxy does not itself guarantee closed lookup. The pinned implementation
  may fall back to host file hashing/ifstream for absolute paths after empty
  proxy results or exceptions. Disable such fallback. Latch budget/cancellation
  failures independently; do not reinterpret them as missing resources eligible
  for Look fallback, nor accept a processor returned after a swallowed failure.
- Global file/hash caches are path-keyed and not isolated by a private Config.
  Use content-identity-qualified logical paths and audited isolated/accounted
  cache state; processor-cache OFF alone is insufficient. The initial adapter
  disables optional caches, including these global paths through integration
  hooks. Retained compiled state remains explicitly owned/charged. Do not call
  global ClearAllCaches while another graph relies on live state.
- Public APIs do not account all std::vector/new/stream allocations or expose
  bounded probes throughout parse/CLUT/inverse construction. Add reviewed hooks
  or proven equivalent accounting and bounded cooperative work/cancel probes.
  Installing a proxy or checking around a long call alone is insufficient.

Freeze successful numeric behavior while adding host hooks and identify that
integration revision. Use private construction state, then immutable non-dynamic
processor instances; concurrent sharing requires actual thread-safety validation,
otherwise retain private worker instances under budgets. Cancellation/currentness
checks occur before/after bounded pixel batches and inside bounded construction
loops. No detached work or asynchronous thread killing. Discard unpublished
writes/state on failure, retain valid published owners and release all temporary
capacity. Same-identity resources may be shared only under normal root accounting.

## Errors and acceptance

Inherit NUM Status delivery, transaction/publication scope and required-upstream
failure propagation. Static unsupported dtype/group/layout or incompatible
semantic model uses TypeMismatch; missing/conflicting selectors, endpoints,
resources, invalid tree/config/inverse/bridge or alpha-effect admission uses
InvalidArgument/InvalidDomain. Forced view and unsupported concrete interpolation
are static InvalidArgument/InvalidDomain. Runtime consumed NaN/Inf uses
OperationFailed/InvalidDomain; finite narrowing overflow and requested nonfinite
results use OperationFailed/ArithmeticOverflow. ResourceExhausted preserves the
root limit reason; Cancelled/stale retains inherited precedence and attribution.
Integration invariant failures are not misreported as invalid user configs.

Diagnostics identify member/coordinate/component, immutable resource, selected
route and engine error without dumping resource bytes. Do not publish invalid
requested pixels or substitute a different engine/config/optimization/identity
copy. Actual host failures cannot trigger config or Look alternatives.

Future acceptance must cover both dtypes and configurations; exact identity
Float64 narrowing; simple matrices/ranges/gamma and independent LUT fixtures;
negative/HDR/domain failures; aliases/roles/case errors; both directions and
lossy inverse cases; same-name/equalitygroup/isData skips; default bridges and
explicit views; Look candidate selection; static dynamic-property values;
alpha mixing/replacement rejection and valid RGB cross-talk; bypass/empty,
noncanonical slots, disjoint/offset/cross-tile ROI, lane tails and worker schedules.
Check low budgets/cancel in parse, LUT inversion and execution; cache-off,
same-path/different-resource isolation, missing-file no-host-fallback and latched
failure cleanup. Verify output/read windows survive context destruction and
all final allocations retire. Supply actual public workflows plus direct-engine
and independent numeric evidence only after implementation. Benchmark construction
separately from application, reporting exact build/CPU path and managed memory.

This revision does not implement GPU execution, runtime dynamic-property ports,
config authoring/baking/export, heuristic cross-config matching or automatic
display selection. Cross-config use requires explicit intermediate interpretation
and graph composition; no new implicit inter-config processor is introduced.
ACES support is through explicit versioned config/builtin transforms, not a
promise that a space name adopts all ACES workflows. FMT-15/18 retain their own
native rendering/proofing contracts. No legacy aliases are restored.

## Primary sources inspected

- [OCIO transforms](https://opencolorio.readthedocs.io/en/v2.4.2/guides/authoring/transforms.html):
  initial API terminology; not the eventual engine version selection.
- [OCIO releases](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases):
  v2.5.2 is listed as the latest release when inspected on 2026-09-23.
- [Pinned public types](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/include/OpenColorIO/OpenColorTypes.h)
  and [public API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/include/OpenColorIO/OpenColorIO.h):
  bit depth, optimization, resource and processor interfaces.
- [CPUProcessor](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/CPUProcessor.cpp)
  and [image packing](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/ImagePacking.cpp):
  F32 engine and absent-alpha behavior.
- [ColorSpaceTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/ColorSpaceTransform.cpp),
  [DisplayViewTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/DisplayViewTransform.cpp)
  and [LookTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/LookTransform.cpp):
  configured routes, skips and candidate behavior.
- [Processor construction](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/Processor.cpp),
  [YAML loader](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/OCIOYaml.cpp)
  and [Config](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/Config.cpp):
  environment and resolution boundaries.
- [FileTransform](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/transforms/FileTransform.cpp)
  and [PathUtils](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/PathUtils.cpp):
  proxy fallback and global cache integration prerequisites.
- [Transform API](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/include/OpenColorIO/OpenColorTransforms.h)
  and [op optimization](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/v2.5.2/src/OpenColorIO/OpOptimizers.cpp):
  static property preparation and optimization behavior.
