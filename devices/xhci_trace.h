#ifndef HENGEVM_XHCI_TRACE_H
#define HENGEVM_XHCI_TRACE_H

#include <stdint.h>

/* Full xHCI trace, written to a file separate from henge.log / stderr.
 *
 * Path selection:
 *   1. HV_XHCI_TRACE_FILE, if set and non-empty
 *   2. $HOME/Documents/xhci-trace.log
 *   3. /tmp/xhci-trace.log
 *
 * The file is opened append-only. Each line is timestamped in milliseconds
 * since the first xHCI trace call and flushed immediately, so a guest halt or
 * a crash does not lose the tail. */
void hv_xhci_trace_init(void);
void hv_xhci_trace_close(void);
int hv_xhci_trace_on(void);
const char *hv_xhci_trace_path(void);

void hv_xhci_trace(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Compact one-line port snapshot: portsc + CSC + event-sent + EINT. */
void hv_xhci_trace_ports(const char *where, const uint32_t *portsc, const int *sent,
                         int count, uint32_t usbsts);

#endif
