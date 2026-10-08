# 缓存模型

## 范围与 ownership

Kernel 分别管理编译计划、不可变输入快照、已完成结构化 Result 复用、共享中的 Result work 和 native backend input-copy cache identity。进入缓存只赋予复用资格；活动 Results、snapshots、typed backing Values 和 read windows 在 entry 清除或淘汰后仍由各自 owner 与资源租约持有。

`PlanCacheKey` 标识 physical plan，不包含输入 payload，也不证明 graph currentness 或标识计算结果。已完成 structured Result 有两条不同的复用路径。可选的 `ResultCache` LRU 强持有 `StructuredCacheManifest`；manifest 拥有其 `ResultRef`、dependency bundle、source observations 和 replay proof。因此 LRU 会一直保留 Result payload 与 metadata，直到 entry 被淘汰或调用 `clear`。候选索引本身使用弱引用；但在 entry 仍驻留时取得 candidate 的 lookup 会强持有 manifest 以完成 replay，前提是 cache epoch 与 Root 容量仍允许。另一条路径 `SharedResults` 会合并同一 semantic key 的 producer 和 waiter 工作。其完成结果索引是 `WeakResultRef`：producer 完成后，只有在其他 owner 仍持有 Result 时，table 才能复用它。Producer 活动期间，entry 可以为 waiters 保留当前 publication。Dependency records 只保存 ancestry 和 support metadata，不保存输入 payload。Context cache 也会以 `Value` entry 保留 native input backing；这些 entry 保留的是传输 backing，不是 structured Result 语义结果。`ResultRef::capture()` 在一个 revision snapshot 已认证的 descriptor、field/image relations、descriptor basis 和 dependency bundle。Result 的有序 source association 是共享 Result object 上独立的单调 owner 关系；消费新输入时可以继续扩展。

每个 structured Run 都拥有一个编译后的 `StructuredResultCache` 组件，负责 completed-result key、manifest、source proof、replay、snapshot 恢复和可选 cache-work quota。Run coordinator 以同步借用 view 提供 context 与 actor 状态，并提供 replay Host 执行 Need 请求、查询 cancellation 和 adoption；cache 组件不会保留这些 view，也不会调度 callbacks。Lookup 在候选项驻留时取得其强 manifest owner。即使 LRU 随后淘汰该 entry，candidate 仍会在 proof、replay 和 adoption 期间保持拥有。Adoption 不要求再次证明 LRU residency；可选 lookup 仅在同一 manifest 仍驻留时提升其 LRU 位置。Cache closing 或 epoch 变化仍可拒绝 adoption；独立持有的 candidate 会在该次尝试释放后才释放其 Result backing。

## 不可变 Value snapshots

```cpp
#include "photospider/data/input_snapshot.hpp"

namespace ps {
Result<InputSnapshot> import_snapshot(const InputSnapshotStore& store,
                                      const Value& value) {
  return store.import_value(value);
}
}
```

`InputSnapshotStore` 为受支持的内建 numeric Value 类型保留 rank 1 至 rank 8 的不可变 blocks。Import 保留有效 generic 位模式，并校验受支持的 typed scalar/tensor samples。Store 分别限制 block 数量、跨版本实际 payload bytes 和目录 metadata。调用方持有的 Values 与 snapshot metadata 不计入 retained-payload 字节限额。

Patch 要求类型、shape 和 facets 匹配，并替换一个精确非空 Region。Store 复制相交 blocks 并保留先前版本。Reads 与 patches 接受 cancellation 和样本上限。取消的 read 可能已部分写入调用方 destination；只有成功才能确认结果完整。

`content_identity(region)` 使用规范 framing 对 dtype、shape、请求坐标、facets 和精确 sample bits 进行 hash。Block geometry、origin、stride 和 allocation 不影响 identity。正负零与不同 NaN payload 保持区分。Snapshot/session identifiers 描述 provenance；deterministic operations 不能从 identifier 字符串推导样本值或依赖。

## 完成 Result 复用与共享 work

Completed-result cache 为可缓存且 selected-input producer closure 纯净、backend 与编译 plan 一致的 operation 保存 sealed structured Results。Cache key 包含编译后的 operation template、selected output、backend、tensor slot 和精确 tensor footprint Q；不使用 node IDs 或运行时 input ObjectIds。`StructuredResultCache` 从 Result dependency bundle 生成 source observations，按当前 Result bindings 对被观察的 descriptor 或 field/tensor bytes 做 hash，并保存 callback 的 Result Needs 及重放所需的 supplied descriptor/coverage facts。观察到 backend fallback、quality outcome、未证明的 source support、`photospider.path_set` 和不符合条件的 operation closure 都不会被保留。

查询时，cache component 根据当前 Result bindings 重新计算 source proof，通过普通 Result coordinator 重放每个保存的 Need，并比较 ready facts。Proof 与重放都匹配且 cache epoch 仍有效时，replay Host 将强持有的 candidate Result 重新绑定到当前 semantic key 和 source association。重新绑定后的 Result 拥有新的 ObjectId，并共享缓存的物理 backing。Cache metadata、proof 遍历、hash 和保留的 payload 计入各自 Root/cache 限制。可选 proof work、metadata 超限或 retention capacity 不足时，会跳过复用或保留并继续计算。Completed-result cache 的语义输入/输出对象是 Result；Value 仅作为 typed backing 和内部 cache storage。

Frozen execution 固定其 plan 与不可变 Result bindings；调用方替换 bindings 不会改变已捕获的输入。相同 key 的活动 Result requests 可独立于 completed-result retention 共享 producer。每个 waiter 保持自己的 cancellation 和 currentness 状态。Producer 或其他 waiter 继续时，waiter 可消费 captured publication。一个 waiter 的 cancellation 不会取消其他 waiter 仍需要的工作；最后一个 waiter 离开后，已准入的 producer work 排空才释放 owners。完成后的 `SharedResults` entry 使用 weak Result reference，因此最后一个 strong Result owner 释放后，其 image pages、fields、relations 和 retained input associations 随之释放。该生命周期规则与上文由 ResultCache LRU 强持有 Result 的路径相互独立。

当 frozen-plan caller 在 structured Result producer callback 尚未完成时被取消，或其 Demand binding generation 变为 stale，且健康 peer 仍需要该 producer，`SharedResults` 可以保留 coordinator，让正等待 Result 的 peer 在当前线程继续驱动它。原 caller 在最终检查时仍收到 `Cancelled` 或 `Stale`；frozen peer 保留捕获的 plan 和 bindings。Borrowed-graph execution 继续严格检查 graph currentness。Coordinator 必须等 callback 退休后才能恢复 actor state。如果 adopt 失败，或 adoption 完成前 peer 已离开，caller 会撤销临时 registry owner 并同步 drain；同一次 run 不会重试 adoption。借用 plan 的调用和没有 live peer 的调用也会同步 drain。Context shutdown 会先在 registry lock 内取消 shared work，再释放该锁并 drain 所有 parked coordinator；此路径不会创建 worker thread，也不改变同步 cache replay 的行为。

Dependency records 保留不可变 relation evidence、Need manifests、source observations 和 content identities，不拥有 input 或 image payloads。复用时按记录的 support 比较 source bytes，包括已消费的 Control observations。已消费 Control 变化后，下次 request 可发现不同 Data support；无关或未消费的 controls 不增加 support。返回的 Result 与 dependency maps/vectors 保留 root-accounted metadata owner，因此调用方持有副本期间仍计入预算。

`maximum_dependency_cache_metadata` 默认 65536 proof units，范围为 1..1048576。`maximum_dependency_cache_work` 是单独的 per-run traversal 上限，默认 1048576，设为零会禁用 dependency reuse。该可选 quota 由 `StructuredResultCache` 持有和扣除；cache proof、key 构造、replay bookkeeping 与 snapshot 工作除 Root 必需 work 外也使用它。Proof 超限时跳过复用或保留，并继续计算。关闭可选复用不会退还已经消耗的 Root work。Deduplication 不会抵消已经花费的遍历、hash、association 或 normalization work。

## 限制与当前入口

- Native GPU results 与 CPU-exact result keys 隔离。Fallback result 不写入 native keys，近似 native results 不替代 exact CPU results。
- Native input-copy reuse 与 completed Result computation reuse 分离。它可复用所需 Result tensor backing 的 transport，但不会使上游 producer computation 获得缓存资格。
- LRU eviction 会撤销新 lookup 对该 entry 的资格。已经取得强 candidate 的 lookup 可在 eviction 后完成 replay 和 adoption，只要 cache 仍开启且 epoch 仍有效。Adoption 不要求再次证明 LRU residency；可选 promotion lookup 仅在同一 manifest 仍驻留时影响 LRU 顺序。Cache closing 或 epoch 变化会拒绝 adoption，而 candidate 的独立 owner 会继续有效，直到该次尝试释放它。Frozen executions 是内存 owners，不是序列化 plans。

Public Result 图像入口见[`examples/unified_result_workflow`](../../../examples/unified_result_workflow/README.zh.md)；当前 production image operation 可用性见[图像 operations](Image-Operations.zh.md)。Snapshot 和 generic cache 行为由 `test_input_snapshot`、`test_frozen_execution`、`test_generic_result_cache`、`test_shared_results` 与 `test_computed_scalar` 覆盖。Result capture 和 owner release 由 `test_result_image_contracts`、`test_global_results` 与 `test_shared_results` 覆盖。
