/* log_ring.c - kernel-side ring of recent debug lines. GUI polls via ioctl. */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"

static gvm_log_entry g_ring[GVM_LOG_ENTRIES];
static ULONG64 g_seq = 0;               // strictly increasing, monotonic
static ULONG   g_head = 0;              // next write slot
static KSPIN_LOCK g_lock;
static BOOLEAN g_init = FALSE;

void
gvm_log_init(void)
{
    if (g_init) return;
    RtlZeroMemory(g_ring, sizeof(g_ring));
    KeInitializeSpinLock(&g_lock);
    g_init = TRUE;
}

void
gvm_log_push(const char* fmt, ...)
{
    if (!g_init) return;
    char tmp[GVM_LOG_ENTRY_LEN];
    va_list ap; va_start(ap, fmt);
    RtlStringCbVPrintfA(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    // also mirror to DbgPrint so DebugView keeps working
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans] %s\n", tmp);

    LARGE_INTEGER now;
    KeQuerySystemTimePrecise(&now);

    KIRQL irql;
    KeAcquireSpinLock(&g_lock, &irql);
    gvm_log_entry* e = &g_ring[g_head];
    e->timestamp_100ns = (ULONG64)now.QuadPart;
    RtlStringCbCopyA(e->line, sizeof(e->line), tmp);
    g_seq++;
    g_head = (g_head + 1) % GVM_LOG_ENTRIES;
    KeReleaseSpinLock(&g_lock, irql);
}

NTSTATUS
gvm_ioctl_tail_log(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_tail_in) || out_len < sizeof(gvm_tail_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_tail_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    gvm_tail_out* out = (gvm_tail_out*)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(gvm_tail_out), 'gLog');
    if (!out) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(out, sizeof(*out));

    KIRQL irql;
    KeAcquireSpinLock(&g_lock, &irql);

    ULONG64 seq_here = g_seq;
    ULONG64 want_from = in.last_seq;
    ULONG64 available = 0;

    if (seq_here > want_from) {
        available = seq_here - want_from;
        if (available > GVM_LOG_ENTRIES) {
            out->dropped = (unsigned int)(available - GVM_LOG_ENTRIES);
            available = GVM_LOG_ENTRIES;
        }
    }

    // walk backwards from head, taking `available` newest entries in order
    ULONG head = g_head;
    for (ULONG64 i = 0; i < available; i++) {
        ULONG idx = (head + GVM_LOG_ENTRIES - (ULONG)available + (ULONG)i) % GVM_LOG_ENTRIES;
        out->entries[i] = g_ring[idx];
    }
    out->count    = (unsigned int)available;
    out->next_seq = seq_here;

    KeReleaseSpinLock(&g_lock, irql);

    RtlCopyMemory(buf, out, sizeof(*out));
    ExFreePoolWithTag(out, 'gLog');
    irp->IoStatus.Information = sizeof(gvm_tail_out);
    return STATUS_SUCCESS;
}
