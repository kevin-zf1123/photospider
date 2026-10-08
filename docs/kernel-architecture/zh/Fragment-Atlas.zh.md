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

`preparation_work()` 报告通用目录规划工作，`materialization_work()` 报告通用 packing 工作。`ResultTensorInput` overload 还会通过 `FootprintLimits::consume_work` 计入 window acquisition 和 read-bound 工作，因此 `materialization_work()` 不代表其完整计费。`ValueFragments` overload 保留调用方原有的外部计费方式。

`ResultTensorInput` 可以由单个 source Result 支持，也可以包含 structured C2 producer 的 tensor Need 展开为 singleton-atom 请求后形成的私有 pieces。Atlas 的准备和物化只使用该 capability 获准的 sample coverage。复合 capability 没有单一 Result 身份（`object_id() == 0`），但这既不使获准 pieces 无效，也不会增加读取权限。输入 windows 在 atlas 打包请求 samples 期间保留原始 Result owners；物化后的 atlas 独立拥有 packed buffers。此路径处理 tensor Need，不聚合 object 或 field support。Descriptor-only Empty C2 输入不受此路径支持。

目录 slot 数为 2 的幂，至少为 2 且至少为占用 tile 数的两倍，使用开放寻址和线性探测。每个 slot 为 80 字节 little-endian：0..63 是八个 uint64 tile 坐标（未使用轴为零），64..71 为 uint64 有效样本 mask（零表示空 slot），72..79 为 uint64 payload 字节偏移。

Prepare 的 allocation 大小、`maximum_boxes`/`maximum_work` 限额、工作量计费与取消释放规则见[算法与 wire format](#算法与-wire-format)。

公开 `FragmentAtlas::address` 与 C 兼容 SDK 宏 `PS_FRAGMENT_ATLAS_MSL_V11` 的查找一致，后者定义 Metal `ps_atlas_address`。宿主提供真实 binding span、slot 数、shape、 tile extents 和 payload 字节数。任意 fragment 数仅需 payload 与目录两个 binding。 边界映射必须先作用于全局坐标。缺失 slot/mask bit 是明确缺页，不是零值或逐 fragment clamp。

同步 Result callback 使用其精确矩形或 Need 授权 atlas 路径。Float64/Int64 原始 bits 传输不代表已支持相应 GPU 浮点算术。GPU discovery 的 Result Need 规则见 [GPU Discovery](GPU-Discovery.zh.md)。

## 调度与资源状态机

`ResultTensorInput` atlas 只打包 tensor Need 授权的 samples。Preparation 保留 descriptor、coverage 和占用的 tile 坐标；materialization 将 sample bytes 复制到 Root 计量的 payload 与 directory buffers。生成的 atlas 自己拥有这些 buffers，但不保留 source Result 或 read windows。Atlas 不新增 read 权限；可以传输某种 scalar type 不代表 GPU 算术支持该类型。

Native callback 使用配置的同步 invocation。Host 在 dispatch 排空前保留 native views，随后释放 poll-scoped resources。缺失样本仍作为 directory hole 保留。查找不存在的 directory entry 返回 `NotFound`；CPU phase、缺少 Need 或 Descriptor-only Need 均不能扩大授权。Cancellation、分配失败和 sticky service error 会阻止 publication，并退休新分配的 buffers。

C Result bridge 使用相同的 Need-scoped atlas。Atlas buffer tokens 和 dispatch services 仅在 active poll 内借用；释放 token pair 中任一成员会使整对失效，下一次获取返回新 handle。Host 在 publication 时冻结转交的 output scratch，旧 writable token 之后不能修改已发布字节。每个 Result output 仍记录自己的当前 association 和 dependency evidence。

只有 operation 允许 CPU fallback 且失败是符合条件的 `BackendUnavailable` 时，同步 GPU attempt 才会重试 CPU。CPU attempt 开始前先排空 GPU invocation。Cancellation、stale state、protocol error、已发布 output 和其他不可重试错误不会转为 CPU retry。Result cache retention 与 active producer sharing 独立于 fallback 决策。

当前 integration 源码检查 transport 与失败边界：

- [`test_gpu_fragment_execution.cpp`](../../../tests/integration/gpu/test_gpu_fragment_execution.cpp) 用独立坐标 oracle 比较真实 native lookup 与输出，覆盖 rank、width、sparse holes、fragment packing、grant enforcement 和 Root admission。
- [`test_gpu_c_abi_execution.cpp`](../../../tests/integration/gpu/test_gpu_c_abi_execution.cpp) 加载 [`gpu_result_c_plugin.c`](../../../tests/fixtures/gpu_result_c_plugin.c)，检查 C atlas handles、stale tokens、publication revocation、malformed requests、错误与 native dispatch。
- [`test_fragment_atlas_gpu.cpp`](../../../tests/unit/test_fragment_atlas_gpu.cpp) 通过 native buffer capacity 与 readback 检查较底层的 atlas SDK。
- [`test_gpu_sync_fallback.cpp`](../../../tests/integration/gpu/test_gpu_sync_fallback.cpp) 检查 CPU retry、取消和保留 producer 行为。

所需设备不可用时，这些 native tests 返回已配置的 skip code。Protocol mock 与 CPU fallback 不能证明 native execution。
## 算法与 wire format

`prepare` 在物化前报告 payload 与目录的逻辑 allocation 大小。GPU 宿主应分别按设备实际 capacity 取整再 admission。`maximum_boxes` 限制占用 tile，`maximum_work` 限制元数据扫描、样本发现、hash/probe 和 packing；cardinality/equality 扫描前检查取消并预扣 metadata。`preparation_work()` 报告通用几何规划工作量，`materialization_work()` 报告通用 packing 工作量。`ResultTensorInput` overload 通过 `FootprintLimits::consume_work` 直接计入其工作；Result 物化还会额外计入 window acquisition 和 read-bound 工作，这些不包含在 `materialization_work()` 中。`ValueFragments` overload 保留调用方原有的外部计费方式。任一 allocation 后取消（包括 Empty）都会释放新 owner，不发布 atlas。

每个 tile 至多 64 个逻辑样本。可指定正值、rank 对齐且乘积不超过 64 的 tile extents； 默认从最右轴开始确定性填充。仅占用 tile 进入目录。Lookup 使用逐轴全局 tile 坐标， 不要求全局 shape 乘积可表示为 uint64。`ValueFragments` 拒绝需要 `PlanarImage` 的结构化 image facets；atlas 只传输 generic Value 与可接受的完整 ColorArray tuple，不提供 image-v2 dependency execution。

Payload 按 tile key 和有效 bit 顺序紧凑存放，孔洞不占样本。空端口使用一个惰性 payload 字节和两个空 slot，均不授权样本。

## 限制与错误处理

通用 atlas unit suite 注册为 `test_fragment_atlas`，源码为 [`tests/unit/test_fragment_atlas.cpp`](../../../tests/unit/test_fragment_atlas.cpp)。Native SDK suite 源码为 [`tests/unit/test_fragment_atlas_gpu.cpp`](../../../tests/unit/test_fragment_atlas_gpu.cpp)；Result callback 集成路径见 [`tests/integration/gpu/test_gpu_fragment_execution.cpp`](../../../tests/integration/gpu/test_gpu_fragment_execution.cpp)。这些源码共同覆盖 wire layout、坐标查找、sparse holes、有符号与零 stride、rank 和 dtype transport、buffer 准入、取消与 native readback。所需 Metal device 不可用时，native tests 可 skip。

`ValueFragments` overload 保留其允许的 Value coverage；需要 `PlanarImage` 的 structural image facets 会被拒绝。`ResultTensorInput` overload 只读取 tensor Need 授权的 samples。两种 packer 均保留 holes，缺失 directory sample 返回 `NotFound`，超出 tile 或 work limits 的 plan 会失败，取消会退休新分配的 owner。Device host 必须分别准入取整后的 payload 与 directory capacity；Atlas 查找不会触发额外 read 或提供 CPU fallback。
