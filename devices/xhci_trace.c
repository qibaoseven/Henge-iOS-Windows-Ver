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

void hv_xhci_trace_init(void) {
    if (g_trace) return;

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
    if (!g_trace) return;

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
