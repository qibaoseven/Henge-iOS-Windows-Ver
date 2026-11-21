#include "xhci_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static FILE *g_trace;
static pthread_mutex_t g_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_trace_path[1024];
static struct timespec g_trace_start;
static int g_trace_started;

static void trace_timestamp(char *out, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!g_trace_started) {
        g_trace_start = ts;
        g_trace_started = 1;
    }
    long long ms = (long long)(ts.tv_sec - g_trace_start.tv_sec) * 1000LL
                 + (ts.tv_nsec - g_trace_start.tv_nsec) / 1000000LL;
    snprintf(out, n, "%lld.%03lld", ms / 1000, (ms % 1000 + 1000) % 1000);
}

static void trace_close_at_exit(void) {
    hv_xhci_trace_close();
}

/* Tracing is off unless HV_XHCI_TRACE is set (see hv_xhci_trace_init). When it
   is on it writes every MMIO read and every poll tick: ~290k lines in a few
   minutes, which is the level of detail we want when a guest misbehaves.
   HV_XHCI_TRACE_QUIET=1 drops only the highest-frequency lines (bar_read /
   poll # / run_tray) when the file would grow too fast for a long session. */
static int trace_full(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_XHCI_TRACE_QUIET") == NULL;
    return on;
}

static int trace_quiet_line(const char *fmt) {
    if (trace_full()) return 0;
    return strncmp(fmt, "bar_read", 8) == 0 ||
           strncmp(fmt, "poll #", 6) == 0 ||
           strncmp(fmt, "run_tray", 8) == 0;
}

void hv_xhci_trace_init(void) {
    if (g_trace) return;

    /* Off unless asked for. The USB bring-up that needed every MMIO read is
       over (the guest loads from the CD and the keyboard/trackpad work), and
       full tracing wrote ~290k lines in a few minutes into a file that sits
       next to the log we are actually reading. HV_XHCI_TRACE=1 turns it back
       on; HV_XHCI_TRACE_FILE still picks the path. */
    if (!getenv("HV_XHCI_TRACE")) return;

    const char *env = getenv("HV_XHCI_TRACE_FILE");
    if (env && *env) {
        snprintf(g_trace_path, sizeof g_trace_path, "%s", env);
    } else {
        const char *home = getenv("HOME");
        if (home && *home)
            snprintf(g_trace_path, sizeof g_trace_path, "%s/Documents/xhci-trace.log", home);
        else
            snprintf(g_trace_path, sizeof g_trace_path, "/tmp/xhci-trace.log");
    }

    g_trace = fopen(g_trace_path, "a");
    if (!g_trace && strcmp(g_trace_path, "/tmp/xhci-trace.log") != 0) {
        snprintf(g_trace_path, sizeof g_trace_path, "/tmp/xhci-trace.log");
        g_trace = fopen(g_trace_path, "a");
    }
    if (!g_trace) {
        fprintf(stderr, "xhci trace: cannot open any trace file; tracing disabled\n");
        return;
    }
    setvbuf(g_trace, NULL, _IOLBF, 0);
    atexit(trace_close_at_exit);

    char now[64];
    trace_timestamp(now, sizeof now);
    fprintf(g_trace, "\n===== xHCI trace session %s (monotonic %s) =====\n", g_trace_path, now);
    fflush(g_trace);
    fprintf(stderr, "xhci trace: %s\n", g_trace_path);
}

void hv_xhci_trace_close(void) {
    pthread_mutex_lock(&g_trace_mu);
    if (g_trace) {
        fflush(g_trace);
        fclose(g_trace);
        g_trace = NULL;
    }
    pthread_mutex_unlock(&g_trace_mu);
}

int hv_xhci_trace_on(void) {
    return g_trace != NULL;
}

const char *hv_xhci_trace_path(void) {
    return g_trace ? g_trace_path : "";
}

void hv_xhci_trace(const char *fmt, ...) {
    if (!g_trace || trace_quiet_line(fmt)) return;

    char now[32];
    trace_timestamp(now, sizeof now);

    pthread_mutex_lock(&g_trace_mu);
    if (!g_trace) {
        pthread_mutex_unlock(&g_trace_mu);
        return;
    }
    fprintf(g_trace, "[%s] ", now);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace);
    fflush(g_trace);
    pthread_mutex_unlock(&g_trace_mu);
}

void hv_xhci_trace_ports(const char *where, const uint32_t *portsc, const int *sent,
                         int count, uint32_t usbsts) {
    if (!g_trace) return;
    if (!trace_full() && strcmp(where, "poll") == 0) return;
    char now[32];
    trace_timestamp(now, sizeof now);
    pthread_mutex_lock(&g_trace_mu);
    if (!g_trace) {
        pthread_mutex_unlock(&g_trace_mu);
        return;
    }
    fprintf(g_trace, "[%s] ports(%s) usbsts=%08x EINT=%d", now, where, usbsts,
            (usbsts >> 3) & 1);
    for (int i = 0; i < count; i++)
        fprintf(g_trace, " p%d=%08x%s%s", i + 1, portsc[i],
                (portsc[i] & (1u << 17)) ? "/CSC" : "",
                sent ? (sent[i] ? "/sent" : "") : "");
    fputc('\n', g_trace);
    fflush(g_trace);
    pthread_mutex_unlock(&g_trace_mu);
}