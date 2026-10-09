---
spec_schema_version: 1
id: KERNEL-tensor-storage-zh
kind: shared_kernel_contract
status: Accepted
implementation_status: implemented_cpu
---

# 张量存储与区域访问

本文定义由 `Result` 拥有的张量存储和有界访问契约。[英文版本](../Tensor-Storage-and-Region-Access.md)为权威文档。`ResultTensorSpec` 定义逻辑形状、样本分组及可选的空间拓扑；底层存储可以使用私有 planar 页或 affine CPU 存储。颜色与 alpha 的含义属于 tensor facets，字节布局和区域访问由本文定义。

## 范围与所有权

`SchemaTemplate::tensors` 是 rank-1..8 张量 cell 的 schema 集合。每个 cell 具有表示元素类型和 cell 形状的 `ValueDescriptor`、零个或多个显式 `batch_axes`、可选的 `atomic_trailing_axes` 分组，以及一个 `ResultTensorLayout`。`ResultRef` 拥有 schema、已认证样本覆盖、relations、保留的源 association、resources 和 backing 生命周期。`PlanarImage` 是空间张量的内部 backing 实现，不定义另一个语义 result。

`ValueDescriptor`、`StridedLayout`、`CpuStorage` 和内部 `Value` 表示提供标量与 affine 存储机制。它们不会改变哪个 `Result` slot 拥有样本，也不会改变哪些区域已经认证。`ResultTensorSpec::sample_shape()` 将声明的 batch axes 前置到 cell shape。普通数值张量可以没有 batch axes；存储层不会自行增加 frame 或 layer 轴。空间 backing 将 cell shape 中声明的 height、width 和可选 channel axes 映射为物理布局。

rank 限制针对完整 sample shape。Cell rank 为 1..8，batch-axis 数量为 0..7，两者之和至多为 8。每个 tensor cell 和 batch-axis extent 均为正数。验证逻辑 schema 不要求创建稠密分配，也不要求所有 extent 的乘积可表示；需要总元素数的操作使用经检查的 `sample_count()`，乘积溢出时可能返回资源错误。仅凭 schema validation 无法判断后续 planar backing 请求能否通过 reservation 和 page-budget 检查。

### C++ 注册中的 metadata-derived Result 输出

C++ operation 可以为 Result port 声明 tensor-member predicate，并将 constraint 的 schema id/version 留空。对 output constraint，这要求 metadata specialization 和非空 member predicate：`tensor_key` 可以指定 member；省略该 key 时要求恰好一个 tensor。注册的 `OperationOutputTraits::result_schema` prototype 仍须完整。Specializer 返回具体 schema；registry 验证后将其 id/version 收敛到 output constraint。固定 schema 注册仍须保留声明的 id/version。C ABI 仍要求固定输出 schema。

`core.identity` 将输出专化为输入 schema，并接受恰好一个 tensor、无 scalar fields、任意 member key/schema id，以及七种受支持 dtype 中的一种。它保留 schema id/version、member key、batch axes 和 facets。它声明 bitwise-mapped movement；映射可用时执行可保留 view，不可用时可物化 copy。输出 association 记录实际输入 ObjectId；identity 发布新的 Result ObjectId。其输入读取使用 role 13（Data、Validation、Descriptor），映射 relation 使用 role 5。

## 核心数据模型与内存

```cpp
struct ResultTensorLayout final {
  bool spatial = false;
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
  std::vector<ImageComponentGroup> groups;
};

struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
  std::vector<ValueFacet> facets;

  std::vector<std::uint64_t> sample_shape() const;
  std::vector<std::uint64_t> observation_shape() const;
  Result<std::uint64_t> sample_count() const;
  Result<Footprint> close_samples(
      const Footprint& samples, const FootprintLimits& limits = {}) const;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultTensorSpec> tensors;
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultFacet> metadata;
};
```

片段省略了 validation、managed-copy 和 canonical identity 方法。`ResultFieldSpec` 仍用于有界 primitive records；张量样本使用 `SchemaTemplate::tensors`。物理 order 和 row pitch 不参与 semantic schema identity。因此，逻辑 schema 相同的两个 slot 可以采用不同且有效的 CPU backing 策略。

`atomic_trailing_axes` 将指定的末尾 cell axes 作为一个不可拆分的 observation：`close_samples()` 会将每个非空请求扩展到这些 axes 的完整范围。已识别的 Semantic Image 和 ColorArray facets 也会闭合到完整 channel tuple。Tensor description metadata 和物理 `groups` 本身不创建 peer-channel demand。Operation 声明的 sample Need 仍决定样本读取权限。

逻辑 batch axes 可以表达 schema 声明的上下文，包括应用选择使用这些名称时的 frame/layer。存储层不会给非空间张量附加 frame/layer 含义。Spatial batch domain 使用逻辑 extents 表示；schema validation 没有固定的 4096 乘积上限，Result 也不会按完整乘积分配稠密 backing directory。具体读取、relation 构造和 backing 请求仍受各自的算术检查与资源限额约束。

### Affine backing

Affine `StridedLayout` 为每个逻辑轴提供一个有符号 byte stride，并可指定 byte offset 对应的逻辑 origin。当经过检查的地址边界证明每个样本都位于保留的 `CpuStorage` span 中时，可以使用正、负或零 stride。零 stride 表示广播样本。Singleton axis 不消耗地址 span，其 stride 值不影响寻址。存储 span 包含末尾元素；发布前会检查此范围。Padding 字节不属于样本。

这种布局可以用很小的物理 span 表示很大的逻辑域。例如，shape `{INT64_MAX+2, 3}` 配合 stride `{0, -1}`，可由三个字节承载：第一个轴上的坐标都广播，第二个轴从后向前遍历。因为逻辑 extent 不会被当作稠密分配请求，这种布局有效。地址和偏移的有符号、无符号算术仍须通过溢出检查。

`ResultTensorReadWindow::row_run()` 返回一段借用 run；`data` 指向第一个逻辑样本，`sample_stride_bytes` 可以为负或零。`bytes` 是包括末尾元素的经检查物理地址跨度，不表示 `data[0..bytes)` 是连续逻辑数据。读取各样本时须应用 stride。`rectangle_run()` 另提供有符号 row stride，也不表示各行在物理内存中连续。

### 空间 backing

`layout.spatial` 选择空间访问约定。Height 和 width axes 标记 cell-local 坐标，channel axis 可选。私有 `PlanarImage` 为它实际 backing 的坐标预留一个连续虚拟地址范围。每个逻辑坐标在该范围内有稳定 byte offset，物理页 backing 按需提供。`ResultBuilder::start()` 保存 schema、空 coverage 与 relation，以及物理配置；它不会为每个逻辑 batch coordinate 都预留 planar span。Affine `CpuStorage` publication 与读取不会创建 planar backing。无论采用哪种 backing，都使用相同的 `ResultTensorReadWindow` run 轴和坐标。

物化 planar 行中的相邻样本具有正 scalar-width stride。Continuous 存储可以带有经检查的行尾 padding。Tiled 存储使用 `Tw*d` 作为 row pitch，其中 `d` 为元素宽度；遇到 tile 边界时，run 可以在请求区域结束之前停止。没有 padding 且平面紧密排列的常规 planar HWC 张量可以用 stride `{W*d, d, H*W*d}` 表示；对应 CHW 顺序为 `{H*W*d, W*d, d}`。物理 planar 存储不要求逻辑顺序为 CHW。一般 affine 存储可以具有有符号或零 stride，也不会因此自动变成 planar backing。

`PlanningOptions` 为 DAG plan 提供一组共用 tile 高和宽；operation 不会单独选择输出 tile 尺寸。详见[并行执行模型](../../kernel-architecture/Parallel-Execution-Model.md)和 [`PlanningOptions`](../../../include/photospider/compiler/compiler.hpp)。`ResultBuilder::start()` 在接受物理配置前校验 tile 高宽均为正的 2 次幂，包括 1。Planar reservation 大小、页上限和地址算术在 operation 首次请求该 backing 时检查。当前默认值为 128×128。逻辑图像和 ROI extent 可以是任意正数。以下公式只描述私有 tiled backing，不定义语义坐标，也不让 padding 成为有效样本。

[FMT 公共契约](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)和[codec 边界](../../built-in_ops/02-format-color/op_specs/FMT_codec_boundary.md)分别定义颜色 metadata 与图像导入／导出的职责。内部颜色和 alpha 平面使用相同尺寸与共位样本，包括全分辨率的 Y、Cb 和 Cr。外部色度 subsampling 与物理打包属于 codec。这些约束不禁止 tensor 使用 affine CPU backing。

## Tile 布局与溢出检查

设 tile 尺寸为 `(Th,Tw)`，平面 extent 为 `H x W`，scalar width 为 `d`，主机页大小为 `P`。网格以图像坐标 `(0,0)` 为原点，不随请求 ROI 改变。坐标 `(y,x)` 位于 tile `(floor(y/Th), floor(x/Tw))`，tile 内偏移为 `(y mod Th, x mod Tw)`。

对有效尺寸为 `ht x wt`、byte offset 为 `ot` 的 tile：

```text
row_pitch      = Tw*d
valid_bytes    = ht*wt*d
padding_bytes  = ht*(Tw-wt)*d
storage_span   = ht*Tw*d
next_offset    = align_up(ot + storage_span, P)
alignment_gap  = next_offset - (ot + storage_span)
```

内部 tile 保存紧密排列的 `Th*Tw` block。右边缘 tile 在每个有效行末尾 padding。底边保留实际有效行数，不预留缺少的 `Th-ht` 行。因此，当 `Tw=128` 时，有效尺寸 `2 x 72` 的边缘块占用 `2 x 128` 个样本槽，而不是 `128 x 128`。每个 tile 起点均按页对齐，即使 payload 小于一页也如此。图像基址按页对齐；平台的预留粒度取整可能增加尾部。

设 `nx=ceil(W/Tw)`、`q=floor(H/Th)`、`hrem=H mod Th`。每一行 tile 的有效高度相同。经过检查的布局量为：

```text
full_step    = align_up(Th*Tw*d, P)
edge_step    = 0 if hrem=0 else align_up(hrem*Tw*d, P)
plane_step   = nx*(q*full_step + edge_step)
ht           = min(Th, H-ty*Th)
tile_offset  = plane_offset + ty*nx*full_step
               + tx*align_up(ht*Tw*d, P)
sample_offset(y,x) = tile_offset + (y mod Th)*Tw*d + (x mod Tw)*d
```

对尺寸相同的平面，`plane_offset=c*plane_step`。预留 span 包含按页对齐的 tile 槽位。每个 tile 槽内只有 `ht*Tw*d` 字节属于行填充后的样本存储。页对齐间隙和行 padding 都不是样本。Tile 数为 `ceil(H/Th)*ceil(W/Tw)`；分配前检查字节数、pitch、offset、对齐量和 shape 乘积。

Padding 不是隐式零值、图像样本或滤波边界规则。它不会改变颜色值、样本身份或 dirty-region 映射。Continuous backing 中，一页可能跨平面边界。在页对齐的 tiled backing 中，一页不会包含下一个 tile，但可能覆盖同一 tile 中未请求的位置。提供某页不会认证其余样本。

### 行填充边缘示例

取 `W=200`、`H=130`、`C=4`、Float32、`T=128`，页与预留粒度 `P=16384`。首平面中的 tile 如下，offset 和 pitch 单位为字节：

| 有效高 | 有效宽 | 行 pitch | Tile offset |
| --- | --- | --- | --- |
| 128 | 128 | 512 | 0 |
| 128 | 72 | 512 | 65536 |
| 2 | 128 | 512 | 131072 |
| 2 | 72 | 512 | 147456 |

下一平面从 163840 开始。四个平面共预留 655360 字节（640 KiB）：有效样本 416000 字节、行 padding 116480 字节、地址间隙与尾部 122880 字节。Tile 存储连同行 padding 共 532480 字节（520 KiB）。R 在 `(y=129,x=199)` 的 byte offset 为 148252。没有已有 backing 时，准备这个样本会提供索引 9 的页，即 16384 字节；逻辑请求仍为 4 字节，同页其余像素仍未认证。

## Read window 与事务写入

```cpp
struct ResultTensorRun final {
  const std::uint8_t* data = nullptr;
  std::uint64_t samples = 0, bytes = 0;
  std::int64_t sample_stride_bytes = 0;
};

struct ResultTensorRectangle final {
  ResultTensorRun row;
  std::uint64_t rows = 0;
  std::int64_t row_stride_bytes = 0;
};

Result<ResultTensorReadWindow> ResultRef::acquire_tensor(
    const ResultDescriptor&, std::uint32_t slot, const Region&,
    const CancellationToken&) const;

Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Region&,
    const std::function<Status(
        const ResourceVector<ResultTensorWriteWindow>&)>& write,
    ResultRelation, ResultFinality, const CancellationToken&);

Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint&,
    const std::function<Status(
        const ResourceVector<ResultTensorWriteWindow>&)>& write,
    ResultRelation, ResultFinality, const CancellationToken&);
```

完整声明见 [`result.hpp`](../../../include/photospider/data/result.hpp)。Read window 拥有源 `ResultRef`、captured descriptor facts、schema、association、resources，以及精确授权区域所需的 backing。它是 move-only。Window 销毁前，run 指针保持有效。Captured descriptor 限定可见前缀：之后的发布不能扩展早先 descriptor 的 coverage。请求超出已认证 coverage 时返回 `NotFound`；过时 descriptor facts 返回 `Stale`。

Read window 授权一个逻辑矩形；只要整个矩形均已认证，它就可以跨多个 batch coordinate。对私有 planar backing，acquire 会为每个覆盖到的 batch prefix 保留独立的物理 piece，并在每个 piece 中记录完整 batch coordinate。`row_run()` 和 `rectangle_run()` 一次访问一个坐标：spatial run 沿 cell width axis 前进，rectangle 沿 cell height axis 前进，batch coordinates 由调用者提供。因此一个 window 可以覆盖多个 batch，但单个物理 run 不跨 batch 边界。非空间 window 可以覆盖一般区域；当 rank 允许时，run 沿最后一个轴前进，rectangle 沿倒数第二个轴前进。

Coordinator 在建立 window 前会授权精确逻辑样本集合。对 affine 存储，它保留获准 `CpuStorage` 的 subviews；对私有 planar backing，它保留页 window，包括由认证 view 映射到源页面的情形。它不会将预留地址或 padding 暴露为可读数据。Batch-piece 索引比较会向 source root 计入 work。Window 保留 acquisition 时的 cancellation token；该 token 取消后，之后的 run lookup 返回 `Cancelled`。已经返回的指针仍有效，直到 window 销毁。页面供给量可能超过请求字节数，并与逻辑样本 coverage 分开计费。Callback 执行前，coordinator 显式准备页面和上游样本；page-fault 处理不会调度 DAG 工作。系统不会自动驱逐仍存活的页面，也不会回放 producer 来恢复页面。稀疏 bookkeeping 只记录选中的页面，不会为虚拟范围内所有可能的页面预先分配 metadata record。

`publish_tensor_kernel()` 只允许 callback 写入请求中的未发布区域。Callback 获取的 borrowed windows 和指针在其返回后失效；并发工作必须写互不重叠的 run，并在返回前结束。一般 tensor 使用一次自有 payload 分配，callback 成功后直接发布该存储。空间 tensor 在 callback 执行前准备私有目标页。对每个请求的 batch coordinate，builder 会复用已安装的 `PlanarImage`，或准备一个私有 candidate，并只将该坐标记录在稀疏 backing directory 中。只有所有 writer 成功后才安装新 candidate；准备或 callback 失败会释放 candidate 的页、虚拟预留和 metadata，同时保留旧的已认证前缀。若请求的 planar span 无法预留，publication 会在 callback 执行前失败。空区域会跳过 callback、样本 payload 和页面工作，不增加样本 coverage；Result 仍保留 descriptor relation obligations。

Footprint 重载在一个事务中写入一组精确的、互不相交的 canonical box。单 box footprint 走 Region 重载。有多个 box 时，builder 为所有 tensor（包括 spatial tensor）分配一块私有 affine 缓冲，并按 `boxes()` 顺序为每个 box 传入一个 write window；空洞不获得存储。Callback 成功后，该缓冲按每个 box 一个 affine 片段发布，因此 slot 保留其逻辑 shape、spatial layout metadata 和全局坐标，而这些 box 的 backing 是 affine 存储而非 planar 页面。读取方与读取任何 affine backing 时一样使用实际的 run stride。失败、取消和重入修改遵循下文的事务规则，不增加 coverage。

空间写路径采用事务语义。它在 callback 执行前准备所需页面与容量，仅在 callback 成功且取消检查通过后提交，然后认证精确区域和 relation。Callback 失败、取消、覆盖重叠或资源失败都不会认证新样本 coverage；先前已认证的前缀保持可读且不可变。分配或供页失败会回滚未发布页面。准入失败返回 `ResourceExhausted` 等类型化状态；不会驱逐或静默重算仍存活的已发布页面。

Callback 执行期间，同一 producer 的 mutation 和 seal 会被拒绝。移动或替换 builder 会使当前事务在提交前失败。调用者须保持 producer 对象存活，直到 `publish_tensor_kernel()` 返回。读取已经认证的前缀，以及操作独立 builder，仍然可行。失败的 publication 会记录 producer failure；之后的 publication 会返回该失败。共享页面或 backing 分配不意味着可以读取未来样本。

## Mapping view

```cpp
struct ResultTensorViewTransform final {
  std::vector<ResultMappedAxis> source_axes;
  bool reshape = false;
};

Status ResultBuilder::publish_tensor_view(
    std::uint32_t slot, const Region& target,
    const ResultTensorReadWindow& source,
    const ResultTensorViewTransform& transform,
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation = {});
```

`ResultBuilder` 提供两种 view publication。Ordered-window 形式从同一 root 组装连续的物理 planar 平面，要求元素类型、空间坐标矩形、order 和 pitch 相匹配。Transform 形式将一个逻辑上已授权的 affine source window 映射到输出区域。若多个 affine fragments 共用同一个 `CpuStorage` owner，且合起来描述同一 affine 地址映射，该 window 可以由这些 fragments 组成。Kernel 从相邻逻辑坐标推导 singleton axes 的 stride，再验证每个 fragment 都符合该映射。两种形式都保留源 `ResultRef`、captured facts 和 resource owners，只发布获准的目标样本，不复制 payload。源 payload 仍由来源 root 计费；目标自身计费 metadata 与 relation。Affine transform 路径可以在 target payload capacity 为零时发布。系统拒绝源到目标的循环关系。

Axis transform 的 `source_axes` 为每个 source-window axis 提供一个 [`ResultMappedAxis`](../../../include/photospider/data/result_relation.hpp)，其中包括 batch axes。每个 map 只选择一个 source coordinate（`extent == 1`）。输出轴使用 `output_axis == -1` 表示固定 source coordinate；`0..target_rank-1` 表示合法 target axis。映射轴的坐标满足：

```text
source_coordinate = source_origin
                    + step * (target_coordinate - output_origin)
```

该计算会检查完整 unsigned coordinate 范围。正、负 step 分别选择正向和反向切片；零 step 广播一个 source coordinate。输出轴可以重排、插入、删除，也可以由多个 source-axis maps 共享。Source window 的精确 region 始终是授权边界。

设置 `reshape == true` 时，`source_axes` 必须为空。Transform 将完整 source window 按逻辑 row-major 顺序展平，再将该顺序分配给完整 output shape。Relation 使用完整 output shape 作为 ordinal 基准：先按 source-window extents 将 output ordinal 反解为坐标，再把各局部坐标加上 source-window offset，得到更大 input tensor 中的坐标。该逻辑映射独立于 source 的物理布局。有符号和零 stride 影响 payload 是否能表示为 view，但不改变 dependency support。

`ResultRelation::reshape()` 会构造匹配的 compact witness。两个 shape 的 rank 均为 1 到 8，且 factorized cardinality 必须相等；实现不会形成 `uint64_t` 稠密元素乘积。Output `Region` 是 witness，区域外的样本没有 support。`project(Q)` 将 output footprint `Q` 裁剪到 witness，再把精确的 boxes 映射到 source coordinates，并发出带类型的 tensor support。`preimage(Q, changed)` 计算 `Q` 内精确的 dirty 子集；`certify()` 检查 witness coverage。空请求和空 witness 不产生 dirty samples。Holes 保持为精确 box 集合，不会扩展成 bounding box。若精确投影超出 rectangle 或 work 上限、发生取消，或 metadata/scratch 申请失败，操作返回错误，不会先以部分投影集合调用 visitor。Scratch boxes 由 relation 的 resource root 分配；work 通过提供的 `consume_work` callback 或直接向该 root 计费，每个 work unit 只计一次。

C ABI 2 service [`ps_result_services_v2::publish_tensor_view`](../../../include/photospider/plugin/result_operation_plugin_api.h) 使用可空的 `ps_result_tensor_transform_v2`。`NULL` 选择 ordered planar assembly，可以提供多个 source window handles。非 `NULL` transform 选择 affine 形式，并且必须提供一个 source window。`reshape == 0` 时，`axes` 对每个 source axis 提供一个 anchored point map；`reshape == 1` 时，`source_rank` 为零且 `axes` 为 `NULL`。C transform 对象仅借用到本次 service call 结束；成功 publication 会独立保留 source，不依赖其 window handle。

对于 affine transform 形式，`ViewUnavailable` 表示 source backing 无法表达所请求的映射，包括 page-backed 或非 affine source windows，以及 owner 不同的 affine fragments。Ordered-window 形式则单独支持符合条件的 planar pieces。Affine `ViewUnavailable` 会保留 builder 可用，使调用者可以显式执行 materializing copy；numeric `auto` layout 可选择该路径，`view` 则报告映射不可用。资源准入或 work-limit 错误、取消、Invalid transform、source authorization、channel closure、target overlap、finality、relation ownership 或 coverage，以及循环都会返回错误，不会选择 copy retry。因而 `2` reshape 为 `3` 会先返回 `InvalidArgument`，即使 source 的物理布局随后也可能被判为 fragmented。

行为测试覆盖六种 C++ affine 情形：转置、负 step 切片、reverse reshape、`{UINT64_MAX, UINT64_MAX}` 零 stride reshape、带 `INT64_MIN` stride 的 singleton axis，以及 broadcast。测试还验证 compact reshape projection、逆向 dirty region、witness coverage、巨大 factorized shape、精确 holes、取消及资源上限错误。C11 DSO fixture 使用 `make_reshape()` 和 reshape view publication，将 `2x3` source 映射为 `3x2`，并分别检查负 stride 和零 stride。测试检查 output samples、精确 dirty mapping、pointer 与 owner identity，以及 source capability 和 binding 退役后对 retained window 的读取。`make_reshape()` 构造 dependency support，不授予 payload read authorization。

## 资源计费与释放

分别记录虚拟地址预留、已提供页容量、有效样本 coverage 和已计费 metadata。一个小逻辑 view 可能保留较大的 backing 页集合。Resource root 按各自限制计量输出存储、windows 和 metadata。`resident_bytes()` 返回 backing 与 metadata 的计费快照，不是实测进程 RSS，也不保证操作系统会让每一页常驻。

`ResultRef::association()` 记录经过验证且保持顺序的 source object IDs；dependency bundle 保留 source observations，但不会持有 input payload。因此 materialized 或 scalar-copy output 可以在 inputs 退役后继续保留 source identities 和 dependency facts。已发布的 affine 或 planar view 会独立保留其实际使用 payload bytes 的每个 source `ResultRef`，并随之保留 source schema、resources 和 backing。最后一个 owning Result 或 retained window 会释放这些 view-source owners。只要 Result 仍存活，关闭 read window 不会撤销已发布样本。Tensor facets 引用的 resources 会随其 Result owner 保留。取消或失败会正常释放未发布分配，同时保留已发布样本和 observations。

## 限制与非目标

- Schema 最多有八个逻辑 sample axes。Spatial backing 仅为请求中的 batch coordinates 创建，不需要为逻辑 batch domain 建立稠密目录。
- 当前私有 planar 几何要求 tile 高和宽均为正的 2 次幂，不支持逐平面 tile 尺寸。
- 物理布局 groups 描述存储组织，不授予样本读取权限，不验证颜色值，也不会自行创建 channel dependencies。
- 提供某页不会使整页的样本 coverage 有效。缺失样本不视为零。
- Affine source-window publication 支持 anchored point maps 和兼容的有符号／零 stride reshape chunks。C ABI `make_reshape()` 提供 compact dependency support；payload access 仍须通过显式 tensor window authorization 获取。
- 本文定义 CPU tensor storage 和 access，不保证 GPU 地址映射或物理页常驻。
- Package、workflow、traits 和 plugin ABI 的版本值维护在 [Compiler Version Contract](../../development/Compiler-Version-Contract.md) 中。

## 行为证据

当前注册的 image tests 按行为分组。[`test_result_image_backing.cpp`](../../../tests/integration/image/test_result_image_backing.cpp) 覆盖 tensor windows、稀疏 batch access、publication、backing boundary 和 retained owners。[`test_result_image_views.cpp`](../../../tests/integration/image/test_result_image_views.cpp) 覆盖 affine 与 fragmented views；[`test_result_image_transform.cpp`](../../../tests/integration/image/test_result_image_transform.cpp) 覆盖 split 和 downsample 行为。[`test_result_image_format.cpp`](../../../tests/integration/image/test_result_image_format.cpp) 覆盖 image schema 与 controls。共享 Result contract 检查位于 [`test_result_image_contracts.cpp`](../../../tests/integration/test_result_image_contracts.cpp)，C plugin service 和 publication cases 位于 [`test_result_plugin.cpp`](../../../tests/integration/test_result_plugin.cpp)。这些源码覆盖 schema rank 与 atomic grouping、由紧凑有符号／零 stride backing 支持的超大逻辑 extent、有界 work 与 cancellation 下的稀疏 windows、tile boundaries、精确 read 授权、事务写入、planar assembly、affine transforms，以及 source window 或 context 退役后的 access。它们不证明所有 plugin transformation 或 native GPU mapping 均已测试。

页预留与提交是不同的操作系统概念；[Microsoft 的 `VirtualAlloc` 文档](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)说明了 Windows 上的区别。本文中的计费术语描述内核管理的 backing，不表示可跨平台推导 RSS。
