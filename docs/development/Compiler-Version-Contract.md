# Compiler and Package Version Contract

## Current contract

Package 0.35.0 uses SameMinorVersion compatibility. C++ consumers and external C operation modules must rebuild against the matching headers. Result ABI 5 requires exact table/record sizes and provides no compatibility shim. The prefix-negotiated frozen ABI in ADR 0022 remains Proposed.

| Contract | Version |
| --- | --- |
| Package | 0.35.0 |
| WorkflowDocument | 7 |
| OperationTraits | 27 |
| Result operation C ABI | 5, exact-size |
| CPU parallel / tiles service ABI | 2 / 2 |
| Native GPU service ABI | 1 |
| Result schema encoding | `photospider.result-schema.v5` |
| Semantic graph | `semantic-graph-ir-v22` |
| Physical plan | `physical-plan-v22` |
| Plan cache key | `plan-cache-key-v18` |
| Optimizer | `optimizer-v5-canonical-noop` |
| Ops TensorDescription codecs | TDM4/TDM5 |

These axes are independent. A C++ layout change can require rebuilding without changing an unrelated native service ABI. Current authoritative definitions are the public headers, CMake package declaration and compiler canonical encoders.

## Workflow inputs and operation execution

Workflow declarations require static Result schemas. Bindings supply owning ResultRef objects by exact name. A Run fixes plan, binding snapshot and generation. Each output/member has a grid under one plan-wide tile extent, default 128. Payload Needs use member slots and compact intervals; descriptor-only is explicit. Empty payload sets do nothing.

Continuations may span polls and multiple Needs. Prepared shared stages provide one producer per original node/stage within a Run, independent of output selection. Ordinary actors publish one complete member tile. Fixed bundle/prefix visibility remains separate from execution. Mandatory roots and cancellation follow the execution contract.

## Result and image interface

A Result has 1..16 named static Tensor members. Dtype, positive shape, rank, batch/atomic axes and topology are fixed before execution, including statically derived extents. Runtime counts and masks are ordinary payload inside fixed capacity. There are no dynamic field rows, End protocol, numeric port kind, failure-delivery selector or kernel scalar-bounds field.

Tensor unifies backing, fragments and view access. Host allocation defaults to tiled planar. Legal signed/zero strides, external owners and sparse physical coverage remain supported. Owning windows expose actual runs; explicit collection and packing consume capacity and work. Ordinary images default to straight alpha in operation implementations and helpers, without a kernel compliance validator.

C metadata resolution supplies schemas, selected resources and static shared-stage declarations. Retained windows have explicit retain/release lifetimes. Query and service tables expire at callback return. The kernel validates addresses, authorization, owners and synchronization without interpreting pixel values.

## Identity and runtime matching

Canonical identities include the prepared operation contracts, static schemas, parameters, selected resources, input projections, shared-stage definitions and relevant backend choices. Raw pointers, padding and allocation addresses do not define semantic identity. C import rejects wrong ABI or size; native service availability also depends on build and device. Internal IR and plans are not serialized wire formats.
