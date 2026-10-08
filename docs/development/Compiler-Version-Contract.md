# Compiler and Package Version Contract

## Current contract

The current package version is **0.32.0**. C++ consumers must rebuild against the package headers. The package uses `SameMinorVersion` compatibility, so requests for a different minor version are incompatible. The C operation-plugin interface remains Result ABI 2; its exact table and record sizes must match the installed SDK.

| Contract | Current version |
| --- | --- |
| Package | 0.32.0 |
| WorkflowDocument schema | 5 |
| OperationTraits | 24 |
| Operation plugin C table | Result ABI 2 |
| Data provider C table | ABI 1 |
| Semantic graph identity | `semantic-graph-ir-v19` |
| Physical plan identity | `physical-plan-v19` |
| Outer plan cache key | `plan-cache-key-v15` |
| Optimizer identity | `optimizer-v5-canonical-noop` |
| Tensor-description codecs | TDM4/TDM5, selected by the current codec rules |
| Result schema canonical encoding | Version 3 |

These versions describe independent compatibility axes. A C++ layout change requires rebuilding C++ consumers even when a C table version remains unchanged. The package update and the Result-only C++ operation and workflow APIs are breaking C++ changes. The Result C table remains ABI 2 and is the sole operation-plugin C interface. ABI 2 includes tensor-window, tensor-view, mapping, reshape, prefix, neighborhood, and Cartesian-relation services, plus immutable classification fields on named `ps_result_output_v2` records. `observation_kind` accepts `PS_RESULT_ATOMIC_V2` (0) or `PS_RESULT_REQUEST_RECORD_V2` (1). `failure_delivery` is `PS_RESULT_REQUEST_FAILURE_ONLY_V2` (0), or `PS_RESULT_PER_ATOM_OUTCOME_V2` (1) for a contract-2 joint program. The importer requires exact current record sizes; C modules must rebuild against the SDK.

## Workflow inputs and operation execution

`WorkflowInputDeclaration` contains an id, exact binding name, and required `result_schema`. `ExecutionBinding` contains a name and owning `ResultRef`. `ExecutionResult` and `DemandResult` expose named Results; Value is internal typed backing used by Result storage and codecs. Input `OperationMetadata` carries the Result schema while its Value descriptor and facets stay empty. Tensor shape, layout, and semantic facets belong to each `ResultTensorSpec`.

`OperationTraits` version 24 and `WorkflowDocument` schema 5 identify the current C++ contracts. An operation executes through `start_result` and its `ResultContinuation`; joint Result callbacks remain an optional grouped execution contract. `ExecutionContext::execute` returns named Results. Incremental publication uses `ExecutionOptions::result_publication`, which receives certified prefixes as they become available and may retain the owning Result reference. A prefix is not complete execution success. Callback failure stops the Run and follows the existing failure, cancellation, and callback-retirement rules.

## Result and image interface

A Result schema declares typed tensor members, primitive fields, domain, and metadata. Tensor members carry element type, descriptor cell shape, optional batch axes, semantic facets, and physical layout; complete sample coordinates are batch axes followed by cell axes. A spatial tensor's height, width, and channel axes are cell-relative. Physical storage order and row pitch describe backing; tensor payload remains distinct from primitive field records. Planar image pixels use typed Result tensor slots backed by planar pages.

A Result schema can contain tensor slots, fields, or both. `need_result` requests Result descriptor and field facts; `need_tensor` requests sample coverage. `read_tensor` copies authorized samples, while owning tensor windows expose authorized rows or rectangles and retain backing owners until release. `publish_tensor_view` publishes an authorized source view. `make_mapping` maps output tensor support to an input tensor; other relation helpers construct tensor support witnesses. Relations do not authorize sample reads.

The resolver supplies each output schema once through its synchronous metadata sink, and the host copies and validates nested schema records before the sink call returns. The query view reflects the selected output declaration; metadata resolution cannot change its observation or failure classification. A RequestRecord output executes once for the selected captured query Q and must publish one complete Result with exact tensor coverage Q, even when its execution region is Whole. The host rejects that terminal Result as a later operation input, tensor-view source, checkpoint state, or block state. An Empty tensor Q skips the callback only for a RequestRecord output with no fields; Atomic Empty outputs and terminal outputs with fields still execute. Retained tensor and window handles have explicit release or operation-state destruction lifetimes. CPU workers may poll cancellation; retained window row/rectangle access is worker-safe and does not reenter execution services.

## Identity and runtime matching

Semantic identity includes operation contracts, typed Result schema, semantic metadata, parameters, ordered inputs, selected output, and captured query where applicable. Physical plan identity includes physical choices that affect execution. Page offsets and transient addresses are not semantic identity. Internal IR and plans are not serialization formats.

The installed C++ headers, linked kernel library, and selected C table must describe the same package and ABI. The loader validates exact C table version and size before importing a module. Native GPU behavior additionally requires the selected backend and an available device. Package 0.32.0 defines the current C++ interface contract. Migration of retained operations, examples, and integration consumers remains incomplete. A version declaration alone is not evidence of local installation or external consumer validation.
