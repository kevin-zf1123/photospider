# ADR 0018：冻结输入并复用精确局部结果

- 状态：Accepted
- 英文权威文档：[ADR 0018](../0018-local-result-caches-and-frozen-execution.md)
- 当前边界：0.35.0 中，原决定里的快照 facets，以及 import 拒绝图像、color-array、`ImagePlane` 和 `Mask` Tensor 的规则已被取代。快照只校验结构；运行数据以 Result 静态 Tensor 成员 绑定。

## 1. 核心摘要（TL;DR）

可变 workflow 图和编辑中的输入需要稳定的执行输入，tile 计算也需要在记录的依赖未改变时复用 tile。内核拥有不可变输入快照、冻结执行包和有界的派生结果缓存。内容身份与 tile 依赖记录允许复用，同时缓存仍只是加速器。

## 2. 架构心智模型（Mental Model & Intuition）

`InputSnapshotStore` 类似带版本的分块存储：补丁只复制与其相交的块，其余块继续由旧版本共享。冻结执行固定一份已编译计划、对应注册表及不可变绑定，因此后续图编辑不会改变该次运行。

```text
图 + 绑定 --freeze--> 冻结计划与输入所有者
                           |
                  请求的 成员 tile
                       /        \
                   缓存命中      缓存未命中
                      |            |
                      |      在本次 Run 中计算
                      |            |
                      +------ 已验证不可变结果
                                  |
                         内存结果缓存
```

`ExecutionContext` 拥有 worker 资源和缓存条目。若返回的 `Tensor` 仍持有结果存储及预算租约，结果可以比 context 活得更久。预览策略、事件队列、发布仲裁和界面呈现由应用代码负责。

## 3. 契约规约与接口（Formal Contracts & APIs）

```cpp
struct InputSnapshotStoreConfig {
  std::uint64_t maximum_bytes = 256U * 1024U * 1024U;
  std::uint32_t block_size = 128;
  std::uint64_t maximum_blocks = 65536;
};

class InputSnapshotStore {
 public:
  Result<InputSnapshot> import_tensor(const Tensor&, const SnapshotAccessOptions& = {}) const;
  Result<InputSnapshot> patch(const InputSnapshot&, const Tensor& replacement,
                              const SnapshotAccessOptions& = {}) const;
  std::uint64_t live_bytes() const;
};

struct ExecutionContextConfig {
  std::uint32_t cpu_workers = 0;
  bool gpu_enabled = false;
  std::uint32_t maximum_queued_tasks = 1024;
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
  std::uint64_t result_cache_bytes = 0;
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

快照保存任意内置元素类型的完整 rank-1..8 `Tensor` 输入，并保留精确样本位。Import 要求 Tensor 的 coverage 覆盖其完整 shape，且只校验这些结构，因此保存图像、颜色或 mask 样本的 Tensor 与元素类型和 shape 相同的其他 tensor 一样导入。每个快照不可变；`patch` 接受元素类型和 shape 相同的替换 Tensor，以及包含于其中的非空 Footprint，只复制相交块，所有旧版本继续可读。存储对保留版本的负载总字节数实施一个聚合上限。元数据和调用方持有的 `Tensor` 不计入该上限。提供给 workflow 的运行数据（包括图像）以 Result 绑定，样本保存在其 静态 Tensor 成员 中。

`content_identity` 对带版本的身份前缀、元素类型、shape、请求区域坐标和请求区域中的精确样本位计算哈希。分配地址、布局和分块几何不参与。读取和身份计算支持样本上限及协作式取消。读取被取消时，调用方目标缓冲区可能只写入一部分，调用方应丢弃它。

`freeze` 捕获当前有效且匹配的计划、不可变绑定和 operation registry 所有者。每个 workflow input binding 是一个 `ExecutionBinding`，指定一个声明名称并提供一个不可变 `ResultRef`。Preflight 会核验 Result binding 与声明的 schema 匹配且具有有效 final descriptor。context 使用 managed resources 时，execution admission 会核验 Result 的 root ownership；demand read 受声明 domain 和已发布 support 限制。捕获过程不调用 operation callback，并在返回前重新检查图的 currentness。图被替换或销毁后，冻结执行仍使用捕获的输入版本；普通执行仍执行其 currentness 检查。每次 execute 调用独立提供取消状态，所有者会保留到已准入 callback 退出。Result input binding 保留不可变的 `ResultRef` owner。调用方通过 `ResultRef::capture()` 捕获 Result，并使用捕获的 descriptor 调用 `ResultRef::read_tensor()` 或 `ResultRef::acquire_tensor()` 读取 tensor 样本。

已完成结果的保留是可选的，其容量是 `maximum_live_bytes` 的子限额；设为零会关闭保留。复用单位是 一个成员 tile。其键由 step 的物理身份（operation 契约和实现、tile 边长、已解析的输出 schema、相关 backend 与资源身份）、单元编号和为其记录的生产者单元身份组成，见 [ADR 0023](0023-track-dependencies-between-fixed-tiles.zh.md)；workflow 输入的身份按逻辑内容计算。编译图 revision 和无关分支不进入局部结果键。dirty 提示可以缩小计算范围，但不能在缺少内容身份时证明结果可复用。参与计算的每个 operation 都必须确定、无副作用且声明可缓存，结果才可复用。

每个 Run 自行计算未命中缓存的部分；并发 Run 中相同的请求不合并。Run 的 producer 持有冻结输入和 registry，直到已准入 callback 退出。producer 不会占用 worker 等待同一线程池的另一个 callback。只有验证成功的不可变结果才进入完成缓存。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- 快照覆盖不可变输入 `Tensor` 及其内容，不序列化 execution plan 或 workflow document。
- 冻结执行固定一份内存中的计划及输入，不会让后续图编辑自动成为普通执行的当前状态。Workflow input bindings 必须是 schema 匹配的不可变 Result owner。
- 结果缓存是可丢弃的加速数据，不提供事务恢复、持久提交、请求历史或权威 artifact。
- 预览队列、编辑合并、面向用户的新鲜度策略和发布决策由内核之外负责。
- 未证明的可变输入可以执行，但其依赖结果不能跨运行复用。

## 5. 后果与代价（Consequences）

保留旧快照和返回的 `Tensor` 会继续占用其 backing block 和租约。旧版本仍被引用时，补丁还会为变更块消耗额外预算。快照导入或补丁超过存储预算时返回 `ResourceExhausted`；调用方可释放不再使用的版本后重试。

资源准入前，缓存会回收空闲条目。驱逐和清缓存会移除复用资格，但不会使调用方仍持有的 `Tensor` 失效。缓存准入失败时跳过保留；若工作集本身超过 context 的受控 buffer 容量，执行返回 `ResourceExhausted`。调用方需要释放不再使用的结果以归还容量。

取消采用协作方式。已提交工作退出后才释放其所有者；取消一个 Run 不影响其他 Run。

Run 共享 stage 独立于可选缓存保证唯一 producer，并保留成功/失败到 Run 结束。活动 continuation 不跨 Run。Checkpoint/block/completed 复用具有独立资格，且不扩大授权；见 [Global Results](../../kernel-architecture/zh/Global-Results.zh.md)。
