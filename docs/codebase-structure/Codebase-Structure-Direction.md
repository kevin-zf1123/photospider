# Codebase Structure Direction

ADR 0015 defines the breaking repository boundary. The kernel tree contains an embeddable compiler/executor and trusted operation/provider extensions; local daemon orchestration lives only in `photospider-daemon`.

## Public layout

Two include roots are installable. Both install under the `photospider/` prefix:

```text
include/photospider/
  compiler/      WorkflowDocument, typed IR, plan, typed identities, compiler
  execution/     context, cancellation, result, raw diagnostics
  data/          Value, Region, explicit layout and immutable bytes
  plugin/        operation and data-provider ABI/registry
  core/          status, resources, cancellation and symbol export
  benchmark/     raw compile/plan/execute benchmark runner
  photospider.hpp kernel convenience include

plugins/ops/include/photospider/
  ops/format/    format/color workflow-authoring helpers
  ops/numeric/   numeric workflow-authoring helpers
  ops/           FFT, component and statistics operation factories
  ops.hpp        kernel umbrella plus the format, FFT, component and statistics helpers
```

`include/photospider/` holds the kernel contract only. Workflow-authoring helpers that encode built-in operation names, and factories for specific operations, belong in `plugins/ops/include/photospider/ops/`. `photospider.hpp` includes no header from the `ops/` tree.

Public headers never include `src/lib`, expose private compiler/planner nodes, name native device objects, or require a sibling checkout. There is no public policy, server, daemon, worker, evidence, or durable-result header.

## Private layout

```text
src/lib/
  core/          shared primitives: checked arithmetic, Status factories,
                 exact binary sums, numeric bit helpers, resource state
  data/
  compiler/
  graph/
  execution/
  plugin/
  benchmark/

plugins/ops/<NN-family>/   built-in operation implementations

tests/consumer/
tests/fixtures/
tests/unit/
tests/integration/
```

Private source homes follow responsibility. `src/lib/core/` holds primitives with no dependency on other kernel modules; `checked_math.hpp`, `status_helpers.hpp`, `exact_binary_sum.hpp` and `numeric_bits.hpp` are the shared implementations, so other modules do not define local copies of them. The active tree has no `src/lib/server`, `src/lib/policy`, process-isolation subtree, `plugins/policies`, worker application, or execution-profile benchmark family.

## Target shape

| Target | Installed | Role |
| --- | --- | --- |
| `photospider` / `Photospider::kernel` | yes | one public compiler/executor and ABI runtime; also contains the built-in operation implementations |
| `Photospider::ops_headers` | yes | header-only include root for `photospider/ops/`; consumers link it together with `Photospider::kernel` |
| `Photospider::operation_sdk` | yes | header-only trusted operation DSO ABI |
| `Photospider::data_provider_sdk` | yes | header-only data-definition/provider DSO ABI |
| operation/provider fixture modules | no | test-only ABI validation and lifecycle |
| test executables | no | maintained unit/integration/package behavior |

Built-in operation sources under `plugins/ops/` are compiled into the kernel library. They include private headers from `src/lib/core/` and `src/lib/data/`, so the repository has no separately built built-in operation library.

Removed products have no option, default-OFF target, component, export, install rule, preset, or compatibility alias.

## Dependency direction

```text
core <- data <- compiler <- optimizer <- planner <- executor
                                     <- operation/provider host adapters

plugins/ops (built-in operations) -> public kernel headers + private core/data headers

photospider-daemon
  -> installed Photospider::kernel
```

Files in `src/lib/data/` and `include/photospider/data/` include only `core` and `data` headers. They never include execution, plugin, compiler or operation-family headers.

The kernel never depends on daemon source/package targets. Daemon tests install the kernel to a fresh prefix and use only public package exports.

## Naming and documentation

Types use `PascalCase`; files, functions, fields, directories, and internal targets use `snake_case`. A complete rename updates declarations, definitions, includes, tests, CMake, public documents, mirrors, and tracked Issues without aliases. Private OpenSpec working notes have no authority over the rename.

Public API documentation describes applicable parameter constraints, return and error behavior, ownership, lifetime, thread-safety, and cache or scheduling constraints when they affect the contract. Internal comments explain non-obvious state transitions, budget ownership, and reclamation order; routine helpers do not each need exhaustive Doxygen documentation.
