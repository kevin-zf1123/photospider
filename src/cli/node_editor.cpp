// Minimal FTXUI-based node editor using InteractionService
#include "cli/node_editor.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "cli/dependency_tree_formatter.hpp"
#include "cli/node_editor_layout.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "kernel/interaction.hpp"

using namespace ftxui;
namespace node_layout = ps::cli::node_editor_layout;

void run_node_editor_decoupled(ps::InteractionService& svc,
                               const std::string& graph_name,
                               std::optional<int> initial_id) {
  auto screen = ScreenInteractive::Fullscreen();

  // Fetch node ids
  auto ids_opt = svc.cmd_list_node_ids(graph_name);
  if (!ids_opt || ids_opt->empty()) {
    std::cout << "No nodes available in current graph." << std::endl;
    return;
  }
  std::vector<int> ids = *ids_opt;
  std::vector<std::string> entries;
  entries.reserve(ids.size());
  for (int id : ids)
    entries.push_back(std::to_string(id));
  const int node_pane_width = node_layout::node_pane_width(entries);

  int selected_index = 0;
  if (initial_id) {
    auto it = std::find(ids.begin(), ids.end(), *initial_id);
    if (it != ids.end())
      selected_index = int(it - ids.begin());
  }

  std::string editor_text;
  std::string last_saved_text;
  bool dirty = false;

  // Tree panels
  std::string tree1_text;  // dependency of current node
  std::string tree2_text;  // second view: contains/end trees
  bool tree_show_params = true;
  enum class Tree2Mode { ContainsNode, AsEndNode };
  Tree2Mode tree2_mode = Tree2Mode::ContainsNode;
  std::vector<int> containing_ends;
  int containing_idx = 0;  // which end tree to show when in ContainsNode mode
  int focus_index = node_layout::kNodesFocusIndex;
  int tree1_v = 0, tree1_h = 0, tree2_v = 0, tree2_h = 0;
  auto load_current = [&] {
    auto y = svc.cmd_get_node_yaml(graph_name, ids[selected_index]);
    editor_text = y.value_or("# failed to load node\n");
    last_saved_text = editor_text;
    dirty = false;
    // Refresh trees
    auto t1 = svc.cmd_dependency_tree(graph_name, ids[selected_index]);
    tree1_text = t1 ? ps::cli::format_dependency_tree(*t1, tree_show_params)
                    : "(failed to dump tree)\n";
    containing_ends.clear();
    containing_idx = 0;
    auto ends = svc.cmd_trees_containing_node(graph_name, ids[selected_index]);
    if (ends && !ends->empty()) {
      containing_ends = *ends;
    }
    auto t2 = [&]() {
      if (tree2_mode == Tree2Mode::ContainsNode) {
        if (containing_ends.empty())
          return std::optional<std::string>(
              "(no end trees contain this node)\n");
        int end_id = containing_ends[std::min(containing_idx,
                                              (int)containing_ends.size() - 1)];
        auto tree = svc.cmd_dependency_tree(graph_name, end_id);
        if (!tree)
          return std::optional<std::string>();
        return std::optional<std::string>(
            ps::cli::format_dependency_tree(*tree, tree_show_params));
      } else {
        auto tree = svc.cmd_dependency_tree(graph_name, ids[selected_index]);
        if (!tree)
          return std::optional<std::string>();
        return std::optional<std::string>(
            ps::cli::format_dependency_tree(*tree, tree_show_params));
      }
    }();
    tree2_text = t2.value_or("(failed to dump tree)\n");
  };
  load_current();

  int last_selected_index = selected_index;
  MenuOption menu_opt;
  menu_opt.entries = &entries;
  menu_opt.selected = &selected_index;
  menu_opt.on_change = [&] {
    if (dirty) {
      // revert selection change
      selected_index = last_selected_index;
      return;
    }
    last_selected_index = selected_index;
    load_current();
  };
  auto menu = Menu(menu_opt);
  InputOption textarea_opt;
  textarea_opt.content = &editor_text;
  textarea_opt.placeholder = "node yaml";
  textarea_opt.multiline = true;
  textarea_opt.on_change = [&] { dirty = (editor_text != last_saved_text); };
  auto textarea = Input(textarea_opt);
  auto text_with_offsets = [&](const std::string& s, int voff, int hoff) {
    std::istringstream iss(s);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line))
      lines.push_back(line);
    std::string out;
    for (int i = std::max(0, voff); i < (int)lines.size(); ++i) {
      std::string ln = lines[i];
      if ((int)ln.size() > hoff)
        ln = ln.substr(hoff);
      else
        ln.clear();
      out += ln;
      out += "\n";
    }
    return out;
  };
  auto tree1_view = Renderer([&] {
    auto box =
        vbox({text(node_layout::kDependencyTreePaneTitle) | bold, separator(),
              paragraph(text_with_offsets(tree1_text, tree1_v, tree1_h)) |
                  frame | flex});
    auto el = box | border;
    if (focus_index == node_layout::kDependencyTreeFocusIndex)
      el = el | inverted | bold;
    return el;
  });
  auto tree2_view = Renderer([&] {
    std::string title =
        (tree2_mode == Tree2Mode::ContainsNode ? "Tree (contains)"
                                               : "Tree (as end)");
    if (tree2_mode == Tree2Mode::ContainsNode && !containing_ends.empty())
      title += " end=" + std::to_string(containing_ends[std::min(
                             containing_idx, (int)containing_ends.size() - 1)]);
    auto box =
        vbox({text(title) | bold, separator(),
              paragraph(text_with_offsets(tree2_text, tree2_v, tree2_h)) |
                  frame | flex});
    auto el = box | border;
    if (focus_index == node_layout::kContainsTreeFocusIndex)
      el = el | inverted | bold;
    return el;
  });
  auto root = Container::Horizontal({menu, textarea, tree1_view, tree2_view});
  auto renderer = Renderer(root, [&] {
    auto nodes_box = vbox({text(node_layout::kNodesTitle) | bold, separator(),
                           menu->Render() | frame | flex});
    auto nodes_el = nodes_box | border | size(WIDTH, EQUAL, node_pane_width);
    if (focus_index == node_layout::kNodesFocusIndex)
      nodes_el = nodes_el | inverted | bold;
    auto editor_box = vbox({text(node_layout::kYamlPaneTitle) | bold,
                            separator(), textarea->Render() | frame | flex});
    auto editor_el = editor_box | border;
    if (focus_index == node_layout::kYamlFocusIndex)
      editor_el = editor_el | inverted | bold;
    auto main_row = hbox(
        {nodes_el, separator(),
         editor_el | size(WIDTH, GREATER_THAN, node_layout::kYamlPaneMinWidth) |
             xflex,
         separator(),
         // tree1 and tree2 are already bordered/focus-styled inside
         // their renderers
         tree1_view->Render() |
             size(WIDTH, GREATER_THAN,
                  node_layout::kDependencyTreePaneMinWidth) |
             xflex,
         separator(),
         tree2_view->Render() |
             size(WIDTH, GREATER_THAN, node_layout::kContainsTreePaneMinWidth) |
             xflex});
    auto help = vbox({text(node_layout::kPrimaryHelpText),
                      text(node_layout::kTreeHelpText)}) |
                dim;
    return vbox({main_row | flex, separator(), help});
  });

  auto apply_current_editor = [&] {
    svc.cmd_set_node_yaml(graph_name, ids[selected_index], editor_text);
    // Validate acyclicity using traversal
    auto orders = svc.cmd_traversal_orders(graph_name);
    if (!orders) {
      // revert
      svc.cmd_set_node_yaml(graph_name, ids[selected_index], last_saved_text);
      editor_text = last_saved_text;
      dirty = false;
      return true;
    }
    // success
    last_saved_text = editor_text;
    dirty = false;
    // persist content.yaml
    std::string content_path =
        (std::filesystem::path("sessions") / graph_name / "content.yaml")
            .string();
    svc.cmd_save_yaml(graph_name, content_path);
    // refresh trees
    load_current();
    return true;
  };

  auto open_external_editor = [&] {
    // write temp file
    std::string tmp_path =
        (std::filesystem::path("sessions") / graph_name /
         ("node_" + std::to_string(ids[selected_index]) + ".yaml"))
            .string();
    {
      std::ofstream fout(tmp_path);
      fout << editor_text;
    }
    const char* editor = std::getenv("EDITOR");
    std::string cmd =
        std::string(editor ? editor : "vim") + " \"" + tmp_path + "\"";
    std::system(cmd.c_str());
    // read back
    std::ifstream fin(tmp_path);
    std::stringstream buffer;
    buffer << fin.rdbuf();
    std::string new_text = buffer.str();
    if (!new_text.empty() && new_text != editor_text) {
      editor_text = new_text;
      dirty = (editor_text != last_saved_text);
    }
    return true;
  };

  renderer |= CatchEvent([&](Event e) {
    if (e == Event::Character('q') || e == Event::Special({"C-q"})) {
      screen.Exit();
      return true;
    }
    if (e == Event::Tab) {
      focus_index = (focus_index + 1) % 4;
      if (focus_index == node_layout::kNodesFocusIndex)
        menu->TakeFocus();
      else if (focus_index == node_layout::kYamlFocusIndex)
        textarea->TakeFocus();
      else if (focus_index == node_layout::kDependencyTreeFocusIndex)
        tree1_view->TakeFocus();
      else
        tree2_view->TakeFocus();
      return true;
    }
    if (e == Event::Return) {
      return apply_current_editor();
    }
    if (e == Event::Special({"C-s"})) {
      return apply_current_editor();
    }
    if (e == Event::Character('v') || e == Event::Special({"C-e"})) {
      return open_external_editor();
    }
    if (e == Event::Character('t')) {
      tree_show_params = !tree_show_params;
      load_current();
      return true;
    }
    if (e == Event::Character('c')) {
      tree2_mode = (tree2_mode == Tree2Mode::ContainsNode)
                       ? Tree2Mode::AsEndNode
                       : Tree2Mode::ContainsNode;
      load_current();
      return true;
    }
    if (e == Event::Character('[')) {
      if (containing_idx > 0) {
        containing_idx--;
        load_current();
      }
      return true;
    }
    if (e == Event::Character(']')) {
      if (containing_idx + 1 < (int)containing_ends.size()) {
        containing_idx++;
        load_current();
      }
      return true;
    }
    // Tree scroll when focused
    if (focus_index == node_layout::kDependencyTreeFocusIndex) {
      if (e == Event::ArrowUp) {
        tree1_v = std::max(0, tree1_v - 1);
        return true;
      }
      if (e == Event::ArrowDown) {
        tree1_v += 1;
        return true;
      }
      if (e == Event::ArrowLeft) {
        tree1_h = std::max(0, tree1_h - 2);
        return true;
      }
      if (e == Event::ArrowRight) {
        tree1_h += 2;
        return true;
      }
    }
    if (focus_index == node_layout::kContainsTreeFocusIndex) {
      if (e == Event::ArrowUp) {
        tree2_v = std::max(0, tree2_v - 1);
        return true;
      }
      if (e == Event::ArrowDown) {
        tree2_v += 1;
        return true;
      }
      if (e == Event::ArrowLeft) {
        tree2_h = std::max(0, tree2_h - 2);
        return true;
      }
      if (e == Event::ArrowRight) {
        tree2_h += 2;
        return true;
      }
      return true;
    }
    return false;
  });

  // Update editor text when selection changes
  // Focus menu initially so arrow keys switch nodes
  menu->TakeFocus();

  screen.Loop(renderer);
}
