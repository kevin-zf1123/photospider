#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)

// This layout is shared with image_gpu.h; all geometry is integer and origin
// relative. The host derives circle coverage using the CPU double contract.
struct ImageParameters {
  ulong geometry[32];
  float values[8];
  uint kind;
  uint radius;
  uint factor;
  uint pass;
};
inline float read_image(device const uint* data, constant ImageParameters& p,
                        uint base, ulong y, ulong x, ulong c) {
  const ulong address = p.geometry[base] +
      (y - p.geometry[base + 1]) * p.geometry[base + 3] +
      (x - p.geometry[base + 2]) * p.geometry[base + 4] +
      c * p.geometry[base + 5];
  return as_type<float>(data[address / 4]);
}
inline void add_sample(thread float& sum, thread float& correction, float value) {
  const float adjusted = value - correction;
  const float total = sum + adjusted;
  correction = (total - sum) - adjusted;
  sum = total;
}
inline ulong border(ulong position, int tap, ulong length) {
  return ulong(clamp(long(position) + long(tap), long(0), long(length - 1)));
}
kernel void image_operation(
    device const uint* a [[buffer(0)]],
    device const uint* b [[buffer(1)]],
    device uint* output [[buffer(2)]],
    device float* temporary [[buffer(3)]],
    device const uchar* extra [[buffer(4)]],
    constant ImageParameters& p [[buffer(5)]],
    uint3 tid [[thread_position_in_grid]]) {
  const ulong width = p.geometry[3], channels = p.geometry[4];
  const ulong x = p.geometry[1] + tid.x;
  const ulong y = p.geometry[0] + tid.y;
  const ulong c = tid.z;
  const ulong destination = (ulong(tid.y) * width + tid.x) * channels + c;
  float number = 0;
  if (p.kind == 2) {
    device const float* weights = reinterpret_cast<device const float*>(extra);
    float sum = 0, correction = 0;
    for (int tap = -int(p.radius); tap <= int(p.radius); ++tap) {
      const float value = p.pass == 0
          ? read_image(a, p, 9, p.geometry[7] + tid.y,
                       border(x, tap, p.geometry[6]), c)
          : temporary[((border(y, tap, p.geometry[5]) - p.geometry[7]) *
                        width + tid.x) * 4 + c];
      add_sample(sum, correction, value * weights[tap + int(p.radius)]);
    }
    if (p.pass == 0) {
      temporary[(ulong(tid.y) * width + tid.x) * 4 + c] = sum;
      return;
    }
    number = sum;
  } else if (p.kind == 5 || p.kind == 6) {
    const ulong y0 = y * p.factor, x0 = x * p.factor;
    const ulong height = min(ulong(p.factor), p.geometry[5] - y0);
    const ulong extent = min(ulong(p.factor), p.geometry[6] - x0);
    float sum = 0, correction = 0;
    for (ulong row = 0; row < height; ++row)
      for (ulong col = 0; col < extent; ++col)
        add_sample(sum, correction, read_image(a, p, 9, y0 + row, x0 + col, c));
    number = sum / float(height * extent);
  } else {
    number = read_image(a, p, 9, y, x, c);
    if (p.kind == 0) {
      if (c < 3) number = number * p.values[0];
    } else if (p.kind == 1) {
      number = number * p.values[0];
    } else if (p.kind == 3) {
      number = number * read_image(b, p, 15, y, x, 0);
    } else if (p.kind == 4) {
      const float remaining = 1.0f - read_image(a, p, 9, y, x, 3);
      const float back = read_image(b, p, 15, y, x, c) * remaining;
      number = number + back;
    } else if (p.kind == 7) {
      device const ulong* spans = reinterpret_cast<device const ulong*>(extra);
      if (ulong(tid.x) >= spans[tid.y * 2] && ulong(tid.x) < spans[tid.y * 2 + 1]) {
        const float source = c == 3 ? p.values[6] : p.values[3 + c] * p.values[6];
        const float back = number * (1.0f - p.values[6]);
        number = source + back;
      }
    }
  }
  output[destination] = as_type<uint>(number);
}
