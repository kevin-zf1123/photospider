# Region 语义

Value 的 Region 是完整 descriptor 内经过边界检查的逻辑覆盖，与 origin-relative
存储视图分离。S2 提供 CPU 区域执行；跨 Run 脏区传播与结果缓存尚未实现。

## 规划

Whole 要求完整输入；Elementwise 映射对应坐标；Halo 对称扩展并以受检运算裁剪到
完整图像边界。RGBA 必须包含全部四通道，scalar 始终要求 whole {1}，Float32Mask
{H,W} 映射图像空间轴。空、越界、未知名称、图像部分通道需求在规划时失败，也包括
通过通用下游传播的部分通道请求。

PlanningOptions 保留各名称的精确 Region 和正整数 tile_height/tile_width，默认
128x128。改变选项重新规划 optimized IR；命名 Region 和几何进入 physical identity，
即使多个名称指向同一节点。运行期字节不进入 plan identity。

ExecutionPlan::tile_plan(name, region) 派生去掉无关依赖的单个 tile 计划，不重新分析
源文档或构造完整网格。同一 tile 内合并 fan-out，邻接 tile 的 halo 可重算。Whole、
非确定、有副作用节点标为 whole_boundary，须完整物化并满足预算。

数值 schema 可声明有限闭区间。Int64 端点为 +/- (2^53-1) 内精确整数；Float64 端点
有限。halo_radius_parameter 指向 required bounded Int64，min>=1、max<=UINT32_MAX，
算子为 Halo 且固定 radius 为零。analyze 校验并解析到 node traits。无边界 Float64
继续保留原有精确 bit 语义。

## 执行与存储

图像路径、显式区域需求和 RegionalSource 使用区域执行器；完整通用 scalar/broadcast
仍使用原有的有界 worker 执行器。execute 返回每个名称请求的 coverage；图像收集将
该 Region 紧密存储，同时保留完整逻辑 descriptor。未指定请求表示完整输出。

Run 先按拓扑顺序将 Whole/effect 边界执行一次，再按名称、行、列顺序惰性处理 tile。
Whole 结果为 Run 内不可变 Value，不是跨 Run 缓存；没有后续输出需要时释放引用。
外层 tile 顺序执行，依赖已满足的独立分支可使用固定 worker pool 并行。回调收到
精确输入/输出需求，源读取和计算缓冲区共享 context 预算与 worker 队列。

普通 Value 绑定仍为完整 dense 快照。RegionalSource 每 Run 复制元数据/callable，
填充宿主提供的紧密区域缓冲区，并声明独立 scratch 上限；必须返回精确写入的请求
Region。bounded scalar 参数要求普通 Value，其他输入可用区域源。在源或算子回调
之前校验所有名称、descriptor/facet 和 scalar 区间，像素仅在受约束端口消费的区域校验。

源须支持并发不可变读取和协作取消。源/算子回调不得在同一 context 的 worker 上
同步重新进入 execute。全部准入回调完成后，才能复用借用存储。不增加源 codec 或
provider ABI。

## 流式输出与资源观察

execute_stream 通过同步 ExecutionSink 交付借用 ValueView，视图和指针在 sink 返回
时失效。sink 阻塞时不读取下一个 tile，因而暂存有界。sink 在 execute 调用线程运行；
调用方可复制到自己独立拥有的内存。

准入前、回调入口/完成、每次 sink 前后、最终组装后检查取消/currentness。sink 失败
停止后续交付并等待全部准入工作退出。已消费 tile 无法撤回，最终成功才表示完整
stream 有效；收集失败不返回部分 ExecutionResult。入口 Stale 优先于 token/binding
校验，入口后 Cancelled 优先于 Stale 和普通错误。

预算按所有者单次计入实际受控的源/输出/scratch/中间结果/传输/collector 容量。
开始前预留保守完整工作集；分配租约保留到最后所有者释放。源缓冲区和中间槽位在
最后 reader 后清空。调用方已有 Value、源自有外部状态、元数据、线程栈和 RSS 不计入。
retained_input_bytes 报告不同的已有 Value 存储；不测量源的不透明状态。不同计账域
的分配不会被误认为属于当前 context。

诊断按 node/backend 聚合次数、元素数、耗时，并报告成功源读取/字节、交付 tile、
活动回调、实际分配峰值和预留峰值。峰值包含同一 Run 的 collector、Whole 和 tile。
streaming 不提供 result_digest，由 sink 检查实际像素；收集 digest 包含 coverage 和 origin。

test_tile_plan 验证需求合法性和 identity。test_regional_execution 对逻辑 64 GiB 图像
仅执行 5x7 ROI，验证 9 个顺序 tile、精确资源边界、并发快照、背压、源/sink 失败、
取消/stale、Whole/effect 单次执行和多输出。Gaussian/合成场景由 #266 验收。

## S3 修订

ADR 0018 已实现显式正向脏区映射、不可变快照和可选跨 Run 结果缓存。Shrink 规则
使用向上取整空间输出和裁剪整数输入 box；operation_dirty_region 包括 Halo 扩张
及 Whole/标量整图回退。快照保留旧版本；FrozenExecution 使用冻结有效性，普通
execute 仍检查 stale。此前未实现缓存/脏区传播的描述由本节替换。参见缓存模型和
S3 workflow 指南。

共享生产者分配峰值单独报告在 shared_peak_live_bytes；peak_live_bytes 保留调用方局部分配含义。context 预算对共享所有者只计费一次。
