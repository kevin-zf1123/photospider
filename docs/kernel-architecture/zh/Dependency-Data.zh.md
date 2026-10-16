# 依赖数据与执行

## 范围与所有权

内核记录静态成员 tile 之间的依赖。`Footprint` 仍是窗口和 view 使用的精确样本几何类型；`TileSet` 用规范化的成员局部半开区间保存请求与证据。执行协议不再使用样本角色标签或 exact/conservative/unknown relation certificate。

## 核心数据结构与内存

`Tensor` 组合 descriptor、授权 Footprint 和拥有型矩形 span。它校验完整覆盖、地址映射和重叠；缺失样本是错误。`restrict` 同时收窄 owner 与授权。显式 `collect` 准入复制并检查 work 与取消。保留字节按不同物理 owner 计数。

`TileUnit` 标识 producer 输出、成员 slot 和 tile 索引。`TileDependency` 保存 producer 和紧凑成员 tile 集合；依赖记录中的空集合表示 descriptor 证据，不能与不产生任何证据的空 payload Need 混淆。`TileEvidence` 冻结一次共享提交的局部 Need 和继承证据。`TileRecord` 保存普通 tile 的局部与继承依赖。见 [Global Results](Global-Results.zh.md)。

完整成员覆盖在等待登记、依赖保存、反向索引和 checkpoint 证据中保持紧凑。实际任务按有界批次展开。共享证据以不可变对象复用，不为每个消费输出 tile 复制。几何与元数据计入 Root；实际读取、比较和计算按发生量消耗 work。

## 执行与状态机

Run 固定 plan、binding 和 generation。普通 tile actor 与 Run 共享 stage 都是 continuation，通过非阻塞 Need 暂停；worker callback 不同步等待上游生产。Stage 声明、schema、输入投影和资源上限在执行前准备。每个 Run 的原始节点和声明 stage 只有一个 producer，其成功或失败保留到 Run 结束。

普通执行即使 plan 输出选择为空也推进 mandatory stage。显式 `DemandQuery` 仅在全部选择为空时跳过它们。共享等待登记与完成检查协调进行，避免丢失唤醒；递归及 shared/tile 混合等待环失败。取消停止新工作、唤醒等待者并排空已准入 callback。

普通 tile 发布必须完整，精确覆盖所选成员 tile，其他成员为空。共享 stage 可以跨多次 poll 提交多个完整 tile。校验通过后，每次提交原子地公开 payload、coverage 和冻结证据；失败不安装该事务内容。准备后的发布范围独立控制 bundle 和 prefix 可见性。继承证据不授予上游读取权限。

不存在自动输入 view preparation 策略。算子显式使用 `to_whole`、`to_tile`、拥有型窗口，以及必要时计费的 collection 或 transfer。Callback query 和服务表只借用到该 callback 返回；窗口或算子状态显式保留的 owner 遵循自己的释放生命周期。

## C 分阶段程序契约

Result ABI 5 是 C operation 接口。Importer 要求表和 record 大小精确匹配，复制已校验元数据，并在 callback 或算子状态存活时保留 module。C 服务提供静态 schema、成员 tile Need、显式 descriptor-only 请求、共享 stage Need、拥有型窗口和事务发布。见 [Plugin ABI](Plugin-ABI.zh.md)。

Callback 即使忽略服务失败，失败仍保持 sticky。Poll 准入非阻塞：并发或重入 poll 在不进入 callback、不改变已准入 invocation 的情况下失败。Factory 和 poll 有异常栅栏。`bad_alloc` 映射为 ResourceExhausted，其他异常映射为 OperationFailed/HostException；更早的服务失败仍优先。

### C checkpoint 服务

普通纯确定性 actor 可以使用 `checkpoint_before`、`checkpoint_read` 和 `checkpoint_publish`。共享 stage phase 不提供 checkpoint 或 block callback。普通 actor 处理直接 shared Need 后跳过可选 checkpoint/block 复用及 completed-result 缓存。资格还取决于上游纯度、fallback 状态、缓存 work 和容量；见 [Global Results](Global-Results.zh.md)。

Checkpoint 查找返回不超过上限的最大 sequence，未命中返回零 handle；该 handle 只在当前 callback 借用。状态是已 seal 的普通单 Tensor Result，完整覆盖、非空间、无 batch、无 domain 和 publication scope。`create_block_state` 复制受检的规范字节；生成的算子拥有型 handle 可跨 poll 保留，直到释放或状态销毁。发布 checkpoint 不消耗该 handle。

`checkpoint_read` 按逻辑顺序复制正长度的合法字节区间，可跨元素边界。它检查 work、容量与取消，不暴露存储指针。恢复以事务暂存依赖证据、祖先、事实和 association。可选容量拒绝成为 miss，不改变 actor。成功恢复不填充当前输入 capability 或授予样本访问，必须重新提出当前 Need。

## 精确 demand 与不可变 binding replacement

`DemandQuery` 把名称映射到 `TileRequest`，按成员 slot 选择紧凑 tile 集合。`open_demand(plan, bindings)` 捕获 plan 和不可变 Result。每个请求保留其 generation 和成员 tile 选择。`execute_fragments` 使用显式冻结的 bundle。Descriptor 访问不需要样本生产。

Replacement 在公开前校验全部 binding 和静态 schema；失败保留旧 generation。竞争中的发布或替换按契约返回 Stale，取消优先。冻结 bundle 独立于后续编辑。`release` 移除订阅但不取消活动请求；`cancel` 请求取消并清除保留发布和 bundle，不等待活动调用；这些调用协作退役，context close 等待其结束。Context 关闭在 worker 退役前排空活动 demand 调用。Handle 不拥有 worker 或独立 payload cache。

内容变化按 plan 网格上已观察的成员 tile 比较。增加已提交覆盖、descriptor revision 或生产进度本身不表示内容编辑。Schema 改变需要重新准备和编译。恢复证据不恢复读取授权。

## Result block 状态转换

Block 服务消费显式 seal 的单 Tensor 状态并返回原状态或新状态。Block callback 不发出 Need、不发布算子输出，后续输入由 poll 请求。Block 保留是可选的，除纯度外还要求算子可缓存，并可因容量/work 限制拒绝。它不保证 Run 内唯一 producer，该保证由共享 stage 提供。

## GPU 片段传输与执行

[Fragment Atlas](Fragment-Atlas.zh.md) 描述精确授权传输及其独立物理目录几何。[GPU Discovery](GPU-Discovery.zh.md) 描述有界成员区间发现。Native 服务要求设备、遵循同步并保留 owner 到完成。服务声明和 CPU mock 不构成 native 执行证据。

## 就绪 Atomic 执行组

协调器维护 Root 拥有的 actor 和待处理工作，推进可运行 Need cursor，每波提交至多 `maximum_parallelism` 个 callback，并在其退役后应用结果。CPU 分阶段协调和 GPU lane 保留独立角色。已提交任务保留状态和观测，直到协调器合并。关闭时先排空所有任务，再释放借用状态。

已提交 tile 和保留读窗口各自拥有 backing owner。共享任务、等待记录、终态和 payload 在 owner 退役前计入 Root。Poll 计时包含队列和 wave 等待，不是纯计算时间。

### 跨输出纯 block 共享

OperationTraits 27 保留纯确定性算子的可选 `share_blocks_across_outputs`。Block 命名空间绑定准备后的 operation/output 契约、输入元数据、参数和资源；每次转换还绑定显式 range、mode 和输入状态。兼容的已完成转换可以复用，活动 callback 不通过该缓存共享。公开输出 identity 与 checkpoint scope 保持独立。

## 限制与失败处理

Tile 运算、区间规范化、等待/证据元数据、callback 状态、work 和 payload 都有显式上限。耗尽返回 ResourceExhausted，取消返回 Cancelled，非法或未授权请求无法通过校验。不提供动态 End、隐式填零、隐藏上游获取或自动稠密物化。
