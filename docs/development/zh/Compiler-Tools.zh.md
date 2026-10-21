# psc 与 psx

`psc` 是编译驱动程序；`psx` 加载并执行已编译 Plan。前端、校验、优化、规划和执行仍是可独立调用的库组件。驱动程序负责路径、JSON manifest、诊断、会话保留和输出发布。其职责划分参考 Clang 的编译驱动设计，由主目标决定执行哪些库阶段；实现不依赖 LLVM 或 LLVM IR。

驱动程序使用物理 Plan v5 和固定 invocation ABI 8。外部算子通过 [operation module ABI 8](../../kernel-architecture/zh/Plugin-ABI.zh.md)提供静态 preparation、task lowering 和固定 kernel。只返回输入的 IR 可以使用空算子 registry 运行。

## 构建与算子加载

`PHOTOSPIDER_BUILD_TOOLS=ON`（默认）构建 `psc` 和 `psx`。在已有构建目录执行：

```sh
cmake --build build/kernel-dev --target psc psx -j 2
```

`PHOTOSPIDER_BUILD_OPS=ON` 时，工具先注册内置算子，再按命令行顺序加载显式 `--load-ops PATH` 指定的受信任进程内 ABI 8 module；OFF 时 registry 初始为空。加载后冻结 registry。重复库、重复算子名称和不兼容契约均失败。`--list-operations` 输出已加载名称、traits 版本与 CPU/GPU 能力。Artifact 不指定或加载库。`--resources` 提供不可变静态资源身份与字节；每次运行的 Tensor 输入由 `psx --bindings` 提供。

工具私有 JSON codec 依赖 yyjson 0.13.0，固定提交 `6447536015f3d600f3d65323b10976103b337ca7`，附 MIT 许可证。配置前，按照 [yyjson 准备说明](../../../third_party/YYJSON.md)，将 `yyjson.c`、`yyjson.h` 和 `LICENSE` 放到 `third_party/yyjson/`。该目录由 Git 忽略，不包含在仓库检出内容中；CMake 不下载源码。设置 `PHOTOSPIDER_BUILD_TOOLS=OFF` 可关闭工具及其 yyjson 依赖。yyjson 不进入内核公共头文件或导出依赖。不支持 YAML 输入。

## 编译与版本

```sh
# 无 optimizer：DAG -> 校验 IR0 -> planner -> Plan。
psc input.json --planner static-tiles -o output.psxp
# 导入成为本次会话 IR0，然后执行一次可选优化。
psc input.psir -O optimize --emit ir -o optimized.psir
# 保存两个不可变版本。
psc input.json -O optimize --emit ir -o optimized.psir --save-ir 0=frontend.psir
# 重复名称仍是两个阶段。
psc input.json -O optimize -O optimize --save-ir 1=once.psir -o twice.psxp
```

`psc` 对 DAG JSON 使用默认注册的 `dag@1` 前端，对 PSIR 直接导入。前端注册与按名称选择通过库的 `FrontendRegistry` 和 `Compiler::analyze`/`compile` 提供；CLI 保持现有输入识别方式。

按输入契约添加 `--load-ops`、`--resources`。`IR0` 是通过校验的输入或前端结果，每次成功优化产生下一个编号。未指定 `-O` 时调用 optimizer 的次数为零。内置 `optimize@1` 是没有选项的可选依赖路径去重示例；planner 自己在私有 lowering 副本中进行必要去重。同一 Call ID 的重复引用代表同一次调用，不同 Call ID 保留不同调用；CFG 循环的动态激活独立。

所有版本统一使用 `ValidatedIR`。变换读取不可变输入，返回独立 `IRProgram`，重新校验后发布。内容 identity 不包含来源信息，no-op 可以产生 digest 相同的新版本。来源记录父 identity、生产者/版本和已解析选项。IR 导入保留来源，但会话编号从零重新开始。原始与优化后的 `.psir` 都可直接规划。C++ `OptimizerRegistry` 在冻结前允许注册。每个定义声明支持的 IR 形式，以及包含类型化默认值、可选闭区间数值范围、可选枚举值和说明的 `option_schema`。注册时校验 schema；调用时先解析默认值并检查全部覆盖值，再进入 callback。编译器重新校验输出程序，并检查公开输入输出的名称与 schema 保持不变。结构校验不能证明数值、控制流和副作用等价；变换仍须提供语义测试。名称最多 1024 字节、版本 128、选项 1024 项、key 1024、字符串值 8192。没有动态 optimizer ABI 或数字优化等级。

| 参数 | 行为 |
| --- | --- |
| `<input>` / `-` | 一个文件或 stdin；根据内容识别 DAG JSON 或 PSIR。 |
| `-x dag\|ir` | 断言输入类型，与内容不符时报错。 |
| `--emit plan\|ir\|ir:N` | 默认 `plan`；`ir` 执行全部所选优化；`ir:N` 只执行前 N 个。 |
| `-o PATH` | 主 artifact；`-` 表示 stdout。默认当前目录 `stem.psxp` 或 `stem.irN.psir`；stdin 必须指定输出。 |
| `--check` | 读取、必要的前端处理及校验；不优化、规划或输出。冲突的阶段/输出参数报错。 |
| `-O NAME` | 追加一次调用，保留顺序和重复。 |
| `--opt-option N:key=value` | 覆盖第 N 次调用的类型化选项，编号从 1 开始。 |
| `--list-optimizers`、`--help-optimizer NAME` | 列出支持的 IR 形式，或查看参数类型、默认值、范围、枚举值及说明。 |
| `--save-ir N=PATH` | 额外快照，可重复，一个版本可有多个目的地；不能延长流水线或使用 stdout。 |
| `--release-ir-after-save N` | 所有请求保存成功后移除该版本的会话保留；必须请求保存，不删除文件。 |
| `--print-pipeline` | 只打印动作图，不加载算子、不调用静态 hook、不编译；格式识别可读取输入并检查 JSON 编码及格式标识/版本，不校验 workflow 字段。PSIR 允许开头分号注释，stdin 需要 `-x`。 |

默认保留所有成功 IR 到会话结束。最多两个快照导出任务可与串行优化/规划并发；每个任务持有不可变副本。显式释放只移除会话保留，活跃消费者继续持有自己的副本。IR 存储释放后，版本记录仍保留内容 digest、父 identity、生产者版本和已解析选项。驱动等待导出任务结束后才销毁 graph、registry 和资源 owners。后续失败保留已经发布的快照；主 artifact 等待全部必要阶段及快照成功后发布。失败变换不修改旧版本。

## 物理规划

以下参数仅允许用于 Plan 主目标：

| 参数 | 默认值/含义 |
| --- | --- |
| `--planner NAME` | 按 registry 查找，默认 `static-tiles`；内置后端按依赖顺序展开所需 tile。未知名称返回参数错误；与 `--check`、IR 输出目标冲突。 |
| `--backend cpu\|gpu` | 默认 `cpu`；选择固定 CPU 或 GPU lowering 目标。Preparation 必须支持精确的 placement。 |
| `--tile-extent N` | 128，正的二次幂。 |
| `--outputs a,b` | 选择具名输出及依赖闭包。 |
| `--requests FILE` | 输出/成员 tile 区间，与 `--outputs` 冲突。 |
| `--max-tasks N` | 4194304，正物理 task 上限；metadata 和传入 tile 具有独立结构上限。 |
| `--max-dependency-edges N` | 16777216，正静态边上限。 |

`psc --list-planners` 列出注册名称、版本、说明和支持的 IR 形式。`psc --help-planner NAME` 显示指定定义及共享规划参数。这些查询无需输入文件，不加载算子库，不调用规划回调。未知帮助名称返回参数错误。`--print-pipeline` 查找所选定义并显示其名称与版本。

库调用通过 `PlanningOptions::planner = "static-tiles"` 选择名称，`Compiler::plan` 和 `compile` 均使用该选项。将冻结的 `PlannerRegistry` 作为 `Compiler` 构造函数第三个参数传入即可使用自定义后端；省略时使用惰性初始化的默认目录。注册时拒绝重复名称、缺失回调和非法 IR 形式。`--backend` 独立控制 CPU/GPU placement。

`PlannerDefinition` 回调接收原始 `ValidatedIR`、本次回调专用的 `PlanBuilder` 和共享 `PlanningOptions`。Builder 提供只读链接声明和可编辑的物理 Task/Tile/Call/Block 表。编译器检查 CFG、schema、算子契约、placement、归一化输出选择及生成的 task 数量后发布 Plan。后端负责映射的语义正确性。参见[planner 构造契约](../../kernel-architecture/zh/Compiler-and-Execution.zh.md#物理-plan)。CLI 目录包含编译时注册的后端；`--load-ops` 只注册算子库。

输出选择保留控制流和 mandatory effect task。库的 `PlanningOptions::output_names` 用 `nullopt` 表示所有输出、空 vector 表示无输出。库 `output_requests` 中未出现的已选输出表示全部 tiles。CLI 请求 manifest 显式选择其列出的名称。运行请求只能缩小已保存的覆盖。

## 执行

```sh
psx output.psxp --load-ops ./operations.dylib \
  --resources resources.json --bindings bindings.json -o output.results
```

Plan 导入接收非空且冻结的 `OperationRegistry` 和显式资源；调用方保持算子环境的寿命。导入链接保存的算子和资源，检查物理表以及保存的邻接表、存活值和预约容量，不运行 optimizer、lowerer 或 planner。`ExecutionContext::prepare` 检查精确 placement 和 kernel invocation 契约；执行使用 `submit` 和 `join`。不可用的 placement 在 Run 提交前失败。

Plan 的每个输入必须恰好绑定一次，包括所选输出未使用的输入。每个静态 Tensor slot 对应一个完整 raw-le 文件，dtype、完整 sample shape（batch 轴加 cell shape）和字节长度必须精确匹配。绑定读取器使用执行 context 的 ResourceBudget 暂存完整 raw 文件。对于 `DirectTiles`，读取器将所需区域复制到独立、对齐的 dense Host tile，其 byte offset 为零，容量与编译契约相同。对于 `DenseHost`，只要请求了某个 member 的任一 tile，就保留该 member 的一份完整稠密分配；Plan 的显式 COPY task 在执行期间物化选定 tile。其他不兼容的物理 layout 返回 `TypeMismatch`。`--check` 导入并准备 Plan，链接资源及固定 kernel，然后停止，不提交 Run 或导出。检查模式可不提供 bindings；一旦提供 manifest，仍须完整校验每个声明和 slot。实际执行始终要求绑定全部声明输入。

| 参数 | 默认值/含义 |
| --- | --- |
| `--bindings FILE` | 实际执行有输入声明的 Plan 时必需；`--check` 可省略。 |
| `--outputs a,b` / `--requests FILE` | 已编译覆盖内的输出子集或成员区间。 |
| `--threads N` | CPU worker 数；省略使用 context 默认，显式范围为 1..4096。 |
| `--parallelism N` | 本次 Run 的最大并发 task 数，范围 1..4096；省略时使用 context worker 数量，call 按 Plan 顺序退休。 |
| `--max-live-bytes N` | 268435456，正的受控 Payload 容量。 |
| `-o DIRECTORY` | 默认当前目录 `plan-stem.results`；stdin 必须指定输出；目录必须不存在。 |

`psx` 拒绝优化、backend、tile extent 参数。更大的覆盖或不同物理目标需要重新编译。输出是 manifest 和记录真实覆盖的小端二进制片段，不填补缺失区域。逻辑窗口拷贝遵循负/零 stride，保留 NaN、负零等位模式。不创建持久执行服务或恢复状态。

## 实现边界

两个可执行入口分别调用各自的驱动库。参数 schema 与类型化 invocation 独立；`psc` 不包含运行绑定参数，`psx` 不包含编译参数。

| 源码区域 | 职责 |
| --- | --- |
| `src/tools/psc/` | 编译 invocation、动作图、IR 会话与 DAG JSON 前端 I/O。 |
| `src/tools/psx/` | 执行 invocation、Plan 加载、运行绑定与 Results 导出。 |
| `src/tools/common/` | 参数词法处理、诊断、进程信号、文件发布、JSON/schema 辅助、算子加载、资源与请求 manifest。 |
| `src/lib/compiler/frontend.cpp`、`frontend_registry.cpp`、`frontend/DAG/dag.cpp` | 前端名称分发、注册目录、统一 IR 绑定及 DAG lowering。 |
| `src/lib/compiler/optimizer.cpp` | 组织变换、输出校验与公开接口保持检查。 |
| `src/lib/compiler/optimizer_registry.cpp`、`optimizers/deduplicate.cpp` | 注册目录/参数 schema，以及独立的可选示例变换。 |
| `src/lib/compiler/planner.cpp`、`planner_registry.cpp`、`planners/static_tiles.cpp` | 按名称分发后端、注册目录、结果校验及注册的静态 tile 规划实现。 |
| `src/lib/compiler/plan_linking.cpp`、`plan_validation.cpp`、`plan_selection.cpp` | 已保存 Plan 的链接、物理表校验与不调用 lowerer 的覆盖缩小。 |

公共工具库不持有编译会话或执行 context。仅为绑定或规划构造库 `Compiler` 不会初始化默认 optimizer 目录；显式优化或查询目录时才需要它。

## 文件、限制与诊断

输出不得与任何输入、资源、库或另一输出指向同一文件；检查包括符号链接解析和硬链接等价性。普通输出先完整编码、写入私有同目录 staging，再原子 rename；允许替换既有普通文件。结果目录在 macOS/Linux/Windows 上通过 exclusive rename 发布；不支持的平台返回 I/O 错误，不复用既有目录。原子发布保证可见性，不承诺断电持久性。stdin/stdout 传递 artifact 字节；诊断、verbose 和 timing 写 stderr。帮助、清单和流水线信息命令使用 stdout，不生成 artifact。

输入 JSON 与 artifact 文本最多 64 MiB。JSON 最多 128 层、4194304 个 DOM 值；拒绝重复键、未知字段、非法 Unicode、错误类型和数值溢出。请求最多 65536 个区间，每个成员只归一化一次。静态资源每项 1..64 MiB，必须匹配 SHA-256。绑定 staging 在读取前申请容量；导出使用受预算管理的最多 1 MiB 片段，每个 Tensor 最多 1048576 片。预算涵盖声明的 buffer，不等于进程 RSS、文件系统缓存或插件私有分配。

两个工具均保留选项值中的 `=` 字符，包括 `-oalpha=beta` 这样的短选项附着写法。独立的 `--` 开始位置参数，除非它已被前一个选项作为值消费。非法调用的诊断格式也按同样的参数边界确定。重复指定 `--diagnostic-format` 仍属于参数错误，由第一次指定的格式渲染该错误。

共用的诊断和信息参数包括 `--diagnostic-format text|json`、`--color auto|always|never`、`--time-report`、`--version`、`-h`/`--help`。编译器专属帮助列出用于优化阶段和 IR 释放事件的 `-v`/`--verbose`、用于限制编译错误汇总数量的 `--error-limit N`，以及警告控制参数。`psc` 接受这些控制；`psx` 将 `-v`/`--verbose`、`--error-limit` 和所有以 `-W` 开头的参数作为未知参数拒绝，退出码为 2。JSON 诊断为 version-1 JSONL，包含 `severity`、稳定 `id`、`stage`、`message`，以及已知的 `source`、`ir_revision`、`status_code`、`node_id`、`input_id`。解析位置写在 message 中，未知位置省略。仅在渲染诊断时替换系统或路径中的非 UTF-8 字节。输出名称、schema identity 等语义 JSON 字符串若含非法编码则报错，不会静默改名。Timing 使用 wall microseconds，并发导出的时间不可直接相加。

`psc` 在首个串行错误处停止，等待已调度保存结束，并逐项报告收集到的失败，最多 `--error-limit` 项（默认 20，0 表示无限）。退出码由最高优先级失败决定，不受显示上限影响。后续编译或主 artifact 发布失败时，`PS-SNAPSHOT-SAVED` note 标出仍保留的成功快照。并发失败优先级固定为：中断、内部错误、I/O、参数错误、编译/运行失败；同级按 IR 编号、阶段名称、来源路径排序。唯一可用的警告组 `unused-optimizer` 默认启用，报告所选 `ir:N` 终点以后的优化器；支持 `-Wall`、`-Wextra`、`-Werror`、`-Wno-error`、`-W[no-]unused-optimizer` 和 `-W[no-]error=unused-optimizer`。具体组设置优先于全局设置，同优先级最后一次设置生效。未知警告组报错。

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 输入、编译/运行/资源失败或升级为错误的警告 |
| 2 | 参数无效或冲突 |
| 3 | 内部错误，包括编译器回调生成非法 IR 或 Plan |
| 4 | 文件/stdout I/O 失败 |
| 130 | SIGINT/协作中断 |

编译阶段之间、文件操作期间检查 SIGINT，执行监控线程将其转为取消。进程内算子 callback 必须协作，工具不强制终止 callback。

参见[文件格式](Compiler-Tool-Formats.zh.md)、[编译器架构](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)和[版本轴](Compiler-Version-Contract.zh.md)。CLI 回归通过真实二进制和消费资源的外部 ABI fixture，覆盖 ops OFF/ON 注册、IR 往返、数值结果、部分覆盖、路径冲突与非法输入。各平台结论需要对应平台的实际运行证据。
