# Compiler and Package Version Contract

## Current contract

Package 0.37.0 uses SameMinorVersion compatibility. C++ consumers and external C operation modules must rebuild against the matching headers. Result ABI 7 requires exact table/record sizes and provides no compatibility shim; modules built for ABI 1 through 6 are rejected. The prefix-negotiated frozen ABI in ADR 0022 remains Proposed.

| Contract | Version |
| --- | --- |
| Package | 0.37.0 |
| WorkflowDocument | 7 |
| OperationTraits | 29 |
| Result operation C ABI | 7, exact-size |
| CPU parallel / tiles service ABI | 2 / 2 |
| Native GPU service ABI | 1 |
| `.psir` / `.psxp` text format | 3 |
| Result schema encoding | `photospider.result-schema.v6` |
| Semantic IR | `semantic-ssa-ir-v25` |
| Optimizer | `optimizer-v7-dependency-paths` |
| Physical plan | `static-tile-program-v26` |
| Plan cache key | `plan-cache-key-v19` |
| Saved operation contract | `artifact-static-operation-contract-v2` |
| Step dependency template | `photospider.dependency-static.v4` |
| Fixed grant / block input proof | `photospider.fixed-grant-cache.v2` / `photospider.fixed-block-inputs.v2` |
| Ops TensorDescription codecs | TDM4/TDM5 |

These axes are independent. Package `0.37.0` uses SOVERSION `0.37`. `SchemaTemplate` defines structural members and scopes; `ResultTensorSpec` defines shape, dtype, batch/atomic axes and optional spatial layout. `ResultViewAxis` uses anchored affine coordinates and signed steps. `TileGrid::from_schema` returns `Result<TileGrid>`; `OperationRegistry::prepare_operation` prepares the static contract. `ExecutionContext` uses `ResourceBudget` for Root admission, while snapshot reservations have an independent bounded budget. External C DSOs export `ps_result_operation_plugin_get_api_v7` with ABI 7's exact sizes, an optional static `map_dependencies` hook as the last member of each operation record, explicit non-NULL tensor-view transforms, and a 1..16-piece C transaction service. A module that also exports any `_v1` through `_v6` getter fails with `TypeMismatch`; a version or record-size mismatch is rejected during import. Renaming the getter or changing `abi_version` alone does not make an ABI 6 module valid, because the operation record layout and the meaning of run-time Needs changed. ADR 0022 remains a Proposed, unimplemented separate contract. Current authoritative definitions are the public headers, CMake package declaration and compiler canonical encoders.

## Compiler stages and artifacts

The compiler exposes four independently callable stages: DAG analysis or `bind_ir` to `SemanticGraphIR`, optimization to `OptimizedGraphIR`, `plan` to `ExecutionPlan`, and execution. DAG analysis emits `IRForm::DependencyPaths`, preserving repeated requests for shared Call IDs in the raw paths. Optimization retains the first request for each call, removes empty chains and reconnects the blocks as `ControlFlow`. Explicit ControlFlow input retains its CFG. The program-form field changes public C++ IR layouts, so consumers must rebuild with matching headers and libraries; WorkflowDocument schema 7 and C Result operation ABI 7 are unchanged.

`CompilerArtifact` stores IR stages as `.psir` (`stage semantic` or `stage optimized`) and plans as `.psxp`, both at text format version 3. Each file must declare `form dependency_paths` or `form control_flow`. Only semantic IR permits dependency paths; optimized IR and plans require control flow. Earlier versions, including v2, are rejected without a converter; regenerate them from source workflow or IR. An artifact manifest names each operation key with its `artifact-static-operation-contract-v2` digest and each resource by SHA-256 and byte length. Import fails with `TypeMismatch` when a registered contract digest differs and `NotFound` when an operation or resource is missing. Digests are recomputed on import, and the stored parent digest is provenance rather than a signature. See [Compiler and local execution](../kernel-architecture/Compiler-and-Execution.md#compiler-artifacts).

## Workflow inputs and operation execution

Workflow declarations require static Result schemas. Bindings supply owning ResultRef objects by exact name. A Run fixes plan, binding snapshot and generation. Each output/member has a grid under one plan-wide tile extent, default 128. Static mappers return member-slot reads with compact intervals at planning time; descriptor-only reads are explicit, and empty payload sets do nothing. The plan stores these as fixed grants.

Continuations may span polls, but a poll-time Need only asserts compiled grants. Shared-stage reads require explicit static mapping entries. Stages reached through those reads or mandatory roots share one producer per call, stage and block activation, even across multiple outputs. Ordinary tile instructions publish one complete member tile. Fixed bundle/prefix visibility is compiled into read closures. Each call's produced outputs are certified after its instructions and before the next call starts. Mandatory roots and cancellation follow the execution contract.

## Result and image interface

A Result has 1..16 named static Tensor members. Dtype, positive shape, rank, batch/atomic axes and topology are fixed before execution, including statically derived extents. Runtime counts and masks are ordinary payload inside fixed capacity. There are no dynamic field rows, End protocol, numeric port kind, failure-delivery selector or kernel scalar-bounds field.

Tensor unifies backing, fragments and view access. Host allocation defaults to tiled planar. Legal signed/zero strides, external owners and sparse physical coverage remain supported. Owning windows expose actual runs; explicit collection and packing consume capacity and work. Ordinary images default to straight alpha in operation implementations and helpers, without a kernel compliance validator.

C metadata resolution supplies schemas, selected resources and static shared-stage declarations; the C mapping hook supplies static reads. Retained windows have explicit retain/release lifetimes. Query and service tables expire at callback return. The kernel validates addresses, authorization, owners and synchronization without interpreting pixel values.

## Identity and runtime matching

Canonical compiler identities include the program form, program identifiers, prepared operation contracts, static schemas, parameters, selected resources, input projections, shared-stage definitions, relevant backend choices and, for plans, every instruction and grant. Renumbering calls or values therefore changes semantic, optimized and plan digests. Content cache identities exclude physical node, step and plan identifiers. Raw pointers, padding and allocation addresses do not define identity. C import rejects wrong ABI or size; native service availability also depends on build and device. Compiler artifacts are editable versioned text and carry no native pointers, library paths, device handles, Run state or cache content.
