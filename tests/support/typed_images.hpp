#pragma once

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace typed_images {
/** @brief Distinct valid image meanings over the same finite sample payload. */
inline std::vector<ps::SemanticDescriptor> descriptions() {
  auto rgb = ps::rgba_semantics();
  rgb.channels.resize(3);
  rgb.association = "none";
  auto bgr = rgb;
  std::swap(bgr.channels[0], bgr.channels[2]);
  auto straight = ps::rgba_semantics();
  straight.association = "straight";
  auto xyz = rgb;
  xyz.model = "xyz";
  xyz.primaries.clear();
  xyz.channels = {{"X", "x", "relative"},
                  {"Y", "y", "relative"},
                  {"Z", "z", "relative"}};
  auto lab = xyz;
  lab.model = "lab";
  lab.transfer = "identity";
  lab.unit = "lab";
  lab.channels = {{"L", "lightness", "lab_lightness"},
                  {"a", "a", "lab_opponent"},
                  {"b", "b", "lab_opponent"}};
  auto d50 = lab;
  d50.white = {.96422, 1, .82521};
  auto xyza = xyz, laba = lab;
  for (auto* s : {&xyza, &laba}) {
    s->channels.push_back({"A", "coverage", "dimensionless"});
    s->association = "straight";
  }
  return {rgb, bgr, straight, ps::rgba_semantics(), xyz, lab, d50, xyza, laba};
}
/** @brief Signed/HDR colors, negative zero and hidden straight-alpha colors. */
inline ps::Value value(const ps::SemanticDescriptor& semantic) {
  const std::vector<std::uint64_t> shape{3, 5, semantic.channels.size()};
  auto writer = ps::MutableValue::allocate({ps::ElementType::Float32, shape},
                                           ps::Region::whole(shape),
                                           ps::BufferAllocator{})
                    .take_value();
  const float samples[] = {-1.F, 2.F, -0.F,
                           semantic.association == "straight" ? 0.F : .5F};
  for (std::size_t offset = 0; offset < writer.size(); offset += 4)
    std::memcpy(writer.data() + offset, &samples[(offset / 4) % shape[2]], 4);
  return std::move(writer)
      .publish({ps::encode_semantic(semantic).take_value()})
      .take_value();
}
inline ps::WorkflowDocument document(const ps::Value& value) {
  ps::WorkflowDocument document;
  document.inputs = {{1, "image", value.descriptor(), value.region(),
                      value.layout(), value.facets()}};
  document.nodes = {{1, "core.identity", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  return document;
}
inline bool same(const ps::Value& a, const ps::Value& b) {
  return a.descriptor().element_type == b.descriptor().element_type &&
         a.descriptor().shape == b.descriptor().shape &&
         a.copy_bytes() == b.copy_bytes() && a.facets().size() == 1 &&
         b.facets().size() == 1 && a.facets()[0].key == b.facets()[0].key &&
         a.facets()[0].version == b.facets()[0].version &&
         a.facets()[0].payload == b.facets()[0].payload;
}
}  // namespace typed_images
