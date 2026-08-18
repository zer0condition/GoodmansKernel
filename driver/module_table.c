/* module_table.c - fixed-size slot allocator for loaded modules.
 * refcount is atomic (LONG). teardown holds the module's call_mutex so
 * any in-flight m3_Call has to finish before the runtime is freed. */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"

static gvm_module g_modules[GVM_MAX_MODULES];
static KSPIN_LOCK g_lock;

NTSTATUS
gvm_modtab_init(void)
{
    RtlZeroMemory(g_modules, sizeof(g_modules));
    KeInitializeSpinLock(&g_lock);
    // init every mutex up front so iterators that wait on all slots (e.g. the
    // callback dispatch worker) don't touch an uninitialized DISPATCHER_HEADER
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++)
        KeInitializeMutex(&g_modules[i].call_mutex, 0);
    return STATUS_SUCCESS;
}

void
gvm_modtab_teardown(void)
{
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        if (g_modules[i].used) {
            g_modules[i].refcount = 1;   // force teardown
            gvm_modtab_free(&g_modules[i]);
        }
    }
}

gvm_module*
gvm_modtab_alloc(void)
{
    KIRQL irql;
    gvm_module* out = NULL;

    KeAcquireSpinLock(&g_lock, &irql);
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        if (!g_modules[i].used) {
            g_modules[i].used     = TRUE;
            g_modules[i].id       = i + 1;
            g_modules[i].refcount = 1;
            KeInitializeMutex(&g_modules[i].call_mutex, 0);
            out = &g_modules[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_lock, irql);
    return out;
}

gvm_module*
gvm_modtab_get(unsigned int id)
{
    if (id == 0 || id > GVM_MAX_MODULES) return NULL;
    gvm_module* m = &g_modules[id - 1];
    return m->used ? m : NULL;
}

gvm_module*
gvm_modtab_find_by_hash(unsigned long long hash)
{
    KIRQL irql;
    gvm_module* out = NULL;

    KeAcquireSpinLock(&g_lock, &irql);
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        if (g_modules[i].used && g_modules[i].hash == hash) {
            out = &g_modules[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_lock, irql);
    return out;
}

// atomic find + increment under the table lock. safer than
// find_by_hash + separate refcount++ which races with concurrent unload.
gvm_module*
gvm_modtab_find_by_hash_incref(unsigned long long hash, unsigned int expected_size)
{
    KIRQL irql;
    gvm_module* out = NULL;

    KeAcquireSpinLock(&g_lock, &irql);
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        if (g_modules[i].used
            && g_modules[i].hash == hash
            && g_modules[i].wasm_size == expected_size) {
            InterlockedIncrement(&g_modules[i].refcount);
            out = &g_modules[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_lock, irql);
    return out;
}

gvm_module*
gvm_modtab_iter(unsigned int idx)
{
    if (idx >= GVM_MAX_MODULES) return NULL;
    return &g_modules[idx];
}

gvm_module*
gvm_modtab_owner_of_runtime(IM3Runtime rt)
{
    if (!rt) return NULL;
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        if (g_modules[i].used && g_modules[i].runtime == rt)
            return &g_modules[i];
    }
    return NULL;
}

void
gvm_modtab_free(gvm_module* m)
{
    if (!m || !m->used) return;

    // only the last reference proceeds to teardown
    if (InterlockedDecrement(&m->refcount) > 0) return;

    // block until any in-flight m3_Call finishes so the runtime is quiescent
    KeWaitForSingleObject(&m->call_mutex, Executive, KernelMode, FALSE, NULL);

    if (m->runtime) {
        m3_FreeRuntime(m->runtime);
        m->runtime = NULL;
    }
    if (m->env) {
        m3_FreeEnvironment(m->env);
        m->env = NULL;
    }
    if (m->wasm_bytes) {
        ExFreePoolWithTag(m->wasm_bytes, GVM_TAG_WBUF);
        m->wasm_bytes = NULL;
    }
    m->module    = NULL;
    m->wasm_size = 0;
    m->hash      = 0;

    // publish used=FALSE before releasing so waiters see it on retry
    m->used = FALSE;

    KeReleaseMutex(&m->call_mutex, FALSE);
}
