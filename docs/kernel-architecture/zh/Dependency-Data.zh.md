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
和 source callback 使用既有 CPU 或选定的 GPU 队列与共享 waiting admission。Waiting 记录不占 worker
或未使用的活动预留。阶段在同一 MemoryBudget 中非阻塞申请；seal 归还未使用容量，
实际 state/input/output lease 继续计费。无法容纳最小工作集时返回 ResourceExhausted。
Source callback 精确填写请求矩形；稀疏端口仍通过 ValueFragments 供给。返回的外部分配
经受控 allocator 导入后才能转交使用者。

默认路径每次执行一个 Atomic 观察。既有同步 Whole 保留完整全局观察及 validation，
不能据此扩大分阶段 sample 查询。终端按完整 original Q 执行一次。直接
`OperationRegistry::invoke` 对已提供的不可变 Value 采用同一观察规则。Frozen 执行
保留捕获的图和输入 owner。当前 CPU 依赖执行按串行 ready 顺序推进，符合调用者的
maximum parallelism 上限。Legacy Whole 记录在实际请求时每 Run 至多执行一次，未连边 effect 仍执行一次。
纯 Whole 祖先延迟解析，后代合法缓存命中无需再次物化祖先像素。Atomic stream 按配置的 tile 交付并释放，终端 stream 保留完整 original Q。

公开 progressive workflow 与 `test_dependency_program` 覆盖真实源发现、
legacy→staged→legacy 组合、完整 Q 终端、单 worker 推进、有限 admission、取消及
frozen 输入所有权。成功的依赖 Run 现已发布不可变结构证据，见下文。活跃 demand
替换、共享 Flight 与依赖 result cache 复用已按下文及[缓存模型](Cache-Model.zh.md)
实现。C++ staged GPU fragment 访问已接入，详见 Fragment Atlas；C staged GPU 桥接同样已接入，有界 discovery 仍待实现。

[依赖采样算子](Dependency-Sampling.zh.md) 已实现 STMap 和动态 radius gather/scatter。
可选纯静态 validator 在编译时及直接 Empty 查询的 state 决策之前执行。


## C 分阶段程序

`dependency_plugin_api.h` 提供 ABI 8 的 C 分阶段协议。descriptor 必须恰好提供
一个 `execute` 或 `dependency_program`。loader 复制并校验有界程序表，并在
状态和回调存续期间保留动态库。宿主在 `start` 前将状态字节清零；只要进入
start，destroy 就恰好执行一次，包括 start 失败。Empty 仍运行纯元数据校验，
但跳过所有状态回调。

poll 提交逐输出、逐端口和角色的精确 run 与 tag 关联。Atomic 坐标对应当前
sample 或 HW pixel；terminal 关联不使用原子坐标，保留完整 original Q。
Need 阶段分配输出、完成时仍有未解决 Need 或未发布输出都返回错误。

阶段服务提供 checked read 与借用 fragment view，包含实际 dtype、origin、
有符号 stride、字节跨度和授权区域。跨 poll 必须使用 `retain_input`，其
handle 保留该 fragment 的精确授权和原 storage lease。每次 invocation 内
handle 单调且不复用，在 release 或状态销毁时失效。无效 handle 和被忽略
的服务失败保持 sticky。scratch 指针在 poll 返回后失效；输出指针在成功发布时立即失效，未发布时最晚
在 poll 返回后失效，发布后不能再访问。输出通过宿主 handle 显式发布；image 输出分配前检查完整 C。原生插件仍是受信任的
进程内代码，指针元数据校验不提供内存隔离。

`test_dependency_plugin` 加载真实 C11 模块，检查四种 dtype、负 stride、
两个远端样本与中间缺口、owner 保留、start 失败、忽略读取错误、重复发布、
取消、库生命周期和 full-Q RequestRecord。公开 workflow 只读取 16 字节，
验证实际 admission 边界及少一字节的失败，并检查取消清理后恢复执行。
同一测试和 C 模块也通过安装包分别消费静态库和共享库。

单独运行安装后的 C workflow 和独立检查：

```sh
cmake --build build/issue257-static/consumer-build --target photospider_dependency_consumer -j 8
build/issue257-static/consumer-build/photospider_dependency_consumer
```

只有 Atomic 输出 9、terminal 输出 11、精确源端点、拒绝缺口，以及所有权和资源
失败案例均通过时才返回零。consumer-build 由 `test_installed_consumer` 创建；
共享库安装将路径中的 `static` 替换为 `shared`。


## 运行时结构证据

成功 G4 dependency-network Run 的 `ExecutionResult::dependencies` 保留不可变
直接记录。Atomic 记录合并匹配 node/contract/snapshot 的完整逐观察 rows 和覆盖域；
Whole 与 terminal RequestRecord 保存不可分割 manifest。Legacy regional step
记录 callback 和 validation 实际使用的逐端口 demand。Source declaration、记录、
订阅和命名 root 共同受 metadata 计数限额约束，与受控像素字节分别计费。
证据不持有 Value、source callback、snapshot block 或 worker，在像素结果、graph、
registry 和 ExecutionContext 释放后仍可查询。

`coverage()` 表明证据完整的命名输出样本域。`certificate(node)` 返回已观察的
Atomic rows；未知 node、Whole 和 terminal 均返回 NotFound，不能据此宣称未知 row
为 clean。`potential_dirty(input, samples)` 对捕获关系及已记录输出子域精确，
只沿直接订阅使用真实 `DirtyDeltaQueue` 传播。同代稍后到达的新 delta 会再次传播。
入口 Footprint 复制、遍历和答案增长有总量边界，取消和 ResourceExhausted 均显式
失败，不返回部分 clean/dirty 答案。

该查询仅处理固定 declaration 下的 payload 变化。Descriptor/schema 替换沿重新
编译路径；样本记录图未表示 metadata-output 原子，因此拒绝 Descriptor-role
编辑。逐节点 certificate 仍保留非空间 tag，可直接在该节点执行 transpose，
不把缺少 metadata-only 上游样本记录误判为 clean。

`restrict({name: subset})` 沿已保存直接关联后向收缩所有相关 Atomic rows 和订阅，
移除不再需要的记录和 root。未知覆盖域拒绝；Whole 保留完整全局 manifest；
terminal RequestRecord 仅接受相同完整 Q。Atomic Empty 是已知空，省略的输出名
则不在结果中。该操作不读取像素或调用算子。

`test_execution_dependencies` 通过真实短/长菱形 workflow 验证 B/T 先收到 `{0}`
后再收到 `{1}`，用独立逐端口 oracle 核验，并在像素/context owner 释放后查询证据。
动态 scatter 验证排除项控制证据、相同输出的改边和旧证据隔离。Whole 和真实 C
terminal 测试检查不可分割 manifest；同一检查消费安装后的静态库和共享库。
公开 G4 workflow 也检查 radius 编辑与 frozen 旧关系的运行时证据。证据保持不可变，
订阅由下面的 context demand API 管理。

## 精确 demand 与不可变 binding 替换

`ExecutionContext::open_demand(plan, bindings)` 固定当前图契约及不可变 Value/snapshot
bindings。`DemandHandle` 副本共享 bundle 和 generation。`request({name: Footprint})`
返回精确 `ValueFragments`、诊断和结构证据，每个请求保留 original Q。Atomic callback
仍逐样本或完整图像像素调用；staged terminal RequestRecord 一次接收完整稀疏 Q。
Legacy 同步 terminal 只接受矩形 Q。Empty 输出验证静态 metadata，跳过 source 读取、
admission 和 continuation callback；未请求的名称不出现在结果中。
`execute_fragments(frozen, Q)` 对固定 bundle 提供相同精确查询，结果 generation 为零。

成功请求按精确命名 query 保留结构 publication。`replace_bindings` 比较旧记录所需
source support 的不可变字节，通过既有关系计算 potential dirty，将新 bundle、
generation 和累计 dirty 一起提交，不信任调用者 dirty hint。控制证据变化后，dirty
一直保留到该精确 query 成功重新发布，即使数值结果相同。`source_support()` 是字节
比较使用的有界取数并集，不能替代逐输出证书关联。图像比较包括完整 C，generic 比较
保留全部 dtype 位模式。静态 descriptor/schema 变化需要重新编译 plan。

替换在发布前验证全部 bindings。样本、metadata 限额或验证失败保留旧 generation；
与 request 发布或其他 replacement 竞争时返回 Stale，供调用者重试。已完成替换前
捕获的 latest 请求不能发布到新 generation，取消优先于 Stale。`freeze()` 固定当前
bundle，独立于后续编辑；`release(Q)` 删除一个精确订阅，但不取消活跃请求，后者仍可
重新发布该订阅；`cancel()` 停止该 handle
并退休其 publication 和 bundle。Context 析构先取消并排空活跃 demand 调用，再退休
既有 workers。直接 context 调用不能与析构竞争，已有 handle 调用可与析构竞争。
输入 owner 析构在 publication mutex 之外执行。

`maximum_demands` 限制存活且未取消的 handle，范围 1..65536、默认 1024；活跃 demand
调用上限为既有队列容量加 CPU worker 数再加一个 coordinator 槽。
`DemandConfig::maximum_metadata_entries` 限制每个 handle 保留的 query/evidence/dirty
metadata，范围 1..1048576、默认 65536。Handle 不拥有 worker 或像素 cache；当前调用
通过 context-owned Flights 共享同一不可变 bundle 中重叠的活跃观察，执行仍使用既有
CPU pool、WaitingAdmission 和计费 allocator。已完成 dependency 的内容缓存复用既有像素 LRU 和有界结构证明，见
[缓存模型](Cache-Model.zh.md)。C++ staged GPU fragment 已接入，C staged GPU 桥接同样已接入，有界 discovery 仍待实现。

`test_execution_demand` 覆盖稀疏结果、各 dtype snapshot、连续 dirty 累积、frozen
隔离、陈旧发布、独立取消及 context 排空。真实 C terminal fixture 检查稀疏 Q 仅调用
一次、Empty 不调用。公开 `g4_workflow` demand 场景使用直接 oracle 检查两次替换前后
scatter 两端的结果。

U2 兄弟工作集反例已成为真实 `test_dependency_program` workflow：A、B 各从 context
allocator 分配 1 MiB 输出和 3 MiB scratch。延迟解析子节点时还保留父节点的一个字节
continuation。恰好 4 MiB 在 A 的 callback 前拒绝；4 MiB 加该 state 可使 A 完成，但 B
在 callback 前被拒绝，失败返回后 A 的 storage owner 已释放。同一 context 随后仍可
单独执行 A，观察峰值为 4 MiB。5 MiB 加该 state 时父节点取得两个结果并返回 3，
观察峰值包含上述真实分配。该测试验证此顺序的有限拒绝和真实 lease 退休，不承诺最优调度。

## 共享精确观察 Flight

`request` 和 `execute_fragments` 认领单个 Atomic 样本/完整图像像素，或完整 terminal Q。
Key 绑定捕获的 bundle 身份、plan/operation 契约、节点、geometry、精确 query 和资源
策略。共享要求全部输入祖先的实现都 deterministic 且 side-effect-free。因此，即使
本地 callback 是纯函数，带副作用或非确定性的 Whole 祖先也会阻止下游共享。Dispatch
不扩大 Q，不合批 RequestFailureOnly 观察。

目录线性化认领，为 producer 分配唯一 FlightId，并将 waiter 的取消/当前性与 producer
token 分开。显式 token 和辅助 set token 都属于 waiter。调用方 coordinator 推进阶段并
等待依赖；callback worker 不等待其他 Flight。等待 callback 时 coordinator 检查祖先
waiter。发起父请求取消后，仍被独立 waiter 需要的子节点可以完成。Latest waiter 在
replacement 后变为 Stale 时，frozen waiter 同样可取得旧 bundle 的成功结果。

最后一个 waiter 取消后禁止新加入，后续请求认领新 FlightId。晚完成只有在目录 ID 仍
匹配时才删除该项，因此 P0 退休不能删除 P1。Context 关闭会取消活跃 producer，排空
demand 调用后再销毁 worker。`clear_result_cache()` 同时递增 dependency epoch 并使已完成缓存的保留资格失效。`maximum_dependency_flights` 用同一个配置数值
分别限制活跃 Flight 和全部订阅者的数量，范围 1..1048576、默认 65536。Metadata 容量
耗尽直接返回 ResourceExhausted，不等待容量。

共享成功值携带不可变直接记录，包括精确 certificate 或不可分割 manifest，以及相关
上游记录的链接。导入按拓扑顺序遍历链接，在各 waiter 的 execution evidence 中保留
逐输出关系。结构记录不持有像素/snapshot owner。其析构使用无需分配的迭代退休队列，
长链失去最后 owner 时也适用。即使禁用结果保留，context `cache_statistics()` 仍包含
活跃 dependency Flight 和共享订阅；逐调用共享诊断不重复计入 callback timing。

集成测试覆盖 legacy/staged 共享祖先、显式/辅助取消隔离、非纯祖先排除、旧 P0 与新 P1
重叠、latest/frozen 竞争及导入证据的 dirty 查询。直接生命周期回归检查 epoch/限额和
20000 个链接结构记录的退休。公开 workflow 用两个精确 waiter 和有界 callback barrier
证明 callback 只执行一次，一个 waiter 取消，另一个得到 7 及完整 identity 依赖证据。
带 barrier 的 terminal 用例证明相同稀疏 Q 只共享一次、较小 Q 单独执行，导入的 terminal
证据仍拒绝子集 restriction。含非有限样本的显式联合请求失败时，共享的正常点 waiter
仍成功；后序原子先完成也不改变联合请求的规范错误。独立 `test_scan_waiters` 现在用真实
`numeric.ordered_scan` 和共享成功前缀状态检查这些边界。


## 已完成内部 checkpoint

C++ `DependencyPhase` 提供 `checkpoint_before(phase, sequence)` 和
`checkpoint_publish(phase, sequence, state)`，用于纯 Atomic 程序的可选完成状态服务。
RequestRecord 调用产生 sticky InvalidArgument，即使 callback 忽略错误也拒绝。
不可变 state 必须由当前阶段 allocator 分配，同属宿主但来自其他阶段的分配不接受。
宿主复制已成功供给的规范 history 前计量 metadata/work。查找验证 operation/static
contract、backend、输入 bundle、宿主节点 scope 与 sequence，并导入完整 witness。
Checkpoint 不授权读取当前阶段未供给的 fragment。

每个活跃 context scope 在精确集合元数据限额内至多保留 64 个 checkpoint；context
目录由 `maximum_dependency_flights` 限制，仅持有 scope 弱引用。State payload 纳入
现有 live allocation budget。内存 admission 可以清除可选状态；清除结果缓存会分离旧
scope。已借用 state 随自身 lease 退休。查找不等待 producer，此接口不发布错误或取消
结果。因此查询可以复用另一活跃查询的成功前缀，而不继承后续错误或取消。

保留证据构造使用有界可选 dependency-cache work allowance，耗尽后可以重算；导入
使用普通 dependency work 限额。Checkpoint 提供活跃同 bundle carry 复用；跨 bundle 的纯块 transition 使用下述独立服务。C 分阶段桥通过阶段内 opaque handle 和复制的字节 state 提供这些服务。


C checkpoint 服务为 `checkpoint_before`、`checkpoint_read` 和
`checkpoint_publish`。查找成功但未命中时 handle 为零；命中时导入完整成功证据，并
返回 sequence 与字节数。非零 handle 仅在当前 poll 内有效，沿用 invocation 单调 ID
空间，不授权输入读取。Read 复制正长度、范围内的字节区间，不暴露 storage 指针。
Publish 从当前阶段 allocator 分配 packed UInt8 Value 并复制正长度 opaque state；
算子应声明足够 workspace。State 必须编码完整确定性算法值，不含指针、handle 或
未初始化 padding。调用方修改源字节不影响已发布状态。复制前计量 work，非法 handle、
区间、scope 或 terminal 使用均产生 sticky failure。

C checkpoint 回归检查部分字节读取、carry 副本、过期阶段 handle、非法地址/区间、
terminal 拒绝、前缀证据导入，以及五个输出恰好读取五个输入的真实 C scan。
相同 C11 module 和测试源码参与静态/共享安装包消费检查。


## 纯块 transition 缓存

`DependencyPhase::block` 求值有限纯内部状态 transition。Compute 只依赖当前供给输入、
显式 incoming state、静态算子参数/metadata、phase、半开 range 与 mode。所有 carry
控制和数值均编码进 incoming，不读取原输出 Q、此前未供给 fragment、时序或隐藏可变
状态。这是可信算子契约，不分析任意代码、不授权 RequestFailureOnly 输出合批；
RequestRecord 调用拒绝。

Session key 哈希不可变 registry/operation 契约、输入/state metadata、规范精确供给
集合及实际 dtype 宽度 bits、phase/range/mode 和实际 incoming state。Snapshot identity
与原 Q 仅为 provenance，故不进入 transition key；不同 registry 自带唯一运行时实现
身份。不保存失败块。命中要求 descriptor、region、facets 与 incoming 相同，并复制到
当前阶段 allocator；新算出的 state 也必须由该 allocator 分配。保留当前供给/history
作为证据，不通过块缓存导入旧前缀证书。

ExecutionContext 的可选 `DependencyBlockServices` 使用现有计量结果 LRU，仅接受纯且
cacheable 的祖先链。样本哈希前扣除 `maximum_dependency_cache_work`；零或耗尽时
直接求值、不查找/保留。查找和发布核对 epoch，防止旧 Run 重新填入已清除的缓存。
借用 Value 的 lease 保持驱逐后的寿命。内部 hit/miss 与完成输出 `cache_hits` 分开计数。
Direct host 可提供相同服务，异常及被忽略的失败仍 sticky。

真实 scan 回归将 `[1,2^54]` 前的 incoming 0 改为 1：改变的块重算、早期输出从 1
变为 2，仅在完整 carry 重汇合后复用后续匹配块。Mean/variance 检查第一遍未变块复用
及 mean 改变后的第二遍 miss。Direct 协议测试让两个不同实现 registry 共用一个 host
cache，另检查错误 metadata、外部分配和忽略 host 错误均拒绝。C 桥也提供 `block`，其只读服务表仅有当前输入 read、计量 scratch、work 与取消，
不提供 association、checkpoint、retained-owner 或输出发布。Compute 前复制 incoming，
仅成功时复制 outgoing 回调用方，故两者缓冲区可以重叠。Compute 接收宿主分配并清零
的 outgoing，必须写完整 state，返回 SUCCESS 或错误；NEED 非法。两份状态副本和
scratch 必须容纳在声明的阶段 workspace 内。

C callback 的 `user` 仅能传递由 key 输入确定的数据或固定的注册实现常量，其他不可变
配置与地址不自动进入 key。每个 phase/mode 在注册实现内标识一个固定算法；另一种
transition 必须使用不同身份。C11 scan workflow 检查初次六次计算、重汇合编辑后三次
计算加三次命中，以及完整当前源支持。空 callback、忽略非法 read、NEED 返回、scratch
耗尽和普通 compute 失败均拒绝；重复失败请求仍实际调用 compute。


## 稀疏 GPU 传输

[Fragment Atlas](Fragment-Atlas.zh.md) 说明已实现的精确 atlas/mask 目录、SDK MSL
lookup helper 及原生传输验证。C++ staged GPU 执行已接入，有界 discovery 仍待实现。


C++ staged GPU 已通过 [Fragment Atlas](Fragment-Atlas.zh.md) 接入现有 GPU worker 与
共同 admission。每阶段按需 materialize 精确端口，工作量预扣、真实 native 容量独立计费，
错误 sticky，普通/stream/frozen Run 均保持辅助取消优先级。C staged GPU 桥接同样已接入，有界
GPU discovery 仍属 G4 剩余工作。
