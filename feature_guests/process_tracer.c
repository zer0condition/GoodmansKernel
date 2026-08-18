/* process_tracer.c - live process create/exit + image load stream.
 *
 * TWO invocation models are demonstrated here:
 *
 * 1) polled: call start() once, then call poll() repeatedly. poll() drains
 *    the ring buffer and prints every event.
 *
 * 2) reactive: call start_reactive(). the driver's dispatch worker will
 *    invoke on_process_create() / on_process_exit() / on_image_load() below
 *    directly from the kernel notify callback. no polling.
 */
#include "gvm.h"

GVM_MANIFEST(GVM_CAP_ALLOC | GVM_CAP_CALLBACKS | GVM_CAP_INTROSPECT);

static void print_evt(const gvm_event* e)
{
    char line[160]; char t[32]; u32 n = 0;
    switch (e->kind) {
    case 0:
        n += gvm_strcpy(line + n, "PROC_CREATE pid=");
        gvm_dec(e->pid, t);  n += gvm_strcpy(line + n, t);
        n += gvm_strcpy(line + n, " ppid=");
        gvm_dec(e->ppid, t); n += gvm_strcpy(line + n, t);
        n += gvm_strcpy(line + n, " ");
        n += gvm_strcpy(line + n, e->name);
        break;
    case 1:
        n += gvm_strcpy(line + n, "PROC_EXIT   pid=");
        gvm_dec(e->pid, t); n += gvm_strcpy(line + n, t);
        break;
    case 2:
        n += gvm_strcpy(line + n, "IMAGE_LOAD  pid=");
        gvm_dec(e->pid, t); n += gvm_strcpy(line + n, t);
        n += gvm_strcpy(line + n, " base=0x");
        gvm_hex64(e->image_base, t); n += gvm_strcpy(line + n, t);
        n += gvm_strcpy(line + n, " ");
        n += gvm_strcpy(line + n, e->name);
        break;
    default:
        n += gvm_strcpy(line + n, "unknown event");
        break;
    }
    gvm_print(line);
}

GVM_EXPORT(start)
u64 start(void)
{
    u32 a = gvm_notify_enable(0);
    u32 b = gvm_notify_enable(1);
    gvm_print(a == 0 ? "process notify: on" : "process notify: FAILED");
    gvm_print(b == 0 ? "image notify: on"   : "image notify: FAILED");
    return (u64)a | ((u64)b << 32);
}

GVM_EXPORT(start_reactive)
u64 start_reactive(void)
{
    u32 a = gvm_notify_enable(0);
    u32 b = gvm_notify_enable(1);
    u32 d = gvm_dispatch_start();
    gvm_print(d == 0 ? "reactive dispatch: on" : "reactive dispatch: FAILED");
    return ((u64)a) | ((u64)b << 8) | ((u64)d << 16);
}

GVM_EXPORT(poll)
u64 poll(void)
{
    gvm_event e;
    u32 count = 0;
    while (gvm_notify_poll((u32)(u64)&e, sizeof(e)) == sizeof(e)) {
        print_evt(&e);
        count++;
    }
    return count;
}

// reactive callbacks: driver-side dispatch worker invokes these directly.
// signature is fixed: (pid, aux1, aux2) where aux1 is ppid or image_base,
// aux2 is 0 or image_size depending on event.

GVM_EXPORT(on_process_create)
u64 on_process_create(u64 pid, u64 ppid, u64 _unused)
{
    (void)_unused;
    char line[80]; char t[32]; u32 n = 0;
    n += gvm_strcpy(line + n, "[reactive] PROC_CREATE pid=");
    gvm_dec((u32)pid, t);  n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " ppid=");
    gvm_dec((u32)ppid, t); n += gvm_strcpy(line + n, t);
    gvm_print(line);
    return 0;
}

GVM_EXPORT(on_process_exit)
u64 on_process_exit(u64 pid, u64 _a1, u64 _a2)
{
    (void)_a1; (void)_a2;
    char line[80]; char t[32]; u32 n = 0;
    n += gvm_strcpy(line + n, "[reactive] PROC_EXIT pid=");
    gvm_dec((u32)pid, t); n += gvm_strcpy(line + n, t);
    gvm_print(line);
    return 0;
}

GVM_EXPORT(on_image_load)
u64 on_image_load(u64 pid, u64 base, u64 size)
{
    char line[128]; char t[32]; u32 n = 0;
    n += gvm_strcpy(line + n, "[reactive] IMAGE_LOAD pid=");
    gvm_dec((u32)pid, t); n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " base=0x");
    gvm_hex64(base, t); n += gvm_strcpy(line + n, t);
    n += gvm_strcpy(line + n, " size=");
    gvm_dec((u32)size, t); n += gvm_strcpy(line + n, t);
    gvm_print(line);
    return 0;
}
