#include "d3d11_sc6_paths.hpp"
#include "d3d11_sc6_toggles.hpp"
#include "config/config.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

namespace mullion {

static const std::array<Sc6PathInfo, size_t(Sc6Path::Count)> kPaths = {{
    {"cbuffer-bounds", "mullion.cbufferBounds", SM50_SHADER_FLAG_SKIP_CBUFFER_BOUNDS},
    {"frac-floor", "mullion.fracFloor", SM50_SHADER_FLAG_SKIP_FRAC_FLOOR},
    {"sample-branch-merge", "mullion.sampleBranchMerge", SM50_SHADER_FLAG_SKIP_SAMPLE_BRANCH_MERGE},
    {"minmax-branch-merge", "mullion.minmaxBranchMerge", SM50_SHADER_FLAG_SKIP_MINMAX_BRANCH_MERGE},
    {"group-sync-fence", "mullion.groupSyncFence", SM50_SHADER_FLAG{}},
    {"gs-layered", "mullion.gsLayered", SM50_SHADER_FLAG{}},
    {"pipeline-archive", "mullion.pipelineArchive", SM50_SHADER_FLAG{}},
    {"anisotropic-gradients", "mullion.anisoGradients", SM50_SHADER_FLAG_SKIP_ANISO_GRADIENTS},
    {"lazy-sampler-emulation", "mullion.lazySamplerEmulation", SM50_SHADER_FLAG{}},
    {"hoist-loads", "mullion.hoistLoads", SM50_SHADER_FLAG_SKIP_HOIST_LOADS},
    {"bounds-guard", "mullion.boundsGuard", SM50_SHADER_FLAG_SKIP_BOUNDS_GUARD},
    {"cbuffer-dwords", "mullion.cbufferDwords", SM50_SHADER_FLAG_SKIP_CBUFFER_DWORDS},
    {"unbound-drop", "mullion.unboundDrop", SM50_SHADER_FLAG_SKIP_UNBOUND_DROP},
    /* in the shader variants' keys instead of the flags (ShaderVariantVertex's and ShaderVariantPixel's packed_varyings) */
    {"split-varyings", "mullion.splitVaryings", SM50_SHADER_FLAG{}},
}};

const Sc6PathInfo &
Sc6PathDescription(Sc6Path path) {
  return kPaths[size_t(path)];
}

// whether the comma-separated `list` names `name` as a whole item, or is "all"
static bool
ListNames(const std::string &list, const std::string &name) {
  size_t begin = 0;
  while (begin <= list.size()) {
    size_t end = list.find(',', begin);
    if (end == std::string::npos)
      end = list.size();
    auto item = list.substr(begin, end - begin);
    while (!item.empty() && item.front() == ' ')
      item.erase(item.begin());
    while (!item.empty() && item.back() == ' ')
      item.pop_back();
    if (item == name || item == "all")
      return true;
    begin = end + 1;
  }
  return false;
}

static constexpr size_t kPathCount = size_t(Sc6Path::Count);

// what the config option and the lists say, which stays the same for the process
struct PathBase {
  std::array<bool, kPathCount> option_on;
  std::array<bool, kPathCount> listed_off;
  bool gs_layered_env_off;
};

static const PathBase &
Base() {
  static PathBase base;
  static std::once_flag once;
  std::call_once(once, [] {
    auto &config = Config::getInstance();
    auto config_list = config.getOption<std::string>("mullion.disable", "");
    auto env_list = env::getEnvVar("MULLION_SC6_DISABLE");
    for (size_t i = 0; i < kPaths.size(); i++) {
      base.option_on[i] = config.getOption<bool>(kPaths[i].option, true);
      base.listed_off[i] = ListNames(config_list, kPaths[i].name) || ListNames(env_list, kPaths[i].name);
    }
    // the older switch for the layered geometry shader path keeps working
    base.gs_layered_env_off = env::getEnvVar("MULLION_SC6_GS_LAYERED") == "0";
  });
  return base;
}

// sc6vrr.ini [optimizations]'s toggle for the path, which decides in place of the config option; the lists still turn a
// path off
static std::optional<bool>
Optimization(Sc6Path path) {
  switch (path) {
  case Sc6Path::CbufferDwords: return Sc6ToggleValue(Sc6Toggle::WholeRowConstantLoads);
  case Sc6Path::SplitVaryings: return Sc6ToggleValue(Sc6Toggle::PackedVaryings);
  case Sc6Path::UnboundDrop: return Sc6ToggleValue(Sc6Toggle::SkipUnboundSelect);
  case Sc6Path::HoistLoads: return Sc6ToggleValue(Sc6Toggle::SkipHoistLoads);
  case Sc6Path::BoundsGuard: return Sc6ToggleValue(Sc6Toggle::SkipBoundsGuard);
  case Sc6Path::CbufferBounds:
  case Sc6Path::AnisoGradients: return Sc6ToggleValue(Sc6Toggle::SkipCbufferBoundsAndLodGradients);
  default: return {};
  }
}

/* the values in force. The toggle file's watcher thread replaces them (Sc6RecomputePaths) while other threads read
   them, so each is an atomic. */
static std::array<std::atomic<bool>, kPathCount> g_enabled{};
static std::atomic<uint32_t> g_skip_flags = 0;
static std::atomic<uint64_t> g_settings_key = 0;
static std::mutex g_compute_mutex;
static std::string g_off;

static void
Compute(bool first) {
  std::lock_guard<std::mutex> lock(g_compute_mutex);
  auto &base = Base();
  std::string off;
  uint32_t flags = 0;
  for (size_t i = 0; i < kPaths.size(); i++) {
    auto optimization = Optimization(Sc6Path(i));
    bool enabled = (optimization ? !*optimization : base.option_on[i]) && !base.listed_off[i];
    if (Sc6Path(i) == Sc6Path::GSLayered && base.gs_layered_env_off)
      enabled = false;
    g_enabled[i].store(enabled);
    if (!enabled) {
      flags |= kPaths[i].skip_flag;
      off += off.empty() ? kPaths[i].name : std::string(", ") + kPaths[i].name;
    }
  }
  g_skip_flags.store(flags);
  g_settings_key.store(uint64_t(flags) | (uint64_t(!g_enabled[size_t(Sc6Path::SplitVaryings)].load()) << 32));
  if (first ? !off.empty() : off != g_off)
    WARN(first ? "SC6 paths turned off: " : "SC6 paths turned off after the reload: ", off.empty() ? "none" : off);
  g_off = off;
}

static void
Init() {
  static std::once_flag once;
  std::call_once(once, [] { Compute(true); });
}

void
Sc6RecomputePaths() {
  Init();
  Compute(false);
}

bool
Sc6PathEnabled(Sc6Path path) {
  Init();
  return g_enabled[size_t(path)].load(std::memory_order_relaxed);
}

SM50_SHADER_FLAG
Sc6SkipShaderFlags() {
  Init();
  return SM50_SHADER_FLAG(g_skip_flags.load(std::memory_order_relaxed));
}

uint64_t
Sc6SettingsKey() {
  Init();
  return g_settings_key.load(std::memory_order_relaxed);
}

bool
Sc6SettingsKeyPacksVaryings(uint64_t key) {
  return (key >> 32) & 1;
}

static std::atomic<bool> g_minmax_emulation_needed{false};

void
NoteSamplerNeedsMinMaxEmulation() {
  if (!g_minmax_emulation_needed.exchange(true))
    WARN("A sampler needs the minimum or maximum filter emulation; shaders made from now on include it");
}

SM50_SHADER_FLAG
SamplerEmulationsLeftOut() {
  if (!Sc6PathEnabled(Sc6Path::LazySamplerEmulation))
    return SM50_SHADER_FLAG{};
  uint32_t flags = SM50_SHADER_FLAG_NO_CUSTOM_BORDER;
  if (!g_minmax_emulation_needed.load(std::memory_order_relaxed))
    flags |= SM50_SHADER_FLAG_NO_MINMAX_FILTER;
  return SM50_SHADER_FLAG(flags);
}

} // namespace mullion
