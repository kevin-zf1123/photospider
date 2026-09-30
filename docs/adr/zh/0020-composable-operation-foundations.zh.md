# ADR 0020：描述类型化 Value 并推断算子输出

- 状态：Accepted
- 英文权威文档：[ADR 0020](../0020-composable-operation-foundations.md)

## 1. 核心摘要（TL;DR）

组合 workflow 中的算子既要计算样本，也要保留、转换或移除样本的语义。Compiler 与 operation registry 共享声明式输入约束、输出 shape 和 dtype 推断、语义规则及命名输出契约。只有静态元数据和参数成功解析后，callback 才会运行。

## 2. 架构心智模型（Mental Model & Intuition）

`ValueDescriptor` 描述样本数量和物理标量类型。`ValueFacet` 添加图像通道、颜色模型等解释。Compiler 在执行前传播这些描述，callback 则按解析后的契约计算样本。

```text
有序输入 + 静态参数
             |
      registry 专化
             |
     校验约束并推断
     dtype / shape / facets
             |
       SemanticGraphIR 输出
             |
         请求的结果
             |
  每个被需求的结果对应一个 PlanStep
             |
 callback -> 校验输出 -> 发布
```

C++ registry 拥有复制后的 traits 和不可变 preparation。C plugin 提供由 descriptor 拥有的记录；主机在 registry 发布前校验并复制这些记录，并在 callback 可能运行期间持有模块租约。`Value` 发布后其存储保持不可变。semantic descriptor 是元数据，不代表样本一定满足该语义；契约要求时由 operation 校验样本。

## 3. 契约规约与接口（Formal Contracts & APIs）

```cpp
enum class ElementType : std::uint32_t {
  UInt8 = 1, Int64 = 2, Float64 = 3, Float32 = 4,
  Int8 = 5, UInt16 = 6, Int16 = 7
};

struct ValueDescriptor {
  ElementType element_type;
  std::vector<std::uint64_t> shape;  // rank 1..8；每个 extent 大于零
};

struct ValueFacet {
  std::string key;
  std::uint32_t version;
  std::vector<std::uint8_t> payload;
};

struct OperationOutputTraits {
  std::string key = "value";
  OperationShapeRule shape_rule = OperationShapeRule::Scalar;
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  OperationDtypeRule output_dtype_rule = OperationDtypeRule::Declared;
  OperationSemanticRule output_semantic_rule = OperationSemanticRule::Drop;
  std::vector<OperationExtent> output_axes;
  std::optional<std::vector<std::uint32_t>> input_indices;
};

struct OperationTraits {
  bool deterministic = true;
  bool side_effect_free = true;
  bool supports_cpu = true;
  bool supports_gpu = false;
  bool allows_cpu_fallback = false;
  bool cacheable = true;
  std::vector<OperationPortConstraint> input_schema;
  std::vector<OperationOutputTraits> outputs;
};
```

以上为契约摘录，不是完整构造代码。当前 C++ `OperationTraits` 记录版本为 20。Package 版本为 0.28.0，C operation plugin 使用 ABI 11，独立版本化的 planar operation extension 使用 ABI 3。Plugin descriptor 最多声明 64 个命名输出；输出名称唯一，并映射到声明顺序的索引。单输出 operation 明确使用 `value`。

`ValueDescriptor` 的 rank 为 1 到 8，extent 均非零。元素类型与语义解释及内存布局相互独立。`Value` 可以是 strided；shape 推断描述逻辑样本，不意味着内存连续。样本位是否合法由 operation 或类型化语义契约负责，不由通用 Value 容器统一限制。输入约束可指定精确 dtype 或允许的 dtype mask、rank、semantic kind/facets，或有限标量区间。Registry 会在发布前拒绝互相矛盾或未知的约束字段。

`SemanticDescriptor` 是 C++ 所有权类型，经公开 `encode_semantic` helper 编码。图像描述使用 facet `photospider.image` 版本 2；其他类型化语义使用 `photospider.semantic` 版本 1。Generic Value 可以不带语义 facet。解码器只接受当前规范格式，不转换旧 image facet。规范元数据最多 4096 编码字节，每个文本字段最多 128 UTF-8 字节，通道最多 64 个。未使用字段为空或正零；元数据数值必须有限。`semantic_parameter` helper 返回小写十六进制，最多 8192 字符。

对于 `Image` `SemanticDescriptor`，Value 的逻辑 shape 为 Float32 HWC。首个 image profile 是 linear-sRGB/Rec.709、D65 白点和相对 scene-referred 值；有限 signed 和 HDR RGB 值有效。RGB、XYZ 和 Lab 使用显式通道角色、单位和参考白点。若包含 alpha，它是最后一个 coverage 通道，样本范围为 [0,1]。Straight alpha 可在 alpha 为零时保留隐藏颜色；coverage-premultiplied RGB 在 alpha 为零时也必须为零。Unassociation 对每个正 alpha 直接相除，不加 epsilon；alpha 为零时 RGB 输出为零。alpha 为零时执行 association 会丢失隐藏的 straight 颜色。非线性色彩转换要求颜色未关联，使用声明的白点，且不会隐式执行色适应或色域裁切。逻辑 HWC 轴不规定存储必须交错还是 planar。

其他语义类型描述 mask、scalar/vector/complex field、sampled signal、LUT、byte resource 和 image plane。Mask 区分 coverage、probability 和 membership。Vector field 声明坐标空间和方向。Complex field 声明实部/虚部以及未移位频率约定。`ImagePlane` 保存一个 Float32 HW 平面，并显式声明名义采样原点及正的 Y/X 步长；该逻辑采样描述本身不能证明依赖支持或物理存储布局。

Operation 输出分别声明 dtype 和 shape。Registry 支持 scalar、沿用首输入、匹配所有输入、固定 shape、shrink 及静态轴表达式规则。Dtype 规则从声明类型、某个输入类型或允许的静态参数中选择。轴表达式可从常量、静态参数、输入轴/数量、规范通道索引列表长度或有界有限 Float64 参数的向上取整结果中解析，并可执行经检查的减法、ceil 除法、正乘数和非负常量偏移。Compiler 在任何 value callback 运行前解析 descriptor，并按推断得到的 dtype、shape 和 facets 校验发布结果。运行时样本不会决定输出 shape。

输出语义规则明确保留输入 facet、建立 facets、读取静态语义参数，或移除类型保证。共享规则也描述通道提取/选择/合并、alpha association 变换和支持的颜色模型转换。Generic 中间结果不会仅凭 shape 恢复已丢失的语义；后续 merge 或 metadata assignment 必须声明目标含义并校验约束。规范通道索引参数使用 1..64 个十进制索引，取值 0 到 63，以逗号分隔，不含空格、正负号或前导零。调用方使用 helper API 构造和解析这些值，不手写语义 payload 的十六进制。

每个 operation 输出有自己的有序输入投影。Compiler 使用完整声明签名推断元数据，然后只将所选输出相关输入降低到可执行祖先中。重复输入模板由固定前缀和一个同构尾部模板组成；解析后的调用展开为有序表，输入总数不超过 1024。Dtype、shape 推断、静态参数和重复数量边界均进入语义身份。

`Float32Scalar` 端口接受完整 Float32 `{1}`，facet 可以缺省，也可以是无量纲 `Scalar` 或无量纲单样本 `SampledSignal`。每个消费 callback 执行前都会检查其有限闭区间范围，包括值来自结果缓存时。直接绑定的无效样本返回 `InvalidArgument`；计算得出的无效值返回 `OperationFailed`；不兼容的 facet 返回 `TypeMismatch`。

公开 expression 规则接受 Generic Float64 系数 `[K]`，其中 `1 <= K <= 256`，start 有限、step 为正且有限，推断出的 Float32 输出长度为 1 至 1,048,576。输出是值单位和采样轴单位均为 dimensionless 的 sampled signal。Host parser 将源文本限制为 4096 UTF-8 字节，表达式树限制为 256 个节点、深度最多 32。它接受十进制/科学计数数字、`x`、`c[index]`、括号、一元 `+`/`-`、二元 `+`、`-`、`*`、`/`、`^`，以及纯函数 `abs`、`sqrt`、`exp`、`log`、`sin`、`cos`、`min`、`max`。`lut.apply_1d` 接受 Float32 sampled-signal 查询和至少有两个均匀采样项的 Float32 单通道 Signal/LUT 表。查询样本单位必须与表轴单位匹配；越界行为显式选择 `reject` 或 `clip`。两种 operation 都使用 Whole Region 语义。

数值精度和转换行为由各 operation profile 定义。通用 Value 容器保留有效标量位，包括浮点 signed zero，以及其物理类型允许的非有限模式。Operation 自行增加有限值/范围约束、整数舍入、溢出处理和归约顺序。单个 operation 的 oracle 不构成整张图的统一误差容限或位级一致性承诺。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- 仅 shape 相同不能证明数据具有 image、mask、field、signal 或 LUT 语义。
- 静态推断不会检查运行时样本来选择 dtype、rank 或 shape。
- Compiler 不插入隐式 cast、broadcast、gamma 运算、epsilon、色适应或色域裁切。
- Generic arithmetic 不保留类型化语义保证，除非声明的语义规则能够证明结果含义。
- `Region` 规则定义数据依赖，不定义 worker 分片或内部计算 tile。
- 本决策不增加脚本运行时、任意表达式语言、operation 文件系统访问、每次运行动态输出长度或隐式可变状态。
- 本文定义可复用契约，不保证所有构建或 backend 都注册并支持每个已描述的 operation 家族。

## 5. 后果与代价（Consequences）

Schema 错误、不支持的语义组合、无效静态参数和无法表示的 checked extent 都由 compiler 在 callback 运行前拒绝。运行时输出若与推断出的 descriptor 或 facet 契约不一致，则发布失败。调用方应使用 semantic 与通道列表 helper，确保静态参数采用规范格式。

Typed image 的每个像素都必须覆盖完整逻辑通道，即使空间 Region 只有部分区域。RGBA 的 H/W tile 对每个像素仍保留四个通道样本；planar 存储可将通道放在不同平面，因此逻辑通道完整不意味着物理字节交错连续。Generic 和其他类型数组遵循各自的 Region 契约。

Semantic descriptor 和静态推断记录会占用元数据并进入 compiler/result identity。改变通道角色、dtype 规则、输出 shape 规则或相关输入投影都会改变这些身份。计算代价及输出/scratch 分配仍由 callback 承担；长循环应检查取消状态并遵守声明的资源上限。缓存资格还要求实现确定、无副作用且依赖已证明。
