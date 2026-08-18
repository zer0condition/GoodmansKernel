/* watchdog.c - system thread that patrols loaded modules for stuck guests.
 * a module holding call_mutex longer than GVM_WATCHDOG_MAX_MS is flagged
 * "poisoned" so no more calls dispatch, and the incident is logged.
 * force-unload IOCTL bypasses the standard mutex wait for these cases.
 */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"

#define GVM_WATCHDOG_MAX_MS   5000    // max wall-clock a single call may hold
#define GVM_WATCHDOG_PERIOD_MS 500    // scan cadence

static PETHREAD  g_thread   = NULL;
static KEVENT    g_shutdown;
static BOOLEAN   g_running  = FALSE;

static VOID gvm_watchdog_body(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    LARGE_INTEGER wait; wait.QuadPart = -((LONGLONG)GVM_WATCHDOG_PERIOD_MS * 10 * 1000);
    LARGE_INTEGER freq;
    ULONG64 max_ticks = 0;
    (void)KeQueryPerformanceCounter(&freq);
    if (freq.QuadPart) max_ticks = ((ULONG64)freq.QuadPart * GVM_WATCHDOG_MAX_MS) / 1000ULL;

    for (;;) {
        NTSTATUS s = KeWaitForSingleObject(&g_shutdown, Executive, KernelMode, FALSE, &wait);
        if (s == STATUS_SUCCESS) break;   // shutdown signaled

        if (!max_ticks) continue;

        ULONG64 now = (ULONG64)KeQueryPerformanceCounter(NULL).QuadPart;
        for (unsigned int i = 0; i < 32; i++) {
            gvm_module* m = gvm_modtab_iter(i);
            if (!m || !m->used) continue;
            ULONG64 h = m->mutex_hold_qpc;
            if (!h) continue;
            if (now <= h) continue;
            if ((now - h) < max_ticks) continue;
            if (m->poisoned) continue;

            InterlockedExchange(&m->poisoned, 1);
            gvm_log("watchdog: module %u '%s' stuck >%u ms - marked poisoned",
                    m->id, m->name, GVM_WATCHDOG_MAX_MS);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

void gvm_watchdog_start(void)
{
    if (g_running) return;
    KeInitializeEvent(&g_shutdown, NotificationEvent, FALSE);
    HANDLE h;
    NTSTATUS s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, gvm_watchdog_body, NULL);
    if (!NT_SUCCESS(s)) { gvm_log("watchdog: thread create failed 0x%x", s); return; }
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&g_thread, NULL);
    ZwClose(h);
    g_running = TRUE;
    gvm_log("watchdog: started (period=%ums, max_hold=%ums)", GVM_WATCHDOG_PERIOD_MS, GVM_WATCHDOG_MAX_MS);
}

void gvm_watchdog_stop(void)
{
    if (!g_running) return;
    KeSetEvent(&g_shutdown, IO_NO_INCREMENT, FALSE);
    if (g_thread) {
        KeWaitForSingleObject(g_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_thread);
        g_thread = NULL;
    }
    g_running = FALSE;
}

// IOCTL handler: bypass the normal unload path's mutex wait when a module is
// poisoned. safe only because poisoned modules can no longer make host calls.
NTSTATUS
gvm_ioctl_force_unload(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len = sp->Parameters.DeviceIoControl.InputBufferLength;
    void* buf    = irp->AssociatedIrp.SystemBuffer;
    if (in_len < sizeof(gvm_unload_in) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }
    gvm_unload_in in; RtlCopyMemory(&in, buf, sizeof(in));

    gvm_module* m = gvm_modtab_get(in.module_id);
    if (!m) { irp->IoStatus.Information = 0; return STATUS_NOT_FOUND; }

    InterlockedExchange(&m->poisoned, 1);
    gvm_log("force_unload: module %u '%s'", m->id, m->name);

    // drain refcount to 1 then free (which decrements to 0). same pattern
    // as unload_all; safe even on a wedged guest because poison stops new calls.
    while (InterlockedCompareExchange(&m->refcount, 1, m->refcount) != 1) {
        if (!m->used) break;
    }
    gvm_modtab_free(m);

    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}
