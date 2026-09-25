# Tensor semantic metadata, FMT-08 and FMT-11

[Chinese reader version](zh/Tensor-Semantic-Metadata.zh.md).

Package 0.22.0 implements FMT-08A as `metadata.assign_strict`,
`metadata.assign_accelerated_apple_silicon` and
`metadata.assign_accelerated_x86_64`. FMT-08B is the transactional public
`format::remove_metadata` helper; it emits one A node without set entries.
The named CPU profiles retain their normal backend admission rules and produce
identical sample bits. There is no native removal key or sample-only result cache.

## Version and interpretation

Package 0.25.0 accepts `photospider.tensor-description` runtime versions **4**
(`TDM4`) and **5** (`TDM5`). The v4 wire meaning is unchanged. Encoding chooses v5
when any tensor, component, channel or group interpretation contains a
`coordinates` record, including a present-but-empty record; otherwise it chooses
v4. Versions 1 through 3, mismatched version/discriminator pairs, noncanonical
encodings and trailing bytes are rejected. No old bytes receive new units.
WorkflowDocument and operation/provider C ABI versions do not change. Canonical
facet bytes and static edit parameters enter compiler identity. Legacy ColorArray
v1 remains a separate old-coordinate consumer contract and cannot coexist with
these tensor facets on one Value. FMT-08 rejects legacy typed facets.

The public C++ layout changes introduced by FMT-11 require rebuilding consumers
against package 0.25. `find_package(Photospider 0.24)` is rejected, rather than
claiming compatibility with an earlier layout. See the package migration note
in [Compiler-Version-Contract](../development/Compiler-Version-Contract.md).

`relative-v1` identifies the new native convention, including CIELAB/CIELCh
lightness `l=L*/100`, unchanged a/b/chroma and existing XYZ Y=1 reference scale.
It is not a range clamp. `icc-native` and `ocio-native` identify resource-defined
coordinates. Absolute units remain explicit component/axis strings; assigning
those strings never rescales samples.

The public `TensorDescription` codec is bounded to 4096 bytes. Strings are strict
UTF-8, at most 128 bytes; complete groups are bounded to 128, each with at most
64 components. The canonical binary codec uses little-endian integer and IEEE
binary64 bit fields, presence bytes 0/1, ordered channel/axis tables and explicit
resource identities. V4 additionally encodes reduced signed rational endpoints
as bounded little-endian base-2^32 numerator and positive denominator limbs.
The new endpoint variant changes the installed C++ ABI; it carries exact
composed decoders when an integer or binary64 endpoint cannot represent them.
Decoding re-encodes to reject noncanonical/trailing bytes.
Opaque annotations are separate `ValueFacet`s, not semantic fields. They retain
normal host facet limits (64 facets, 64 KiB each, 1 MiB total).

## Registered schema

| Record | Fields and meaning |
| --- | --- |
| Tensor | `channel_axis`, `channels`, `component`, `axes`, `groups`; global encoding/sampling and interpretation defaults |
| Component | `name`, `role`, `unit`, optional `interpretation`, `encoding`, `sampling` |
| Axis | `name`, `unit`, finite `origin`, positive finite `step`; describes world coordinates without changing indices |
| Group | unique `name`, ordered distinct `indices`, corresponding `components`, complete `interpretation`, optional same-tensor `alpha` index outside color indices |
| Interpretation | `model`, `primaries`, `transfer`, `reference`, `association`, `white`, `primaries_xy`, `profile`, `convention`, `configured`, `analytic_binding`, `coordinates` |
| Encoding | exact typed endpoint pairs `stored` and `decoded`; stored endpoints increase, decoded endpoints differ and may descend |
| Sampling | explicit `grid`, `scale=(1,1)`, `offset=(0,0)` for same-size co-sited internal planes; external subsampling is rejected |
| Configured space | frozen `config` identity, exact declared canonical `space`, `reference_space=scene|display` |
| Analytic binding | caller-asserted model/primaries/transfer/reference/white, ordered roles and units, `convention=relative-v1` |

A `TensorEndpoint` is Int64, finite Float64, or an exact bounded rational. Its tag and exact bits survive
serialization; Int64 maximum never passes through Float64. Encoding means
`D(x)=decoded[0]+(x-stored[0])*(decoded[1]-decoded[0])/(stored[1]-stored[0])`.
Stored intervals must fit the tensor dtype. A complete integer color group needs
an explicit decoder, supplied by its component, channel, or tensor default.
Removing a necessary decoder cannot leave a complete native-color claim.
Conflicting explicit component/group encodings or sampling grids fail.

Complete groups validate model roles, indices, alpha, required color-space
information, sampling compatibility and analytic-binding order/units. Independent
component descriptions can remain incomplete provenance. Profiles/configured
spaces do not invent analytic primaries, transfer or white; explicit bindings
are caller assertions, not mathematical equivalence proofs. No field certifies
sample finiteness, coverage range, premultiplied zeros or color validity.

## FMT-11 model-coordinate assertions

`TensorModelCoordinates` carries optional interpretation information:

| Field | Admitted metadata value |
| --- | --- |
| `scale` | empty, `relative`, or `absolute` |
| `observer` | empty or an explicitly supplied UTF-8 identifier (at most 128 bytes) |
| `gray_kind` | empty, `linear_y`, `encoded_luma`, `cielab_l`, or `oklab_l` |
| `ncl_coefficients` | absent, or two finite binary64 coefficients `[Kr, Kb]` |

Empty strings and absent coefficients make **no assertion**. Compatible channel,
component and overlapping group assertions are combined **field by field**. For
example, `{scale: relative}` and `{gray_kind: linear_y}` are complementary, not
conflicting. Two nonempty unequal values for the same field are rejected. A
later declaration that omits a field must not erase a preceding assertion and
hide a conflict with another group. Exact `operator==` is unchanged: it compares
whole records, and is not the compatibility predicate. The codec validates
compatibility without filling or rewriting provenance fields in the supplied
records; channel assembly resolves overlays in its resulting description.

TDM5 retains the same 4096-byte facet bound, strict UTF-8, finite coefficient and
canonical presence-byte checks. The codec admitting finite coefficients does
not imply that a particular conversion formula accepts them: operator semantic
admission imposes its own required domain and coordinate conditions.

FMT-08 can set/remove the complete `coordinates` subtree or its individual
`scale`, `observer`, `gray_kind`, and `ncl_coefficients` leaves under:
`/semantic/coordinates`,
`/semantic/component/interpretation/coordinates`,
`/semantic/channels/index:0/interpretation/coordinates`, and
`/semantic/groups/gray/interpretation/coordinates` (with the usual selectors).
Whole-subtree replacement remains replacement; leaf patches preserve siblings.
Deleting the last coordinate record produces canonical v4 again, provided no
other coordinate records remain. A present empty record remains v5.

## Frozen resources

`ResourceBindings` seals and deduplicates ICC profiles and `OcioConfigResource`
handles. All referenced identities must resolve at compilation/binding/output
admission; result headers retain only resources named by their facets.
Views can retain older backing owners independently. Header resource removal
therefore does not promise immediate release of every ancestor allocation.

ICC admission checks explicit v2/v4 bytes, header model/class/PCS, required tags,
TRC/LUT envelopes and profile ID; it supports RGB/Gray matrix/TRC or admitted LUT
profiles plus the existing CMYK output path and profile-defined XYZ/Lab LUT
spaces. Abstract and DeviceLink profiles are not endpoint resources. Header
models must match declared models, including the legacy CMYK contract. This
structural admission runs no CMM and does not establish FMT-12 engine compatibility.

An OCIO snapshot contains explicit config bytes, the entire sorted logical file
map, already-resolved context, caller-declared canonical space/reference pairs,
and pinned engine/build/settings identity. SHA-256 plus byte length covers all
these framed bytes. Lookup absence is frozen; no file, network or process
environment is consulted. Snapshot admission validates the resource schema and
ownership, not OCIO YAML or transform executability. A future FMT-13 processor
must validate its space declarations and selected transitive dependencies with
the pinned engine, using only this closed snapshot. Thus this implementation
completes the resource-bearing description schema without claiming an OCIO transform.

Copied snapshot streams, owning metadata and cross-root references use
`ResourceBudget`; copy/hash loops poll cancellation and work at most every
1024 bytes. Lookup scans at most 1024 declared entries. Failed admission releases
unpublished owners. Actual retained storage identities are deduplicated in
`ValueFragments::retained_bytes`, including config manifests.

## Atomic authoring and path codec

`format::assign_metadata(document, input, MetadataOptions)` appends one node.
`mode=patch`, `dependencies=error`, `missing=error`, `layout=auto` and
`profile=strict` are defaults. Replace requires an explicit complete `description`
(possibly empty), keeps opaque annotations, and permits only annotation set/remove
entries. Patch forbids `description`. Unknown options, paths, value types,
overlapping edits and invalid static records fail without appending a helper node.
Source-relative validation occurs during compilation.

Paths are slash-separated with `~0` for `~` and `~1` for `/`:

- `/semantic` names the complete semantic record.
- `/semantic/channels/index:0/unit`, `name:R`, or `role:red` selects an original
  channel; names and roles require an exact unique match. Missing deletion may
  be ignored, while ambiguous selectors always fail.
- `/semantic/groups/color/interpretation/primaries` names one group leaf.
- `/semantic/axes/0/origin` names an axis leaf.
- `/annotations/app.note` names an opaque facet; the `photospider.` prefix is
  reserved and cannot carry annotation-based semantic claims.

Selectors resolve against original input before any changes. Whole-subtree sets
replace all its fields; leaf sets retain applicable siblings. Duplicate and
ancestor/descendant overlaps, including selector aliases, fail. Deleting a channel
or axis description leaves its data slot and resets that description entry.

The node's `edits` String is lowercase hexadecimal of the bounded v1 transaction
record: ordered `version`, `set`, `remove`, optional `description` fields. Tree
records use `kind + decimal-length-or-count + ':' + contents`; `o` is an ordered
map with string keys, `s` string, `u` unsigned integer, `i` exact signed integer,
`d` eight little-endian Float64 bits, and `b` opaque bytes. The decoded bound is
4096 bytes, 1024 nodes and depth 12; the String bound is 8192. This transaction
codec is separate from the published TDM4/TDM5 codec. Prefer the typed public helper.

Cascade follows the closed schema dependencies: channel axis to channel tables
and groups; encoding/sampling/configured/profile fields to their containing
interpretation or component unit; complete groups to required fields, referenced
component assertions and overlapping explicit assignments. It visits affected
units, preserves independent names/units/planes, and rejects any cleanup that
would erase newly assigned content. Final validation publishes one immutable
candidate; it never modifies source headers or repairs sample values.

Static preparation is bounded by the facet/transaction limits and belongs to
compiler/preparation storage, as for the existing `OperationPreparer` API; that
API has no runtime cancellation token. Runtime publication admits actual facet
capacity, windows, owners and requested output backing. No pixel-sized metadata
scratch, hidden alpha binding or private worker pool is introduced.

## Exact execution and validation

Every output coordinate depends on exactly the same input coordinate. Static
metadata/resource checks do not add pixel Validation or Control support. Data
dirty mapping is identity. Required upstream failures retain their scope.

Auto/view publish independent metadata over existing immutable backing with exact
successful coverage; materialize always allocates requested output bits. Generic
execution supports legal positive, negative and zero strides; continuous/tiled
planar execution retains one physical owner and the DAG tile geometry. A direct
planar invocation has a caller-owned writer, so forced view reports
`ViewUnavailable`; public compile/execute can publish a retained image view.
Contiguous copy runs never cross requested/fragment/physical-tile boundaries and
poll cancellation/currentness at most every 1024 samples.

The [minimal public workflow](../../examples/metadata_workflow/README.md) and
`test_metadata_assignment` check exact bytes, immutable metadata, seven dtypes,
rank 1..8, layouts, sparse tile-crossing requests, missing coverage, typed paths,
cascade, signed strides, v3 endpoint/resource semantics and error rollback.
`photospider_metadata_consumer` builds the same fixture against an isolated
installed package. [Performance workloads](../../examples/metadata_performance/README.md)
separate static preparation, public execution, internal operation timing and
retained/new backing; sample-only cache is disabled.
