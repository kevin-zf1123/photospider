#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"
#include "graph_cli/node_editor_layout.hpp"

namespace {

using namespace ftxui;  // NOLINT(build/namespaces): concise FTXUI test DSL.
namespace layout = ps::cli::node_editor_layout;

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(NodeEditorLayout, ReservesReadableWidthForNodeIds) {
  EXPECT_EQ(layout::node_pane_width({"0", "66"}), layout::kNodePaneMinWidth);
  EXPECT_GE(layout::node_pane_width({"0", "123456"}), 10);
  EXPECT_EQ(layout::node_pane_width({"12345678901234567890"}),
            layout::kNodePaneMaxWidth);
}

TEST(NodeEditorLayout, PaneTitlesFitTheirMinimumWidths) {
  EXPECT_LE(std::string(layout::kNodesTitle).size() + 2,
            static_cast<size_t>(layout::kNodePaneMinWidth));
  EXPECT_LE(std::string(layout::kYamlPaneTitle).size() + 2,
            static_cast<size_t>(layout::kYamlPaneMinWidth));
  EXPECT_LE(std::string(layout::kDependencyTreePaneTitle).size() + 2,
            static_cast<size_t>(layout::kDependencyTreePaneMinWidth));
  EXPECT_LE(std::string("Tree (contains) end=66").size() + 2,
            static_cast<size_t>(layout::kContainsTreePaneMinWidth));
}

TEST(NodeEditorLayout, HelpTextDocumentsEditingAndTreeShortcuts) {
  const std::string help =
      std::string(layout::kPrimaryHelpText) + "\n" + layout::kTreeHelpText;

  EXPECT_TRUE(contains(help, "Tab focus")) << help;
  EXPECT_TRUE(contains(help, "v edit YAML")) << help;
  EXPECT_TRUE(contains(help, "Ctrl+S/Enter apply")) << help;
  EXPECT_TRUE(contains(help, "q quit")) << help;
  EXPECT_TRUE(contains(help, "t show/hide params")) << help;
  EXPECT_TRUE(contains(help, "c contains/as-end")) << help;
  EXPECT_TRUE(contains(help, "[ ] switch containing end tree")) << help;
  EXPECT_TRUE(contains(help, "arrows scroll focused tree")) << help;
}

TEST(NodeEditorLayout, RendersAllPaneHeadersAtOneHundredTwentyColumns) {
  std::vector<std::string> entries = {"0", "66", "123"};
  const int node_width = layout::node_pane_width(entries);

  auto nodes = vbox({text(layout::kNodesTitle) | bold, separator(),
                     text("123") | frame | flex}) |
               border | size(WIDTH, EQUAL, node_width);
  auto editor = vbox({text(layout::kYamlPaneTitle) | bold, separator(),
                      text("id: 123") | frame | flex}) |
                border | size(WIDTH, GREATER_THAN, layout::kYamlPaneMinWidth) |
                xflex;
  auto dependency_tree =
      vbox({text(layout::kDependencyTreePaneTitle) | bold, separator(),
            text("- Node 123") | frame | flex}) |
      border | size(WIDTH, GREATER_THAN, layout::kDependencyTreePaneMinWidth) |
      xflex;
  auto contains_tree =
      vbox({text("Tree (contains) end=66") | bold, separator(),
            text("- Node 66") | frame | flex}) |
      border | size(WIDTH, GREATER_THAN, layout::kContainsTreePaneMinWidth) |
      xflex;

  auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(8));
  Render(screen, hbox({nodes, separator(), editor, separator(), dependency_tree,
                       separator(), contains_tree}));
  const std::string rendered = screen.ToString();

  EXPECT_TRUE(contains(rendered, layout::kNodesTitle)) << rendered;
  EXPECT_TRUE(contains(rendered, "123")) << rendered;
  EXPECT_TRUE(contains(rendered, layout::kYamlPaneTitle)) << rendered;
  EXPECT_TRUE(contains(rendered, layout::kDependencyTreePaneTitle)) << rendered;
  EXPECT_TRUE(contains(rendered, "Tree (contains) end=66")) << rendered;
}

TEST(NodeEditorLayout, RendersHelpTextAtOneHundredTwentyColumns) {
  auto help =
      vbox({text(layout::kPrimaryHelpText), text(layout::kTreeHelpText)}) | dim;
  auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(2));
  Render(screen, help);
  const std::string rendered = screen.ToString();

  EXPECT_TRUE(contains(rendered, "v edit YAML")) << rendered;
  EXPECT_TRUE(contains(rendered, "Ctrl+S/Enter apply")) << rendered;
  EXPECT_TRUE(contains(rendered, "t show/hide params")) << rendered;
  EXPECT_TRUE(contains(rendered, "arrows scroll focused tree")) << rendered;
}
