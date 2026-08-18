/* ioctl_handler.c - LOAD / CALL / UNLOAD / LIST / INFO / UNLOAD_ALL */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"
#include "wasm3/m3_env.h"
#include "wasm3/m3_function.h"

static void
copy_errmsg(char* dst, size_t dst_sz, const char* src)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = 0;
    if (src) RtlStringCbCopyA(dst, dst_sz, src);
}

// FNV-1a 64-bit hash for wasm-blob dedup
static unsigned long long fnv1a64(const unsigned char* p, size_t n)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// reads the module's capability bitmask by invoking its exported __gvm_caps
// function. must run AFTER link so the guest can use hosts inside its manifest
// function if it needs to (typical case: returns a constant). if the export
// is absent, returns GVM_CAP_ALL (open policy, backward compat).
unsigned int gvm_read_module_caps(gvm_module* mod)
{
    if (!mod || !mod->runtime) return GVM_CAP_ALL;

    IM3Function fn = NULL;
    M3Result r = m3_FindFunction(&fn, mod->runtime, "__gvm_caps");
    if (r || !fn) return GVM_CAP_ALL;

    // must be called under the mutex + big stack, but at this point no one
    // else has a handle on this module yet. still, use the standard path.
    // start with wide-open caps so the manifest call itself isn't gated.
    mod->caps = GVM_CAP_ALL;
    r = gvm_call_locked(mod, fn, 0, NULL);
    if (r) return GVM_CAP_ALL;

    unsigned int caps = 0;
    void* rvp = &caps;
    m3_GetResults(fn, 1, (const void**)&rvp);
    return caps ? caps : GVM_CAP_ALL;
}

BOOLEAN gvm_deadline_exceeded(gvm_module* mod)
{
    if (!mod || mod->exec_deadline_qpc == 0) return FALSE;
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);
    return (ULONG64)now.QuadPart >= mod->exec_deadline_qpc;
}

NTSTATUS
gvm_ioctl_load(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_load_in) || out_len < sizeof(gvm_load_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_load_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    if (in.wasm_size == 0 || in.wasm_size > (16u * 1024u * 1024u)) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }
    if (in_len < sizeof(gvm_load_in) + in.wasm_size) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_load_out out = { 0 };
    unsigned char* wasm_start = (unsigned char*)buf + sizeof(gvm_load_in);

    // dedup: hash first, return existing module id if match. atomic incref
    // via find_by_hash_incref so a concurrent unload can't win the race.
    unsigned long long h = fnv1a64(wasm_start, in.wasm_size);
    gvm_module* existing = gvm_modtab_find_by_hash_incref(h, in.wasm_size);
    if (existing) {
        out.module_id = existing->id;
        out.status    = 0;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "reused");
        goto done;
    }

    gvm_module* mod = gvm_modtab_alloc();
    if (!mod) {
        out.status = -1;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "module table full");
        goto done;
    }

    mod->wasm_bytes = (unsigned char*)ExAllocatePoolWithTag(NonPagedPoolNx, in.wasm_size, GVM_TAG_WBUF);
    if (!mod->wasm_bytes) {
        gvm_modtab_free(mod);
        out.status = -2;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "wasm alloc failed");
        goto done;
    }
    RtlCopyMemory(mod->wasm_bytes, wasm_start, in.wasm_size);
    mod->wasm_size = in.wasm_size;
    mod->hash      = h;
    mod->pool_budget = in.pool_budget ? (LONG64)in.pool_budget : (4LL * 1024 * 1024);
    mod->pool_used   = 0;
    mod->caps      = GVM_CAP_ALL;   // set to real value after link+manifest call
    mod->exec_deadline_qpc = 0;
    RtlStringCbCopyA(mod->name, sizeof(mod->name), in.name[0] ? in.name : "guest");

    mod->env = m3_NewEnvironment();
    if (!mod->env) {
        gvm_modtab_free(mod);
        out.status = -3;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "NewEnvironment failed");
        goto done;
    }

    unsigned int stack = in.stack_bytes ? in.stack_bytes : GVM_STACK_DEFAULT;
    mod->runtime = m3_NewRuntime(mod->env, stack, NULL);
    if (!mod->runtime) {
        gvm_modtab_free(mod);
        out.status = -4;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "NewRuntime failed");
        goto done;
    }

    M3Result r = m3_ParseModule(mod->env, &mod->module, mod->wasm_bytes, mod->wasm_size);
    if (r) {
        gvm_modtab_free(mod);
        out.status = -5;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), r);
        goto done;
    }

    r = m3_LoadModule(mod->runtime, mod->module);
    if (r) {
        gvm_modtab_free(mod);
        out.status = -6;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), r);
        goto done;
    }

    r = gvm_link_host_imports(mod->module);
    if (r && r != m3Err_functionLookupFailed) {
        gvm_modtab_free(mod);
        out.status = -7;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), r);
        goto done;
    }

    // resolve any leftover env.* imports against ntoskrnl/hal export table
    gvm_link_kernel_fallback(mod->module);

    // read the guest's declared capability manifest (if any) and lock down.
    mod->caps = gvm_read_module_caps(mod);
    gvm_log("load module_id=%u name=%s wasm=%u bytes hash=%016llx caps=%08x",
            mod->id, mod->name, mod->wasm_size, mod->hash, mod->caps);

    out.module_id = mod->id;
    out.status    = 0;
    copy_errmsg(out.err_msg, sizeof(out.err_msg), "ok");

done:
    RtlCopyMemory(buf, &out, sizeof(out));
    irp->IoStatus.Information = sizeof(out);
    return STATUS_SUCCESS;
}

NTSTATUS
gvm_ioctl_call(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_call_in) || out_len < sizeof(gvm_call_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_call_in in;
    RtlCopyMemory(&in, buf, sizeof(in));
    in.export_name[sizeof(in.export_name) - 1] = 0;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] IOCTL_CALL mod=%u export=%s argc=%u\n",
               in.module_id, in.export_name, in.argc);

    gvm_call_out out = { 0 };

    gvm_module* mod = gvm_modtab_get(in.module_id);
    if (!mod) {
        out.status = -1;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "no such module");
        goto done;
    }

    IM3Function fn = NULL;
    M3Result r = m3Err_none;
    __try {
        r = m3_FindFunction(&fn, mod->runtime, in.export_name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = "m3_FindFunction raised kernel exception";
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] m3_FindFunction export=%s fn=%p r=%s\n",
               in.export_name, fn, r ? r : "ok");
    if (r || !fn) {
        out.status = -2;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), r ? r : "export not found");
        goto done;
    }

    if (in.argc > GVM_MAX_ARGS) in.argc = GVM_MAX_ARGS;

    const void* argp[GVM_MAX_ARGS];
    for (unsigned int i = 0; i < in.argc; i++)
        argp[i] = &in.argv[i];

    gvm_set_deadline_ms(mod, in.timeout_ms);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] pre-gvm_call_locked mod=%u fn=%p export=%s\n",
               in.module_id, fn, in.export_name);

    r = gvm_call_locked(mod, fn, in.argc, argp);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[goodmans] post-gvm_call_locked r=%s\n", r ? r : "ok");

    if (!r && gvm_deadline_exceeded(mod)) r = "execution deadline exceeded";
    if (r) {
        out.status = -3;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), r);
        M3ErrorInfo einfo = { 0 };
        m3_GetErrorInfo(mod->runtime, &einfo);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "[goodmans] m3_Call fail: r=%s message=%s file=%s line=%u\n",
            r, einfo.message ? einfo.message : "(none)",
            einfo.file ? einfo.file : "(none)", einfo.line);
        goto done;
    }

    unsigned long long rv = 0;
    void* rvp = &rv;
    r = m3_GetResults(fn, 1, (const void**)&rvp);
    // r may be non-null when the export returns void
    out.rv     = rv;
    out.status = 0;
    copy_errmsg(out.err_msg, sizeof(out.err_msg), "ok");
    gvm_log("call mod=%u %s(argc=%u) rv=0x%llx", in.module_id, in.export_name, in.argc, rv);

done:
    RtlCopyMemory(buf, &out, sizeof(out));
    irp->IoStatus.Information = sizeof(out);
    return STATUS_SUCCESS;
}

NTSTATUS
gvm_ioctl_unload(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len = sp->Parameters.DeviceIoControl.InputBufferLength;
    void* buf    = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_unload_in) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_unload_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    gvm_module* mod = gvm_modtab_get(in.module_id);
    if (!mod) {
        irp->IoStatus.Information = 0;
        return STATUS_NOT_FOUND;
    }
    gvm_modtab_free(mod);
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static void fill_entry(gvm_module_entry* e, const gvm_module* m)
{
    e->id         = m->id;
    e->wasm_size  = m->wasm_size;
    e->hash       = m->hash;
    e->exports    = 0;
    e->mem_pages  = 0;
    e->pool_bytes = (unsigned long long)m->pool_used;
    if (m->runtime) {
        uint32_t mem_sz = 0;
        m3_GetMemory(m->runtime, &mem_sz, 0);
        e->mem_pages = mem_sz / 65536;
    }
    RtlCopyMemory(e->name, m->name, sizeof(e->name));
}

NTSTATUS
gvm_ioctl_list(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (out_len < sizeof(gvm_list_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_list_out out = { 0 };
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        gvm_module* m = gvm_modtab_iter(i);
        if (!m || !m->used) continue;
        if (out.count >= GVM_MAX_MODULES) break;
        fill_entry(&out.entries[out.count], m);
        out.count++;
    }

    RtlCopyMemory(buf, &out, sizeof(out));
    irp->IoStatus.Information = sizeof(out);
    return STATUS_SUCCESS;
}

NTSTATUS
gvm_ioctl_info(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_info_in) || out_len < sizeof(gvm_info_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_info_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    gvm_info_out out = { 0 };

    gvm_module* mod = gvm_modtab_get(in.module_id);
    if (!mod) {
        out.status = -1;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "no such module");
        goto done;
    }

    fill_entry(&out.base, mod);

    // enumerate exports + imports from the wasm3 module chain
    if (mod->runtime) {
        IM3Module m = mod->runtime->modules;
        while (m) {
            for (u32 i = 0; i < m->numFunctions; i++) {
                IM3Function fn = &m->functions[i];
                if (i < m->numFuncImports && fn->import.fieldUtf8 && out.import_count < GVM_MAX_INFO_IMPORTS) {
                    RtlStringCbCopyA(out.imports[out.import_count], GVM_INFO_NAME_LEN, fn->import.fieldUtf8);
                    out.import_count++;
                }
                if (fn->export_name && out.export_count < GVM_MAX_INFO_EXPORTS) {
                    RtlStringCbCopyA(out.exports[out.export_count], GVM_INFO_NAME_LEN, fn->export_name);
                    out.export_count++;
                }
            }
            m = m->next;
        }
    }

    out.status = 0;
    copy_errmsg(out.err_msg, sizeof(out.err_msg), "ok");

done:
    RtlCopyMemory(buf, &out, sizeof(out));
    irp->IoStatus.Information = sizeof(out);
    return STATUS_SUCCESS;
}

NTSTATUS
gvm_ioctl_unload_all(PIRP irp, PIO_STACK_LOCATION sp)
{
    UNREFERENCED_PARAMETER(sp);
    for (unsigned int i = 0; i < GVM_MAX_MODULES; i++) {
        gvm_module* m = gvm_modtab_iter(i);
        if (!m || !m->used) continue;
        // drain refcount atomically to 1, then free (which decrements to 0)
        while (InterlockedCompareExchange(&m->refcount, 1, m->refcount) != 1) {
            if (!m->used) break;
        }
        gvm_modtab_free(m);
    }
    irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

NTSTATUS
gvm_ioctl_read_guest(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf     = irp->AssociatedIrp.SystemBuffer;

    if (in_len < sizeof(gvm_read_guest_in) || out_len < sizeof(gvm_read_guest_out) || !buf) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    gvm_read_guest_in in;
    RtlCopyMemory(&in, buf, sizeof(in));

    gvm_read_guest_out out;
    RtlZeroMemory(&out, sizeof(out));

    if (in.length == 0 || in.length > GVM_MAX_READ_GUEST_BYTES) {
        out.status = -1;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "bad length");
        goto done;
    }

    gvm_module* mod = gvm_modtab_get(in.module_id);
    if (!mod || !mod->runtime) {
        out.status = -2;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "no such module");
        goto done;
    }

    uint32_t mem_sz = 0;
    uint8_t* mem = m3_GetMemory(mod->runtime, &mem_sz, 0);
    if (!mem) {
        out.status = -3;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "no memory");
        goto done;
    }
    if ((uint64_t)in.offset + in.length > mem_sz) {
        out.status = -4;
        copy_errmsg(out.err_msg, sizeof(out.err_msg), "out of range");
        goto done;
    }

    RtlCopyMemory(out.data, mem + in.offset, in.length);
    out.length = in.length;
    out.status = 0;
    copy_errmsg(out.err_msg, sizeof(out.err_msg), "ok");

done:
    RtlCopyMemory(buf, &out, sizeof(out));
    irp->IoStatus.Information = sizeof(out);
    return STATUS_SUCCESS;
}
