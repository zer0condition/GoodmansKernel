/* ffi_demo.c - shows the direct FFI patterns exposed by the driver.
 * every kernel API call here goes through the trampoline: no name lookup
 * at call time, standard x64 ABI, traced with the real symbol name.
 */

#include "../guest_sdk/gvm.h"

GVM_MANIFEST(GVM_CAP_ALLOC | GVM_CAP_READ_KMEM | GVM_CAP_INTROSPECT | GVM_CAP_HOSTCALL);

// direct kernel imports beyond what's in the SDK. any exported nt/hal
// symbol works this way.
GVM_IMPORT_KERNEL(KeQueryTimeIncrement) u32 KeQueryTimeIncrement(void);
GVM_IMPORT_KERNEL(MmGetPhysicalAddress) u64 MmGetPhysicalAddress(PVOID va);
GVM_IMPORT_KERNEL(PsIsSystemThread)     BOOLEAN PsIsSystemThread(PVOID thread);

static char g_scratch[512];

GVM_EXPORT(demo_scalar_apis)
u32 demo_scalar_apis(void)
{
    // pure scalar-in / scalar-out FFI. no buffer marshalling needed.
    u32 pid   = (u32)(u64)PsGetCurrentProcessId();
    u32 tid   = (u32)(u64)PsGetCurrentThreadId();
    u8  irql  = KeGetCurrentIrql();
    u32 tinc  = KeQueryTimeIncrement();
    u8  isys  = PsIsSystemThread(KeGetCurrentThread());

    char buf[128];
    u32 n = 0;
    n += gvm_strcpy(buf + n, "pid=");   gvm_dec(pid, buf + n); n += gvm_strlen(buf + n);
    n += gvm_strcpy(buf + n, " tid=");  gvm_dec(tid, buf + n); n += gvm_strlen(buf + n);
    n += gvm_strcpy(buf + n, " irql="); gvm_dec(irql, buf + n); n += gvm_strlen(buf + n);
    n += gvm_strcpy(buf + n, " tinc="); gvm_dec(tinc, buf + n); n += gvm_strlen(buf + n);
    n += gvm_strcpy(buf + n, " sys=");  gvm_dec(isys, buf + n); n += gvm_strlen(buf + n);
    gvm_print(buf);
    return pid;
}

// showing a POINTER out-param pattern. own-buffer VA computed via gvm_kva.
GVM_EXPORT(demo_pointer_out)
u64 demo_pointer_out(u64 va)
{
    // MmGetPhysicalAddress returns a PHYSICAL_ADDRESS struct (union of u64).
    // on x64 with FFI it comes back in RAX as a u64.
    u64 pa = MmGetPhysicalAddress((PVOID)va);
    return pa;
}

// buffer read INTO a wasm-side struct via a real kernel VA computed from
// our own linear memory. no host_read_bytes needed.
GVM_EXPORT(demo_buffer)
u64 demo_buffer(u64 kernel_src, u32 len)
{
    if (len > sizeof(g_scratch)) len = sizeof(g_scratch);
    // build a real kernel VA of &g_scratch[0] inside our wasm memory
    u64 dst = gvm_kva(g_scratch);
    RtlCopyMemory((PVOID)dst, (PVOID)kernel_src, len);
    // return the first 8 bytes as a sanity check
    u64 v = 0;
    for (u32 i = 0; i < 8 && i < len; i++)
        v |= ((u64)(u8)g_scratch[i]) << (i * 8);
    return v;
}

// UNICODE_STRING roundtrip. build one, resolve a symbol with it, free it.
GVM_EXPORT(demo_unistr)
u64 demo_unistr(void)
{
    // "ExAllocatePool2" as UTF-16LE
    static const u16 wname[] = {
        'E','x','A','l','l','o','c','a','t','e','P','o','o','l','2'
    };
    u64 us = host_make_unistr((u32)(unsigned long)wname, sizeof(wname));
    if (!us) return 0;
    u64 addr = (u64)MmGetSystemRoutineAddress((PVOID)us);
    host_free_unistr(us);
    return addr;
}

// canonical entry point exercised by the GUI's Call button
GVM_EXPORT(run_all)
u64 run_all(void)
{
    demo_scalar_apis();
    (void)demo_unistr();
    return 0;
}
