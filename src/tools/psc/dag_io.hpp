#pragma once
#include <string>

#include "photospider/compiler/workflow_document.hpp"
namespace ps::tools::psc {
std::string detect(const std::string& text);
std::string inspect_input(const std::string& text);
WorkflowDocument read_dag(const std::string& text);
}  // namespace ps::tools::psc
