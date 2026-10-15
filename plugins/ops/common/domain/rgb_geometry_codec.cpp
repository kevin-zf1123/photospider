#include "common/domain/rgb_geometry_codec.hpp"

#include <string>

#include "common/floating_environment.hpp"
namespace ps::format {
Result<std::string> rgb_basis_parameter(const RgbBasis& basis) {
  using namespace plugin_internal::basis_ops;  // NOLINT(build/namespaces)
  try {
    plugin_internal::FloatingEnvironment environment;
    typed(environment.active(), "floating environment unavailable");
    const auto resolved = resolve(basis);
    (void)rgb_matrix(resolved.xy, resolved.white);
    return Result<std::string>(basis_codec(resolved));
  } catch (const CheckedFailure& f) {
    return Result<std::string>(f.status);
  } catch (const GeometryError&) {
    return Result<std::string>(invalid("invalid/singular RGB geometry"));
  } catch (const data_internal::format_numeric::ExactWorkFailure& f) {
    return Result<std::string>(f.status);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(Status{ErrorCode::ResourceExhausted,
                                      "FMT-10 geometry capacity exhausted",
                                      FailureReason::CapacityLimit});
  }
}
Result<std::string> xyz_white_parameter(const std::array<double, 2>& white) {
  using namespace plugin_internal::basis_ops;  // NOLINT(build/namespaces)
  try {
    (void)white_xyz(white);
    return Result<std::string>(white_codec(white));
  } catch (const CheckedFailure& f) {
    return Result<std::string>(f.status);
  } catch (const GeometryError&) {
    return Result<std::string>(invalid("invalid white xy"));
  } catch (const data_internal::format_numeric::ExactWorkFailure& f) {
    return Result<std::string>(f.status);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(Status{ErrorCode::ResourceExhausted,
                                      "FMT-10 geometry capacity exhausted",
                                      FailureReason::CapacityLimit});
  }
}
}  // namespace ps::format
