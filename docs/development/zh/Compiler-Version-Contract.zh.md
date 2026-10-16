# 编译器与包版本契约

## 当前契约

Package 0.35.0 使用 SameMinorVersion 兼容规则。C++ consumer 和外部 C operation module 必须针对匹配头文件重建。Result ABI 5 要求 table/record 大小精确匹配，不提供兼容 shim。ADR 0022 的前缀协商冻结 ABI 仍为 Proposed。

| 契约 | 版本 |
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

这些版本轴相互独立。C++ 布局改变可要求重新构建，而不改变无关 native service ABI。当前权威定义来自公共头文件、CMake package 声明和 compiler 规范编码。

## Workflow 输入与算子执行

Workflow 声明要求静态 Result schema，binding 按精确名称提供拥有型 ResultRef。Run 固定 plan、binding snapshot 和 generation。每个输出/成员在 plan-wide tile extent 下拥有网格，默认 128。Payload Need 使用成员 slot 和紧凑区间；descriptor-only 必须显式提出。空 payload 集合不执行工作。

Continuation 可以跨 poll 和多个 Need。准备后的共享 stage 在 Run 内按原始节点/stage 保证唯一 producer，不依赖输出选择。普通 actor 发布一个完整成员 tile。固定 bundle/prefix 可见性与执行分离。Mandatory root 和取消遵循执行契约。

## Result 与图像接口

Result 包含 1 到 16 个具名静态 Tensor 成员。Dtype、正 shape、rank、batch/atomic 轴和拓扑在执行前固定，包括静态推导的 extent。运行期 count 和 mask 是固定容量内的普通 payload。不提供动态 field 行、End 协议、数值端口 kind、failure-delivery selector 或内核 scalar-bounds 字段。

Tensor 统一 backing、片段和 view 访问。宿主默认 tiled planar 分配，继续支持合法的有符号/零 stride、外部 owner 和稀疏物理覆盖。拥有型窗口暴露真实 run；显式 collection 和 packing 消耗容量与 work。普通图像的 straight alpha 默认由算子实现和 helper 体现，不通过内核合规 validator 认证。

C metadata resolution 提供 schema、所选资源和静态共享 stage 声明。保留窗口具有显式 retain/release 生命周期。Query 和服务表在 callback 返回时失效。内核检查地址、授权、owner 与同步，不解释像素值。

## Identity 与运行期匹配

规范 identity 包含准备后的算子契约、静态 schema、参数、所选资源、输入投影、共享 stage 定义和相关 backend 选择。原始指针、padding 和 allocation 地址不定义语义 identity。C import 拒绝错误 ABI 或大小；native 服务可用性还取决于构建和设备。内部 IR 和 plan 不属于序列化 wire 格式。
