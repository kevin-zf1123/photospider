# ADR 0004：OpenCV CPU Operation 是可重入的 Provider 工作

## 状态

已接受，并已在仓库自有 CPU operation provider 中实现。

## 背景

Operation callback 从进程级 registry 中复制出来，并可能被 scheduler worker、reader snapshot
或 legacy HP compatibility bridge 并发调用。Registry lock 只保护 ownership mutation、发布、
snapshot capture 与卸载，不会串行化 callback execution。因此，operation plugin contract 要求
每个 provider 保证 callback 可重入，或自行同步其共享可变状态。

Builtin OpenCV provider 过去在实际实现中违背了这项契约。一个进程范围的 mutex 会跨全部 Graph
和 HP/RT route 串行化 13 个 monolithic 与 tiled callback entrypoint，尽管这些 callback 使用
不可变 input view、callback-local matrix 与独立拥有的 output storage。Scheduler worker grant
在 task dispatch 层可观察，但无法让这一组 operation 真正重叠执行。

残留的 `cv::UMat` 路径，以及只在注册线程调用的
`cv::ocl::setUseOpenCL(false)`，也模糊了 backend 所有权。在 OpenCV 4.12 中，该 OpenCL 开关
存储在线程局部 core state 中，因此在注册线程设置它不能定义 scheduler worker 的行为。相对地，
`cv::setNumThreads` 控制进程级 OpenCV CPU 并行，且不得在 OpenCV 工作活跃时重新配置。

CLI benchmark service 还存在另一个证据缺陷：最初它会报告 `execution.threads`，却不会把它
应用到 execution；随后的一版过渡迁移又改成按每个 session 的值重新配置 Host execution
default。后一种做法与固定的 process worker pool 不兼容：例如先跑单线程、再跑双线程的
`RunAll` sweep 会拒绝并跳过后一个 session。

## 决策

仓库自有 CPU OpenCV operation callback 是可重入的 provider 工作。移除共享外层 operation
mutex。Builtin resize、crop、extract，以及仓库自有标准 CPU operation plugin 全部使用
`cv::Mat`，不使用 `cv::UMat`。输入保持不可变；可变 matrix 与 output region 由 callback
局部拥有或 task 独占。

可选的仓库 OpenCV operation provider 会在发布自身任何 callback 前，恰好一次调用
`cv::setNumThreads(1)`。这会禁用 OpenCV CPU 嵌套并行，让 scheduler 已准入的 worker grant
成为仓库自有的外层并行层。仓库代码不再调用 `cv::ocl::setUseOpenCL(false)`，也不会在
callback 可能活跃时重新配置 OpenCV threading。

同一 provider 拥有其 OpenCV algorithm 实现与异常 fence。OpenCV 资源耗尽会以新建的
`std::bad_alloc` 重新抛出；其他每个 `cv::Exception` 都会翻译为携带
`GraphErrc::ComputeError` 的 host-owned `GraphError`。只有启用
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` 时才会包含该 provider。禁用时，依赖中立 core
operation 与 v2 registrar 仍然可用；启用时，v2 provider 可以替换相同 registry slot。

当 provider 确实拥有共享可变状态时，同步仍由 provider 局部负责。因此 Metal Perlin provider
会保留其 DSO-private mutex，用于保护共享 Metal device、command queue、pipeline 与 buffer
lifecycle。该锁不是 OpenCV operation lock、scheduler exclusivity flag 或跨 provider 契约。

本决策不增加 scheduler `exclusive` metadata，也不修改 public operation/plugin ABI。第三方
provider 仍自行负责可重入性与 backend state。

`BenchmarkService` 会验证每个 enabled session 的零至八
`execution.threads`，并把零恰好一次解析为有界 hardware concurrency。解析后的正值是 benchmark
Run 的 `maximum_parallelism`，不是 process worker request。一个 service 生命周期至多一次以
`worker_count=0` 准备未来 HP/RT CPU route；这会自动启动尚未固定的 pool，或保留已经固定的
pool。每次 repetition 都携带并报告同一个 Run cap。`RunAll` 会预检 enabled session；配置文件
解析完成后，它不会对 disabled session 执行 thread 范围校验或运行该 session；它还会诊断并
跳过非法 enabled session，并在不 resize pool 的情况下执行混合的合法 cap。一个注册到 CTest
的 Host-boundary regression 会验证一次性准备与逐 Run 值，callback regression 则会在一个
固定八 worker pool 上、不使用 elapsed-time 断言地证明 `1/2/4/8` cap 对应精确 callback
overlap。另一个独立手工 benchmark 会经过同一条 Host、execution service、Graph 与 operation
路径并报告原始 timing sample；其性能比率只是观察结果，不是正确性门禁。

## 结果

- 独立的仓库自有 CPU operation callback 可以在 tile、Graph 和 intent route 之间重叠执行，
  上限是固定 process lane 与每个 Run 已准入 parallelism cap 的较小值。
- 进程不再为原先加锁的 13 个 entrypoint 承担隐藏的全局串行化开销。
- OpenCV 内部 threading 在 callback 发布前固定为一，因此嵌套 CPU 工作仍然有界。
- 禁用 OpenCV operation provider 会移除其 operation callback，但不会移除依赖中立
  registry/plugin 契约。该选项尚不会移除彼此独立的 OpenCV codec、normalization、adapter 或
  static product link 依赖。
- 注册 builtin algorithm 抛出的 OpenCV 动态异常类型不再逃出 provider 边界。
- `cv::setNumThreads(1)` 仍会影响同一进程中的其他 OpenCV 用户。若需要 embedding override，
  必须另行决定进程所有权与 ABI。
- 当真实共享 backend state 需要同步时，provider-local lock 仍可能降低并发；provider 必须记录
  这些锁的边界。
- Host execution accounting 不包含第三方内部线程、恶意 DSO、仓库自有 provider 之外的
  OpenCV 使用，也不包含 platform runtime worker。
- 确定性并发与 output-equality regression 是长期产品测试。性能 measurement 继续保持手工运行，
  因为依赖机器的 speedup 不能成为稳定 CTest 或 CI threshold。

## 未采用的方案

### 增加 scheduler exclusivity metadata

不采用，因为它会把一项实现偶然性编码进 planning，串行化无状态 provider，而且仍不能保护
direct 或第三方 callback invocation。

### 保留外层 mutex

不采用，因为它会抵消已准入的 task parallelism，与既有 provider reentrancy contract 冲突，
同时又不能保护全部 OpenCV 使用。

### 保留 `cv::UMat`，并在每个 callback 中设置 OpenCL state

不采用，因为这会在热路径修改 thread-local backend state，模糊 scheduler/backend 所有权，
并在 scheduler admission 之外引入隐式执行层。

### 让 OpenCV 自行选择内部线程数

当前产品不采用，因为 execution worker 会由此产生未计入账本的嵌套 CPU parallelism。以后若要
改变策略，必须先进行显式的进程级所有权设计。

### 按每个 benchmark session 重新配置 process worker

不采用，因为 process pool 是固定的 composition resource，而 benchmark thread sweep 是逐 Run
workload control。重新配置要么在第一次 pool freeze 后跳过混合 session，要么重新制造
per-session worker owner。Run `maximum_parallelism` 可以保留 sweep，又不引入这两种所有权错误。
