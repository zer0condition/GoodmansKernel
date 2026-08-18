/* host_imports.c - kernel APIs exposed to wasm guests */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"
#include "wasm3/wasm3.h"
#include "wasm3/m3_env.h"
#include <intrin.h>
#include <ntimage.h>

// checks the caller module's capability bitmask against a required cap.
// returns TRUE if allowed OR if we can't identify the caller (fail-open only
// happens when we can't find the module for the runtime, which shouldn't
// occur in normal use).
static __forceinline BOOLEAN gvm_cap_ok(IM3Runtime rt, unsigned int need)
{
    gvm_module* owner = gvm_modtab_owner_of_runtime(rt);
    if (!owner) return TRUE;
    return (owner->caps & need) == need;
}

// non-canonical / unmapped kernel VA short-circuits so no #GP escapes SEH
static __forceinline BOOLEAN gvm_addr_ok(uint64_t addr, size_t len)
{
    if (addr == 0) return FALSE;
    uint64_t hi = addr >> 48;
    if (hi != 0x0000 && hi != 0xFFFF) return FALSE;
    if ((addr + len) < addr) return FALSE;
    if (!MmIsAddressValid((void*)(uintptr_t)addr)) return FALSE;
    if (len > 1 && !MmIsAddressValid((void*)(uintptr_t)(addr + len - 1))) return FALSE;
    return TRUE;
}

#define GVM_EVT_RING 512

typedef struct {
    uint32_t kind;         // 0 = process create, 1 = process exit, 2 = image load
    uint32_t pid;
    uint32_t ppid;
    uint32_t flags;
    uint64_t eprocess;
    uint64_t image_base;   // image_load only
    uint32_t image_size;   // image_load only
    uint32_t _pad;
    char     name[80];     // image name (basename only)
} gvm_event;

static gvm_event g_evt_ring[GVM_EVT_RING];
static ULONG g_evt_head = 0;
static ULONG g_evt_tail = 0;
static KSPIN_LOCK g_evt_lock;
static BOOLEAN g_evt_lock_init = FALSE;
static BOOLEAN g_process_notify_on = FALSE;
static BOOLEAN g_image_notify_on = FALSE;

static KEVENT g_evt_wake;
static KEVENT g_evt_shutdown;
static PETHREAD g_dispatch_thread = NULL;
static BOOLEAN g_dispatch_on = FALSE;

static void gvm_push_event(const gvm_event* e)
{
    KIRQL irql;
    if (!g_evt_lock_init) return;
    KeAcquireSpinLock(&g_evt_lock, &irql);
    ULONG next = (g_evt_head + 1) % GVM_EVT_RING;
    if (next != g_evt_tail) {
        g_evt_ring[g_evt_head] = *e;
        g_evt_head = next;
        if (g_dispatch_on)
            KeSetEvent(&g_evt_wake, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&g_evt_lock, irql);
}

// used by ih.c to push a SYSCALL event (kind=3) from the trampoline.
// re-uses the existing event ring so we get async dispatch to guest exports
// through the same worker that handles process/image events.
void gvm_push_event_generic(unsigned int kind, uint32_t pid,
                             uint64_t a, uint64_t b, uint32_t c)
{
    gvm_event e = { 0 };
    e.kind       = kind;
    e.pid        = pid;
    e.image_base = a;
    e.eprocess   = b;
    e.image_size = c;
    gvm_push_event(&e);
}

static BOOLEAN gvm_pop_event(gvm_event* out)
{
    KIRQL irql;
    BOOLEAN got = FALSE;
    if (!g_evt_lock_init) return FALSE;
    KeAcquireSpinLock(&g_evt_lock, &irql);
    if (g_evt_head != g_evt_tail) {
        *out = g_evt_ring[g_evt_tail];
        g_evt_tail = (g_evt_tail + 1) % GVM_EVT_RING;
        got = TRUE;
    }
    KeReleaseSpinLock(&g_evt_lock, irql);
    return got;
}

static void copy_wide_to_basename_ascii(char* dst, size_t dst_sz, PCUNICODE_STRING us)
{
    if (!us || !us->Buffer || dst_sz == 0) { if (dst_sz) dst[0] = 0; return; }
    USHORT len = us->Length / sizeof(WCHAR);
    // find last backslash
    USHORT start = 0;
    for (USHORT i = 0; i < len; i++)
        if (us->Buffer[i] == L'\\') start = i + 1;
    USHORT n = len - start;
    if (n > dst_sz - 1) n = (USHORT)(dst_sz - 1);
    for (USHORT i = 0; i < n; i++) dst[i] = (char)us->Buffer[start + i];
    dst[n] = 0;
}

static VOID gvm_process_notify_ex(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO Info)
{
    gvm_event e = { 0 };
    e.pid = (uint32_t)(uintptr_t)ProcessId;
    e.eprocess = (uint64_t)(uintptr_t)Process;

    if (Info) {
        e.kind = 0;
        e.ppid = (uint32_t)(uintptr_t)Info->ParentProcessId;
        copy_wide_to_basename_ascii(e.name, sizeof(e.name), Info->ImageFileName);
        gvm_log("PROC_CREATE pid=%u ppid=%u %s", e.pid, e.ppid, e.name);
    } else {
        e.kind = 1;
        gvm_log("PROC_EXIT pid=%u", e.pid);
    }
    gvm_push_event(&e);
}

static VOID gvm_image_notify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_ HANDLE ProcessId,
    _In_ PIMAGE_INFO ImageInfo)
{
    gvm_event e = { 0 };
    e.kind = 2;
    e.pid = (uint32_t)(uintptr_t)ProcessId;
    e.image_base = (uint64_t)(uintptr_t)ImageInfo->ImageBase;
    e.image_size = (uint32_t)ImageInfo->ImageSize;
    // IMAGE_INFO's first ULONG is a packed bitfield: SystemModeImage, ImageSignatureLevel, etc
    e.flags = *(ULONG*)ImageInfo;
    copy_wide_to_basename_ascii(e.name, sizeof(e.name), FullImageName);
    gvm_log("IMAGE_LOAD pid=%u base=%p size=%u %s",
            e.pid, (PVOID)(uintptr_t)e.image_base, e.image_size, e.name);
    gvm_push_event(&e);
}

void gvm_notify_init(void)
{
    if (!g_evt_lock_init) {
        KeInitializeSpinLock(&g_evt_lock);
        KeInitializeEvent(&g_evt_wake,     SynchronizationEvent, FALSE);
        KeInitializeEvent(&g_evt_shutdown, NotificationEvent,    FALSE);
        g_evt_lock_init = TRUE;
    }
}

// dispatch worker: on each wake, drain the ring and for every event look up
// on_process_create / on_process_exit / on_image_load in every loaded module.
// holds the module's call_mutex across FindFunction+Call so a concurrent
// unload can't free the runtime out from under us.
static const char* g_export_names[4] = {
    "on_process_create",
    "on_process_exit",
    "on_image_load",
    "on_syscall",
};

typedef struct {
    IM3Function fn;
    unsigned int argc;
    const void** argp;
    M3Result r;
} gvm_disp_ctx;

static VOID gvm_disp_callout(_In_ PVOID p)
{
    gvm_disp_ctx* c = (gvm_disp_ctx*)p;
    c->r = m3_Call(c->fn, c->argc, c->argp);
}

static void gvm_dispatch_worker(_In_ PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    PVOID waits[2] = { &g_evt_wake, &g_evt_shutdown };

    for (;;) {
        NTSTATUS s = KeWaitForMultipleObjects(2, waits, WaitAny,
            Executive, KernelMode, FALSE, NULL, NULL);
        if (s == STATUS_WAIT_1) break;

        gvm_event e;
        while (gvm_pop_event(&e)) {
            if (e.kind >= 4) continue;
            const char* export_name = g_export_names[e.kind];

            for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
                gvm_module* m = gvm_modtab_iter(i);
                if (!m) continue;

                KeWaitForSingleObject(&m->call_mutex, Executive, KernelMode, FALSE, NULL);

                if (m->used && m->runtime) {
                    IM3Function fn = NULL;
                    M3Result fr = m3_FindFunction(&fn, m->runtime, export_name);
                    if (!fr && fn) {
                        uint64_t a0 = e.pid;
                        uint64_t a1 = (e.kind == 2 || e.kind == 3) ? e.image_base : e.ppid;
                        uint64_t a2 = (e.kind == 2 || e.kind == 3) ? (uint64_t)e.image_size : 0;
                        const void* argp[3] = { &a0, &a1, &a2 };
                        gvm_disp_ctx dc = { fn, 3, argp, m3Err_none };
                        KeExpandKernelStackAndCalloutEx(gvm_disp_callout, &dc,
                            64 * 1024, FALSE, NULL);
                    }
                }

                KeReleaseMutex(&m->call_mutex, FALSE);
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

extern PDEVICE_OBJECT g_device;
static volatile LONG g_dispatch_pending = 0;

static void gvm_spawn_worker_now(void)
{
    if (g_dispatch_on) return;
    HANDLE h = NULL;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                      gvm_dispatch_worker, NULL);
    if (!NT_SUCCESS(s)) { gvm_log("dispatch: PsCreateSystemThread=0x%x", s); return; }

    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                              (PVOID*)&g_dispatch_thread, NULL);
    ZwClose(h);
    g_dispatch_on = TRUE;
    gvm_log("dispatch: worker started");
}

static IO_WORKITEM_ROUTINE_EX gvm_dispatch_spawner;
static VOID gvm_dispatch_spawner(_In_ PVOID io_object,
                                 _In_opt_ PVOID context,
                                 _In_ PIO_WORKITEM wi)
{
    UNREFERENCED_PARAMETER(io_object);
    UNREFERENCED_PARAMETER(context);
    gvm_spawn_worker_now();
    IoFreeWorkItem(wi);
    InterlockedExchange(&g_dispatch_pending, 0);
}

// safe from any PASSIVE_LEVEL context, including from inside a wasm expand-
// stack callout. queues thread creation onto a system worker thread rather
// than spawning it inline (spawning inside the callout was causing IRQL
// bugchecks in the newly-created thread's early stack setup).
void gvm_dispatch_start(void)
{
    if (g_dispatch_on) return;
    if (InterlockedCompareExchange(&g_dispatch_pending, 1, 0) != 0) return;
    gvm_notify_init();
    if (!g_device) { InterlockedExchange(&g_dispatch_pending, 0); return; }

    PIO_WORKITEM wi = IoAllocateWorkItem(g_device);
    if (!wi) { InterlockedExchange(&g_dispatch_pending, 0); return; }
    IoQueueWorkItemEx(wi, gvm_dispatch_spawner, DelayedWorkQueue, NULL);
}

// signal-only: safe to call from any thread that may hold a module mutex,
// because we don't wait for the worker to complete its current callback.
void gvm_dispatch_stop_signal(void)
{
    if (g_dispatch_on) KeSetEvent(&g_evt_shutdown, IO_NO_INCREMENT, FALSE);
}

// signal + block until worker exits. only safe when caller does NOT hold any
// module mutex (typical case: driver unload).
void gvm_dispatch_stop_wait(void)
{
    if (!g_dispatch_on) return;
    KeSetEvent(&g_evt_shutdown, IO_NO_INCREMENT, FALSE);
    if (g_dispatch_thread) {
        KeWaitForSingleObject(g_dispatch_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_dispatch_thread);
        g_dispatch_thread = NULL;
    }
    g_dispatch_on = FALSE;
    KeClearEvent(&g_evt_shutdown);
}

void gvm_notify_teardown(void)
{
    gvm_dispatch_stop_wait();

    if (g_process_notify_on) {
        PsSetCreateProcessNotifyRoutineEx(gvm_process_notify_ex, TRUE);
        g_process_notify_on = FALSE;
    }
    if (g_image_notify_on) {
        PsRemoveLoadImageNotifyRoutine(gvm_image_notify);
        g_image_notify_on = FALSE;
    }
}

NTSTATUS
gvm_ioctl_notify_stop(PIRP irp, PIO_STACK_LOCATION sp)
{
    UNREFERENCED_PARAMETER(sp);
    gvm_notify_teardown();
    gvm_log("notify: all callbacks removed, dispatch worker stopped");
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

// host_mem_base() -> i64  (kernel VA of the guest's wasm linear memory base)
// wasm3 memory is one contiguous non-paged buffer, so base + wasm_off gives
// the real kernel VA of any byte in the guest's linear memory. exposed so
// guests can construct pointer args for kernel APIs called via generic FFI.
static const void*
host_mem_base(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(rt); UNREFERENCED_PARAMETER(ctx);
    *sp = (uint64_t)(uintptr_t)mem;
    return m3Err_none;
}

// host_mem_size() -> i32  (linear memory byte count, tracks memory.grow)
static const void*
host_mem_size(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t sz = 0;
    m3_GetMemory(rt, &sz, 0);
    *(uint32_t*)sp = sz;
    return m3Err_none;
}

// host_make_unistr(wasm_str_off, byte_len) -> i64
// builds a UNICODE_STRING in non-paged pool from a UTF-16LE string in guest
// memory. returns kernel VA usable directly as PUNICODE_STRING for kernel
// APIs. caller must free with host_free_unistr when done.
static const void*
host_make_unistr(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t off  = (uint32_t)*(sp + 1);
    uint32_t blen = (uint32_t)*(sp + 2);
    *sp = 0;

    if (!gvm_cap_ok(rt, GVM_CAP_ALLOC)) return m3Err_none;
    uint32_t mem_sz = 0; m3_GetMemory(rt, &mem_sz, 0);
    if (blen == 0 || blen > 0x1000 || (uint64_t)off + blen > (uint64_t)mem_sz)
        return m3Err_none;

    UNICODE_STRING* us = (UNICODE_STRING*)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(UNICODE_STRING) + blen, GVM_TAG_WBUF);
    if (!us) return m3Err_none;
    WCHAR* buf = (WCHAR*)((unsigned char*)us + sizeof(UNICODE_STRING));
    RtlCopyMemory(buf, (unsigned char*)mem + off, blen);
    us->Length        = (USHORT)blen;
    us->MaximumLength = (USHORT)blen;
    us->Buffer        = buf;
    *sp = (uint64_t)(uintptr_t)us;
    return m3Err_none;
}

// host_free_unistr(kva) -> void
static const void*
host_free_unistr(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(rt); UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint64_t va = *(sp + 0);
    if (va) ExFreePoolWithTag((PVOID)(uintptr_t)va, GVM_TAG_WBUF);
    return m3Err_none;
}

static const void*
host_dbg_print(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);

    uint32_t off = (uint32_t)*(sp + 0);
    uint32_t len = (uint32_t)*(sp + 1);

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)off + len > (uint64_t)mem_sz || len == 0)
        return m3Err_none;

    const char* s = (const char*)mem + off;
    char tmp[512];
    ULONG n = (len < sizeof(tmp) - 1) ? (ULONG)len : (ULONG)(sizeof(tmp) - 1);
    RtlCopyMemory(tmp, s, n);
    tmp[n] = 0;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans/guest] %s\n", tmp);
    return m3Err_none;
}

// pool budget accounting: prepend an 8-byte size prefix so host_free can
// refund the charge. guest sees the pointer AFTER the prefix.
typedef struct {
    uint64_t size;
    unsigned char data[1];
} gvm_alloc_hdr;

#define GVM_HDR_OFF ((size_t)((gvm_alloc_hdr*)0)->data)

static const void*
host_alloc(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);

    uint64_t* raw_return = sp++;
    uint32_t size = (uint32_t)*sp;
    if (size == 0) size = 1;

    if (!gvm_cap_ok(rt, GVM_CAP_ALLOC)) { *raw_return = 0; return m3Err_none; }

    gvm_module* owner = gvm_modtab_owner_of_runtime(rt);
    if (owner && owner->pool_budget > 0) {
        LONG64 want = (LONG64)size + (LONG64)GVM_HDR_OFF;
        LONG64 after = InterlockedAdd64(&owner->pool_used, want);
        if (after > owner->pool_budget) {
            InterlockedAdd64(&owner->pool_used, -want);
            *raw_return = 0;
            return m3Err_none;
        }
    }

    gvm_alloc_hdr* h = (gvm_alloc_hdr*)ExAllocatePoolWithTag(
        NonPagedPoolNx, (SIZE_T)size + GVM_HDR_OFF, GVM_TAG_MOD);
    if (!h) {
        if (owner && owner->pool_budget > 0)
            InterlockedAdd64(&owner->pool_used, -((LONG64)size + (LONG64)GVM_HDR_OFF));
        *raw_return = 0;
        return m3Err_none;
    }
    h->size = size;
    *raw_return = (uint64_t)(uintptr_t)h->data;
    return m3Err_none;
}

static const void*
host_free(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    if (!gvm_cap_ok(rt, GVM_CAP_ALLOC)) return m3Err_none;
    uint64_t p = *(sp + 0);
    if (!p) return m3Err_none;

    gvm_alloc_hdr* h = (gvm_alloc_hdr*)((unsigned char*)(uintptr_t)p - GVM_HDR_OFF);
    uint64_t sz = h->size;
    ExFreePoolWithTag(h, GVM_TAG_MOD);

    gvm_module* owner = gvm_modtab_owner_of_runtime(rt);
    if (owner && owner->pool_budget > 0)
        InterlockedAdd64(&owner->pool_used, -((LONG64)sz + (LONG64)GVM_HDR_OFF));
    return m3Err_none;
}

static const void*
host_read_u8(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint64_t addr = *sp;
    uint32_t v = 0xFFFFFFFF;
    if (gvm_cap_ok(rt, GVM_CAP_READ_KMEM) && gvm_addr_ok(addr, 1)) {
        __try { v = *(volatile unsigned char*)(uintptr_t)addr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFFFFFF; }
    }
    *raw_return = v;
    return m3Err_none;
}

static const void*
host_read_u32(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint64_t addr = *sp;
    uint32_t v = 0xFFFFFFFF;
    if (gvm_cap_ok(rt, GVM_CAP_READ_KMEM) && gvm_addr_ok(addr, 4)) {
        __try { v = *(volatile uint32_t*)(uintptr_t)addr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFFFFFF; }
    }
    *raw_return = v;
    return m3Err_none;
}

static const void*
host_read_u64(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint64_t* raw_return = sp++;
    uint64_t addr = *sp;
    uint64_t v = 0;
    if (gvm_cap_ok(rt, GVM_CAP_READ_KMEM) && gvm_addr_ok(addr, 8)) {
        __try { v = *(volatile uint64_t*)(uintptr_t)addr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    }
    *raw_return = v;
    return m3Err_none;
}

static const void*
host_write_u64(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint64_t addr = *(sp + 0);
    uint64_t val  = *(sp + 1);
    if (gvm_cap_ok(rt, GVM_CAP_WRITE_KMEM) && gvm_addr_ok(addr, 8)) {
        __try { *(volatile uint64_t*)(uintptr_t)addr = val; }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return m3Err_none;
}

static const void*
host_read_bytes(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint64_t kaddr     = *(sp + 0);
    uint32_t guest_off = (uint32_t)*(sp + 1);
    uint32_t len       = (uint32_t)*(sp + 2);

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)guest_off + len > (uint64_t)mem_sz || len == 0) {
        *raw_return = 0;
        return m3Err_none;
    }

    unsigned char* dst = (unsigned char*)mem + guest_off;
    uint32_t copied = 0;
    if (gvm_cap_ok(rt, GVM_CAP_READ_KMEM) && gvm_addr_ok(kaddr, len)) {
        __try {
            RtlCopyMemory(dst, (const void*)(uintptr_t)kaddr, len);
            copied = len;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            copied = 0;
        }
    }
    *raw_return = copied;
    return m3Err_none;
}

static const void*
host_write_bytes(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint64_t kaddr     = *(sp + 0);
    uint32_t guest_off = (uint32_t)*(sp + 1);
    uint32_t len       = (uint32_t)*(sp + 2);

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)guest_off + len > (uint64_t)mem_sz || len == 0) {
        *raw_return = 0;
        return m3Err_none;
    }

    unsigned char* src = (unsigned char*)mem + guest_off;
    uint32_t copied = 0;
    if (gvm_cap_ok(rt, GVM_CAP_WRITE_KMEM) && gvm_addr_ok(kaddr, len)) {
        __try {
            RtlCopyMemory((void*)(uintptr_t)kaddr, src, len);
            copied = len;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            copied = 0;
        }
    }
    *raw_return = copied;
    return m3Err_none;
}

static const void*
host_current_irql(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp;
    *raw_return = gvm_cap_ok(rt, GVM_CAP_INTROSPECT) ? (uint32_t)KeGetCurrentIrql() : 0;
    return m3Err_none;
}

static const void*
host_process_id(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp;
    *raw_return = gvm_cap_ok(rt, GVM_CAP_INTROSPECT) ? (uint32_t)(uintptr_t)PsGetCurrentProcessId() : 0;
    return m3Err_none;
}

static const void*
host_thread_id(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp;
    *raw_return = gvm_cap_ok(rt, GVM_CAP_INTROSPECT) ? (uint32_t)(uintptr_t)PsGetCurrentThreadId() : 0;
    return m3Err_none;
}

// host_current_process() -> i64  (PEPROCESS)
static const void*
host_current_process(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint64_t* raw_return = sp;
    *raw_return = gvm_cap_ok(rt, GVM_CAP_INTROSPECT) ? (uint64_t)(uintptr_t)PsGetCurrentProcess() : 0;
    return m3Err_none;
}

// host_cpuid(leaf, subleaf, guest_out_off) -> void
// writes eax/ebx/ecx/edx as 4 consecutive u32s at guest_out_off
static const void*
host_cpuid(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t leaf    = (uint32_t)*(sp + 0);
    uint32_t subleaf = (uint32_t)*(sp + 1);
    uint32_t out_off = (uint32_t)*(sp + 2);

    if (!gvm_cap_ok(rt, GVM_CAP_CPUID_TSC)) return m3Err_none;

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)out_off + 16 > (uint64_t)mem_sz)
        return m3Err_none;

    int regs[4] = { 0 };
    __cpuidex(regs, (int)leaf, (int)subleaf);
    uint32_t* dst = (uint32_t*)((unsigned char*)mem + out_off);
    dst[0] = (uint32_t)regs[0];
    dst[1] = (uint32_t)regs[1];
    dst[2] = (uint32_t)regs[2];
    dst[3] = (uint32_t)regs[3];
    return m3Err_none;
}

// host_writemsr(idx, val) -> i32  (0 = ok, -1 = #GP)
static const void*
host_writemsr(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint32_t idx = (uint32_t)*(sp + 0);
    uint64_t val = *(sp + 1);
    uint32_t rv = 0;
    if (!gvm_cap_ok(rt, GVM_CAP_MSR_WRITE)) { *raw_return = (uint32_t)-1; return m3Err_none; }
    __try { __writemsr(idx, val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rv = (uint32_t)-1; }
    *raw_return = rv;
    return m3Err_none;
}

// host_phys_read(pa, guest_off, len) -> i32 (bytes copied, 0 on fail)
static const void*
host_phys_read(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint64_t pa       = *(sp + 0);
    uint32_t guest_off = (uint32_t)*(sp + 1);
    uint32_t len       = (uint32_t)*(sp + 2);

    if (!gvm_cap_ok(rt, GVM_CAP_PHYSMEM)) { *raw_return = 0; return m3Err_none; }

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)guest_off + len > (uint64_t)mem_sz || len == 0 || len > 4096) {
        *raw_return = 0; return m3Err_none;
    }

    PHYSICAL_ADDRESS pha; pha.QuadPart = (LONGLONG)pa;
    PVOID kva = MmMapIoSpaceEx(pha, len, PAGE_READONLY);
    if (!kva) { *raw_return = 0; return m3Err_none; }

    uint32_t copied = 0;
    __try {
        RtlCopyMemory((unsigned char*)mem + guest_off, kva, len);
        copied = len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { copied = 0; }

    MmUnmapIoSpace(kva, len);
    *raw_return = copied;
    return m3Err_none;
}

// host_phys_write(pa, guest_off, len) -> i32 (bytes written, 0 on fail)
static const void*
host_phys_write(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint64_t pa       = *(sp + 0);
    uint32_t guest_off = (uint32_t)*(sp + 1);
    uint32_t len       = (uint32_t)*(sp + 2);

    if (!gvm_cap_ok(rt, GVM_CAP_PHYSMEM)) { *raw_return = 0; return m3Err_none; }

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)guest_off + len > (uint64_t)mem_sz || len == 0 || len > 4096) {
        *raw_return = 0; return m3Err_none;
    }

    PHYSICAL_ADDRESS pha; pha.QuadPart = (LONGLONG)pa;
    PVOID kva = MmMapIoSpaceEx(pha, len, PAGE_READWRITE);
    if (!kva) { *raw_return = 0; return m3Err_none; }

    uint32_t written = 0;
    __try {
        RtlCopyMemory(kva, (unsigned char*)mem + guest_off, len);
        written = len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { written = 0; }

    MmUnmapIoSpace(kva, len);
    *raw_return = written;
    return m3Err_none;
}

// host_readmsr(idx) -> i64  (0 on #GP)
static const void*
host_readmsr(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint64_t* raw_return = sp++;
    uint32_t idx = (uint32_t)*sp;
    uint64_t v = 0;
    if (!gvm_cap_ok(rt, GVM_CAP_MSR_READ)) { *raw_return = 0; return m3Err_none; }
    __try { v = __readmsr(idx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    *raw_return = v;
    return m3Err_none;
}

// host_rdtsc() -> i64
static const void*
host_rdtsc(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint64_t* raw_return = sp;
    *raw_return = gvm_cap_ok(rt, GVM_CAP_CPUID_TSC) ? __rdtsc() : 0;
    return m3Err_none;
}

// host_notify_enable(kind) -> i32
//   kind: 0 = process (create+exit), 1 = image load
// returns 0 on success, negative on error
static const void*
host_notify_enable(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint32_t kind = (uint32_t)*sp;

    if (!gvm_cap_ok(rt, GVM_CAP_CALLBACKS)) { *raw_return = (uint32_t)-1; return m3Err_none; }
    gvm_notify_init();

    NTSTATUS s = STATUS_INVALID_PARAMETER;
    switch (kind) {
    case 0:
        if (!g_process_notify_on) {
            s = PsSetCreateProcessNotifyRoutineEx(gvm_process_notify_ex, FALSE);
            if (NT_SUCCESS(s)) g_process_notify_on = TRUE;
        } else {
            s = STATUS_SUCCESS;
        }
        break;
    case 1:
        if (!g_image_notify_on) {
            s = PsSetLoadImageNotifyRoutine(gvm_image_notify);
            if (NT_SUCCESS(s)) g_image_notify_on = TRUE;
        } else {
            s = STATUS_SUCCESS;
        }
        break;
    }
    *raw_return = NT_SUCCESS(s) ? 0 : (uint32_t)-1;
    return m3Err_none;
}

// host_dispatch_start() -> i32
// starts the driver-side worker that invokes on_process_create/exit/on_image_load
// exports of every loaded module as events fire. call once, no unregistering needed.
static const void*
host_dispatch_start(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp;
    if (!gvm_cap_ok(rt, GVM_CAP_CALLBACKS)) { *raw_return = (uint32_t)-1; return m3Err_none; }
    gvm_dispatch_start();
    *raw_return = 0;
    return m3Err_none;
}

// host_dispatch_stop() -> i32
// host_ih_trampoline() -> i64
// returns the kernel VA of the driver's native ETW-hook trampoline. guest
// writes this into WMI_LOGGER_CONTEXT.GetCpuClock via gvm_write_u64 after
// resolving the slot itself.
static const void*
host_ih_trampoline(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    if (!gvm_cap_ok(rt, GVM_CAP_WRITE_KMEM | GVM_CAP_CALLBACKS)) { *sp = 0; return m3Err_none; }
    gvm_dispatch_start();
    *sp = gvm_ih_trampoline_addr();
    return m3Err_none;
}

// host_ih_configure(rate, nt_base, nt_size) -> void
// tells the trampoline: sample 1-in-rate calls, only push events when the
// return address falls inside [nt_base, nt_base+nt_size).
static const void*
host_ih_configure(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(rt); UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t rate     = (uint32_t)*(sp + 0);
    uint64_t nt_base  =            *(sp + 1);
    uint32_t nt_size  = (uint32_t)*(sp + 2);
    gvm_ih_configure(rate, nt_base, nt_size);
    return m3Err_none;
}

// host_ih_quiesce() -> i64  (returns total trampoline hit count)
// spins until in-flight trampoline calls drain. call after guest restored
// the original GetCpuClock pointer, before it unloads.
static const void*
host_ih_quiesce(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(rt); UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    gvm_ih_wait_quiescent();
    *sp = gvm_ih_hit_count();
    return m3Err_none;
}

static const void*
host_dispatch_stop(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(rt); UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(mem);
    uint32_t* raw_return = (uint32_t*)sp;
    gvm_dispatch_stop_signal();
    *raw_return = 0;
    return m3Err_none;
}

// host_notify_poll(out_off, out_len) -> i32
//   copies one gvm_event into guest memory. returns bytes written or 0 if empty.
static const void*
host_notify_poll(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint32_t* raw_return = (uint32_t*)sp; sp++;
    uint32_t out_off = (uint32_t)*(sp + 0);
    uint32_t out_len = (uint32_t)*(sp + 1);

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)out_off + sizeof(gvm_event) > (uint64_t)mem_sz || out_len < sizeof(gvm_event)) {
        *raw_return = 0;
        return m3Err_none;
    }

    gvm_event e;
    if (!gvm_pop_event(&e)) {
        *raw_return = 0;
        return m3Err_none;
    }

    RtlCopyMemory((unsigned char*)mem + out_off, &e, sizeof(e));
    *raw_return = (uint32_t)sizeof(gvm_event);
    return m3Err_none;
}

// generic dispatcher: resolve `name` via MmGetSystemRoutineAddress, call via win64 ABI
static const void*
host_call(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(ctx);
    uint64_t* raw_return = sp++;

    if (!gvm_cap_ok(rt, GVM_CAP_HOSTCALL)) { *raw_return = 0; return m3Err_none; }
    if (gvm_deadline_exceeded(gvm_modtab_owner_of_runtime(rt))) { *raw_return = 0; return m3Err_none; }

    uint32_t name_off = (uint32_t)*(sp + 0);
    uint32_t name_len = (uint32_t)*(sp + 1);
    uint32_t argc     = (uint32_t)*(sp + 2);
    uint64_t a0 = *(sp + 3), a1 = *(sp + 4), a2 = *(sp + 5), a3 = *(sp + 6);
    uint64_t a4 = *(sp + 7), a5 = *(sp + 8), a6 = *(sp + 9), a7 = *(sp + 10);

    *raw_return = 0;

    uint32_t mem_sz = 0;
    m3_GetMemory(rt, &mem_sz, 0);
    if ((uint64_t)name_off + name_len > (uint64_t)mem_sz || name_len == 0 || name_len > 128)
        return m3Err_none;

    WCHAR wname[130];
    unsigned char* src = (unsigned char*)mem + name_off;
    for (uint32_t i = 0; i < name_len; i++) wname[i] = (WCHAR)src[i];
    wname[name_len] = 0;

    UNICODE_STRING us;
    us.Buffer        = wname;
    us.Length        = (USHORT)(name_len * sizeof(WCHAR));
    us.MaximumLength = (USHORT)((name_len + 1) * sizeof(WCHAR));

    PVOID target = MmGetSystemRoutineAddress(&us);
    if (!target)
        return m3Err_none;

    typedef uint64_t (*fn0)(void);
    typedef uint64_t (*fn1)(uint64_t);
    typedef uint64_t (*fn2)(uint64_t, uint64_t);
    typedef uint64_t (*fn3)(uint64_t, uint64_t, uint64_t);
    typedef uint64_t (*fn4)(uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (*fn5)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (*fn6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (*fn7)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    typedef uint64_t (*fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

    uint64_t rv = 0;
    __try {
        switch (argc) {
            case 0: rv = ((fn0)target)();                               break;
            case 1: rv = ((fn1)target)(a0);                             break;
            case 2: rv = ((fn2)target)(a0, a1);                         break;
            case 3: rv = ((fn3)target)(a0, a1, a2);                     break;
            case 4: rv = ((fn4)target)(a0, a1, a2, a3);                 break;
            case 5: rv = ((fn5)target)(a0, a1, a2, a3, a4);             break;
            case 6: rv = ((fn6)target)(a0, a1, a2, a3, a4, a5);         break;
            case 7: rv = ((fn7)target)(a0, a1, a2, a3, a4, a5, a6);     break;
            case 8: rv = ((fn8)target)(a0, a1, a2, a3, a4, a5, a6, a7); break;
            default: rv = 0; break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rv = 0;
    }
    *raw_return = rv;
    return m3Err_none;
}

// per-import trace wrapper. ret_slot = 1 for functions that return a value
// (sp[0] is the return slot, args start at sp[1]), ret_slot = 0 for void
// functions (args start at sp[0]).
#define WRAP_TR(name, ret_slot, argc_hint) \
    static const void* name##_tr(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem) { \
        gvm_module* mo = gvm_modtab_owner_of_runtime(rt); \
        unsigned int mid = mo ? mo->id : 0; \
        if (!gvm_trace_enabled_for(mid)) return name(rt, ctx, sp, mem); \
        uint64_t saved[4] = {0,0,0,0}; \
        int cap = (argc_hint) < 4 ? (argc_hint) : 4; \
        for (int i = 0; i < cap; i++) saved[i] = sp[(ret_slot) + i]; \
        const void* r = name(rt, ctx, sp, mem); \
        unsigned long long targv[4] = {saved[0], saved[1], saved[2], saved[3]}; \
        unsigned long long rv = (ret_slot) ? sp[0] : 0; \
        gvm_trace_push(mid, GVM_TRK_IMPORT, #name, cap, targv, rv); \
        return r; \
    }

WRAP_TR(host_dbg_print,       0, 2)
WRAP_TR(host_alloc,           1, 1)
WRAP_TR(host_free,            0, 1)
WRAP_TR(host_read_u8,         1, 1)
WRAP_TR(host_read_u32,        1, 1)
WRAP_TR(host_read_u64,        1, 1)
WRAP_TR(host_write_u64,       0, 2)
WRAP_TR(host_read_bytes,      1, 3)
WRAP_TR(host_write_bytes,     1, 3)
WRAP_TR(host_current_irql,    1, 0)
WRAP_TR(host_process_id,      1, 0)
WRAP_TR(host_thread_id,       1, 0)
WRAP_TR(host_current_process, 1, 0)
WRAP_TR(host_cpuid,           0, 3)
WRAP_TR(host_readmsr,         1, 1)
WRAP_TR(host_writemsr,        1, 2)
WRAP_TR(host_rdtsc,           1, 0)
WRAP_TR(host_phys_read,       1, 3)
WRAP_TR(host_phys_write,      1, 3)
WRAP_TR(host_notify_enable,   1, 1)
WRAP_TR(host_notify_poll,     1, 2)
WRAP_TR(host_dispatch_start,  1, 0)
WRAP_TR(host_dispatch_stop,   1, 0)
WRAP_TR(host_ih_trampoline,   1, 0)
WRAP_TR(host_ih_configure,    0, 3)
WRAP_TR(host_ih_quiesce,      1, 0)
WRAP_TR(host_call,            1, 4)
WRAP_TR(host_mem_base,        1, 0)
WRAP_TR(host_mem_size,        1, 0)
WRAP_TR(host_make_unistr,     1, 2)
WRAP_TR(host_free_unistr,     0, 1)

M3Result
gvm_link_host_imports(IM3Module module)
{
    M3Result r;

    #define LINK(name, sig, fn) do { \
        r = m3_LinkRawFunction(module, "env", (name), (sig), (fn)); \
        if (r && r != m3Err_functionLookupFailed) { \
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
                "[goodmans] link err: env.%s (%s) -> %s\n", (name), (sig), r); \
            return r; \
        } \
    } while (0)

    LINK("host_dbg_print",       "v(ii)",           &host_dbg_print_tr);
    LINK("host_alloc",           "I(i)",            &host_alloc_tr);
    LINK("host_free",            "v(I)",            &host_free_tr);
    LINK("host_read_u8",         "i(I)",            &host_read_u8_tr);
    LINK("host_read_u32",        "i(I)",            &host_read_u32_tr);
    LINK("host_read_u64",        "I(I)",            &host_read_u64_tr);
    LINK("host_write_u64",       "v(II)",           &host_write_u64_tr);
    LINK("host_read_bytes",      "i(Iii)",          &host_read_bytes_tr);
    LINK("host_write_bytes",     "i(Iii)",          &host_write_bytes_tr);
    LINK("host_current_irql",    "i()",             &host_current_irql_tr);
    LINK("host_process_id",      "i()",             &host_process_id_tr);
    LINK("host_thread_id",       "i()",             &host_thread_id_tr);
    LINK("host_current_process", "I()",             &host_current_process_tr);
    LINK("host_cpuid",           "v(iii)",          &host_cpuid_tr);
    LINK("host_readmsr",         "I(i)",            &host_readmsr_tr);
    LINK("host_writemsr",        "i(iI)",           &host_writemsr_tr);
    LINK("host_rdtsc",           "I()",             &host_rdtsc_tr);
    LINK("host_phys_read",       "i(Iii)",          &host_phys_read_tr);
    LINK("host_phys_write",      "i(Iii)",          &host_phys_write_tr);
    LINK("host_notify_enable",   "i(i)",            &host_notify_enable_tr);
    LINK("host_notify_poll",     "i(ii)",           &host_notify_poll_tr);
    LINK("host_dispatch_start",  "i()",             &host_dispatch_start_tr);
    LINK("host_dispatch_stop",   "i()",             &host_dispatch_stop_tr);
    LINK("host_ih_trampoline",   "I()",             &host_ih_trampoline_tr);
    LINK("host_ih_configure",    "v(iIi)",          &host_ih_configure_tr);
    LINK("host_ih_quiesce",      "I()",             &host_ih_quiesce_tr);
    LINK("host_call",            "I(iiiIIIIIIII)",  &host_call_tr);
    LINK("host_mem_base",        "I()",             &host_mem_base_tr);
    LINK("host_mem_size",        "i()",             &host_mem_size_tr);
    LINK("host_make_unistr",     "I(ii)",           &host_make_unistr_tr);
    LINK("host_free_unistr",     "v(I)",            &host_free_unistr_tr);

    #undef LINK
    return m3Err_none;
}

// generic FFI trampoline. dispatches to the kernel export stashed in
// ctx->userdata under the Windows x64 int-arg calling convention. up to 8
// integer/pointer args. floats are not supported in kernel mode without
// KeSaveFloatingPointState fencing, and no kernel export we care about
// takes floats anyway. return value writes back to sp[0].
typedef uint64_t (*gvm_fn_i8)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t);

static const void*
gvm_ffi_kernel(IM3Runtime rt, IM3ImportContext ctx, uint64_t* sp, void* mem)
{
    UNREFERENCED_PARAMETER(mem);
    gvm_fn_i8 fn = (gvm_fn_i8)ctx->userdata;
    IM3Function f = ctx->function;
    uint16_t numArgs = f->funcType ? f->funcType->numArgs : 0;
    uint16_t numRets = f->funcType ? f->funcType->numRets : 0;
    uint16_t sp_off  = numRets ? 1 : 0;
    if (numArgs > 8) numArgs = 8;

    uint64_t a[8] = {0};
    for (uint16_t i = 0; i < numArgs; i++) a[i] = sp[sp_off + i];

    const char* fname = f->import.fieldUtf8 ? f->import.fieldUtf8 : "?";
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] ffi-> %s fn=%p nargs=%u a0=%llx a1=%llx a2=%llx a3=%llx a4=%llx\n",
               fname, fn, numArgs, a[0], a[1], a[2], a[3], a[4]);

    uint64_t rv = 0;
    __try {
        rv = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[goodmans] ffi-EXC %s code=%08x\n", fname, GetExceptionCode());
        rv = 0;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] ffi<- %s rv=%llx\n", fname, rv);

    if (numRets) sp[0] = rv;

    gvm_module* mo = gvm_modtab_owner_of_runtime(rt);
    if (mo && gvm_trace_enabled_for(mo->id)) {
        const char* name = (f->import.fieldUtf8) ? f->import.fieldUtf8 : "<ffi>";
        gvm_trace_push(mo->id, GVM_TRK_IMPORT, name, numArgs, a, numRets ? rv : 0);
    }
    return m3Err_none;
}

// build a wasm3 raw-link signature string from the function type,
// e.g. numArgs=2 (i64, i32) numRets=1 (i32) -> "i(Ii)"
static void
gvm_wasm_sig(char* out, size_t out_sz, IM3FuncType t)
{
    if (out_sz < 4) { if (out_sz) out[0] = 0; return; }
    size_t p = 0;
    char c = 'v';
    if (t && t->numRets > 0) {
        switch (t->types[0]) {
        case c_m3Type_i32: c = 'i'; break;
        case c_m3Type_i64: c = 'I'; break;
        case c_m3Type_f32: c = 'f'; break;
        case c_m3Type_f64: c = 'F'; break;
        default:           c = 'i'; break;
        }
    }
    out[p++] = c;
    out[p++] = '(';
    uint16_t nargs = t ? t->numArgs : 0;
    // FuncType.types layout is [rets...][args...] per m3_function.h
    for (uint16_t i = 0; i < nargs && p + 2 < out_sz; i++) {
        u8 at = t->types[t->numRets + i];
        char ac;
        switch (at) {
        case c_m3Type_i32: ac = 'i'; break;
        case c_m3Type_i64: ac = 'I'; break;
        case c_m3Type_f32: ac = 'f'; break;
        case c_m3Type_f64: ac = 'F'; break;
        default:           ac = 'i'; break;
        }
        out[p++] = ac;
    }
    if (p + 1 < out_sz) out[p++] = ')';
    out[p] = 0;
}

// LDR_DATA_TABLE_ENTRY-ish subset. only fields we need. must not be paged.
typedef struct _GVM_KLDR_ENTRY {
    LIST_ENTRY     InLoadOrderLinks;
    PVOID          Rsv1[3];
    PVOID          DllBase;
    PVOID          EntryPoint;
    ULONG          SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} GVM_KLDR_ENTRY;

extern LIST_ENTRY PsLoadedModuleList;

// walk PsLoadedModuleList, find a loaded driver by base name (case-insensitive,
// with or without .sys), return its ImageBase. NULL if not present.
static PVOID
gvm_find_module_base(const char* mod_name)
{
    if (!mod_name || !mod_name[0]) return NULL;

    ANSI_STRING as; UNICODE_STRING want; NTSTATUS st;
    RtlInitAnsiString(&as, mod_name);
    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&want, &as, TRUE))) return NULL;

    PVOID found = NULL;
    for (PLIST_ENTRY e = PsLoadedModuleList.Flink;
         e && e != &PsLoadedModuleList;
         e = e->Flink)
    {
        GVM_KLDR_ENTRY* le = CONTAINING_RECORD(e, GVM_KLDR_ENTRY, InLoadOrderLinks);
        if (!le->BaseDllName.Buffer) continue;
        // try full match then match-without-.sys
        if (RtlEqualUnicodeString(&le->BaseDllName, &want, TRUE)) {
            found = le->DllBase; break;
        }
        // strip .sys off le->BaseDllName if present
        UNICODE_STRING trimmed = le->BaseDllName;
        if (trimmed.Length >= 8) {
            WCHAR* end = (WCHAR*)((unsigned char*)trimmed.Buffer + trimmed.Length - 8);
            if (_wcsnicmp(end, L".sys", 4) == 0) trimmed.Length -= 8;
        }
        if (RtlEqualUnicodeString(&trimmed, &want, TRUE)) {
            found = le->DllBase; break;
        }
    }
    RtlFreeUnicodeString(&want);
    return found;
}

// resolve a named export from a loaded module by walking its PE export
// directory. avoids needing the driver's PDB.
static PVOID
gvm_find_export(PVOID module_base, const char* export_name)
{
    if (!module_base || !export_name) return NULL;
    unsigned char* base = (unsigned char*)module_base;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    IMAGE_DATA_DIRECTORY* dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dd->Size == 0 || dd->VirtualAddress == 0) return NULL;

    IMAGE_EXPORT_DIRECTORY* ex =
        (IMAGE_EXPORT_DIRECTORY*)(base + dd->VirtualAddress);
    ULONG*  names   = (ULONG*)(base + ex->AddressOfNames);
    USHORT* ords    = (USHORT*)(base + ex->AddressOfNameOrdinals);
    ULONG*  funcs   = (ULONG*)(base + ex->AddressOfFunctions);

    for (ULONG i = 0; i < ex->NumberOfNames; i++) {
        const char* n = (const char*)(base + names[i]);
        if (strcmp(n, export_name) == 0) {
            ULONG rva = funcs[ords[i]];
            // forwarder if RVA points inside the export dir
            if (rva >= dd->VirtualAddress && rva < dd->VirtualAddress + dd->Size)
                return NULL;   // forwarder resolution skipped
            return base + rva;
        }
    }
    return NULL;
}

// walk the module's import table and auto-link anything that resolves via
// MmGetSystemRoutineAddress (nt/hal) OR by walking PsLoadedModuleList for
// exports of any other loaded driver. import naming:
//   env.SymbolName             -> nt/hal
//   drv$modulename.SymbolName  -> that specific loaded driver
M3Result
gvm_link_kernel_fallback(IM3Module module)
{
    if (!module) return m3Err_none;
    for (u32 i = 0; i < module->numFuncImports; i++) {
        IM3Function f = &module->functions[i];
        if (f->compiled) continue;
        if (!f->import.moduleUtf8 || !f->import.fieldUtf8) continue;

        const char* mod   = f->import.moduleUtf8;
        const char* fname = f->import.fieldUtf8;
        PVOID kfn = NULL;

        if (strcmp(mod, "env") == 0) {
            UNICODE_STRING us; ANSI_STRING as;
            RtlInitAnsiString(&as, fname);
            if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&us, &as, TRUE))) {
                kfn = MmGetSystemRoutineAddress(&us);
                RtlFreeUnicodeString(&us);
            }
        } else if (strncmp(mod, "drv$", 4) == 0) {
            PVOID base = gvm_find_module_base(mod + 4);
            if (base) kfn = gvm_find_export(base, fname);
        }

        if (!kfn) continue;

        char sig[40];
        gvm_wasm_sig(sig, sizeof(sig), f->funcType);
        M3Result r = m3_LinkRawFunctionEx(module, mod, fname, sig,
                                          &gvm_ffi_kernel, kfn);
        if (r && r != m3Err_functionLookupFailed) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "[goodmans] ffi link err: %s.%s (%s) -> %s\n", mod, fname, sig, r);
        } else if (!r) {
            gvm_log("ffi resolved %s.%s (%s) -> %p", mod, fname, sig, kfn);
        }
    }
    return m3Err_none;
}
