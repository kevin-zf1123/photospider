// tui_editor.cpp - Fully updated to handle dirty flag correctly, full-node YAML
// editing, XY scrolling, and external editor integration using
// ScreenInteractive::WithRestoredIO.

#include "cli/tui_editor.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "kernel/services/graph_traversal_service.hpp"

using namespace ftxui;

namespace ps {

// -----------------------------------------------------------------------------
// Helpers to gather node ids and pretty titles
// -----------------------------------------------------------------------------
static std::vector<int> SortedNodeIds(const GraphModel& g) {
  GraphTraversalService traversal;
  std::set<int> s;
  auto ends = traversal.ending_nodes(g);
  for (int end_id : ends) {
    auto order = traversal.topo_postorder_from(g, end_id);
    s.insert(order.begin(), order.end());
    s.insert(end_id);
  }
  std::vector<int> ids(s.begin(), s.end());
  std::sort(ids.begin(), ids.end());
  return ids;
}

static std::string NodeTitle(const ps::Node& n) {
  std::ostringstream os;
  os << "[" << n.id << "] " << n.name << "  (" << n.type << ":" << n.subtype
     << ")";
  return os.str();
}

// -----------------------------------------------------------------------------
// Full-node YAML (serialize / apply)
// -----------------------------------------------------------------------------
static std::string NodeToYAML(const ps::Node& n) {
  YAML::Node root;
  root["id"] = n.id;
  root["name"] = n.name;
  root["type"] = n.type;
  root["subtype"] = n.subtype;

  if (!n.image_inputs.empty()) {
    YAML::Node arr(YAML::NodeType::Sequence);
    for (auto& e : n.image_inputs) {
      YAML::Node x;
      x["from_node_id"] = e.from_node_id;
      if (!e.from_output_name.empty())
        x["from_output_name"] = e.from_output_name;
      arr.push_back(x);
    }
    root["image_inputs"] = arr;
  }
  if (!n.parameter_inputs.empty()) {
    YAML::Node arr(YAML::NodeType::Sequence);
    for (auto& p : n.parameter_inputs) {
      YAML::Node x;
      x["from_node_id"] = p.from_node_id;
      if (!p.from_output_name.empty())
        x["from_output_name"] = p.from_output_name;
      x["to_parameter_name"] = p.to_parameter_name;
      arr.push_back(x);
    }
    root["parameter_inputs"] = arr;
  }
  if (n.parameters)
    root["parameters"] = n.parameters;

  // NOTE: caches omitted here to avoid type dependency mismatches across
  // builds.

  std::stringstream ss;
  ss << root;
  return ss.str();
}

static bool ApplyYAMLToFullNode(ps::Node& n, const std::string& yaml_text,
                                std::string& err) {
  try {
    YAML::Node root = YAML::Load(yaml_text);
    if (!root || !root.IsMap()) {
      err = "Node YAML must be a mapping";
      return false;
    }

    if (root["name"])
      n.name = root["name"].as<std::string>();
    if (root["type"])
      n.type = root["type"].as<std::string>();
    if (root["subtype"])
      n.subtype = root["subtype"].as<std::string>();

    n.image_inputs.clear();
    if (root["image_inputs"] && root["image_inputs"].IsSequence()) {
      for (auto x : root["image_inputs"]) {
        ps::ImageInput ii;
        ii.from_node_id = x["from_node_id"].as<int>();
        if (x["from_output_name"])
          ii.from_output_name = x["from_output_name"].as<std::string>();
        n.image_inputs.push_back(ii);
      }
    }

    n.parameter_inputs.clear();
    if (root["parameter_inputs"] && root["parameter_inputs"].IsSequence()) {
      for (auto x : root["parameter_inputs"]) {
        ps::ParameterInput pi;
        pi.from_node_id = x["from_node_id"].as<int>();
        if (x["from_output_name"])
          pi.from_output_name = x["from_output_name"].as<std::string>();
        pi.to_parameter_name = x["to_parameter_name"].as<std::string>();
        n.parameter_inputs.push_back(pi);
      }
    }

    if (root["parameters"])
      n.parameters = root["parameters"];
    else
      n.parameters = YAML::Node();

    // caches writeback intentionally omitted (keeps compatibility regardless of
    // CacheSpec type)

    return true;
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

// -----------------------------------------------------------------------------
// Tree pane state and rendering with XY scrolling
// -----------------------------------------------------------------------------
struct TreePaneState {
  std::string text;
  int cursor_row = 0;  // highlighted row (absolute index in lines)
  int scroll_y = 0;    // top line index in view
  int scroll_x = 0;    // left column index in view
};

static size_t LineCount(const std::string& s) {
  return std::count(s.begin(), s.end(), '\n') + 1;
}

// --- MODIFICATION: The hardcoded height limit is removed.
static Element DrawTreePaneContent(const TreePaneState& pane) {
  std::stringstream ss(pane.text);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(ss, line))
    lines.push_back(line);

  const int view_cols = 80;

  int start_row =
      std::clamp(pane.scroll_y, 0, std::max(0, (int)lines.size() - 1));
  // The loop now renders all lines from the current scroll position to the end.
  // The parent `frame` component will handle clipping and scrolling.
  int end_row = lines.size();

  Elements rows;
  for (int i = start_row; i < end_row; ++i) {
    int start_col = std::clamp(pane.scroll_x, 0,
                               (int)std::max<int>(0, (int)lines[i].size()));
    std::string slice = (start_col < (int)lines[i].size())
                            ? lines[i].substr(start_col, view_cols)
                            : "";
    auto row = text(slice);
    if (i == pane.cursor_row)
      row = row | inverted;
    rows.push_back(row);
  }

  return vbox(std::move(rows));
}

// -----------------------------------------------------------------------------
// Node picker (opened when `node` command has no id)
// -----------------------------------------------------------------------------
class NodePicker {
public:
  explicit NodePicker(GraphModel& graph) : graph_(graph) {
    ids_ = SortedNodeIds(graph_);
    entries_.reserve(ids_.size());
    for (int id : ids_)
      entries_.push_back(std::to_string(id));

    list_ = Menu(&entries_, &selected_index_);

    root_ = Renderer(list_, [&] {
      auto header =
          text(
              "Select a node — ↑↓ choose, <enter> open, <esc>/<Ctrl-C> abort") |
          center;
      return vbox({header, separator(), list_->Render() | frame | border}) |
             size(WIDTH, GREATER_THAN, 40) | size(HEIGHT, GREATER_THAN, 12);
    });

    root_ = CatchEvent(root_, [&](Event e) {
      if (e == Event::Return) {
        accepted_ = true;
        screen_->Exit();
        return true;
      }
      if (e == Event::Escape || e == Event::Special({"C-c"})) {
        accepted_ = false;
        screen_->Exit();
        return true;
      }
      return false;
    });
  }

  std::optional<int> Pick(ScreenInteractive& screen) {
    screen_ = &screen;
    screen.Loop(root_);
    if (!accepted_ || selected_index_ < 0 ||
        selected_index_ >= (int)ids_.size())
      return std::nullopt;
    return ids_[selected_index_];
  }

private:
  GraphModel& graph_;
  std::vector<int> ids_;
  std::vector<std::string> entries_;
  int selected_index_ = 0;
  bool accepted_ = false;
  ScreenInteractive* screen_ = nullptr;
  Component list_;
  Component root_;
};

// -----------------------------------------------------------------------------
// Main editor UI
// -----------------------------------------------------------------------------
class NodeEditorUI {
public:
  NodeEditorUI(GraphModel& graph, int node_id)
      : graph_(graph), active_node_id_(node_id) {
    ids_ = SortedNodeIds(graph_);
    auto it = std::find(ids_.begin(), ids_.end(), node_id);
    node_list_index_ =
        (it == ids_.end()) ? 0 : int(std::distance(ids_.begin(), it));

    BuildComponents();
    RefreshTrees();
    LoadEditorFromCurrentNode();
    // init previous toggle state (for instant refresh)
    prev_tree_context_mode_index_ = tree_context_mode_index_;
    prev_detail_mode_index_ = detail_mode_index_;
  }

  void Run(ScreenInteractive& screen) {
    screen_ = &screen;
    screen.Loop(root_);
  }

private:
  enum FocusPane {
    NODE_LIST = 0,
    NODE_PARAM = 1,
    TREE_FULL = 2,
    TREE_CONTEXT = 3
  };

  void BuildComponents() {
    auto focus_catcher = [this](FocusPane target_pane) {
      return [this, target_pane](Event event) {
        if (event.is_mouse() && event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Pressed) {
          focus_ = target_pane;  // focus the pane only
          return true;           // consume click
        }
        return false;
      };
    };
    // Node list entries
    node_entries_.clear();
    for (int id : ids_)
      node_entries_.push_back(NodeTitle(graph_.nodes.at(id)));

    MenuOption menu_option;
    menu_option.entries = &node_entries_;
    menu_option.selected = &node_list_index_;
    menu_option.on_change = [this] {
      if (!dirty_)
        OnNodeListChanged();
    };
    node_list_ = Menu(menu_option);
    // The panel itself will catch the event, not the component inside.

    // Editor
    input_opt_.on_change = [&] {
      dirty_ = (editor_text_ != last_applied_text_);
    };
    input_opt_.multiline = true;
    editor_input_ = Input(&editor_text_, input_opt_);

    btn_apply_ = Button("Apply (Ctrl+S)", [&] { ApplyEditor(); });
    btn_discard_ = Button("Discard (Ctrl+D)", [&] { DiscardEditor(); });
    btn_extedit_ = Button("Open in $EDITOR (Ctrl+E)", [&] { EditExternal(); });

    // Right side toggles and tree panes
    toggle_tree_mode_ = Toggle(&tree_context_modes_, &tree_context_mode_index_);
    toggle_detail_ = Toggle(&detail_modes_, &detail_mode_index_);

    tree_full_component_ =
        Renderer([&] { return DrawTreePaneContent(tree_full_); });
    tree_context_component_ =
        Renderer([&] { return DrawTreePaneContent(tree_context_); });

    left_column_ = Renderer(
        Container::Vertical(
            {node_list_, editor_input_,
             Container::Horizontal({btn_apply_, btn_discard_, btn_extedit_})}),
        [&] {
          Component list_pane_c = Renderer([&] {
            return vbox({text("Nodes — ↑↓ select, click to focus") | bold,
                         node_list_->Render() | frame |
                             size(HEIGHT, EQUAL, 12)}) |
                   (focus_ == NODE_LIST ? borderHeavy : border);
          });
          list_pane_c = list_pane_c | CatchEvent(focus_catcher(NODE_LIST));

          Component editor_pane_c = Renderer([&] {
            return vbox(
                       {text("Editor (full node YAML) — click to focus") | bold,
                        editor_input_->Render() | frame |
                            size(HEIGHT, GREATER_THAN, 8),
                        separator(),
                        hbox({btn_apply_->Render(), separator(),
                              btn_discard_->Render(), separator(),
                              btn_extedit_->Render()})}) |
                   (focus_ == NODE_PARAM ? borderHeavy : border);
          });
          editor_pane_c = editor_pane_c | CatchEvent(focus_catcher(NODE_PARAM));

          auto tips = text(
                          "Global: <tab>=focus, <esc>/<Ctrl-C>=exit, "
                          "Ctrl+S/D/E=actions") |
                      dim;
          return vbox({list_pane_c->Render(), separator(),
                       editor_pane_c->Render(), separator(), tips});
        });

    right_column_ = Renderer(
        Container::Vertical(
            {Container::Horizontal({toggle_tree_mode_, toggle_detail_}),
             Container::Horizontal(
                 {tree_full_component_, tree_context_component_})}),
        [&] {
          auto toggles =
              hbox({text("Context: "), toggle_tree_mode_->Render(), separator(),
                    text("View: "), toggle_detail_->Render()});

          Component full_pane_c = Renderer([&] {
            auto full_content = tree_full_component_->Render();
            return window(text("Whole Graph") | bold, full_content,
                          focus_ == TREE_FULL ? HEAVY : ROUNDED);
          });
          full_pane_c = full_pane_c | CatchEvent(focus_catcher(TREE_FULL));

          Component context_pane_c = Renderer([&] {
            auto context_content = tree_context_component_->Render();
            return window(text(ContextTreeHeader()) | bold, context_content,
                          focus_ == TREE_CONTEXT ? HEAVY : ROUNDED);
          });
          context_pane_c =
              context_pane_c | CatchEvent(focus_catcher(TREE_CONTEXT));

          auto panes = hbox({full_pane_c->Render() | flex, separator(),
                             context_pane_c->Render() | flex});
          return vbox({toggles, separator(), panes | flex});
        });

    main_container_ = Container::Horizontal({left_column_, right_column_});

    root_ = CatchEvent(
        Renderer(main_container_,
                 [&] {
                   return hbox({left_column_->Render() | flex, separator(),
                                right_column_->Render() | flex});
                 }),
        [&](Event e) {
          if (e == Event::Tab) {
            CycleFocus();
            return true;
          }
          if (e == Event::Escape || e == Event::Special({"C-c"})) {
            screen_->Exit();
            return true;
          }
          if (e == Event::Special({"C-s"})) {
            ApplyEditor();
            return true;
          }
          if (e == Event::Special({"C-d"})) {
            DiscardEditor();
            return true;
          }
          if (e == Event::Special({"C-e"})) {
            EditExternal();
            return true;
          }

          if (focus_ == TREE_FULL || focus_ == TREE_CONTEXT) {
            TreePaneState& pane =
                (focus_ == TREE_FULL) ? tree_full_ : tree_context_;
            if (e == Event::ArrowUp) {
              pane.cursor_row = std::max(0, pane.cursor_row - 1);
              pane.scroll_y = std::max(0, pane.scroll_y - 1);
              return true;
            }
            if (e == Event::ArrowDown) {
              pane.cursor_row =
                  std::min((int)LineCount(pane.text) - 1, pane.cursor_row + 1);
              pane.scroll_y += 1;
              return true;
            }
            if (e == Event::ArrowLeft) {
              pane.scroll_x = std::max(0, pane.scroll_x - 1);
              return true;
            }
            if (e == Event::ArrowRight) {
              pane.scroll_x += 1;
              return true;
            }
            if (e.is_mouse() && e.mouse().button == Mouse::WheelUp) {
              pane.scroll_y = std::max(0, pane.scroll_y - 1);
              return true;
            }
            if (e.is_mouse() && e.mouse().button == Mouse::WheelDown) {
              pane.scroll_y += 1;
              return true;
            }
          }

          if (tree_context_mode_index_ != prev_tree_context_mode_index_ ||
              detail_mode_index_ != prev_detail_mode_index_) {
            RefreshTrees();
            prev_tree_context_mode_index_ = tree_context_mode_index_;
            prev_detail_mode_index_ = detail_mode_index_;
            return true;
          }
          return false;
        });
  }

  std::string ContextTreeHeader() const {
    return tree_context_mode_index_ == 0 ? "Tree from node"
                                         : "Trees containing node";
  }

  void CycleFocus() {
    focus_ = static_cast<FocusPane>((static_cast<int>(focus_) + 1) % 4);
  }

  void RefreshTrees() {
    const bool detailed = (detail_mode_index_ == 0);  // 0=Full, 1=Simplified
    GraphTraversalService traversal;
    tree_full_.text = [&] {
      std::stringstream ss;
      traversal.print_dependency_tree(graph_, ss, detailed);
      return ss.str();
    }();

    if (tree_context_mode_index_ == 0) {
      std::stringstream ss;
      traversal.print_dependency_tree(graph_, ss, active_node_id_, detailed);
      tree_context_.text = ss.str();
    } else {
      std::stringstream ss;
      auto trees = traversal.get_trees_containing_node(graph_, active_node_id_);
      for (size_t i = 0; i < trees.size(); ++i) {
        ss << "[Tree #" << (i + 1) << "] ending at node " << trees[i] << "\n";
        traversal.print_dependency_tree(graph_, ss, trees[i], detailed);
        ss << "\n";
      }
      tree_context_.text = ss.str();
    }
  }

  void LoadEditorFromCurrentNode() {
    const auto& n = graph_.nodes.at(ids_[node_list_index_]);
    active_node_id_ = n.id;
    editor_text_ = NodeToYAML(n);
    last_applied_text_ = editor_text_;
    dirty_ = false;
    // Reset scrolling when switching nodes
    tree_context_.scroll_x = 0;
    tree_context_.scroll_y = 0;
    tree_context_.cursor_row = 0;
    RefreshTrees();
  }

  void OnNodeListChanged() { LoadEditorFromCurrentNode(); }

  void ApplyEditor() {
    auto& n = graph_.nodes.at(active_node_id_);
    std::string err;
    if (!ApplyYAMLToFullNode(n, editor_text_, err)) {
      ShowToast(std::string("Invalid YAML: ") + err);
      return;
    }
    last_applied_text_ = editor_text_;
    dirty_ = false;
    ShowToast("Applied");
    RefreshTrees();
  }

  void DiscardEditor() {
    LoadEditorFromCurrentNode();
    ShowToast("Discarded");
  }

  void EditExternal() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() /
                   ("ps_node_" + std::to_string(active_node_id_) + ".yaml");
    {
      std::ofstream ofs(tmp);
      ofs << editor_text_;
    }
    const char* ed = std::getenv("EDITOR");
    std::string editor = ed ? ed : "nano";
    if (screen_) {
      screen_->WithRestoredIO([&] {
        std::system((editor + std::string(" ") + tmp.string()).c_str());
      })();
    } else {
      std::system((editor + std::string(" ") + tmp.string()).c_str());
    }
    std::ifstream ifs(tmp);
    std::stringstream ss;
    ss << ifs.rdbuf();
    editor_text_ = ss.str();
    dirty_ = (editor_text_ != last_applied_text_);
  }

  void ShowToast(const std::string& msg) {
    status_ = msg; /* could be extended to timed banner */
  }

private:
  GraphModel& graph_;
  ScreenInteractive* screen_ = nullptr;

  // IDs
  std::vector<int> ids_;
  std::vector<std::string> node_entries_;
  int node_list_index_ = 0;
  int active_node_id_ = -1;

  // Components (left)
  Component node_list_;
  Component editor_input_;
  InputOption input_opt_{};
  Component btn_apply_;
  Component btn_discard_;
  Component btn_extedit_;
  std::string editor_text_;
  std::string last_applied_text_;
  bool dirty_ = false;

  // Components (right)
  TreePaneState tree_full_{};
  TreePaneState tree_context_{};
  std::vector<std::string> tree_context_modes_{"From node", "Trees containing"};
  int tree_context_mode_index_ = 0;
  std::vector<std::string> detail_modes_{"Full", "Simplified"};
  int detail_mode_index_ = 0;
  int prev_tree_context_mode_index_ = -1;
  int prev_detail_mode_index_ = -1;
  Component toggle_tree_mode_;
  Component toggle_detail_;
  Component tree_full_component_;
  Component tree_context_component_;

  // Focus
  FocusPane focus_ = NODE_LIST;

  // Layout
  Component left_column_;
  Component right_column_;
  Component main_container_;
  Component root_;

  std::string status_;
};

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------
void run_node_editor(GraphModel& graph, std::optional<int> initial_id) {
  ScreenInteractive screen = ScreenInteractive::Fullscreen();
  int start_id = -1;
  if (!initial_id.has_value()) {
    NodePicker picker(graph);
    auto chosen = picker.Pick(screen);
    if (!chosen.has_value())
      return;  // aborted
    start_id = *chosen;
  } else {
    start_id = *initial_id;
  }

  NodeEditorUI ui(graph, start_id);
  ui.Run(screen);
}

}  // namespace ps
