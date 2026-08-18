/* ih.c - the ONE piece of InfinityHook that cannot live in wasm.
 *
 * this file exposes a native trampoline that guests install into
 * WMI_LOGGER_CONTEXT.GetCpuClock. everything else (nt base discovery,
 * EtwpDebuggerData pattern scan, offset resolution, atomic pointer swap)
 * lives in sample_guest/infinity_hook.c as a portable wasm demo.
 *
 * the trampoline runs at ETW callback IRQL (up to DISPATCH_LEVEL), samples
 * calls, pushes a SYSCALL event onto the shared dispatch ring, and returns
 * the real QPC. the ring is drained by the same dispatch worker that
 * handles process/image notify events.
 */

#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"

extern void gvm_push_event_generic(unsigned int kind, uint32_t pid,
                                    uint64_t a, uint64_t b, uint32_t c);

static volatile LONG64 g_ih_hits     = 0;
static volatile LONG   g_ih_inflight = 0;
static volatile LONG   g_ih_rate     = 1000;

// caller (guest) sets this before installing the hook so the trampoline
// can reject callers outside nt's image range (avoids false positives from
// unrelated ETW paths).
static volatile UINT64 g_ih_nt_lo    = 0;
static volatile UINT64 g_ih_nt_hi    = 0;

// signature matches WMI_LOGGER_CONTEXT.GetCpuClock: takes no args, returns
// a QPC value. must be __stdcall/default x64 ABI (matches).
static UINT64 gvm_ih_trampoline(void)
{
    InterlockedIncrement(&g_ih_inflight);
    LONG64 hits = InterlockedIncrement64(&g_ih_hits);
    LONG rate = g_ih_rate;
    if (rate <= 0) rate = 1;

    if ((hits % rate) == 0) {
        PVOID retaddr = _ReturnAddress();
        UINT64 lo = g_ih_nt_lo, hi = g_ih_nt_hi;
        BOOLEAN in_nt = (lo && (UINT64)(uintptr_t)retaddr >= lo &&
                                (UINT64)(uintptr_t)retaddr <  hi);
        if (in_nt || !lo) {
            HANDLE tid = PsGetCurrentThreadId();
            gvm_push_event_generic(3,                          // SYSCALL kind
                                   (uint32_t)(uintptr_t)tid,
                                   (UINT64)(uintptr_t)retaddr, // a1 in on_syscall
                                   0,
                                   (UINT32)hits);              // a2 in on_syscall
        }
    }

    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);
    UINT64 rv = (UINT64)qpc.QuadPart;
    InterlockedDecrement(&g_ih_inflight);
    return rv;
}

// exposed as host imports: guests get the trampoline VA to plug into the
// GetCpuClock slot they located, and configure sampling / nt-range so the
// trampoline can skip non-syscall callers.

UINT64 gvm_ih_trampoline_addr(void)
{
    return (UINT64)(uintptr_t)&gvm_ih_trampoline;
}

void gvm_ih_configure(UINT32 rate, UINT64 nt_base, UINT32 nt_size)
{
    if (rate > 0) InterlockedExchange(&g_ih_rate, (LONG)rate);
    g_ih_nt_lo = nt_base;
    g_ih_nt_hi = nt_base + nt_size;
}

// spin until any in-flight trampoline calls drain. call after guest writes
// the original pointer back to the GetCpuClock slot but before it unloads.
void gvm_ih_wait_quiescent(void)
{
    for (int i = 0; i < 1000 && g_ih_inflight > 0; i++) {
        LARGE_INTEGER li; li.QuadPart = -10000;   // 1ms
        KeDelayExecutionThread(KernelMode, FALSE, &li);
    }
}

// stats (guest asks for hit count so it can report accurately)
UINT64 gvm_ih_hit_count(void) { return (UINT64)g_ih_hits; }

// driver unload safety: nothing to tear down since we don't own the slot,
// but we can spin briefly in case guests left the hook installed.
void gvm_ih_teardown(void)
{
    gvm_ih_wait_quiescent();
}
