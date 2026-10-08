# Compiler 与 Package 版本契约

## 当前契约

当前 package version 为 **0.32.0**。C++ consumers 必须针对该 package 的 headers 重建。Package 使用 `SameMinorVersion` 兼容规则，因此不同 minor version 的请求不兼容。C operation-plugin interface 保持 Result ABI 2；table 和 record 的精确大小必须与已安装 SDK 一致。

| 契约 | 当前版本 |
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
| Tensor-description codecs | 按当前 codec 规则选择 TDM4/TDM5 |
| Result schema canonical encoding | Version 3 |

这些版本属于独立兼容轴。即使 C table 版本不变，C++ layout 变化仍要求 C++ consumers 重建。Package 更新以及 Result-only C++ operation 和 workflow API 都属于 breaking C++ changes。Result C table 保持 ABI 2，且是唯一的 operation-plugin C interface。ABI 2 包含 tensor-window、tensor-view、mapping、reshape、prefix、neighborhood 和 Cartesian-relation services，并在具名 `ps_result_output_v2` records 中包含不可变分类字段。`observation_kind` 接受 `PS_RESULT_ATOMIC_V2`（0）或 `PS_RESULT_REQUEST_RECORD_V2`（1）。`failure_delivery` 为 `PS_RESULT_REQUEST_FAILURE_ONLY_V2`（0）；contract-2 joint program 使用 `PS_RESULT_PER_ATOM_OUTCOME_V2`（1）。Importer 要求 record 大小与当前定义精确匹配；C modules 必须针对 SDK 重建。

## Workflow 输入与算子执行

`WorkflowInputDeclaration` 由 id、精确 binding name 和必需的 `result_schema` 组成。`ExecutionBinding` 由 name 和 owning `ResultRef` 组成。`ExecutionResult` 与 `DemandResult` 提供具名 Results；Value 是 Result storage 和 codecs 使用的内部 typed backing。输入 `OperationMetadata` 携带 Result schema，其 Value descriptor 和 facets 保持为空。Tensor shape、layout 和 semantic facets 属于各自的 `ResultTensorSpec`。

`OperationTraits` version 24 与 `WorkflowDocument` schema 5 标识当前 C++ 契约。算子通过 `start_result` 和其 `ResultContinuation` 执行；joint Result callback 仍是可选的分组执行契约。`ExecutionContext::execute` 返回具名 Results。增量 publication 使用 `ExecutionOptions::result_publication`，在前缀获得认证后逐步通知调用方，调用方可以保留 owning Result reference。前缀不代表整次执行成功。Callback failure 会停止 Run，并遵循现有 failure、cancellation 和 callback retirement 规则。

## Result 与图像接口

Result schema 声明 typed tensor members、primitive fields、domain 和 metadata。Tensor members 携带 element type、descriptor cell shape、可选 batch axes、semantic facets 和 physical layout；完整 sample coordinates 由 batch axes 后接 cell axes 构成。Spatial tensor 的 height、width 和 channel axes 相对于 cell axes。Physical storage order 与 row pitch 描述 backing；tensor payload 与 primitive field records 分开。Planar image pixels 使用由 planar pages 支撑的 typed Result tensor slots。

Result schema 可以包含 tensor slots、fields 或两者。`need_result` 请求 Result descriptor 和 field facts；`need_tensor` 请求 sample coverage。`read_tensor` 复制已授权 samples；owning tensor windows 提供授权的 rows 或 rectangles，并在释放前保留 backing owners。`publish_tensor_view` 发布授权的 source view。`make_mapping` 将 output tensor support 映射至 input tensor；其他 relation helpers 构造 tensor support witnesses。Relations 不授权 sample reads。

Resolver 通过同步 metadata sink 对每个 output 提供一次 schema；host 在 sink 调用返回前复制并校验嵌套 schema records。Query view 反映选中的 output 声明，metadata resolution 不能更改 observation 或 failure classification。RequestRecord output 对选中的 captured query Q 执行一次，并必须发布 tensor coverage 精确为 Q 的完整 Result，即使 execution region 为 Whole 也如此。Host 会拒绝将该 terminal Result 用作后续 operation input、tensor-view source、checkpoint state 或 block state。只有无 fields 的 RequestRecord output 才会在 Empty tensor Q 时跳过 callback；Atomic Empty output 和含 fields 的 terminal output 仍会执行。Retained tensor 和 window handles 具有显式释放或 operation-state 销毁生命周期。CPU workers 可检查取消；retained window 的 row/rectangle access 可安全地由 worker 执行，且不会重新进入 execution services。

## Identity 与 runtime 匹配

Semantic identity 包含 operation contracts、typed Result schema、semantic metadata、parameters、ordered inputs，以及适用时的 selected output 和 captured query。Physical plan identity 包含影响执行的物理选择。Page offsets 和临时 addresses 不属于 semantic identity。内部 IR 和 plans 不是 serialization formats。

已安装的 C++ headers、链接的 kernel library 和所选 C table 必须描述相同的 package 与 ABI。Loader 在导入 module 前验证精确的 C table 版本和大小。Native GPU 行为还要求选定 backend 和可用 device。Package 0.32.0 定义当前的 C++ interface 契约；保留 operation、examples 与 integration consumers 的迁移尚未全部完成。版本声明本身不能证明本地安装或 external consumer validation 已完成。
