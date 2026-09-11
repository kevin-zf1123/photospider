# G4 workflow

当前 data 场景通过公开 WorkflowDocument、Compiler、ExecutionContext，在十亿元素
逻辑源上执行 identity workflow。它使用既有区域执行器分别请求坐标 1 和 999999998，
构造精确 ValueFragments，检查中间空洞读取失败、逐输出证书转置正确，以及 generic
Int64 snapshot 导入成功。真实 source 只读取两个样本。此场景验证数据基础；分阶段
调度场景随对应运行时实现加入。

在仓库中运行：

```sh
cmake --build build/issue257-static --target photospider_g4_workflow -j 8
build/issue257-static/examples/g4_workflow/photospider_g4_workflow
```

独立检查后的预期输出：

```text
data: values=[1,999999998], source_reads=2, hole=rejected, transpose={1}, generic_snapshot=ok
```

也可作为独立安装包消费者：

```sh
cmake --install build/issue257-static --prefix "$PWD/out/g4-install-static"
cmake -S examples/g4_workflow -B out/g4-consumer-static \
  -DCMAKE_PREFIX_PATH="$PWD/out/g4-install-static"
cmake --build out/g4-consumer-static -j 8
out/g4-consumer-static/photospider_g4_workflow
```
