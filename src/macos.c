/*
 * dramtop - macOS collection backend (Apple Silicon)
 *
 * DRAM traffic comes from the memory controller counters that IOReport
 * publishes ("AMC Stats"): exact read and write bytes for every part of the
 * chip (CPU clusters, GPU, Neural Engine, media engines, display, IO).
 * Processes, CPU time, instructions, cycles and energy come from libproc,
 * GPU time from the GPU driver's user clients in the IORegistry.
 *
 * Per-process DRAM traffic is an estimate. As root, each CPU samples its own
 * L1D miss and write-back counters every PMI_PERIOD active cycles and at every
 * context switch (kperf actions, logged through kdebug), and the change since
 * that CPU's previous sample is charged to the process running at the time, so
 * each process's counts are exact. Each CPU cluster's exact DRAM bytes are then
 * split in proportion to those counts, weighted per process by a regression
 * over time, the GPU's bytes in proportion to GPU time and the Neural Engine's
 * by each app's Neural Engine time, then within the app by Neural Engine memory.
 *
 * IOReport and kperf are private interfaces, so they are loaded at run time.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#include "dramtop.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/kdebug.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#ifndef SZOMB
#define SZOMB 5
#endif

/* Which coalitions a process belongs to (bsd/sys/proc_info.h, private) */
#ifndef PROC_PIDCOALITIONINFO
#define PROC_PIDCOALITIONINFO 20
struct proc_pidcoalitioninfo {
    uint64_t coalition_id[2]; /* resource, jetsam */
    uint64_t reserved1, reserved2, reserved3;
};
#endif

/* struct coalition_resource_usage (osfmk/mach/coalition.h) read as 64-bit fields. Older
 * kernels fill in fewer of them and leave the rest zero. */
#define COAL_FIELDS    45
#define COAL_ANE_TIME  38 /* ane_mach_time, in mach time units */
#define MAX_COALS      64

#define MAX_GROUPS   8
#define MAX_CPUS     64
#define PMI_PERIOD   35000000ULL /* active cycles between overflow samples, about 10 ms */
#define KD_RECORDS   (1 << 18)
#define ACC_SLOTS    4096      /* power of two */
#define MAX_COUNTERS 32

/* Per-cluster regression of DRAM bytes on L1D events over the last WINDOW slices */
#define SLICE_NS    100000000L
#define WINDOW      200
#define SLICE_PIDS  16   /* busiest processes of a cluster kept per slice; the rest are pooled */
#define REG_VARS    12   /* processes with a coefficient of their own */
#define MIN_SHARE   0.02 /* ... if they have at least this share of the group's events in the window */
#define MIN_SLICES  20
#define RIDGE       0.02

/* kperf and kdebug values from XNU (osfmk/kperf/action.h, buffer.h) */
#define KPERF_SAMPLER_TH_INFO (1U << 0)
#define KPERF_SAMPLER_PMC_CPU (1U << 5)
#define PERF_THREADINFO       1
#define PERF_TI_DATA          1
#define PERF_KPC              6
#define PERF_KPC_DATA         3
#define KDBG_VALCHECK_        0x00200000U
#define KDBG_NOWRAP_          0x2 /* stop tracing when the buffer fills (bsd/sys/kdebug_private.h) */
#define KPC_CONFIGURABLE      2

typedef struct {
    unsigned int type, value1, value2, value3, value4;
} kdreg_t;

typedef struct {
    int nkdbufs, nolog; /* nolog: tracing is off, e.g. because the buffer filled */
    unsigned int flags;
    int nkdthreads, bufid;
} kdbufinfo_t;

/* Context switches, traced by the thread being switched out (bsd/sys/kdebug.h). A kperf sample on
 * each charges a CPU's counts up to the switch to the right thread, as Linux per-task counters do. */
#define MACH_SCHED_ID         ((uint32_t)MACHDBG_CODE(DBG_MACH_SCHED, MACH_SCHED))
#define MACH_HANDOFF_ID       ((uint32_t)MACHDBG_CODE(DBG_MACH_SCHED, MACH_STACK_HANDOFF))
#define KPERF_FILTER_ID       4 /* kperf trigger filter entry matching one event ID */

enum { AG_PCPU, AG_ECPU, AG_GPU, AG_ANE, AG_MEDIA, AG_DISPLAY, AG_OTHER, AG_N };
static const char *const AGENT_NAME[AG_N] = {"P-CPU", "E-CPU", "GPU", "ANE", "Media", "Display", "Other"};

enum { EV_LD, EV_ST, EV_WB, EV_N };
static const char *const EVENT_NAME[EV_N] = {"L1D_CACHE_MISS_LD", "L1D_CACHE_MISS_ST", "L1D_CACHE_WRITEBACK"};
static const char TRIGGER_EVENT[] = "CORE_ACTIVE_CYCLE";

typedef struct IOReportSubscription *IOReportSubscriptionRef;

typedef struct {
    uint64_t timestamp, arg1, arg2, arg3, arg4, arg5;
    uint32_t debugid, cpuid;
    uint64_t unused;
} kdbuf_t;

/* One "<agent> DCS RD|WR" channel of the AMC Stats group. */
typedef struct {
    char name[48];
    int agent; /* AG_*, or -1 for the chip totals */
    int group; /* CPU cluster group, or -1 */
    bool write;
} chan_t;

typedef struct {
    double rd[AG_N], wr[AG_N];               /* bytes per agent */
    double grd[MAX_GROUPS], gwr[MAX_GROUPS]; /* bytes per CPU group */
    double rd_all, wr_all;                   /* bytes of the whole chip */
} amc_t;

typedef struct {
    bool used;
    int pid; /* 0: kernel threads */
    double ev[MAX_GROUPS][EV_N];
} acc_t;

typedef struct {
    acc_t slot[ACC_SLOTS];
} accmap_t;

/* One SLICE_NS slice: each CPU group's DRAM bytes and its busiest processes' L1D events. */
typedef struct {
    float y[MAX_GROUPS][2];    /* bytes read, written */
    float rest[MAX_GROUPS][2]; /* misses and write-backs of the processes not listed */
    struct {
        int pid;
        float x[2]; /* L1D misses (loads and stores), write-backs */
    } e[MAX_GROUPS][SLICE_PIDS];
    unsigned char n[MAX_GROUPS];
} slice_t;

/* Bytes per event for each of a group's busiest processes, and one for all others. */
typedef struct {
    bool ok;
    int n;
    int pid[REG_VARS];
    double beta[REG_VARS], rest;
} fit_t;

typedef struct {
    dt_proc_t pub;
    uint64_t start;                          /* start time in microseconds, to spot PID reuse */
    uint64_t cpu, cycles, insns, energy, gpu; /* cumulative values at the last scan */
    uint64_t neural;                         /* current Neural Engine memory footprint */
    double dgpu;                             /* GPU nanoseconds during the last interval */
    double ane_share;                        /* of the Neural Engine's traffic in the last interval */
    bool seen, counted, primed;
} proc_t;

/* A resource coalition (an app and its helper processes) with Neural Engine memory. */
typedef struct {
    uint64_t id, ane; /* coalition ID, its Neural Engine time so far */
    double mem, dt;   /* its processes' Neural Engine memory, its Neural Engine time in the interval */
} coal_t;

static struct {
    CFDictionaryRef (*copy_group)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
    IOReportSubscriptionRef (*subscribe)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
    CFDictionaryRef (*samples)(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
    CFDictionaryRef (*delta)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
    int64_t (*value)(CFDictionaryRef, int32_t *);
    CFStringRef (*name)(CFDictionaryRef);
} ior;

static struct {
    int (*force_all_ctrs_set)(int);
    int (*force_all_ctrs_get)(int *);
    int (*set_config)(uint32_t, uint64_t *);
    int (*set_counting)(uint32_t);
    int (*set_period)(uint32_t, uint64_t *);
    int (*set_actionid)(uint32_t, uint32_t *);
    uint32_t (*config_count)(uint32_t);
    uint32_t (*counter_count)(uint32_t);
    int (*action_count_set)(uint32_t);
    int (*action_samplers_set)(uint32_t, uint32_t);
    int (*sample_set)(uint32_t);
    int (*reset)(void);
    int (*db_create)(const char *, void **);
    int (*db_event)(void *, const char *, void **);
    int (*config_create)(void *, void **);
    int (*config_force_counters)(void *);
    int (*config_add_event)(void *, void **, uint32_t, uint32_t *);
    int (*config_add_trigger)(void *, void **, uint32_t, uint64_t, uint32_t *);
    int (*config_classes)(void *, uint32_t *);
    int (*config_kpc_count)(void *, size_t *);
    int (*config_kpc_map)(void *, size_t *, size_t);
    int (*config_kpc)(void *, uint64_t *, size_t);
    int (*config_periods)(void *, uint64_t *, size_t);

    bool on, forced, tracing, stop, cswitch;
    const char *why; /* reason per-process estimates are off */
    uint32_t classes, nctr;
    size_t map[EV_N];
    pthread_t thread;
    pthread_mutex_t mu;
    kdbuf_t *buf;
    size_t nbuf; /* records the trace buffer and buf hold */
    accmap_t *slice, *ivl; /* events of the current slice, of the current sampling interval */
    slice_t *ring;
    int ring_n, ring_head;
    amc_t ivl_amc;    /* AMC bytes of the current sampling interval */
    double ivl_start; /* when the AMC reading that began the interval was taken */
    int cpupid[MAX_CPUS], have[MAX_CPUS];
    bool cpuseen[MAX_CPUS];
    uint64_t now[MAX_CPUS][MAX_COUNTERS], last[MAX_CPUS][MAX_COUNTERS];
    char why_buf[96];
} kp = {.mu = PTHREAD_MUTEX_INITIALIZER, .why = "need sudo"};

static struct {
    dt_config_t cfg;
    pid_t self;
    double tick_ns;
    char cpu_name[96], backend[192], scope[96];
    int ncpus;
    unsigned line;
    double spec;

    IOReportSubscriptionRef sub;
    CFMutableDictionaryRef subbed;
    CFDictionaryRef prev, probe;
    double prev_at; /* when prev was read */
    chan_t chan[128];
    int nchan;

    int ngroups;
    char group_name[MAX_GROUPS][16];
    int group_agent[MAX_GROUPS]; /* AG_PCPU or AG_ECPU */
    int cpu_group[MAX_CPUS];

    int (*coal_usage)(uint64_t id, uint64_t *usage, size_t size); /* coalition_info_resource_usage */
    coal_t coal[MAX_COALS];
    int ncoal;

    proc_t *procs;
    int nprocs, cap;
    dt_proc_t **list;
    int list_cap;
    int *pids;
    int pids_cap;
    struct gpu_use {
        int pid;
        uint64_t ns;
    } gpu[512], gpu_prev[512];
    int ngpu, ngpu_prev;
    uint64_t prev_scan_us;
} g;

const char plat_name[] = "macOS";

double gpu_probe_gbs(unsigned long long max_bytes); /* macos_metal.m */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool sysctl_str(const char *name, char *buf, size_t n) {
    size_t len = n;
    if (sysctlbyname(name, buf, &len, NULL, 0) || len == 0) return false;
    buf[n - 1] = '\0';
    return true;
}

static long long sysctl_ll(const char *name, long long fallback) {
    long long v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0)) return fallback;
    if (len == sizeof(int)) return *(int *)&v;
    return v;
}

static const char *cstr(CFStringRef s, char *buf, size_t n) {
    buf[0] = '\0';
    if (s) CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8);
    return buf;
}

static int64_t cf_int(CFTypeRef v) {
    int64_t x = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberSInt64Type, &x);
    if (v && CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength(v) >= 4) {
        uint32_t u;
        memcpy(&u, CFDataGetBytePtr(v), 4);
        x = u;
    }
    return x;
}

static double mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int kd(int op, int arg, void *buf, size_t *len) {
    int mib[4] = {CTL_KERN, KERN_KDEBUG, op, arg};
    return sysctl(mib, arg >= 0 ? 4 : 3, buf, len, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Chip detection                                                      */
/* ------------------------------------------------------------------ */

static int gpu_cores(void) {
    io_service_t acc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AGXAccelerator"));
    if (!acc) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(acc, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
    int n = (int)cf_int(v);
    if (v) CFRelease(v);
    IOObjectRelease(acc);
    return n;
}

/* Peak DRAM bandwidth of each chip: bus width times LPDDR transfer rate. */
static double spec_of(const char *chip, int gpu) {
    static const struct {
        const char *name;
        int min_gpu;
        double gbs;
    } T[] = {
        {"Apple M1", 0, 68.25},       {"Apple M1 Pro", 0, 204.8},   {"Apple M1 Max", 0, 409.6},
        {"Apple M1 Ultra", 0, 819.2}, {"Apple M2", 0, 102.4},       {"Apple M2 Pro", 0, 204.8},
        {"Apple M2 Max", 0, 409.6},   {"Apple M2 Ultra", 0, 819.2}, {"Apple M3", 0, 102.4},
        {"Apple M3 Pro", 0, 153.6},   {"Apple M3 Max", 0, 307.2},   {"Apple M3 Max", 40, 409.6},
        {"Apple M3 Ultra", 0, 819.2}, {"Apple M4", 0, 120.0},       {"Apple M4 Pro", 0, 273.1},
        {"Apple M4 Max", 0, 409.6},   {"Apple M4 Max", 40, 546.1},  {"Apple M5", 0, 153.6},
    };
    double best = 0;
    int best_gpu = -1;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(chip, T[i].name) && gpu >= T[i].min_gpu && T[i].min_gpu > best_gpu) {
            best = T[i].gbs;
            best_gpu = T[i].min_gpu;
        }
    return best;
}

static void detect_chip(void) {
    char chip[64] = "Apple Silicon", level[32], key[64];
    sysctl_str("machdep.cpu.brand_string", chip, sizeof chip);
    sanitize(chip);
    int gpu = gpu_cores();
    g.spec = spec_of(chip, gpu);
    g.ncpus = (int)sysctl_ll("hw.logicalcpu", 1);
    g.line = (unsigned)sysctl_ll("hw.cachelinesize", 128);

    /* "Apple M2 (4P+4E, 10-core GPU)" */
    char cores[48] = "";
    int nlevels = (int)sysctl_ll("hw.nperflevels", 0), k = 0;
    for (int i = 0; i < nlevels && i < 4; i++) {
        snprintf(key, sizeof key, "hw.perflevel%d.name", i);
        if (!sysctl_str(key, level, sizeof level)) snprintf(level, sizeof level, "?");
        snprintf(key, sizeof key, "hw.perflevel%d.physicalcpu", i);
        k += snprintf(cores + k, sizeof cores - (size_t)k, "%s%lld%c", i ? "+" : "", sysctl_ll(key, 0), level[0]);
        if (k >= (int)sizeof cores) break;
    }
    if (gpu > 0) snprintf(g.cpu_name, sizeof g.cpu_name, "%s (%s, %d-core GPU)", chip, cores, gpu);
    else snprintf(g.cpu_name, sizeof g.cpu_name, "%s (%s)", chip, cores);
}

/* Which AMC CPU group each logical CPU belongs to, from the device tree. */
static void map_cpus(void) {
    struct {
        int cpu, cluster;
        char type;
    } c[MAX_CPUS];
    int n = 0;
    for (int i = 0; i < MAX_CPUS; i++) g.cpu_group[i] = -1;
    io_registry_entry_t cpus = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/cpus");
    if (!cpus) return;
    io_iterator_t it;
    if (IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &it) == KERN_SUCCESS) {
        io_registry_entry_t e;
        while ((e = IOIteratorNext(it)) && n < MAX_CPUS) {
            CFTypeRef id = IORegistryEntryCreateCFProperty(e, CFSTR("logical-cpu-id"), kCFAllocatorDefault, 0);
            CFTypeRef cl = IORegistryEntryCreateCFProperty(e, CFSTR("logical-cluster-id"), kCFAllocatorDefault, 0);
            CFTypeRef ty = IORegistryEntryCreateCFProperty(e, CFSTR("cluster-type"), kCFAllocatorDefault, 0);
            if (id && cl && ty && CFGetTypeID(ty) == CFDataGetTypeID() && CFDataGetLength(ty) > 0) {
                c[n].cpu = (int)cf_int(id);
                c[n].cluster = (int)cf_int(cl);
                c[n].type = (char)CFDataGetBytePtr(ty)[0];
                n++;
            }
            if (id) CFRelease(id);
            if (cl) CFRelease(cl);
            if (ty) CFRelease(ty);
            IOObjectRelease(e);
        }
        IOObjectRelease(it);
    }
    IOObjectRelease(cpus);

    /* The k-th cluster of a type goes to the k-th AMC prefix of that type, or to the only one. */
    for (int i = 0; i < n; i++) {
        if (c[i].cpu < 0 || c[i].cpu >= MAX_CPUS) continue;
        int clusters = 0, rank = 0, seen[MAX_CPUS], ns = 0;
        for (int j = 0; j < n; j++) {
            if (c[j].type != c[i].type) continue;
            bool dup = false;
            for (int s = 0; s < ns; s++) dup |= seen[s] == c[j].cluster;
            if (dup) continue;
            seen[ns++] = c[j].cluster;
            clusters++;
            if (c[j].cluster < c[i].cluster) rank++;
        }
        int match[MAX_GROUPS], nm = 0;
        for (int k = 0; k < g.ngroups; k++)
            if ((g.group_agent[k] == AG_ECPU ? 'E' : 'P') == c[i].type) match[nm++] = k;
        if (nm == 1) g.cpu_group[c[i].cpu] = match[0];
        else if (nm == clusters && rank < nm) g.cpu_group[c[i].cpu] = match[rank];
    }
}

/* ------------------------------------------------------------------ */
/* Memory controller counters (IOReport "AMC Stats")                   */
/* ------------------------------------------------------------------ */

static int classify(const char *p) {
    static const char *const media[] = {"VDEC", "VENC", "AVD", "AVE", "SCODEC", "ISP", "JPG", "JPEG", "MSR", "PRORES"};
    const char *sp = strchr(p, ' ');
    if (!strncmp(p, "DIE", 3) && sp) p = sp + 1; /* "DIE1 PCPU0" on chips built from two dies */
    const char *cpu = strstr(p, "CPU");
    if (cpu) return cpu > p && cpu[-1] == 'E' ? AG_ECPU : AG_PCPU;
    if (!strncmp(p, "GFX", 3) || !strncmp(p, "AGX", 3)) return AG_GPU;
    if (!strncmp(p, "ANE", 3)) return AG_ANE;
    if (!strncmp(p, "DISP", 4)) return AG_DISPLAY;
    for (size_t i = 0; i < sizeof media / sizeof media[0]; i++)
        if (!strncmp(p, media[i], strlen(media[i]))) return AG_MEDIA;
    return AG_OTHER;
}

static bool load_ioreport(void) {
    void *h = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!h) return false;
    *(void **)&ior.copy_group = dlsym(h, "IOReportCopyChannelsInGroup");
    *(void **)&ior.subscribe = dlsym(h, "IOReportCreateSubscription");
    *(void **)&ior.samples = dlsym(h, "IOReportCreateSamples");
    *(void **)&ior.delta = dlsym(h, "IOReportCreateSamplesDelta");
    *(void **)&ior.value = dlsym(h, "IOReportSimpleGetIntegerValue");
    *(void **)&ior.name = dlsym(h, "IOReportChannelGetChannelName");
    return ior.copy_group && ior.subscribe && ior.samples && ior.delta && ior.value && ior.name;
}

/* "<agent> DCS RD|WR" and the chip totals "DCS RD|WR": bytes at the DRAM controllers. */
static bool is_dcs(const char *name, bool *write) {
    size_t len = strlen(name);
    *write = len >= 6 && !strcmp(name + len - 6, "DCS WR");
    return *write || (len >= 6 && !strcmp(name + len - 6, "DCS RD"));
}

static bool amc_open(void) {
    if (!load_ioreport()) return false;
    CFDictionaryRef ch = ior.copy_group(CFSTR("AMC Stats"), NULL, 0, 0, 0);
    if (!ch) return false;
    CFArrayRef all = CFDictionaryGetValue(ch, CFSTR("IOReportChannels"));
    CFMutableArrayRef keep = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    char name[64];
    bool wr;
    for (CFIndex i = 0; all && CFGetTypeID(all) == CFArrayGetTypeID() && i < CFArrayGetCount(all); i++) {
        CFDictionaryRef c = CFArrayGetValueAtIndex(all, i);
        if (is_dcs(cstr(ior.name(c), name, sizeof name), &wr)) CFArrayAppendValue(keep, c);
    }
    CFMutableDictionaryRef want = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, ch);
    CFDictionarySetValue(want, CFSTR("IOReportChannels"), keep); /* fewer channels make each reading quicker */
    CFRelease(keep);
    CFRelease(ch);
    g.sub = ior.subscribe(NULL, want, &g.subbed, 0, NULL);
    CFRelease(want);
    if (!g.sub || !g.subbed) return false;

    CFArrayRef arr = CFDictionaryGetValue(g.subbed, CFSTR("IOReportChannels"));
    int ncpu = 0;
    bool totals = false;
    for (CFIndex i = 0; arr && i < CFArrayGetCount(arr) && g.nchan < (int)(sizeof g.chan / sizeof g.chan[0]); i++) {
        if (!is_dcs(cstr(ior.name(CFArrayGetValueAtIndex(arr, i)), name, sizeof name), &wr)) continue;
        size_t len = strlen(name);
        chan_t *c = &g.chan[g.nchan++];
        copy_str(c->name, sizeof c->name, name);
        c->write = wr;
        c->group = -1;
        if (len == 6) { /* "DCS RD" / "DCS WR": the whole chip */
            c->agent = -1;
            totals = true;
            continue;
        }
        char prefix[48], extra;
        int die;
        copy_str(prefix, sizeof prefix, name);
        prefix[len - 7] = '\0';
        if (sscanf(prefix, "DIE%d%c", &die, &extra) == 1) { /* "DIE0 DCS RD": all of one die */
            c->agent = -1;
            totals = true;
            continue;
        }
        c->agent = classify(prefix);
        if (c->agent == AG_PCPU || c->agent == AG_ECPU) {
            int k = 0;
            while (k < ncpu && strcmp(g.group_name[k], prefix)) k++;
            if (k == ncpu && ncpu < MAX_GROUPS) {
                copy_str(g.group_name[ncpu], sizeof g.group_name[0], prefix);
                g.group_agent[ncpu++] = c->agent;
            }
            c->group = k < MAX_GROUPS ? k : -1;
        }
    }
    if (!totals) return false;
    g.ngroups = ncpu;
    map_cpus();
    g.prev = ior.samples(g.sub, g.subbed, NULL);
    g.prev_at = mono_sec();
    return g.prev != NULL;
}

static void amc_add(amc_t *to, const amc_t *m) {
    for (int a = 0; a < AG_N; a++) to->rd[a] += m->rd[a], to->wr[a] += m->wr[a];
    for (int k = 0; k < MAX_GROUPS; k++) to->grd[k] += m->grd[k], to->gwr[k] += m->gwr[k];
    to->rd_all += m->rd_all;
    to->wr_all += m->wr_all;
}

/* Bytes since *base, which then moves to now. Returns when the new reading was taken: the
 * time right after each call, so that the time a call takes does not count as traffic time. */
static double amc_read(CFDictionaryRef *base, amc_t *out) {
    memset(out, 0, sizeof *out);
    CFDictionaryRef cur = ior.samples(g.sub, g.subbed, NULL);
    double at = mono_sec();
    if (!cur) return at;
    CFDictionaryRef d = *base ? ior.delta(*base, cur, NULL) : NULL;
    if (*base) CFRelease(*base);
    *base = cur;
    if (!d) return at;
    CFArrayRef arr = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
    char name[64];
    double known_rd = 0, known_wr = 0;
    for (CFIndex i = 0; arr && i < CFArrayGetCount(arr); i++) {
        CFDictionaryRef ch = CFArrayGetValueAtIndex(arr, i);
        cstr(ior.name(ch), name, sizeof name);
        const chan_t *c = NULL;
        for (int k = 0; k < g.nchan && !c; k++)
            if (!strcmp(g.chan[k].name, name)) c = &g.chan[k];
        if (!c) continue;
        double v = (double)ior.value(ch, NULL);
        if (v < 0) v = 0;
        if (c->agent < 0) {
            if (c->write) out->wr_all += v;
            else out->rd_all += v;
            continue;
        }
        if (c->write) {
            out->wr[c->agent] += v;
            known_wr += v;
        } else {
            out->rd[c->agent] += v;
            known_rd += v;
        }
        if (c->group >= 0) {
            if (c->write) out->gwr[c->group] += v;
            else out->grd[c->group] += v;
        }
    }
    CFRelease(d);
    /* Requesters without their own channel show up only in the totals. */
    if (out->rd_all > known_rd) out->rd[AG_OTHER] += out->rd_all - known_rd;
    if (out->wr_all > known_wr) out->wr[AG_OTHER] += out->wr_all - known_wr;
    return at;
}

/* ------------------------------------------------------------------ */
/* Per-process CPU counter sampling (kperf + kdebug, root only)        */
/* ------------------------------------------------------------------ */

static acc_t *acc_get(accmap_t *m, int pid, bool add) {
    unsigned h = (unsigned)pid * 2654435761u;
    for (unsigned i = 0; i < ACC_SLOTS; i++) {
        acc_t *a = &m->slot[(h + i) & (ACC_SLOTS - 1)];
        if (a->used && a->pid == pid) return a;
        if (!a->used) {
            if (!add) return NULL;
            a->used = true;
            a->pid = pid;
            return a;
        }
    }
    return NULL;
}

/* The CPU counters' change since the previous sample on the same CPU is charged to the
 * process that was running when the sample was taken: at a counter overflow, or at a
 * context switch, where kperf samples the thread leaving the CPU. */
static void kp_parse(const kdbuf_t *b, size_t n, accmap_t *m) {
    const uint32_t ti = (uint32_t)KDBG_EVENTID(DBG_PERF, PERF_THREADINFO, PERF_TI_DATA);
    const uint32_t pmc = (uint32_t)KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA);
    for (size_t i = 0; i < n; i++, b++) {
        uint32_t cpu = b->cpuid, id = b->debugid & ~(uint32_t)KDBG_FUNC_MASK;
        if (cpu >= MAX_CPUS) continue;
        if (id == ti) {
            kp.cpupid[cpu] = (int)b->arg1;
            continue;
        }
        if (id != pmc) continue;
        if ((b->debugid & KDBG_FUNC_MASK) == DBG_FUNC_START) kp.have[cpu] = 0;
        if (kp.have[cpu] < 0) continue;
        const uint64_t v[4] = {b->arg1, b->arg2, b->arg3, b->arg4};
        for (int k = 0; k < 4 && kp.have[cpu] < (int)kp.nctr; k++) kp.now[cpu][kp.have[cpu]++] = v[k];
        if (kp.have[cpu] < (int)kp.nctr) continue;
        kp.have[cpu] = -1;
        int grp = g.cpu_group[cpu], pid = kp.cpupid[cpu];
        if (kp.cpuseen[cpu] && grp >= 0 && pid >= 0) {
            acc_t *a = acc_get(m, pid, true);
            for (int e = 0; a && e < EV_N; e++) {
                uint64_t now = kp.now[cpu][kp.map[e]], was = kp.last[cpu][kp.map[e]];
                if (now >= was) a->ev[grp][e] += (double)(now - was);
            }
        }
        kp.cpuseen[cpu] = true;
        memcpy(kp.last[cpu], kp.now[cpu], sizeof kp.last[cpu]);
    }
}

/* Ends the current slice: its events move to the interval totals and, for the
 * busiest processes of each CPU group, into the regression window. */
static void slice_close(void) {
    size_t len = sizeof(kdbuf_t) * kp.nbuf; /* bytes in, records out */
    if (kd(KERN_KDREADTR, -1, kp.buf, &len) == 0) kp_parse(kp.buf, len, kp.slice);
    /* The trace stops when its buffer fills (KDBG_NOWRAP); once read, it goes back on. */
    kdbufinfo_t bi;
    size_t bl = sizeof bi;
    if (kd(KERN_KDGETBUF, -1, &bi, &bl) == 0 && bi.nolog) kd(KERN_KDENABLE, 1, NULL, NULL);
    amc_t m;
    g.prev_at = amc_read(&g.prev, &m);
    amc_add(&kp.ivl_amc, &m);

    slice_t *s = &kp.ring[kp.ring_head];
    memset(s, 0, sizeof *s);
    for (int k = 0; k < g.ngroups; k++) {
        s->y[k][0] = (float)m.grd[k];
        s->y[k][1] = (float)m.gwr[k];
    }
    for (int i = 0; i < ACC_SLOTS; i++) {
        acc_t *a = &kp.slice->slot[i];
        if (!a->used) continue;
        acc_t *t = acc_get(kp.ivl, a->pid, true);
        for (int k = 0; k < g.ngroups; k++) {
            float x[2] = {(float)(a->ev[k][EV_LD] + a->ev[k][EV_ST]), (float)a->ev[k][EV_WB]};
            if (t)
                for (int e = 0; e < EV_N; e++) t->ev[k][e] += a->ev[k][e];
            if (x[0] <= 0 && x[1] <= 0) continue;
            int n = s->n[k], low = 0;
            if (n == SLICE_PIDS) { /* keep the busiest; the smallest goes to the pool */
                for (int j = 1; j < n; j++)
                    if (s->e[k][j].x[0] < s->e[k][low].x[0]) low = j;
                if (s->e[k][low].x[0] >= x[0]) {
                    s->rest[k][0] += x[0];
                    s->rest[k][1] += x[1];
                    continue;
                }
                s->rest[k][0] += s->e[k][low].x[0];
                s->rest[k][1] += s->e[k][low].x[1];
                n = low;
            } else {
                s->n[k]++;
            }
            s->e[k][n].pid = a->pid;
            s->e[k][n].x[0] = x[0];
            s->e[k][n].x[1] = x[1];
        }
        memset(a, 0, sizeof *a);
    }
    kp.ring_head = (kp.ring_head + 1) % WINDOW;
    if (kp.ring_n < WINDOW) kp.ring_n++;
}

/* Slices must not keep step with periodic work such as the 10 ms scheduler quantum: on an exact
 * grid, one process's samples could keep landing just before the boundary and another's just
 * after, which skews the regression. Sleeping SLICE_NS after each slice lets the phase drift. */
static void *kp_reader(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = {0, SLICE_NS};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&kp.mu);
        bool stop = kp.stop;
        if (!stop) slice_close();
        pthread_mutex_unlock(&kp.mu);
        if (stop) return NULL;
    }
}

/* Non-negative least squares over the window, y = sum(beta_p * x_p), by coordinate
 * descent, for the processes with at least MIN_SHARE of a group's events. All
 * others keep the pooled bytes-per-event ratio, which is plain proportional
 * sharing; a light ridge towards it settles processes whose events always rose
 * and fell together, which the data cannot tell apart.
 * w selects reads (0: bytes on L1D misses) or writes (1: bytes on write-backs). */
static void fit_group(int k, int w, fit_t *f) {
    memset(f, 0, sizeof *f);
    if (kp.ring_n < MIN_SLICES) return;

    struct {
        int pid;
        double sum;
    } cand[256];
    int nc = 0;
    for (int t = 0; t < kp.ring_n; t++) {
        const slice_t *s = &kp.ring[t];
        for (int j = 0; j < s->n[k]; j++) {
            int c = 0;
            while (c < nc && cand[c].pid != s->e[k][j].pid) c++;
            if (c == nc) {
                if (nc == (int)(sizeof cand / sizeof cand[0])) continue;
                cand[nc].pid = s->e[k][j].pid;
                cand[nc++].sum = 0;
            }
            cand[c].sum += s->e[k][j].x[w];
        }
    }
    double sy = 0, sx = 0;
    for (int t = 0; t < kp.ring_n; t++) {
        sy += kp.ring[t].y[k][w];
        sx += kp.ring[t].rest[k][w];
    }
    for (int c = 0; c < nc; c++) sx += cand[c].sum;
    if (sx <= 0) return;
    double prior = sy / sx;
    for (int i = 0; i < nc && f->n < REG_VARS; i++) {
        int best = i;
        for (int c = i + 1; c < nc; c++)
            if (cand[c].sum > cand[best].sum) best = c;
        if (cand[best].sum < MIN_SHARE * sx) break;
        int pid = cand[best].pid;
        cand[best] = cand[i];
        f->pid[f->n++] = pid;
    }

    /* y minus what the pooled ratio gives everyone without a coefficient */
    int v = f->n;
    double ata[REG_VARS][REG_VARS] = {{0}}, aty[REG_VARS] = {0}, beta[REG_VARS];
    for (int t = 0; t < kp.ring_n; t++) {
        const slice_t *s = &kp.ring[t];
        double x[REG_VARS] = {0}, y = s->y[k][w] - prior * s->rest[k][w];
        for (int j = 0; j < s->n[k]; j++) {
            int c = 0;
            while (c < v && f->pid[c] != s->e[k][j].pid) c++;
            if (c < v) x[c] += s->e[k][j].x[w];
            else y -= prior * s->e[k][j].x[w];
        }
        for (int i = 0; i < v; i++) {
            aty[i] += x[i] * y;
            for (int j = 0; j < v; j++) ata[i][j] += x[i] * x[j];
        }
    }
    for (int i = 0; i < v; i++) {
        double d = ata[i][i];
        ata[i][i] += RIDGE * d;
        aty[i] += RIDGE * d * prior;
        beta[i] = prior;
    }
    for (int it = 0; it < 200; it++)
        for (int i = 0; i < v; i++) {
            if (ata[i][i] <= 0) continue;
            double r = aty[i];
            for (int j = 0; j < v; j++)
                if (j != i) r -= ata[i][j] * beta[j];
            beta[i] = r > 0 ? r / ata[i][i] : 0;
        }
    for (int i = 0; i < v; i++) f->beta[i] = beta[i];
    f->rest = prior;
    f->ok = true;
}

static double beta_of(const fit_t *f, int pid) {
    if (!f->ok) return 1.0;
    for (int i = 0; i < f->n; i++)
        if (f->pid[i] == pid) return f->beta[i];
    return f->rest;
}

static void kp_stop(void) {
    if (kp.thread) {
        pthread_mutex_lock(&kp.mu);
        kp.stop = true;
        pthread_mutex_unlock(&kp.mu);
        pthread_join(kp.thread, NULL);
        kp.thread = 0;
    }
    if (kp.tracing) {
        kd(KERN_KDENABLE, 0, NULL, NULL);
        kd(KERN_KDREMOVE, -1, NULL, NULL);
        kp.tracing = false;
    }
    if (kp.forced) {
        uint64_t zero[MAX_COUNTERS] = {0};
        uint32_t none[MAX_COUNTERS] = {0};
        int off = 0;
        kp.sample_set(0);
        if (kp.cswitch) sysctlbyname("kperf.kdebug.action", NULL, NULL, &off, sizeof off);
        kp.cswitch = false;
        kp.reset();
        kp.set_counting(0);
        kp.set_period(KPC_CONFIGURABLE, zero);
        kp.set_actionid(KPC_CONFIGURABLE, none);
        kp.set_config(KPC_CONFIGURABLE, zero);
        kp.force_all_ctrs_set(0);
        kp.forced = false;
    }
    kp.on = false;
}

#define KSYM(h, f, name) (*(void **)&kp.f = dlsym(h, name))

static bool kp_load(void) {
    void *k = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    void *d = dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_LAZY);
    return k && d && KSYM(k, force_all_ctrs_set, "kpc_force_all_ctrs_set") &&
           KSYM(k, force_all_ctrs_get, "kpc_force_all_ctrs_get") && KSYM(k, set_config, "kpc_set_config") &&
           KSYM(k, set_counting, "kpc_set_counting") && KSYM(k, set_period, "kpc_set_period") &&
           KSYM(k, set_actionid, "kpc_set_actionid") && KSYM(k, config_count, "kpc_get_config_count") &&
           KSYM(k, counter_count, "kpc_get_counter_count") && KSYM(k, action_count_set, "kperf_action_count_set") &&
           KSYM(k, action_samplers_set, "kperf_action_samplers_set") && KSYM(k, sample_set, "kperf_sample_set") &&
           KSYM(k, reset, "kperf_reset") && KSYM(d, db_create, "kpep_db_create") && KSYM(d, db_event, "kpep_db_event") &&
           KSYM(d, config_create, "kpep_config_create") &&
           KSYM(d, config_force_counters, "kpep_config_force_counters") &&
           KSYM(d, config_add_event, "kpep_config_add_event") &&
           KSYM(d, config_add_trigger, "kpep_config_add_event_trigger") &&
           KSYM(d, config_classes, "kpep_config_kpc_classes") && KSYM(d, config_kpc_count, "kpep_config_kpc_count") &&
           KSYM(d, config_kpc_map, "kpep_config_kpc_map") && KSYM(d, config_kpc, "kpep_config_kpc") &&
           KSYM(d, config_periods, "kpep_config_kpc_periods");
}

static bool kp_fail(const char *why) {
    kp_stop();
    kp.why = why;
    return false;
}

/* Samples on context switches too, as action 1 does on counter overflows. */
static bool kp_cswitch(void) {
    struct {
        uint64_t types[2];
        uint32_t ids[2];
    } __attribute__((packed)) f = {{KPERF_FILTER_ID | KPERF_FILTER_ID << 4, 0}, {MACH_SCHED_ID, MACH_HANDOFF_ID}};
    int action = 1;
    return !sysctlbyname("kperf.kdebug.filter", NULL, NULL, &f, sizeof f) &&
           !sysctlbyname("kperf.kdebug.action", NULL, NULL, &action, sizeof action);
}

static bool kp_start(void) {
    if (geteuid() != 0) return false;
    if (g.ngroups == 0) return kp_fail("unknown CPU clusters");
    if (!kp_load()) return kp_fail("kperf not found");
    int forced = 0;
    if (kp.force_all_ctrs_get(&forced)) return kp_fail("no access to CPU counters");
    if (forced) return kp_fail("CPU counters in use by another tool");

    void *db = NULL, *cfg = NULL, *ev = NULL;
    if (kp.db_create(NULL, &db) || kp.config_create(db, &cfg) || kp.config_force_counters(cfg))
        return kp_fail("no PMU event database for this CPU");
    for (int e = 0; e < EV_N; e++)
        if (kp.db_event(db, EVENT_NAME[e], &ev) || kp.config_add_event(cfg, &ev, 0, NULL))
            return kp_fail("CPU lacks the L1D miss events");
    if (kp.db_event(db, TRIGGER_EVENT, &ev) || kp.config_add_trigger(cfg, &ev, 0, PMI_PERIOD, NULL))
        return kp_fail("CPU lacks a cycle event");

    size_t nreg = 0, map[MAX_COUNTERS] = {0};
    uint64_t regs[MAX_COUNTERS] = {0}, periods[MAX_COUNTERS] = {0};
    uint32_t actions[MAX_COUNTERS] = {0};
    if (kp.config_classes(cfg, &kp.classes) || kp.config_kpc_count(cfg, &nreg) || nreg > MAX_COUNTERS ||
        kp.config_kpc_map(cfg, map, sizeof map) || kp.config_kpc(cfg, regs, sizeof regs) ||
        kp.config_periods(cfg, periods, sizeof periods))
        return kp_fail("cannot configure CPU counters");
    for (int e = 0; e < EV_N; e++) kp.map[e] = map[e];
    for (size_t i = 0; i < nreg; i++) actions[i] = periods[i] ? 1 : 0;

    kp.forced = true;
    if (kp.force_all_ctrs_set(1) || kp.set_config(kp.classes, regs) || kp.set_period(KPC_CONFIGURABLE, periods) ||
        kp.set_actionid(KPC_CONFIGURABLE, actions))
        return kp_fail("cannot configure CPU counters");
    kp.nctr = kp.counter_count(kp.classes);
    if (kp.nctr == 0 || kp.nctr > MAX_COUNTERS) return kp_fail("no CPU counters");
    for (int e = 0; e < EV_N; e++)
        if (kp.map[e] >= kp.nctr) return kp_fail("cannot configure CPU counters");
    /* The action count can only grow, so use the maximum like other kperf clients. */
    if (kp.action_count_set(32) || kp.action_samplers_set(1, KPERF_SAMPLER_PMC_CPU | KPERF_SAMPLER_TH_INFO))
        return kp_fail("cannot start kperf sampling");
    kp.cswitch = kp_cswitch();
    if (kp.sample_set(1) || kp.set_counting(kp.classes))
        return kp_fail("cannot start kperf sampling");

    if (kd(KERN_KDREMOVE, -1, NULL, NULL)) {
        char who[64] = "";
        size_t n = sizeof who;
        if (sysctlbyname("ktrace.configured_by", who, &n, NULL, 0) || !who[0]) snprintf(who, sizeof who, "another tool");
        who[sizeof who - 1] = '\0';
        sanitize(who);
        snprintf(kp.why_buf, sizeof kp.why_buf, "kernel tracing in use by %s", who);
        return kp_fail(kp.why_buf);
    }
    kp.tracing = true;
    /* kperf only sees the events this filter lets into the trace, so the switches go in too. */
    kdreg_t reg = {KDBG_VALCHECK_, (unsigned)KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA),
                   (unsigned)KDBG_EVENTID(DBG_PERF, PERF_THREADINFO, PERF_TI_DATA), kp.cswitch ? MACH_SCHED_ID : 0,
                   kp.cswitch ? MACH_HANDOFF_ID : 0};
    size_t rl = sizeof reg;
    /* Trace and kperf settings belong to the whole system and outlive a dramtop killed with
     * SIGKILL or crashed: XNU releases the counters when the process exits, but resets the rest
     * only when another process configures tracing. Without wrapping, the leftover trace fills
     * up and stops within seconds (about 30 s on an idle machine), and the kperf samples it
     * triggers on every context switch stop with it.
     *
     * Known issue: until the next dramtop run (`sudo dramtop -n 1` is enough), another tracing
     * tool or a reboot, three things remain: the trace buffer (the kernel's minimum, about
     * 20 MB on an M2, however few records are asked for), the counter overflow samples (a few
     * per second when idle, about 0.02% of the CPU under full load with PMI_PERIOD), and the
     * pause of the system's background tracing (tailspin), which also pauses while dramtop
     * runs. Only a process that outlives dramtop could reset them. */
    kdbufinfo_t bi;
    size_t bl = sizeof bi;
    if (kd(KERN_KDSETBUF, KD_RECORDS, NULL, NULL) || kd(KERN_KDSETUP, -1, NULL, NULL) ||
        kd(KERN_KDSETREG, -1, &reg, &rl) || kd(KERN_KDEFLAGS, KDBG_NOWRAP_, NULL, NULL) ||
        kd(KERN_KDENABLE, 1, NULL, NULL) || kd(KERN_KDGETBUF, -1, &bi, &bl) || bi.nkdbufs <= 0)
        return kp_fail("cannot set up kernel tracing");

    kp.nbuf = (size_t)bi.nkdbufs;
    kp.buf = xrealloc(NULL, sizeof(kdbuf_t) * kp.nbuf);
    kp.slice = calloc(1, sizeof(accmap_t));
    kp.ivl = calloc(1, sizeof(accmap_t));
    kp.ring = calloc(WINDOW, sizeof(slice_t));
    if (!kp.slice || !kp.ivl || !kp.ring) return kp_fail("out of memory");
    for (int c = 0; c < MAX_CPUS; c++) kp.have[c] = -1;
    if (pthread_create(&kp.thread, NULL, kp_reader, NULL)) {
        kp.thread = 0;
        return kp_fail("cannot start a thread");
    }
    atexit(kp_stop);
    kp.on = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

static int proc_index(int pid) {
    int lo = 0, hi = g.nprocs;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g.procs[mid].pub.pid < pid) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static proc_t *proc_find(int pid) {
    int i = proc_index(pid);
    return i < g.nprocs && g.procs[i].pub.pid == pid ? &g.procs[i] : NULL;
}

static proc_t *proc_insert(int pid) {
    int i = proc_index(pid);
    if (g.nprocs == g.cap) {
        g.cap = g.cap ? g.cap * 2 : 512;
        g.procs = xrealloc(g.procs, (size_t)g.cap * sizeof *g.procs);
    }
    memmove(&g.procs[i + 1], &g.procs[i], (size_t)(g.nprocs - i) * sizeof *g.procs);
    g.nprocs++;
    memset(&g.procs[i], 0, sizeof g.procs[i]);
    g.procs[i].pub.pid = pid;
    return &g.procs[i];
}

static uint64_t delta(uint64_t now, uint64_t was) { return now >= was ? now - was : 0; }

static void visit(int pid) {
    struct proc_taskallinfo ti;
    if (pid <= 0 || pid == g.self) return;
    if (proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return;
    if (ti.pbsd.pbi_status == SZOMB) return;
    char name[NAME_LEN];
    copy_str(name, sizeof name, ti.pbsd.pbi_name[0] ? ti.pbsd.pbi_name : ti.pbsd.pbi_comm);
    sanitize(name);
    if (g.cfg.filter_name && !strstr(name, g.cfg.filter_name)) return;

    uint64_t start = (uint64_t)ti.pbsd.pbi_start_tvsec * 1000000u + ti.pbsd.pbi_start_tvusec;
    proc_t *p = proc_find(pid);
    if (p && p->start != start) { /* PID was reused */
        memset(p, 0, sizeof *p);
        p->pub.pid = pid;
    }
    if (!p) p = proc_insert(pid);
    bool fresh = !p->start;
    p->start = start;
    p->seen = true;
    copy_str(p->pub.name, sizeof p->pub.name, name);
    p->pub.threads = (int)ti.ptinfo.pti_threadnum;

    struct rusage_info_v6 ru;
    uint64_t cpu, cycles = 0, insns = 0, energy = 0;
    p->counted = proc_pid_rusage(pid, RUSAGE_INFO_V6, (rusage_info_t *)&ru) == 0;
    if (p->counted) {
        cpu = (uint64_t)((double)(ru.ri_user_time + ru.ri_system_time) * g.tick_ns);
        cycles = ru.ri_cycles;
        insns = ru.ri_instructions;
        energy = ru.ri_energy_nj;
        p->neural = ru.ri_neural_footprint;
    } else {
        cpu = (uint64_t)((double)(ti.ptinfo.pti_total_user + ti.ptinfo.pti_total_system) * g.tick_ns);
    }
    /* Started since the previous scan: everything it used is new. */
    bool young = fresh && g.prev_scan_us && start >= g.prev_scan_us;
    if (fresh && !young) {
        p->cpu = cpu, p->cycles = cycles, p->insns = insns, p->energy = energy;
    }
    p->pub.cpu_sec = (double)delta(cpu, p->cpu) * 1e-9;
    p->pub.c.cycles = (double)delta(cycles, p->cycles);
    p->pub.c.insns = (double)delta(insns, p->insns);
    p->pub.energy_j = (double)delta(energy, p->energy) * 1e-9;
    p->cpu = cpu, p->cycles = cycles, p->insns = insns, p->energy = energy;
    p->primed = !fresh || young;
}

static void scan_procs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    for (int i = 0; i < g.nprocs; i++) g.procs[i].seen = false;
    if (g.cfg.filter_pid > 0) {
        visit(g.cfg.filter_pid);
    } else {
        int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
        if (bytes > 0) {
            int want = bytes / (int)sizeof(int) + 64;
            if (want > g.pids_cap) {
                g.pids_cap = want;
                g.pids = xrealloc(g.pids, (size_t)want * sizeof *g.pids);
            }
            bytes = proc_listpids(PROC_ALL_PIDS, 0, g.pids, g.pids_cap * (int)sizeof(int));
            for (int i = 0; i < bytes / (int)sizeof(int); i++) visit(g.pids[i]);
        }
    }
    int w = 0;
    for (int i = 0; i < g.nprocs; i++)
        if (g.procs[i].seen) g.procs[w++] = g.procs[i];
    g.nprocs = w;
    g.prev_scan_us = now_us;
}

static uint64_t gpu_ns(const struct gpu_use *u, int n, int pid) {
    for (int k = 0; k < n; k++)
        if (u[k].pid == pid) return u[k].ns;
    return 0;
}

/* GPU time per process, summed over the GPU driver's user clients. */
static void read_gpu_time(void) {
    memcpy(g.gpu_prev, g.gpu, sizeof g.gpu);
    g.ngpu_prev = g.ngpu;
    g.ngpu = 0;
    io_service_t acc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AGXAccelerator"));
    if (!acc) return;
    io_iterator_t it;
    if (IORegistryEntryGetChildIterator(acc, kIOServicePlane, &it) == KERN_SUCCESS) {
        io_registry_entry_t e;
        char who[96];
        while ((e = IOIteratorNext(it))) {
            CFTypeRef creator = IORegistryEntryCreateCFProperty(e, CFSTR("IOUserClientCreator"), kCFAllocatorDefault, 0);
            CFTypeRef usage = IORegistryEntryCreateCFProperty(e, CFSTR("AppUsage"), kCFAllocatorDefault, 0);
            int pid = 0;
            if (creator && CFGetTypeID(creator) == CFStringGetTypeID() &&
                sscanf(cstr(creator, who, sizeof who), "pid %d", &pid) != 1)
                pid = 0;
            uint64_t ns = 0;
            if (pid > 0 && usage && CFGetTypeID(usage) == CFArrayGetTypeID()) {
                for (CFIndex i = 0; i < CFArrayGetCount(usage); i++) {
                    CFTypeRef app = CFArrayGetValueAtIndex(usage, i);
                    if (CFGetTypeID(app) == CFDictionaryGetTypeID())
                        ns += (uint64_t)cf_int(CFDictionaryGetValue(app, CFSTR("accumulatedGPUTime")));
                }
            }
            if (ns > 0) {
                int k = 0;
                while (k < g.ngpu && g.gpu[k].pid != pid) k++;
                if (k == g.ngpu && g.ngpu < (int)(sizeof g.gpu / sizeof g.gpu[0])) {
                    g.gpu[k].pid = pid;
                    g.gpu[k].ns = 0;
                    g.ngpu++;
                }
                if (k < g.ngpu) g.gpu[k].ns += ns;
            }
            if (creator) CFRelease(creator);
            if (usage) CFRelease(usage);
            IOObjectRelease(e);
        }
        IOObjectRelease(it);
    }
    IOObjectRelease(acc);
}

/* ------------------------------------------------------------------ */
/* Backend interface                                                   */
/* ------------------------------------------------------------------ */

bool plat_init(const dt_config_t *cfg, dt_info_t *info) {
    g.cfg = *cfg;
    g.self = getpid();
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g.tick_ns = tb.denom ? (double)tb.numer / (double)tb.denom : 1.0;
    *(void **)&g.coal_usage = dlsym(RTLD_DEFAULT, "coalition_info_resource_usage");
    detect_chip();
    if (!amc_open()) {
        fputs("dramtop: the memory controller counters (IOReport \"AMC Stats\") are not available.\n"
              "  They exist on Apple Silicon Macs; virtual machines do not expose them.\n",
              stderr);
        return false;
    }
    if (geteuid() != 0)
        fputs("dramtop: not running as root, so only the whole chip is measured (use sudo for processes)\n", stderr);
    *info = (dt_info_t){
        .cpu_name = g.cpu_name, .backend = "", .scope = "", .mode = "amc", .ncpus = g.ncpus,
        .eff_cpus = g.ncpus, .line = g.line, .has_rw = true, .has_gpu = true, .estimated = true,
        .spec_gbs = g.spec, .nagents = AG_N, .cpu_agents = AG_ECPU + 1, .gpu_agent = AG_GPU,
    };
    for (int a = 0; a < AG_N; a++) info->agent[a] = AGENT_NAME[a];
    return true;
}

void plat_scan(double now) {
    (void)now;
    scan_procs();
    read_gpu_time();
    for (int i = 0; i < g.nprocs; i++) g.procs[i].gpu = gpu_ns(g.gpu, g.ngpu, g.procs[i].pub.pid);
}

bool plat_has_proc(int pid) { return proc_find(pid) != NULL; }

void plat_start_system(dt_info_t *info) {
    if (g.cfg.measure_caps) info->gpu_cap_gbs = gpu_probe_gbs(plat_mem_available() / 4);
    amc_t skip;
    g.prev_at = amc_read(&g.prev, &skip); /* calibration traffic does not count */
    kp.ivl_start = g.prev_at;
    kp_start();
    snprintf(g.backend, sizeof g.backend, "Apple memory controller counters (processes: %s)",
             kp.on ? "estimated" : kp.why);
    snprintf(g.scope, sizeof g.scope, "Whole chip (AMC)");
    info->backend = g.backend;
    info->scope = g.scope;
    info->estimated = kp.on;
}

static uint64_t coalition_of(int pid) {
    struct proc_pidcoalitioninfo ci;
    return proc_pidinfo(pid, PROC_PIDCOALITIONINFO, 0, &ci, sizeof ci) == (int)sizeof ci ? ci.coalition_id[0] : 0;
}

static void coal_add(coal_t *cs, int *n, uint64_t id, double mem) {
    int k = 0;
    while (k < *n && cs[k].id != id) k++;
    if (k == *n) {
        if (*n == MAX_COALS) return;
        cs[(*n)++] = (coal_t){.id = id};
    }
    cs[k].mem += mem;
}

/* Each process's share of the Neural Engine's traffic in the interval. The kernel keeps the
 * Neural Engine time of every coalition (an app with its helper processes), so the traffic is
 * split between coalitions by that time and within a coalition by Neural Engine memory; when
 * no time is known (older macOS), by memory alone. Processes outside the filter count too. */
static void ane_shares(void) {
    coal_t cs[MAX_COALS];
    int n = 0;
    double mem_all = 0;
    if (g.cfg.filter_pid <= 0 && !g.cfg.filter_name) {
        for (int i = 0; i < g.nprocs; i++)
            if (g.procs[i].neural) coal_add(cs, &n, coalition_of(g.procs[i].pub.pid), (double)g.procs[i].neural);
    } else {
        int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0), want = bytes / (int)sizeof(int) + 64;
        if (bytes > 0 && want > g.pids_cap) {
            g.pids_cap = want;
            g.pids = xrealloc(g.pids, (size_t)want * sizeof *g.pids);
        }
        bytes = bytes > 0 ? proc_listpids(PROC_ALL_PIDS, 0, g.pids, g.pids_cap * (int)sizeof(int)) : 0;
        for (int i = 0; i < bytes / (int)sizeof(int); i++) {
            struct rusage_info_v6 ru;
            if (g.pids[i] > 0 && proc_pid_rusage(g.pids[i], RUSAGE_INFO_V6, (rusage_info_t *)&ru) == 0 &&
                ru.ri_neural_footprint)
                coal_add(cs, &n, coalition_of(g.pids[i]), (double)ru.ri_neural_footprint);
        }
    }
    double dt_all = 0;
    for (int k = 0; k < n; k++) {
        mem_all += cs[k].mem;
        uint64_t u[COAL_FIELDS] = {0};
        if (cs[k].id && g.coal_usage && g.coal_usage(cs[k].id, u, sizeof u) == 0) cs[k].ane = u[COAL_ANE_TIME];
        for (int j = 0; j < g.ncoal; j++)
            if (g.coal[j].id == cs[k].id && cs[k].ane > g.coal[j].ane) cs[k].dt = (double)(cs[k].ane - g.coal[j].ane);
        dt_all += cs[k].dt;
    }
    memcpy(g.coal, cs, (size_t)n * sizeof cs[0]);
    g.ncoal = n;

    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        p->ane_share = 0;
        if (!p->neural) continue;
        if (dt_all <= 0) {
            p->ane_share = mem_all > 0 ? (double)p->neural / mem_all : 0;
            continue;
        }
        uint64_t id = coalition_of(p->pub.pid);
        for (int k = 0; k < n; k++)
            if (cs[k].id == id && cs[k].mem > 0) p->ane_share = cs[k].dt / dt_all * (double)p->neural / cs[k].mem;
    }
}

void plat_sample(double now, dt_snapshot_t *snap) {
    (void)now;
    bool filtered = g.cfg.filter_pid > 0 || g.cfg.filter_name;
    scan_procs();
    read_gpu_time();
    if (kp.on) ane_shares();
    double gpu_all = 0;
    for (int k = 0; k < g.ngpu; k++)
        gpu_all += (double)delta(g.gpu[k].ns, gpu_ns(g.gpu_prev, g.ngpu_prev, g.gpu[k].pid));

    amc_t m;
    double span; /* seconds between the AMC readings that bound the interval */
    fit_t fr[MAX_GROUPS] = {{0}}, fw[MAX_GROUPS] = {{0}};
    double den_r[MAX_GROUPS] = {0}, den_w[MAX_GROUPS] = {0};
    if (kp.on) { /* held until the interval's events are handed out below */
        pthread_mutex_lock(&kp.mu);
        slice_close();
        span = g.prev_at - kp.ivl_start;
        kp.ivl_start = g.prev_at;
        m = kp.ivl_amc;
        memset(&kp.ivl_amc, 0, sizeof kp.ivl_amc);
        for (int k = 0; k < g.ngroups; k++) {
            fit_group(k, 0, &fr[k]);
            fit_group(k, 1, &fw[k]);
        }
        /* Shares are taken against everything sampled, including kernel threads and processes outside the filter. */
        for (int s = 0; s < ACC_SLOTS; s++) {
            const acc_t *a = &kp.ivl->slot[s];
            if (!a->used) continue;
            for (int k = 0; k < g.ngroups; k++) {
                den_r[k] += beta_of(&fr[k], a->pid) * (a->ev[k][EV_LD] + a->ev[k][EV_ST]);
                den_w[k] += beta_of(&fw[k], a->pid) * a->ev[k][EV_WB];
            }
        }
    } else {
        double start = g.prev_at;
        g.prev_at = amc_read(&g.prev, &m);
        span = g.prev_at - start;
    }

    memset(snap, 0, sizeof *snap);
    double line = (double)g.line, sum_rd = 0, sum_wr = 0, sum_cpu = 0, cycles = 0, insns = 0, gpu_sec = 0, energy = 0;
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        uint64_t gnow = gpu_ns(g.gpu, g.ngpu, p->pub.pid);
        p->dgpu = p->primed ? (double)delta(gnow, p->gpu) : 0;
        p->gpu = gnow;
        p->pub.gpu_sec = p->dgpu * 1e-9;
        p->pub.denied = !kp.on || !p->counted;
        p->pub.has_ipc = p->counted && p->primed;
        p->pub.has_bw = kp.on && p->counted && p->primed && span > 1e-3;
        p->pub.bw_sec = span;
        cycles += p->pub.c.cycles;
        insns += p->pub.c.insns;
        gpu_sec += p->pub.gpu_sec;
        energy += p->pub.energy_j;
        if (!p->pub.has_bw) continue;

        int pid = p->pub.pid;
        const acc_t *a = acc_get(kp.ivl, pid, false);
        double rd = 0, wr = 0, weighted = 0;
        for (int k = 0; k < g.ngroups; k++) {
            double xr = a ? beta_of(&fr[k], pid) * (a->ev[k][EV_LD] + a->ev[k][EV_ST]) : 0;
            double xw = a ? beta_of(&fw[k], pid) * a->ev[k][EV_WB] : 0;
            double sr = den_r[k] > 0 ? xr / den_r[k] : 0, sw = den_w[k] > 0 ? xw / den_w[k] : 0;
            rd += m.grd[k] * sr;
            wr += m.gwr[k] * sw;
            weighted += m.grd[k] * sr * sr + m.gwr[k] * sw * sw;
        }
        p->pub.c.cpu_read = rd / line;
        sum_cpu += p->pub.c.cpu_read;
        if (gpu_all > 0 && p->dgpu > 0) {
            double share = p->dgpu / gpu_all;
            rd += m.rd[AG_GPU] * share;
            wr += m.wr[AG_GPU] * share;
            weighted += (m.rd[AG_GPU] + m.wr[AG_GPU]) * share * share;
        }
        if (p->ane_share > 0) {
            double share = p->ane_share;
            rd += m.rd[AG_ANE] * share;
            wr += m.wr[AG_ANE] * share;
            weighted += (m.rd[AG_ANE] + m.wr[AG_ANE]) * share * share;
        }
        p->pub.confidence = rd + wr > 0 ? weighted / (rd + wr) : 1.0;
        p->pub.c.read = rd / line;
        p->pub.c.write = wr / line;
        p->pub.c.total = p->pub.c.read + p->pub.c.write;
        sum_rd += p->pub.c.read;
        sum_wr += p->pub.c.write;
    }
    if (kp.on) {
        memset(kp.ivl, 0, sizeof *kp.ivl);
        pthread_mutex_unlock(&kp.mu);
    }

    if (filtered) {
        snap->sys.read = sum_rd;
        snap->sys.write = sum_wr;
        snap->sys.cpu_read = sum_cpu;
    } else {
        snap->sys.read = m.rd_all / line;
        snap->sys.write = m.wr_all / line;
        for (int k = 0; k < g.ngroups; k++) snap->sys.cpu_read += m.grd[k] / line;
        gpu_sec = gpu_all * 1e-9;
    }
    snap->sys.total = snap->sys.read + snap->sys.write;
    snap->bw_sec = span;
    if (kp.on && !filtered) {
        /* Kernel threads, processes gone since the last scan, and GPU or ANE work of no listed
         * process; display, media and other traffic stays with its part of the chip. */
        double rd = m.rd[AG_GPU] + m.rd[AG_ANE], wr = m.wr[AG_GPU] + m.wr[AG_ANE];
        for (int k = 0; k < g.ngroups; k++) rd += m.grd[k], wr += m.gwr[k];
        rd = rd / line - sum_rd;
        wr = wr / line - sum_wr;
        snap->other.read = rd > 0 ? rd : 0;
        snap->other.write = wr > 0 ? wr : 0;
        snap->other.total = snap->other.read + snap->other.write;
        snap->has_other = true;
    }
    if (geteuid() == 0) { /* otherwise only part of the processes is visible */
        snap->sys.cycles = cycles;
        snap->sys.insns = insns;
    }
    snap->gpu_sec = gpu_sec;
    snap->energy_j = energy;
    for (int a = 0; a < AG_N; a++) {
        snap->agent[a].read = m.rd[a] / line;
        snap->agent[a].write = m.wr[a] / line;
    }

    if (g.list_cap < g.nprocs) {
        g.list_cap = g.nprocs + 64;
        g.list = xrealloc(g.list, (size_t)g.list_cap * sizeof *g.list);
    }
    for (int i = 0; i < g.nprocs; i++) g.list[i] = &g.procs[i].pub;
    snap->procs = g.list;
    snap->nprocs = g.nprocs;
}

void plat_shutdown(void) { kp_stop(); }

/* ------------------------------------------------------------------ */
/* Peak bandwidth calibration                                          */
/* ------------------------------------------------------------------ */

unsigned long long plat_mem_available(void) { return (unsigned long long)sysctl_ll("hw.memsize", 1LL << 30) / 2; }

/* Threads cannot be pinned on macOS; the probe asks for the highest QoS instead. */
int plat_cal_cpus(int *cpus, int max) {
    int n = max < g.ncpus ? max : g.ncpus;
    for (int i = 0; i < n; i++) cpus[i] = i;
    return n;
}

void plat_pin_thread(int cpu) {
    (void)cpu;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

void plat_probe_begin(void) {
    amc_t skip;
    amc_read(&g.probe, &skip);
}

double plat_probe_lines(void) {
    amc_t m;
    amc_read(&g.probe, &m);
    return (m.rd[AG_PCPU] + m.rd[AG_ECPU]) / (double)g.line;
}

void plat_probe_end(void) {
    if (g.probe) CFRelease(g.probe);
    g.probe = NULL;
}
