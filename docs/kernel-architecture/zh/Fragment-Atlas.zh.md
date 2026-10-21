# 将稀疏 fragment 打包为 atlas

[英文权威文档](../Fragment-Atlas.md)。

## 范围与所有权

`FragmentAtlasPlan` 是面向 `Tensor` 的数据层传输工具。Preparation 记录 descriptor、精确 coverage 和占用的 tile 坐标。Materialization 将覆盖范围内的 sample bits 复制到独立拥有的 payload 与 directory buffer。Plan 不保留来源存储；输入 Tensor 释放后，物化的 atlas 仍然有效。

Atlas transport 支持所有内置 scalar 宽度，这不证明相应 GPU 算术已受支持。固定执行器传递物理 Plan 声明的 view；atlas packing 是显式数据操作，不属于固定 kernel invocation 的 service table。

## 公共接口

```cpp
struct FragmentAtlas final {
  TensorDescriptor descriptor;
  std::vector<std::uint64_t> tile_shape;
  std::uint64_t slot_count = 0, payload_bytes = 0;
  TensorSpan payload, directory;
  Result<std::uint64_t> address(
      const std::vector<std::uint64_t>& coordinate) const;
};

class FragmentAtlasPlan final {
 public:
  static Result<FragmentAtlasPlan> prepare(
      const Tensor& input, std::vector<std::uint64_t> tile_shape = {},
      const FootprintLimits& limits = {});
  Result<FragmentAtlas> materialize(
      const Tensor& input, const BufferAllocator& allocator,
      const FootprintLimits& limits = {}) const;
};
```

以上声明省略大小与工作量 accessor 以及私有状态；完整 API 位于 [`fragment_atlas.hpp`](../../../include/photospider/data/fragment_atlas.hpp)。物化输入必须匹配 preparation 的 dtype、shape 与 coverage。物化时读取当前样本值，因此同一个 plan 可以打包静态 geometry 相同、样本值不同的输入。

Packing 保留 UInt8、Int64、Float64、Float32、Int8、UInt16 和 Int16 bits。Tensor 读取支持碎片化存储、有符号 stride 和 broadcast 存储。缺失样本保持为空洞。查找空洞返回 `NotFound`，查询 domain 外坐标返回 `InvalidArgument`。

## Geometry 与 wire format

Rank 为 1..8。每个传输 tile 至多包含 64 个逻辑样本。显式 tile extent 必须为正数、与 rank 匹配且乘积至多为 64。空 geometry 从最右轴开始填充，选择确定性布局。Atlas 传输 tile 独立于物理 Plan tile。

只有占用的 tile 进入 directory。Lookup 使用逐轴坐标，全局 shape 乘积无需能以 uint64 表示。Directory 的 slot 数为 2 的幂，至少为 2 且至少为占用 tile 数的两倍。Slot 使用开放寻址和线性探测，每个 slot 为 80 个 little-endian 字节：

| 字节 | 含义 |
| --- | --- |
| 0..63 | 八个 uint64 全局 tile 坐标；未使用轴为零 |
| 64..71 | uint64 有效样本 mask；零表示空 slot |
| 72..79 | uint64 packed payload 起始字节偏移 |

Hash 对 rank 个坐标字逐个执行 FNV-1a，每个字低字节在前，offset basis 为 14695981039346656037，prime 为 1099511628211。局部 row-major 坐标选择 mask bit。存在样本的 payload offset 等于 tile 字节偏移，加上其前方置位 bit 数乘以 dtype 宽度。Payload 按 tile key 和递增有效 mask bit 排列。空洞不占 payload 样本。

空 Tensor 使用一个惰性 payload 字节和两个空 directory slot，不包含可读样本。边界映射属于采样算法，必须在 lookup 前作用于全局坐标。

C 兼容 SDK 宏 `PS_FRAGMENT_ATLAS_MSL_V11` 为 Metal shader 定义使用相同 directory format 与 hash 的 `ps_atlas_address`。该 shader helper 仅接受 1、4、8 字节样本，拒绝 UInt16 和 Int16 使用的 2 字节宽度。CPU packing 与 `FragmentAtlas::address` 支持这两种类型。Host 必须提供有效 buffer span、shape、tile extent、slot 数和 payload 字节数。固定执行器通过 native kernel preparation 与异步 completion 链接 GPU placement。Atlas packing 保持显式：operation 声明 packed buffer 及其固定 read view。`test_fragment_atlas_gpu` 在 rank 1、3、8 的稀疏 atlas payload 上执行 Metal address helper；公开物理 Plan 集成由 `test_fixed_native` 独立验证。

## 资源限额与失败

`payload_allocation_bytes()` 和 `directory_allocation_bytes()` 在 packing 前报告精确逻辑分配大小。Device host 必须分别准入取整后的物理容量。Payload 和 directory 保留 allocator 的存储 owner。

`maximum_boxes` 限制占用 tile 数。`maximum_work` 限制 metadata 扫描、样本发现、hash、probe 与 packing。`preparation_work()` 和 `materialization_work()` 报告相应的工作量估计，供调用方计费。Tensor 入口执行局部限额和取消 token 检查，但不调用 `FootprintLimits::consume_work`。

无效输入、geometry 或物化输入不匹配会导致失败，不发布 atlas。工作量或容量耗尽以及取消会释放新分配的 owner，包括任一次分配后发生的取消。在无效 plan 上调用大小或工作量 accessor 会抛出 `std::logic_error`；`valid()` 返回 false。

[`test_fragment_atlas.cpp`](../../../tests/unit/kernel/test_fragment_atlas.cpp) 检查数据工具的字节、稀疏查找、geometry、stride、所有权、限额与取消。Native SDK 测试属于独立验证范围；CPU 测试成功不证明设备执行。
