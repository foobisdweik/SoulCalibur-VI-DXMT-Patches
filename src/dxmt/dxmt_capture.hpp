#pragma once
#include <cstdint>

namespace dxmt {

// SC6 local: plays the WAV file named by env var env_name, if set.
void sc6PlayCaptureSound(const char *env_name);

class CaptureState {
public:
  enum class NextAction {
    StartCapture,
    StopCapture,
    Nothing,
  };
  NextAction getNextAction(uint64_t frame);
  void scheduleNextFrameCapture(uint64_t next = 0);
  bool shouldCaptureNextFrame();

  CaptureState();

private:
  enum class State {
    Freezed,
    Idle,
    WillCapture,
    Capturing,
  };
  State state = State::Freezed;
  uint64_t next_capture_frame = 0;
  bool capture_key_pressed_ = false;
  // SC6 local: R2 (which starts a Critical Edge) arms a capture that fires a
  // delay later, inside the slow part of the effect. Once per game launch.
  bool sc6_r2_pressed_ = false;
  bool sc6_r2_used_ = false;
  uint64_t sc6_r2_fire_ms_ = 0;
};

} // namespace dxmt
