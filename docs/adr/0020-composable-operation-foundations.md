# ADR 0020: Describe Static Tensors and Infer Operation Outputs

- Status: Accepted
- Reader mirror: [Chinese](zh/0020-composable-operation-foundations.zh.md)

## 1. Core summary (TL;DR)

Composition requires each output structure to be known before execution. Result ports, named outputs, structural constraints, pure preparation and explicitly bound validators provide that contract. Domain interpretation belongs to operation parameters and SDK helpers.

## 2. Mental model and intuition

Preparation receives ordered input schemas and static parameters, resolves output schemas and input projections, selects immutable resources and fixes shared-stage declarations. The compiler incorporates those contracts into identity. Runtime callbacks compute samples under that fixed structure. Tensor storage does not certify a numerical domain or propagate color descriptions.

## 3. Formal contracts and APIs

`TensorDescriptor` specifies dtype and positive shape. Complete sample rank remains 1..8. `SchemaTemplate` contains 1..16 named static Tensor members with explicit batch/atomic axes and optional spatial topology. Extents are fixed or derived from static input axes/element counts. No runtime sample value chooses member count, dtype, rank or shape.

OperationTraits version 28 and Result C ABI 6 expose Result-only ports. `OperationPortConstraint` supports structural predicates and explicit validator identities. `OperationOutputTraits` supplies each output schema and input projection. There is no port-kind selector, kernel scalar-bounds interval or Region execution-rule field.

A finite Float32 interval is checked only when an operation explicitly binds the ops SDK `ops.scalar-range` validator. Member slot, bounds and validator implementation enter prepared validation identity. Ordinary scalar `[1]` inputs do not automatically acquire interval requirements. Complete validators require authorized coverage of their full scope and never add Needs themselves.

Data-dependent result counts use static-capacity Tensor members and ordinary `count`/mask payload. Readers issue payload Needs for those values. Published tiles are complete and padded as specified by the operation; complete-result algorithms fail when capacity is insufficient.

Algorithms needing complete inputs use nonblocking SDK acquisition and prepared Run-shared stages. Outputs publish selected member tiles from the shared result. Independent outputs retain separate projections; joint stages have their own fixed projection. See [Global Results](../kernel-architecture/Global-Results.md).

## 4. Non-goals and explicit boundaries

The kernel does not attach domain descriptions, infer RGBA, select semantic validators by shape, insert casts or color conversions, or introduce dynamic per-Run output lengths. Explicit association conversions and ops-domain configuration remain valid computational choices.

## 5. Consequences

Invalid schemas, parameters and missing resources fail before payload callbacks. Runtime publication must match the prepared schema and tile scope. Spatial tiles cover all non-H/W cell axes independently of atomic closure. Changing structure, relevant projections, stage definitions, validators or numerical implementation changes the corresponding identity. Numeric rounding, finite checks and reduction order remain operation contracts; independent mathematical oracles do not change to accommodate implementation.
