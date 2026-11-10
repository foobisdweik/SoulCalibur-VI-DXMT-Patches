#include "dxmt_capture.hpp"
#include "log/log.hpp"
#include "util_env.hpp"

#ifdef __WIN32
#include "windows.h"
#endif

namespace dxmt {

#ifdef _WIN32
// SC6 local: read the controller's right trigger (R2) through XInput, so a
// capture can start during play without a keyboard. The struct matches
// XINPUT_STATE from xinput.h; it is declared here to avoid a link dependency.
namespace {
struct Sc6XInputState {
  DWORD packet;
  WORD buttons;
  BYTE left_trigger;
  BYTE right_trigger;
  SHORT thumbs[4];
};
typedef DWORD(WINAPI *Sc6XInputGetState)(DWORD, Sc6XInputState *);

bool
sc6RightTriggerDown() {
  static Sc6XInputGetState get_state;
  static bool loaded;
  if (!loaded) {
    loaded = true;
    for (const char *name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE mod = LoadLibraryA(name)) {
        get_state = (Sc6XInputGetState)(void *)GetProcAddress(mod, "XInputGetState");
        if (get_state)
          break;
      }
    }
    WARN("SC6 capture trigger: XInput ", get_state ? "loaded" : "not available");
  }
  if (!get_state)
    return false;
  for (DWORD i = 0; i < 4; i++) {
    Sc6XInputState st{};
    // 200 of 255 is a firm press, so a light touch does not start a capture.
    if (get_state(i, &st) == ERROR_SUCCESS && st.right_trigger > 200)
      return true;
  }
  return false;
}
} // namespace

// SC6 local: play a WAV file named by an env var (a Windows path), so the
// player hears when a capture starts and when the trace is written. winmm is
// loaded at runtime to avoid a link dependency. SND_ASYNC returns at once, so
// the command-queue thread does not wait for the sound.
void
sc6PlayCaptureSound(const char *env_name) {
  typedef BOOL(WINAPI * PlaySoundAFn)(LPCSTR, HMODULE, DWORD);
  static PlaySoundAFn play;
  static bool loaded;
  if (!loaded) {
    loaded = true;
    if (HMODULE mod = LoadLibraryA("winmm.dll"))
      play = (PlaySoundAFn)(void *)GetProcAddress(mod, "PlaySoundA");
    WARN("SC6 capture sound: winmm ", play ? "loaded" : "not available");
  }
  auto path = env::getEnvVar(env_name);
  if (!play || path.empty())
    return;
  // 0x20000 = SND_FILENAME, 0x0001 = SND_ASYNC, 0x0002 = SND_NODEFAULT
  if (!play(path.c_str(), nullptr, 0x20000 | 0x0001 | 0x0002))
    WARN("SC6 capture sound: cannot play ", path);
}
#else
void
sc6PlayCaptureSound(const char *) {}
#endif

CaptureState::CaptureState() {
  if (env::getEnvVar("MTL_CAPTURE_ENABLED") != "1")
    return;
  if (env::getExeBaseName() != env::getEnvVar("DXMT_CAPTURE_EXECUTABLE"))
    return;
  WARN("DXMT capture enabled");
  state = State::Idle;
}

CaptureState::NextAction
CaptureState::getNextAction(uint64_t frame) {
  switch (state) {
  case State::Freezed:
    return NextAction::Nothing;
  case State::Idle:
    break;
  case State::WillCapture:
    if (frame == next_capture_frame) {
      state = State::Capturing;
      return NextAction::StartCapture;
    }
    break;
  case State::Capturing:
    if (frame > next_capture_frame) {
      next_capture_frame = 0;
      state = State::Idle;
      return NextAction::StopCapture;
    }
    break;
  }
  return NextAction::Nothing;
};

void
CaptureState::scheduleNextFrameCapture(uint64_t next) {
  if (state != State::Idle)
    return;
  state = State::WillCapture;
  next_capture_frame = next;
}

bool
CaptureState::shouldCaptureNextFrame() {
#ifdef _WIN32
  if (state != State::Idle)
    return false;
  SHORT state_ = GetAsyncKeyState(VK_F10);
  if (state_ & 0x8000 && !capture_key_pressed_) {
    capture_key_pressed_ = true;
    return true;
  }
  capture_key_pressed_ = state_ & 0x8000;
  if (sc6_r2_fire_ms_ && GetTickCount64() >= sc6_r2_fire_ms_) {
    sc6_r2_fire_ms_ = 0;
    WARN("SC6 capture trigger: delay over, capturing next frame");
    return true;
  }
  if (!sc6_r2_used_) {
    bool r2 = sc6RightTriggerDown();
    if (r2 && !sc6_r2_pressed_) {
      sc6_r2_used_ = true;
      auto delay = env::getEnvVar("DXMT_SC6_CAPTURE_DELAY_MS");
      sc6_r2_fire_ms_ = GetTickCount64() + (delay.empty() ? 1500 : std::stoull(delay));
      WARN("SC6 capture trigger: R2 pressed, capture armed");
    }
    sc6_r2_pressed_ = r2;
  }
#endif
  return false;
};

} // namespace dxmt
