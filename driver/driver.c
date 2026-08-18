/* driver.c - entry, device create, dispatch */
#include "inc/gvm.h"
#include "../shared/goodmans_ioctl.h"

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD gvm_unload;
static DRIVER_DISPATCH gvm_create_close;
static DRIVER_DISPATCH gvm_device_control;

PDEVICE_OBJECT g_device;
static UNICODE_STRING g_symlink;

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT drv, _In_ PUNICODE_STRING regpath)
{
    UNREFERENCED_PARAMETER(regpath);

    // filter mask so DbgView captures our lines without Verbose
    DbgSetDebugFilterState(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,   TRUE);
    DbgSetDebugFilterState(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL, TRUE);
    DbgSetDebugFilterState(DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL,   TRUE);
    DbgSetDebugFilterState(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,    TRUE);

    gvm_log_init();
    gvm_trace_init();
    gvm_watchdog_start();
    gvm_log("driver load");

    UNICODE_STRING dev_name;
    RtlInitUnicodeString(&dev_name, GVM_DEVICE_NAME_U);

    NTSTATUS s = IoCreateDevice(drv, 0, &dev_name, GVM_DEVICE_TYPE,
                                FILE_DEVICE_SECURE_OPEN, FALSE, &g_device);
    if (!NT_SUCCESS(s)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans] IoCreateDevice=%08x\n", s);
        return s;
    }

    RtlInitUnicodeString(&g_symlink, GVM_SYMLINK_NAME_U);
    s = IoCreateSymbolicLink(&g_symlink, &dev_name);
    if (!NT_SUCCESS(s)) {
        IoDeleteDevice(g_device);
        g_device = NULL;
        return s;
    }

    s = gvm_modtab_init();
    if (!NT_SUCCESS(s)) {
        IoDeleteSymbolicLink(&g_symlink);
        IoDeleteDevice(g_device);
        g_device = NULL;
        return s;
    }

    drv->MajorFunction[IRP_MJ_CREATE]         = gvm_create_close;
    drv->MajorFunction[IRP_MJ_CLOSE]          = gvm_create_close;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = gvm_device_control;
    drv->DriverUnload = gvm_unload;

    g_device->Flags |= DO_BUFFERED_IO;
    g_device->Flags &= ~DO_DEVICE_INITIALIZING;

    gvm_log("driver ready, device \\\\.\\Goodmans");
    return STATUS_SUCCESS;
}

static VOID
gvm_unload(_In_ PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[goodmans] unload\n");

    gvm_ih_teardown();
    gvm_watchdog_stop();
    gvm_notify_teardown();
    gvm_modtab_teardown();

    if (g_symlink.Buffer)
        IoDeleteSymbolicLink(&g_symlink);
    if (g_device)
        IoDeleteDevice(g_device);
}

static NTSTATUS
gvm_create_close(_In_ PDEVICE_OBJECT dev, _Inout_ PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
gvm_device_control(_In_ PDEVICE_OBJECT dev, _Inout_ PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);

    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS s = STATUS_INVALID_DEVICE_REQUEST;

    switch (sp->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_GVM_LOAD_MODULE:   s = gvm_ioctl_load(irp, sp);       break;
    case IOCTL_GVM_CALL_EXPORT:   s = gvm_ioctl_call(irp, sp);       break;
    case IOCTL_GVM_UNLOAD_MODULE: s = gvm_ioctl_unload(irp, sp);     break;
    case IOCTL_GVM_LIST_MODULES:  s = gvm_ioctl_list(irp, sp);       break;
    case IOCTL_GVM_MODULE_INFO:   s = gvm_ioctl_info(irp, sp);       break;
    case IOCTL_GVM_UNLOAD_ALL:    s = gvm_ioctl_unload_all(irp, sp); break;
    case IOCTL_GVM_TAIL_LOG:      s = gvm_ioctl_tail_log(irp, sp);   break;
    case IOCTL_GVM_READ_GUEST:    s = gvm_ioctl_read_guest(irp, sp); break;
    case IOCTL_GVM_TAIL_TRACE:    s = gvm_ioctl_tail_trace(irp, sp); break;
    case IOCTL_GVM_TRACE_CTL:     s = gvm_ioctl_trace_ctl(irp, sp);  break;
    case IOCTL_GVM_FORCE_UNLOAD:  s = gvm_ioctl_force_unload(irp, sp); break;
    case IOCTL_GVM_NOTIFY_STOP:   s = gvm_ioctl_notify_stop(irp, sp);  break;
    default: break;
    }

    irp->IoStatus.Status = s;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return s;
}
