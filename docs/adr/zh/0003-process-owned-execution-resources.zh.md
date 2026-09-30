# ADR 0003：ExecutionContext 拥有本地执行资源

- 状态：已接受

## 1. 核心摘要 (TL;DR)
`ExecutionContext` 拥有有界的本地 worker、callback 准入、缓存和资源记账。每次调用创建私有 Run 状态，并将绑定与结果和其他调用隔开。资源共享因此显式可见，worker 数量也不会随 graph 数量增加。

## 2. 架构心智模型

```text
                  +--> CPU workers / FIFO --+
ExecutionContext |                           |--> Run callback --> Values/results
                  +--> optional GPU lane ----+
                  shared admission, allocator, caches

Run A: dependencies, cancellation, staging, diagnostics
Run B: dependencies, cancellation, staging, diagnostics
```

多个 Run 会竞争 context 配置的资源，但各自保有依赖、取消状态、绑定、中间 Value 和诊断。CPU 执行始终可用；native GPU 执行取决于主机能力和配置。

## 3. 契约规约与接口

```cpp
struct ExecutionContextConfig {
  std::uint32_t cpu_workers = 0;
  bool gpu_enabled = false;
  std::uint32_t maximum_queued_tasks = 1024;
  std::uint64_t maximum_live_bytes = 256U * 1024U * 1024U;
};
struct ExecutionOptions { std::uint32_t maximum_parallelism = 0; };
class ExecutionContext {
 public:
  Result<ExecutionResult> execute(const ExecutionPlan&, ExecutionBindings,
      const CancellationToken&, const ExecutionOptions&);
};
```

完整配置还控制 demand 工作量、结果保留、受管容量、可选磁盘缓存和 scheduler 观测。构造时拒绝为零的队列或 live-byte 上限，并校验其他配置边界。CPU worker 数为零时使用受限的硬件推导值；`maximum_parallelism` 为零时使用 context 的 CPU worker 数。调用方须在创建 context 前冻结 operation registry，context 会保留该 registry。

CPU 与 GPU callback 共用一个等待上限。Worker 开始普通 callback 时会释放其队列名额。Staged CPU job 会一直保留名额，直到其 tile callback 全部结束并离开队列。

受控缓冲区在最后一个 owner 释放前计入 context。调用方原先持有的不可变输入字节不计入 live-byte payload 上限。

## 4. 非目标与明确边界
- 资源属于进程本地。Context 不创建 daemon job、远程 worker 或进程级单例。
- 配置的字节模型只涵盖有埋点的受控资源，不代表 RSS、OS 开销或任意 plugin 分配。
- 调用方必须避免直接调用与 context 析构并发。取消采用协作方式，不能抢占 callback。

## 5. 后果与代价
等待队列已满或受管容量不足时，系统可能以类型化错误拒绝工作；调用方应限制并发 Run，并按工作量设置上限。慢 callback 会持续占用 worker。析构会关闭准入并 join 所有 worker，因此需要等待 callback 结束。可选缓存会占用配置的内存，可以主动清除。
