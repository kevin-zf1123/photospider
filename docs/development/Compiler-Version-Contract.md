# Compiler and Package Version Contract

## Current installed contract

The current kernel package is **0.30.0**. `DiskCacheConfig` and `DiskCacheStatistics` are removed. `ExecutionContextConfig` no longer carries disk-cache configuration; `ExecutionContext::clear_disk_cache()`, `flush_disk_cache()`, and `disk_cache_statistics()` are removed; and `OperationRegistry::persistent_cache_identity()` is removed. C++ consumers must rebuild against the 0.30 headers. The package uses `SameMinorVersion` compatibility, so 0.29 and 0.30 package requests are incompatible.

| Contract | Current version |
| --- | --- |
| Package | 0.30.0 |
| WorkflowDocument schema | 4 |
| OperationTraits | 21 |
| Numeric operation C table | ABI 11 |
| Structured Result operation C table | ABI 1 |
| Data provider C table | ABI 1 |
| Semantic graph identity | `semantic-graph-ir-v19` |
| Physical plan identity | `physical-plan-v19` |
| Outer plan cache key | `plan-cache-key-v15` |
| Optimizer identity | `optimizer-v5-canonical-noop` |
| Tensor-description codecs | TDM4/TDM5, selected by the current codec rules |
| Result schema canonical encoding | Version 2 |

These versions are independent compatibility axes. A C++ layout change requires rebuilding C++ consumers even when a C table version remains unchanged. The numeric C table remains ABI 11; Result modules use the independently versioned ABI 1 table. The removed planar C table has no compatibility adapter, and modules exporting its v1-v3 entry points are rejected. These removals change the public C++ API and `ExecutionContextConfig` layout. The WorkflowDocument schema, OperationTraits version, numeric operation C ABI, Result operation C ABI, data-provider C ABI, semantic/physical identity versions, and Result schema encoding remain unchanged.

## Result and image interface

A structured Result schema declares typed image slots and/or primitive fields. `PlanarImage` is storage backing inside a Result image slot. Ordinary non-image numeric Values keep the Value storage contract. Result image samples use bounded frame/layer axes, descriptor facts, planar storage order, and row pitch. Image bytes are not represented as packed primitive Result fields.

Result ABI 1 supports named outputs, selected input projections, pure metadata resolution through a synchronous sink, staged Result callbacks, typed image needs, dependency relations, and native services. `requested_kind` distinguishes a complete Result object (`0`), Value footprint (`1`), and image footprint (`2`). A Value or image footprint with zero Regions is Empty. The resolver calls `set_output` once per output; the host copies and validates nested schema records before that synchronous call returns.

## Identity and runtime matching

Semantic identity includes operation contracts, typed Result schema, semantic metadata, parameters, ordered inputs, selected output, and captured query where applicable. Physical plan identity includes physical choices that affect execution. Page offsets and transient addresses are not semantic identity. Internal IR and plans are not serialization formats.

The installed C++ headers, linked kernel library, and selected C table must describe the same package and ABI. The loader validates the exact C table version and size before importing a module. Native GPU behavior additionally requires the selected backend and an available device. The kernel package does not claim that every registered production image operation implements the Result image contract; see [Image operations](../kernel-architecture/Image-Operations.md).
