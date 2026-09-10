# 精确依赖数据

G4 数据 API 提供精确集合、稀疏不可变片段与逐观察解析证书。执行集成是独立的实现层；
这些类型本身不授权算子合批错误、消费 RequestRecord 或绕过 producer/snapshot 来源。

Footprint 表示非零 rank 1–8 逻辑域中的精确集合，以规范不交矩形表示 Empty、All 与
非连续覆盖。递归轴扫描仅在后续轴集合相同时合并相邻区间，因此插入顺序、重复矩形
与不同分块不影响集合相等。即使完整域元素乘积溢出，All 也保持压缩。并、交、差要求
相同域并保留空洞。tile_cover(geometry) 返回 ceil-divide tile 域中实际相交的 tile 坐标，
不把 tile 内所有样本标为有效。visit 在显式样本上限内按 row-major 顺序逐个访问。

FootprintLimits 对每次集合操作限制候选工作与矩形条目，包括重复构造工作。超限返回
ResourceExhausted，取消返回 Cancelled，两者均不等于 Empty 或 bbox 近似。复合调用方
还需限制跨调用的总工作和元数据；单次集合额度不是完整执行预算。

ValueFragments 保存完整 descriptor/facets、授权 Footprint 与有 owner 的矩形 Value。
构造裁剪输入覆盖，拒绝缺失样本和不一致重叠；同 owner 的等价 origin-relative 映射去重。
每个 typed image fragment 及授权矩形都须包含完整 C，不能以两个部分通道 Value 拼接
规避。Generic 数组允许任意样本子集。read 通过 Value 的 checked signed-stride 地址
复制实际 dtype 宽度；空洞、未授权地址、错误宽度均失败，不同步取数或补零。
restrict 同时限制 owner 与覆盖；collect 在通过完整矩形检查后使用 BufferAllocator
分配。retained capacity 统计实际唯一 storage owner，与有效区域大小分开。

DependencyCertificate 保存身份、精确观察覆盖、声明输入域及每项观察的 AtomCertificate。
Generic 观察是逻辑 sample；图像观察域为 HW，每项代表完整像素。Row 保存输入端口、
各项依赖 role、精确样本 Footprint 和非空间 tagged atom。多角色输入规范展开，发布前
限制展开后的元数据。显式空 row 表示已知空，缺失 row 表示未知，不能发布为完整解析。
身份绑定调用方的访问/数值/错误合同及快照；数据类型不能证明任意 callback 遵守读取声明。

restrict(P) 拒绝覆盖域以外的 P。backward(P) 仅按端口/role 对相应 row 求取数并集，
该投影不是证书。transpose(dirty) 精确返回覆盖域内与相同端口、相交 role 的样本/tag
支持相交的观察。merge 要求身份/域一致，重叠观察的规范 row 相同。请求级失败证据及
终端 RequestRecord 不属于原子成功证书。

内部 DirtyDeltaQueue 在一个证书 generation 内为每条记录保存 accumulated 和 propagated。
同一 mutex 下取 accumulated−propagated、记录已传播集合并清 queued；后到的新原子
可以再次入队。依赖结构由 demand coordinator 持有，与像素缓存 owner 分离；队列自身
不拥有像素或 worker。任何下游入队失败都必须令该 generation 失败。

Focused 检查为 test_footprint、test_value_fragments、test_dependency、test_dependency_dirty
及 test_input_snapshot，使用独立有限集合/可达性 oracle，覆盖未知 row、identity/swap、
role/tag 隔离、迟到 dirty、dtype/stride/owner 上限和快照 COW。通用快照身份与所有权见
[缓存模型](Cache-Model.zh.md)。

## C++ 分阶段程序与当前 Run 集成

OperationTraits 8 区分本地 `Atomic`、终端 `RequestRecord`、请求级失败交付、依赖协议
版本、continuation 字节上限与有限阶段数。注册时必须选择一个同步 callback 或一个
分阶段 start。分阶段程序要求 deterministic、side-effect-free。协议版本 1 使用 RegionRule::Dependency，允许 Typed/Axes/重复输入
静态推断，无需强制 Whole demand。编译器拒绝 RequestRecord 的全部出边，包括未使用
路径，并沿全部输入祖先计算 EffectiveAtomic。依赖计划保留未解析需求，不生成矩形近似。

`start_dependency` 复制校验后的 metadata、参数、original Q 与不可变输入 bundle
identity。Generic Atomic 每次最多一个 sample，image v2 每次最多一个完整像素。
RequestRecord 保留完整 Q。PerAtomOutcome 目前保留但拒绝注册，必须实现逐观察 outcome
交付后才能启用；修改标志不能使请求级失败 callback 获得合批能力。

Continuation 在宿主分配中原位构造。`poll` 只消费已提供 fragment，返回逐输出关联的
Need 或完整结果。`supply` 的每个端口必须精确匹配取数并集及 bundle identity。
即使不读取源像素，每个原子 row 仍保留 descriptor 证据。终端依赖与 original Q 单独
保存，不产生原子证书。证书 identity 包括 registry definition 实例、backend、全部
traits、参数、输入输出 metadata 和 bundle identity；只有同一契约下的不同原子查询
才能 restrict/merge。

Discovery 工作量、poll 数、continuation 字节及阶段输出/scratch 有限。读取、显式工作量
扣除和分配失败具有粘滞性，callback 忽略返回 Status 也会失败。某个父 observer 抛异常
不会阻断其他 observer。已接纳调用失败时，state 恰好析构一次并释放输入 owner，清理后
以取消优先。被拒绝的并发或重入调用不干扰活动调用。借用的 phase/query/service 引用
在返回后失效；禁止并发析构。State 必须使用授予的 allocator。

`ExecutionContext` 当前在 ExecutionRun 中用显式记录栈推进依赖模板。每个 start、poll
和 source callback 使用既有 CPU 队列与共享 waiting admission。Waiting 记录不占 worker
或未使用的活动预留。阶段在同一 MemoryBudget 中非阻塞申请；seal 归还未使用容量，
实际 state/input/output lease 继续计费。无法容纳最小工作集时返回 ResourceExhausted。
Source callback 精确填写请求矩形；稀疏端口仍通过 ValueFragments 供给。返回的外部分配
经受控 allocator 导入后才能转交使用者。

默认路径每次执行一个 Atomic 观察。既有同步 Whole 保留完整全局观察及 validation，
不能据此扩大分阶段 sample 查询。终端按完整 original Q 执行一次。直接
`OperationRegistry::invoke` 对已提供的不可变 Value 采用同一观察规则。Frozen 执行
保留捕获的图和输入 owner。当前 CPU 依赖执行按串行 ready 顺序推进，符合调用者的
maximum parallelism 上限。Legacy Whole/effect 边界每个 Run 执行一次，包括未连边
effect。Atomic stream 按配置的 tile 交付并释放，终端 stream 保留完整 original Q。

公开 progressive workflow 与 `test_dependency_program` 覆盖真实源发现、
legacy→staged→legacy 组合、完整 Q 终端、单 worker 推进、有限 admission、取消及
frozen 输入所有权。当前尚未发布共享结构记录或复用依赖 result cache。Dirty 传播、
共享 Flight、动态内建算子、C 分阶段服务和原生 GPU fragment 访问继续属于本轮 G4。
直接证书 API 已独立实现，不能据此声称这些缓存与调度集成已完成。
