# 逻辑 Region 与 Result 图像 samples

英文权威文档：[Region-Semantics.md](../Region-Semantics.md)。

## 模块边界与所有权

`Region` 描述 descriptor domain 中的逻辑 samples。`Footprint` 保存这些 samples 的精确集合，包括不相邻的 boxes 和 holes。Storage origin、byte offsets、strides、padding、planar pages 和 tile geometry 描述物理访问方式，不会重新定义逻辑坐标或有效 coverage。

对图像而言，descriptor domain 外加 tensor 的 batch axes（例如 frame 和 layer）。`ResultRef` 拥有图像结构、descriptor、已发布 coverage、dependency relation 和生命周期；颜色与 alpha 含义来自算子参数。`PlanarImage` 为 Result image slot 提供 backing，并读取获准的 samples。Planner 确定请求范围，operation 声明输入 support，coordinator 只准入请求所需的读取和发布。

## 核心数据结构与内存布局

```cpp
struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
  std::vector<std::uint64_t> sample_shape() const;
  Result<Footprint> close_samples(const Footprint& samples,
                                  const FootprintLimits& limits = {}) const;
};
```

图像是 `layout.spatial` 为 true 的 tensor slot。其逻辑 sample 形状为 `batch_axes` 后接 cell descriptor，例如 `{N,L,H,W}` 或 `{N,L,H,W,C}`：`N` 与 `L` 是 frame、layer 之类的 batch axes，`H/W` 选择空间 sample，可选的 `C` 选择 channel。Cell descriptor 的 rank 为 2 或 3。四通道像素可以放在分离的 planes 中；逻辑 tuple 完整不代表物理字节交错。

`close_samples` 把请求扩展到末尾 `atomic_trailing_axes` 个 cell axes 上。对 `atomic_trailing_axes = 1` 的 HWC 图像，请求某像素的一个 channel 会得到该像素的全部 channel；若 alpha 存储在 channel axis 上，也包含 alpha。Structural layout groups、schema id 和 channel 数量都不会扩展 sample set。`atomic_trailing_axes = 0` 的 slot 严格按 descriptor sample 坐标访问。Padding、plane gaps 和尚未发布的 samples 仍不可读。Storage views 保留 backing owner；Result coverage 及 captured descriptor 则授权 callback 可读范围。

普通 Value descriptors 和结构化 Result image descriptors 遵循不同契约。Value 的 extent 非零；在保留非零 descriptor 时，其请求 Footprint 可以为空。Result 的动态 structured field 可以有零行，image slot 的请求 footprint 也可以为空。两种空状态都不能伪造 samples 或省略 descriptor/count obligations。

## Demand、执行与状态

编译器解析每个具名 output 的请求 `Q`。对 image output，Result continuation 收到 captured output footprint 和 image slot。Callback 可分阶段请求 input needs：先取 Control samples，再根据 control 值选择 Data image samples。每个 Need 都依据 Result schema、slot、逻辑 sample domain 和 input 已发布 coverage 校验。Callback 不能同步取回空洞或当前 capability 以外的 sample。

Whole operations 保持 Whole dependencies。只有 `atomic_trailing_axes` 参与 sample closure；channel roles 和颜色解释是算子参数，不改变 dependency support。物理 `PlanarImageLayout::groups` 不会暗中新增 dependency edges。绑定了 `requires_complete` 校验器的输入端口需要 Whole Need，因此其算子不能在该端口上使用 tile Need。

Dirty 表示可能受影响，不表示数值不同。Footprint transformations 有界，并计入 execution root；超出 work 或 metadata 限额时返回错误。

## Tile 投影的算术

对正数 tile extent `T` 和逻辑 extent `E`，tile 数量为：

$$
C = \left\lceil \frac{E}{T} \right\rceil,
\qquad
C = E / T + (E \bmod T \ne 0).
$$

该整数表达式避免计算可能溢出的 `E + T - 1`。Tile projection 仅为物理调度分组精确请求的 samples，不扩展其 dependency support 或 coverage。

## 限制与错误处理

越界 regions、未授权读取、不完整发布和重叠 image writes 均会校验失败。请求在访问前沿 atomic trailing axes 扩展；`atomic_trailing_axes` 大于 cell rank，或 closure 超出 slot domain 时校验失败。Resource exhaustion、取消、stale binding replacement、sticky callback failure 和 protocol errors 保留各自状态，不会变成 Empty coverage。Coordinator 复用借用存储前，所有已准入 callbacks 都会退出。

空 output demand 不执行 image sample 工作，但 descriptor 和 control obligations 仍需明确记录。成功的 image publication 必须覆盖 captured output demand，并提供 relation evidence。已发布的 Result prefix 保持不可变；之后的 producer failure 不会扩大旧 descriptor 的授权。

C11 fixture 覆盖 runtime Field rows，包括零行与非零行之间的替换。跨 frame support 和各 slot 独立 dirty 由 `test_result_image_contracts` 覆盖。
