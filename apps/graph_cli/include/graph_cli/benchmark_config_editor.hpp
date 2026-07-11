#pragma once

#include <string>

namespace ps {
class Host;
}

namespace ps {

/**
 * @brief 启动基准测试配置的 TUI 编辑器。
 *
 * @param svc Host used to read available operation types.
 * @param benchmark_dir 包含 benchmark_config.yaml 的目录路径。
 * @return Nothing.
 * @throws std::bad_alloc if path, YAML, Host-result, or TUI allocation fails.
 * @throws std::filesystem::filesystem_error if directory creation fails.
 * @note The editor synchronously borrows svc and does not retain it after the
 * fullscreen event loop exits.
 */
void run_benchmark_config_editor(ps::Host& svc,
                                 const std::string& benchmark_dir);

}  // namespace ps
