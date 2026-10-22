#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

#include "03-generation/paths.hpp"

namespace ps::plugin_internal::generation {
inline constexpr std::uint64_t kNoPathRow = UINT64_MAX;

// Separate fields let source-record maps, containment and assignment metadata
// share one admitted O(T) array without overwriting each other's indices.
struct ReverseKnotState final {
  using Wide = __int128;
  std::uint64_t mapped = kNoPathRow, source = kNoPathRow;
  std::uint64_t owner = kNoPathRow, relative = 0, position = 0;
  std::uint64_t root = 0, next = kNoPathRow, previous = kNoPathRow;
  std::uint64_t assigned = 0, way = 0;
  Wide u = 0, v = 0, minimum = 0;
  bool visited = false;
};

struct PrimitiveReverseLayout final {
  std::uint64_t knots, states, arcs, arc_hash, bytes;
  PrimitiveReverseLayout(std::uint64_t begin, const PathInfo& info) {
    knots = begin;
    begin += info.capacities[7];
    constexpr auto alignment = alignof(ReverseKnotState);
    states = (begin + alignment - 1) & ~(alignment - 1);
    arcs = states + (info.capacities[5] + 1) * sizeof(ReverseKnotState);
    arc_hash = arcs + 2 * info.capacities[3] * sizeof(std::uint64_t);
    bytes = arc_hash + (2 * info.capacities[3] + 1) * sizeof(std::uint64_t);
  }
};

class ReverseKnots final {
 public:
  ReverseKnots(const Reader& input, const Dense& output, Math& math,
               ReverseKnotState* metadata, std::uint8_t* occupied,
               std::uint64_t capacity)
      : reader(input),
        out(output),
        m(math),
        state(metadata),
        occupied(occupied),
        capacity(capacity) {}
  std::uint64_t run(std::uint64_t rows, std::uint64_t old_count) {
    if (!rows) {
      clear_tail(0, old_count);
      return 0;
    }
    for (std::uint64_t offset = 0; offset < old_count;) {
      const auto count = std::min(UINT64_C(65536), old_count - offset);
      out.work.poll(count);
      std::memset(occupied + offset, 0, count);
      offset += count;
    }
    bool compatible = true;
    Wide total = 0;
    for (std::uint64_t row = 0; row < rows; ++row) {
      out.work.poll();
      const auto count = record(row, 4), begin = record(row, 3);
      total += count;
      if (!compatible) {
        continue;
      }
      for (std::uint64_t j = 0; j < count; ++j) {
        const auto bits = value(row, j);
        if (occupied[begin + j] && out.get(10, begin + j) != bits) {
          compatible = false;
          break;
        }
        out.set(10, begin + j, bits);
        occupied[begin + j] = 1;
      }
    }
    if (compatible) {
      return old_count;
    }
    std::uint64_t end = 0;
    if (total <= capacity) {
      // Common conflicting case: enough room for disjoint slices.
      for (std::uint64_t row = 0; row < rows; ++row) {
        out.set(8, 5 * row + 3, end);
        for (std::uint64_t j = 0; j < record(row, 4); ++j) {
          out.set(10, end++, value(row, j));
        }
      }
    } else {
      end = pack(rows);
    }
    clear_tail(end, old_count);
    return end;
  }

 private:
  using Wide = ReverseKnotState::Wide;
  const Reader& reader;
  const Dense& out;
  Math& m;
  ReverseKnotState* state;
  std::uint8_t* occupied;
  std::uint64_t capacity;
  std::array<std::uint64_t, 2> cached{kNoPathRow, kNoPathRow};
  std::array<Rational, 2> endpoints{};
  unsigned replace = 0;

  std::uint64_t record(std::uint64_t row, unsigned column) const {
    return static_cast<std::uint64_t>(
        reader.index(0, 8, {state[row].source, column}));
  }
  std::uint64_t value(std::uint64_t row, std::uint64_t index) {
    out.work.poll();
    m.exact.reset();
    unsigned slot = 0;
    if (cached[0] != row) {
      slot = 1;
      if (cached[1] != row) {
        slot = replace;
        replace ^= 1;
        const auto begin = record(row, 3);
        endpoints[slot] =
            m.add(m.input(reader, 0, 10, {begin + record(row, 0)}),
                  m.input(reader, 0, 10, {begin + record(row, 2)}));
        cached[slot] = row;
      }
    }
    const auto source = record(row, 3) + record(row, 4) - 1 - index;
    return m.rounded(m.sub(endpoints[slot], m.input(reader, 0, 10, {source})),
                     false);
  }
  bool equal(std::uint64_t a, std::uint64_t ab, std::uint64_t b,
             std::uint64_t bb, std::uint64_t count) {
    for (std::uint64_t j = 0; j < count; ++j) {
      out.work.poll();
      if (value(a, ab + j) != value(b, bb + j)) {
        return false;
      }
    }
    return true;
  }
  std::uint64_t overlap(std::uint64_t a, std::uint64_t b) {
    if (a == b) {
      return 0;
    }
    const auto na = record(a, 4), nb = record(b, 4);
    for (auto count = std::min(na, nb); count; --count) {
      out.work.poll();
      if (equal(a, na - count, b, 0, count)) {
        return count;
      }
    }
    return 0;
  }
  std::uint64_t roots(std::uint64_t rows) {
    std::uint64_t count = 0;
    for (std::uint64_t row = 0; row < rows; ++row) {
      out.work.poll();
      state[row].owner = row;
      const auto length = record(row, 4);
      for (std::uint64_t other = 0; other < rows; ++other) {
        out.work.poll();
        const auto other_length = record(other, 4);
        if (other_length < length || (other_length == length && other >= row)) {
          continue;
        }
        for (std::uint64_t at = 0; at <= other_length - length; ++at) {
          out.work.poll();
          if (equal(row, 0, other, at, length)) {
            state[row].owner = other;
            state[row].relative = at;
            break;
          }
        }
        if (state[row].owner != row) {
          break;
        }
      }
      if (state[row].owner == row) {
        state[count++].root = row;
      }
    }
    return count;
  }
  void assignment(std::uint64_t n) {
    constexpr Wide infinity = Wide{1} << 120;
    // Complete assignment with zero-cost nonedges extends any positive partial
    // matching. Only positive edges are retained after solving.
    for (std::uint64_t i = 1; i <= n; ++i) {
      out.work.poll();
      state[0].assigned = i;
      std::uint64_t column = 0;
      for (std::uint64_t j = 0; j <= n; ++j) {
        out.work.poll();
        state[j].minimum = infinity;
        state[j].visited = false;
      }
      do {
        state[column].visited = true;
        const auto row = state[column].assigned;
        Wide delta = infinity;
        std::uint64_t next = 0;
        for (std::uint64_t j = 1; j <= n; ++j) {
          out.work.poll();
          if (state[j].visited) {
            continue;
          }
          const Wide cost = -static_cast<Wide>(
              overlap(state[row - 1].root, state[j - 1].root));
          const auto reduced = cost - state[row].u - state[j].v;
          if (reduced < state[j].minimum) {
            state[j].minimum = reduced;
            state[j].way = column;
          }
          if (state[j].minimum < delta) {
            delta = state[j].minimum;
            next = j;
          }
        }
        for (std::uint64_t j = 0; j <= n; ++j) {
          out.work.poll();
          if (state[j].visited) {
            state[state[j].assigned].u += delta;
            state[j].v -= delta;
          } else if (j) {
            state[j].minimum -= delta;
          }
        }
        column = next;
      } while (state[column].assigned);
      do {
        const auto previous = state[column].way;
        state[column].assigned = state[previous].assigned;
        column = previous;
      } while (column);
    }
  }
  std::uint64_t pack(std::uint64_t rows) {
    const auto n = roots(rows);
    assignment(n);
    Wide needed = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      out.work.poll();
      needed += record(state[i].root, 4);
    }
    for (std::uint64_t j = 1; j <= n; ++j) {
      out.work.poll();
      const auto a = state[state[j].assigned - 1].root;
      const auto b = state[j - 1].root;
      const auto shared = overlap(a, b);
      if (shared) {
        state[a].next = b;
        state[b].previous = a;
        needed -= shared;
      }
    }
    operational_capacity(needed <= capacity);
    // Nondecreasing, noncontained strings have an acyclic positive-overlap
    // graph: every proper suffix/prefix edge increases lexicographic order.
    // Thus maximum-weight matching gives a minimum-length path cover.
    std::uint64_t end = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      out.work.poll();
      auto row = state[i].root;
      if (state[row].previous != kNoPathRow) {
        continue;
      }
      std::uint64_t shared = 0;
      while (row != kNoPathRow) {
        out.work.poll();
        state[row].position = end - shared;
        for (auto j = shared; j < record(row, 4); ++j) {
          out.set(10, end++, value(row, j));
        }
        const auto next = state[row].next;
        shared = next == kNoPathRow ? 0 : overlap(row, next);
        row = next;
      }
    }
    for (std::uint64_t row = 0; row < rows; ++row) {
      out.work.poll();
      auto owner = row;
      std::uint64_t relative = 0;
      while (state[owner].owner != owner) {
        out.work.poll();
        relative += state[owner].relative;
        owner = state[owner].owner;
      }
      out.set(8, 5 * row + 3, state[owner].position + relative);
    }
    return end;
  }
  void clear_tail(std::uint64_t begin, std::uint64_t end) const {
    for (auto j = begin; j < end; ++j) {
      out.work.poll();
      out.set(10, j, 0);
    }
  }
};

inline void reverse_spline_pools(const Reader& reader, const Dense& out,
                                 Math& m,
                                 const std::array<std::uint64_t, 8>& count,
                                 const std::array<std::uint64_t, 8>& capacity,
                                 ReverseKnotState* state,
                                 std::uint8_t* occupied,
                                 std::array<std::uint64_t, 8>* used) {
  for (std::uint64_t row = 0; row <= count[5]; ++row) {
    out.work.poll();
    new (state + row) ReverseKnotState{};
  }
  // A single reversal preserves all overlap/containment relationships in the
  // control pool. Controls and weights move together, preserving their bits.
  for (std::uint64_t j = 0; j < count[6]; ++j) {
    out.work.poll(3);
    for (unsigned axis = 0; axis < 2; ++axis) {
      out.set(9, 2 * j + axis, reader.bits(0, 9, {count[6] - 1 - j, axis}));
    }
    out.set(11, j, reader.bits(0, 11, {count[6] - 1 - j}));
  }
  std::uint64_t rows = 0;
  for (std::uint64_t ref = 0; ref < count[0]; ++ref) {
    out.work.poll();
    if (out.get(0, 2 * ref) != 5) {
      continue;
    }
    const auto source = out.get(0, 2 * ref + 1);
    if (state[source].mapped == kNoPathRow) {
      const auto row = rows++;
      state[source].mapped = row;
      state[row].source = source;
      for (unsigned j = 0; j < 5; ++j) {
        out.set(8, 5 * row + j, reader.bits(0, 8, {source, j}));
      }
      const auto begin =
          static_cast<std::uint64_t>(reader.index(0, 8, {source, 1}));
      const auto length =
          static_cast<std::uint64_t>(reader.index(0, 8, {source, 2}));
      out.set(8, 5 * row + 1, count[6] - begin - length);
    }
    out.set(0, 2 * ref + 1, state[source].mapped);
  }
  for (auto row = rows; row < count[5]; ++row) {
    out.work.poll(5);
    for (unsigned j = 0; j < 5; ++j) {
      out.set(8, 5 * row + j, 0);
    }
  }
  (*used)[5] = rows;
  ReverseKnots knots(reader, out, m, state, occupied, capacity[7]);
  (*used)[7] = knots.run(rows, count[7]);
}

inline void reverse_arc_pool(const Reader& reader, const Dense& out, Math& m,
                             const std::array<std::uint64_t, 8>& count,
                             std::uint64_t capacity, std::uint64_t* mapped,
                             std::uint64_t* table,
                             std::array<std::uint64_t, 8>* used) {
  for (std::uint64_t i = 0; i < 2 * count[3]; ++i) {
    out.work.poll();
    mapped[i] = kNoPathRow;
  }
  const auto table_size = 2 * std::min(capacity, 2 * count[3]) + 1;
  for (std::uint64_t i = 0; i < table_size; ++i) {
    out.work.poll();
    table[i] = 0;
  }
  std::uint64_t rows = 0;
  for (std::uint64_t ref = 0; ref < count[0]; ++ref) {
    out.work.poll();
    const auto kind = out.get(0, 2 * ref);
    if (kind != 2 && kind != 3) {
      continue;
    }
    const auto source = out.get(0, 2 * ref + 1);
    auto& target = mapped[2 * source + (kind == 3 ? 1 : 0)];
    if (target == kNoPathRow) {
      m.exact.reset();
      std::array<std::uint64_t, 8> value{};
      for (unsigned j = 0; j < 8; ++j) {
        value[j] = reader.bits(0, 6, {source, j});
      }
      if (kind == 2) {
        value[6] = m.rounded(m.add(m.input(reader, 0, 6, {source, 6}),
                                   m.input(reader, 0, 6, {source, 7})),
                             false);
      }
      value[7] ^= UINT64_C(1) << 63;
      std::uint64_t hash = UINT64_C(14695981039346656037);
      for (const auto bits : value) {
        hash = (hash ^ bits) * UINT64_C(1099511628211);
        hash ^= hash >> 32;
      }
      auto slot = hash % table_size;
      while (table[slot]) {
        out.work.poll(8);
        const auto row = table[slot] - 1;
        unsigned j = 0;
        while (j < 8 && out.get(6, 8 * row + j) == value[j]) {
          ++j;
        }
        if (j == 8) {
          break;
        }
        slot = slot + 1 == table_size ? 0 : slot + 1;
      }
      if (!table[slot]) {
        operational_capacity(rows < capacity);
        for (unsigned j = 0; j < 8; ++j) {
          out.set(6, 8 * rows + j, value[j]);
        }
        table[slot] = ++rows;
      }
      target = table[slot] - 1;
    }
    out.set(0, 2 * ref + 1, target);
  }
  for (auto row = rows; row < count[3]; ++row) {
    out.work.poll(8);
    for (unsigned j = 0; j < 8; ++j) {
      out.set(6, 8 * row + j, 0);
    }
  }
  (*used)[3] = rows;
}
}  // namespace ps::plugin_internal::generation
