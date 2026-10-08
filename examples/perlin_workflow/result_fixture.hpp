#pragma once

#include <cstring>
#include <stdexcept>
#include <vector>

#include "../numeric_workflow/result_fixture.hpp"

namespace perlin_fixture {
inline void declare(ps::WorkflowDocument* document, const ps::Value& input) {
  numeric_result_fixture::declare_sources(document, {input});
  document->inputs[0].name = "coordinates";
}
inline ps::ExecutionBindings bind(const ps::ResourceBudget& root,
                                  const ps::Value& input) {
  return {{{"coordinates", numeric_result_fixture::source(root, input)}}};
}
}  // namespace perlin_fixture
