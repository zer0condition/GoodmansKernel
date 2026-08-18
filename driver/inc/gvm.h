/* gvm.h - internal driver-wide declarations */
#pragma once

#include <ntddk.h>
#include "kshim/kshim.h"
#include "wasm3/wasm3.h"

#define GVM_TAG_MOD  'doMG'
#define GVM_TAG_WBUF 'BWMG'

#define GVM_STACK_DEFAULT (64u * 1024u)

typedef struct _gvm_module {
    unsigned int   id;
    BOOLEAN        used;
    IM3Environment env;
    IM3Runtime     runtime;
    IM3Module      module;
    unsigned char* wasm_bytes;
    unsigned int   wasm_size;
    unsigned long long hash;
    volatile LONG  refcount;         // touch only with Interlocked*
    KMUTEX         call_mutex;       // held around all m3_* activity on this module
    // pool budget for host_alloc/host_free
    volatile LONG64 pool_used;
    LONG64         pool_budget;
    // capability bitmask parsed from the guest's __gvm_caps export.
    // absent export defaults to GVM_CAP_ALL for backward compat.
    unsigned int   caps;
    // cooperative abort deadline in QPC ticks. 0 = no limit
    volatile ULONG64 exec_deadline_qpc;
    // QPC tick at which call_mutex was acquired. watchdog polls this to
    // find guests stuck past GVM_WATCHDOG_MAX_MS. 0 = not held
    volatile ULONG64 mutex_hold_qpc;
    // watchdog sets this to reject further calls into a wedged guest.
    // subsequent force-unload skips waiting on call_mutex
    volatile LONG    poisoned;
    char           name[64];
} gvm_module;

// ioctl_handler.c
unsigned int gvm_read_module_caps(gvm_module* mod);
BOOLEAN      gvm_deadline_exceeded(gvm_module* mod);

// wasm_call.c
M3Result gvm_call_locked(gvm_module* mod, IM3Function fn, unsigned int argc, const void** argp);
void     gvm_set_deadline_ms(gvm_module* mod, unsigned int timeout_ms);

NTSTATUS    gvm_modtab_init(void);
void        gvm_modtab_teardown(void);
gvm_module* gvm_modtab_alloc(void);
gvm_module* gvm_modtab_get(unsigned int id);
gvm_module* gvm_modtab_find_by_hash(unsigned long long hash);
gvm_module* gvm_modtab_find_by_hash_incref(unsigned long long hash, unsigned int expected_size);
gvm_module* gvm_modtab_iter(unsigned int idx);   // returns null past end
gvm_module* gvm_modtab_owner_of_runtime(IM3Runtime rt);
void        gvm_modtab_free(gvm_module* m);

M3Result    gvm_link_host_imports(IM3Module module);
M3Result    gvm_link_kernel_fallback(IM3Module module);
void        gvm_notify_init(void);
void        gvm_notify_teardown(void);

// callback dispatch (host_imports.c)
void        gvm_dispatch_start(void);
void        gvm_dispatch_stop_signal(void);   // just sets shutdown, returns
void        gvm_dispatch_stop_wait(void);     // signal + wait for worker exit (unload only)

NTSTATUS    gvm_ioctl_load(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_call(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_unload(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_list(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_info(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_unload_all(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_tail_log(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_read_guest(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_tail_trace(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_trace_ctl(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_force_unload(PIRP irp, PIO_STACK_LOCATION sp);
NTSTATUS    gvm_ioctl_notify_stop(PIRP irp, PIO_STACK_LOCATION sp);

// watchdog thread (watchdog.c)
void        gvm_watchdog_start(void);
void        gvm_watchdog_stop(void);

// infinity-hook (ih.c) - only the trampoline. install/uninstall happens in
// wasm via kernel FFI + gvm_write_u64.
unsigned long long gvm_ih_trampoline_addr(void);
void               gvm_ih_configure(unsigned int rate, unsigned long long nt_base, unsigned int nt_size);
void               gvm_ih_wait_quiescent(void);
unsigned long long gvm_ih_hit_count(void);
void               gvm_ih_teardown(void);

// trace ring (trace_ring.c)
void        gvm_trace_init(void);
void        gvm_trace_push(unsigned int module_id, unsigned int kind, const char* name,
                           unsigned int argc, const unsigned long long* argv, unsigned long long rv);
BOOLEAN     gvm_trace_enabled_for(unsigned int module_id);

// log ring (log_ring.c)
void        gvm_log_init(void);
void        gvm_log_push(const char* fmt, ...);
#define     gvm_log(fmt, ...) gvm_log_push(fmt, ##__VA_ARGS__)
