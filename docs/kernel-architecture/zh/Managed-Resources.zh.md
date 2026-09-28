# 受控资源与必需临时存储

英文权威文档：[Managed resources](../Managed-Resources.md)。

`ExecutionContextConfig::managed_resources` 为现有受控 buffer reservation 和显式
临时存储启用同一个 `ResourceBudget`。`maximum_live_bytes` 保留为 payload 子限额。
`ExecutionContext::resource_budget()` 返回可共享根；它独立于 completed cache，
结果 lease 可以超过 context 的寿命。

## 容量与工作

容量向量分别限制 host、device、shared、metadata、referenced input、临时 disk、
entries、files、I/O slots、queue 和 Payload。Payload 统计受控 buffer 字节，
在 execution context 中受 maximum_live_bytes 限制，含 structured callback。host 包含 metadata/shared，device 包含 shared；
不能把重叠维度相加作为物理内存。整向量在 admission 前检查。lease 副本共享一个
owner；增长必须计算旧新容量共存，只有存储释放或尚未提交的预留取消后才能缩减。
cleanup 保护额度不能用于普通阶段。admission 不等待其他持有者，容量不足返回 ResourceExhausted / CapacityLimit。

lease 对象容量自动计费，buffer、file 和 window owner 申报其 C++ 对象容量。
固定 root/device 启动状态、线程栈、驱动状态和 OS page cache 在此模型之外。
标准库、Objective-C 和驱动私有分配仍在模型之外，除非由显式 managed allocator
持有。ResourceAllocator 在分配前准入申请块和显式对齐 header；取消 state/control storage 及展平 source 列表使用该
allocator。返回的 allocator-aware diagnostics 独立于 payload 持有容量。
ResourceAllocationKind::Payload 将STL计算数据元素同时计入Payload，显式header仍为
Metadata；复制、rebind、active-scope复制均保留该角色。不能据此认证进程 RSS 或原生
设备不透明分配器开销。live 表示仍持有的已批准容量，含未使用 reservation；peak 是该
计数的实测峰值，不是完整输入类别的证明上界。保证限定为此模型的 WithinBudgetOrFail。

`reference(storage)` 按完整 caller allocation capacity 计入 Referenced 子限额，
同根按实际 owner 去重。返回 alias 保留 CpuStorage 地址，并让下游 view 保留引用
lease。启用根预算的 execute bindings 使用此机制；不推测 source callback 私有状态。
并发首次引用与最后引用释放采用串行事务，同一活跃 owner 不重复申请容量。

`consume(ResourceWork)` 原子预扣 work、bytes、requests 和 stages。工作或 I/O
超限返回 WorkLimit；仅 stages 超限返回 StageLimit；两者错误码均为
ResourceExhausted，失败不改变 issued 计数。已提交的工作
不因失败、fallback 或取消退款。singleton/joint dependency session 在执行前向当前
Run 根收费，包括 start 失败和 GPU discovery normalization。FootprintLimits 可携带
借用的根收费 callback，它不保存到不可变 Footprint 或语义身份。
每个入队的 source、Whole/backend attempt、singleton/joint dependency 和 structured
callback 均在提交前预扣一个根 stage，根计数跨 Run 累计。Queue 统计等待 worker 的
callback，在 callback 入口前释放；其封装 metadata 持续计费到 callback 退出。
这些限制在关闭 cache 时同样生效。

## 原生 GPU metadata

`ExecutionContext` 先创建 `MemoryBudget`，再创建可选原生 device，并将同一个显式
`ResourceBudget` root 传给 device。未设置 `managed_resources` 时，device 使用 null
root 和普通 allocator；它不会继承调用线程的 thread-local allocation scope。`Invocation`
也显式接收 device metadata account，因为它在进入 callback allocation scope 前构造。

启用 root 后，原生 GPU 动态分配通过 `ResourceAllocator` 计费：pipeline key 与 map
node、native-buffer owner、allocation-address lookup node，以及 invocation view
vector 已分配的容量和临时 SPIR-V module word 存储。Vulkan pipeline wrapper 和
control block 也使用该 allocator；
Metal pipeline 对象属于 Objective-C 对象，其内部存储仍不透明。allocator 计入申请块字节、对齐
header 和一个 Entries 槽；`ResourceLease` 自身的 managed overhead 另行计入。native
buffer 存储按已准入的实际容量独立收费。Host、Metadata、Shared、Device 和 Payload
是相互重叠的预算维度，不能将它们当作独立物理分配相加。

原生 pipeline cache 最多保留 64 项。已提交 batch 使用固定 32 个 command 槽持有
pipeline 引用，每个 command 最多有 31 个 storage binding。Invocation 自己最多保留
1024 个 view token。allocation-address map 持有的是弱 `CpuStorage` 引用，不拥有
native buffer。这些上限限制对应结构；线程栈与固定 bootstrap 对象仍在 managed
metadata 计数之外。Metal 和 Vulkan 串行执行 device queue 操作，并使用独立 cache
mutex 保护 pipeline map。清空 cache 会释放计费的 key 和 map node。Vulkan wrapper
metadata 在 batch pin 持有期间继续计费；Metal native pipeline 对象是不透明资源，
其原生寿命不属于 managed metadata 计费。

managed native metadata 分配有一次有界恢复重试。首次尝试使用 failure sink 为 null
的嵌套 resource scope。失败后 device 清理原生 pipeline cache，并在调用方原始 scope
中重试一次；最终分配失败会保留该 scope 的 sticky failure。metadata reclaim 只清理
原生 pipeline cache。GPU payload admission 使用独立路径：先释放可回收的 pending
disk 和 result-cache owner，必要时清理原生 pipeline cache，再执行 root 原子预留。
unmanaged device 直接使用显式 null root，不执行 managed cache-clear 重试。


## 临时 backing

TemporaryStorage 使用私有无缓冲临时文件和算术字节寻址，encoded extent 分别向
4096 字节取整。disk 统计 encoded file capacity，不代表 filesystem blocks 或物理
设备 I/O。无每页驻留目录、mmap 或 optional-cache eviction 依赖。read 显式检查范围
与窗口上限，不触发 producer；不可变 owning window 同时保留 backing 与根。

append 在写入前预留增长量；失败恢复旧 allocation end，回滚无法核验时保留 quarantine
额度。成功关闭文件可结算 quarantine；关闭失败继续计账。单调 frozen prefix 不可覆盖，
seal 禁止继续生产。prefix finality 和跨字段 association 属于结果发布者。取消阻止新
I/O，已提交同步 I/O 返回后才释放 owner；cleanup 不需要新窗口。

## 验证入口

英文页面列出 focused build/CTest 命令。test_resources 使用 16384 字节受控 host
限额处理 65536 字节临时 payload，检查独立计数、alias 寿命、并发 admission、输入
owner 去重、取消和有界失败。test_dependency_program 的 Need/上游/恢复例中，根
10000 只允许上游 6000 算法单位，根 30000 允许两个各 6000 的阶段；协议另计工作。
安装 consumer 目标 photospider_resource_consumer 仅通过已安装的 0.14 公共包运行。
test_managed_dispatch 检查 Whole、source、dependency 与 atom 执行的零/累计 stage、
零/单 Queue 槽，以及 structured source 返回错误或抛异常时的实际输入归属。

## 当前 staged metadata 与 view 边界

分阶段 dependency certificate 和 `NeedBatch` metadata 在各自公开边界准入并复制。copy 会
重新准入 metadata 容量并拥有新的 metadata owner，不复制源 owner。宿主在接受最终的可变
batch vectors 前调用 private `reseal_metadata()` 重新封存。Dependency session 及 callback
优先使用当前 TLS resource root；没有 active TLS 时恢复 session start 保存的 root，包括
跨 scope 的 work。

区域布局算子中，`regional_atomic` 将原始 query 及 normalized 请求矩形集合传给 callback。
每个逻辑样本仍是 Atomic observation；矩形集合不是一个 Atomic observation。设置
`preserve_output_views` 时，合法 affine view 的 payload admission 通过现有 nonblocking
reserve 和 cache-reclaim 路径按实际新分配字节计算，保留的 source owner 另行计费。
`ValueFragments` 可携带已拥有 metadata 的 publication
lifetime token；每个已发布 `Value` 保留 immutable storage alias，直到最后 owner 释放。
布局算子设置 `cacheable=false`，因为 content cache entry 不编码物理 owner/stride 分区；
pure 与 active-Run sharing 使用独立 lifetime。

计费边界保持明确：调用方取出裸 `Value` 或返回 raw vector 后自行复制时，不计入 publication
token。Empty 容器和当前实现内部 geometry 工作尚未全面计费。因此 `WithinBudgetOrFail`
只适用于已声明的 capacity model，不证明整个进程 RSS 都受到控制。

此边界也包括 `ExecutionRun` 和 structured execution 的宿主容器重建：这些路径
提取已发布 Value，再创建 `ValueFragments`，未转移原容器 token。每个 Value
仍保留 source 与 publication owner；重建的外层 vector、coverage、descriptor
存储属于既有容器 metadata，可能在最后一个 Value 释放 owner 后才退役。
