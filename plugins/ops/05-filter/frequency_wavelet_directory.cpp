// Copyright 2026 Photospider contributors
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/frequency_native.hpp"

namespace ps::plugin_internal::frequency {
namespace {
struct Directory final : Program {
  unsigned selected = 0;
  Directory(const std::vector<OperationMetadata>& in, const Parameters& p,
            const std::string& profile, bool assemble) {
    SchemaTemplate schema;
    schema.id =
        assemble ? "photospider.filter_bands." + profile : "photospider.tensor";
    if (assemble) {
      require(!in.empty() && in.size() <= 16 && (in.size() - 1) % 3 == 0,
              "wavelet assembly member count");
      const unsigned levels = (in.size() - 1) / 3;
      for (unsigned i = 0; i < in.size(); ++i) {
        auto t = tensor(in[i], true);
        t.key = i == 3 * levels ? "LL"
                                : std::string(i % 3 == 0   ? "LH_y_"
                                              : i % 3 == 1 ? "HL_x_"
                                                           : "HH_") +
                                      std::to_string(i / 3 + 1);
        schema.tensors.push_back(std::move(t));
      }
    } else {
      require(in[0].result_schema &&
                  std::string_view(in[0].result_schema->id) ==
                      "photospider.filter_bands." + profile &&
                  in[0].result_schema->version == 1,
              "wavelet selection schema");
      const auto& tensors = in[0].result_schema->tensors;
      const auto name = text(p, "band");
      while (selected < tensors.size() &&
             std::string_view(tensors[selected].key) != name) {
        ++selected;
      }
      parameter(selected < tensors.size(), "unknown wavelet band");
      schema.tensors.push_back(tensors[selected]);
      schema.tensors[0].key = "samples";
    }
    check(schema.validate());
    add_output(this, std::move(schema));
  }
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override {}
};
}  // namespace
OperationDefinition make_wavelet_directory(const std::string& profile,
                                           bool assemble) {
  auto schema = tensor_schema(ElementType::Float64, {1, 1});
  if (assemble) {
    schema.id = "photospider.filter_bands." + profile;
    schema.tensors[0].key = "LL";
  }
  auto def = generation::operation(
      "frequency." + std::string(assemble ? "assemble_" : "select_") + profile,
      assemble ? 0 : 1,
      assemble ? std::vector<Parameter>{}
               : std::vector<Parameter>{{"band", Type::String}},
      {{assemble ? "bands" : "output", schema}},
      [=](const auto& in, const auto& p) {
        return std::make_shared<Directory>(in, p, profile, assemble);
      },
      assemble);
  if (assemble) {
    def.traits.repeated_maximum = 16;
  } else {
    def.traits.input_schema[0].element_type_mask = 0;
    def.traits.input_schema[0].result_schema_id =
        "photospider.filter_bands." + profile;
    def.traits.input_schema[0].result_schema_version = 1;
  }
  def.lower =
      [assemble](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& s = *static_cast<const Directory*>(q.static_state);
      OperationProgramBuilder b(q);
      for (const auto& m : q.requests[0].members) {
        for (auto tile : m.second) {
          const auto task = take(b.add_copy());
          check(b.read_input(task, assemble ? m.first : 0,
                             assemble ? 0 : s.selected, tile));
          check(b.write_output(task, 0, m.first, tile));
        }
      }
      return b.finish();
    } catch (const Status& e) {
      return Result<OperationProgram>(e);
    }
  };
  return def;
}
}  // namespace ps::plugin_internal::frequency
