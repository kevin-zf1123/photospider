#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "runtime.hpp"  // NOLINT(build/include_subdir)
struct Memory {
  std::map<uint8_t*, size_t> blocks;
  size_t live = 0, peak = 0;
  ~Memory() {
    for (auto& p : blocks) {
      std::free(p.first);
    }
  }
};
int main(int argc, char** argv) try {
  if (argc < 3) {
    return 2;
  }
  uint32_t w = std::stoul(argv[1]), h = std::stoul(argv[2]);
  int reps = argc > 3 ? std::stoi(argv[3]) : 3;
  Memory memory;
  ps_planar_services_v1 services{};
  services.struct_size = sizeof(services);
  services.context = &memory;
  services.cancelled = [](void*) { return 0; };
  services.allocate_scratch = [](void* p, uint64_t n) -> uint8_t* {
    auto& m = *static_cast<Memory*>(p);
    auto* data = static_cast<uint8_t*>(std::calloc(n, 1));
    if (data) {
      m.blocks[data] = n;
      m.live += n;
      m.peak = std::max(m.peak, m.live);
    }
    return data;
  };
  services.release_scratch = [](void* p, uint8_t* data) {
    auto& m = *static_cast<Memory*>(p);
    auto found = m.blocks.find(data);
    if (found == m.blocks.end()) {
      return 0;
    }
    m.live -= found->second;
    m.blocks.erase(found);
    std::free(data);
    return 1;
  };
  px::Environment env;
  px::Context setup(&services);
  auto input = setup.image(h, w);
  auto* ptr = static_cast<float*>(input.data);
  if (argc > 4) {
    std::ifstream in(argv[4], std::ios::binary);
    std::vector<float> row(w * 3);
    for (uint32_t y = 0; y < h; ++y) {
      in.read(reinterpret_cast<char*>(row.data()), w * 12);
      for (uint32_t x = 0; x < w; ++x) {
        for (uint32_t c = 0; c < 3; ++c) {
          ptr[static_cast<uint64_t>(c) * h * w + static_cast<uint64_t>(y) * w +
              x] = row[x * 3 + c];
        }
      }
    }
    if (!in) {
      throw std::runtime_error("raw input read failed");
    }
  } else {
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        for (uint32_t c = 0; c < 3; ++c) {
          ptr[static_cast<uint64_t>(c) * h * w + static_cast<uint64_t>(y) * w +
              x] = static_cast<float>((static_cast<uint64_t>(x) * 17 +
                                       static_cast<uint64_t>(y) * 31 + c * 71 +
                                       (static_cast<uint64_t>(x) * y) % 113) %
                                      256) /
                   255;
        }
      }
    }
  }
  std::vector<double> timings, kernels;
  std::map<std::string, double> stages;
  for (int i = -1; i < reps; ++i) {
    px::Context ctx(&services);
    auto start = std::chrono::steady_clock::now();
    auto out = px::run(ctx, input, {}, 0);
    auto time = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
    if (i >= 0) {
      timings.push_back(time);
      kernels.push_back(ctx.kernel_ms);
      for (auto& s : ctx.kernel_times) {
        stages[s.first] += s.second / reps;
      }
    }
  }
  std::sort(timings.begin(), timings.end());
  std::sort(kernels.begin(), kernels.end());
  std::cout << "native_median_ms=" << timings[timings.size() / 2]
            << " kernel_median_ms=" << kernels[kernels.size() / 2]
            << " peak_scratch=" << memory.peak << "\n";
  std::vector<std::pair<double, std::string>> sorted;
  for (auto& s : stages) {
    sorted.push_back({s.second, s.first});
  }
  std::sort(sorted.rbegin(), sorted.rend());
  for (auto& s : sorted) {
    std::cout << s.second << " " << s.first << "\n";
  }
} catch (const std::exception& e) {
  std::cerr << e.what() << "\n";
  return 1;
}
