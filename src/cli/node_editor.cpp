// Minimal FTXUI-based node editor using InteractionService
#include "cli/node_editor.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "kernel/interaction.hpp"

using namespace ftxui;

static std::string to_string_list(const std::vector<int>& v) {
  std::ostringstream os;
  bool first = true;
  for (int x : v) {
    if (!first)
      os << ", ";
    os << x;
    first = false;
  }
  return os.str();
}

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
  int focus_index = 0;     // 0:menu,1:editor,2:tree1,3:tree2
  int tree1_v = 0, tree1_h = 0, tree2_v = 0, tree2_h = 0;
  auto load_current = [&] {
    auto y = svc.cmd_get_node_yaml(graph_name, ids[selected_index]);
    editor_text = y.value_or("# failed to load node\n");
    last_saved_text = editor_text;
    dirty = false;
    // Refresh trees
    auto t1 =
        svc.cmd_dump_tree(graph_name, ids[selected_index], tree_show_params);
    tree1_text = t1.value_or("(failed to dump tree)\n");
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
        return svc.cmd_dump_tree(graph_name, end_id, tree_show_params);
      } else {
        return svc.cmd_dump_tree(graph_name, ids[selected_index],
                                 tree_show_params);
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
  auto textarea = Input(&editor_text, "node yaml");
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
        vbox({text("Tree (dependency)") | bold, separator(),
              paragraph(text_with_offsets(tree1_text, tree1_v, tree1_h)) |
                  frame | flex});
    auto el = box | border;
    if (focus_index == 2)
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
    if (focus_index == 3)
      el = el | inverted | bold;
    return el;
  });
  auto root = Container::Horizontal({menu, textarea, tree1_view, tree2_view});
  auto renderer = Renderer(root, [&] {
    auto nodes_box =
        vbox({text("Nodes") | bold, separator(),
              menu->Render() | frame | size(WIDTH, LESS_THAN, 20)});
    auto nodes_el = nodes_box | border;
    if (focus_index == 0)
      nodes_el = nodes_el | inverted | bold;
    auto editor_box = vbox({text("YAML (Ctrl+S save, Ctrl+Q quit, Enter "
                                 "applies, v:vim, t:toggle full/simplified) ") |
                                dim,
                            textarea->Render() | frame | flex});
    auto editor_el = editor_box | border;
    if (focus_index == 1)
      editor_el = editor_el | inverted | bold;
    return hbox({nodes_el, separator(), editor_el, separator(),
                 // tree1 and tree2 are already bordered/focus-styled inside
                 // their renderers
                 tree1_view->Render(), separator(), tree2_view->Render()});
  });

  renderer |= CatchEvent([&](Event e) {
    if (e == Event::Character('q') || e == Event::Special({"C-q"})) {
      screen.Exit();
      return true;
    }
    if (e == Event::Tab) {
      focus_index = (focus_index + 1) % 4;
      if (focus_index == 0)
        menu->TakeFocus();
      else if (focus_index == 1)
        textarea->TakeFocus();
      else if (focus_index == 2)
        tree1_view->TakeFocus();
      else
        tree2_view->TakeFocus();
      return true;
    }
    if (e == Event::Return) {  // Apply & validate & persist
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
    }
    if (e == Event::Special({"C-s"})) {  // Save (same as apply here)
      return renderer->OnEvent(Event::Return);
    }
    if (e == Event::Character('v')) {  // Open in vim (or $EDITOR)
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
    }
    if (e == Event::Character('t')) {  // toggle tree mode
      tree_show_params = !tree_show_params;
      load_current();
      return true;
    }
    if (e == Event::Character('c')) {  // toggle tree2 mode
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
    if (focus_index == 2) {
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
    if (focus_index == 3) {
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

  // Track dirty state in editor
  textarea |= CatchEvent([&](Event e) {
    if (e.is_character() || e == Event::Backspace || e == Event::Delete) {
      dirty = (editor_text != last_saved_text);
    }
    return false;
  });

  screen.Loop(renderer);
}
