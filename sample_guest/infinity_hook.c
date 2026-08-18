/* infinity_hook.c - InfinityHook as a wasm guest.
 * original technique: github.com/everdox/InfinityHook (MIT). */

#include "../guest_sdk/gvm.h"

GVM_MANIFEST(GVM_CAP_READ_KMEM | GVM_CAP_WRITE_KMEM | GVM_CAP_INTROSPECT |
             GVM_CAP_CALLBACKS | GVM_CAP_HOSTCALL);

static u64  g_nt_base       = 0;
static u32  g_nt_size       = 0;
static u64  g_getclock_slot = 0;
static u64  g_orig_getclock = 0;

// walk pages backwards from any VA inside nt.exe until we find the MZ+PE
// header pair. classic module-base recovery.
static u64 find_nt_base(u64 va_in_nt)
{
    u64 p = va_in_nt & ~0xFFFULL;
    for (u32 i = 0; i < 4096 && p >= 0x1000; i++, p -= 0x1000) {
        if (!MmIsAddressValid((PVOID)p)) continue;
        if ((gvm_read_u32(p) & 0xFFFF) != 0x5A4D) continue;
        u32 pe_off = gvm_read_u32(p + 0x3C);
        if (!pe_off || pe_off > 0x1000) continue;
        if (gvm_read_u32(p + pe_off) != 0x00004550) continue;
        return p;
    }
    return 0;
}

// scan nt's .data section for the classic InfinityHook signature that
// marks the start of EtwpDebuggerData: 2C 08 04 38
static u64 find_etwp_debugger_data(u64 nt)
{
    u32 pe_off = gvm_read_u32(nt + 0x3C);
    u16 nsec   = (u16)(gvm_read_u32(nt + pe_off + 6) & 0xFFFF);
    u16 opt_sz = (u16)(gvm_read_u32(nt + pe_off + 20) & 0xFFFF);
    u64 sec = nt + pe_off + 24 + opt_sz;

    for (u16 i = 0; i < nsec; i++, sec += 40) {
        if (gvm_read_u32(sec) != 0x61746164) continue;   // '.dat'
        u32 v_sz  = gvm_read_u32(sec + 8);
        u32 v_rva = gvm_read_u32(sec + 12);
        u64 base = nt + v_rva;
        u64 end  = base + v_sz;
        for (u64 q = base; q + 8 < end; q += 4) {
            if (!MmIsAddressValid((PVOID)q)) continue;
            if (gvm_read_u8(q + 0) == 0x2C &&
                gvm_read_u8(q + 1) == 0x08 &&
                gvm_read_u8(q + 2) == 0x04 &&
                gvm_read_u8(q + 3) == 0x38)
                return q;
        }
        return 0;
    }
    return 0;
}

// EtwpDebuggerData -> silo -> kernel WMI_LOGGER_CONTEXT -> GetCpuClock slot.
// GetCpuClock offset inside the context varies by Windows build (0x28 on
// most modern Win10/11, 0x18/0x30 on older). we try each and accept the
// one whose current value points into nt (unhooked baseline).
static u64 locate_getclock_slot(u64 nt, u32 nt_size, u64* out_orig)
{
    u64 etwp = find_etwp_debugger_data(nt);
    if (!etwp) return 0;

    u64 silo = gvm_read_u64(etwp + 0x10);
    if (!silo || !MmIsAddressValid((PVOID)silo)) return 0;

    u64 ctx = gvm_read_u64(silo + 2 * 8);   // silo[2] = kernel logger context
    if (!ctx || !MmIsAddressValid((PVOID)ctx)) return 0;

    static const u32 candidates[] = { 0x28, 0x18, 0x30 };
    for (u32 i = 0; i < 3; i++) {
        u64 slot = ctx + candidates[i];
        if (!MmIsAddressValid((PVOID)slot)) continue;
        u64 v = gvm_read_u64(slot);
        if (v >= nt && v < nt + nt_size) {
            *out_orig = v;
            return slot;
        }
    }
    return 0;
}

// install: locate everything, atomically swap in the driver's trampoline.
GVM_EXPORT(start)
u64 start(u64 sample_rate)
{
    if (g_getclock_slot) { gvm_print("[ih] already installed"); return 0; }

    PVOID any = MmGetSystemRoutineAddress((PVOID)0);  // arg unused for demo
    // real resolve: build a UNICODE_STRING for a known nt export, pass it in
    u64 ustr = host_make_unistr((u32)(unsigned long)L"KeBugCheckEx",
                                sizeof(L"KeBugCheckEx") - sizeof(u16));
    any = MmGetSystemRoutineAddress((PVOID)ustr);
    host_free_unistr(ustr);
    if (!any) { gvm_print("[ih] MmGetSystemRoutineAddress failed"); return 1; }

    g_nt_base = find_nt_base((u64)any);
    if (!g_nt_base) { gvm_print("[ih] nt base not found"); return 2; }
    u32 pe_off = gvm_read_u32(g_nt_base + 0x3C);
    g_nt_size = gvm_read_u32(g_nt_base + pe_off + 24 + 56);   // SizeOfImage

    g_getclock_slot = locate_getclock_slot(g_nt_base, g_nt_size, &g_orig_getclock);
    if (!g_getclock_slot) {
        gvm_print("[ih] GetCpuClock slot not located (build offsets may differ)");
        return 3;
    }

    // hand the trampoline our nt bounds so it filters non-syscall callers
    gvm_ih_configure((u32)sample_rate, g_nt_base, g_nt_size);

    // atomic pointer swap. aligned 8-byte write on x64 is single-copy atomic
    u64 tramp = gvm_ih_trampoline();
    if (!tramp) { gvm_print("[ih] trampoline addr = 0"); return 4; }
    gvm_write_u64(g_getclock_slot, tramp);

    char line[128], t[24]; u32 n = 0;
    n += gvm_strcpy(line + n, "[ih] hook installed. nt=0x");
    gvm_hex64(g_nt_base, t);       n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " slot=0x");
    gvm_hex64(g_getclock_slot, t); n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " orig=0x");
    gvm_hex64(g_orig_getclock, t); n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " tramp=0x");
    gvm_hex64(tramp, t);           n += gvm_strcpy(line + n, t);
    gvm_print(line);
    return 0;
}

// uninstall: atomically restore original, then wait for in-flight trampoline
// calls to drain before we let the module be unloaded.
GVM_EXPORT(stop)
u64 stop(void)
{
    if (!g_getclock_slot) { gvm_print("[ih] not installed"); return 0; }
    gvm_write_u64(g_getclock_slot, g_orig_getclock);
    u64 total = gvm_ih_quiesce();
    g_getclock_slot = 0;
    g_orig_getclock = 0;

    char line[80], t[24]; u32 n = 0;
    n += gvm_strcpy(line + n, "[ih] hook removed. trampoline hits=");
    gvm_dec((u32)total, t); n += gvm_strcpy(line + n, t);
    gvm_print(line);
    return total;
}

// dispatched by the driver's worker on every sampled syscall event.
static volatile u32 g_seen = 0;

GVM_EXPORT(on_syscall)
u64 on_syscall(u64 tid, u64 retaddr, u64 counter)
{
    g_seen++;
    if ((g_seen & 63) != 0) return 0;   // print 1 in 64 to keep log readable

    char line[128], t[24]; u32 n = 0;
    n += gvm_strcpy(line + n, "[ih] tid=");
    gvm_dec((u32)tid, t);      n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " ret=0x");
    gvm_hex64(retaddr, t);     n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " hit=");
    gvm_dec((u32)counter, t);  n += gvm_strcpy(line + n, t);
    gvm_print(line);
    return 0;
}
