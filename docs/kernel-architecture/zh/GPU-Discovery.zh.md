# 有界 GPU 请求发现

### 范围与所有权

Executor 拥有 table 分配、native work 排空、记录校验和 typed Need 附加。Discovery 不会追踪任意 shader 读取。C Result ABI 3 service 和 C++ `ResultProgramPhase::discover` 入口都使用这条 Result 路径。`Value` 可以提供私有 backing storage，但不再公开独立的 dependency discovery phase。

C Result operation 在普通 `poll` callback 中调用 `ps_result_services_v3::discover`。Discovery callback 只能使用当前 Need 的 read、native atlas、scratch、work、cancellation 和 GPU dispatch services；它返回普通 status，并且必须提交实际 native work。若 receipt 非空，外层 operation callback 必须返回 `PS_RESULT_NEED_V3`。Executor 会在这些 Need 尚未供给时拒绝 publication。空 receipt 不增加 Need，callback 可以继续执行。

```c
uint64_t receipt = 0;
if (services->discover(services->context, capacity, candidates,
                       compute_requests, user, &receipt))
  return 1;
return PS_RESULT_NEED_V3;
```

若之后的 poll 需要读取规范化记录，callback 应将 receipt handle 保存在 operation state 中。读取完 records 后释放 handle。`discovery_requests` 支持 count-only 查询：`output == NULL` 且 `capacity == 0`；随后可将 records 复制到已初始化的目标结构中。C handle 在跨 poll 期间归该 plugin instance 所有，直到显式释放或 operation 销毁。

### Wire 格式与内存

容量为 `K` 时，逻辑 table 大小为：

$$
S = 16 + 16K.
$$

Executor 在分配零初始化 Root table 前预扣 `candidates + 2S` work。Table 使用独立的 Root native storage，不占 operation workspace。设备取整后的 table capacity 另行准入。同步 callback 及其已提交 GPU work 排空后，host 才冻结并解码 table。Table 指针和 service pointer 不会越过 discovery callback 生命周期。

### 执行与 Need 状态

Discovery callback 是同步 GPU 路径。它可以读取当前 Need 覆盖的 samples、为该 Need 获取 atlas、分配或释放 scratch、计入 work、检查取消并提交 GPU commands。成功 discovery 必须至少执行一次真实 native dispatch。

Executor 将发现的 Needs 附加到 operation poll callback 返回的 `ResultProgramNeed`。Operation 也可以在同一 Need 中额外请求输入。Stage 的总条目数不得超过 65,536 与 dependency set `maximum_boxes` 中较小者。

未知数量时先执行 count-only 查询，再设置每个目标 record 的 `struct_size` 并提供足够容量。Receipt metadata 不可变，也不含 tensor bytes。每个 `candidates` 值只限制对应的一张 table；一个 active poll 内多次 discovery 共享 work 与 metadata 计量。Actor 的 discovery-work 额度跨 poll 保留；每次调用也会消耗 Run 与 Root work。重复 raw records 仍然消耗 emit 与 normalization work。

### 限额与失败处理

`capacity` 必须为正，且不得超过 65,536、`DependencyLimits::maximum_gpu_requests` 或适用的 `maximum_boxes`。`maximum_gpu_requests=0` 会禁用 discovery。`candidates` 必须为正，它限定并计费全部 emit attempts，包括重复项及超过 table 容量的尝试。Shader dispatch 的 emitter 数量不得超过此上限。仅当 emit 时没有可用 slot 才设置 overflow；正好写满最后一个 slot 合法。Host 拒绝 overflow，也会检查尝试数是否超过 `candidates`。

Table bytes、取整后的 native capacity、candidate work、decode work 与 request metadata 分别计入相应 Root/dependency 限额。Read 和 atlas 仍受当前 Need 限制。Discovery callback 不能发起新 Need、发布 output、进入 block callback 或递归调用 discovery。未执行 native work 的 callback 会以 `OperationFailed` 失败。已提交 dispatch 排空后才释放 table owners。

## 已注册 workflow 与 decoder 覆盖

当前 C discovery fixture 是 [`tests/fixtures/gpu_result_discovery_plugin.c`](../../../tests/fixtures/gpu_result_discovery_plugin.c)，由 [`tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp`](../../../tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp) 加载。独立期望输出为 `8` 和 `24`；Metal 路径断言四次 native dispatch。Control 编辑会改变选中的 Data support，frozen execution 则保留捕获时的 Result。同一 integration 源码还覆盖 malformed records、overflow、device 缺失、publication failure、取消、Root 清理和 retry。

Decoder 与 normalization 源测试为 [`tests/unit/kernel/test_result_discovery_services.cpp`](../../../tests/unit/kernel/test_result_discovery_services.cpp) 和 [`tests/unit/kernel/test_result_discovery_decoder.cpp`](../../../tests/unit/kernel/test_result_discovery_decoder.cpp)。这些注册项指出可执行覆盖，不表示以前某次运行已通过。

Discovery 必须在当前 callback entry thread 执行。C++ `ResultProgramPhase::consume_work` 可从 worker thread 计费；C discovery service 仍限于 entry thread。Work 与 metadata 限额跨 active poll 中的调用累计，每次调用也消耗 Run 和 Root work。重复记录仍消耗 emission 与 normalization work。

Capacity、candidate、Root 和 dependency 限额相互独立。读取与 atlas access 仍受当前 Needs 限制。Discovery 不能再发 Need、发布 output、进入 block callback 或递归调用 discovery。未提交 GPU work 的 native callback 返回 `OperationFailed`。表 owner 退休前会排空已提交的 dispatch。
Native GPU service 独立使用 `PS_GPU_ABI_VERSION_1` 版本，提供 discovery 使用的同步 buffer 与 dispatch 操作。Result discovery 本身属于 Operation Plugin ABI 3。Vulkan 构建支持为可选项。Native GPU 行为要求构建启用了对应 backend 且设备可用；协议 mock 只能覆盖校验和错误路径，不能证明 native 执行。
