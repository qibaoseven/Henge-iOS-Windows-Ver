/* Non-Apple hosts (Linux/Android): what core/ takes from <mach/mach.h>.
 *
 * All of it is --stats diagnostics: whether a vCPU's host thread is running at
 * the sampling instant, and dropping the thread port it kept. None of it
 * affects what the guest sees, so a port is an opaque token here and
 * thread_info always says "unknown".
 *
 * Only for building the engine off Apple platforms; the Apple build uses the
 * real header. */
#ifndef HENGE_PORT_MACH_MACH_H
#define HENGE_PORT_MACH_MACH_H

#include <stdint.h>

#include "mach/vm_remap.h"

typedef uint32_t integer_t;
typedef int kern_return_t;
typedef unsigned int mach_port_t;
typedef unsigned int mach_msg_type_number_t;
typedef integer_t *thread_info_t;
typedef unsigned char boolean_t;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define KERN_SUCCESS 0
#define MACH_PORT_NULL 0
#define THREAD_BASIC_INFO 3
#define TH_STATE_RUNNING 1

/* mach/mach_time.h and mach/message.h define this; the stats code names the type. */
typedef struct {
    integer_t seconds;
    integer_t microseconds;
} time_value_t;

typedef struct {
    integer_t cpu_usage; /* scaled by TH_USAGE_SCALE */
    integer_t policy;
    integer_t run_state;
    integer_t flags;
    integer_t suspend_count;
    integer_t sleep_time;
    time_value_t user_time;   /* total user run time */
    time_value_t system_time; /* total system run time */
} thread_basic_info_data_t;
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(integer_t)))

static inline mach_port_t mach_task_self(void) { return (mach_port_t)-1; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)1; }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) {
    (void)task;
    (void)name;
    return KERN_SUCCESS;
}
static inline kern_return_t thread_info(mach_port_t thread, int flavor, thread_info_t out, mach_msg_type_number_t *count) {
    (void)thread;
    (void)flavor;
    (void)out;
    (void)count;
    return -1; /* unknown: the caller reports the CPU as not known to be running */
}

#endif
