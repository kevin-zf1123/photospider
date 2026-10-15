# ADR 0020：描述类型化 Value 并推断算子输出

- 状态：Accepted
- 英文权威文档：[ADR 0020](../0020-composable-operation-foundations.md)
- 当前边界：原决策中的语义 facet、类型化语义 descriptor 和输出语义规则已被取代。Result 只携带结构，领域解释是各 operation 的显式静态参数。参见 [数据模型](../../kernel-architecture/zh/Data-Model.zh.md)、[结构化 Result 与 tensor slot](../../kernel-architecture/zh/Global-Results.zh.md) 和 [Tensor 描述](../../kernel-architecture/zh/Tensor-Semantic-Metadata.zh.md)。

## 1. 核心摘要（TL;DR）

组合 workflow 要求每个 operation 输出在执行前即可确定。Compiler 与 operation registry 共享声明式输入约束、Result schema 推断、显式绑定的 validator 及命名输出契约。只有静态元数据和参数成功解析后，callback 才会运行。

## 2. 架构心智模型（Mental Model & Intuition）

`ValueDescriptor` 描述样本数量和物理标量类型。Result schema 组织类型化 tensor slot 和打包 field；原子尾轴与空间布局描述结构完整性和存储方式。通道角色、颜色模型、采样网格等解释是读取它的 operation 的静态参数，输出不会从输入继承这些解释。Compiler 在执行前解析 schema，callback 则按解析后的契约计算样本。

```text
有序输入 + 静态参数
             |
      registry 专化
             |
     校验约束并推断
  Result schema (dtype / shape / slots)
             |
       SemanticGraphIR 输出
             |
         请求的结果
             |
  每个被需求的结果对应一个 PlanStep
             |
 callback -> 校验输出 -> 发布
```

C++ registry 拥有复制后的 traits 和不可变 preparation。C plugin 提供由 descriptor 拥有的记录；主机在 registry 发布前校验并复制这些记录，并在 callback 可能运行期间持有模块租约。Result 和 `Value` 发布后其存储保持不可变。Result schema 只陈述结构，不代表样本值满足某个领域契约；operation 需要时由显式绑定的 validator 校验样本。

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

struct OperationPortConstraint {
  std::vector<ExtensionIdentity> validators;
  OperationPortKind kind = OperationPortKind::Result;
  float minimum = 0.0F;
  float maximum = 0.0F;
  std::uint32_t element_type = 0;
  std::uint32_t rank = 0;
  std::uint32_t element_type_mask = 0;
  std::string result_schema_id;
  std::uint32_t result_schema_version = 0;
  std::string tensor_key;
  bool scalar_bounds = false;
};

struct OperationOutputTraits {
  std::string key = "value";
  std::optional<std::vector<std::uint32_t>> input_indices;
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  OperationPortConstraint output_schema;
  std::optional<SchemaTemplate> result_schema;
};

struct OperationTraits {
  bool deterministic = true;
  bool side_effect_free = true;
  bool supports_cpu = true;
  bool supports_gpu = false;
  bool allows_cpu_fallback = false;
  std::uint32_t version = 26U;
  bool cacheable = true;
  std::vector<OperationParameterSpec> parameter_schema;
  std::vector<OperationPortConstraint> input_schema;
  std::vector<OperationOutputTraits> outputs;
};
```

以上为契约摘录，不是完整构造代码，省略了其他成员。Operation plugin 使用 Result C operation ABI 4。一个 operation 最多声明 64 个命名输出；输出名称唯一，并映射到声明顺序的索引。

`ValueDescriptor` 的 rank 为 1 到 8，extent 均非零。元素类型与解释及内存布局相互独立。`Value` 可以是 strided；shape 推断描述逻辑样本，不意味着内存连续。样本位是否合法由 operation 或其绑定的 validator 负责，不由通用 Value 容器统一限制。输入约束可指定精确 dtype 或允许的 dtype mask、rank、固定的 Result schema id 与版本、命名 tensor 成员、显式 validator，或有限 Float32 标量区间。Registry 会在发布前拒绝互相矛盾或未知的约束字段。

Result schema 的 id 和版本命名一个显式结构契约，并进入编译身份；kernel 不根据它们分派到某种领域解释。Tensor slot 声明 cell descriptor、batch 前缀、`atomic_trailing_axes` 和可选的空间 planar 布局。结构相同的两个 Result 可以连接到同一端口，即使调用方赋予它们不同含义。需要通道角色、颜色模型、transfer、白点、alpha association、采样或 LUT 轴的 operation，从显式静态参数读取这些信息，通常是由 operations SDK（`Photospider::ops_sdk`）编码的 `TensorDescription`。Operation 在 preparation 阶段按输入 descriptor 校验该参数，且不会把它复制到输出上。

端口约束通过已注册的 `ExtensionIdentity` 绑定 validator；validator 顺序有意义并进入身份。Prepared operation 在主机授予的数据上只运行这些 validator。Kernel 不会根据 schema id、shape 或通道数选择 validator。现有领域数据契约属于 operations SDK：`photospider/ops/numeric/lut3d_bake.hpp` 中的 LUT3D bake report 与 owned-table Result schema（`lut3d_bake_schema` 和 `lut3d_bake_table_schema`），以及 `photospider/ops/numeric/quality.hpp` 中的 `QualityReport` 类型。`photospider/ops/numeric/diagnostics.hpp` 中的 `NumericDiagnostics` 是通用 diagnostic report 通道的 payload codec，由 `register_numeric_diagnostics` 以 `numeric_diagnostic_identity()` 注册。`photospider/ops/composite/layer.hpp` 中的 Layer 类型是带校验和算术函数的纯值类型，不定义 Result schema。

每个 operation 输出都解析为完整的 Result schema。固定输出携带已注册的 `result_schema` 原型，其 extent 可以是固定值、来自某个输入轴或元素数量、绑定到 field 行数，或在发布时为某个 field 计数。Traits 设置 `requires_metadata_specialization` 的 operation 通过纯 specializer 或 `prepare_static` callback 逐节点解析输出，这些 callback 只读取静态输入元数据和参数。静态 preparation 同时列出其程序所需的 `ResourceIdentity`。Compiler 在任何 value callback 运行前解析 schema，并按 schema 校验发布结果。运行时样本不会决定 dtype、rank 或 tensor shape；只有声明为运行时行数的 field 在发布时获得该行数。

每个 operation 输出有自己的有序输入投影。Compiler 使用完整声明签名推断元数据，然后只将所选输出相关输入降低到可执行祖先中。重复输入模板由固定前缀和一个同构尾部模板组成；解析后的调用展开为有序表，输入总数不超过 1024。解析后的 Result schema、静态参数和重复数量边界均进入语义身份。

设置 `scalar_bounds` 的端口接受完整的 Float32 tensor 成员，其完整样本 shape 为 `{1}` 且没有 batch 轴；其他 shape 返回 `TypeMismatch`。每个消费 callback 执行前都会检查其有限闭区间 `[minimum, maximum]`，包括值来自结果缓存时。计算得出的越界值返回 `OperationFailed`。

`numeric.sample_expression` 接受 Generic Float64 系数 `[K]`，其中 `1 <= K <= 256`，`start` 有限、`step` 为正且有限，推断出的 Float32 输出长度为 1 至 1,048,576。Host parser 将源文本限制为 4096 UTF-8 字节，表达式树限制为 256 个节点、深度最多 32。它接受十进制/科学计数数字、`x`、`c[index]`、括号、一元 `+`/`-`、二元 `+`、`-`、`*`、`/`、`^`，以及纯函数 `abs`、`sqrt`、`exp`、`log`、`sin`、`cos`、`min`、`max`。`lut.apply_1d` 接受 Float32 查询和至少两项的 rank-1 Float32 表，输出 shape 与查询相同。表的采样网格来自静态参数 `sample_origin` 和 `sample_step`，越界行为来自 `out_of_domain`，取值为 `reject` 或 `clip`。两种 operation 都使用 Whole Region 语义。

数值精度和转换行为由各 operation profile 定义。通用 Value 容器保留有效标量位，包括浮点 signed zero，以及其物理类型允许的非有限模式。Operation 自行增加有限值/范围约束、整数舍入、溢出处理和归约顺序。单个 operation 的 oracle 不构成整张图的统一误差容限或位级一致性承诺。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- Result 结构不携带也不证明 image、mask、field、signal 或 LUT 语义。
- Operation 输出不继承输入的领域描述。Kernel 不附加、传播、匹配或自动识别领域标签。
- 静态推断不会检查运行时样本来选择 dtype、rank 或 tensor shape。
- Compiler 不插入隐式 cast、broadcast、gamma 运算、epsilon、色适应或色域裁切。
- `Region` 规则定义数据依赖，不定义 worker 分片或内部计算 tile。
- 本决策不增加脚本运行时、任意表达式语言、operation 文件系统访问、每次运行动态输出长度或隐式可变状态。
- 本文定义可复用契约，不保证所有构建或 backend 都注册并支持每个已描述的 operation 家族。

## 5. 后果与代价（Consequences）

Schema 错误、无效静态参数、缺失的请求资源和无法表示的 checked extent 都由 compiler 在 callback 运行前拒绝。运行时输出若与推断出的 Result schema 不一致，则发布失败。调用方应使用 operations SDK 的 codec 构造描述参数，确保静态参数采用规范格式。

带原子通道轴的 tensor slot 对每个像素都获得完整逻辑通道覆盖，即使空间 Region 只有部分区域。RGBA 的 H/W tile 对每个像素仍保留四个通道样本；planar 存储可将通道放在不同平面，因此逻辑通道完整不意味着物理字节交错连续。没有原子尾轴的 slot 严格按样本坐标读取。

解析后的 Result schema 和静态推断记录会占用元数据并进入 compiler/result identity。改变 dtype、输出 schema、静态描述参数或相关输入投影都会改变这些身份。计算代价及输出/scratch 分配仍由 callback 承担；长循环应检查取消状态并遵守声明的资源上限。缓存资格还要求实现确定、无副作用且依赖已证明。
