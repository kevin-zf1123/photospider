# 精确依赖数据

英文权威文档：[Dependency-Data.md](../Dependency-Data.md)。

数据 API 表示精确覆盖和不可变 dependency evidence。Coordinator 调度 Result 工作并负责 callback/error 交付。`ValueFragments` 表示 typed backing coverage，`DependencyCertificate` 保存 execution evidence。Operations 使用 Result protocol 2 并发布 structured Results。

## 模块边界与职责

Structured Result execution 组合精确逻辑覆盖、typed input capability、宿主持有的 continuation 和 Root resource admission。`Footprint` 持有覆盖 metadata，`ValueFragments` 保留 typed backing owners，`DependencyCertificate` 保存 relation rows 但不持有像素。Result continuation 使用 `ResultObjectNeed`、`ResultTensorNeed`、`ResultIoRequest` 和 `ResultRelation`。

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

class DependencyCertificate final {
 public:
  static Result<DependencyCertificate> create(
      std::string identity, Footprint coverage,
      std::vector<std::vector<std::uint64_t>> input_shapes,
      std::vector<AtomCertificate> rows, const FootprintLimits& limits = {});
};
```

`Footprint` 拥有规范区域元数据；`ValueFragments` 持有片段 owner；证书保存逐观察依赖行，不持有像素。

Footprint 表示非零 rank 1..8 逻辑域中的精确集合，以规范不交矩形表示 Empty、All 与非连续覆盖。递归轴扫描仅在后续轴集合相同时合并相邻区间，因此插入顺序、重复矩形与不同分块不影响集合相等。即使完整域元素乘积溢出，All 仍保持压缩。并、交、差要求相同域并保留空洞。`tile_cover(geometry)` 返回 ceil-divided tile 域中实际相交的 tile 坐标，不把 tile 内所有样本标为有效；`visit` 在显式样本上限内按 row-major 顺序逐个访问。

FootprintLimits 对每次集合操作限制候选工作与矩形条目，包括重复构造工作。超限返回 ResourceExhausted，取消返回 Cancelled，两者均不等于 Empty 或 bbox 近似。复合调用方还需限制跨调用的总工作和元数据；单次集合额度不是完整执行预算。

ValueFragments 保存完整 descriptor/facets、授权 Footprint 与有 owner 的矩形 Value。构造时裁剪输入覆盖，拒绝缺失样本和不一致重叠；同 owner 的等价 origin-relative mapping 去重。Generic 数组允许任意样本子集。经验证的 ColorArray tuple 必须覆盖完整 C。结构化 tensor 数据不进入此 Value 容器，而由 typed Result tensor slot 携带，并通过 `ResultTensorInput` 读取。`read` 通过 Value 的 checked signed-stride 地址复制实际 dtype 宽度；空洞、未授权地址或错误宽度均失败，不会同步取数或补零。`restrict` 同时限制 owner 与覆盖；`collect` 在通过完整矩形检查后使用 BufferAllocator 分配。retained capacity 统计实际唯一 storage owner，与有效区域大小分开。

`DependencyCertificate` 保存身份、精确观察覆盖、声明输入域及每项观察的 `AtomCertificate`。`ResultRelation` 记录 typed Value、field、tensor 或 descriptor support，包括 selected slot 与 roles。显式空 row 表示已知空，缺失 row 表示未知，不能发布为完整解析。身份绑定调用方的访问、数值和错误合同及快照；这些数据类型不能证明任意 callback 遵守读取声明。

restrict(P) 拒绝覆盖域以外的 P。backward(P) 仅按端口/role 对相应 row 求取数并集， 该投影不是证书。transpose(dirty) 精确返回覆盖域内与相同端口、相交 role 的样本/tag 支持相交的观察。merge 要求身份/域一致，重叠观察的规范 row 相同。请求级失败证据及终端 RequestRecord 不属于原子成功证书。

内部 DirtyDeltaQueue 在一个证书 generation 内为每条记录保存 accumulated 和 propagated。 同一 mutex 下取 accumulated−propagated、记录已传播集合并清 queued；后到的新原子可以再次入队。依赖结构由 demand coordinator 持有，与像素缓存 owner 分离；队列自身不拥有像素或 worker。任何下游入队失败都必须令该 generation 失败。

Focused 检查为 test_footprint、test_value_fragments、test_dependency、test_dependency_dirty 及 test_input_snapshot，使用独立有限集合/可达性 oracle，覆盖未知 row、identity/swap、 role/tag 隔离、迟到 dirty、dtype/stride/owner 上限和快照 COW。通用快照身份与所有权见 [缓存模型](Cache-Model.zh.md)。

## 调度与状态机

`OperationTraits` 使用语义版本 25 和 Result protocol 2。Result traits 描述 `Atomic` 或 terminal `RequestRecord`、failure delivery、continuation/workspace 上限和 typed output ports。除了声明的 Whole Atomic RequestFailureOnly 情况，staged Result program 必须 deterministic 且 side-effect-free。编译器检查所请求 output 和 side-effect root 可达的执行边，并按每个 output 的相关 input ancestry 计算 EffectiveAtomic；RequestRecord 不得供给活跃 consumer。Excluded ports 保留静态 metadata，不会运行 producer。Result plan 保留未解析 input demand，不生成矩形近似。普通 execution 会将每个非 side-effect-free operation 作为强制 root 执行，包括没有具名 output 请求的 operation。Fragment 和 atom execution 仅在 query 包含非空 demand 时执行这些 roots；完全空的 query 会跳过无关副作用 roots。同一 Run 内，副作用 tensor output 的内部 actor identity 使用 slot 0 和完整 coverage，后续请求复用该 actor。该规则不会扩大其他 slots 的授权：每个 slot 仍需自己的 Need 与有效 publication coverage。副作用工作不会跨 Run 共享或缓存。强制 root failure 或 cancellation 会使整个 Run 失败。

Operation execution 使用 `start_result`，query 捕获所选 output 和 tensor-slot demand。Staged poll 可以先请求 Control samples，再请求相应 Data samples。它通过 `ResultObjectNeed`、`ResultTensorNeed` 和 `ResultIoRequest` 请求 Result objects、tensor samples 或闭合 I/O。成功 publication 附带 `ResultRelation`，记录实际消费的 Control 和 Data support；`Exact`、`Conservative` 和 `Unknown` 在组合及 dirty query 中保留原义。C++ RequestRecord 和 Result continuation 保留所选 output 的 captured query；Whole region rule 不会扩大 RequestRecord 的 Q。Result C table 为 ABI 2，也是唯一的 operation-plugin C 接口。C operation 在 inference 或 start 前必须经过 registry preparation。定义了 `resolve_metadata` 时，preparation 会先校验完整 inputs，再调用 resolver；固定 schema operation 则由 importer 安装的 `specialize_metadata` 校验 input prototypes 并返回声明的 outputs。Direct callers 必须先使用 registry preparation/resolution 路径。Output record 声明 `observation_kind` 和 `failure_delivery`：没有 contract-2 joint callback 的 operation 使用 RequestFailureOnly，contract-2 operation 使用 PerAtomOutcome。Singleton callbacks 仍为必需，并返回普通 C status code；typed Atom failure 和 quality attachment 通过 contract-2 joint outcome 提供。

每个 `ps_result_output_v2` 还携带不可变的 view policy 和可选 output payload bound。`PRESERVE_VIEWS` 适用于 CPU、非 joint 的 Atomic output；`REQUIRE_INPUT_VIEWS` 隐含该 flag，且要求 CPU Whole execution。在 compiled structured CPU Whole execution 中，coordinator 会验证 payload Tensor Need，在 computation poll 前准备授权 boxes，再证明 affine views 或采用 Auto collection。Direct registry start 不会自动生成这一 Need preparation。`PAYLOAD_BOUND` 启用 `maximum_output_payload_bytes`，值为零时表示显式零上限；未设置该 flag 时字节字段必须为零。该 bound 支持 CPU Whole 或 staged output，并在 publication 时检查；joint execution 沿用已有的 member-local failure 处理。Input backing、workspace、metadata 与 work 仍分别计入 Root。Importer 会拒绝未知或不一致的 flags；C `start` 和 `poll` 的 `query.output` 都会反映注册时的 policy，metadata resolver 可以推导 output port/schema，但不能改变 policy。这些尾部字段改变嵌套 output record 布局，operation C ABI 仍为版本 2。精确的 `struct_size` 校验会拒绝旧的 prefix record，因此 C module 必须重建；不提供兼容 shim。

Result discovery、poll 数、continuation state、output/scratch 和 GPU request table 都有有限上限。读取、显式 work 扣除和分配失败具有粘滞性，即使 callback 忽略返回 Status 也会失败。某个 observer 抛异常不会阻断其他 observer。首个记录的 failure 仍是最终报告原因；host 另外记录是否禁止 CPU fallback。只有 origin/scope 合规且未限定的 `BackendUnavailable` 可继续参与重试判断。已接纳调用失败时，continuation state 只销毁一次并释放 input owners，随后应用 cancellation。Poll admission 非阻塞：并发或重入调用返回 `InvalidArgument/Protocol/Group`，不会进入 callback，也不会触发 failure observer 或 failure latch，也不会修改活动 phase、首个 failure 或 work count。借用的 phase/query/service references 在 callback 返回后失效；不得并发移动或销毁 continuation，且 continuation 必须使用授予的 allocator。`start_result` 在 factory 前后检查 cancellation；factory 进入后观察到 cancellation 时，取消优先于 factory、allocation 和返回 state 校验错误。任何返回的 state 都会在函数返回前销毁，definition lease 保持到销毁完成。

Structured Result execution 使用 Root-owned Actor registry 和 pending-task list。Coordinator 通过现有 CPU worker queue 或所选 native GPU queue 提交就绪 callback。Worker 不等待上游 Result producer。每个 stage 在同一 Root budget 下非阻塞 admission；seal 归还未使用容量，保留中的 state、input 和 output leases 继续计费。无法容纳最小工作集的请求返回 `ResourceExhausted`。Result callback 通过 `ResultTensorInput` 读取授权样本。Result callback 分配只有经 host 受控 allocator 导入后才能逃逸。

Staged Result RequestRecord 按 captured semantic-closure Q 执行一次，并为该 Q 发布一个完整 sealed Result。Frozen execution 保留捕获的 graph 与 input owners。Root Actor 以有界 callback wave 提交；每 wave 的已提交 callback 全部退休后，coordinator 才应用结果并推进下一波。活跃 demand 重用与完成结果缓存见下文及[缓存模型](Cache-Model.zh.md)。C++ staged GPU fragment access 使用[Fragment Atlas](Fragment-Atlas.zh.md)，有界 native discovery 见[GPU Discovery](GPU-Discovery.zh.md)。

[依赖采样算子](Dependency-Sampling.zh.md)描述 STMap helper 契约及 generic radius operations。`image.stmap`、`numeric.radius_gather` 和 `numeric.radius_scatter` 使用 structured Result dependency execution；STMap operation 发布 typed image Result，radius operations 发布 generic numeric Results。其输入契约见[图像 operations](Image-Operations.zh.md)。可选纯静态 validator 在编译时及直接 Empty 查询的 state 决策之前执行。

## Structured Result tensor 依赖

Structured Result callbacks 使用 `ResultTensorNeed` 和 `ResultObjectNeed` records，并通过 `ResultIoRequest` 描述必需 I/O。`ResultTensorNeed` 指定 input、tensor slot、Footprint 和 role；Footprint 使用 tensor 的完整逻辑 sample shape，batch axes 排在 cell axes 之前。Coordinator 在提供 `ResultTensorInput` 前，会依照 input Result schema 和已认证 coverage 校验 Need。该 capability 只授权请求的 samples，不会触发隐藏读取。

任何 structured Result consumer 向计算型 C2 producer 请求 samples 时，structured execution 都会将请求展开为 singleton-atom source queries，最多 `min(65,536, maximum_boxes)` 个，并按每组最多 64 个请求调度。如果请求方是 C2 member，它仍对应一个 output observation，但其输入 Tensor Need 可以覆盖多个 source observation；展开后的区域通过私有 `ResultTensorInput` pieces 供给。每个 piece 保留原始 `ResultRef`、captured descriptor 和获准 samples。Coordinator 不会合成 aggregate Result 或新的 validation domain。Need cache fingerprint 包含每个 piece 的 captured facts。其他路径仍可使用更宽的 computed Tensor Need。读取和 window acquisition 会再次校验每个 piece 的授权。复合 capability 的 `object_id()` 为零，表示它没有单一 Result 身份；零 ID 本身不授予 payload 访问权或 Object Need 权限。读取和窗口访问仍由各 piece 的 Tensor Need 授权，而 `result_descriptor` 仍要求显式 Object Need。Phase association 继续列出每个真实 input port 和 ObjectId。

组合后的 read window 保留原始 owners 与 leases，包括保持 field pages、associations 或 resource-backed storage 存活的 owners。它组合已获准的 backing，不复制 tensor payload。View publication 继续使用既有 affine-transform 或 ordered planar-plane 路径，各自遵循物理 mapping 条件并保留 source owners。组合不会放宽这些条件；仅有多个 pieces 并不保证任一路径都能发布单一 view。多 piece expansion 只处理 tensor Needs，不实现宽 Object 或 Field aggregation。Collection-only C2 Field Result 仍可通过普通 Object Need 和 field I/O 使用；`test_result_atoms.cpp` 中的 `field_collector` 覆盖这一行 field 的用例，但不代表通用聚合。

计算型 C2 producer 不含 fields 且 Tensor Need 并集为空时，coordinator 使用 metadata-only Empty Result 路径，不展开 atom queries。Descriptor-only 和 empty-payload-role grants 也适用；seal 后的 Empty metadata Result 不授权 sample read，也不会 finalize atom domain。后续非空 Need 仍会展开为真实 atom queries。含 fields 的计算型 C2 producer 的 Empty Tensor Need 仍不在支持的 expansion 范围内。

`test_result_atoms.cpp` 中的 `compound_profile_owners` 检查 65-piece tensor window 如何通过合法 affine view 和已加载 output window 保留各个 source Result 及关联的 field/profile backing。该 fixture 的 pieces 共用兼容的 tensor backing；这不表示任意碎片映射都可组合成 view。

C 桥接中的 `result_descriptor(input, ...)` 要求该 input 有显式 Result Object Need。Tensor 或 field Need 只授权各自成员；缺少 Object Need 时会返回 sticky `InvalidArgument`，并带 `UnauthorizedRead` 与 Protocol/Group detail。`acquire_tensor_window` 仍可返回有效的复合 window，其 `object_id` 为零。`result_atoms_fixture.c` 和 `test_result_c_atoms` 覆盖 65 个 source 的读取、稀疏空洞读取触发的 sticky failure、缺少 Object Need 时的 descriptor failure、跨 poll 保留 tensor/window 访问以及 common-storage view publication。Context 退休后仍可读取这些保留输入；释放最后一个 owner 后，其 Root accounting 归零。C++ `test_result_atoms` 还检查了预取消 demand 以及 Host admission 耗尽后的恢复。两个测试均未验证 window acquisition 期间取消或 partial-window fault。

Callback 可分阶段执行。例如，先请求 Control tensor samples，在后续 poll 读取这些 samples，再请求 control values 选中的 Data tensor samples。最终 publication 为每个 output observation 附带 `ResultRelation` row，记录已消费 Control support 和选中的 Data support。未消费的 control coordinate 不会产生 relation edge；tile 边界也不会把整个 tile 的所有 samples 变成 dependency。

`ResultRelation` 是不可变的结构 evidence。其 tensor support 使用完整 sample shape 中的逻辑坐标，包括排在 cell axes 之前的 batch axes。每个 support address 保留 target 和 tensor slot，roles 用于区分 Data、Control、Validation 和 Descriptor。Tensor description facets 和其他已声明语义 descriptor facts 通过对应的 Descriptor/Validation support 消费；layout group 不会隐式创建 sample support。Ancestry relation 记录原始 Need port，以便 replacement 沿同一个逻辑 input rebind evidence。Result runtime descriptor witness 可通过 Descriptor role（bit 8）执行 transpose；numeric Value descriptor/facet 是静态契约，变化需要重新编译，因此 numeric Value dirty query 只接受 payload roles 1..7。`visit_declared()` 为 representation/projection validation 枚举声明的已知 spans，包括 Unknown relation 中仍可见的部分；成功不证明关系完整或 clean。普通 `visit()` 和 `intersects()` 对 Unknown 仍返回 unresolved。只有完整 captured coverage 才能使 `Exact` 证明 clean；`Conservative` 可以扩大 potential dirty；`Unknown` 始终是 unresolved。

在 compiled structured Tensor publication 中，host 还会针对每个 publication region、input port 和 slot 验证 ColorArray tuple closure。Data mapping 必须有同一 observation 对应完整 channel tuple 的 Validation support；其他 observation、port 或 slot 的 evidence 不能填补缺口。Output observation 会同时按 schema 声明的 `atomic_trailing_axes` 和 typed ColorArray channel axis 分组。Compact proof 可以处理分组后的 Data/Validation role-5 mappings，包括 identity view。需要更精确判断时，validator 将检查投影到请求的 output Footprint，并在 Run work 与 cancellation 限制下访问已声明的 support；它不会枚举 dense input domain。超出限制会返回错误，不会扩大 support。`SchemaTemplate::validate` 会拒绝 `atomic_trailing_axes > 1` 的 ColorArray tensor。该检查适用于 compiled structured Tensor publication；这里不对 field 或 direct continuation publication 声称 tuple closure 校验。`ResultRef` 仍可获取授权的 partial tensor window；`ResultBuilder` 则要求每个 ColorArray publication region 包含完整 tuple。

每次 tensor publication 只会在 guarantee 非 `Unknown` 时认证所提交 samples 的 relation；所有 relation（包括 `Unknown`）都必须匹配 tensor slot 的完整 output shape，并限制到精确 publication region。互不相交的 region 保留各自 source spans、roles 和 targets；union 不会跨 hole 扩大 support，也不会升级 `Unknown`。Restricted relation 保存有界 region mask，不枚举完整 tensor canvas。随着 region 到达，coalescer 会重新检查 union frontier；只有 child witness 和 output shape 相同、至多一个轴不同且该轴区间重叠或相接的 mask 才合并。分离区间保持独立，不会用 bounding box 代替 union。Coalescer 按 child witness 和 output shape 为 Restricted leaf 建立索引，每个新 leaf 只在自己的分组内尝试相邻矩形合并，每次查找和尝试都计入有界 work。被合并的 leaf 腾出稳定槽位，因此遍历顺序和无关 witness 的位置不变；此前捕获的 relation snapshot 不可变，保留其已发布前缀。Witness 指针只作为内部分组键，不构成语义或缓存身份。对于 `Unknown` leaf，`visit_declared()` 遵守该 leaf 自己的 region mask；sibling mask 不会隐藏 leaf 区域内部已声明的 witness。`ResultRelation::project` 在递归前用 region mask 裁剪每个 Restricted leaf。请求 footprint 已位于 mask 内时，leaf 共享该不可变 footprint；否则将每个 canonical 矩形与 mask 求交一次，并规范化精确结果，work 与 scratch capacity 按 projection 限额计费。裁剪结果为空的 leaf 直接跳过，不递归也不映射。Projection 仍在第一次调用 visitor 之前预检完整表达式，因此不受支持的不相交 sibling 返回 `NotFound`，不会部分交付。Dependency query 收集 mapped support 时，先校验每个 support 地址与 shape，再在完整地址 `(port, roles, target, slot)` 内对 mapped 矩形去重。重复 tap 只付出有界索引 work，不增加新的规范化候选。

`test_global_results.cpp` 覆盖 Exact/Unknown 混合的不相交 support、nested union、元素数相同但 shape 错误的拒绝，以及让未填充点继续 unresolved 的 frontier 压缩；已捕获的 publication 前缀在后续压缩后保持不变。`test_result_image_relations.cpp` 覆盖在 visitor 交付前拒绝不受支持的不相交 sibling、以点 oracle 检查分离条带与空洞的裁剪，以及 work 准入期间的取消。`test_footprint.cpp` 检查单矩形规范化路径的 box 与 work 限额、取消，以及尾部空 region 的实测 work。`test_result_plugin.cpp::many_view_windows` 覆盖 1,024×2 frame/layer views（2,048 个 windows），均在默认 Run work 限额内执行。

`ExecutionDependencies` 和 shared dependency manifests 保留 relation evidence 与 source associations，不保留 pixel payload、`ResultRef` owners、source callbacks 或 workers。已发布的 `ResultRef` 另外拥有 typed tensor backing，并可通过单调 association 保留已消费的输入 Result owners。`ResultRef::capture()` 将 descriptor、relations 和 dependency evidence 固定在同一个认证 revision；host actor 发布该 captured view。`DemandHandle::replace_bindings` 提交新的 immutable inputs 时，会按旧 evidence 的 source support 对比字节，并沿 captured relation 计算 potential dirty。已消费的 Control sample 变化会使旧输出 relation dirty；下次 request 在新 binding generation 下发现新的 Data support。未消费的 controls 保持 clean。Semantic-alias diamond ancestry rebind 已由 focused Result contract fixture 验证。旧 evidence 保持不可变，继续描述旧 generation。

每条 Result dependency record 都对应一个逻辑 output target、typed member 和 query scope。Scope 是完整 public semantic key 的 domain-separated SHA-256，用于区分 Q identity，不改变 public key。Record 按每个 input port 保存已消费的上游 query identity、support kind 和 slot。Capture 与 restriction 只保留可到达同一 typed child record 的 edge；dirty propagation 和 cache rebind 沿这些精确的 port/scope/kind/slot edge 路由，不会把同一 producer 的不同请求合并。WorkflowInput 变化会传播到各个已订阅的 query scope；computed input 变化则只沿已记录的 edge 传播。同一 port 再次消费同一个 producer object 时，payload-free dependency bundle 保留观察到的最大 revision。Terminal RequestRecord 将完整 Q 和 manifest 作为不可分割记录保留。

如果 plan 位置、snapshot、routes、coverage、scope、relations 和 typed child edges 仍相同，不可变的 `DependencyBundle` 可以复用。Cached record rebind 仅在 record identity 绑定当前 plan 和 snapshot，且 logical step、routes、child mapping 均未变化时直接返回现有结构 owner。Logical step 或 plan 不同则沿当前 routes 重新绑定；别名的 child routes 无歧义时可以映射。Rebind 会共享不可变 relation owner，只复制结构 metadata，不复制 payload。只有 Actor 均无 pending work，且每个 Actor 都 complete 或 terminal 时，builder 才会将 records 移入最终结果。只要 producer 仍可能服务 peer，Run 就返回 snapshot copy 并保留可变 builder。Record entries 和 capture/rebind traversal 消耗 Root capacity 与有界 Run work；payload 仍由 Result object 单独拥有。

在一个 Run 内，structured actor registry 按编译后的 output template、snapshot、所选 output 或 tensor slot、request kind 和 canonical query Q 标识活动请求。键相同的请求复用同一个 Actor；不同 Q 使用独立 Actor。独立的 dependency scope 是该完整 semantic key 的 domain-separated digest，public semantic key 本身不变。Registry 强持有 pending Actor，完成后通常只保留 weak observation；contract-2 Actor 会保持注册至首次交付。公开 `ExecutionContext::execute_atoms` 会先校验命名 CPU Result query，再将请求的 sample Footprint 展开为 observation key，并收集各 Result 和其 Q-scoped dependency root。不同 Q 不会合并成一个 observation。每个逻辑 step 的 alias 指向该 step 最近一次请求；contract-1 policy 下，通过 weak lookup 取回已完成 Actor 仍需其他 owner 持有它。

`ExecutionOptions::result_publication` 通知属于单个 caller。Subscription 按逻辑 step 和 `ResultRef` object identity 保存 revision watermark；同一 object 的非递增 revision 会被抑制，同一步骤的不同 object 则开始独立 revision stream。每次通知最多回收八个已失效的 weak watermark 条目。多个 alias 分别收到通知，并对应同一已发布 object 的 captured certified view。

Dependency 和 dirty queries 消费逻辑 sample relation 作为证据。Tile projection 可以合并物理 I/O，但不能取代 relation，也不能把无关 samples 认证为已读取。无 fields 且 tensor coverage 为空的 C++ terminal `RequestRecord`、C `PS_RESULT_REQUEST_RECORD_V2` 或 contract-2 joint output 使用 host stateless 路径：静态检查和资源准入仍会执行，随后由 host seal 空 Result，不调用 producer callbacks，也不读取输入。该路径不会创建 AtomObservation 或 validation-ledger finality。普通 Atomic Empty demand 仍可能需要读取 Control 或 Validation 输入；含 fields 的 output 也会运行 producer。Public `ResourceMap` 和 `ResourceVector` 结果拥有 root 计量的 metadata。复制的 coverage map、source observation vector 或 Result output map 会保留 names、容器存储以及其中的 Result owners，直到释放为止。Consumer 不再使用时应 move 或释放这些容器，并将其副本纳入与 payload 和 relation 工作相同的资源预算。Continuation state、I/O、relation/maps、payload、队列、work 和 retained owners 在同一个 execution root 下受预算限制。

## C 分阶段程序的内存与错误路径

Result ABI 2 C operation table 是唯一的 C staged-operation 接口，提供 typed Result ports、callback-scoped services、分阶段 Needs 和 Result publication。Loader 会校验并复制有界 table，再原子发布 definitions；只要导入的 callbacks 仍可能运行，就保留 module owner。C++ API 通过 `ResultContinuation` 使用同一 staged Result model；Result ABI 2 仍是唯一 C operation-plugin interface。

poll 提交逐输出、逐端口和角色的精确 run 与 tag 关联。Atomic 坐标对应当前 sample 或 HW pixel；terminal 关联不使用原子坐标，保留完整 original Q。 Need 阶段分配输出、完成时仍有未解决 Need 或未发布输出都返回错误。

阶段服务提供 checked read 与借用 fragment view，包含实际 dtype、origin、有符号 stride、字节跨度和授权区域。缺失样本不会触发隐藏的上游读取。跨 poll 保留输入必须使用 `retain_input`；无效 handle 和被忽略的服务失败保持 sticky。

| 资源 | 有效期与所有权 |
| --- | --- |
| 借用 fragment view | 在当前 poll callback 内有效；Run 保留供给它的底层 owner。 |
| retained input handle | 在 invocation 内单调且不复用；直到 release 或 state 销毁时失效；只保留该 fragment 的授权区域和 storage lease。 |
| scratch 指针 | 在当前 poll callback 内有效，poll 返回即失效。 |
| output 指针 | 成功发布时立即失效；未发布时最晚在 poll 返回时失效。 |

[`tests/integration/test_result_c_block.cpp`](../../../tests/integration/test_result_c_block.cpp) 覆盖 C block-state handle 生命周期和有序 Float64 fixture scan。Producer 通过 poll callback 请求后续输入样本，因为 block callback 不能发 Need。Finite input/sum 检查属于该 fixture；通用 Float64 block service 传输 canonical bytes。测试还检查失败清理与 Root ownership。Block-cache reuse 和 hit 数属于实现细节，不是性能契约。

### C checkpoint 服务

C Result services `checkpoint_before`、`checkpoint_read` 和 `checkpoint_publish` 为纯 staged operation 提供 Result checkpoint 路径。查找返回不大于请求上限的最大已存 sequence。未命中时成功返回 handle 0；命中时返回 opaque handle、sequence 和 canonical tensor byte size。Opaque handle 只可在当前 start 或 poll callback 期间使用；其 callback-epoch entry 会保留 state 和 witness，joint poll 中各成员的 handle 相互隔离。

Checkpoint state 是 sealed generic `CompleteBundle` Result，包含一个完整覆盖的 tensor，不含 fields、metadata 或 domain；tensor 不带 facets、groups、batch axes 或 spatial layout。`create_block_state` 将字节 payload 复制到该 Result，并按 tensor 的逻辑 samples 校验 payload 大小。调用方可将返回的 operation-owned handle 用作 block state 或传给 `checkpoint_publish`。Block-state handle 会跨 poll 保留，直到调用方释放或 callback state 销毁。可选 retention 获准时，发布会保留 Result 与成功 dependency witness。此调用不会消费 handle；调用方仍须通过 `release_block_state` 释放它。

`checkpoint_read` 按 canonical row-major logical sample 顺序复制正长度且范围有效的字节区间。区间可以从一个 element 中间开始或结束，也可以跨越 sample 边界。Service 不暴露保留存储的指针，会计入读取 work，并检查 Root capacity 与取消状态。恢复 checkpoint 会重新导入成功 dependency history、facts、ancestry 和 association；它不会恢复 Result input capabilities，也不会授权新的输入读取。Callback 必须另行请求当前 inputs。Checkpoint API 要求 phase 非零且 operation deterministic、side-effect-free。RequestRecord operations、block callbacks 和 GPU discovery 不能调用这些服务。无效 handle、区间、scope 或禁止的调用会成为 sticky failure。

可选 `checkpoint_publish` retention 会遍历 callback 实际消费的 input bundles，并计量其唯一结构 DAG；每个 `DependencyRecord` 和不可变 relation owner 只计算一次。Metadata weight 包含 scope、typed port query、sample footprint、domain、route、manifest、certificate 和 association record。Admission 检查 cache metadata capacity 与 `dependencies.sets.maximum_boxes`，遍历消耗 `maximum_dependency_cache_work`。若可选限制超出，host 跳过 checkpoint retention，计算继续执行。

`test_result_c_checkpoint.cpp` 还检查 finite-prefix fixture：有效前缀可以发布，后续请求若遇到 infinity、NaN 或溢出求和则失败。有限值约束属于该 fixture 的 scan 逻辑；generic Float64 checkpoint service 只复制 canonical bytes，本身不会拒绝非有限值。

Result C module 与 C++ Result continuation 使用相同的结构化输入和 publication 模型。原生插件仍是受信任的进程内代码；指针 metadata 校验不提供内存隔离。C++ Result continuations 与 Result C modules 使用相同 structured input 和 publication model。

`test_result_plugin.cpp` 覆盖 Result ABI 2 C module 导入、callback 执行、sticky service failure 和 plugin owner 生命周期。其 `c_whole_view_policies()` 检查所选 output flags 与 payload bound 在两个 C callback 阶段都可见，并覆盖 Auto collection、strict view 拒绝、兼容/负 stride/零 stride backing、output cap 和 owner 退役。`fixture.result.bounded_joint` 声明两个 Float64 `{4}` Atomic outputs，payload bound 分别为 0 与 8 bytes；singleton 和 joint callbacks 均验证每个 output query 中的 bound。`c_joint_payload_bounds()` 检查 grouping 开启和关闭两种情况：零字节上限的成员以准确 Atom key 返回 `ResourceExhausted/CapacityLimit`，8-byte peer 成功发布值 `11`，执行后所有 Root usage 均已释放。Fixture 还注册 malformed modules，覆盖未知或不一致的 policy flags、bound 声明不匹配、不支持的 view 组合以及截断的 output record。`test_result_c_joint.cpp` 覆盖 C joint contract 1，包括不同 output shapes 和关闭 grouping 后的 singleton 路径；`test_result_c_atoms.cpp` 覆盖 contract-2 C Atom outcomes。Result checkpoint 和 block fixtures 覆盖对应的 scoped services。

consumer-build 由 `test_installed_consumer` 创建；共享库安装将路径中的 `static` 替换为 `shared`。

## 运行时结构证据与 dirty 状态机

成功的 structured dependency-network Run 会在 `ExecutionResult::dependencies` 中保留不可变直接记录。Atomic 记录合并匹配 node/contract/snapshot 的完整逐观察 rows 和覆盖域；Whole 与 terminal RequestRecord 保存不可分割 manifest。Legacy regional step 记录 callback 和 validation 实际使用的逐端口 demand。Atom collection 会按每个 observation 的 Q scope 建立命名 root；关联的 Field/Tensor record 通过 typed logical mapping 保持在同一 evidence 中。同一请求名下多个 atom observation 因而保留为不同 query root。Builder 只合并 node/contract/snapshot 匹配的行。Source declaration、记录、订阅和命名 root 共用有界 metadata 计数，与受控像素字节分别计费。证据不持有 Value、source callback、snapshot block 或 worker；像素结果、graph、registry 和 ExecutionContext 释放后仍可查询。

`coverage()` 表明证据完整的命名输出样本域。`certificate(node)` 返回已观察的 Atomic rows；未知 node、Whole 和 terminal 均返回 NotFound，不能据此宣称未知 row 为 clean。`potential_dirty(input, samples)` 对捕获关系及已记录输出子域精确， 只沿直接订阅使用真实 `DirtyDeltaQueue` 传播。同代稍后到达的新 delta 会再次传播。 入口 Footprint 复制、遍历和答案增长有总量边界，取消和 ResourceExhausted 均显式失败，不返回部分 clean/dirty 答案。

该查询处理固定 declaration 下的 payload 变化。Static input tensor schema descriptor/facet 是静态契约，变化需要重新编译，不作为 dirty sample edit 接受。Typed Result runtime descriptor observations 使用 role bit 8，可沿 captured relation transpose；改变已编译 schema 的 Result descriptor/schema 仍需重新编译。样本记录图没有跨节点 metadata-output atoms。逐节点 certificate 保留非空间 tags，可在该节点 transpose；若未记录 metadata-output atom，API 不会据此推导上游 metadata clean。

`restrict({name: subset})` 沿已保存的直接关联反向收缩所有相关 Atomic row 和订阅，并移除不再需要的记录和 root。未知覆盖域会被拒绝；非空 Whole 子集保留完整全局 manifest；terminal RequestRecord 仅接受相同的完整 Q。Atomic Empty 是已知空，省略的 output name 则不存在。原始 query 已为空的 output record 仍可能带有已记录的非 payload Control 或 Validation obligations；将它 restrict 为 Empty 会保留这些 obligations 及其 upstream records，重复 Empty restriction 也一样。将原本非空的 output restrict 为 Empty，则会移除其 payload obligations 和 support。具名 Result atom root 的 Empty restriction 保留 output namespace。该操作不读取像素，也不调用算子。

`test_result_plugin.cpp` 的 `image_transport` case 覆盖两种情况：原本为空的 Result 在重复 restriction 后仍保留已消费的 Control/Validation source observation 和 `source_support()`；将非空 output restrict 为 Empty 则会移除 payload obligations。

[`tests/integration/test_execution_dependencies.cpp`](../../../tests/integration/test_execution_dependencies.cpp)、[`dependency_workflows/dynamic.cpp`](../../../tests/integration/dependency_workflows/dynamic.cpp) 和 [`dependency_workflows/demand.cpp`](../../../tests/integration/dependency_workflows/demand.cpp) 覆盖逐 port dirty propagation、Control 驱动的 support 变化、frozen bindings 和独立 source oracle。[`dependency_workflow_fixture.hpp`](../../../tests/support/dependency_workflow_fixture.hpp) 提供当前共享测试算子。`test_result_request_record.cpp` 覆盖不可分割 terminal manifest。Evidence 保持 immutable；订阅由下文的 context-managed demand API 所有。

## 精确 demand 与 binding generation 状态机

`ExecutionContext::open_demand(plan, bindings)` 固定当前图契约及不可变 Result bindings。`DemandHandle` 副本共享 bundle 和 generation。`request({name: Footprint})` 返回具名 ResultRefs、诊断和结构证据；每个请求保留 original Q。Result continuation 通过 typed Needs 接收 selected tensor slots。Staged terminal RequestRecord 保留其 selected output 的完整 captured query。无 fields 且 tensor coverage 为空的 C++ terminal `RequestRecord` 或 contract-2 joint request 会校验静态 metadata、backend 和 resources，然后由 host seal 空 Result，不调用 producer callbacks 或读取输入；该路径不创建 AtomObservation 或 validation-ledger finality。普通 Atomic Empty request 仍可能需要 Control 或 Validation 输入，含 fields 的 output 也会运行 producer。未请求的名称不出现在结果中。`execute_fragments(frozen, Q)` 对固定 bundle 提供相同精确查询，结果 generation 为零。

成功请求按精确命名 query 捕获不可变结构 publication。`replace_bindings` 将新 bindings 与 captured evidence 实际消费过的 source support 比较，通过旧关系计算 potential dirty，再原子提交新 bundle、generation 和累计 dirty。不信任调用者 dirty hint。已消费的 Control evidence 变化后，dirty 会保留到对应 query 成功重新发布，即使 output value 恰好相同。新请求可以在 replacement control values 下发现不同 Data sample 或 Result tensor slot；新关系取代新 generation 的 support，旧 evidence 保持不变。`source_support()` 是字节比较用的有界取数并集，不替代逐 output relations。ColorArray tuple 比较包括完整 C；generic 比较保留全部 dtype 位模式。Static input tensor schema descriptor/facet 变化，以及改变已编译契约的 Result schema 变化，需要新编译 plan；typed Result runtime descriptor edits 通过 roles=8 和声明的 Descriptor/Validation support 参与 dirty query。Tensor semantic metadata 和 descriptor edits 由声明的 support 表示，不从 tile geometry 推导。

替换在发布前验证全部 bindings。样本、metadata 限额或验证失败保留旧 generation； 与 request 发布或其他 replacement 竞争时返回 Stale，供调用者重试。已完成替换前捕获的 latest 请求不能发布到新 generation，取消优先于 Stale。`freeze()` 固定当前 bundle，独立于后续编辑；`release(Q)` 删除一个精确订阅，但不取消活跃请求，后者仍可重新发布该订阅；`cancel()` 停止该 handle 并退休其 publication 和 bundle。Context 析构先取消并排空活跃 demand 调用，再退休既有 workers。直接 context 调用不能与析构竞争，已有 handle 调用可与析构竞争。 输入 owner 析构在 publication mutex 之外执行。

`maximum_demands` 限制存活且未取消的 handle，范围 1..65536、默认 1024；活跃 demand 调用上限为既有队列容量加 CPU worker 数再加一个 coordinator 槽。`DemandConfig::maximum_metadata_entries` 限制每个 handle 保留的 query/evidence/dirty metadata，范围 1..1048576、默认 65536。Handle 不拥有 worker 或像素 cache。Structured Result producer 使用既有 CPU/GPU pool、WaitingAdmission 和计费 allocator；兼容 producer 通过 `SharedResults` 共享。已完成 Result content reuse 使用可选像素 LRU 和有界结构证明，见[缓存模型](Cache-Model.zh.md)。GPU transport 见 [Fragment Atlas](Fragment-Atlas.zh.md)，有界 discovery 见 [GPU Discovery](GPU-Discovery.zh.md)。

`test_execution_demand` 覆盖稀疏结果、typed snapshots、连续 dirty 累积、frozen 隔离、陈旧 publication、独立取消及 context drain。`test_result_request_record` 验证 terminal Result 查询和 exact-Q 行为。Radius 与 dynamic demand oracle 位于 [`tests/integration/dependency_workflows`](../../../tests/integration/dependency_workflows)，由 `test_dependency_workflows_{radius,dynamic,demand}` 注册。

## 活跃 Result demand sharing

Demand coordinator 与 Root-owned `SharedResults` directory 会为相同不可变 bundle 和 exact selected query 共享兼容的 structured Result producer work。Query key 包含 captured bundle、plan/operation contract、node、geometry、selected output 和 resource policy。Sharing 和 cache eligibility 要求 selected input closure 中的实现 deterministic 且 side-effect-free；被 projection 排除的 inputs 不影响资格。即使 consumer 是纯函数，带副作用或非确定性的 ancestor 也会阻断下游 sharing。Coordinator 将 waiter 的取消/currentness 与 producer lease 分开，因此一个 waiter 取消不会终止其他 request 仍需要的工作。Callback worker 不等待上游 producer。

Shared Result entries 和 waiters 使用自己的 Root `Entries`、`Host`、`Metadata` admission。`ExecutionContextConfig::maximum_result_checkpoint_scopes` 只限制 Result checkpoint-scope directory（1..1,048,576，默认 65,536），不限制活动 Result sharing。Consumer 接收已完成的 shared Result 时，coordinator 会导入其 captured dependency bundle，并把 captured descriptor revision 记录到 Actor。Frozen publication 中的 Validation ancestry 因此会继续参与 dirty propagation。Context 关闭时先排空 demand calls 和已提交 callback，再销毁 workers。`cache_statistics()` 合并 retained-result cache 与 structured Result sharing 计数，同时保留各来源的计数范围。即使关闭 completed-result retention，活动 producer epoch 仍会计数。

`test_result_dag_parallel`、`test_joint_execution` 和 `test_scan_waiters` 覆盖活动 Result sharing、独立取消、结构 evidence reuse 和请求顺序。Fixture 检查 producer-wave admission、较小 Q 独立运行、joint outcome 隔离及真实 `numeric.ordered_scan`。

## Result block 状态转移

`ResultProgramPhase::block` 和 C block service 作用于一个显式、已 sealed 的 `CompleteBundle` tensor state。Block callback 读取该 state，并返回它或一个新建的 state；它不发出 Need，也不发布 operation output。Producer 通过 poll callback 请求之后的 tensor 样本。Checkpoint 与 block 服务相互独立：checkpoint 恢复可导入成功的 dependency evidence，block state 只携带转移数据。可选的 block-state 保留可能跳过一次转移，或在资源限制下重新计算。其命中次数和转移复用属于实现细节，不构成性能承诺。

[`tests/integration/test_result_c_block.cpp`](../../../tests/integration/test_result_c_block.cpp) 覆盖 C block-state handle 和 Float64 fixture scan，包括 state 生命周期与失败清理。Finite input 和 finite sum 规则属于该 fixture；通用 state service 传输 tensor 字节。

## GPU fragment 传输与执行

[Fragment Atlas](Fragment-Atlas.zh.md) 说明已实现的精确 atlas/mask 目录、SDK MSL lookup helper 及原生传输验证。C++ staged GPU 执行已接入，[有界 discovery](GPU-Discovery.zh.md) 已实现。

同步 GPU producer 也通过既有 native worker 执行，实际容量与 CPU 回退见 [Fragment Atlas](Fragment-Atlas.zh.md)。

## Result joint contracts

Joint callback 通过 structured Result protocol 的 `OperationDefinition::start_result_joint` 和 `OperationRegistry::start_result_joint` 执行。Joint callback 是可选的分组 Result execution contract。下文说明 Result joint contract 及其 direct 和 structured execution 路径。

### Structured Result joint callbacks

`OperationDefinition::start_result_joint` 为 structured Result output 提供 direct CPU joint callbacks；`OperationRegistry::start_result_joint` 用于启动 continuation。两种 contract 都要求 Atomic outputs 来自同一个 prepared static invocation，具有一致的 input metadata 和 parameters、CPU backend，以及相同的非空 `snapshot_identity`（最多 4096 字节）。Contract 1 要求 RequestFailureOnly delivery，接受 2 至 64 个不同 output index。Contract 2 要求 PerAtomOutcome delivery，接受 1 至 64 个不同 `AtomKey`；不同 coordinate 可以对应同一 output，但该 output 的成员必须共用一个 tensor slot。Snapshot identity 用于分组与路由 provenance，不提供数值输入语义。

对每个 tensor 成员，start 捕获原始 tensor Q，按 tuple channel 与 atomic trailing axes 闭合，并要求结果是一个非空 Atomic observation。其他 sample axes 的 extent 均须为 1。`result_atom_key(query)` 编码 output index 和其余坐标，包括 batch axes；tuple 与 atomic trailing axes 会省略。`result_observation_domain(query)` 返回这些剩余轴构成的完整固定 domain。仅有 collection 的 output 映射到 coordinate 零的 singleton key。共享 start callback 接收未取消成员的闭合 query 子集，该子集可以只含一个成员。每次 poll 检查 raw Q，再校验 prepared schemas、static parameters 和 backend，之后才向成员 callback 提供闭合 query。Registry 保留 definition、prepared invocation、keys、取消 token、continuation 和 Root-owned resources。

每次 poll 为就绪且未取消的成员子集调用 callback 一次。Callback 为每个成员返回一个以 `AtomKey` 标识的 Root-owned `ResultJointOutcome`：Result tensor/field Need、完整 Result publication 或局部错误。Contract 1 的 key 对应不同 output；Contract 2 可以返回同一 output 的不同 coordinate。Host 在暴露任何结果前校验整个 envelope，包括 outcome vector 和嵌套 Need containers 的 Root ownership、成员数量和身份、selected input projection、Need 的 slot/shape/roles、field index，以及每个成员 query 的 publication 完整性。整轮累计 Result Need entries 上限为 65,536。Host 在验证 Need entries 前预扣各个 envelope 的 entry count。Malformed envelope 会使 continuation 以 sticky group error 失败；合法的成员局部错误仍归属于对应 output。取消按成员返回，被取消成员不会传给 callback。

它合并同一成员对同一 port 和 slot 的 Data、Control、Validation 请求，并要求合并后的 coverage 包含完整 channel tuple。同一成员可以用多个 Need 合并出完整 tuple；其他成员、port 或 slot 的请求不能填补缺口，host 也不会补齐缺失 channel。Descriptor-only request 不增加 payload transport。该检查闭合 ColorArray channel axis，但不额外闭合 `atomic_trailing_axes`；tuple 不完整时，在提交任何成员回复前返回 `Protocol/Group`。

Phase 为本轮提供一个受限 workspace allocator 和共享的 Run/root work charge。每个 member ResourceBudget 必须属于 continuation Root；phase 与 member allocator 必须共享其非空 accounting domain。Host 使用 outcome 和嵌套 Need storage 前会验证 Root ownership。Contract-2 quality report 必须属于成员 ResourceBudget 和 phase resources，并由 phase 或 shared workspace allocator 持有。`BufferAllocator::same_owner` 只比较 accounting-domain identity，不比较 quota 或 provenance，也不授予 payload 访问权限。Callback exception fence 会保留共享 allocation、work 或 I/O 错误中的首个错误。C++ Result factory 和 poll 将 `std::bad_alloc` 转为 `ResourceExhausted`，将标准异常转为带 `HostException` 的 `OperationFailed` 并保留诊断（`what()` 为 null 时 message 为空），将非标准异常转为带固定 `HostException` 诊断的 `OperationFailed`。Phase 已记录的 failure 优先于随后抛出的异常；host failure observer 抛出的异常不会替换 producer 已选定的错误。后续 poll 返回已锁存的 group failure；并发或重入 poll 会被拒绝，但不会覆盖活动 poll 的结果。

Contract 2 的 Atom failure 必须指定精确 `AtomKey`。`ValidationDomain` failure 必须使用 Domain 或 Schema origin，并匹配 output 的完整固定 observation domain，不能以当前 query 子集代替。Host 将它应用于 continuation 内匹配的 Ready 和 Waiting 成员，并拒绝撤销 semantic-terminal 成员的后续 domain failure。Contract-2 outcome 可携带 `QualityReport`：成功证据会与已发布 Float64 estimate 核对，Measured evidence 可附于匹配的 Domain failure。完整证据契约见 [Atom 错误与数值质量](Atom-Errors-and-Quality.zh.md)。Quality report 的 snapshot label 与路由 `snapshot_identity` 相互独立。

Structured execution 会通过 contract 1 或 contract 2 组合相同 node 上符合条件且已就绪的 CPU Result query。Contract 2 最多接受 64 个单 observation query，也可接收同一 output 的不同 Q；跨多个 observation 的 query 不会拆成 atom。已完成、已缓存、已剪枝或其他不符合条件的 Actor 不进入共享 start。关闭 C1 grouping 时，contract 1 走 singleton 路径；contract 2 仍使用必需的单成员 joint callback。Coordinator 调用 shared start、满足各成员的 Needs，并在成员就绪后提交后续轮次，发布前校验整轮结果。Contract-1 fallback 仅适用于其 optional unscoped-error predicate 接受的 failure，包括符合条件的 backend 或 admission failure；Cancelled、Stale、Protocol、Domain/Schema 和 Run-scope failure 不重试。Contract-2 group、protocol 和 Run failure 都是终局错误，不会回退到 singleton callback。

Contract 2 的 Run ledger 按编译后的 producer semantic identity、output 和 tensor slot 保存 ValidationDomain failure 与 semantic-terminal observation。同一 compiled producer 的 semantic alias 共享终局记录；failure detail 仍指出实际失败的 workflow node。同一 cohort 中每个 output 固定使用一个 tensor slot；slot 冲突由后续 cohort 处理。Ledger 跨 cohort、completed-cache 复用和 Actor 退休继续有效。Coordinator 在 callback 前为每个 query 分配 payload-free failure 与 quality record，记录终局结果时无需再分配。Contract-2 Actor 会保持注册，直到其已完成或终止的结果首次交付；contract 1 保留原有 weak-completion 策略。Shared producer entry 会将 quality 与 publication 或 failure 一起提供给 waiter。Domain failure 的身份和 evidence 会继续向下游传播；成功 transformation 不继承输入 quality。带 quality 的 result 会跳过 completed-output content retention。`ExecutionContext::execute_atoms` 使用 structured coordinator 收集命名 Result observation 及其 Q-scoped dependency root。Structured Result joint execution 仅支持 CPU。

C Result ABI 2 output record 声明 `observation_kind` 和 `failure_delivery`。可选 CPU `joint` program 支持 contract 1 和 contract 2；operation 仍须提供 singleton callbacks。Contract 1 要求至少两个 Atomic output，使用 RequestFailureOnly delivery，并按不同 output index 分组 2..64 个成员。Contract 2 要求 Atomic output 和 PerAtomOutcome delivery，通过 direct registry entry point 或 structured CPU workflow 接受 1..64 个完整 `AtomKey` 成员。不同 observation coordinate 可以指向同一 output，但必须使用该 output 固定的 tensor slot。Importer 要求 program 及嵌套结构采用当前完整结构大小。

两种 contract 的 start callback 都接收过滤已取消成员后的 query 子集，子集可以只含一个成员，且不提供 payload-read service。Host 分配一个清零的 state payload；start 进入后，host 恰好调用一次 `destroy`，即使 start 失败也如此。每次 `poll` 接收就绪成员、每成员独立的 Result service，以及共享 scratch/work service；active subset 也可以只含一个成员。Outcome array 必须恰好包含每个完整 AtomKey 一次。Contract 1 还要求 output index 各不相同，并允许普通 C error 作为成员结果。Contract 2 的 typed member failure 必须使用 `PS_RESULT_ATOM_FAILURE_V2`；poll callback 返回非零表示整个 group 失败。Host service 产生的 cancellation、resource exhaustion 等错误保留 host 状态和 failure detail。Host 在转换前检查整个 array，再执行共用的 Result schema、scope、Root、完整性和 exact-query 校验。Scratch 在 poll 返回时退休，work 计入 Run 与 Root，共享 handle source 避免成员间 handle 冲突。

Contract-2 failure record 会复制 error code、reason、origin、scope 和最多 4096 字节的诊断。Scope 仅限精确 Atom，或 origin 为 Domain/Schema 的固定 ValidationDomain。Callback 不能提供 producer node/input identity；host 根据实际 producer 确定身份。共享 poll service 还可创建 Measured 或 IntegerDiagonal quality report。Opaque handle 只在该 callback epoch 有效，可附加给同一 poll 内成员的成功 publication，或携带 Measured evidence 的 Domain-origin Atom/ValidationDomain failure；它与成员 Result service handle 不同，也不能跨 joint group 复用。即使 poll 最终返回 Need，callback 也可以创建 report，但 Need outcome 的 quality 字段必须为零，未使用的 report 会随 callback lease 一起退休。Lease 退休前 host 会复制附加的 owning report，再由 continuation 执行 Result evidence 校验，包括与已发布 Float64 estimate 的核对。Contract 1 不携带 quality。C singleton operation table 没有 typed Atom failure 或 quality attachment。

`test_result_c_joint.cpp` 覆盖 contract-1 成员使用不同 output shape：left output 的 shape 为 `{1}`，right output 的 shape 为 `{2}`；请求 right coordinate `1` 返回 `11`，而 coordinate `0` 保持不可读。关闭 joint grouping 后使用 singleton callbacks，仍得到相同数值和逐 output dirty 结果。`c_joint_payload_bounds()` 还检查 contract-2 成员的逐 output payload cap：零字节上限使 left 成员以 `CapacityLimit` 失败，right 成员的 8-byte cap 则允许发布值 `11`；grouping 开启和关闭时均验证此行为。 同一测试中的 `wide_sparse_output` 使用大小为 `2^61` 的 Float64 domain，仅通过 8-byte window 读取首尾 point，并在 64 KiB Payload 限额下检查 Root 清理。它验证稀疏端点访问，不代表稠密分配或宽域 GPU 执行。

新增 `joint` 会改变 `ps_result_operation_v2` 的二进制布局。ABI 2 要求 `struct_size` 等于当前完整结构大小；旧的 prefix-sized operation record 会被拒绝，因此 C modules 必须重新构建。C joint bridge 仅支持 CPU。Contract 2 没有 GPU joint backend。Joint callback 组合 grouped Result execution；每个声明的 output 仍可使用 singleton Result callback。

## 就绪 Atomic 调度与 admission

`ExecutionPlan::execution_groups()` 列出保留 singleton step 的可选 CPU contract-1 分组。`ExecutionOptions::enable_joint` 只关闭此 C1 物理分组优化。Contract 2 仍使用 joint protocol；关闭分组时执行必需的单成员 callback。Coordinator 登记请求根和已声明的输入需求，只组合已经确定且 Ready 的观察点；它不会等待未来请求，也不会跨 Run 分组。

任意 structured Result consumer 返回经校验的 Need 后，coordinator 会先登记其中符合条件的计算型 CPU contract-1 输入，再读取上游。它按 input port 和 slot 闭合并合并 tensor footprint，将排序和登记 work 计入 Run，并在短暂的 Root-owned scope 中记录选中的 C1 producer steps。该 scope 活跃期间，只有匹配的 C1 Actor 及其 aliases 会延迟启动，使整批输入需求先完成登记；无关 peer 仍可推进。此路径不会预登记 C2 producer 输入，cache replay 也不会登记新需求。安装单项需求时，Actor 初始化仍可能准备 scalar inputs 或复用缓存 Result，因此后续 admission failure 不会撤回已经完成的准备。`test_joint_execution` 中的低 Root-work case 不需要 scalar preparation；它验证 consumer 首次 poll 后返回 `ResourceExhausted`，且 C1 producer callback 尚未运行。

Coordinator 在推进 Actor 前先登记显式请求的 named Result roots。潜在的 CPU contract-1 joint 候选根可以延迟启动，直到其 consumer Actor 验证 Need 并登记相关 producer inputs；这样 peer C1 inputs 有机会在候选启动前完成合组。Root-owned pending-task list 会保留每个 queued 或 submitted Actor，直到 dispatch 被拒绝或 callback 退出。Dependency frontier 有 queued work 后，coordinator 最多提交 `maximum_parallelism` 个 callback，等待整批已提交任务退出，再串行应用 phase results 并推进下一 frontier。CPU staged-tile controller 与 GPU dispatch 保留现有路径；Actor wave scheduler 不会独立驱动 dependency graph。

Producer 响应 Need 前，coordinator 先验证完整 Need envelope。每个 input 有独立供给 cursor，coordinator 会在可推进的 input 之间轮转。一次 traversal 中每个 Actor 最多访问一次，从而避免 peer 等待上游时反复递归遍历深链。每个 submitted task 持有其 stage-local transfer、tile 和 native observations，driver 退休 task 时才合并。Joint worker 标记进入 callback 并执行它；driver 在退休时只对 group 记一次。Checkpoint 与 block-cache 的 metadata 锁只保护有界查找或提交区间，不覆盖计算或测试 hook。

`JointScope` 将嵌套 joint input evaluation 限制在 16 层；更深工作继续通过普通 Actor 推进。每个 scope 在步骤结束后恢复 active joint、取消和错误路由，包括宿主 Metadata 分配失败。相同 ready input set 共享 transport，各成员仍保留自己的 source records 与 role associations。Shutdown 和同步 Run 完成会先排空所有已提交 task，再释放借给 callback 的状态。

Result joint Need preflight 中，只有不属于 Protocol origin 且不在 Run/Group scope 的成员局部 `Cancelled` 或 `Stale` 才只终止该成员，符合条件的 peer 可以继续。Protocol origin 错误以及任何 Run/Group-scope failure 都是 enclosing failure，会终止该 joint round。

Whole result 的释放采用保守条件。只有完成且非 contract-2 的 Whole Actor，并且其每个 tensor slot 都有完整 coverage，才能将别名 step 标记为完成。Liveness 只统计从请求根可达的 step，并沿各 step 选定的 `input_indices` 传播。Coordinator 仅在所有 alias 都没有剩余 consumer 或 named-output pin，且没有 pending/joint work 时，清除 published object 和 producer lease。Dynamic Dependency、Prefix、Empty 或 C2 的一次部分发布不会据此标记 step 完成。已发布 Result 和保留的 read window 分别持有自己的 storage owner。Poll timing 从 phase 创建计至 driver 退休，包含 callback queue 和 wave 等待，不是纯计算时间。

`test_result_dag_parallel` 检查 callback wave 上限和取消；source backing 在独立 left/right consumer 读取期间保持存活，并在 join 发布前检查 source owner 已释放。Named-output case 并发运行 left/right producer 并返回分别拥有的 Results；单输出 case 将最终值 `7` 保存在逃逸的 `ResultRef` 中，ExecutionContext 退出后仍可读取，并在该引用释放前保留 8 字节 Payload。Held-output admission 失败会释放资源，后续 Run 可恢复。`test_joint_execution` 检查两个 gated C1 producer factory 独立进入 joint，以及 Queue admission 失败时 callback 未进入。

每个 observation 分别认领 shared producer entry 并检查内容证明。可选兄弟 claim 超过 admission 时会跳过；由其他 Run 所有的计算会单独订阅。Cache hit 不加入计算。成功成员分别发布 evidence、flight 和符合条件的缓存；局部错误仍附着在自己的结果上。已完成兄弟值保持计费，直到登记的 consumer 取走。Shared backing 使用自身 storage domain 和唯一 owner accounting；fallback ancestry 只会阻止受影响成员进入缓存。

Result joint group 只分配一次共享 continuation state 与宿主成员适配器。每次 poll 对就绪成员的 output/workspace、实际供给输入 multiplier 和 shared scratch 执行 admission；joint work 使用 Run 剩余预算。Contract 1 仅在 failure 符合其 optional unscoped-error predicate 时回退到 singleton，并先释放 group resources。Contract-2 group、protocol 和 Run failure 都是终局错误，不会回退；取消和失效也不会重试。上游工作期间 coordinator 刷新成员 leases；原请求取消不会取消另一个 Run 的活跃 waiter。

`test_joint_execution` 覆盖 root 和 consumer-driven Result grouping、独立投影、共享 owner window、sum `18`、64 层依赖链、admission/fallback、取消和 GPU fallback ancestry。其 `maximum_result_checkpoint_scopes=1` case 仍能启动 Result joint group，因为该配置限制 checkpoint scope，而不是 shared-producer entry。另一个 1 GiB shared-continuation case 覆盖 contract-1 group admission 失败后的 fallback。Consumer-driven case 检查一个 singleton Need 登记两个计算型 C1 output。`test_dependency_joint` 覆盖 64 个 output shape 不同的 Result contract-1 成员、非零首/中/末坐标、取消，以及预先失败后抑制 callback。`test_atom_outcomes` 检查跨 65 个 observation 的 Result domain failure 和 dependency DAG。`joint_groups`、`joint_polls` 和 `joint_fallbacks` 报告实际物理路径。未请求的纯兄弟端口不会登记需求，也不会执行。

### 紧凑静态依赖映射

`DependencyCertificate::create_mapped` 保存不交覆盖片段及端口/role 轴映射。 每个输入轴选择一个独立 observation 轴或固定区间，tags 单独保存。该闭合集合表示可精确计算 broadcast/permutation 的 backward demand 和 dirty transpose，无需枚举复制后的输出样本。Restriction 与相同 map 的 merge 使用集合几何；不同 map 的重叠可能需要有界逐行比较。`row` 解析一个观察，`materialize` 显式生成有界 rows，mapped 证书调用 `rows()` 会抛异常。所有变换限制工作量和保留元数据，包括规范化增加的 boxes。 `storage_entries()` 统计实际保留的坐标、支持集和 tags，供 cache admission 使用， 独立于去重后的源支持投影。

`DependencyCertificate::create_mapped` 保存不交覆盖片段和 port/role axis map，供 metadata relation 表示使用。每个 input axis 选择 observation axis 或固定区间，tags 独立保留。该表示可以在不枚举复制输出样本的情况下表达精确 broadcast/permutation 投影和 dirty transpose。Restriction 与相同 map 的 merge 使用集合几何；重叠区域的不同 map 可能需要有界逐行比较。`row` 解析一个观察，`materialize` 显式生成有界 rows，mapped certificate 调用 `rows()` 会抛异常。变换会限制工作量和保留 metadata，包括规范化 box 扩张。`storage_entries()` 统计实际保留的坐标、support 和 tag，独立于去重后的 source-support projection。Mapped certificate 描述 evidence，不定义 staged callback protocol。

Structured Result tensor operation 通过 `ResultRelation` 发布完整 sample shape 的逻辑坐标依赖，包括 batch axes。Tensor description facets 参与声明的 metadata/validation support；物理 layout 不会发明依赖。

### 跨 output 的 pure block sharing

`share_blocks_across_outputs` 是 `OperationTraits` v25 的可选字段，默认关闭，适用于 pure deterministic Atomic Result operations。启用后，runtime 可在满足当前输出与输入契约的 block 状态转移间共享 retention；状态不兼容或 retention 不获准时仍会重新计算。Block retention 与完整 Result cache、checkpoint 和公开输出分开管理。命中次数与复用范围是当前实现细节，不构成性能保证。

## 限制与错误处理

精确集合构造、证书增长、Result discovery、callback 阶段和保留的 demand metadata 分别受有限上限约束。非法 domain 或 protocol envelope 返回错误；资源上限耗尽返回 `ResourceExhausted`；取消返回 `Cancelled`。借用输入在调用期间必须保持有效；是否可重试由所属 execution API 的状态契约决定。
