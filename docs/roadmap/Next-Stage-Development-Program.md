# Next-Stage Development Program

## Status and Authority

This document is the authoritative public delivery map for the development
program that follows GitHub Projects #1 through #6. It records intended work,
not current software behavior. Current behavior remains authoritative in
`docs/kernel-architecture/`, architectural decisions remain authoritative in
`docs/adr/`, and live delivery state is authoritative in the linked GitHub
Projects and Issues.

The portfolio consists of eight private user-level GitHub Projects. Every
Project has one open parent Issue and five open executable child Issues in
`kevin-zf1123/photospider`. Closing a planning task, changing a Project field,
or finishing an exploratory prototype does not promote a target to current
behavior. Promotion requires scoped architecture and OpenSpec authority,
implementation, durable tests, synchronized English and Chinese documentation,
and the completion evidence defined below.

## Starting Baseline

Closed Projects #1 through #6 established the boundaries this program extends:
Graph identity and revision, request-owned `ComputeRun`, a process-owned
`ExecutionService`, cancellation and latest-wins arbitration, resource
accounting, generic `Value`/`Region`/`Binding`/`ReadyFence` contracts, the
pure-C operation plugin ABI, isolated workers, and a single-tenant durable job
vertical. The new Projects must reuse those authorities rather than introduce
parallel graph, scheduling, resource, output, plugin, or durable-job owners.

The central missing layer is the explicit pipeline:

```text
WorkflowDocument
        ↓
SemanticGraphIR
        ↓
OptimizedGraphIR
        ↓
ExecutionPlan
        ↓
existing Run and execution domain
```

The remaining product work is divided around that pipeline so that engineering
risk, compiler semantics, execution policy, media meaning, and product-facing
verticals can advance without one indefinitely broad umbrella Project.

## Portfolio

| Order | Project | Parent Issue | Priority | Area | Depends on | Intended outcome |
| --- | --- | --- | --- | --- | --- | --- |
| #7 | [engineering-foundations-plugin-dx](https://github.com/users/kevin-zf1123/projects/7) | [#139](https://github.com/kevin-zf1123/photospider/issues/139) | P0 | Engineering | — | Maintainable build, package, source, security, persistence, release, and plugin-development foundations. |
| #8 | [graph-ir-optimization-planning](https://github.com/users/kevin-zf1123/projects/8) | [#145](https://github.com/kevin-zf1123/photospider/issues/145) | P0 | Compiler | — | An explicit, immutable, explainable document-to-plan compiler pipeline and semantics-preserving graph optimizations. |
| #9 | [cost-aware-heterogeneous-execution](https://github.com/users/kevin-zf1123/projects/9) | [#151](https://github.com/kevin-zf1123/photospider/issues/151) | P1 | Execution | #8 | Unified ROI, tile, cache, memory, transfer, and device planning with safe device-resident execution. |
| #10 | [media-semantics-color-time](https://github.com/users/kevin-zf1123/projects/10) | [#157](https://github.com/kevin-zf1123/photospider/issues/157) | P1 | Media | #8 | Explicit color, alpha, channel, frame, time, and sample meaning shared by every consumer. |
| #11 | [interactive-viewer-editing](https://github.com/users/kevin-zf1123/projects/11) | [#163](https://github.com/kevin-zf1123/photospider/issues/163) | P2 | Interactive | #8, #9, #10 | Viewer sessions, undoable edits, strokes, dirty tiles, cancellation, and progressive presentation. |
| #12 | [progressive-renderer-outputs](https://github.com/users/kevin-zf1123/projects/12) | [#169](https://github.com/kevin-zf1123/photospider/issues/169) | P2 | Renderer | #8, #9, #10 | Renderer adapters, progressive tiles, AOVs, staged deep/multiview output, and checkpoint validation. |
| #13 | [python-testbench-batch-automation](https://github.com/users/kevin-zf1123/projects/13) | [#175](https://github.com/kevin-zf1123/photospider/issues/175) | P2 | Automation | #8, #10 | A stable Python facade, algorithm testbench, sequence orchestration, provenance, resume, and packaging. |
| #14 | [multi-tenant-production-services](https://github.com/users/kevin-zf1123/projects/14) | [#181](https://github.com/kevin-zf1123/photospider/issues/181) | P2 | Service | #7, #9, #13 | Authenticated, quota-governed, isolated, durable, observable production service boundaries. |

## Dependency and Delivery Order

```text
                    ┌──────────────────────────────────┐
                    │ #7 engineering foundations       │
                    └────────────────┬─────────────────┘
                                     │
                                     └──────────────────────────┐

┌──────────────────────────────────┐                            │
│ #8 graph IR and planning         │                            │
└───────────────┬──────────────────┘                            │
                ├───────────────┐                               │
                ↓               ↓                               │
┌───────────────────────┐  ┌────────────────────────┐           │
│ #9 heterogeneous exec │  │ #10 media semantics    │           │
└───────────┬───────────┘  └───────────┬────────────┘           │
            │                          │                        │
            ├──────────────┬───────────┼─────────────┐          │
            ↓              ↓           ↓             ↓          │
     ┌────────────┐  ┌────────────┐  ┌────────────┐             │
     │ #11 viewer │  │ #12 render │  │ #13 Python │─────────────┤
     └────────────┘  └────────────┘  └────────────┘             │
                                                                  ↓
                                                        ┌────────────────┐
                                                        │ #14 services   │
                                                        └────────────────┘
```

Projects #7 and #8 begin in parallel. Project #8 establishes the typed planning
seam needed by #9 and #10. Projects #11 and #12 require both execution and
media semantics; #13 requires graph IR and temporal/media semantics. The
service Project #14 is deliberately last: it requires engineering hardening,
heterogeneous resource policy, and durable batch automation before untrusted
multi-tenant exposure is considered.

A dependency means the downstream Project cannot claim its product completion
gate until the cited upstream contracts and required vertical slices are
verified. It does not prevent bounded research, interface exploration, or test
fixture preparation from occurring earlier.

## Common Project Model

All eight Projects use the `Portfolio` table view and the same planning fields:

| Field | Values or role |
| --- | --- |
| `Status` | `Todo`, `In Progress`, `Done` |
| `Priority` | `P0`, `P1`, `P2`, `P3` |
| `Area` | `Engineering`, `Compiler`, `Execution`, `Media`, `Interactive`, `Renderer`, `Automation`, `Service` |
| `Phase` | `Discovery`, `Contract`, `Vertical Slice`, `Integration`, `Hardening`, `Production` |
| `Target` | `Foundation`, `Vertical Slice`, `Product`, `Production` |
| `Risk` | `Low`, `Medium`, `High` |
| `Work Type` | `AFK`, `HITL` |
| `Verification` | `Planned`, `In Review`, `Verified`, `Blocked` |

GitHub reserves `Type` and rejects it as a custom Project V2 field name, so the
established AFK/HITL routing concept is represented as `Work Type`.

The repository has no release milestones, and this portfolio does not invent
calendar commitments without release evidence. Phase, target, and dependency
fields carry the current sequencing intent; a future milestone must represent
a separately approved release boundary.

Every parent Issue is a native GitHub parent of its five child Issues. Parent
Issues carry `enhancement`, `codebase-structure`, and `ready-for-human`; each
contract-setting first child starts as `ready-for-human`, while independently
executable later children start as `ready-for-agent`. The standard triage
labels remain authoritative; this program creates no competing role labels.

Every Issue defines Problem, Goal, Scope, Non-goals, Dependencies, Acceptance
criteria, Docs, Tests, Risks, and Project traceability. A child must link the
upstream parent Issues on which it depends, and a parent must contain the real
child checklist and dependency links. All future-development Issues remain
open until their own evidence is complete.

## Project #7: Engineering Foundations and Plugin DX

Goal: keep the kernel easy to change, package, secure, diagnose, and extend as
new product domains are added.

Invariants:

- runtime behavior and public/plugin contracts change only through scoped,
  tested proposals;
- build, package, and CI gates validate long-lived software behavior rather
  than migration residue; and
- complexity reduction preserves established ownership and lifecycle
  boundaries without compatibility wrappers.

Non-goals include reimplementing capabilities delivered by Projects #1–#6 and
registering source-quality or migration-residue audits as product CTest/CI
tests.

| Issue | Executable slice |
| --- | --- |
| [#140](https://github.com/kevin-zf1123/photospider/issues/140) | Harden CI, CMake, packaging, and security gates. |
| [#141](https://github.com/kevin-zf1123/photospider/issues/141) | Split the Host mega-interface and clarify facade ownership. |
| [#142](https://github.com/kevin-zf1123/photospider/issues/142) | Reduce ready-store, worker-loop, and monitor complexity. |
| [#143](https://github.com/kevin-zf1123/photospider/issues/143) | Deliver plugin SDK tooling, templates, and conformance harnesses. |
| [#144](https://github.com/kevin-zf1123/photospider/issues/144) | Establish cross-platform persistence and release governance. |

## Project #8: Graph IR, Optimization, and Planning

Goal: create the missing compiler/planning layer between user workflow intent
and the existing execution kernel.

The document, semantic intent, optimized topology, and physical execution plan
remain distinct immutable artifacts. Optimizations preserve declared media and
value semantics, generate deterministic provenance, and never move topology
authority into scheduling. Backend-specific code generation is not committed
before these contracts are proven.

| Issue | Executable slice |
| --- | --- |
| [#146](https://github.com/kevin-zf1123/photospider/issues/146) | Define `WorkflowDocument` and `SemanticGraphIR` contracts. |
| [#147](https://github.com/kevin-zf1123/photospider/issues/147) | Lower `SemanticGraphIR` to `OptimizedGraphIR` and `ExecutionPlan`. |
| [#148](https://github.com/kevin-zf1123/photospider/issues/148) | Implement validation and explain/inspection tooling. |
| [#149](https://github.com/kevin-zf1123/photospider/issues/149) | Implement deterministic dead, identity, constant, and common-subexpression passes. |
| [#150](https://github.com/kevin-zf1123/photospider/issues/150) | Implement transform concatenation, pointwise fusion, channel pruning, and static-subgraph hoisting. |

## Project #9: Cost-Aware Heterogeneous Execution

Goal: turn current device, residency, transfer, and resource primitives into a
cost-aware end-to-end execution planner.

The Host resource ledger and exact ownership identities remain authoritative.
Transfers and residency are explicit plan decisions with observable fallback;
hidden copies are not permitted. No universal GPU acceleration promise or
numeric performance target is accepted before representative baselines exist.

| Issue | Executable slice |
| --- | --- |
| [#152](https://github.com/kevin-zf1123/photospider/issues/152) | Define a unified ROI, tile, cache, memory, and device cost model. |
| [#153](https://github.com/kevin-zf1123/photospider/issues/153) | Plan explicit transfers and device residency across operation chains. |
| [#154](https://github.com/kevin-zf1123/photospider/issues/154) | Execute GPU-resident operation chains with safe fallback. |
| [#155](https://github.com/kevin-zf1123/photospider/issues/155) | Calibrate memory, cache, and tile budgets with reproducible benchmarks. |
| [#156](https://github.com/kevin-zf1123/photospider/issues/156) | Add heterogeneous-plan explainability and regression gates. |

## Project #10: Media Semantics, Color, Channel, and Time

Goal: make media meaning explicit enough for correct optimization,
compositing, rendering, and sequence processing.

Storage representation, declared media semantics, and observed statistics
remain distinct. Color, alpha, channel, and time transformations are explicit
and versioned. Ambiguous conversions and optimizations fail closed. The kernel
does not embed one studio configuration or infer authoritative meaning from
filenames.

| Issue | Executable slice |
| --- | --- |
| [#158](https://github.com/kevin-zf1123/photospider/issues/158) | Define color-space and OCIO integration boundaries. |
| [#159](https://github.com/kevin-zf1123/photospider/issues/159) | Define alpha association and compositing semantics. |
| [#160](https://github.com/kevin-zf1123/photospider/issues/160) | Define channel sets, names, routing, and pruning. |
| [#161](https://github.com/kevin-zf1123/photospider/issues/161) | Define frame, time, sample, and sequence semantics. |
| [#162](https://github.com/kevin-zf1123/photospider/issues/162) | Build a media-semantics reference corpus and conformance suite. |

## Project #11: Interactive Viewer and Editing

Goal: deliver a low-latency editing loop without moving graph, resource, or
commit authority into the UI.

Viewer state never becomes graph or execution authority. Edits, histories,
strokes, dirty tiles, cancellation, and presentation have explicit identities
and commit boundaries. The kernel contract does not choose a permanent GUI
toolkit, and latency targets follow the established baseline-before-target
protocol.

| Issue | Executable slice |
| --- | --- |
| [#164](https://github.com/kevin-zf1123/photospider/issues/164) | Define viewer session, render request, and presentation contracts. |
| [#165](https://github.com/kevin-zf1123/photospider/issues/165) | Implement undo and redo edit transactions. |
| [#166](https://github.com/kevin-zf1123/photospider/issues/166) | Implement brush and stroke lifecycle with dirty tiles. |
| [#167](https://github.com/kevin-zf1123/photospider/issues/167) | Implement low-latency updates, cancellation, and quality ladders. |
| [#168](https://github.com/kevin-zf1123/photospider/issues/168) | Build an interactive benchmark and deterministic UI integration harness. |

## Project #12: Progressive Renderer and Outputs

Goal: use Photospider as a renderer-facing graph and execution kernel without
embedding renderer ownership into the core.

Renderer backends remain adapters/providers behind versioned contracts.
Progressive updates preserve revision, channel, time, ownership, and
completion identity. Deep and multiview support is staged and explicitly
bounded; the first slice does not claim complete deep tiled or multipart
coverage.

| Issue | Executable slice |
| --- | --- |
| [#170](https://github.com/kevin-zf1123/photospider/issues/170) | Define renderer backend and execution contracts. |
| [#171](https://github.com/kevin-zf1123/photospider/issues/171) | Implement progressive tile scheduling and convergence. |
| [#172](https://github.com/kevin-zf1123/photospider/issues/172) | Define and transport AOV and channel outputs. |
| [#173](https://github.com/kevin-zf1123/photospider/issues/173) | Add staged deep and multiview output support. |
| [#174](https://github.com/kevin-zf1123/photospider/issues/174) | Build a renderer corpus, golden validation, and checkpoint resume. |

## Project #13: Python Testbench and Batch Automation

Goal: make the kernel usable for image-algorithm development and
high-throughput automation without bypassing Host, job, or artifact contracts.

Python exposes owned values and stable error/lifetime semantics, never borrowed
mutable kernel state. Batch manifests, provenance, outcomes, and resume
decisions are durable and deterministic. Convenience APIs do not become a
second graph or execution authority, and the first slice does not embed CPython
into the kernel.

| Issue | Executable slice |
| --- | --- |
| [#176](https://github.com/kevin-zf1123/photospider/issues/176) | Define a stable Python facade and ownership model. |
| [#177](https://github.com/kevin-zf1123/photospider/issues/177) | Build an image-algorithm testbench with independent oracles. |
| [#178](https://github.com/kevin-zf1123/photospider/issues/178) | Implement batch sequence and frame orchestration. |
| [#179](https://github.com/kevin-zf1123/photospider/issues/179) | Add high-throughput manifests, provenance, and resume. |
| [#180](https://github.com/kevin-zf1123/photospider/issues/180) | Package the Python SDK, examples, and cross-platform CI. |

## Project #14: Multi-Tenant Production Services

Goal: deliver networked multi-tenant service boundaries without relabeling the
current local sidecar as a public server.

Tenant identity, authorization, quota, durable state, worker isolation, and
artifact access are explicit security domains. The single-tenant job vertical
remains valid until each authority is replaced through a scoped migration. The
current local IPC sidecar is never exposed directly to untrusted networks, and
production SLOs require reproducible load and failure baselines first.

| Issue | Executable slice |
| --- | --- |
| [#182](https://github.com/kevin-zf1123/photospider/issues/182) | Define tenant identity, authentication, authorization, and quota contracts. |
| [#183](https://github.com/kevin-zf1123/photospider/issues/183) | Extend durable job state to tenant namespaces. |
| [#184](https://github.com/kevin-zf1123/photospider/issues/184) | Build a production API and idempotent control plane. |
| [#185](https://github.com/kevin-zf1123/photospider/issues/185) | Harden worker pools, the artifact plane, and network isolation. |
| [#186](https://github.com/kevin-zf1123/photospider/issues/186) | Establish service baselines, load/chaos tests, and operational readiness. |

## Completion Gates

An executable child Issue is complete only when all of the following are true:

1. Its current-state baseline and exact boundary are documented.
2. A scoped OpenSpec change and any required ADRs are approved before the
   implementation changes authority or public contracts.
3. Implementation and durable behavior tests cover success, failure,
   concurrency, cancellation, resource settlement, and package/consumer
   boundaries as applicable.
4. English authoritative documentation and faithful Chinese mirrors are
   synchronized.
5. Verification records list the exact commands, environments, raw evidence,
   and limitations; numeric targets are adopted only after representative
   baseline review.
6. The child Issue, native parent relationship, Project fields, and dependency
   links agree with the delivered scope.

A Project is complete only after all five child Issues satisfy those gates,
the parent evidence is reviewed, downstream dependencies are updated, no
duplicate authority or permanent compatibility layer remains, and the Project
is explicitly closed. Research results may alter or reject a planned design;
they are not silently converted into product claims.

## Planning-Change Boundary

The active OpenSpec change `establish-next-stage-development-program` governs
creation and verification of this portfolio, its issue hierarchy, its
traceability fields, and these planning documents. It does not implement the
48 future development Issues. Each child Issue that changes software behavior
requires its own bounded proposal (or an explicitly compatible small change),
tests, and completion evidence. This keeps the planning change finite while
preserving a single traceable program.
