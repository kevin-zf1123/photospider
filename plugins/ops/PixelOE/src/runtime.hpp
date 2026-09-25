#pragma once
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/planar_operation_plugin_api.h"
namespace px {
struct Failure : std::runtime_error {
  int code;
  Failure(int c, const std::string& s) : std::runtime_error(s), code(c) {}
};
struct Environment {
  fenv_t prior;
  Environment() {
    if (fegetenv(&prior) || fesetenv(FE_DFL_ENV)) {
      throw Failure(1, "FP environment");
    }
  }
  ~Environment() { fesetenv(&prior); }
};
struct Buffer {
  void* data = nullptr;
  size_t count = 0;
  uint32_t height = 1, width = 1, channels = 1;
  std::shared_ptr<void> owner;
};
using Array = Buffer;
struct Argument {
  void* data = nullptr;
  size_t count = 0;
  uint64_t u = 0;
  float f = 0;
  Argument() = default;
  Argument(const Buffer& b)              // NOLINT(runtime/explicit)
      : data(b.data), count(b.count) {}  // NOLINT(runtime/explicit)
  Argument(uint32_t v) : u(v) {}         // NOLINT(runtime/explicit)
  Argument(int v) : u(v) {}              // NOLINT(runtime/explicit)
  Argument(uint64_t v) : u(v) {}         // NOLINT(runtime/explicit)
  Argument(float v) : f(v) {}            // NOLINT(runtime/explicit)
};
struct Arguments {
  std::map<std::string, Argument> items;
  Arguments(std::initializer_list<std::pair<const std::string, Argument>> list)
      : items(list) {}
  const Argument& get(const char* name) const {
    auto it = items.find(name);
    if (it == items.end()) {
      throw Failure(1, std::string("missing kernel arg ") + name);
    }
    return it->second;
  }
};
struct Range {
  uint32_t begin[3], end[3];
};
struct Kernel {
  const char* name;
  std::array<uint32_t, 3> group;
  void (*run)(const Range&, const Arguments&);
};
const std::vector<Kernel>& kernels();
struct Options {
  uint32_t pixel_size = 6, thickness = 3, num_colors = 32, blur_rank = 1;
  float sharpen_factor = 0.5f;
  bool do_color_match = true, do_quant = false, no_post_upscale = false;
  std::string mode = "contrast", sharpen_mode = "none", quant_mode = "kmeans",
              dither_mode = "ordered", weight_mapping = "current",
              weight_normalize = "global", colorfix_blur = "exact",
              blur_impl = "lowrank", local_stats = "lattice",
              stat_padding = "zero";
};
class Context {
 public:
  explicit Context(const ps_planar_services_v1* services);
  Array empty(uint64_t count, uint32_t bytes = 4);
  Array image(uint32_t h, uint32_t w, uint32_t c = 3);
  Array constant(const float* p, size_t count);
  void dispatch(const char* entry, std::array<uint32_t, 3> grid,
                Arguments args);
  void flat(const char* entry, uint32_t count, Arguments args);
  void check() const;
  bool simd_enabled() const { return simd_; }
  double kernel_ms = 0;
  uint64_t dispatches = 0;
  std::map<std::string, double> kernel_times;

 private:
  const ps_planar_services_v1* services_;
  bool simd_ = false;
};
// Internal ISA implementation; call only after runtime feature admission.
bool dispatch_simd(Context&, const char*, std::array<uint32_t, 3>,
                   const Arguments&);
struct Outputs {
  Array image, expanded, weight;
};
Outputs run(Context&, Array, const Options&, uint32_t selected);
const std::vector<float>& table(const std::string&);
}  // namespace px
