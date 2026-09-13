/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this
license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                        Intel License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective icvers.
//
// Redistribution and use in source and binary forms, with or without
modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright
notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of Intel Corporation may not be used to endorse or promote
products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is"
and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are
disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any
direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

// Standalone Float32 NS port of OpenCV 4.12.0 modules/photo/src/inpaint.cpp.
// Source blob 2f2f368fa13da0bc1426b71862205048c6ea0f94.
#include <algorithm>
#include <cmath>
#include <functional>
#include <new>

#include "09-composite/inpaint_ns.hpp"

namespace ps::plugin_internal::inpaint_ns {
namespace {
constexpr unsigned char KNOWN = 0, INSIDE = 2, BAND = 1;
struct Point {
  float x, y;
};
float length(const Point& p) {
  return p.x * p.x + p.y * p.y;
}
float dot(const Point& p, const Point& q) {
  return p.x * q.x + p.y * q.y;
}
float min4(float a, float b, float c, float d) {
  a = std::min(a, b);
  c = std::min(c, d);
  return std::min(a, c);
}
template <class T>
struct Plane {
  T* data;
  int rows, cols;
  T& at(int y, int x) const {
    return data[static_cast<std::size_t>(y) * cols + x];
  }
};
struct Entry {
  float time;
  int y, x, order;
  bool operator>(const Entry& rhs) const {
    if (time > rhs.time)
      return true;
    if (time < rhs.time)
      return false;
    return order > rhs.order;
  }
};
static_assert(sizeof(Entry) == 16);
// Each initial band pixel is inserted once. Only INSIDE neighbors are inserted
// later, and are immediately changed to BAND. Total insertions are <= H*W.
struct Heap {
  Entry* data;
  std::size_t size = 0;
  int order = 0;
  void push(int y, int x, float time) {
    new (data + size) Entry{time, y, x, order++};
    ++size;
    std::push_heap(data, data + size, std::greater<Entry>{});
  }
  bool pop(int* y, int* x) {
    if (!size)
      return false;
    *y = data[0].y;
    *x = data[0].x;
    std::pop_heap(data, data + size, std::greater<Entry>{});
    --size;
    return true;
  }
};
float solve(int i1, int j1, int i2, int j2, const Plane<unsigned char>& f,
            const Plane<float>& t) {
  double sol, a11 = t.at(i1, j1), a22 = t.at(i2, j2), m12 = std::min(a11, a22);
  if (f.at(i1, j1) != INSIDE) {
    if (f.at(i2, j2) != INSIDE) {
      if (fabs(a11 - a22) >= 1.0)
        sol = 1 + m12;
      else
        sol = (a11 + a22 + sqrt(2 - (a11 - a22) * (a11 - a22))) * 0.5;
    } else {
      sol = 1 + a11;
    }
  } else if (f.at(i2, j2) != INSIDE) {
    sol = 1 + a22;
  } else {
    sol = 1 + m12;
  }
  return static_cast<float>(sol);
}
}  // namespace
void native(float* pixels, const unsigned char* mask, int h, int w, int range,
            unsigned char* state, float* times, void* entries,
            const CancellationToken& token) {
  Plane<unsigned char> f{state, h + 2, w + 2};
  Plane<float> t{times, h + 2, w + 2}, out{pixels, h, w};
  Heap heap{static_cast<Entry*>(entries)};
  std::uint64_t visits = 0, pops = 0;
  for (int y = 0; y < h + 2; ++y)
    for (int x = 0; x < w + 2; ++x) {
      if ((visits++ & 1023U) == 0)
        check_stop(token);
      new (&f.at(y, x)) unsigned char{0};
      new (&t.at(y, x)) float{1.0e6f};
      if (y > 0 && x > 0 && y <= h && x <= w && mask[(y - 1) * w + x - 1])
        f.at(y, x) = INSIDE;
    }
  for (int y = 1; y <= h; ++y)
    for (int x = 1; x <= w; ++x) {
      if ((visits++ & 1023U) == 0)
        check_stop(token);
      if (f.at(y, x) == KNOWN &&
          (f.at(y - 1, x) == INSIDE || f.at(y, x - 1) == INSIDE ||
           f.at(y + 1, x) == INSIDE || f.at(y, x + 1) == INSIDE)) {
        heap.push(y, x, 0);
        t.at(y, x) = 0;
      }
    }
  // The pinned NS call receives mask, not the separate initial f matrix:
  // initial band state therefore stays KNOWN even while queued.
  int ii, jj, i, j, k, l, q;
  float dist;
  while (heap.pop(&ii, &jj)) {
    if ((pops++ & 63U) == 0)
      check_stop(token);

    f.at(ii, jj) = KNOWN;
    for (q = 0; q < 4; q++) {
      if (q == 0) {
        i = ii - 1;
        j = jj;
      } else if (q == 1) {
        i = ii;
        j = jj - 1;
      } else if (q == 2) {
        i = ii + 1;
        j = jj;
      } else if (q == 3) {
        i = ii;
        j = jj + 1;
      }
      if ((i <= 0) || (j <= 0) || (i > t.rows - 1) || (j > t.cols - 1))
        continue;

      if (f.at(i, j) == INSIDE) {
        dist = min4(
            solve(i - 1, j, i, j - 1, f, t), solve(i + 1, j, i, j - 1, f, t),
            solve(i - 1, j, i, j + 1, f, t), solve(i + 1, j, i, j + 1, f, t));
        t.at(i, j) = dist;

        {
          Point gradI, r;
          float Ia = 0, s = 1.0e-20f, w, dst, dir;

          for (k = i - range; k <= i + range; k++) {
            int km = k - 1 + (k == 1), kp = k - 1 - (k == t.rows - 2);
            for (l = j - range; l <= j + range; l++) {
              if ((visits++ & 4095U) == 0)
                check_stop(token);
              int lm = l - 1 + (l == 1), lp = l - 1 - (l == t.cols - 2);
              if (k > 0 && l > 0 && k < t.rows - 1 && l < t.cols - 1) {
                if ((f.at(k, l) != INSIDE) &&
                    ((l - j) * (l - j) + (k - i) * (k - i) <= range * range)) {
                  r.y = static_cast<float>(i - k);
                  r.x = static_cast<float>(j - l);

                  dst = 1 / (length(r) * length(r) + 1);

                  if (f.at(k + 1, l) != INSIDE) {
                    if (f.at(k - 1, l) != INSIDE) {
                      gradI.x = static_cast<float>(
                          std::abs(out.at(kp + 1, lm) - out.at(kp, lm)) +
                          std::abs(out.at(kp, lm) - out.at(km - 1, lm)));
                    } else {
                      gradI.x = static_cast<float>(std::abs(out.at(kp + 1, lm) -
                                                            out.at(kp, lm))) *
                                2.0f;
                    }
                  } else {
                    if (f.at(k - 1, l) != INSIDE) {
                      gradI.x = static_cast<float>(std::abs(
                                    out.at(kp, lm) - out.at(km - 1, lm))) *
                                2.0f;
                    } else {
                      gradI.x = 0;
                    }
                  }
                  if (f.at(k, l + 1) != INSIDE) {
                    if (f.at(k, l - 1) != INSIDE) {
                      gradI.y = static_cast<float>(
                          std::abs(out.at(km, lp + 1) - out.at(km, lm)) +
                          std::abs(out.at(km, lm) - out.at(km, lm - 1)));
                    } else {
                      gradI.y = static_cast<float>(std::abs(out.at(km, lp + 1) -
                                                            out.at(km, lm))) *
                                2.0f;
                    }
                  } else {
                    if (f.at(k, l - 1) != INSIDE) {
                      gradI.y = static_cast<float>(std::abs(
                                    out.at(km, lm) - out.at(km, lm - 1))) *
                                2.0f;
                    } else {
                      gradI.y = 0;
                    }
                  }

                  gradI.x = -gradI.x;
                  dir = dot(r, gradI);

                  if (fabs(dir) <= 0.01) {
                    dir = 0.000001f;
                  } else {
                    if (!std::isfinite(dot(r, gradI)) ||
                        !std::isfinite(length(r) * length(gradI)))
                      throw NumericFailure{};
                    dir = static_cast<float>(
                        fabs(dot(r, gradI) / sqrt(length(r) * length(gradI))));
                  }
                  w = dst * dir;
                  if (!std::isfinite(gradI.x) || !std::isfinite(gradI.y) ||
                      !std::isfinite(dir) || !std::isfinite(w))
                    throw NumericFailure{};
                  Ia += static_cast<float>(w) *
                        static_cast<float>(out.at(k - 1, l - 1));
                  s += w;
                  if (!std::isfinite(Ia) || !std::isfinite(s))
                    throw NumericFailure{};
                }
              }
            }
          }
          out.at(i - 1, j - 1) =
              static_cast<float>(static_cast<double>(Ia) / s);
        }

        f.at(i, j) = BAND;
        heap.push(i, j, dist);
      }
    }
  }
}
}  // namespace ps::plugin_internal::inpaint_ns
