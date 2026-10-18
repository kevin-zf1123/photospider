# ADR 0014：Compiler 阶段与兼容版本保持独立

- 状态：已接受

## 当前实现：统一 IR 与 v4 产物

Package 0.38.0 的所有合法 IR 版本统一使用 `ValidatedIR`。优化可选；planner 接受合法依赖路径，并私有地对重复 Call ID 去重。显式 CFG 循环保留激活语义。有序命名 optimizer 调用产生经过校验的不可变版本，内容 identity 不含来源。`.psir` 与 `.psxp` 为 v4，版本 1–3 需要重新导出。Plan 标识实际输入 IR，导入不执行 optimizer/mapper/planner。参见[当前契约](../../development/zh/Compiler-Version-Contract.zh.md)。

下面保留原始决策中独立版本轴的历史说明。历史阶段类型示例已被统一 IR API 取代，document、package、ABI 和 artifact 版本仍相互独立。

## 1. 核心摘要 (TL;DR)

Compiler 从版本化 workflow 文档逐步构造不同的不可变语义、优化和物理 plan。文档 schema、plugin ABI、package API、planner 行为和 daemon IPC 是不同契约。分别管理这些版本，避免暗示并不存在的兼容性。

## 2. 架构心智模型

```text
WorkflowDocument -> SemanticGraphIR -> OptimizedGraphIR -> ExecutionPlan
       |                    |                  |                |
 document schema       semantic digest    optimized digest   plan digest/key
```

每次阶段转换都会校验输入，并返回完整对象或错误。Plan 面向本地能力，不携带远程设备句柄。Frozen registry 身份用于运行时检查阶段来源，但不属于规范化语义内容。

## 3. 契约规约与接口

```cpp
// Excerpt; compiler and value types are declared by the public headers.
class Compiler {
 public:
  Result<SemanticGraphIR> analyze(const GraphSnapshot&, ResourceSet) const;
  Result<OptimizedGraphIR> optimize(const SemanticGraphIR&) const;
  Result<ExecutionPlan> plan(const OptimizedGraphIR&, const PlanningOptions&) const;
};

struct SemanticGraphDigest { std::string value; };
struct OptimizedGraphDigest { std::string value; };
struct ExecutionPlanDigest { std::string value; };
struct PlanCacheKey { std::string value; };
```

当前 WorkflowDocument schema 版本为 7，OperationTraits 版本为 29，package 版本为 0.37.0，operation-plugin C table 为 Result ABI 7。不同 digest 使用不同身份域；Plan cache key 用于可丢弃的派生查找。Compiler 按对应阶段编码封闭文档字段、规范化参数、复制的 operation trait、静态 preparation identity 字段、输入/输出 demand、optimizer identity 和目标能力等信息。运行时地址、分配 ID、计时、取消状态、队列状态和 daemon ID 不纳入 digest。Float 参数身份按复制得到的 binary64 位模式保留，并以小端编码，因此正零和负零不同；schema 允许的非有限 payload 也不规范化。对符号敏感的 operation 因此不会在 semantic、optimized、plan 或 cache identity 阶段发生碰撞。

独立版本轴包括 WorkflowDocument schema、OperationTraits、operation-plugin C ABI、semantic IR、optimizer 规则、physical planner、安装 package/API 和 daemon IPC。WorkflowDocument schema 7、OperationTraits 29、package 0.37.0 与 Result ABI 7 表示当前各自契约；一个轴改变不表示其他轴兼容。内部 IR 和 plan 是进程内契约，不是 daemon wire format；跨发布版本没有内部读取兼容承诺。每个阶段保留 exact frozen registry 的私有 weak identity，防止 optimizer、planner 或 executor 使用来自另一 registry 的对象，即使 operation key 相同也不行；该运行时身份不进入 digest 或序列化数据。静态 prepared state 和 library pointer 不编码进 digest；按契约需要时，其推导元数据和声明的 workspace 上限会参与阶段身份。

缓存命中仍须校验 plan 和 currentness。嵌入方提供的 cache hit 也要重新验证；格式错误或过期的条目作为 miss 处理。阶段身份和校验细节见 [Compiler 和执行](../../kernel-architecture/Compiler-and-Execution.md)。

## 4. 后果与代价

Consumer 应分别维护自己公开的每种契约版本，并在源文档、trait、规则或目标能力变化时重建可丢弃的 plan。过期或无效 cache entry 会被拒绝或视为 miss。即使 workflow schema 不变，package API 仍可能发生不兼容变化。
