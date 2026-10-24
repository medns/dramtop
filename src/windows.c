/*
 * dramtop - Windows backend. An ETW system session records the CPU's counters at every context
 * switch and timer profile interrupt; the difference between two records on one CPU, counters and
 * time alike, belongs to the thread that ran in between, so each process gets its DRAM lines,
 * instructions and CPU time over the same interval. The DRAM lines are last-level cache misses on
 * Intel; on AMD they are the fills from DRAM of the Linux backend, which Windows does not list, so
 * dramtop registers them as custom counters for as long as it runs.
 *
 * Windows itself offers no access to the memory controller's counters. With PawnIO installed, its
 * signed IntelMCHBAR module reads them on Intel client CPUs: they give the reads and writes of the
 * whole machine, which the misses then divide among processes. Otherwise DRAM reads are estimated
 * from the counted lines, as on Linux, scaled by what the startup calibration measured.
 * Hyper-V gives every partition core counters of its own, so the misses of virtual machines and
 * WSL2 are not seen.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>
#include <winternl.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif

#include <limits.h>
#include <stddef.h>

#include "dramtop.h"

/* winternl.h declares the query but not the set, nor the version. */
NTSYSAPI NTSTATUS NTAPI NtSetSystemInformation(SYSTEM_INFORMATION_CLASS cls, PVOID info, ULONG len);
NTSYSAPI NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW info);

#define MAX_CTR       5     /* hardware counters: DRAM lines (one, or three on AMD), instructions, core cycles */
#define AMD_DRAM_CTR  3     /* the AMD DRAM events, which come first and are kept when counters run short */
#define V_DEMAND      0     /* DRAM lines: all LLC misses on Intel, demand fills on AMD */
#define V_PREFETCH    1     /* DRAM lines the hardware prefetchers fetched, on AMD */
#define V_INSNS       2
#define V_CYCLES      3
#define V_TIME        4     /* the time the thread ran, in QueryPerformanceCounter ticks */
#define NVAL          5
#define SLOTS_PER_SEC 20    /* counts are filed by the time stamp of their event, in 50 ms slots */
#define SCAN_SEC      0.95  /* the process list is read about once a second, whatever the interval */
#define NSLOT         256   /* slots a process keeps: the longest interval plus the delivery delay */
#define PHASH         1024  /* processes with counts */
#define THASH         16384 /* threads */
#define PEND_MAX      65536 /* counts of threads whose start event has not arrived yet */
#define PEND_SEC      3     /* after which those counts stay unattributed */
#define IDLE_SEC      10    /* a process without counts for this long gives up its slots */
#define LAG_MIN       0.2   /* seconds a sample waits for late events, adjusted to the measured delay */
#define LAG_MAX       3.0
#define CAL_LOG_MAX   (1 << 20) /* misses of this process during the calibration */
#define IMC_HZ        20    /* memory controller samples per second */
#define IMC_RING      4096  /* minutes of them, far more than a sample interval plus the delay */
#define IMC_SCALE_MAX 1.5   /* the most CPU traffic one miss may stand for; stores that bypass the cache cause no misses */

#define IOCTL_PIO_LOAD_BINARY 0xA1B22084u
#define IOCTL_PIO_EXECUTE_FN  0xA1B22104u

/* Registering a custom PMU profile source at runtime (as WPR does): the undocumented
 * SystemPerformanceTraceInformation with EventTraceProfileSourceAddInformation. The source is
 * gone at the next boot; removing one creates the registry key below, which stays empty. */
#define PROFILE_SOURCE_KEY     L"SYSTEM\\CurrentControlSet\\Control\\WMI\\ProfileSource"
#define SYSTEM_PERF_TRACE_INFO 31
#define ETI_PROFILE_ADD        21
#define ETI_PROFILE_REMOVE     22
#define DT_SOURCE_INTERVAL     0x10000 /* for sampling, which dramtop does not use; it only has to be valid */

typedef struct {
    ULONG cls;
    UCHAR version; /* 2 */
    union {
        struct {
            UCHAR event, unit;
        } x86; /* Intel and AMD */
        ULONG align[2];
    } ev;
    ULONG cpu[3]; /* {family, model, stepping} of the CPU it applies to */
    ULONG interval;
    BOOLEAN persist;
    WCHAR name[64];
} profile_add_t;

typedef struct {
    ULONG cls;
    ULONG source;
    ULONG cpu[3];
} profile_remove_t;

enum { IMC_RD, IMC_WR, IMC_IA, IMC_NREG };
/* MCHBAR offsets: DRAM data reads, DRAM data writes, requests of the cores; 64 bytes each. */
static const ULONG64 IMC_OFFSET[IMC_NREG] = {0x5050, 0x5054, 0x5044};

static const GUID THREAD_GUID = {0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
static const GUID PERFINFO_GUID = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
enum { OP_START = 1, OP_END = 2, OP_DC_START = 3, OP_CSWITCH = 36, OP_SAMPLED_PROFILE = 46 };

typedef struct {
    uint64_t v[NVAL];
} ctr_t;

typedef struct {
    uint64_t raw[MAX_CTR], ts;
} cpu_last_t;

typedef struct {
    long long slot[NSLOT]; /* the absolute slot number each entry holds */
    ctr_t c[NSLOT];
} ring_t;

typedef struct pacc {
    struct pacc *next;
    ULONG pid;
    long long last; /* newest slot with counts */
    ring_t ring;
} pacc_t;

/* Thread ids are reused quickly, so a thread keeps the time it started and the process of the
 * previous thread with its id: events that arrive late still go to the right process. */
typedef struct thr {
    struct thr *next;
    ULONG tid, pid, prev_pid;
    LONGLONG start, end; /* end 0: running */
} thr_t;

typedef struct {
    ULONG tid;
    LONGLONG ts;
    ctr_t c;
} pend_t;

/* SYSTEM_PROCESS_INFORMATION with the fields that winternl.h leaves unnamed. */
typedef struct {
    ULONG next, threads;
    LARGE_INTEGER reserved[3];
    LARGE_INTEGER create, user, kernel;
    UNICODE_STRING name;
    LONG priority;
    HANDLE pid, parent;
} spi_t;

typedef struct {
    dt_proc_t pub;
    LONGLONG create; /* tells a reused PID apart */
    double gone;     /* when the process was first missing from the list, 0 while it runs */
    ctr_t acc;
    bool seen;
} proc_t;

typedef struct {
    LONGLONG ts;
    uint64_t misses;
} cal_miss_t;

typedef struct {
    LONGLONG t;
    uint64_t v[IMC_NREG]; /* cache lines since the sampler started */
} imc_sample_t;

static struct {
    dt_config_t cfg;
    DWORD self;
    int ncpus, maxcpus, family, model, stepping;
    double eff_cpus;
    unsigned line;
    bool intel, amd;
    char cpu_name[64], backend[96], scope[96];
    int nctr;
    ULONG source[MAX_CTR];
    int ctr_val[MAX_CTR];         /* the value each counter adds to */
    const wchar_t *owned[MAX_CTR]; /* custom sources to remove on exit */
    ULONG src_cpus[3][3];         /* CPUs a custom source is added for, most specific first; ~0 matches any */
    bool split;                   /* demand and prefetch fills are counted apart */
    double read_scale; /* DRAM reads per miss, from the calibration; 1 without it */

    /* Calibration passes: when each ended and how many bytes it read. */
    struct {
        LONGLONG t;
        double bytes;
    } probe[16];
    int nprobe;

    /* Memory controller through PawnIO. */
    bool exact, imc_ia;
    uint32_t imc_first[IMC_NREG]; /* the reading that the first sample stands for */
    HANDLE pio, imc_thread;
    volatile LONG imc_quit;
    SRWLOCK imc_lock;
    imc_sample_t *imc;
    long long nimc;

    wchar_t name[32];
    TRACEHANDLE session, trace;
    HANDLE consumer;
    bool on;
    DWORD other_dramtop; /* a running dramtop, which holds the counters */
    LONGLONG qpc_hz, slot_ticks, started;
    LONGLONG from_ts; /* events before this are not counted */

    /* Shared with the consumer thread, under lock. */
    SRWLOCK lock;
    cpu_last_t *last; /* per CPU */
    bool *have;
    ring_t chip;
    ctr_t chip_total;
    pacc_t *pacc[PHASH];
    thr_t *thr[THASH];
    pend_t *pend;
    int npend;
    long long drained; /* newest slot handed to a sample */
    LONGLONG lat_max;
    uint64_t n_events, n_pmc;
    bool cal_on;
    cal_miss_t *cal;
    int ncal;

    double lag, lat_peak, last_sample, last_scan;
    proc_t *procs;
    int nprocs, cap;
    dt_proc_t **list;
    int list_cap;
    void *spi;
    ULONG spi_size;
} g;

static unsigned hash_id(ULONG id, unsigned size) { return (id * 2654435761u >> 16) & (size - 1); }

static LONGLONG qpc(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* ------------------------------------------------------------------ */
/* Counts filed by time                                                */
/* ------------------------------------------------------------------ */

static void ring_add(ring_t *r, long long s, const uint64_t *d) {
    int i = (int)(s & (NSLOT - 1));
    if (r->slot[i] != s) {
        r->slot[i] = s;
        memset(&r->c[i], 0, sizeof r->c[i]);
    }
    for (int k = 0; k < NVAL; k++) r->c[i].v[k] += d[k];
}

/* Moves slots (from, to] into out. */
static void ring_take(ring_t *r, long long from, long long to, uint64_t *out) {
    if (to - from > NSLOT) from = to - NSLOT;
    for (long long s = from + 1; s <= to; s++) {
        int i = (int)(s & (NSLOT - 1));
        if (r->slot[i] != s) continue;
        for (int k = 0; k < NVAL; k++) out[k] += r->c[i].v[k];
        r->slot[i] = -1;
    }
}

/* Events older than the last sample go into the next one rather than being lost. */
static long long slot_of(LONGLONG ts) {
    long long s = ts / g.slot_ticks;
    return s > g.drained ? s : g.drained + 1;
}

static void credit_pid(ULONG pid, LONGLONG ts, const uint64_t *d) {
    if (g.cal_on && pid == g.self && g.ncal < CAL_LOG_MAX) {
        g.cal[g.ncal].ts = ts;
        g.cal[g.ncal++].misses = d[V_DEMAND] + d[V_PREFETCH];
    }
    if (ts < g.from_ts) return;
    unsigned h = hash_id(pid, PHASH);
    pacc_t *a = g.pacc[h];
    while (a && a->pid != pid) a = a->next;
    if (!a) {
        if (!(a = calloc(1, sizeof *a))) return;
        a->pid = pid;
        a->next = g.pacc[h];
        g.pacc[h] = a;
    }
    long long s = slot_of(ts);
    ring_add(&a->ring, s, d);
    if (s > a->last) a->last = s;
}

static thr_t *thr_find(ULONG tid) {
    thr_t *t = g.thr[hash_id(tid, THASH)];
    while (t && t->tid != tid) t = t->next;
    return t;
}

static void credit(ULONG tid, LONGLONG ts, const uint64_t *d) {
    if (ts >= g.from_ts) {
        ring_add(&g.chip, slot_of(ts), d);
        for (int k = 0; k < NVAL; k++) g.chip_total.v[k] += d[k];
    }
    if (tid == 0) return; /* the idle threads */
    thr_t *t = thr_find(tid);
    if (t && ts >= t->start && (!t->end || ts <= t->end)) {
        credit_pid(t->pid, ts, d);
    } else if (t && ts < t->start) {
        if (t->prev_pid) credit_pid(t->prev_pid, ts, d);
    } else if (g.npend < PEND_MAX) { /* events on other CPUs can arrive before the thread's start */
        pend_t *p = &g.pend[g.npend++];
        p->tid = tid;
        p->ts = ts;
        memcpy(p->c.v, d, sizeof p->c.v);
    }
}

static void thread_start(ULONG tid, ULONG pid, LONGLONG ts) {
    thr_t *t = thr_find(tid);
    if (t && ts < t->start) return; /* the late start of an earlier thread with this id */
    if (!t) {
        if (!(t = calloc(1, sizeof *t))) return;
        unsigned h = hash_id(tid, THASH);
        t->tid = tid;
        t->next = g.thr[h];
        g.thr[h] = t;
    } else {
        t->prev_pid = t->pid;
    }
    t->pid = pid;
    t->start = ts;
    t->end = 0;
    for (int i = 0; i < g.npend; i++) {
        if (g.pend[i].tid != tid || g.pend[i].ts < ts) continue;
        credit_pid(pid, g.pend[i].ts, g.pend[i].c.v);
        g.pend[i--] = g.pend[--g.npend];
    }
}

static void thread_end(ULONG tid, LONGLONG ts) {
    thr_t *t = thr_find(tid);
    if (t && ts >= t->start) t->end = ts;
}

static void on_counters(const EVENT_RECORD *r, ULONG tid) {
    const ULONG64 *v = NULL;
    for (USHORT i = 0; i < r->ExtendedDataCount; i++)
        if (r->ExtendedData[i].ExtType == EVENT_HEADER_EXT_TYPE_PMC_COUNTERS &&
            r->ExtendedData[i].DataSize >= (USHORT)(g.nctr * sizeof(ULONG64)))
            v = (const ULONG64 *)(ULONG_PTR)r->ExtendedData[i].DataPtr;
    unsigned cpu = r->BufferContext.ProcessorIndex;
    if (!v || cpu >= (unsigned)g.maxcpus) return;
    g.n_pmc++;
    cpu_last_t *last = &g.last[cpu];
    uint64_t ts = (uint64_t)r->EventHeader.TimeStamp.QuadPart;
    if (g.have[cpu]) {
        uint64_t d[NVAL] = {0};
        bool ok = ts >= last->ts;
        for (int k = 0; k < g.nctr; k++) {
            ok = ok && v[k] >= last->raw[k];
            d[g.ctr_val[k]] += v[k] - last->raw[k];
        }
        d[V_TIME] = ts - last->ts;
        if (ok) credit(tid, (LONGLONG)ts, d);
    }
    for (int k = 0; k < g.nctr; k++) last->raw[k] = v[k];
    last->ts = ts;
    g.have[cpu] = true;
}

static void WINAPI on_event(PEVENT_RECORD r) {
    UCHAR op = r->EventHeader.EventDescriptor.Opcode;
    bool thread = !memcmp(&r->EventHeader.ProviderId, &THREAD_GUID, sizeof(GUID));
    if (!thread && (op != OP_SAMPLED_PROFILE || memcmp(&r->EventHeader.ProviderId, &PERFINFO_GUID, sizeof(GUID))))
        return;
    const ULONG *u = r->UserData;
    LONGLONG ts = r->EventHeader.TimeStamp.QuadPart;
    AcquireSRWLockExclusive(&g.lock);
    if (!(g.n_events++ & 63)) { /* how late events arrive; a buffer holds far more than 64 */
        LONGLONG lat = qpc() - ts;
        if (lat > g.lat_max) g.lat_max = lat;
    }
    if (!thread) { /* SampledProfile: InstructionPointer, ThreadId */
        unsigned off = r->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER ? 4 : 8;
        if (r->UserDataLength >= off + 4) on_counters(r, *(const ULONG *)((const BYTE *)r->UserData + off));
    } else if (r->UserDataLength >= 8) {
        /* A rundown (DCStart, DCEnd) lists the threads that exist; a DCEnd is not an exit. */
        switch (op) {
        case OP_START: thread_start(u[1], u[0], ts); break; /* ProcessId, TThreadId */
        case OP_DC_START: thread_start(u[1], u[0], 0); break;
        case OP_END: thread_end(u[1], ts); break;
        case OP_CSWITCH: on_counters(r, u[1]); break; /* NewThreadId, OldThreadId */
        default: break;
        }
    }
    ReleaseSRWLockExclusive(&g.lock);
}

static DWORD WINAPI consume(void *arg) {
    (void)arg;
    ProcessTrace(&g.trace, 1, NULL, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trace session                                                       */
/* ------------------------------------------------------------------ */

#define NAME_BYTES 1024

static EVENT_TRACE_PROPERTIES *new_props(void) {
    size_t size = sizeof(EVENT_TRACE_PROPERTIES) + 2 * NAME_BYTES;
    EVENT_TRACE_PROPERTIES *p = xrealloc(NULL, size);
    memset(p, 0, size);
    p->Wnode.BufferSize = (ULONG)size;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    p->LogFileNameOffset = sizeof(EVENT_TRACE_PROPERTIES) + NAME_BYTES;
    return p;
}

static void stop_session_named(const wchar_t *name) {
    EVENT_TRACE_PROPERTIES *p = new_props();
    ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_STOP);
    free(p);
}

/* The session is system-wide and outlives the process, so every way out stops it. */
static void etw_stop(void) {
    if (!g.on) return;
    g.on = false;
    stop_session_named(g.name);
    if (g.trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(g.trace);
    if (g.consumer) {
        WaitForSingleObject(g.consumer, 2000);
        CloseHandle(g.consumer);
        g.consumer = NULL;
    }
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    (void)e;
    if (g.on) ControlTraceW(g.session, NULL, new_props(), EVENT_TRACE_CONTROL_STOP);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool dramtop_running(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH, code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && QueryFullProcessImageNameW(h, 0, path, &len) &&
                 wcsstr(_wcslwr(path), L"dramtop");
    CloseHandle(h);
    return alive;
}

/* Sessions left behind by a dramtop that was killed. */
static void stop_stale_sessions(void) {
    enum { MAX_SESSIONS = 64 };
    EVENT_TRACE_PROPERTIES *p[MAX_SESSIONS];
    for (int i = 0; i < MAX_SESSIONS; i++) p[i] = new_props();
    ULONG n = 0;
    ULONG st = QueryAllTracesW(p, MAX_SESSIONS, &n);
    for (ULONG i = 0; (st == ERROR_SUCCESS || st == ERROR_MORE_DATA) && i < n && i < MAX_SESSIONS; i++) {
        const wchar_t *name = (const wchar_t *)((const char *)p[i] + p[i]->LoggerNameOffset);
        if (wcsncmp(name, L"dramtop-", 8)) continue;
        DWORD pid = wcstoul(name + 8, NULL, 10);
        if (pid == g.self) continue;
        if (dramtop_running(pid)) g.other_dramtop = pid;
        else stop_session_named(name);
    }
    for (int i = 0; i < MAX_SESSIONS; i++) free(p[i]);
}

static bool elevated(void) {
    HANDLE tok;
    TOKEN_ELEVATION e = {0};
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

static void enable_privilege(const wchar_t *name) {
    HANDLE tok;
    TOKEN_PRIVILEGES tp = {.PrivilegeCount = 1};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok)) return;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    }
    CloseHandle(tok);
}

/* The kernel names the counters it can program, for the whole system; the list needs no session
 * and no elevation. Returns the size of the list in buf, 0 on failure. */
static ULONG source_list(BYTE **buf) {
    ULONG size = 64 * 1024, need = 0;
    *buf = xrealloc(NULL, size);
    ULONG st = TraceQueryInformation(0, TraceProfileSourceListInfo, *buf, size, &need);
    if (st != ERROR_SUCCESS && need > size) {
        *buf = xrealloc(*buf, size = need);
        st = TraceQueryInformation(0, TraceProfileSourceListInfo, *buf, size, &need);
    }
    return st != ERROR_SUCCESS ? 0 : need && need < size ? need : size;
}

#define FOR_EACH_SOURCE(s, buf, size)                                                                                  \
    for (const PROFILE_SOURCE_INFO *s = (const PROFILE_SOURCE_INFO *)(buf);                                       \
         s && (const BYTE *)s + sizeof(PROFILE_SOURCE_INFO) <= (buf) + (size);                                      \
         s = s->NextEntryOffset ? (const PROFILE_SOURCE_INFO *)((const BYTE *)s + s->NextEntryOffset) : NULL)

static long source_id(const wchar_t *name) {
    BYTE *buf;
    ULONG size = source_list(&buf);
    long id = -1;
    FOR_EACH_SOURCE(s, buf, size)
        if (!wcscmp(s->Description, name)) id = (long)s->Source;
    free(buf);
    return id;
}

/* Intel: architectural events that Windows lists; the fixed counters leave the general ones free. */
static bool intel_sources(void) {
    static const wchar_t *const want[3][3] = {
        {L"LLCMisses", NULL, NULL},
        {L"InstructionsRetiredFixed", L"InstructionRetired", NULL},
        {L"UnhaltedCoreCyclesFixed", L"UnhaltedCoreCycles", L"TotalCycles"},
    };
    static const int val[3] = {V_DEMAND, V_INSNS, V_CYCLES};
    int rank[3] = {INT_MAX, INT_MAX, INT_MAX};
    BYTE *buf;
    ULONG size = source_list(&buf);
    FOR_EACH_SOURCE(s, buf, size) {
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3 && want[k][j]; j++)
                if (j < rank[k] && !wcscmp(s->Description, want[k][j])) {
                    rank[k] = j;
                    g.source[k] = s->Source;
                }
    }
    free(buf);
    for (int k = 0; k < 3; k++) {
        if (rank[k] == INT_MAX) return false;
        g.ctr_val[k] = val[k];
    }
    g.nctr = 3;
    return true;
}

static NTSTATUS add_source(const wchar_t *name, UCHAR event, UCHAR unit, const ULONG cpu[3]) {
    profile_add_t a;
    memset(&a, 0, sizeof a);
    a.cls = ETI_PROFILE_ADD;
    a.version = 2;
    a.ev.x86.event = event;
    a.ev.x86.unit = unit;
    memcpy(a.cpu, cpu, sizeof a.cpu);
    a.interval = DT_SOURCE_INTERVAL;
    a.persist = FALSE;
    wcsncpy(a.name, name, sizeof a.name / sizeof a.name[0] - 1);
    ULONG size = (ULONG)(offsetof(profile_add_t, name) + (wcslen(a.name) + 1) * sizeof(WCHAR));
    return NtSetSystemInformation((SYSTEM_INFORMATION_CLASS)SYSTEM_PERF_TRACE_INFO, &a, size);
}

/* Windows lists no DRAM events for AMD CPUs, so dramtop adds the ones of its Linux backend, as WPR
 * adds custom counters: they last until the next boot at most, and dramtop removes those it added. */
static bool amd_sources(void) {
    bool zen4 = g.family >= 0x1a ||
                (g.family == 0x19 && ((g.model >= 0x10 && g.model <= 0x1f) || (g.model >= 0x60 && g.model <= 0xaf)));
    const struct {
        const wchar_t *name;
        UCHAR event, unit;
        int val;
    } ev[MAX_CTR] = {
        /* Counters the PMU cannot take are dropped from the end: IPC goes before DRAM traffic. */
        {L"dramtop.DemandDramFills", 0x43, 0x48, V_DEMAND},     /* ls_dmnd_fills_from_sys.all_dram_io */
        {L"dramtop.PrefetchDramFills", 0x5a, 0x48, V_PREFETCH}, /* ls_hw_pf_dc_fills.all_dram_io */
        /* l2_pf_miss_l2_l3: from Zen 4 on, umask bits 5-7 are the L1 prefetchers, which 0x5a
         * already counts, so only the L2 prefetcher bits are selected. */
        {zen4 ? L"dramtop.L2PrefetchMissesZen4" : L"dramtop.L2PrefetchMisses", 0x72, zen4 ? 0x1f : 0xff, V_PREFETCH},
        {L"dramtop.Instructions", 0xc0, 0x00, V_INSNS},         /* ex_ret_instr */
        {L"dramtop.Cycles", 0x76, 0x00, V_CYCLES},              /* ls_not_halted_cyc */
    };
    ULONG max = 0, ret = 0;
    int n = TraceQueryInformation(0, TraceMaxPmcCounterQuery, &max, sizeof max, &ret) == ERROR_SUCCESS && max > 0 &&
                    max < MAX_CTR
                ? (int)max
                : MAX_CTR;
    /* Sources left by a dramtop that was killed are taken over, unless another one runs and uses them. */
    stop_stale_sessions();
    for (int k = 0; k < n; k++) {
        long id = source_id(ev[k].name);
        NTSTATUS st = 0;
        for (int c = 0; id < 0 && c < 3; c++) {
            st = add_source(ev[k].name, ev[k].event, ev[k].unit, g.src_cpus[c]);
            id = source_id(ev[k].name);
        }
        if (id < 0) {
            RTL_OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
            bool old = RtlGetVersion(&v) >= 0 && v.dwBuildNumber < 18362; /* Windows 10 version 1903 */
            fprintf(stderr, "dramtop: Windows did not take the CPU counter %ls (status 0x%08lx)%s\n", ev[k].name,
                    (unsigned long)st, old ? "; custom CPU counters need Windows 10 version 1903 or later" : "");
            return false;
        }
        if (!g.other_dramtop) g.owned[k] = ev[k].name;
        g.source[k] = (ULONG)id;
        g.ctr_val[k] = ev[k].val;
    }
    g.nctr = n;
    g.split = true;
    return true;
}

static void remove_sources(void) {
    bool removed = false;
    for (int k = 0; k < MAX_CTR; k++) {
        if (!g.owned[k]) continue;
        long id;
        for (int c = 0; c < 3 && (id = source_id(g.owned[k])) >= 0; c++) {
            profile_remove_t r;
            memset(&r, 0, sizeof r);
            r.cls = ETI_PROFILE_REMOVE;
            r.source = (ULONG)id;
            memcpy(r.cpu, g.src_cpus[c], sizeof r.cpu);
            NtSetSystemInformation((SYSTEM_INFORMATION_CLASS)SYSTEM_PERF_TRACE_INFO, &r, sizeof r);
        }
        g.owned[k] = NULL;
        removed = true;
    }
    /* Removing them created this key; without values or subkeys it configures nothing. */
    HKEY key;
    DWORD subkeys = 0, values = 0;
    if (!removed || RegOpenKeyExW(HKEY_LOCAL_MACHINE, PROFILE_SOURCE_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    bool empty = RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL) ==
                     ERROR_SUCCESS &&
                 !subkeys && !values;
    RegCloseKey(key);
    if (empty) RegDeleteKeyW(HKEY_LOCAL_MACHINE, PROFILE_SOURCE_KEY);
}

static bool etw_fail(const char *what, ULONG err) {
    if (err == ERROR_ACCESS_DENIED)
        fprintf(stderr, "dramtop: %s: access denied (run dramtop from a terminal opened as administrator)\n", what);
    else if (err == ERROR_NO_SYSTEM_RESOURCES)
        fprintf(stderr, "dramtop: %s: too many system trace sessions are running (close WPR or xperf traces)\n", what);
    else
        fprintf(stderr, "dramtop: %s failed (error %lu)\n", what, err);
    etw_stop();
    return false;
}

static bool etw_start(void) {
    swprintf(g.name, sizeof g.name / sizeof g.name[0], L"dramtop-%lu", g.self);
    stop_stale_sessions();
    enable_privilege(L"SeSystemProfilePrivilege"); /* for the profile interrupt and its counters */

    EVENT_TRACE_PROPERTIES *p = new_props();
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1; /* QueryPerformanceCounter time stamps */
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    p->EnableFlags = EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_PROFILE;
    p->BufferSize = 64; /* KB */
    p->MinimumBuffers = 2 * (ULONG)g.maxcpus + 8;
    p->MaximumBuffers = p->MinimumBuffers + 256;
    p->FlushTimer = 1; /* seconds, which sets the delay before events arrive */
    p->LogFileNameOffset = 0;
    ULONG st = StartTraceW(&g.session, g.name, p);
    free(p);
    if (st != ERROR_SUCCESS) return etw_fail("starting the trace session", st);
    g.started = g.from_ts = qpc();
    g.drained = g.started / g.slot_ticks;
    g.on = true;
    g.trace = INVALID_PROCESSTRACE_HANDLE;
    atexit(etw_stop);
    SetUnhandledExceptionFilter(on_crash);

    st = TraceSetInformation(g.session, TracePmcCounterListInfo, g.source, (ULONG)(g.nctr * sizeof g.source[0]));
    /* The profile interrupt takes a general-purpose counter, so custom counters can outnumber the
     * free ones; a list that failed can be set again. */
    while (st == ERROR_BUSY && g.split && g.nctr > AMD_DRAM_CTR && !g.other_dramtop) {
        g.nctr--;
        st = TraceSetInformation(g.session, TracePmcCounterListInfo, g.source, (ULONG)(g.nctr * sizeof g.source[0]));
    }
    if (st != ERROR_SUCCESS) {
        if (st == ERROR_BUSY && !g.other_dramtop) stop_stale_sessions(); /* one that started at the same time */
        if (st == ERROR_BUSY && g.other_dramtop) {
            for (int k = 0; k < MAX_CTR; k++) g.owned[k] = NULL; /* the custom sources are in use by the other one */
            fprintf(stderr, "dramtop: another dramtop (PID %lu) is using the CPU counters\n", g.other_dramtop);
        } else {
            fprintf(stderr, "dramtop: the CPU counters cannot be set up (error %lu); another profiler such as "
                            "WPR, xperf or VTune may be using them\n", st);
        }
        etw_stop();
        return false;
    }
    CLASSIC_EVENT_ID ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].EventGuid = THREAD_GUID;
    ev[0].Type = OP_CSWITCH;
    ev[1].EventGuid = PERFINFO_GUID;
    ev[1].Type = OP_SAMPLED_PROFILE;
    st = TraceSetInformation(g.session, TracePmcEventListInfo, ev, sizeof ev);
    if (st != ERROR_SUCCESS) return etw_fail("attaching the CPU counters to trace events", st);

    EVENT_TRACE_LOGFILEW lf;
    memset(&lf, 0, sizeof lf);
    lf.LoggerName = g.name;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = on_event;
    g.trace = OpenTraceW(&lf);
    if (g.trace == INVALID_PROCESSTRACE_HANDLE) return etw_fail("opening the trace", GetLastError());
    if (!(g.consumer = CreateThread(NULL, 0, consume, NULL, 0, NULL)))
        return etw_fail("starting the trace reader", GetLastError());

    /* Virtual machines without a virtual PMU deliver the events but no counter values. */
    for (int i = 0; i < 20; i++) {
        p = new_props();
        ControlTraceW(g.session, NULL, p, EVENT_TRACE_CONTROL_FLUSH);
        free(p);
        Sleep(50);
        AcquireSRWLockShared(&g.lock);
        uint64_t events = g.n_events, pmc = g.n_pmc, counted = 0;
        for (int k = 0; k < V_TIME; k++) counted |= g.chip_total.v[k];
        ReleaseSRWLockShared(&g.lock);
        if (counted) return true;
        if (i < 19) continue;
        if (!events) fputs("dramtop: no trace events arrived\n", stderr);
        else if (!pmc) fputs("dramtop: trace events carry no CPU counter values (virtual machines often lack them)\n", stderr);
        else fputs("dramtop: the CPU counters stay at zero (virtual machines often lack them)\n", stderr);
    }
    etw_stop();
    return false;
}

/* ------------------------------------------------------------------ */
/* Memory controller through PawnIO                                    */
/* ------------------------------------------------------------------ */

static NTSTATUS pio_ioctl(HANDLE ev, ULONG code, const void *in, ULONG in_len, void *out, ULONG out_len) {
    IO_STATUS_BLOCK iosb;
    memset(&iosb, 0, sizeof iosb);
    NTSTATUS st = NtDeviceIoControlFile(g.pio, ev, NULL, NULL, &iosb, code, (PVOID)in, in_len, out, out_len);
    if (st == STATUS_PENDING) {
        WaitForSingleObject(ev, INFINITE);
        st = iosb.Status;
    }
    return st;
}

static bool imc_read(HANDLE ev, uint32_t v[IMC_NREG]) {
    for (int i = 0; i < IMC_NREG; i++) {
        BYTE in[40] = "ioctl_read_dword"; /* the module function, padded to 32 bytes, then its argument */
        memcpy(in + 32, &IMC_OFFSET[i], 8);
        ULONG64 out = 0;
        if (pio_ioctl(ev, IOCTL_PIO_EXECUTE_FN, in, sizeof in, &out, sizeof out) < 0) return false;
        v[i] = (uint32_t)out;
    }
    return true;
}

/* The counters are 32 bits wide, which at 50 GB/s wraps in five seconds; samples with their time
 * also let a sample take exactly the span that the trace's slots cover. */
static DWORD WINAPI imc_sampler(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); /* a machine busy with its own threads */
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint32_t prev[IMC_NREG], cur[IMC_NREG];
    uint64_t acc[IMC_NREG] = {0};
    memcpy(prev, g.imc_first, sizeof prev);
    while (ev && !g.imc_quit) {
        Sleep(1000 / IMC_HZ);
        LONGLONG t0 = qpc();
        if (!imc_read(ev, cur)) continue; /* the next sample spans the gap */
        LONGLONG t1 = qpc();
        AcquireSRWLockExclusive(&g.imc_lock);
        imc_sample_t *s = &g.imc[g.nimc % IMC_RING];
        s->t = t0 + (t1 - t0) / 2;
        for (int i = 0; i < IMC_NREG; i++) {
            acc[i] += (uint32_t)(cur[i] - prev[i]);
            prev[i] = cur[i];
            s->v[i] = acc[i];
        }
        g.nimc++;
        ReleaseSRWLockExclusive(&g.imc_lock);
    }
    if (ev) CloseHandle(ev);
    return 0;
}

static bool imc_at(LONGLONG t, double out[IMC_NREG]) {
    bool ok = false;
    AcquireSRWLockShared(&g.imc_lock);
    long long lo = g.nimc > IMC_RING ? g.nimc - IMC_RING : 0, hi = g.nimc - 1;
    if (hi > lo && g.imc[lo % IMC_RING].t <= t && t <= g.imc[hi % IMC_RING].t) {
        while (hi - lo > 1) {
            long long mid = lo + (hi - lo) / 2;
            if (g.imc[mid % IMC_RING].t <= t) lo = mid;
            else hi = mid;
        }
        const imc_sample_t *a = &g.imc[lo % IMC_RING], *b = &g.imc[hi % IMC_RING];
        double f = b->t > a->t ? (double)(t - a->t) / (double)(b->t - a->t) : 0.0;
        for (int i = 0; i < IMC_NREG; i++) out[i] = (double)a->v[i] + f * (double)(b->v[i] - a->v[i]);
        ok = true;
    }
    ReleaseSRWLockShared(&g.imc_lock);
    return ok;
}

/* Cache lines over [a, b). */
static bool imc_span(LONGLONG a, LONGLONG b, double out[IMC_NREG]) {
    double va[IMC_NREG];
    if (!imc_at(a, va) || !imc_at(b, out)) return false;
    for (int i = 0; i < IMC_NREG; i++) out[i] -= va[i];
    return true;
}

/* Client parts from Sandy Bridge to Comet Lake, which have the counters at these offsets. */
static bool imc_supported(void) {
    static const int models[] = {0x2A, 0x3A, 0x3C, 0x45, 0x46, 0x3D, 0x47, 0x4E, 0x5E, 0x8E, 0x9E, 0xA5, 0xA6};
    if (!g.intel || g.family != 6) return false;
    for (size_t i = 0; i < sizeof models / sizeof models[0]; i++)
        if (models[i] == g.model) return true;
    return false;
}

static bool imc_open(void) {
    if (!imc_supported()) return false;
    HRSRC res = FindResourceW(NULL, L"PAWNIO_MCHBAR", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL mem = res ? LoadResource(NULL, res) : NULL;
    const void *blob = mem ? LockResource(mem) : NULL;
    DWORD size = res ? SizeofResource(NULL, res) : 0;
    if (!blob || !size) return false;

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Device\\PawnIO");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
    IO_STATUS_BLOCK iosb;
    if (NtOpenFile(&g.pio, GENERIC_READ | GENERIC_WRITE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   0) < 0) {
        g.pio = NULL;
        return false;
    }
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint32_t a[IMC_NREG], b[IMC_NREG];
    bool ok = ev && pio_ioctl(ev, IOCTL_PIO_LOAD_BINARY, blob, size, NULL, 0) >= 0 && imc_read(ev, a);
    LONGLONG t = 0;
    if (ok) { /* the counters run: some DRAM traffic never stops */
        Sleep(20);
        t = qpc();
        ok = imc_read(ev, b) && (b[IMC_RD] != a[IMC_RD] || b[IMC_WR] != a[IMC_WR]);
        g.imc_ia = ok && b[IMC_IA] != a[IMC_IA];
    }
    if (ev) CloseHandle(ev);
    if (ok) { /* the first sample predates the trace session, so every slot has one before it */
        g.imc = xrealloc(NULL, IMC_RING * sizeof *g.imc);
        InitializeSRWLock(&g.imc_lock);
        memcpy(g.imc_first, b, sizeof b);
        g.imc[0].t = t;
        memset(g.imc[0].v, 0, sizeof g.imc[0].v);
        g.nimc = 1;
        ok = (g.imc_thread = CreateThread(NULL, 0, imc_sampler, NULL, 0, NULL)) != NULL;
    }
    if (!ok) {
        CloseHandle(g.pio);
        g.pio = NULL;
    }
    return ok;
}

static void imc_close(void) {
    if (!g.pio) return;
    InterlockedExchange(&g.imc_quit, 1);
    if (g.imc_thread) {
        WaitForSingleObject(g.imc_thread, 1000);
        CloseHandle(g.imc_thread);
        g.imc_thread = NULL;
    }
    CloseHandle(g.pio);
    g.pio = NULL;
}

/* ------------------------------------------------------------------ */
/* CPU                                                                 */
/* ------------------------------------------------------------------ */

static void *cpu_info(LOGICAL_PROCESSOR_RELATIONSHIP rel, DWORD *len) {
    *len = 0;
    GetLogicalProcessorInformationEx(rel, NULL, len);
    if (!*len) return NULL;
    void *buf = xrealloc(NULL, *len);
    if (GetLogicalProcessorInformationEx(rel, buf, len)) return buf;
    free(buf);
    return NULL;
}

#define FOR_EACH_INFO(it, buf, len)                                                                  \
    for (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *it = (buf); (buf) && (BYTE *)it < (BYTE *)(buf) + (len); \
         it = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((BYTE *)it + it->Size))

static bool reg_string(const char *value, char *out, DWORD size) {
    return RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", value, RRF_RT_REG_SZ,
                        NULL, out, &size) == ERROR_SUCCESS;
}

static void detect_cpu(void) {
    char buf[128], vendor[64];
    if (!reg_string("ProcessorNameString", buf, sizeof buf)) copy_str(buf, sizeof buf, "Unknown CPU");
    char *s = buf, *e = buf + strlen(buf);
    while (*s == ' ') s++;
    while (e > s && e[-1] == ' ') *--e = '\0';
    copy_str(g.cpu_name, sizeof g.cpu_name, s);
    sanitize(g.cpu_name);
    bool known = reg_string("VendorIdentifier", vendor, sizeof vendor);
    g.intel = known && !strcmp(vendor, "GenuineIntel");
#if defined(_M_X64) || defined(_M_IX86)
    int r[4];
    __cpuid(r, 1);
    unsigned eax = (unsigned)r[0];
    g.family = (int)(eax >> 8 & 0xF);
    g.model = (int)(eax >> 4 & 0xF);
    g.stepping = (int)(eax & 0xF);
    if (g.family == 6 || g.family == 15) g.model |= (int)(eax >> 12 & 0xF0);
    if (g.family == 15) g.family += (int)(eax >> 20 & 0xFF);
#endif
    g.amd = known && (!strcmp(vendor, "AuthenticAMD") || !strcmp(vendor, "HygonGenuine")) && g.family >= 0x17;
    /* "AMD64 Family 25 Model 33 Stepping 0": the values the kernel matches custom counters against. */
    char ident[128];
    const char *f = reg_string("Identifier", ident, sizeof ident) ? strstr(ident, "Family ") : NULL;
    unsigned fam = (unsigned)g.family, mod = (unsigned)g.model, step = (unsigned)g.stepping;
    if (f) sscanf(f, "Family %u Model %u Stepping %u", &fam, &mod, &step);
    const ULONG cpus[3][3] = {{fam, mod, step}, {fam, mod, ~0u}, {~0u, ~0u, ~0u}};
    memcpy(g.src_cpus, cpus, sizeof g.src_cpus);
    if (g.amd)
        snprintf(g.backend, sizeof g.backend, "AMD Zen PMU via ETW (demand + L1/L2 HW prefetch DRAM fills)");
    else
        snprintf(g.backend, sizeof g.backend, "%s via ETW (LLC misses: demand loads + prefetch/other)",
                 g.intel ? "Intel PMU" : "CPU counters");

    g.ncpus = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    g.maxcpus = (int)GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS);
    if (g.maxcpus < g.ncpus) g.maxcpus = g.ncpus;
    g.eff_cpus = g.ncpus;
    DWORD_PTR mask, sys;
    if (GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &mask, &sys)) {
        int n = 0;
        for (; mask; mask &= mask - 1) n++;
        if (n > 0) g.eff_cpus = n;
    }

    g.line = 64;
    DWORD len;
    void *caches = cpu_info(RelationCache, &len);
    BYTE level = 0;
    FOR_EACH_INFO(it, caches, len) {
        if (it->Cache.Level > level && it->Cache.LineSize) {
            level = it->Cache.Level;
            g.line = it->Cache.LineSize;
        }
    }
    free(caches);
}

unsigned long long plat_mem_available(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : ~0ULL;
}

typedef struct {
    int cpu, l3, rank, order;
} cal_cpu_t;

static int cmp_cal_cpu(const void *a, const void *b) {
    const cal_cpu_t *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    if (x->order != y->order) return x->order - y->order;
    return x->l3 - y->l3;
}

/* CPUs are numbered group * 64 + bit. Physical cores first, round-robin across last-level
 * caches, SMT siblings last, as on Linux. */
int plat_cal_cpus(int *out, int max) {
    DWORD clen, len;
    void *caches = cpu_info(RelationCache, &clen), *cores = cpu_info(RelationProcessorCore, &len);
    DWORD_PTR allowed = ~(DWORD_PTR)0, sys;
    bool one_group = GetActiveProcessorGroupCount() == 1;
    if (one_group) GetProcessAffinityMask(GetCurrentProcess(), &allowed, &sys);
    int cap = g.maxcpus + 64, n = 0;
    cal_cpu_t *c = xrealloc(NULL, (size_t)cap * sizeof *c);
    FOR_EACH_INFO(it, cores, len) {
        const GROUP_AFFINITY *ga = &it->Processor.GroupMask[0];
        int rank = 0;
        for (int bit = 0; bit < 64 && n < cap; bit++) {
            if (!(ga->Mask >> bit & 1)) continue;
            if (one_group && !(allowed >> bit & 1)) continue;
            int l3 = -1, idx = 0;
            FOR_EACH_INFO(ci, caches, clen) {
                if (ci->Cache.Level == 3 && ci->Cache.GroupMask.Group == ga->Group && (ci->Cache.GroupMask.Mask >> bit & 1))
                    l3 = idx;
                idx++;
            }
            c[n] = (cal_cpu_t){ga->Group * 64 + bit, l3, rank++ > 0, 0};
            for (int k = 0; k < n; k++)
                if (c[k].rank == c[n].rank && c[k].l3 == c[n].l3) c[n].order++;
            n++;
        }
    }
    free(caches);
    free(cores);
    qsort(c, (size_t)n, sizeof *c, cmp_cal_cpu);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = c[i].cpu;
    free(c);
    return n;
}

void plat_pin_thread(int cpu) {
    GROUP_AFFINITY ga;
    memset(&ga, 0, sizeof ga);
    ga.Group = (WORD)(cpu / 64);
    ga.Mask = (KAFFINITY)1 << (cpu % 64);
    SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL);
}

/* The trace arrives up to a second late, too late for the short calibration passes, which then
 * rely on the bytes they read. Those bytes and the misses this process caused meanwhile give the
 * factor by which the misses exceed the DRAM reads of a streaming load; plat_start_system()
 * works it out once the trace has caught up. */
void plat_probe_begin(void) {
    g.nprobe = 0;
    if (g.exact) return;
    cal_miss_t *log = xrealloc(NULL, CAL_LOG_MAX * sizeof *log);
    AcquireSRWLockExclusive(&g.lock);
    g.cal = log;
    g.ncal = 0;
    g.cal_on = true;
    ReleaseSRWLockExclusive(&g.lock);
}

double plat_probe_lines(double bytes) {
    if (g.nprobe < (int)(sizeof g.probe / sizeof g.probe[0])) {
        g.probe[g.nprobe].t = qpc();
        g.probe[g.nprobe++].bytes = bytes;
    }
    return 0.0;
}

void plat_probe_end(void) {}

/* The first probe ends the first touch and the second the warm-up pass; the timed passes follow. */
static void calibrate_scale(void) {
    AcquireSRWLockExclusive(&g.lock);
    g.cal_on = false;
    cal_miss_t *log = g.cal;
    int n = g.ncal;
    g.cal = NULL;
    ReleaseSRWLockExclusive(&g.lock);
    if (!log) return;
    if (g.nprobe >= 3 && n < CAL_LOG_MAX) {
        LONGLONG a = g.probe[1].t, b = g.probe[g.nprobe - 1].t;
        double bytes = 0, misses = 0;
        for (int i = 2; i < g.nprobe; i++) bytes += g.probe[i].bytes;
        for (int i = 0; i < n; i++)
            if (log[i].ts > a && log[i].ts <= b) misses += (double)log[i].misses;
        double k = bytes > 0 ? misses * g.line / bytes : 0.0;
        if (k >= 0.8 && k <= 2.0) g.read_scale = 1.0 / k;
    }
    free(log);
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

static bool query_procs(void) {
    for (;;) {
        ULONG need = 0;
        NTSTATUS st = NtQuerySystemInformation(SystemProcessInformation, g.spi, g.spi_size, &need);
        if (st >= 0) return true;
        if ((ULONG)st != 0xC0000004u /* STATUS_INFO_LENGTH_MISMATCH */) return false;
        g.spi_size = need + 64 * 1024;
        g.spi = xrealloc(g.spi, g.spi_size);
    }
}

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
        g.cap = g.cap ? g.cap * 2 : 256;
        g.procs = xrealloc(g.procs, (size_t)g.cap * sizeof *g.procs);
    }
    memmove(&g.procs[i + 1], &g.procs[i], (size_t)(g.nprocs - i) * sizeof *g.procs);
    g.nprocs++;
    memset(&g.procs[i], 0, sizeof g.procs[i]);
    g.procs[i].pub.pid = pid;
    return &g.procs[i];
}

static bool contains_nocase(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!_strnicmp(s, sub, n)) return true;
    return !n;
}

static void visit(const spi_t *e) {
    int pid = (int)(ULONG_PTR)e->pid;
    if (pid == 0 || (DWORD)pid == g.self) return;
    if (g.cfg.filter_pid > 0 && pid != g.cfg.filter_pid) return;
    /* Enough characters for NAME_LEN bytes; anything beyond ASCII becomes '?' anyway. */
    char name[NAME_LEN], utf8[256];
    int wlen = e->name.Length / 2 < NAME_LEN ? e->name.Length / 2 : NAME_LEN;
    int n = wlen ? WideCharToMultiByte(CP_UTF8, 0, e->name.Buffer, wlen, utf8, (int)sizeof utf8 - 1, NULL, NULL) : 0;
    utf8[n > 0 ? n : 0] = '\0';
    if (n > 0) copy_str(name, sizeof name, utf8);
    else snprintf(name, sizeof name, "pid %d", pid);
    sanitize(name);
    if (g.cfg.filter_name && !contains_nocase(name, g.cfg.filter_name)) return;

    proc_t *p = proc_find(pid);
    if (p && p->create != e->create.QuadPart) { /* PID was reused */
        memset(p, 0, sizeof *p);
        p->pub.pid = pid;
    }
    if (!p) p = proc_insert(pid);
    p->create = e->create.QuadPart;
    p->gone = 0;
    p->seen = true;
    p->pub.threads = (int)e->threads;
    memcpy(p->pub.name, name, sizeof p->pub.name);
}

/* With some ten thousand threads, reading the list takes milliseconds; CPU time comes from the
 * trace, so the list only has to catch new and exited processes. An exited process stays listed
 * while the trace still brings its last counts. */
static void scan_procs(double now) {
    g.last_scan = now;
    for (int i = 0; i < g.nprocs; i++) g.procs[i].seen = false;
    if (query_procs()) {
        for (const BYTE *b = g.spi;;) {
            const spi_t *e = (const spi_t *)b;
            visit(e);
            if (!e->next) break;
            b += e->next;
        }
    }
    int w = 0;
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        if (!p->seen && !p->gone) p->gone = now;
        if (p->seen || now - p->gone < g.lag + SCAN_SEC) g.procs[w++] = *p;
    }
    g.nprocs = w;
}

/* ------------------------------------------------------------------ */
/* Backend interface                                                   */
/* ------------------------------------------------------------------ */

const char plat_name[] = "Windows";
const bool plat_pid_children = false;

bool plat_init(const dt_config_t *cfg, dt_info_t *info) {
    g.cfg = *cfg;
    g.self = GetCurrentProcessId();
    detect_cpu();
    if (!g.amd && !intel_sources()) {
        fputs("dramtop: Windows offers no LLC-miss counter for this CPU; dramtop supports Intel and AMD Zen CPUs on "
              "Windows\n", stderr);
        return false;
    }
    if (!elevated()) {
        fputs("dramtop: reading the CPU counters needs administrator rights; run dramtop from a terminal "
              "opened as administrator\n", stderr);
        return false;
    }
    if (g.amd) {
        enable_privilege(L"SeSystemProfilePrivilege");
        bool ok = amd_sources();
        atexit(remove_sources);
        if (!ok) return false;
    }
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    g.qpc_hz = hz.QuadPart;
    g.slot_ticks = g.qpc_hz / SLOTS_PER_SEC;
    g.lag = 1.2; /* the flush timer's second, until the delay is measured */
    g.read_scale = 1.0;
    InitializeSRWLock(&g.lock);
    g.last = xrealloc(NULL, (size_t)g.maxcpus * sizeof *g.last);
    g.have = xrealloc(NULL, (size_t)g.maxcpus * sizeof *g.have);
    memset(g.have, 0, (size_t)g.maxcpus * sizeof *g.have);
    g.pend = xrealloc(NULL, PEND_MAX * sizeof *g.pend);
    /* Memory controller samples start first, so that they cover every slot of the trace. */
    g.exact = imc_open();
    if (!etw_start()) {
        imc_close();
        return false;
    }
    /* A process filter has no read/write split of its own. */
    bool filtered = cfg->filter_pid > 0 || cfg->filter_name;
    *info = (dt_info_t){
        .cpu_name = g.cpu_name, .backend = g.backend, .scope = "", .mode = g.exact ? "pawnio" : "etw",
        .ncpus = g.ncpus, .eff_cpus = g.eff_cpus, .line = g.line, .has_rw = g.exact && !filtered,
        .has_split = g.split, .rw_system_only = true, .estimated = g.exact,
    };
    return true;
}

void plat_scan(double now) { scan_procs(now); }

bool plat_has_proc(int pid) { return proc_find(pid) != NULL; }

/* The calibration's own reads are not part of what is monitored. A sample takes only slots whose
 * events have all arrived, so the first one waits that long; otherwise it would come out empty.
 * By then the calibration's misses have arrived too. */
void plat_start_system(dt_info_t *info) {
    if (g.cfg.measure_caps) {
        AcquireSRWLockExclusive(&g.lock);
        g.from_ts = qpc();
        g.drained = g.from_ts / g.slot_ticks;
        ReleaseSRWLockExclusive(&g.lock);
        EVENT_TRACE_PROPERTIES *p = new_props();
        ControlTraceW(g.session, NULL, p, EVENT_TRACE_CONTROL_FLUSH);
        free(p);
    }
    double ran = (double)(qpc() - g.from_ts) / (double)g.qpc_hz;
    if (ran < g.lag) os_sleep(g.lag - ran);
    calibrate_scale();

    if (g.exact)
        snprintf(g.backend, sizeof g.backend, "Intel memory controller via PawnIO (procs: estimated from LLC misses)");
    else if (g.read_scale != 1.0 && g.amd)
        snprintf(g.backend, sizeof g.backend, "AMD Zen PMU via ETW (DRAM fills x%.2f, from the startup calibration)",
                 g.read_scale);
    else if (g.read_scale != 1.0)
        snprintf(g.backend, sizeof g.backend, "%s via ETW (LLC misses x%.2f, from the startup calibration)",
                 g.intel ? "Intel PMU" : "CPU counters", g.read_scale);
    if (g.exact)
        snprintf(g.scope, sizeof g.scope, "Whole machine, processes via ETW (%d CPUs, Kernel+User)", g.ncpus);
    else
        snprintf(g.scope, sizeof g.scope, "Windows host via ETW (%d CPUs, Kernel+User)", g.ncpus);
    info->scope = g.scope;
}

static uint64_t lines_of(const ctr_t *a) { return a->v[V_DEMAND] + a->v[V_PREFETCH]; }

static void to_counts(const ctr_t *a, double scale, dt_counts_t *c) {
    memset(c, 0, sizeof *c);
    c->demand = (double)a->v[V_DEMAND] * scale;
    c->prefetch = (double)a->v[V_PREFETCH] * scale;
    c->total = c->demand + c->prefetch;
    c->insns = (double)a->v[V_INSNS];
    c->cycles = (double)a->v[V_CYCLES];
}

void plat_sample(double now, dt_snapshot_t *snap) {
    bool filtered = g.cfg.filter_pid > 0 || g.cfg.filter_name;
    if (now - g.last_scan >= SCAN_SEC) scan_procs(now);
    for (int i = 0; i < g.nprocs; i++) memset(&g.procs[i].acc, 0, sizeof g.procs[i].acc);

    LONGLONG t = qpc();
    long long to = (t - (LONGLONG)(g.lag * (double)g.qpc_hz)) / g.slot_ticks;
    ctr_t sys = {{0}};
    AcquireSRWLockExclusive(&g.lock);
    long long from = g.drained;
    if (to < from) to = from;
    ring_take(&g.chip, from, to, sys.v);
    for (int h = 0; h < PHASH; h++) {
        for (pacc_t **pp = &g.pacc[h]; *pp;) {
            pacc_t *a = *pp;
            ctr_t c = {{0}};
            ring_take(&a->ring, from, to, c.v);
            proc_t *p = lines_of(&c) | c.v[V_INSNS] | c.v[V_CYCLES] | c.v[V_TIME] ? proc_find((int)a->pid) : NULL;
            for (int k = 0; p && k < NVAL; k++) p->acc.v[k] += c.v[k];
            if (a->last < to - IDLE_SEC * SLOTS_PER_SEC) {
                *pp = a->next;
                free(a);
            } else {
                pp = &a->next;
            }
        }
    }
    for (int h = 0; h < THASH; h++) {
        for (thr_t **pt = &g.thr[h]; *pt;) {
            thr_t *th = *pt;
            if (th->end && th->end < t - IDLE_SEC * g.qpc_hz) {
                *pt = th->next;
                free(th);
            } else {
                pt = &th->next;
            }
        }
    }
    for (int i = 0; i < g.npend; i++)
        if (g.pend[i].ts < t - PEND_SEC * g.qpc_hz) g.pend[i--] = g.pend[--g.npend];
    g.drained = to;
    double lat = (double)g.lat_max / (double)g.qpc_hz;
    g.lat_max = 0;
    ReleaseSRWLockExclusive(&g.lock);

    /* The newest slot taken trails the clock by the delay of the slowest recent event. The delay
     * moves by at most half an interval per sample, so no interval comes out empty. */
    double dt = g.last_sample > 0 ? now - g.last_sample : 0.0;
    g.last_sample = now;
    g.lat_peak = lat > g.lat_peak - 0.05 * dt ? lat : g.lat_peak - 0.05 * dt;
    double target = g.lat_peak + 0.1, step = 0.5 * dt;
    if (target < LAG_MIN) target = LAG_MIN;
    if (target > LAG_MAX) target = LAG_MAX;
    g.lag = target > g.lag + step ? g.lag + step : target < g.lag - step ? g.lag - step : target;

    /* With the memory controller, each miss stands for its share of the cores' traffic, writes
     * included; that share is capped, so traffic that causes no misses is left unattributed. */
    double imc[IMC_NREG], scale = g.read_scale, conf = 1.0;
    bool exact = g.exact && to > from && imc_span((from + 1) * g.slot_ticks, (to + 1) * g.slot_ticks, imc);
    if (exact) {
        double cores = g.imc_ia ? imc[IMC_IA] : imc[IMC_RD] + imc[IMC_WR];
        scale = lines_of(&sys) ? cores / (double)lines_of(&sys) : 0.0;
        if (scale > IMC_SCALE_MAX) {
            conf = IMC_SCALE_MAX / scale;
            scale = IMC_SCALE_MAX;
        }
    }

    /* dramtop.c divides CPU time by the time between samples, which differs a little from the
     * span of the slots. */
    double bw_sec = (double)(to - from) / SLOTS_PER_SEC, to_sec = bw_sec > 0 ? dt / bw_sec / (double)g.qpc_hz : 0.0;
    ctr_t attributed = {{0}};
    for (int i = 0; i < g.nprocs; i++) {
        proc_t *p = &g.procs[i];
        p->pub.cpu_sec = (double)p->acc.v[V_TIME] * to_sec;
        p->pub.has_bw = p->pub.has_ipc = bw_sec > 0;
        p->pub.bw_sec = bw_sec;
        p->pub.confidence = conf;
        to_counts(&p->acc, scale, &p->pub.c);
        for (int k = 0; k < NVAL; k++) attributed.v[k] += p->acc.v[k];
    }

    memset(snap, 0, sizeof *snap);
    if (filtered) {
        to_counts(&attributed, scale, &snap->sys);
    } else {
        ctr_t rest;
        for (int k = 0; k < NVAL; k++) rest.v[k] = sys.v[k] > attributed.v[k] ? sys.v[k] - attributed.v[k] : 0;
        to_counts(&sys, scale, &snap->sys);
        to_counts(&rest, scale, &snap->other);
        if (exact) {
            double listed = (double)lines_of(&attributed) * scale;
            snap->sys.read = imc[IMC_RD];
            snap->sys.write = imc[IMC_WR];
            snap->sys.total = imc[IMC_RD] + imc[IMC_WR];
            snap->other.total = snap->sys.total > listed ? snap->sys.total - listed : 0.0;
        }
        snap->has_other = true;
    }
    snap->bw_sec = bw_sec;
    if (g.list_cap < g.nprocs) {
        g.list_cap = g.nprocs + 64;
        g.list = xrealloc(g.list, (size_t)g.list_cap * sizeof *g.list);
    }
    for (int i = 0; i < g.nprocs; i++) g.list[i] = &g.procs[i].pub;
    snap->procs = g.list;
    snap->nprocs = g.nprocs;
}

void plat_shutdown(void) {
    imc_close();
    etw_stop();
    remove_sources(); /* after the session that used them */
}
