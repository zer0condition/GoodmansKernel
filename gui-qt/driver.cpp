#include "driver.h"
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

#include "goodmans_ioctl.h"

DriverClient::DriverClient(QObject* p) : QObject(p) {}

static HANDLE dev_open()
{
    return CreateFileA(GVM_USER_PATH, GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool DriverClient::device_open() const
{
    HANDLE h = dev_open();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool DriverClient::refresh_modules(QVector<ModuleRow>& out)
{
    out.clear();
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    gvm_list_out r{};
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_LIST_MODULES, nullptr, 0, &r, sizeof(r), &ret, nullptr);
    CloseHandle(dev);
    if (!ok) return false;
    for (unsigned int i = 0; i < r.count; i++) {
        ModuleRow m;
        m.id         = r.entries[i].id;
        m.wasm_size  = r.entries[i].wasm_size;
        m.mem_pages  = r.entries[i].mem_pages;
        m.pool_bytes = r.entries[i].pool_bytes;
        m.hash       = r.entries[i].hash;
        m.name       = QString::fromUtf8(r.entries[i].name);
        out.push_back(m);
    }
    return true;
}

bool DriverClient::module_info(quint32 id, ModuleInfo& info)
{
    info = {};
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    gvm_info_in in{ id };
    auto* out = (gvm_info_out*)calloc(1, sizeof(gvm_info_out));
    if (!out) { CloseHandle(dev); return false; }
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_MODULE_INFO, &in, sizeof(in), out, sizeof(*out), &ret, nullptr);
    CloseHandle(dev);
    if (!ok || out->status != 0) { free(out); return false; }
    info.base.id         = out->base.id;
    info.base.name       = QString::fromUtf8(out->base.name);
    info.base.wasm_size  = out->base.wasm_size;
    info.base.mem_pages  = out->base.mem_pages;
    info.base.pool_bytes = out->base.pool_bytes;
    info.base.hash       = out->base.hash;
    info.caps = 0;
    for (unsigned int i = 0; i < out->export_count && i < GVM_MAX_INFO_EXPORTS; i++)
        info.exports.push_back(QString::fromUtf8(out->exports[i]));
    for (unsigned int i = 0; i < out->import_count && i < GVM_MAX_INFO_IMPORTS; i++)
        info.imports.push_back(QString::fromUtf8(out->imports[i]));
    info.valid = true;
    free(out);
    return true;
}

bool DriverClient::load_module(const QString& path, quint64 budget, quint32& out_id, QString& err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { err = "cannot open file"; return false; }
    QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty() || data.size() > (16 << 20)) { err = "bad file size"; return false; }

    QByteArray buf; buf.resize(sizeof(gvm_load_in) + data.size());
    auto* in = (gvm_load_in*)buf.data();
    memset(in, 0, sizeof(*in));
    in->wasm_size = (unsigned int)data.size();
    in->pool_budget = budget;
    QByteArray nameU8 = QFileInfo(path).fileName().toUtf8();
    strncpy_s(in->name, GVM_MAX_MODULE_NAME, nameU8.constData(), _TRUNCATE);
    memcpy(buf.data() + sizeof(gvm_load_in), data.constData(), data.size());

    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) { err = "device closed"; return false; }
    gvm_load_out out{}; DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_LOAD_MODULE, buf.data(), (DWORD)buf.size(), &out, sizeof(out), &ret, nullptr);
    CloseHandle(dev);
    if (!ok)          { err = "ioctl failed"; return false; }
    if (out.status)   { err = QString::fromUtf8(out.err_msg); return false; }
    out_id = out.module_id;
    return true;
}

bool DriverClient::call_export(quint32 id, const QString& exp, quint32 timeout, const QVector<quint64>& argv, quint64& rv, QString& err)
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) { err = "device closed"; return false; }
    gvm_call_in in{}; in.module_id = id; in.timeout_ms = timeout;
    in.argc = (unsigned int)argv.size(); if (in.argc > GVM_MAX_ARGS) in.argc = GVM_MAX_ARGS;
    for (unsigned int i = 0; i < in.argc; i++) in.argv[i] = argv[i];
    QByteArray e = exp.toUtf8();
    strncpy_s(in.export_name, e.constData(), _TRUNCATE);
    gvm_call_out out{}; DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_CALL_EXPORT, &in, sizeof(in), &out, sizeof(out), &ret, nullptr);
    CloseHandle(dev);
    if (!ok)         { err = "ioctl failed"; return false; }
    if (out.status)  { err = QString::fromUtf8(out.err_msg); return false; }
    rv = out.rv;
    return true;
}

bool DriverClient::unload(quint32 id)
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    gvm_unload_in in{ id }; DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_UNLOAD_MODULE, &in, sizeof(in), nullptr, 0, &ret, nullptr);
    CloseHandle(dev);
    return ok;
}

bool DriverClient::unload_all()
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_UNLOAD_ALL, nullptr, 0, nullptr, 0, &ret, nullptr);
    CloseHandle(dev);
    return ok;
}

static int classify_line(const QString& line)
{
    if (line.contains("load module_id"))                                     return EVT_LOAD;
    if (line.contains("unload module"))                                      return EVT_UNLOAD;
    if (line.contains("call mod="))                                          return EVT_CALL;
    if (line.contains("PROC_CREATE") || line.contains("proc create"))        return EVT_PROC;
    if (line.contains("PROC_EXIT")   || line.contains("proc exit"))          return EVT_EXIT;
    if (line.contains("IMAGE_LOAD")  || line.contains("image load"))         return EVT_IMAGE;
    if (line.contains("error", Qt::CaseInsensitive) || line.contains("ERR")) return EVT_ERROR;
    return EVT_MSG;
}

bool DriverClient::tail_log(quint64& last_seq, QVector<LogEntry>& out_entries, quint32& dropped)
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    gvm_tail_in in{ last_seq };
    auto* out = (gvm_tail_out*)malloc(sizeof(gvm_tail_out));
    if (!out) { CloseHandle(dev); return false; }
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_TAIL_LOG, &in, sizeof(in), out, sizeof(*out), &ret, nullptr);
    CloseHandle(dev);
    if (!ok) { free(out); return false; }
    last_seq = out->next_seq;
    dropped  = out->dropped;
    for (unsigned int i = 0; i < out->count; i++) {
        SYSTEMTIME sys; FILETIME ft, lft;
        ft.dwLowDateTime  = (DWORD) out->entries[i].timestamp_100ns;
        ft.dwHighDateTime = (DWORD)(out->entries[i].timestamp_100ns >> 32);
        FileTimeToLocalFileTime(&ft, &lft);
        FileTimeToSystemTime(&lft, &sys);
        LogEntry e;
        e.ts   = QString::asprintf("%02u:%02u:%02u.%03u", sys.wHour, sys.wMinute, sys.wSecond, sys.wMilliseconds);
        e.line = QString::fromUtf8(out->entries[i].line);
        e.kind = classify_line(e.line);
        out_entries.push_back(e);
    }
    free(out);
    return true;
}

bool DriverClient::service_present() const
{
    bool present = false;
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, "Goodmans", SERVICE_QUERY_STATUS);
    if (svc) { present = true; CloseServiceHandle(svc); }
    CloseServiceHandle(scm);
    return present;
}

bool DriverClient::service_running() const
{
    bool running = false;
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, "Goodmans", SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS_PROCESS ss{}; DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ss, sizeof(ss), &needed))
            running = (ss.dwCurrentState == SERVICE_RUNNING);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

void DriverClient::run_sc(const QString& args)
{
    QByteArray a = args.toLocal8Bit();
    SHELLEXECUTEINFOA si{ sizeof(si) };
    si.lpVerb = "runas"; si.lpFile = "sc.exe"; si.lpParameters = a.constData();
    si.nShow = SW_HIDE; si.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (ShellExecuteExA(&si) && si.hProcess) {
        WaitForSingleObject(si.hProcess, 8000);
        CloseHandle(si.hProcess);
    }
}

bool DriverClient::read_guest(quint32 module_id, quint32 offset, quint32 len, QByteArray& out, QString& err)
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) { err = "device closed"; return false; }
    gvm_read_guest_in in{ module_id, offset, len };
    auto* r = (gvm_read_guest_out*)calloc(1, sizeof(gvm_read_guest_out));
    if (!r) { CloseHandle(dev); err = "oom"; return false; }
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_READ_GUEST, &in, sizeof(in), r, sizeof(*r), &ret, nullptr);
    CloseHandle(dev);
    if (!ok)        { err = "ioctl failed"; free(r); return false; }
    if (r->status)  { err = QString::fromUtf8(r->err_msg); free(r); return false; }
    out = QByteArray((const char*)r->data, (int)r->length);
    free(r);
    return true;
}

bool DriverClient::tail_trace(quint64& last_seq, QVector<TraceEvent>& out, quint32& dropped)
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    gvm_trace_in in{ last_seq };
    auto* r = (gvm_trace_out*)malloc(sizeof(gvm_trace_out));
    if (!r) { CloseHandle(dev); return false; }
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_TAIL_TRACE, &in, sizeof(in), r, sizeof(*r), &ret, nullptr);
    CloseHandle(dev);
    if (!ok) { free(r); return false; }
    last_seq = r->next_seq;
    dropped  = r->dropped;
    for (unsigned int i = 0; i < r->count; i++) {
        SYSTEMTIME sys; FILETIME ft, lft;
        ft.dwLowDateTime  = (DWORD) r->entries[i].timestamp_100ns;
        ft.dwHighDateTime = (DWORD)(r->entries[i].timestamp_100ns >> 32);
        FileTimeToLocalFileTime(&ft, &lft);
        FileTimeToSystemTime(&lft, &sys);
        TraceEvent e;
        e.timestamp_100ns = r->entries[i].timestamp_100ns;
        e.ts        = QString::asprintf("%02u:%02u:%02u.%03u", sys.wHour, sys.wMinute, sys.wSecond, sys.wMilliseconds);
        e.module_id = r->entries[i].module_id;
        e.kind      = r->entries[i].kind;
        e.thread_id = r->entries[i].thread_id;
        e.irql      = r->entries[i].irql;
        e.name      = QString::fromUtf8(r->entries[i].name);
        for (unsigned int a = 0; a < r->entries[i].argc && a < 4; a++)
            e.argv.push_back(r->entries[i].argv[a]);
        e.rv        = r->entries[i].rv;
        out.push_back(e);
    }
    free(r);
    return true;
}

bool DriverClient::trace_ctl(int mode, quint32 module_id)
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    gvm_trace_ctl_in in{ (unsigned int)mode, module_id };
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_TRACE_CTL, &in, sizeof(in), nullptr, 0, &ret, nullptr);
    CloseHandle(dev);
    return ok;
}

bool DriverClient::notify_stop()
{
    HANDLE dev = dev_open();
    if (dev == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_GVM_NOTIFY_STOP, nullptr, 0, nullptr, 0, &ret, nullptr);
    CloseHandle(dev);
    return ok;
}
