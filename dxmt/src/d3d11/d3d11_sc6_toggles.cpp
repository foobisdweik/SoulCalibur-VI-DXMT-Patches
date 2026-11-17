#include "d3d11_sc6_toggles.hpp"
#include "d3d11_sc6_paths.hpp"
#include "airconv_public.h"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>

namespace mullion {

static constexpr std::array<const char *, size_t(Sc6Toggle::Count)> kNames = {
    "EagerWarmUp",      "WideWarmUpThreads", "WholeRowConstantLoads", "PackedVaryings",
    "SkipUnboundSelect", "SkipHoistLoads",   "SkipBoundsGuard",       "SkipCbufferBoundsAndLodGradients",
    "PostProcessingOff",
};

static constexpr size_t kCount = size_t(Sc6Toggle::Count);

// what one read of the file says
struct ToggleFile {
  std::array<std::optional<bool>, kCount> values;
  std::array<bool, kCount> watch{};
  std::string summary;
};

/* the values in force, which the watcher thread replaces while other threads read them: 0 = the file leaves the
   toggle out, 1 = false, 2 = true */
static std::array<std::atomic<uint8_t>, kCount> g_values{};
static std::array<std::atomic<bool>, kCount> g_watch{};
static std::string g_path;
static std::once_flag g_once;

static std::mutex g_listeners_mutex;
static std::map<int, std::function<void()>> g_listeners;
static int g_next_listener = 1;

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

// false when the file does not exist. Only the [optimizations] section of sc6vrr.ini counts: the file's other
// sections belong to the sc6vrr DLL and the launch scripts.
static bool
Parse(ToggleFile &file) {
  std::ifstream stream(str::topath(g_path.c_str()).c_str());
  if (!stream)
    return false;
  std::string line;
  unsigned number = 0;
  bool in_section = false;
  while (std::getline(stream, line)) {
    number++;
    // text after ";" is a comment, as in the rest of sc6vrr.ini
    line = Trim(line.substr(0, line.find(';')));
    if (line.empty())
      continue;
    if (line.front() == '[') {
      in_section = Lower(line) == "[optimizations]";
      continue;
    }
    if (!in_section)
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
      WARN("SC6 toggles: ", g_path, " line ", number, " is not `Name = true` or `Name = false` for a known name: ", line);
      continue;
    }
    file.summary += (file.summary.empty() ? "" : ", ") + Trim(line.substr(0, eq)) + "=" + (on ? "true" : "false");
  }
  return true;
}

static uint8_t
Encode(std::optional<bool> value) {
  return value ? (*value ? 2 : 1) : 0;
}

static const char *
Describe(uint8_t value) {
  return value == 2 ? "true" : value == 1 ? "false" : "left out (the default)";
}

static void
Store(const ToggleFile &file) {
  for (size_t i = 0; i < kCount; i++) {
    g_values[i].store(Encode(file.values[i]));
    g_watch[i].store(file.watch[i]);
  }
}

// the file's modification time and size, which change when an editor saves it; 0 when it does not exist
struct Stamp {
  uint64_t time = 0, size = 0;
  bool operator==(const Stamp &) const = default;
};

static Stamp
ReadStamp() {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(str::tows(g_path.c_str()).c_str(), GetFileExInfoStandard, &data))
    return {};
  return {(uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
          (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
}

static void
Reload() {
  ToggleFile file;
  bool exists = Parse(file);
  std::array<uint8_t, kCount> old_values;
  std::array<bool, kCount> old_watch;
  for (size_t i = 0; i < kCount; i++) {
    old_values[i] = g_values[i].load();
    old_watch[i] = g_watch[i].load();
  }
  /* Saves of sc6vrr.ini that change only the DLL's or the launch scripts' sections leave the toggles as they were.
     Skip those, because the listeners below restart the pipeline warm-up. */
  bool changed = false;
  for (size_t i = 0; i < kCount; i++)
    changed |= Encode(file.values[i]) != old_values[i] || file.watch[i] != old_watch[i];
  if (!changed)
    return;
  Store(file);
  WARN("SC6 toggles reloaded from ", g_path, ": ",
       !exists ? std::string("no file, so every optimization keeps its default")
       : file.summary.empty() ? std::string("none")
                              : file.summary);
  for (size_t i = 0; i < kCount; i++) {
    uint8_t value = Encode(file.values[i]);
    if (value != old_values[i] && (file.watch[i] || old_watch[i]))
      WARN("[watch] ", kNames[i], ": now ", Describe(value), ", was ", Describe(old_values[i]));
    if (value != old_values[i] && Sc6Toggle(i) == Sc6Toggle::PostProcessingOff)
      WARN("SC6 toggles: PostProcessingOff changes Engine.ini, so it applies once the game is closed and started again");
    if (file.watch[i] != old_watch[i])
      WARN("[watch] ", kNames[i], ": watch lines ", file.watch[i] ? "on" : "off");
  }
  // the paths first, because the listeners read them
  Sc6RecomputePaths();
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  for (auto &[id, listener] : g_listeners)
    listener();
}

/* checks the file once a second. A save can show its new time before the editor has written all of it, so a
   change applies once the stamp has stayed the same for 250 ms. */
static void
WatchFile() {
  env::setThreadName("mullion-sc6-toggles");
  Stamp applied = ReadStamp();
  for (;;) {
    ::Sleep(1000);
    Stamp now = ReadStamp();
    if (now == applied)
      continue;
    for (;;) {
      ::Sleep(250);
      Stamp later = ReadStamp();
      if (later == now)
        break;
      now = later;
    }
    applied = now;
    Reload();
  }
}

static void
Init() {
  std::call_once(g_once, [] {
    auto exe = env::getExePath();
    auto slash = exe.find_last_of(env::PlatformDirSlash);
    g_path = (slash == std::string::npos ? std::string() : exe.substr(0, slash + 1)) + "sc6vrr.ini";
    ToggleFile file;
    if (!Parse(file))
      Logger::info(str::format("SC6 toggles: no ", g_path, ", so every optimization keeps its default"));
    else
      WARN("SC6 toggles from ", g_path, ": ", file.summary.empty() ? std::string("none") : file.summary);
    Store(file);
    mullion::thread(WatchFile).detach();
  });
}

const char *
Sc6ToggleName(Sc6Toggle toggle) {
  return kNames[size_t(toggle)];
}

std::optional<bool>
Sc6ToggleValue(Sc6Toggle toggle) {
  Init();
  auto value = g_values[size_t(toggle)].load(std::memory_order_relaxed);
  return value ? std::optional<bool>(value == 2) : std::nullopt;
}

bool
Sc6Watch(Sc6Toggle toggle) {
  Init();
  return g_watch[size_t(toggle)].load(std::memory_order_relaxed);
}

int
Sc6AddToggleListener(std::function<void()> listener) {
  Init();
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  int id = g_next_listener++;
  g_listeners.emplace(id, std::move(listener));
  return id;
}

void
Sc6RemoveToggleListener(int id) {
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  g_listeners.erase(id);
}

void
Sc6WatchLog(Sc6Toggle toggle, const std::string &text) {
  if (Sc6Watch(toggle))
    WARN("[watch] ", Sc6ToggleName(toggle), ": ", text);
}

void
Sc6WatchCount(Sc6Toggle toggle, const std::string &text) {
  static std::array<std::atomic<uint64_t>, kCount> counts{};
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
