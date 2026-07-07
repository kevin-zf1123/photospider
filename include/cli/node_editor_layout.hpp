#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace ps::cli::node_editor_layout {

constexpr int kNodePaneMinWidth = 10;
constexpr int kNodePaneMaxWidth = 18;
constexpr int kYamlPaneMinWidth = 40;
constexpr int kDependencyTreePaneMinWidth = 24;
constexpr int kContainsTreePaneMinWidth = 36;

constexpr int kNodesFocusIndex = 0;
constexpr int kYamlFocusIndex = 1;
constexpr int kDependencyTreeFocusIndex = 2;
constexpr int kContainsTreeFocusIndex = 3;

constexpr const char* kNodesTitle = "Nodes";
constexpr const char* kYamlPaneTitle = "Node YAML";
constexpr const char* kDependencyTreePaneTitle = "Tree (dependency)";
constexpr const char* kPrimaryHelpText =
    "Keys: Tab focus | v edit YAML in $EDITOR/vim | Ctrl+S/Enter apply | q "
    "quit";
constexpr const char* kTreeHelpText =
    "Tree: t show/hide params | c contains/as-end | [ ] switch containing "
    "end tree | arrows scroll focused tree";

inline int node_pane_width(const std::vector<std::string>& entries) {
  int max_entry_width = 1;
  for (const auto& entry : entries) {
    max_entry_width = std::max(max_entry_width, static_cast<int>(entry.size()));
  }

  const int title_width = static_cast<int>(std::string(kNodesTitle).size());
  // Two border pairs are present: the outer node pane border and menu frame.
  const int required_width = std::max(title_width, max_entry_width) + 4;
  return std::clamp(required_width, kNodePaneMinWidth, kNodePaneMaxWidth);
}

}  // namespace ps::cli::node_editor_layout
