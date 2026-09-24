#include "photospider/data/ocio_config_resource.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "data/content_digest.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/utf8_validation.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message, FailureReason::InvalidDomain};
}
bool name(const std::string& s) {
  return !s.empty() && s.size() <= 128 && plugin_internal::valid_utf8_key(s);
}
std::uint64_t read(ByteView bytes, std::size_t* offset) {
  std::uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i) {
    n |= static_cast<std::uint64_t>(bytes[(*offset)++]) << (8 * i);
  }
  return n;
}
ByteView field(ByteView bytes, std::size_t* offset) {
  auto n = read(bytes, offset);
  auto result = ByteView(bytes.data() + *offset, n);
  *offset += n;
  return result;
}
bool equal(ByteView bytes, const std::string& s) {
  return bytes.size() == s.size() &&
         !std::memcmp(bytes.data(), s.data(), s.size());
}
}  // namespace
struct OcioConfigResource::Impl final {
  std::shared_ptr<const CpuStorage> storage;
  ColorProfileIdentity identity;
};
Result<OcioConfigResource> OcioConfigResource::import(
    const OcioConfigSnapshot& snapshot, const ResourceBudget& resources,
    const CancellationToken& cancellation, std::uint64_t maximum_work) try {
  using Answer = Result<OcioConfigResource>;
  if (snapshot.config.empty() || snapshot.engine_version != "2.5.2" ||
      !name(snapshot.build_identity) || !name(snapshot.settings) ||
      snapshot.spaces.empty() ||
      snapshot.files.size() + snapshot.context.size() + snapshot.spaces.size() >
          1024) {
    return Answer(invalid("invalid explicit OCIO resource snapshot"));
  }
  for (const auto& e : snapshot.spaces) {
    if (!name(e.first) || (e.second != "scene" && e.second != "display")) {
      return Answer(invalid("invalid OCIO space declaration"));
    }
  }
  for (const auto& e : snapshot.context) {
    if (!name(e.first) || e.second.size() > 4096 ||
        e.second.find('\0') != std::string::npos ||
        e.second.find('$') != std::string::npos) {
      return Answer(
          invalid("OCIO context must contain explicit resolved values"));
    }
  }
  for (const auto& e : snapshot.files) {
    if (!name(e.first)) {
      return Answer(invalid("invalid OCIO logical file name"));
    }
  }
  auto work = [&](std::uint64_t n) {
    if (cancellation.cancelled()) {
      return Status{ErrorCode::Cancelled, "OCIO snapshot cancelled"};
    }
    if (n > maximum_work) {
      return Status{ErrorCode::ResourceExhausted, "OCIO snapshot work limit",
                    FailureReason::WorkLimit};
    }
    maximum_work -= n;
    return resources.consume({n});
  };
  auto status = work(0);
  if (!status.ok()) {
    return Answer(status);
  }
  // Two passes: compute exact framed size without scratch payload, then copy
  // directly into one admitted immutable backing in bounded cancellation
  // chunks.
  std::uint64_t size = 0;
  const auto visit = [&](auto emit) {
    emit("identity", "engine", snapshot.engine_version.data(),
         snapshot.engine_version.size());
    emit("identity", "build", snapshot.build_identity.data(),
         snapshot.build_identity.size());
    emit("identity", "settings", snapshot.settings.data(),
         snapshot.settings.size());
    emit("config", "", snapshot.config.data(), snapshot.config.size());
    for (const auto& e : snapshot.files) {
      emit("files", e.first, e.second.data(), e.second.size());
    }
    for (const auto& e : snapshot.context) {
      emit("context", e.first, e.second.data(), e.second.size());
    }
    for (const auto& e : snapshot.spaces) {
      emit("spaces", e.first, e.second.data(), e.second.size());
    }
  };
  bool overflow = false;
  visit([&](const std::string& category, const std::string& key, const void*,
            std::size_t n) {
    if (n > UINT64_MAX - size - 24 - category.size() - key.size()) {
      overflow = true;
    } else {
      size += 24 + category.size() + key.size() + n;
    }
  });
  if (overflow || size > SIZE_MAX) {
    return Answer(invalid("OCIO snapshot size overflow"));
  }
  auto allocated = resources.allocator().allocate(size);
  if (!allocated.ok()) {
    return Answer(allocated.status());
  }
  auto buffer = allocated.take_value();
  std::size_t at = 0;
  content_internal::Sha256 hash;
  const auto put = [&](const void* pointer, std::size_t n) {
    if (!status.ok()) {
      return;
    }
    std::uint8_t header[8];
    for (unsigned i = 0; i < 8; ++i) {
      header[i] = static_cast<std::uint8_t>(n >> (8 * i));
    }
    std::memcpy(buffer.data() + at, header, 8);
    hash.bytes(header, 8);
    at += 8;
    const auto* bytes = static_cast<const std::uint8_t*>(pointer);
    for (std::size_t offset = 0; offset < n;) {
      const auto count = std::min<std::size_t>(1024, n - offset);
      status = work(count);
      if (!status.ok()) {
        return;
      }
      std::memcpy(buffer.data() + at, bytes + offset, count);
      hash.bytes(bytes + offset, count);
      at += count;
      offset += count;
    }
  };
  visit([&](const std::string& category, const std::string& key,
            const void* bytes, std::size_t n) {
    put(category.data(), category.size());
    put(key.data(), key.size());
    put(bytes, n);
  });
  if (!status.ok()) {
    return Answer(status);
  }
  auto impl = std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources));
  impl->storage = std::move(buffer).freeze();
  impl->identity.byte_length = size;
  const auto digest = hash.finish();
  const std::string digits = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    impl->identity.sha256[i] = static_cast<std::uint8_t>(
        (digits.find(digest[2 * i]) << 4) | digits.find(digest[2 * i + 1]));
  }
  return Answer(OcioConfigResource(std::move(impl)));
} catch (const std::bad_alloc&) {
  return Result<OcioConfigResource>(Status{ErrorCode::ResourceExhausted,
                                           "OCIO snapshot metadata capacity",
                                           FailureReason::CapacityLimit});
}
const ColorProfileIdentity& OcioConfigResource::identity() const {
  if (!impl_) {
    throw std::logic_error("invalid OCIO snapshot");
  }
  return impl_->identity;
}
const std::shared_ptr<const CpuStorage>& OcioConfigResource::storage() const {
  if (!impl_) {
    throw std::logic_error("invalid OCIO snapshot");
  }
  return impl_->storage;
}
Result<ByteView> OcioConfigResource::lookup(const std::string& category,
                                            const std::string& key) const {
  if (!impl_) {
    return Result<ByteView>(invalid("invalid OCIO snapshot"));
  }
  auto bytes = impl_->storage->bytes();
  std::size_t at = 0;
  while (at < bytes.size()) {
    auto type = field(bytes, &at), name = field(bytes, &at),
         value = field(bytes, &at);
    if (equal(type, category) && equal(name, key)) {
      return Result<ByteView>(value);
    }
  }
  return Result<ByteView>(
      Status{ErrorCode::NotFound, "OCIO snapshot lookup is absent"});
}
Status OcioConfigResource::validate_space(const std::string& name,
                                          const std::string& reference) const {
  auto space = lookup("spaces", name);
  if (!space.ok() || !equal(space.value(), reference)) {
    return invalid("unresolved OCIO space/reference declaration");
  }
  return Status::success();
}
Result<OcioConfigResource> OcioConfigResource::reference(
    const ResourceBudget& resources) const try {
  if (!impl_) {
    return Result<OcioConfigResource>(invalid("invalid OCIO snapshot"));
  }
  auto storage = resources.reference(impl_->storage);
  if (!storage.ok()) {
    return Result<OcioConfigResource>(storage.status());
  }
  auto impl = std::allocate_shared<Impl>(ResourceAllocator<Impl>(resources));
  impl->identity = impl_->identity;
  impl->storage = storage.take_value();
  return Result<OcioConfigResource>(OcioConfigResource(std::move(impl)));
} catch (const std::bad_alloc&) {
  return Result<OcioConfigResource>(
      Status{ErrorCode::ResourceExhausted, "OCIO reference capacity"});
}
}  // namespace ps
