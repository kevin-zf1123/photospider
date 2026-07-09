#include <gtest/gtest.h>

#include <string>

#include "cli/dependency_tree_formatter.hpp"

namespace ps::cli {
namespace {

TEST(CliDirtySnapshotFormatter, RendersMonolithicAndEdgeMappings) {
  DirtyRegionInspectionSnapshot snapshot;
  snapshot.graph_generation = 7;
  snapshot.dirty_monolithic_nodes.push_back(DirtyMonolithicRegionSnapshot{
      NodeId{2}, DirtyDomain::HighPrecision, PixelRect{0, 0, 8, 6}, true});
  snapshot.actual_dirty_rois[2].push_back(PixelRect{0, 0, 8, 6});
  snapshot.edge_mappings.push_back(DirtyEdgeMappingSnapshot{
      NodeId{1}, NodeId{2}, DirtyDomain::HighPrecision, PixelRect{0, 0, 8, 6},
      PixelRect{1, 1, 2, 2}, DirtyEdgeDirection::BackwardDemand});

  const std::string text = format_dirty_snapshot(snapshot);

  EXPECT_EQ(text.find("(No dirty snapshot recorded.)"), std::string::npos);
  EXPECT_NE(text.find("Monolithic dirty regions: 1"), std::string::npos);
  EXPECT_NE(text.find("node 2 hp whole=true roi=0,0 8x6"), std::string::npos);
  EXPECT_NE(text.find("Edge mappings: 1"), std::string::npos);
  EXPECT_NE(text.find("node 1 -> 2 hp backward-demand "
                      "from=[0,0 8x6] to=[1,1 2x2]"),
            std::string::npos);
}

}  // namespace
}  // namespace ps::cli
