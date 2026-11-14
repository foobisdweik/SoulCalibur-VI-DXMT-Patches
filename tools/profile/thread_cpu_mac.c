/* CPU use of every thread of one macOS process (for example SoulcaliburVI.exe under CrossOver), by thread name.
 *
 * Usage: thread_cpu_mac <pid> [seconds]     (default 10 seconds)
 *
 * Under CrossOver, GetThreadTimes returns 0 for another process's threads (notes/thread-identity.md), so this
 * reads the macOS counters instead: proc_pidinfo(PROC_PIDLISTTHREADIDS) lists the threads, and
 * PROC_PIDTHREADID64INFO gives each thread's CPU time and name. It never pauses a thread. Wine copies a name to
 * macOS when a thread names itself (SetThreadDescription on the current thread), and the sc6vrr DLL does that for
 * the game's thread names when SC6VRR_THREAD_NAMES=1 or SC6VRR_SAMPLE=1.
 * Output: TSV, busiest first; cpu_pct is the share of one core (100 = one full core). */
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <time.h>
#include <unistd.h>

#ifndef PROC_PIDLISTTHREADIDS
#define PROC_PIDLISTTHREADIDS 28 /* private in the SDK: the 64-bit ids of every thread (xnu bsd/sys/proc_info.h) */
#endif

typedef struct {
  uint64_t id;
  double start, end;
  int seen_end;
  char name[MAXTHREADNAMESIZE];
} thread_row;

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int list(pid_t pid, uint64_t *ids, int max) {
  int bytes = proc_pidinfo(pid, PROC_PIDLISTTHREADIDS, 0, ids, max * (int)sizeof(uint64_t));
  return bytes > 0 ? bytes / (int)sizeof(uint64_t) : 0;
}

static int info(pid_t pid, uint64_t id, double *cpu, char *name) {
  struct proc_threadinfo ti;
  if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, id, &ti, sizeof(ti)) != sizeof(ti))
    return 0;
  *cpu = (ti.pth_user_time + ti.pth_system_time) / 1e9;
  if (name)
    snprintf(name, MAXTHREADNAMESIZE, "%s", ti.pth_name[0] ? ti.pth_name : "-");
  return 1;
}

static int by_cpu(const void *a, const void *b) {
  const thread_row *x = a, *y = b;
  double dx = x->end - x->start, dy = y->end - y->start;
  return dx < dy ? 1 : dx > dy ? -1 : 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <pid> [seconds]\n", argv[0]);
    return 2;
  }
  pid_t pid = atoi(argv[1]);
  double seconds = argc > 2 ? atof(argv[2]) : 10;
  static uint64_t ids[4096];
  static thread_row rows[4096];
  int n = list(pid, ids, 4096), count = 0;
  if (!n) {
    fprintf(stderr, "cannot list the threads of pid %d\n", pid);
    return 1;
  }
  for (int i = 0; i < n; i++)
    if (info(pid, ids[i], &rows[count].start, rows[count].name))
      rows[count++].id = ids[i];
  double t0 = now();
  usleep((useconds_t)(seconds * 1e6));
  double wall = now() - t0, total = 0;
  for (int i = 0; i < count; i++) {
    char name[MAXTHREADNAMESIZE];
    rows[i].seen_end = info(pid, rows[i].id, &rows[i].end, name);
    if (rows[i].seen_end)
      memcpy(rows[i].name, name, sizeof(name)); /* a thread may name itself during the window */
    else
      rows[i].end = rows[i].start;
    total += rows[i].end - rows[i].start;
  }
  qsort(rows, count, sizeof(rows[0]), by_cpu);
  printf("# pid=%d threads=%d wall_s=%.3f total_cpu_pct=%.1f\n", pid, count, wall, 100 * total / wall);
  printf("cpu_pct\tthread_id\tname\n");
  for (int i = 0; i < count; i++)
    printf("%.1f\t%llu\t%s%s\n", 100 * (rows[i].end - rows[i].start) / wall, (unsigned long long)rows[i].id,
           rows[i].name, rows[i].seen_end ? "" : " (ended)");
  return 0;
}
