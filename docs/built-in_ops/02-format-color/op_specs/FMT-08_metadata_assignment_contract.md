---
spec_schema_version: 1
id: FMT-08
kind: sdk_function_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# FMT-08: edit an explicit tensor description

FMT-08 is an operations-SDK function, not a workflow operation. The pure function
`format::edit_description` in
[`photospider/ops/format/metadata.hpp`](../../../../include/photospider/ops/format/metadata.hpp)
patches or replaces a caller-supplied `TensorDescription` and returns a new
description. There is no `metadata.assign` or `metadata.remove` registry key and
no authoring helper that appends a node. Data never carries a description, so
there is nothing in a workflow to relabel. The specification status remains
Proposed; it is separate from the implementation status.

Inherit [FMT-common](FMT_common_contract.md), in particular C13 (explicit static
interpretation). Members are [A patch/replace](FMT-08A_assign_metadata.md) and
[B delete](FMT-08B_remove_metadata.md); both are options of the same function.
The description type, codec and path grammar are documented in
[Tensor descriptions](../../../kernel-architecture/Tensor-Semantic-Metadata.md).

## Purpose

A caller that needs a different interpretation for the next consuming operation
edits a description and passes the result as that operation's static
`input_description` (or as an entry of FMT-02 `input_descriptions`). Assigning
new primaries, a unit, transfer or numeric decoder in a description changes how
that consumer reads stored values; it performs no color, unit, transfer or
encoding arithmetic and touches no samples.

```cpp
namespace ps::format {
struct MetadataOptions final {
  std::string mode = "patch";        // "patch" or "replace"
  std::vector<MetadataSet> set;      // path + typed MetadataValue
  std::optional<TensorDescription> description;  // required for replace
  std::vector<std::string> remove;   // deletion paths
  std::string dependencies = "error";  // "error" or "cascade"
  std::string missing = "error";       // "error" or "ignore"
};
Result<TensorDescription> edit_description(
    const TensorDescription& source, const ValueDescriptor& descriptor,
    const MetadataOptions& options = {});
}  // namespace ps::format
```

## Confirmed choices

| Topic | Selected contract |
| --- | --- |
| Members | A patch or replace; B delete selected descriptions. Both are `edit_description` options. |
| Semantic scope | Channel roles/names/units, color groups, internal alpha relations, numeric encoding and coordinate/sampling interpretation. |
| A modes | patch by default; replace substitutes a complete description. |
| Atomicity | One edit can set and delete fields; only the final description is validated and returned. |
| Dependencies | error by default; explicit cascade removes affected dependent descriptions, never samples. |
| Completeness | Independent component descriptions are legal; declared complete color groups must satisfy all required structural fields/references. |
| Paths | Only `/semantic` paths are accepted. |
| Missing deletion | error by default, ignore explicitly; empty edits/removal lists are legal. |
| Side effects | None. No graph node, payload read, or resource lookup. |

## Parameters

| Option | Definition / default |
| --- | --- |
| `mode` | `patch` by default, or `replace`. |
| `set` | Static field/subtree assignments; empty means none. Forbidden in replace. |
| `description` | Required complete description for replace, possibly empty; forbidden for patch. |
| `remove` | Deletion paths; empty allowed. Forbidden in replace. |
| `dependencies` | `error` by default; `cascade` explicitly. |
| `missing` | `error` by default; `ignore` applies only to absent deletion targets. |

`descriptor` is the `ValueDescriptor` of the tensor the description will be used
with. Both the source and the result are validated against it with
`validate_tensor_description`, so a description that does not fit the tensor's
rank or channel extent is rejected before it reaches an operation.

Paths identify description fields/subtrees, channel entries, or explicitly
identified groups. Channel selectors may use static indices or exact unique
name/role selectors with distinct namespaces as in FMT-01B. Names are
case-sensitive; no aliases, regex, wildcards or implicit "first match". Resolve
all selectors against the original source before any edits. Duplicate or
ancestor/descendant-overlapping target edits fail rather than depending on list
order. Setting a subtree replaces that subtree; it is not a recursive merge of
unmentioned members. To replace an entire group, set its complete group record.

A full replace discards every field of the source; it cannot borrow missing
required fields from the source. Patch may set fields and delete disjoint fields
atomically. `missing=ignore` neither resolves ambiguous selectors nor suppresses
malformed paths, unknown field names or invalid target data. A missing parent does
not permit synthesizing an incomplete group; set a complete new subtree or use a
coherent full description.

## Atomic reference semantics and cascade

1. Validate the source against `descriptor` and resolve the static edits.
2. Resolve unique targets and reject conflicting edits. Check absent deletions
   against `missing`. Construct a private candidate by applying all set/remove
   edits to a copy for patch, or by taking the complete replacement for replace.
   An invalid intermediate state is never returned.
3. Validate the final candidate with its reference/required-field dependencies.
   With `error`, any incoherent retained description fails. With `cascade`,
   remove only descriptions invalidated by the requested changes until the
   dependency closure is stable; then validate the final result against
   `descriptor`.
4. Return the candidate. Group order in patch mode follows the source order.

Cascade uses actual description dependencies, not names that happen to match or
a heuristic about three/four channels. Deleting an optional alpha relation may
leave a straight color group valid. Removing a required color-space/profile
description may require removing the dependent complete group while retaining
independent component names/units. Remove the smallest dependent unit whose
remaining parent can be valid; otherwise remove that parent description. Do not
relabel RGB as Gray, select another alpha/profile, or fabricate fields. Only the
affected dependency closure can be removed.

Explicit newly assigned target content is protected: if cascade would remove it,
its enclosing explicitly assigned subtree, or a required part of an explicit
replacement, the edit fails. Cascade cannot turn a malformed requested target
into a successful empty description. Unrelated pre-existing inconsistencies must
be explicitly repaired/removed, not opportunistically cleaned.

The complete edit succeeds or fails as one transaction. The source description
is unchanged. Empty patch/removal returns an equal description; replace with an
empty description returns an empty description.

## Structural facts, completeness and semantic validity

A description cannot change dtype, shape, byte layout/strides, address map,
tile geometry, owner, valid coverage, runtime provenance or cache state; those
belong to the Result schema and storage. No description value manufactures
missing samples or a validity certificate.

Independent component names/roles/units need not constitute a full image. A
declared complete group must contain all required fields and valid references
for its model, including profile/white/transfer/primaries where applicable.
Required numeric encoding must be compatible with dtype; removing an integer code
decoder cannot leave a false native-color claim.

Complete canonical images use straight colors and internal alpha. A description
of a premultiplied boundary payload is legal, without claiming a canonical
complete image or performing FMT-04 arithmetic. Removing an alpha description
never removes its data plane or changes shape; use FMT-05C for logical channel
removal.

Sample values are never read. Describing samples containing 1.6 as coverage
succeeds if the description is structurally valid; a later coverage consumer
that receives this description, or a port-bound validator, can reject that
sample.

## Resources and identity

A description may name a profile `ResourceIdentity` or a configured-space
config identity. `edit_description` does not look up or own resources. The
consuming operation's static preparation selects the named identity from the
`ResourceSet` passed to the compiler; a missing resource fails there. A filename
or pointer-shaped string does not acquire a resource or cause file/network I/O.

The edited description enters a workflow only as a static parameter value of the
next operation, so it is part of that operation's invocation identity and cache
key. Two consumers given different descriptions of the same source have
distinct identities.

## Algorithms, resources and errors

Let M be description entries, E their declared dependency edges and P edit
entries. Reference editing costs O((M+P) log(M+P)+E). Monotone cascade removes
nodes/edges once using a worklist, not repeated full scans. The internal edit
transaction is bounded to 4096 encoded bytes, 1024 nodes and depth 12; paths are
at most 1024 bytes. Allocation uses the ordinary heap; `std::bad_alloc` may be
thrown.

| Failure | Status |
| --- | --- |
| Malformed/unknown field, invalid/ambiguous path, non-`/semantic` path, overlapping edits, missing deletion under error, invalid mode, set/remove in replace, description in patch, incomplete/inconsistent target or failed protected cascade | `InvalidArgument` |
| Source or result incompatible with `descriptor` (rank, channel axis, table length, encoding/dtype) | The status returned by `validate_tensor_description` |

Identify the path, group or field in the diagnostic; there is no pixel
coordinate.

## Acceptance

Members give independent edit examples. Test patch/replacement/deletion,
missing/ignore, ambiguous names, protected cascade, incomplete groups, source
immutability and descriptor conflicts. Then pass an edited description to a
consuming operation (for example FMT-01B named extraction or FMT-09 semantic
transfer) and check that the consumer reads it and that its invocation identity
differs from a call with the unedited description. These are acceptance
requirements, not a record of completed runs.
