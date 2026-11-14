#include "d3d11_sc6_toggles.hpp"
#include "airconv_public.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>

namespace mullion {

static constexpr std::array<const char *, size_t(Sc6Toggle::Count)> kNames = {
    "EagerWarmUp",      "WideWarmUpThreads", "WholeRowConstantLoads", "PackedVaryings",
    "SkipUnboundSelect", "SkipHoistLoads",   "SkipBoundsGuard",       "SkipCbufferBoundsAndLodGradients",
    "PostProcessingOff",
};

struct ToggleFile {
  std::array<std::optional<bool>, size_t(Sc6Toggle::Count)> values;
  std::array<bool, size_t(Sc6Toggle::Count)> watch{};
};

static std::string
Lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower((unsigned char)c));
  return s;
}

static std::string
Trim(const std::string &s) {
  auto begin = s.find_first_not_of(" \t\r");
  if (begin == std::string::npos)
    return "";
  auto end = s.find_last_not_of(" \t\r");
  return s.substr(begin, end - begin + 1);
}

static const ToggleFile &
File() {
  static ToggleFile file;
  static std::once_flag once;
  std::call_once(once, [] {
    auto exe = env::getExePath();
    auto slash = exe.find_last_of(env::PlatformDirSlash);
    std::string path = (slash == std::string::npos ? std::string() : exe.substr(0, slash + 1)) + "sc6-optimizations.ini";
    std::ifstream stream(str::topath(path.c_str()).c_str());
    if (!stream) {
      Logger::info(str::format("SC6 toggles: no ", path, ", so every optimization keeps its default"));
      return;
    }
    std::string line, summary;
    unsigned number = 0;
    while (std::getline(stream, line)) {
      number++;
      line = Trim(line);
      if (line.empty())
        continue;
      auto eq = line.find('=');
      std::string key = eq == std::string::npos ? "" : Lower(Trim(line.substr(0, eq)));
      std::string value = eq == std::string::npos ? "" : Lower(Trim(line.substr(eq + 1)));
      bool on = value == "true" || value == "1" || value == "on" || value == "yes";
      bool off = value == "false" || value == "0" || value == "off" || value == "no";
      bool known = false;
      for (size_t i = 0; i < kNames.size() && (on || off); i++) {
        if (key == Lower(kNames[i])) {
          file.values[i] = on;
          known = true;
        } else if (key == "watch" + Lower(kNames[i])) {
          file.watch[i] = on;
          known = true;
        }
      }
      if (!known) {
        WARN("SC6 toggles: ", path, " line ", number, " is not `Name = true` or `Name = false` for a known name: ", line);
        continue;
      }
      summary += (summary.empty() ? "" : ", ") + Trim(line.substr(0, eq)) + "=" + (on ? "true" : "false");
    }
    WARN("SC6 toggles from ", path, ": ", summary.empty() ? std::string("none") : summary);
  });
  return file;
}

const char *
Sc6ToggleName(Sc6Toggle toggle) {
  return kNames[size_t(toggle)];
}

std::optional<bool>
Sc6ToggleValue(Sc6Toggle toggle) {
  return File().values[size_t(toggle)];
}

bool
Sc6Watch(Sc6Toggle toggle) {
  return File().watch[size_t(toggle)];
}

void
Sc6WatchLog(Sc6Toggle toggle, const std::string &text) {
  if (Sc6Watch(toggle))
    WARN("[watch] ", Sc6ToggleName(toggle), ": ", text);
}

void
Sc6WatchCount(Sc6Toggle toggle, const std::string &text) {
  static std::array<std::atomic<uint64_t>, size_t(Sc6Toggle::Count)> counts{};
  if (!Sc6Watch(toggle))
    return;
  auto n = ++counts[size_t(toggle)];
  if (n <= 5 || n % 100 == 0)
    WARN("[watch] ", Sc6ToggleName(toggle), ": #", n, " ", text);
}

void
Sc6WatchConvertedShader(unsigned shader_flags, const std::string &name) {
  auto on = [&](unsigned flag) { return (shader_flags & flag) != 0; };
  auto note = [&](Sc6Toggle toggle, bool active) {
    Sc6WatchCount(toggle, std::string(active ? "on" : "off") + " in converted shader " + name);
  };
  note(Sc6Toggle::WholeRowConstantLoads, on(SM50_SHADER_FLAG_SKIP_CBUFFER_DWORDS));
  note(Sc6Toggle::SkipUnboundSelect, on(SM50_SHADER_FLAG_SKIP_UNBOUND_DROP));
  note(Sc6Toggle::SkipHoistLoads, on(SM50_SHADER_FLAG_SKIP_HOIST_LOADS));
  note(Sc6Toggle::SkipBoundsGuard, on(SM50_SHADER_FLAG_SKIP_BOUNDS_GUARD));
  note(Sc6Toggle::SkipCbufferBoundsAndLodGradients,
       on(SM50_SHADER_FLAG_SKIP_CBUFFER_BOUNDS) && on(SM50_SHADER_FLAG_SKIP_ANISO_GRADIENTS));
}

void
Sc6WatchPackedVaryings(bool packed, const char *reason) {
  static std::atomic<uint64_t> packed_count = 0, split_count = 0;
  if (!Sc6Watch(Sc6Toggle::PackedVaryings))
    return;
  auto p = packed ? ++packed_count : packed_count.load();
  auto s = packed ? split_count.load() : ++split_count;
  auto n = p + s;
  if (n <= 5 || n % 100 == 0 || (!packed && s <= 20))
    WARN("[watch] PackedVaryings: pipeline ", n, " ", packed ? "packed" : "split", packed ? "" : " (", packed ? "" : reason,
         packed ? "" : ")", "; packed ", p, ", split ", s, " so far");
}

} // namespace mullion
