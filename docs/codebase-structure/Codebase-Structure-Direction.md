# Codebase Structure Direction

ADR 0015 defines the breaking repository boundary. The kernel tree contains an
embeddable compiler/executor and trusted operation/provider extensions; local
daemon orchestration lives only in `photospider-daemon`.

## Public layout

Only `include/photospider/**` is installable:

```text
include/photospider/
  benchmark/     raw benchmark observations and correctness oracle
  compiler/      WorkflowDocument, typed IR, plan, typed identities, compiler
  execution/     context, cancellation, result, raw diagnostics
  data/          Value, Region, explicit layout and immutable bytes
  plugin/        operation and data-provider ABI/registry
  core/          status and symbol export
  photospider.hpp complete convenience include
```

Public headers never include `src/lib`, expose private compiler/planner nodes,
name native device objects, or require a sibling checkout. There is no public
policy, server, daemon, worker, evidence, or durable-result header.

## Private layout

```text
src/lib/
  benchmark/
  compiler/
  data/
  graph/
  execution/
  plugin/

tests/consumer/
tests/fixtures/
tests/unit/
tests/integration/
```

Private source homes follow responsibility. The active tree has no
`src/lib/server`, `src/lib/policy`, process-isolation subtree,
`plugins/policies`, worker application, or execution-profile benchmark family.

## Target shape

| Target | Installed | Role |
| --- | --- | --- |
| `photospider` / `Photospider::kernel` | yes | one public compiler/executor and ABI runtime |
| `Photospider::operation_sdk` | yes | header-only trusted operation DSO ABI |
| `Photospider::data_provider_sdk` | yes | header-only data-definition/provider DSO ABI |
| operation/provider fixture modules | no | test-only ABI validation and lifecycle |
| test executables | no | maintained unit/integration/package behavior |

Removed products have no option, default-OFF target, component, export, install
rule, preset, or compatibility alias.

## Dependency direction

```text
public values and traits
  <- compiler <- optimizer <- planner <- executor
  <- operation/provider host adapters

photospider-daemon
  -> installed Photospider::kernel
```

The kernel never depends on daemon source/package targets. Daemon tests install
the kernel to a fresh prefix and use only public package exports.

## Naming and documentation

Types use `PascalCase`; files, functions, fields, directories, and internal
targets use `snake_case`. A complete rename updates declarations, definitions,
includes, tests, CMake, public documents, mirrors, and tracked Issues without
aliases. Private OpenSpec working notes have no authority over the rename.

Every added or changed class, struct, enum, function, important field, and
anonymous helper has complete Doxygen covering behavior, parameters, return,
exceptions, threading, ownership, lifetime, and cache/scheduling effects.
