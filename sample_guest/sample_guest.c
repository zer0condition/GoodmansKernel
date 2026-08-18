/* sample_guest.c - reference guest exercising the Goodmans SDK */

#include "gvm.h"

/* print pid/tid/irql via SDK wrappers over host_call */
GVM_EXPORT(show_context)
u64 show_context(void)
{
    HANDLE pid  = PsGetCurrentProcessId();
    HANDLE tid  = PsGetCurrentThreadId();
    u8     irql = KeGetCurrentIrql();

    char hex[17];
    gvm_print("context:");
    gvm_hex64((u64)pid, hex);  gvm_print(hex);
    gvm_hex64((u64)tid, hex);  gvm_print(hex);
    gvm_hex64((u64)irql, hex); gvm_print(hex);

    return ((u64)(u32)(u64)pid << 32) | (u32)(u64)tid;
}

/* alloc a kernel pool, write a pattern, read it back, free */
GVM_EXPORT(pool_roundtrip)
u64 pool_roundtrip(void)
{
    const SIZE_T sz = 256;
    PVOID p = ExAllocatePoolWithTag(NonPagedPoolNx, sz, 'GsmP');
    if (!p) { gvm_print("alloc failed"); return 0; }

    for (u32 i = 0; i < sz / 8; i++)
        gvm_write_u64((u64)p + i * 8, 0xAABBCCDD00000000ULL | i);

    u64 v = gvm_read_u64((u64)p);
    char hex[17];
    gvm_print("first slot:");
    gvm_hex64(v, hex); gvm_print(hex);

    ExFreePoolWithTag(p, 'GsmP');
    return (u64)p;
}

/* resolve arbitrary ntoskrnl exports via host_call */
GVM_EXPORT(resolve_export)
u64 resolve_export(void)
{
    u64 irql = GVM_CALL0(KeGetCurrentIrql);
    u64 tick = GVM_CALL0(KeQueryTimeIncrement);

    char hex[17];
    gvm_print("KeGetCurrentIrql:");
    gvm_hex64(irql, hex); gvm_print(hex);
    gvm_print("KeQueryTimeIncrement:");
    gvm_hex64(tick, hex); gvm_print(hex);

    return tick;
}

GVM_EXPORT(run_all)
u64 run_all(void)
{
    show_context();
    pool_roundtrip();
    resolve_export();
    gvm_print("run_all done");
    return 0;
}
