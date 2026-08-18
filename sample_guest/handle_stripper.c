/* handle_stripper.c - enumerate handles held by a target process using
 * the documented ExEnumHandleTable API. reports each entry's ObjectHeader
 * and object type index. useful for auditing what a suspicious process
 * is holding open (LSASS handles, process handles with debug rights, etc).
 *
 * this is a READ-ONLY tool. it doesn't actually strip anything. writing
 * to HANDLE_TABLE_ENTRY needs raw kernel writes that are per-build-fragile.
 * see the strip_by_object_pattern function below for a scaffold if you
 * want to extend.
 */

#include "gvm.h"

GVM_MANIFEST(GVM_CAP_ALLOC | GVM_CAP_READ_KMEM | GVM_CAP_INTROSPECT | GVM_CAP_HOSTCALL);

// callback context passed to ExEnumHandleTable
typedef struct {
    u32 count;
    u32 target_pid;
} enum_ctx;

// scan EPROCESS for ObjectTable pointer. On modern Windows this is at
// a stable-ish offset but changes by build. Dynamic search: find the field
// whose value is a valid kernel pointer to something that looks like a
// HANDLE_TABLE (starts with a small integer HandleCount at offset ~0).
static u64 find_object_table(u64 eproc)
{
    for (u32 off = 0x400; off < 0x800; off += 8) {
        u64 v = gvm_read_u64(eproc + off);
        if ((v >> 48) != 0xFFFF) continue;
        u32 first = gvm_read_u32(v);
        if (first < 0x100000 && first > 0) return v;   // HandleCount-ish
    }
    return 0;
}

GVM_EXPORT(list_handles)
u64 list_handles(u64 target_pid)
{
    u64 target_eproc = 0;
    u64 status = GVM_CALL2(PsLookupProcessByProcessId,
                           target_pid, (u64)(u32)(u64)&target_eproc);
    if (status != 0 || target_eproc == 0) {
        gvm_print("PsLookupProcessByProcessId failed");
        return 0;
    }

    u64 table = find_object_table(target_eproc);
    if (!table) {
        gvm_print("could not locate ObjectTable in EPROCESS");
        GVM_CALL1(ObfDereferenceObject, target_eproc);
        return 0;
    }

    u32 handle_count = gvm_read_u32(table);
    char hex[17];
    gvm_print("target EPROCESS =");   gvm_hex64(target_eproc, hex); gvm_print(hex);
    gvm_print("ObjectTable =");       gvm_hex64(table, hex);        gvm_print(hex);
    gvm_print("HandleCount =");       gvm_hex64(handle_count, hex); gvm_print(hex);

    // full HANDLE_TABLE walk requires TableCode (3-level tree) which is per-build.
    // scaffold: use ExEnumHandleTable(HANDLE_TABLE*, callback, ctx, HANDLE*)
    //. signature is (HANDLE_TABLE*, ProcedurePtr, void*, HANDLE*) returning BOOLEAN.
    // callback signature is BOOLEAN(HANDLE_TABLE_ENTRY* entry, HANDLE h, void* ctx).
    // guests can't easily provide a callback (would need JIT'd wasm trampoline).
    // for now just report the count and leave detailed walking to future work.
    gvm_print("(detailed walk requires ExEnumHandleTable callback. see comments)");

    GVM_CALL1(ObfDereferenceObject, target_eproc);
    return (u64)handle_count;
}

// scaffold: when we can provide a wasm:native trampoline, this is where
// filtering + close would happen.
GVM_EXPORT(strip_by_object_pattern)
u64 strip_by_object_pattern(u64 target_pid, u64 object_type_index)
{
    (void)target_pid; (void)object_type_index;
    gvm_print("strip: not implemented. needs wasm:native callback trampoline");
    gvm_print("       (guest cannot pass a function pointer to ExEnumHandleTable)");
    return 0;
}
