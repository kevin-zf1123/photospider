# ADR 0017：CPU 区域执行与存储

- 状态：Accepted
- 日期：2026-09-09
- 接受记录：维护者在本任务中明确要求实现完整 S2 计划，包括 0.4 / operation ABI 4 和遵守保护规则的 PR 交付。
- 基线：`main@70b760fee96575391b825df1b179480407f33a2b`
- 决策 Issue：#263；实现：#264、#210、#211、#265、#266
- 权威来源：[英文 ADR](../0017-cpu-regional-execution-and-storage.md)

接受记录定义目标，不代表实现完成。GitHub Issue 维护交付状态。ADR 0015 保持产品
归属及排除项。本 ADR 替换 ADR 0016 的完整运行期存储/输出、整图像素预检、
operation ABI 3 和回调估算预算条款；源 schema、不可变绑定快照、图像 profile、
标量范围和取消/currentness 优先级继续适用。

## 研究与方案

基线中 `Value::create` 按完整 descriptor 检查地址，算子宿主要求完整输入/输出，
物理需求虽已传播但不按区域物化。ExecutionRun 保留所有中间结果，却在回调结束后
释放估算资源租约，因此无法证明区域执行的内存上限。

[libvips evaluation](https://www.libvips.org/API/current/how-it-works.html) 提供区域源与
sink 驱动的参考；[Halide scheduling](https://halide-lang.org/docs/tutorial/lesson_08_scheduling_2.html)
将算法与计算/存储调度分离，并讨论局部性、重算和并行的取舍。S2 在现有编译器与
ExecutionContext 中采用区域需求和按完成时刻管理的存储，不引入框架依赖或未经
测量的速度结论。

整图执行后裁剪不能满足小 ROI 的内存验收；同时保留 ABI 3 与区域 ABI 会增加一套
输出/生命周期契约，维护者选择统一升级。进程 RSS 约束需要范围外的分配跟踪；
S2 约束受控计算缓冲区。原生设备、跨 Run 缓存、正向脏区传播和磁盘派生数据留待后续。

## Value 与 CPU 存储

Value 分离完整逻辑 descriptor、有效 Region、存储视图及共享只读所有者。视图具有
显式逻辑原点、字节偏移和有符号 strides。地址检查覆盖有效 Region，并验证 rank、
包含关系、整数运算、元素末尾与分配边界。负 stride 和广播视图在证明安全时仍合法。
发布原子完成，副本只共享不可变存储。

宿主可写分配在发布后转为只读；可写输出不得与活动输入或其他可写分配重叠。只读
视图可共享所有者。所有使用回调完成且全部 Value/视图引用释放后才能复用。fan-out
和重复边不能提前释放。结果的租约保持到最后一个副本销毁；计账状态可晚于
ExecutionContext 销毁。

ExecutionBindings 增加元数据与声明精确匹配的区域源，源同步填充宿主提供的区域
缓冲区。Run 保留不可变源快照到全部回调退出。先校验所有名称、元数据和 scalar
区间，像素在读取需求区域时校验，未读像素不扫描。完整 dense Value 输入继续可用。
源和 sink 不得保留借用的可写或只读指针。

## 算子与版本

软件包升至 0.4.0，OperationTraits 与 operation C ABI 升至 4。维护中的 C 类型、
常量和入口统一使用 `_v4` / `_V4`；在查询 API table 前拒绝 ABI 3，不保留适配器。
C++ 消费者重新构建。C++17、WorkflowDocument schema 2、provider ABI 1 保持不变；
provider 仍定义语义 schema，不承担运行期存储或文件编解码。

两种算子 API 都提供有序区域输入、明确输出 Region、宿主输出分配与 scratch。
宿主管理校验、分配失败和只读发布；仅成功完成才发布。保留 C 指针/count/size 校验
与异常边界。workspace 必须有可检查计算的上限，供 tile 准入预留；算子像素缓冲区
使用宿主分配器。可信回调违反分配契约不构成宿主提供沙箱保证。可选 GPU 回调仍与
原生设备区分。

静态 halo specialization 属于复制的 trait，在 analyze 中从已校验参数解析，不依赖
每次执行的像素/标量。新增 Float32 蒙版输入端口：二维 {H,W}、无 facet、有限 [0,1]，
与图像空间尺寸一致。scalar 保持完整 {1}；RGBA 保持全部四通道和 ADR 0016 profile。

## 需求、调度与预算

PlanningOptions 增加正整数 tile 高/宽，默认 128/128，并保留命名输出 Region。
ROI/tile 变化重新规划 optimized IR。物理 identity 包含规范化需求及 tile 几何；
trait、静态 halo 和 mask 语义进入 semantic identity。semantic、optimizer、physical
plan、plan cache 使用独立 v4 domain；源 schema 仍为 2。运行期数据、地址、计时和
取消状态不进入这些 identity。

按输出名称字典序、空间行列序惰性生成 tile。逐 tile 反推需求并合并本 tile 的 fan-out；
相邻 tile 的 halo 可以重算。halo 相对于完整逻辑图像扩展/裁剪，不能在 tile 边缘做
图像边界扩展。retile 只拼接所需覆盖，缓冲区计账。部分通道、空或越界图像请求在
规划时失败。

只有确定、无副作用、区域规则合法的路径可逐 tile 重算。Whole 算子建立明确完整
物化边界，完整工作集与保留结果都计账；放不下则返回 ResourceExhausted。

ExecutionContext 的 maximum_live_bytes 约束实际受控分配容量，包含源读取、输出、
scratch、中间结果、传输和 sink 暂存。共享存储只计一次；调用方已有输入及自行复制
的数据单列，元数据、线程栈、RSS 不计入。工作集预留和实际分配峰值分开报告；空闲
复用容量在实际释放前仍计账。

tile 准入前预留完整保守工作集。临时竞争降低并发，或等待能在已有租约内完成的任务。
最小工作集放不下则失败，禁止持有部分资源等待剩余资源。外部保留结果不能导致
无限等待调用方释放。worker、最大并行度、共享等待队列均有界，诊断按图大小聚合，
不能随 tile 总数无界增加。

## 收集、流式输出与失败

execute 收集各命名输出精确请求的 Region，未指定则为完整输出，分配计入预算。
流式入口复用相同 plan/bindings，按顺序同步调用 sink，借用视图在返回时失效。
背压约束已完成而未消费的 tile；调用方可复制到自己的独立存储。

准入前、回调完成后、每次 sink 交付前、最后一次 sink 返回后和最终组装后返回前检查取消及 currentness。入口先
拒绝 foreign/default/stale plan，再读取绑定或取消。有效入口之后 Cancelled 优先于
Stale，再优先于普通失败。sink 失败停止后续交付，所有准入工作退出后返回。已经
消费的 tile 无法回滚；仅最终成功表示完整 stream 有效。流式执行不提供持久提交协议。

错误名称/参数/源描述在回调前失败；读取到的绑定像素域错误为 InvalidArgument，
合法对象类型/覆盖不匹配为 TypeMismatch，错误计算像素为 OperationFailed，字节溢出
或分配耗尽为 ResourceExhausted。源/sink 异常转换为执行失败。失败不发布部分收集结果。

## 图像场景与 oracle

S2Image.RegionAndTiles：前景 → Gaussian → 曝光 → 蒙版 → source-over(背景)。

- image.gaussian_blur 要求静态 radius:Int64 [1,64] 和有限 sigma:Float64 [0.1,64]，无默认值。
  radius 决定空间 halo；归一化采样 Gaussian 核先横向后纵向，图像边缘 clamp，固定遍历顺序，
  每遍输出 Float32。
- image.exposure_gain 沿用动态 Float32 gain [0,16]。
- image.mask 用独立 Float32 {H,W} [0,1] 缩放预乘 RGBA。
- image.source_over 按 F + B × (1 − F.alpha) 计算全部 RGBA，依据
  [source-over 公式](https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators_srcover)。

保留有限/HDR/预乘规则、nearest ties-to-even 与 gradual underflow，禁用重排与 FMA
contraction。核系数使用归一化 binary64 运算，tap 按 -radius 到 +radius 遍历，每项乘法与累加使用无 contraction 的 binary64 运算，每遍写 binary32。同一实现的整图与
分块结果逐位相等；独立整图 oracle 使用 abs(error) <= 1e-6 + 1e-5*abs(reference)。
覆盖手算小场景、边缘、非零 ROI、不可整除 tile、tile 小于 halo、mask 0/1、透明、HDR、
非法参数。

程序化大图区域源与同步校验 sink 在不保存整图的条件下证明内存边界。覆盖 fan-out、
重复输入、多输出、并发 Run、慢/失败 sink、恰好足够及少一字节预算、非法视图、取消/
stale、清理及 context 销毁后的结果所有权。内置与安装 C 插件路径运行同一示例和独立 oracle。

## 交付

顺序：#263 契约、#264 存储/ABI、#210 CPU liveness、#211 tile/halo、#265 区域执行、
#266 图像场景、daemon 安装消费迁移。#152 保留父索引；#209 标定及关联 #153/#154 的
原生存储/liveness/tile 工作继续开放。S2 不关闭整个 HEX/MED 父任务。Issue 记录原生
依赖、实际测试与合并 commit，Project 同步。

执行独立局部代码/契约审核、受影响 static/shared 安装消费验证和既有保护 CI。
daemon 只消费安装后的公开 0.4 包，适配调用/codec，不添加协议功能；协调两仓 PR。
不包含 OpenSpec、feedback、C++20、发布归档或无关优化。
