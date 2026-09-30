# ADR 0018：冻结输入并复用精确局部结果

- 状态：Accepted
- 英文权威文档：[ADR 0018](../0018-local-result-caches-and-frozen-execution.md)

## 1. 核心摘要（TL;DR）

可变 workflow 图和编辑中的输入需要稳定的执行输入，区域计算也需要在精确依赖未改变时复用结果。内核拥有不可变输入快照、冻结执行包和有界的派生结果缓存。精确内容与依赖身份允许复用，同时缓存仍只是加速器。

## 2. 架构心智模型（Mental Model & Intuition）

`InputSnapshotStore` 类似带版本的分块存储：补丁只复制与其相交的块，其余块继续由旧版本共享。冻结执行固定一份已编译计划、对应注册表及不可变绑定，因此后续图编辑不会改变该次运行。

```text
图 + 绑定 --freeze--> 冻结计划与输入所有者
                           |
                       局部结果请求
                       /        \
                 精确缓存命中    缓存未命中
                      |            |
                      |         producer flight
                      |         /           \
                      |      等待者 A      等待者 B
                      |         \           /
                      +------ 已验证不可变结果
                                  |
                         内存缓存 / 可选磁盘缓存
```

`ExecutionContext` 拥有 worker 资源、in-flight 协调和缓存条目。若返回的 `Value` 仍持有结果存储及预算租约，结果可以比 context 活得更久。预览策略、事件队列、发布仲裁和界面呈现由应用代码负责。

## 3. 契约规约与接口（Formal Contracts & APIs）

```cpp
struct InputSnapshotStoreConfig {
  std::uint64_t maximum_bytes = 256U * 1024U * 1024U;
  std::uint32_t block_size = 128;
  std::uint64_t maximum_blocks = 65536;
};

class InputSnapshotStore {
 public:
  Result<InputSnapshot> import_value(const Value&, const SnapshotAccessOptions& = {}) const;
  Result<InputSnapshot> patch(const InputSnapshot&, const Value& replacement,
                              const SnapshotAccessOptions& = {}) const;
  std::uint64_t live_bytes() const;
};

struct ExecutionContextConfig {
  std::uint32_t cpu_workers = 0;
  bool gpu_enabled = false;
  std::uint32_t maximum_queued_tasks = 1024;
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
  std::uint64_t result_cache_bytes = 0;
  std::optional<DiskCacheConfig> disk_cache = {};
};

class ExecutionContext {
 public:
  Result<FrozenExecution> freeze(const ExecutionPlan&,
                                 ExecutionBindings = {}) const;
  Result<ExecutionResult> execute(const FrozenExecution&,
                                  const CancellationToken& = {},
                                  const ExecutionOptions& = {});
};
```

以上摘录省略 `ExecutionContext` 的其他成员。

快照保存受支持的 rank-1..8 `Value` 输入，并保留样本位和 facets。每个快照不可变；`patch` 接受相同 descriptor 和 facets，以及非空替换 Region，只复制相交块，所有旧版本继续可读。存储对保留版本的负载总字节数实施一个聚合上限。元数据和调用方持有的 `Value` 不计入该上限。当前实现会拒绝带 `photospider.image`、rank-3 或更高维 `photospider.color-array`、`ImagePlane` 和类型化 `Mask` 的输入快照，因为它们需要结构化 planar storage；图像 workflow 使用 `PlanarImage` 绑定路径。

`content_identity` 对规范元数据和请求区域中的精确样本位计算哈希。shape、dtype、坐标和 facets 参与身份；分配地址、布局和分块几何不参与。读取和身份计算支持样本上限及协作式取消。读取被取消时，调用方目标缓冲区可能只写入一部分，调用方应丢弃它。

`freeze` 捕获当前有效且匹配的计划、不可变绑定和 operation registry 所有者。自定义 `RegionalSource` 必须先导入内核快照。捕获过程不调用 operation callback，并在返回前重新检查图的 currentness。图被替换或销毁后，冻结执行仍使用捕获的输入版本；普通执行仍执行其 currentness 检查。每次 execute 调用独立提供取消状态，所有者会保留到已准入 callback 退出。当前捕获 API 会拒绝需要结构化 planar image capture 的计划，因此不冻结 `PlanarImage` 绑定路径。

已完成结果的保留是可选的，其容量是 `maximum_live_bytes` 的子限额；设为零会关闭保留，但相同的 in-flight demand 仍可共享。键包含局部 operation 契约和实现、精确请求输入内容及元数据、输出 Region 和相关 backend 身份。编译图 revision 和无关分支不进入局部结果键。dirty 提示可以缩小计算范围，但不能在缺少内容身份时证明结果可复用。参与计算的每个 operation 都必须确定、无副作用且声明可缓存，输入依赖也必须已证明，结果才可复用。

同一 context 中完全相同的 in-flight 请求由协调器共享一个 producer。每个 waiter 独立取消。取消一个 waiter 不会移除仍在等待的其他请求；最后一个 waiter 离开时，协调器请求取消 producer。producer 持有冻结输入和 registry，直到已准入 callback 退出。producer 不会占用 worker 等待同一线程池的另一个 callback。只有验证成功的不可变结果才进入完成缓存。

可选磁盘缓存要求调用方明确选择本地目录、启用正数的内存结果缓存容量，并使用维护的内建 operation registry 身份。自定义 operation registry 没有持久缓存身份，因此磁盘查询未命中且不能写入。缓存只保存 CPU-exact 模式下、带 image-v2 或规范 coverage-mask 元数据的 Float32 值，并受字节数、条目数和排队写入数限制。缓存存活期间，内核独占所选目录。启动时会清理遗留临时文件并索引有界条目；命中后再依据预期键、descriptor、facets、语义样本和校验和校验条目。未知或损坏条目作为缓存未命中处理。磁盘数据不恢复 workflow 状态，也不授予结果权威性；异步写入遇到失败或队列压力时会丢弃。GPU 模式运行不会读写磁盘条目。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- 快照覆盖不可变输入 `Value` 及其内容，不序列化 execution plan 或 workflow document。
- 冻结执行固定一份内存中的计划及输入，不会让后续图编辑自动成为普通执行的当前状态。当前 `freeze` 会拒绝需要结构化 planar image capture 的计划。
- 结果缓存是可丢弃的加速数据，不提供事务恢复、持久提交、请求历史或权威 artifact。
- 预览队列、编辑合并、面向用户的新鲜度策略和发布决策由内核之外负责。
- 未证明的可变输入可以执行，但其依赖结果不能跨运行复用。
- 磁盘持久化仅支持当前 disk-cache 实现接受的格式。缓存文件不是插件、交换或备份格式。

## 5. 后果与代价（Consequences）

保留旧快照和返回的 `Value` 会继续占用其 backing block 和租约。旧版本仍被引用时，补丁还会为变更块消耗额外预算。快照导入或补丁超过存储预算时返回 `ResourceExhausted`；调用方可释放不再使用的版本后重试。

资源准入前，缓存会回收空闲条目。驱逐和清缓存会移除复用资格，但不会使调用方仍持有的 `Value` 失效。缓存准入失败时跳过保留；若工作集本身超过 context 的受控 buffer 容量，执行返回 `ResourceExhausted`。调用方需要释放不再使用的结果以归还容量。

取消采用协作方式。已提交工作退出后才释放其所有者；只要仍有订阅者需要结果，producer 就继续运行。磁盘写入可能因 I/O 错误或队列压力被丢弃，因此后续未命中属于正常情况。`flush_disk_cache` 由调用方显式执行，结果发布不会等待磁盘写入。
