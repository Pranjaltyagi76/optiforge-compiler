/* libofprof - the OptiForge profile runtime.
 *
 * Linked only into instrumented builds (`optiforge prog.of --profile`). Plain C
 * with no dependency on any compiler header: this is built for the TARGET, not
 * the host (architectural_design.md section 3, rule 7).
 *
 * Everything below is driven by data the compiler emits into the program's own
 * object file. This code knows nothing about control flow: the compiler decides
 * what each counter means and encodes it in __ofprof_records, and this walks
 * that table and formats. That division is what lets a profile carry FUNCTION,
 * BRANCH and LOOP records while the hot path stays a single `incq`.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* --- Emitted by the compiler into the instrumented program ---------------- */

extern uint64_t __ofprof_counters[];
extern const uint64_t __ofprof_num_counters;
extern const uint64_t __ofprof_num_records;
extern const uint64_t __ofprof_num_times;
extern const uint64_t __ofprof_src_hash;
extern const uint64_t __ofprof_opt_level;
extern const char __ofprof_source[];
extern const char __ofprof_compiler[];
extern const char __ofprof_default_path[];
extern const char __ofprof_names[];

/* Four words per record: kind, counter A, counter B, offset into the name blob.
 * Laid out by the compiler, so a change on either side is a link-time or
 * obviously-wrong-output failure rather than a subtle one. */
extern const uint64_t __ofprof_records[];

/* Always emitted, with a single unused slot when --profile-time was not given:
 * PE/COFF has no weak undefined symbol that would let this be conditional. The
 * guard is __ofprof_num_times, which is zero unless timing was asked for. */
extern uint64_t __ofprof_times[];
extern uint64_t __ofprof_time_depth[];
extern uint64_t __ofprof_time_start[];

enum {
  OFPROF_FUNCTION = 0,
  OFPROF_BLOCK = 1,
  OFPROF_BRANCH = 2,
  OFPROF_LOOP = 3,
  OFPROF_TIME = 4
};

#define OFPROF_FORMAT_VERSION 1

/* --- Timing ---------------------------------------------------------------
 *
 * Opt-in, because unlike a counter increment this is a real call. Recursion is
 * handled by only starting the clock at depth zero and only stopping it on the
 * way back out of the outermost call, so a function that calls itself is
 * charged for its whole tree once rather than once per level.
 */

static uint64_t ofprof_now_ns(void) {
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void __ofprof_enter(long long slot) {
  if ((uint64_t)slot >= __ofprof_num_times) {
    return;
  }
  if (__ofprof_time_depth[slot]++ == 0) {
    __ofprof_time_start[slot] = ofprof_now_ns();
  }
}

void __ofprof_exit(long long slot) {
  if ((uint64_t)slot >= __ofprof_num_times) {
    return;
  }
  if (__ofprof_time_depth[slot] == 0) {
    return; /* unbalanced; better to lose the sample than to underflow */
  }
  if (--__ofprof_time_depth[slot] == 0) {
    __ofprof_times[slot] += ofprof_now_ns() - __ofprof_time_start[slot];
  }
}

/* --- Writing the profile -------------------------------------------------- */

static const char* ofprof_name(uint64_t offset) { return __ofprof_names + offset; }

static uint64_t ofprof_counter(uint64_t index) {
  return index < __ofprof_num_counters ? __ofprof_counters[index] : 0;
}

static void ofprof_dump(void) {
  const char* path = getenv("OPTIFORGE_PROFILE_OUT");
  if (path == 0 || path[0] == '\0') {
    path = __ofprof_default_path;
  }

  FILE* out = fopen(path, "w");
  if (out == 0) {
    fprintf(stderr, "ofprof: cannot write '%s'\n", path);
    return;
  }

  uint64_t total = 0;
  for (uint64_t i = 0; i < __ofprof_num_counters; ++i) {
    total += __ofprof_counters[i];
  }

  fprintf(out, "OPTIFORGE_PROFILE %d\n", OFPROF_FORMAT_VERSION);
  fprintf(out, "SOURCE %s\n", __ofprof_source);
  fprintf(out, "SRCHASH 0x%016llx\n", (unsigned long long)__ofprof_src_hash);
  fprintf(out, "OPTLEVEL %llu\n", (unsigned long long)__ofprof_opt_level);
  fprintf(out, "COMPILER %s\n", __ofprof_compiler);
  fprintf(out, "RUNS 1\n");
  fprintf(out, "TOTAL_SAMPLES %llu\n", (unsigned long long)total);

  /* Records are emitted grouped by kind so the file reads top-down: what ran,
   * then where, then which way the branches went. */
  static const int kOrder[] = {OFPROF_FUNCTION, OFPROF_BLOCK, OFPROF_BRANCH,
                               OFPROF_LOOP, OFPROF_TIME};

  for (unsigned pass = 0; pass < sizeof(kOrder) / sizeof(kOrder[0]); ++pass) {
    const int wanted = kOrder[pass];
    int printedAny = 0;

    for (uint64_t i = 0; i < __ofprof_num_records; ++i) {
      const uint64_t* record = &__ofprof_records[i * 4];
      const uint64_t kind = record[0];
      const uint64_t a = record[1];
      const uint64_t b = record[2];
      const char* name = ofprof_name(record[3]);

      if ((int)kind != wanted) {
        continue;
      }
      if (!printedAny) {
        fputc('\n', out);
        printedAny = 1;
      }

      switch (kind) {
        case OFPROF_FUNCTION:
          fprintf(out, "FUNCTION %s %llu\n", name,
                  (unsigned long long)ofprof_counter(a));
          break;

        case OFPROF_BLOCK:
          fprintf(out, "BLOCK %s %llu\n", name,
                  (unsigned long long)ofprof_counter(a));
          break;

        case OFPROF_BRANCH:
          fprintf(out, "BRANCH %s taken=%llu not_taken=%llu\n", name,
                  (unsigned long long)ofprof_counter(a),
                  (unsigned long long)ofprof_counter(b));
          break;

        case OFPROF_LOOP: {
          /* A loop header runs once per entry plus once per iteration, so the
           * difference is the iteration count. Clamped: a profile that says a
           * loop ran fewer times than it was entered is corrupt, and reporting
           * a huge unsigned number instead of zero would hide that. */
          const uint64_t header = ofprof_counter(a);
          const uint64_t entries = ofprof_counter(b);
          const uint64_t iterations = header > entries ? header - entries : 0;
          fprintf(out, "LOOP %s entries=%llu iterations=%llu\n", name,
                  (unsigned long long)entries, (unsigned long long)iterations);
          break;
        }

        case OFPROF_TIME:
          if (a < __ofprof_num_times) {
            fprintf(out, "TIME %s %.3f\n", name, (double)__ofprof_times[a] / 1e6);
          }
          break;

        default:
          break; /* a record kind this runtime predates; skip it */
      }
    }
  }

  fclose(out);
}

/* Registering from a constructor is what makes PROF-07 true: the program's
 * source needs no change at all, and nothing has to be called by hand.
 *
 * A program that dies by signal or calls _exit loses its profile. That is
 * inherent to atexit; __ofprof_flush exists for anyone who needs the data out
 * before then. */
__attribute__((constructor)) static void ofprof_init(void) { atexit(ofprof_dump); }

void __ofprof_flush(void) { ofprof_dump(); }
