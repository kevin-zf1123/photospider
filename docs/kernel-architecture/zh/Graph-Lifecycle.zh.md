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
```

Compiler stage 保留 currentness identity，但不会保留对 source document 的可变访问。若 execution 观察到替换或 context 析构，会返回 `Stale`；callback 完成后，最终结果发布前还会再次检查 currentness。

## 4. 错误与生命周期

替换分配失败或 revision 溢出时，不改变已发布状态。Context 析构会将现存 snapshot 标为 non-current。析构不会拥有或停止 `ExecutionContext`；调用方需按普通 C++ 生命周期规则，保证执行中的调用所用对象仍然有效。

## 5. 限制与非目标

Kernel 不提供 document filesystem adapter、隐式工作目录、持久 graph identifier 或持久化服务。`GraphContext` revision 是本地 compiler 状态，不是 daemon Session 标识。
