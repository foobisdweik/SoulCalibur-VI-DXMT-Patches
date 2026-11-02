#ifndef SC6_INPUT_CONTROL_H
#define SC6_INPUT_CONTROL_H
#include <windows.h>
#include <stdint.h>
#define SC6_INPUT_CONTROL_VERSION 1
#define SC6_INPUT_CAPTURE_MAX 16384
enum { SC6_INPUT_STATUS = 1, SC6_INPUT_A, SC6_INPUT_B, SC6_INPUT_STOP, SC6_INPUT_RESTORE };
typedef struct {
  double seconds;
  uint32_t battle, cycles, reads, presses, early;
} sc6_input_sample;
typedef struct {
  uint32_t version, pid;
  LONG command, rate, latch, early, capturing;
  uint32_t count, dropped;
  sc6_input_sample samples[SC6_INPUT_CAPTURE_MAX];
} sc6_input_control;
static void sc6_input_object_name(char *out, size_t size, DWORD pid, const char *kind) {
  snprintf(out, size, "Local\\SC6VRR.Input.%lu.%s", (unsigned long)pid, kind);
}
#endif
