/*
 * dramtop - real-time DRAM bandwidth monitor for Linux
 *
 * Reads hardware PMU counters through perf_event_open(2) and reports
 * system-wide and per-process DRAM read traffic (cache-line fills from
 * memory, split into demand reads and hardware-prefetch fills), IPC,
 * DRAM misses per kilo-instruction and cgroup CFS throttling.
 *
 * Build: cc -O2 -pthread dramtop.c -o dramtop
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef DRAMTOP_VERSION
#define DRAMTOP_VERSION "1.1.0"
#endif

#ifndef PERF_FLAG_FD_CLOEXEC
#define PERF_FLAG_FD_CLOEXEC (1UL << 3)
#endif

#define MAX_EVENTS        6
#define RING_SIZE         64
#define JSON_TOP_PROCS    5
#define IDLE_DETACH_SEC   30.0
#define ATTACH_RETRY_SEC  30.0
#define PF_KTHREAD        0x00200000u
#define NAME_LEN          48

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

enum ev_kind { EV_DEMAND, EV_PREFETCH, EV_TOTAL, EV_CYCLES, EV_INSNS, EV_KINDS };

typedef struct {
    uint32_t type;
    uint64_t config;
    enum ev_kind kind;
} event_t;

typedef struct {
    uint64_t val, ena, run;
} snap_t;

/* One instance of every selected event, bound to a single thread or CPU. */
typedef struct {
    int fd[MAX_EVENTS];
    snap_t snap[MAX_EVENTS];
} counter_set_t;

typedef enum {
    ST_SAMPLING, ST_IDLE, ST_MEM_BW, ST_MEM_LAT, ST_CONTENTION,
    ST_COMPUTE, ST_ACTIVE, ST_OTHER, ST_NOACCESS
} state_t;

static const struct {
    const char *label, *color;
} STATE[] = {
    [ST_SAMPLING]   = {"SAMPLING...",                      YELLOW},
    [ST_IDLE]       = {"IDLE",                             GRAY},
    [ST_MEM_BW]     = {"MEMORY-BOUND (DRAM BW Saturated)", GREEN},
    [ST_MEM_LAT]    = {"MEMORY-BOUND (DRAM Latency)",      GREEN},
    [ST_CONTENTION] = {"LOCK-CONTENTION / CFS-THROTTLED",  RED},
    [ST_COMPUTE]    = {"COMPUTE-BOUND (Cache-Resident)",   BLUE},
    [ST_ACTIVE]     = {"ACTIVE (Balanced Workload)",       YELLOW},
    [ST_OTHER]      = {"KERNEL / EPHEMERAL",               CYAN},
    [ST_NOACCESS]   = {"NO PMU ACCESS",                    GRAY},
};

typedef struct {
    double total_gbs, demand_gbs, prefetch_gbs;
    double util_pct, cpu_pct, ipc, mpki;
    bool has_bw;
    state_t state;
} metrics_t;

typedef struct {
    pid_t pid, ppid;
    unsigned long long start_ticks;   /* process start, clock ticks since boot */
    unsigned long long ticks, dticks; /* utime + stime, and its increase over the last scan */
    char comm[16];
    char name[NAME_LEN];
    int threads;
    bool seen, attached, just_attached, rearm, denied;
    /* Counters opened with inherit=1 also count children forked afterwards;
     * such children are "covered" by this ancestor until it is re-armed. */
    pid_t covered_by;
    unsigned long long attach_ticks;
    double last_active, last_read, retry_at;
    counter_set_t *cs;
    int ncs;
    uint64_t acc[EV_KINDS];
    metrics_t m;
} proc_t;

typedef struct {
    char time[9];
    double ts;
    metrics_t m;
    char top[NAME_LEN];
    pid_t top_pid;
    bool cfs_ok;
    long long d_periods, d_throttled;
} sample_t;

typedef struct {
    int vendor, family, model;
    unsigned impl, part;
} cpu_id_t;

enum { V_OTHER, V_AMD, V_INTEL };
enum { VIEW_SPLIT, VIEW_PROCS, VIEW_TIMELINE, VIEW_COUNT };
static const char *const VIEW_NAME[VIEW_COUNT] = {"Split", "Top Processes", "Timeline"};

static struct {
    /* options */
    pid_t filter_pid;
    const char *filter_name;
    double interval;
    long max_count;
    double ceiling;
    bool batch, json;

    /* environment */
    pid_t self;
    long clk_tck;
    char cpu_name[128];
    int *cpus, ncpus;
    double eff_cpus;
    bool has_quota;
    unsigned line;

    /* PMU */
    const char *backend;
    event_t ev[MAX_EVENTS];
    int nev;
    bool has_split, has_total, kernel_ok;
    counter_set_t *sys;
    int sys_cpus;
    bool sys_on;
    char scope[96];

    /* processes */
    proc_t *procs;
    int nprocs, cap;
    proc_t other;
    proc_t **view;
    int nview, view_cap;
    uint64_t pending[EV_KINDS];
    bool fd_limited;

    /* sampling */
    double scan_ts;
    unsigned long long prev_boot;
    bool cfs_ok;
    long long cfs_periods, cfs_throttled;
    double peak, max_bw, sum_bw;
    long nsamples;
} g = {.interval = 1.0};

static volatile sig_atomic_t g_stop, g_winch, g_tstp;
static bool g_tui, g_color;
static struct termios g_saved_tio;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static unsigned long long boot_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (unsigned long long)ts.tv_sec * (unsigned long long)g.clk_tck +
           (unsigned long long)ts.tv_nsec * (unsigned long long)g.clk_tck / 1000000000ULL;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) {
        fputs("dramtop: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static ssize_t read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

static bool read_ll(const char *path, long long *out) {
    char buf[64], *end;
    if (read_file(path, buf, sizeof buf) <= 0) return false;
    errno = 0;
    long long v = strtoll(buf, &end, 10);
    if (end == buf || errno) return false;
    *out = v;
    return true;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void copy_str(char *dst, size_t n, const char *src) {
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Process names are attacker-controlled; keep the terminal and JSON safe. */
static void sanitize(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f) *s = '?';
}

static const char *C(const char *esc) { return g_color ? esc : ""; }

/* ------------------------------------------------------------------ */
/* System detection                                                    */
/* ------------------------------------------------------------------ */

static const struct {
    unsigned impl, part;
    const char *name;
} ARM_PARTS[] = {
    {0x41, 0xd0c, "Arm Neoverse N1"},       {0x41, 0xd40, "Arm Neoverse V1"},
    {0x41, 0xd49, "Arm Neoverse N2"},       {0x41, 0xd4f, "Arm Neoverse V2"},
    {0x41, 0xd84, "Arm Neoverse V3"},       {0x41, 0xd8e, "Arm Neoverse N3"},
    {0x46, 0x001, "Fujitsu A64FX"},         {0x48, 0xd01, "HiSilicon Kunpeng 920"},
    {0xc0, 0xac3, "Ampere Ampere-1"},       {0xc0, 0xac4, "Ampere Ampere-1A"},
};

static void detect_cpu(cpu_id_t *id) {
    memset(id, 0, sizeof *id);
    snprintf(g.cpu_name, sizeof g.cpu_name, "Unknown CPU");
    FILE *fp = fopen("/proc/cpuinfo", "re");
    if (!fp) return;
    char line[512];
    bool any = false;
    while (fgets(line, sizeof line, fp)) {
        char *colon = strchr(line, ':');
        if (!colon) {
            if (any) break; /* only the first processor block is needed */
            continue;
        }
        *colon = '\0';
        char *key = trim(line), *val = trim(colon + 1);
        any = true;
        if (!strcmp(key, "vendor_id")) {
            if (!strcmp(val, "AuthenticAMD") || !strcmp(val, "HygonGenuine")) id->vendor = V_AMD;
            else if (!strcmp(val, "GenuineIntel")) id->vendor = V_INTEL;
        } else if (!strcmp(key, "cpu family")) {
            id->family = atoi(val);
        } else if (!strcmp(key, "model")) {
            id->model = atoi(val);
        } else if (!strcmp(key, "model name") && *val) {
            copy_str(g.cpu_name, sizeof g.cpu_name, val);
        } else if (!strcmp(key, "CPU implementer")) {
            id->impl = (unsigned)strtoul(val, NULL, 0);
        } else if (!strcmp(key, "CPU part")) {
            id->part = (unsigned)strtoul(val, NULL, 0);
        }
    }
    fclose(fp);
    if (id->impl) {
        snprintf(g.cpu_name, sizeof g.cpu_name, "Arm CPU (implementer 0x%02x, part 0x%03x)", id->impl, id->part);
        for (size_t i = 0; i < sizeof ARM_PARTS / sizeof ARM_PARTS[0]; i++)
            if (ARM_PARTS[i].impl == id->impl && ARM_PARTS[i].part == id->part)
                snprintf(g.cpu_name, sizeof g.cpu_name, "%s", ARM_PARTS[i].name);
    }
    sanitize(g.cpu_name);
}

static void add_cpu(int **ids, int *n, int *cap, int cpu) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *ids = xrealloc(*ids, (size_t)*cap * sizeof **ids);
    }
    (*ids)[(*n)++] = cpu;
}

/* CPU ids can be sparse ("0-31,64-95"), so parse the online list. */
static void detect_cpus(void) {
    char buf[4096];
    int *ids = NULL, n = 0, cap = 0;
    if (read_file("/sys/devices/system/cpu/online", buf, sizeof buf) > 0) {
        char *p = buf, *end;
        for (;;) {
            long a = strtol(p, &end, 10), b;
            if (end == p) break;
            b = a;
            if (*end == '-') {
                p = end + 1;
                b = strtol(p, &end, 10);
            }
            for (long c = a; c <= b && c < 65536; c++) add_cpu(&ids, &n, &cap, (int)c);
            if (*end != ',') break;
            p = end + 1;
        }
    }
    if (n == 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < (c > 0 ? c : 1); i++) add_cpu(&ids, &n, &cap, (int)i);
    }
    g.cpus = ids;
    g.ncpus = n;
}

static double cgroup_cpu_quota(void) {
    char buf[128];
    long long q, p;
    if (read_file("/sys/fs/cgroup/cpu.max", buf, sizeof buf) > 0)
        return sscanf(buf, "%lld %lld", &q, &p) == 2 && q > 0 && p > 0 ? (double)q / (double)p : 0.0;
    static const char *const dirs[] = {"/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char qp[96], pp[96];
        snprintf(qp, sizeof qp, "%s/cpu.cfs_quota_us", dirs[i]);
        snprintf(pp, sizeof pp, "%s/cpu.cfs_period_us", dirs[i]);
        if (read_ll(qp, &q) && read_ll(pp, &p) && q > 0 && p > 0) return (double)q / (double)p;
    }
    return 0.0;
}

static void detect_cpu_budget(void) {
    double avail = g.ncpus;
    cpu_set_t *set = CPU_ALLOC(8192);
    size_t sz = CPU_ALLOC_SIZE(8192);
    if (set && sched_getaffinity(0, sz, set) == 0 && CPU_COUNT_S(sz, set) > 0) avail = CPU_COUNT_S(sz, set);
    if (set) CPU_FREE(set);
    double quota = cgroup_cpu_quota();
    g.eff_cpus = avail;
    g.has_quota = quota > 0 && quota < avail;
    if (g.has_quota) g.eff_cpus = quota;
}

static unsigned detect_line_size(void) {
    char path[128];
    long long v;
    for (int i = 7; i >= 0; i--) {
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/coherency_line_size", g.cpus[0], i);
        if (read_ll(path, &v) && v >= 32 && v <= 1024) return (unsigned)v;
    }
    return 64;
}

static bool read_cfs(long long *periods, long long *throttled) {
    static const char *const paths[] = {"/sys/fs/cgroup/cpu.stat", "/sys/fs/cgroup/cpu/cpu.stat",
                                        "/sys/fs/cgroup/cpu,cpuacct/cpu.stat"};
    char buf[1024], *s;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        if (read_file(paths[i], buf, sizeof buf) <= 0) continue;
        bool hp = (s = strstr(buf, "nr_periods ")) && sscanf(s + 11, "%lld", periods) == 1;
        bool ht = (s = strstr(buf, "nr_throttled ")) && sscanf(s + 13, "%lld", throttled) == 1;
        if (hp && ht) return true;
    }
    return false;
}

static unsigned long long mem_available(void) {
    unsigned long long avail = ~0ULL;
    char buf[8192], *s;
    long long lim, use;
    if (read_file("/proc/meminfo", buf, sizeof buf) > 0 && (s = strstr(buf, "MemAvailable:")))
        avail = strtoull(s + 13, NULL, 10) * 1024ULL;
    if ((read_ll("/sys/fs/cgroup/memory.max", &lim) && read_ll("/sys/fs/cgroup/memory.current", &use)) ||
        (read_ll("/sys/fs/cgroup/memory/memory.limit_in_bytes", &lim) &&
         read_ll("/sys/fs/cgroup/memory/memory.usage_in_bytes", &use))) {
        unsigned long long room = lim > use ? (unsigned long long)(lim - use) : 0;
        if (room < avail) avail = room;
    }
    return avail;
}

static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* ------------------------------------------------------------------ */
/* PMU counters                                                        */
/* ------------------------------------------------------------------ */

static int perf_open(const event_t *e, pid_t pid, int cpu, bool inherit) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = e->type;
    a.config = e->config;
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    a.inherit = inherit;
    a.exclude_kernel = !g.kernel_ok;
    a.exclude_hv = !g.kernel_ok;
    return (int)syscall(__NR_perf_event_open, &a, pid, cpu, -1, PERF_FLAG_FD_CLOEXEC);
}

static void cs_init(counter_set_t *cs) {
    memset(cs, 0, sizeof *cs);
    for (int i = 0; i < MAX_EVENTS; i++) cs->fd[i] = -1;
}

static void cs_close(counter_set_t *cs) {
    for (int i = 0; i < MAX_EVENTS; i++)
        if (cs->fd[i] >= 0) {
            close(cs->fd[i]);
            cs->fd[i] = -1;
        }
}

/* Scale each interval by its own enabled/running ratio: scaling the cumulative
 * totals and then subtracting goes wrong whenever multiplexing varies. */
static uint64_t ctr_delta(int fd, snap_t *s) {
    uint64_t v[3];
    if (fd < 0 || read(fd, v, sizeof v) != (ssize_t)sizeof v) return 0;
    if (v[0] < s->val || v[1] < s->ena || v[2] < s->run) {
        s->val = v[0], s->ena = v[1], s->run = v[2];
        return 0;
    }
    uint64_t dv = v[0] - s->val, de = v[1] - s->ena, dr = v[2] - s->run;
    s->val = v[0], s->ena = v[1], s->run = v[2];
    if (dr == 0) return 0;
    return de > dr ? (uint64_t)((double)dv * (double)de / (double)dr) : dv;
}

static void cs_read(counter_set_t *cs, uint64_t acc[EV_KINDS]) {
    for (int i = 0; i < g.nev; i++) acc[g.ev[i].kind] += ctr_delta(cs->fd[i], &cs->snap[i]);
}

static uint64_t total_lines(const uint64_t acc[EV_KINDS]) {
    if (!g.has_total) return acc[EV_DEMAND] + acc[EV_PREFETCH];
    return acc[EV_TOTAL] > acc[EV_DEMAND] ? acc[EV_TOTAL] : acc[EV_DEMAND];
}

#if defined(__aarch64__)
static bool arm_event_listed(const char *name) {
    DIR *d = opendir("/sys/bus/event_source/devices");
    if (!d) return false;
    struct dirent *de;
    char path[512];
    bool found = false;
    while (!found && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/events/%s", de->d_name, name);
        found = access(path, F_OK) == 0;
    }
    closedir(d);
    return found;
}
#endif

static void select_events(const cpu_id_t *id, bool generic) {
    int n = 0;
    g.backend = NULL;
    if (!generic && id->vendor == V_AMD && id->family >= 0x17) {
        bool zen4 = id->family >= 0x1a ||
                    (id->family == 0x19 && ((id->model >= 0x10 && id->model <= 0x1f) ||
                                            (id->model >= 0x60 && id->model <= 0xaf)));
        g.ev[n++] = (event_t){PERF_TYPE_RAW, 0x4843, EV_DEMAND};   /* ls_dmnd_fills_from_sys.all_dram_io */
        g.ev[n++] = (event_t){PERF_TYPE_RAW, 0x485a, EV_PREFETCH}; /* ls_hw_pf_dc_fills.all_dram_io */
        /* l2_pf_miss_l2_l3: from Zen 4 on, umask bits 5-7 are the L1 prefetchers,
         * which 0x5a already counts, so only the L2 prefetcher bits are selected. */
        g.ev[n++] = (event_t){PERF_TYPE_RAW, zen4 ? 0x1f72 : 0xff72, EV_PREFETCH};
        g.backend = "AMD Zen PMU (demand + L1/L2 HW prefetch DRAM fills)";
    } else if (!generic && id->vendor == V_INTEL) {
        g.ev[n++] = (event_t){PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, EV_TOTAL}; /* LONGEST_LAT_CACHE.MISS */
        if (id->family == 6) g.ev[n++] = (event_t){PERF_TYPE_RAW, 0x20d1, EV_DEMAND};    /* MEM_LOAD_RETIRED.L3_MISS */
        g.backend = "Intel PMU (LLC misses: demand loads + prefetch/other)";
    }
#if defined(__aarch64__)
    /* The kernel accepts any raw ARM event; only sysfs tells whether it is implemented. */
    else if (!generic && arm_event_listed("ll_cache_miss_rd")) {
        g.ev[n++] = (event_t){PERF_TYPE_RAW, 0x37, EV_TOTAL};
        g.backend = "Arm PMUv3 (LL_CACHE_MISS_RD)";
    } else if (!generic && arm_event_listed("l2d_cache_refill")) {
        g.ev[n++] = (event_t){PERF_TYPE_RAW, 0x17, EV_TOTAL};
        g.backend = "Arm PMUv3 (L2D_CACHE_REFILL)";
    }
#endif
    if (!g.backend) {
        g.ev[n++] = (event_t){PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, EV_TOTAL};
        g.backend = "Generic PMU (LLC misses)";
    }
    g.ev[n++] = (event_t){PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, EV_CYCLES};
    g.ev[n++] = (event_t){PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, EV_INSNS};
    g.nev = n;
    g.has_split = g.has_total = false;
    for (int i = 0; i < n; i++) {
        g.has_split |= g.ev[i].kind == EV_DEMAND;
        g.has_total |= g.ev[i].kind == EV_TOTAL;
    }
}

static int probe_event(const event_t *e) {
    int fd = perf_open(e, 0, -1, false);
    if (fd < 0) return errno;
    close(fd);
    return 0;
}

static void pmu_fail(int err) {
    long long paranoid = -9;
    switch (err) {
    case ENOSYS:
        fputs("dramtop: perf_event_open() is not supported by this kernel.\n", stderr);
        break;
    case EACCES:
    case EPERM:
        read_ll("/proc/sys/kernel/perf_event_paranoid", &paranoid);
        fprintf(stderr,
                "dramtop: perf_event_open() permission denied (kernel.perf_event_paranoid = %lld).\n"
                "  Run as root or grant CAP_PERFMON; in Docker add --cap-add PERFMON (or use --privileged).\n",
                paranoid);
        break;
    case ENOENT:
    case ENODEV:
    case EOPNOTSUPP:
    case EINVAL:
        fputs("dramtop: hardware performance counters are not available on this system.\n"
              "  Virtual machines need a virtualized PMU; most cloud VMs and WSL2 do not expose one.\n",
              stderr);
        break;
    default:
        fprintf(stderr, "dramtop: perf_event_open() failed: %s\n", strerror(err));
    }
    exit(1);
}

static void setup_pmu(const cpu_id_t *id) {
    event_t cycles = {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, EV_CYCLES};
    g.kernel_ok = true;
    int err = probe_event(&cycles);
    if (err == EACCES || err == EPERM) {
        g.kernel_ok = false;
        err = probe_event(&cycles);
    }
    if (err) pmu_fail(err);

    for (int generic = 0; generic <= 1; generic++) {
        select_events(id, generic);
        err = 0;
        for (int i = 0; i < g.nev && !err; i++) err = probe_event(&g.ev[i]);
        if (!err) return;
    }
    pmu_fail(err);
}

static void sys_open(void) {
    if (g.filter_pid > 0 || g.filter_name) return;
    int fd = perf_open(&g.ev[0], -1, g.cpus[0], false);
    if (fd < 0) return; /* per-CPU events need perf_event_paranoid <= 0 or CAP_PERFMON */
    close(fd);
    g.sys = xrealloc(NULL, (size_t)g.ncpus * sizeof *g.sys);
    uint64_t scratch[EV_KINDS] = {0};
    for (int c = 0; c < g.ncpus; c++) {
        cs_init(&g.sys[c]);
        for (int i = 0; i < g.nev; i++) g.sys[c].fd[i] = perf_open(&g.ev[i], -1, g.cpus[c], false);
        if (g.sys[c].fd[0] >= 0) g.sys_cpus++;
        cs_read(&g.sys[c], scratch);
    }
    g.sys_on = g.sys_cpus > 0;
}

/* ------------------------------------------------------------------ */
/* Peak bandwidth calibration                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool go;
    pthread_barrier_t bar;
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

typedef struct {
    int cpu, rank, l3, order;
} cal_cpu_t;

static int first_int(const char *path) {
    char buf[64];
    return read_file(path, buf, sizeof buf) > 0 ? (int)strtol(buf, NULL, 10) : -1;
}

static int cmp_cal_cpu(const void *a, const void *b) {
    const cal_cpu_t *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    if (x->order != y->order) return x->order - y->order;
    return x->l3 - y->l3;
}

/* Each last-level-cache domain (CCD/CCX on AMD) has its own link to the memory
 * controllers; leaving thread placement to the scheduler can pile threads onto
 * one domain and understate the peak, so spread them: physical cores first,
 * round-robin across domains, SMT siblings last. */
static int plan_cal_cpus(int *out, int max) {
    cpu_set_t *set = CPU_ALLOC(8192);
    size_t sz = CPU_ALLOC_SIZE(8192);
    bool masked = set && sched_getaffinity(0, sz, set) == 0;
    cal_cpu_t *c = xrealloc(NULL, (size_t)g.ncpus * sizeof *c);
    char path[128];
    int n = 0;
    for (int i = 0; i < g.ncpus; i++) {
        int cpu = g.cpus[i];
        if (masked && cpu < 8192 && !CPU_ISSET_S((size_t)cpu, sz, set)) continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index3/id", cpu);
        c[n].l3 = first_int(path);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        int sibling = first_int(path);
        c[n].rank = sibling >= 0 && sibling != cpu;
        c[n].cpu = cpu;
        c[n].order = 0;
        for (int k = 0; k < n; k++)
            if (c[k].rank == c[n].rank && c[k].l3 == c[n].l3) c[n].order++;
        n++;
    }
    if (set) CPU_FREE(set);
    qsort(c, (size_t)n, sizeof *c, cmp_cal_cpu);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = c[i].cpu;
    free(c);
    return n;
}

static void pin_self(int cpu) {
    cpu_set_t *set = CPU_ALLOC(8192);
    size_t sz = CPU_ALLOC_SIZE(8192);
    if (!set) return;
    CPU_ZERO_S(sz, set);
    CPU_SET_S((size_t)cpu, sz, set);
    sched_setaffinity(0, sz, set);
    CPU_FREE(set);
}

static void *cal_worker(void *arg) {
    cal_job_t *j = arg;
    pthread_mutex_lock(&j->sync->mu);
    while (!j->sync->go) pthread_cond_wait(&j->sync->cv, &j->sync->mu);
    pthread_mutex_unlock(&j->sync->mu);

    if (j->cpu >= 0) pin_self(j->cpu);
    memset(j->base, 0x5a, j->len); /* first touch puts the pages on this thread's NUMA node */
    pthread_barrier_wait(&j->sync->bar);
    size_t step = j->stride / sizeof(uint64_t), n = j->len / sizeof(uint64_t);
    for (int pass = 0; pass < CAL_PASSES; pass++) {
        pthread_barrier_wait(&j->sync->bar);
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
        pthread_barrier_wait(&j->sync->bar);
    }
    return NULL;
}

static double calibrate(void) {
    unsigned long long bytes = mem_available() / 4;
    if (bytes > CAL_MAX_BYTES) bytes = CAL_MAX_BYTES;
    int nt = (int)g.eff_cpus, pins[CAL_MAX_THREADS];
    if (nt < 1) nt = 1;
    if (nt > CAL_MAX_THREADS) nt = CAL_MAX_THREADS;
    int npins = plan_cal_cpus(pins, nt);
    size_t chunk = (size_t)(bytes / (unsigned long long)nt) & ~(size_t)4095;
    if (chunk < 4096 || (unsigned long long)chunk * (unsigned long long)nt < CAL_MIN_BYTES) return 0.0;

    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 4096, chunk * (size_t)nt)) return 0.0;

    /* Opened with inherit=1 before the workers exist, so their fills are included. */
    counter_set_t cs;
    cs_init(&cs);
    for (int i = 0; i < g.nev; i++)
        if (g.ev[i].kind <= EV_TOTAL) cs.fd[i] = perf_open(&g.ev[i], 0, -1, true);

    cal_sync_t sync = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .sweeps = 1};
    cal_job_t *jobs = xrealloc(NULL, (size_t)nt * sizeof *jobs);
    pthread_t *th = xrealloc(NULL, (size_t)nt * sizeof *th);
    int started = 0;
    for (; started < nt; started++) {
        jobs[started] = (cal_job_t){&sync, buf + (size_t)started * chunk, chunk, g.line,
                                    started < npins ? pins[started] : -1, 0};
        if (pthread_create(&th[started], NULL, cal_worker, &jobs[started])) break;
    }

    double best = 0.0;
    if (started > 0) {
        pthread_barrier_init(&sync.bar, NULL, (unsigned)started + 1);
        pthread_mutex_lock(&sync.mu);
        sync.go = true;
        pthread_cond_broadcast(&sync.cv);
        pthread_mutex_unlock(&sync.mu);
        pthread_barrier_wait(&sync.bar);

        uint64_t scratch[EV_KINDS] = {0};
        cs_read(&cs, scratch);
        for (int pass = 0; pass < CAL_PASSES; pass++) {
            uint64_t acc[EV_KINDS] = {0};
            double t0 = mono_now();
            pthread_barrier_wait(&sync.bar);
            pthread_barrier_wait(&sync.bar);
            double dt = mono_now() - t0;
            cs_read(&cs, acc);
            if (dt <= 0) continue;
            if (pass == 0) {
                /* Warm-up pass: size the timed passes so short scheduler hiccups do not dominate. */
                int sweeps = (int)(CAL_PASS_SEC / dt + 0.5);
                sync.sweeps = sweeps < 1 ? 1 : sweeps > CAL_MAX_SWEEPS ? CAL_MAX_SWEEPS : sweeps;
                continue;
            }
            double gbs = (double)chunk * started * sync.sweeps / dt / 1e9;
            double pmu = (double)total_lines(acc) * g.line / dt / 1e9;
            if (pmu > gbs) gbs = pmu;
            if (gbs > best) best = gbs;
        }
        for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
        pthread_barrier_destroy(&sync.bar);
    }
    cs_close(&cs);
    free(jobs);
    free(th);
    free(buf);
    return best;
}

/* ------------------------------------------------------------------ */
/* Process tracking                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char comm[16];
    char state;
    pid_t ppid;
    unsigned flags;
    unsigned long long ticks, start;
    int threads;
} proc_stat_t;

static bool read_stat(pid_t pid, proc_stat_t *st) {
    char path[32], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_file(path, buf, sizeof buf) <= 0) return false;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r || r < l) return false;
    size_t n = (size_t)(r - l - 1);
    if (n >= sizeof st->comm) n = sizeof st->comm - 1;
    memcpy(st->comm, l + 1, n);
    st->comm[n] = '\0';
    int ppid;
    unsigned long utime, stime;
    long threads;
    if (sscanf(r + 2, "%c %d %*d %*d %*d %*d %u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %ld %*d %llu",
               &st->state, &ppid, &st->flags, &utime, &stime, &threads, &st->start) != 7)
        return false;
    st->ppid = ppid;
    st->ticks = (unsigned long long)utime + stime;
    st->threads = (int)threads;
    return true;
}

/* Some runtimes (Node.js, Electron) rename the main thread to "MainThread". */
static void make_name(pid_t pid, const char *comm, char *out, size_t n) {
    copy_str(out, n, comm);
    if (!strcmp(comm, "MainThread")) {
        char path[40], buf[256];
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        if (read_file(path, buf, sizeof buf) > 0) {
            const char *base = strrchr(buf, '/');
            base = base ? base + 1 : buf;
            if (*base) copy_str(out, n, base);
        }
    }
    sanitize(out);
}

static int proc_index(pid_t pid) {
    int lo = 0, hi = g.nprocs;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g.procs[mid].pid < pid) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static proc_t *proc_find(pid_t pid) {
    int i = proc_index(pid);
    return i < g.nprocs && g.procs[i].pid == pid ? &g.procs[i] : NULL;
}

static proc_t *proc_insert(pid_t pid) {
    int i = proc_index(pid);
    if (g.nprocs == g.cap) {
        g.cap = g.cap ? g.cap * 2 : 256;
        g.procs = xrealloc(g.procs, (size_t)g.cap * sizeof *g.procs);
    }
    memmove(&g.procs[i + 1], &g.procs[i], (size_t)(g.nprocs - i) * sizeof *g.procs);
    g.nprocs++;
    memset(&g.procs[i], 0, sizeof g.procs[i]);
    g.procs[i].pid = pid;
    return &g.procs[i];
}

static void uncover(pid_t ancestor) {
    for (int i = 0; i < g.nprocs; i++)
        if (g.procs[i].covered_by == ancestor) g.procs[i].covered_by = 0;
}

static int open_thread(counter_set_t *cs, pid_t tid) {
    cs_init(cs);
    int ok = 0;
    for (int i = 0; i < g.nev; i++) {
        cs->fd[i] = perf_open(&g.ev[i], tid, -1, true);
        if (cs->fd[i] >= 0) {
            ok++;
            continue;
        }
        int e = errno;
        if (e == EMFILE || e == ENFILE) g.fd_limited = true;
        if (e == EMFILE || e == ENFILE || e == ESRCH || e == EACCES || e == EPERM) {
            cs_close(cs);
            return e == EACCES || e == EPERM ? -EACCES : e == ESRCH ? -ESRCH : -EMFILE;
        }
    }
    return ok;
}

/* With inherit=1 on every existing thread, threads created later are counted
 * by their creator's counters, so counters never need reopening on thread churn. */
static bool attach_counters(proc_t *p, double now) {
    char path[40];
    snprintf(path, sizeof path, "/proc/%d/task", p->pid);
    DIR *d = opendir(path);
    if (!d) return false;
    int cap = p->threads > 0 ? p->threads + 8 : 16, n = 0;
    counter_set_t *cs = xrealloc(NULL, (size_t)cap * sizeof *cs);
    bool denied = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        if (n == cap) {
            cap *= 2;
            cs = xrealloc(cs, (size_t)cap * sizeof *cs);
        }
        int r = open_thread(&cs[n], (pid_t)atoi(de->d_name));
        if (r > 0) n++;
        else if (r == -EACCES) denied = true;
        if (r == -EACCES || r == -EMFILE) break;
    }
    closedir(d);
    if (denied || n == 0) {
        for (int i = 0; i < n; i++) cs_close(&cs[i]);
        free(cs);
        p->denied = denied;
        p->retry_at = now + ATTACH_RETRY_SEC;
        return false;
    }
    uint64_t scratch[EV_KINDS] = {0};
    for (int i = 0; i < n; i++) cs_read(&cs[i], scratch);
    p->cs = cs;
    p->ncs = n;
    p->attached = true;
    p->denied = false;
    p->attach_ticks = boot_ticks();
    return true;
}

/* A final read keeps the traffic of exiting threads, processes and inherited
 * children; closing the counters also ends coverage of those children. */
static void close_counters(proc_t *p, bool final_read) {
    for (int i = 0; i < p->ncs; i++) {
        if (final_read) cs_read(&p->cs[i], p->acc);
        cs_close(&p->cs[i]);
    }
    free(p->cs);
    p->cs = NULL;
    p->ncs = 0;
    p->attached = false;
    uncover(p->pid);
}

static void move_to_pending(proc_t *p) {
    for (int k = 0; k < EV_KINDS; k++) {
        g.pending[k] += p->acc[k];
        p->acc[k] = 0;
    }
}

static void proc_release(proc_t *p) {
    if (p->attached) close_counters(p, true);
    else uncover(p->pid);
    move_to_pending(p);
}

static void visit(pid_t pid, double now) {
    proc_stat_t st;
    if (pid == g.self || !read_stat(pid, &st)) return;
    if ((st.flags & PF_KTHREAD) || st.state == 'Z' || st.state == 'X') return;

    proc_t *p = proc_find(pid);
    bool fresh = false;
    if (p && p->start_ticks != st.start) { /* PID was reused */
        proc_release(p);
        memset(p, 0, sizeof *p);
        p->pid = pid;
        fresh = true;
    }
    char name[NAME_LEN];
    if (p && !fresh && !strcmp(p->comm, st.comm)) memcpy(name, p->name, sizeof name);
    else make_name(pid, st.comm, name, sizeof name);
    if (g.filter_name && !strstr(name, g.filter_name)) return;

    if (!p) {
        p = proc_insert(pid);
        fresh = true;
    }
    if (fresh) {
        p->start_ticks = st.start;
        /* Started since the previous scan: all of its CPU time is new. */
        p->dticks = g.prev_boot && st.start >= g.prev_boot ? st.ticks : 0;
        p->last_active = now;
        proc_t *parent = proc_find(st.ppid);
        if (parent && parent->attached && st.start + 1 >= parent->attach_ticks) p->covered_by = parent->pid;
        else if (parent && parent->covered_by) p->covered_by = parent->covered_by;
    } else {
        p->dticks = st.ticks >= p->ticks ? st.ticks - p->ticks : 0;
    }
    p->ticks = st.ticks;
    if (p->dticks) p->last_active = now;
    p->seen = true;
    p->ppid = st.ppid;
    p->threads = st.threads;
    memcpy(p->comm, st.comm, sizeof p->comm);
    memcpy(p->name, name, sizeof p->name);
}

static void scan_procs(double now) {
    unsigned long long boot = boot_ticks();
    for (int i = 0; i < g.nprocs; i++) g.procs[i].seen = false;
    if (g.filter_pid > 0) {
        visit(g.filter_pid, now);
    } else {
        DIR *d = opendir("/proc");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL)
                if (isdigit((unsigned char)de->d_name[0])) visit((pid_t)atoi(de->d_name), now);
            closedir(d);
        }
    }
    for (int i = 0; i < g.nprocs; i++)
        if (!g.procs[i].seen) proc_release(&g.procs[i]);
    int w = 0;
    for (int i = 0; i < g.nprocs; i++)
        if (g.procs[i].seen) g.procs[w++] = g.procs[i];
    g.nprocs = w;
    g.prev_boot = boot;
}

/* ------------------------------------------------------------------ */
/* Sampling and diagnosis                                              */
/* ------------------------------------------------------------------ */

static void compute_rates(metrics_t *m, const uint64_t acc[EV_KINDS], double dt) {
    double total = (double)total_lines(acc), demand = (double)acc[EV_DEMAND];
    double prefetch = g.has_total ? total - demand : (double)acc[EV_PREFETCH];
    double scale = (double)g.line / dt / 1e9;
    m->total_gbs = total * scale;
    m->demand_gbs = demand * scale;
    m->prefetch_gbs = prefetch * scale;
    m->ipc = acc[EV_CYCLES] ? (double)acc[EV_INSNS] / (double)acc[EV_CYCLES] : 0.0;
    m->mpki = acc[EV_INSNS] ? total * 1000.0 / (double)acc[EV_INSNS] : 0.0;
    m->has_bw = true;
}

static double util_of(double gbs) {
    double u = g.peak > 0 ? gbs / g.peak * 100.0 : 0.0;
    return u > 100.0 ? 100.0 : u;
}

/* Roofline-style heuristics on DRAM utilization, CPU time, IPC and DRAM MPKI.
 * "Busy" means at least about one core of CPU time. */
static state_t diagnose(const metrics_t *m, double throttle_ratio) {
    if (!m->has_bw) return m->cpu_pct >= 5.0 ? ST_SAMPLING : ST_IDLE;
    if (m->total_gbs < 0.25 && m->cpu_pct < 5.0) return ST_IDLE;
    if (m->util_pct >= 42.0) return ST_MEM_BW;
    if (throttle_ratio >= 0.25) return ST_CONTENTION;
    bool busy = m->cpu_pct >= 90.0;
    bool dram_stalls = m->mpki >= 10.0 || (m->mpki >= 3.0 && m->ipc > 0 && m->ipc < 0.5);
    if (busy && dram_stalls) return ST_MEM_LAT;
    /* Low IPC without DRAM misses while the CPU budget is exhausted: spinning or oversubscription. */
    if (m->cpu_pct >= 90.0 * g.eff_cpus && m->ipc > 0 && m->ipc < 0.3) return ST_CONTENTION;
    return busy ? ST_COMPUTE : ST_ACTIVE;
}

static int cmp_view(const void *a, const void *b) {
    const proc_t *x = *(proc_t *const *)a, *y = *(proc_t *const *)b;
    long bx = (long)(x->m.total_gbs * 100), by = (long)(y->m.total_gbs * 100);
    if (bx != by) return bx < by ? 1 : -1;
    long cx = (long)x->m.cpu_pct, cy = (long)y->m.cpu_pct;
    if (cx != cy) return cx < cy ? 1 : -1;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static void sample(sample_t *s, bool discard) {
    double now = mono_now(), dt = now - g.scan_ts;
    bool filtered = g.filter_pid > 0 || g.filter_name;
    if (dt < 1e-3) dt = 1e-3;
    scan_procs(now);
    g.scan_ts = now;
    g.fd_limited = false;

    /* A covered child that becomes active needs counters of its own; re-arm
     * the ancestor first so the child is not counted twice. */
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        if (!p->covered_by || !p->dticks) continue;
        proc_t *a = proc_find(p->covered_by);
        if (a && a->attached) a->rearm = true;
        else p->covered_by = 0;
    }
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *a = &g.procs[i];
        if (!a->rearm) continue;
        a->rearm = false;
        close_counters(a, true);
        if (!attach_counters(a, now)) move_to_pending(a);
    }

    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        if (p->attached || p->covered_by || now < p->retry_at || !(filtered || p->dticks)) continue;
        if (attach_counters(p, now)) {
            memset(p->acc, 0, sizeof p->acc);
            p->last_read = now;
            p->just_attached = true;
        }
    }

    uint64_t attributed[EV_KINDS];
    memcpy(attributed, g.pending, sizeof attributed);
    memset(g.pending, 0, sizeof g.pending);
    double cpu_total = 0.0;
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        memset(&p->m, 0, sizeof p->m);
        p->m.cpu_pct = (double)p->dticks / (double)g.clk_tck / dt * 100.0;
        cpu_total += p->m.cpu_pct;
        if (!p->attached) continue;
        for (int c = 0; c < p->ncs; c++) cs_read(&p->cs[c], p->acc);
        double pdt = now - p->last_read;
        p->last_read = now;
        if (p->acc[EV_CYCLES]) p->last_active = now;
        if (p->just_attached) {
            p->just_attached = false;
        } else if (pdt > 1e-3) {
            compute_rates(&p->m, p->acc, pdt);
        }
        for (int k = 0; k < EV_KINDS; k++) attributed[k] += p->acc[k];
        memset(p->acc, 0, sizeof p->acc);
        if (!filtered && now - p->last_active > IDLE_DETACH_SEC) {
            close_counters(p, true);
            move_to_pending(p);
        }
    }

    uint64_t sys[EV_KINDS] = {0};
    if (g.sys_on) {
        for (int c = 0; c < g.ncpus; c++) cs_read(&g.sys[c], sys);
    } else {
        memcpy(sys, attributed, sizeof sys);
    }
    memset(s, 0, sizeof *s);
    compute_rates(&s->m, sys, dt);
    s->m.cpu_pct = cpu_total;
    if (!discard && s->m.total_gbs > g.peak) g.peak = s->m.total_gbs;

    double throttle = 0.0;
    long long periods, throttled;
    if (g.has_quota && read_cfs(&periods, &throttled)) {
        if (g.cfs_ok) {
            s->cfs_ok = true;
            s->d_periods = periods - g.cfs_periods;
            s->d_throttled = throttled - g.cfs_throttled;
            if (s->d_periods > 0) throttle = (double)s->d_throttled / (double)s->d_periods;
        }
        g.cfs_ok = true;
        g.cfs_periods = periods;
        g.cfs_throttled = throttled;
    }

    s->m.util_pct = util_of(s->m.total_gbs);
    s->m.state = diagnose(&s->m, throttle);

    if (g.view_cap < g.nprocs + 1) {
        g.view_cap = g.nprocs + 64;
        g.view = xrealloc(g.view, (size_t)g.view_cap * sizeof *g.view);
    }
    g.nview = 0;
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        p->m.util_pct = util_of(p->m.total_gbs);
        /* CFS throttling applies to the whole cgroup, so it also stalls every busy member. */
        p->m.state = p->denied ? ST_NOACCESS : diagnose(&p->m, p->m.cpu_pct >= 90.0 ? throttle : 0.0);
        g.view[g.nview++] = p;
    }
    if (g.sys_on) {
        uint64_t rest[EV_KINDS];
        for (int k = 0; k < EV_KINDS; k++) rest[k] = sys[k] > attributed[k] ? sys[k] - attributed[k] : 0;
        memset(&g.other, 0, sizeof g.other);
        snprintf(g.other.name, sizeof g.other.name, "[kernel/ephem]");
        compute_rates(&g.other.m, rest, dt);
        g.other.m.util_pct = util_of(g.other.m.total_gbs);
        g.other.m.state = ST_OTHER;
        if (g.other.m.total_gbs >= 0.05) g.view[g.nview++] = &g.other;
    }
    qsort(g.view, (size_t)g.nview, sizeof *g.view, cmp_view);

    if (g.nview && (g.view[0]->m.total_gbs >= 0.05 || g.view[0]->m.cpu_pct >= 1.0)) {
        snprintf(s->top, sizeof s->top, "%s", g.view[0]->name);
        s->top_pid = g.view[0]->pid;
    } else {
        snprintf(s->top, sizeof s->top, "idle");
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s->ts = (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    strftime(s->time, sizeof s->time, "%H:%M:%S", &tm);
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
    if (g.has_quota) snprintf(buf, n, "Quota: %.1f cores / %d logical", g.eff_cpus, g.ncpus);
    else if ((int)g.eff_cpus < g.ncpus) snprintf(buf, n, "%d of %d logical CPUs", (int)g.eff_cpus, g.ncpus);
    else snprintf(buf, n, "%d logical CPUs", g.ncpus);
}

static int g_rows, g_line;

__attribute__((format(printf, 1, 2))) static void out_line(const char *fmt, ...) {
    if (g_line >= g_rows) return;
    if (g_line > 0) fputc('\n', stdout);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputs(g_color ? RST "\033[K" : "\033[K", stdout);
    g_line++;
}

static void proc_header(void) {
    out_line("%s================================== [ TOP PROCESSES BY DRAM BANDWIDTH ] ===================================", C(CYAN));
    out_line(" %s%-7s | %-15s | %-4s | %-11s | %-10s | %-11s | %-17s | %-6s | %-4s | %s%s", C(WHITE), "PID", "PROCESS",
             "THR", "DRAM TOTAL", "DEMAND RD", "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "BOTTLENECK", C(RST));
    out_line("%s", DASH);
}

static void proc_row(const proc_t *p) {
    char pid[16] = "-", thr[16] = "-", tot[24], dem[24], pf[24], ipc[16], ub[96], up[16];
    bool other = p == &g.other, split = p->m.has_bw && g.has_split;
    if (!other) {
        snprintf(pid, sizeof pid, "%d", p->pid);
        snprintf(thr, sizeof thr, "%d", p->threads);
    }
    if (p->m.has_bw) snprintf(up, sizeof up, "%5.1f%%", p->m.util_pct);
    else snprintf(up, sizeof up, "%6s", "-");
    out_line(" %-7s | %-15.15s | %4s | %s | %s | %s | %s %s | %5.0f%% | %s | %s%s%s", pid, p->name, thr,
             fmt_bw(tot, sizeof tot, p->m.has_bw, p->m.total_gbs, 6), fmt_bw(dem, sizeof dem, split, p->m.demand_gbs, 5),
             fmt_bw(pf, sizeof pf, split, p->m.prefetch_gbs, 6), bar(ub, sizeof ub, p->m.util_pct, 8), up,
             p->m.cpu_pct, fmt_ipc(ipc, sizeof ipc, p->m.ipc), C(STATE[p->m.state].color), STATE[p->m.state].label,
             C(RST));
}

static void timeline_header(void) {
    out_line("%s=================================== [ REAL-TIME ROLLING TIMELINE ] =======================================", C(CYAN));
    out_line(" %s%-8s | %-11s | %-10s | %-11s | %-17s | %-6s | %-4s | %-13s | %s%s", C(WHITE), "TIME", "DRAM TOTAL",
             "DEMAND RD", "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "TOP PROCESS", "DIAGNOSIS", C(RST));
    out_line("%s", DASH);
}

static void timeline_row(const sample_t *s) {
    char tot[24], dem[24], pf[24], ipc[16], ub[96];
    out_line(" %-8s | %s | %s | %s | %s %5.1f%% | %5.0f%% | %s | %-13.13s | %s%s%s", s->time,
             fmt_bw(tot, sizeof tot, true, s->m.total_gbs, 6), fmt_bw(dem, sizeof dem, g.has_split, s->m.demand_gbs, 5),
             fmt_bw(pf, sizeof pf, g.has_split, s->m.prefetch_gbs, 6), bar(ub, sizeof ub, s->m.util_pct, 8),
             s->m.util_pct, s->m.cpu_pct, fmt_ipc(ipc, sizeof ipc, s->m.ipc), s->top, C(STATE[s->m.state].color),
             STATE[s->m.state].label, C(RST));
}

static void render(const sample_t *ring, int count, int head, int view, bool paused) {
    static const sample_t placeholder = {.time = "--:--:--", .top = "-", .m = {.state = ST_SAMPLING}};
    const sample_t *cur = count ? &ring[(head + RING_SIZE - 1) % RING_SIZE] : &placeholder;
    struct winsize ws;
    bool filtered = g.filter_pid > 0 || g.filter_name;
    char budget[64], b1[96], b2[96], ipc[16], thr[48];

    g_rows = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 ? ws.ws_row : 24;
    g_line = 0;
    fputs("\033[H", stdout);

    cpu_budget(budget, sizeof budget);
    if (cur->cfs_ok) snprintf(thr, sizeof thr, "%lld/%lld periods", cur->d_throttled, cur->d_periods);
    else snprintf(thr, sizeof thr, "n/a");
    out_line("%s%s", C(CYAN), RULE);
    out_line(" %sdramtop v%s%s  |  CPU: %s%s%s (%s)", C(WHITE), DRAMTOP_VERSION, C(RST), C(YELLOW), g.cpu_name, C(RST),
             budget);
    out_line(" PMU Backend   : %s", g.backend);
    out_line(" Monitor Scope : %s%s%s (%d procs)%s  |  Refresh: %.2fs%s  |  View: %s%s%s", C(GREEN), g.scope, C(RST),
             g.nprocs, g.fd_limited ? " [fd limit reached]" : "", g.interval, paused ? " [PAUSED]" : "", C(MAGENTA),
             VIEW_NAME[view], C(RST));
    out_line("%s%s", C(CYAN), DASH);
    out_line(" %s%s%s: %s %s%6.2f GB/s%s (%5.1f%% of %.1f GB/s cap)", C(BOLD),
             filtered ? "Filter DRAM BW" : "System DRAM BW", C(RST), bar(b1, sizeof b1, cur->m.util_pct, 30), C(CYAN),
             cur->m.total_gbs, C(RST), cur->m.util_pct, g.peak);
    if (g.has_split)
        out_line("   ├─ Demand Rd: %s%6.2f GB/s%s   ├─ HW Prefetch: %s%6.2f GB/s%s   ├─ Session Max/Avg: %s%.2f%s / %s%.2f GB/s%s",
                 C(WHITE), cur->m.demand_gbs, C(RST), C(WHITE), cur->m.prefetch_gbs, C(RST), C(YELLOW), g.max_bw, C(RST),
                 C(GREEN), g.nsamples ? g.sum_bw / (double)g.nsamples : 0.0, C(RST));
    else
        out_line("   ├─ Demand/Prefetch split: n/a on this CPU   ├─ Session Max/Avg: %s%.2f%s / %s%.2f GB/s%s",
                 C(YELLOW), g.max_bw, C(RST), C(GREEN), g.nsamples ? g.sum_bw / (double)g.nsamples : 0.0, C(RST));
    out_line(" %s%s%s: %s %s%6.0f%%%s     (IPC %s | MPKI %.1f | Throttled %s)", C(BOLD),
             g.has_quota ? "CPU Quota Util" : "CPU Util      ", C(RST),
             bar(b2, sizeof b2, cur->m.cpu_pct / g.eff_cpus, 30), C(WHITE), cur->m.cpu_pct, C(RST),
             fmt_ipc(ipc, sizeof ipc, cur->m.ipc), cur->m.mpki, thr);
    out_line(" %sLive Diagnosis%s: %s%s%s  (Top Bandwidth Consumer: %s%s%s)", C(BOLD), C(RST),
             C(STATE[cur->m.state].color), STATE[cur->m.state].label, C(RST), C(YELLOW), cur->top, C(RST));

    int body = g_rows - 11, np = 0, nh = 0;
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
    printf("dramtop v%s | CPU: %s (%s)\n", DRAMTOP_VERSION, g.cpu_name, budget);
    printf("PMU Backend   : %s\n", g.backend);
    printf("Monitor Scope : %s (%d procs) | DRAM cap: %.1f GB/s (%s)\n", g.scope, g.nprocs, g.peak,
           g.ceiling > 0 ? "manual" : "calibrated");
    printf("%s\n", RULE);
    printf("%-8s | %-11s | %-10s | %-11s | %-17s | %-6s | %-4s | %-13s | %s\n", "TIME", "DRAM TOTAL", "DEMAND RD",
           "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "TOP PROCESS", "DIAGNOSIS");
    printf("%s\n", DASH);
    fflush(stdout);
}

static void print_batch(const sample_t *s) {
    char tot[24], dem[24], pf[24], ipc[16], ub[96];
    printf("%-8s | %s | %s | %s | %s %5.1f%% | %5.0f%% | %s | %-13.13s | %s%s%s\n", s->time,
           fmt_bw(tot, sizeof tot, true, s->m.total_gbs, 6), fmt_bw(dem, sizeof dem, g.has_split, s->m.demand_gbs, 5),
           fmt_bw(pf, sizeof pf, g.has_split, s->m.prefetch_gbs, 6), bar(ub, sizeof ub, s->m.util_pct, 8),
           s->m.util_pct, s->m.cpu_pct, fmt_ipc(ipc, sizeof ipc, s->m.ipc), s->top, C(STATE[s->m.state].color),
           STATE[s->m.state].label, C(RST));
    fflush(stdout);
}

static void json_str(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

static void json_bw(const char *key, bool ok, double v) {
    if (ok) printf("\"%s\":%.3f,", key, v);
    else printf("\"%s\":null,", key);
}

static void print_json(const sample_t *s) {
    printf("{\"time\":\"%s\",\"timestamp\":%.3f,\"mode\":\"%s\",\"system\":{\"total_bw_gbs\":%.3f,", s->time, s->ts,
           g.sys_on ? "per-cpu" : "per-thread", s->m.total_gbs);
    json_bw("demand_bw_gbs", g.has_split, s->m.demand_gbs);
    json_bw("prefetch_bw_gbs", g.has_split, s->m.prefetch_gbs);
    printf("\"peak_cap_gbs\":%.2f,\"util_pct\":%.2f,\"cpu_pct\":%.1f,\"ipc\":%.3f,\"mpki\":%.2f,", g.peak,
           s->m.util_pct, s->m.cpu_pct, s->m.ipc, s->m.mpki);
    if (s->cfs_ok) printf("\"throttled\":%lld,\"periods\":%lld,", s->d_throttled, s->d_periods);
    else printf("\"throttled\":null,\"periods\":null,");
    printf("\"top_process\":");
    json_str(s->top);
    printf(",\"top_pid\":%d,\"diagnosis\":\"%s\"},\"top_processes\":[", s->top_pid, STATE[s->m.state].label);
    int n = g.nview < JSON_TOP_PROCS ? g.nview : JSON_TOP_PROCS;
    for (int i = 0; i < n; i++) {
        const proc_t *p = g.view[i];
        bool split = p->m.has_bw && g.has_split;
        printf("%s{\"pid\":%d,\"comm\":", i ? "," : "", p->pid);
        json_str(p->name);
        printf(",\"threads\":%d,", p->threads);
        json_bw("total_bw_gbs", p->m.has_bw, p->m.total_gbs);
        json_bw("demand_bw_gbs", split, p->m.demand_gbs);
        json_bw("prefetch_bw_gbs", split, p->m.prefetch_gbs);
        printf("\"util_pct\":%.2f,\"cpu_pct\":%.1f,\"ipc\":%.3f,\"diagnosis\":\"%s\"}", p->m.util_pct, p->m.cpu_pct,
               p->m.ipc, STATE[p->m.state].label);
    }
    printf("]}\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Terminal and signals                                                */
/* ------------------------------------------------------------------ */

static void on_signal(int sig) {
    if (sig == SIGWINCH) g_winch = 1;
    else if (sig == SIGTSTP) g_tstp = 1;
    else g_stop = 1;
}

static void install_handler(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void tui_enter(void) {
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return;
    struct termios t = g_saved_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return;
    /* Alternate screen, auto-wrap off (long rows are clipped, not wrapped), cursor hidden. */
    fputs("\033[?1049h\033[?7l\033[?25l\033[2J\033[H", stdout);
    fflush(stdout);
    g_tui = true;
}

static void tui_leave(void) {
    if (!g_tui) return;
    fputs(g_color ? RST "\033[?25h\033[?7h\033[?1049l" : "\033[?25h\033[?7h\033[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
    g_tui = false;
}

static void suspend_self(void) {
    tui_leave();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    install_handler(SIGTSTP);
    tui_enter();
}

static int wait_key(double sec) {
    if (sec < 0) sec = 0;
    if (!g_tui) {
        struct timespec ts = {(time_t)sec, (long)((sec - (double)(time_t)sec) * 1e9)};
        nanosleep(&ts, NULL);
        return 0;
    }
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&pfd, 1, (int)(sec * 1000.0 + 0.5)) <= 0) return 0;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        g_stop = 1;
        return 0;
    }
    unsigned char ch;
    return read(STDIN_FILENO, &ch, 1) == 1 ? ch : 0;
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
            "dramtop v%s - real-time DRAM bandwidth monitor for Linux\n\n"
            "Usage: dramtop [options]\n\n"
            "Options:\n"
            "  -p, --pid <PID>        Monitor a single process (and children it forks)\n"
            "  -P, --proc <NAME>      Monitor processes whose name contains NAME\n"
            "  -i, --interval <SEC>   Refresh interval in seconds (default: 1.0)\n"
            "  -n, --count <N>        Exit after N samples (default: run until stopped)\n"
            "  -c, --ceiling <GB/s>   DRAM bandwidth ceiling; skips the startup calibration\n"
            "  -b, --batch            Line-by-line output instead of the full-screen TUI\n"
            "  -J, --json             JSON Lines output, one object per sample\n"
            "  -v, --version          Print version and exit\n"
            "  -h, --help             Show this help\n\n"
            "Colors are disabled when NO_COLOR is set or output is not a terminal.\n",
            DRAMTOP_VERSION);
}

static void bad_arg(const char *opt, const char *val) {
    fprintf(stderr, "dramtop: invalid value for %s: '%s'\nTry 'dramtop --help'.\n", opt, val);
    exit(2);
}

static double parse_double(const char *opt, const char *s, double lo, double hi) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || v < lo || v > hi) bad_arg(opt, s);
    return v;
}

static long parse_long(const char *opt, const char *s, long lo, long hi) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) bad_arg(opt, s);
    return v;
}

static void parse_args(int argc, char **argv) {
    static const struct option opts[] = {
        {"pid", required_argument, 0, 'p'},     {"proc", required_argument, 0, 'P'},
        {"interval", required_argument, 0, 'i'}, {"count", required_argument, 0, 'n'},
        {"ceiling", required_argument, 0, 'c'},  {"batch", no_argument, 0, 'b'},
        {"json", no_argument, 0, 'J'},           {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},           {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "p:P:i:n:c:bJvh", opts, NULL)) != -1) {
        switch (opt) {
        case 'p': g.filter_pid = (pid_t)parse_long("--pid", optarg, 1, 0x7fffffff); break;
        case 'P': g.filter_name = *optarg ? optarg : NULL; break;
        case 'i': g.interval = parse_double("--interval", optarg, 0.1, 3600.0); break;
        case 'n': g.max_count = parse_long("--count", optarg, 1, 0x7fffffff); break;
        case 'c': g.ceiling = parse_double("--ceiling", optarg, 0.1, 1e6); break;
        case 'b': g.batch = true; break;
        case 'J': g.json = g.batch = true; break;
        case 'v': printf("dramtop v%s\n", DRAMTOP_VERSION); exit(0);
        case 'h': usage(stdout); exit(0);
        default: fputs("Try 'dramtop --help'.\n", stderr); exit(2);
        }
    }
    if (optind < argc) {
        fprintf(stderr, "dramtop: unexpected argument '%s'\nTry 'dramtop --help'.\n", argv[optind]);
        exit(2);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    parse_args(argc, argv);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) g.batch = true;
    const char *no_color = getenv("NO_COLOR");
    g_color = !g.json && isatty(STDOUT_FILENO) && !(no_color && *no_color);

    install_handler(SIGINT);
    install_handler(SIGTERM);
    install_handler(SIGHUP);
    install_handler(SIGWINCH);
    raise_fd_limit();

    g.self = getpid();
    g.clk_tck = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    cpu_id_t id;
    detect_cpu(&id);
    detect_cpus();
    detect_cpu_budget();
    g.line = detect_line_size();
    setup_pmu(&id);

    scan_procs(mono_now()); /* CPU-time baseline; the calibration below doubles as the activity window */
    g.scan_ts = mono_now();
    if (g.filter_pid > 0 && !proc_find(g.filter_pid)) {
        fprintf(stderr, "dramtop: no such process: %d\n", g.filter_pid);
        return 1;
    }

    if (g.ceiling > 0) {
        g.peak = g.ceiling;
    } else {
        bool tell = isatty(STDERR_FILENO) && !g.json;
        if (tell) fputs("dramtop: measuring peak DRAM bandwidth (skip with -c <GB/s>)...", stderr);
        g.peak = calibrate();
        if (tell) fputs("\r\033[K", stderr);
        if (g.peak <= 0) g.peak = 100.0;
    }

    sys_open();
    if (g.filter_pid > 0) snprintf(g.scope, sizeof g.scope, "PID %d", g.filter_pid);
    else if (g.filter_name) snprintf(g.scope, sizeof g.scope, "Process filter '%.40s'", g.filter_name);
    else if (g.sys_on)
        snprintf(g.scope, sizeof g.scope, "Per-CPU System PMU (pid=-1, %d CPUs, %s)", g.sys_cpus,
                 g.kernel_ok ? "Kernel+User" : "User");
    else snprintf(g.scope, sizeof g.scope, "All Processes (Thread-PMU Container Mode)");

    double since = mono_now() - g.scan_ts;
    if (since < 0.25) wait_key(0.25 - since);
    sample_t ring[RING_SIZE], scratch;
    sample(&scratch, true); /* attach counters to processes that were active in the window */

    if (!g.batch) {
        tui_enter();
        if (g_tui) {
            atexit(tui_leave);
            install_handler(SIGTSTP);
        }
    }
    if (!g_tui) g.batch = true;
    if (g.batch && !g.json) print_batch_header();

    int head = 0, count = 0, view = VIEW_SPLIT;
    long printed = 0;
    bool paused = false, exited = false;
    if (g_tui) render(ring, count, head, view, paused);

    while (!g_stop) {
        double next = g.scan_ts + g.interval;
        for (;;) {
            if (g_stop) break;
            if (g_tstp) {
                g_tstp = 0;
                suspend_self();
                if (!paused) sample(&scratch, true);
                next = g.scan_ts + g.interval;
                if (g_tui) render(ring, count, head, view, paused);
                continue;
            }
            if (g_winch) {
                g_winch = 0;
                if (g_tui) render(ring, count, head, view, paused);
            }
            double rem = paused ? 0.5 : next - mono_now();
            if (!paused && rem <= 0) break;
            int key = wait_key(rem);
            if (!key) continue;
            switch (key) {
            case 'q': case 'Q':
                g_stop = 1;
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
                    next = g.scan_ts + g.interval;
                }
                break;
            case '+': case '=':
                g.interval = step_interval(g.interval, -1);
                next = g.scan_ts + g.interval;
                break;
            case '-': case '_':
                g.interval = step_interval(g.interval, +1);
                next = g.scan_ts + g.interval;
                break;
            default:
                continue;
            }
            if (g_tui && !g_stop) render(ring, count, head, view, paused);
        }
        if (g_stop) break;

        sample_t *s = &ring[head];
        sample(s, false);
        head = (head + 1) % RING_SIZE;
        if (count < RING_SIZE) count++;
        g.nsamples++;
        g.sum_bw += s->m.total_gbs;
        if (s->m.total_gbs > g.max_bw) g.max_bw = s->m.total_gbs;

        if (g_tui) render(ring, count, head, view, paused);
        else if (g.json) print_json(s);
        else print_batch(s);

        if (g.max_count > 0 && ++printed >= g.max_count) break;
        if (g.filter_pid > 0 && !proc_find(g.filter_pid)) {
            exited = true;
            break;
        }
    }

    tui_leave();
    if (exited) fprintf(stderr, "dramtop: process %d exited\n", g.filter_pid);
    for (int i = 0; i < g.nprocs; i++)
        if (g.procs[i].attached) close_counters(&g.procs[i], false);
    for (int c = 0; g.sys_on && c < g.ncpus; c++) cs_close(&g.sys[c]);
    return 0;
}
