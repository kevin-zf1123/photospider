# 当前实现与规格前置条件

当前 package 为 Photospider 0.35.0，operation plugin 使用 Result C ABI 5，OperationTraits 为 27。本文概括当前包内可核对的注册入口、数据类型和执行边界；单个算子的端口、参数、数值行为和 Region 规则以对应实现契约为准。公共 C++ 契约有破坏性变更，consumer 必须重建。

## 默认 registry 与类型边界

默认 registry 的注册入口位于[`plugins/ops/registry.cpp`](../../../plugins/ops/registry.cpp)，公开 operation C ABI 定义于[`result_operation_plugin_api.h`](../../../include/photospider/plugin/result_operation_plugin_api.h)。当前分类指南描述的是各族已实现子集；逐 key 的参数、dtype、shape、Region 与错误契约仍以对应专题为准。

存在 key 只说明 registry 接受该定义，不单独证明任意存储路径都可执行。当前 operation 契约通过 `OperationTraits` 的输入约束、backend 能力和有序 `OperationOutputTraits` 声明输入输出；输出的 `result_schema`、metadata resolver 与 `ResultTensorSpec` 定义 Result 类型和 tensor 描述。Result-only operation 按其注册的 callback、schema 和 backend 契约执行，没有独立的 planar-storage capability flag。实现入口见[`operation_registry.hpp`](../../../include/photospider/plugin/operation_registry.hpp)和[`operation_registry.cpp`](../../../src/lib/plugin/operation_registry.cpp)。

[`ElementType` in `tensor_span.hpp`](../../../include/photospider/data/tensor_span.hpp) 定义七种内建元素类型：`UInt8`、`Int8`、`UInt16`、`Int16`、`Int64`、`Float32` 和 `Float64`。descriptor 的 rank 为 1 到 8，各轴长度必须大于零。浮点 `Tensor` 可保留通用位模式；算子的数值契约可施加更窄的限制。

图像 operation 按各自显式静态参数、布局、dtype 和 Region 契约执行。Result/Tensor 只保存结构、数据和实际存储 owner；不附着或传播颜色等领域描述。`Tensor` 是统一数值 backing，任意合法通道数不触发 RGB/alpha 推断。领域校验由 SDK 显式调用或端口绑定校验器完成；schema id/version 不触发 Layer、LUT 或颜色分派。

`Photospider::ops` 提供内置算子注册与 authoring 实现，并依赖 `Photospider::ops_sdk`。SDK 通过 kernel 公共接口提供颜色配置、ICC/OCIO 资源、数值报告与领域数学；kernel 不依赖这些领域实现。

## 当前能力入口

| 能力 | 实现子集与调用入口 | 数据和执行边界 |
| --- | --- | --- |
| Numeric、expression 与 LUT | 默认 registry 的数值、curve、field、统计和 expression/LUT operation；从[基础算子](../../kernel-architecture/Basic-Operations.md)、[数值操作](../../kernel-architecture/Numeric-Operations.md)、[表达式与 LUT](../../kernel-architecture/Expression-and-LUT-Operations.md)查看 key 契约。 | 仅按具体 key 公开的 dtype、shape、参数和 complete-input/Region 规则执行；普通数值 Result 不因形状而获得额外图像领域约束。 |
| Channel 与 color | 默认 registry 中明确注册的通道构造、提取、颜色模型转换和 RGB 基底节点，见[通道与颜色操作](../../kernel-architecture/Channel-and-Color-Operations.md)。 | 各节点校验显式配置、dtype、结构与布局；ICC/OCIO 资源封装不等于完整颜色转换引擎。 |
| Image 与 mask | 内置图像 operation 通过 Result tensor storage 执行；当前清单与各 key 的输入约束见[图像操作](../../kernel-architecture/Image-Operations.md)。 | 图像样本使用 typed `photospider.image` Result；空间与非空间成员使用统一 Tensor backing。 |
| Layer 纯值计算 | `LayerPixel` 及纯计算 helper 位于 SDK 的 [`ops/composite/layer.hpp`](../../../include/photospider/ops/composite/layer.hpp)。 | 当前没有 Layer operation factory 或 Result schema helper；kernel 不按 Layer schema 名称分派或拒绝。调用者选择一致的线性空间；见 [Layer runtime](../../kernel-architecture/Layer-Runtime.md)。 |

## Result、资源与错误边界

Result 的全部成员为编译后固定的静态 Tensor。有效数量由普通 count/mask payload 表示，零有效数量不改变正 extent。每个公开 tile 完整且不可变。`CompleteBundle` 按准备后的固定范围统一公开，`IndependentChunks` 允许完整 tile 独立公开，`StablePrefix` 按成员局部 tile 顺序公开。共享 stage 在 Run 内复用完整计算，失败不公开当前事务。见 [Global Results](../../kernel-architecture/Global-Results.md)。

受管资源预算限制内核声明并计量的 managed capacity、work 和 stage，不构成进程 RSS 上限；调用方输入和未计量的系统/驱动分配不因此受限。见[Managed Resources](../../kernel-architecture/Managed-Resources.md)。普通 `execute` 对执行错误保持 fail-fast。见[Atom errors and quality](../../kernel-architecture/Atom-Errors-and-Quality.md)。

## 规格状态、边界与修订入口

规格的 `Proposed` 或 `Accepted` 状态描述目标契约状态，不表示实现已经注册。oracle 覆盖、operation 注册、Result storage 能力和 CPU/GPU backend 支持是不同事实，应分别按对应实现文档核对。

通用路径光栅、任意 solver、RAW、时域、Deep 和 ML 等能力不由现有基础设施自动提供；只有当前 registry、factory 或明确执行接口实现的部分才属于可调用能力。按[规格模板](spec-template.md)选择规格、实现说明或 workflow 的维护位置。
