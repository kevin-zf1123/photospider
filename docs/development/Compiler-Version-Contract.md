# Compiler and Package Version Contract

## Current contract

The current package version is **0.34.0**. C++ consumers must rebuild against the package headers. The package uses `SameMinorVersion` compatibility, so requests for a different minor version are incompatible. The C operation-plugin interface remains Result ABI 4; its exact table and record sizes must match the installed SDK.

| Contract | Current version |
| --- | --- |
| Package | 0.34.0 |
| WorkflowDocument schema | 6 |
| OperationTraits | 26 |
| Operation plugin C table | Result ABI 4 |
| Semantic graph identity | `semantic-graph-ir-v21` |
| Physical plan identity | `physical-plan-v21` |
| Outer plan cache key | `plan-cache-key-v17` |
| Optimizer identity | `optimizer-v5-canonical-noop` |
| Tensor-description codecs | TDM4/TDM5, selected by the current codec rules |
| Result schema canonical encoding | Version 4 |

These versions describe independent compatibility axes. A C++ layout change requires rebuilding C++ consumers even when a C table version remains unchanged. The package update and the Result-only C++ operation and workflow APIs are breaking C++ changes. The Result C table remains ABI 4 and is the sole operation-plugin C interface. ABI 4 includes tensor-window, tensor-view, reshape services, plus immutable classification fields on named `ps_result_output_v4` records. `failure_delivery` is `PS_RESULT_REQUEST_FAILURE_ONLY_V4` (0). The importer requires exact current record sizes; C modules must rebuild against the SDK.

## Workflow inputs and operation execution

`WorkflowInputDeclaration` contains an id, exact binding name, and required `result_schema`. `ExecutionBinding` contains a name and owning `ResultRef`. `ExecutionResult` and `DemandResult` expose named Results; Value is internal typed backing used by Result storage and codecs. Input `OperationMetadata` carries the Result schema. Tensor shape and layout belong to each `ResultTensorSpec`.

`OperationTraits` version 26 and `WorkflowDocument` schema 6 identify the current C++ contracts. An operation executes through `start_result` and its `ResultContinuation`. `ExecutionContext::execute` returns named Results. Incremental publication uses `ExecutionOptions::result_publication`, which receives certified prefixes as they become available and may retain the owning Result reference. A prefix is not complete execution success. Callback failure stops the Run and follows the existing failure, cancellation, and callback-retirement rules.

## Result and image interface

A Result schema declares typed tensor members, primitive fields and domain. Tensor members carry element type, descriptor cell shape, optional batch axes and physical layout; complete sample coordinates are batch axes followed by cell axes. A spatial tensor's height, width, and channel axes are cell-relative. Physical storage order and row pitch describe backing; tensor payload remains distinct from primitive field records. Planar image pixels use typed Result tensor slots backed by planar pages.

A Result schema can contain tensor slots, fields, or both. `read_tensor` copies authorized samples, while owning tensor windows expose authorized rows or rectangles and retain backing owners until release. `publish_tensor_view` publishes an authorized source view. `make_mapping` maps output tensor support to an input tensor; other relation helpers construct tensor support witnesses. Relations do not authorize sample reads.

The resolver supplies each output schema once through its synchronous metadata sink, and the host copies and validates nested schema records before the sink call returns. The query view reflects the selected output declaration; metadata resolution cannot change its failure classification. Retained tensor and window handles have explicit release or operation-state destruction lifetimes. CPU workers may poll cancellation; retained window row/rectangle access is worker-safe and does not reenter execution services.

## Identity and runtime matching

Semantic identity includes operation contracts, typed Result schema, parameters, ordered inputs, selected output, and captured query where applicable. Physical plan identity includes physical choices that affect execution. Page offsets and transient addresses are not semantic identity. Internal IR and plans are not serialization formats.

The installed C++ headers, linked kernel library, and selected C table must describe the same package and ABI. The loader validates exact C table version and size before importing a module. Native GPU behavior additionally requires the selected backend and an available device. Package 0.34.0 defines the current C++ interface contract. C++ layout and field removals are breaking changes; rebuild every C++ consumer against the installed 0.34.0 package. The package version file accepts compatible releases in the 0.34 minor line.
