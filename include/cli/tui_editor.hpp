// moved to include/cli/tui_editor.hpp
#pragma once
#include "ftxui/component/screen_interactive.hpp"

#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include "ps_types.hpp"
#include "node.hpp"
#include "node_graph.hpp"

// A base class for our interactive editors to share the screen instance.
class TuiEditor {
public:
    explicit TuiEditor(ftxui::ScreenInteractive& screen) : screen_(screen) {}
    virtual ~TuiEditor() = default;
    virtual void Run() = 0; // Each editor must implement its own Run loop.

protected:
    ftxui::ScreenInteractive& screen_;
};
namespace ps {

// Launch the node editor UI. If initial_id has value, open directly on that node.
// If not, open the node picker window first.
void run_node_editor(NodeGraph &graph, std::optional<int> initial_id);

} // namespace ps
