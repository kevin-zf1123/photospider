# Post-Split Development Contract

## Purpose

This document is the maintained repository, version, preset, and CI contract
after the IPC v2 daemon split. It defines the K0 development baseline; it does
not claim that future typed-compiler capabilities are implemented.

Roadmap ordering is authoritative in
[Post-Split Roadmap v3](../roadmap/Next-Stage-Execution-Plan.md). Current runtime
behavior remains authoritative in `docs/kernel-architecture/`.

## Repository ownership

| Concern | Photospider kernel | photospider-daemon |
| --- | --- | --- |
| Embedded Host/runtime | owner | installed consumer only |
| Operation runtime, plugin SDKs, package | owner | consumes required components |
| WorkflowDocument/compiler IR/planner | future owner | never internal-schema authority |
| IPC v2 client/protocol/transport/router/registries | not owner | compatible-maintenance owner |
| `photospiderd` lifecycle and daemon tests/docs | not owner | owner |
| Job/worker/policy/trust/isolation/evidence | owner, retained | not owner |

The split is complete. New daemon-owned client/protocol/transport/router/
registry/lifecycle work is filed in the daemon repository. Mixed work is split
into reciprocal focused Issues; it is not kept under one cross-repository
authority.

## Independent version axes

| Axis | K0 value | Generated compatibility | Consumer pin |
| --- | --- | --- | --- |
| Photospider CMake package | 0.1.0 | `SameMinorVersion` | daemon requires `0.1.0 EXACT` |
| PhotospiderDaemon CMake package | 0.1.0 | `SameMinorVersion` | installed client requires exact Photospider 0.1.0 runtime |
| Local IPC wire | v2 | exact protocol-v2 surface | exact v2 admission/method inventory |

Same-minor package compatibility is intentionally narrower than same-major for
0.x development. The daemon exact dependency is narrower still and fails
closed. A package match does not imply a wire match, and a wire match does not
make compiler schemas compatible.

Future WorkflowDocument, IR, planner, digest, plan-cache, and operation-trait
versions are independently decided in #245. They are not package or IPC
versions.

## Kernel configure presets

Run `cmake --list-presets` to inspect the maintained presets.
The preset file declares CMake 3.21 as its minimum because it uses preset
schema version 3. This does not raise the project-wide CMake 3.16 minimum for
direct `cmake -S . -B <build>` configuration; it is a requirement only for the
maintained preset frontend.

| Preset | Intended work | Default closure |
| --- | --- | --- |
| `kernel-dev` | embedded kernel/runtime and maintained dependency-neutral tests | Job, CLI, optional providers/plugins, OpenEXR, and fuzzers off |
| `op-dev` | operation runtime/SDK iteration | tests and optional large products off |
| `legacy-full` | historical full developer/product validation | tests, CLI, OpenCV/YAML provider/plugin surface, and Job explicitly on |

The normal CMake default also leaves single-tenant Job `OFF`. The option,
implementation, and maintained tests remain available when explicitly enabled.
No `heavy-evidence` architecture or new placeholder option exists.

Typical commands are:

```bash
cmake --preset kernel-dev
cmake --build --preset kernel-dev

cmake --preset op-dev
cmake --build --preset op-dev

cmake --preset legacy-full
cmake --build --preset legacy-full
ctest --preset legacy-full --output-on-failure
```

## Kernel CI contract

Kernel pull requests and maintained pushes verify:

- whitespace and exact checkout health;
- all three maintained preset configure paths with proportionate builds;
- one explicit legacy-full Job-enabled producer build;
- six durable build/package smoke consumers; and
- the `unit`, `integration`, and `verification` CTest labels.

Kernel CI does not checkout `photospider-daemon`, use private overlay content,
or register migration residue as CTest. It owns kernel, operation, future
compiler, package, and installed-consumer signals.

A kernel pull request requests daemon downstream validation only when it
changes an installed public API, component/export/package contract, package
version tuple, or when a release gate explicitly requires it. Internal
compiler-only changes do not make the daemon a per-PR gate.

## Daemon downstream contract

Daemon pull requests and maintained branch pushes build against an exact
supported post-split kernel revision and separately retain the archived
full-stack revision for the old side of the four-cell gate. They verify daemon,
client, server, install, installed consumer, layout/RPATH, lifecycle, ownership,
and old/new interoperability.

A weekly/manual Ubuntu job checks current kernel `main` from an isolated
installed prefix. It is a maintenance drift signal; it neither changes the
pinned PR tuple nor gates every kernel pull request. The detailed daemon
support matrix lives in that repository's `docs/Version-and-CI-Compatibility.md`.

## Typed-compiler handoff constraints

K0 hands future development to #245 -> #199 -> #200 -> #201/#202. Until those
Issues land:

- operation ABI v1 remains the exact-size pure-C contract;
- traits may later use an engine-owned registry or versioned sidecar;
- an ABI-v1 plugin without traits is `Unknown` and permits conservative no-opt
  lowering only;
- trait v1 is limited to purity/side effects, determinism, cacheability, shape
  inference, Region/halo, static/dynamic inputs, supported candidates, and
  fail-closed unknowns;
- time/media/numeric/fusion/in-place/materialization retain extensible identity
  only;
- the first WorkflowDocument remains one function, one region, one block, and
  acyclic; and
- internal WorkflowDocument/IR/planner state is never exposed to the daemon.

#194 Host work and unrelated peripheral cleanup remain separate from #199.
K0 does not implement #199, #200, #201, or #202.

## Maintenance procedure

Any baseline change updates together:

1. CMake option/package/preset source;
2. affected kernel and daemon CI;
3. this English authority and its Chinese mirror;
4. Roadmap/OpenSpec/tracking tasks;
5. live Issue dependencies and Project fields; and
6. scoped tests followed by the single final clean verification allowed by the
   repository testing policy.

Claims must name an exact revision, command, environment, and observed result.
Future targets remain described as future until implementation evidence exists.
