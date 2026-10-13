#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "execution/native_gpu_metadata.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps::gpu_internal;  // NOLINT(build/namespaces)
  // Binary keys include NUL and unsigned high bytes. Compare every possible
  // split against the flattened ordering, including prefixes and empty parts.
  const std::vector<std::string> keys{"",
                                      "a",
                                      "ab",
                                      "abc",
                                      std::string("a\0b", 3),
                                      std::string("a\xff", 2),
                                      std::string("\0\0", 2),
                                      "abcd"};
  PipelineKeyLess less;
  for (const auto& left : keys) {
    for (std::size_t split = 0; split <= left.size(); ++split) {
      const std::string_view bytes(left);
      PipelineKeyView view{{std::string_view{}, bytes.substr(0, split),
                            std::string_view{}, bytes.substr(split),
                            std::string_view{}}};
      const auto copy = view.own({});
      PS_CHECK(std::string(copy.data(), copy.size()) == left);
      for (const auto& right : keys) {
        NativeString owned(right.data(), right.size());
        PS_CHECK(less(view, owned) == (left < right));
        PS_CHECK(less(owned, view) == (right < left));
      }
    }
  }
  auto root = std::make_shared<ps::ResourceBudget>();
  auto account = std::make_shared<MetadataAccount>(root);
  auto cache = make_pipeline_cache<unsigned>(account);
  std::string source(65536, 's');
  PipelineKeyView query{{source, std::string_view("\0", 1), "main"}};
  cache->entries.emplace(query.own(account), 7);
  const auto cached = root->statistics().live;
  PS_CHECK(cached[ps::ResourceKind::Metadata] > source.size());
  // Exhaust the entry quota. A warm lookup must retain the existing pipeline
  // without allocation, eviction or a sticky resource failure.
  ps::ResourceCapacity capacity;
  capacity[ps::ResourceKind::Entries] =
      root->available_capacity()[ps::ResourceKind::Entries];
  auto held = root->reserve(capacity).take_value();
  auto full = root->statistics().live;
  ps::ErrorCode failure = ps::ErrorCode::Ok;
  {
    ps::ResourceAllocationScope scope(*root, &failure);
    PS_CHECK(cache->entries.find(query)->second == 7);
  }
  PS_CHECK(failure == ps::ErrorCode::Ok);
  PS_CHECK(root->statistics().live.values == full.values);
  held = {};
  PS_CHECK(root->statistics().live.values == cached.values);
  source[0] = 't';
  PS_CHECK(cache->entries.find(query) == cache->entries.end());
  source[0] = 's';
  PS_CHECK(cache->entries.find(query)->second == 7);
  cache->clear();
  for (auto value : root->statistics().live.values)
    PS_CHECK(value == 0);
  return 0;
}
