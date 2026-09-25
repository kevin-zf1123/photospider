---
spec_schema_version: 1
id: FMT-08
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-08: assign and remove semantic interpretation

Runtime update (package 0.22.0): FMT-08A/B are implemented with canonical
TensorDescription v3, typed encoding/sampling/ICC/OCIO resource descriptions,
opaque annotations, atomic edits and exact planar/generic-numeric regional
execution. See the [runtime schema and interfaces](../../../kernel-architecture/Tensor-Semantic-Metadata.md),
[minimal public workflow](../../../../examples/metadata_workflow/README.md)
and [measured performance](../../../../examples/metadata_performance/README.md).
Proposed remains the specification decision status; external FMT-12/13 transform
engines are not claimed by the metadata/resource implementation.


Implementation update: package 0.20.0 [removes the legacy format/color code](FMT_legacy_retirement.md).
Descriptions of old registrations below record the inspected baseline only;
those keys and pixel callbacks are no longer available. The target decision remains Proposed; its CPU implementation is recorded above.

Inherit [FMT-common](FMT_common_contract.md), its NUM execution/resource baseline,
canonical straight/internal-alpha rules and the
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
Members are [A assign/edit](FMT-08A_assign_metadata.md), a proposed native node,
and [B remove](FMT-08B_remove_metadata.md), a compile-time deletion-only helper.
The specification decision and the runtime implementation record remain distinct.

## Purpose and inspected baseline

FMT-08 publishes a new immutable interpretation of unchanged numeric samples.
A downstream consumer receives that result metadata; call-local override changes
only one invocation's source interpretation. Neither changes another consumer's
input. Assigning new primaries, a unit, transfer or numeric decoder reinterprets
stored values; it does not perform color, unit, transfer or encoding arithmetic.

At the inspection commit, the legacy color.assign registration
used Float32 rank-three input, Image output semantics and required dense output.
Its callback copied
sample bytes for the Parameter semantic rule. The inspected legacy contract
also required typed target-domain validation. Those retired typed-image rules
do not establish this generic immutable metadata/planar contract or its exact
regional behavior. No old key is silently upgraded or aliased by this document.

## Confirmed choices

| Topic | Selected contract |
| --- | --- |
| Members | A assign/reinterpret; B remove selected descriptions. |
| Semantic scope | Channel roles/names/units, color groups, internal alpha relations, numeric encoding and coordinate/sampling interpretation. |
| A modes | patch by default; replace replaces registered semantic descriptions completely. |
| Atomicity | One edit can set and delete fields; only the final target is validated/published. |
| Dependencies | error by default; explicit cascade removes affected dependent descriptions, never samples. |
| Completeness | Independent component descriptions are legal; declared complete color groups must satisfy all required structural fields/references. |
| Opaque metadata | Preserve unmentioned unknown annotations; explicit naming is required to edit/delete them. They are not validated known semantics. |
| Staticness | Edit paths, descriptors and resource declarations are compile-time fixed. |
| Missing deletion | error by default, ignore explicitly; empty edits/removal lists are legal. |
| Layout | auto default; legal read-only view where possible, otherwise materialize; forced view fails if impossible. |
| Implementation | A native; B transactionally expands into A with no set entries. |

## Ports, scope and parameters

A takes one `input` tensor and returns one `values` tensor. B takes a graph and
input edge and returns a single values edge. Dtype, rank, logical extents and
index order are unchanged. The target dtypes are UInt8/UInt16/Int8/Int16/Int64/
Float32/Float64, with missing native widths remaining implementation dependencies.
Rank 1..8, positive extents and checked logical-size limits inherit NUM/FMT.
An annotation does not make an unsupported runtime dtype available.

Parameters below are logical typed metadata/path records, not an invented
serialization of the current v1 facet codec. Their canonical codec, schema
registry and immutable result metadata representation are implementation
prerequisites. The named CPU strict/Apple Silicon/x86-64 keys have identical
sample bits and descriptor behavior; no numerical approximation applies.

| Parameter | Definition / authoring default |
| --- | --- |
| A mode | patch by default, or replace. |
| A set | Static explicit field/subtree assignments; absent/empty means none. In replace, only named opaque annotation assignments are permitted here. |
| A description | Required full registered semantic description for replace, possibly empty; forbidden for patch. Replace forbids additional semantic set entries. |
| A remove / B targets | Static explicit deletion paths; empty allowed. In replace, deletion paths may name opaque annotations retained outside the semantic replacement. |
| dependencies | error by default; cascade explicitly. |
| missing | error by default; ignore applies only to absent deletion targets. |
| layout | auto by default; view/materialize explicit. |
| B profile | strict by default or named CPU profile; selects A's corresponding key. |

No additional raw/override parameter is needed to authorize an explicit edit.
The edit itself authorizes reinterpretation of the named fields; unspecified
source semantics remain subject to their output applicability. This does not
permit editing kernel facts or producing a contradictory target. Other operators'
call-local raw/override behavior remains separate.

Paths identify registered semantic fields/subtrees, channel entries, explicitly
identified groups, or opaque annotation keys. Channel selectors may use static
indices or exact unique name/role selectors with distinct namespaces as in
FMT-01B. Names are case-sensitive; no aliases, regex, wildcards or implicit
"first match". Resolve all selectors against the original input before any edits.
New descriptions use explicit destination identities/indices. Duplicate or
ancestor/descendant-overlapping target edits fail rather than depending on list
order. Setting a subtree replaces that subtree; it is not a recursive merge of
unmentioned members. To replace an entire group, set its complete group record.

A full replace discards unspecified registered semantic fields, retaining
unmentioned opaque annotations. It cannot borrow missing required semantic fields
from the source. Explicit annotation assignments/deletions remain available in this mode.
Patch may set semantic fields or named annotations and delete disjoint fields
atomically. Missing=ignore neither resolves ambiguous selectors nor suppresses
malformed paths, unknown registered-semantic field names or invalid target data.
A missing parent does not permit synthesizing an incomplete group; set a complete
new subtree or use a coherent full description.

## Atomic reference semantics and cascade

1. Read/resolve the admitted source description, structural descriptor and static
   edits, including any immutable resource declarations. No image pixels are read.
2. Resolve unique targets and reject conflicting edits. Check absent deletions
   against missing. Construct a private candidate by applying all set/remove
   edits to a copy for patch, or taking the complete replacement semantics plus
   retained and explicitly edited annotations for replace. An invalid intermediate state is not published.
3. Validate the final candidate with schema-declared reference/required-field
   dependencies. With error, any incoherent retained description fails. With
   cascade, remove only descriptions invalidated by the requested changes until
   the dependency closure is stable; then validate the final result.
4. Admit result metadata/resources and the selected data layout. At execution,
   provide the requested unchanged samples and publish the independent immutable
   target description with exact successful coverage.

Cascade uses actual schema dependencies, not names that happen to match or a
heuristic about three/four channels. Deleting an optional alpha relation may leave
a straight color group valid. Removing a required color-space/profile description
may require removing the dependent complete group while retaining independent
component names/units. Remove the smallest schema-defined dependent unit whose
remaining parent can be valid; otherwise remove that parent description. Do not
relabel RGB as Gray, select another alpha/profile, fabricate fields or repair
sample values. Only the affected dependency closure can be removed.

Explicit newly assigned target content is protected: if cascade would remove it,
its enclosing explicitly assigned subtree, or a required part of an explicit
replacement, fail instead. Cascade cannot turn a malformed requested target into
a successful empty description. Unrelated pre-existing inconsistencies must be
explicitly repaired/removed, not opportunistically cleaned. For a source that is
otherwise admissible, discarded old fields need not satisfy target semantics;
no metadata edit bypasses a failed source producer or structural input admission.

The complete edit succeeds or fails as one description transaction. Source
metadata, shared consumers and previously published results remain unchanged.
Empty patch/removal is identity for semantics, subject to normal structural
checks and layout; replace with an empty semantic description explicitly clears
registered semantics, retaining unmentioned annotations.

## Structural facts, completeness and semantic validity

Kernel dtype, shape, byte layout/strides, address map, tile geometry, owner,
valid coverage, runtime provenance, validation proofs and cache state are not
user-editable semantic fields. Unknown annotations cannot override them. No
metadata value manufactures missing samples or a validity certificate.
Materialize can allocate a new valid backing under normal copy rules, but it
cannot legalize a contradictory requested structural declaration by relabeling.

A declared image's axis interpretation must be compatible with its actual
planar image layout. Assigning an image label to an incompatible interleaved
buffer is not an import conversion, even with materialize; use the explicit
storage boundary first. A numeric tensor's semantic axes may be described or
reinterpreted only where consistent with its admitted structure. Changing world
origin/units changes coordinate interpretation, not tensor index positions or
which input element corresponds to an output request.

Independent component names/roles/units need not constitute a full image.
A declared complete group must contain all required fields and valid references
for its model, including profile/white/transfer/primaries where applicable.
No downstream guessing or temporarily incomplete complete-group declaration is
allowed. Required numeric encoding must be compatible with dtype; removing an
integer code decoder cannot leave a false native-color claim.

Complete canonical images use straight colors and internal alpha. Explicit
premultiplied numeric boundary descriptions are allowed as reinterpretations,
without claiming a canonical complete image or performing FMT-04 arithmetic.
No persistent external alpha binding is introduced. Removing alpha descriptions
never removes its data plane or changes shape; use FMT-05C for logical channel
removal. Surviving independent component meaning remains when applicable.

Sample values are never scanned for color finiteness, alpha range, premultiplied
zero invariants or profile agreement. Assigning coverage to samples containing
1.6 succeeds if the description is structurally valid; a later coverage consumer
validates and can reject that requested sample. NaN payloads, signaling NaN,
Inf, signed zeros and integer extrema are copied/shared exactly. No quieting,
clipping, division, transfer, range or unit arithmetic occurs.

## Opaque annotations, resources and proof identity

Unknown extension/application annotations are retained as immutable opaque data,
not as known semantic claims. Full semantic replace does not erase them by
omission. Explicit annotation assignments/deletions are allowed, but no hidden
pointer, ownership escape or active validation proof is created by an opaque
value. Known schema fields and unverified annotation namespaces remain distinct.

Profiles/configuration and other resource-bearing descriptions require explicit
admitted immutable resources, with content/identity and required interpretation
bound consistently. A filename or pointer-shaped string does not acquire a
resource or cause an implicit file/network read. Validate required resource
schema/profile compatibility as descriptor work, without transforming pixels.
Input-shared resources retain their ordinary owners; new resources retain their
own charged owners. Resource content or required semantic structure changes
require re-inference/recompilation/binding validation, not a mutable global edit.

Changed effective semantic fields invalidate any dependent sample-validity
proofs. Unchanged descriptions do not gain a new proof. Normal host proof reuse
is allowed only when sample identity/correspondence, effective interpretation and
observed domain still satisfy the common contract. This operation exposes no
"validated=true" field and may not turn an application annotation into a
certificate. Other consumers keep their own interpretations/evidence.

## Exact requests, layouts and dirty mapping

For output Q, Data support is exactly the same input coordinates Q. A legal view
need not dereference/copy those bytes, but valid source coverage and declared DAG
support are still required; no view manufactures a successful missing observation.
Materialization reads/copies only Q. The operation introduces no pixel Validation
or Control dependency. Static metadata, referenced resources and consumed source
descriptions form Descriptor dependencies, independent of Q. A descriptor-only
inference query needs no source pixel execution.

Dirty input samples map identically to output coordinates. A description/resource
change invalidates semantic inference and downstream consumers that use it,
even when pixels are identical. There is no spatial resampling or complete-color
closure. Required upstream Whole work and intrinsic producer failures retain
their original scope. A partial request does not produce all tensor coverage.

Auto shares a legal immutable backing via an independent target-metadata result
header when possible; otherwise copies Q into new admitted storage. View requires
that representation and fails ViewUnavailable rather than copying. Materialize
always creates new owned requested coverage, including for semantic identity.
No mode mutates the source header or changes numeric values. All modes must pass
the same structural/target checks; automatic copy cannot hide a metadata error.
Image allocations remain planar with the fixed DAG tile geometry, one virtual
span and explicit required-page backing. View retention is ordinary storage
ownership, not a forbidden external alpha association.

## Algorithms, resources and errors

Let M be metadata entries, E their declared dependency edges, P edit entries,
Nq requested samples, r rank and F windows. Build bounded schema/selector lookup
and dependency tables; reference editing costs O((M+P) log(M+P)+E), apart from
bounded profile validation. Monotone cascade removes nodes/edges once using a
worklist, not repeated full scans. Keep work/storage for this metadata graph and
any parsed immutable resource in host budgets. Pixel work is O(Nq*r+F*r) for
materialization and window/coverage work for views; metadata edits allocate no
full-image pixel scratch or eager per-reserved-page directory.

Admit simultaneous source/result owners, metadata headers, dependency/path maps,
resources, windows, scratch and actual backing. Deduplicate shared resources.
A view may retain a larger original backing; logical sample bytes are not an RSS
bound. Deleted metadata does not promise immediate physical resource release
while the input or another consumer owns it. Poll cancellation/currentness at
most every 1024 metadata/copy entries, within bounded resource-validation work,
and before publication. Release unpublished candidate resources on failure.
Use host scheduling, no private pool, eviction, replay or mutable shared cache.

Initial optional sample-only result caching is disabled; a future cache must
include source and target descriptions, static edits/modes, resource identities,
layout and exact coverage. Equal pixel bytes do not make differently assigned
color/profile/alpha interpretations interchangeable. Optimizers may eliminate
copying only while preserving the full description effect and dependency/error
semantics; they cannot erase an assignment as a numerical identity.

| Failure | Phase | Status / reason |
| --- | --- | --- |
| Malformed/unknown semantic field, invalid/ambiguous path, overlapping edits, missing deletion under error, invalid mode, incomplete/inconsistent target or failed protected cascade | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Requested semantics incompatible with actual dtype/rank/channel structure or physical image layout | Static preflight | TypeMismatch / None. |
| Forced view cannot represent the result | Observation evaluation | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Missing/invalid required resource, metadata capacity/work, missing sample coverage, producer failure, backend, cancellation or stale failure | Inherited phase | Preserve NUM/kernel/resource code, origin and scope. |

Identify the member, path/group/schema or resource in descriptor errors; do not
invent a pixel coordinate for them. Merely encountering a NaN or out-of-range
alpha byte is not an FMT-08 domain failure. B stages its expansion transactionally,
using collision-aware authoring IDs, and leaves the caller graph unchanged on
failure. Host graph/arity admission limits still apply.

## Acceptance and implementation dependencies

Members give independent edit/byte examples. Test patch/replacement/deletion,
missing/ignore, aliases/ambiguous names, protected cascade, incomplete groups,
unknown annotations, source immutability, all supported dtypes, shape/axes/layout
conflicts, exact partial requests and upstream failures. Verify profile/resource
retention, context-independent produced views, final release, budget/cancel
rollback and optimizer/cache preservation of semantic effects.

Implementation requires canonical metadata/path/resource codecs, registered
schema dependencies, immutable same-backing result descriptions, generic planar
copy/view execution and the proposed native/helper interfaces. Deliver actual
public compile/execute workflows with sample-byte and descriptor assertions.
Benchmark large planar inputs with many metadata records, patch versus replacement
and cascade, view versus materialize, full and one-channel/tile-crossing requests;
report metadata/copy work, pixels read, backing and retained resource peaks plus
build/profile/ISA and timing. Runtime and benchmark evidence is linked in the update above.

Documentation checks covered local links/front matter/whitespace, independent
expected metadata snapshots and a dependency-closure example, plus exact-bit
copy fixtures including signaling NaN and Int64 maximum. These are specification
examples, not tests of a registered metadata node, schema validator, resource
lifetime implementation or physical view backend.
