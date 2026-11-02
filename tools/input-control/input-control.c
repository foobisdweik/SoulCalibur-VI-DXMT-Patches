/* This console client connects to the mod after the user launches SC6. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../dll/src/input_control.h"

static HANDLE open_map(DWORD pid) {
  char name[128];
  sc6_input_object_name(name, sizeof(name), pid, "map");
  return OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: input-control status|a|b|stop|restore [output.tsv] [--pid PID]\n");
    return 2;
  }
  LONG command = !strcmp(argv[1], "status") ? SC6_INPUT_STATUS :
                 !strcmp(argv[1], "a") ? SC6_INPUT_A :
                 !strcmp(argv[1], "b") ? SC6_INPUT_B :
                 !strcmp(argv[1], "stop") ? SC6_INPUT_STOP :
                 !strcmp(argv[1], "restore") ? SC6_INPUT_RESTORE : 0;
  if (!command) { fprintf(stderr, "Unknown command.\n"); return 2; }
  DWORD pid = 0;
  const char *output = NULL;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = strtoul(argv[++i], NULL, 10);
    else if (!output) output = argv[i];
    else return 2;
  }
  if (output && command != SC6_INPUT_STOP) {
    fprintf(stderr, "Only stop accepts an output path.\n"); return 2;
  }
  HANDLE mapping = NULL;
  if (pid) mapping = open_map(pid);
  else {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry = { .dwSize = sizeof(entry) };
    if (snapshot != INVALID_HANDLE_VALUE) {
      if (Process32First(snapshot, &entry)) do {
        if (_stricmp(entry.szExeFile, "SoulcaliburVI.exe")) continue;
        HANDLE candidate = open_map(entry.th32ProcessID);
        if (!candidate) continue;
        if (mapping) {
          fprintf(stderr, "Multiple endpoints found. Select one with --pid PID.\n");
          CloseHandle(candidate); CloseHandle(mapping); CloseHandle(snapshot); return 1;
        }
        pid = entry.th32ProcessID; mapping = candidate;
      } while (Process32Next(snapshot, &entry));
      CloseHandle(snapshot);
    }
  }
  if (!mapping) {
    fprintf(stderr, "No input-control endpoint found. Launch SC6 with the updated mod first.\n");
    return 1;
  }
  sc6_input_control *control = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*control));
  if (!control || control->version != SC6_INPUT_CONTROL_VERSION || control->pid != pid) {
    fprintf(stderr, "The endpoint version or process does not match.\n"); return 1;
  }
  char name[128];
  sc6_input_object_name(name, sizeof(name), pid, "client");
  HANDLE mutex = CreateMutexA(NULL, FALSE, name);
  DWORD lock = mutex ? WaitForSingleObject(mutex, 2000) : WAIT_FAILED;
  if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) {
    fprintf(stderr, "Another client is using this endpoint.\n"); return 1;
  }
  sc6_input_object_name(name, sizeof(name), pid, "request");
  HANDLE request = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
  sc6_input_object_name(name, sizeof(name), pid, "done");
  HANDLE done = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
  if (!request || !done) { fprintf(stderr, "The endpoint events are unavailable.\n"); return 1; }
  ResetEvent(done);
  InterlockedExchange(&control->command, command);
  SetEvent(request);
  if (WaitForSingleObject(done, 2000) != WAIT_OBJECT_0) {
    fprintf(stderr, "The game did not acknowledge within two seconds. It may be paused or closing.\n");
    return 1;
  }
  printf("pid=%lu polling_target_hz=%ld latch=%ld early_step=%ld recording=%ld frames=%u dropped=%u\n",
         (unsigned long)pid, control->rate, control->latch, control->early,
         control->capturing, control->count, control->dropped);
  if (command == SC6_INPUT_STOP && output) {
    FILE *file = fopen(output, "w");
    if (!file) { fprintf(stderr, "Cannot open output file: %s\n", output); return 1; }
    fprintf(file, "# input-control v1 pid=%lu polling_target_hz=%ld latch=%ld early_step=%ld dropped=%u\n",
            (unsigned long)pid, control->rate, control->latch, control->early, control->dropped);
    fprintf(file, "seconds\tframe_ms\tbattle_steps\tpoll_cycles\tengine_reads\tpresses\tearly_steps\n");
    for (uint32_t i = 1; i < control->count; i++) {
      sc6_input_sample *a = &control->samples[i - 1], *b = &control->samples[i];
      fprintf(file, "%.6f\t%.6f\t%u\t%u\t%u\t%u\t%u\n",
              b->seconds - control->samples[0].seconds, (b->seconds - a->seconds) * 1000,
              b->battle - a->battle, b->cycles - a->cycles, b->reads - a->reads,
              b->presses - a->presses, b->early - a->early);
    }
    if (fclose(file)) { fprintf(stderr, "Writing the capture failed.\n"); return 1; }
    printf("Saved %s\n", output);
  }
  ReleaseMutex(mutex);
  CloseHandle(mutex); CloseHandle(request); CloseHandle(done);
  UnmapViewOfFile(control); CloseHandle(mapping);
  return 0;
}
