# 托管资源

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

`CancellationToken`、`ResourceBudget`、`ResourceLease`、`ResourceAllocationScope` 和 `ResourceAllocator` 是 `photospider/core/` 下声明的 public facilities。Cancellation 是协作且单调的；`ResourceBudget::consume` 原子准入累计 work。失败或取消不会退还已发出的 work。资源计量覆盖受 instrument 的 capacity 和 work，不限制进程 RSS。

预算跟踪 Host、Device、Shared、Metadata、Referenced、Entries、Queue 和 Payload。部分维度描述同一物理字节：Host 包含 Metadata 与 Shared，Device 包含 Shared，不能把重叠计数相加。context 的 Payload 另受 `maximum_live_bytes` 限制；调用方原有存储通过 Referenced 单独准入，不计入 Payload 子限额。

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

`consume(ResourceWork)` 原子准入 work 和 stage 计数。work 超限返回 `ResourceExhausted` 和 `WorkLimit`；stage 超限返回 `StageLimit`。失败不增加已发放计数，已发放工作不会因失败、回退或取消退款。回调提交还会消耗跨 Run 累计的 root stage。普通 callback 提交使用 Queue 计量等待 worker 开始的回调，worker 取走 callback 时释放 Queue slot；callback envelope metadata 保留到回调退出。CPU tile job 会保留等待准入和 managed Queue lease，直到所有已提交 tile 退出且 job 从队列摘除。

Coordinator 在提供 capability 前，会按 input schema 和已发布 coverage 校验每个 `TileNeed`。`ResultTensorInput` 只授权读取请求的 samples。Tensor window acquisition 保留获准的 backing owners 与 leases；只要 owning handle 仍存活，window 可在 callback 返回后继续使用。

结构化 Result 缓存和 checkpoint 缓存状态各自保存一份从执行 root 复制而来的 `ResourceBudget` handle。副本共享同一个 root，因此即使 coordinator 的包装对象被重新赋值或销毁，这两个组件仍向该 root 计费，其正确性也不依赖 coordinator 中成员的声明顺序。

Structured Result execution 在同一 Root 下计费 Tensor backing、coverage、tile 记录、冻结共享证据和保留 owner。物化按实际请求样本容量分配；view 发布保留已有 owner 而不复制 payload。元数据与目录计入 Metadata，数值 backing 计入 Payload，外部保留存储按适用规则计入 Referenced。`capture()` 固定 descriptor revision 和授权覆盖，后续进度不扩大旧 capture。

Tile 依赖元数据和已发布 Tensor payload 使用所选 Root。每次事务检查 work、payload、stage 和 I/O 上限。复用冻结证据不授予样本访问。Result schema 不携带资源。算子资源是通用的 `ImmutableResource` handle：拥有不可变字节及 `ResourceIdentity`（字节长度与 SHA-256），在某个 root 下准入，不做任何格式解析。`ImmutableResource::import` 复制并哈希字节；`from_storage` 哈希已有存储而不复制；二者都在提供的预算中计入 work 并观察取消，失败时释放全部临时分配。`ResourceSet::create` 对 backing 去重并拒绝冲突；`select` 精确保留请求的 identity，缺失时返回 `NotFound`；`reference` 在另一个 root 下重新准入整个集合而不复制 payload。Compiler 从调用方集合中选择每个算子声明的 identity，plan 保留它们；执行 root 与 owning root 不同时，这些引用会在执行 root 下准入。Result 自身不会让资源保持存活。

内核调用显式绑定 validator，不添加默认样本域规则。Float32 scalar 闭区间属于 ops SDK `ops.scalar-range` validator 契约。Validator 读取消耗 work，scratch 在进入前准入，失败不退还已消耗 work。算法保留自身领域检查。

GPU context 在创建 device 时使用 execution context 的资源 root 进行 native 分配；无论是否提供自定义 managed-resource limits，该 root 都存在。Invocation metadata 使用显式独立的计量域。native pipeline cache 最多保留 64 个 entry；单批最多保留 32 条命令，每条最多 31 个 storage binding；一次 invocation 最多 1024 个存活 view token。地址映射仅持有弱 `CpuStorage` 引用，不拥有 native buffer。

托管 native metadata 准入失败后，device 清空 native pipeline cache 并重试一次。GPU payload 准入可先回收可丢弃的待写磁盘缓存和 result-cache owner，再原子预留实际 native capacity。这些恢复路径不会重试 operation callback。

算子显式获取并发布授权 view。兼容片段映射需要检查，reshape 还要求可合并为仿射映射。Root 外部来源 owner 获得 Referenced 租约，Root 自有存储保持原计费而不重复收费。新 backing 使用活动 allocator。稠密收集和设备 packing/transfer 是具有容量、work 与取消检查的显式操作，不是协调器策略。

## 算法与数学实现 (Algorithms & Math)

预算乘积、对齐和页取整在分配或发布前进行溢出检查。内核统一使用 `include/photospider/core/checked_math.hpp` 中的一组无符号辅助函数：带可选上限的加法、乘法、乘加和对齐检查，以及饱和乘法。检查失败时不写输出，各调用点保留自己的处理方式：

| 数量 | 溢出时 |
| --- | --- |
| 分配、存储、传输和工作量计费 | 拒绝，通常返回 `ResourceExhausted` |
| 逻辑 domain 大小和 reshape 成本估计 | 饱和到上限 |
| 可选 checkpoint 缓存的成本估计 | 跳过缓存路径，算子照常执行 |

容量检查接受恰好填满剩余容量的请求；例如配置为 24 字节的 result cache 可以同时容纳一个 16 字节和一个 8 字节的条目而不发生淘汰。准入依据申报或查询到的 allocation capacity；`live` 表示当前准入容量（包括未使用 reservation），`peak` 表示观测到的该计数最大值。

## 限制与非目标 (Limitations & Non-Goals)

- `WithinBudgetOrFail` 只覆盖已纳入计量的 allocation，不限制总 RSS、驱动私有存储和标准库外部存储。
- 空容器及部分内部几何/重建 metadata 未被完整计量。重建的外层 `Tensor` metadata 可能独立于原 publication token 存活，但每个 `Tensor` 仍持有自身 storage owner。
- 调用方从管理范围取出原始 `Tensor` 或 vector 并在 managed allocator 之外复制时，自行承担该副本的内存成本。
- Result C ABI 5 在每次 bridge `poll` 结束时释放普通的 C callback scratch。已保留的 tensor handle 以及由 `create_block_state` 创建的不可变 block-state Result 可以跨 poll 存在。
