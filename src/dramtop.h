/*
 * dramtop - real-time DRAM bandwidth monitor
 *
 * Types shared by all source files and the two platform interfaces:
 *   - the collection backend (linux.c, macos.c, windows.c): CPU detection,
 *     process tracking and hardware counters;
 *   - the system layer (os_posix.c, os_win.c): time, threads, terminal and
 *     signals.
 * dramtop.c holds everything else and uses only standard C.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#ifndef DRAMTOP_H
#define DRAMTOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define DT_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define DT_PRINTF(fmt, first)
#endif

#define NAME_LEN   48
#define MAX_AGENTS 8

typedef enum {
    ST_SAMPLING, ST_IDLE, ST_MEM_BW, ST_MEM_LAT, ST_CONTENTION,
    ST_COMPUTE, ST_GPU, ST_ACTIVE, ST_OTHER, ST_NOACCESS
} state_t;

typedef struct {
    double total_gbs, demand_gbs, prefetch_gbs, read_gbs, write_gbs, gpu_gbs;
    double util_pct, cpu_pct, gpu_pct, cpu_power_w, ipc, mpki;
    bool has_bw;
    state_t state;
} metrics_t;

/* Counter totals over one interval. DRAM traffic is in cache lines. */
typedef struct {
    double total, demand, prefetch, read, write;
    double cpu_read; /* lines read for the CPU clusters, which MPKI counts when the chip has agents */
    double gpu;      /* lines of the GPU's traffic that go to a process by its share of GPU time */
    double cycles, insns;
} dt_counts_t;

/* A process as reported by the backend; m is filled in by dramtop.c. */
typedef struct {
    int pid;
    char name[NAME_LEN];
    int threads;
    double cpu_sec;    /* CPU time used during the last scan interval */
    double gpu_sec;    /* GPU time, likewise */
    double energy_j;   /* CPU energy, likewise */
    bool has_bw;       /* c holds counts over the last bw_sec seconds */
    bool has_ipc;      /* c.cycles and c.insns are valid even without has_bw */
    bool denied;       /* no permission to count this process */
    double bw_sec;
    double confidence; /* for estimated traffic: 1 when the process had its CPU cluster or the GPU to itself */
    dt_counts_t c;
    metrics_t m;
} dt_proc_t;

typedef struct {
    int filter_pid;          /* 0: every process */
    const char *filter_name; /* NULL: no name filter */
    bool measure_caps;       /* probe what each part of the chip can reach (skipped with -c) */
} dt_config_t;

typedef struct {
    const char *cpu_name;
    const char *backend;    /* which counters measure DRAM traffic */
    const char *scope;      /* how the whole system is covered, set by plat_start_system() */
    const char *mode;       /* the same as a short JSON value */
    const char *no_proc_bw; /* why per-process traffic is unavailable, NULL when it is available */
    int ncpus;           /* online logical CPUs */
    double eff_cpus;     /* CPUs this process may use: affinity mask or cgroup quota */
    bool has_quota;      /* eff_cpus comes from a cgroup CPU quota */
    unsigned line;       /* cache line size in bytes */
    bool has_split;      /* demand reads and prefetch fills are counted separately */
    bool has_rw;         /* DRAM reads and writes are counted separately */
    bool rw_total_only;  /* the platform splits reads and writes, but this chip gives only their sum */
    bool rw_system_only; /* reads and writes are split for the whole system, not for each process */
    bool has_cfs;        /* cgroup CPU throttling is tracked */
    bool has_gpu;        /* per-process GPU time and CPU energy are reported */
    bool estimated;      /* per-process traffic is apportioned, not counted */
    double spec_gbs;     /* DRAM bandwidth of the chip's specification, 0 if unknown */
    double gpu_cap_gbs;  /* measured GPU streaming bandwidth, 0 if not measured */
    int nagents;         /* parts of the chip with their own DRAM traffic figures */
    const char *agent[MAX_AGENTS];
    int cpu_agents;      /* agent[0 .. cpu_agents-1] are CPU clusters */
    int gpu_agent;       /* index of the GPU in agent[], or -1 */
} dt_info_t;

typedef struct {
    double read, write; /* cache lines */
} dt_rw_t;

typedef struct {
    dt_counts_t sys;   /* everything counted in the monitored scope */
    dt_counts_t other; /* the part of sys that no listed process accounts for */
    bool has_other;
    double bw_sec;     /* seconds that sys, other and agent cover; 0: the sampling interval */
    dt_rw_t agent[MAX_AGENTS];
    double gpu_sec;    /* GPU time of all processes */
    double energy_j;   /* CPU energy of all processes */
    dt_proc_t **procs;
    int nprocs;
    bool fd_limited; /* the open file limit kept counters from being attached */
    bool cfs_ok;     /* d_periods and d_throttled are valid */
    long long d_periods, d_throttled;
} dt_snapshot_t;

/* ---- Collection backend ---- */

extern const char plat_name[];       /* "Linux", "macOS", "Windows" */
extern const bool plat_pid_children; /* -p also covers the children that the process starts */
bool plat_init(const dt_config_t *cfg, dt_info_t *info); /* prints why and returns false on failure */
void plat_scan(double now);                               /* process list and CPU time baseline */
bool plat_has_proc(int pid);
void plat_start_system(dt_info_t *info); /* system-wide counters, opened after calibration */
void plat_sample(double now, dt_snapshot_t *snap);
void plat_shutdown(void);

/* Peak bandwidth calibration */
unsigned long long plat_mem_available(void);
int plat_cal_cpus(int *cpus, int max); /* CPUs to pin calibration threads to, best first */
void plat_pin_thread(int cpu);
void plat_probe_begin(void);   /* count this process's DRAM fills, including threads started later */
double plat_probe_lines(double bytes); /* cache lines filled since the previous call, in which this process read bytes */
void plat_probe_end(void);

/* ---- System layer ---- */

double os_now(void); /* monotonic seconds */
void os_sleep(double sec);
void *os_alloc_pages(size_t size); /* page-aligned and taken from the OS, so freeing returns it at once */
void os_free_pages(void *p, size_t size);

typedef struct os_thread os_thread_t;
os_thread_t *os_thread_start(void (*fn)(void *), void *arg); /* NULL on failure */
void os_thread_join(os_thread_t *t);

/* A mutex paired with one condition variable. */
typedef struct os_monitor os_monitor_t;
os_monitor_t *os_monitor_new(void);
void os_monitor_free(os_monitor_t *m);
void os_monitor_lock(os_monitor_t *m);
void os_monitor_unlock(os_monitor_t *m);
void os_monitor_wait(os_monitor_t *m);
void os_monitor_broadcast(os_monitor_t *m);

/* Releases all waiting threads at once; calibration passes are timed between two waits. */
typedef struct os_barrier os_barrier_t;
os_barrier_t *os_barrier_new(int parties);
void os_barrier_free(os_barrier_t *b);
void os_barrier_wait(os_barrier_t *b);

bool os_isatty(int fd);  /* 0: stdin, 1: stdout, 2: stderr */
bool os_term_dumb(void); /* the terminal can do neither colors nor cursor addressing */
extern const char os_dumb_term[]; /* what os_term_dumb() checks, for --help and the warning */
int os_term_rows(void);
bool os_term_raw(bool on);       /* key-at-a-time input without echo; false if not possible */
int os_read_key(double timeout); /* 0 on timeout */

void os_signals_init(void);
bool os_stop_requested(void);
bool os_take_resize(void);
void os_catch_suspend(bool on);
bool os_take_suspend(void);
void os_suspend(void); /* stop until resumed, as Ctrl+Z normally does */

/* ---- Helpers ---- */

static inline void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) {
        fputs("dramtop: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static inline void copy_str(char *dst, size_t n, const char *src) {
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Process names are attacker-controlled; keep the terminal and JSON safe. */
static inline void sanitize(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f) *s = '?';
}

#endif
