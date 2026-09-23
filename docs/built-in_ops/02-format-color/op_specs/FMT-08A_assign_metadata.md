---
spec_schema_version: 1
id: FMT-08A
parent_id: FMT-08
function: assign_metadata
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - metadata.assign_strict
  - metadata.assign_accelerated_apple_silicon
  - metadata.assign_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-08A: atomically assign or reinterpret metadata

Inherit the complete [FMT-08 contract](FMT-08_metadata_assignment_contract.md).
These are proposed native registry keys, not aliases for legacy color.assign.
A publishes an independent immutable output description over unchanged logical
samples. It is not a validation certificate or a numeric conversion.

## Interface and inference

Input `input` yields one `values` tensor with the identical logical dtype, shape,
axes/index order and sample bits. The family defines target dtype/rank support,
static typed edits/resources, schema paths and authoring defaults. Parameters
are mode, set, conditional description, remove, dependencies, missing and layout.
Mode patch defaults to editing named semantic fields or opaque annotations, with
atomic explicit deletions. Mode replace takes a complete registered semantic
description, retains unspecified opaque annotations and permits only named
annotation set/remove edits outside that replacement description.

Resolve selectors against the original input. A set of a whole group/subtree
replaces it completely; leaf updates retain unmentioned applicable siblings.
Reject duplicate/overlapping paths and order-dependent edit combinations. A
single call can change model/space and remove obsolete fields without publishing
an invalid intermediate state. Unknown semantic field names are errors rather
than silently becoming valid annotations. Use the explicit opaque namespace for
uninterpreted application data.

The resulting target must be structurally self-consistent, even if input samples
would fail its claimed numerical meaning. A complete color group needs its
required schema fields/references, while independent component roles/units are
allowed without a complete group. Shape, dtype, memory layout, runtime proof
and coverage fields cannot be assigned. A canonical image target remains straight
with internal alpha. Explicit boundary-payload reinterpretation must not claim
that an actual alpha conversion or image import occurred.

## Reference descriptor transform

Let M be the source metadata and S the actual immutable structural facts.
For patch, construct candidate N from M, apply the nonoverlapping set/remove
operations atomically, then resolve the selected dependency policy. For replace,
construct N from the full target semantics plus retained/explicitly edited opaque
annotations. Check N against S and all schema/resource requirements. If successful,
output samples satisfy Y[q].bits = X[q].bits for every produced q.

Dependencies=error rejects dangling/invalidated descriptions. Cascade may remove
affected dependent descriptions to a stable valid target, but cannot delete newly
assigned content, invent a replacement interpretation or repair sample values.
An unrepaired invalid explicit target fails. Missing=ignore only ignores absent
remove targets; it does not ignore ambiguous selection or invalid schema.
Empty patch is semantic identity, while empty replacement clears known semantic
descriptions and preserves unmentioned annotations. Neither bypasses layout rules.

## Demand, ownership and execution

Descriptor inference uses static metadata, schema and immutable resource inputs,
not image pixels. A runtime request Q depends on the same source coordinates Q;
view mode can share backing without loading sample bytes. Materialize copies only
Q. No complete RGB/alpha sample validation, global scan or arithmetic is added.
Data dirty mapping is identity; metadata/resource changes invalidate semantic
inference and affected consumers even when sample bytes are unchanged.

Independent output metadata must not mutate a shared source header. Retain normal
backing/profile/config owners as needed, with the family's budgets and lifetimes;
no hidden alpha ownership is created. ViewUnavailable and all static/resource/
coverage/upstream errors follow the family. An optimizer may remove a copy but
must preserve the description effect. Equal source/output bytes alone do not
permit reuse of another assigned interpretation's cache entry.

## Independent acceptance fixtures

- Start from a valid straight RGB description and immutable samples. Patch its
  primaries to another complete valid primary definition without changing other
  applicable fields. Require identical sample bytes, changed output primaries
  and unchanged source/other-consumer metadata. No RGB matrix is evaluated.
- A bare component tensor [1.6] can receive a valid coverage-role description
  without a sample-domain error or clamp. A later coverage consumer must reject
  that sample when it uses the [0,1] invariant. Likewise preserve NaN payloads,
  signaling NaN, +/-Inf and signed zeros by bits.
- Atomically replace a complete RGB group with a complete Lab group description
  and remove obsolete dependent fields explicitly. Values remain unchanged;
  the edit reinterprets components, not an RGB->Lab conversion. An incomplete
  requested Lab group fails and cascade cannot erase it to manufacture success.
- Full semantic replacement clears unmentioned old semantic fields but keeps
  an application annotation such as note="original" unless explicitly changed.
  Empty replacement leaves only retained opaque annotations; empty patch leaves
  the original description unchanged.
- Removing a required space description under error fails; under cascade remove
  the affected dependent group, retaining independent component names and all
  pixel planes. New explicitly assigned fields that would be lost make it fail.
- Assigning Float64 dtype, a different shape, a runtime proof, an external alpha
  binding or an incompatible physical image channel axis through metadata fails.
  Materialize does not turn this into an implicit import or numeric conversion.

Conceptual graph: tensor -> A -> consumers using the assigned interpretation,
with a second consumer still reading the original tensor. Implementation must
provide runnable public compile/execute examples and independent byte/descriptor
checks, partial requests, all layouts, proof/cache separation, resource lifetime,
low budgets and transactional failures. No runtime implementation or benchmark
result is claimed by this Proposed specification.
