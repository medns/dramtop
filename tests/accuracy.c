/*
 * Accuracy test: runs memory workloads whose read bandwidth is known and
 * compares it with what dramtop reports over the same time window.
 *
 * The workloads run in copies of this program started with --load. Each one
 * streams reads through buffers far larger than any cache, counts the bytes
 * and reports them on stdout. dramtop runs next to them with a -P filter on
 * this program's name, so other activity on the machine does not matter.
 *
 * Needs hardware counters and the rights to use them:
 *     make accuracy && sudo tests/accuracy [path/to/dramtop]
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CHUNK       (256UL << 20)
#define LOAD_SEC    8.0
#define SLICE_SEC   0.3
#define TOLERANCE   0.05
#define MAX_THREADS 8
#define MAX_SAMPLES 256

static double now(void) {
    struct timespec ts = {0};
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void pause_for(double sec) {
    struct timespec ts = {(time_t)sec, (long)((sec - (double)(time_t)sec) * 1e9)};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static pid_t spawn(char *const argv[], int out_fd) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out_fd >= 0) posix_spawn_file_actions_adddup2(&fa, out_fd, STDOUT_FILENO);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) {
        errno = rc;
        return -1;
    }
    return pid;
}

/* ------------------------------------------------------------------ */
/* Load processes                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    double until, bytes;
    uint64_t sink;
    pthread_t th;
} job_t;

static uint8_t *new_buffer(void) {
    uint8_t *b = malloc(CHUNK);
    if (b) memset(b, 1, CHUNK);
    return b;
}

/* One load per 64 bytes still brings in every cache line, whether lines are 64 or 128 bytes. */
static double stream(job_t *j, double until) {
    double bytes = 0;
    uint64_t s = 0;
    while (now() < until) {
        const volatile uint64_t *q = (const volatile uint64_t *)j->buf;
        for (size_t i = 0; i < CHUNK / 8; i += 8) s += q[i];
        bytes += (double)CHUNK;
    }
    j->sink += s;
    return bytes;
}

static void *steady_thread(void *arg) {
    job_t *j = arg;
    j->bytes = stream(j, j->until);
    return NULL;
}

static void *short_thread(void *arg) {
    job_t *j = arg;
    double end = now() + SLICE_SEC;
    j->bytes = stream(j, end < j->until ? end : j->until);
    return NULL;
}

/* Keeps replacing a short-lived streaming thread until the slot's time is up. */
static void *churn_slot(void *arg) {
    job_t *slot = arg;
    while (now() < slot->until) {
        job_t one = {.buf = slot->buf, .until = slot->until};
        pthread_t t;
        if (pthread_create(&t, NULL, short_thread, &one)) break;
        pthread_join(t, NULL);
        slot->bytes += one.bytes;
        slot->sink += one.sink;
    }
    return NULL;
}

/* Streams with n threads for secs seconds, then reports "pid bytes start end". */
static int run_threads(int n, bool churn, double secs) {
    job_t jobs[MAX_THREADS];
    for (int i = 0; i < n; i++) {
        jobs[i] = (job_t){.buf = new_buffer()};
        if (!jobs[i].buf) {
            fputs("accuracy: out of memory\n", stderr);
            return 1;
        }
    }
    double start = now();
    for (int i = 0; i < n; i++) {
        jobs[i].until = start + secs;
        if (pthread_create(&jobs[i].th, NULL, churn ? churn_slot : steady_thread, &jobs[i])) {
            fputs("accuracy: cannot create a thread\n", stderr);
            return 1;
        }
    }
    double bytes = 0;
    for (int i = 0; i < n; i++) {
        pthread_join(jobs[i].th, NULL);
        bytes += jobs[i].bytes;
    }
    double end = now();
    printf("%d %.0f %.6f %.6f\n", (int)getpid(), bytes, start, end);
    fflush(stdout);
    for (int i = 0; i < n; i++) free(jobs[i].buf);
    return 0;
}

/* Streams alone first so that dramtop attaches to this process before its
 * children exist, then starts n single-threaded children. */
static int run_parent(const char *self, int n, double secs) {
    job_t warm = {.buf = new_buffer()};
    if (!warm.buf) {
        fputs("accuracy: out of memory\n", stderr);
        return 1;
    }
    stream(&warm, now() + 1.5);
    free(warm.buf);

    char secs_s[32];
    snprintf(secs_s, sizeof secs_s, "%.1f", secs);
    char *argv[] = {(char *)self, "--load", "steady", "1", secs_s, NULL};
    int status = 0;
    for (int i = 0; i < n; i++)
        if (spawn(argv, -1) < 0) {
            perror("accuracy: cannot start a child");
            status = 1;
        }
    while (wait(NULL) > 0) {}
    return status;
}

static int run_load(const char *self, int argc, char **argv) {
    if (argc != 3) return 2;
    int n = atoi(argv[1]);
    double secs = atof(argv[2]);
    if (n < 1 || n > MAX_THREADS || secs <= 0) return 2;
    if (!strcmp(argv[0], "steady")) return run_threads(n, false, secs);
    if (!strcmp(argv[0], "churn")) return run_threads(n, true, secs);
    if (!strcmp(argv[0], "fork")) return run_parent(self, n, secs);
    return 2;
}

/* ------------------------------------------------------------------ */
/* Test driver                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name, *mode;
    int n;
    char desc[48];
} scenario_t;

typedef struct {
    double ts, gbs;
} dsample_t;

static bool json_number(const char *line, const char *key, double *out) {
    const char *p = strstr(line, key);
    if (!p) return false;
    char *end;
    p += strlen(key);
    *out = strtod(p, &end);
    return end != p;
}

static void cloexec(const int fds[2]) {
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
}

static bool run_case(const char *self, const char *dramtop, const char *filter, const scenario_t *sc) {
    int lp[2], dp[2];
    if (pipe(lp)) {
        perror("accuracy: pipe");
        return false;
    }
    if (pipe(dp)) {
        perror("accuracy: pipe");
        close(lp[0]);
        close(lp[1]);
        return false;
    }
    cloexec(lp);
    cloexec(dp);

    char n_s[16], secs_s[32];
    snprintf(n_s, sizeof n_s, "%d", sc->n);
    snprintf(secs_s, sizeof secs_s, "%.1f", LOAD_SEC);
    char *load_argv[] = {(char *)self, "--load", (char *)sc->mode, n_s, secs_s, NULL};
    pid_t load = spawn(load_argv, lp[1]);
    close(lp[1]);
    pause_for(0.2);
    char *dt_argv[] = {(char *)dramtop, "-J", "-c", "100", "-P", (char *)filter, NULL};
    pid_t dt = load < 0 ? -1 : spawn(dt_argv, dp[1]);
    close(dp[1]);
    if (load < 0 || dt < 0) {
        const char *why = strerror(errno);
        printf("%-7s %-36s FAIL  (cannot start %s: %s)\n", sc->name, sc->desc, load < 0 ? "the load" : dramtop, why);
        fflush(stdout);
        if (load > 0) {
            kill(load, SIGKILL);
            waitpid(load, NULL, 0);
        }
        close(lp[0]);
        close(dp[0]);
        return false;
    }

    /* Bandwidth the load processes measured themselves, and the window when all of them ran. */
    char line[16384];
    double truth = 0, start = 0, end = 1e300;
    int reports = 0;
    FILE *lf = fdopen(lp[0], "r");
    while (lf && fgets(line, sizeof line, lf)) {
        int pid;
        double bytes, t0, t1;
        if (sscanf(line, "%d %lf %lf %lf", &pid, &bytes, &t0, &t1) != 4 || t1 <= t0) continue;
        truth += bytes / (t1 - t0) / 1e9;
        if (t0 > start) start = t0;
        if (t1 < end) end = t1;
        reports++;
    }
    if (lf) fclose(lf);
    waitpid(load, NULL, 0);

    pause_for(1.5); /* dramtop needs one more sample to cover the end of the window */
    kill(dt, SIGINT);
    dsample_t s[MAX_SAMPLES];
    int ns = 0;
    FILE *df = fdopen(dp[0], "r");
    while (df && fgets(line, sizeof line, df))
        if (ns < MAX_SAMPLES && json_number(line, "\"timestamp\":", &s[ns].ts) &&
            json_number(line, "\"system\":{\"total_bw_gbs\":", &s[ns].gbs))
            ns++;
    if (df) fclose(df);
    waitpid(dt, NULL, 0);

    /* Default 1-second samples that lie entirely inside the window. */
    double sum = 0;
    int used = 0;
    for (int i = 0; i < ns; i++)
        if (s[i].ts - 1.1 >= start && s[i].ts <= end) {
            sum += s[i].gbs;
            used++;
        }
    double measured = used ? sum / used : 0, error = truth > 0 ? measured / truth - 1.0 : 0;
    bool ok = reports == (strcmp(sc->mode, "fork") ? 1 : sc->n) && used >= 3 && truth > 0 &&
              error >= -TOLERANCE && error <= TOLERANCE;

    printf("%-7s %-36s %9.1f %12.1f %+8.1f%%   %s", sc->name, sc->desc, truth, measured, error * 100.0,
           ok ? "PASS" : "FAIL");
    if (!ns) printf("  (no output from dramtop)");
    else if (used < 3) printf("  (only %d samples inside the window)", used);
    putchar('\n');
    fflush(stdout);
    return ok;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--load")) return run_load(argv[0], argc - 2, argv + 2);
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: %s [path/to/dramtop]\n", argv[0]);
        return 2;
    }
    const char *dramtop = argc == 2 ? argv[1] : "./dramtop";
    if (strchr(dramtop, '/') && access(dramtop, X_OK)) {
        fprintf(stderr, "accuracy: cannot run %s: %s\n", dramtop, strerror(errno));
        return 2;
    }

    /* The kernel keeps the first 15 bytes of the program name, and -P matches against that. */
    const char *base = strrchr(argv[0], '/');
    char filter[16];
    snprintf(filter, sizeof filter, "%s", base ? base + 1 : argv[0]);

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = cpus >= MAX_THREADS ? MAX_THREADS : cpus > 0 ? (int)cpus : 1;
    int children = threads >= 4 ? 4 : threads;
    scenario_t cases[] = {
        {"steady", "steady", threads, ""},
        {"fork", "fork", children, ""},
        {"churn", "churn", threads, ""},
    };
    snprintf(cases[0].desc, sizeof cases[0].desc, "%d threads in one process", threads);
    snprintf(cases[1].desc, sizeof cases[1].desc, "%d child processes started later", children);
    snprintf(cases[2].desc, sizeof cases[2].desc, "%d threads, each replaced every %.1fs", threads, SLICE_SEC);

    printf("dramtop accuracy test: %s, %.0f s per case, pass within %.0f%%\n\n", dramtop, LOAD_SEC,
           TOLERANCE * 100.0);
    printf("%-7s %-36s %9s %12s %9s   %s\n", "CASE", "LOAD", "TRUE GB/s", "DRAMTOP GB/s", "ERROR", "RESULT");
    fflush(stdout);
    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) failed += !run_case(argv[0], dramtop, filter, &cases[i]);
    return failed ? 1 : 0;
}
