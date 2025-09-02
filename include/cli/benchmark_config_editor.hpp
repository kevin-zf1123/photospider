#pragma once

#include <string>

namespace ps {
    class InteractionService;
}

namespace ps {

/**
 * @brief 启动基准测试配置的 TUI 编辑器。
 * 
 * @param svc The interaction service to get ops list from.
 * @param benchmark_dir 包含 benchmark_config.yaml 的目录路径。
 */
void run_benchmark_config_editor(ps::InteractionService& svc, const std::string& benchmark_dir);

} // namespace ps