# 张量语义元数据与原子编辑

英文权威版本：[Tensor-Semantic-Metadata.md](../Tensor-Semantic-Metadata.md)。

## 1. 模块边界与职责

`TensorDescription` 描述逻辑 tensor、channel、axis、color group、encoding、sampling、profile 和 configured space。它不拥有样本存储、不认证样本值，也不隐含图像布局。`ResourceBindings` 持有 metadata 引用的不可变 ICC profile 与冻结 OCIO snapshot。Result 持有不可变 schema 和 facets；Result view 可独立保留 source backing 与 resource owners，不依赖 metadata header 的生命周期。

## 2. 核心数据结构与内存布局

```cpp
using TensorEndpoint = std::variant<std::int64_t, double, TensorRationalEndpoint>;
struct TensorDescription final {
  std::optional<std::uint32_t> channel_axis;
  std::vector<TensorChannelDescription> channels;
  std::optional<TensorChannelDescription> component;
  std::vector<TensorAxisDescription> axes;
  std::string model, primaries, transfer, reference, association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ColorProfileIdentity> profile;
  std::vector<TensorColorGroup> groups;
  std::optional<TensorEncoding> encoding;
  std::optional<TensorSampling> sampling;
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
  std::optional<TensorModelCoordinates> coordinates;
};
Result<ValueFacet> encode_tensor_description(const TensorDescription&);
Result<TensorDescription> decode_tensor_description(const ValueFacet&);
```

`TensorDescription` 保存可选 channel axis 上的有序 channels、一个选中的 component、逻辑 axes、全局 interpretation 默认值和完整 color groups。校验要求 rank 为 1 到 8，channel axis 必须在 rank 内，axes 表为空或每个逻辑轴各有一项。每个 axis 有 name、unit、有限 origin 和正 step。完整 group 包含有序且不重复的 component indices 及对应 channel 描述；可选 alpha index 必须属于同一 tensor，且不能与颜色 indices 重合。Group 与显式提供的 channel 描述必须一致。

`TensorEncoding` 按下式将存储值解释为解码值：

$$
D(x)=d_0 + (x-s_0)\frac{d_1-d_0}{s_1-s_0}.
$$

Stored 端点必须递增；decoded 端点必须不同，也可以递减。Stored 端点必须符合 tensor dtype。端点保留 Int64、binary64 或约分后的精确有理数类型。有理数幅度使用小端 base-2^32 limbs 编码，每个约分后的分子与正分母最多 128 个 word；整数不会先转成 binary64。描述只附加解释，不会转换、裁剪或缩放样本。完整整数颜色组必须从 component、channel 或 tensor 默认值取得显式 decoder。删除该 decoder 会使完整 native-color 声明失效。显式 encoding 或 sampling grid 冲突会导致校验失败。

内部颜色组使用同尺寸、共位采样：显式 grid、scale `(1,1)`、offset `(0,0)`。外部 subsampling 属于 I/O codec。Configured space 指定冻结 config identity、canonical space 和 `scene` 或 `display` reference。Analytic binding 由调用方提供 model/primaries/transfer/reference/white、有序 roles/units 和 convention；它不证明数学等价。

Color coordinates 可记录空值或 `relative`/`absolute` scale、可选描述性 observer、灰度解释（`linear_y`、`encoded_luma`、`cielab_l`、`oklab_l`）以及可选有限 binary64 NCL 系数 `[Kr,Kb]`。空字段表示未作断言。`relative-v1` 将 CIELAB/CIELCh lightness 存为 `L*/100`，a/b/chroma 保持不变，XYZ 继续使用 Y=1 reference scale；它不是范围裁剪。ICC/OCIO-native convention 指资源定义的坐标。上述字段均不证明样本有限性、alpha 合法性或预乘约束。

`photospider.tensor-description` facet 上限为 4096 字节。文本为严格 UTF-8，最长 128 字节；最多 128 个 group，每组最多 64 个 component。Canonical 小端 codec 保留整数与 IEEE binary64 位、表顺序、显式 presence byte 和资源身份。版本 4 保持既有含义；版本 5 增加 model-coordinate 记录。tensor、component、channel 或 group 任一层存在 coordinates（包括显式存在但为空的记录）时使用 v5，否则使用 v4。旧版本、版本与 discriminator 不匹配、非 canonical 编码及尾随字节均被拒绝。Opaque annotation 是 Result tensor 上独立的 facet，受宿主最多 64 个 facet、每个 64 KiB、总计 1 MiB 的限制。

## 3. 调度与状态机

`ps::format::assign_metadata` 先检查编辑语法、类型、选项、路径重叠和有界事务编码，再追加一个节点。Compiler 随后解析源相关 selectors 并校验完整候选。当前注册使用 Result operation ABI 2、WorkflowDocument 5、OperationTraits 24 和 package 0.32.0。每个输入 Result 恰有一个 tensor member 且没有 fields。内部 node 可以提供可选 canonical schema assertion；public helper 不会设置它。

```text
authoring：校验编辑语法 -> 编码有界事务 -> 追加节点
                                         |
compile：在源对象上解析 selector -> 构建候选 -> 校验
                                      +----------+----------+
                                      |                     |
                                     通过                  无效
                                      |                     |
runtime：精确坐标映射 -> view/copy    编译错误；无输出
```

公开 helper 位于 `ps::format`：

```cpp
struct MetadataOptions final {
  std::string mode = "patch";
  std::vector<MetadataSet> set;
  std::optional<TensorDescription> description;
  std::vector<std::string> remove;
  std::string dependencies = "error", missing = "error";
  std::string layout = "auto", profile = "strict";
};
Result<WorkflowNodeOutput> assign_metadata(
    WorkflowDocument&, WorkflowInput, const MetadataOptions& = {});
Result<WorkflowNodeOutput> remove_metadata(
    WorkflowDocument&, WorkflowInput, const std::vector<std::string>&,
    const MetadataOptions& = {});
```

默认值为 `mode=patch`、`dependencies=error`、`missing=error`、`layout=auto`、`profile=strict`。注册的 CPU profile 为 `strict`、`accelerated_apple_silicon`、`accelerated_x86_64`；在各自后端准入规则下，它们产生相同 metadata 和样本位。Replace 要求完整 `description`（可为空），保留 opaque annotations，且只允许编辑 annotation。Patch 禁止填写 `description`。`remove_metadata` 通过同一 authoring 路径降低为仅删除的 patch。无效 authoring 不改变 document。源相关 selectors 和目标结构在 compile 阶段校验。输出 schema 保留 source schema id、tensor key、descriptor、batch axes 和 physical layout，只更新 semantic 及被编辑的 annotation facets。

对输出 footprint Q，Result continuation 使用 role mask 9（Data 1 | Descriptor 8）请求 Q 的 Data support 及 source description 的 Descriptor support，不请求像素 Validation 或 Control support。Dependency-v2 将每个输出坐标映射到相同输入坐标，因此输入样本变化会使对应输出样本变脏。Empty 输出需求使用 stateless continuation 发布空 Result，不请求 payload。

当请求映射可表示时，`auto` 和 `view` 发布 Result view，保留 source backing、resources 和 association。只有返回 `ViewUnavailable` 时 `auto` 才 materialize；强制 `view` 返回该错误。`materialize` 为请求的输出 coverage 分配空间，并通过 transactional writer 逐位复制样本。复制每 256 个样本以内检查取消。通用 tensor 在 Result view 有效时支持正、负和零 stride。Spatial layout 可保留源 physical owner 与 DAG tile geometry。该 operation 保留 source publication policy 并禁用 Result cache。

## 4. 算法与数学

路径使用 `/` 分段，`~0` 转义 `~`，`~1` 转义 `/`。

- `/semantic` 选择整个语义记录。
- `/semantic/channels/index:0/unit` 选择原始 channel；`name:` 和 `role:` selector 必须精确唯一匹配。`missing=ignore` 可以忽略零匹配删除，歧义 selector 仍会失败。
- `/semantic/groups/color/interpretation/primaries` 选择 group 字段；`/semantic/axes/0/origin` 选择 axis 字段。
- Coordinates 子树和叶子路径包括 `/semantic/coordinates`、`/semantic/component/interpretation/coordinates`、`/semantic/channels/index:0/interpretation/coordinates`、`/semantic/groups/gray/interpretation/coordinates`，并支持相同的 channel selectors。
- `/annotations/app.note` 选择 opaque facet。`photospider.` 命名空间保留给语义 facet。

Selector 在修改前基于原始输入解析。Whole-subtree set 会替换整个子树；leaf set 保留兄弟字段。重复路径、祖先/后代重叠以及多个 selector 指向同一目标都会失败。删除 channel 或 axis 描述不会删除其数据槽位。Cascade 只清理受影响的旧依赖描述，并保留所有显式 set；清理会删除新设置的值时事务失败。Compiler 发布一个不可变候选，不会改写源对象或修复样本。

Channel、component 和重叠 group 的 coordinates assertion 逐字段合并。空值表示未作断言，且不能擦除此前的非空断言。同一字段有两个不等的非空值时冲突。精确 `operator==` 比较完整记录，不是兼容性判断。Codec 检查兼容性但不改写来源字段；channel assembly 会在输出描述中解析 overlay。

编辑事务是节点 `edits` 字符串中的有界 v1 记录，编码为小写十六进制。解码后最多 4096 字节、1024 个节点、深度 12；hex String 最长 8192 字节。树编码中，`o` 表示有序 map，`s` 表示 string，`u`/`i` 表示无符号/精确有符号整数，`d` 表示八个小端 Float64 字节，`b` 表示 opaque bytes；每项均以十进制长度或数量和 `:` 分隔。此事务 codec 与 TDM4/TDM5 facet codec 相互独立。优先使用 `MetadataOptions` 和类型化路径，不手写编码。

`ResourceBindings` 封存并去重显式 ICC/OCIO handles。每个引用身份必须在 compile、binding 和 output 准入时解析，ICC header model 也必须与声明的 interpretation 一致。ICC 准入检查显式 v2/v4 字节、profile class/PCS、必需 tags、TRC/LUT 边界和 profile identity。它接受 RGB/Gray matrix/TRC 和准入的 LUT profile；profile 定义的 XYZ/Lab endpoint 必须显式带 LUT，CMYK endpoint 路径要求对应 LUT 集。Abstract 与 DeviceLink profile 不是 endpoint resource。此结构校验不运行 color-management module。

OCIO snapshot 包含显式 config 字节、完整排序的逻辑文件映射、已解析 context、声明的 canonical spaces/references，以及固定的 engine/build/settings identity。Hash 和 byte length 覆盖所有分帧字节，包括缺失 lookup。准入只验证 snapshot 结构和所有权，不证明 transform 可执行。冻结后 lookup 不访问文件、网络或进程环境。Snapshot 复制和比较使用 `ResourceBudget`，按最多 1024 字节分块检查取消/work；file、context、space 表合计最多 1024 条目。`ValueFragments::retained_bytes` 按实际资源存储身份去重，也计入 config manifest。

## 5. 限制与非目标

- Tensor metadata 描述逻辑 axes 和 interpretation；不定义物理 strides、planar/tiled 存储或图像所有权。
- Profile identity 不包含资源字节；调用方必须在 `ResourceBindings` 提供可解析的 handle。
- Profile/configured-space 声明不会推断 analytic primaries、transfer 或 white。Analytic binding 仍是调用方来源信息。
- Metadata 校验不认证数值范围、有限性、coverage 或颜色正确性。除 `photospider.tensor-description` 外，已注册的 `photospider.` facet 必须显式导入后才能由此编辑器消费。
- ICC 准入是结构校验；OCIO 准入校验冻结快照，不执行 transform。Metadata assignment 不运行颜色管理转换，也不创建私有 worker pool。Profile class、tags 和 LUT 边界见[ICC profile 校验实现](../../../src/lib/data/icc_validation.cpp)。
- 静态 authoring 受 facet 和 transaction 限制；其 preparation path 没有运行时 cancellation token。运行时资源准入使用对应的根预算与取消检查。
- Metadata 编辑会禁用 sample-only cache reuse，因为输出 identity 包含 metadata。

资源准入、复制或 hash 失败会阻止发布并释放未发布 owner。Result header 可以移除资源引用，而旧 view 仍持有样本 backing；删除 facet 不保证所有祖先分配立即释放。当前 Result focused CTest 5/5 通过，public workflow 通过，installed consumer 的 2/2 检查通过。这些测试没有覆盖与 `channel.extract` 或 `channel.assemble` 的跨家族组合。旧 [metadata_performance](../../../examples/metadata_performance/README.md) 测量 Value/planar 执行，不是这些 Result operation 的性能证据。
