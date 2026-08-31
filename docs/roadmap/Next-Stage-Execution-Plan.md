# Post-Split Roadmap v3

## Status and authority

This document is the authoritative post-split execution roadmap as of
2026-08-31. It replaces Execution Plan v2 as the current ordering authority for
K0 through K5. It describes target work unless a row explicitly says
`completed`; it does not claim that the typed compiler exists.

Current runtime facts remain authoritative in `docs/kernel-architecture/`,
accepted decisions in `docs/adr/` and English OpenSpec, the version/build/CI
baseline in the
[Post-Split Development Contract](../development/Post-Split-Development-Contract.md),
the accepted compiler identity rules in the
[Compiler Version Contract](../development/Compiler-Version-Contract.md),
and live readiness in GitHub Issues and Projects. Every implementation slice
inherits the
[Execution Slice Definition of Done](../development/Execution-Slice-Definition-of-Done.md).

## Verified post-split baseline

- [#242](https://github.com/kevin-zf1123/photospider/issues/242) is completed.
  Kernel [PR #243](https://github.com/kevin-zf1123/photospider/pull/243)
  removed daemon/IPC ownership from this repository, and daemon
  [PR #1](https://github.com/kevin-zf1123/photospider-daemon/pull/1)
  established the standalone history-preserving repository.
- `photospider` is the primary development repository for the embedded kernel,
  operation runtime, installed package, and future typed compiler.
- `photospider-daemon` is in IPC v2 compatible-maintenance. It owns the client,
  protocol, transport, codec, router, registries, `photospiderd` lifecycle, and
  daemon-owned tests and docs. It does not expand protocol v3 at K0.
- Kernel Host/runtime, Job/worker, policy, trust, isolation, and evidence code
  remain implemented and kernel-owned. K0 default-disables the single-tenant
  Job product; it does not delete its option, targets, or tests.
- Current source has `GraphDefinition`/YAML ingestion and `ComputePlan`
  diagnostics. `WorkflowDocument`, typed compiler IRs, semantic traits, plan
  digests, plan cache, and incremental compilation remain future targets.

## Repository and version boundary

| Axis | K0 value | Owner | Compatibility rule |
| --- | --- | --- | --- |
| Photospider package | 0.1.0 | kernel repository | generated package file accepts the same minor |
| PhotospiderDaemon package | 0.1.0 | daemon repository | generated package file accepts the same minor |
| Local IPC protocol | v2 | daemon repository | exact frozen 60-method wire surface |

The daemon producer and installed client currently require exact Photospider
0.1.0 even though ordinary package files advertise same-minor compatibility.
Package compatibility and wire compatibility are independent. The accepted K1
contract gives WorkflowDocument, IR, planner, digest, plan-cache, trait,
canonical-byte, and extension schemas independent initial `1.0` identities;
none is a package or IPC version. Those identities remain future runtime
objects until their implementing Issues land.

## K0-K5 execution sequence

### K0: stabilize the post-split development baseline

K0 establishes ownership, defaults, package ranges, presets, proportional CI,
Roadmap v3, focused Issues, agent guidance, and verification. It performs no
typed-compiler implementation and no protocol-v3 work.

Required K0 outcomes are:

- kernel `kernel-dev`, `op-dev`, and `legacy-full` presets;
- single-tenant Job default `OFF`, with `legacy-full` explicitly retaining it;
- same-minor package files and daemon exact Photospider 0.1.0 discovery;
- pinned daemon PR CI plus a bounded scheduled current-main signal;
- focused Host, compiler-version, plugin-DX, and daemon-maintenance Issues; and
- English/Chinese contracts, OpenSpec, tracking, and clean verification.

### K1: freeze compiler, document, and plan versions

[#245](https://github.com/kevin-zf1123/photospider/issues/245) decides only
WorkflowDocument, semantic/optimized IR, planner, digest, plan-cache, and
operation-trait versions plus breaking migration. It is a focused native child
of open parent #196. #199 and #200 wait for #245, not for all release,
rollback, signing, persistence, and artifact work under #196 to close.

[ADR 0014](../adr/0014-compiler-document-and-plan-versions-are-independent.md)
and the
[Compiler Version Contract](../development/Compiler-Version-Contract.md)
accept K1's contract-only outcome: thirteen independent `1.0` identities,
deterministic canonical bytes, three typed digest domains, explicit directed
compatibility, no writer downgrade, one-way durable migration, derived-artifact
rebuild, typed plan-key/cache invalidation, and versioned extension effects.
This acceptance implements no document, trait, IR, compiler, optimizer,
planner, digest service, cache, or execution path. K2 begins with #199.

### K2: freeze traits, then the source document

1. [#199](https://github.com/kevin-zf1123/photospider/issues/199) freezes the
   minimal OperationSemanticTraits v1.
2. [#200](https://github.com/kevin-zf1123/photospider/issues/200) freezes the
   versioned WorkflowDocument and one-way GraphDefinition/YAML migration.

Trait v1 covers only purity/side effects, determinism, cacheability, shape
inference, Region/halo behavior, static/dynamic inputs, supported candidates,
and fail-closed `Unknown`. Time/media, numeric, fusion, in-place, and
materialization retain extensible identities but no v1 behavior promise.

The operation ABI v1 exact-size C contract does not change. Traits may use an
engine-owned registry or separately versioned sidecar. An ABI-v1 plugin without
traits is `Unknown` and permits only conservative no-optimization lowering.
The first WorkflowDocument is extensible in version shape but supports exactly
one function, one region, one block, and no cycles.

### K3: deliver the differential Compiler MVP

[#201](https://github.com/kevin-zf1123/photospider/issues/201) and
[#202](https://github.com/kevin-zf1123/photospider/issues/202) proceed after
K2. Together they must prove the canonical no-optimization document-to-plan
path and independent semantic/optimized/plan identities. Unknown semantics
must reject or lower conservatively; stale plan identity cannot execute.

K3 must use engine-owned APIs and installed package consumers. Internal IR is
not serialized into the daemon, and
[#194](https://github.com/kevin-zf1123/photospider/issues/194) or unrelated
peripheral cleanup is not folded into #199.

### K4: retire the legacy planner, then optimize incrementally

After the differential path proves semantic equivalence, delete the legacy
planner in one authority cut. Do not maintain dual planners or compatibility
aliases. Then implement
[#203](https://github.com/kevin-zf1123/photospider/issues/203) incremental
recompilation and
[#149](https://github.com/kevin-zf1123/photospider/issues/149) canonical
dead/identity/constant/CSE passes against the new authority.

### K5: prove real operations before heterogeneous planning

Use real operation packages and independent oracles before advancing into
cost, liveness, tiling, transfer, residency, and heterogeneous placement.
The P0 starter/conformance/embedded CPU runner is
[#246](https://github.com/kevin-zf1123/photospider/issues/246). Data-provider
tooling is P2 in #247; the lockfile #248 is blocked by #200; policy tooling #249
is closed as deferred beyond the Compiler MVP.

Only after real-operation compiler evidence may HEX planning and the flagship
vertical become active execution work.

## Critical path

```text
post-split contract (K0)
  -> compiler/document/plan versions (#245)
  -> OperationSemanticTraits v1 (#199)
  -> WorkflowDocument (#200)
  -> no-opt lowering + identities (#201 + #202)
  -> delete the legacy planner
  -> incremental compile + canonical passes (#203 + #149)
  -> real operations and independent oracles
  -> heterogeneous planner
```

Parallel branches may proceed only where their own Start decisions are
accepted. Project parents and Completion gates are not Start edges.

## Focused post-split Issue map

| Former aggregate/mixed scope | Current disposition |
| --- | --- |
| #195 installed Host plus IPC callers | closed as superseded; kernel facade #244 and daemon adoption `photospider-daemon#2` |
| #196 broad version/release policy | remains open; completed extraction #242 and focused compiler-version child #245 |
| #143 plugin tooling mega-slice | aggregate with operation #246, data provider #247, lockfile #248, and deferred policy #249 |
| daemon package/CI/protocol work | daemon #3 package range, #4 pinned/scheduled CI, #5 IPC v2 maintenance, #6 compiler-blocked next-protocol design |

The complete OPEN inventory found no pure daemon-owned Issue that could be
natively transferred without splitting. Mixed #195 retained its history and
native parent in the kernel repository; reciprocal successors preserve
provenance. Closed #242 remains historical evidence and is not transferred.

## Projects #7-#14 remain long-term domain boundaries

Roadmap v3 changes active ordering, not the accepted Portfolio Architecture v1
domains in the
[Next-Stage Development Program](Next-Stage-Development-Program.md).

| Project | Retained long-term authority |
| --- | --- |
| #7 FND | build/package/Host seam/plugin DX/release mechanisms |
| #8 IR | WorkflowDocument, compiler IR, identities, plans, and legal optimization |
| #9 HEX | costs, memory/liveness, tile/halo, placement, transfer, residency, fallback |
| #10 MED | color, alpha, channels, time, and independent semantic oracles |
| #11 INT | viewer/edit/history/cancellation/quality product semantics |
| #12 REN | progressive renderer/AOV/checkpoint product semantics |
| #13 AUT | Python/testbench/batch/provenance/resume product semantics |
| #14 SRV | future kernel service, durable Job, worker, security, and operational domains |

Project #14 is not the standalone IPC v2 daemon. Its deferred Job, worker,
policy, trust, isolation, evidence, and service targets remain kernel-owned.
The former Execution Plan v2 Wave F is retained in Git history as portfolio
planning context, not as the active Compiler development phase.

## CI and cross-repository gates

Kernel pull requests verify kernel, operation, compiler, package, and installed
consumer behavior. Kernel CI does not checkout the daemon repository. A kernel
change requests the daemon downstream gate only when it changes an installed
API/package boundary or when a release gate explicitly requires it.

Daemon pull requests verify a pinned supported kernel revision and the frozen
old four-cell baseline on supported platforms. A scheduled/manual Ubuntu job
checks current kernel `main` as a drift signal; it is not a required check on
every kernel pull request.

## Completion rule

An Issue closes only after its focused evidence, English authority, Chinese
mirror, implementation, tests, Project fields, and live dependencies agree.
Future compiler Issues remain open until they implement their own scope. K0
completion must not be used as evidence that K1-K5 capabilities exist.
