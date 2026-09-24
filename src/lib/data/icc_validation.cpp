#include "data/icc_validation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "photospider/execution/resource_allocator.hpp"

namespace ps::color_internal {
namespace {
struct Stop {
  Status status;
};
[[noreturn]] void invalid(const char* message) {
  throw Stop{
      {ErrorCode::InvalidArgument, message, FailureReason::InvalidDomain}};
}
void checked(Status status) {
  if (!status.ok())
    throw Stop{std::move(status)};
}
constexpr std::uint32_t signature(const char (&text)[5]) {
  return (static_cast<std::uint32_t>(text[0]) << 24) |
         (static_cast<std::uint32_t>(text[1]) << 16) |
         (static_cast<std::uint32_t>(text[2]) << 8) |
         static_cast<std::uint32_t>(text[3]);
}
struct Reader {
  ByteView bytes;
  void range(std::uint64_t offset, std::uint64_t size) const {
    if (offset > bytes.size() || size > bytes.size() - offset)
      invalid("ICC byte range exceeds frozen resource");
  }
  std::uint32_t integer(std::uint64_t offset, unsigned width = 4) const {
    range(offset, width);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
      value = (value << 8) | bytes[offset + i];
    return value;
  }
  Reader sub(std::uint64_t offset, std::uint64_t size) const {
    range(offset, size);
    return {ByteView(bytes.data() + offset, static_cast<std::size_t>(size))};
  }
};
struct Tag {
  std::uint32_t key, offset, size;
};
class Parser {
 public:
  Parser(ByteView bytes, const ResourceBudget& budget,
         const std::function<Status(std::uint64_t)>& consume)
      : source_{bytes},
        tags_(ResourceAllocator<Tag>(budget)),
        consume_(consume) {}
  void parse() {
    work(128);
    if (source_.bytes.size() < 132 || source_.bytes.size() > UINT32_MAX ||
        source_.integer(0) != source_.bytes.size() ||
        source_.integer(36) != signature("acsp"))
      invalid("ICC requires exact size and profile signature");
    const auto model = source_.integer(16);
    const auto profile_class = source_.integer(12);
    const bool cmyk = model == signature("CMYK");
    if ((cmyk && profile_class != signature("prtr")) ||
        (!cmyk && profile_class != signature("prtr") &&
         profile_class != signature("mntr") &&
         profile_class != signature("scnr") &&
         profile_class != signature("spac") &&
         profile_class != signature("abst")) ||
        (profile_class == signature("abst")) ||
        (!cmyk && model != signature("RGB ") && model != signature("GRAY") &&
         model != signature("XYZ ") && model != signature("Lab ")))
      invalid("unsupported ICC profile class/model");
    major_ = source_.integer(8, 1);
    minor_ = source_.integer(9, 1) >> 4;
    if ((major_ != 2 && major_ != 4) || minor_ > 9 ||
        (source_.integer(9, 1) & 15) > 9 || source_.integer(10, 2))
      invalid("ICC requires v2/v4 BCD version");
    pcs_ = source_.integer(20);
    if (pcs_ != signature("XYZ ") && pcs_ != signature("Lab "))
      invalid("ICC output PCS must be XYZ or Lab");
    const auto year = source_.integer(24, 2), month = source_.integer(26, 2),
               day = source_.integer(28, 2);
    constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30,
                                            31, 31, 30, 31, 30, 31};
    if (!year || month < 1 || month > 12 || !day ||
        day > days[month - 1] + (month == 2 && year % 4 == 0 &&
                                 (year % 100 != 0 || year % 400 == 0)) ||
        source_.integer(30, 2) > 23 || source_.integer(32, 2) > 59 ||
        source_.integer(34, 2) > 59 || source_.integer(64) > 3)
      invalid("invalid ICC date or rendering intent");
    // Vendor bits are permitted. ICC-reserved bits in these fields are zero.
    if ((source_.integer(44) & 0x0000fffcU) ||
        (source_.integer(60) & 0xfffffff0U))
      invalid("invalid ICC reserved flags or attributes");
    zeros(source_, major_ == 2 ? 84 : 100, major_ == 2 ? 44 : 28);
    constexpr std::array<std::int64_t, 3> d50{9642, 10000, 8249};
    for (unsigned i = 0; i < 3; ++i) {
      const auto raw = source_.integer(68 + 4 * i);
      if (raw >> 31)
        invalid("invalid ICC PCS illuminant");
      const auto difference =
          static_cast<std::int64_t>(raw) * 10000 - d50[i] * 65536;
      if (difference <= -32768 || difference >= 32768)
        invalid("ICC PCS illuminant is not D50 to four decimals");
    }
    const auto count = source_.integer(128);
    if (count > (source_.bytes.size() - 132) / 12)
      invalid("ICC tag directory count exceeds resource");
    directory_end_ = 132ULL + 12ULL * count;
    tags_.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
      work(12);
      Tag tag{source_.integer(132 + 12 * i), source_.integer(136 + 12 * i),
              source_.integer(140 + 12 * i)};
      if (tag.offset < directory_end_ || tag.offset % 4 || tag.size < 8)
        invalid("invalid ICC tag offset/alignment/size");
      auto payload = source_.sub(tag.offset, tag.size);
      zeros(payload, 4, 4);
      tags_.push_back(tag);
    }
    std::sort(tags_.begin(), tags_.end(), [&](const auto& a, const auto& b) {
      work(1);
      return a.key < b.key;
    });
    for (std::size_t i = 1; i < tags_.size(); ++i)
      if (tags_[i - 1].key == tags_[i].key)
        invalid("duplicate ICC tag signature");
    text(required(signature("desc")),
         major_ == 2 ? signature("desc") : signature("mluc"));
    text(required(signature("cprt")),
         major_ == 2 ? signature("text") : signature("mluc"));
    auto white = required(signature("wtpt"));
    if (white.bytes.size() != 20 || white.integer(0) != signature("XYZ "))
      invalid("ICC media white requires one XYZNumber");
    if (cmyk) {
      for (auto key : {signature("A2B0"), signature("A2B1"), signature("A2B2")})
        lut(required(key), 4, 3, true);
      for (auto key : {signature("B2A0"), signature("B2A1"), signature("B2A2")})
        lut(required(key), 3, 4, false);
      lut(required(signature("gamt")), 3, 1, false);
    } else {
      const auto has = [&](std::uint32_t key) {
        return std::any_of(tags_.begin(), tags_.end(),
                           [&](const auto& t) { return t.key == key; });
      };
      const bool output_color =
          profile_class == signature("prtr") && model != signature("GRAY");
      const bool bidirectional = profile_class == signature("spac") ||
                                 profile_class == signature("mntr");
      if (model == signature("GRAY") && profile_class != signature("spac"))
        curve(required(signature("kTRC")));
      if (profile_class == signature("spac") || output_color ||
          has(signature("A2B0"))) {
        const unsigned channels = model == signature("GRAY") ? 1 : 3;
        required(signature("A2B0"));
        if ((bidirectional && model != signature("GRAY")) ||
            profile_class == signature("spac") || output_color)
          required(signature("B2A0"));
        if (output_color) {
          for (auto key : {signature("A2B1"), signature("A2B2"),
                           signature("B2A1"), signature("B2A2")})
            required(key);
          lut(required(signature("gamt")), 3, 1, false);
        }
        for (auto key :
             {signature("A2B0"), signature("A2B1"), signature("A2B2")}) {
          if (has(key)) {
            lut(required(key), channels, 3, true);
          }
        }
        for (auto key :
             {signature("B2A0"), signature("B2A1"), signature("B2A2")}) {
          if (has(key)) {
            lut(required(key), 3, channels, false);
          }
        }
      } else if (model == signature("RGB ")) {
        if (pcs_ != signature("XYZ "))
          invalid("matrix/TRC ICC profile requires XYZ PCS");
        for (auto key :
             {signature("rXYZ"), signature("gXYZ"), signature("bXYZ")}) {
          auto xyz = required(key);
          if (xyz.bytes.size() != 20 || xyz.integer(0) != signature("XYZ ")) {
            invalid("invalid ICC colorant XYZ");
          }
        }
        for (auto key :
             {signature("rTRC"), signature("gTRC"), signature("bTRC")}) {
          curve(required(key));
        }
      } else if (model == signature("GRAY")) {
        curve(required(signature("kTRC")));
      } else {
        invalid("profile-defined XYZ/Lab requires an explicit LUT");
      }
    }
    for (const auto& tag : tags_) {
      if (tag.key == signature("chad")) {
        auto matrix = source_.sub(tag.offset, tag.size);
        if (tag.size != 44 || matrix.integer(0) != signature("sf32"))
          invalid(
              "ICC adaptation tag requires nine fixed-point matrix entries");
      }
    }
    std::sort(tags_.begin(), tags_.end(), [&](const auto& a, const auto& b) {
      work(1);
      return a.offset != b.offset ? a.offset < b.offset : a.size < b.size;
    });
    std::uint64_t end = directory_end_;
    for (std::size_t i = 0; i < tags_.size(); ++i) {
      const auto tag = tags_[i];
      work(1);
      if (i && tag.offset == tags_[i - 1].offset &&
          tag.size == tags_[i - 1].size)
        continue;  // Legal complete data sharing across intents/tags.
      if (major_ == 4 && minor_ >= 4 && tag.offset != end)
        invalid("ICC v4.4 tags must be contiguous without partial overlap");
      if (major_ == 4 && minor_ >= 1) {
        if (!i && tag.offset != directory_end_)
          invalid("ICC v4.1+ data must immediately follow tag directory");
        const auto padding = (4 - tag.size % 4) % 4;
        zeros(source_, static_cast<std::uint64_t>(tag.offset) + tag.size,
              padding);
      }
      end = static_cast<std::uint64_t>(tag.offset) + tag.size;
      if (major_ == 4 && minor_ >= 1)
        end = (end + 3) & ~UINT64_C(3);
    }
    if (major_ == 4 && minor_ >= 1 && source_.bytes.size() % 4)
      invalid("ICC v4.1+ total size requires four-byte alignment");
    if (major_ == 4 && minor_ >= 4 && end != source_.bytes.size())
      invalid("ICC v4.4 tagged data must reach end of resource");
  }

 private:
  Reader source_;
  ResourceVector<Tag> tags_;
  const std::function<Status(std::uint64_t)>& consume_;
  unsigned major_ = 0, minor_ = 0;
  std::uint32_t pcs_ = 0;
  std::uint64_t directory_end_ = 0;
  void work(std::uint64_t count) { checked(consume_(count)); }
  void zeros(const Reader& r, std::uint64_t start, std::uint64_t length) {
    r.range(start, length);
    for (std::uint64_t i = 0; i < length; ++i) {
      if (!(i & 1023))
        work(std::min<std::uint64_t>(1024, length - i));
      if (r.bytes[start + i])
        invalid("nonzero ICC reserved/padding bytes");
    }
  }
  Reader required(std::uint32_t key) {
    auto found = std::lower_bound(tags_.begin(), tags_.end(), key,
                                  [&](const auto& tag, auto k) {
                                    work(1);
                                    return tag.key < k;
                                  });
    if (found == tags_.end() || found->key != key)
      invalid("missing required ICC CMYK output tag");
    return source_.sub(found->offset, found->size);
  }
  void ascii(const Reader& r, std::uint64_t offset, std::uint64_t count) {
    r.range(offset, count);
    if (!count || r.integer(offset + count - 1, 1))
      invalid("ICC ASCII text requires terminating NUL");
    for (std::uint64_t i = 0; i + 1 < count; ++i) {
      if (!(i & 1023))
        work(std::min<std::uint64_t>(1024, count - i));
      if (!r.bytes[offset + i] || r.bytes[offset + i] > 127)
        invalid("invalid ICC ASCII text");
    }
  }
  void unicode(const Reader& r, std::uint64_t offset, std::uint64_t size,
               bool terminator) {
    r.range(offset, size);
    if (size % 2 ||
        (terminator && (size < 2 || r.integer(offset + size - 2, 2))))
      invalid("invalid ICC Unicode length/terminator");
    if (terminator)
      size -= 2;
    for (std::uint64_t i = 0; i < size; i += 2) {
      work(2);
      const auto c = r.integer(offset + i, 2);
      if (c >= 0xd800 && c <= 0xdbff) {
        if (i + 4 > size)
          invalid("truncated ICC UTF-16 surrogate pair");
        work(2);
        const auto low = r.integer(offset + i + 2, 2);
        if (low < 0xdc00 || low > 0xdfff)
          invalid("invalid ICC UTF-16 surrogate pair");
        i += 2;
      } else if (c >= 0xdc00 && c <= 0xdfff) {
        invalid("unpaired ICC UTF-16 low surrogate");
      }
    }
  }
  void text(const Reader& r, std::uint32_t expected) {
    if (r.integer(0) != expected)
      invalid("ICC required text tag type mismatch");
    if (expected == signature("text")) {
      ascii(r, 8, r.bytes.size() - 8);
    } else if (expected == signature("desc")) {
      const auto n = r.integer(8);
      ascii(r, 12, n);
      const std::uint64_t base = 12ULL + n;
      const auto count = r.integer(base + 4);
      if (count)
        unicode(r, base + 8, 2ULL * count, true);
      const auto mac = base + 8 + 2ULL * count;
      r.range(mac, 70);
      const auto length = r.integer(mac + 2, 1);
      if (length > 67 || (length && r.integer(mac + 2 + length, 1)))
        invalid("invalid ICC Macintosh description count/terminator");
      if (mac + 70 != r.bytes.size())
        invalid("ICC description size mismatch");
    } else {
      const auto count = r.integer(8), stride = r.integer(12);
      if (!count || stride < 12 || count > (r.bytes.size() - 16) / stride)
        invalid("invalid ICC multilingual record table");
      const auto end = 16ULL + static_cast<std::uint64_t>(count) * stride;
      for (std::uint64_t i = 0; i < count; ++i) {
        work(12);
        const auto at = 16 + i * stride;
        const auto length = r.integer(at + 4), offset = r.integer(at + 8);
        if (offset < end)
          invalid("ICC Unicode string overlaps record table");
        unicode(r, offset, length, false);
      }
    }
  }
  struct Span {
    std::uint64_t begin, end;
    bool curve;
  };
  void multi_lut(const Reader& r, unsigned inputs, unsigned outputs,
                 bool forward) {
    r.range(0, 32);
    zeros(r, 10, 2);
    const auto b = r.integer(12), matrix = r.integer(16), m = r.integer(20),
               clut = r.integer(24), a = r.integer(28);
    if (!b || !a || !clut || static_cast<bool>(matrix) != static_cast<bool>(m))
      invalid("ICC unequal-channel LUT requires A/CLUT/B and paired matrix/M");
    if (matrix && (forward ? outputs : inputs) != 3)
      invalid("ICC LUT matrix requires three adjacent channels");
    std::array<Span, 14> spans{};
    unsigned used = 0;
    const auto append = [&](std::uint64_t begin, std::uint64_t length,
                            bool curve) {
      if (begin < 32 || begin % 4 || used == spans.size())
        invalid("invalid ICC processing element offset");
      r.range(begin, length);
      const Span next{begin, begin + length, curve};
      for (unsigned i = 0; i < used; ++i) {
        const auto prior = spans[i];
        if (next.begin < prior.end && prior.begin < next.end &&
            !(curve && prior.curve && next.begin == prior.begin &&
              next.end == prior.end))
          invalid("ICC processing elements partially overlap");
      }
      spans[used++] = next;
      const auto padding = (4 - length % 4) % 4;
      // The directory size excludes final padding; intermediate curve padding
      // is inside the tag. The resource-level check handles the final bytes.
      if (begin + length < r.bytes.size())
        zeros(r, begin + length, padding);
    };
    const auto curves = [&](std::uint64_t offset, unsigned count) {
      for (unsigned i = 0; i < count; ++i) {
        work(16);
        const auto kind = r.integer(offset);
        zeros(r, offset + 4, 4);
        std::uint64_t size;
        if (kind == signature("curv")) {
          size = 12ULL + 2ULL * r.integer(offset + 8);
        } else if (kind == signature("para")) {
          constexpr std::array<unsigned, 5> parameters{1, 3, 4, 5, 7};
          const auto function = r.integer(offset + 8, 2);
          if (function > 4)
            invalid("unknown ICC parametric curve function");
          zeros(r, offset + 10, 2);
          size = 12 + 4 * parameters[function];
        } else {
          invalid("ICC LUT embeds unsupported curve type");
        }
        append(offset, size, true);
        offset += (size + 3) & ~UINT64_C(3);
      }
    };
    curves(b, forward ? outputs : inputs);
    curves(a, forward ? inputs : outputs);
    if (m)
      curves(m, forward ? outputs : inputs);
    if (matrix)
      append(matrix, 48, false);
    r.range(clut, 20);
    const auto precision = r.integer(clut + 16, 1);
    if (precision != 1 && precision != 2)
      invalid("invalid ICC CLUT precision");
    std::uint64_t bytes = precision * outputs;
    for (unsigned i = 0; i < 16; ++i) {
      const auto grid = r.integer(clut + i, 1);
      if (i >= inputs) {
        if (grid)
          invalid("nonzero unused ICC CLUT dimension");
      } else {
        if (grid < 2 || bytes > r.bytes.size() / grid) {
          invalid("ICC CLUT size exceeds resource");
        }
        bytes *= grid;
      }
    }
    zeros(r, clut + 17, 3);
    append(clut, 20 + bytes, false);
  }
  void curve(const Reader& r) {
    work(16);
    const auto type = r.integer(0);
    std::uint64_t size = 0;
    if (type == signature("curv")) {
      size = 12ULL + 2ULL * r.integer(8);
    } else if (type == signature("para") && major_ == 4) {
      constexpr std::array<unsigned, 5> parameters{1, 3, 4, 5, 7};
      const auto function = r.integer(8, 2);
      if (function > 4) {
        invalid("invalid ICC parametric TRC");
      }
      zeros(r, 10, 2);
      size = 12 + 4 * parameters[function];
    } else {
      invalid("invalid ICC TRC type");
    }
    if (r.bytes.size() != size) {
      invalid("invalid ICC TRC length");
    }
  }
  void lut(const Reader& r, unsigned inputs, unsigned outputs, bool forward) {
    work(64);
    const auto type = r.integer(0);
    if (r.integer(8, 1) != inputs || r.integer(9, 1) != outputs)
      invalid("ICC LUT channel count mismatch");
    if (major_ == 4 &&
        type == (forward ? signature("mAB ") : signature("mBA "))) {
      multi_lut(r, inputs, outputs, forward);
      return;
    }
    if (type != signature("mft1") && type != signature("mft2"))
      invalid("invalid ICC required LUT type");
    const bool wide = type == signature("mft2");
    const std::uint64_t header = wide ? 52 : 48;
    r.range(0, header);
    zeros(r, 11, 1);
    if (forward || pcs_ != signature("XYZ ")) {
      for (unsigned i = 0; i < 9; ++i)
        if (r.integer(12 + 4 * i) != (i % 4 == 0 ? 65536U : 0U))
          invalid("ICC non-XYZ LUT matrix must be identity");
    }
    const auto grid = r.integer(10, 1);
    const auto n = wide ? r.integer(48, 2) : 256,
               m = wide ? r.integer(50, 2) : 256;
    if (grid < 2 || n < 2 || n > 4096 || m < 2 || m > 4096)
      invalid("invalid ICC LUT grid or table entries");
    std::uint64_t clut = outputs;
    for (unsigned i = 0; i < inputs; ++i) {
      if (clut > r.bytes.size() / grid)
        invalid("ICC CLUT product exceeds resource");
      clut *= grid;
    }
    const auto total =
        header + (wide ? 2 : 1) * (inputs * n + clut + outputs * m);
    if (total != r.bytes.size())
      invalid("ICC LUT table length mismatch");
  }
};
}  // namespace
Status validate_icc(ByteView bytes, const ResourceBudget& budget,
                    const std::function<Status(std::uint64_t)>& consume) {
  try {
    Parser(bytes, budget, consume).parse();
    return Status::success();
  } catch (const Stop& stop) {
    return stop.status;
  } catch (const std::bad_alloc&) {
    return {ErrorCode::ResourceExhausted, "ICC parser metadata capacity",
            FailureReason::CapacityLimit};
  }
}
}  // namespace ps::color_internal
