# 精确依赖数据

英文权威文档：[Dependency-Data.md](../Dependency-Data.md)。

## 模块边界与职责

```text
具名 Result roots
        |
        v
Root-owned Actor registry -> ready frontier -> context worker queues
        ^                                     |
        |          Need 或 publication         v
        +------ coordinator 满足 Needs <----- callback
                       |
                 Root resource admission
```

Coordinator 推进 dependency-ready Result Actor；等待上游 producer 时不占 worker。就绪 callback 竞争现有 worker 容量和 Root resource limit。

## 核心数据结构与内存布局

```cpp
class Footprint final {
 public:
  static Result<Footprint> from_regions(
      std::vector<std::uint64_t> shape, const std::vector<Region>& boxes,
      const FootprintLimits& limits = {},
      std::uint64_t* consumed_work = nullptr);
};

class ValueFragments final {
 public:
  static Result<ValueFragments> create(
      ValueDescriptor descriptor, std::vector<ValueFacet> facets,
      Footprint authorized, const std::vector<Value>& fragments,
      const FootprintLimits& limits = {}, ResourceBindings resources = {});
  Status read(const std::vector<std::uint64_t>& coordinate, void* destination,
              std::size_t size) const;
};

```

`Footprint` 拥有规范区域元数据；`ValueFragments` 持有片段 owner；证书保存逐观察依赖行，不持有像素。

Footprint 表示非零 rank 1..8 逻辑域中的精确集合，以规范不交矩形表示 Empty、All 与非连续覆盖。递归轴扫描仅在后续轴集合相同时合并相邻区间，因此插入顺序、重复矩形与不同分块不影响集合相等。即使完整域元素乘积溢出，All 仍保持压缩。并、交、差要求相同域并保留空洞。`tile_cover(geometry)` 返回 ceil-divided tile 域中实际相交的 tile 坐标，不把 tile 内所有样本标为有效；`visit` 在显式样本上限内按 row-major 顺序逐个访问。

FootprintLimits 对每次集合操作限制候选工作与矩形条目，包括重复构造工作。超限返回 ResourceExhausted，取消返回 Cancelled，两者均不等于 Empty 或 bbox 近似。复合调用方还需限制跨调用的总工作和元数据；单次集合额度不是完整执行预算。

ValueFragments 保存完整 descriptor/facets、授权 Footprint 与有 owner 的矩形 Value。构造时裁剪输入覆盖，拒绝缺失样本和不一致重叠；同 owner 的等价 origin-relative mapping 去重。Generic 数组允许任意样本子集。经验证的 ColorArray tuple 必须覆盖完整 C。结构化 tensor 数据不进入此 Value 容器，而由 typed Result tensor slot 携带，并通过 `ResultTensorInput` 读取。`read` 通过 Value 的 checked signed-stride 地址复制实际 dtype 宽度；空洞、未授权地址或错误宽度均失败，不会同步取数或补零。`restrict` 同时限制 owner 与覆盖；`collect` 在通过完整矩形检查后使用 BufferAllocator 分配。retained capacity 统计实际唯一 storage owner，与有效区域大小分开。

## 调度与状态机

`OperationTraits` 使用语义版本 25 和 Result protocol 2。Result traits 描述 failure delivery、continuation/workspace 上限和 typed output ports。除了声明的 Whole RequestFailureOnly 情况，staged Result program 必须 deterministic 且 side-effect-free。编译器检查所请求 output 和 side-effect root 可达的执行边。Excluded ports 保留静态 metadata，不会运行 producer。Result plan 保留未解析 input demand，不生成矩形近似。普通 execution 会将每个非 side-effect-free operation 作为强制 root 执行，包括没有具名 output 请求的 operation。Fragment execution 仅在 query 包含非空 demand 时执行这些 roots；完全空的 query 会跳过无关副作用 roots。同一 Run 内，副作用 tensor output 的内部 actor identity 使用 slot 0 和完整 coverage，后续请求复用该 actor。该规则不会扩大其他 slots 的授权：每个 slot 仍需自己的 Need 与有效 publication coverage。副作用工作不会跨 Run 共享或缓存。强制 root failure 或 cancellation 会使整个 Run 失败。

每个 `ps_result_output_v3` 还携带不可变的 view policy 和可选 output payload bound。`PRESERVE_VIEWS` 适用于 CPU 的 Atomic output；`REQUIRE_INPUT_VIEWS` 隐含该 flag，且要求 CPU Whole execution。在 compiled structured CPU Whole execution 中，coordinator 会验证 payload Tensor Need，在 computation poll 前准备授权 boxes，再证明 affine views 或采用 Auto collection。Direct registry start 不会自动生成这一 Need preparation。`PAYLOAD_BOUND` 启用 `maximum_output_payload_bytes`，值为零时表示显式零上限；未设置该 flag 时字节字段必须为零。该 bound 支持 CPU Whole 或 staged output，并在 publication 时检查。Input backing、workspace、metadata 与 work 仍分别计入 Root。Importer 会拒绝未知或不一致的 flags；C `start` 和 `poll` 的 `query.output` 都会反映注册时的 policy，metadata resolver 可以推导 output port/schema，但不能改变 policy。这些尾部字段改变嵌套 output record 布局，operation C ABI 仍为版本 3。精确的 `struct_size` 校验会拒绝旧的 prefix record，因此 C module 必须重建；不提供兼容 shim。

Result discovery、poll 数、continuation state、output/scratch 和 GPU request table 都有有限上限。读取、显式 work 扣除和分配失败具有粘滞性，即使 callback 忽略返回 Status 也会失败。某个 observer 抛异常不会阻断其他 observer。首个记录的 failure 仍是最终报告原因；host 另外记录是否禁止 CPU fallback。只有 origin/scope 合规且未限定的 `BackendUnavailable` 可继续参与重试判断。已接纳调用失败时，continuation state 只销毁一次并释放 input owners，随后应用 cancellation。Poll admission 非阻塞：并发或重入调用返回 `InvalidArgument/Protocol/Group`，不会进入 callback，也不会触发 failure observer 或 failure latch，也不会修改活动 phase、首个 failure 或 work count。借用的 phase/query/service references 在 callback 返回后失效；不得并发移动或销毁 continuation，且 continuation 必须使用授予的 allocator。`start_result` 在 factory 前后检查 cancellation；factory 进入后观察到 cancellation 时，取消优先于 factory、allocation 和返回 state 校验错误。任何返回的 state 都会在函数返回前销毁，definition lease 保持到销毁完成。

Structured Result execution 使用 Root-owned Actor registry 和 pending-task list。Coordinator 通过现有 CPU worker queue 或所选 native GPU queue 提交就绪 callback。Worker 不等待上游 Result producer。每个 stage 在同一 Root budget 下非阻塞 admission；seal 归还未使用容量，保留中的 state、input 和 output leases 继续计费。无法容纳最小工作集的请求返回 `ResourceExhausted`。Result callback 通过 `ResultTensorInput` 读取授权样本。Result callback 分配只有经 host 受控 allocator 导入后才能逃逸。

Frozen execution 保留捕获的 graph 与 input owners。Root Actor 以有界 callback wave 提交；每 wave 的已提交 callback 全部退休后，coordinator 才应用结果并推进下一波。活跃 demand 重用与完成结果缓存见下文及[缓存模型](Cache-Model.zh.md)。C++ staged GPU fragment access 使用[Fragment Atlas](Fragment-Atlas.zh.md)，有界 native discovery 见[GPU Discovery](GPU-Discovery.zh.md)。

## C 分阶段程序的内存与错误路径

Result ABI 3 C operation table 是唯一的 C staged-operation 接口，提供 typed Result ports、callback-scoped services、分阶段 Needs 和 Result publication。Loader 会校验并复制有界 table，再原子发布 definitions；只要导入的 callbacks 仍可能运行，就保留 module owner。C++ API 通过 `ResultContinuation` 使用同一 staged Result model；Result ABI 3 仍是唯一 C operation-plugin interface。

阶段服务提供 checked read，包含实际 dtype、origin、有符号 stride、字节跨度和授权区域。缺失样本不会触发隐藏的上游读取。无效 handle 和被忽略的服务失败保持 sticky。

| 资源 | 有效期与所有权 |
| --- | --- |
| scratch 指针 | 在当前 poll callback 内有效，poll 返回即失效。 |
| output 指针 | 成功发布时立即失效；未发布时最晚在 poll 返回时失效。 |

[`tests/integration/test_result_c_block.cpp`](../../../tests/integration/test_result_c_block.cpp) 覆盖 C block-state handle 生命周期和有序 Float64 fixture scan。Finite input/sum 检查属于该 fixture；通用 Float64 block service 传输 canonical bytes。测试还检查失败清理与 Root ownership。Block-cache reuse 和 hit 数属于实现细节，不是性能契约。

### C checkpoint 服务

C Result services `checkpoint_before`、`checkpoint_read` 和 `checkpoint_publish` 为纯 staged operation 提供 Result checkpoint 路径。查找返回不大于请求上限的最大已存 sequence。未命中时成功返回 handle 0；命中时返回 opaque handle、sequence 和 canonical tensor byte size。Opaque handle 只可在当前 start 或 poll callback 期间使用；其 callback-epoch entry 会保留 state 和 witness。

Checkpoint state 是 sealed generic `CompleteBundle` Result，包含一个完整覆盖的 tensor，不含 fields、metadata 或 domain；tensor 不带 facets、groups、batch axes 或 spatial layout。`create_block_state` 将字节 payload 复制到该 Result，并按 tensor 的逻辑 samples 校验 payload 大小。调用方可将返回的 operation-owned handle 用作 block state 或传给 `checkpoint_publish`。Block-state handle 会跨 poll 保留，直到调用方释放或 callback state 销毁。可选 retention 获准时，发布会保留 Result 与成功 dependency witness。此调用不会消费 handle；调用方仍须通过 `release_block_state` 释放它。

`checkpoint_read` 按 canonical row-major logical sample 顺序复制正长度且范围有效的字节区间。区间可以从一个 element 中间开始或结束，也可以跨越 sample 边界。Service 不暴露保留存储的指针，会计入读取 work，并检查 Root capacity 与取消状态。恢复 checkpoint 会重新导入成功 dependency history、facts、ancestry 和 association；它不会恢复 Result input capabilities，也不会授权新的输入读取。Callback 必须另行请求当前 inputs。Checkpoint API 要求 phase 非零且 operation deterministic、side-effect-free。Block callbacks 和 GPU discovery 不能调用这些服务。无效 handle、区间、scope 或禁止的调用会成为 sticky failure。

`test_result_c_checkpoint.cpp` 还检查 finite-prefix fixture：有效前缀可以发布，后续请求若遇到 infinity、NaN 或溢出求和则失败。有限值约束属于该 fixture 的 scan 逻辑；generic Float64 checkpoint service 只复制 canonical bytes，本身不会拒绝非有限值。

Result C module 与 C++ Result continuation 使用相同的结构化输入和 publication 模型。原生插件仍是受信任的进程内代码；指针 metadata 校验不提供内存隔离。C++ Result continuations 与 Result C modules 使用相同 structured input 和 publication model。

`test_result_plugin.cpp` 覆盖 Result ABI 3 C module 导入、callback 执行、sticky service failure 和 plugin owner 生命周期。其 `c_whole_view_policies()` 检查所选 output flags 与 payload bound 在两个 C callback 阶段都可见，并覆盖 Auto collection、strict view 拒绝、兼容/负 stride/零 stride backing、output cap 和 owner 退役。Fixture 还注册 malformed modules，覆盖未知或不一致的 policy flags、bound 声明不匹配、不支持的 view 组合以及截断的 output record。Result checkpoint 和 block fixtures 覆盖对应的 scoped services。


## 精确 demand 与 binding generation 状态机

`ExecutionContext::open_demand(plan, bindings)` 固定当前图契约及不可变 Result bindings。`DemandHandle` 副本共享 bundle 和 generation。`request({name: Footprint})` 返回具名 ResultRefs、诊断和结构证据；每个请求保留 original Q。Result continuation 通过 typed Needs 接收 selected tensor slots。未请求的名称不出现在结果中。`execute_fragments(frozen, Q)` 对固定 bundle 提供相同精确查询，结果 generation 为零。

替换在发布前验证全部 bindings。样本、metadata 限额或验证失败保留旧 generation； 与 request 发布或其他 replacement 竞争时返回 Stale，供调用者重试。已完成替换前捕获的 latest 请求不能发布到新 generation，取消优先于 Stale。`freeze()` 固定当前 bundle，独立于后续编辑；`release(Q)` 删除一个精确订阅，但不取消活跃请求，后者仍可重新发布该订阅；`cancel()` 停止该 handle 并退休其 publication 和 bundle。Context 析构先取消并排空活跃 demand 调用，再退休既有 workers。直接 context 调用不能与析构竞争，已有 handle 调用可与析构竞争。 输入 owner 析构在 publication mutex 之外执行。

`maximum_demands` 限制存活且未取消的 handle，范围 1..65536、默认 1024；活跃 demand 调用上限为既有队列容量加 CPU worker 数再加一个 coordinator 槽。`DemandConfig::maximum_metadata_entries` 限制每个 handle 保留的 query/evidence/dirty metadata，范围 1..1048576、默认 65536。Handle 不拥有 worker 或像素 cache。Structured Result producer 使用既有 CPU/GPU pool、WaitingAdmission 和计费 allocator。已完成 Result content reuse 使用可选像素 LRU 和有界结构证明，见[缓存模型](Cache-Model.zh.md)。GPU transport 见 [Fragment Atlas](Fragment-Atlas.zh.md)，有界 discovery 见 [GPU Discovery](GPU-Discovery.zh.md)。

`ExecutionContextConfig::maximum_result_checkpoint_scopes` 只限制 Result checkpoint-scope directory（1..1,048,576，默认 65,536）。

Context 关闭时先排空 demand calls 和已提交 callback，再销毁 workers。

## Result block 状态转移

`ResultProgramPhase::block` 和 C block service 作用于一个显式、已 sealed 的 `CompleteBundle` tensor state。Block callback 读取该 state，并返回它或一个新建的 state；它不发出 Need，也不发布 operation output。Producer 通过 poll callback 请求之后的 tensor 样本。Checkpoint 与 block 服务相互独立：checkpoint 恢复可导入成功的 dependency evidence，block state 只携带转移数据。可选的 block-state 保留可能跳过一次转移，或在资源限制下重新计算。其命中次数和转移复用属于实现细节，不构成性能承诺。

[`tests/integration/test_result_c_block.cpp`](../../../tests/integration/test_result_c_block.cpp) 覆盖 C block-state handle 和 Float64 fixture scan，包括 state 生命周期与失败清理。Finite input 和 finite sum 规则属于该 fixture；通用 state service 传输 tensor 字节。

## GPU fragment 传输与执行

[Fragment Atlas](Fragment-Atlas.zh.md) 说明已实现的精确 atlas/mask 目录、SDK MSL lookup helper 及原生传输验证。C++ staged GPU 执行已接入，[有界 discovery](GPU-Discovery.zh.md) 已实现。

同步 GPU producer 也通过既有 native worker 执行，实际容量与 CPU 回退见 [Fragment Atlas](Fragment-Atlas.zh.md)。

`BufferAllocator::same_owner` 只比较 accounting-domain identity，不比较 quota 或 provenance，也不授予 payload 访问权限。Callback exception fence 会保留共享 allocation、work 或 I/O 错误中的首个错误。C++ Result factory 和 poll 将 `std::bad_alloc` 转为 `ResourceExhausted`，将标准异常转为带 `HostException` 的 `OperationFailed` 并保留诊断（`what()` 为 null 时 message 为空），将非标准异常转为带固定 `HostException` 诊断的 `OperationFailed`。Phase 已记录的 failure 优先于随后抛出的异常；host failure observer 抛出的异常不会替换 producer 已选定的错误。

## 就绪 Atomic 调度与 admission

Coordinator 在推进 Actor 前先登记显式请求的 named Result roots。Root-owned pending-task list 会保留每个 queued 或 submitted Actor，直到 dispatch 被拒绝或 callback 退出。Dependency frontier 有 queued work 后，coordinator 最多提交 `maximum_parallelism` 个 callback，等待整批已提交任务退出，再串行应用 phase results 并推进下一 frontier。CPU staged-tile controller 与 GPU dispatch 保留现有路径；Actor wave scheduler 不会独立驱动 dependency graph。

Producer 响应 Need 前，coordinator 先验证完整 Need envelope。每个 input 有独立供给 cursor，coordinator 会在可推进的 input 之间轮转。一次 traversal 中每个 Actor 最多访问一次，从而避免 peer 等待上游时反复递归遍历深链。每个 submitted task 持有其 stage-local transfer、tile 和 native observations，driver 退休 task 时才合并。Checkpoint 与 block-cache 的 metadata 锁只保护有界查找或提交区间，不覆盖计算或测试 hook。

Shutdown 和同步 Run 完成会先排空所有已提交 task，再释放借给 callback 的状态。

Whole result 的释放采用保守条件。只有完成的 Whole Actor，并且其每个 tensor slot 都有完整 coverage，才能将别名 step 标记为完成。Liveness 只统计从请求根可达的 step，并沿各 step 选定的 `input_indices` 传播。Coordinator 仅在所有 alias 都没有剩余 consumer 或 named-output pin，且没有 pending work 时，清除 published object。Dynamic Dependency、Prefix、Empty 的一次部分发布不会据此标记 step 完成。已发布 Result 和保留的 read window 分别持有自己的 storage owner。Poll timing 从 phase 创建计至 driver 退休，包含 callback queue 和 wave 等待，不是纯计算时间。

`test_result_dag_parallel` 检查 callback wave 上限和取消；source backing 在独立 left/right consumer 读取期间保持存活，并在 join 发布前检查 source owner 已释放。Named-output case 并发运行 left/right producer 并返回分别拥有的 Results；单输出 case 将最终值 `7` 保存在逃逸的 `ResultRef` 中，ExecutionContext 退出后仍可读取，并在该引用释放前保留 8 字节 Payload。Held-output admission 失败会释放资源，后续 Run 可恢复。

已完成兄弟值保持计费，直到登记的 consumer 取走。Shared backing 使用自身 storage domain 和唯一 owner accounting。

### 跨 output 的 pure block sharing

`share_blocks_across_outputs` 是 `OperationTraits` v25 的可选字段，默认关闭，适用于 pure deterministic Atomic Result operations。启用后，runtime 可在满足当前输出与输入契约的 block 状态转移间共享 retention；状态不兼容或 retention 不获准时仍会重新计算。Block retention 与完整 Result cache、checkpoint 和公开输出分开管理。命中次数与复用范围是当前实现细节，不构成性能保证。

## 限制与错误处理

精确集合构造、证书增长、Result discovery、callback 阶段和保留的 demand metadata 分别受有限上限约束。非法 domain 或 protocol envelope 返回错误；资源上限耗尽返回 `ResourceExhausted`；取消返回 `Cancelled`。借用输入在调用期间必须保持有效；是否可重试由所属 execution API 的状态契约决定。
