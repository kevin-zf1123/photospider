# 缓存模型

## 范围与 ownership

Kernel 分别管理编译阶段、plan cache key、不可变输入快照、已完成结构化 Result 复用、checkpoint 与 block 状态，以及 native backend input-copy cache 的 identity。进入缓存只赋予复用资格；活动 Results、snapshots、Tensor backing 和 read windows 在 entry 清除或淘汰后仍由各自 owner 与资源租约持有。每个 Run 执行静态 plan，VM 为其正在执行的指令直接进行 completed-result、checkpoint 与 block 查找，并使用自己的可选 cache-work 配额。

## Identity 域

| Identity | 域标签 | 内容 | 何时改变 |
| --- | --- | --- | --- |
| Semantic IR digest | `semantic-ssa-ir-v24` | 规范 SSA 程序：declarations、calls、values、blocks、terminators、参数、traits、资源 identity、schemas | 任何程序变化，包括 call 或 value 重新编号 |
| Optimized IR digest | `optimizer-v6-ssa-noop` | 父 semantic digest 与优化后程序 | 同上 |
| Physical plan digest | `static-tile-program-v26` | 父 optimized digest、执行模式、tile extent、steps 与放置、输出请求，以及每个 block 的指令、授权、依赖和 terminator | 任何指令、授权或请求变化，或重新编号 |
| Plan cache key | `plan-cache-key-v19` | Plan digest 的域分离包装 | 与 plan digest 相同 |
| Step structured template | `photospider.dependency-static.v4` | Tile extent、执行模式、后端、operation key、输出索引、traits、资源 identity、参数、输入 schema | 契约、参数、资源或输入 schema 变化；重新编号不改变 |
| 固定授权内容 key | `photospider.fixed-grant-cache.v2` | Step template、后端、成员、tile、publication samples、descriptor 授权端口、已授权输入的 schema 与样本字节 | 任何输入字节或授权变化；重新编号不改变 |
| 固定 block 输入证明 | `photospider.fixed-block-inputs.v2` | Block 契约加上相同的授权内容（不含成员与 tile） | 与内容 key 相同 |
| 已保存算子契约 | `artifact-static-operation-contract-v2` | 一个 operation key 的注册 traits 与 validator/report 引用 | 算子契约变化 |

前四个 identity 是编译器 identity。它们包含程序标识符，因此即使计算等价，重新编号 call 或 value 也会改变它们；它们是可复现辅助，不是安全 identity。Template 与内容 identity 不含物理 node、step 或 plan 标识符。因此位于不同 step 的两个等价 call、重命名或重新编号的 plan，或从 `.psxp` 重新加载的 plan，在其固定输入内容相同时共享 completed-result、checkpoint 与 block 条目。依赖记录仍属于当前 plan；复用从不导入另一个 plan 的记录。

Descriptor-only 授权按端口索引进入内容 key。编辑已保存的 plan 使某个 descriptor 授权指向不同端口时，key 随之改变，编辑后的 plan 不能复用原 plan 的缓存值。

## 不可变 Tensor snapshots

```cpp
#include "photospider/data/input_snapshot.hpp"

namespace ps {
Result<InputSnapshot> import_snapshot(const InputSnapshotStore& store,
                                      const Tensor& value) {
  return store.import_tensor(value);
}
}
```

`InputSnapshotStore` 为受支持的内建 numeric Tensor 类型保留 rank 1 至 rank 8 的不可变 blocks。Import 保留所有位模式，只检查 Tensor 的 descriptor 与 coverage，不解释样本值。Store 分别限制 block 数量、跨版本实际 payload bytes 和目录 metadata。调用方持有的 Tensor 与 snapshot metadata 不计入 retained-payload 字节限额。

Patch 要求类型和 shape 匹配，并替换一个精确非空 Footprint。Store 复制相交 blocks 并保留先前版本。Reads 与 patches 接受 cancellation 和样本上限。取消的 read 可能已部分写入调用方 destination；只有成功才能确认结果完整。

`content_identity(region)` 使用规范 framing 对 dtype、shape、请求坐标和精确 sample bits 进行 hash。Block geometry、origin、stride 和 allocation 不影响 identity。正负零与不同 NaN payload 保持区分。Snapshot/session identifiers 描述 provenance；deterministic operations 不能从 identifier 字符串推导样本值或依赖。

## 纯度与可缓存性

VM 为每条指令和每个值跟踪两个独立属性：

- **Pure（纯）**：算子确定且无副作用，指令没有共享 stage 读取且本身不是共享 stage，attempt 未被 fallback 污染，每个 payload 输入都来自 workflow 输入或纯 producer，本次 activation 的控制证据是纯的，且没有 payload 输入是 block 参数。
- **Cacheable（可缓存）**：纯，并且算子带有 `cacheable` trait、producer 可缓存、控制证据可缓存。

Checkpoint 与 block 服务只要求纯度。因此即使其自身算子或某个祖先设置了 `cacheable = false`，纯指令仍可使用这两项服务。Completed-result 查找与保留要求可缓存性。Block 参数、共享闭包和回退路径绕过全部可选复用；这是保守策略，因为 block 参数在每轮循环中可能携带不同的值。

## 完成 Result 复用

Completed-result 查找计算指令的固定授权内容 key，读取该 key 下至多八个候选，跳过后端不同的候选，并以新 ObjectId 和当前 producer scope 重新绑定匹配项。重新绑定的 Result 像计算所得发布一样被校验（包括输出 payload 上限），并共享缓存的物理 backing。Run 安装它时，依赖证据由当前 plan 的授权重建。Cache metadata、key 哈希、样本读取和保留的 payload 计入各自 Root/cache 限制。可选 work 耗尽、metadata 超限或保留容量不足时，跳过复用或保留并继续计算。命中不产生 `OperationTiming` 记录，也不增加 `computed_elements`。

Frozen execution 固定其 plan 与不可变 Result bindings；调用方替换 bindings 不会改变已捕获的输入。Borrowed-graph execution 继续严格检查 graph currentness。

`ExecutionContextConfig::result_cache_bytes` 默认为 0，即禁用 completed-result 保留与 block 复用。`maximum_dependency_cache_metadata` 默认 65536 proof units，范围为 1..1048576。`ExecutionOptions::maximum_dependency_cache_work` 是单独的每 Run 可选配额，默认 1048576，设为零会禁用该 Run 的 completed-result、checkpoint 与 block 复用。Key 构造、样本哈希、重新绑定、checkpoint witness 和 block key 在 Root 必需 work 之外使用该配额。耗尽时跳过复用或保留并继续计算；不会退还已经消耗的 Root work。

## Checkpoint 与 block 状态

Checkpoint 范围是固定授权内容 key 加 `/checkpoint` 后缀，因此等价 call 独立于其物理 step 共享 checkpoint 状态。恢复 checkpoint 只恢复状态内容；当前指令保留自己的固定授权与证据，旧 witness 从不替换它们。`ExecutionContextConfig::maximum_result_checkpoint_scopes` 限制存活范围数，每个范围至多保留 64 个条目。Checkpoint 不需要正的 `result_cache_bytes`。

Block 状态复用把 block 契约与固定 block 输入证明，同 block kind、range、mode 和输入状态绑定在一起。只有当 compute 回调在该 block 期间确实 dispatch 了原生工作时，GPU block 才进入缓存；后端与固定输入证明都是 key 的一部分，因此没有原生计算证明的 GPU 路径从不被复用。见[全局结果](Global-Results.zh.md#跨输出-block-状态共享)。

## 限制与当前入口

- Native GPU results 与 CPU-exact result keys 隔离，因为执行模式与后端同时属于 template 和内容 key。带 fallback 污染的结果不进入任何复用缓存，近似 native results 不替代 exact CPU results。
- `ResultCacheStatistics::native_retained_bytes` 统计缓存保留的唯一原生分配，同时包括保留的 GPU 输出和保留的 GPU block 状态。它不是仅 block 的容量。
- Native input-copy reuse 与 completed Result computation reuse 分离。它可复用已授权 Result tensor backing 的 transport，但不会使上游 producer computation 获得缓存资格。
- LRU eviction 撤销条目今后的查找资格；持有 Result 或读窗口的调用方保留其 backing。`clear()` 推进缓存 epoch，因此更早 epoch 的工作不能重新填充已清空的缓存。Frozen executions 是内存 owners，不是序列化 plans。

当前 production image operation 可用性见[图像 operations](Image-Operations.zh.md)。Snapshot 和 generic cache 行为由 `test_input_snapshot`、`test_frozen_execution`、`test_generic_result_cache` 与 `test_computed_scalar` 覆盖。跨编辑后 descriptor 授权的内容 identity 复用，以及跨物理 step 的 checkpoint 共享，由 `test_static_compiler` 覆盖。Result capture 和 owner release 由 `test_result_image_contracts` 与 `test_result_objects` 覆盖。

Run 共享 stage 由 plan 本身在每个 call、stage 和 block activation 中执行一次，独立于可选缓存。它们不提供 checkpoint 或 block 服务，也不跨 Run 保留活动状态。带共享 stage 读取的指令绕过 completed-result 缓存；见[全局结果](Global-Results.zh.md#completed-result-缓存)。Snapshot block 几何独立于 plan tile extent。
