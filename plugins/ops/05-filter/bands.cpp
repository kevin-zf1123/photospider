#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/advanced.hpp"

namespace ps::plugin_internal::filter {
namespace {
struct SelectBand final : Program {
  unsigned selected = 0;
  SelectBand(const std::vector<OperationMetadata>& in, const Parameters& p) {
    require(in[0].result_schema != nullptr, "missing FilterBands schema");
    const auto& s = *in[0].result_schema;
    check(s.validate());
    require(s.version == 1 &&
                s.id == "photospider.filter_bands.pyramid_binomial5_v1",
            "unsupported FilterBands profile");
    const auto name = text(p, "band");
    while (selected < s.tensors.size() &&
           std::string_view(s.tensors[selected].key) != name)
      ++selected;
    parameter(selected < s.tensors.size(), "unknown FilterBands member");
    SchemaTemplate out;
    out.id = "photospider.tensor";
    out.tensors.push_back(s.tensors[selected]);
    out.tensors[0].key = "samples";
    check(out.validate());
    add_output(this, std::move(out));
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, selected, q}};
  }
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override {}
};
}  // namespace
OperationDefinition make_band() {
  auto def = generation::operation(
      "filter.select_band", 1, {{"band", Type::String}},
      {{"output", tensor_schema(ElementType::Float64, {1, 1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<SelectBand>(in, p);
      });
  def.traits.input_schema[0].element_type_mask = 0;
  def.traits.input_schema[0].result_schema_id =
      "photospider.filter_bands.pyramid_binomial5_v1";
  def.traits.input_schema[0].result_schema_version = 1;
  def.lower = [](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const SelectBand*>(q.static_state);
      OperationProgramBuilder b(q);
      for (const auto& member : q.requests[0].members)
        for (auto tile : member.second) {
          const auto task = take(b.add_copy());
          check(b.read_input(task, 0, s.selected, tile));
          check(b.write_output(task, 0, 0, tile));
        }
      return b.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  return def;
}
}  // namespace ps::plugin_internal::filter
