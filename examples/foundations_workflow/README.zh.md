# Foundations 工作流示例

此 C++17 示例使用 `Photospider::kernel`，运行 `numeric`、`expression-lut` 和 `generator-gain` 三个场景。默认选择器 `all` 会运行全部三个场景。`generator-gain` 是显式手动 workflow，没有单独的 CTest 注册。

| 场景 | 独立核验结果 |
| --- | --- |
| `numeric` | `[3,2,1] - [4,4,4] = [-1,-2,-3]`；`[1,2,3]` 的均值为 `2`，方差为 `2/3`。 |
| `expression-lut` | 采样 `x^2` 得到 `[0,0.25,1]`；线性 LUT 在 `0.25` 处得到 `0.125`。 |
| `generator-gain` | 复用同一编译后的图像增益 workflow，绑定新的系数 Result，并检查并发绑定；无效和非有限增益会失败。 |

三个场景都声明 Result workflow 输入并读取返回的 Result 输出。Helper 使用 `Value` 仅构造私有 packed fixture 数据；随后在 execution context 的 Root resource budget 下发布源 Result。`numeric` 和 `expression-lut` 会在执行后检查返回 tensor；即使 execution context 已销毁，两个场景的 Result 仍可读取。`generator-gain` 对顺序和并发系数绑定复用同一 compiled plan。当 gain 与 image 两个 Result owner 都保持存活时，相同 frozen request 会复用 gain Result，且没有 producer timings；无效和 NaN 系数仍会失败。

在仓库根目录构建并运行：

```sh
cmake --build build/kernel-dev --target photospider_foundations_workflow -j8
build/kernel-dev/examples/foundations_workflow/photospider_foundations_workflow --scenario expression-lut
build/kernel-dev/examples/foundations_workflow/photospider_foundations_workflow --scenario generator-gain
build/kernel-dev/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build/kernel-dev -R '^(test_workflow_numeric_reductions|test_workflow_expression_lut)$' --output-on-failure
```

可传入 `--scenario numeric`、`--scenario expression-lut` 或 `--scenario generator-gain` 单独运行。三个场景均通过后，程序打印 `Foundations scenarios=3 oracle=passed backend=cpu`。CTest 注册 `numeric` 和 expression/LUT 两个场景；要检查 plan reuse 和并发绑定，请显式运行 `generator-gain`。发生错误时打印 `Foundations failed: ...` 并以状态码 1 退出。

独立 CMake 项目要求已安装 Photospider 0.30。配置示例：

```sh
cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/foundations-consumer -j8
build/foundations-consumer/photospider_foundations_workflow --scenario all
```

Consumer 只链接 `Photospider::kernel`。源码级 Result 检查和两个已注册 CTest 覆盖 `numeric`、`expression-lut` 场景；`generator-gain` 通过显式手动运行检查。这些 workflow 不验证其他算子族或原生 GPU 执行。

[英文说明](README.md)为权威来源。
