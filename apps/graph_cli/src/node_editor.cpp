#include "graph_cli/node_editor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "graph_cli/dependency_tree_formatter.hpp"
#include "graph_cli/node_editor_layout.hpp"
#include "photospider/host/host.hpp"

using namespace ftxui;  // NOLINT(build/namespaces)
namespace node_layout = ps::cli::node_editor_layout;

/** @copydoc run_node_editor_decoupled */
void run_node_editor_decoupled(ps::Host& svc, const std::string& graph_name,
                               std::optional<int> initial_id) {
  auto screen = ScreenInteractive::Fullscreen();

  // Fetch node ids
  auto ids_result = svc.list_node_ids(ps::GraphSessionId{graph_name});
  if (!ids_result.status.ok || ids_result.value.empty()) {
    std::cout << "No nodes available in current graph." << std::endl;
    return;
  }
  std::vector<int> ids;
  ids.reserve(ids_result.value.size());
  for (const auto& id : ids_result.value) {
    ids.push_back(id.value);
  }
  std::vector<std::string> entries;
  entries.reserve(ids.size());
  for (int id : ids)
    entries.push_back(std::to_string(id));
  const int node_pane_width = node_layout::node_pane_width(entries);

  int selected_index = 0;
  if (initial_id) {
    auto it = std::find(ids.begin(), ids.end(), *initial_id);
    if (it != ids.end())
      selected_index = static_cast<int>(it - ids.begin());
  }

  std::string editor_text;
  std::string last_saved_text;
  bool dirty = false;

  std::string tree1_text;
  std::string tree2_text;
  bool tree_show_params = true;

  /**
   * @brief Selects the relationship shown in the secondary tree pane.
   * @throws Nothing.
   * @note This event-loop-local state owns no Host or snapshot value.
   */
  enum class Tree2Mode {
    /** @brief Show an ending-node tree that contains the selected node. */
    ContainsNode,

    /** @brief Show the selected node as the dependency-tree root. */
    AsEndNode,
  };
  Tree2Mode tree2_mode = Tree2Mode::ContainsNode;
  std::vector<int> containing_ends;
  int containing_idx = 0;
  int focus_index = node_layout::kNodesFocusIndex;
  int tree1_v = 0, tree1_h = 0, tree2_v = 0, tree2_h = 0;

  /**
   * @brief Reloads selected-node YAML and both copied dependency-tree panes.
   *
   * The helper refreshes editor text, clears the dirty flag, queries the
   * primary tree and containing ending nodes, and formats the active secondary
   * tree mode.
   *
   * @return Nothing.
   * @throws std::bad_alloc if Host results, snapshots, or formatted UI text
   *         exhaust memory.
   * @note The lambda borrows all captured event-loop state and `svc`; it must
   *       run on the UI thread while those captures remain alive. Recoverable
   *       Host failures are represented by stable fallback pane text.
   */
  auto load_current = [&] {
    auto y = svc.get_node_yaml(ps::GraphSessionId{graph_name},
                               ps::NodeId{ids[selected_index]});
    editor_text = y.status.ok ? y.value : "# failed to load node\n";
    last_saved_text = editor_text;
    dirty = false;
    auto t1 = svc.dependency_tree(ps::GraphSessionId{graph_name},
                                  ps::NodeId{ids[selected_index]});
    tree1_text = t1.status.ok ? ps::cli::format_dependency_tree(
                                    t1.value, tree_show_params)
                              : "(failed to dump tree)\n";
    containing_ends.clear();
    containing_idx = 0;
    auto ends = svc.trees_containing_node(ps::GraphSessionId{graph_name},
                                          ps::NodeId{ids[selected_index]});
    if (ends.status.ok && !ends.value.empty()) {
      for (const auto& end : ends.value) {
        containing_ends.push_back(end.value);
      }
    }
    /**
     * @brief Builds the active secondary dependency-tree pane value.
     *
     * @return Formatted tree text, a stable no-containing-tree message, or an
     *         empty optional when the matching Host inspection fails.
     * @throws std::bad_alloc if Host result or formatted text construction
     *         exhausts memory.
     * @note The lambda borrows the surrounding selection and Host state only
     *       for this immediate call and retains no copied Host snapshot.
     */
    auto t2 = [&]() {
      if (tree2_mode == Tree2Mode::ContainsNode) {
        if (containing_ends.empty())
          return std::optional<std::string>(
              "(no end trees contain this node)\n");
        int end_id = containing_ends[std::min(
            containing_idx, static_cast<int>(containing_ends.size()) - 1)];
        auto tree = svc.dependency_tree(ps::GraphSessionId{graph_name},
                                        ps::NodeId{end_id});
        if (!tree.status.ok)
          return std::optional<std::string>();
        return std::optional<std::string>(
            ps::cli::format_dependency_tree(tree.value, tree_show_params));
      } else {
        auto tree = svc.dependency_tree(ps::GraphSessionId{graph_name},
                                        ps::NodeId{ids[selected_index]});
        if (!tree.status.ok)
          return std::optional<std::string>();
        return std::optional<std::string>(
            ps::cli::format_dependency_tree(tree.value, tree_show_params));
      }
    }();
    tree2_text = t2.value_or("(failed to dump tree)\n");
  };
  load_current();

  int last_selected_index = selected_index;
  MenuOption menu_opt;
  menu_opt.entries = &entries;
  menu_opt.selected = &selected_index;

  /**
   * @brief Handles node-menu selection changes.
   *
   * @return Nothing.
   * @throws std::bad_alloc if reloading selected Host values or pane text
   *         exhausts memory.
   * @note The callback borrows UI state for the event-loop lifetime. It rejects
   *       a selection change while unsaved editor text is dirty.
   */
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

  /**
   * @brief Recomputes whether the YAML editor differs from its saved baseline.
   * @return Nothing.
   * @throws Nothing.
   * @note The callback borrows editor strings and the dirty flag for the
   *       event-loop lifetime and performs no Host call.
   */
  textarea_opt.on_change = [&] { dirty = (editor_text != last_saved_text); };
  auto textarea = Input(textarea_opt);

  /**
   * @brief Applies vertical and horizontal offsets to pane text.
   *
   * @param s Borrowed multiline pane text.
   * @param voff Requested first line; negative values are clamped to zero.
   * @param hoff Requested byte offset within every emitted line.
   * @return Owned visible suffix with newline termination per emitted line.
   * @throws std::bad_alloc if stream, line-vector, or output construction
   *         exhausts memory.
   * @note The helper uses byte offsets, retains no reference to `s`, and
   *       performs no Host call. Callers provide a non-negative `hoff`.
   */
  auto text_with_offsets = [&](const std::string& s, int voff, int hoff) {
    std::istringstream iss(s);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line))
      lines.push_back(line);
    std::string out;
    for (int i = std::max(0, voff); i < static_cast<int>(lines.size()); ++i) {
      std::string ln = lines[i];
      if (static_cast<int>(ln.size()) > hoff) {
        ln = ln.substr(hoff);
      } else {
        ln.clear();
      }
      out += ln;
      out += "\n";
    }
    return out;
  };

  /**
   * @brief Renders the primary dependency-tree pane and its focus state.
   * @return Newly composed FTXUI element for the current frame.
   * @throws std::bad_alloc if pane text or FTXUI element construction exhausts
   *         memory.
   * @note The renderer borrows captured pane and focus state for the UI-loop
   *       lifetime and performs no Host call during rendering.
   */
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

  /**
   * @brief Renders the secondary tree pane, active mode, and focus state.
   * @return Newly composed FTXUI element for the current frame.
   * @throws std::bad_alloc if title, pane text, or FTXUI element construction
   *         exhausts memory.
   * @note The renderer borrows captured pane, selection, and focus state for
   *       the UI-loop lifetime and performs no Host call during rendering.
   */
  auto tree2_view = Renderer([&] {
    std::string title =
        (tree2_mode == Tree2Mode::ContainsNode ? "Tree (contains)"
                                               : "Tree (as end)");
    if (tree2_mode == Tree2Mode::ContainsNode && !containing_ends.empty())
      title +=
          " end=" +
          std::to_string(containing_ends[std::min(
              containing_idx, static_cast<int>(containing_ends.size()) - 1)]);
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

  /**
   * @brief Composes node, YAML, tree, and help panes for the current frame.
   * @return Newly composed root FTXUI element.
   * @throws std::bad_alloc if UI labels or FTXUI element construction exhausts
   *         memory.
   * @note The renderer borrows all component and focus state for the UI-loop
   *       lifetime and performs no Host call during rendering.
   */
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

  /**
   * @brief Applies edited YAML, validates traversal, and refreshes saved state.
   *
   * The helper submits the current YAML, queries traversal orders as the
   * existing acyclicity signal, best-effort reverts on traversal failure, and
   * otherwise best-effort saves `content.yaml` before reloading both panes.
   *
   * @return `true` so FTXUI treats the triggering event as handled.
   * @throws std::bad_alloc if Host results, path text, or refreshed pane state
   *         exhausts memory.
   * @note The lambda borrows captured UI state and `svc` for the event-loop
   *       lifetime. Recoverable set/revert/save statuses are not surfaced by
   *       this legacy UI boundary; no backend object is retained.
   */
  auto apply_current_editor = [&] {
    svc.set_node_yaml(ps::GraphSessionId{graph_name},
                      ps::NodeId{ids[selected_index]}, editor_text);
    // Validate acyclicity using traversal
    auto orders = svc.traversal_orders(ps::GraphSessionId{graph_name});
    if (!orders.status.ok) {
      // revert
      svc.set_node_yaml(ps::GraphSessionId{graph_name},
                        ps::NodeId{ids[selected_index]}, last_saved_text);
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
    svc.save_graph(ps::GraphSessionId{graph_name}, content_path);
    // refresh trees
    load_current();
    return true;
  };

  /**
   * @brief Round-trips the current YAML through a blocking external editor.
   *
   * The helper writes a session-local node file, invokes `$EDITOR` or `vim`
   * synchronously, rereads the file, and updates dirty state when nonempty text
   * changed.
   *
   * @return `true` so FTXUI treats the triggering event as handled.
   * @throws std::bad_alloc if path, command, stream, or text construction
   *         exhausts memory.
   * @note The lambda borrows captured state for the event-loop lifetime.
   *       Ordinary stream failures and the editor process exit status are
   *       intentionally best-effort; the helper performs no Host call.
   */
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
    const int editor_status = std::system(cmd.c_str());
    (void)editor_status;
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

  /**
   * @brief Routes editor keyboard events to exit, focus, edit, and tree
   * actions.
   *
   * @param e FTXUI event delivered by the fullscreen event loop.
   * @return `true` when the event is consumed, otherwise `false` for normal
   *         component propagation.
   * @throws std::bad_alloc if an action reloads Host results or rebuilds UI
   *         text and exhausts memory.
   * @note The callback borrows the complete editor state and Host for the
   *       event-loop lifetime and must execute on that loop's thread.
   */
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
      if (containing_idx + 1 < static_cast<int>(containing_ends.size())) {
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
