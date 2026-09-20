# Compiler 与本地 Execution

## Compile stage

`Compiler::analyze` 检查 current `GraphSnapshot`、bounded document count/text、unique
node/output id、reference、port、operation availability、input count、每个 operation 的
closed required typed parameter schema、deterministic acyclic topology 与 static
scalar/preserve/match/fixed output descriptor inference。unknown、missing 或 wrong-type
parameter 会在 IR publication 前失败；builtin 不合成 default。它按 node-id tie-break 的 topological order
发布 immutable `SemanticGraphIR` 与 `SemanticGraphDigest`。

Fixed inference 验证 logical nonzero rank-1..8 descriptor，不要求 dense element/byte
product 可表示。C++ embedding operation 可把该 descriptor materialize 为合法 strided 或
zero-stride broadcast Value；plan 记录独立声明的 `estimated_bytes`，因此 modeled admission
与 peak diagnostic 反映 callback 的实际 materialization，而不是隐含 dense allocation。
显式调用 `Region::element_count()` 时仍执行 checked arithmetic，对同一个 multi-dimensional
logical shape 仍可返回 overflow。不携带 stride 的 C DSO Fixed descriptor 会在 load 时被
独立要求 dense representability：其 checked uint64 contiguous product 必须产生 total
bytes `B > 0`，并满足 `B - 1 <= INT64_MAX` 与 `B <= SIZE_MAX`。这个仅属于 DSO 的
address/allocation boundary 不约束 C++ broadcast descriptor。

每个通过 schema validation 的 Float64 parameter 都以 copied IEEE-754 binary64 的精确
bit、按 fixed little-endian order 进入 canonical stage identity。signed zero 会被保留，
因此 sign-sensitive callback 不能共享 semantic、optimized、plan 或 cache-key identity。
Compiler 不引入 finite-only rule，也不规范化 NaN payload 或 infinity。

`Compiler::optimize` 在当前 baseline 中是显式 conservative no-op。它把 semantic node
复制进独立 `OptimizedGraphIR`，并产生 domain-separated `OptimizedGraphDigest`。

`Compiler::plan` 复制 dependency-ordered step，选择 CPU 或声明支持的 optional local GPU
backend，记录 estimated bytes，并使用 Whole、elementwise-exact 或 clipped Halo rule
把 optional named output Region 反向传播为每个 step 的 output/input demand，然后产生
`ExecutionPlan`、`ExecutionPlanDigest` 与
`PlanCacheKey`。任何 stage 都不包含 callback pointer、native device 或 daemon object。
Node 或 step 可以保留已注册 static program 的 immutable `PreparedOperation` owner；该
owner 保持 definition lease，等依赖它的 plan/Run owner 释放后再销毁。Preparation state
不是 runtime mutable state，也不进入 semantic 或 cache identity。每个 stage 还携带 exact
frozen operation registry 的 private runtime-only weak identity；它不进入
digest/serialization。

## Static operation preparation

`OperationDefinition::prepare_static` 是 NUM-01 等 deterministic operation 使用的公开
纯 preparation hook。`OperationRegistry::prepare_operation` 先验证完整 static input
metadata 与 parameters，包括 copied IEEE-754 parameter bits，然后在 registry
synchronization 之外调用 hook 一次。返回的 `OperationPreparation` 在
`PreparedOperation` 中拥有已解析 output metadata 和 optional immutable state，不包含
Value payload、Run data、I/O state 或 private mutable cache。

Compiler node/plan step 跨多次执行保留这个 owner。Direct request 可以传入匹配的已有
handle；没有 handle 时，每次 direct preflight 准备一次。Joint request 为兼容成员准备
一次。不同调用不会仅因 identity 相同而自动共享状态。Request 自己拥有复制后的 record，continuation 收到的
`DependencyQuery` 仍是 borrowed，不能保留。Preparation 与 plan 使用普通宿主分配，
位于 per-Atom runtime scratch admission 之外；目前没有单独强制的 preparation budget，
算子必须限制静态源码与程序大小。Continuation state 仍受 runtime limits 约束。没有
global preparation cache 或 dynamic preparation state。Session 先销毁 continuation，再释放
prepared owner；prepared owner 先销毁程序，再释放 definition/library lease。外部 registry
owner 可以提前释放，已有 lease 仍保持有效。

## Execution

`ExecutionContext` 拥有固定 CPU pool、optional single-worker GPU callback lane、每个 lane
一个 deterministic FIFO、frozen operation registry 与 共享受控缓冲区预算。两个 FIFO 对尚未
开始的 callback 共享 single nonblocking `maximum_queued_tasks` admission。Worker 在进入
callback 前释放 move-only admission token，因此 running callback 不占 waiting bound；
rejection、exception 与 shutdown drop path 会把 token 恰好释放一次。`execute` 创建一个
private `ExecutionRun`，采用 deterministic ready-step ordering 与 caller-selected maximum
parallelism。

Run 在其 mutex 下只有一个 first-failure linearization。每个 scheduler、waiting-admission、
backend-queue 与 callback failure 都会在 first storage 前立即重新检查 cooperative token 与
plan currentness：cancellation 优先于 graph `Stale`，graph `Stale` 又优先于 original
failure。该 selector 只执行 scalar/currentness observation，不分配内存，并被 no-throw
diagnostic-construction fallback 复用。若两种 stop 均不存在，且 copied trait 明确拒绝
fallback，则 unavailable GPU 仍保持 `BackendUnavailable`；普通 admission/queue rejection
也保持原 category。

Run-loop 对 cancellation 或 staleness 的 observation 本身也是 no-throw boundary。若 Run
首次观察到任一 stop 时仍有 active callback，而 owned diagnostic 或 `Status` 构造失败，
它会在继续持有 Run mutex 的情况下，以空 message 记录相同的 prioritized code。该
fallback 既不重新取得该 mutex，也不让任何 in-flight slot 退役；它停止新的 admission，
并等待每个 callback 正常退役。Maintained regression 会直接证明这一 ownership：唯一
CPU worker 被占用期间，先 admission 目标 CPU callback，再把另一个独立 CPU Run 作为
FIFO successor 排在其后。Successor 完成证明目标 callback 已完成 abandonment；此时另一个
GPU callback 仍被 gate 阻塞，目标 future 必须继续保持未完成。

从任一 backend FIFO pop 的每个 callback 都会进入 Run mutex，并在复制 dependency、
transfer Value、取得 已预留的缓冲区分配 或调用 operation 前，使用同一个 no-throw
external-stop observation。若 failure 已存在或刚被选中，该 worker 会通过 abandonment
path 精确退役自己的唯一 in-flight slot。Direct CPU work、GPU work 与 GPU-to-CPU fallback
都会汇入这一 queued-attempt admission cutoff；observation 本身绝不退役 slot，
`GraphContext` 也不会注册或通知 Run。Cutoff 之后发生的 cancellation/replacement 仍可能
与 transfer、resource acquisition 或不可抢占的 in-process operation entry 竞争。该较晚
区间继续采用 cooperative/best-effort 语义，并不承诺 global mutex 或 forced preemption；
completion 与 final result publication 仍会严格拒绝观察到的 cancelled/stale outcome。

在 operation-registry boundary，invocation validation 保持如下顺序：operation lookup、
input/demand count、逐 input validity 与 demand bound、parameter、cancellation、闭合的
CPU/GPU backend vocabulary、backend capability 与 static descriptor compatibility。
读取任何 input descriptor 前都必须先确认其 `Value` 合法。未知 backend representation
返回 `InvalidArgument`，不进入 C++ 或 DSO code；已知但不支持的 backend 仍返回
`BackendUnavailable`。Capability 通过后，registry 预计算唯一一份预期
output descriptor/facets，复用共享推断。Preserve/Match 独立于 dtype 检查 shape；显式
input dtype、shape 和语义冲突在 callback entry 前返回 `TypeMismatch`。Callback 返回后复用同一
descriptor 验证 output type、shape 与请求的 Region，包括 default-invalid output。这不会
重复 Run 的 plan-derived demand coverage check，也不会重复 DSO adapter 的 contiguous-
layout/facet view validation。

MetalFp32 计划暴露 Upload/Operation/HostAccess。Run 验证实际打包上传字节与容量，
复用原生 buffer，检查回退新增上传。CpuStorage 可持有完成后的 Metal shared buffer，
CPU 访问不强制再次复制。原生输入/输出/scratch/保留副本共用受控预算，单队列同步服务
在既有 completion/currentness 边界前排空设备工作，原生句柄私有。

缓存按 CPU 精确与 Metal 数值/实现/设备身份隔离；回退及后继不写预期原生结果，Metal
模式关闭磁盘读写。参见 Cache-Model 与 S4-Workflow。

每个 operation result 都会按 planned element type/shape 检查。每个 producer Value 在
transfer/callback entry 前必须覆盖 consumer planned input demand；callback 与 ABI v9 input
view 会接收该精确 demand。图像和区域源 Run 惰性物化需求 tile，Whole/副作用边界每个 Run
完整物化一次，参见[区域语义](Region-Semantics.zh.md)。Execution context 必须使用
产生 plan 的同一 frozen registry。Work 前、completion 期间、result assembly 前，以及
全部 named Value、diagnostic、plan/result digest 与 execute timing 组装完毕后，都会检查
cancellation 与 plan currentness。Run 在最终 cancellation-then-currentness recheck 期间
保持其 mutex；紧邻唯一 success return 通过该检查，构成 success-publication
linearization point。Late cancelled/stale local result 及其 diagnostic 会被丢弃，全部 Value
与 resource owner 正常退役，不能进入 caller-visible `ExecutionResult`。

Operation ABI v9 增加宿主管理同步 GPU 服务，callback 能区分 ordinary
failure 与 backend unavailable。只有 optional GPU attempt 返回显式 backend-unavailable
result、没有调用 output sink，且 copied trait 允许 fallback 时，executor 才会在 CPU
上重试。只要尝试发布 output，backend unavailable 就变为 terminal：accepted output
是 contract failure，rejected output 保留 sink failure。ordinary failure 与 unknown
nonzero callback result 会让 Run 失败，不产生 CPU attempt。

## Diagnostic

Raw diagnostic 包含 compile-stage duration、execute duration、operation attempt
timing/outcome、selected backend、transfer count/bytes、实际分配峰值、fallback reason、
`strict_math_calls`、8x4 的 `function_fallbacks` matrix、plan digest 与 result digest。
它们是 observation，不是 verdict 或 release evidence。

NUM-01 每次 strict math call 计一次 `strict_math_calls`，并按 function 记录 fallback。
Merge 后无法归属的差额进入 `Other`；未 instrumented operator 不推断调用数，保持为 0。
这些诊断字段是 observation，不表示 NUM-01 已完成。

## Runtime input 降级与执行

Schema 2 在 semantic publication 前验证所有 input declaration，将 canonical table
复制到 semantic IR、optimized IR 和 plan。Ordered source 保留 node/declaration tag。
Scalar port 接受 declaration 或兼容 producer 的 Float32 `{1}`：generic、dimensionless
Scalar 或 dimensionless 单样本 Signal。Analyze 检查 dtype/shape/已知 facet，并继续检查
直接 declaration 的消费 interval 交集非空。Image consumer 要求 declaration
的精确 profile 或 producer 的 image output guarantee，通用 producer 不隐式获得该保证。

`execute(plan, bindings, cancellation, options)` 复制 input name 和 Value metadata，
先检查 name multiset，再按 declaration id 检查 Value，最后检查全部直接 scalar
约束，之后才允许首个 callback 或 transfer。图像/蒙版像素仅检查消费区域，检查先于其消费 callback。
Computed scalar 在依赖/缓存查询之后、消费 callback 之前检查 metadata、完整 coverage
及 finite/range；数值错误为 OperationFailed，metadata 失配为 TypeMismatch，直接绑定
数值错误仍为 InvalidArgument。Entry 在读取 binding/token 前将 default、
stale 或 foreign-registry plan 判为 Stale。Entry 后 cancellation 优先于 Stale 和普通
binding failure。长数值扫描周期检查 cancellation 与 graph currentness。
Run-owned snapshot 保留到全部已准入 callback 退场；返回 Value 独立拥有 immutable
bytes。Run 不共享可变 binding/result，也不按 plan digest 重用 output。

外部输入初始 backend label 为 CPU，沿用显式 transfer/fallback 路径。调用方已有 input bytes
不计入 maximum_live_bytes；源读取和收集/流式输出缓冲区计入预算。每个 step 的上限包含输出容量、workspace_bytes，以及
workspace_input_multiplier（0..16）乘需求输入字节数。图像不再预留重复 sink copy。
执行器在回调之前预留保守完整工作集，包含可能的传输；输出、复制、scratch 各自取得
子租约。每次 invocation 的上限防止 scratch 占用其他 step 预算。临时竞争仅等待活动工作，
同时检查取消/currentness；外部结果保留导致容量不足时失败。

fan-out/重复边逐个计数。回调局部输入先于完成信号释放；非命名输出在最后 reader 完成后
清空 producer 槽位。共享存储只有一个租约。返回 Value 可晚于 Run/ExecutionContext 销毁，
完成时退回未用预留，最后一个结果所有者销毁时退回保留容量。诊断区分 planned_peak_bytes、
实际 peak_live_bytes、retained_input_bytes 和 peak_active_tasks；元数据/栈/RSS 不计入。
test_memory_liveness 验证 gated fan-out、跨 context 生命周期、预算恢复及精确/少一字节
workspace 上限。RawBenchmarkOptions.bindings 在入口复制一次，供每个独立编译 sample 使用。

直接 OperationRegistry::invoke 与物理规划共享输入需求推导：在 callback 分配前拒绝
不完整 RGBA 输出、非整图 Whole 输出、不足 halo、蒙版空间 shape 不匹配和 Value 未覆盖
其声明需求。Whole 缓存在最后剩余边界/输出 reader 完成后释放，包括连续 Whole 链；
test_regional_execution 验证三节点链的 16/15 字节预算。

Run 完成同时等待队列 callback 所有者和逻辑 in-flight step 退场。最终作用域在成功、
取消和失败时清理 Run 持有的 Value/binding，因此调用方释放返回结果后立即退回其
payload 容量。test_memory_liveness 的私有 callback-body gate 使用八字节预算验证
成功/取消边界，队列元数据退场不再延长结果缓冲区的保留。

S4 diagnostic 增加每算子 native dispatch/设备时间、输入复制、收集输出复制、shared host access 和原生缓存复用，均为实际观察。

## 算子基础（共享契约切片）

#289 实现 ABI/traits 7、`SemanticDescriptor`、静态 dtype/axis/重复输入推断及 IR/plan
中的真实 output facets。新增 axes/typed contract 使用 Whole，不增加 G4 映射。完整约束/
推断 facets 进入 v7 compiler domain 和 v3 result-region key，no-op optimizer 保持 v5。
公开 helper 和阶段限制见 [Plugin ABI](Plugin-ABI.zh.md)。Computed bounded scalar 消费与
受支持 image-v2 snapshot/cache 已实现。采样域元数据与样本值单位分别保留，并进入符合
资格的 result key。Numeric、channel/color、expression/LUT、component 已使用这些
契约；[独立 foundations workflow](Foundations-Workflow.zh.md)运行其公开组合。

## G4 分阶段执行

当前 package 0.9、ABI/traits 9 增加依赖计划模板与 C++ start/poll/supply 协议。
同步实现保留 Whole 推断规则；依赖实现可在运行期发现逐端口精确 fragment。
已实现行为和剩余集成范围见 [依赖数据与执行](Dependency-Data.zh.md)。

## 独立结果降低

M2（#305）为每个 SemanticNode 提供有序 outputs，每项独立推导 descriptor、facets 和
EffectiveAtomic。Workflow 的生产者端口名称解析为 ValueRef{node_id, output_index}；
终端 RequestRecord 端口不能供给消费者，Atomic 兄弟输出仍可组合。每个 PlanStep 选择
原始输出索引并携带单输出契约。未引用的纯结果步骤被移除，具有副作用的单输出根保留。
生产者引用指向所选物理 step，不同 shape/type 不共用节点级 metadata 槽。
semantic/physical v9 身份分别编码有序输出和所选索引。裁剪保留图选择的分阶段执行协议
及既有 Whole 流式行为。运行时缓存/记录路由与联合执行由 #306–#308 分别交付。

## 独立结果执行

M3（#306）按 `ValueRef` 路由证书、订阅、脏区传播、冻结快照结果、flight、
Whole 复用与诊断。公开证书查询接受结果引用；耗时与后端记录标识所选结果。
内容 witness 编码所选命名契约，不包含全局 node ID。静态参数保持完整，样本
证据跟随实际读取。

同步回调仅接收所选输出声明的输入。`input_indices` 保留原始端口编号，
`input_metadata` 描述完整静态输入签名。C value view 暴露 `input_index`。
显式空投影执行时不获取输入样本；缺省投影保留全部输入行为。分阶段读取超出
所选输入投影时验证失败。空输入集合不会执行其生产者。

`test_multi_output_execution` 覆盖独立兄弟缓存、证书、脏区订阅、冻结快照、
命名 C 输出及未使用的失败生产者。既有单输出 fallback 文本保持原样；命名
输出附加端口名称以标识失败结果。可选联合执行已由 #307–#308 实现。


## Atomic 执行组

Plan 对同一语义节点的不同 PerAtomOutcome 结果暴露可选 CPU 执行组。
Coordinator 先登记全部已知根需求和新发现的输入需求，再立即从每输出选择一个
就绪观察，不等待未来请求，也不跨 Run 合组。成员 shape/ROI 可不同。
关闭 `enable_joint`、不足两个成员、无联合实现或共享预算不足时使用 singleton。
跨 Run 继续按 observation 共享 flight，允许混合 cache hit 和已有 flight。

C/C++ joint poll 独立验证各成员 Needs/Complete/error。相同读取可共享传输，
各成员保留独立关联与错误。成功成员独立完成 flight 并缓存。无法归属成员的
执行错误先释放共享临时资源，再逐一 singleton 重试尚未完成的成员一次；
取消、失效和协议错误不重试。共享 work/backing owner 只计费一次。
仅当所有剩余观察均无 waiter 时取消共享计算；上游错误只退休对应成员。
RequestRecord 保持独立。协议、调度与真实组合分别由 `test_dependency_joint`、
`test_joint_execution`、`test_multi_output_ops` 及安装示例验证。

分阶段完成结果的内容模板编码所选输出契约、静态参数、可观察输入元数据与生产者
契约，不包含 plan、graph、node 或物理 step ID。实际样本身份来自 Data/Control/
Validation witness；公开 binding name 标识输入路由。跨 plan 命中时，按对应
输入端口重绑定各 record 与 certificate，再发布；歧义拓扑只导致可选缓存未命中。
Flight 身份仍限定 plan/snapshot。模板 hash 与重绑定计入 cache-work 预算。
回归覆盖节点/输入声明重编号、兄弟裁剪和多层缓存生产者 DAG。

## 按节点推导的数组视图

标记 `requires_metadata_specialization` 的 C++ definition 提供纯
`specialize_metadata` callback。Registry 先验证普通参数与输入契约，在 mutex 外通过
保留的 definition lease 调用，再验证固定输出数量及 metadata。Compiler 与直接入口
使用 `resolve_traits`；未解析模板不能推导占位 descriptor。Shape、dtype、facets、tuple
分组、payload 上限和 static dependency pieces 成为不可变节点 traits 并进入阶段
identity。该过程不得读取像素或依赖查询改变 metadata。每个 piece 保持不相交的
observation coverage 与完整各端口 dependency，替代旧的 static dependency maps 表述。

CPU staged 输出的 `maximum_output_payload_bytes` 替换 dense payload admission 下限。
发布时另外核对新增输出 owner 的实际容量；借用 owner 必须已由该 session 接收。
源 owner、metadata 和 workspace 仍计费。因此 scalar-backed constant 和 source-backed
broadcast 无需预留逻辑 dense 字节。普通、直接和 structured 执行保留单一覆盖 view；
多个 owner 使用 `execute_fragments` 或显式 dense layout。`ValueFragments::collect`
显式生成 packed copy，并遵守取消。

`make_default_operation_registry(false)` 允许在 built-ins 旁注册 embedding 算子。
编译或执行前必须 freeze。成功自定义注册清除 built-in persistent-cache identity，
失败注册保持不变。默认工厂调用仍返回 frozen registry。

## 结构化 schema 特化

纯每节点 metadata specializer 可以解析 protocol-2 Result schema，同时保留注册的
Result kind、schema id 和 version。此路径拒绝返回 Value metadata、tuple grouping
和物理 Value flags。解析后的闭式 SchemaTemplate 在输出推断前完成校验，并进入
现有 semantic、plan、cache identity。此能力不改变公开记录布局或 C ABI。
CRV-09 使用它解析固定 measured report metadata 和 owned sampled-table shape；
源采样、报告与 table gate 仍是同一 frozen workflow snapshot 中的普通节点。

## 区域布局执行

当前布局算子通过每节点 metadata specialization 在执行前解析 shape、permutation 或
counts 与 layout。适用时，`regional_atomic` 将原始 query 及其 normalized 请求矩形集合
传给 callback；每个逻辑样本仍是 Atomic observation，不把矩形集合转换成一个 Atomic
observation。`preserve_output_views` 允许合法 affine view 保留 source owner；此时 output
payload admission 通过现有 nonblocking reserve 和 cache-reclaim 路径按实际新分配容量
计算，不按逻辑 dense 大小预留。由于这些算子设置
`cacheable=false`，content cache 不复用同一物理 owner/stride 分区；pure 与 active-Run
sharing 仍是独立路径。

Dependency certificate 和 `NeedBatch` metadata 在各自公开边界复制并计费。
`DependencyCertificate` 或 `DependencyNeedBatch` copy 会重新准入 metadata 容量并拥有
新的 metadata owner，不复制源 owner。宿主在接受可变 batch 前调用 private
`reseal_metadata()` 重新封存。`DependencySession` 及其 callback 优先使用当前 TLS
resource root；没有 active TLS 时恢复 session start 保存的 root，包含跨 scope 的 work
和 metadata。`ValueFragments` publication 通过 lifetime token 保留已发布 metadata；每个
发布的 `Value` 保留 immutable storage alias，直到最后 owner 销毁。

这些机制不改变 legacy 边界。调用方取出裸 `Value` 或返回 raw vector 后自行复制时，不在
publication token 的计费范围内。Empty 容器和当前实现内部的 geometry 工作尚未全面计费；
受控资源模型也不证明所有进程 RSS 都受到控制。

## 分段静态映射

`static_dependency_pieces` 用互不相交的 coverage 集合划分完整的推导 observation
域。每个 piece 记录逐端口 Data/Control/Validation 关系和可选 descriptor tags。
选择的输入轴可以加有符号 translation；固定区间的 translation 必须为零。证书
创建按对应 piece 坐标检查源域，backward 与 transpose 使用加宽算术和精确裁剪，
因此可表达 concatenate 分段而不枚举其中的逻辑样本。

Session 在回调前将每个声明 piece 与 Q 求交，分别保留 descriptor evidence。
裁剪、axes/tags 复制和 descriptor 扩展计入 Session 及 shared work 限额；保留的
geometry 容量包含 vector 扩容及构造重叠。资源不足不会扩大 Q 或请求未命中源端口。
动态 regional 算子保留有界显式行：gather 记录观察到的索引位置，scatter 记录全局
索引扫描和每个输出的实际贡献者。

## Whole callback 的静态准备

CPU Whole callback 可以复用不可变静态 preparation。执行器通过
`OperationInvocation::prepared` 传递 plan 持有的 owner；registry 核验定义、完整
metadata 和参数原始位后，再进行 callback 验证并向规范化调用提供 owner。直接调用
未提供 handle 时准备一次。prepared state 不包含运行期输入字节。

`OperationOutputSpecialization::input_indices` 可依据静态 metadata/参数收窄 CPU
Whole 输出的注册输入投影。缺省保留注册投影，空 vector 不读取任何 payload；重复、
越界或扩大注册投影均拒绝。完整 metadata 始终必要，投影复用既有 traits/digest 字段。
CPU Whole Atomic 输出可保留 generic trailing-axis tuple 身份；GPU、image tuple
及其他非法组合仍被拒绝。

### CPU Whole 输入视图

package0.18 / OperationTraits16 扩展 CPU Whole 视图发布。视图输出优先保留
覆盖完整输入需求的原始 Value、strides、storage 和 resources；不存在单个
覆盖 Value 时，Auto 可以 collect，`requires_input_views=true` 则在 callback
之前返回 Domain/Run 的 InvalidArgument/InvalidDomain、ViewUnavailable。
该字段要求 CPU Whole 的 `preserve_output_views`，排除 GPU/joint/Result，
并参与编译身份。typed 验证仍覆盖全部有效输入。普通和 structured 执行桥接
遵循同一规则。

Whole 支持显式输出 payload 上界和按实际分配计费。借用输入 owner 独立计费；
callback allocator 限制为输出上界加 workspace，分配失败保持 sticky；新返回
backing 也必须满足输出上界。直接调用已经逐输入提供单个 Value。

C++ traits/specialization 布局改变，安装消费方必须重编译，拒绝 package0.17。
canonical framing14、document2、C operation ABI9 和 provider ABI1 不变；
traits16 改变语义身份，不引入 daemon 所有权或持久格式变化。
