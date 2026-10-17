# ADR 0022：所有算子冻结同一套按前缀协商的 C ABI

- 状态：Proposed
- 英文权威文档：[ADR 0022](../0022-freeze-operation-c-abi-v3.md)
- 相关：[ADR 0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.zh.md)、[ADR 0014](0014-compiler-document-and-plan-versions-are-independent.zh.md)、[ADR 0015](0015-breaking-product-boundary-scope-reset.zh.md)、[ADR 0016](0016-workflow-inputs-and-execution-bindings.zh.md)、[ADR 0023](0023-track-dependencies-between-fixed-tiles.zh.md)

## 1. 核心摘要（TL;DR）

Package 0.36.0 中，内置算子通过未冻结的 C++ registry 注册，外部模块使用 Result operation C ABI 6（OperationTraits 28），后者只接受精确相等的结构体大小。ABI 6 已能静态解析元数据、按显式身份选择不可变资源、通过 tile Need 请求输入，并注册校验器与诊断；但 C 算子没有持久的准备状态，没有稳定前缀协商，也没有通过基础入口 `get_service` 查询和协商服务表的机制。CPU parallel、CPU tile 和 native GPU 服务已经具有独立的 ABI 版本。Photospider 将冻结一套带版本的 C ABI，作为宿主与所有算子模块之间唯一的二进制契约；header-only 的 C++ SDK 编译进每个模块，内置算子与外部模块走同一条导入路径，结构体按稳定前缀协商，服务表各自带版本，冻结范围只覆盖保留的内置算子实际需要的能力。

## 2. 架构心智模型（Mental Model & Intuition）

```text
   built-in ops library            external module (.so/.dylib)
   (static or shared)              (dynamically loaded)
          |                                  |
   module-specific getter            exported frozen-ABI entry point
   (unique symbol or fn pointer)             |
          \                                  /
           +------- frozen module table ----+
                           |
             host import: whole-table validation
             (prefix sizes, required capabilities,
              unknown required content -> reject)
                 /                      \
          all records valid          any record invalid
                 |                          |
     copy into private registry      reject module, publish nothing,
     candidate, swap atomically      run destroy hook, keep error location
                 |
   compiler: pure prepare callbacks (metadata + static parameters only)
                 |
   executor: start/poll callbacks with borrowed versioned service tables
                 |
   host validates publication, tile records, budgets, cancellation
```

宿主负责编译、调度、依赖跟踪、预算、取消、缓存、发布和模块生命周期。算子模块负责公式、参数定义、元数据推导、输入请求和依赖声明。宿主可以保留私有 C++ 注册记录，这些记录不属于冻结契约。

模块内部可以使用 C++。STL 对象、异常和不稳定的内核 C++ 符号不跨越边界。SDK 提供 builder、RAII 封装、类型化 continuation、窗口封装和常用推导规则，全部编译进使用它们的模块。

## 3. 契约规约与接口（Formal Contracts & APIs）

冻结 ABI 的具体头文件属于后续设计，其版本号在冻结时确定。下面的形态只说明协商规则；它是目标，不是任何现有头文件中的声明，字段和类型名称仅作示意。当前精确大小的 ABI 6 声明位于 `photospider/plugin/result_operation_plugin_api.h`，说明见[插件 ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)。

```c
/* Illustrative target shape; not declared in any current header. */
typedef struct ps_op_module {
  uint32_t struct_size;   /* bytes the module filled; host reads min(size, sizeof) */
  uint32_t abi_major;     /* major of the frozen ABI; a different major is rejected */
  uint32_t abi_minor;     /* informational; capability checks decide acceptance */
  uint32_t operation_count;
  uint32_t operation_stride;          /* extensible record array */
  const struct ps_op_operation *operations;
  void (*destroy)(const struct ps_op_module *);
} ps_op_module;

typedef struct ps_op_service_query {
  uint32_t struct_size;
  /* Returns a separately versioned service table, or NULL if unsupported.
     Called outside per-sample loops; the table lives for the stated scope. */
  const void *(*get_service)(const void *host, const char *service_id,
                             uint32_t major, uint32_t minimum_minor);
} ps_op_service_query;
```

### D1. 唯一冻结的二进制契约

带版本的 C ABI 是算子模块唯一冻结的二进制契约。C++ SDK 是源码层面的支持，编译在模块一侧。每个句柄的分配与销毁责任都明确规定；只要模块的代码或表仍可能运行，宿主就保持该模块的所有者存活。调用 `Compiler`、`ExecutionContext` 等 C++ API 的应用另有独立的兼容政策；本决定不冻结内核的 C++ ABI。

模块侧的 header-only SDK 尚不存在。`Photospider::operation_sdk` 针对当前头文件提供 C++ helper，`Photospider::ops_sdk` 是提供颜色描述、codec、LUT 契约和数值诊断的算子领域库；两者都不是本决定要求的模块侧 ABI SDK。

### D2. 冻结接口提供的能力

1. **推导。** 纯准备回调返回已解析的输出契约，其中包括由静态参数推导的输入投影（本次计算实际需要哪些输入），以及准备阶段推导的额外执行 workspace 上界。SDK 提供 shape、dtype、halo 和 spatial factor 的常用规则；这些规则词汇留在 SDK 中，不照搬进 C ABI。准备只依赖输入元数据和静态参数，不读取运行时样本，不执行上游节点，也不做文件 I/O。在 package 0.36.0 中，C++ `OperationPreparation` 已能返回推导的投影和 `additional_workspace_bytes`；ABI 6 的 `resolve_metadata` 回调返回输出 schema、资源身份和可替换的共享 stage 声明，包括 stage 投影和 workspace 上限。
2. **schema 时机。** 准备阶段可以根据输入元数据确定具体 schema，包括受约束的 schema 继承。结构在执行前固定。运行时只改变生产覆盖和完成状态；数据依赖的有效数量使用静态容量 Tensor 中的普通 payload。读取样本后新增字段或更换 dtype 不在冻结 ABI 范围内。
3. **内存与状态。** 宿主提供跨 poll 保留的存储。静态准备状态、一次执行的可变状态和已发布的不可变数据是三种不同的生命周期。执行工作区经宿主资源准入计费。准备存储与执行预算之间的现有边界保持不变。在 package 0.36.0 中，C++ 算子把静态准备状态保存在 `PreparedOperation::state()` 中；C 算子有每次执行的 continuation 状态（`state_bytes`），没有静态准备状态。
4. **依赖。** Need 指定输入端口、成员 slot 和紧凑 tile 区间，或显式请求 descriptor-only。完整覆盖使用区间选择，静态结构来自 plan，空 payload 集合不授予访问。冻结证据记录实际 Need，不扩大授权；坐标到 tile 的转换由 SDK helper 完成。
5. **资源。** 不可变资源按显式身份（字节长度与 SHA-256）从提供给编译的 `ResourceSet` 中选择，回调只收到所选资源的借用视图；不存在隐式查找。ABI 6 已通过元数据 sink 的 `set_resources` 提供这一能力；冻结 ABI 保留它，模型与引擎政策按 D8。宿主可以在内部代管 `semantic_key`、association 等事实，不必公开 C++ query 的每个字段。
6. **校验与诊断。** 端口绑定显式校验器，算子声明有界诊断报告，回调附加不透明的领域失败码。ABI 6 已提供这些能力；冻结 ABI 保留它们。

回调错误继续采用结构化错误和 sticky failure，取消、stale 工作和发布规则继续适用。CPU/GPU 车道、窗口、视图、block 和 checkpoint 保持当前行为。纯工具代码和算法代码属于 SDK 或算子辅助库，迁移不把它们移入内核。

### D3. 兼容政策

1. 同一主版本内，旧模块可以在新宿主上运行。新模块在旧宿主上运行的条件是它实际必需的能力全部存在；平台和外部运行条件仍然适用。
2. 每个结构体有稳定的最小前缀。宿主只读取完整存在的字段，每个可能缺失的字段都有规定的缺省语义。可扩展的记录数组带 count 和 stride。可扩展的子结构通过指针引用。内嵌的固定值类型保持布局不变。输出结构遵守调用方提供的容量。
3. 一个小型基础入口返回各自独立带版本的服务表。表指针在文档规定的范围内保持有效。服务查询发生在逐样本循环之外。改变现有 exact-sized 服务契约需要新的服务版本。
4. 未知的必需内容被拒绝。未知的可选内容只有在契约明确允许时才被忽略。宿主不猜测未知的 dtype、必需的 schema 语义、访问权或发布模式。接口支持与实际设备或资源可用性分别检查。
5. 改变已有布局、函数签名、语义、所有权或线程规则时，升级对应的主版本。小版本不是唯一的能力判据。算子行为版本与 ABI 版本分开管理。
6. 从 Result operation C ABI 6 迁到冻结 ABI 需要重新编译模块，不增加 ABI 6 兼容层。宿主对提交的模块表整体校验、整体加入，不静默跳过不支持的部分。失败清理、模块生命周期和错误定位保持不变。
7. reserved 字段不是主要的扩展机制。语义身份由规范的字段编码计算，不对原始 C 结构字节、padding、指针或物理 stride 求 hash。语义相同的缺失字段与显式默认值产生相同的身份。

### D4. 内置与外部算子共用一条导入路径

内置算子作为独立库交付，可以静态链接。外部模块通常动态加载。两者取得模块表的方式不同，之后共用导入、校验、注册和执行。静态模块提供名称唯一的 getter 或函数指针；宿主不要求所有静态模块导出同一个会冲突的符号名。在 package 0.36.0 中，内置算子仍通过 `OperationRegistry::register_operation` 注册，只有外部模块使用 ABI 6 getter。

### D5. 只有 Result 端口

每个 workflow 算子端口都传递 Result。`Tensor` 是 Result 和实现内部的数值存储，不形成第二套 Tensor 或 planar 执行接口。Result-only 端口不意味着单输出、完整 bundle 或强制复制：命名输出、局部请求、合法视图和发布策略按各算子契约定义。模块在可信宿主进程内执行；ABI 检查不提供隔离。

### D6. 元素类型

管线元素类型恰好是 UInt8、Int8、UInt16、Int16、Int64、Float32 和 Float64。未知 dtype 被拒绝，不重新解释为字节或其他类型。已有编码不复用于新含义，也不提前声明未实现的类型。ABI 中的长度、容量、偏移和句柄继续使用 `uint32_t` 和 `uint64_t`，它们与 tensor dtype 是不同的概念。算子内部私有的复数运算或模型内部的 fp16/int8 不扩展公共管线；任何公开转换都必须显式进行。

### D7. 内核与时间无关

宿主应用负责时间表示、精确时间比较、选帧、窗口、权重、执行顺序、历史状态和历史失效。内核保留显式多输入算子、普通 batch 轴和显式的 `state_in`/`state_out` Result；"单张图像"不表示"只能有一个图像输入"。continuation、block、checkpoint 和缓存保持通用。schema 兼容的 plan 可以使用不同的不可变输入绑定运行，见 ADR 0016。宿主根据有理时间计算权重并转为 Float64 时，这个舍入边界是显式的，不等价于对原始有理表达式只做一次舍入。Sequence 与时间策略类算子迁到宿主侧库或 workflow builder。

### D8. 外部资源与引擎

1. 资源是显式、不可变、按内容寻址的对象或封闭集合。它有所有者，可以分片或共享，被 hash 的内容在使用期间保持稳定。可变路径永远不是执行身份。
2. 内核提供通用承载。可信插件或宿主适配器负责解释模型和引擎；内核不内置推理框架，也不提供专用的 infer 或 decode 服务。数据、资源字节、可执行实现和派生 session 相互分开。
3. 语义资源随已编译节点固定。更换模型、权重或配置需要重新准备，底层不可变资源可以共享。静态准备不下载模型，也不为推断 shape 执行模型；重型解析和初始化在明确的阶段进行。
4. 资源内容、算法实例和执行实现各有独立身份。结果复用覆盖实际影响输出的配置和后端。不影响计算的路径和来源说明不进入数学身份。
5. 可以复用有界、显式管理的只读缓存和 session。可变实例有明确的并发或独占规则。资源、配置或设备变化使相关状态失效。模型的历史 hidden state 只通过显式 Result 传递。
6. 原生包装器保留各自的 NUM/FMT 精度契约。外部实例单独验收，不自动视为 strict，也不继承统一误差界。默认算法通过普通 tile Need 和 Run 共享 stage 获取完整输入覆盖；使用局部 halo 需要证明。输出不同的分块推理属于另一个实例或算法版本。
7. 跨请求的结果缓存默认只对经过验证、符合纯计算与身份契约的确定性实例开启。测量一致的实例和随机实例默认关闭。固定 seed 不能证明确定性。权重与 session 的复用和结果缓存是两回事。
8. 每个实例声明宿主计量的部分、引擎可计量的部分、可靠的上界和未覆盖的部分；由宿主决定接受范围。不可中断调用使用的资源在调用结束后释放，取消后禁止不合法的发布。隔离和强制终止位于内核之外，与 ADR 0015 一致。线程池和并发能力显式声明。

### D9. 图像表示

图像是具有显式结构轴的静态 Tensor 成员。宿主新建 backing 默认 tiled planar；普通图像算子的 straight alpha 默认由实现、参数和 authoring helper 体现。这些默认值不要求合规声明、schema 标签或自动 validator。内核不推断 RGBA、不检查 alpha association，也不执行 associate/unassociate。

HWC/CHW 及其他合法逻辑轴顺序独立于物理映射。负/零 stride、broadcast、多片段和外部 backing 经地址、owner 与同步检查后仍合法。默认存储不要求完整画布预留，也不拒绝兼容 view。

显式 alpha 转换和 association 选项仍选择实际计算。Alpha-aware 重采样在算子内部完成必要加权。领域 channel group 保留为显式 ops 配置，不进入 kernel schema 或存储 group。Float32/Float64 支持由算子/后端决定，不引入隐式 narrowing。

### D10. Region 容器

C ABI 与内核 `Region`、`Footprint` 容器的布局相互分离。冻结 ABI 不冻结 C++ 容器布局，容器优化依据以后的测量处理。

### D11. 算子专用的元数据规则

现有表中的算子专用声明，包括 SDK 所拥有的推导规则，不以当前形式冻结。算子专用的推导移入各模块的 prepare 回调和 SDK helper，kernel 校验解析后的契约。冻结 ABI 不定义声明式内核能力格式，也不为其预留扩展位置。

### 验收

Package 0.36.0 尚未完成冻结 ABI 的整体验收。下列条件保留已经交付的能力，并规定验收冻结 ABI 实现仍需满足的要求。

- **行为对等。** 代表性迁移覆盖：identity 与通道提取/组装（schema、布局、视图、所有权）；表达式、Bezier 与序列算子（静态输入集合缩减）；LUT3D bake（静态 Tensor 报告、受 gate 控制的 Need）；STMap（两阶段 tile Need、tile 记录、内存）；block 与 checkpoint（依赖恢复）；独立安装的 PixelOE（native GPU）。代表集通过之后，01-numeric、02-format-color、STMap 与 PixelOE 中保留的全部算子都要完成迁移。目前保留的 01-numeric、02-format-color 和 STMap 算子通过 C++ registry 在 CPU 上交付；独立安装的 PixelOE 消费者和 native GPU 覆盖仍然延期。
- **兼容性。** 用独立旧头文件编译的 fixture 覆盖旧模块/新宿主、新模块/旧宿主、必需与可选能力、多元素数组 stride、短输出缓冲、未知扩展、失败清理和模块生命周期。仅用当前头文件伪造较小的 `struct_size` 不够。这些 fixture 需要已冻结的头文件，因此尚不存在。
- **外部消费者。** 独立构建并安装的消费者（例如 PixelOE）加载冻结 ABI。该验证只在内核开发完成后进行；在此之前不运行任何安装消费测试。
- **图像。** Float32/Float64、HWC/CHW 逻辑排列、非末尾 alpha、无 alpha、多组、hidden RGB、显式 association 转换且无默认合规拒绝、planar 物理访问、ROI 与通道依赖、合法视图与物化、有意的契约变化，以及 native 后端的支持边界。
- **外部引擎。** 包装器语义、冻结制品与独立直连引擎运行的对照、任务层面的效果；另外验证所有权、并发、取消、错误、资源和缓存身份。mock 或离线 oracle 不能替代真实的推理、解码或质量验证。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- 冻结 ABI 是契约决定。Package 0.36.0 实现只接受精确结构体大小的 Result operation C ABI 6，以及[插件 ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)所述的未冻结 C++ `OperationDefinition` 注册路径。ABI 6 不是冻结 ABI。
- 本决定不冻结应用使用的内核 C++ API。
- 不提供 ABI 6 兼容层、shim 或双注册路径。
- 不新增元素类型：Float16、Int32、UInt32、UInt64 和原生 complex 不进入管线。
- 内核不提供原生 Sequence 或 RationalTime 类型、按时间取帧服务、帧率或缺帧政策、播放、seek、重放或隐式的跨调用时间状态。
- 不提供内置推理框架、自动许可判定或制品审批产品。许可记录与资源身份、执行身份分开。
- 不提供沙箱、签名校验、隔离 worker 或强制终止；模块仍是可信的进程内代码。
- 本文不定义声明式内核能力格式或扩展位置、flat-box 或 bitmap Region 容器，也不作性能承诺。
- 内核不在 Result 数据上附着颜色、alpha、transfer 或采样描述，也不传播这类描述。
- 规范图像的 straight 与 planar 不重新作为待定的产品选项。

## 5. 后果与代价（Consequences）

每个模块，无论内置还是外部，都必须针对冻结 ABI 头文件重新编译；ABI 6 模块在加载时被拒绝。内置算子不再使用 C++ 注册接口和内核私有头文件，它们需要的辅助代码迁入 SDK 或算子辅助库。宿主承担为每个可缺失字段维护缺省语义、并在发布前整体校验模块表的成本。

前缀协商让结构体可以增长而不拒绝旧模块，但只有带规定缺省语义的追加才是小版本变更。改变已有字段的语义、所有权或线程规则都是主版本变更；模块要求宿主不理解的内容时，宿主拒绝该模块。模块要求的服务或设备能力缺失时，宿主拒绝它或报告不受支持，不以猜测的行为运行。

内置算子改走 C 回调后，每个 block 或 tile 增加一次函数指针调用。代价高的是逐行窗口、逐坐标读取以及逐元素取消或计工作量的回调；迁移前这些调用需要批量化形式，并在代表性的 01-numeric 算子和 PixelOE 上测量 C 桥相对 C++ 直连的开销。现有 premultiplied 图像算子改用 straight 图像时，其公共契约随之改变；这些变化与保持行为的 ABI 迁移分开报告、分开验证。

需要时间行为的宿主应用自行实现选帧、加权和状态传递，使用显式输入和 `state_in`/`state_out` Result。包装模型或引擎的算子在开启结果缓存之前，需要提供自己的验证、计量声明和确定性证据。
