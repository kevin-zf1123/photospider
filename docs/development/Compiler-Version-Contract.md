# Compiler and Package Version Contract

## Current contract

Package 0.36.0 uses SameMinorVersion compatibility. C++ consumers and external C operation modules must rebuild against the matching headers. Result ABI 6 requires exact table/record sizes and provides no compatibility shim. The prefix-negotiated frozen ABI in ADR 0022 remains Proposed.

| Contract | Version |
| --- | --- |
| Package | 0.36.0 |
| WorkflowDocument | 7 |
| OperationTraits | 28 |
| Result operation C ABI | 6, exact-size |
| CPU parallel / tiles service ABI | 2 / 2 |
| Native GPU service ABI | 1 |
| Result schema encoding | `photospider.result-schema.v6` |
| Semantic graph | `semantic-graph-ir-v23` |
| Physical plan | `physical-plan-v23` |
| Plan cache key | `plan-cache-key-v18` |
| Optimizer | `optimizer-v5-canonical-noop` |
| Ops TensorDescription codecs | TDM4/TDM5 |

These axes are independent. Package `0.36.0` uses SOVERSION `0.36`. `SchemaTemplate` defines structural members and scopes; `ResultTensorSpec` defines shape, dtype, batch/atomic axes and optional spatial layout. `ResultViewAxis` uses anchored affine coordinates and signed steps. `TileGrid::from_schema` returns `Result<TileGrid>`; `OperationRegistry::prepare_operation` prepares the static contract. `ExecutionContext` uses `ResourceBudget` for Root admission, while snapshot reservations have an independent bounded budget. External C DSOs export `ps_result_operation_plugin_get_api_v6` with ABI 6's exact sizes, explicit non-NULL tensor-view transforms, and a 1..16-piece C transaction service. A version or record-size mismatch is rejected during import. ADR 0022 remains a Proposed, unimplemented separate contract. Current authoritative definitions are the public headers, CMake package declaration and compiler canonical encoders.

## Workflow inputs and operation execution

Workflow declarations require static Result schemas. Bindings supply owning ResultRef objects by exact name. A Run fixes plan, binding snapshot and generation. Each output/member has a grid under one plan-wide tile extent, default 128. Payload Needs use member slots and compact intervals; descriptor-only is explicit. Empty payload sets do nothing.

Continuations may span polls and multiple Needs. Prepared shared stages provide one producer per original node/stage within a Run, independent of output selection. Ordinary actors publish one complete member tile. Fixed bundle/prefix visibility remains separate from execution. Mandatory roots and cancellation follow the execution contract.

## Result and image interface

A Result has 1..16 named static Tensor members. Dtype, positive shape, rank, batch/atomic axes and topology are fixed before execution, including statically derived extents. Runtime counts and masks are ordinary payload inside fixed capacity. There are no dynamic field rows, End protocol, numeric port kind, failure-delivery selector or kernel scalar-bounds field.

Tensor unifies backing, fragments and view access. Host allocation defaults to tiled planar. Legal signed/zero strides, external owners and sparse physical coverage remain supported. Owning windows expose actual runs; explicit collection and packing consume capacity and work. Ordinary images default to straight alpha in operation implementations and helpers, without a kernel compliance validator.

C metadata resolution supplies schemas, selected resources and static shared-stage declarations. Retained windows have explicit retain/release lifetimes. Query and service tables expire at callback return. The kernel validates addresses, authorization, owners and synchronization without interpreting pixel values.

## Identity and runtime matching

Canonical identities include the prepared operation contracts, static schemas, parameters, selected resources, input projections, shared-stage definitions and relevant backend choices. Raw pointers, padding and allocation addresses do not define semantic identity. C import rejects wrong ABI or size; native service availability also depends on build and device. Internal IR and plans are not serialized wire formats.
