---
spec_schema_version: 1
id: FMT-08A
parent_id: FMT-08
function: assign_metadata
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
operation_keys:
  - metadata.assign_strict
  - metadata.assign_accelerated_apple_silicon
  - metadata.assign_accelerated_x86_64
---

# FMT-08A: atomically assign or reinterpret metadata

Inherit the complete [FMT-08 contract](FMT-08_metadata_assignment_contract.md).
The current CPU registrations use Result operation ABI 2, WorkflowDocument 4,
OperationTraits 21 and package 0.30.0. The specification status remains
Proposed. A publishes updated immutable metadata through a Result tensor while
preserving the logical samples and Result schema. It is neither a validation
certificate nor a numeric conversion. The metadata-to-
`channel.extract` configuration/resource chain is covered; `channel.assemble`
composition is not.

## Interface and inference

Input `input` is a Result with exactly one tensor member and no fields. Output
`values` preserves the source schema identity, tensor key, dtype, descriptor,
batch axes and layout. Its tensor has the same sample bits for every produced
coordinate. The current supported element types are UInt8, UInt16, Int8, Int16,
Int64, Float32 and Float64, with rank 1 through 8. Parameters
are mode, set, conditional description, remove, dependencies, missing and layout.
Mode patch defaults to editing named semantic fields or opaque annotations, with
atomic explicit deletions. Mode replace takes a complete registered semantic
description, retains unspecified opaque annotations and permits only named
annotation set/remove edits outside that replacement description.

When an internal node explicitly supplies the optional `expected_source`
parameter, static preparation compares it with the input schema's canonical
form. The public helper does not set this parameter. Resolve selectors against
the original input. A set of a whole group/subtree
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

Static preparation validates the Result schema, tensor description, typed edits,
resources and complete candidate; it does not read sample values. For output
footprint Q, Result Need requests Data support Q and Descriptor support for the
input description, with role mask 9 (Data 1 | Descriptor 8). No Validation or
Control role is requested. Dependency-v2 maps each output coordinate to the
same input coordinate. Changed data dirties the corresponding coordinates;
metadata changes invalidate the result description and its consumers.

The Result continuation retains source backing, resource owners and association
when it publishes a legal view. `auto` falls back to copying only on
`ViewUnavailable`; `view` reports that failure, and `materialize` always copies
requested coverage. Copy publication is transactional and polls cancellation at
most every 256 samples. An empty output request uses a stateless continuation
and does not request sample payload. The three profiles share these semantics.
Result caching is disabled. An optimizer must preserve the metadata effect,
support relation and failure behavior.

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

The public [metadata workflow](../../../../examples/metadata_workflow/README.md)
shows Result binding, compilation, execution and exact-byte checks. The old
[metadata performance workload](../../../../examples/metadata_performance/README.md)
measures the previous Value/planar implementation and is not current Result
performance evidence.
