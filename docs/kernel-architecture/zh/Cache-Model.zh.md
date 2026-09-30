# 缓存模型

## 范围与 ownership

Kernel 分别管理编译计划、不可变输入快照、普通 Value 结果、共享中的 Result work 和可丢弃磁盘记录的 identity。进入缓存只赋予复用资格；活动 Values、Results、snapshots 和 read windows 在 entry 清除或淘汰后仍由各自 owner 与资源租约持有。

`PlanCacheKey` 标识 physical plan，不包含输入 payload，也不证明 graph currentness 或标识计算结果。已完成 Result 的索引采用弱引用：缓存可定位仍存活的 Result，但 entry 本身不保留 payload。强引用 `ResultRef`、captured publication 或活动 waiter 拥有该 Result。`ResultRef::capture()` 在一个 revision snapshot 已认证的 descriptor、field/image relations、descriptor basis 和 dependency bundle。Result 的有序 source association 是共享 Result object 上独立的单调 owner 关系；消费新输入时可以继续扩展。

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

## Value 结果复用与共享 work

Value result cache 返回命中前会重新验证 descriptor、demanded coverage、output semantic rules 和 typed samples。Regional keys 覆盖所需 producer regions、operation semantics 与 parameters，以及稳定 source content。无关 graph 编辑和 node IDs 不会使相同内容失效。Whole dependencies 采用保守策略。未证明稳定的 generic input 仍可执行，但会禁用相关 descendants 的跨 Run 复用。只有确定性、无副作用、可缓存且实现身份已证明的 operations 可复用。

当推导需求覆盖整个 Value 且不超过当前 2048-byte 限额时，小型、经过 preflight 验证、offset 为零的 dense Values 可建立 compact content identity。Snapshot、bounded-scalar 和 compact whole-Value sources 使用不同 key 类别。更大或局部的 direct inputs 不具备稳定性证明。Frozen execution 固定其 plan 与不可变 input bindings；调用方替换 snapshot handle 不会改变 frozen input。

共享 Result execution 捕获一个不可变 input bundle 和 selected output query。相同的 in-flight requests 可共享 producer。每个 waiter 保持自己的 cancellation 和 currentness 状态。Producer 或其他 waiter 继续时，waiter 可消费 captured publication。一个 waiter 的 cancellation 不会取消其他 waiter 仍需要的工作；最后一个 waiter 离开后，已准入的 producer work 排空才释放 owners。已完成查询 metadata 使用弱 Result references，因此最后一个 strong Result owner 释放后，其 image pages、fields、relations 和 retained input associations 随之释放。

Dependency cache manifests 保留不可变 relation evidence、source observations、content identities 和 fragment keys，不拥有 input 或 image payloads。复用时按记录的 support 比较 source bytes，包括已消费的 Control observations。已消费 Control 变化后，下次 request 可发现不同 Data support；无关或未消费的 controls 不增加 support。返回的 Result 与 dependency maps/vectors 保留 root-accounted metadata owner，因此调用方持有副本期间仍计入预算。

`maximum_dependency_cache_metadata` 默认 65536 proof units，范围为 1..1048576。`maximum_dependency_cache_work` 是单独的 per-run traversal 上限，默认 1048576，设为零会禁用 dependency reuse。Proof 超限时跳过复用或保留，并继续计算。Deduplication 不会抵消已经花费的遍历、hash、association 或 normalization work。

## 磁盘缓存

磁盘缓存使用规范 SHA-256 keys 和默认 registry 的 implementation fingerprint 保存符合条件的普通 Value records。发布前会校验 descriptor、Region、facets、payload length、checksum 和 result key。分配大小来自已验证 plan，而不是文件字段。损坏、截断、不匹配或不支持的记录按可丢弃 miss 处理。

写入先写临时文件，完整写入后再 rename。该缓存不是 durable result store。队列压力或写入失败只跳过保留，不使已计算结果失败。`flush_disk_cache()` 等待排队写入；`clear_disk_cache()` 删除 entries 并使待写 epoch 失效。一个 context 独占配置目录，其他文件会被忽略。磁盘资格限于受支持的 CPU-exact 普通 Value 契约；structured image Results 使用 Result ownership 与 identity。

## 限制与当前入口

- 磁盘缓存要求显式配置，且内存 Value-cache 容量为正。
- 默认 registry 提供持久 implementation identity。Custom 与 C-module registries 可使用进程内缓存，但不具有持久 implementation identity。
- Native GPU results 与 CPU-exact result keys 隔离。Fallback result 不写入 native keys，近似 native results 不替代 exact CPU results。
- Native input-copy reuse 对所需逻辑 bytes、descriptor、facets、Region 和 device identity 做 hash。它可避免重复上传不可变 Values，但不会使任意 `RegionalSource` computation 获得缓存资格。
- 清除或淘汰 cache entries 会撤销复用资格，不会使活动 owners 失效。Frozen executions 是内存 owners，不是序列化 plans。

Public Result 图像入口见[`examples/unified_result_workflow`](../../../examples/unified_result_workflow/README.zh.md)；当前 production image operation 可用性见[图像 operations](Image-Operations.zh.md)。Snapshot 和 generic cache 行为由 `test_input_snapshot`、`test_frozen_execution`、`test_generic_result_cache` 与 `test_shared_results` 覆盖。Result capture、owner release 和图像缓存由 `test_result_image_contracts`、`test_global_results` 与 `test_shared_results` 覆盖。
