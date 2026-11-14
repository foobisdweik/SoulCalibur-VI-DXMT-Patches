#pragma once
#include <functional>
#include <optional>
#include <string>

namespace mullion {

/* SC6 fork: the optimizations of the SC6 build, which sc6-optimizations.ini next to the game's executable turns on or
 * off. The file holds only booleans, one `Name = true` or `Name = false` a line, and each optimization `X` has a
 * `WatchX` under it, which logs what that optimization does (WARN lines that start with "[watch] X"). A name the file
 * leaves out keeps its default: the config option or the SoulcaliburVI.exe profile.
 * d3d11.dll reads the file at its first use, and a thread then checks the file once a second, so a saved change applies
 * while the game runs ("SC6 toggles reloaded" in the log):
 *   - the Watch lines and both warm-up toggles apply at once,
 *   - a shader toggle applies to each pipeline the game asks for after the change. Those pipelines and their shaders
 *     compile again, so a change stutters like a first run; changing back finds the earlier ones (Sc6SettingsKey()).
 * PostProcessingOff is read by the SC6-VRR scripts, which change the game's Engine.ini while the game is closed;
 * d3d11.dll only logs it. */
enum class Sc6Toggle : unsigned {
  /* compile the recorded pipelines when the device starts (d3d11.pipelineEagerWarmUp) */
  EagerWarmUp,
  /* warm-up compiles use all but 2 cores (d3d11.pipelineWarmUpThreads), in place of a quarter of them */
  WideWarmUpThreads,
  /* a constant buffer read loads its whole 16-byte row (mullion.cbufferDwords = False) */
  WholeRowConstantLoads,
  /* varyings pass as one float4 a register (mullion.splitVaryings = False) */
  PackedVaryings,
  /* no select drops what an unbound texture answers (mullion.unboundDrop = False) */
  SkipUnboundSelect,
  /* loads stay where the shader uses them (mullion.hoistLoads = False) */
  SkipHoistLoads,
  /* no branch around a buffer access that may fall outside its view (mullion.boundsGuard = False) */
  SkipBoundsGuard,
  /* no bounds check on constant buffer rows and no explicit-gradient sampling for an anisotropic sampler's LOD bias
     (mullion.cbufferBounds = False, mullion.anisoGradients = False) */
  SkipCbufferBoundsAndLodGradients,
  /* the game's post-processing passes off ([SystemSettings] in Engine.ini, config/apply-system-settings.sh) */
  PostProcessingOff,
  Count,
};

const char *Sc6ToggleName(Sc6Toggle toggle);

/* what sc6-optimizations.ini sets the toggle to, or nothing when it leaves it out */
std::optional<bool> Sc6ToggleValue(Sc6Toggle toggle);

/* whether sc6-optimizations.ini has WatchX = true for the toggle */
bool Sc6Watch(Sc6Toggle toggle);

/* `listener` runs on the watcher thread after each reload, once the paths follow the new values; the id removes it.
   A listener must not add or remove listeners. */
int Sc6AddToggleListener(std::function<void()> listener);
void Sc6RemoveToggleListener(int id);

/* a WARN line "[watch] <name>: <text>", where the toggle is watched */
void Sc6WatchLog(Sc6Toggle toggle, const std::string &text);

/* counts one event of a watched toggle, and logs the first 5 and then every 100th */
void Sc6WatchCount(Sc6Toggle toggle, const std::string &text);

/* the shader flags' converter toggles that are on, as watch lines for one converted shader `name` */
void Sc6WatchConvertedShader(unsigned shader_flags, const std::string &name);

/* a pipeline that packs its varyings, or the reason it does not */
void Sc6WatchPackedVaryings(bool packed, const char *reason);

} // namespace mullion
