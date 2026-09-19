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
C++ 与 C 分阶段程序通过既有 Run 和 allocator 执行，支持精确 fragments、不可变
依赖记录、独立共享观察和内容结果复用。原生 fragment atlas、有界 GPU discovery
及 CPU fallback 使用同一协议。已实现契约和可执行验收案例见
[依赖数据与执行](../../kernel-architecture/zh/Dependency-Data.zh.md)及
[G4 workflows](../../../examples/g4_workflow/README.zh.md)。

## 独立结果契约

#302 分支依 ADR 0021 以包 0.9.0、operation ABI/traits 9 为目标。M1（#304）引入有序
命名输出契约、所选输出的 C/C++ 调用以及全部结果的静态 metadata 推导。自有单输出
调用者显式配置 outputs[0]；ABI 8 在访问 API table 前拒绝。C v9 descriptor 的有界
内联表包含 1..64 个有效输出记录。semantic/physical-plan/plan-cache 域为 v9，
result-region key 为 v5；完整输出声明、输入投影和检查过的 extent 算术进入身份。
schema 2、provider ABI 1、C++17、image v2、保守 optimizer v5、result digest v2 保持。
C++ 消费者须重编译，0.8 包消费者被拒绝。多输出规划、执行身份及 Atomic 联合执行
由后续独立叶项验收，M1 不宣称这些运行时条件已完成。

## 阶段 A 受控资源基础

package 0.10.0 改变 C++ execution config 与 host work service 签名。C++ 消费者需
重新构建，0.9 包请求被拒绝。本资源基础保留 operation ABI/traits 9、document schema 2
及既有 compiler identity domain；资源限额不进入语义身份。Result/schema ABI 变更由
阶段 A #316 单独跟踪。见[受控资源](../../kernel-architecture/zh/Managed-Resources.zh.md)。

## Phase A 结构化 C++ 契约

#316 在 package 0.10.0 中加入 OperationTraits 10、dependency protocol 2、编译器
可见的 result schema 和 owning paged ResultRef。semantic/physical-plan/plan-cache
域升级为 v10，result-region identity 升级为 v6；规范 schema 和 Result port
约束进入这些身份。资源限额和页大小仍是物理/准入选择。optimizer v5、
WorkflowDocument schema 2、provider ABI 1、result digest v2 保持不变。

C operation DSO 布局和 v9 入口不变。加载 ABI 9 Value/dependency plugin 时，
宿主在内部构造当前 C++ traits。structured callback 当前通过安装的 C++ API
注册，没有 structured C descriptor 表或兼容 shim。各版本轴独立：C++ consumer
必须为 0.10 重编译，布局不变的 ABI 9 C DSO 仍可加载。参见
[全局结果](../../kernel-architecture/Global-Results.md)。

#318 在同一 0.10 C++ package 增加闭集 Layer schema version 1、可注册的 CPU Layer
operation factory、逐原语严格舍入与 `Status.reason`。Status 布局改变，C++ consumer
须重新构建；C ABI 9 仍投影既有 error code，不增加 C++ Status 字段。具名 working-space
和算术定义由 schema version 与 operation identity 固定。参见
[Layer 运行时](../../kernel-architecture/zh/Layer-Runtime.zh.md)。

#319 在本轮 0.10 C++ 包中增加完整 `Status.detail`、基于坐标 `AtomKey`
的 joint contract 2、`execute_atoms` 和不可伪造枚举的 `QualityReport`。
C ABI 9 保留按不同输出分组的 joint contract 1 和原错误码投影。C++ 回调结果与
supply 签名使用 AtomKey，消费方重新构建，不提供 output-index shim。
joint contract 2 使用既有 v10 canonical framing 中的新 trait 值；contract 1
语义和 C 布局保持原样。见[原子错误与质量](../../kernel-architecture/zh/Atom-Errors-and-Quality.zh.md)。

## 数值元组与诊断契约

Package 0.13.0 使用 C++ OperationTraits 13、通用尾轴观察分组、数值 axis dtype
推导和宿主持有的 CPU 数值诊断。输出分组、regional execution 与 view traits 进入 operation identity，因此 semantic、
physical-plan、plan-cache 域使用 v13。Dependency protocol 2、joint contract 2、
result-region v6、WorkflowDocument schema 2 和 C operation ABI 9 保持不变。
C++ 消费方须针对 0.13 重编译；旧 minor package 请求被拒绝。C ABI loader 拒绝既有
v9 枚举之外的 dtype rule；其布局不增加 C++ 分组、新 dtype rule 或诊断回调。

内核 C/C++ 构建要求 Clang，包含 Apple Clang；Ubuntu WSL 正确性验证也使用
Clang。[数值 workflow](../../../examples/numeric_workflow/README.md) 通过安装后的
公开 API 手动运行，不新增集成测试注册。

## NUM-09 布局实现

Package 版本为 0.13.0，C++ `OperationTraits` 版本为 13。operation
plugin ABI 保持 C ABI 9，descriptor 与 entrypoint 布局不变。九个
`array.reshape`、`array.transpose` 和 `array.slice` key 使用现有 C++ traits 与
dependency protocol，不增加 C ABI 字段、兼容别名或 shim。C++ installed consumer
须针对 0.13 package 重新构建。

布局 traits 携带每节点 shape/permutation/count metadata，并在适用时携带
regional atomic execution 和 `preserve_output_views`。Static dependency pieces 替代
之前的 static dependency maps；每个 piece 携带不相交的 observation coverage 与完整的
各端口 dependency。这些字段进入 C++ operation identity。布局算子设置 `cacheable=false`，因为当前 content cache 不记录物理 owner/
stride 分区；pure 与 active-Run sharing 仍独立。

静态映射 identity 包含每个互不相交 piece 的完整 observation coverage 和每个
`DependencyAxis::translation`。translation 是有符号 Int64；创建时使用加宽整数
算术逐 piece 检查平移后的范围。C++ 字段为 `static_dependency_pieces`，不提供
旧字段别名。repeated input template 只有在存在 metadata specializer、由其验证
描述符关系时才能设置 `repeated_match=false`；concatenate 用它检查非拼接轴维度。

## NUM-12 纯 block sharing

Package 0.14.0 使用 C++ `OperationTraits` 版本 14，semantic、physical-plan 和
plan-cache domain 使用 v14；C operation ABI 保持 C ABI 9。Opt-in
`share_blocks_across_outputs` 默认 false，仅适用于 pure Atomic dependency-v1。
其 common block identity 包含完整 resolved output contracts、static parameters、
input metadata 与 supplied bytes、incoming state、phase/range 和 mode。公开 output
与 certificate identity 仍独立。可选 result-LRU reuse 要求正数
`result_cache_bytes` 与已计费 proof budget；miss 会重算，不能合并并发 producer，也不
承诺每个 Run 只求值一次。C++ layout 改变要求 consumer 使用 0.14 重新构建，拒绝
旧 minor 请求。WorkflowDocument schema 2、provider ABI 1、optimizer v5 与
result-region v6 保持不变。
