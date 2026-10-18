# 依赖数据与执行

## 范围与所有权

内核记录静态成员 tile 之间的依赖。依赖发现由 planner 负责：它在编译期调用每个算子的静态 mapper，并把结果作为固定读取授权和 block 内指令依赖保存到 `ExecutionPlan`。Run 拥有其在执行这些指令时记录的证据。`Footprint` 仍是窗口与 view 使用的精确样本几何类型；`TileSet` 为授权和证据保存规范化的成员局部半开区间。执行协议不包含样本角色标签，也不包含 exact/conservative/unknown 关系证书。

## 核心数据结构与内存

`Tensor` 组合 descriptor、授权 Footprint 和拥有的矩形 span。它验证完整覆盖、地址映射与重叠；缺失样本是错误。`restrict` 同时缩小 owner 和授权。显式 `collect` 准入复制，并检查工作量和取消。保留字节按不同物理 owner 计数。

`TileUnit` 标识 producer 记录、输出序号、成员槽和 tile 索引。`TileDependency` 保存 producer 和紧凑的成员 tile 集合；依赖记录中的空集合表示 descriptor 证据。不要把这种记录形式与空 payload 授权混淆，后者不产生证据。`TileRecord` 保存一个已执行 tile 的依赖。`TileEvidence` 冻结一次共享 stage 提交的输入和继承证据。见[全局结果](Global-Results.zh.md#tile-记录与冻结证据)。

记录索引 `0 .. steps().size() - 1` 对应 call 输出。每个具名 return 还有一条 synthetic 记录，其索引为 `steps().size()` 加 return 序号，output ordinal 为 0；`ExecutionPlan::output(name).record_index` 返回该索引。Synthetic 记录独立于产生其值的分支来标识具名 return，因此消费者可以在走过不同路径的多次运行之间比较同一条稳定记录。

## 静态映射契约

```cpp
namespace ps {
struct ResultStaticQuery {           // borrowed for one mapper call
  const ResultProgramMetadata& metadata;   // input/output schemas and grids
  const std::map<std::string, ParameterValue>& parameters;
  const PreparedOperation& prepared;       // traits and resource identities
  Backend backend;
  std::uint64_t tile_extent;
  std::uint32_t output_index;
  std::uint32_t shared_stage;              // UINT32_MAX for an output tile
  std::uint32_t tensor_slot;
  TileIndex tile_unit;
  TileRequest publication;                 // complete publication target
  std::optional<Footprint> tensor_outputs; // output samples of this tile
};
struct ResultStaticMapping {
  std::vector<TileNeed> inputs;            // {input, tensor_slot, descriptor_only, tiles}
  std::vector<ResultSharedNeed> shared_stages;  // {stage, optional tiles}
};
using OperationDependencyMapper =
    std::function<Result<ResultStaticMapping>(const ResultStaticQuery&)>;
Result<ResultStaticMapping> complete_input_dependencies(const ResultStaticQuery&);
}  // namespace ps
```

`OperationDefinition::map_dependencies` 默认为 `complete_input_dependencies`，它授权所选输出或 stage 的 input projection 中每个输入的全部成员 tile。读取更少的算子提供自己的 mapper；`ps::sdk::pointwise_dependencies` 和 `ps::sdk::tile_need_for_samples` 覆盖常见的逐点和样本区域情形。Mapper 必须是确定性的，不得执行 I/O 或启动 continuation，并且可能并发运行；registry 隔离异常和浮点环境变更。显式设为空 `std::function` 的 C++ mapper 不是有效 mapper：planning 遇到该算子时返回 `InvalidArgument`。空依赖声明是显式、成功返回的空 `ResultStaticMapping`。

Input projection 只限制 mapper 可以指定哪些端口，共享 stage 声明只限制存在哪些 stage。两者都不会创建读取边。共享 stage 的消费者必须在 mapper 中返回该 stage，不读取任何内容的算子返回空 mapping。Planner 以 `InvalidArgument` 拒绝超出 projection 或指向不存在 stage 的读取，合并重叠条目，丢弃空 payload 条目，把读取扩展到其 publication 与校验闭包，并把每条指令的 producer tile 记录为依赖。

授权必须有限且固定。数据相关的地址模式声明静态超集，例如有界偏移 gather 能到达的全部地址的并集；运行时由 CPU 或 GPU 代码在该超集内选择读取。在运行时于整段计算之间做选择需要显式 IR 分支。

## 执行与状态机

Run 固定其 plan、bindings 和 generation，并按顺序遍历 plan 的 blocks；见[Compiler 与本地执行](Compiler-and-Execution.zh.md#静态-vm-执行)。在指令第一次 poll 之前，VM 绑定所有固定授权：payload 授权成为 `ResultTensorInput` 能力，其 samples 必须已经认证；descriptor-only 授权成为 `ResultDescriptorInputs` 条目。Descriptor-only 授权只提供编译期 schema 事实，object id 为 0，不会通过 tensors、读取窗口或 native atlas 暴露 payload。共享 stage 授权成为限定在授权 tile 内的 stage Result 投影。

返回 `ResultProgramNeed` 的 poll 声明其请求位于这些授权之内。VM 对每个请求条目计入一单位 Run work，以 protocol `InvalidArgument` 拒绝超出授权的请求，然后再次 poll。该声明从不计算、等待、收窄授权、清空能力，也不把 descriptor-only 访问升级为 payload。C 的 `need_tiles`、`need_shared_stage` 和 `need_shared_tiles` 行为相同。

共享 stage 作为普通指令运行。在一次 block activation 内，每个 call 的每个 stage 只有一份共享 stage 状态；进入 block 时清空该状态，因此每轮循环都会重新开始其共享 stage。共享 stage 可以在多次 poll 中提交多个完整 tile，每次提交在验证后原子地公开 payload、覆盖和冻结证据；失败时该事务的任何内容都不安装。普通 tile 发布必须完整且精确覆盖其自身的成员 tile，其他成员为空。已准备的 publication scope 独立于执行控制 bundle 与 prefix 的可见性。继承证据从不授予上游读取。

每条已执行指令记录一个 `TileRecord`，其中包含本次 activation 中迄今所经过分支条件的控制证据、所读取 producer tile 的记录、外部输入 tile，以及 block 参数携带的证据。同一外部来源经由多条内部路径到达某个 tile 时，记录把这些依赖合并为一个条目。每个 Run 都从固定授权重建这些证据，从不从更早的 Run 或缓存 witness 复制。

内核没有自动输入 view 准备策略。算子显式使用 `to_whole`、`to_tile`、owning 窗口，并在需要时使用计费的 collection 或 transfer。由于授权在第一次 poll 前已经绑定，`to_whole` 在第一次调用时就可能返回其就绪分支。Callback query 和服务表只借用到 callback 返回。由窗口或算子状态显式保留的 owner 遵循各自的释放生命周期。

## C 分阶段程序契约

Result ABI 7 是 C 算子接口。导入器要求精确的表与记录大小，复制已验证的元数据，并在 callback 或算子状态存在期间保留模块。可选的 `map_dependencies` hook 接收 `ps_result_static_query_v7` 和同步的 `ps_result_static_mapping_sink_v7`；hook 为 NULL 时选择完整的 projection 输入。运行时 C 服务提供静态 schema、授权声明、owning 窗口访问和事务式发布。见[Plugin ABI](Plugin-ABI.zh.md)。

即使 callback 忽略服务失败，失败仍保持 sticky。Poll 准入是非阻塞的：并发或重入 poll 不进入 callback，也不改变已准入调用。Factory、poll 和 mapper 都有异常隔离。`bad_alloc` 映射为 ResourceExhausted；其他异常映射为 OperationFailed/HostException，更早的服务失败保持优先。

### C checkpoint 服务

普通的纯确定性 tile 指令可以使用 `checkpoint_before`、`checkpoint_read` 和 `checkpoint_publish`。共享 stage 指令不暴露 checkpoint 或 block callback，带共享 stage 读取的指令在可选复用中不视为纯。资格还取决于上游纯度、回退状态、缓存工作量和容量；见[全局结果](Global-Results.zh.md#checkpoint)。

Checkpoint 查找返回不大于上界的最大 sequence，未命中时返回 handle 零。其 handle 只在当前 callback 内借用。State 是一个已 seal 的普通 Result，其 schema 只有一个完整覆盖、非空间、无 batch 的 Tensor 成员，且没有 publication scope。`create_block_state` 复制已检查的规范字节；得到的算子拥有 handle 跨 poll 存活，直到释放或状态销毁。发布 checkpoint 不消耗该 handle。

`checkpoint_read` 按逻辑顺序复制一个正长度、位于范围内的字节区间，包括跨元素边界的区间。它检查工作量、容量和取消，不暴露存储指针。Checkpoint scope 是内容 identity，因此位于不同物理 step 的等价 call 共享同一 scope。恢复 checkpoint 只恢复状态字节：当前指令保留自己的固定授权和证据，任何 witness 都不会替换它们，也不会授予来源读取。可选容量拒绝变为未命中，不改变该指令。

## 精确 demand 与不可变 binding replacement

`DemandQuery` 把名称映射到 `TileRequest`，后者按成员槽选择紧凑 tile 集合。`open_demand(plan, bindings)` 捕获 plan 和不可变 Results。每个请求保留其 generation 和所请求的成员 tile。`execute_fragments` 使用显式 frozen bundle，并在执行前用 `select_outputs` 过滤 frozen plan；请求必须是该 plan 已编译输出请求的子集。Descriptor 访问不需要产出样本。

Replacement 在发布前验证全部 bindings 和静态 schema。失败时保留旧 generation。与发布或 replacement 竞争时按规定返回 Stale；取消优先。Frozen bundle 不受后续编辑影响。`release` 移除订阅但不取消活动请求；`cancel` 请求取消，并清除保留的发布与 bundle，但不等待活动调用。这些调用协作退出；context close 会等待它们。Context shutdown 在 worker 退出前排空活动 demand 调用。Handle 既不拥有 worker，也没有单独的 payload cache。

变更内容按 plan 网格在观察到的成员 tile 上比较。仅有额外提交覆盖、descriptor revision 或 producer 进度并不意味着内容被编辑。Schema 改变需要重新 preparation 和编译。证据恢复不会恢复读取权限。

## Result block 状态转换

Block 服务消费一个显式的、已 seal 的单 Tensor 状态，并返回该状态或新状态。Block callback 既不声明授权，也不发布算子输出。Block 复用要求纯确定性的 tile 指令；它可能因容量或工作量上限而放弃，也不替代共享 stage 作为每次 activation 只计算一次某个值的方式。

### 跨输出纯 block 共享

OperationTraits 29 保留可选的 `share_blocks_across_outputs`，供纯确定性算子使用。Block 命名空间绑定已准备的算子/输出契约、输入元数据、参数和资源。每次转换还绑定其显式范围、mode 和传入状态。兼容的已完成转换可以复用；活动 callback 不通过该缓存共享。已发布输出 identity 和 checkpoint scope 保持分离。

## GPU 片段传输与执行

[Fragment Atlas](Fragment-Atlas.zh.md) 说明精确授权的传输及其独立的物理目录几何。[GPU Discovery](GPU-Discovery.zh.md) 说明在已编译授权内的有界选择。Native 服务需要设备，遵守同步规则，并在完成前保留 owner。服务声明和 CPU mock 不构成原生执行证据。

## 指令执行与退役

Coordinator 同一时刻只执行一条指令。它把该指令的 start 和 poll callback 提交到 CPU pool 或 GPU lane，等待每个 callback 退役，并在下一条指令前应用其结果；`cpu_staged_tiles` 的 poll 在 coordinator 上运行并使用 CPU tile 服务。已提交的 callback 在 coordinator 合并之前保留其状态和观察值。取消或 shutdown 会在释放借用状态之前排空已提交的 callback。

已提交 tile 和保留的读取窗口有各自独立的 backing owner。共享 stage 状态、记录和 payload 在其 owner 退役前都计入 Root。Poll 计时包含排队等待，不是纯计算时间。

## 限制与失败处理

Tile 算术、区间规范化、授权与证据元数据、callback 状态、工作量和 payload 都有显式上限。Planner 施加 `maximum_instructions` 和 `maximum_dependency_edges`；VM 施加 `maximum_dependency_work`、每条指令的 poll 上限和授权声明上限。耗尽返回 ResourceExhausted，取消返回 Cancelled，格式错误或未授权的请求在验证时失败。不存在动态 End、隐式零填充、隐藏上游获取或自动稠密物化。
