# 稀疏 fragment atlas 传输

[英文实现说明](../Fragment-Atlas.md) 为权威来源。

## 范围与所有权

Plan 只保留 descriptor domain、精确 sample coverage 和占用 tile 坐标。物化得到的 payload 与目录 Value 通过调用方 allocator 拥有 packed bytes；Result atlas 不保留 source Result 或 read window。Native host 在同步 invocation drain 前保留 device view。Atlas 的 scalar-width transport 不证明相应 GPU 算术已支持。

## 核心数据结构与内存布局

```cpp
class ResultTensorInput;

struct FragmentAtlas final {
  ValueDescriptor descriptor;
  std::vector<std::uint64_t> tile_shape;
  std::uint64_t slot_count = 0, payload_bytes = 0;
  Value payload, directory;
  Result<std::uint64_t> address(
      const std::vector<std::uint64_t>& coordinate) const;
};

class FragmentAtlasPlan final {
 public:
  static Result<FragmentAtlasPlan> prepare(
      const ValueFragments& input, std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  static Result<FragmentAtlasPlan> prepare(
      const ResultTensorInput& input,
      std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  Result<FragmentAtlas> materialize(
      const ValueFragments& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
  Result<FragmentAtlas> materialize(
      const ResultTensorInput& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
};
```

`FragmentAtlasPlan` 从精确 coverage 准备传输元数据，再通过调用方 allocator 物化。支持 rank 1..8 以及所有内建 `ElementType`：UInt8、Int64、Float64、Float32、Int8、UInt16、Int16；原始样本 bits 不转换。Plan 只持有 domain、coverage 和 tile masks，不保留 source data owner。`ResultTensorInput` overload 取得拥有型 windows，仅复制该 Need 授权的 samples；通过 `row_run()` 读取，支持负 stride、broadcast stride、碎片化 windows 和带 batch prefix 的 spatial tensor，不填补空洞或 bounding box。Atlas 物化后可独立于 input Result、poll 和 execution context 存活。

`preparation_work()` 报告通用目录规划工作，`materialization_work()` 报告通用 packing 工作。`ResultTensorInput` overload 还会通过 `FootprintLimits::consume_work` 计入 window acquisition 和 read-bound 工作，因此 `materialization_work()` 不代表其完整计费。旧 `ValueFragments` overload 保留调用方原有的外部计费方式。

`ResultTensorInput` 可以由单个 source Result 支持，也可以包含 structured C2 producer 的 tensor Need 展开为 singleton-atom 请求后形成的私有 pieces。Atlas 的准备和物化只使用该 capability 获准的 sample coverage。复合 capability 没有单一 Result 身份（`object_id() == 0`），但这既不使获准 pieces 无效，也不会增加读取权限。输入 windows 在 atlas 打包请求 samples 期间保留原始 Result owners；物化后的 atlas 独立拥有 packed buffers。此路径处理 tensor Need，不聚合 object 或 field support。Descriptor-only Empty C2 输入不受此路径支持。

目录 slot 数为 2 的幂，至少为 2 且至少为占用 tile 数的两倍，使用开放寻址和线性探测。每个 slot 为 80 字节 little-endian：0..63 是八个 uint64 tile 坐标（未使用轴为零），64..71 为 uint64 有效样本 mask（零表示空 slot），72..79 为 uint64 payload 字节偏移。

Prepare 的 allocation 大小、`maximum_boxes`/`maximum_work` 限额、工作量计费与取消释放规则见[算法与 wire format](#算法与-wire-format)。

公开 `FragmentAtlas::address` 与 C 兼容 SDK 宏 `PS_FRAGMENT_ATLAS_MSL_V11` 的查找一致，后者定义 Metal `ps_atlas_address`。宿主提供真实 binding span、slot 数、shape、 tile extents 和 payload 字节数。任意 fragment 数仅需 payload 与目录两个 binding。 边界映射必须先作用于全局坐标。缺失 slot/mask bit 是明确缺页，不是零值或逐 fragment clamp。

现有 `DependencySession` 使用下述 `ValueFragments` transport。G4 C++ staged 示例 `main.cpp` 与 C Result plugin `c_plugin.c`、`discovery_plugin.c` 使用 Result tensor-Need atlas 接口；后者也使用 Result GPU discovery，见 [GPU Discovery](GPU-Discovery.zh.md)。旧 dependency-service discovery protocol 在该文档另行说明。同步 Result producer 使用其精确矩形或 Need 授权 atlas 路径。Float64/Int64 原始 bits 传输不代表已支持相应 GPU 浮点算术。

## 调度与资源状态机

```text
prepare atlas -> 分别准入取整后的 payload 与 directory
      |                              |
      |                              +-> admission failure -> ResourceExhausted
      v
materialize -> synchronous native callback -> drain views -> retire leases
      |                    |
      +-> cancellation     +-> service/protocol failure -> 不发布成功结果
```

### 分阶段 GPU 执行

GPU `DependencySession::poll` 要求宿主提供完整 `DependencyGpuServices`。Run 复用现有 GPU worker、waiting admission、native device 和同步 `Invocation`；start/source 仍使用 CPU worker。状态缓存命中及 control/constant-only 路径可以没有新 dispatch。 backend 表示实现及数值契约；dispatch/submission/device-time 和 poll timing 记录实际工作。

`phase.atlas(port)` 按需准备当前 stage 已验证的精确端口，准备与打包工作在分配前扣除， 同一 poll 重复调用复用相同 atlas。未供给端口及 CPU 调用失败。native buffer/execute 服务错误和异常 sticky，忽略错误也不能发布成功。宿主持有 native view 直到同步 callback 完成 drain。普通、stream 和 frozen Run 合并 caller 与 sets cancellation；取消优先于普通 callback/backend 错误。此前已检测到的 Protocol 违规（包括未授权 native buffer 请求）保持 Protocol 状态，不被随后取消覆盖。

每个 atlas 的 payload/目录按分别取整后的真实 native capacity 执行独立非阻塞 reservation。 stage 输出按各矩形分别取整，workspace 声明包含每次 native 分配的真实 capacity。 seal 只释放未用 reservation；atlas、state、scratch/output 活跃 owner 仍计费。无法与已持有 owner 一同放入预算的 stage 有限失败，atlas 不扩大读权限、不授权合批。

公开 [GPU workflow README](../../../examples/g4_gpu_workflow/README.md) 对应的 `examples/g4_gpu_workflow/main.cpp` 用 Result Need 先获取每个 output 的 Int64 control，再请求 65 个离散 Float32 samples。Native callback 用 `acquire_native_atlas` 打包 Need coverage，并通过三个 binding 执行 Metal 求和。独立预期为 2145、4290、2145；两次 native dispatch 后，第三个观察复用相同 state/data 的 pure block，同时保留自身 Control `{2}` evidence。Native 输出与 state cache 共计 36 bytes。构建与运行命令见 [GPU workflow README](../../../examples/g4_gpu_workflow/README.md)。

`test_dependency_gpu` 使用明确非 native 的 mock，验证服务缺失、CPU 误用、异常、忽略错误、先计工作后分配和 atlas 复用/退休；不构成 native 证据。static/shared 安装 consumer 运行此测试并编译相同公开 workflow。没有 native Metal 时 workflow 验证 CPU 后返回 77。

### Result callback 的 atlas 访问与 Root 所有权

`ResultProgramPhase::acquire_native_atlas(input, slot)` 只打包 callback tensor Need 授权的精确 coverage。缺少 Need、Descriptor-only Need 或 CPU phase 调用会产生 sticky `InvalidArgument`，不创建 atlas，也不扩大 sample 授权。同一 poll 内对相同 port 和 slot 重复调用会复用同一个 `shared_ptr<const FragmentAtlas>`。空的 payload Need 会返回有效 atlas：一个惰性 payload 字节和两个空目录 slot；不读取 source samples，也不执行 native dispatch。


Atlas payload 与目录使用独立于 operation workspace 的 Root-accounted storage。Atlas 拥有这些 buffers，但不保留 source Result 或 read window，所以可在 source 和 execution context 退休后继续读取。Native operation 仍需通过当前 GPU service 提交 dispatch，并负责发布输出。

`main.cpp` 还覆盖反向、零 stride broadcast 和碎片化输入。独立的 spatial case 使用 batch axes `{2,2}` 和 cell shape `{2,4}`；mask Need 在真实 Metal 上返回 30，未请求的 sample 保持缺失。Test 在 source/context 退休后继续读取 atlas，并检查最后一个 atlas owner 释放时 Root Payload 归零。

失败用例覆盖缺少第 65 个 sample（`OperationFailed`）、CPU atlas 访问、Descriptor-only 或缺少 port、不可变输入升级为可写，以及无 dispatch 的 block callback。越权访问返回 sticky `InvalidArgument`；其他失败计算返回 `OperationFailed`。失败 owner 释放后会进行一次新的 native retry。另有用例以两种顺序注入 native service failure 和 work exhaustion：第一次观察到的错误保持为最终 status，因此后续 work exhaustion 不会覆盖较早的 service error，较后的 service error 也不会替换较早的 work exhaustion。普通取消返回 `Cancelled`；较早的未授权 native-buffer 请求即使随后发生取消，也保持 `InvalidArgument`、`UnauthorizedRead` 和 Protocol origin；两者都不发布输出。

Result Root Payload 准入临界值为 131520 字节：两个 8192 样本输入 tensor 共 98304 字节，`SparseState` 为 152 字节，输出 4 字节，incoming/outgoing native state 各 12 字节，scratch 8 字节，稀疏 atlas payload 260 字节，目录 32768 字节。131519 字节被拒绝，131520 字节准入。该示例会释放前次 output owner 后再检查同一临界值。

### C Result staged GPU 桥接与 handle 生命周期

C plugin 的每次 Result callback 收到 `ps_result_services_v2`。其中的 native GPU service 是独立版本化的 `ps_gpu_service_v1` 表，定义在独立 header `photospider/plugin/native_gpu_api.h`；外围 Result operation 接口仍为 ABI 2。`acquire_native_atlas` 只打包当前 tensor Need 授权的 coverage。返回的 `ps_result_native_atlas_v2` 描述完整 sample shape、tile shape、slot 数、字节数及 payload/目录 GPU token。同一 poll 对同一 input 和 slot 的重复获取会复用 atlas，前提是两个 token 都未释放。释放任一 token 后，下次获取会使原 token pair 一并失效并返回新 pair；旧 token 不会重新有效。C adapter 将这些 token 映射为单调递增 handle，只接受创建它们的 poll 中的 token；过期、伪造或错误 kind 的 token 会在 GPU dispatch 前被拒绝。Need coverage 上限为 65,536 boxes，复制区域列表前会预扣 `count * (1 + 2 * rank)` Root work；Footprint 和剩余 work 限额继续约束 atlas 规划与物化。

`ps_result_services_v2::block` 为 block callback 提供受限的 `ps_result_block_services_v2`。Incoming state 是显式的 generic 单 tensor `CompleteBundle` Result handle。`create_state` 将 typed sample bytes 复制到当前 Root allocator 并 seal 为 Result；`read_state` 复制该状态的全部 tensor samples；`release_state` 释放 handle。Callback 可返回借用的 incoming handle，也可返回新建的 state handle。Host 消费新 handle，并为调用方创建不同的 outgoing handle。Block callback 可读取当前 Need 授权的输入 samples，也可使用 atlas 与 GPU services；不能发起新 Need、嵌套 block 或发布 operation output。

`publish_tensor_buffer` 接收精确的 scratch allocation、row-major output region 与 relation rows。Envelope、region 和 relation 验证后，service 会把 allocation 从 scratch 中移出并冻结以供发布。即使之后的 publication 步骤失败，该 allocation 也已被消费；原 writable scratch token 不能修改转移后的 bytes。C GPU command 每次最多 32 个 dispatch，每个 dispatch 最多 31 个 binding；token map 与 atlas 限于当前 poll，operation state handle 则保留至显式释放或被 host 消费。Service errors 均为 sticky。

公开 C11 plugin `examples/g4_gpu_workflow/c_plugin.c` 与 loader `c_main.cpp` 注册 Result ABI 2 operation，输入是至少 4,225 个 sample 的 rank-one Float32 tensor。Workflow 的 CPU 路径和 Metal 路径均返回两次观察值 `2145,2145`；Metal 路径执行一次 native dispatch、命中一次 block cache，并保留 16 bytes native cache storage。第二次观察保留当前 Control evidence `{1}` 并排除旧 `{0}`。Fixture 还覆盖 Empty output、动态 input extent、state handle 错误、超过 Need 上限、错误 atlas record、未授权提权、发布后写入、shader 缺样本和 poll token 生命周期。这些是 C staged Result bridge 行为；C discovery Result workflow 见 [GPU Discovery](GPU-Discovery.zh.md)，与旧 dependency-service discovery protocol 分开说明。

Standalone example CMake project 会构建这个 C11 module 与 loader。可选 loader 参数用于指定 module 路径。返回 77 表示 CPU 检查后发现当前没有 native Metal，不代表原生成功。Installed consumer 测试 `installed_gpu_c_abi_execution` 使用安装后的 kernel package 验证 C Result loader，且在原生设备路径上通过。

### 同步 producer 执行与 CPU 回退

依赖 Run 通过 context 原有 native worker 调用同步 GPU producer。host 按声明的矩形输入需求收集到紧密 native storage，分别计入实际取整容量，并在 callback 生命周期内提供独立 `native_gpu_api.h` 中的 `ps_gpu_service_v1`。GPU callback 必须提交真实 native work。Invocation view 在 owner 退休或 CPU 重试前完成 drain；Whole 保持全域验证与证据。

仅 `BackendUnavailable`、支持 CPU 且 `allows_cpu_fallback` 时重试。staged 失败后退休 continuation，在 CPU worker 从原始 Q 重新 start。对发起请求的 caller，取消优先。由 frozen plan 启动的 producer 在原 caller 被取消或 Demand binding generation 变为 stale 后，仍可为健康 peer 继续工作；原 caller 在最终检查时仍返回 `Cancelled` 或 `Stale`。Borrowed-graph execution 继续严格检查 graph currentness。Frozen peer 保留捕获的 input bundle；最新 Demand 上的新请求使用替换后的 binding。每个 attempt 使用 per-session 限额，所有 attempt 计入同一 Run 工作上限。诊断记录实际 backend 和拒绝 attempt。回退来源随子结果、shared Flight、Whole record 传播；禁止保留 dependency 结果缓存，后续阶段也不使用或保留 checkpoint/pure block。waiter 取消继续独立。

对于基于 atlas 的 staged GPU attempt，`DependencySession` Run 会在 dispatch 前保存有界记录覆盖域。CPU 重启恢复该覆盖域（包含此前合并的 rows），重建本地索引，弃用的祖先不再占用重试的记录额度。保存和恢复计入工作量，GPU discovery 已消耗的工作不退还。共享不可变记录、Flight 与 cache epoch 保持不变。Whole 像素及完整结构 DAG 保持每 Run 至多一次的生命周期，仅在重启路径实际请求这些像素时重新导入证据。

Result 同步 GPU 示例见 [sync_main.cpp](../../../examples/g4_gpu_workflow/sync_main.cpp)。它将 5000 样本的 Float32 输入 Result 绑定到 workflow，并请求 `{0,2}`，用独立 `x+1` oracle 检查输出 1、3。真实 Metal 路径执行两次 dispatch，并记录实际 backend 和各次 attempt；缺少设备时则验证 CPU fallback。新的 frozen execution 对已完成的纯 Result 命中一次缓存且不 dispatch；fallback-tainted Result 没有缓存命中。

该 Result workflow 分别验证 Whole、staged poll 和 start-rejected GPU attempt 返回 `BackendUnavailable` 后的 CPU 重试。staged 与 start-rejected continuation 在有 Metal 与缺少设备两条路径中都各启动两次。四种记录回滚案例在 regional 祖先后使用 16-entry 上限，在 Whole 祖先后使用 12-entry 上限；它们检查常量重试保留空输入 support 且不保留被放弃的 Tensor/Descriptor records、此前 `{0,2}` 的 support 保留，以及 Whole 祖先的完整 5000 样本 support 保留，并在 CPU 重试需要时复用一次。该路径保存和恢复精确的类型化依赖状态，而非仅恢复 coverage checkpoint。

Whole 资源案例让 20000 字节输入 Result 与两个 native buffer 同时计入同一 Root；两个 buffer 各取整为 32768 字节，因此总 live 容量为 85536 字节。85535 字节时准入失败，85536 字节时成功。释放首个输出后，同一上限也容纳 CPU fallback 和新一轮 native retry。没有 Metal 时先执行缺少设备的 fallback 检查，再返回 77；这表示 skip，不是原生成功。`test_execution_demand` 还检查共享 fallback 中取消一个 producer waiter 时另一个 waiter 仍可继续，以及零缓存保留。C++ staged Result 路径见上文；当前 C discovery workflow 也基于 Result，见 [GPU Discovery](GPU-Discovery.zh.md)。`ValueFragments` 保留为 atlas preparation/materialization 使用的 typed-backing helper，不是独立的 workflow 或 dependency-execution 入口。

## 算法与 wire format

`prepare` 在物化前报告 payload 与目录的逻辑 allocation 大小。GPU 宿主应分别按设备实际 capacity 取整再 admission。`maximum_boxes` 限制占用 tile，`maximum_work` 限制元数据扫描、样本发现、hash/probe 和 packing；cardinality/equality 扫描前检查取消并预扣 metadata。`preparation_work()` 报告通用几何规划工作量，`materialization_work()` 报告通用 packing 工作量。`ResultTensorInput` overload 通过 `FootprintLimits::consume_work` 直接计入其工作；Result 物化还会额外计入 window acquisition 和 read-bound 工作，这些不包含在 `materialization_work()` 中。`ValueFragments` overload 保留调用方原有的外部计费方式。任一 allocation 后取消（包括 Empty）都会释放新 owner，不发布 atlas。

每个 tile 至多 64 个逻辑样本。可指定正值、rank 对齐且乘积不超过 64 的 tile extents； 默认从最右轴开始确定性填充。仅占用 tile 进入目录。Lookup 使用逐轴全局 tile 坐标， 不要求全局 shape 乘积可表示为 uint64。`ValueFragments` 拒绝需要 `PlanarImage` 的结构化 image facets；atlas 只传输 generic Value 与可接受的完整 ColorArray tuple，不提供 image-v2 dependency execution。

Hash 对 rank 个坐标逐字节（低位先）执行 FNV-1a，offset basis 为 14695981039346656037，prime 为 1099511628211。Tile 内 row-major 坐标选择 mask bit；地址为 tile 偏移加此前有效 bit 数乘 dtype width。Payload 按 tile key 和有效 bit 顺序紧凑存放，孔洞不占样本。空端口使用一个惰性 payload 字节和两个空 slot，均不授权样本。

## 限制与错误处理

### 验证证据

公开安装 consumer 对应 `tests/unit/test_fragment_atlas.cpp`，验证四种 dtype（UInt8、Int64、Float32、Float64）、wire words、bit63 及孔洞、输入 bits 变化、负 stride、全域超过 uint64 的 rank8、allocation/work 精确边界、源 owner 退休和分配时取消。Native 测试 `tests/unit/test_fragment_atlas_gpu.cpp` 在真实 Metal 上执行同一 SDK helper，并用独立坐标字典检查结果。可在仓库根目录构建并运行：

```sh
cmake --build build --target test_fragment_atlas test_fragment_atlas_gpu -j 8
ctest --test-dir build -R '^test_fragment_atlas(_gpu)?$' --output-on-failure
```

Native 测试有七次 dispatch，覆盖 rank1/3/8、width1/4/8、65 个离散 fragment 使用四个 binding、超过 16 KiB 的目录实际 capacity、少一字节分配失败、全局越界与内部孔洞。 返回 77 为无原生 Metal 的 skip，不是原生成功；这些 atlas transport 检查不覆盖有界 GPU discovery；当前 Result discovery 行为见 [GPU Discovery](GPU-Discovery.zh.md)。

`ValueFragments` overload 只保留其接受范围内的 Value 样本；需要 `PlanarImage` 的结构化 image facets 会被拒绝。`ResultTensorInput` overload 只保留 tensor Need 授权的样本。两种路径都会保留孔洞，查找缺失目录样本返回 `NotFound`，超出 tile 或 work 限制时拒绝计划。取消会释放本次新分配的 owners。设备端需分别按实际容量准入 payload 与目录；atlas lookup 不会额外读取输入，也不提供 CPU 回退。
