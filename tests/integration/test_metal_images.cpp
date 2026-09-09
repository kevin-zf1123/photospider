#include <iostream>
#include <memory>

#include "photospider/photospider.hpp"
#include "s4_gpu_workflow/image_fixture.hpp"

int main(int argc, char** argv) {
  try {
    auto operations = argc > 1 ? std::make_shared<ps::OperationRegistry>()
                               : ps::make_default_operation_registry();
    if (argc > 1) {
      s4_fixture::require(operations->load_plugin(argv[1]).ok(),
                          "plugin load failed");
      s4_fixture::require(operations->freeze().ok(), "plugin freeze failed");
    }
    ps::ExecutionContextConfig config;
    config.gpu_enabled = true;
    ps::ExecutionContext execution(operations, config);
    const auto dispatches = s4_fixture::all_operations(
        execution, operations, ps::ExecutionMode::MetalFp32);
    s4_fixture::numeric_edges(execution, operations);
    if (!execution.gpu_enabled()) {
      std::cout << "CPU fallback oracle passed; native hardware skipped\n";
      return 77;
    }
    std::cout << "all_operations=8 dispatches=" << dispatches
              << " oracle=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
