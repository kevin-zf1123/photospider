#include "graph/graph_cache_service.hpp"

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
#include <atomic>
#endif
#include <chrono>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include "graph/graph_traversal_service.hpp"
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
#include "graph/graph_cache_service_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

namespace {

using DiskCacheLoadStatus = GraphModel::DiskCacheLoadStatus;

/**
 * @brief Holds one disk-cache read attempt plus the loaded output, if any.
 *
 * The public diagnostic record is stored separately from the NodeOutput so
 * GraphModel can retain lightweight inspectable state without owning decoded
 * image payloads for failed or historical attempts.
 */
struct DiskCacheReadAttempt {
  GraphModel::DiskCacheLoadResult result;
  NodeOutput output;
};

/**
 * @brief Converts the legacy cache precision label to codec precision.
 * @param precision Cache configuration string used by existing compute paths.
 * @return UInt16 for the exact `int16` label; UInt8 for every other label.
 * @throws Nothing.
 * @note The fallback preserves the existing default-to-int8 cache behavior.
 */
ImageArtifactPrecision artifact_precision(
    const std::string& precision) noexcept {
  return precision == "int16" ? ImageArtifactPrecision::UInt16
                              : ImageArtifactPrecision::UInt8;
}

/**
 * @brief Returns a pointer to a node's formal HP cache when it exists.
 *
 * @param node Node whose reusable HP output should be inspected.
 * @return Pointer to cached HP output, or nullptr when no HP cache exists.
 * @throws Nothing.
 * @note RT state is intentionally ignored because disk cache authority is
 * limited to formal HP output.
 */
const NodeOutput* hp_cache_ptr(const Node& node) {
  if (node.cached_output_high_precision) {
    return &*node.cached_output_high_precision;
  }
  return nullptr;
}

/**
 * @brief Tests whether a node has formal HP cache state.
 *
 * @param node Node whose memory cache fields should be inspected.
 * @return true when the HP cache field is populated.
 * @throws Nothing.
 * @note RT proxy state is owned outside GraphModel and is not cleared by this
 * node-local helper.
 */
bool has_memory_cache(const Node& node) {
  return node.cached_output_high_precision.has_value();
}

/**
 * @brief Clears formal HP cache state from a node.
 *
 * @param node Node whose memory cache fields should be reset.
 * @throws Destructors for cached payload members are expected not to throw.
 * @note Topology, cache entries, and version counters are left unchanged. RT
 * proxy state is not stored on Node.
 */
void reset_memory_cache(Node& node) {
  node.cached_output_high_precision.reset();
}

/**
 * @brief Adds elapsed disk IO duration to the graph's aggregate IO counter.
 *
 * @param graph Graph whose atomic timing counter should be incremented.
 * @param start_io Start timestamp captured immediately before IO work.
 * @throws Nothing.
 * @note Uses compare-exchange because std::atomic<double> has no fetch_add in
 * C++17.
 */
void add_io_duration(GraphModel& graph,
                     std::chrono::high_resolution_clock::time_point start_io) {
  auto end_io = std::chrono::high_resolution_clock::now();
  double duration_ms =
      std::chrono::duration<double, std::milli>(end_io - start_io).count();

  double expected = graph.total_io_time_ms.load();
  while (!graph.total_io_time_ms.compare_exchange_weak(
      expected, expected + duration_ms)) {
  }
}

/**
 * @brief Builds a lightweight diagnostic record for a disk-cache attempt.
 *
 * @param node_id Node id whose cache entry is being inspected.
 * @param cache_entry Optional cache entry that supplied type and location.
 * @param cache_file Resolved image path, when available.
 * @param metadata_file Resolved metadata path, when available.
 * @param status Outcome status for the attempt.
 * @param code Error category when the attempt failed.
 * @param message Human-readable diagnostic text.
 * @return Populated diagnostic result.
 * @throws std::bad_alloc from string/path copies.
 * @note `code` is meaningful only for Error status; callers pass Unknown for
 * hits, misses, and skipped attempts.
 */
GraphModel::DiskCacheLoadResult make_load_result(
    int node_id, const CacheEntry* cache_entry, fs::path cache_file,
    fs::path metadata_file, DiskCacheLoadStatus status, GraphErrc code,
    std::string message) {
  GraphModel::DiskCacheLoadResult result;
  result.node_id = node_id;
  if (cache_entry) {
    result.cache_type = cache_entry->cache_type;
    result.location = cache_entry->location;
  }
  result.cache_file = std::move(cache_file);
  result.metadata_file = std::move(metadata_file);
  result.status = status;
  result.code = code;
  result.message = std::move(message);
  return result;
}

/**
 * @brief Creates a skipped-attempt result without a concrete cache entry.
 *
 * @param node_id Node id associated with the skipped attempt.
 * @param message Reason the service did not inspect disk files.
 * @return Diagnostic result with Skipped status.
 * @throws std::bad_alloc from message allocation.
 * @note This is used for disabled cache roots, empty cache lists, unsupported
 * entries, and nodes that already have HP memory cache.
 */
DiskCacheReadAttempt make_skipped_attempt(int node_id, std::string message) {
  DiskCacheReadAttempt attempt;
  attempt.result =
      make_load_result(node_id, nullptr, {}, {}, DiskCacheLoadStatus::Skipped,
                       GraphErrc::Unknown, std::move(message));
  return attempt;
}

/**
 * @brief Creates an error result for a concrete cache entry.
 *
 * @param node_id Node id whose cache file failed to load.
 * @param cache_entry Cache entry that supplied the failed paths.
 * @param cache_file Resolved image cache path.
 * @param metadata_file Resolved metadata path.
 * @param code Error category for the failure.
 * @param message Human-readable failure reason.
 * @return Diagnostic result with Error status.
 * @throws std::bad_alloc from string/path copies.
 * @note The loaded output remains empty and must not be consumed by callers.
 */
DiskCacheReadAttempt make_error_attempt(int node_id,
                                        const CacheEntry& cache_entry,
                                        const fs::path& cache_file,
                                        const fs::path& metadata_file,
                                        GraphErrc code, std::string message) {
  DiskCacheReadAttempt attempt;
  attempt.result =
      make_load_result(node_id, &cache_entry, cache_file, metadata_file,
                       DiskCacheLoadStatus::Error, code, std::move(message));
  return attempt;
}

/**
 * @brief Reads one concrete disk-cache entry and converts failures to results.
 *
 * @param graph Graph whose cache root anchors the cache entry.
 * @param node Node that owns the cache entry.
 * @param cache_entry Image cache entry to inspect.
 * @param image_codec Injected codec used to decode image bytes.
 * @param metadata_codec Injected codec used to decode named-value metadata.
 * @return Hit, Miss, or Error attempt with diagnostic details.
 * @throws std::bad_alloc from result/message allocation.
 * @note Exceptions from filesystem and injected codecs are converted into
 * Error results instead of being silently collapsed into miss.
 */
DiskCacheReadAttempt read_cache_entry(
    const GraphModel& graph, const Node& node, const CacheEntry& cache_entry,
    const ImageArtifactCodec& image_codec,
    const CacheMetadataCodec& metadata_codec) {
  auto cache_file =
      graph.cache_root / std::to_string(node.id) / cache_entry.location;
  auto metadata_file = cache_file;
  metadata_file.replace_extension(".yml");

  try {
    const bool has_cache_file = fs::exists(cache_file);
    const bool has_metadata_file = fs::exists(metadata_file);
    if (!has_cache_file && !has_metadata_file) {
      DiskCacheReadAttempt attempt;
      attempt.result = make_load_result(
          node.id, &cache_entry, cache_file, metadata_file,
          DiskCacheLoadStatus::Miss, GraphErrc::Unknown,
          "No disk cache image or metadata file exists for configured entry.");
      return attempt;
    }

    DiskCacheReadAttempt attempt;
    if (has_cache_file) {
      attempt.output.image_buffer = image_codec.decode(cache_file);
    }
    if (has_metadata_file) {
      attempt.output.data = metadata_codec.read(metadata_file);
    }
    attempt.result =
        make_load_result(node.id, &cache_entry, cache_file, metadata_file,
                         DiskCacheLoadStatus::Hit, GraphErrc::Unknown,
                         "Loaded disk cache entry.");
    return attempt;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const fs::filesystem_error& e) {
    return make_error_attempt(
        node.id, cache_entry, cache_file, metadata_file, GraphErrc::Io,
        std::string("Filesystem failed while reading disk cache: ") + e.what());
  } catch (const GraphError& e) {
    return make_error_attempt(node.id, cache_entry, cache_file, metadata_file,
                              e.code(), e.what());
  } catch (const std::exception& e) {
    return make_error_attempt(
        node.id, cache_entry, cache_file, metadata_file, GraphErrc::Unknown,
        std::string("Unexpected exception while reading disk cache: ") +
            e.what());
  } catch (...) {
    return make_error_attempt(
        node.id, cache_entry, cache_file, metadata_file, GraphErrc::Unknown,
        "Unknown non-standard exception while reading disk cache.");
  }
}

/**
 * @brief Scans a node's cache entries and returns the first terminal result.
 *
 * @param graph Graph whose cache root anchors the entries.
 * @param node Node whose cache entries should be inspected.
 * @param image_codec Injected codec used for every supported image entry.
 * @param metadata_codec Injected codec used for every existing metadata file.
 * @return Hit/Error for the first existing or failing entry, Miss when all
 * supported entries are absent, or Skipped when no load should be attempted.
 * @throws std::bad_alloc from diagnostic construction.
 * @note Missing files remain true cache misses and do not stop scanning later
 * entries; read/parse errors stop immediately to preserve their diagnostics.
 */
DiskCacheReadAttempt read_first_disk_cache_entry(
    const GraphModel& graph, const Node& node,
    const ImageArtifactCodec& image_codec,
    const CacheMetadataCodec& metadata_codec) {
  if (graph.cache_root.empty()) {
    return make_skipped_attempt(node.id, "Graph has no disk cache root.");
  }
  if (node.caches.empty()) {
    return make_skipped_attempt(node.id, "Node has no configured cache entry.");
  }

  bool saw_supported_entry = false;
  DiskCacheReadAttempt last_miss =
      make_skipped_attempt(node.id, "No supported image cache entry found.");
  for (const auto& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
      continue;
    }

    saw_supported_entry = true;
    DiskCacheReadAttempt attempt =
        read_cache_entry(graph, node, cache_entry, image_codec, metadata_codec);
    if (attempt.result.status != DiskCacheLoadStatus::Miss) {
      return attempt;
    }
    last_miss = std::move(attempt);
  }

  if (saw_supported_entry) {
    last_miss.result.message =
        "No disk cache files exist for configured image cache entries.";
    return last_miss;
  }
  return last_miss;
}

/**
 * @brief Stores the diagnostic result from one disk-cache load attempt.
 *
 * @param graph Graph receiving the latest diagnostic record.
 * @param result Diagnostic result to move into GraphModel.
 * @throws std::bad_alloc if optional storage needs allocation.
 * @note The graph stores only the most recent attempt by design; detailed
 * histories can be added later through event services if needed.
 */
void record_disk_cache_load_result(GraphModel& graph,
                                   GraphModel::DiskCacheLoadResult result) {
  graph.record_disk_cache_load_result(std::move(result));
}

/**
 * @brief Commits a successful read attempt through a caller-supplied consumer.
 *
 * @param graph Graph whose IO timing and diagnostics should be updated.
 * @param attempt Read attempt returned by read_first_disk_cache_entry.
 * @param start_io Start timestamp captured before scanning disk cache entries.
 * @param consume_output Callable that accepts a NodeOutput rvalue on hit.
 * @return true when the attempt was a disk-cache hit; false otherwise.
 * @throws Exceptions from `consume_output` or diagnostic storage.
 * @note The template keeps assignment into node HP cache and execution temp
 * output slots unified without exposing output ownership in GraphModel.
 */
template <typename OutputConsumer>
bool finalize_disk_cache_load(
    GraphModel& graph, DiskCacheReadAttempt attempt,
    std::chrono::high_resolution_clock::time_point start_io,
    OutputConsumer&& consume_output) {
  const bool loaded = attempt.result.status == DiskCacheLoadStatus::Hit;
  if (loaded) {
    consume_output(std::move(attempt.output));
    add_io_duration(graph, start_io);
  }
  record_disk_cache_load_result(graph, std::move(attempt.result));
  return loaded;
}

}  // namespace

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
namespace testing {
namespace {

/** @brief Borrowed cache hook pointer stored by the test-only seam. */
using GraphCacheServiceTestHookPtr = const GraphCacheServiceTestHook*;

/**
 * @brief Process-local observer for deterministic cache-clear tests.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize installation and clear the pointer before destroying
 * the borrowed hook or context.
 */
std::atomic<GraphCacheServiceTestHookPtr> g_graph_cache_service_test_hook{
    nullptr};  // NOLINT(whitespace/indent_namespace)

}  // namespace

/** @copydoc ps::testing::set_graph_cache_service_test_hook */
void set_graph_cache_service_test_hook(
    const GraphCacheServiceTestHook* hook) noexcept {
  g_graph_cache_service_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_graph_cache_service_test_hook */
void notify_graph_cache_service_test_hook(
    GraphCacheServiceTestEvent event, const std::filesystem::path& cache_root) {
  const GraphCacheServiceTestHook* hook =
      g_graph_cache_service_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->notify != nullptr) {
    hook->notify(hook->context, event, cache_root);
  }
}

}  // namespace testing
#endif

/** @copydoc GraphCacheService::GraphCacheService */
GraphCacheService::GraphCacheService(
    std::shared_ptr<const ImageArtifactCodec> image_codec,
    std::shared_ptr<const CacheMetadataCodec> metadata_codec)
    : image_codec_(std::move(image_codec)),
      metadata_codec_(std::move(metadata_codec)) {
  if (!image_codec_) {
    throw std::invalid_argument(
        "GraphCacheService requires an image artifact codec");
  }
  if (!metadata_codec_) {
    throw std::invalid_argument(
        "GraphCacheService requires a cache metadata codec");
  }
}

std::filesystem::path GraphCacheService::node_cache_dir(const GraphModel& graph,
                                                        int node_id) const {
  return graph.cache_root / std::to_string(node_id);
}

void GraphCacheService::save_cache_if_configured(
    GraphModel& graph, const Node& node,
    const std::string& cache_precision) const {
  if (graph.skip_save_cache_.load(std::memory_order_relaxed)) {
    return;
  }
  const NodeOutput* output = hp_cache_ptr(node);
  if (graph.cache_root.empty() || node.caches.empty() || !output) {
    return;
  }

  for (const auto& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
      continue;
    }

    auto dir = node_cache_dir(graph, node.id);
    fs::create_directories(dir);
    auto final_path = dir / cache_entry.location;

    auto start_io = std::chrono::high_resolution_clock::now();

    if (output->image_buffer.width > 0 && output->image_buffer.height > 0 &&
        output->image_buffer.device == Device::CPU &&
        output->image_buffer.data) {
      image_codec_->encode(final_path, output->image_buffer,
                           artifact_precision(cache_precision));
    }

    if (!output->data.empty()) {
      fs::path meta_path = final_path;
      meta_path.replace_extension(".yml");
      metadata_codec_->write(meta_path, output->data);
    }

    add_io_duration(graph, start_io);
  }
}

bool GraphCacheService::try_load_from_disk_cache(GraphModel& graph,
                                                 Node& node) const {
  if (node.cached_output_high_precision.has_value()) {
    record_disk_cache_load_result(
        graph, make_skipped_attempt(node.id,
                                    "Node already has formal HP memory cache.")
                   .result);
    return node.cached_output_high_precision.has_value();
  }

  auto start_io = std::chrono::high_resolution_clock::now();
  DiskCacheReadAttempt attempt =
      read_first_disk_cache_entry(graph, node, *image_codec_, *metadata_codec_);
  return finalize_disk_cache_load(
      graph, std::move(attempt), start_io, [&](NodeOutput output) {
        node.cached_output_high_precision = std::move(output);
      });
}

bool GraphCacheService::try_load_from_disk_cache_into(GraphModel& graph,
                                                      const Node& node,
                                                      NodeOutput& out) const {
  auto start_io = std::chrono::high_resolution_clock::now();
  DiskCacheReadAttempt attempt =
      read_first_disk_cache_entry(graph, node, *image_codec_, *metadata_codec_);
  return finalize_disk_cache_load(
      graph, std::move(attempt), start_io,
      [&](NodeOutput output) { out = std::move(output); });
}

/** @copydoc GraphCacheService::clear_drive_cache */
GraphModel::DriveClearResult GraphCacheService::clear_drive_cache(
    GraphModel& graph) const {
  GraphModel::DriveClearResult result;
  if (!graph.cache_root.empty() && fs::exists(graph.cache_root)) {
    result.removed_entries = fs::remove_all(graph.cache_root);
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING)
    testing::notify_graph_cache_service_test_hook(
        testing::GraphCacheServiceTestEvent::DriveCacheRootRemoved,
        graph.cache_root);
#endif
    fs::create_directories(graph.cache_root);
  }
  return result;
}

GraphModel::MemoryClearResult GraphCacheService::clear_memory_cache(
    GraphModel& graph) const {
  GraphModel::MemoryClearResult result;
  for (int node_id : graph.node_ids()) {
    Node& node = graph.mutable_node(node_id);
    if (has_memory_cache(node)) {
      reset_memory_cache(node);
      result.cleared_nodes++;
    }
  }
  return result;
}

void GraphCacheService::clear_cache(GraphModel& graph) const {
  (void)clear_drive_cache(graph);
  (void)clear_memory_cache(graph);
}

GraphModel::CacheSaveResult GraphCacheService::cache_all_nodes(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::CacheSaveResult result;
  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (hp_cache_ptr(node)) {
      save_cache_if_configured(graph, node, cache_precision);
      result.saved_nodes++;
    }
  }
  return result;
}

GraphModel::MemoryClearResult GraphCacheService::free_transient_memory(
    GraphModel& graph) const {
  GraphTraversalService traversal;
  auto ends = traversal.ending_nodes(graph);
  std::unordered_set<int> endset(ends.begin(), ends.end());

  GraphModel::MemoryClearResult result;
  for (int node_id : graph.node_ids()) {
    Node& node = graph.mutable_node(node_id);
    if (has_memory_cache(node) && !endset.count(node_id)) {
      reset_memory_cache(node);
      result.cleared_nodes++;
    }
  }
  return result;
}

GraphModel::DiskSyncResult GraphCacheService::synchronize_disk_cache(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::DiskSyncResult result;
  result.saved_nodes = cache_all_nodes(graph, cache_precision).saved_nodes;

  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (node.cached_output_high_precision.has_value() || node.caches.empty()) {
      continue;
    }

    auto dir_path = node_cache_dir(graph, node.id);
    if (!fs::exists(dir_path)) {
      continue;
    }

    for (const auto& cache_entry : node.caches) {
      if (cache_entry.location.empty()) {
        continue;
      }
      auto cache_file = dir_path / cache_entry.location;
      auto meta_file = cache_file;
      meta_file.replace_extension(".yml");

      if (fs::exists(cache_file)) {
        const bool removed_cache_file = fs::remove(cache_file);
        if (removed_cache_file) {
          result.removed_files++;
        }
      }
      if (fs::exists(meta_file)) {
        const bool removed_meta_file = fs::remove(meta_file);
        if (removed_meta_file) {
          result.removed_files++;
        }
      }
    }

    if (fs::is_empty(dir_path)) {
      const bool removed_dir = fs::remove(dir_path);
      if (removed_dir) {
        result.removed_dirs++;
      }
    }
  }

  return result;
}

}  // namespace ps
