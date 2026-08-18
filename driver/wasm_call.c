/* wasm_call.c - serialized wasm3 call under a per-module mutex on a fresh 64KB
 * kernel stack. one entry point everyone in the driver uses to invoke guests. */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"
#include "wasm3/m3_function.h"
#include "wasm3/wasm3.h"

typedef struct {
    IM3Function fn;
    uint32_t    argc;
    const void** argp;
    M3Result    result;
} gvm_call_ctx;

static VOID gvm_call_callout(_In_ PVOID p)
{
    gvm_call_ctx* c = (gvm_call_ctx*)p;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] pre-m3Call fn=%p name=%s\n",
               c->fn, (c->fn && c->fn->export_name) ? c->fn->export_name : "?");
    __try {
        c->result = m3_Call(c->fn, c->argc, c->argp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c->result = "wasm3 dispatch raised kernel exception";
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] post-m3Call result=%s\n", c->result ? c->result : "ok");
}

M3Result
gvm_call_locked(gvm_module* mod, IM3Function fn, unsigned int argc, const void** argp)
{
    if (!mod || !fn) return "invalid module or function";
    if (mod->poisoned) return "module poisoned by watchdog";

    KeWaitForSingleObject(&mod->call_mutex, Executive, KernelMode, FALSE, NULL);

    if (mod->poisoned) { KeReleaseMutex(&mod->call_mutex, FALSE); return "module poisoned"; }

    // stamp mutex acquire for watchdog visibility
    LARGE_INTEGER _f;
    mod->mutex_hold_qpc = (ULONG64)KeQueryPerformanceCounter(&_f).QuadPart;

    // trace: entering export
    if (gvm_trace_enabled_for(mod->id)) {
        unsigned long long targv[4] = {0,0,0,0};
        unsigned int tac = argc > 4 ? 4 : argc;
        for (unsigned int i = 0; i < tac; i++) targv[i] = argp[i] ? *(uint64_t*)argp[i] : 0;
        gvm_trace_push(mod->id, GVM_TRK_CALL, fn->export_name ? fn->export_name : "?", tac, targv, 0);
    }

    gvm_call_ctx c = { fn, argc, argp, m3Err_none };
    NTSTATUS s = KeExpandKernelStackAndCalloutEx(
        gvm_call_callout, &c,
        64 * 1024,
        FALSE,
        NULL);

    // clear deadline after every call so it doesn't leak across invocations
    mod->exec_deadline_qpc = 0;

    // trace: return or trap
    if (gvm_trace_enabled_for(mod->id)) {
        if (!NT_SUCCESS(s)) {
            unsigned long long argv[1] = { (unsigned long long)s };
            gvm_trace_push(mod->id, GVM_TRK_TRAP, "kstack_callout_failed", 1, argv, 0);
        } else if (c.result) {
            // push the trap reason itself
            gvm_trace_push(mod->id, GVM_TRK_TRAP, c.result, 0, NULL, 0);
            // and each frame of the wasm backtrace as its own entry
            IM3BacktraceInfo bt = m3_GetBacktrace(mod->runtime);
            if (bt) {
                unsigned int depth = 0;
                for (IM3BacktraceFrame f = bt->frames; f && f != M3_BACKTRACE_TRUNCATED && depth < 16;
                     f = f->next, depth++) {
                    unsigned long long a[2] = { (unsigned long long)f->moduleOffset, depth };
                    const char* fname = (f->function && f->function->export_name)
                        ? f->function->export_name : "<anon>";
                    gvm_trace_push(mod->id, GVM_TRK_TRAP, fname, 2, a, 0);
                }
            }
        } else {
            gvm_trace_push(mod->id, GVM_TRK_RETURN, fn->export_name ? fn->export_name : "?", 0, NULL, 0);
        }
    }

    mod->mutex_hold_qpc = 0;
    KeReleaseMutex(&mod->call_mutex, FALSE);

    if (!NT_SUCCESS(s))
        return "KeExpandKernelStackAndCalloutEx failed";
    return c.result;
}

// caller sets timeout_ms > 0 before invoking gvm_call_locked to arm the deadline
void gvm_set_deadline_ms(gvm_module* mod, unsigned int timeout_ms)
{
    if (!mod || timeout_ms == 0) { if (mod) mod->exec_deadline_qpc = 0; return; }
    LARGE_INTEGER freq;
    LARGE_INTEGER now = KeQueryPerformanceCounter(&freq);
    ULONG64 ticks = ((ULONG64)freq.QuadPart * timeout_ms) / 1000ULL;
    mod->exec_deadline_qpc = (ULONG64)now.QuadPart + ticks;
}
