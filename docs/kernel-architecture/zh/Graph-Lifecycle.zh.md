# Graph 生命周期

## 1. 模块职责与所有权

`GraphContext` 持有复制的 `WorkflowDocument`、单调递增的 revision，以及供 snapshot 和编译阶段判断 currentness 的共享状态。构造时会以 revision 1 发布初始 source。调用方持有 context 对象，并确保正在调用的代码期间对象仍存活。Graph 构造仅保存 source；语义校验由编译阶段执行。

## 2. Snapshot 数据

```cpp
class GraphContext {
 public:
  GraphSnapshot snapshot() const;
  std::uint64_t replace(WorkflowDocument document);
};
```

`snapshot()` 捕获一致的 source 与 revision。`replace()` 先准备替换内容，再在 context 锁内发布。成功后 revision 前进，因此旧 snapshot 和 plan 会变为 stale。替换失败时，已发布的 source 与 revision 保持不变。

## 3. 状态转换与执行

```text
在 revision r 构造 -> snapshot(r) -> analyze / optimize / plan
            |                                      |
            +-- replace 成功 -> revision r+1      +-> execution 检查 currentness
            |                                                   |
            +-- 析构 -> snapshot 失效                            +-> Stale，丢弃结果

IRProgram -> bind_ir -------------+
.psir / .psxp -> import / load ---+-> 独立阶段，revision 1
                                  |      registry 存活期间保持 current
                                  +-> 使用同一冻结 registry 执行
```

Compiler stage 保留 currentness identity，但不会保留对 source document 的可变访问。`Compiler::analyze` 或 `Compiler::compile` 从 snapshot 构建的阶段记录该 snapshot 的 revision，其 `current()` 跟随该 snapshot。`optimize`、`plan` 和 `select_outputs` 复制父阶段的 currentness，因此从同一 snapshot 派生的所有阶段会一起变为 stale。若 execution 观察到替换或 context 析构，会返回 `Stale`；callback 完成后，最终结果发布前还会再次检查 currentness。

## 4. 独立阶段

`Compiler::bind_ir` 和 `CompilerArtifact` 导入函数产生不关联任何 `GraphContext` 的阶段。它们的 `revision()` 为 1，`current()` 表示其链接的冻结 `OperationRegistry` 是否仍存活。替换或销毁任何 `GraphContext` 都不影响它们。保存的 `.psir` 或 `.psxp` 文件不携带 snapshot、revision 或源活性；保存一个绑定 snapshot 的阶段后再加载，会得到 revision 为 1 的独立阶段。

每个阶段（无论独立还是绑定 snapshot）都持有其 registry 的弱引用。`optimize` 和 `plan` 以 `Stale` 拒绝链接到其他 registry 的阶段；`ExecutionContext::execute`、`freeze` 和 `open_demand` 以 `Stale` 拒绝 registry 与 context 不同的 plan。因此加载产物的宿主需要注册或加载所需算子、冻结 registry、在同一 registry 上构建 `Compiler` 和 `ExecutionContext`，并在阶段使用期间保持 registry 存活。

## 5. 错误与生命周期

替换分配失败或 revision 溢出时，不改变已发布状态。Context 析构会将现存 snapshot 标为 non-current。析构不会拥有或停止 `ExecutionContext`；调用方需按普通 C++ 生命周期规则，保证执行中的调用所用对象仍然有效。

## 6. 限制与非目标

Kernel 不为 `GraphContext` 提供 document filesystem adapter、隐式工作目录、持久 graph identifier 或持久化服务。Compiler 产物文件只持久化 IR 与 plan 阶段，只读写调用方给出的显式路径。`GraphContext` revision 是本地 compiler 状态，不是 daemon Session 标识。
