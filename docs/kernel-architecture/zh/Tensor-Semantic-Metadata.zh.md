# Tensor 描述与描述编辑（算子 SDK）

英文权威文档：[Tensor-Semantic-Metadata.md](../Tensor-Semantic-Metadata.md)。

本页说明算子 SDK 中的一种配置类型。内核不存储、不传播、也不校验 tensor 描述：Result 与 Value 只携带结构、数据和存储 owner。`TensorDescription` 只作为读取它的算子的显式静态参数存在。

## 1. 模块边界与所有权

`TensorDescription` 记录逻辑 tensor、channel、axis、color group、encoding、sampling、profile 和 configured space 的解释。该类型及其 codec 声明在 `photospider/ops/tensor_description.hpp`；纯编辑函数声明在 `photospider/ops/format/metadata.hpp`。二者都属于 `Photospider::ops_sdk`，后者只依赖内核公共接口。

SDK 拥有描述值、其 codec 和编辑器。调用方决定哪个描述用于哪个算子输入：格式或颜色算子以静态 String 参数（例如 `input_description`）接收描述，该参数参与算子的编译身份。不可变的 profile 与配置字节是独立的 `ImmutableResource` handle；调用方把它们放在 `ResourceSet` 中传给 `Compiler::compile`，算子的 preparation 按 `ResourceIdentity` 选择。描述可以命名 profile identity，但不能创建资源所有权。

描述不拥有样本存储，不认证样本值，也不隐含图像布局。

## 2. 数据结构与内存

```cpp
using TensorEndpoint =
    std::variant<std::int64_t, double, TensorRationalEndpoint>;

struct TensorDescription final {
  ResourceLease metadata_owner;
  std::optional<std::uint32_t> channel_axis;
  std::vector<TensorChannelDescription> channels;
  std::optional<TensorChannelDescription> component;
  std::vector<TensorAxisDescription> axes;
  std::string model, primaries, transfer, reference, association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ResourceIdentity> profile;
  std::vector<TensorColorGroup> groups;
  std::optional<TensorEncoding> encoding;
  std::optional<TensorSampling> sampling;
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
  std::optional<TensorModelCoordinates> coordinates = {};
};

Result<std::vector<std::uint8_t>> encode_tensor_description(
    const TensorDescription&);
Result<TensorDescription> decode_tensor_description(
    const std::vector<std::uint8_t>&);
Result<std::string> tensor_description_parameter(const TensorDescription&);
Result<TensorDescription> tensor_description_from_parameter(
    const std::string&);
Status validate_tensor_description(const TensorDescription&,
                                   const ValueDescriptor&);
```

`TensorDescription` 记录可选 channel axis 上的有序 channel、一个选中的 component、逻辑轴、全局解释默认值和完整 color group。`validate_tensor_description` 不读取样本，只把描述与 `ValueDescriptor` 比对。校验要求 rank 为 1 到 8，channel axis 位于该 rank 内，axes 表为空或每个逻辑轴恰有一项。每个轴有 name、unit、有限 origin 和正有限 step。完整 group 列出有序且互异的 component 索引及对应 channel 描述；可选 alpha 索引位于同一 tensor 内，且不在颜色索引中。Group 声明与显式提供的 channel 声明必须一致。

`TensorEncoding` 按下式把存储值映射为解码值：

$$
D(x)=d_0 + (x-s_0)\frac{d_1-d_0}{s_1-s_0}.
$$

存储端点必须递增；解码端点必须不同，可以递减。存储端点必须能用 tensor dtype 表示。端点保留其类型：Int64、binary64 或约分后的精确有理数。有理数幅值使用小端 base-2^32 limb，约分后的分子和正分母各最多 128 个 word；整数不经过 binary64。描述只陈述解释，不转换、不截断、不重新缩放样本。完整的整数 color group 需要来自 component、channel 或 tensor 默认值的显式解码器；删除该解码器会使完整原生颜色声明失效。冲突的显式 encoding 或 sampling grid 无法通过校验。

内部 color group 使用同尺寸共址采样：显式 grid、scale `(1,1)`、offset `(0,0)`。外部子采样属于 I/O codec。Configured space 命名一个冻结的配置 identity、canonical space 和 `scene` 或 `display` 参考。Analytic binding 是调用方提供的 model/primaries/transfer/reference/white、有序 roles/units 和 convention 断言，不证明数学等价。

颜色坐标允许空的或 `relative`/`absolute` 的 scale、可选的描述性 observer、灰度解释（`linear_y`、`encoded_luma`、`cielab_l` 或 `oklab_l`），以及可选的有限 binary64 NCL 系数 `[Kr,Kb]`。空字段不作断言。`relative-v1` 以 `L*/100` 存储 CIELAB/CIELCh 明度，a/b/chroma 不变，并保留 XYZ Y=1 参考尺度；它不是范围截断。ICC/OCIO 原生约定指向资源定义的坐标。这些字段都不能确立样本有限性、alpha 合法性或预乘关系。

Canonical 编码上限为 4096 字节。文本为严格 UTF-8，最长 128 字节；最多 128 个 group，每个 group 最多 64 个 component。Canonical 小端 codec 保留整数与 IEEE binary64 位、有序表、显式存在字节和资源 identity。Version 4 保持原有含义；version 5 增加 model-coordinate 记录。只要 tensor、component、channel 或 group 任一层存在 coordinates（包括存在但为空的记录），编码选择 v5，否则选择 v4。更旧版本、版本与判别字节不匹配、非 canonical 编码和尾随字节都会被拒绝。`tensor_description_parameter` 把 canonical 字节写成用于 String 参数的小写十六进制，`tensor_description_from_parameter` 只接受这种 canonical 形式。

## 3. 执行：纯描述编辑

```cpp
namespace ps::format {
struct MetadataSet final {
  std::string path;
  MetadataValue value;
};
struct MetadataOptions final {
  std::string mode = "patch";        // "patch" 或 "replace"
  std::vector<MetadataSet> set;
  std::optional<TensorDescription> description;
  std::vector<std::string> remove;
  std::string dependencies = "error";  // "error" 或 "cascade"
  std::string missing = "error";       // "error" 或 "ignore"
};
Result<TensorDescription> edit_description(
    const TensorDescription& source, const ValueDescriptor& descriptor,
    const MetadataOptions& options = {});
}  // namespace ps::format
```

`edit_description` 是纯函数。它把 `source` 与 `descriptor` 比对校验，执行一次事务编辑，校验候选结果，并返回新描述。它不追加 workflow node，不读取 payload，不查找资源；失败时 `source` 不变。分配失败可能抛出 `std::bad_alloc`。不存在 `metadata.assign` 或 `metadata.remove` workflow 算子；需要编辑后描述的调用方把返回值作为静态参数传给消费它的算子。

```text
source description + descriptor
              |
       validate source
              |
   resolve paths on source -> build candidate -> validate candidate
                                                  +--------+--------+
                                                  |                 |
                                                valid            invalid
                                                  |                 |
                                    new TensorDescription    InvalidArgument;
                                                             source unchanged
```

`replace` 需要完整的 `description`（可以为空），且不接受路径编辑。`patch` 禁止 `description`。只接受 `/semantic` 路径。

## 4. 算法与数学

路径以斜杠分隔；`~0` 转义 `~`，`~1` 转义 `/`。路径最长 1024 字节。

- `/semantic` 选择完整描述。
- `/semantic/channels/index:0/unit` 选择原始 channel。`name:` 与 `role:` 选择器要求恰好一个唯一匹配；`missing=ignore` 可以忽略零匹配的删除，歧义选择器仍然失败。
- `/semantic/groups/color/interpretation/primaries` 选择 group 字段；`/semantic/axes/0/origin` 选择轴字段。
- 坐标子树与叶路径包括 `/semantic/coordinates`、`/semantic/component/interpretation/coordinates`、`/semantic/channels/index:0/interpretation/coordinates` 和 `/semantic/groups/gray/interpretation/coordinates`，可配合支持的 channel 选择器使用。

选择器在编辑前针对原始 source 解析。整棵子树的 set 替换该子树；叶 set 保留兄弟字段。重复路径、祖先/后代重叠编辑以及解析到同一目标的别名都会失败。可以删除 channel 或 axis 描述而保留其数据位置。`cascade` 只删除受影响的旧依赖描述，并保留每个显式 set；如果清理会抹去新的显式值，事务失败。Patch 模式下 group 顺序沿用 source 顺序。

坐标断言在 channel、component 与重叠 group 声明之间逐字段合并。空值不作断言，也不能抹去较早的非空断言。同一字段的不同非空值互相冲突。精确的 `operator==` 比较完整记录，不是兼容性判定。Codec 检查兼容性而不改写来源。

编辑器内部把选项编码为有界的 v1 事务树：最多 4096 编码字节、1024 个节点、深度 12。树编码用 `o` 表示有序 map，`s` 表示字符串，`u`/`i` 表示无符号/精确有符号整数，`d` 表示八个小端 Float64 字节，`b` 表示不透明字节，每项带十进制长度或计数及 `:` 分隔符。该事务 codec 独立于 TDM4/TDM5 描述 codec，也不是公开参数格式。

`IccProfile`（`photospider/ops/icc_profile.hpp`）复制并准入显式 profile 字节。准入检查 v2/v4 字节、profile class/PCS、必需 tag、TRC/LUT 包络和 profile identity。它接受 RGB/Gray matrix/TRC profile 和已准入的 LUT 形式；profile 定义的 XYZ/Lab 端点需要显式 LUT，CMYK 端点路径需要其声明的 LUT 集。Abstract 与 DeviceLink profile 不是端点资源。这种结构准入不运行色彩管理模块。`resource()` 返回供调用方放入 `ResourceSet` 的 owning `ImmutableResource`。

`OcioConfigResource`（`photospider/ops/ocio_config_resource.hpp`）封装显式快照：配置字节、完整排序的逻辑文件表、已解析 context、声明的 canonical space/reference，以及固定的 engine/build/settings identity。其 SHA-256 identity 与字节长度覆盖成帧后的字节，包括查找缺失。准入校验快照结构和所有权，不校验 transform 能否执行。冻结后查找不访问文件、网络或进程环境。快照复制和比较按最多 1024 字节的块消耗 `ResourceBudget` 并检查取消/工作量；file/context/space 表合计最多 1024 项。

## 5. 限制与非目标

- 描述是静态算子配置。它不附着在 Result 或 Value 上，不沿图的边流动，任何算子输出都不会继承输入的描述。
- 内核不会因两个 Result 的预期描述不同而拒绝连线；端口兼容只由结构决定。算子在 preparation 时把显式描述与输入 descriptor 比对，绑定的校验器可以检查样本值。
- Tensor 描述说明逻辑轴和解释，不定义物理 stride、planar/tiled 存储或图像所有权。
- Profile identity 不携带资源字节。调用方必须在传给 compiler 的 `ResourceSet` 中提供资源。
- Profile 或 configured-space 声明不会推断 analytic primaries、transfer 或 white。Analytic binding 仍是调用方来源声明。
- 描述校验不认证数值样本范围、有限性、覆盖或颜色正确性。
- ICC 准入是结构性的；OCIO 准入校验冻结快照而不执行 transform。不提供 ICC 或 OCIO 颜色转换引擎。
- Color-group 与 alpha 索引指 channel axis 上的位置，axis role 指逻辑维度；二者都不选择字节顺序。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md) D9（Proposed）记录规范图像目标。

格式错误的描述和编辑由 SDK 调用返回 `InvalidArgument`。格式算子在静态 preparation 中解码并校验其 `input_description` 参数，因此参数格式错误或描述与输入 descriptor 不匹配时编译失败。ICC 与 OCIO 资源准入向导入调用提供的 `ResourceBudget` 计费；资源准入、复制或哈希失败时不发布任何 owner。
