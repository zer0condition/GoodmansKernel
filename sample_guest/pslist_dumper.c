/* pslist_dumper.c - walk SYSTEM_PROCESS_INFORMATION via ZwQuerySystemInformation.
 * safer than raw EPROCESS.ActiveProcessLinks traversal because we use the
 * documented API surface. no offsets, no PatchGuard exposure. */
#include "gvm.h"

#define SystemProcessInformation 5

// SYSTEM_PROCESS_INFORMATION layout (x64). offsets verified against public winternl.
typedef struct {
    u32   next_off;         // 0x000  ULONG NextEntryOffset
    u32   thread_count;     // 0x004  ULONG NumberOfThreads
    u64   pad1[3];          // 0x008  reserved + times
    u64   create_time;      // 0x020  LARGE_INTEGER CreateTime
    u64   user_time;
    u64   kernel_time;
    u16   img_name_len;     // 0x038  USHORT Length of ImageName
    u16   img_name_max;     // 0x03A
    u32   pad2;             // 0x03C
    u64   img_name_ptr;     // 0x040  PWSTR Buffer
    u32   base_priority;    // 0x048
    u32   pad3;             // 0x04C
    u64   unique_pid;       // 0x050  HANDLE UniqueProcessId
    u64   inherited_ppid;   // 0x058  HANDLE InheritedFromUniqueProcessId
    // ... more fields we don't need
} spi_hdr;

static void wide_to_ascii(u64 wsrc, u16 wlen_bytes, char* dst, u32 dst_max)
{
    u16 chars = wlen_bytes / 2;
    if (chars > dst_max - 1) chars = (u16)(dst_max - 1);
    for (u16 i = 0; i < chars; i++)
        dst[i] = (char)gvm_read_u8(wsrc + i * 2);
    dst[chars] = 0;
}

#define POOL_FLAG_NON_PAGED  0x0000000000000040ull

GVM_EXPORT(dump)
u64 dump(void)
{
    gvm_print("dump: enter");

    u32 sz = 64 * 1024;
    PVOID buf = 0;
    NTSTATUS s = 0;

    for (int tries = 0; tries < 8; tries++) {
        if (buf) ExFreePool(buf);
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, 'psLD');
        if (!buf) { gvm_print("dump: alloc failed"); return 0; }

        gvm_print("dump: alloc ok, calling ZwQSI");

        u32 got_off = 0;
        s = ZwQuerySystemInformation(SystemProcessInformation, buf, sz, (u32*)gvm_kva(&got_off));

        gvm_print("dump: ZwQSI returned");

        if (s == 0) break;
        if ((u32)s != 0xC0000004) break;
        sz *= 2;
    }

    if (!NT_SUCCESS(s)) {
        char hex[17];
        gvm_print("ZwQuerySystemInformation failed:");
        gvm_hex64((u64)(u32)s, hex); gvm_print(hex);
        if (buf) ExFreePool(buf);
        return 0;
    }

    u32 count = 0;
    u64 cur = (u64)buf;

    for (;;) {
        // read the header via host_read_bytes into a local, then parse
        spi_hdr h;
        u32 n = gvm_read_bytes(cur, (u32)(u64)&h, sizeof(h));
        if (n != sizeof(h)) break;

        char name[64] = { 0 };
        if (h.img_name_ptr && h.img_name_len)
            wide_to_ascii(h.img_name_ptr, h.img_name_len, name, sizeof(name));
        else
            gvm_strcpy(name, "<System Idle Process>");

        char line[160]; char t[32]; u32 lp = 0;
        lp += gvm_strcpy(line + lp, "pid=");
        gvm_dec((u32)h.unique_pid, t); lp += gvm_strcpy(line + lp, t);
        lp += gvm_strcpy(line + lp, " ppid=");
        gvm_dec((u32)h.inherited_ppid, t); lp += gvm_strcpy(line + lp, t);
        lp += gvm_strcpy(line + lp, " thr=");
        gvm_dec(h.thread_count, t); lp += gvm_strcpy(line + lp, t);
        lp += gvm_strcpy(line + lp, " ");
        lp += gvm_strcpy(line + lp, name);
        gvm_print(line);

        count++;
        if (h.next_off == 0) break;
        cur += h.next_off;
    }

    ExFreePool(buf);

    char sum[64]; char t[32]; u32 lp = 0;
    lp += gvm_strcpy(sum + lp, "total: ");
    gvm_dec(count, t); lp += gvm_strcpy(sum + lp, t);
    lp += gvm_strcpy(sum + lp, " processes");
    gvm_print(sum);
    return count;
}
