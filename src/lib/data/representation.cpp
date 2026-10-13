#include "photospider/data/representation.hpp"

#include "core/status_helpers.hpp"
#include "data/lut3d_bake_validation.hpp"
#include "photospider/data/lut3d_bake.hpp"

namespace ps {
bool has_representation_schema(std::string_view id) noexcept {
  return id == "curve.bake_lut3d.report" || id == "curve.bake_lut3d.table";
}
Status validate_representation_schema(const SchemaTemplate& schema) {
  if (schema.id == "curve.bake_lut3d.report" ||
      schema.id == "curve.bake_lut3d.table")
    return lut3d_bake_description(schema).status();
  return Status::success();
}
Status validate_representation(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  if (!result.valid() || !result.owned_by(resources))
    return core_internal::type_mismatch("foreign representation owner");
  if (result.schema().id == "curve.bake_lut3d.table" ||
      result.schema().id == "curve.bake_lut3d.report") {
    auto admission = resources.reserve(ResourceCapacity::host(65536, 65536));
    if (!admission.ok())
      return admission.status();
    ResourceAllocationScope metadata_scope(resources);
    if (result.schema().id == "curve.bake_lut3d.table")
      return input_internal::validate_lut3d_bake_table(
          result, resources, maximum_window, cancellation, consume_work);
    return read_lut3d_bake_report(result, maximum_window, cancellation,
                                  [&](std::uint64_t count) {
                                    auto work = resources.consume({count});
                                    if (work.ok() && consume_work)
                                      work = consume_work(count);
                                    return work;
                                  })
        .status();
  }
  return Status::success();
}
}  // namespace ps
