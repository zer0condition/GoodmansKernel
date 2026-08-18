/* goodmans_ioctl.h - shared UM/KM ioctl interface */
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#define GVM_DEVICE_NAME_U       L"\\Device\\Goodmans"
#define GVM_SYMLINK_NAME_U      L"\\DosDevices\\Goodmans"
#define GVM_USER_PATH           "\\\\.\\Goodmans"

#define GVM_DEVICE_TYPE         0x8000
#define GVM_IOCTL(fn)           CTL_CODE(GVM_DEVICE_TYPE, (fn), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_GVM_LOAD_MODULE   GVM_IOCTL(0x800)
#define IOCTL_GVM_CALL_EXPORT   GVM_IOCTL(0x801)
#define IOCTL_GVM_UNLOAD_MODULE GVM_IOCTL(0x802)
#define IOCTL_GVM_LIST_MODULES  GVM_IOCTL(0x803)
#define IOCTL_GVM_MODULE_INFO   GVM_IOCTL(0x804)
#define IOCTL_GVM_UNLOAD_ALL    GVM_IOCTL(0x805)
#define IOCTL_GVM_TAIL_LOG      GVM_IOCTL(0x806)
#define IOCTL_GVM_READ_GUEST    GVM_IOCTL(0x807)
#define IOCTL_GVM_TAIL_TRACE    GVM_IOCTL(0x808)
#define IOCTL_GVM_TRACE_CTL     GVM_IOCTL(0x809)
#define IOCTL_GVM_FORCE_UNLOAD  GVM_IOCTL(0x80a)
#define IOCTL_GVM_NOTIFY_STOP   GVM_IOCTL(0x80b)

#define GVM_MAX_MODULE_NAME 64
#define GVM_MAX_EXPORT_NAME 64
#define GVM_MAX_ARGS        8
#define GVM_MAX_MODULES     32

// capability bits, packed into gvm_module.caps
#define GVM_CAP_ALLOC       (1u << 0)  // host_alloc / host_free
#define GVM_CAP_READ_KMEM   (1u << 1)  // host_read_u8/u32/u64/bytes
#define GVM_CAP_WRITE_KMEM  (1u << 2)  // host_write_u64/bytes
#define GVM_CAP_MSR_READ    (1u << 3)  // host_readmsr
#define GVM_CAP_MSR_WRITE   (1u << 4)  // host_writemsr
#define GVM_CAP_PHYSMEM     (1u << 5)  // host_phys_read / host_phys_write
#define GVM_CAP_CPUID_TSC   (1u << 6)  // host_cpuid, host_rdtsc
#define GVM_CAP_CALLBACKS   (1u << 7)  // host_notify_enable / dispatch
#define GVM_CAP_HOSTCALL    (1u << 8)  // host_call (arbitrary export resolve)
#define GVM_CAP_INTROSPECT  (1u << 9)  // pid/tid/irql/current_process
#define GVM_CAP_ALL         0xFFFFFFFFu

typedef struct _gvm_load_in {
    unsigned int        wasm_size;
    unsigned int        stack_bytes;   // 0 = 64KB default
    unsigned long long  pool_budget;   // 0 = 4MB default
    char                name[GVM_MAX_MODULE_NAME];
} gvm_load_in;

typedef struct _gvm_load_out {
    unsigned int  module_id;
    int           status;
    char          err_msg[128];
} gvm_load_out;

typedef struct _gvm_call_in {
    unsigned int        module_id;
    unsigned int        argc;
    unsigned int        timeout_ms;   // 0 = no limit
    unsigned int        _pad;
    char                export_name[GVM_MAX_EXPORT_NAME];
    unsigned long long  argv[GVM_MAX_ARGS];
} gvm_call_in;

typedef struct _gvm_call_out {
    unsigned long long rv;
    int                status;
    char               err_msg[128];
} gvm_call_out;

typedef struct _gvm_unload_in {
    unsigned int  module_id;
} gvm_unload_in;

typedef struct _gvm_module_entry {
    unsigned int        id;
    unsigned int        wasm_size;
    unsigned long long  hash;
    unsigned int        exports;
    unsigned int        mem_pages;
    unsigned long long  pool_bytes;
    char                name[GVM_MAX_MODULE_NAME];
} gvm_module_entry;

typedef struct _gvm_list_out {
    unsigned int      count;
    unsigned int      pad;
    gvm_module_entry  entries[GVM_MAX_MODULES];
} gvm_list_out;

typedef struct _gvm_info_in {
    unsigned int  module_id;
} gvm_info_in;

#define GVM_MAX_INFO_EXPORTS 64
#define GVM_MAX_INFO_IMPORTS 64
#define GVM_INFO_NAME_LEN    48

typedef struct _gvm_info_out {
    gvm_module_entry  base;
    unsigned int      export_count;
    unsigned int      import_count;
    char              exports[GVM_MAX_INFO_EXPORTS][GVM_INFO_NAME_LEN];
    char              imports[GVM_MAX_INFO_IMPORTS][GVM_INFO_NAME_LEN];
    int               status;
    char              err_msg[128];
} gvm_info_out;

// TAIL_LOG: driver keeps a ring of recent DbgPrint lines. GUI polls this to
// show live driver output without needing DebugView/WinDbg.
#define GVM_LOG_ENTRY_LEN   200
#define GVM_LOG_ENTRIES     256

typedef struct _gvm_log_entry {
    unsigned long long timestamp_100ns;
    char               line[GVM_LOG_ENTRY_LEN];
} gvm_log_entry;

typedef struct _gvm_tail_in {
    unsigned long long last_seq;      // returns entries with seq > last_seq
} gvm_tail_in;

#define GVM_MAX_READ_GUEST_BYTES  (16 * 1024)

typedef struct _gvm_read_guest_in {
    unsigned int  module_id;
    unsigned int  offset;
    unsigned int  length;
} gvm_read_guest_in;

typedef struct _gvm_read_guest_out {
    int            status;
    unsigned int   length;
    unsigned char  data[GVM_MAX_READ_GUEST_BYTES];
    char           err_msg[128];
} gvm_read_guest_out;

typedef struct _gvm_tail_out {
    unsigned long long next_seq;      // pass this back as last_seq next time
    unsigned int       count;
    unsigned int       dropped;       // events dropped since last poll (ring wrap)
    gvm_log_entry      entries[GVM_LOG_ENTRIES];
} gvm_tail_out;

// trace ring: records every wasm->host import invocation when tracing is
// enabled, plus trap/error captures. lets developers watch their guest
// modules run in real time and see exactly what triggered a failure.
#define GVM_TRACE_ENTRIES     512
#define GVM_TRACE_NAME_LEN    48

// kind field values
#define GVM_TRK_IMPORT    1   // host import invoked
#define GVM_TRK_CALL      2   // export call started
#define GVM_TRK_RETURN    3   // export call returned
#define GVM_TRK_TRAP      4   // guest trapped (OOB, div0, unreachable, timeout)


typedef struct _gvm_trace_entry {
    unsigned long long timestamp_100ns;
    unsigned int       module_id;
    unsigned int       kind;
    unsigned int       thread_id;    // OS TID of the thread that made the call
    unsigned int       irql;         // KIRQL at the time
    unsigned int       argc;
    unsigned int       _pad;
    unsigned long long argv[4];      // up to 4 args captured
    unsigned long long rv;           // return value (or 0 for void)
    char               name[GVM_TRACE_NAME_LEN];
} gvm_trace_entry;

typedef struct _gvm_trace_in {
    unsigned long long last_seq;
} gvm_trace_in;

typedef struct _gvm_trace_out {
    unsigned long long next_seq;
    unsigned int       count;
    unsigned int       dropped;
    gvm_trace_entry    entries[GVM_TRACE_ENTRIES];
} gvm_trace_out;

// trace control
#define GVM_TRACE_OFF        0
#define GVM_TRACE_ON_ALL     1   // trace every module
#define GVM_TRACE_ON_MODULE  2   // trace only module_id

typedef struct _gvm_trace_ctl_in {
    unsigned int mode;
    unsigned int module_id;    // used when mode == GVM_TRACE_ON_MODULE
} gvm_trace_ctl_in;
