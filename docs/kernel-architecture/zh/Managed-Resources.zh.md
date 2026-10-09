# 托管资源与临时存储

## 模块边界与职责 (Scope & Ownership)

每个 `ExecutionContext` 都创建供受控 CPU/GPU 工作共享的资源根。若提供 `ExecutionContextConfig::managed_resources`，其中的 limits 用于该 root；否则使用默认 `ResourceLimits`。`ExecutionContext::resource_budget()` 以 `Result` 返回已初始化的 budget。租约可在 context 销毁后继续存活。计量覆盖已申报的容量，不等于进程 RSS 上限；线程栈、驱动私有分配、操作系统页缓存和未使用托管分配器的内存不在范围内。

```cpp
#include <cstdint>

#include "photospider/core/resources.hpp"

namespace ps {
Result<ResourceLease> reserve_capacity(const ResourceBudget& budget,
                                       std::uint64_t bytes) {
  return budget.reserve(ResourceCapacity::host(bytes));
}
}
```

`CancellationToken`、`ResourceBudget`、`ResourceLease`、`ResourceAllocationScope` 和 `ResourceAllocator` 是 `photospider/core/` 下声明的 public facilities。Cancellation 是协作且单调的；`ResourceBudget::consume` 原子准入累计 work。失败或取消不会退还已发出的 work 与 I/O。资源计量覆盖受 instrument 的 capacity 和 work，不限制进程 RSS。

预算跟踪 Host、Device、Shared、Metadata、Referenced、Disk、Entries、Files、I/O slots、Queue 和 Payload。部分维度描述同一物理字节：Host 包含 Metadata 与 Shared，Device 包含 Shared，不能把重叠计数相加。context 的 Payload 另受 `maximum_live_bytes` 限制；调用方原有存储通过 Referenced 单独准入，不计入 Payload 子限额。

在内核内部，data 层拥有 Payload reservation：`src/lib/data/memory_budget.hpp` 中的内部类型 `MemoryBudget` 与 `MemoryReservation` 实现 Payload capacity 以及 storage 和 Result 保留的 reservation lease。借用 Run 或 context state 的准入保留在 execution 层。内部 helper `ScopedMemoryAdmission` 向 allocator 提供可复制的准入 callback，其 owner 在借用的 state 退休前关闭该 callback；payload lease 不保留它。`test_resource_internals` 直接测试该 helper。公共 resource 与 storage 头文件只前向声明这些类型，布局和签名保持不变。

## 核心数据结构与内存布局 (Data Layout & Memory)

`ResourceLease` 的副本共享同一 reservation，容量直到最后一个 lease owner 销毁后才释放。`grow` 原子地准入增量；只有关联存储释放后调用方才可 `shrink`。容量不足立即返回带 capacity-limit 状态的 `ResourceExhausted`，不会等待其他 owner 释放。

`ResourceAllocator` 在分配前准入请求块及对齐头，并把租约保存在分配头中，直到物理存储释放之后才销毁租约。Payload allocator 的 STL 元素计入 Payload，分配头计入 Metadata。`CpuStorage` 提供只读借用字节；底层 native owner 先于容量和 requested-byte 租约释放。

`BufferAllocator::limited()` 限制存活 backing capacity 总量，`limited_requested()` 限制存活请求字节总量。两类 scope 可以嵌套；native 转换保留 allocation provenance 与失败观察器，并按实际 native capacity 向 root 计费。`reference(storage)` 对资源 root 之外的 storage 按 live owner 去重，在 Referenced 维度准入完整 capacity。若 root 的 allocator 已拥有该 storage，函数直接返回它，不再准入 Referenced；原有 Payload 和 Host capacity lease 继续计账。下游 view 保留对应 owner 及其原计账 lease。

在调用方已持有 allocator 时，可组合两个独立限额：

```cpp
#include <cstdint>

#include "photospider/data/storage.hpp"

namespace ps {
void allocate_with_quotas(BufferAllocator allocator,
                          std::uint64_t maximum_capacity,
                          std::uint64_t maximum_requested,
                          std::uint64_t bytes) {
  auto scoped = allocator.limited(maximum_capacity);
  auto requested = scoped.limited_requested(maximum_requested);
  auto allocation = requested.allocate(bytes);
}
}
```

## 调度与状态机 (Execution & State)

`consume(ResourceWork)` 原子准入 work、byte、request 和 stage 计数。work 或 I/O 超限返回 `ResourceExhausted` 和 `WorkLimit`；stage 超限返回 `StageLimit`。失败不增加已发放计数，已发放工作不会因失败、回退或取消退款。回调提交还会消耗跨 Run 累计的 root stage。普通 callback 提交使用 Queue 计量等待 worker 开始的回调，worker 取走 callback 时释放 Queue slot；callback envelope metadata 保留到回调退出。CPU tile job 会保留等待准入和 managed Queue lease，直到所有已提交 tile 退出且 job 从队列摘除。

Coordinator 在提供 capability 前，会按 input schema 和已发布 coverage 校验每个 `ResultObjectNeed` 与 `ResultTensorNeed`。`ResultTensorInput` 只授权读取请求的 samples。Tensor window acquisition 保留获准的 backing owners 与 leases；只要 owning handle 仍存活，window 可在 callback 返回后继续使用。`TemporaryStorage` 接收单独传入的 `ResourceBudget`，用它计量有界私有临时文件容量和返回的 buffers。

Structured Result execution 使用同一 root 管理图像页、不可变 sample coverage、relation witnesses、field storage 和 retained source owners。图像 publication 选择 PlanarImage materialization 时，每个 frame 和 layer 使用有界 backing；metadata 与 coverage maps 计入 Metadata，planar pixel capacity 计入 Payload。其他 tensor publication 路径可保留 affine 或 `CpuStorage` backing 及 source Result owners，而不复制 payload；这些 owner 继续持有原有 accounting leases。Image read capability 保留 Result 和授权其 sample Region 的 captured descriptor。复制 capability 也会保留 backing 和 accounting lease。`ResultRef::capture()` 固定一份不可变的 descriptor revision、coverage、relations 和 dependency evidence；共享 waiter 消费该 publication snapshot，不观察生产者之后的 revision。

Result relation rows 与已发布图像 payload 使用所选 resource root 计量。每次 publication 都必须符合配置的 payload、work、I/O 和 stage 限额。`Exact`、`Conservative` 与 `Unknown` 具有不同的 dirty-propagation 行为；未解析的 relation 不能证明输出为 clean。PlanarImage materialization 会将字节复制到 managed backing，并在公开 sample coverage 前计入 payload；affine 与 storage-view publication 则保留 source owners 及其 accounting，不复制 tensor bytes。Schema selection 只保留声明过的 typed image 与 Result metadata resources，包括 ICC 和 OCIO bindings。Compiler 将嵌套 Result schemas 及其 resource identities 带入 plan；runtime Result bindings 会在 execution root 下重新准入所需 owners。

Typed tensor 样本校验按最多 256 个标量样本的额度批次计费。读取一个没有剩余额度的样本前，validator 为该区域计入 `min(256, remaining)` 个 work 单位，然后用相同的数值与语义检查逐个校验每个样本。通过校验的 `N` 样本区域恰好消耗 `N` 个单位。失败的样本可能留下最多 255 个已预付但未读取的样本，这部分 work 不退还。被拒绝的计费直接返回其状态，不以更小批次重试。Validator 在每次读取样本前和每次计费后检查取消，并在每个已校验区域之后检查取消和宿主 stop 状态，因此被取消、stale 或停止的 Run 最迟在下一个区域边界结束。失败校验记录的 work 因而可能超过实际读取的样本数。

依赖数据值的 relation 表、Footprint box 索引、radix sort scratch 和操作自有缓存都在执行 root 上使用 `ResourceAllocator`。`ResultRelation::gather` 表、其规范化 support 以及 STMap 的 map 和 source 缓存在其 owner 存活期间计入 Host 与 Metadata 容量；已发布的 relation 保留其表，直到引用它的最后一个 Result 或依赖快照被释放。Host 容量包含 Shared 和 Metadata，因此一次 Run 的 Host 峰值与 Metadata 峰值相互重叠，不能相加来估计进程内存。

`ExecutionDependencies` 返回的 coverage 和 guarantee maps 使用同一 resource root 的 `ResourceMap` allocator。`source_support()` 与 `potential_dirty()` 返回 root-owned `ResourceMap<Footprint>`；`source_observations()` 返回 root-owned `ResourceVector<SourceObservation>`。每条 observation 自有其 `ResourceString` input name 和 `Footprint`，并记录 typed target、slot 与 roles。这些值可比 `ExecutionDependencies` 对象和 `ExecutionContext` 活得更久；其 allocator owners 会让 accounting root 保持存活，直到最后一个 map、vector、name 或 footprint 释放。

```cpp
#include "photospider/execution/dependencies.hpp"

// dependencies is ExecutionResult::dependencies from a completed run.
ps::ResourceMap<ps::Footprint> coverage = dependencies.coverage();
ps::ResourceMap<ps::DependencyGuarantee> guarantees = dependencies.guarantees();
auto observations = dependencies.source_observations();
```

Maps 和 observation sequence 是复制后的 snapshots。调用方可以保留或移动这些值而不借用 dependency object，其 root-owned storage 会继续计入预算。

GPU context 在创建 device 时使用 execution context 的资源 root 进行 native 分配；无论是否提供自定义 managed-resource limits，该 root 都存在。Invocation metadata 使用显式独立的计量域。native pipeline cache 最多保留 64 个 entry；单批最多保留 32 条命令，每条最多 31 个 storage binding；一次 invocation 最多 1024 个存活 view token。地址映射仅持有弱 `CpuStorage` 引用，不拥有 native buffer。

托管 native metadata 准入失败后，device 清空 native pipeline cache 并重试一次。GPU payload 准入可先回收可丢弃的待写磁盘缓存和 result-cache owner，再原子预留实际 native capacity。这些恢复路径不会重试 operation callback。

`TemporaryStorage` 拥有私有、无缓冲的临时文件，并以字节偏移寻址。编码 extent 分别按 4096 字节取整；磁盘限额统计编码文件 capacity，不统计文件系统块。读取有范围和窗口上限、同步执行，返回不可变 owner buffer，并让文件和 root lease 保持存活。追加前先预留容量；回滚无法确认时将 reservation 隔离到文件成功关闭。冻结前缀不可覆盖，seal 后不能继续生产。取消阻止新 I/O，已提交的同步调用结束后才释放 owner。

`TemporaryStorage` 在创建文件前以及每次 append、write 或 read 前向当前线程的 I/O protocol fence 请求许可。该 fence 是 core 层的同步 thread-local capability：owner 提供策略，状态在 scope 退出前处于借用状态，嵌套 scope 退出时恢复上一层。未安装 scope 时 I/O 照常进行。Execution 在受 fence 保护的 Result callback 周围安装绑定 callback failure code 与 failure latch 的 scope。在该 callback 内调用 `TemporaryStorage` 属于强制 I/O protocol violation，会在文件改变前以 `InvalidArgument`、`UnauthorizedRead` 和 Protocol/Group detail 失败。此前已记录的失败仍是报告的首个原因，例如先前的 `ResourceExhausted` 与 `CapacityLimit`；latch 记录的 violation 会否决 CPU retry。

`preserve_output_views` operation 在仿射 view 覆盖输入需求时可以发布该 view。同一 owner 的兼容 fragment 只有经地址映射验证覆盖关系后才可合并。root 外部的源 storage 通过 Referenced lease 计费；root allocator 已拥有的源 storage 保持原有 Payload 和 Host 计账，不重复计入 Referenced。新分配的输出 backing 由活动输出 allocator 计费。该选项适用于 CPU Atomic staged 或 Whole 执行，排除 GPU 和 joint 执行。`requires_input_views` 会进一步要求 Whole 执行。

## 算法与数学实现 (Algorithms & Math)

预算乘积、对齐和页取整在分配或发布前进行溢出检查。准入依据申报或查询到的 allocation capacity；`live` 表示当前准入容量（包括未使用 reservation），`peak` 表示观测到的该计数最大值。

## 限制与非目标 (Limitations & Non-Goals)

- `WithinBudgetOrFail` 只覆盖已纳入计量的 allocation，不限制总 RSS、驱动私有存储和标准库外部存储。
- 空容器及部分内部几何/重建 metadata 未被完整计量。重建的外层 `ValueFragments` metadata 可能独立于原 publication token 存活，但每个 `Value` 仍持有自身 storage owner。
- 调用方从管理范围取出原始 `Value` 或 vector 并在 managed allocator 之外复制时，自行承担该副本的内存成本。
- 前缀最终性和跨字段关联校验由 Result publisher 负责，不属于 `TemporaryStorage`。
