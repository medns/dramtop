/*
 * dramtop - Linux collection backend
 *
 * Hardware counters come from perf_event_open(2): one set per CPU when the
 * kernel allows it, otherwise one set per thread with inherit=1. DRAM traffic
 * is counted as cache-line fills from memory, split into demand reads and
 * hardware-prefetch fills where the CPU tells them apart. Process names and
 * CPU time come from /proc, CPU quota and throttling from the cgroup files.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#define _GNU_SOURCE
#include "dramtop.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PERF_FLAG_FD_CLOEXEC
#define PERF_FLAG_FD_CLOEXEC (1UL << 3)
#endif

#define MAX_EVENTS        6
#define IDLE_DETACH_SEC   30.0
#define ATTACH_RETRY_SEC  30.0
#define PF_KTHREAD        0x00200000u

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

typedef struct {
    int vendor, family, model;
    unsigned impl, part;
} cpu_id_t;

enum { V_OTHER, V_AMD, V_INTEL };

typedef struct {
    dt_proc_t pub;                    /* pid, name, threads and counts as the core sees them */
    pid_t ppid;
    unsigned long long start_ticks;   /* process start, clock ticks since boot */
    unsigned long long ticks, dticks; /* utime + stime, and its increase over the last scan */
    char comm[16];
    bool seen, attached, just_attached, rearm;
    /* Counters opened with inherit=1 also count children forked afterwards;
     * such children are "covered" by this ancestor until it is re-armed. */
    pid_t covered_by;
    unsigned long long attach_ticks;
    double last_active, last_read, retry_at;
    counter_set_t *cs;
    int ncs;
    uint64_t acc[EV_KINDS];
} proc_t;

static struct {
    dt_config_t cfg;
    pid_t self;
    long clk_tck;
    char cpu_name[128];
    char scope[96];
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
    counter_set_t probe;

    /* processes, sorted by pid */
    proc_t *procs;
    int nprocs, cap;
    dt_proc_t **list;
    int list_cap;
    uint64_t pending[EV_KINDS];
    bool fd_limited;
    unsigned long long prev_boot;
    bool cfs_ok;
    long long cfs_periods, cfs_throttled;
} g;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static unsigned long long boot_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (unsigned long long)ts.tv_sec * (unsigned long long)g.clk_tck +
           (unsigned long long)ts.tv_nsec * (unsigned long long)g.clk_tck / 1000000000ULL;
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
static uint64_t snap_delta(snap_t *s, const uint64_t v[3]) {
    if (v[0] < s->val || v[1] < s->ena || v[2] < s->run) {
        s->val = v[0], s->ena = v[1], s->run = v[2];
        return 0;
    }
    uint64_t dv = v[0] - s->val, de = v[1] - s->ena, dr = v[2] - s->run;
    s->val = v[0], s->ena = v[1], s->run = v[2];
    if (dr == 0) return 0;
    return de > dr ? (uint64_t)((double)dv * (double)de / (double)dr) : dv;
}

static uint64_t ctr_delta(int fd, snap_t *s) {
    uint64_t v[3];
    if (fd < 0 || read(fd, v, sizeof v) != (ssize_t)sizeof v) return 0;
    return snap_delta(s, v);
}

static void cs_read(counter_set_t *cs, uint64_t acc[EV_KINDS]) {
    for (int i = 0; i < g.nev; i++) acc[g.ev[i].kind] += ctr_delta(cs->fd[i], &cs->snap[i]);
}

static uint64_t total_lines(const uint64_t acc[EV_KINDS]) {
    if (!g.has_total) return acc[EV_DEMAND] + acc[EV_PREFETCH];
    return acc[EV_TOTAL] > acc[EV_DEMAND] ? acc[EV_TOTAL] : acc[EV_DEMAND];
}

static void to_counts(const uint64_t acc[EV_KINDS], dt_counts_t *c) {
    c->total = (double)total_lines(acc);
    c->demand = (double)acc[EV_DEMAND];
    c->prefetch = g.has_total ? c->total - c->demand : (double)acc[EV_PREFETCH];
    c->cycles = (double)acc[EV_CYCLES];
    c->insns = (double)acc[EV_INSNS];
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

static bool pmu_fail(int err) {
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
    return false;
}

static bool setup_pmu(const cpu_id_t *id) {
    event_t cycles = {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, EV_CYCLES};
    g.kernel_ok = true;
    int err = probe_event(&cycles);
    if (err == EACCES || err == EPERM) {
        g.kernel_ok = false;
        err = probe_event(&cycles);
    }
    if (err) return pmu_fail(err);

    for (int generic = 0; generic <= 1; generic++) {
        select_events(id, generic);
        err = 0;
        for (int i = 0; i < g.nev && !err; i++) err = probe_event(&g.ev[i]);
        if (!err) return true;
    }
    return pmu_fail(err);
}

static void sys_open(void) {
    if (g.cfg.filter_pid > 0 || g.cfg.filter_name) return;
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

unsigned long long plat_mem_available(void) {
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

/* Each last-level-cache domain (CCD/CCX on AMD) has its own link to the memory
 * controllers; leaving thread placement to the scheduler can pile threads onto
 * one domain and understate the peak, so spread them: physical cores first,
 * round-robin across domains, SMT siblings last. */
int plat_cal_cpus(int *out, int max) {
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

void plat_pin_thread(int cpu) {
    cpu_set_t *set = CPU_ALLOC(8192);
    size_t sz = CPU_ALLOC_SIZE(8192);
    if (!set) return;
    CPU_ZERO_S(sz, set);
    CPU_SET_S((size_t)cpu, sz, set);
    sched_setaffinity(0, sz, set);
    CPU_FREE(set);
}

/* inherit=1 includes the fills of every thread this process creates afterwards. */
void plat_probe_begin(void) {
    cs_init(&g.probe);
    for (int i = 0; i < g.nev; i++)
        if (g.ev[i].kind <= EV_TOTAL) g.probe.fd[i] = perf_open(&g.ev[i], 0, -1, true);
}

double plat_probe_lines(double bytes) {
    (void)bytes;
    uint64_t acc[EV_KINDS] = {0};
    cs_read(&g.probe, acc);
    return (double)total_lines(acc);
}

void plat_probe_end(void) { cs_close(&g.probe); }

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
        if (g.procs[mid].pub.pid < pid) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static proc_t *proc_find(pid_t pid) {
    int i = proc_index(pid);
    return i < g.nprocs && g.procs[i].pub.pid == pid ? &g.procs[i] : NULL;
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
    g.procs[i].pub.pid = pid;
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
    snprintf(path, sizeof path, "/proc/%d/task", p->pub.pid);
    DIR *d = opendir(path);
    if (!d) return false;
    int cap = p->pub.threads > 0 ? p->pub.threads + 8 : 16, n = 0;
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
        p->pub.denied = denied;
        p->retry_at = now + ATTACH_RETRY_SEC;
        return false;
    }
    uint64_t scratch[EV_KINDS] = {0};
    for (int i = 0; i < n; i++) cs_read(&cs[i], scratch);
    p->cs = cs;
    p->ncs = n;
    p->attached = true;
    p->pub.denied = false;
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
    uncover(p->pub.pid);
}

static void move_to_pending(proc_t *p) {
    for (int k = 0; k < EV_KINDS; k++) {
        g.pending[k] += p->acc[k];
        p->acc[k] = 0;
    }
}

static void proc_release(proc_t *p) {
    if (p->attached) close_counters(p, true);
    else uncover(p->pub.pid);
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
        p->pub.pid = pid;
        fresh = true;
    }
    char name[NAME_LEN];
    if (p && !fresh && !strcmp(p->comm, st.comm)) memcpy(name, p->pub.name, sizeof name);
    else make_name(pid, st.comm, name, sizeof name);
    if (g.cfg.filter_name && !strstr(name, g.cfg.filter_name)) return;

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
        if (parent && parent->attached && st.start + 1 >= parent->attach_ticks) p->covered_by = parent->pub.pid;
        else if (parent && parent->covered_by) p->covered_by = parent->covered_by;
    } else {
        p->dticks = st.ticks >= p->ticks ? st.ticks - p->ticks : 0;
    }
    p->ticks = st.ticks;
    if (p->dticks) p->last_active = now;
    p->seen = true;
    p->ppid = st.ppid;
    p->pub.threads = st.threads;
    memcpy(p->comm, st.comm, sizeof p->comm);
    memcpy(p->pub.name, name, sizeof p->pub.name);
}

static void scan_procs(double now) {
    unsigned long long boot = boot_ticks();
    for (int i = 0; i < g.nprocs; i++) g.procs[i].seen = false;
    if (g.cfg.filter_pid > 0) {
        visit(g.cfg.filter_pid, now);
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
/* Backend interface                                                   */
/* ------------------------------------------------------------------ */

const char plat_name[] = "Linux";
const bool plat_pid_children = true;

bool plat_init(const dt_config_t *cfg, dt_info_t *info) {
    g.cfg = *cfg;
    raise_fd_limit();
    g.self = getpid();
    g.clk_tck = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    cpu_id_t id;
    detect_cpu(&id);
    detect_cpus();
    detect_cpu_budget();
    g.line = detect_line_size();
    if (!setup_pmu(&id)) return false;
    cs_init(&g.probe);
    *info = (dt_info_t){
        .cpu_name = g.cpu_name, .backend = g.backend, .scope = "", .mode = "per-thread", .ncpus = g.ncpus,
        .eff_cpus = g.eff_cpus, .has_quota = g.has_quota, .line = g.line, .has_split = g.has_split, .has_cfs = true,
    };
    return true;
}

void plat_scan(double now) { scan_procs(now); }

bool plat_has_proc(int pid) { return proc_find((pid_t)pid) != NULL; }

void plat_start_system(dt_info_t *info) {
    sys_open();
    if (g.sys_on)
        snprintf(g.scope, sizeof g.scope, "Per-CPU System PMU (pid=-1, %d CPUs, %s)", g.sys_cpus,
                 g.kernel_ok ? "Kernel+User" : "User");
    else snprintf(g.scope, sizeof g.scope, "All Processes (Thread-PMU Container Mode)");
    info->scope = g.scope;
    info->mode = g.sys_on ? "per-cpu" : "per-thread";
}

void plat_sample(double now, dt_snapshot_t *snap) {
    bool filtered = g.cfg.filter_pid > 0 || g.cfg.filter_name;
    scan_procs(now);
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
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        p->pub.cpu_sec = (double)p->dticks / (double)g.clk_tck;
        p->pub.has_bw = false;
        if (!p->attached) continue;
        for (int c = 0; c < p->ncs; c++) cs_read(&p->cs[c], p->acc);
        double pdt = now - p->last_read;
        p->last_read = now;
        if (p->acc[EV_CYCLES]) p->last_active = now;
        if (p->just_attached) {
            p->just_attached = false;
        } else if (pdt > 1e-3) {
            to_counts(p->acc, &p->pub.c);
            p->pub.bw_sec = pdt;
            p->pub.has_bw = true;
        }
        for (int k = 0; k < EV_KINDS; k++) attributed[k] += p->acc[k];
        memset(p->acc, 0, sizeof p->acc);
        if (!filtered && now - p->last_active > IDLE_DETACH_SEC) {
            close_counters(p, true);
            move_to_pending(p);
        }
    }

    memset(snap, 0, sizeof *snap);
    uint64_t sys[EV_KINDS] = {0};
    if (g.sys_on) {
        for (int c = 0; c < g.ncpus; c++) cs_read(&g.sys[c], sys);
        uint64_t rest[EV_KINDS];
        for (int k = 0; k < EV_KINDS; k++) rest[k] = sys[k] > attributed[k] ? sys[k] - attributed[k] : 0;
        to_counts(rest, &snap->other);
        snap->has_other = true;
    } else {
        memcpy(sys, attributed, sizeof sys);
    }
    to_counts(sys, &snap->sys);

    long long periods, throttled;
    if (g.has_quota && read_cfs(&periods, &throttled)) {
        if (g.cfs_ok) {
            snap->cfs_ok = true;
            snap->d_periods = periods - g.cfs_periods;
            snap->d_throttled = throttled - g.cfs_throttled;
        }
        g.cfs_ok = true;
        g.cfs_periods = periods;
        g.cfs_throttled = throttled;
    }

    if (g.list_cap < g.nprocs) {
        g.list_cap = g.nprocs + 64;
        g.list = xrealloc(g.list, (size_t)g.list_cap * sizeof *g.list);
    }
    for (int i = 0; i < g.nprocs; i++) g.list[i] = &g.procs[i].pub;
    snap->procs = g.list;
    snap->nprocs = g.nprocs;
    snap->fd_limited = g.fd_limited;
}

void plat_shutdown(void) {
    for (int i = 0; i < g.nprocs; i++)
        if (g.procs[i].attached) close_counters(&g.procs[i], false);
    for (int c = 0; g.sys_on && c < g.ncpus; c++) cs_close(&g.sys[c]);
}
