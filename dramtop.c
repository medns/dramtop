/*
 * dramtop.c - Real-time DRAM Memory Bandwidth & Bottleneck Monitor for Linux
 *
 * A zero-dependency, single-file C utility that uses Linux `perf_event_open`
 * hardware PMU counters directly (no external `perf` binary required) to
 * measure real-time system-wide & per-process DRAM memory channel bandwidth
 * (GB/s), L1/L2 hardware prefetcher traffic, IPC, and diagnose Memory-Bound
 * vs Compute-Bound vs Lock-Contention states (ideal for LLM inference like
 * llama.cpp / vLLM / SGLang / Ollama / HPC / Databases).
 *
 * Build:
 *   gcc -O3 -march=native -pthread dramtop.c -o dramtop -lm
 *   # Or portable static binary (with musl-gcc):
 *   musl-gcc -O3 -static -pthread dramtop.c -o dramtop -lm
 *
 * Author: Super Zheng (https://github.com/medns)
 * License: MIT
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#define DRAMTOP_VERSION "1.0.0"
#define CACHE_LINE_BYTES 64ULL
#define MAX_CPUS 512
#define MAX_PROCS 96
#define MAX_TIDS_PER_PROC 512
#define MAX_EVENTS 8
#define RING_SIZE 32

typedef enum {
    ARCH_UNKNOWN = 0,
    ARCH_AMD_ZEN,
    ARCH_INTEL,
    ARCH_ARM64
} cpu_arch_t;

typedef enum {
    VIEW_SPLIT = 0,      /* Top Processes Table + Rolling History Timeline */
    VIEW_PROCS_ONLY = 1, /* Full-screen Top Processes Table */
    VIEW_HIST_ONLY = 2   /* Full-screen Rolling History Timeline */
} view_mode_t;

typedef struct {
    const char *name;
    uint32_t type;
    uint64_t config;
    uint64_t config1;
    bool is_demand_dram;
    bool is_prefetch_dram;
    bool is_cycles;
    bool is_instructions;
} pmu_event_def_t;

typedef struct {
    bool active;
    bool alive_in_scan;
    pid_t pid;
    char comm[64];
    int num_threads;
    int opened_tids;
    int fds[MAX_TIDS_PER_PROC][MAX_EVENTS];
    uint64_t prev_vals[MAX_EVENTS];
    uint64_t prev_cpu_ticks;
    bool initialized;

    /* Current sample metrics */
    double demand_bw_gbs;
    double prefetch_bw_gbs;
    double total_bw_gbs;
    double util_pct;
    double cpu_pct;
    double ipc;
    char status_tag[64];
    int status_color;
} tracked_proc_t;

typedef struct {
    bool available;
    bool include_kernel;
    int num_cpus;
    int opened_cpus;
    int fds[MAX_CPUS][MAX_EVENTS];
    uint64_t prev_vals[MAX_EVENTS];
} sys_cpu_pmu_t;

typedef struct {
    char time_str[16];
    double timestamp;
    char top_comm[64];
    pid_t top_pid;
    double total_bw_gbs;
    double demand_bw_gbs;
    double prefetch_bw_gbs;
    double util_pct;
    double cpu_pct;
    double ipc;
    long throttled_delta;
    char status_tag[64];
    int status_color;
} sample_metrics_t;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_winch = 0;
static struct termios g_orig_termios;
static bool g_tui_active = false;
static tracked_proc_t g_procs[MAX_PROCS];

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static void handle_sigwinch(int sig) {
    (void)sig;
    g_winch = 1;
}

static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = (rl.rlim_max > 65536) ? 65536 : rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

static void leave_tui_mode(void) {
    if (g_tui_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        printf("\033[?25h\033[0m\033[?1049l");
        fflush(stdout);
        g_tui_active = false;
    }
}

static void enter_tui_mode(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        g_tui_active = true;
        atexit(leave_tui_mode);
        printf("\033[?1049h\033[2J\033[H\033[?25l");
        fflush(stdout);
    }
}

static int get_term_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return (int)ws.ws_row;
    }
    return 28;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long sys_perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                                int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static cpu_arch_t detect_cpu_arch(char *model_buf, size_t model_len) {
    snprintf(model_buf, model_len, "Generic Linux CPU");
    cpu_arch_t arch = ARCH_UNKNOWN;
#if defined(__aarch64__)
    snprintf(model_buf, model_len, "ARM64 Processor");
    arch = ARCH_ARM64;
#endif
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return arch;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "vendor_id", 9) == 0) {
            if (strstr(line, "AuthenticAMD")) arch = ARCH_AMD_ZEN;
            else if (strstr(line, "GenuineIntel")) arch = ARCH_INTEL;
        } else if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Processor", 9) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                char *nl = strchr(colon, '\n');
                if (nl) *nl = '\0';
                if (colon[0]) snprintf(model_buf, model_len, "%s", colon);
            }
        }
    }
    fclose(fp);
    return arch;
}

static long read_cgroup_throttled(void) {
    const char *paths[] = {
        "/sys/fs/cgroup/cpu/cpu.stat",
        "/sys/fs/cgroup/cpu.stat",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *fp = fopen(paths[i], "r");
        if (!fp) continue;
        char line[128];
        long val = -1;
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "nr_throttled %ld", &val) == 1) {
                fclose(fp);
                return val;
            }
        }
        fclose(fp);
    }
    return -1;
}

static double detect_effective_cpus(int *logical_cpus_out) {
    int logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (logical < 1) logical = 1;
    if (logical_cpus_out) *logical_cpus_out = logical;

    FILE *fq = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r");
    FILE *fp = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r");
    if (fq && fp) {
        long quota = -1, period = 100000;
        if (fscanf(fq, "%ld", &quota) == 1 && fscanf(fp, "%ld", &period) == 1) {
            if (quota > 0 && period > 0) {
                fclose(fq);
                fclose(fp);
                return (double)quota / (double)period;
            }
        }
    }
    if (fq) fclose(fq);
    if (fp) fclose(fp);

    FILE *fmax = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (fmax) {
        long quota = -1, period = 100000;
        if (fscanf(fmax, "%ld %ld", &quota, &period) == 2 && quota > 0 && period > 0) {
            fclose(fmax);
            return (double)quota / (double)period;
        }
        fclose(fmax);
    }
    return (double)logical;
}

/* Probe whether a specific PMU event config is supported by the running CPU/kernel */
static bool probe_pmu_event(uint32_t type, uint64_t config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = type;
    pe.size = sizeof(pe);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    long fd = sys_perf_event_open(&pe, 0, -1, -1, 0);
    if (fd >= 0) {
        close((int)fd);
        return true;
    }
    return false;
}

static int build_pmu_events(cpu_arch_t arch, pmu_event_def_t *events, const char **backend_desc) {
    int n = 0;
    if (arch == ARCH_AMD_ZEN && probe_pmu_event(PERF_TYPE_RAW, 0x0843ULL)) {
        *backend_desc = "AMD Zen Core PMU (L1/L2 Demand + HW Prefetch DRAM Fills)";
        events[n++] = (pmu_event_def_t){"lcl_dram_demand", PERF_TYPE_RAW, 0x0843ULL, 0, true, false, false, false};
        events[n++] = (pmu_event_def_t){"rmt_dram_demand", PERF_TYPE_RAW, 0x4043ULL, 0, true, false, false, false};
        events[n++] = (pmu_event_def_t){"lcl_dram_l1pf",   PERF_TYPE_RAW, 0x085aULL, 0, false, true, false, false};
        events[n++] = (pmu_event_def_t){"rmt_dram_l1pf",   PERF_TYPE_RAW, 0x405aULL, 0, false, true, false, false};
        events[n++] = (pmu_event_def_t){"dram_l2pf",       PERF_TYPE_RAW, 0xff72ULL, 0, false, true, false, false};
    } else if (arch == ARCH_INTEL && probe_pmu_event(PERF_TYPE_RAW, 0x20d1ULL)) {
        /*
         * Intel Core PMU:
         * - MEM_LOAD_RETIRED.L3_MISS (0x20d1): Demand data loads that missed L3 (served from DRAM)
         * - PERF_COUNT_HW_CACHE_MISSES (0x412e): Total LLC misses (Demand + L2 HW Prefetch).
         *   Only add L2_RQSTS.PF_MISS (0x3824) if supported; otherwise use CACHE_MISSES as total.
         */
        if (probe_pmu_event(PERF_TYPE_RAW, 0x3824ULL)) {
            *backend_desc = "Intel Core PMU (MEM_LOAD_RETIRED.L3_MISS + L2_RQSTS.PF_MISS)";
            events[n++] = (pmu_event_def_t){"l3_miss_demand", PERF_TYPE_RAW, 0x20d1ULL, 0, true, false, false, false};
            events[n++] = (pmu_event_def_t){"l2_pf_miss",     PERF_TYPE_RAW, 0x3824ULL, 0, false, true, false, false};
        } else {
            *backend_desc = "Intel Core PMU (Hardware LLC Misses to DRAM * 64B)";
            events[n++] = (pmu_event_def_t){"llc_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, 0, true, false, false, false};
        }
    } else if (arch == ARCH_ARM64 && probe_pmu_event(PERF_TYPE_RAW, 0x0017ULL)) {
        /* ARMv8/v9 PMUv3: L2D_CACHE_REFILL (0x0017) or LL_CACHE_MISS_RD (0x0037) */
        if (probe_pmu_event(PERF_TYPE_RAW, 0x0037ULL)) {
            *backend_desc = "ARM64 PMUv3 (LL_CACHE_MISS_RD * 64B)";
            events[n++] = (pmu_event_def_t){"ll_cache_miss", PERF_TYPE_RAW, 0x0037ULL, 0, true, false, false, false};
        } else {
            *backend_desc = "ARM64 PMUv3 (L2D_CACHE_REFILL * 64B)";
            events[n++] = (pmu_event_def_t){"l2d_refill", PERF_TYPE_RAW, 0x0017ULL, 0, true, false, false, false};
        }
    } else {
        *backend_desc = "Generic Linux PMU (Hardware LLC Cache Misses * 64B)";
        events[n++] = (pmu_event_def_t){"cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, 0, true, false, false, false};
    }

    events[n++] = (pmu_event_def_t){"cycles",       PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,   0, false, false, true, false};
    events[n++] = (pmu_event_def_t){"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, 0, false, false, false, true};
    return n;
}

static bool is_user_process(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    char ch = 0;
    size_t n = fread(&ch, 1, 1, fp);
    fclose(fp);
    return n > 0; /* Kernel threads have empty /proc/<pid>/cmdline */
}

static bool read_proc_stat(pid_t pid, uint64_t *cpu_ticks_out, int *num_threads_out, char *comm_out, size_t comm_len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char buf[1024];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    char *lpar = strchr(buf, '(');
    char *rpar = strrchr(buf, ')');
    if (!lpar || !rpar || rpar < lpar) return false;

    if (comm_out && comm_len > 0) {
        size_t len = (size_t)(rpar - lpar - 1);
        if (len >= comm_len) len = comm_len - 1;
        memcpy(comm_out, lpar + 1, len);
        comm_out[len] = '\0';

        /* If thread renamed itself to generic "MainThread", resolve real binary from /proc/<pid>/cmdline */
        if (strcmp(comm_out, "MainThread") == 0) {
            char cmdpath[64];
            snprintf(cmdpath, sizeof(cmdpath), "/proc/%d/cmdline", (int)pid);
            FILE *cfp = fopen(cmdpath, "r");
            if (cfp) {
                char cmdbuf[256] = {0};
                size_t cn = fread(cmdbuf, 1, sizeof(cmdbuf) - 1, cfp);
                fclose(cfp);
                if (cn > 0) {
                    cmdbuf[cn] = '\0';
                    char *slash = strrchr(cmdbuf, '/');
                    const char *base = slash ? (slash + 1) : cmdbuf;
                    if (base[0]) snprintf(comm_out, comm_len, "%s", base);
                }
            }
        }
    }

    unsigned long utime = 0, stime = 0;
    long num_threads = 1;
    int n = sscanf(rpar + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %ld",
                   &utime, &stime, &num_threads);
    if (n >= 2 && cpu_ticks_out) {
        *cpu_ticks_out = (uint64_t)utime + (uint64_t)stime;
    }
    if (n >= 3 && num_threads_out) {
        *num_threads_out = (int)num_threads;
    }
    return true;
}

static void close_tracked_proc(tracked_proc_t *p, int num_events) {
    if (!p->active) return;
    for (int t = 0; t < p->opened_tids; t++) {
        for (int e = 0; e < num_events; e++) {
            if (p->fds[t][e] >= 0) {
                close(p->fds[t][e]);
                p->fds[t][e] = -1;
            }
        }
    }
    p->opened_tids = 0;
    p->active = false;
    p->initialized = false;
}

static bool open_pmu_for_proc(tracked_proc_t *p, const pmu_event_def_t *events, int num_events) {
    char task_path[64];
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", (int)p->pid);
    DIR *dir = opendir(task_path);
    if (!dir) return false;

    p->opened_tids = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && p->opened_tids < MAX_TIDS_PER_PROC) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        pid_t tid = (pid_t)atoi(ent->d_name);
        if (tid <= 0) continue;

        int t_idx = p->opened_tids;
        bool any_opened = false;
        for (int e = 0; e < num_events; e++) {
            p->fds[t_idx][e] = -1;
            struct perf_event_attr pe;
            memset(&pe, 0, sizeof(pe));
            pe.type = events[e].type;
            pe.size = sizeof(pe);
            pe.config = events[e].config;
            pe.config1 = events[e].config1;
            pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            pe.disabled = 0;
            pe.inherit = 1;
            pe.exclude_kernel = 0;
            pe.exclude_hv = 0;

            long fd = sys_perf_event_open(&pe, tid, -1, -1, 0);
            if (fd < 0 && (errno == EACCES || errno == EPERM)) {
                pe.exclude_kernel = 1;
                pe.exclude_hv = 1;
                fd = sys_perf_event_open(&pe, tid, -1, -1, 0);
            }
            p->fds[t_idx][e] = (int)fd;
            if (fd >= 0) any_opened = true;
        }
        if (any_opened) {
            p->opened_tids++;
        }
    }
    closedir(dir);
    return p->opened_tids > 0;
}

static void read_proc_pmu_totals(tracked_proc_t *p, int num_events, uint64_t *totals_out) {
    for (int e = 0; e < num_events; e++) totals_out[e] = 0;
    for (int t = 0; t < p->opened_tids; t++) {
        for (int e = 0; e < num_events; e++) {
            int fd = p->fds[t][e];
            if (fd >= 0) {
                uint64_t buf[3] = {0, 0, 0};
                if (read(fd, buf, sizeof(buf)) == sizeof(buf)) {
                    uint64_t scaled = buf[0];
                    if (buf[2] > 0 && buf[1] > buf[2]) {
                        scaled = (uint64_t)((double)buf[0] * ((double)buf[1] / (double)buf[2]));
                    }
                    totals_out[e] += scaled;
                }
            }
        }
    }
}

/*
 * Tier-1 System-Wide Per-CPU Hardware PMU Engine (pid = -1, cpu = 0..N-1)
 * Automatically used on bare-metal hosts or privileged containers where
 * perf_event_paranoid <= 0 or CAP_PERFMON / CAP_SYS_ADMIN is available.
 * Captures 100% of system DRAM traffic including kernel threads, page-cache,
 * and short-lived ephemeral processes.
 */
static void close_sys_cpu_pmu(sys_cpu_pmu_t *s, int num_events) {
    if (!s->available) return;
    for (int c = 0; c < s->num_cpus; c++) {
        for (int e = 0; e < num_events; e++) {
            if (s->fds[c][e] >= 0) {
                close(s->fds[c][e]);
                s->fds[c][e] = -1;
            }
        }
    }
    s->available = false;
    s->opened_cpus = 0;
}

static bool open_sys_cpu_pmu(sys_cpu_pmu_t *s, int num_cpus, const pmu_event_def_t *events, int num_events) {
    memset(s, 0, sizeof(*s));
    if (num_cpus > MAX_CPUS) num_cpus = MAX_CPUS;
    s->num_cpus = num_cpus;
    for (int c = 0; c < MAX_CPUS; c++) {
        for (int e = 0; e < MAX_EVENTS; e++) s->fds[c][e] = -1;
    }

    int excl_kernel = 0;
    struct perf_event_attr probe_pe;
    memset(&probe_pe, 0, sizeof(probe_pe));
    probe_pe.type = events[0].type;
    probe_pe.size = sizeof(probe_pe);
    probe_pe.config = events[0].config;
    probe_pe.config1 = events[0].config1;
    probe_pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    probe_pe.exclude_kernel = 0;
    probe_pe.exclude_hv = 0;

    long probe_fd = sys_perf_event_open(&probe_pe, -1, 0, -1, 0);
    if (probe_fd < 0) {
        probe_pe.exclude_kernel = 1;
        probe_pe.exclude_hv = 1;
        probe_fd = sys_perf_event_open(&probe_pe, -1, 0, -1, 0);
        if (probe_fd < 0) {
            return false; /* Blocked by perf_event_paranoid in unprivileged container */
        }
        excl_kernel = 1;
    }
    close((int)probe_fd);

    s->include_kernel = (excl_kernel == 0);
    s->opened_cpus = 0;

    for (int c = 0; c < num_cpus; c++) {
        bool cpu_ok = false;
        for (int e = 0; e < num_events; e++) {
            struct perf_event_attr pe;
            memset(&pe, 0, sizeof(pe));
            pe.type = events[e].type;
            pe.size = sizeof(pe);
            pe.config = events[e].config;
            pe.config1 = events[e].config1;
            pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            pe.disabled = 0;
            pe.exclude_kernel = excl_kernel;
            pe.exclude_hv = excl_kernel;

            long fd = sys_perf_event_open(&pe, -1, c, -1, 0);
            s->fds[c][e] = (int)fd;
            if (fd >= 0) cpu_ok = true;
        }
        if (cpu_ok) s->opened_cpus++;
    }

    if (s->opened_cpus == 0) {
        return false;
    }
    s->available = true;
    return true;
}

static void read_sys_cpu_pmu_totals(sys_cpu_pmu_t *s, int num_events, uint64_t *totals_out) {
    for (int e = 0; e < num_events; e++) totals_out[e] = 0;
    if (!s->available) return;
    for (int c = 0; c < s->num_cpus; c++) {
        for (int e = 0; e < num_events; e++) {
            int fd = s->fds[c][e];
            if (fd >= 0) {
                uint64_t buf[3] = {0, 0, 0};
                if (read(fd, buf, sizeof(buf)) == sizeof(buf)) {
                    uint64_t scaled = buf[0];
                    if (buf[2] > 0 && buf[1] > buf[2]) {
                        scaled = (uint64_t)((double)buf[0] * ((double)buf[1] / (double)buf[2]));
                    }
                    totals_out[e] += scaled;
                }
            }
        }
    }
}

/*
 * Built-in Zero-Config Automatic DRAM Bandwidth Calibrator
 */
typedef struct {
    const uint8_t *buf;
    size_t start_offset;
    size_t end_offset;
    uint64_t sink;
} bw_worker_arg_t;

static void *bw_read_worker(void *arg) {
    bw_worker_arg_t *w = (bw_worker_arg_t *)arg;
    const uint8_t *p = w->buf + w->start_offset;
    const uint8_t *end = w->buf + w->end_offset;
    uint64_t sum = 0;

#if defined(__AVX512F__)
    __m512i acc0 = _mm512_setzero_si512();
    __m512i acc1 = _mm512_setzero_si512();
    for (; p + 128 <= end; p += 128) {
        __m512i v0 = _mm512_load_si512((const __m512i *)p);
        __m512i v1 = _mm512_load_si512((const __m512i *)(p + 64));
        acc0 = _mm512_add_epi64(acc0, v0);
        acc1 = _mm512_add_epi64(acc1, v1);
    }
    acc0 = _mm512_add_epi64(acc0, acc1);
    uint64_t tmp[8];
    _mm512_storeu_si512((__m512i *)tmp, acc0);
    for (int i = 0; i < 8; i++) sum += tmp[i];
#elif defined(__AVX2__)
    __m256i acc0 = _mm256_setzero_si256();
    __m256i acc1 = _mm256_setzero_si256();
    for (; p + 64 <= end; p += 64) {
        __m256i v0 = _mm256_load_si256((const __m256i *)p);
        __m256i v1 = _mm256_load_si256((const __m256i *)(p + 32));
        acc0 = _mm256_add_epi64(acc0, v0);
        acc1 = _mm256_add_epi64(acc1, v1);
    }
    acc0 = _mm256_add_epi64(acc0, acc1);
    uint64_t tmp[4];
    _mm256_storeu_si256((__m256i *)tmp, acc0);
    for (int i = 0; i < 4; i++) sum += tmp[i];
#elif defined(__aarch64__)
    uint64x2_t acc0 = vdupq_n_u64(0);
    uint64x2_t acc1 = vdupq_n_u64(0);
    for (; p + 64 <= end; p += 64) {
        uint64x2_t v0 = vld1q_u64((const uint64_t *)p);
        uint64x2_t v1 = vld1q_u64((const uint64_t *)(p + 16));
        uint64x2_t v2 = vld1q_u64((const uint64_t *)(p + 32));
        uint64x2_t v3 = vld1q_u64((const uint64_t *)(p + 48));
        acc0 = vaddq_u64(acc0, vaddq_u64(v0, v1));
        acc1 = vaddq_u64(acc1, vaddq_u64(v2, v3));
    }
    acc0 = vaddq_u64(acc0, acc1);
    sum += vgetq_lane_u64(acc0, 0) + vgetq_lane_u64(acc0, 1);
#else
    const volatile uint64_t *q = (const volatile uint64_t *)p;
    const volatile uint64_t *qend = (const volatile uint64_t *)end;
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (; q + 8 <= qend; q += 8) {
        s0 += q[0] + q[1];
        s1 += q[2] + q[3];
        s2 += q[4] + q[5];
        s3 += q[6] + q[7];
    }
    sum += s0 + s1 + s2 + s3;
#endif
    w->sink = sum;
    return NULL;
}

static double auto_calibrate_peak_bandwidth(double effective_cpus, const pmu_event_def_t *events, int num_events) {
    const size_t buf_size = 512ULL * 1024ULL * 1024ULL;
    void *raw = NULL;
    if (posix_memalign(&raw, 64, buf_size) != 0 || !raw) {
        return 100.0;
    }
    memset(raw, 0x5A, buf_size);

    tracked_proc_t self_proc;
    memset(&self_proc, 0, sizeof(self_proc));
    self_proc.pid = getpid();
    self_proc.active = true;
    bool has_pmu = open_pmu_for_proc(&self_proc, events, num_events);

    int max_t = (int)ceil(effective_cpus);
    if (max_t > 64) max_t = 64;
    if (max_t < 1) max_t = 1;
    int half_t = max_t / 2;
    if (half_t < 1) half_t = 1;

    int test_threads[2] = {half_t, max_t};
    int num_tests = (half_t == max_t) ? 1 : 2;
    double best_bw = 0.0;

    for (int c = 0; c < num_tests; c++) {
        int nt = test_threads[c];
        pthread_t threads[64];
        bw_worker_arg_t args[64];
        size_t chunk = (buf_size / (size_t)nt) & ~127ULL;

        for (int iter = 0; iter < 2; iter++) {
            uint64_t pmu_start[MAX_EVENTS] = {0};
            if (has_pmu) read_proc_pmu_totals(&self_proc, num_events, pmu_start);

            double t0 = now_sec();
            for (int i = 0; i < nt; i++) {
                args[i].buf = (const uint8_t *)raw;
                args[i].start_offset = (size_t)i * chunk;
                args[i].end_offset = (i == nt - 1) ? (buf_size & ~127ULL) : ((size_t)(i + 1) * chunk);
                args[i].sink = 0;
                pthread_create(&threads[i], NULL, bw_read_worker, &args[i]);
            }
            for (int i = 0; i < nt; i++) {
                pthread_join(threads[i], NULL);
            }
            double dt = now_sec() - t0;

            double simd_gbs = (dt > 0.0001) ? ((double)buf_size / dt / 1e9) : 0.0;
            double pmu_gbs = 0.0;
            if (has_pmu && dt > 0.0001) {
                uint64_t pmu_end[MAX_EVENTS] = {0};
                read_proc_pmu_totals(&self_proc, num_events, pmu_end);
                uint64_t lines = 0;
                for (int e = 0; e < num_events; e++) {
                    if (events[e].is_demand_dram || events[e].is_prefetch_dram) {
                        if (pmu_end[e] > pmu_start[e]) lines += (pmu_end[e] - pmu_start[e]);
                    }
                }
                pmu_gbs = (double)(lines * CACHE_LINE_BYTES) / dt / 1e9;
            }

            double measured = (pmu_gbs > simd_gbs) ? pmu_gbs : simd_gbs;
            if (measured > best_bw) best_bw = measured;
        }
    }

    if (has_pmu) close_tracked_proc(&self_proc, num_events);
    free(raw);
    return best_bw > 5.0 ? best_bw : 100.0;
}

static void diagnose_bottleneck(double total_bw_gbs, double util_pct, double cpu_pct,
                                double ipc, long throttled_delta, double effective_cpus,
                                char *status_out, size_t status_len, int *color_out) {
    double cpu_quota_ratio = (cpu_pct / 100.0) / effective_cpus;
    if (total_bw_gbs < 0.25 && cpu_pct < 5.0) {
        snprintf(status_out, status_len, "IDLE");
        *color_out = 90;
    } else if (util_pct >= 42.0) {
        snprintf(status_out, status_len, "MEMORY-BOUND (Decode / BW Saturated)");
        *color_out = 32;
    } else if ((throttled_delta > 15 || (cpu_quota_ratio > 0.88 && ipc < 0.32)) && util_pct < 42.0) {
        snprintf(status_out, status_len, "LOCK-CONTENTION / THROTTLED");
        *color_out = 31;
    } else if (cpu_pct > 150.0 && util_pct < 42.0) {
        snprintf(status_out, status_len, "COMPUTE-BOUND (Prefill / Matrix Math)");
        *color_out = 34;
    } else {
        snprintf(status_out, status_len, "ACTIVE (Mixed Workload)");
        *color_out = 33;
    }
}

/* Scan /proc and update tracked processes */
static int sync_and_sample_processes(pid_t filter_pid, const char *filter_name,
                                     const pmu_event_def_t *events, int num_events,
                                     double dt, long clk_tck, double effective_cpus,
                                     double *peak_bw_gbs_inout, tracked_proc_t **sorted_out) {
    for (int i = 0; i < MAX_PROCS; i++) {
        g_procs[i].alive_in_scan = false;
    }

    DIR *dir = opendir("/proc");
    if (!dir) return 0;

    pid_t self_pid = getpid();
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 1 || pid == self_pid) continue;
        if (filter_pid > 0 && pid != filter_pid) continue;
        if (!is_user_process(pid)) continue;

        char comm[64] = {0};
        uint64_t ticks = 0;
        int nthreads = 1;
        if (!read_proc_stat(pid, &ticks, &nthreads, comm, sizeof(comm))) continue;
        if (filter_name && filter_name[0] && strstr(comm, filter_name) == NULL) continue;

        int slot = -1;
        int free_slot = -1;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (g_procs[i].active && g_procs[i].pid == pid) {
                slot = i;
                break;
            }
            if (!g_procs[i].active && free_slot < 0) {
                free_slot = i;
            }
        }

        if (slot < 0) {
            if (free_slot < 0) continue;
            slot = free_slot;
            memset(&g_procs[slot], 0, sizeof(tracked_proc_t));
            g_procs[slot].pid = pid;
            snprintf(g_procs[slot].comm, sizeof(g_procs[slot].comm), "%s", comm);
            g_procs[slot].num_threads = nthreads;
            if (!open_pmu_for_proc(&g_procs[slot], events, num_events)) {
                continue;
            }
            g_procs[slot].active = true;
            g_procs[slot].alive_in_scan = true;
            g_procs[slot].prev_cpu_ticks = ticks;
            read_proc_pmu_totals(&g_procs[slot], num_events, g_procs[slot].prev_vals);
            g_procs[slot].initialized = true;
            diagnose_bottleneck(0, 0, 0, 0, 0, effective_cpus,
                                g_procs[slot].status_tag, sizeof(g_procs[slot].status_tag),
                                &g_procs[slot].status_color);
            continue;
        }

        tracked_proc_t *p = &g_procs[slot];
        p->alive_in_scan = true;
        snprintf(p->comm, sizeof(p->comm), "%s", comm);

        if (nthreads != p->num_threads) {
            p->num_threads = nthreads;
            close_tracked_proc(p, num_events);
            p->pid = pid;
            if (open_pmu_for_proc(p, events, num_events)) {
                p->active = true;
                p->alive_in_scan = true;
                p->prev_cpu_ticks = ticks;
                read_proc_pmu_totals(p, num_events, p->prev_vals);
                p->initialized = true;
            }
            continue;
        }

        uint64_t ticks_delta = (ticks >= p->prev_cpu_ticks) ? (ticks - p->prev_cpu_ticks) : 0;
        p->prev_cpu_ticks = ticks;

        /* Fast-path: if process had 0 CPU ticks and 0 previous bandwidth, skip syscall read */
        if (ticks_delta == 0 && p->total_bw_gbs < 0.01 && p->cpu_pct < 0.1) {
            p->demand_bw_gbs = 0.0;
            p->prefetch_bw_gbs = 0.0;
            p->total_bw_gbs = 0.0;
            p->util_pct = 0.0;
            p->cpu_pct = 0.0;
            p->ipc = 0.0;
            diagnose_bottleneck(0, 0, 0, 0, 0, effective_cpus,
                                p->status_tag, sizeof(p->status_tag), &p->status_color);
            continue;
        }

        uint64_t cur_vals[MAX_EVENTS];
        read_proc_pmu_totals(p, num_events, cur_vals);

        uint64_t demand_lines = 0;
        uint64_t prefetch_lines = 0;
        uint64_t cycles_delta = 0;
        uint64_t insns_delta = 0;

        for (int e = 0; e < num_events; e++) {
            uint64_t delta = (cur_vals[e] >= p->prev_vals[e]) ? (cur_vals[e] - p->prev_vals[e]) : 0;
            p->prev_vals[e] = cur_vals[e];
            if (events[e].is_demand_dram) demand_lines += delta;
            if (events[e].is_prefetch_dram) prefetch_lines += delta;
            if (events[e].is_cycles) cycles_delta += delta;
            if (events[e].is_instructions) insns_delta += delta;
        }

        p->demand_bw_gbs = (double)(demand_lines * CACHE_LINE_BYTES) / dt / 1e9;
        p->prefetch_bw_gbs = (double)(prefetch_lines * CACHE_LINE_BYTES) / dt / 1e9;
        p->total_bw_gbs = p->demand_bw_gbs + p->prefetch_bw_gbs;
        if (p->total_bw_gbs > *peak_bw_gbs_inout) {
            *peak_bw_gbs_inout = p->total_bw_gbs;
        }
        p->util_pct = (p->total_bw_gbs / *peak_bw_gbs_inout) * 100.0;
        if (p->util_pct > 100.0) p->util_pct = 100.0;

        p->cpu_pct = ((double)ticks_delta / (double)clk_tck) / dt * 100.0;
        p->ipc = (cycles_delta > 0) ? ((double)insns_delta / (double)cycles_delta) : 0.0;

        diagnose_bottleneck(p->total_bw_gbs, p->util_pct, p->cpu_pct, p->ipc, 0,
                            effective_cpus, p->status_tag, sizeof(p->status_tag), &p->status_color);
    }
    closedir(dir);

    int count = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (g_procs[i].active) {
            if (!g_procs[i].alive_in_scan) {
                close_tracked_proc(&g_procs[i], num_events);
            } else {
                sorted_out[count++] = &g_procs[i];
            }
        }
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            bool swap = false;
            if (sorted_out[j]->total_bw_gbs > sorted_out[i]->total_bw_gbs + 0.005) {
                swap = true;
            } else if (fabs(sorted_out[j]->total_bw_gbs - sorted_out[i]->total_bw_gbs) <= 0.005) {
                if (sorted_out[j]->cpu_pct > sorted_out[i]->cpu_pct + 0.1) {
                    swap = true;
                } else if (fabs(sorted_out[j]->cpu_pct - sorted_out[i]->cpu_pct) <= 0.1 &&
                           sorted_out[j]->num_threads > sorted_out[i]->num_threads) {
                    swap = true;
                }
            }
            if (swap) {
                tracked_proc_t *tmp = sorted_out[i];
                sorted_out[i] = sorted_out[j];
                sorted_out[j] = tmp;
            }
        }
    }

    return count;
}

static void format_bar(double pct, int width, bool use_color, char *out, size_t out_len) {
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    int filled = (int)round((pct / 100.0) * width);
    if (filled > width) filled = width;

    const char *color = "";
    const char *reset = "";
    if (use_color) {
        color = "\033[1;32m";
        if (pct >= 70.0) color = "\033[1;31m";
        else if (pct >= 35.0) color = "\033[1;33m";
        reset = "\033[0m";
    }

    int pos = snprintf(out, out_len, "[%s", color);
    for (int i = 0; i < width && pos < (int)out_len - 16; i++) {
        out[pos++] = (i < filled) ? '#' : '-';
    }
    snprintf(out + pos, out_len - (size_t)pos, "%s]", reset);
}

static const double g_interval_steps[] = {0.10, 0.25, 0.50, 1.00, 2.00, 5.00};
#define NUM_INTERVAL_STEPS ((int)(sizeof(g_interval_steps) / sizeof(g_interval_steps[0])))

static double step_interval(double cur, int dir) {
    int best_idx = 3;
    double min_diff = 1e9;
    for (int i = 0; i < NUM_INTERVAL_STEPS; i++) {
        double d = fabs(cur - g_interval_steps[i]);
        if (d < min_diff) {
            min_diff = d;
            best_idx = i;
        }
    }
    int next_idx = best_idx + dir;
    if (next_idx < 0) next_idx = 0;
    if (next_idx >= NUM_INTERVAL_STEPS) next_idx = NUM_INTERVAL_STEPS - 1;
    return g_interval_steps[next_idx];
}

static char wait_or_read_key(double wait_sec) {
    if (wait_sec < 0.0) wait_sec = 0.0;
    if (!g_tui_active) {
        if (wait_sec > 0.0) usleep((useconds_t)(wait_sec * 1e6));
        return 0;
    }
    struct timeval tv;
    tv.tv_sec = (long)wait_sec;
    tv.tv_usec = (long)((wait_sec - (double)tv.tv_sec) * 1e6);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);

    int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            return ch;
        }
    }
    return 0;
}

static void render_tui_dashboard(
    const char *cpu_model, double effective_cpus, int logical_cpus,
    const char *backend_desc, const char *scope_desc, int total_procs,
    double peak_bw_gbs, double max_observed_bw, double avg_bw, double interval,
    bool paused, view_mode_t view_mode, tracked_proc_t **sorted_procs, int num_procs,
    const sample_metrics_t *ring, int ring_count, int ring_head)
{
    const sample_metrics_t *cur = &ring[(ring_head - 1 + RING_SIZE) % RING_SIZE];
    char main_bar[160];
    char cpu_bar[160];
    double cpu_util_pct = (cur->cpu_pct / (effective_cpus * 100.0)) * 100.0;
    format_bar(cur->util_pct, 30, true, main_bar, sizeof(main_bar));
    format_bar(cpu_util_pct, 30, true, cpu_bar, sizeof(cpu_bar));

    const char *view_name = (view_mode == VIEW_SPLIT) ? "Split (Top Procs + Timeline)" :
                            (view_mode == VIEW_PROCS_ONLY) ? "Top Processes Only" : "Rolling Timeline Only";

    printf("\033[H");
    printf("\033[1;36m==========================================================================================================\033[K\033[0m\n");
    printf(" \033[1;37mdramtop v%s\033[0m  |  CPU: \033[1;33m%s\033[0m (Quota: %.1f cores / %d logical)\033[K\n",
           DRAMTOP_VERSION, cpu_model, effective_cpus, logical_cpus);
    printf(" PMU Backend   : %s\033[K\n", backend_desc);
    printf(" Monitor Scope : \033[1;32m%s\033[0m (%d procs)  |  Refresh: %.2fs%s  |  View: \033[1;35m%s\033[0m\033[K\n",
           scope_desc, total_procs, interval, paused ? " \033[1;41;37m [PAUSED] \033[0m" : "", view_name);
    printf("\033[1;36m----------------------------------------------------------------------------------------------------------\033[K\033[0m\n");
    printf(" \033[1mSystem DRAM BW\033[0m: %s \033[1;36m%6.2f GB/s\033[0m (%5.1f%% of %.1f GB/s Cap)\033[K\n",
           main_bar, cur->total_bw_gbs, cur->util_pct, peak_bw_gbs);
    printf("   ├─ Demand Rd: \033[1;37m%6.2f GB/s\033[0m   ├─ HW Prefetch: \033[1;37m%6.2f GB/s\033[0m   ├─ Session Peak/Avg: \033[1;33m%.2f\033[0m / \033[1;32m%.2f GB/s\033[0m\033[K\n",
           cur->demand_bw_gbs, cur->prefetch_bw_gbs, max_observed_bw, avg_bw);
    printf(" \033[1mCPU Quota Util\033[0m: %s \033[1;37m%6.0f%%\033[0m     (IPC: \033[1;37m%.2f\033[0m | Throttled: %ld)\033[K\n",
           cpu_bar, cur->cpu_pct, cur->ipc, cur->throttled_delta);
    printf(" \033[1mLive Diagnosis\033[0m: \033[1;%dm%s\033[0m  (Top Bandwidth Consumer: \033[1;33m%s\033[0m)\033[K\n",
           cur->status_color, cur->status_tag, cur->top_comm[0] ? cur->top_comm : "none");

    int term_rows = get_term_rows();
    int avail_rows = term_rows - 12;
    if (avail_rows < 8) avail_rows = 8;

    int proc_rows_to_show = 0;
    int hist_rows_to_show = 0;
    if (view_mode == VIEW_SPLIT) {
        proc_rows_to_show = (avail_rows - 4) / 2;
        if (proc_rows_to_show < 4) proc_rows_to_show = 4;
        if (proc_rows_to_show > 8) proc_rows_to_show = 8;
        hist_rows_to_show = avail_rows - proc_rows_to_show - 4;
        if (hist_rows_to_show < 4) hist_rows_to_show = 4;
        if (hist_rows_to_show > 10) hist_rows_to_show = 10;
    } else if (view_mode == VIEW_PROCS_ONLY) {
        proc_rows_to_show = avail_rows - 2;
        if (proc_rows_to_show > 20) proc_rows_to_show = 20;
    } else {
        hist_rows_to_show = avail_rows - 2;
        if (hist_rows_to_show > 20) hist_rows_to_show = 20;
    }

    if (proc_rows_to_show > 0) {
        printf("\033[1;36m================================== [ TOP PROCESSES BY DRAM BANDWIDTH ] ===================================\033[K\033[0m\n");
        printf(" \033[1;37m%-7s | %-15s | %-4s | %-11s | %-10s | %-11s | %-16s | %-6s | %-4s | %s\033[0m\033[K\n",
               "PID", "PROCESS", "THR", "DRAM TOTAL", "DEMAND RD", "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "BOTTLENECK");
        printf("----------------------------------------------------------------------------------------------------------\033[K\n");

        int n_show = (num_procs < proc_rows_to_show) ? num_procs : proc_rows_to_show;
        for (int i = 0; i < n_show; i++) {
            const tracked_proc_t *p = sorted_procs[i];
            char pbar[96];
            format_bar(p->util_pct, 8, true, pbar, sizeof(pbar));
            printf(" %-7d | %-15.15s | %4d | %6.2f GB/s | %5.2f GB/s | %6.2f GB/s | %s %5.1f%% | %5.0f%% | %4.2f | \033[1;%dm%s\033[0m\033[K\n",
                   (int)p->pid, p->comm, p->num_threads, p->total_bw_gbs, p->demand_bw_gbs, p->prefetch_bw_gbs,
                   pbar, p->util_pct, p->cpu_pct, p->ipc, p->status_color, p->status_tag);
        }
    }

    if (hist_rows_to_show > 0) {
        printf("\033[1;36m=================================== [ REAL-TIME ROLLING TIMELINE ] =======================================\033[K\033[0m\n");
        printf(" \033[1;37m%-8s | %-11s | %-10s | %-11s | %-16s | %-6s | %-4s | %-13s | %s\033[0m\033[K\n",
               "TIME", "DRAM TOTAL", "DEMAND RD", "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "TOP PROCESS", "DIAGNOSIS");
        printf("----------------------------------------------------------------------------------------------------------\033[K\n");

        int show_n = (ring_count < hist_rows_to_show) ? ring_count : hist_rows_to_show;
        for (int i = 0; i < show_n; i++) {
            int idx = (ring_head - show_n + i + RING_SIZE) % RING_SIZE;
            const sample_metrics_t *s = &ring[idx];
            char sbar[96];
            format_bar(s->util_pct, 8, true, sbar, sizeof(sbar));
            printf(" %-8s | %6.2f GB/s | %5.2f GB/s | %6.2f GB/s | %s %5.1f%% | %5.0f%% | %4.2f | %-13.13s | \033[1;%dm%s\033[0m\033[K\n",
                   s->time_str, s->total_bw_gbs, s->demand_bw_gbs, s->prefetch_bw_gbs,
                   sbar, s->util_pct, s->cpu_pct, s->ipc,
                   s->top_comm[0] ? s->top_comm : "-", s->status_color, s->status_tag);
        }
    }

    printf("\033[1;36m----------------------------------------------------------------------------------------------------------\033[K\033[0m\n");
    printf(" Keys: [\033[1;33mq\033[0m] Quit  [\033[1;33mt\033[0m] View  [\033[1;33mSpace\033[0m] Pause  [\033[1;33mr\033[0m] Reset  [\033[1;33m+\033[0m/\033[1;33m-\033[0m] Speed (%.2fs)  (-b batch / -J json)\033[K\033[J", interval);
    fflush(stdout);
}

static void print_json_sample(const sample_metrics_t *sys_m, double peak_bw_gbs,
                              tracked_proc_t **sorted_procs, int num_procs) {
    printf("{\"time\":\"%s\",\"timestamp\":%.3f,\"system\":{\"total_bw_gbs\":%.3f,"
           "\"demand_bw_gbs\":%.3f,\"prefetch_bw_gbs\":%.3f,\"peak_cap_gbs\":%.2f,"
           "\"util_pct\":%.2f,\"cpu_pct\":%.1f,\"ipc\":%.3f,\"throttled\":%ld,"
           "\"top_process\":\"%s\",\"top_pid\":%d,\"diagnosis\":\"%s\"},\"top_processes\":[",
           sys_m->time_str, sys_m->timestamp, sys_m->total_bw_gbs,
           sys_m->demand_bw_gbs, sys_m->prefetch_bw_gbs, peak_bw_gbs,
           sys_m->util_pct, sys_m->cpu_pct, sys_m->ipc, sys_m->throttled_delta,
           sys_m->top_comm, (int)sys_m->top_pid, sys_m->status_tag);

    int max_p = (num_procs < 5) ? num_procs : 5;
    for (int i = 0; i < max_p; i++) {
        const tracked_proc_t *p = sorted_procs[i];
        printf("%s{\"pid\":%d,\"comm\":\"%s\",\"threads\":%d,\"total_bw_gbs\":%.3f,"
               "\"demand_bw_gbs\":%.3f,\"prefetch_bw_gbs\":%.3f,\"util_pct\":%.2f,"
               "\"cpu_pct\":%.1f,\"ipc\":%.3f,\"diagnosis\":\"%s\"}",
               (i > 0) ? "," : "", (int)p->pid, p->comm, p->num_threads,
               p->total_bw_gbs, p->demand_bw_gbs, p->prefetch_bw_gbs,
               p->util_pct, p->cpu_pct, p->ipc, p->status_tag);
    }
    printf("]}\n");
    fflush(stdout);
}

static void print_usage(const char *prog) {
    printf("dramtop v%s - Real-time DRAM Memory Bandwidth & Bottleneck Monitor for Linux\n\n", DRAMTOP_VERSION);
    printf("Usage:\n");
    printf("  %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -p, --pid <PID>        Monitor only a specific process PID (default: all active processes)\n");
    printf("  -P, --proc <NAME>      Monitor only processes matching NAME (e.g., llama-server)\n");
    printf("  -i, --interval <SEC>   Refresh interval in seconds (default: 1.0)\n");
    printf("  -n, --count <N>        Exit after N samples (default: 0 = infinite)\n");
    printf("  -c, --ceiling <GB/s>   Override DRAM bandwidth ceiling in GB/s (default: auto-calibrate)\n");
    printf("  -b, --batch            Batch mode: scroll line-by-line instead of in-place TUI (for logs/pipes)\n");
    printf("  -J, --json             JSON Lines (NDJSON) streaming output for telemetry/Grafana/CI\n");
    printf("  -v, --version          Print version and exit\n");
    printf("  -h, --help             Show this help message\n");
}

int main(int argc, char **argv) {
    pid_t filter_pid = -1;
    const char *filter_name = NULL;
    double interval = 1.0;
    int max_count = 0;
    double manual_ceiling = 0.0;
    bool batch_mode = !isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO);
    bool json_mode = false;
    view_mode_t view_mode = VIEW_SPLIT;

    static struct option long_opts[] = {
        {"pid",       required_argument, 0, 'p'},
        {"proc",      required_argument, 0, 'P'},
        {"interval",  required_argument, 0, 'i'},
        {"count",     required_argument, 0, 'n'},
        {"ceiling",   required_argument, 0, 'c'},
        {"batch",     no_argument,       0, 'b'},
        {"json",      no_argument,       0, 'J'},
        {"version",   no_argument,       0, 'v'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:P:i:n:c:bJvh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': filter_pid = (pid_t)atoi(optarg); break;
            case 'P': filter_name = optarg; break;
            case 'i': interval = atof(optarg); if (interval < 0.1) interval = 0.1; break;
            case 'n': max_count = atoi(optarg); break;
            case 'c': manual_ceiling = atof(optarg); break;
            case 'b': batch_mode = true; break;
            case 'J': json_mode = true; batch_mode = true; break;
            case 'v': printf("dramtop v%s\n", DRAMTOP_VERSION); return 0;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGWINCH, handle_sigwinch);
    raise_fd_limit();

    char cpu_model[256];
    cpu_arch_t arch = detect_cpu_arch(cpu_model, sizeof(cpu_model));
    int logical_cpus = 1;
    double effective_cpus = detect_effective_cpus(&logical_cpus);

    pmu_event_def_t events[MAX_EVENTS];
    const char *backend_desc = "";
    int num_events = build_pmu_events(arch, events, &backend_desc);

    double peak_bw_gbs = (manual_ceiling > 1.0)
                             ? manual_ceiling
                             : auto_calibrate_peak_bandwidth(effective_cpus, events, num_events);

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    sys_cpu_pmu_t sys_pmu;
    memset(&sys_pmu, 0, sizeof(sys_pmu));
    bool use_sys_cpu_pmu = (filter_pid <= 0 && (!filter_name || !filter_name[0]));
    if (use_sys_cpu_pmu) {
        if (open_sys_cpu_pmu(&sys_pmu, logical_cpus, events, num_events)) {
            read_sys_cpu_pmu_totals(&sys_pmu, num_events, sys_pmu.prev_vals);
        }
    }

    char scope_desc[128] = "All System Processes (System-Wide)";
    if (filter_pid > 0) {
        snprintf(scope_desc, sizeof(scope_desc), "PID %d", (int)filter_pid);
    } else if (filter_name && filter_name[0]) {
        snprintf(scope_desc, sizeof(scope_desc), "Process Filter: '%s'", filter_name);
    } else if (sys_pmu.available) {
        snprintf(scope_desc, sizeof(scope_desc), "Per-CPU System PMU (pid=-1, %d CPUs, %s)",
                 sys_pmu.opened_cpus, sys_pmu.include_kernel ? "Kernel+User" : "User");
    } else {
        snprintf(scope_desc, sizeof(scope_desc), "All Processes (Thread-PMU Container Mode)");
    }

    tracked_proc_t *sorted_procs[MAX_PROCS + 1];
    tracked_proc_t kernel_ephem_proc;
    memset(&kernel_ephem_proc, 0, sizeof(kernel_ephem_proc));
    long prev_throttled = read_cgroup_throttled();
    double prev_ts = now_sec();
    int num_procs = sync_and_sample_processes(filter_pid, filter_name, events, num_events,
                                              1.0, clk_tck, effective_cpus, &peak_bw_gbs, sorted_procs);
    if (num_procs == 0 && !sys_pmu.available) {
        fprintf(stderr, "dramtop: No matching accessible processes found.\n");
        return 1;
    }

    bool use_color = isatty(STDOUT_FILENO) && (getenv("NO_COLOR") == NULL);
    if (!batch_mode) {
        enter_tui_mode();
    } else if (!json_mode) {
        printf("==========================================================================================================\n");
        printf("dramtop v%s | CPU: %s (Quota: %.1f cores / %d logical)\n",
               DRAMTOP_VERSION, cpu_model, effective_cpus, logical_cpus);
        printf("PMU Backend   : %s\n", backend_desc);
        printf("Monitor Scope : %s (%d active processes) | Auto-Calibrated DRAM Cap: %.1f GB/s\n",
               scope_desc, num_procs, peak_bw_gbs);
        printf("==========================================================================================================\n");
        printf("%-8s | %-11s | %-10s | %-11s | %-16s | %-6s | %-4s | %-13s | %s\n",
               "TIME", "DRAM TOTAL", "DEMAND RD", "HW PREFETCH", "DRAM UTIL%", "CPU%", "IPC", "TOP PROCESS", "DIAGNOSIS");
        printf("----------------------------------------------------------------------------------------------------------\n");
    }

    sample_metrics_t ring[RING_SIZE];
    memset(ring, 0, sizeof(ring));
    snprintf(ring[RING_SIZE - 1].status_tag, sizeof(ring[RING_SIZE - 1].status_tag), "SAMPLING...");
    ring[RING_SIZE - 1].status_color = 33;
    int ring_head = 0;
    int ring_count = 0;
    double max_observed_bw = 0.0;
    double sum_bw = 0.0;
    int stat_samples = 0;
    bool paused = false;

    if (g_tui_active) {
        render_tui_dashboard(cpu_model, effective_cpus, logical_cpus, backend_desc,
                             scope_desc, num_procs, peak_bw_gbs,
                             0.0, 0.0, interval, paused, view_mode,
                             sorted_procs, num_procs, ring, ring_count, ring_head);
    }

    int iter = 0;
    while (!g_stop) {
        while (!g_stop) {
            double elapsed = now_sec() - prev_ts;
            double rem = paused ? 0.25 : (interval - elapsed);
            if (!paused && rem <= 0.0) break;

            char key = wait_or_read_key(rem);
            if (g_stop || key == 'q' || key == 'Q') {
                g_stop = 1;
                break;
            }

            bool need_redraw = false;
            if (g_winch) {
                g_winch = 0;
                need_redraw = true;
            }
            if (key == 0 && !need_redraw) {
                if (!paused) break; /* Timeout reached, time to sample */
                continue;
            }

            if (key == 'r' || key == 'R') {
                max_observed_bw = 0.0;
                sum_bw = 0.0;
                stat_samples = 0;
                ring_count = 0;
                ring_head = 0;
                need_redraw = true;
            } else if (key == 't' || key == 'T' || key == '\t') {
                view_mode = (view_mode_t)((view_mode + 1) % 3);
                need_redraw = true;
            } else if (key == ' ' || key == 'p' || key == 'P') {
                paused = !paused;
                if (!paused) prev_ts = now_sec();
                need_redraw = true;
            } else if (key == '+' || key == '=') {
                interval = step_interval(interval, -1); /* Faster: smaller interval */
                need_redraw = true;
            } else if (key == '-' || key == '_') {
                interval = step_interval(interval, +1); /* Slower: larger interval */
                need_redraw = true;
            }

            if (need_redraw && g_tui_active) {
                double avg_bw = (stat_samples > 0) ? (sum_bw / (double)stat_samples) : 0.0;
                render_tui_dashboard(cpu_model, effective_cpus, logical_cpus, backend_desc,
                                     scope_desc, num_procs, peak_bw_gbs,
                                     max_observed_bw, avg_bw, interval, paused, view_mode,
                                     sorted_procs, num_procs, ring, ring_count, ring_head);
            }
        }
        if (g_stop) break;

        double cur_ts = now_sec();
        double dt = cur_ts - prev_ts;
        if (dt <= 0.0001) dt = 0.0001;
        prev_ts = cur_ts;

        num_procs = sync_and_sample_processes(filter_pid, filter_name, events, num_events,
                                              dt, clk_tck, effective_cpus, &peak_bw_gbs, sorted_procs);

        long cur_throttled = read_cgroup_throttled();
        long throttled_delta = (cur_throttled >= 0 && prev_throttled >= 0) ? (cur_throttled - prev_throttled) : 0;
        prev_throttled = cur_throttled;

        sample_metrics_t sys_m;
        memset(&sys_m, 0, sizeof(sys_m));
        sys_m.throttled_delta = throttled_delta;

        double weighted_ipc_sum = 0.0;
        double weight_sum = 0.0;
        for (int i = 0; i < num_procs; i++) {
            sys_m.demand_bw_gbs += sorted_procs[i]->demand_bw_gbs;
            sys_m.prefetch_bw_gbs += sorted_procs[i]->prefetch_bw_gbs;
            sys_m.total_bw_gbs += sorted_procs[i]->total_bw_gbs;
            sys_m.cpu_pct += sorted_procs[i]->cpu_pct;
            if (sorted_procs[i]->cpu_pct > 0.5) {
                weighted_ipc_sum += sorted_procs[i]->ipc * sorted_procs[i]->cpu_pct;
                weight_sum += sorted_procs[i]->cpu_pct;
            }
        }
        sys_m.ipc = (weight_sum > 0.0) ? (weighted_ipc_sum / weight_sum) :
                    (num_procs > 0 ? sorted_procs[0]->ipc : 0.0);

        /* If Tier-1 Per-CPU System-Wide PMU (pid=-1, cpu=0..N) is active, use exact hardware totals */
        if (sys_pmu.available) {
            uint64_t cur_sys_vals[MAX_EVENTS];
            read_sys_cpu_pmu_totals(&sys_pmu, num_events, cur_sys_vals);
            uint64_t sys_demand_lines = 0, sys_prefetch_lines = 0;
            uint64_t sys_cycles = 0, sys_insns = 0;
            for (int e = 0; e < num_events; e++) {
                uint64_t delta = (cur_sys_vals[e] >= sys_pmu.prev_vals[e]) ?
                                 (cur_sys_vals[e] - sys_pmu.prev_vals[e]) : 0;
                sys_pmu.prev_vals[e] = cur_sys_vals[e];
                if (events[e].is_demand_dram) sys_demand_lines += delta;
                if (events[e].is_prefetch_dram) sys_prefetch_lines += delta;
                if (events[e].is_cycles) sys_cycles += delta;
                if (events[e].is_instructions) sys_insns += delta;
            }
            double hw_demand_bw = (double)(sys_demand_lines * CACHE_LINE_BYTES) / dt / 1e9;
            double hw_prefetch_bw = (double)(sys_prefetch_lines * CACHE_LINE_BYTES) / dt / 1e9;
            double hw_total_bw = hw_demand_bw + hw_prefetch_bw;

            if (hw_total_bw > sys_m.total_bw_gbs + 0.05 && num_procs < MAX_PROCS) {
                memset(&kernel_ephem_proc, 0, sizeof(kernel_ephem_proc));
                kernel_ephem_proc.pid = 0;
                snprintf(kernel_ephem_proc.comm, sizeof(kernel_ephem_proc.comm), "[kernel/ephem]");
                kernel_ephem_proc.num_threads = sys_pmu.opened_cpus;
                kernel_ephem_proc.demand_bw_gbs = (hw_demand_bw > sys_m.demand_bw_gbs) ?
                                                  (hw_demand_bw - sys_m.demand_bw_gbs) : 0.0;
                kernel_ephem_proc.prefetch_bw_gbs = (hw_prefetch_bw > sys_m.prefetch_bw_gbs) ?
                                                    (hw_prefetch_bw - sys_m.prefetch_bw_gbs) : 0.0;
                kernel_ephem_proc.total_bw_gbs = hw_total_bw - sys_m.total_bw_gbs;
                kernel_ephem_proc.util_pct = (kernel_ephem_proc.total_bw_gbs / peak_bw_gbs) * 100.0;
                kernel_ephem_proc.ipc = (sys_cycles > 0) ? ((double)sys_insns / (double)sys_cycles) : 0.0;
                snprintf(kernel_ephem_proc.status_tag, sizeof(kernel_ephem_proc.status_tag), "KERNEL / EPHEMERAL");
                kernel_ephem_proc.status_color = 36;

                int ins = num_procs;
                while (ins > 0 && sorted_procs[ins - 1]->total_bw_gbs < kernel_ephem_proc.total_bw_gbs) {
                    sorted_procs[ins] = sorted_procs[ins - 1];
                    ins--;
                }
                sorted_procs[ins] = &kernel_ephem_proc;
                num_procs++;
            }

            if (hw_total_bw > sys_m.total_bw_gbs) {
                sys_m.demand_bw_gbs = hw_demand_bw;
                sys_m.prefetch_bw_gbs = hw_prefetch_bw;
                sys_m.total_bw_gbs = hw_total_bw;
            }
            if (sys_cycles > 0) {
                sys_m.ipc = (double)sys_insns / (double)sys_cycles;
            }
        }

        if (num_procs > 0 && (sorted_procs[0]->total_bw_gbs > 0.05 || sorted_procs[0]->cpu_pct > 1.0)) {
            snprintf(sys_m.top_comm, sizeof(sys_m.top_comm), "%s", sorted_procs[0]->comm);
            sys_m.top_pid = sorted_procs[0]->pid;
        } else {
            snprintf(sys_m.top_comm, sizeof(sys_m.top_comm), "idle");
            sys_m.top_pid = 0;
        }

        if (sys_m.total_bw_gbs > peak_bw_gbs) {
            peak_bw_gbs = sys_m.total_bw_gbs;
        }
        if (sys_m.total_bw_gbs > max_observed_bw) {
            max_observed_bw = sys_m.total_bw_gbs;
        }
        sum_bw += sys_m.total_bw_gbs;
        stat_samples++;
        double avg_bw = sum_bw / (double)stat_samples;

        sys_m.util_pct = (sys_m.total_bw_gbs / peak_bw_gbs) * 100.0;
        if (sys_m.util_pct > 100.0) sys_m.util_pct = 100.0;

        diagnose_bottleneck(sys_m.total_bw_gbs, sys_m.util_pct, sys_m.cpu_pct,
                            sys_m.ipc, sys_m.throttled_delta, effective_cpus,
                            sys_m.status_tag, sizeof(sys_m.status_tag), &sys_m.status_color);

        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        sys_m.timestamp = (double)tv_now.tv_sec + (double)tv_now.tv_usec * 1e-6;
        time_t now_t = tv_now.tv_sec;
        struct tm tm_now;
        localtime_r(&now_t, &tm_now);
        strftime(sys_m.time_str, sizeof(sys_m.time_str), "%H:%M:%S", &tm_now);

        ring[ring_head] = sys_m;
        ring_head = (ring_head + 1) % RING_SIZE;
        if (ring_count < RING_SIZE) ring_count++;

        if (g_tui_active) {
            render_tui_dashboard(cpu_model, effective_cpus, logical_cpus, backend_desc,
                                 scope_desc, num_procs, peak_bw_gbs,
                                 max_observed_bw, avg_bw, interval, paused, view_mode,
                                 sorted_procs, num_procs, ring, ring_count, ring_head);
        } else if (json_mode) {
            print_json_sample(&sys_m, peak_bw_gbs, sorted_procs, num_procs);
        } else {
            char bar[96];
            format_bar(sys_m.util_pct, 8, use_color, bar, sizeof(bar));
            if (use_color) {
                printf("%-8s | %6.2f GB/s | %5.2f GB/s | %6.2f GB/s | %s %5.1f%% | %5.0f%% | %4.2f | %-13.13s | \033[1;%dm%s\033[0m\n",
                       sys_m.time_str, sys_m.total_bw_gbs, sys_m.demand_bw_gbs, sys_m.prefetch_bw_gbs,
                       bar, sys_m.util_pct, sys_m.cpu_pct, sys_m.ipc,
                       sys_m.top_comm, sys_m.status_color, sys_m.status_tag);
            } else {
                printf("%-8s | %6.2f GB/s | %5.2f GB/s | %6.2f GB/s | %s %5.1f%% | %5.0f%% | %4.2f | %-13.13s | %s\n",
                       sys_m.time_str, sys_m.total_bw_gbs, sys_m.demand_bw_gbs, sys_m.prefetch_bw_gbs,
                       bar, sys_m.util_pct, sys_m.cpu_pct, sys_m.ipc,
                       sys_m.top_comm, sys_m.status_tag);
            }
            fflush(stdout);
        }

        iter++;
        if (max_count > 0 && iter >= max_count) break;
    }

    leave_tui_mode();
    close_sys_cpu_pmu(&sys_pmu, num_events);
    for (int i = 0; i < MAX_PROCS; i++) {
        close_tracked_proc(&g_procs[i], num_events);
    }
    return 0;
}
