#include <exception>
#include <iostream>

#include "photospider/plugin/operation_registry.hpp"

int main() {
  try {
    const auto registry = ps::make_default_operation_registry();
    for (const auto& key : registry->keys())
      std::cout << key << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
