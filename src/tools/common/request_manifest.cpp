#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/codec.hpp"

namespace ps::tools {
std::map<std::string, TileRequest> read_requests(const std::string& manifest,
                                                 Files* files) {
  JsonDocument document(files->read(manifest));
  auto json = document.root();
  json.fields({"format", "version", "outputs"});
  format(json, "photospider.requests");
  std::map<std::string, TileRequest> result;
  ResourceBudget request_budget;
  std::size_t interval_count = 0;
  for (auto output : json.at("outputs").array()) {
    output.fields({"name", "members"});
    TileRequest request;
    for (auto member : output.at("members").array()) {
      member.fields({"slot", "intervals"});
      std::vector<TileInterval> intervals;
      for (auto interval : member.at("intervals").array()) {
        auto bounds = interval.array();
        if (bounds.size() != 2)
          throw ToolError("tile interval requires [begin,end]");
        if (++interval_count > 65536)
          throw ToolError("request interval limit exceeded");
        intervals.push_back({bounds[0].u64(), bounds[1].u64()});
      }
      auto tiles = take(TileSet::from_intervals(
          request_budget, intervals.data(), intervals.size()));
      if (!request.members.emplace(u32(member.at("slot")), std::move(tiles))
               .second)
        throw ToolError("duplicate tensor slot");
    }
    if (!result.emplace(output.at("name").text(), std::move(request)).second)
      throw ToolError("duplicate output request");
  }
  return result;
}
}  // namespace ps::tools
