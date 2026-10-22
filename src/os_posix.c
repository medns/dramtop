/*
 * dramtop - system layer for POSIX systems (Linux and macOS): time, threads,
 * terminal and signals.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#include "dramtop.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop, g_winch, g_tstp;
static struct termios g_saved_tio;

double os_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void os_sleep(double sec) {
    if (sec <= 0) return;
    struct timespec ts = {(time_t)sec, (long)((sec - (double)(time_t)sec) * 1e9)};
    nanosleep(&ts, NULL);
}

void *os_alloc_aligned(size_t align, size_t size) {
    void *p = NULL;
    return posix_memalign(&p, align, size) ? NULL : p;
}

void os_free_aligned(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

struct os_thread {
    pthread_t id;
    void (*fn)(void *);
    void *arg;
};

static void *thread_main(void *arg) {
    os_thread_t *t = arg;
    t->fn(t->arg);
    return NULL;
}

os_thread_t *os_thread_start(void (*fn)(void *), void *arg) {
    os_thread_t *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->id, NULL, thread_main, t)) {
        free(t);
        return NULL;
    }
    return t;
}

void os_thread_join(os_thread_t *t) {
    pthread_join(t->id, NULL);
    free(t);
}

struct os_monitor {
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

os_monitor_t *os_monitor_new(void) {
    os_monitor_t *m = malloc(sizeof *m);
    if (!m) return NULL;
    if (pthread_mutex_init(&m->mu, NULL)) {
        free(m);
        return NULL;
    }
    if (pthread_cond_init(&m->cv, NULL)) {
        pthread_mutex_destroy(&m->mu);
        free(m);
        return NULL;
    }
    return m;
}

void os_monitor_free(os_monitor_t *m) {
    if (!m) return;
    pthread_cond_destroy(&m->cv);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

void os_monitor_lock(os_monitor_t *m) { pthread_mutex_lock(&m->mu); }
void os_monitor_unlock(os_monitor_t *m) { pthread_mutex_unlock(&m->mu); }
void os_monitor_wait(os_monitor_t *m) { pthread_cond_wait(&m->cv, &m->mu); }
void os_monitor_broadcast(os_monitor_t *m) { pthread_cond_broadcast(&m->cv); }

#if defined(_POSIX_BARRIERS) && _POSIX_BARRIERS > 0
struct os_barrier {
    pthread_barrier_t b;
};

os_barrier_t *os_barrier_new(int parties) {
    os_barrier_t *b = malloc(sizeof *b);
    if (b && pthread_barrier_init(&b->b, NULL, (unsigned)parties)) {
        free(b);
        return NULL;
    }
    return b;
}

void os_barrier_free(os_barrier_t *b) {
    if (!b) return;
    pthread_barrier_destroy(&b->b);
    free(b);
}

void os_barrier_wait(os_barrier_t *b) { pthread_barrier_wait(&b->b); }
#else
/* Fallback for systems without POSIX barriers (macOS). Woken threads have to
 * retake the mutex one by one, which a native barrier avoids. */
struct os_barrier {
    os_monitor_t *mon;
    int parties, arrived;
    unsigned long turn;
};

os_barrier_t *os_barrier_new(int parties) {
    os_barrier_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    if (!(b->mon = os_monitor_new())) {
        free(b);
        return NULL;
    }
    b->parties = parties;
    return b;
}

void os_barrier_free(os_barrier_t *b) {
    if (!b) return;
    os_monitor_free(b->mon);
    free(b);
}

void os_barrier_wait(os_barrier_t *b) {
    os_monitor_lock(b->mon);
    unsigned long turn = b->turn;
    if (++b->arrived == b->parties) {
        b->arrived = 0;
        b->turn++;
        os_monitor_broadcast(b->mon);
    } else {
        while (turn == b->turn) os_monitor_wait(b->mon);
    }
    os_monitor_unlock(b->mon);
}
#endif

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

bool os_isatty(int fd) { return isatty(fd) == 1; }

/* Same rule as git: without TERM there is no color and no cursor addressing. */
bool os_term_dumb(void) {
    const char *term = getenv("TERM");
    return !term || !*term || !strcmp(term, "dumb");
}

int os_term_rows(void) {
    struct winsize ws;
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 ? ws.ws_row : 24;
}

bool os_term_raw(bool on) {
    if (!on) return tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio) == 0;
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return false;
    struct termios t = g_saved_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0;
}

int os_read_key(double timeout) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&pfd, 1, (int)(timeout * 1000.0 + 0.5)) <= 0) return 0;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        g_stop = 1;
        return 0;
    }
    unsigned char ch;
    return read(STDIN_FILENO, &ch, 1) == 1 ? ch : 0;
}

/* ------------------------------------------------------------------ */
/* Signals                                                             */
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

void os_signals_init(void) {
    install_handler(SIGINT);
    install_handler(SIGTERM);
    install_handler(SIGHUP);
    install_handler(SIGWINCH);
}

bool os_stop_requested(void) { return g_stop != 0; }

bool os_take_resize(void) {
    if (!g_winch) return false;
    g_winch = 0;
    return true;
}

void os_catch_suspend(bool on) {
    if (on) install_handler(SIGTSTP);
    else signal(SIGTSTP, SIG_DFL);
}

bool os_take_suspend(void) {
    if (!g_tstp) return false;
    g_tstp = 0;
    return true;
}

void os_suspend(void) {
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    install_handler(SIGTSTP);
}
