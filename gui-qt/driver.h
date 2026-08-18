#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <QByteArray>
#include <cstdint>

struct ModuleRow {
    quint32   id{};
    QString   name;
    quint32   wasm_size{};
    quint32   mem_pages{};
    quint64   pool_bytes{};
    quint64   hash{};
};

struct ModuleInfo {
    ModuleRow          base;
    quint32            caps{0};
    QVector<QString>   exports;
    QVector<QString>   imports;
    bool               valid{false};
};

struct LogEntry {
    QString ts;
    QString line;
    int     kind{0};
};

enum LogKind { EVT_MSG, EVT_LOAD, EVT_CALL, EVT_UNLOAD, EVT_PROC, EVT_EXIT, EVT_IMAGE, EVT_ERROR };

// trace kinds mirror driver-side GVM_TRK_*
enum TraceKind { TRK_IMPORT = 1, TRK_CALL = 2, TRK_RETURN = 3, TRK_TRAP = 4 };

struct TraceEvent {
    quint64  timestamp_100ns;  // raw for delta-time math
    QString  ts;               // human-readable
    quint32  module_id;
    quint32  kind;
    quint32  thread_id;
    quint32  irql;
    QString  name;
    QVector<quint64> argv;
    quint64  rv;
};

enum TraceMode { TRACE_OFF = 0, TRACE_ON_ALL = 1, TRACE_ON_MODULE = 2 };

class DriverClient : public QObject {
    Q_OBJECT
public:
    explicit DriverClient(QObject* parent = nullptr);

    bool device_open() const;
    bool refresh_modules(QVector<ModuleRow>& out);
    bool module_info(quint32 id, ModuleInfo& out);
    bool load_module(const QString& path, quint64 budget, quint32& out_id, QString& err);
    bool call_export(quint32 id, const QString& exp, quint32 timeout_ms, const QVector<quint64>& argv, quint64& rv, QString& err);
    bool unload(quint32 id);
    bool unload_all();
    bool tail_log(quint64& last_seq, QVector<LogEntry>& out, quint32& dropped);

    bool service_present() const;
    bool service_running() const;
    void run_sc(const QString& args);

    bool read_guest(quint32 module_id, quint32 offset, quint32 len, QByteArray& out, QString& err);
    bool tail_trace(quint64& last_seq, QVector<TraceEvent>& out, quint32& dropped);
    bool trace_ctl(int mode, quint32 module_id = 0);
    bool notify_stop();
};
