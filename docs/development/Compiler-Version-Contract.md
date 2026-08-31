# Compiler, Document, and Plan Version Contract

## Status and authority

This document is the authoritative K1 version, canonical-byte, compatibility,
migration, digest, plan-cache, and extension contract accepted by
[ADR 0014](../adr/0014-compiler-document-and-plan-versions-are-independent.md)
for [Issue #245](https://github.com/kevin-zf1123/photospider/issues/245).
It constrains later compiler implementation; it does not claim that the typed
compiler exists.

Current source-tree facts remain authoritative in
[`docs/kernel-architecture/`](../kernel-architecture/README.md). The current
tree has detached `GraphDefinition`/YAML ingestion, request-local
`ComputePlan`, a `FullTaskGraph` cache key, formal Value/artifact caches, and
operation ABI v1. It does not implement `WorkflowDocument`,
`OperationSemanticTraits`, compiler IRs, compiler digests, compiler
`ExecutionPlan`, or a typed plan cache.

Roadmap ordering remains
[K1 through K5](../roadmap/Next-Stage-Execution-Plan.md). #199 defines trait
fields, #200 defines the source-document schema and legacy importer, #201 and
#202 implement the no-optimization compiler path and identities, and K4 deletes
the legacy planner only after differential equivalence.

## Authority and scope

| Question | Authority |
| --- | --- |
| Version/canonical/digest/cache contract owner | Photospider kernel |
| Current source input | `GraphDefinition` plus configured YAML adapter |
| Future durable compiler source | `WorkflowDocument` |
| Future derived authorities | `SemanticGraphIR`, `OptimizedGraphIR`, `ExecutionPlan` |
| Future executable-plan commit authority | Kernel compiler/planner feeding existing `ComputeRun` and execution owners |
| Future plan-cache persistence | Kernel-owned discardable derived cache |
| Non-owner | `photospider-daemon`; IPC v2 is not a compiler-schema authority |

This K1 slice freezes identities and rules only. It does not define trait
fields, document/IR/plan fields, compiler APIs, runtime status types, resource
limits, optimizer legality, release/signing policy, protocol v3, or daemon
schemas. Operation ABI v1 retains its exact-size C records and entry points.

## Version identity registry

A compiler `VersionIdentity` is the tuple:

```text
(identity: ASCII [a-z0-9.-]+, major: uint32 > 0, minor: uint32)
```

Identity and version are both authoritative. Matching numeric versions under
different identities have no relationship.

| Contract | Identity | K1 version | Accepted producer -> consumer | Unknown/unsupported | Downgrade | Breaking-change effect |
| --- | --- | --- | --- | --- | --- | --- |
| Canonical bytes | `photospider.compiler-canonical-bytes` | `1.0` | `1.0 -> 1.0` only | reject before payload decode | never written | invalidate every canonical object, digest, key, and record |
| WorkflowDocument | `photospider.workflow-document` | `1.0` | `1.0 -> 1.0` only | reject before field interpretation | `UnsupportedDowngrade` | named one-way source migration; current writer only |
| OperationSemanticTraits | `photospider.operation-semantic-traits` | `1.0` | `1.0 -> 1.0` only | fail closed to `Unknown`; no optimistic optimization | `UnsupportedDowngrade` | republish/migrate sidecar; rebuild derived artifacts |
| SemanticGraphIR | `photospider.semantic-graph-ir` | `1.0` | `1.0 -> 1.0` only | reject | never written | discard and lower again from current source |
| OptimizedGraphIR | `photospider.optimized-graph-ir` | `1.0` | `1.0 -> 1.0` only | reject | never written | discard and optimize again |
| Planner behavior | `photospider.planner-contract` | `1.0` | `1.0 -> 1.0` only | reject | never written | invalidate plans and plan-cache namespace |
| ExecutionPlan | `photospider.execution-plan` | `1.0` | `1.0 -> 1.0` only | reject before scheduling | never written | discard and replan |
| SemanticGraphDigest | `photospider.semantic-graph-digest` | `1.0` | `1.0 -> 1.0`, SHA-256 | reject domain/version/algorithm | never written | recompute; invalidate downstream identity |
| OptimizedGraphDigest | `photospider.optimized-graph-digest` | `1.0` | `1.0 -> 1.0`, SHA-256 | reject domain/version/algorithm | never written | recompute; invalidate plan identity |
| ExecutionPlanDigest | `photospider.execution-plan-digest` | `1.0` | `1.0 -> 1.0`, SHA-256 | reject domain/version/algorithm | never written | recompute; stale plan cannot execute |
| PlanCacheKey | `photospider.plan-cache-key` | `1.0` | `1.0 -> 1.0`, SHA-256 | incompatible miss | never written | select a new key namespace |
| PlanCacheRecord | `photospider.plan-cache-record` | `1.0` | `1.0 -> 1.0` only | incompatible miss; record ineligible; rebuild | never written | discard record; do not migrate plan bytes |
| Compiler extension | `photospider.compiler-extension` | `1.0` | envelope `1.0 -> 1.0`; extension versions exact unless listed | required extension rejects; diagnostic may remain opaque | `UnsupportedDowngrade` | required change invalidates applicable digest/key |

The table is the complete K1 compatibility manifest. Same-major and
lower-minor relationships are not inferred. A future compatible edge must be
added explicitly with a normative migration/admission rule and golden
evidence.

## Existing identities are not aliases

| Existing identity | Current role | Why it is not a compiler identity |
| --- | --- | --- |
| Unversioned GraphDefinition/YAML | Current graph-document adapter input | #200 owns its one-way import into WorkflowDocument; it is not WorkflowDocument v1 |
| `ComputePlan` | Current request-local runtime static plan | It has no compiler schema/digest contract and is not ExecutionPlan v1 |
| `task-shape-v5` | Current full-task-graph shape token | It is one input to the legacy task-graph cache, not planner-contract v1 |
| topology generation | Current graph topology cache invalidation | It is process/runtime topology state, not semantic graph identity |
| registry task-shape generation | Current callback-shape invalidation | It is process registry state, not trait schema or planner identity |
| operation implementation identity | Current selected implementation identity | It becomes an included plan input; it is not plan/schema version |
| graph-cache manifest v2 | Current named-Value disk transaction record | It persists computed Values, not compiler plans |
| Descriptor/Content/Layout digests | Current Value/artifact identities | They answer Value questions, not graph/plan identity |
| Photospider package 0.1.0 | Installed package compatibility | Package compatibility does not admit compiler bytes |
| operation ABI v1 | Installed exact-size pure-C DSO contract | Traits remain a separate engine registry/sidecar contract |
| IPC protocol v2 | Daemon wire contract | It carries no internal document, IR, plan, digest, or plan-cache schema |

## Compiler Canonical Encoding v1

All framing integers are unsigned big-endian. A canonical object has this
exact layout:

```text
"PSCC"
u16 profile_identity_size || profile_identity_ascii
u32 profile_major || u32 profile_minor
u16 contract_identity_size || contract_identity_ascii
u32 contract_major || u32 contract_minor
u64 payload_size || canonical_value_payload
```

The profile is `photospider.compiler-canonical-bytes@1.0`. Identity bytes are
nonempty ASCII matching `[a-z0-9.-]+`. Every declared length must equal the
available bytes, the payload contains exactly one value, and trailing bytes
are malformed.

### Canonical value tags

| Tag | Value | Following bytes |
| --- | --- | --- |
| `00` | null | none |
| `01` | false | none |
| `02` | true | none |
| `10` | signed integer | exact two's-complement `i64` bits, big-endian |
| `11` | unsigned integer | exact `u64` bits, big-endian |
| `12` | binary32 | exact IEEE-754 32-bit bits, big-endian |
| `13` | binary64 | exact IEEE-754 64-bit bits, big-endian |
| `20` | UTF-8 string | `u64` byte length, then well-formed UTF-8 bytes |
| `21` | byte string | `u64` byte length, then exact bytes |
| `30` | sequence | `u64` item count, then values in logical order |
| `31` | map | `u64` pair count, then string-key/value pairs |

Map keys are canonical UTF-8 strings, unique, and strictly increasing by raw
UTF-8 key bytes. Contract field names are ASCII. An absent map key and a
present null are different. Schema-specific normalization rejects unknown core
fields and malformed values before encoding. Ordered data retains logical
order; a schema-defined unordered collection sorts by complete canonical item
bytes.

The encoding preserves exact floating-point bits. A later field schema must
explicitly normalize any numeric equivalence before encoding; K1 does not
silently decide `-0`, NaN, infinity, precision, or promotion semantics owned by
later trait/document work.

An implementation must validate magic, identities, versions, lengths, checked
arithmetic, UTF-8, tags, map order/uniqueness, schema, and its separately
frozen per-contract resource bound before allocating the complete payload. The
`u64` framing range is not an allocation allowance.

YAML/JSON spelling, whitespace, map insertion order, C++ memory, padding, host
endianness, pointers, handles, and process identities are never canonical
bytes.

## Digest framing and domain separation

Every compiler digest uses SHA-256 over this exact preimage:

```text
"PSDG"
u16 domain_identity_size || domain_identity_ascii
u32 domain_major || u32 domain_minor
u16 algorithm_size || "sha-256"
u64 canonical_object_size || canonical_object_bytes
```

The external spelling is:

```text
<domain>@<major>.<minor>:sha256:<64-lowercase-hex>
```

`SemanticGraphDigest`, `OptimizedGraphDigest`, and `ExecutionPlanDigest` use
their independent registry rows. `PlanCacheKey` uses the same framed hash
construction under its own domain. Comparing only the raw 32 digest bytes is
invalid because domain, version, algorithm, canonical profile, and owning
contract are all part of typed identity.

### Golden framing fixture

`semantic-empty-map-envelope-v1` is an encoding fixture, not a claim that an
empty map is a valid SemanticGraphIR. Its payload is the canonical empty map:

```text
31 0000000000000000
```

The complete 106-byte object is:

```text
50534343002470686f746f7370696465722e636f6d70696c65722d63616e6f6e
6963616c2d62797465730000000100000000001d70686f746f7370696465722e
73656d616e7469632d67726170682d697200000001000000000000000000000009
310000000000000000
```

Hashing that same object under four domains produces:

| Domain | SHA-256 |
| --- | --- |
| `photospider.semantic-graph-digest@1.0` | `1f64d517d05dfe9f8b79aa5478d3dd28b41c565fa76b5959c6f5cab400f30abd` |
| `photospider.optimized-graph-digest@1.0` | `a4e91909ffcebae8e7571e390a64b9850dd730b214ebd7791dfbf3acb5849f73` |
| `photospider.execution-plan-digest@1.0` | `b6d04d19fd53db3e814d843ffb8859bc807e32bf21d621115fe1b9ee9cf58524` |
| `photospider.plan-cache-key@1.0` | `bc74be910cb5ff335d062ec7e1626b5363a1cabac5ccae27fdeb21be1e4dc988` |

The distinct outputs are domain-separation evidence. They do not validate a
future graph schema.

## Checked identity chain

Later schemas must bind this exact provenance chain:

```text
WorkflowDocument canonical bytes
  -> SemanticGraphIR(version, source version, resolved traits/extensions)
  -> SemanticGraphDigest
  -> OptimizedGraphIR(version, semantic digest, pass-pipeline identity)
  -> OptimizedGraphDigest
  -> ExecutionPlan(version, optimized digest, planner identity, target facts)
  -> ExecutionPlanDigest
```

Before use, each consumer validates its own identity and every required
upstream identity. A stale semantic digest cannot be optimized, a stale
optimized digest cannot be planned, and a stale plan cannot execute. The three
digests remain distinct even when the first optimization pipeline is identity.

## Plan-cache key contract

The rule is semantic: every fact that can change plan bytes is included; every
excluded fact must be unable to change them.

| Included | Excluded |
| --- | --- |
| Canonical-profile, key, digest-domain, planner, trait, IR, and plan schema versions | Raw source spelling after semantic identity exists |
| `SemanticGraphDigest` and `OptimizedGraphDigest` | Comments, source locations, display names, diagnostic extensions |
| Effective trait snapshot identity | Graph revision alone; request, Run, session, trace, and progress ids |
| Selected operation/package/implementation identities | Timestamps, cancellation state, queue occupancy, worker selection |
| Required semantic/planning extension identities, codecs, and payload digests | Dynamic payloads declared not to shape the plan |
| Pass-pipeline identity and plan-affecting compiler options | Cache path/root, LRU/eviction state, persistence receipts |
| Static inputs that affect lowering | Daemon delivery/job state |
| Target/backend/device capability identity used to shape the plan | Native pointers, allocations, bindings, fences, leases, residency |

If an excluded fact is later shown to change `ExecutionPlan` bytes, it was
misclassified. The owning identity must change, the fact must become an
included versioned input, existing records must be invalidated, and reuse must
remain disabled until regression evidence exists.

The current `full_task_graph_cache_key()` remains a separate runtime cache key.
It is not the K1 plan key and cannot be promoted by renaming.

## Plan-cache record and invalidation

A future plan-cache record carries:

- `photospider.plan-cache-record@1.0`;
- the exact typed `PlanCacheKey`;
- `photospider.execution-plan@1.0` canonical plan bytes;
- the typed `ExecutionPlanDigest`; and
- every included identity needed for independent validation.

Lookup validates framing and all identities before plan decode. Exact match may
hit. Any unknown version, missing required extension, key/input mismatch,
stale upstream digest, canonical-byte failure, or plan-digest mismatch is
`IncompatiblePlanCacheRecord`: lookup reports a miss, makes the complete record
ineligible for reuse, and rebuilds from current source. Physical eviction may
be best effort. There is no partial hit, migrated plan, or stale runtime
fallback.

## Directed compatibility and one-way migration

Compatibility is a directed relation:

```text
(producer identity/version) -> (consumer identity/version)
```

Only a listed edge is legal. K1 lists exact `1.0 -> 1.0` for core contracts.
Same major, older minor, or newer minor is not automatically compatible.

Current writers emit only current versions. Every write downgrade fails as
`UnsupportedDowngrade`; fields and extensions are never silently dropped.

Durable `WorkflowDocument` and external trait sidecars may later have a named,
deterministic, bounded one-way migrator. It validates the complete old object,
produces current canonical bytes, atomically commits the current authority,
and never writes the old version again. The old reader exists only inside the
bounded migrator, not as a parallel durable API.

Compiler IRs, plans, digests, keys, and cache records are derived. They are
discarded and rebuilt, never migrated or reinterpreted. The unversioned
GraphDefinition/YAML importer is owned by #200. The temporary legacy/new
planner differential path is owned by #201/#202 and ends with the K4 one-way
authority cut.

Software rollback is not data downgrade. Reverting source is safe only when
the older software already has an explicit compatible read edge for every
durable input it may encounter; otherwise it must fail closed. Parent #196
continues to own broader operational rollback policy.

## Compiler extension identity

Every extension entry uses `photospider.compiler-extension@1.0` and carries:

```text
(owner, name, major, minor, effect, canonical_codec_identity,
 canonical_payload)
```

`owner` and `name` are nonempty ASCII tokens; the pair is unique in one object.
`major` is nonzero. `effect` is exactly `semantic`, `planning`, or
`diagnostic`.

- Semantic and planning extensions are required. Identity, version, codec,
  and canonical payload enter the applicable canonical object, digest chain,
  and plan key. Unknown or unsupported required extensions reject compilation
  or cache reuse.
- Diagnostic extensions may be preserved as opaque canonical bytes at the
  source-document boundary. They are excluded from semantic, optimized, plan,
  and plan-cache identities and are not interpreted as internal IR fields.
- Duplicate or conflicting extension identities reject the complete object.

The source document is the opaque-preservation authority. Internal IR and
daemon IPC are not extension archives. A future public explain/API view must
define a separate versioned projection rather than serialize compiler IR.

## Fail-closed reason taxonomy

K1 freezes stable reason names, not a public enum or numeric ABI:

| Reason | Boundary |
| --- | --- |
| `MalformedCanonicalBytes` | framing, length, UTF-8, tag, order, duplicate, arithmetic, trailing bytes |
| `UnknownContractIdentity` | unrecognized core contract or extension envelope |
| `UnknownVersion` | identity known but version absent from supported inventory |
| `UnsupportedVersion` | identity/version recognized but no directed admission edge |
| `UnsupportedDowngrade` | writer asked to emit an older version |
| `ConflictingVersionIdentity` | envelope and required embedded provenance disagree |
| `UnknownRequiredExtension` | semantic/planning extension or codec cannot be interpreted |
| `StaleDerivedIdentity` | IR/plan provenance differs from current validated upstream identity |
| `IncompatiblePlanCacheRecord` | any key/record/plan/version/digest/cache mismatch |

Later implementation may map these reasons into an engine-owned status type.
It must not turn them into successful optimistic fallback or require callers
to parse diagnostic text.

## Review examples

| Example | Input | Required result |
| --- | --- | --- |
| Exact compatible | producer and consumer both `SemanticGraphIR@1.0` | admit version, then validate bytes and dependencies |
| Same-major unknown | reader supports `WorkflowDocument@1.0`, receives `1.1` | `UnknownVersion`; no field probing |
| Breaking source migration | listed old WorkflowDocument -> current | validate old whole, atomically write current, retain no dual writer |
| Derived break | planner contract changes | discard plan/digests/cache record and rebuild |
| Unsupported downgrade | current writer asked for older document/sidecar | `UnsupportedDowngrade`; no field loss |
| Malformed bytes | duplicate map key or trailing byte | `MalformedCanonicalBytes` before digest/object publication |
| Conflicting identity | plan envelope and embedded planner identity disagree | `ConflictingVersionIdentity`; choose neither |
| Domain separation | same canonical bytes under three digest domains | three typed, non-interchangeable hashes |
| Exact cache hit | key/record/plan/digest/all included identities match | validated plan may be returned |
| Cache invalidation | trait/implementation/extension/target/version changes | incompatible miss; record becomes ineligible and rebuilds |
| Unknown semantic extension | required identity/version/codec unavailable | `UnknownRequiredExtension`; no optimistic lowering |
| Unknown diagnostic extension | well-framed source-only diagnostic payload | preserve opaquely; exclude from compiler identity |

## K1 evidence applicability

| Evidence category | K1 disposition |
| --- | --- |
| Accepted authority/version/schema | Applicable: this contract, ADR 0014, English OpenSpec |
| Named fixture and independent oracle | Applicable: `semantic-empty-map-envelope-v1`; independent SHA-256 reproductions |
| Compatible/malformed/unknown/conflicting/stale | Applicable as normative review fixtures above |
| Canonical bytes/digest/identity/cache invalidation | Applicable as framing, golden vectors, key/record rules |
| One-way migration/no dual authority | Applicable as contract; implementation belongs to #200/#201/#202 |
| Conservative fallback/fail-closed | Applicable: traitless ABI-v1 operation remains `Unknown`; unknown required identity rejects |
| Resource exhaustion | N/A runtime evidence: no decoder/allocation exists; implementing Issues must freeze and test bounds before allocation |
| Cancellation/concurrency | N/A: no compile/execute path changes; #201/#202 own runtime integration evidence |
| Runtime persistence/recovery | N/A: no plan-cache files are created; later cache implementation owns faults and recovery |
| Package consumer/platform matrix | N/A: no installed/public/package boundary changes |
| Clean configure/full build/CTest | Not run: no runtime or source behavior changed |
| Daemon downstream gate | N/A: no installed package or IPC boundary changes |
| English/Chinese/OpenSpec/tracking | Applicable in the same change |
| Live Issue/Project/PR/CI/review | Issue/Project state is audited; remote mutation, PR, CI, and review remain delivery gates outside this local slice |

## Adoption constraints for later Issues

- #199 may define trait fields only within
  `photospider.operation-semantic-traits@1.0`; it must not alter operation ABI
  v1 or infer optimistic behavior for `Unknown`.
- #200 may define `WorkflowDocument@1.0` and the one-way legacy importer. The
  first document remains one function, one region, one block, and acyclic.
- #201/#202 implement the canonical no-optimization path, identities, stale
  rejection, plan cache, installed-consumer evidence when an installed API is
  actually introduced, and differential equivalence.
- K4 deletes the legacy planner once. No permanent wrapper, alias, dual
  planner, or dual document authority is permitted.
- None of these kernel-owned objects becomes an IPC v2 or daemon schema.
