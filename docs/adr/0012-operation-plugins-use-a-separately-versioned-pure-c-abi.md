# ADR 0012: Operation Plugins Use a Separately Versioned Pure-C ABI

- Status: Accepted and implemented
- Date: 2026-08-18
- Related: ADR 0008, ADR 0011, ADR 0013, Issues #102, #103, #132

## Context

Operation plugins are independently built dynamic libraries. A public C++
registration boundary would couple plugin binaries to compiler ABI, standard
library layout, exception runtime, ownership types, and private registry
objects. DI-1 additionally requires exact DenseImage descriptor/facet/layout
metadata, while DI-2 requires immutable output plans and Host-owned allocation,
grant, seal, and publication authority. Untrusted CPU execution must cross the
existing isolated runtime without serializing process-local addresses.

The prior provisional registration generation could not express or enforce
these requirements. The migration therefore had to be one breaking package
cut: one new operation ABI, one loader path, complete repository/provider
migration, and deletion rather than compatibility adaptation.

## Decision

### The installed operation contract is pure-C ABI v1

Photospider installs `operation_plugin_api.h`, a self-contained C11/C++17
header, plus `operation_plugin.hpp`, a header-only C++17 authoring helper. The
helper may use C++ internally but emits only C records, C-linkage symbols, C
function pointers, and frozen statuses. No C++ object, callback abstraction,
exception, allocator, registry, runtime owner, or standard-library type crosses
the DSO boundary.

Every operation DSO exports exactly:

```c
uint32_t ps_operation_plugin_get_abi_version(void);
ps_operation_status_v1 ps_operation_plugin_get_api_v1(
    ps_operation_plugin_api_v1 *api);
```

The numeric probe is side-effect free. Root discovery fills an exact
Host-prepared 96-byte record. The Host queries exact 64-byte Definition,
Configuration, Inference, Region, Dependency, and Execution suites. Version,
size, kind, flags, reserved values, stride, count, alignment, pointer/count
relations, enums, identities, and nested relationships are all validated
exactly. There is no minimum-size, missing-tail, dual-loader, alias, wrapper,
or forwarding compatibility.

### ABI v1 contains complete DI-1/DI-2 records

The ABI defines 30 exact semantic record kinds. A Value projection preserves
Schema, Facet, and Layout identity/version/digest; DenseTensor rank, extents,
element semantics, storage encoding, and optional quantization; physical
StridedLayout; signed image data/display windows; axes; channels and groups;
sample-domain and color facts; bounded buffers; input slot/edge identity; and
logical Region.

Inference emits immutable output plans. Each plan names a Host-minted identity,
complete result descriptor/Region, and exact buffer size/alignment/access rows.
Only the Host allocates. Execution receives callback-scoped mutable output
bindings containing Host-minted grant identities and exact authorized spans.
The plugin receives no allocator, seal callback, `ValueBuilder`, or
transferable owner. Any non-OK status, sink failure, exception, cancellation,
malformed echo, unauthorized span, or late result fails the complete binding
closed; the Host seals and publishes at most once.

Region, dependency, and diagnostic rows are emitted through bounded Host
sinks. The first sink failure is sticky. The Host deep-copies and independently
revalidates all plugin output before it can affect planning, allocation,
execution, sealing, or publication.

### Registration prepares one immutable generation

The loader authorizes the exact opened object, confirms numeric ABI v1, fills
the prepared root/suites, enumerates and validates all bounded definitions,
deep-copies immutable metadata, creates callback/context owners, and installs
the combined sealed-object/native DSO lease before one no-throw atomic registry
publication. Any earlier failure leaves visible state unchanged.

Each publication retains revision and predecessor identity. Replacements
shadow older generations; unloading a shadowed middle generation splices its
predecessor into the newer snapshot; unload-all follows reverse successful
publication order. Registry locks are released before any foreign callback,
context destruction, generation destruction, or DSO close.

Every successful configured-context creation receives exactly one matching
destroy. Every complete generation receives one `destroy_plugin` attempt only
after definitions, contexts, results, callback snapshots, and in-flight calls
release. Destruction runs under the exact DSO lease and is never retried.

### Trusted and supervised modes share one Host validation boundary

Trusted CPU mode invokes pure-C callbacks in process under the generation
lease. Supervised CPU mode carries only a nonzero signed runtime-package
identity and resolves the matching private `PluginInvocationExecutor`. A
missing or mismatched route fails before direct callback entry; there is no
trusted fallback.

The independently versioned isolation protocol is version 2. It transports
bounded canonical copies of configuration, descriptor/facet/layout/Region,
immutable output plans, capability indices, diagnostics, dependencies, and
written ranges. It never transports pointers, callback/context values, mapped
addresses, PIDs, paths, descriptors, native handles, or DSO-generation owners.
The child validates before callback entry, and the Host decodes into fresh
objects and revalidates hostile results against the immutable request, current
invocation/resource identity, output plan, and Host grant before publication.

### Package migration is atomic

The installed component/target is `operation_plugin_sdk` /
`Photospider::operation_plugin_sdk`; it has no external link dependency.
`operation_runtime` remains a separate explicit Value/runtime component.
Repository operations, lifecycle fixtures, the OpenCV provider, optional
platform providers, CMake exports, and installed C11/C++17 consumers all use
ABI v1. All predecessor headers, symbols, registrar/callback contracts, loader
lookups, fixtures, component assertions, adapters, and active documentation are
deleted in the same migration.

## Consequences

### Positive

- C11 and C++17 consumers share one exact, compiler-neutral binary contract.
- DI-1 metadata and DI-2 Host output authority cross the boundary without
  opaque lossy payloads or a second allocator/publication owner.
- Staged publication, predecessor restoration, reverse unload, in-flight DSO
  lifetime, and exactly-once destruction remain explicit and testable.
- Trusted and supervised CPU paths converge on one Host validation/seal/commit
  boundary, while the isolation wire contains no process-local pointer.
- Package consumers can select a dependency-neutral SDK independently from
  Value/runtime or OpenCV support.

### Negative and mitigations

- The exact record surface is large. Independent C11/C++17 layout assertions
  and hostile root/suite/record/count/stride/reserved/tail fixtures lock it.
- Registration and inference deep-copy bounded metadata. Bounds make the cost
  deterministic and keep DSO-borrowed pointers out of published state.
- Pure C does not sandbox trusted native code. Trust admission remains
  explicit; untrusted work uses the fresh-process supervisor route.
- Darwin cannot perform positive exact-object supervised execution under the
  current trust policy. It proves portable layout and fail-before-process
  behavior; Linux remains the native DSO/isolation integration gate.
- This is an intentional installed binary break. Consumers must rebuild with
  the matching SDK; no ambiguous compatibility path exists.

## Scope boundary

This decision migrates only the operation plugin and isolated-invocation
boundary. DI-4 still owns the final removal or migration of public Host,
IPC/worker, durable, codec, CLI, and remaining `ImageBuffer` surfaces. Policy
ABI v1 and data-definition provider ABI v3 remain independently versioned and
unchanged.

## Validation

The durable gate includes exact installed C11/C++17 consumers, repository and
OpenCV operation behavior, rich metadata/output-plan/grant/Region/dependency
tests, malformed record rejection, replacement and middle-generation unload,
reverse unload, in-flight DSO lease, failure rollback, destroy-once, isolation
protocol-v2 round trips and hostile responses, route-before-process failure,
and supported-platform supervised execution and recovery.
