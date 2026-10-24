/*
 * dramtop - system layer for Windows: time, threads, console and Ctrl+C.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>

#include "dramtop.h"

static volatile LONG g_stop, g_resize;
static HANDLE g_wake; /* set by Ctrl+C, so waits end at once instead of at their timeout */
static LARGE_INTEGER g_qpc_hz;

static bool g_console_ready;
static UINT g_saved_cp;
static DWORD g_saved_out, g_saved_err, g_saved_in;
static bool g_has_out, g_has_err, g_raw;

double os_now(void) {
    LARGE_INTEGER t;
    if (!g_qpc_hz.QuadPart) QueryPerformanceFrequency(&g_qpc_hz);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpc_hz.QuadPart;
}

static DWORD to_ms(double sec) { return sec <= 0 ? 0 : sec >= 86400.0 ? 86400000u : (DWORD)(sec * 1000.0 + 0.5); }

void os_sleep(double sec) {
    if (sec <= 0) return;
    if (g_wake) WaitForSingleObject(g_wake, to_ms(sec));
    else Sleep(to_ms(sec));
}

void *os_alloc_pages(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

void os_free_pages(void *p, size_t size) {
    (void)size;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

struct os_thread {
    HANDLE h;
    void (*fn)(void *);
    void *arg;
};

static unsigned __stdcall thread_main(void *arg) {
    os_thread_t *t = arg;
    t->fn(t->arg);
    return 0;
}

os_thread_t *os_thread_start(void (*fn)(void *), void *arg) {
    os_thread_t *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_main, t, 0, NULL);
    if (!h) {
        free(t);
        return NULL;
    }
    t->h = (HANDLE)h;
    return t;
}

void os_thread_join(os_thread_t *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

struct os_monitor {
    SRWLOCK mu;
    CONDITION_VARIABLE cv;
};

os_monitor_t *os_monitor_new(void) {
    os_monitor_t *m = malloc(sizeof *m);
    if (!m) return NULL;
    InitializeSRWLock(&m->mu);
    InitializeConditionVariable(&m->cv);
    return m;
}

void os_monitor_free(os_monitor_t *m) { free(m); }
void os_monitor_lock(os_monitor_t *m) { AcquireSRWLockExclusive(&m->mu); }
void os_monitor_unlock(os_monitor_t *m) { ReleaseSRWLockExclusive(&m->mu); }
void os_monitor_wait(os_monitor_t *m) { SleepConditionVariableSRW(&m->cv, &m->mu, INFINITE, 0); }
void os_monitor_broadcast(os_monitor_t *m) { WakeAllConditionVariable(&m->cv); }

struct os_barrier {
    SYNCHRONIZATION_BARRIER b;
};

os_barrier_t *os_barrier_new(int parties) {
    os_barrier_t *b = malloc(sizeof *b);
    if (b && !InitializeSynchronizationBarrier(&b->b, parties, -1)) {
        free(b);
        return NULL;
    }
    return b;
}

void os_barrier_free(os_barrier_t *b) {
    if (!b) return;
    DeleteSynchronizationBarrier(&b->b);
    free(b);
}

void os_barrier_wait(os_barrier_t *b) { EnterSynchronizationBarrier(&b->b, 0); }

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static HANDLE std_handle(int fd) {
    return GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

static void console_restore(void) {
    if (g_raw) SetConsoleMode(std_handle(0), g_saved_in);
    if (g_has_out) SetConsoleMode(std_handle(1), g_saved_out);
    if (g_has_err) SetConsoleMode(std_handle(2), g_saved_err);
    if (g_saved_cp) SetConsoleOutputCP(g_saved_cp);
}

/* The output is UTF-8 with escape sequences, as on a POSIX terminal; the console's own
 * settings are put back on exit, because they outlive the process. */
static void console_init(void) {
    if (g_console_ready) return;
    g_console_ready = true;
    g_has_out = GetConsoleMode(std_handle(1), &g_saved_out) != 0;
    g_has_err = GetConsoleMode(std_handle(2), &g_saved_err) != 0;
    if (!g_has_out && !g_has_err) return;
    g_saved_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    if (g_has_out) {
        SetConsoleMode(std_handle(1), g_saved_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        setvbuf(stdout, NULL, _IOFBF, 1 << 16); /* the console CRT stream is unbuffered otherwise */
    }
    if (g_has_err) SetConsoleMode(std_handle(2), g_saved_err | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    atexit(console_restore);
}

bool os_isatty(int fd) {
    DWORD mode;
    return GetConsoleMode(std_handle(fd), &mode) != 0;
}

const char os_dumb_term[] = "TERM is \"dumb\" or the console cannot show escape sequences";

/* Consoles older than Windows 10 cannot do escape sequences. */
bool os_term_dumb(void) {
    const char *term = getenv("TERM");
    if (term && !strcmp(term, "dumb")) return true;
    console_init();
    DWORD mode;
    return g_has_out && (!GetConsoleMode(std_handle(1), &mode) || !(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING));
}

int os_term_rows(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(std_handle(1), &info)) return 24;
    int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return rows > 0 ? rows : 24;
}

bool os_term_raw(bool on) {
    HANDLE in = std_handle(0);
    if (!on) {
        if (!g_raw) return true;
        g_raw = false;
        return SetConsoleMode(in, g_saved_in) != 0;
    }
    console_init();
    if (!GetConsoleMode(in, &g_saved_in)) return false;
    /* Ctrl+C stays a signal; window size changes arrive as input records. */
    DWORD mode = (g_saved_in & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT)) |
                 ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(in, mode)) return false;
    FlushConsoleInputBuffer(in);
    g_raw = true;
    return true;
}

int os_read_key(double timeout) {
    HANDLE in = std_handle(0), wait[2] = {in, g_wake};
    double end = os_now() + timeout;
    for (;;) {
        DWORD r = WaitForMultipleObjects(g_wake ? 2 : 1, wait, FALSE, to_ms(end - os_now()));
        if (r == WAIT_TIMEOUT || r == WAIT_OBJECT_0 + 1) return 0;
        INPUT_RECORD rec;
        DWORD n = 0;
        if (r != WAIT_OBJECT_0 || !ReadConsoleInputW(in, &rec, 1, &n)) {
            InterlockedExchange(&g_stop, 1);
            return 0;
        }
        if (n == 1 && rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            InterlockedExchange(&g_resize, 1);
            return 0;
        }
        if (n == 1 && rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            WCHAR c = rec.Event.KeyEvent.uChar.UnicodeChar;
            if (c > 0 && c < 0x80) return c;
        }
        if (os_now() >= end) return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Ctrl+C and closing the console                                      */
/* ------------------------------------------------------------------ */

static BOOL WINAPI on_ctrl(DWORD type) {
    InterlockedExchange(&g_stop, 1);
    SetEvent(g_wake);
    /* Windows ends the process as soon as this returns; the main loop needs the time to stop
     * the system-wide trace session. */
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) Sleep(5000);
    return TRUE;
}

void os_signals_init(void) {
    g_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}

/* A failed write means the reader of a pipe went away, which SIGPIPE reports elsewhere. */
bool os_stop_requested(void) { return g_stop != 0 || ferror(stdout); }

bool os_take_resize(void) { return InterlockedExchange(&g_resize, 0) != 0; }

/* Windows consoles have no job control. */
void os_catch_suspend(bool on) { (void)on; }
bool os_take_suspend(void) { return false; }
void os_suspend(void) {}
