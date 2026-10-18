# 有界 GPU 请求发现

### 范围与所有权

GPU discovery 是在指令已编译授权之内的运行期选择。Planner 在执行前固定指令的输入授权；数据相关的算子为其 GPU 代码可能读取的全部内容声明有限的静态超集。在 poll 期间，GPU shader 计算它实际需要哪些已授权 tile，并写入有界请求表。Executor 拥有该表，排空 native work，按已编译授权校验每条记录，并返回不可变 receipt。Discovery 不追踪任意 shader 读取，不启动 producer，也不增加 plan 指令或依赖。C Result ABI 7 service 和 C++ `ResultProgramPhase::discover` 入口都使用这条路径。`Tensor` 可以提供私有 backing storage，但不公开独立的 dependency discovery phase。

C Result operation 在普通 `poll` callback 中调用 `ps_result_services_v7::discover`。Discovery callback 只能使用当前授权内的读取、native atlas、scratch、work、cancellation 和 GPU dispatch services；它返回普通 status，并且必须提交实际 native work。所有被选输入都已就绪，因此算子可以在同一次 poll 中读取被选 tile、计算并发布。带着被选记录返回 `PS_RESULT_NEED_V7` 是可选的，只用于声明这些记录位于授权之内。

```c
uint64_t receipt = 0;
if (services->discover(services->context, capacity, candidates,
                       compute_requests, user, &receipt))
  return PS_RESULT_STATUS_FAILURE_V7;
/* Read records with discovery_requests, then read the selected granted
   tiles and publish in this poll. */
```

若之后的 poll 需要读取规范化记录，callback 应将 receipt handle 保存在 operation state 中。读取完 records 后释放 handle。`discovery_requests` 支持 count-only 查询：`output == NULL` 且 `capacity == 0`；随后可将 records 复制到已初始化的目标结构中。C handle 在跨 poll 期间归该 plugin instance 所有，直到显式释放或 operation 销毁。Receipt 不保留来源 payload，也不授予额外读取。

### Wire 格式与内存

容量为 `K` 时，逻辑 table 大小为：

$$
S = 16 + 32K.
$$

Executor 在分配零初始化 Root table 前预扣 `candidates + 2S` work。Table 使用独立的 Root native storage，不占 operation workspace。设备取整后的 table capacity 另行准入。同步 callback 及其已提交 GPU work 排空后，host 才冻结并解码 table。Table 指针和 service pointer 不会越过 discovery callback 生命周期。

每条 wire record 为 32 字节：四个 uint32 字段 `input, kind, tensor_slot, reserved`，随后是 uint64 `begin, end`。Payload kind 0 指定合法、非空的成员局部半开 tile 区间；descriptor kind 1 要求 slot 和区间端点为零。Reserved 位必须为零。空 receipt 含零条记录。

### 执行与授权检查

Discovery callback 是同步 GPU 路径。它可以读取当前授权覆盖的 samples、为已授权输入获取 atlas、分配或释放 scratch、计入 work、检查取消并提交 GPU commands。成功 discovery 必须至少执行一次真实 native dispatch；callback 没有提交任何 native work 时以 `OperationFailed` 失败。

解码后，executor 按指令的 `PlanTensorRead` 授权检查每条被选记录：记录必须指向已授权的输入，payload 记录必须位于同一成员的 payload 授权之内。选择授权之外 tile 的记录会使该 poll 以 protocol `InvalidArgument`（"GPU selection exceeds compiled input grants"）失败。这包括碎片化授权中空洞处的 tile：当 mapper 授权 tile `[0,2)` 和 `[4,6)` 时，tile 3 的记录即使位于已授权 tile 之间也会失败。对任何有已编译授权的输入，descriptor 记录都被接受。每比较一条记录与一个授权计入一单位 discovery work。

未知数量时先执行 count-only 查询，再设置每个目标 record 的 `struct_size` 并提供足够容量。Receipt metadata 不可变，也不含 tensor bytes。每个 `candidates` 值只限制对应的一张 table。指令的 discovery-work 额度（`DependencyLimits::maximum_work`）跨其 poll 保留，每次调用也会消耗 Run 与 Root work。重复 raw records 仍然消耗 emit 与 normalization work。

### 限额与失败处理

`capacity` 必须为正，且不得超过 65,536、`DependencyLimits::maximum_gpu_requests` 或适用的 `maximum_boxes`。`maximum_gpu_requests=0` 会禁用 discovery。`candidates` 必须为正，它限定并计费全部 emit attempts，包括重复项及超过 table 容量的尝试。Shader dispatch 的 emitter 数量不得超过此上限。仅当 emit 时没有可用 slot 才设置 overflow；正好写满最后一个 slot 合法。Host 拒绝 overflow，也会检查尝试数是否超过 `candidates`。

Table bytes、取整后的 native capacity、candidate work、decode work 与 request metadata 分别计入相应 Root/dependency 限额。对 GPU 放置的指令，discovery 必须在当前 callback entry thread 上运行；它不能嵌套，不能在 block callback 内运行，也不能使用 checkpoint。进入 discovery 会否决该 attempt 的 CPU 回退。已提交 dispatch 排空后才释放 table owners。C++ `ResultProgramPhase::consume_work` 可从 worker thread 计费；C discovery service 仍限于 entry thread。

## 已注册 workflow 与 decoder 覆盖

当前 C discovery fixture 是 [`tests/fixtures/gpu_result_discovery_plugin.c`](../../../tests/fixtures/gpu_result_discovery_plugin.c)，由 [`tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp`](../../../tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp) 加载。其 mapping hook 把 Data 输入的全部 tile 作为静态超集授权，并授权 Control 输入的对应 tile；shader 在该授权内选择 Data tile。独立期望的和为 `8` 和 `24`；Control 编辑会改变选中的 Data support，frozen execution 则保留捕获时的 Result。同一 integration 源码还覆盖 malformed records、overflow、device 缺失、publication failure、取消、Root 清理和 retry。

Decoder 与 normalization 源测试为 [`tests/unit/kernel/test_result_discovery_services.cpp`](../../../tests/unit/kernel/test_result_discovery_services.cpp) 和 [`tests/unit/kernel/test_result_discovery_decoder.cpp`](../../../tests/unit/kernel/test_result_discovery_decoder.cpp)。这些注册项指出可执行覆盖，不表示以前某次运行已通过。

Native GPU service 独立使用 `PS_GPU_ABI_VERSION_1` 版本，提供 discovery 使用的同步 buffer 与 dispatch 操作。Result discovery 本身属于 Result operation ABI 7。Vulkan 构建支持为可选项。Native GPU 行为要求构建启用了对应 backend 且设备可用；协议 mock 只能覆盖校验和错误路径，不能证明 native 执行。普通 GPU step 和 GPU 共享 stage 都初始化 Run-native upload。
