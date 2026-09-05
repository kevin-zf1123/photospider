# ADR 0016：工作流输入与每次运行的图像绑定

- 状态：Accepted，目标契约已接受，尚未实现
- 日期：2026-09-05
- 已接受范围：维护者在当前任务明确接受调整后的开发方向、Float32 图像输入和普通运行期参数
- 接受记录：2026-09-05，维护者针对 operation ABI v3 及配套契约的明确确认问题回复“是，继续。”
- 任务：[kernel #256](https://github.com/kevin-zf1123/photospider/issues/256)
- 已核验源码：`main@369da60bdcf7aa26eefbd7a99f7e5d1a8afd79e8`
- 实现状态：尚未实现，由 #257 和 #258 处理
- 英文权威原稿：[英文](../0016-workflow-inputs-and-execution-bindings.md)

维护者已接受[调整后的方向](../../development/zh/Refactor-Development-Plan.zh.md)。
本次以统一的 Float32 图像与普通标量契约替换此前仅含 UInt8 的候选稿，无需再次
确认方向。具体 ABI 及配套契约已经维护者明确接受；本文件不改变
已安装头文件、运行版本或 GitHub 状态。

ADR 0015 继续规定产品所有权。本 ADR 仅替代 ADR 0015/0012/0008 中
operation v2 与最小元素词汇的对应条款，并按下文扩展 ADR 0014 的身份规则。
它不授权 S2 部分存储、S3 缓存或恢复、原生 GPU 驻留或 daemon 协议工作。
当前实际基线保持 C++17，C++20 作为独立任务评估。

## 背景与选定方案

现有 WorkflowDocument 边只能引用节点，execute 没有运行期绑定。Value 仅有
UInt8/Int64/Float64，而 Elementwise/Halo 要求所有输入形状与输出相同。
图像与秩为1的普通参数共同参与计算，需要明确的端口语义。现有 operation ABI
v2 严格检查 C 结构大小，因此不能原地增加描述符字段。

| 事项 | 修订目标 | 依据与未采用方案 |
| --- | --- | --- |
| 动态数据 | 图像和有界标量 Value 放入同一不可变 ExecutionBindings 快照。 | 图像、增益和不透明度改变时复用计划，不将其转为源文档常量。 |
| 端口契约 | 复制到 traits 的端口类型、标量区间和图像需求验证。 | 全部采用 Whole 会丢失现有请求 Region 的有效信息；隐式广播所有标量会改变未标记算子。 |
| 插件 ABI | 单一 operation ABI v3，为 C++ 和已安装 C 插件提供端口 schema。 | v2 中只加 Float32 能力位无法公布完整端口约束；仅由 C++ 验证会遗漏 #258 的插件契约。不保留 v2 适配器。 |
| 存储 | S1 固定形状、连续且自持有的 CPU 字节、完整物化。 | 部分或设备 Storage 后续单独决策。 |
| 输出 | 名称固定于文档，Region 在规划时选择。 | 改变需求时对优化后 IR 重新规划，执行时不修改计划。 |
| 图像约定 | 线性 sRGB、预乘 alpha 的 RGBA Float32，明确数值和 facet 规则。 | S1 采用有限图像约定，不引入颜色管理或文件编解码产品。 |

## 精确的提议 source 与执行 API

以下是在命名空间 `ps` 中的替换或新增声明，使用现有的 `ValueDescriptor`、`Region`、`StridedLayout`、`ValueFacet`、`Value`、`CancellationToken`、`ExecutionOptions` 和 `Result`。公开声明在 #257 中获得现有的 `PHOTOSPIDER_API` 导出和完整 Doxygen。

```cpp
struct WorkflowNodeOutput final {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};
struct WorkflowInputReference final {
  std::uint64_t input_id = 0;
};
using WorkflowInput =
    std::variant<WorkflowNodeOutput, WorkflowInputReference>;

struct WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  ValueDescriptor descriptor;
  Region region;
  StridedLayout layout;
  std::vector<ValueFacet> facets;
};
struct WorkflowDocument final {
  std::uint32_t schema_version = 2;
  std::vector<WorkflowInputDeclaration> inputs;
  std::vector<WorkflowNode> nodes;
  std::vector<WorkflowOutput> outputs;
};
struct ExecutionBinding final {
  std::string name;
  Value value;
};
struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};

// Replaces the existing member declaration of ExecutionContext.
Result<ExecutionResult> execute(
    const ExecutionPlan& plan, ExecutionBindings bindings = {},
    const CancellationToken& cancellation = CancellationToken(),
    const ExecutionOptions& options = {});
```

`WorkflowNode.inputs` 保留其 vector 名称和位置语义，并使用新的带标签 `WorkflowInput`。不存在外部输入的零 id sentinel、虚构的 operation key 或旧结构兼容别名。节点 id 和 input id 使用独立命名空间；variant 标签消除相同数字的歧义。`WorkflowOutput` 继续引用 operation 节点的 `value` port。不增加不经过 operation 的直接 input 到 output 透传。

声明按非零 id 和名称唯一。名称为精确、区分大小写的 1..128 字节可打印 ASCII 字符串，不含空格（字节 0x21..0x7e），不进行 Unicode 规范化。声明数量为 0..4096。每个声明必须恰好绑定一次，包括未被节点引用的声明。声明顺序和绑定顺序没有语义；规范声明顺序为 id。现有节点、边、参数和输出限制继续适用。

绑定 vector 包含 0..4096 条记录，并保留重复绑定以便验证。静默替换较早值的 map 不能作为公开验证边界。回调开始前，bindings 按精确名称解析为声明 id。不存在可选 bindings、默认值、隐式转换、resize、布局重打包、facet 强制转换或隐藏文件加载。

### 输入 descriptor、Region、layout 和 facets

descriptor 固定 UInt8、Int64、Float64 或 Float32 中的一种元素类型以及非零的 rank-1..8 形状。声明 Region 必须等于 `Region::whole(shape)`。layout 必须具有字节偏移零和规范的正行主序 strides：最后一个轴的 stride 是元素宽度，每个前置轴的 stride 将下一个 stride 乘以下一个轴 extent。singleton axes 使用相同的规范 stride 规则。所有乘积、有符号 stride、寻址尾部和 host-size 转换都要检查；有效的 dense 字节数 B 必须满足 `B > 0`、`B - 1 <= INT64_MAX` 和 `B <= SIZE_MAX`。验证声明时不分配 payload 缓冲区；元数据规范化可能分配内存。

绑定的 Value 必须有效，并匹配精确 descriptor、完整 Region、零偏移和规范 strides；`bytes().size()` 必须等于 B。即使负或零 stride、前缀或尾部 padding、部分或空覆盖以及不同逻辑形状在其他位置构成有效通用 Value，也必须在这个 binding 边界拒绝。通用 `Value::create` 行为保持不变。

facets 是按 key 规范化的封闭精确集合。复用 Value 的现有限制：最多 64 条记录，唯一的 1..256 字节可打印 ASCII key，正版本，每个 facet 最多 64 KiB payload 字节，聚合 payload 最多 1 MiB。key、version 和 payload 字节都必须匹配。缺少或额外 facets、版本变化和 payload 变化都会失败；空声明集合要求空 Value 集合。facets 携带编译时语义，不提供每次运行的扩展字段。

### 编译时事实与每次运行数据

影响形状、halo 或特化的 `WorkflowNode.parameters` 保留其当前 variant 和 Float64 位规则。普通增益和不透明度使用运行期标量 Value 输入，不放入该参数表。operation 生成的 source Values 保留现有 source-operation 模型。本变更不增加编译时任意 Value literal 或常量/动态模式切换。

声明只包含元数据；图、IR 和 plans 永远不保留 binding Value、数据地址或动态 payload。`ExecutionBindings` 中复制的 `Value` 只属于其执行。替换 payload 字节需要新的不可变 Value 和新的 binding snapshot，不需要替换图或重新编译未改变的 plan。

## 精确端口与图像契约

增加 `ElementType::Float32 = 4`，宽度为4字节，采用 IEEE-754 binary32；值1至3
的含义保持不变。通用 Value 仍做结构验证，在普通缓冲区规则下允许有限或
非有限 Float32 位模式。不要求新增标量访问器：通过 Value::create 构造，使用
memcpy 复制标量字节，不能使用可能未对齐的 float 指针。

```cpp
enum class OperationPortKind : std::uint32_t {
  Value = 1,
  Float32Scalar = 2,
  LinearPremultipliedRgbaFloat32 = 3,
};
struct OperationPortConstraint final {
  OperationPortKind kind = OperationPortKind::Value;
  float minimum = 0.0F;
  float maximum = 0.0F;
};
// OperationTraits additions; version becomes 3.
std::vector<OperationPortConstraint> input_schema;
OperationPortConstraint output_schema;
```

`input_schema.size()` 必须等于 input_count，最多1024项；无输入算子使用空 schema。
Value 端口保持现有形状和 Region 行为。Float32Scalar 端口只能直接引用工作流
输入，要求精确 Float32 shape {1}、完整 Region、偏移0、stride {4}、4字节且无
facet。引用计算产生的标量在分析阶段返回 InvalidArgument。标量区间为有限
binary32 端点组成的闭区间，minimum <= maximum；非标量约束的边界必须为正零
位模式。image 输出必须声明 output_element_type=Float32；使用 Elementwise/Halo 的图像输入要求对应图像输出种类，Whole 可用于统计等通用输出。输出类型仅允许 Value 或 LinearPremultipliedRgbaFloat32，S1 不接受
标量输出种类。MatchAllInputs 不得含 Float32Scalar 端口；PreserveFirstInput
的首端口必须为非标量。未知种类、数量错误或非法组合在注册发布前失败。

图像端口要求 Float32 shape {H,W,4}，H/W 为编译期固定正数，并且恰有一个
facet：key 为 `photospider.image`，version 为1，payload 为不含 NUL 的 ASCII
`rgba;linear-srgb;premultiplied;hwc`，长度34字节。通道顺序为 R,G,B,A，轴顺序
H,W,C，按行主序存储。RGB 有限且非负，alpha 有限并处于 [0,1]；alpha 为零时
RGB 必须为零。HDR RGB 可大于1或 alpha。允许有符号数值零，不归一化像素值。
RGB 已在线性 sRGB 中，不做 gamma 转换、颜色转换、反预乘或隐式裁剪。

LinearPremultipliedRgbaFloat32 输出要求 PreserveFirstInput 和图像首输入，
保证相同图像约定、类型和形状。分析阶段传播声明的图像约定，拒绝无对应输出
保证的通用生产者进入图像端口。宿主在调用前重新验证图像输入，在发布前验证
图像输出。所有绑定图像的像素内容与直接标量的值域，在 Run 完整预检中验证，
首个回调之前完成；后续计算产生的非法图像属于回调契约失败。通用 Value
端口不自动获得图像语义。

| 算子 | 有序端口 | 输出与计算 |
| --- | --- | --- |
| `image.exposure_gain` | 图像；区间 [0,16] 内的 Float32Scalar 增益 | 图像；RGB 乘以增益，复制 alpha。增益为无单位直接乘数，不采用曝光档数。 |
| `image.opacity` | 图像；区间 [0,1] 内的 Float32Scalar 不透明度 | 图像；全部四个通道乘以不透明度。 |

两个算子均有两个输入、空源文档参数 schema、PreserveFirstInput、Elementwise，
CPU 路径确定、无副作用且可缓存，S1 不声明 GPU 候选。每次乘法在下一个算子
之前舍入为 binary32，采用最近值、平局取偶数；不允许融合计算、fast-math 或
flush-to-zero 改变该结果。alpha 复制保留位模式。非有限计算输出返回
OperationFailed，不发布结果。一般测试对独立逐步舍入参考的每个有限通道要求
`abs(actual-ref) <= 1e-7 + 2e-6*abs(ref)`；下文二进制分数示例要求有限值逐位
精确。两个算子显式发布该 profile facet，不依赖 PreserveFirstInput 自动传播。

标量字节属于普通运行数据。形状、halo 半径、特化和源 ParameterValue 仍属
编译期事实。一个声明可供多个标量端口使用；预检验证所有消费区间，即其交集，
交集为空时分析返回 InvalidArgument。同一输入不能同时满足冲突的图像和标量
描述符。源事实变化需重新分析和优化，单独改变像素或标量字节不需要。

## 阶段 lowering 与标识

每个阶段发布复制的规范 `input_declarations()` vector。公开的 `SemanticNode.inputs` 变为 `std::vector<WorkflowInput>`，带标签的 producer references 按 input-port 顺序排列。

物理 plan 将 `PlanStep.input_steps` 替换为以下带标签的 index vector，使外部 input 不会被误认为 dependency task：

```cpp
struct PlanStepInput final { std::size_t step_index = 0; };
struct PlanWorkflowInput final { std::size_t declaration_index = 0; };
using PlanInput = std::variant<PlanStepInput, PlanWorkflowInput>;
// PlanStep member replacing input_steps:
std::vector<PlanInput> inputs;
// New const member on SemanticGraphIR, OptimizedGraphIR and ExecutionPlan:
const std::vector<WorkflowInputDeclaration>& input_declarations() const noexcept;
```

step references 必须指向更早的 plan steps；声明 indexes 必须在边界内。只有 step references 会计入 ready-task dependency counts。外部 descriptors 参与 Scalar/Fixed/Preserve/Match 验证，但不运行 input callback。现有 graph/registry currentness 检查仍然强制执行，包括 stage digests 匹配时。

| 标识 | 包含的 input facts | 排除的 run facts |
| --- | --- | --- |
| SemanticGraphDigest | Schema domain；按 id 排序的 declarations：id、精确名称、element enum、shape、Region intervals、字节偏移/strides、排序后的 facet key/version/payload；带顺序的 tagged node sources 以及现有 parameter/trait/output facts。 | Binding container 顺序、动态 payload 字节、内存地址、Value storage owners。 |
| OptimizedGraphDigest | Parent semantic digest、optimizer identity、规范化 declaration table 和优化后的 tagged topology。 | 所有每次运行的 bindings 和 scheduling observations。 |
| ExecutionPlanDigest | Parent optimized digest、规范 declarations、带标签的 step/declaration indexes、选定 backends、建模 step bytes 以及每个 output/input demand。 | Payload、runtime allocation、cancellation、timings、transfer observations、daemon ids。 |
| PlanCacheKey | 新 cache domain 和完整 physical plan digest。 | Binding data；没有由 payload 派生的 plan specialization。 |

所有 counts、ids、ranks、extents、enums、versions 和 indexes 都编码为经过检查的 uint64 little-endian 字段；有符号 strides 编码为其 uint64 two's-complement 表示。字符串和 facet payload 使用带长度前缀的原始字节。Source tags 对 node/step 编码为 1，对 declaration 编码为 2。现有 parameter encoding、node topological ordering、named-output ordering 和 trait fields 仍按 ADR 0014 定义。不对原始 C++ object padding 进行 hash。

使用 domains `semantic-graph-ir-v3`、`optimizer-v3-canonical-noop`、`physical-plan-v3` 和 `plan-cache-key-v3`。仅重新排序 declarations/bindings 会保持标识；重命名 input、改变其 id/constraint、重新连接 tagged input 或改变编译时 scalar 会改变相应阶段标识。无效 constraint 在发布 digest 前失败。最终规范化的每步骤 demand 变化保持 semantic/optimized 标识不变，但改变 plan/cache 标识。多个名称选择同一节点时，先合并需求再编码身份；另一个名称已要求完整节点时，仅修改某个子 Region 请求可能保持规范化 plan 及身份不变。不承诺每次请求文本修改都改变物理身份。cache hit 仍需要完整验证；过期或不匹配的条目会丢弃并重新构建。不存在仅以可复用 plan 为 key 的运行时 result cache。


端口 schema、output_schema、种类和区间都是复制到 traits 的编译期事实，按
输入顺序参与每级身份；端点按精确 binary32 位模式零扩展为 uint64 编码。
图像 facet 与标量声明属于静态事实；图像和标量的运行字节均排除。只改变这些
运行字节或绑定顺序，不改变各级身份。结果摘要仅描述输出，改变参数后输出
可能相同，因此摘要也可相同；未来结果缓存键另行决定。

## Operation ABI v3 与逐端口需求

Value 端口完整保留现有 Whole/Elementwise/Halo 规则。Float32Scalar 端口始终
要求完整 {1}，不受算子 Region 规则或 halo 半径影响。图像端口在 H/W 上应用
Whole 或 Elementwise；Halo 只在 H/W 上扩展与裁剪。图像需求的通道轴必须为
{offset=0, extent=4}，部分通道请求在规划时返回 InvalidArgument。需求传播前
验证秩、类型和图像约定。即使空间需求较小，仍产生完整 Value；空输出需求
返回 InvalidArgument。输出名称仍由 WorkflowDocument 选择，唯一需求入口为
PlanningOptions.output_regions。请求变化重新规划优化后的 IR，不全量重编语义图。

operation C ABI 从 v2 单向破坏性迁移至 v3。所有 operation 所属 `_v2`/`_V2`
类型、常量和 get_api 入口机械改为 `_v3`/`_V3`；除下列新增字段外，保持既有
字段顺序、宽度、回调、结果码和参数语义。现有无版本标记值及
`ps_operation_plugin_get_abi_version` 名称不变。在 ps_operation_element_type_v3 中
增加 Float32 值4。不保留 v2 别名、读取器或适配器。新宿主在查询 get_api_v3 前
拒绝 ABI-2 插件；旧宿主在回调前拒绝 ABI-3。#257 同步迁移所有维护中的测试
插件和 SDK 消费者。

```c
#define PS_OPERATION_ABI_VERSION_3 3U
#define PS_OPERATION_PORT_VALUE_V3 1U
#define PS_OPERATION_PORT_FLOAT32_SCALAR_V3 2U
#define PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V3 3U

typedef struct ps_operation_port_constraint_v3 {
  uint32_t struct_size;
  uint32_t kind;
  uint32_t minimum_bits;
  uint32_t maximum_bits;
} ps_operation_port_constraint_v3;

/* Insert after parameter_count/parameters, before execute/user_data,
   in the mechanically renamed ps_operation_descriptor_v3. */
uint32_t input_schema_count;
const ps_operation_port_constraint_v3* input_schema;
ps_operation_port_constraint_v3 output_schema;

uint32_t ps_operation_plugin_get_abi_version(void);
const ps_operation_plugin_api_v3* ps_operation_plugin_get_api_v3(void);
```

机械映射和上述插入共同定义新描述符的精确布局，所有嵌套回调和表使用 v3 类型。
C 声明保留 PS_OPERATION_EXPORT 和 C linkage。约束结构为四个 uint32 字段，
区间端点通过数值 uint32 承载 IEEE binary32 位模式，使用 memcpy 解码。
input_schema_count 等于 input_count 且不超过1024；数量为零时指针必须为空，
其余情况要求自然对齐并在插件生命周期内有效。宿主验证每条记录和 output_schema
的精确 struct_size、种类、数量、区间和组合，发布前复制所有记录；错误时不
发布部分注册表。C 与 C++ 共用相同的约束验证和调用语义。回调和插件指针不
进入复制后的语义事实或阶段身份。

数据 provider ABI v1 的静态 schema 布局与入口保持不变，闭合元素解码增加4。
新宿主接受旧1至3 schema，旧宿主拒绝新 Float32 schema。Provider 没有运行
Value 回调，不定义另一套图像或端口契约。Value 构造、facet 边界、异常隔离、
单输出 sink、CPU fallback 结果规则及准确清理在 operation 版本变化中保持。

## 验证、生命周期、并发与取消

验证在 publication 前失败。Analyze 先验证 declaration count、ids/names 和 duplicate identities，然后验证 descriptors、dense layout/Region/facets 以及 tagged references，再发布 semantic。Binding validation 先检查精确的 name multiset，然后按 declaration-id 顺序检查 Values。单个 Value 检查 validity、element、shape、Region、layout/storage length，然后检查 facets。完整 binding set 及全部直接端口约束成功验证前，不得发生 registry callback、transfer 或 result publication。

| 失败 | 稳定 ErrorCode / effect |
| --- | --- |
| 缺失、额外、重复或格式错误的 binding name；无效/默认 Value | InvalidArgument；没有 callback 和 partial result。 |
| 有效但错误的 element type、shape、Region、layout、byte length 或 facet set/version/payload | TypeMismatch；没有 callback 和 partial result。 |
| 无效 declaration id/name/count/schema/Region/layout/facet structure 或重复 id/name | analyze 期间 InvalidArgument；没有 semantic IR。 |
| 未知 tagged source reference | analyze 期间 NotFound；没有 semantic IR。 |
| 与 operation trait 冲突的 type/shape | analyze 期间 TypeMismatch；没有 semantic IR。 |
| Dense products、signed strides、host-size conversion 或有界 resource exhaustion 无效 | ResourceExhausted；没有 partial stage/result。 |
| Default、stale 或 foreign-registry plan | 现有 Stale plan-entry failure，在读取 bindings 或观察 token 前发生。 |
| plan entry 成功后观察到 cancellation 或 stale graph | 进入执行后的失败选择和发布检查遵循 Cancelled 优先于 Stale，Stale 优先于普通运行失败。 |

当多个 name errors 同时存在时，验证按 bounded/name format、duplicates、extra names、missing names 的顺序进行。每一类内部按 lexical name order 选择诊断。诊断文本不稳定；调用者基于 `ErrorCode` 分支。现有 entry check 首先将 non-current/default 或 foreign-registry plan 拒绝为 Stale，即使 token 已取消。entry 成功后，在 binding validation 前、选择 binding failure 时、admission 前和 final publication 时再次观察 cancellation/currentness。使用现有的 no-throw stop-selection fallback。只要当前公开 execute 契约允许，分配失败仍可抛出 `std::bad_alloc`，包括 C++ 按值参数构造期间。

`execute` 是同步的。按值传递的 bindings 为调用提供自己的名称和 Value metadata。复制 Value 会共享其不可变的 owned byte vector。调用者不得在参数复制期间并发修改 source container；并发 wrapper 必须在复制完成前拥有其 arguments。Plan、cancellation 和 options references 在调用期间保持有效且不被修改。`ExecutionContext` 不能与 `execute` 并发销毁。

每个 Run 保留 snapshot，直到所有已准入 callbacks 都退出，包括 cooperative cancellation 后的不可抢占 callbacks。拒绝 late cancelled/stale publication，然后恰好一次释放 Run-owned references。返回的 Values 拥有其字节，并可超过 Run 生命周期；销毁某个 caller 的 Value copy 或某个 result 不会使另一个 owner 失效。kernel 不增加新的公开 release method。

同一 plan 可在相同或不同的匹配 ExecutionContexts 中使用不同快照并发执行，前提是其 GraphContext 和冻结 registry 仍有效且保持当前版本。Runs 永远不在共享 plan/registry state 中存储 bindings，永远不共享 mutable result slots，也不按 plan digest 去重工作。Graph replacement/destruction 保留现有 stale behavior。

Inputs 以 Run-local CPU backend label 开始。GPU consumer 使用现有明确的不可变 transfer 和 fallback mechanism。保留 caller-owned immutable input 不消耗 `maximum_live_bytes`；该限制和 `peak_live_bytes` 保持当前建模的 callback-byte 含义。它们不限制 total retained inputs、intermediate results 或 process RAM。Admission 统计等待执行的 callbacks，不统计声明引用或同步调用线程。Embedding 控制 input allocations 和 concurrent calls。


标量 NaN、Infinity 或越界，以及绑定图像的非法像素内容，预检返回
InvalidArgument。错误描述符、图像 facet 或端口连接返回 TypeMismatch；非法
schema 或计算产生的受约束标量引用返回 InvalidArgument。计算产生的非法
图像、非有限结果或错误输出图像约定返回 OperationFailed。首次回调前预检所有
声明和直接消费约束；算子输入/输出仍在各次调用边界验证。大图像扫描定期观察
取消及 currentness，并在预检结束、失败选择和发布前沿用现有优先级。

## 兼容性、基准测试与下游影响

#257 将 package 0.2.0 更新为 0.3.0，将 source schema 1 更新为 2，并使用以上 named stage domains。Schema-1 document 显式 version validation 失败；更新后的 default writer 发出 2。只有一个 source/API implementation，所有 callers 都迁移。现有 `execute(plan, token, options)` 调用变为 `execute(plan, {}, token, options)`。公开 C++ struct/type/signature 变更要求 consumers 重建；现有 SameMinorVersion package matching 必须拒绝要求 0.2 兼容性的 consumer 使用 0.3 package。更新 public header/export，以及 #257 中 static/shared 两种 isolated `find_package(Photospider)` consumers。本已接受决策当前不改变 CMake version 或 installed header。OperationTraits 版本改为3，operation ABI 改为3；provider ABI 保持1并支持新元素4。

Raw plan identity 排除 payload；现有 result identity 描述 output Values，并且可能因 inputs 不同而不同，不承诺 collision-free 或 persistent identity。正确性使用独立的 named oracle 比较实际字节。保持 backend、transfer、resource、timing、plan/result identity 和 correctness observations 分离。不引入 input artifact 或 evidence identity。#257 在 `RawBenchmarkOptions` 中增加 `ExecutionBindings bindings;`；每个现有 independently compiled sample 接收该 snapshot。runner 在 entry 复制一次 bindings，并为其 samples 复用该不可变 snapshot；调用者不得在调用期间修改 options。其现有 iteration semantics 保持不变。#258 通过直接 compile/execute calls 证明 compile-once/two-payload reuse，也可以对每个 payload 使用 matching captured oracle 各运行一次现有 benchmark。通用 benchmark-mode redesign 超出本决策范围。

[Daemon #10](https://github.com/kevin-zf1123/photospider-daemon/issues/10) 在本决策接受后开始。它必须明确决定 Session immutability 和 bounded public binding projection、retain/copy points、IPC/client versions 以及 cancellation/release rules。建议为一个不可变的 Session document/plan 和独立的 per-Job snapshots；声明改变时关闭并创建新的 Session。Kernel `GraphContext::replace` 不创建 daemon update method。本处不规定 wire encoding、bulk transport、internal IR serialization 或 daemon implementation。


Daemon 新功能按已接受方向需求驱动。kernel 0.3 的破坏性变更仍需最小
consumer/package 协调迁移，或另行批准的版本/CI 选版策略；现有0.2消费者和
跟随主线的 CI 不能自动兼容。#10 的具体 Session/协议决策继续独立处理。

## 命名测试与图像参考

这些为 #257/#258 的未来验收要求，当前没有运行实现。声明分别为 id1/name=image、
id2/name=gain、id3/name=opacity。图像为 Float32 {2,2,4}、完整 Region、偏移0、
strides {32,16,4}、64字节及精确图像 facet；两个标量均为 Float32 {1}、完整
Region、偏移0、stride4、4字节、无 facet。

Node10 的 image.exposure_gain 接 id1、id2；node20 的 image.opacity 接 node10
与 id3；输出 result 选择 node20。全部源参数为空。只编译一次，先后及并发绑定
下表两组数据，每次两个 CPU 回调，无 CPU-only 传输。请求输出 Region 为
{0,1,0}/{1,1,4}，对应像素 (0,1)，仍返回完整图像。

| 案例 | 四个输入 RGBA 像素 | 增益 / 不透明度 | 四个预期输出 RGBA 像素 |
| --- | --- | --- | --- |
| A | (1/8,1/4,0,1/2); (1/4,1/8,1/8,1/2); (0,1/4,1/2,1); (0,0,0,0) | 2 / 1/2 | (1/8,1/4,0,1/4); (1/4,1/8,1/8,1/4); (0,1/4,1/2,1/2); (0,0,0,0) |
| B | (1/4,0,1/8,1/2); (1/8,1/4,1/8,1); (1/2,1/4,0,1); (0,0,0,0) | 1/2 / 1/4 | (1/32,0,1/64,1/8); (1/64,1/32,1/64,1/4); (1/16,1/32,0,1/4); (0,0,0,0) |

参考名为 `s1-rgba32f-exposure-opacity-v1`。独立参考逐阶段按 binary32 舍入；
本例全部为二进制分数，要求输出逐位精确。一般非精确数据使用前述容差。

| 测试 | 要求 |
| --- | --- |
| S1Image.DynamicBindings | 同一编译计划处理下表 A/B，像素精确且编译次数不变，每次两个 CPU 回调。 |
| S1Image.SingleBindingChanges | 分别只改图像、只改增益、只改不透明度，再将不透明度设为零；不复用旧绑定。全零输出相同时结果摘要允许相同。 |
| S1Image.ConcurrentSnapshots | 通过同步条件控制两个 Run，使用独立绑定和 token；回调运行时释放调用方副本，各自结果仍正确。 |
| S1Bindings.Names | 缺失、额外、重复、非法名称及默认无效 Value 分别在回调前返回 InvalidArgument；合法顺序变化不影响结果。 |
| S1Bindings.Descriptors | 合法但不匹配的元素、形状、部分或空 Region、负/广播/填充步长、非零偏移或多余字节：TypeMismatch。 |
| S1Bindings.Facets | 缺失、额外、键/版本/载荷不匹配：TypeMismatch；重复或非法 facet 结构在 Value 构造或分析时失败。 |
| S1Scalar.Validation | 类型/形状/facet 错误：TypeMismatch；NaN、无穷、增益低于0或高于16、不透明度超出 [0,1]：InvalidArgument，回调数为零。 |
| S1Scalar.Schema | 计算生成的标量或消费区间空交集在分析时失败；重复引用须满足所有消费区间；未知种类/数量/边界在注册时失败。 |
| S1Image.PixelDomain | 直接绑定中的负/非有限 RGB、非法 alpha 或 alpha 零时 RGB 非零，在预检返回 InvalidArgument；通用 Float32 Value 仍可构造。 |
| S1Image.AlphaUnderflow | 合法 HDR 输入 (1,0,0,2^-149) 乘不透明度0.5后，binary32 结果为 (0.5,0,0,0)，违反 alpha 零时 RGB 零的规则，返回 OperationFailed；全部通道仍有限。 |
| S1Image.OutputFailure | 计算溢出或非法计算图像/约定返回 OperationFailed，不发布结果或触发后端重试。 |
| S1Image.Demand | 请求 offsets {0,1,0}、extents {1,1,4}，两个图像端口接收该需求，两个标量端口接收 {0}/{1}，结果仍完整64字节。 |
| S1Image.Halo | 在 {3,3,4} 上请求 {0,0,0}/{1,1,4}，半径1：图像需求 {0,0,0}/{2,2,4}，标量 {0}/{1}，不扩展通道轴。 |
| S1Image.DemandFailures | 空、越界、部分通道、未知名称需求在规划时失败；图像消费者在分析时拒绝无约定保证的生产者。 |
| S1Bindings.Identity | 静态 schema/图像约定/边界/源事实变化影响身份；运行图像/标量字节及顺序不影响；合并后最终需求不变时计划身份保持。 |
| S1Bindings.StopsAndBounds | 默认/外部/过期计划、准入前后取消、队列拒绝及连续字节/2B 溢出保持约定的错误优先级与准确清理。 |
| S1Abi.VersionAndSchema | 查询 ABI3 表前拒绝 ABI2；非法 ABI3 端口 schema 原子拒绝；合法 C 和 C++ 图像结果一致。 |
| S1Abi.ProviderFloat32 | provider-v1 元素4仅由新宿主接受；旧1至3仍有效，未知元素继续失败。 |
| S1Bindings.StaticSharedConsumer | 已安装 schema2、Float32、ABI3 C 插件及 C++ 消费者在静态/共享版本运行同一参考；0.2 消费者拒绝0.3软件包。 |

图像输出的计划调用字节数为 `max(traits.estimated_bytes, 2*B)`，B 是输出连续
字节数。2*B 保守包含回调输出和宿主接收副本，乘法需检查溢出；通用 Value
输出保持原估计语义。示例每步128建模字节，孤立顺序 CPU 运行的峰值为128。
这仍不统计全部输入、中间结果或传输分配，不等于进程内存上限。该规则进入
计划身份和资源验证，不新增 S2 的真实存储预算保证。

## 决策状态与交付

维护者已接受调整方向、Float32、上述完整端口 schema 和单一 operation ABI v3。
接受依据为当前任务中针对具体 ABI 升级及配套契约问题的明确回复“是，继续。”。
无需再次确认这些决定。已接受目标尚未实现，#257/#258 负责代码及运行验收。

#256 跟踪公开文档交付与 Issue/Project 结算。维护者已另行授权两个文档 PR、
CI 通过后的合并及 #256 结算。决策接受不表示实现完成，也不自动启动 #257。
