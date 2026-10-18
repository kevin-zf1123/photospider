// 非必要示例optimizer, planner阶段也会去重
#include <map>
#include <string>
#include <utility>

#include "compiler/dependency_paths.hpp"
#include "compiler/optimizer_internal.hpp"

namespace ps::compiler_internal {
OptimizerDefinition dependency_path_optimizer() {
  return {
      "optimize",
      "1",
      "Optional dependency-path deduplication example",
      {},
      [](const ValidatedIR& ir, const std::map<std::string, ParameterValue>&) {
        auto program = ir.program();
        program.blocks = lower_dependency_paths(program.blocks, program.form);
        if (program.form == IRForm::DependencyPaths)
          program.entry = program.blocks.front().id;
        program.form = IRForm::ControlFlow;
        return Result<IRProgram>(std::move(program));
      }};
}
}  // namespace ps::compiler_internal
