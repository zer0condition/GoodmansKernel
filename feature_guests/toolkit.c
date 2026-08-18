/* toolkit.c - frida-style primitives for the GUI to drive
 *
 * exposes a set of exports that the GUI calls to read/write kernel state.
 * results that don't fit in a u64 are written into the guest's own linear
 * memory at a caller-provided offset, and the GUI pulls them back with the
 * new IOCTL_GVM_READ_GUEST ioctl.
 */

#include "../guest_sdk/gvm.h"

// declare all caps we might exercise
GVM_MANIFEST(GVM_CAP_ALLOC | GVM_CAP_READ_KMEM | GVM_CAP_WRITE_KMEM |
             GVM_CAP_MSR_READ | GVM_CAP_MSR_WRITE | GVM_CAP_PHYSMEM |
             GVM_CAP_CPUID_TSC | GVM_CAP_HOSTCALL | GVM_CAP_INTROSPECT);

// scratch buffer accessible from linear memory; GUI reads bytes out of here
static u8 g_scratch[16384];

// forces the scratch buffer to appear in linear memory so its address is
// stable across calls. return its offset so the GUI knows where to read from.
GVM_EXPORT(toolkit_scratch_off)
u32 toolkit_scratch_off(void)
{
    return (u32)(unsigned long long)&g_scratch[0];
}

GVM_EXPORT(toolkit_scratch_size)
u32 toolkit_scratch_size(void) { return sizeof(g_scratch); }

// KERNEL VIRTUAL MEMORY
GVM_EXPORT(toolkit_read_kmem)
u32 toolkit_read_kmem(u64 va, u32 guest_off, u32 len)
{
    return gvm_read_bytes(va, guest_off, len);
}

GVM_EXPORT(toolkit_read_u8)
u32 toolkit_read_u8(u64 va) { return gvm_read_u8(va); }

GVM_EXPORT(toolkit_read_u32)
u32 toolkit_read_u32(u64 va) { return gvm_read_u32(va); }

GVM_EXPORT(toolkit_read_u64)
u64 toolkit_read_u64(u64 va) { return gvm_read_u64(va); }

// PHYSICAL MEMORY
GVM_EXPORT(toolkit_read_phys)
u32 toolkit_read_phys(u64 pa, u32 guest_off, u32 len)
{
    return gvm_phys_read(pa, guest_off, len);
}

// MSR
GVM_EXPORT(toolkit_readmsr)
u64 toolkit_readmsr(u32 idx) { return gvm_readmsr(idx); }

// CPUID - writes eax,ebx,ecx,edx (4 x u32) at guest_off
GVM_EXPORT(toolkit_cpuid)
void toolkit_cpuid(u32 leaf, u32 sub, u32 guest_off)
{
    gvm_cpuid(leaf, sub, guest_off);
}

// RDTSC
GVM_EXPORT(toolkit_rdtsc)
u64 toolkit_rdtsc(void) { return gvm_rdtsc(); }

// EXPORT LOOKUP - returns kernel VA of an ntoskrnl/hal export by name
// name_off in linear memory, name_len bytes (nul terminator NOT required)
GVM_EXPORT(toolkit_resolve)
u64 toolkit_resolve(u32 name_off, u32 name_len)
{
    if (name_len == 0 || name_len > 128) return 0;
    // scratch a copy so we can nul-terminate cleanly
    static char buf[144];
    for (u32 i = 0; i < name_len && i < sizeof(buf)-1; i++) {
        buf[i] = ((char*)0)[name_off + i];
    }
    buf[name_len < sizeof(buf) ? name_len : sizeof(buf)-1] = 0;
    // use host_call with resolve helper: convention is fn_name = "MmGetSystemRoutineAddress",
    // arg0 = pointer to UNICODE_STRING. Building UNICODE_STRING is fragile from wasm.
    // Simpler: we do it host-side. Return 0 here for now, GUI can compare with a
    // dedicated api if needed. This is a placeholder for future extension.
    return 0;
}

// INTROSPECTION
GVM_EXPORT(toolkit_current_process)
u64 toolkit_current_process(void) { return gvm_current_process(); }

GVM_EXPORT(toolkit_current_pid)
u32 toolkit_current_pid(void) { return gvm_process_id(); }

GVM_EXPORT(toolkit_current_tid)
u32 toolkit_current_tid(void) { return gvm_thread_id(); }

GVM_EXPORT(toolkit_current_irql)
u32 toolkit_current_irql(void) { return gvm_current_irql(); }

// SIMPLE PROCESS ENUM - walks ActiveProcessLinks from PsGetCurrentProcess()
// writes packed records: [u64 pid][u64 eprocess][char name[16]] each
// returns count of records written. cap = max records the buffer can hold.
//
// offsets are Win10/11 x64. hard-coded here for demo. real toolkit would
// use signatures to derive them at load.
#define EPROC_ACTIVE_LINKS  0x448     // _EPROCESS.ActiveProcessLinks
#define EPROC_UNIQUE_PID    0x440     // _EPROCESS.UniqueProcessId
#define EPROC_IMAGE_NAME    0x5a8     // _EPROCESS.ImageFileName

GVM_EXPORT(toolkit_enum_procs)
u32 toolkit_enum_procs(u32 guest_off, u32 cap)
{
    if (cap == 0) return 0;

    u64 self = gvm_current_process();
    if (!self) return 0;

    u64 head = self + EPROC_ACTIVE_LINKS;
    u64 cur  = head;
    u32 count = 0;

    for (u32 iter = 0; iter < 4096 && count < cap; iter++) {
        cur = gvm_read_u64(cur);  // follow Flink
        if (!cur || cur == head) break;
        u64 eproc = cur - EPROC_ACTIVE_LINKS;
        u64 pid   = gvm_read_u64(eproc + EPROC_UNIQUE_PID);

        // read record into scratch at guest_off + count*32
        u32 rec_off = guest_off + count * 32;
        *(u64*)((char*)0 + rec_off)      = pid;
        *(u64*)((char*)0 + rec_off + 8)  = eproc;
        // 16 bytes of image name
        gvm_read_bytes(eproc + EPROC_IMAGE_NAME, rec_off + 16, 15);
        ((char*)0)[rec_off + 31] = 0;
        count++;
    }
    return count;
}
