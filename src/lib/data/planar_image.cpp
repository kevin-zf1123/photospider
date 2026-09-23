#include "photospider/data/planar_image.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ps {
PlanarPageBudget::PlanarPageBudget(std::uint64_t maximum_bytes, Reserve reserve,
                                   std::shared_ptr<void> accounting_domain)
    : maximum_bytes_(maximum_bytes),
      reserve_(std::move(reserve)),
      accounting_domain_(std::move(accounting_domain)) {
}  // NOLINT(whitespace/indent_namespace)
std::uint64_t PlanarPageBudget::live_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_bytes_;
}
Result<std::shared_ptr<void>> PlanarPageBudget::charge(std::uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bytes > maximum_bytes_ - live_bytes_)
    return Result<std::shared_ptr<void>>(Status::failure(
        ErrorCode::ResourceExhausted, "aggregate image page budget exceeded"));
  std::shared_ptr<void> lease;
  if (reserve_ && bytes) {
    auto reserved = reserve_(bytes);
    if (!reserved.ok())
      return reserved;
    lease = reserved.take_value();
  }
  live_bytes_ += bytes;
  return Result<std::shared_ptr<void>>(std::move(lease));
}
void PlanarPageBudget::release(std::uint64_t bytes) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  live_bytes_ -= bytes;
}
namespace {
using Interval = std::pair<std::uint64_t, std::uint64_t>;

bool add(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
  if (b > UINT64_MAX - a)
    return false;
  *out = a + b;
  return true;
}
bool multiply(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
  if (b && a > UINT64_MAX / b)
    return false;
  *out = a * b;
  return true;
}
bool align_up(std::uint64_t value, std::uint64_t alignment,
              std::uint64_t* out) {
  if (!alignment)
    return false;
  const auto remainder = value % alignment;
  return add(value, remainder ? alignment - remainder : 0, out);
}
bool metadata_capacity(std::uint64_t base, std::uint64_t rows,
                       std::uint64_t intervals, std::uint64_t pages,
                       std::uint64_t* out) {
  std::uint64_t row_bytes = 0, interval_bytes = 0, page_bytes = 0;
  return multiply(rows, 128, &row_bytes) &&
         multiply(intervals, 32, &interval_bytes) &&
         multiply(pages, 192, &page_bytes) && add(base, row_bytes, out) &&
         add(*out, interval_bytes, out) && add(*out, page_bytes, out);
}
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
Status exhausted(const char* message) {
  return Status::failure(ErrorCode::ResourceExhausted, message);
}
std::uint64_t host_page_size() {
#if defined(_WIN32)
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  const auto size = sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<std::uint64_t>(size) : 0;
#endif
}
std::uint64_t reservation_granularity() {
#if defined(_WIN32)
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwAllocationGranularity;
#else
  return host_page_size();
#endif
}
void* reserve_address(std::size_t size) {
#if defined(_WIN32)
  return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
  void* address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return address == MAP_FAILED ? nullptr : address;
#endif
}
bool provide_page(void* address, std::size_t size) {
#if defined(_WIN32)
  return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) == address;
#else
  return mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
#endif
}
void withdraw_page(void* address, std::size_t size) noexcept {
#if defined(_WIN32)
  VirtualFree(address, size, MEM_DECOMMIT);
#else
  madvise(address, size, MADV_DONTNEED);
  mprotect(address, size, PROT_NONE);
#endif
}
void release_address(void* address, std::size_t size) noexcept {
#if defined(_WIN32)
  VirtualFree(address, 0, MEM_RELEASE);
#else
  munmap(address, size);
#endif
}
}  // namespace

struct PlanarImage::Impl final {
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  ResourceBindings resources;
  PlanarImageConfig config;
  std::uint64_t height = 0, width = 0, channels = 0, scalar_width = 0;
  std::uint64_t page = 0, plane_step = 0, row_pitch = 0;
  std::uint64_t full_step = 0, edge_step = 0, columns = 0;
  std::uint64_t virtual_bytes = 0, sample_count = 0;
  std::uint8_t* base = nullptr;
  mutable std::shared_timed_mutex mutex;
  std::set<std::uint64_t> pages;
  std::vector<std::shared_ptr<void>> page_leases;
  std::shared_ptr<void> metadata_lease;
  std::uint64_t metadata_charge = 0;
  std::uint64_t base_metadata = 0;
  std::uint64_t execution_pins = 0;
  // One interval set per physical plane and logical row. Half-open X ranges.
  std::map<std::uint64_t, std::vector<Interval>> coverage;

  ~Impl() {
    if (base)
      release_address(base, static_cast<std::size_t>(virtual_bytes));
    if (config.aggregate_budget)
      config.aggregate_budget->release(pages.size() * page + metadata_charge);
  }

  std::uint64_t offset(std::uint64_t y, std::uint64_t x,
                       std::uint64_t channel) const noexcept {
    const auto plane = channel * plane_step;
    if (config.order == ImagePlaneOrder::Continuous)
      return plane + y * row_pitch + x * scalar_width;
    const auto ty = y / config.tile_height, tx = x / config.tile_width;
    const auto local_y = y % config.tile_height;
    const auto local_x = x % config.tile_width;
    const auto step = ty < height / config.tile_height ? full_step : edge_step;
    return plane + ty * columns * full_step + tx * step +
           local_y * config.tile_width * scalar_width + local_x * scalar_width;
  }

  std::uint64_t channel_of(const std::vector<std::uint64_t>& at) const {
    return config.channel_axis ? at[*config.channel_axis] : 0;
  }
  std::uint64_t row_key(std::uint64_t channel, std::uint64_t y) const {
    return channel * height + y;
  }
};

Status PlanarImage::validate_layout(const ValueDescriptor& descriptor,
                                    const PlanarImageLayout& layout) {
  const auto rank = descriptor.shape.size();
  if ((rank != 2 && rank != 3) || layout.height_axis >= rank ||
      layout.width_axis >= rank || layout.height_axis == layout.width_axis ||
      (rank == 2 && layout.channel_axis) ||
      (rank == 3 && (!layout.channel_axis || *layout.channel_axis >= rank ||
                     *layout.channel_axis == layout.height_axis ||
                     *layout.channel_axis == layout.width_axis)) ||
      (layout.order != ImagePlaneOrder::Continuous &&
       layout.order != ImagePlaneOrder::Tiled) ||
      (layout.order == ImagePlaneOrder::Tiled && layout.row_pitch_bytes) ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](auto extent) { return extent == 0; }))
    return invalid("invalid structural planar layout");
  std::uint64_t scalar_width = 0;
  try {
    scalar_width = Value::element_size(descriptor.element_type);
  } catch (const std::invalid_argument&) {
    return invalid("invalid planar dtype");
  }
  std::uint64_t row_bytes = 0;
  if (!multiply(descriptor.shape[layout.width_axis], scalar_width, &row_bytes))
    return exhausted("planar row width overflow");
  if (layout.row_pitch_bytes && (layout.row_pitch_bytes < row_bytes ||
                                 layout.row_pitch_bytes % scalar_width != 0))
    return invalid("invalid planar row pitch");
  const auto channels =
      layout.channel_axis ? descriptor.shape[*layout.channel_axis] : 1;
  if (layout.groups.size() > channels || layout.groups.size() > 64)
    return invalid("too many planar component groups");
  std::vector<Interval> groups;
  groups.reserve(layout.groups.size());
  for (const auto& group : layout.groups) {
    if (group.role.empty() || group.role.size() > 128 || !group.channel_count ||
        group.first_channel >= channels ||
        group.channel_count > channels - group.first_channel)
      return invalid("invalid planar component group");
    groups.emplace_back(group.first_channel,
                        group.first_channel + group.channel_count);
  }
  std::sort(groups.begin(), groups.end());
  for (std::size_t i = 1; i < groups.size(); ++i)
    if (groups[i].first < groups[i - 1].second)
      return invalid("overlapping planar component groups");
  return Status::success();
}

Result<PlanarImage> PlanarImage::create(ValueDescriptor descriptor,
                                        PlanarImageConfig config,
                                        std::vector<ValueFacet> facets,
                                        ResourceBindings resources) {
  PlanarImageLayout layout;
  layout.order = config.order;
  layout.height_axis = config.height_axis;
  layout.width_axis = config.width_axis;
  layout.channel_axis = config.channel_axis;
  layout.row_pitch_bytes = config.row_pitch_bytes;
  layout.groups = std::move(config.groups);
  auto layout_status = validate_layout(descriptor, layout);
  if (!layout_status.ok())
    return Result<PlanarImage>(layout_status);
  config.groups = std::move(layout.groups);
  auto facet_status = input_internal::canonicalize_facets(&facets);
  if (!facet_status.ok())
    return Result<PlanarImage>(facet_status);
  auto selected = resources.select(facets);
  if (!selected.ok())
    return Result<PlanarImage>(selected.status());
  const auto rank = descriptor.shape.size();
  if ((rank != 2 && rank != 3) || config.height_axis >= rank ||
      config.width_axis >= rank || config.height_axis == config.width_axis ||
      (rank == 2 && config.channel_axis) ||
      (rank == 3 && (!config.channel_axis || *config.channel_axis >= rank ||
                     *config.channel_axis == config.height_axis ||
                     *config.channel_axis == config.width_axis)) ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](auto extent) { return extent == 0; }) ||
      config.tile_height == 0 || config.tile_width == 0 ||
      (config.order != ImagePlaneOrder::Continuous &&
       config.order != ImagePlaneOrder::Tiled))
    return Result<PlanarImage>(invalid("invalid planar image descriptor"));
  if (!config.aggregate_budget)
    config.aggregate_budget =
        std::make_shared<PlanarPageBudget>(config.maximum_backed_bytes);
  auto out = std::make_shared<Impl>();
  out->descriptor = std::move(descriptor);
  out->facets = std::move(facets);
  out->resources = selected.take_value();
  out->config = std::move(config);
  out->height = out->descriptor.shape[out->config.height_axis];
  out->width = out->descriptor.shape[out->config.width_axis];
  out->channels = out->config.channel_axis
                      ? out->descriptor.shape[*out->config.channel_axis]
                      : 1;
  try {
    out->scalar_width = Value::element_size(out->descriptor.element_type);
  } catch (const std::invalid_argument&) {
    return Result<PlanarImage>(invalid("invalid planar image dtype"));
  }
  out->page = host_page_size();
  if (!out->page || out->page > SIZE_MAX)
    return Result<PlanarImage>(exhausted("unsupported host page size"));
  std::uint64_t row_bytes = 0, span = 0, plane = 0, count = 0;
  if (!multiply(out->width, out->scalar_width, &row_bytes) ||
      !multiply(out->height, out->width, &count) ||
      !multiply(count, out->channels, &out->sample_count))
    return Result<PlanarImage>(exhausted("image size overflow"));
  if (out->config.order == ImagePlaneOrder::Continuous) {
    out->row_pitch =
        out->config.row_pitch_bytes ? out->config.row_pitch_bytes : row_bytes;
    if (out->row_pitch < row_bytes || out->row_pitch % out->scalar_width != 0 ||
        !multiply(out->height, out->row_pitch, &span) ||
        !align_up(span, out->page, &plane))
      return Result<PlanarImage>(exhausted("continuous plane overflow"));
  } else {
    if (out->config.row_pitch_bytes)
      return Result<PlanarImage>(invalid("tiled row pitch is fixed"));
    std::uint64_t tile_row = 0, full_span = 0, edge_span = 0;
    if (!multiply(out->config.tile_width, out->scalar_width, &tile_row) ||
        !multiply(out->config.tile_height, tile_row, &full_span) ||
        !align_up(full_span, out->page, &out->full_step))
      return Result<PlanarImage>(exhausted("tile size overflow"));
    const auto rem = out->height % out->config.tile_height;
    if (rem && (!multiply(rem, tile_row, &edge_span) ||
                !align_up(edge_span, out->page, &out->edge_step)))
      return Result<PlanarImage>(exhausted("edge tile overflow"));
    out->columns = out->width / out->config.tile_width +
                   (out->width % out->config.tile_width != 0);
    const auto full_rows = out->height / out->config.tile_height;
    std::uint64_t row_steps = 0;
    if (!multiply(full_rows, out->full_step, &row_steps) ||
        !add(row_steps, out->edge_step, &row_steps) ||
        !multiply(out->columns, row_steps, &plane))
      return Result<PlanarImage>(exhausted("tiled plane overflow"));
    out->row_pitch = tile_row;
  }
  out->plane_step = plane;
  if (!multiply(plane, out->channels, &span) ||
      !align_up(span, reservation_granularity(), &out->virtual_bytes) ||
      out->virtual_bytes > SIZE_MAX || out->virtual_bytes > INT64_MAX ||
      out->virtual_bytes > out->config.maximum_virtual_bytes)
    return Result<PlanarImage>(exhausted("image reservation overflow"));
  std::uint64_t initial_metadata = 1024;
  for (const auto& group : out->config.groups)
    initial_metadata += 256 + group.role.capacity();
  for (const auto& facet : out->facets)
    initial_metadata += 256 + facet.key.capacity() + facet.payload.capacity();
  auto metadata = out->config.aggregate_budget->charge(initial_metadata);
  if (!metadata.ok())
    return Result<PlanarImage>(metadata.status());
  out->metadata_lease = metadata.take_value();
  out->metadata_charge = initial_metadata;
  out->base_metadata = initial_metadata;
  out->base = static_cast<std::uint8_t*>(
      reserve_address(static_cast<std::size_t>(out->virtual_bytes)));
  if (!out->base)
    return Result<PlanarImage>(exhausted("image virtual reservation failed"));
  return Result<PlanarImage>(PlanarImage(std::move(out)));
}

Result<PlanarImage> PlanarImage::import_value(
    const Value& value, PlanarImageConfig config,
    const CancellationToken& cancellation) {
  if (!value.valid() || value.region().empty() ||
      value.region().dimensions().size() != value.descriptor().shape.size())
    return Result<PlanarImage>(invalid("invalid image import Value"));
  for (std::size_t axis = 0; axis < value.region().rank(); ++axis)
    if (value.region().dimensions()[axis].offset != 0 ||
        value.region().dimensions()[axis].extent !=
            value.descriptor().shape[axis])
      return Result<PlanarImage>(invalid("image import requires whole Value"));
  auto result = create(value.descriptor(), std::move(config), value.facets(),
                       value.resources());
  if (!result.ok())
    return result;
  auto image = result.take_value();
  auto writer = image.begin_write(value.region(), cancellation);
  if (!writer.ok())
    return Result<PlanarImage>(writer.status());
  auto window = writer.take_value();
  std::vector<std::uint64_t> at(value.descriptor().shape.size(), 0);
  for (std::uint64_t i = 0; i < image.impl_->sample_count; ++i) {
    if ((i & 4095U) == 0 && cancellation.cancelled())
      return Result<PlanarImage>(
          Status::failure(ErrorCode::Cancelled, "image import cancelled"));
    const auto offset = value.byte_address(at);
    if (!offset.ok())
      return Result<PlanarImage>(offset.status());
    const auto target = image.impl_->offset(at[image.impl_->config.height_axis],
                                            at[image.impl_->config.width_axis],
                                            image.impl_->channel_of(at));
    std::memcpy(image.impl_->base + target,
                value.bytes().data() + offset.value(),
                image.impl_->scalar_width);
    for (std::size_t axis = at.size(); axis-- > 0;) {
      if (++at[axis] < value.descriptor().shape[axis])
        break;
      at[axis] = 0;
    }
  }
  auto status = window.commit(cancellation);
  return status.ok() ? Result<PlanarImage>(std::move(image))
                     : Result<PlanarImage>(status);
}

const ValueDescriptor& PlanarImage::descriptor() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  return impl_->descriptor;
}
const std::vector<ValueFacet>& PlanarImage::facets() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  return impl_->facets;
}
const ResourceBindings& PlanarImage::resources() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  return impl_->resources;
}
const PlanarImageConfig& PlanarImage::config() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  return impl_->config;
}
std::uint64_t PlanarImage::page_size() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  return impl_->page;
}
std::uint64_t PlanarImage::reserved_bytes() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  return impl_->virtual_bytes;
}
std::uint64_t PlanarImage::backed_bytes() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  std::shared_lock<std::shared_timed_mutex> lock(impl_->mutex);
  return impl_->pages.size() * impl_->page;
}
std::uint64_t PlanarImage::valid_samples() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  std::shared_lock<std::shared_timed_mutex> lock(impl_->mutex);
  std::uint64_t count = 0;
  for (const auto& row : impl_->coverage)
    for (const auto& interval : row.second)
      count += interval.second - interval.first;
  return count;
}
std::uint64_t PlanarImage::metadata_bytes() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  std::shared_lock<std::shared_timed_mutex> lock(impl_->mutex);
  return impl_->metadata_charge;
}
std::uint64_t PlanarImage::resident_bytes() const {
  if (!impl_)
    throw std::logic_error("invalid planar image");
  std::shared_lock<std::shared_timed_mutex> lock(impl_->mutex);
  return impl_->pages.size() * impl_->page + impl_->metadata_charge;
}

Result<std::shared_ptr<void>> PlanarImage::pin_for_execution(
    const CancellationToken& cancellation) const {
  if (!impl_)
    return Result<std::shared_ptr<void>>(invalid("invalid planar image"));
  std::unique_lock<std::shared_timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return Result<std::shared_ptr<void>>(
          Status::failure(ErrorCode::Cancelled, "image source pin cancelled"));
  if (cancellation.cancelled())
    return Result<std::shared_ptr<void>>(
        Status::failure(ErrorCode::Cancelled, "image source pin cancelled"));
  ++impl_->execution_pins;
  auto owner = impl_;
  lock.unlock();
  return Result<std::shared_ptr<void>>(
      std::shared_ptr<void>(owner.get(), [owner](void*) {
        std::unique_lock<std::shared_timed_mutex> held(owner->mutex);
        --owner->execution_pins;
      }));
}

Result<std::uint64_t> PlanarImage::byte_offset(
    const std::vector<std::uint64_t>& coordinate) const {
  if (!impl_ || coordinate.size() != impl_->descriptor.shape.size())
    return Result<std::uint64_t>(invalid("invalid image coordinate"));
  for (std::size_t axis = 0; axis < coordinate.size(); ++axis)
    if (coordinate[axis] >= impl_->descriptor.shape[axis])
      return Result<std::uint64_t>(invalid("image coordinate out of bounds"));
  return Result<std::uint64_t>(impl_->offset(
      coordinate[impl_->config.height_axis],
      coordinate[impl_->config.width_axis], impl_->channel_of(coordinate)));
}

PlanarImageReadWindow::PlanarImageReadWindow(std::shared_ptr<PlanarImage> image,
                                             Region region)
    : image_(std::move(image)), region_(std::move(region)) {}
const ValueDescriptor& PlanarImageReadWindow::descriptor() const {
  if (!image_)
    throw std::logic_error("invalid image read window");
  return image_->descriptor();
}
const PlanarImageConfig& PlanarImageReadWindow::config() const {
  if (!image_)
    throw std::logic_error("invalid image read window");
  return image_->config();
}
const std::vector<ValueFacet>& PlanarImageReadWindow::facets() const {
  if (!image_)
    throw std::logic_error("invalid image read window");
  return image_->facets();
}

Result<PlanarRowRun> PlanarImageReadWindow::row_run(
    const std::vector<std::uint64_t>& coordinate) const {
  if (!image_ || coordinate.size() != region_.rank())
    return Result<PlanarRowRun>(invalid("invalid image window coordinate"));
  for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
    const auto dim = region_.dimensions()[axis];
    if (coordinate[axis] < dim.offset ||
        coordinate[axis] - dim.offset >= dim.extent)
      return Result<PlanarRowRun>(invalid("coordinate outside image window"));
  }
  const auto& owner = *image_->impl_;
  const auto x = coordinate[owner.config.width_axis];
  const auto x_end = region_.dimensions()[owner.config.width_axis].offset +
                     region_.dimensions()[owner.config.width_axis].extent;
  auto end = x_end;
  if (owner.config.order == ImagePlaneOrder::Tiled) {
    const auto remaining =
        owner.config.tile_width - x % owner.config.tile_width;
    end = std::min(end, x + remaining);
  }
  const auto offset = owner.offset(coordinate[owner.config.height_axis], x,
                                   owner.channel_of(coordinate));
  return Result<PlanarRowRun>(PlanarRowRun{owner.base + offset, end - x,
                                           (end - x) * owner.scalar_width});
}

Result<PlanarImageReadWindow> PlanarImage::acquire(
    const Region& region, const CancellationToken& cancellation) const {
  if (!impl_ || region.empty() ||
      !region.validate(impl_->descriptor.shape).ok())
    return Result<PlanarImageReadWindow>(
        invalid("invalid image window region"));
  auto count = region.element_count();
  if (!count.ok() || count.value() > impl_->config.maximum_access_samples)
    return Result<PlanarImageReadWindow>(
        exhausted("image read sample work limit"));
  if (cancellation.cancelled())
    return Result<PlanarImageReadWindow>(
        Status::failure(ErrorCode::Cancelled, "image read cancelled"));
  std::shared_lock<std::shared_timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return Result<PlanarImageReadWindow>(
          Status::failure(ErrorCode::Cancelled, "image read lock cancelled"));
  const auto y = region.dimensions()[impl_->config.height_axis];
  const auto x = region.dimensions()[impl_->config.width_axis];
  const auto c = impl_->config.channel_axis
                     ? region.dimensions()[*impl_->config.channel_axis]
                     : RegionDimension{0, 1};
  for (std::uint64_t channel = c.offset; channel < c.offset + c.extent;
       ++channel)
    for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row) {
      if (cancellation.cancelled())
        return Result<PlanarImageReadWindow>(
            Status::failure(ErrorCode::Cancelled, "image read cancelled"));
      const auto found = impl_->coverage.find(impl_->row_key(channel, row));
      if (found == impl_->coverage.end())
        return Result<PlanarImageReadWindow>(Status::failure(
            ErrorCode::NotFound, "image sample coverage is missing"));
      bool covered = false;
      for (const auto& interval : found->second)
        covered = covered || (interval.first <= x.offset &&
                              interval.second >= x.offset + x.extent);
      if (!covered)
        return Result<PlanarImageReadWindow>(Status::failure(
            ErrorCode::NotFound, "image sample coverage is missing"));
    }
  if (cancellation.cancelled())
    return Result<PlanarImageReadWindow>(
        Status::failure(ErrorCode::Cancelled, "image read cancelled"));
  lock.unlock();
  return Result<PlanarImageReadWindow>(
      PlanarImageReadWindow(std::make_shared<PlanarImage>(*this), region));
}

namespace {
Status check_region(const PlanarImage::Impl& image, const Region& region,
                    const void* packed, std::uint64_t byte_size) {
  if (!packed || region.empty() ||
      !region.validate(image.descriptor.shape).ok())
    return invalid("invalid planar image region");
  auto count = region.element_count();
  std::uint64_t expected = 0;
  if (!count.ok() || !multiply(count.value(), image.scalar_width, &expected))
    return exhausted("image region size overflow");
  if (expected != byte_size || expected > SIZE_MAX)
    return Status::failure(ErrorCode::TypeMismatch,
                           "packed image region byte size mismatch");
  return Status::success();
}

template <class Function>
Status each_sample(const Region& region, const CancellationToken& cancellation,
                   Function function) {
  std::vector<std::uint64_t> coordinate;
  for (const auto dim : region.dimensions())
    coordinate.push_back(dim.offset);
  std::uint64_t index = 0;
  for (;;) {
    if ((index & 4095U) == 0 && cancellation.cancelled())
      return Status::failure(ErrorCode::Cancelled,
                             "image region access cancelled");
    auto status = function(coordinate, index);
    if (!status.ok())
      return status;
    ++index;
    for (std::size_t axis = coordinate.size(); axis-- > 0;) {
      const auto dim = region.dimensions()[axis];
      if (++coordinate[axis] < dim.offset + dim.extent)
        break;
      coordinate[axis] = dim.offset;
      if (axis == 0)
        return Status::success();
    }
  }
}
}  // namespace

struct PlanarImageWriteWindow::Impl final {
  std::shared_ptr<PlanarImage::Impl> image;
  std::unique_lock<std::shared_timed_mutex> lock;
  Region region;
  std::map<std::uint64_t, std::vector<Interval>> next_coverage;
  std::set<std::uint64_t> next_pages;
  std::vector<std::uint64_t> fresh;
  std::uint64_t charge = 0;
  std::shared_ptr<void> external_lease;
  std::uint64_t metadata_charge = 0;
  std::shared_ptr<void> metadata_lease;
  std::size_t provided = 0;
  bool committed = false;

  ~Impl() noexcept {
    if (committed || !image)
      return;
    for (std::size_t i = 0; i < provided; ++i)
      withdraw_page(image->base + fresh[i] * image->page,
                    static_cast<std::size_t>(image->page));
    image->config.aggregate_budget->release(charge + metadata_charge);
  }
};

PlanarImageWriteWindow::PlanarImageWriteWindow(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
PlanarImageWriteWindow::PlanarImageWriteWindow(
    PlanarImageWriteWindow&&) noexcept =
    default;  // NOLINT(whitespace/indent_namespace)
PlanarImageWriteWindow& PlanarImageWriteWindow::operator=(
    PlanarImageWriteWindow&&) noexcept = default;
PlanarImageWriteWindow::~PlanarImageWriteWindow() noexcept = default;
const Region& PlanarImageWriteWindow::region() const {
  if (!impl_)
    throw std::logic_error("invalid image write window");
  return impl_->region;
}
Result<PlanarMutableRowRun> PlanarImageWriteWindow::row_run(
    const std::vector<std::uint64_t>& coordinate) const {
  if (!impl_ || coordinate.size() != impl_->region.rank())
    return Result<PlanarMutableRowRun>(
        invalid("invalid image write coordinate"));
  for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
    const auto dim = impl_->region.dimensions()[axis];
    if (coordinate[axis] < dim.offset ||
        coordinate[axis] - dim.offset >= dim.extent)
      return Result<PlanarMutableRowRun>(
          invalid("coordinate outside image write window"));
  }
  const auto& image = *impl_->image;
  const auto x = coordinate[image.config.width_axis];
  const auto x_end =
      impl_->region.dimensions()[image.config.width_axis].offset +
      impl_->region.dimensions()[image.config.width_axis].extent;
  auto end = x_end;
  if (image.config.order == ImagePlaneOrder::Tiled)
    end = std::min(end,
                   x + image.config.tile_width - x % image.config.tile_width);
  const auto offset = image.offset(coordinate[image.config.height_axis], x,
                                   image.channel_of(coordinate));
  return Result<PlanarMutableRowRun>(PlanarMutableRowRun{
      image.base + offset, end - x, (end - x) * image.scalar_width});
}

Status PlanarImageWriteWindow::commit(const CancellationToken& cancellation) {
  if (!impl_ || impl_->committed)
    return invalid("invalid image write publication");
  if (cancellation.cancelled())
    return Status::failure(ErrorCode::Cancelled,
                           "image write publication cancelled");
  impl_->image->coverage.swap(impl_->next_coverage);
  impl_->image->pages.swap(impl_->next_pages);
  const auto old_metadata = impl_->image->metadata_charge;
  impl_->image->metadata_charge = impl_->metadata_charge;
  impl_->image->metadata_lease.swap(impl_->metadata_lease);
  impl_->image->config.aggregate_budget->release(old_metadata);
  impl_->metadata_lease.reset();
  if (impl_->external_lease)
    impl_->image->page_leases.push_back(std::move(impl_->external_lease));
  impl_->committed = true;
  impl_->lock.unlock();
  return Status::success();
}

Result<PlanarImageWriteWindow> PlanarImage::begin_write(
    const Region& region, const CancellationToken& cancellation) {
  if (!impl_ || region.empty() ||
      !region.validate(impl_->descriptor.shape).ok())
    return Result<PlanarImageWriteWindow>(
        invalid("invalid planar image write region"));
  if (cancellation.cancelled())
    return Result<PlanarImageWriteWindow>(Status::failure(
        ErrorCode::Cancelled, "image write preparation cancelled"));
  auto count = region.element_count();
  if (!count.ok() || count.value() > impl_->config.maximum_access_samples)
    return Result<PlanarImageWriteWindow>(
        exhausted("image write sample work limit"));
  const auto rows = region.dimensions()[impl_->config.height_axis].extent;
  const auto channels =
      impl_->config.channel_axis
          ? region.dimensions()[*impl_->config.channel_axis].extent
          : 1;
  std::uint64_t row_records = 0;
  if (!multiply(rows, channels, &row_records) ||
      row_records > impl_->config.maximum_metadata_rows)
    return Result<PlanarImageWriteWindow>(
        exhausted("image coverage metadata limit"));
  auto prepared = std::make_unique<PlanarImageWriteWindow::Impl>();
  prepared->image = impl_;
  prepared->lock =
      std::unique_lock<std::shared_timed_mutex>(impl_->mutex, std::defer_lock);
  while (!prepared->lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return Result<PlanarImageWriteWindow>(
          Status::failure(ErrorCode::Cancelled, "image write lock cancelled"));
  if (impl_->execution_pins)
    return Result<PlanarImageWriteWindow>(Status::failure(
        ErrorCode::Stale, "image source is pinned for execution"));
  prepared->region = region;
  const auto y = region.dimensions()[impl_->config.height_axis];
  const auto x = region.dimensions()[impl_->config.width_axis];
  const auto c = impl_->config.channel_axis
                     ? region.dimensions()[*impl_->config.channel_axis]
                     : RegionDimension{0, 1};
  std::uint64_t current_intervals = 0;
  for (const auto& row : impl_->coverage)
    current_intervals += row.second.size();
  std::uint64_t upper_rows = 0, upper_intervals = 0, upper_pages = 0;
  std::uint64_t candidate_metadata = 0;
  const auto possible_pages = std::min<std::uint64_t>(
      count.value(), impl_->virtual_bytes / impl_->page);
  std::uint64_t new_rows = 0;
  for (std::uint64_t channel = c.offset; channel < c.offset + c.extent;
       ++channel)
    for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row)
      if (!impl_->coverage.count(impl_->row_key(channel, row)))
        ++new_rows;
  if (!add(impl_->coverage.size(), new_rows, &upper_rows) ||
      !add(current_intervals, row_records, &upper_intervals) ||
      !add(impl_->pages.size(), possible_pages, &upper_pages) ||
      !metadata_capacity(impl_->base_metadata, upper_rows, upper_intervals,
                         upper_pages, &candidate_metadata))
    return Result<PlanarImageWriteWindow>(
        exhausted("image metadata capacity overflow"));
  auto metadata = impl_->config.aggregate_budget->charge(candidate_metadata);
  if (!metadata.ok())
    return Result<PlanarImageWriteWindow>(metadata.status());
  prepared->metadata_charge = candidate_metadata;
  prepared->metadata_lease = metadata.take_value();
  prepared->next_coverage = impl_->coverage;
  for (std::uint64_t channel = c.offset; channel < c.offset + c.extent;
       ++channel)
    for (std::uint64_t row = y.offset; row < y.offset + y.extent; ++row) {
      if (cancellation.cancelled())
        return Result<PlanarImageWriteWindow>(Status::failure(
            ErrorCode::Cancelled, "image write preparation cancelled"));
      auto& intervals = prepared->next_coverage[impl_->row_key(channel, row)];
      const Interval incoming{x.offset, x.offset + x.extent};
      for (const auto& existing : intervals)
        if (existing.first < incoming.second &&
            incoming.first < existing.second)
          return Result<PlanarImageWriteWindow>(
              invalid("image samples already published"));
      intervals.push_back(incoming);
      std::sort(intervals.begin(), intervals.end());
      std::vector<Interval> merged;
      for (const auto& interval : intervals) {
        if (!merged.empty() && merged.back().second == interval.first)
          merged.back().second = interval.second;
        else
          merged.push_back(interval);
      }
      intervals.swap(merged);
      if (prepared->next_coverage.size() > impl_->config.maximum_metadata_rows)
        return Result<PlanarImageWriteWindow>(
            exhausted("image coverage metadata limit"));
    }
  std::uint64_t intervals = 0;
  for (const auto& row : prepared->next_coverage) {
    if (row.second.size() >
        impl_->config.maximum_metadata_intervals - intervals)
      return Result<PlanarImageWriteWindow>(
          exhausted("image interval metadata limit"));
    intervals += row.second.size();
  }
  std::set<std::uint64_t> required;
  auto status =
      each_sample(region, cancellation, [&](const auto& at, std::uint64_t) {
        const auto offset =
            impl_->offset(at[impl_->config.height_axis],
                          at[impl_->config.width_axis], impl_->channel_of(at));
        required.insert(offset / impl_->page);
        return Status::success();
      });
  if (!status.ok())
    return Result<PlanarImageWriteWindow>(status);
  for (const auto page : required)
    if (!impl_->pages.count(page))
      prepared->fresh.push_back(page);
  if (impl_->pages.size() > impl_->config.maximum_backed_bytes / impl_->page ||
      prepared->fresh.size() >
          (impl_->config.maximum_backed_bytes / impl_->page -
           impl_->pages.size()))
    return Result<PlanarImageWriteWindow>(
        exhausted("image page budget exceeded"));
  prepared->next_pages = impl_->pages;
  prepared->next_pages.insert(prepared->fresh.begin(), prepared->fresh.end());
  impl_->page_leases.reserve(impl_->page_leases.size() + 1);
  const auto candidate_charge = prepared->fresh.size() * impl_->page;
  auto charged = impl_->config.aggregate_budget->charge(candidate_charge);
  if (!charged.ok())
    return Result<PlanarImageWriteWindow>(charged.status());
  prepared->charge = candidate_charge;
  prepared->external_lease = charged.take_value();
  for (const auto page : prepared->fresh) {
    if (cancellation.cancelled())
      return Result<PlanarImageWriteWindow>(Status::failure(
          ErrorCode::Cancelled, "image page preparation cancelled"));
    if (!provide_page(impl_->base + page * impl_->page,
                      static_cast<std::size_t>(impl_->page))) {
      return Result<PlanarImageWriteWindow>(
          exhausted("image page provision failed"));
    }
    ++prepared->provided;
  }
  return Result<PlanarImageWriteWindow>(
      PlanarImageWriteWindow(std::move(prepared)));
}

Status PlanarImage::publish(const Region& region, const std::uint8_t* packed,
                            std::uint64_t byte_size,
                            const CancellationToken& cancellation) {
  if (!impl_)
    return invalid("invalid planar image");
  auto status = check_region(*impl_, region, packed, byte_size);
  if (!status.ok())
    return status;
  auto writer = begin_write(region, cancellation);
  if (!writer.ok())
    return writer.status();
  auto window = writer.take_value();
  status = each_sample(
      region, cancellation, [&](const auto& at, std::uint64_t index) {
        const auto offset =
            impl_->offset(at[impl_->config.height_axis],
                          at[impl_->config.width_axis], impl_->channel_of(at));
        std::memcpy(impl_->base + offset, packed + index * impl_->scalar_width,
                    impl_->scalar_width);
        return Status::success();
      });
  if (!status.ok())
    return status;
  return window.commit(cancellation);
}

Status PlanarImage::read(const Region& region, std::uint8_t* packed,
                         std::uint64_t byte_size,
                         const CancellationToken& cancellation) const {
  if (!impl_)
    return invalid("invalid planar image");
  auto status = check_region(*impl_, region, packed, byte_size);
  if (!status.ok())
    return status;
  auto window = acquire(region, cancellation);
  if (!window.ok())
    return window.status();
  return each_sample(
      region, cancellation, [&](const auto& at, std::uint64_t index) {
        const auto offset =
            impl_->offset(at[impl_->config.height_axis],
                          at[impl_->config.width_axis], impl_->channel_of(at));
        std::memcpy(packed + index * impl_->scalar_width, impl_->base + offset,
                    impl_->scalar_width);
        return Status::success();
      });
}
}  // namespace ps
