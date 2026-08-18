/* trace_ring.c - per-import call trace for wasm guest debugging.
 * ring buffer holds recent host-import invocations, export calls, and traps.
 * GUI polls via ioctl for live view.
 */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"

static gvm_trace_entry g_ring[GVM_TRACE_ENTRIES];
static ULONG64    g_seq  = 0;
static ULONG      g_head = 0;
static KSPIN_LOCK g_lock;
static BOOLEAN    g_init = FALSE;
static volatile LONG g_mode = GVM_TRACE_OFF;
static volatile LONG g_only = 0;   // module_id filter when mode==ON_MODULE

void
gvm_trace_init(void)
{
    if (g_init) return;
    RtlZeroMemory(g_ring, sizeof(g_ring));
    KeInitializeSpinLock(&g_lock);
    g_init = TRUE;
}

BOOLEAN
gvm_trace_enabled_for(unsigned int module_id)
{
    LONG m = g_mode;
    if (m == GVM_TRACE_OFF) return FALSE;
    if (m == GVM_TRACE_ON_ALL) return TRUE;
    return (LONG)module_id == g_only;
}

void
gvm_trace_push(unsigned int module_id, unsigned int kind, const char* name,
               unsigned int argc, const ULONG64* argv, ULONG64 rv)
{
    if (!g_init) return;
    if (!gvm_trace_enabled_for(module_id)) return;

    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);

    // capture pre-lock so lock IRQL doesn't confuse the value
    unsigned int cap_irql = (unsigned int)KeGetCurrentIrql();
    unsigned int cap_tid  = (unsigned int)(ULONG_PTR)PsGetCurrentThreadId();

    KIRQL irql;
    KeAcquireSpinLock(&g_lock, &irql);
    gvm_trace_entry* e = &g_ring[g_head];
    RtlZeroMemory(e, sizeof(*e));
    e->timestamp_100ns = (ULONG64)now.QuadPart;
    e->module_id = module_id;
    e->kind      = kind;
    e->thread_id = cap_tid;
    e->irql      = cap_irql;
    e->argc      = argc > 4 ? 4 : argc;
    for (unsigned int i = 0; i < e->argc; i++) e->argv[i] = argv[i];
    e->rv        = rv;
    if (name) RtlStringCbCopyA(e->name, sizeof(e->name), name);
    g_seq++;
    g_head = (g_head + 1) % GVM_TRACE_ENTRIES;
    KeReleaseSpinLock(&g_lock, irql);
}

NTSTATUS
gvm_ioctl_tail_trace(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_trace_in) || out_len < sizeof(gvm_trace_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_trace_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    gvm_trace_out* out = (gvm_trace_out*)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(gvm_trace_out), 'gTrc');
    if (!out) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(out, sizeof(*out));

    KIRQL irql;
    KeAcquireSpinLock(&g_lock, &irql);

    ULONG64 seq_here  = g_seq;
    ULONG64 want_from = in.last_seq;
    ULONG64 available = 0;
    ULONG64 dropped   = 0;

    if (seq_here > want_from) {
        available = seq_here - want_from;
        if (available > GVM_TRACE_ENTRIES) {
            dropped = available - GVM_TRACE_ENTRIES;
            available = GVM_TRACE_ENTRIES;
        }
        ULONG start = (g_head + GVM_TRACE_ENTRIES - (ULONG)available) % GVM_TRACE_ENTRIES;
        for (ULONG64 i = 0; i < available; i++) {
            ULONG src = (start + (ULONG)i) % GVM_TRACE_ENTRIES;
            out->entries[i] = g_ring[src];
        }
    }
    out->next_seq = seq_here;
    out->count    = (unsigned int)available;
    out->dropped  = (unsigned int)dropped;

    KeReleaseSpinLock(&g_lock, irql);

    RtlCopyMemory(buf, out, sizeof(*out));
    ExFreePoolWithTag(out, 'gTrc');
    irp->IoStatus.Information = sizeof(gvm_trace_out);
    return STATUS_SUCCESS;
}

NTSTATUS
gvm_ioctl_trace_ctl(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len = sp->Parameters.DeviceIoControl.InputBufferLength;
    void* buf    = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_trace_ctl_in) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }
    gvm_trace_ctl_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    InterlockedExchange(&g_mode, (LONG)in.mode);
    InterlockedExchange(&g_only, (LONG)in.module_id);

    gvm_log_push("trace mode=%u module=%u", in.mode, in.module_id);
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}
