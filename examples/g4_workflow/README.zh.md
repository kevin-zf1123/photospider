# 区域依赖 workflow

当前 data 场景通过公开 WorkflowDocument、Compiler、ExecutionContext，在十亿元素
逻辑源上执行 identity workflow。它使用既有区域执行器分别请求坐标 1 和 999999998，
构造精确 ValueFragments，检查中间空洞读取失败、逐输出证书转置正确，以及 generic
Int64 snapshot 导入成功。真实 source 只读取两个样本。此场景验证稀疏区域数据契约。

在仓库中运行：

```sh
cmake --build build/issue257-static --target photospider_dependency_workflow -j 8
build/issue257-static/examples/g4_workflow/photospider_dependency_workflow
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
out/g4-consumer-static/photospider_dependency_workflow
```

CTest 注册名为 `test_dependency_workflow`，不再使用历史 G4 开发阶段名。
同一可执行文件还检查 progressive 控制依赖、动态 radius、demand 替换、waiter
共享、内容缓存、有序归约/扫描及块状态重收敛。预期值与各场景见英文说明。
旧 generic-image STMap 正例及其可选计时入口已退休：结构图像必须使用 planar
绑定，`image.stmap` 尚未实现该执行能力，因此不声称 planar STMap 验收完成。
当前图像绑定拒绝由 `test_planar_image_workflow` 与 `test_semantic_contract` 覆盖。
