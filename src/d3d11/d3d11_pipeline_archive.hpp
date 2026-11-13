#pragma once

#include "Metal.hpp"
#include "d3d11_input_layout.hpp"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct MTL_GRAPHICS_PIPELINE_DESC;

namespace mullion {

/*
 * SC6 fork: a persistent pipeline archive with warm-up (d3d11.pipelineArchive), ported from dxmt-SC6 onto Mullion's
 * pipeline cache.
 */

/**
The state of one pipeline that Mullion compiled, by value: shader hashes
instead of shader objects, and descriptions instead of state objects. A
later launch rebuilds MTL_GRAPHICS_PIPELINE_DESC from it once the game has
created the same shaders again.

Only pipelines without stream output are recorded: graphics pipelines,
geometry shader pipelines (emulated with Metal mesh shaders), and compute
pipelines. Tessellation pipelines are not recorded.
 */
struct PipelineRecord {
  enum Kind : uint8_t {
    Graphics = 1,
    Compute = 2,
    /* SC6 fork: a graphics pipeline with a geometry shader, built by GetGeometryPipeline (Metal mesh shaders). It
       has the same fields as Graphics. */
    Geometry = 3,
  };
  enum Slot : uint8_t {
    SlotVertex = 0,
    SlotPixel = 1,
    SlotHull = 2,
    SlotDomain = 3,
    SlotGeometry = 4,
    NumSlots = 5,
    SlotCompute = 0,
  };

  Kind kind = Graphics;
  /* bit N set: shaders[N] is used */
  uint8_t shader_mask = 0;
  Sha1Digest shaders[NumSlots] = {};

  /* graphics only */
  uint32_t num_color_attachments = 0;
  uint32_t color_formats[8] = {};
  uint32_t depth_stencil_format = 0;
  uint32_t topology_class = 0;
  uint32_t rasterization_enabled = 0;
  uint32_t sample_count = 0;
  uint32_t gs_strip_topology = 0;
  uint32_t index_buffer_format = 0;
  uint32_t sample_mask = 0;
  uint32_t gs_passthrough = 0;
  /* Mullion's pipeline key (MTL_GRAPHICS_PIPELINE_DESC) has these too, and a warm-up pipeline must match the
     game's own key, or it compiles a pipeline nobody uses */
  uint32_t centered_samples = 0;
  uint32_t single_raster_sample = 0;
  uint32_t paired_samples = 0;
  uint32_t clip_distances = 0;
  uint32_t input_primitive = 0;
  uint32_t pass_through = 0;
  uint32_t quad_lines = 0;
  uint32_t pipeline_statistics = 0;
  D3D11_BLEND_DESC1 blend = {};
  bool has_input_layout = false;
  MTL_INPUT_LAYOUT_DESC input_layout;

  /* false if the description cannot be stored (for example stream output) */
  static bool FromGraphicsDesc(const MTL_GRAPHICS_PIPELINE_DESC &desc, PipelineRecord &out);

  void Serialize(std::vector<uint8_t> &out) const;
  /* false on any malformed or out-of-range field */
  bool Deserialize(const uint8_t *data, size_t size);
};

/**
A persistent Metal pipeline archive (MTLBinaryArchive) plus a list of the
pipelines a game compiled, both stored next to Mullion's shader cache.

- Every pipeline compile looks up the archive first. On a hit, Metal loads
  the GPU binary instead of compiling it. On a miss, the pipeline is
  compiled normally, and then added to the archive off the critical path.
- Every newly compiled pipeline description is recorded, together with the
  DXBC bytecode of its shaders (pso_shaders_<version>.bin). At the next
  launch, the pipeline cache creates the stored shaders itself and queues
  every recorded pipeline for compilation (warm-up), so most pipelines are
  ready before the game's first draw with them. A record whose shaders are
  not stored waits until the game has created all of them.
- The list stays valid when the shader converter changes, because a record
  describes Direct3D state and shader hashes, not converted code.
- A background thread writes both files when no pipeline was added for a
  few seconds, and once more at device teardown. Nothing is written on the
  present path.

Only one device per process owns the archive, so two devices never write
the same files.
 */
class PipelineArchive {
public:
  static std::unique_ptr<PipelineArchive> Create(WMT::Device device, WMTMetalVersion metal_version);

  ~PipelineArchive();

  PipelineArchive(const PipelineArchive &) = delete;
  PipelineArchive &operator=(const PipelineArchive &) = delete;

  /**
  Create a pipeline with an archive lookup. `add_later` is set when the
  archive missed; the caller should call AddRenderPipeline() with the same
  info after it has published the pipeline.
   */
  WMT::Reference<WMT::RenderPipelineState>
  NewRenderPipelineState(WMTRenderPipelineInfo &info, WMT::Reference<WMT::Error> &err, bool &add_later);
  void AddRenderPipeline(WMTRenderPipelineInfo &info);

  WMT::Reference<WMT::ComputePipelineState>
  NewComputePipelineState(WMTComputePipelineInfo &info, WMT::Reference<WMT::Error> &err, bool &add_later);
  void AddComputePipeline(WMTComputePipelineInfo &info);

  void Record(const PipelineRecord &record);

  /**
  Called once for each new shader. Appends to `ready` every recorded
  pipeline whose shaders now all exist. Each record is returned once.
   */
  void OnShaderCreated(const Sha1Digest &shader, std::vector<PipelineRecord> &ready);

  void OnWarmUpQueued() { warm_queued_.fetch_add(1, std::memory_order_relaxed); }
  void OnWarmUpFinished(bool success);

  /**
  SC6 fork: keep the DXBC bytecode of a shader the game created, so that the
  shader file can store it if a recorded pipeline uses it.
   */
  void NoteShaderCode(const Sha1Digest &shader, const void *code, size_t length);

  /**
  SC6 fork: the stored shaders (hash and DXBC bytecode) that a recorded
  pipeline still waits for. The pipeline cache creates them at start.
   */
  std::vector<std::pair<Sha1Digest, std::vector<uint8_t>>> StoredShadersForWarmUp();

  /**
  SC6 fork: the encode thread waited `ns` nanoseconds for a pipeline that was
  not compiled yet. Counted for every device, logged with the archive stats.
   */
  static void NoteEncodeWait(uint64_t ns);

  void LogStats(const char *when);

private:
  PipelineArchive(WMT::Device device, std::string archive_path, std::string list_path, std::string shaders_path);

  void LoadArchive();
  void LoadList();
  void LoadShaders();
  void SaveShaders();
  void SaveIfDirty();
  void SaverThread();
  void MarkChanged();

  using clock = std::chrono::steady_clock;

  WMT::Reference<WMT::Device> device_;
  std::string archive_path_;
  std::string list_path_;
  std::string shaders_path_;

  /* Shared by pipeline compiles that read or add to the archive, exclusive
     while the archive is serialized. */
  std::shared_mutex archive_rw_;
  WMT::Reference<WMT::BinaryArchive> archive_;
  obj_handle_t archive_handle_ = NULL_OBJECT_HANDLE;
  /* false when the archive started empty: then every lookup would miss, so
     the extra fail-on-miss attempt is skipped */
  bool archive_loaded_ = false;

  mullion::mutex records_mutex_;
  std::unordered_set<Sha1Digest> known_records_;
  std::vector<std::vector<uint8_t>> record_blobs_;

  /* SC6 fork: DXBC bytecode by shader hash, for every shader the game created and every stored shader; the shader
     file keeps the ones that recorded pipelines use */
  mullion::mutex shader_code_mutex_;
  std::unordered_map<Sha1Digest, std::vector<uint8_t>> shader_code_;
  std::unordered_set<Sha1Digest> recorded_shaders_; /* guarded by shader_code_mutex_ */
  std::atomic<bool> shaders_dirty_ = false;
  uint64_t loaded_shaders_ = 0;

  mullion::mutex warm_mutex_;
  std::vector<PipelineRecord> warm_records_;
  std::vector<uint8_t> warm_missing_;
  std::unordered_map<Sha1Digest, std::vector<uint32_t>> warm_waiting_;

  std::atomic<bool> archive_dirty_ = false;
  std::atomic<bool> list_dirty_ = false;
  std::atomic<int64_t> last_change_ = 0;
  mullion::mutex save_mutex_;

  std::atomic<uint64_t> hits_ = 0;
  std::atomic<uint64_t> misses_ = 0;
  std::atomic<uint64_t> added_ = 0;
  std::atomic<uint64_t> warm_queued_ = 0;
  std::atomic<uint64_t> warm_finished_ = 0;
  std::atomic<uint64_t> warm_failed_ = 0;
  uint64_t loaded_records_ = 0;
  uint64_t logged_stats_sum_ = 0;

  mullion::mutex saver_mutex_;
  mullion::condition_variable saver_cond_;
  bool saver_stop_ = false;
  mullion::thread saver_;
};

} // namespace mullion
