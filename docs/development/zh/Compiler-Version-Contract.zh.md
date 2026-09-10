# Compiler 版本契约

公开 package、`WorkflowDocument`、operation-trait schema、semantic IR、optimizer
rule set、physical planner 与 daemon IPC 是独立 version axis。ADR 0014 定义其
identity 分离；ADR 0015 定义产品边界。

## 公开兼容性

Photospider 处于 0.x 开发。Minor release 可以做明确 breaking public API/package
change。每个 installed-boundary change 必须说明影响，并通过隔离
`find_package(Photospider)` consumer。

内部 semantic/optimized/plan representation 不是公开 serialization format。
Package 不承诺解码或执行其他 build 的内部 IR。Daemon 绝不把内部 IR 放上 local
IPC。

## Digest

`SemanticGraphDigest`、`OptimizedGraphDigest`、`ExecutionPlanDigest` 与
`PlanCacheKey` 使用 canonical domain-separated input。它们排除 runtime allocation、
timing、cancellation、ready-queue state 与 daemon identity；是非安全
reproducibility/cache identity，不是 signature、attestation、durable object id 或
receipt。Operation-v4 parameter schema 与已验证 value 影响 semantic identity。Float64
parameter 会按 fixed little-endian order 编码 copied IEEE-754 binary64 的精确 bit，因此
`+0.0` 与 `-0.0` 具有不同的 semantic、optimized、plan 与 cache-key identity。不会执行
NaN-payload、infinity 或 signed-zero normalization，这份 digest contract 也不增加
finite-only validation。plan-derived output/input Region 影响 physical plan identity。

## Cache 兼容性

`PlanCacheKey` 覆盖 domain-separated plan identity。如果 embedding 创建 derived
compiler cache，它必须在复用前验证 schema、stage identity、operation trait 与
target capability。任何 mismatch 都变成 cache miss 并重建；删除 cache 始终有效。

## Change checklist

- 只更新受影响的 public 或 internal version。
- Canonical byte 有意变化时更新 canonical digest vector。
- 更新受影响的英文公开文档与中文镜像。
- 更新 live GitHub Issue/Project state 与 checked-in delivery snapshot。
- 运行 focused stage validation 与隔离 installed consumer。
- 除非独立显式产品决策要求，否则不增加 compatibility shim 或第二 reader。

## 已实现的 S1 版本

#257 实现 [ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md)：
package 0.3.0、WorkflowDocument schema 2、OperationTraits 3 和 operation ABI 3。
Provider ABI 保持 1，增加 Float32 element code 4。C++ consumer 必须重新构建；
schema 1 和 operation ABI 2 被拒绝，无兼容适配器。
`execute(plan, token, options)` 迁移为 `execute(plan, {}, token, options)`。
SameMinorVersion 拒绝 0.2 package consumer 消费 0.3。

Identity domain 为 `semantic-graph-ir-v3`、`optimizer-v3-canonical-noop`、
`physical-plan-v3` 和 `plan-cache-key-v3`。Canonical declaration 编码 id、name、
element、shape、Region、layout 和按 key 排序的 facet key/version/payload。
Ordered source 使用 tag 1 表示 node/step、2 表示 declaration。Port kind 与 binary32
interval endpoint bit 进入 traits；整数采用 uint64 little-endian 编码。
Payload byte 和 binding order 不进入 stage identity，也未引入 runtime result cache。

仍要求 C++17。静态/共享 installed consumer 通过 C++ 和 C plugin 路径执行具名图像
oracle，并检查旧 minor version 拒绝。Daemon feature/wire 工作独立，其 0.2 package
consumer 需要协调迁移后才能消费 0.3。状态写入遵循[任务协作](Task-Collaboration.zh.md)。

## S2 存储/ABI 基础

#264 实现 package 0.4.0、OperationTraits 4、operation ABI 4、区域存储视图和宿主输出/scratch。
保留 schema 2/provider ABI 1/C++17；C++ 消费者重新构建，拒绝 ABI 3 与 package 0.3 消费者。
semantic/optimizer/physical-plan/cache domain 使用 v4。运行期 origin 地址不进入 semantic
identity。#210/#211/#265 已实现按完成释放、惰性 tile、区域源和同步流式执行；
Gaussian/蒙版/合成场景由 ADR 0017 下的 #266 追踪。Result digest framing 为
photospider.result-digest.v2，包含显式 storage origin；流式 tile 借用后释放，不计算 result digest。

#211 在 v4 semantic framing 中增加静态数值边界（bound flag、精确 binary64 endpoint bits）
和带长度 halo 参数名。physical v4 包含 tile 高/宽及各排序输出名称的精确 Region；即使
合并的 producer demand 相同，不同区域的输出别名也具有不同 physical identity。

## S3 缩放契约

#270 实现 package 0.5.0、OperationTraits 5 和 operation ABI 5；C++17、schema 2、provider ABI 1 保持。拒绝 0.4 软件包消费者与 ABI 4，C++ 消费者重建。新增 [1,16] 整数 box 缩小形状/区域规则及蒙版输出；静态 factor 参数名和解析值进入 v5 编译身份。运行内容身份独立。参见 ADR 0018。

## S4 原生契约

package 0.6.0、operation ABI/traits 6 增加纯 C 宿主 GPU 服务、CPU 可访问原生存储与
显式 CpuExact/MetalFp32；拒绝 ABI 5 和 package 0.5。C++17、schema 2、provider ABI 1、
IPC v3 保持。semantic-graph-ir-v6、physical-plan-v6、plan-cache-key-v6 编码新 trait、
数值模式及显式访问。优化规则仍为 optimizer-v5-canonical-noop，摘要由新语义输入改变。
结果区域键 v2 区分数值/后端/设备/实现，上传键单独散列实际逻辑字节；句柄与耗时不进入
编译身份。参见 ADR 0019 与 S4 安装示例。

## 已接受的算子基础目标

[ADR 0020](../../adr/0020-composable-operation-foundations.md) 由 #287 追踪：
目标 package 0.7.0、operation ABI/OperationTraits 7，image facet v2 替换 v1，
semantic/physical-plan/plan-cache domain v7、result-region-key v3。优化规则保持
optimizer-v5-canonical-noop；result digest framing 保持 v2。磁盘派生数据为 v2，
旧 entry 作为 miss。WorkflowDocument schema 2、provider ABI 1、C++17 保持。
完整约束、输出 facets 和推断规则进入受影响 identity。拒绝旧 operation table/package
minor 请求，C++ consumer 重建。Daemon 0.6 迁移为独立任务。

这些是 `ops-foundations` 上仅交付到 `ops` 的已接受目标。决策基线 `main@fba06270`
仍为 package 0.6.0/ABI 6；本次文档修改不宣称已有 0.7 runtime 或实现已合入 main。

#289 的共享契约实现现已在 ops-foundations 提供上述 0.7.0/ABI7 接口，包括 canonical
typed facets 和完整约束/dense-output identity。这是本地分支实现；#290–#297 及验证后
交付到 ops 是独立完成条件。Main 保持决策基线，后续变更需要独立授权工作。

## G4 实现版本

G4 开发分支使用 package 0.8.0、operation ABI/OperationTraits 8、
semantic-graph-ir-v8、physical-plan-v8、plan-cache-key-v8 和 result-region-key-v4。
五个观察/失败/依赖字段及 EffectiveAtomic 进入适用的 identity。Optimizer-v5-canonical-noop、
result digest v2、disk framing v2、image facet v2、schema 2 与 provider ABI 1
保持原语义。Host 在读取 get_api_v8 前拒绝 version 7 DSO，不提供旧别名或 image-v1
reader。C++ consumer 需要以 0.8 重编。上面的 foundations 段落描述其历史实现边界。
C++ 分阶段协议及 CPU Run 集成已实现；C 分阶段表与共享结构结果集成仍属于本轮
G4 的进行中工作。
