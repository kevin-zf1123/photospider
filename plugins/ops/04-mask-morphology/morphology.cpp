#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
enum class Morph {
  Dilate,
  Erode,
  Open,
  Close,
  Gradient,
  Inner,
  Outer,
  White,
  Black
};
struct Box final {
  std::int64_t y, x;
  std::uint64_t h, w;
  Box expanded(const Footprint& f) const {
    return {y - f.ry, x - f.rx, h + 2 * f.ry, w + 2 * f.rx};
  }
};
Box box(const Region& q) {
  const auto d = spatial_region(q).dimensions();
  return {static_cast<std::int64_t>(d[0].offset),
          static_cast<std::int64_t>(d[1].offset), d[0].extent, d[1].extent};
}
struct Poll final {
  const Work& work;
  std::uint64_t pending = 0;
  void step() {
    if (++pending == 256) {
      work.poll(pending);
      pending = 0;
    }
  }
  void flush() {
    work.poll(pending);
    pending = 0;
  }
};
template <class T>
struct Entry final {
  std::uint64_t index;
  T value;
};
struct Morphology final : Program {
  Morph kind;
  Footprint footprint;
  ElementType type;
  std::uint64_t height, width;
  Morphology(Morph k, const std::vector<OperationMetadata>& in,
             const Parameters& p)
      : kind(k), footprint(p) {
    const auto& s = image(in[0], 2);
    type = s.descriptor.element_type;
    height = s.descriptor.shape[0];
    width = s.descriptor.shape[1];
    add_output(this, spatial_schema(type, {height, width}));
  }
  bool composed() const {
    return kind == Morph::Open || kind == Morph::Close ||
           kind == Morph::White || kind == Morph::Black;
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    return {{0, 0, footprint.support(q, height, width, composed() ? 2 : 1)}};
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    const auto b = box(q), t = composed() ? b.expanded(footprint) : b;
    const auto n = static_cast<unsigned __int128>(b.h) * b.w;
    const auto mid = static_cast<unsigned __int128>(t.h) * t.w;
    const bool square = footprint.kind == "square";
    const auto horizontal =
        square ? static_cast<unsigned __int128>(t.h + 2 * footprint.ry) * t.w
               : 0;
    const auto queue =
        square ? std::max(t.h + 2 * footprint.ry, t.w + 2 * footprint.rx) : 0;
    return bytes((n + mid + horizontal) * generation::width(type) +
                 static_cast<unsigned __int128>(queue) * sizeof(Entry<double>) +
                 64);
  }
  template <class T, class Read>
  void stage(T* target, const Box& b, bool maximum, Read read, T* horizontal,
             Entry<T>* queue, Poll* poll) const {
    const auto better = [maximum](T a, T b) { return maximum ? a > b : a < b; };
    if (footprint.kind == "square") {
      const auto rx = static_cast<std::uint64_t>(footprint.rx);
      const auto ry = static_cast<std::uint64_t>(footprint.ry);
      const auto ih = b.h + 2 * ry, iw = b.w + 2 * rx;
      for (std::uint64_t yy = 0; yy < ih; ++yy) {
        std::uint64_t head = 0, tail = 0;
        for (std::uint64_t xx = 0; xx < iw; ++xx) {
          const auto v =
              read(b.y - footprint.ry + static_cast<std::int64_t>(yy),
                   b.x - footprint.rx + static_cast<std::int64_t>(xx));
          poll->step();
          while (tail > head && better(v, queue[tail - 1].value)) {
            --tail;
            poll->step();
          }
          queue[tail++] = {xx, v};
          if (xx >= 2 * rx) {
            const auto left = xx - 2 * rx;
            while (queue[head].index < left) {
              ++head;
              poll->step();
            }
            horizontal[yy * b.w + left] = queue[head].value;
          }
        }
      }
      for (std::uint64_t xx = 0; xx < b.w; ++xx) {
        std::uint64_t head = 0, tail = 0;
        for (std::uint64_t yy = 0; yy < ih; ++yy) {
          const auto v = horizontal[yy * b.w + xx];
          poll->step();
          while (tail > head && better(v, queue[tail - 1].value)) {
            --tail;
            poll->step();
          }
          queue[tail++] = {yy, v};
          if (yy >= 2 * ry) {
            const auto top = yy - 2 * ry;
            while (queue[head].index < top) {
              ++head;
              poll->step();
            }
            auto best = queue[head].value;
            // Separable reductions keep the lexicographically first extremum.
            // Center priority is applied once for this complete 2D stage.
            const auto center = read(b.y + static_cast<std::int64_t>(top),
                                     b.x + static_cast<std::int64_t>(xx));
            if (center == best)
              best = center;
            target[top * b.w + xx] = best;
          }
        }
      }
      return;
    }
    for (std::uint64_t yy = 0; yy < b.h; ++yy) {
      for (std::uint64_t xx = 0; xx < b.w; ++xx) {
        const auto y = b.y + static_cast<std::int64_t>(yy);
        const auto x = b.x + static_cast<std::int64_t>(xx);
        T best = read(y, x);
        footprint.each([&](std::int64_t dy, std::int64_t dx) {
          poll->step();
          const auto v = read(y + dy, x + dx);
          if (better(v, best))
            best = v;
        });
        target[yy * b.w + xx] = best;
      }
    }
  }
  template <class T>
  void run_t(const Kernel& kernel, const ps_kernel_invocation_v8& inv) const {
    const Reader r{kernel, inv};
    const auto& out = inv.outputs[0];
    const Box b{static_cast<std::int64_t>(out.layout->origin[0]),
                static_cast<std::int64_t>(out.layout->origin[1]),
                out.layout->shape[0], out.layout->shape[1]};
    const auto t = composed() ? b.expanded(footprint) : b;
    const auto n = b.h * b.w, mn = t.h * t.w;
    auto* final = static_cast<T*>(inv.scratch[0].allocation);
    auto* middle = final + n;
    auto* horizontal = middle + mn;
    const auto hn =
        footprint.kind == "square" ? (t.h + 2 * footprint.ry) * t.w : 0;
    const auto address = reinterpret_cast<std::uintptr_t>(horizontal + hn);
    auto* queue = reinterpret_cast<Entry<T>*>(
        (address + alignof(Entry<T>) - 1) & ~(alignof(Entry<T>) - 1));
    Work work{inv, work_limit};
    Poll poll{work};
    const auto source = [&](std::int64_t y, std::int64_t x) -> T {
      if (y < 0 || x < 0 || static_cast<std::uint64_t>(y) >= height ||
          static_cast<std::uint64_t>(x) >= width)
        return T(0);
      return static_cast<T>(r.value(
          0, 0,
          {static_cast<std::uint64_t>(y), static_cast<std::uint64_t>(x)}));
    };
    if (composed()) {
      const bool closing = kind == Morph::Close || kind == Morph::Black;
      stage(middle, t, closing, source, horizontal, queue, &poll);
      const auto intermediate = [&](std::int64_t y, std::int64_t x) {
        return middle[static_cast<std::uint64_t>(y - t.y) * t.w +
                      static_cast<std::uint64_t>(x - t.x)];
      };
      stage(final, b, !closing, intermediate, horizontal, queue, &poll);
    } else {
      const bool maximum = kind == Morph::Dilate || kind == Morph::Outer ||
                           kind == Morph::Gradient;
      stage(final, b, maximum, source, horizontal, queue, &poll);
      if (kind == Morph::Gradient)
        stage(middle, b, false, source, horizontal, queue, &poll);
    }
    for (std::uint64_t yy = 0; yy < b.h; ++yy) {
      for (std::uint64_t xx = 0; xx < b.w; ++xx) {
        const auto index = yy * b.w + xx;
        T v = final[index];
        if (kind == Morph::Gradient)
          v -= middle[index];
        else if (kind == Morph::Inner || kind == Morph::White)
          v = source(b.y + yy, b.x + xx) - v;
        else if (kind == Morph::Outer || kind == Morph::Black)
          v -= source(b.y + yy, b.x + xx);
        if (kind >= Morph::Gradient && v == 0)
          v = 0;
        store(out,
              {static_cast<std::uint64_t>(b.y) + yy,
               static_cast<std::uint64_t>(b.x) + xx},
              convert(v, sizeof(T) == 4));
        poll.step();
      }
    }
    poll.flush();
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (type == ElementType::Float32)
      run_t<float>(k, i);
    else
      run_t<double>(k, i);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask
namespace ps::plugin_internal {
Status register_mask_morphology(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (auto entry : {std::make_pair("dilate", Morph::Dilate),
                       std::make_pair("erode", Morph::Erode),
                       std::make_pair("opening", Morph::Open),
                       std::make_pair("closing", Morph::Close),
                       std::make_pair("morph_gradient", Morph::Gradient),
                       std::make_pair("inner_border", Morph::Inner),
                       std::make_pair("outer_border", Morph::Outer),
                       std::make_pair("white_top_hat", Morph::White),
                       std::make_pair("black_top_hat", Morph::Black)})
      check(registry->register_operation(
          definition(entry.first, 1, footprint_parameters(),
                     [kind = entry.second](const auto& in, const auto& p) {
                       return std::make_shared<Morphology>(kind, in, p);
                     })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
