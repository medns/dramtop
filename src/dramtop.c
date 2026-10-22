/*
 * dramtop - real-time DRAM bandwidth monitor
 *
 * Platform-independent part: command line, sampling loop, diagnosis, peak
 * calibration and the full-screen, line and JSON output. Counter values come
 * from the collection backend; terminal, signals, threads and timers go
 * through the system layer. Only standard C is used here.
 *
 * Build: make (see README.md)
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#include "dramtop.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>

#ifndef DRAMTOP_VERSION
#define DRAMTOP_VERSION "1.2.0"
#endif

#define RING_SIZE         64
#define JSON_TOP_PROCS    5

#define CAL_MAX_BYTES     (512ULL << 20)
#define CAL_MIN_BYTES     (64ULL << 20)
#define CAL_MAX_THREADS   256
#define CAL_PASSES        4
#define CAL_PASS_SEC      0.08
#define CAL_MAX_SWEEPS    32

#define RST     "\033[0m"
#define BOLD    "\033[1m"
#define CYAN    "\033[1;36m"
#define WHITE   "\033[1;37m"
#define YELLOW  "\033[1;33m"
#define GREEN   "\033[1;32m"
#define RED     "\033[1;31m"
#define BLUE    "\033[1;34m"
#define MAGENTA "\033[1;35m"
#define GRAY    "\033[90m"

#define RULE "=========================================================================================================="
#define DASH "----------------------------------------------------------------------------------------------------------"

static const struct {
    const char *label, *color;
} STATE[] = {
    [ST_SAMPLING]   = {"SAMPLING...",                      YELLOW},
    [ST_IDLE]       = {"IDLE",                             GRAY},
    [ST_MEM_BW]     = {"MEMORY-BOUND (DRAM BW Saturated)", GREEN},
    [ST_MEM_LAT]    = {"MEMORY-BOUND (DRAM Latency)",      GREEN},
    [ST_CONTENTION] = {"LOCK-CONTENTION / CFS-THROTTLED",  RED},
    [ST_COMPUTE]    = {"COMPUTE-BOUND (Cache-Resident)",   BLUE},
    [ST_GPU]        = {"COMPUTE-BOUND (GPU)",              BLUE},
    [ST_ACTIVE]     = {"ACTIVE (Balanced Workload)",       YELLOW},
    [ST_OTHER]      = {"KERNEL / EPHEMERAL",               CYAN},
    [ST_NOACCESS]   = {"NO PMU ACCESS",                    GRAY},
};

typedef struct {
    char time[9];
    double ts;
    metrics_t m;
    double agent_rd[MAX_AGENTS], agent_wr[MAX_AGENTS]; /* GB/s */
    char top[NAME_LEN];
    int top_pid;
    bool cfs_ok;
    long long d_periods, d_throttled;
} sample_t;

typedef struct {
    int filter_pid;
    const char *filter_name;
    double interval;
    long max_count;
    double ceiling;
    bool batch, json, no_color;
} opts_t;

enum { VIEW_SPLIT, VIEW_PROCS, VIEW_TIMELINE, VIEW_COUNT };
static const char *const VIEW_NAME[VIEW_COUNT] = {"Split", "Top Processes", "Timeline"};

static struct {
    opts_t o;
    dt_info_t info;
    dt_snapshot_t snap;
    char scope[96];
    dt_proc_t other;
    dt_proc_t **view;
    int nview, view_cap;
    double scan_ts;
    double peak, max_bw, sum_bw;
    double cpu_cap; /* measured CPU read bandwidth when the spec sets the peak */
    long nsamples;
} g;

static bool g_tui, g_color, g_quit;

static const char *C(const char *esc) { return g_color ? esc : ""; }

static bool stopping(void) { return g_quit || os_stop_requested(); }

static const char *label_of(state_t s) {
    if (s == ST_CONTENTION && !g.info.has_cfs) return "LOCK-CONTENTION";
    return STATE[s].label;
}

/* ------------------------------------------------------------------ */
/* Peak bandwidth calibration                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    os_monitor_t *mon;
    bool go;
    os_barrier_t *bar;
    int sweeps; /* set by the main thread before each pass is released */
} cal_sync_t;

typedef struct {
    cal_sync_t *sync;
    uint8_t *base;
    size_t len;
    unsigned stride;
    int cpu;
    uint64_t sink;
} cal_job_t;

static void cal_worker(void *arg) {
    cal_job_t *j = arg;
    os_monitor_lock(j->sync->mon);
    while (!j->sync->go) os_monitor_wait(j->sync->mon);
    os_monitor_unlock(j->sync->mon);
    if (!j->sync->bar) return;

    if (j->cpu >= 0) plat_pin_thread(j->cpu);
    memset(j->base, 0x5a, j->len); /* first touch puts the pages on this thread's NUMA node */
    os_barrier_wait(j->sync->bar);
    size_t step = j->stride / sizeof(uint64_t), n = j->len / sizeof(uint64_t);
    for (int pass = 0; pass < CAL_PASSES; pass++) {
        os_barrier_wait(j->sync->bar);
        uint64_t s0 = 0, s1 = 0;
        for (int k = 0; k < j->sync->sweeps; k++) {
            /* One load per cache line moves the whole line from DRAM. */
            const volatile uint64_t *p = (const volatile uint64_t *)j->base;
            size_t i = 0;
            for (; i + step < n; i += 2 * step) {
                s0 += p[i];
                s1 += p[i + step];
            }
            for (; i < n; i += step) s0 += p[i];
        }
        j->sink += s0 + s1;
        os_barrier_wait(j->sync->bar);
    }
}

static double calibrate(void) {
    unsigned long long bytes = plat_mem_available() / 4;
    if (bytes > CAL_MAX_BYTES) bytes = CAL_MAX_BYTES;
    int nt = (int)g.info.eff_cpus, pins[CAL_MAX_THREADS];
    if (nt < 1) nt = 1;
    if (nt > CAL_MAX_THREADS) nt = CAL_MAX_THREADS;
    int npins = plat_cal_cpus(pins, nt);
    size_t chunk = (size_t)(bytes / (unsigned long long)nt) & ~(size_t)4095;
    if (chunk < 4096 || (unsigned long long)chunk * (unsigned long long)nt < CAL_MIN_BYTES) return 0.0;

    size_t total = chunk * (size_t)nt;
    uint8_t *buf = os_alloc_pages(total);
    if (!buf) return 0.0;
    cal_sync_t sync = {.mon = os_monitor_new(), .sweeps = 1};
    if (!sync.mon) {
        os_free_pages(buf, total);
        return 0.0;
    }

    /* Counting starts before the workers exist, so their fills are included. */
    plat_probe_begin();
    cal_job_t *jobs = xrealloc(NULL, (size_t)nt * sizeof *jobs);
    os_thread_t **th = xrealloc(NULL, (size_t)nt * sizeof *th);
    int started = 0;
    for (; started < nt; started++) {
        jobs[started] = (cal_job_t){&sync, buf + (size_t)started * chunk, chunk, g.info.line,
                                    started < npins ? pins[started] : -1, 0};
        if (!(th[started] = os_thread_start(cal_worker, &jobs[started]))) break;
    }

    sync.bar = started > 0 ? os_barrier_new(started + 1) : NULL;
    os_monitor_lock(sync.mon);
    sync.go = true;
    os_monitor_broadcast(sync.mon);
    os_monitor_unlock(sync.mon);

    double best = 0.0;
    if (sync.bar) {
        os_barrier_wait(sync.bar);
        plat_probe_lines(); /* drop the first-touch traffic */
        for (int pass = 0; pass < CAL_PASSES; pass++) {
            double t0 = os_now();
            os_barrier_wait(sync.bar);
            os_barrier_wait(sync.bar);
            double dt = os_now() - t0;
            double lines = plat_probe_lines();
            if (dt <= 0) continue;
            if (pass == 0) {
                /* Warm-up pass: size the timed passes so short scheduler hiccups do not dominate. */
                int sweeps = (int)(CAL_PASS_SEC / dt + 0.5);
                sync.sweeps = sweeps < 1 ? 1 : sweeps > CAL_MAX_SWEEPS ? CAL_MAX_SWEEPS : sweeps;
                continue;
            }
            double gbs = (double)chunk * started * sync.sweeps / dt / 1e9;
            double pmu = lines * g.info.line / dt / 1e9;
            if (pmu > gbs) gbs = pmu;
            if (gbs > best) best = gbs;
        }
    }
    for (int i = 0; i < started; i++) os_thread_join(th[i]);
    plat_probe_end();
    os_barrier_free(sync.bar);
    os_monitor_free(sync.mon);
    free(jobs);
    free(th);
    os_free_pages(buf, total);
    return best;
}

/* ------------------------------------------------------------------ */
/* Sampling and diagnosis                                              */
/* ------------------------------------------------------------------ */

static void compute_rates(metrics_t *m, const dt_counts_t *c, double dt) {
    double scale = (double)g.info.line / dt / 1e9;
    m->total_gbs = c->total * scale;
    m->demand_gbs = c->demand * scale;
    m->prefetch_gbs = c->prefetch * scale;
    m->read_gbs = c->read * scale;
    m->write_gbs = c->write * scale;
    m->ipc = c->cycles > 0 ? c->insns / c->cycles : 0.0;
    /* With GPU and other agents in the total, only the CPU clusters' reads relate to instructions. */
    double misses = g.info.cpu_agents > 0 ? c->cpu_read : c->total;
    m->mpki = c->insns > 0 ? misses * 1000.0 / c->insns : 0.0;
    m->has_bw = true;
}

/* Traffic of the CPU clusters and of the GPU in a sample, in GB/s. */
static void agent_split(const sample_t *s, double *cpu, double *gpu) {
    *cpu = *gpu = 0.0;
    for (int a = 0; a < g.info.cpu_agents && a < g.info.nagents; a++) *cpu += s->agent_rd[a] + s->agent_wr[a];
    if (g.info.gpu_agent >= 0 && g.info.gpu_agent < g.info.nagents)
        *gpu = s->agent_rd[g.info.gpu_agent] + s->agent_wr[g.info.gpu_agent];
}

static double util_of(double gbs) {
    double u = g.peak > 0 ? gbs / g.peak * 100.0 : 0.0;
    return u > 100.0 ? 100.0 : u;
}

/* Roofline-style heuristics on DRAM utilization, CPU time, IPC and DRAM MPKI.
 * "Busy" means at least about one core of CPU time. */
static state_t diagnose(const metrics_t *m, double throttle_ratio) {
    if (!m->has_bw) return m->cpu_pct >= 5.0 ? ST_SAMPLING : ST_IDLE;
    if (m->total_gbs < 0.25 && m->cpu_pct < 5.0 && m->gpu_pct < 5.0) return ST_IDLE;
    if (m->util_pct >= 42.0) return ST_MEM_BW;
    if (throttle_ratio >= 0.25) return ST_CONTENTION;
    if (m->gpu_pct >= 50.0) return ST_GPU;
    bool busy = m->cpu_pct >= 90.0;
    bool dram_stalls = m->mpki >= 10.0 || (m->mpki >= 3.0 && m->ipc > 0 && m->ipc < 0.5);
    if (busy && dram_stalls) return ST_MEM_LAT;
    /* Low IPC without DRAM misses while the CPU budget is exhausted: spinning or oversubscription. */
    if (m->cpu_pct >= 90.0 * g.info.eff_cpus && m->ipc > 0 && m->ipc < 0.3) return ST_CONTENTION;
    return busy ? ST_COMPUTE : ST_ACTIVE;
}

static int cmp_view(const void *a, const void *b) {
    const dt_proc_t *x = *(dt_proc_t *const *)a, *y = *(dt_proc_t *const *)b;
    long bx = (long)(x->m.total_gbs * 100), by = (long)(y->m.total_gbs * 100);
    if (bx != by) return bx < by ? 1 : -1;
    long cx = (long)x->m.cpu_pct, cy = (long)y->m.cpu_pct;
    if (cx != cy) return cx < cy ? 1 : -1;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static void sample(sample_t *s, bool discard) {
    double now = os_now(), dt = now - g.scan_ts;
    if (dt < 1e-3) dt = 1e-3;
    plat_sample(now, &g.snap);
    g.scan_ts = now;

    double cpu_total = 0.0;
    for (int i = 0; i < g.snap.nprocs; i++) {
        dt_proc_t *p = g.snap.procs[i];
        memset(&p->m, 0, sizeof p->m);
        p->m.cpu_pct = p->cpu_sec / dt * 100.0;
        p->m.gpu_pct = p->gpu_sec / dt * 100.0;
        p->m.cpu_power_w = p->energy_j / dt;
        cpu_total += p->m.cpu_pct;
        if (p->has_bw) compute_rates(&p->m, &p->c, p->bw_sec);
        else if (p->has_ipc && p->c.cycles > 0) p->m.ipc = p->c.insns / p->c.cycles;
    }

    memset(s, 0, sizeof *s);
    double bw_dt = g.snap.bw_sec > 0 ? g.snap.bw_sec : dt;
    compute_rates(&s->m, &g.snap.sys, bw_dt);
    s->m.cpu_pct = cpu_total;
    s->m.gpu_pct = g.snap.gpu_sec / dt * 100.0;
    if (s->m.gpu_pct > 100.0) s->m.gpu_pct = 100.0; /* overlapping GPU work of several processes */
    s->m.cpu_power_w = g.snap.energy_j / dt;
    for (int a = 0; a < g.info.nagents; a++) {
        s->agent_rd[a] = g.snap.agent[a].read * (double)g.info.line / bw_dt / 1e9;
        s->agent_wr[a] = g.snap.agent[a].write * (double)g.info.line / bw_dt / 1e9;
    }
    if (!discard && s->m.total_gbs > g.peak) g.peak = s->m.total_gbs;
    if (!discard) { /* probes run on a busy machine come out low */
        double cpu, gpu;
        agent_split(s, &cpu, &gpu);
        if (g.cpu_cap > 0 && cpu > g.cpu_cap) g.cpu_cap = cpu;
        if (g.info.gpu_cap_gbs > 0 && gpu > g.info.gpu_cap_gbs) g.info.gpu_cap_gbs = gpu;
    }

    double throttle = 0.0;
    if (g.snap.cfs_ok) {
        s->cfs_ok = true;
        s->d_periods = g.snap.d_periods;
        s->d_throttled = g.snap.d_throttled;
        if (s->d_periods > 0) throttle = (double)s->d_throttled / (double)s->d_periods;
    }

    s->m.util_pct = util_of(s->m.total_gbs);
    s->m.state = diagnose(&s->m, throttle);

    if (g.view_cap < g.snap.nprocs + 1) {
        g.view_cap = g.snap.nprocs + 64;
        g.view = xrealloc(g.view, (size_t)g.view_cap * sizeof *g.view);
    }
    g.nview = 0;
    for (int i = 0; i < g.snap.nprocs; i++) {
        dt_proc_t *p = g.snap.procs[i];
        p->m.util_pct = util_of(p->m.total_gbs);
        /* CFS throttling applies to the whole cgroup, so it also stalls every busy member. */
        p->m.state = p->denied ? ST_NOACCESS : diagnose(&p->m, p->m.cpu_pct >= 90.0 ? throttle : 0.0);
        g.view[g.nview++] = p;
    }
    if (g.snap.has_other) {
        memset(&g.other, 0, sizeof g.other);
        snprintf(g.other.name, sizeof g.other.name, "[kernel/ephem]");
        compute_rates(&g.other.m, &g.snap.other, bw_dt);
        g.other.m.util_pct = util_of(g.other.m.total_gbs);
        g.other.m.state = ST_OTHER;
        if (g.other.m.total_gbs >= 0.05) g.view[g.nview++] = &g.other;
    }
    qsort(g.view, (size_t)g.nview, sizeof *g.view, cmp_view);

    bool any_bw = false;
    for (int i = 0; i < g.nview && !any_bw; i++) any_bw = g.view[i]->m.has_bw;
    int top_agent = -1;
    for (int a = 0; !any_bw && a < g.info.nagents; a++)
        if (top_agent < 0 || s->agent_rd[a] + s->agent_wr[a] > s->agent_rd[top_agent] + s->agent_wr[top_agent])
            top_agent = a;
    if (top_agent >= 0) { /* without per-process traffic, name the busiest part of the chip instead */
        snprintf(s->top, sizeof s->top, "%s",
                 s->agent_rd[top_agent] + s->agent_wr[top_agent] >= 0.05 ? g.info.agent[top_agent] : "idle");
    } else if (g.nview && (g.view[0]->m.total_gbs >= 0.05 || g.view[0]->m.cpu_pct >= 1.0)) {
        snprintf(s->top, sizeof s->top, "%s", g.view[0]->name);
        s->top_pid = g.view[0]->pid;
    } else {
        snprintf(s->top, sizeof s->top, "idle");
    }
    struct timespec ts = {0};
    timespec_get(&ts, TIME_UTC);
    s->ts = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    time_t t = ts.tv_sec;
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(s->time, sizeof s->time, "%H:%M:%S", tm)) snprintf(s->time, sizeof s->time, "--:--:--");
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static const char *bar(char *buf, size_t n, double pct, int width) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fill = (int)(pct * width / 100.0 + 0.5);
    int k = snprintf(buf, n, "[%s", C(pct >= 70 ? RED : pct >= 35 ? YELLOW : GREEN));
    for (int i = 0; i < width && k < (int)n - 8; i++) buf[k++] = i < fill ? '#' : '-';
    snprintf(buf + k, n - (size_t)k, "%s]", C(RST));
    return buf;
}

static const char *fmt_bw(char *buf, size_t n, bool ok, double v, int width) {
    if (ok) snprintf(buf, n, "%*.2f GB/s", width, v);
    else snprintf(buf, n, "%*s", width + 5, "-");
    return buf;
}

static const char *fmt_ipc(char *buf, size_t n, double v) {
    if (v > 0) snprintf(buf, n, "%4.2f", v);
    else snprintf(buf, n, "%4s", "-");
    return buf;
}

static void cpu_budget(char *buf, size_t n) {
    if (g.info.has_quota) snprintf(buf, n, "Quota: %.1f cores / %d logical", g.info.eff_cpus, g.info.ncpus);
    else if ((int)g.info.eff_cpus < g.info.ncpus)
        snprintf(buf, n, "%d of %d logical CPUs", (int)g.info.eff_cpus, g.info.ncpus);
    else snprintf(buf, n, "%d logical CPUs", g.info.ncpus);
}

static int g_rows, g_line;

DT_PRINTF(1, 2) static void out_line(const char *fmt, ...) {
    if (g_line >= g_rows) return;
    if (g_line > 0) fputc('\n', stdout);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputs(g_color ? RST "\033[K" : "\033[K", stdout);
    g_line++;
}

/* Estimated per-process traffic is marked '~' or '?' when other processes shared its CPU cluster or the GPU. */
static char conf_mark(const dt_proc_t *p) {
    if (!g.info.estimated || p == &g.other || !p->m.has_bw || p->confidence >= 0.75) return ' ';
    return p->confidence >= 0.4 ? '~' : '?';
}

static void proc_header(void) {
    out_line("%s================================== [ TOP PROCESSES BY DRAM BANDWIDTH ] ===================================", C(CYAN));
    if (g.info.has_rw)
        out_line(" %s%-7s | %-15s | %-4s | %-12s | %-10s | %-11s | %-17s | %-6s | %-5s | %-4s | %s%s", C(WHITE), "PID",
                 "PROCESS", "THR", g.info.estimated ? "DRAM (EST.)" : "DRAM TOTAL", "READ", "WRITE", "DRAM UTIL%", "CPU%",
                 "GPU%", "IPC", "BOTTLENECK", C(RST));
    else
        out_line(" %s%-7s | %-15s | %-4s | %-11s | %-10s | %-11s | %-17s | %-6s | %-4s | %s%s", C(WHITE), "PID", "PROCESS",
                 "THR", "DRAM TOTAL", "DEMAND RD", "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "BOTTLENECK", C(RST));
    out_line("%s", DASH);
}

static void proc_row(const dt_proc_t *p) {
    char pid[16] = "-", thr[16] = "-", tot[24], dem[24], pf[24], ipc[16], ub[96], up[16];
    bool other = p == &g.other, split = p->m.has_bw && g.info.has_split;
    if (!other) {
        snprintf(pid, sizeof pid, "%d", p->pid);
        snprintf(thr, sizeof thr, "%d", p->threads);
    }
    if (p->m.has_bw) snprintf(up, sizeof up, "%5.1f%%", p->m.util_pct);
    else snprintf(up, sizeof up, "%6s", "-");
    if (g.info.has_rw) {
        out_line(" %-7s | %-15.15s | %4s | %s%c | %s | %s | %s %s | %5.0f%% | %4.0f%% | %s | %s%s%s", pid, p->name, thr,
                 fmt_bw(tot, sizeof tot, p->m.has_bw, p->m.total_gbs, 6), conf_mark(p),
                 fmt_bw(dem, sizeof dem, p->m.has_bw, p->m.read_gbs, 5),
                 fmt_bw(pf, sizeof pf, p->m.has_bw, p->m.write_gbs, 6), bar(ub, sizeof ub, p->m.util_pct, 8), up,
                 p->m.cpu_pct, p->m.gpu_pct, fmt_ipc(ipc, sizeof ipc, p->m.ipc), C(STATE[p->m.state].color),
                 label_of(p->m.state), C(RST));
        return;
    }
    out_line(" %-7s | %-15.15s | %4s | %s | %s | %s | %s %s | %5.0f%% | %s | %s%s%s", pid, p->name, thr,
             fmt_bw(tot, sizeof tot, p->m.has_bw, p->m.total_gbs, 6), fmt_bw(dem, sizeof dem, split, p->m.demand_gbs, 5),
             fmt_bw(pf, sizeof pf, split, p->m.prefetch_gbs, 6), bar(ub, sizeof ub, p->m.util_pct, 8), up,
             p->m.cpu_pct, fmt_ipc(ipc, sizeof ipc, p->m.ipc), C(STATE[p->m.state].color), label_of(p->m.state),
             C(RST));
}

/* The timeline and the line output share their columns; the third, fourth and seventh depend on the platform. */
static const char *const *timeline_titles(void) {
    static const char *t[3];
    t[0] = g.info.has_rw ? "READ" : "DEMAND RD";
    t[1] = g.info.has_rw ? "WRITE" : "HW PREFETCH";
    t[2] = g.info.has_gpu ? "GPU%" : "IPC";
    return t;
}

static void timeline_cells(const sample_t *s, char c3[24], char c4[24], char c7[16]) {
    if (g.info.has_rw) {
        fmt_bw(c3, 24, true, s->m.read_gbs, 5);
        fmt_bw(c4, 24, true, s->m.write_gbs, 6);
    } else {
        fmt_bw(c3, 24, g.info.has_split, s->m.demand_gbs, 5);
        fmt_bw(c4, 24, g.info.has_split, s->m.prefetch_gbs, 6);
    }
    if (g.info.has_gpu) snprintf(c7, 16, "%3.0f%%", s->m.gpu_pct);
    else fmt_ipc(c7, 16, s->m.ipc);
}

static void timeline_header(void) {
    const char *const *t = timeline_titles();
    out_line("%s=================================== [ REAL-TIME ROLLING TIMELINE ] =======================================", C(CYAN));
    out_line(" %s%-8s | %-11s | %-10s | %-11s | %-17s | %-6s | %-4s | %-13s | %s%s", C(WHITE), "TIME", "DRAM TOTAL",
             t[0], t[1], "DRAM UTIL%", "CPU%", t[2], "TOP PROCESS", "DIAGNOSIS", C(RST));
    out_line("%s", DASH);
}

static void timeline_row(const sample_t *s) {
    char tot[24], c3[24], c4[24], c7[16], ub[96];
    timeline_cells(s, c3, c4, c7);
    out_line(" %-8s | %s | %s | %s | %s %5.1f%% | %5.0f%% | %s | %-13.13s | %s%s%s", s->time,
             fmt_bw(tot, sizeof tot, true, s->m.total_gbs, 6), c3, c4, bar(ub, sizeof ub, s->m.util_pct, 8),
             s->m.util_pct, s->m.cpu_pct, c7, s->top, C(STATE[s->m.state].color), label_of(s->m.state), C(RST));
}

static void render(const sample_t *ring, int count, int head, int view, bool paused) {
    static const sample_t placeholder = {.time = "--:--:--", .top = "-", .m = {.state = ST_SAMPLING}};
    const sample_t *cur = count ? &ring[(head + RING_SIZE - 1) % RING_SIZE] : &placeholder;
    bool filtered = g.o.filter_pid > 0 || g.o.filter_name;
    char budget[64], b1[96], b2[96], ipc[16], thr[48];

    g_rows = os_term_rows();
    g_line = 0;
    fputs("\033[H", stdout);

    cpu_budget(budget, sizeof budget);
    if (cur->cfs_ok) snprintf(thr, sizeof thr, "%lld/%lld periods", cur->d_throttled, cur->d_periods);
    else snprintf(thr, sizeof thr, "n/a");
    out_line("%s%s", C(CYAN), RULE);
    out_line(" %sdramtop v%s%s  |  CPU: %s%s%s (%s)", C(WHITE), DRAMTOP_VERSION, C(RST), C(YELLOW), g.info.cpu_name,
             C(RST), budget);
    out_line(" PMU Backend   : %s", g.info.backend);
    out_line(" Monitor Scope : %s%s%s (%d procs)%s  |  Refresh: %.2fs%s  |  View: %s%s%s", C(GREEN), g.scope, C(RST),
             g.snap.nprocs, g.snap.fd_limited ? " [fd limit reached]" : "", g.o.interval, paused ? " [PAUSED]" : "",
             C(MAGENTA), VIEW_NAME[view], C(RST));
    out_line("%s%s", C(CYAN), DASH);
    out_line(" %s%s%s: %s %s%6.2f GB/s%s (%5.1f%% of %.1f GB/s cap)", C(BOLD),
             filtered ? "Filter DRAM BW" : "System DRAM BW", C(RST), bar(b1, sizeof b1, cur->m.util_pct, 30), C(CYAN),
             cur->m.total_gbs, C(RST), cur->m.util_pct, g.peak);
    double avg = g.nsamples ? g.sum_bw / (double)g.nsamples : 0.0;
    if (g.info.has_split)
        out_line("   ├─ Demand Rd: %s%6.2f GB/s%s   ├─ HW Prefetch: %s%6.2f GB/s%s   ├─ Session Max/Avg: %s%.2f%s / %s%.2f GB/s%s",
                 C(WHITE), cur->m.demand_gbs, C(RST), C(WHITE), cur->m.prefetch_gbs, C(RST), C(YELLOW), g.max_bw, C(RST),
                 C(GREEN), avg, C(RST));
    else if (g.info.has_rw)
        out_line("   ├─ Read: %s%6.2f GB/s%s   ├─ Write: %s%6.2f GB/s%s   ├─ Session Max/Avg: %s%.2f%s / %s%.2f GB/s%s",
                 C(WHITE), cur->m.read_gbs, C(RST), C(WHITE), cur->m.write_gbs, C(RST), C(YELLOW), g.max_bw, C(RST),
                 C(GREEN), avg, C(RST));
    else
        out_line("   ├─ Demand/Prefetch split: n/a on this CPU   ├─ Session Max/Avg: %s%.2f%s / %s%.2f GB/s%s",
                 C(YELLOW), g.max_bw, C(RST), C(GREEN), avg, C(RST));
    if (g.info.nagents > 0) {
        char line[320], caps[96] = "";
        int k = 0;
        for (int a = 0; a < g.info.nagents && k < (int)sizeof line - 48; a++)
            k += snprintf(line + k, sizeof line - (size_t)k, "%s%s %s%.1f%s", a ? "  " : "", g.info.agent[a], C(WHITE),
                          cur->agent_rd[a] + cur->agent_wr[a], C(RST));
        double cpu, gpu;
        agent_split(cur, &cpu, &gpu);
        if (g.cpu_cap > 0 && g.info.gpu_cap_gbs > 0)
            snprintf(caps, sizeof caps, "  |  CPU %.0f%%, GPU %.0f%% of measured caps", cpu / g.cpu_cap * 100.0,
                     gpu / g.info.gpu_cap_gbs * 100.0);
        out_line("   ├─ %s  (GB/s)%s", line, caps);
    }
    if (g.info.has_gpu)
        out_line(" %s%s%s: %s %s%6.0f%%%s     (IPC %s | MPKI %.1f | GPU %.0f%% | CPU Power %.1f W)", C(BOLD),
                 "CPU Util      ", C(RST), bar(b2, sizeof b2, cur->m.cpu_pct / g.info.eff_cpus, 30), C(WHITE),
                 cur->m.cpu_pct, C(RST), fmt_ipc(ipc, sizeof ipc, cur->m.ipc), cur->m.mpki, cur->m.gpu_pct,
                 cur->m.cpu_power_w);
    else
        out_line(" %s%s%s: %s %s%6.0f%%%s     (IPC %s | MPKI %.1f | Throttled %s)", C(BOLD),
                 g.info.has_quota ? "CPU Quota Util" : "CPU Util      ", C(RST),
                 bar(b2, sizeof b2, cur->m.cpu_pct / g.info.eff_cpus, 30), C(WHITE), cur->m.cpu_pct, C(RST),
                 fmt_ipc(ipc, sizeof ipc, cur->m.ipc), cur->m.mpki, thr);
    out_line(" %sLive Diagnosis%s: %s%s%s  (Top Bandwidth Consumer: %s%s%s)", C(BOLD), C(RST),
             C(STATE[cur->m.state].color), label_of(cur->m.state), C(RST), C(YELLOW), cur->top, C(RST));

    int body = g_rows - 11 - (g.info.nagents > 0), np = 0, nh = 0;
    if (view == VIEW_SPLIT) {
        int data = body - 6 < 2 ? 2 : body - 6;
        np = (data + 1) / 2;
        nh = data - np;
        if (g.nview < np) {
            nh += np - g.nview;
            np = g.nview;
        }
    } else if (view == VIEW_PROCS) {
        np = body - 3;
    } else {
        nh = body - 3;
    }
    if (np > g.nview) np = g.nview;
    if (nh > count) nh = count;
    if (view != VIEW_TIMELINE) {
        proc_header();
        for (int i = 0; i < np; i++) proc_row(g.view[i]);
    }
    if (view != VIEW_PROCS) {
        timeline_header();
        for (int i = 0; i < nh; i++) timeline_row(&ring[(head - nh + i + RING_SIZE) % RING_SIZE]);
    }
    out_line("%s%s", C(CYAN), DASH);
    out_line(" Keys: [%sq%s] Quit  [%st%s] View  [%sSpace%s] Pause  [%sr%s] Reset  [%s+%s/%s-%s] Interval  (-b batch, -J json)",
             C(YELLOW), C(RST), C(YELLOW), C(RST), C(YELLOW), C(RST), C(YELLOW), C(RST), C(YELLOW), C(RST), C(YELLOW),
             C(RST));
    fputs("\033[J", stdout);
    fflush(stdout);
}

static void print_batch_header(void) {
    char budget[64];
    cpu_budget(budget, sizeof budget);
    printf("%s\n", RULE);
    printf("dramtop v%s | CPU: %s (%s)\n", DRAMTOP_VERSION, g.info.cpu_name, budget);
    printf("PMU Backend   : %s\n", g.info.backend);
    printf("Monitor Scope : %s (%d procs) | DRAM cap: %.1f GB/s (%s)", g.scope, g.snap.nprocs, g.peak,
           g.o.ceiling > 0 ? "manual" : g.info.spec_gbs > 0 ? "spec" : "calibrated");
    if (g.cpu_cap > 0 && g.info.gpu_cap_gbs > 0)
        printf(" | measured CPU %.1f, GPU %.1f GB/s", g.cpu_cap, g.info.gpu_cap_gbs);
    printf("\n");
    printf("%s\n", RULE);
    const char *const *t = timeline_titles();
    printf("%-8s | %-11s | %-10s | %-11s | %-17s | %-6s | %-4s | %-13s | %s\n", "TIME", "DRAM TOTAL", t[0], t[1],
           "DRAM UTIL%", "CPU%", t[2], "TOP PROCESS", "DIAGNOSIS");
    printf("%s\n", DASH);
    fflush(stdout);
}

static void print_batch(const sample_t *s) {
    char tot[24], c3[24], c4[24], c7[16], ub[96];
    timeline_cells(s, c3, c4, c7);
    printf("%-8s | %s | %s | %s | %s %5.1f%% | %5.0f%% | %s | %-13.13s | %s%s%s\n", s->time,
           fmt_bw(tot, sizeof tot, true, s->m.total_gbs, 6), c3, c4, bar(ub, sizeof ub, s->m.util_pct, 8),
           s->m.util_pct, s->m.cpu_pct, c7, s->top, C(STATE[s->m.state].color), label_of(s->m.state), C(RST));
    fflush(stdout);
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void json_bw(const char *key, bool ok, double v) {
    if (ok) printf("\"%s\":%.3f,", key, v);
    else printf("\"%s\":null,", key);
}

static void print_json(const sample_t *s) {
    printf("{\"time\":\"%s\",\"timestamp\":%.3f,\"mode\":\"%s\",\"system\":{\"total_bw_gbs\":%.3f,", s->time, s->ts,
           g.info.mode, s->m.total_gbs);
    json_bw("demand_bw_gbs", g.info.has_split, s->m.demand_gbs);
    json_bw("prefetch_bw_gbs", g.info.has_split, s->m.prefetch_gbs);
    if (g.info.has_rw) {
        json_bw("read_bw_gbs", true, s->m.read_gbs);
        json_bw("write_bw_gbs", true, s->m.write_gbs);
    }
    if (g.info.nagents > 0) {
        printf("\"agents\":{");
        for (int a = 0; a < g.info.nagents; a++)
            printf("%s\"%s\":{\"read_bw_gbs\":%.3f,\"write_bw_gbs\":%.3f}", a ? "," : "", g.info.agent[a], s->agent_rd[a],
                   s->agent_wr[a]);
        printf("},");
    }
    printf("\"peak_cap_gbs\":%.2f,", g.peak);
    if (g.cpu_cap > 0 && g.info.gpu_cap_gbs > 0)
        printf("\"measured_caps\":{\"cpu_gbs\":%.2f,\"gpu_gbs\":%.2f},", g.cpu_cap, g.info.gpu_cap_gbs);
    printf("\"util_pct\":%.2f,\"cpu_pct\":%.1f,", s->m.util_pct, s->m.cpu_pct);
    if (g.info.has_gpu) printf("\"gpu_pct\":%.1f,\"cpu_power_w\":%.2f,", s->m.gpu_pct, s->m.cpu_power_w);
    printf("\"ipc\":%.3f,\"mpki\":%.2f,", s->m.ipc, s->m.mpki);
    if (s->cfs_ok) printf("\"throttled\":%lld,\"periods\":%lld,", s->d_throttled, s->d_periods);
    else printf("\"throttled\":null,\"periods\":null,");
    printf("\"top_process\":");
    json_str(stdout, s->top);
    printf(",\"top_pid\":%d,\"diagnosis\":\"%s\"},\"top_processes\":[", s->top_pid, label_of(s->m.state));
    int n = g.nview < JSON_TOP_PROCS ? g.nview : JSON_TOP_PROCS;
    for (int i = 0; i < n; i++) {
        const dt_proc_t *p = g.view[i];
        bool split = p->m.has_bw && g.info.has_split;
        printf("%s{\"pid\":%d,\"comm\":", i ? "," : "", p->pid);
        json_str(stdout, p->name);
        printf(",\"threads\":%d,", p->threads);
        json_bw("total_bw_gbs", p->m.has_bw, p->m.total_gbs);
        json_bw("demand_bw_gbs", split, p->m.demand_gbs);
        json_bw("prefetch_bw_gbs", split, p->m.prefetch_gbs);
        if (g.info.has_rw) {
            json_bw("read_bw_gbs", p->m.has_bw, p->m.read_gbs);
            json_bw("write_bw_gbs", p->m.has_bw, p->m.write_gbs);
        }
        printf("\"util_pct\":%.2f,\"cpu_pct\":%.1f,", p->m.util_pct, p->m.cpu_pct);
        if (g.info.has_gpu) printf("\"gpu_pct\":%.1f,\"cpu_power_w\":%.2f,", p->m.gpu_pct, p->m.cpu_power_w);
        printf("\"ipc\":%.3f,", p->m.ipc);
        if (g.info.estimated) {
            printf("\"estimated\":true,");
            json_bw("confidence", p->m.has_bw && p != &g.other, p->confidence);
        }
        printf("\"diagnosis\":\"%s\"}", label_of(p->m.state));
    }
    printf("]}\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    bool batch, color, warn_dumb;
    bool tell; /* progress messages on stderr */
} outmode_t;

static outmode_t output_mode(const opts_t *o, const char *no_color_env, bool term_dumb, bool tty_in, bool tty_out,
                             bool tty_err) {
    bool tty = tty_in && tty_out;
    outmode_t m;
    m.warn_dumb = tty && term_dumb && !o->batch;
    m.batch = o->batch || !tty || term_dumb;
    m.color = !o->json && !o->no_color && !term_dumb && tty_out && !(no_color_env && *no_color_env);
    m.tell = tty_err && !o->json && !term_dumb;
    return m;
}

static void tui_enter(void) {
    if (!os_term_raw(true)) return;
    /* Alternate screen, auto-wrap off (long rows are clipped, not wrapped), cursor hidden. */
    fputs("\033[?1049h\033[?7l\033[?25l\033[2J\033[H", stdout);
    fflush(stdout);
    g_tui = true;
}

static void tui_leave(void) {
    if (!g_tui) return;
    fputs(g_color ? RST "\033[?25h\033[?7h\033[?1049l" : "\033[?25h\033[?7h\033[?1049l", stdout);
    fflush(stdout);
    os_term_raw(false);
    g_tui = false;
}

static void suspend_self(void) {
    tui_leave();
    os_suspend();
    tui_enter();
}

static int wait_key(double sec) {
    if (sec < 0) sec = 0;
    if (!g_tui) {
        os_sleep(sec);
        return 0;
    }
    return os_read_key(sec);
}

static double dist(double a, double b) { return a > b ? a - b : b - a; }

static double step_interval(double cur, int dir) {
    static const double steps[] = {0.10, 0.25, 0.50, 1.00, 2.00, 5.00};
    const int n = (int)(sizeof steps / sizeof steps[0]);
    int best = 0;
    for (int i = 1; i < n; i++)
        if (dist(cur, steps[i]) < dist(cur, steps[best])) best = i;
    best += dir;
    return steps[best < 0 ? 0 : best >= n ? n - 1 : best];
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

static void usage(FILE *out) {
    fprintf(out,
            "dramtop v%s - real-time DRAM bandwidth monitor for %s\n\n"
            "Usage: dramtop [options]\n\n"
            "Options:\n"
            "  -p, --pid <PID>        Monitor a single process (and children it forks)\n"
            "  -P, --proc <NAME>      Monitor processes whose name contains NAME\n"
            "  -i, --interval <SEC>   Refresh interval in seconds (default: 1.0)\n"
            "  -n, --count <N>        Exit after N samples (default: run until stopped)\n"
            "  -c, --ceiling <GB/s>   DRAM bandwidth ceiling; skips the startup calibration\n"
            "  -b, --batch            Line-by-line output instead of the full-screen TUI\n"
            "  -J, --json             JSON Lines output, one object per sample\n"
            "      --no-color         Disable colors\n"
            "  -v, --version          Print version and exit\n"
            "  -h, --help             Show this help\n\n"
            "Colors are also disabled when NO_COLOR is set or output is not a terminal.\n"
            "If TERM is unset or \"dumb\", line output is used instead of the full-screen TUI.\n",
            DRAMTOP_VERSION, plat_name);
}

enum { ARGS_OK, ARGS_HELP, ARGS_VERSION, ARGS_ERROR };
enum { OPT_NO_COLOR = 256 };

#define SHORT_OPTS "p:P:i:n:c:bJvh"
#define TRY_HELP   "Try 'dramtop --help'.\n"

static const struct {
    const char *name;
    int key;
    bool arg;
} LONG_OPTS[] = {
    {"pid", 'p', true},      {"proc", 'P', true},   {"interval", 'i', true}, {"count", 'n', true},
    {"ceiling", 'c', true},  {"batch", 'b', false}, {"json", 'J', false},    {"no-color", OPT_NO_COLOR, false},
    {"version", 'v', false}, {"help", 'h', false},
};

DT_PRINTF(3, 4) static int arg_error(char *err, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return ARGS_ERROR;
}

/* Written so that NaN fails the range check too. */
static bool parse_double(const char *s, double lo, double hi, double *out) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || !(v >= lo && v <= hi)) return false;
    *out = v;
    return true;
}

static bool parse_long(const char *s, long lo, long hi, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static int bad_value(char *err, size_t n, const char *opt, const char *val) {
    return arg_error(err, n, "dramtop: invalid value for %s: '%s'\n" TRY_HELP, opt, val);
}

static int apply_opt(opts_t *o, int key, const char *val, char *err, size_t n) {
    long l;
    double d;
    switch (key) {
    case 'p':
        if (!parse_long(val, 1, 0x7fffffff, &l)) return bad_value(err, n, "--pid", val);
        o->filter_pid = (int)l;
        break;
    case 'P': o->filter_name = *val ? val : NULL; break;
    case 'i':
        if (!parse_double(val, 0.1, 3600.0, &d)) return bad_value(err, n, "--interval", val);
        o->interval = d;
        break;
    case 'n':
        if (!parse_long(val, 1, 0x7fffffff, &l)) return bad_value(err, n, "--count", val);
        o->max_count = l;
        break;
    case 'c':
        if (!parse_double(val, 0.1, 1e6, &d)) return bad_value(err, n, "--ceiling", val);
        o->ceiling = d;
        break;
    case 'b': o->batch = true; break;
    case 'J': o->json = o->batch = true; break;
    case OPT_NO_COLOR: o->no_color = true; break;
    case 'v': return ARGS_VERSION;
    case 'h': return ARGS_HELP;
    }
    return ARGS_OK;
}

/* An exact name wins; otherwise a unique prefix is accepted. -1: unknown, -2: ambiguous. */
static int find_long(const char *name, size_t len) {
    int found = -1;
    if (len == 0) return -1;
    for (int i = 0; i < (int)(sizeof LONG_OPTS / sizeof LONG_OPTS[0]); i++) {
        if (strncmp(LONG_OPTS[i].name, name, len)) continue;
        if (!LONG_OPTS[i].name[len]) return i;
        found = found == -1 ? i : -2;
    }
    return found;
}

/* Same conventions as getopt_long: options and operands may be mixed, "--" ends
 * the options, and stray operands are reported after all options are handled. */
static int parse_args(int argc, char **argv, opts_t *o, char *err, size_t n) {
    *o = (opts_t){.interval = 1.0};
    const char *extra = NULL;
    int r = ARGS_OK;
    for (int i = 1; i < argc && r == ARGS_OK; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            if (!extra && i + 1 < argc) extra = argv[i + 1];
            break;
        }
        if (a[0] != '-' || !a[1]) {
            if (!extra) extra = a;
            continue;
        }
        if (a[1] == '-') {
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t len = eq ? (size_t)(eq - name) : strlen(name);
            int k = find_long(name, len);
            if (k == -1) return arg_error(err, n, "dramtop: unrecognized option '--%.*s'\n" TRY_HELP, (int)len, name);
            if (k == -2) return arg_error(err, n, "dramtop: option '--%.*s' is ambiguous\n" TRY_HELP, (int)len, name);
            const char *val = NULL;
            if (LONG_OPTS[k].arg) {
                if (eq) val = eq + 1;
                else if (i + 1 < argc) val = argv[++i];
                else return arg_error(err, n, "dramtop: option '--%s' requires an argument\n" TRY_HELP, LONG_OPTS[k].name);
            } else if (eq) {
                return arg_error(err, n, "dramtop: option '--%s' doesn't allow an argument\n" TRY_HELP,
                                 LONG_OPTS[k].name);
            }
            r = apply_opt(o, LONG_OPTS[k].key, val, err, n);
            continue;
        }
        for (const char *c = a + 1; *c && r == ARGS_OK; c++) {
            const char *spec = *c == ':' ? NULL : strchr(SHORT_OPTS, *c);
            if (!spec) return arg_error(err, n, "dramtop: invalid option -- '%c'\n" TRY_HELP, *c);
            if (spec[1] != ':') {
                r = apply_opt(o, *c, NULL, err, n);
                continue;
            }
            const char *val = c[1] ? c + 1 : i + 1 < argc ? argv[++i] : NULL;
            if (!val) return arg_error(err, n, "dramtop: option requires an argument -- '%c'\n" TRY_HELP, *c);
            r = apply_opt(o, *c, val, err, n);
            break;
        }
    }
    if (r == ARGS_OK && extra) return arg_error(err, n, "dramtop: unexpected argument '%s'\n" TRY_HELP, extra);
    return r;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    char err[512];
    switch (parse_args(argc, argv, &g.o, err, sizeof err)) {
    case ARGS_HELP:
        usage(stdout);
        return 0;
    case ARGS_VERSION:
        printf("dramtop v%s\n", DRAMTOP_VERSION);
        return 0;
    case ARGS_ERROR:
        fputs(err, stderr);
        return 2;
    }
    outmode_t om = output_mode(&g.o, getenv("NO_COLOR"), os_term_dumb(), os_isatty(0), os_isatty(1), os_isatty(2));
    if (om.warn_dumb)
        fputs("dramtop: TERM is unset or \"dumb\", using line output (set TERM for the full-screen view)\n", stderr);
    g.o.batch = om.batch;
    g_color = om.color;

    os_signals_init();
    bool measure = g.o.ceiling <= 0;
    dt_config_t cfg = {g.o.filter_pid, g.o.filter_name, measure};
    if (!plat_init(&cfg, &g.info)) return 1;

    plat_scan(os_now()); /* CPU-time baseline; the calibration below doubles as the activity window */
    g.scan_ts = os_now();
    if (g.o.filter_pid > 0 && !plat_has_proc(g.o.filter_pid)) {
        fprintf(stderr, "dramtop: no such process: %d\n", g.o.filter_pid);
        return 1;
    }

    if (measure && om.tell) fputs("dramtop: measuring peak DRAM bandwidth (skip with -c <GB/s>)...", stderr);
    double measured = measure ? calibrate() : 0.0;
    plat_start_system(&g.info); /* runs the backend's own probes when asked to, before its system-wide counters */
    if (measure && om.tell) fputs("\r\033[K", stderr);
    if (!measure) {
        g.peak = g.o.ceiling;
    } else if (g.info.spec_gbs > 0) { /* the CPU alone falls well short of the chip's rated bandwidth */
        g.peak = g.info.spec_gbs;
        g.cpu_cap = measured;
    } else {
        g.peak = measured > 0 ? measured : 100.0;
    }
    if (g.o.filter_pid > 0) snprintf(g.scope, sizeof g.scope, "PID %d", g.o.filter_pid);
    else if (g.o.filter_name) snprintf(g.scope, sizeof g.scope, "Process filter '%.40s'", g.o.filter_name);
    else snprintf(g.scope, sizeof g.scope, "%s", g.info.scope);

    double since = os_now() - g.scan_ts;
    if (since < 0.25) wait_key(0.25 - since);
    sample_t ring[RING_SIZE], scratch;
    sample(&scratch, true); /* attach counters to processes that were active in the window */

    if (!g.o.batch) {
        tui_enter();
        if (g_tui) {
            atexit(tui_leave);
            os_catch_suspend(true);
        }
    }
    if (!g_tui) g.o.batch = true;
    if (g.o.batch && !g.o.json) print_batch_header();

    int head = 0, count = 0, view = VIEW_SPLIT;
    long printed = 0;
    bool paused = false, exited = false;
    if (g_tui) render(ring, count, head, view, paused);

    while (!stopping()) {
        double next = g.scan_ts + g.o.interval;
        for (;;) {
            if (stopping()) break;
            if (os_take_suspend()) {
                suspend_self();
                if (!paused) sample(&scratch, true);
                next = g.scan_ts + g.o.interval;
                if (g_tui) render(ring, count, head, view, paused);
                continue;
            }
            if (os_take_resize() && g_tui) render(ring, count, head, view, paused);
            double rem = paused ? 0.5 : next - os_now();
            if (!paused && rem <= 0) break;
            int key = wait_key(rem);
            if (!key) continue;
            switch (key) {
            case 'q': case 'Q':
                g_quit = true;
                break;
            case 't': case 'T': case '\t':
                view = (view + 1) % VIEW_COUNT;
                break;
            case 'r': case 'R':
                count = head = 0;
                g.max_bw = g.sum_bw = 0.0;
                g.nsamples = 0;
                break;
            case ' ': case 'p': case 'P':
                paused = !paused;
                if (!paused) { /* drop what accumulated while paused */
                    sample(&scratch, true);
                    next = g.scan_ts + g.o.interval;
                }
                break;
            case '+': case '=':
                g.o.interval = step_interval(g.o.interval, -1);
                next = g.scan_ts + g.o.interval;
                break;
            case '-': case '_':
                g.o.interval = step_interval(g.o.interval, +1);
                next = g.scan_ts + g.o.interval;
                break;
            default:
                continue;
            }
            if (g_tui && !stopping()) render(ring, count, head, view, paused);
        }
        if (stopping()) break;

        sample_t *s = &ring[head];
        sample(s, false);
        head = (head + 1) % RING_SIZE;
        if (count < RING_SIZE) count++;
        g.nsamples++;
        g.sum_bw += s->m.total_gbs;
        if (s->m.total_gbs > g.max_bw) g.max_bw = s->m.total_gbs;

        if (g_tui) render(ring, count, head, view, paused);
        else if (g.o.json) print_json(s);
        else print_batch(s);

        if (g.o.max_count > 0 && ++printed >= g.o.max_count) break;
        if (g.o.filter_pid > 0 && !plat_has_proc(g.o.filter_pid)) {
            exited = true;
            break;
        }
    }

    tui_leave();
    if (exited) fprintf(stderr, "dramtop: process %d exited\n", g.o.filter_pid);
    plat_shutdown();
    return 0;
}
