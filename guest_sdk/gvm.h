/* gvm.h - Goodmans guest SDK */
#pragma once

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         i8;
typedef signed short        i16;
typedef signed int          i32;
typedef signed long long    i64;

typedef u32                 size_t;
typedef u8                  BOOLEAN;
typedef u8                  UCHAR;
typedef u16                 USHORT;
typedef u32                 ULONG;
typedef u64                 ULONGLONG;
typedef i32                 LONG;
typedef i64                 LONGLONG;
typedef u64                 SIZE_T;
typedef i32                 NTSTATUS;
typedef u64                 HANDLE;
typedef u64                 PVOID;

#define TRUE                1
#define FALSE               0
#define NULL                ((PVOID)0)

#define NonPagedPool        0
#define PagedPool           1
#define NonPagedPoolNx      512

#define PASSIVE_LEVEL       0
#define APC_LEVEL           1
#define DISPATCH_LEVEL      2

#define STATUS_SUCCESS                ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL           ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED        ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_PARAMETER      ((NTSTATUS)0xC000000DL)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define NT_SUCCESS(s)                 (((NTSTATUS)(s)) >= 0)

// import from the driver's env namespace: custom host_* helpers, plus any
// nt/hal export the driver auto-resolves via MmGetSystemRoutineAddress.
#define GVM_IMPORT(name)         __attribute__((import_module("env"), import_name(#name)))

// alias for readability when importing a native kernel export directly
#define GVM_IMPORT_KERNEL(name)  GVM_IMPORT(name)

// import from another loaded driver's export table. usage:
//   GVM_IMPORT_DRV("vgk", SomeExport) u64 SomeExport(u64 a);
#define GVM_IMPORT_DRV(mod, name) __attribute__((import_module("drv$" mod), import_name(#name)))

#define GVM_EXPORT(name)         __attribute__((export_name(#name)))

// buffer / memory / debug primitives (need wasm-linear-memory access)

GVM_IMPORT(host_dbg_print)       void gvm_dbg_print_raw(const char* msg, u32 len);
GVM_IMPORT(host_read_bytes)      u32  gvm_read_bytes(u64 kaddr, u32 guest_off, u32 len);
GVM_IMPORT(host_write_bytes)     u32  gvm_write_bytes(u64 kaddr, u32 guest_off, u32 len);
GVM_IMPORT(host_cpuid)           void gvm_cpuid(u32 leaf, u32 subleaf, u32 out_off);
GVM_IMPORT(host_phys_read)       u32  gvm_phys_read(u64 pa, u32 guest_off, u32 len);
GVM_IMPORT(host_phys_write)      u32  gvm_phys_write(u64 pa, u32 guest_off, u32 len);

// scalar primitives (thin one-line wrappers exposed via direct FFI)

GVM_IMPORT(host_current_irql)    u32  gvm_current_irql(void);
GVM_IMPORT(host_process_id)      u32  gvm_process_id(void);
GVM_IMPORT(host_thread_id)       u32  gvm_thread_id(void);
GVM_IMPORT(host_current_process) u64  gvm_current_process(void);
GVM_IMPORT(host_read_u8)         u32  gvm_read_u8(u64 kaddr);
GVM_IMPORT(host_read_u32)        u32  gvm_read_u32(u64 kaddr);
GVM_IMPORT(host_read_u64)        u64  gvm_read_u64(u64 kaddr);
GVM_IMPORT(host_write_u64)       void gvm_write_u64(u64 kaddr, u64 val);
GVM_IMPORT(host_readmsr)         u64  gvm_readmsr(u32 idx);
GVM_IMPORT(host_writemsr)        u32  gvm_writemsr(u32 idx, u64 val);
GVM_IMPORT(host_rdtsc)           u64  gvm_rdtsc(void);

// callback dispatch

GVM_IMPORT(host_notify_enable)   u32  gvm_notify_enable(u32 kind);
GVM_IMPORT(host_notify_poll)     u32  gvm_notify_poll(u32 out_off, u32 out_len);
GVM_IMPORT(host_dispatch_start)  u32  gvm_dispatch_start(void);
GVM_IMPORT(host_dispatch_stop)   u32  gvm_dispatch_stop(void);

// Native trampoline for InfinityHook. guest resolves the WMI_LOGGER_CONTEXT
// GetCpuClock slot itself (via kernel FFI) and writes this address into it.
// see sample_guest/infinity_hook.c for the full port.
GVM_IMPORT(host_ih_trampoline)   u64  gvm_ih_trampoline(void);
GVM_IMPORT(host_ih_configure)    void gvm_ih_configure(u32 rate, u64 nt_base, u32 nt_size);
GVM_IMPORT(host_ih_quiesce)      u64  gvm_ih_quiesce(void);

// wasm-linear-memory <-> real-VA bridge for FFI pointer args
//
// host_mem_base returns the kernel VA of the guest's linear memory base.
// build real pointers into your own buffers with gvm_kva(p):
//
//     char buf[64];
//     RtlZeroMemory(gvm_kva(buf), sizeof(buf));

GVM_IMPORT(host_mem_base) u64 host_mem_base(void);
GVM_IMPORT(host_mem_size) u32 host_mem_size(void);

static inline u64 gvm_kva(const void* p)
{
    return host_mem_base() + (u64)(u32)(unsigned long)p;
}

// build a real UNICODE_STRING in non-paged pool from a UTF-16 string in guest
// memory. returns kernel VA usable directly as PUNICODE_STRING. free after.

GVM_IMPORT(host_make_unistr) u64  host_make_unistr(u32 str_off, u32 byte_len);
GVM_IMPORT(host_free_unistr) void host_free_unistr(u64 kva);

// dynamic export dispatcher. use only when the target isn't known at compile
// time; direct GVM_IMPORT_KERNEL declarations are faster and traced by name.

GVM_IMPORT(host_call) u64 __gvm_host_call(
    const char* name, u32 name_len, u32 argc,
    u64 a0, u64 a1, u64 a2, u64 a3,
    u64 a4, u64 a5, u64 a6, u64 a7);

#define GVM_CALL0(name)                  __gvm_host_call(#name, sizeof(#name)-1, 0, 0,0,0,0,0,0,0,0)
#define GVM_CALL1(name, a)               __gvm_host_call(#name, sizeof(#name)-1, 1, (u64)(a),0,0,0,0,0,0,0)
#define GVM_CALL2(name, a,b)             __gvm_host_call(#name, sizeof(#name)-1, 2, (u64)(a),(u64)(b),0,0,0,0,0,0)
#define GVM_CALL3(name, a,b,c)           __gvm_host_call(#name, sizeof(#name)-1, 3, (u64)(a),(u64)(b),(u64)(c),0,0,0,0,0)
#define GVM_CALL4(name, a,b,c,d)         __gvm_host_call(#name, sizeof(#name)-1, 4, (u64)(a),(u64)(b),(u64)(c),(u64)(d),0,0,0,0)
#define GVM_CALL5(name, a,b,c,d,e)       __gvm_host_call(#name, sizeof(#name)-1, 5, (u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),0,0,0)
#define GVM_CALL6(name, a,b,c,d,e,f)     __gvm_host_call(#name, sizeof(#name)-1, 6, (u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),(u64)(f),0,0)
#define GVM_CALL7(name, a,b,c,d,e,f,g)   __gvm_host_call(#name, sizeof(#name)-1, 7, (u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),(u64)(f),(u64)(g),0)
#define GVM_CALL8(name, a,b,c,d,e,f,g,h) __gvm_host_call(#name, sizeof(#name)-1, 8, (u64)(a),(u64)(b),(u64)(c),(u64)(d),(u64)(e),(u64)(f),(u64)(g),(u64)(h))

// direct kernel imports. these go through the FFI trampoline: no name
// lookup at call time, args pass straight into the kernel via x64 ABI.

GVM_IMPORT_KERNEL(ExAllocatePool2)          PVOID    ExAllocatePool2(u64 flags, SIZE_T size, u32 tag);
GVM_IMPORT_KERNEL(ExAllocatePoolWithTag)    PVOID    ExAllocatePoolWithTag(u32 type, SIZE_T size, u32 tag);
GVM_IMPORT_KERNEL(ExFreePool)               void     ExFreePool(PVOID p);
GVM_IMPORT_KERNEL(ExFreePoolWithTag)        void     ExFreePoolWithTag(PVOID p, u32 tag);
GVM_IMPORT_KERNEL(PsGetCurrentProcessId)    HANDLE   PsGetCurrentProcessId(void);
GVM_IMPORT_KERNEL(PsGetCurrentThreadId)     HANDLE   PsGetCurrentThreadId(void);
GVM_IMPORT_KERNEL(IoGetCurrentProcess)      PVOID    IoGetCurrentProcess(void);
GVM_IMPORT_KERNEL(KeGetCurrentThread)       PVOID    KeGetCurrentThread(void);
GVM_IMPORT_KERNEL(KeGetCurrentIrql)         u8       KeGetCurrentIrql(void);
GVM_IMPORT_KERNEL(KeGetCurrentProcessorNumber) u32   KeGetCurrentProcessorNumber(void);
GVM_IMPORT_KERNEL(MmIsAddressValid)         BOOLEAN  MmIsAddressValid(PVOID p);
GVM_IMPORT_KERNEL(MmGetSystemRoutineAddress) PVOID   MmGetSystemRoutineAddress(PVOID us);
GVM_IMPORT_KERNEL(PsGetProcessId)           HANDLE   PsGetProcessId(PVOID eproc);
GVM_IMPORT_KERNEL(PsGetProcessImageFileName) PVOID   PsGetProcessImageFileName(PVOID eproc);
GVM_IMPORT_KERNEL(PsGetProcessInheritedFromUniqueProcessId) HANDLE PsGetProcessInheritedFromUniqueProcessId(PVOID eproc);
GVM_IMPORT_KERNEL(PsLookupProcessByProcessId) NTSTATUS PsLookupProcessByProcessId(HANDLE pid, PVOID* out_eproc);
GVM_IMPORT_KERNEL(ObDereferenceObject)      void     ObDereferenceObject(PVOID obj);
GVM_IMPORT_KERNEL(ZwClose)                  NTSTATUS ZwClose(HANDLE h);
GVM_IMPORT_KERNEL(ZwQuerySystemInformation) NTSTATUS ZwQuerySystemInformation(u32 cls, PVOID buf, u32 len, u32* got);
GVM_IMPORT_KERNEL(RtlZeroMemory)            void     RtlZeroMemory(PVOID dst, SIZE_T len);
GVM_IMPORT_KERNEL(RtlCopyMemory)            void     RtlCopyMemory(PVOID dst, PVOID src, SIZE_T len);
GVM_IMPORT_KERNEL(RtlCompareMemory)         SIZE_T   RtlCompareMemory(PVOID a, PVOID b, SIZE_T len);
GVM_IMPORT_KERNEL(KeStallExecutionProcessor) void    KeStallExecutionProcessor(u32 us);
GVM_IMPORT_KERNEL(KeQueryPerformanceCounter) u64     KeQueryPerformanceCounter(u64* freq_out);

// gvm_event layout must match driver's gvm_event struct in host_imports.c
typedef struct {
    u32  kind;        // 0=proc create, 1=proc exit, 2=image load
    u32  pid;
    u32  ppid;
    u32  flags;
    u64  eprocess;
    u64  image_base;
    u32  image_size;
    u32  _pad;
    char name[80];
} gvm_event;

// tiny string helpers

static inline u32 gvm_strlen(const char* s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static inline void gvm_print(const char* s)
{
    gvm_dbg_print_raw(s, gvm_strlen(s));
}

static inline void gvm_hex64(u64 v, char* out17)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) { out17[i] = h[v & 0xF]; v >>= 4; }
    out17[16] = 0;
}

static inline void gvm_dec(u32 v, char* out)
{
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char buf[16]; int n = 0;
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = buf[n - 1 - i];
    out[n] = 0;
}

static inline int gvm_streq(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline int gvm_stricmp(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

static inline u32 gvm_strcpy(char* dst, const char* src)
{
    u32 n = 0;
    while ((dst[n] = src[n]) != 0) n++;
    return n;
}

// capability manifest. embed in the guest to lock down which host imports
// the driver will let you call. see GVM_CAP_* below and shared/goodmans_ioctl.h.
// example: GVM_MANIFEST(GVM_CAP_ALLOC | GVM_CAP_READ_KMEM | GVM_CAP_HOSTCALL);

#define GVM_CAP_ALLOC        (1u << 0)
#define GVM_CAP_READ_KMEM    (1u << 1)
#define GVM_CAP_WRITE_KMEM   (1u << 2)
#define GVM_CAP_MSR_READ     (1u << 3)
#define GVM_CAP_MSR_WRITE    (1u << 4)
#define GVM_CAP_PHYSMEM      (1u << 5)
#define GVM_CAP_CPUID_TSC    (1u << 6)
#define GVM_CAP_CALLBACKS    (1u << 7)
#define GVM_CAP_HOSTCALL     (1u << 8)
#define GVM_CAP_INTROSPECT   (1u << 9)
#define GVM_CAP_ALL          0xFFFFFFFFu

#define GVM_MANIFEST(caps)                          \
    GVM_EXPORT(__gvm_caps)                          \
    u32 __gvm_caps(void) { return (u32)(caps); }
