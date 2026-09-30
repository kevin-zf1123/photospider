# 托管资源与临时存储

## 模块边界与职责 (Scope & Ownership)

只有配置 `ExecutionContextConfig::managed_resources` 后，`ExecutionContext` 才创建供受控 CPU/GPU 工作共享的资源根。`ExecutionContext::resource_budget()` 返回 `Result`；未启用托管资源时返回 `NotFound`。租约可在 context 销毁后继续存活。计量覆盖已申报的容量，不等于进程 RSS 上限；线程栈、驱动私有分配、操作系统页缓存和未使用托管分配器的内存不在范围内。

预算跟踪 Host、Device、Shared、Metadata、Referenced、TemporaryDisk、Entries、Files、I/O slots、Queue 和 Payload。部分维度描述同一物理字节：Host 包含 Metadata 与 Shared，Device 包含 Shared，不能把重叠计数相加。context 的 Payload 另受 `maximum_live_bytes` 限制；调用方原有存储通过 Referenced 单独准入，不计入 Payload 子限额。

## 核心数据结构与内存布局 (Data Layout & Memory)

```cpp
#include <cstdint>

#include "photospider/execution/resources.hpp"

namespace ps {
Result<ResourceLease> reserve_capacity(const ResourceBudget& budget,
                                       std::uint64_t bytes) {
  return budget.reserve(ResourceCapacity::host(bytes));
}
}
```

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

`consume(ResourceWork)` 原子准入 work、byte、request 和 stage 计数。work 或 I/O 超限返回 `ResourceExhausted` 和 `WorkLimit`；stage 超限返回 `StageLimit`。失败不增加已发放计数，已发放工作不会因失败、回退或取消退款。回调提交还会消耗跨 Run 累计的 root stage。普通 callback 提交使用 Queue 计量等待 worker 开始的回调，worker 取走 callback 时释放 Queue slot；callback envelope metadata 保留到回调退出。planar `CPU_STAGES` job 会保留等待准入和 managed Queue lease，直到所有已提交 tile 退出且 job 从队列摘除。

Staged dependency 的 `NeedBatch` 和 certificate 副本各自取得 metadata owner；复制向量不会转移源 owner 的计费。宿主在接受最终可变 batch 前重新封装 metadata。dependency session 记录 session 开始时的 root；continuation 在没有活动 `ResourceAllocationScope` 时恢复此 root 进行 metadata 分配，活动 scope 始终优先。

GPU context 在创建可选 device 前创建资源 root，并显式把同一 root 传入 native 分配。未配置托管资源时，device 使用普通分配。Invocation metadata 使用显式独立的计量域。native pipeline cache 最多保留 64 个 entry；单批最多保留 32 条命令，每条最多 31 个 storage binding；一次 invocation 最多 1024 个存活 view token。地址映射仅持有弱 `CpuStorage` 引用，不拥有 native buffer。

托管 native metadata 准入失败后，device 清空 native pipeline cache 并重试一次。GPU payload 准入可先回收可丢弃的待写磁盘缓存和 result-cache owner，再原子预留实际 native capacity。这些恢复路径不会重试 operation callback。

`TemporaryStorage` 拥有私有、无缓冲的临时文件，并以字节偏移寻址。编码 extent 分别按 4096 字节取整；磁盘限额统计编码文件 capacity，不统计文件系统块。读取有范围和窗口上限、同步执行，返回不可变 owner buffer，并让文件和 root lease 保持存活。追加前先预留容量；回滚无法确认时将 reservation 隔离到文件成功关闭。冻结前缀不可覆盖，seal 后不能继续生产。取消阻止新 I/O，已提交的同步调用结束后才释放 owner。

`preserve_output_views` operation 在仿射 view 覆盖输入需求时可以发布该 view。同一 owner 的兼容 fragment 只有经地址映射验证覆盖关系后才可合并。root 外部的源 storage 通过 Referenced lease 计费；root allocator 已拥有的源 storage 保持原有 Payload 和 Host 计账，不重复计入 Referenced。新分配的输出 backing 由活动输出 allocator 计费。该选项适用于 CPU Atomic staged 或 Whole 执行，排除 GPU 和 joint 执行。`requires_input_views` 会进一步要求 Whole 执行。

## 算法与数学实现 (Algorithms & Math)

预算乘积、对齐和页取整在分配或发布前进行溢出检查。准入依据申报或查询到的 allocation capacity；`live` 表示当前准入容量（包括未使用 reservation），`peak` 表示观测到的该计数最大值。

## 限制与非目标 (Limitations & Non-Goals)

- `WithinBudgetOrFail` 只覆盖已纳入计量的 allocation，不限制总 RSS、驱动私有存储和标准库外部存储。
- 空容器及部分内部几何/重建 metadata 未被完整计量。重建的外层 `ValueFragments` metadata 可能独立于原 publication token 存活，但每个 `Value` 仍持有自身 storage owner。
- 调用方从管理范围取出原始 `Value` 或 vector 并在 managed allocator 之外复制时，自行承担该副本的内存成本。
- 前缀最终性和跨字段关联校验由 result publisher 负责，不属于 `TemporaryStorage`。
