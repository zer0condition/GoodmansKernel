#pragma once
#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QSet>
#include "driver.h"

class WorkbenchPage : public QWidget {
    Q_OBJECT
public:
    WorkbenchPage(DriverClient* d, QWidget* p = nullptr);
    void refreshModules(const QVector<ModuleRow>& mods);
    void setSelected(quint32 id, const ModuleInfo& info);
    void clearSelection();
signals:
    void moduleLoaded(quint32 id);
    void moduleSelected(quint32 id);
    void toast(const QString& msg, int kind);
private:
    DriverClient*  drv;
    QLineEdit*     loadPath;
    QSpinBox*      loadBudget;
    QListWidget*   moduleList;
    QLabel*        moduleCount;
    QLabel*        modTitle;
    QLabel*        modMeta;
    QWidget*       capsBar;
    QTableWidget*  expTable;
    QTableWidget*  impTable;
    QLineEdit*     callExport;
    QLineEdit*     callArgs;
    QSpinBox*      callTimeout;
    QLabel*        callResult;
    QTableWidget*  callHist;
    ModuleInfo     current;
    QWidget*       infoPanel;
    QWidget*       emptyPanel;
    quint32        pending_select = 0;
    void refreshCaps(quint32 caps);
};

class MemoryPage : public QWidget {
    Q_OBJECT
public:
    MemoryPage(DriverClient* d, QWidget* p = nullptr);
signals:
    void toast(const QString& msg, int kind);
private:
    DriverClient*   drv;
    QSpinBox*       modId;
    QLineEdit*      exp;
    QLineEdit*      addr;
    QSpinBox*       length;
    QLabel*         status;
    QPlainTextEdit* hex;
};

class EventsPage : public QWidget {
    Q_OBJECT
public:
    EventsPage(DriverClient* d, QWidget* p = nullptr);
    void append(const QVector<LogEntry>& entries);
    void clear();
signals:
    void toast(const QString& msg, int kind);
private slots:
    void on_load_tracer();
    void on_start(bool reactive);
    void on_stop();
private:
    DriverClient*  drv;
    QTableWidget*  table;
    QLineEdit*     filter;
    QCheckBox*     cbProc;
    QCheckBox*     cbExit;
    QCheckBox*     cbImage;
    QCheckBox*     cbCall;
    QCheckBox*     cbError;
    QPushButton*   loadBtn;
    QPushButton*   startBtn;
    QPushButton*   reactiveBtn;
    QPushButton*   stopBtn;
    QLabel*        statusLbl;
    int            tracer_id = -1;
    bool           running   = false;
    void set_status(const QString& s, const QString& color);
};

class LogPage : public QWidget {
    Q_OBJECT
public:
    LogPage(QWidget* p = nullptr);
    void append(const QVector<LogEntry>& entries, quint64 seq, quint32 dropped);
    void clear();
private:
    QPlainTextEdit* view;
    QLabel*         statusLbl;
    QCheckBox*      autoscroll;
};

class TracePage : public QWidget {
    Q_OBJECT
public:
    TracePage(DriverClient* d, QWidget* p = nullptr);
    void append(const QVector<TraceEvent>& events);
    void clear();
    void focusFilter() { filter->setFocus(); filter->selectAll(); }
    void saveTraceFile(const QString& path);
    void loadTraceFile(const QString& path);
    void toggleTracing();
    void toggleBookmark();
signals:
    void toast(const QString& msg, int kind);
private:
    DriverClient*  drv;
    QTableWidget*  table;
    QPlainTextEdit* details;
    QPushButton*   toggleBtn;
    QPushButton*   pauseBtn;
    QPushButton*   clearBtn;
    QPushButton*   exportBtn;
    QPushButton*   openBtn;
    QSpinBox*      moduleFilter;
    QCheckBox*     onlyModule;
    QCheckBox*     regexMode;
    QLineEdit*     filter;
    QLabel*        modeLbl;
    QLabel*        statsLbl;
    bool           tracing_on = false;
    bool           paused     = false;
    quint64        total_events = 0;
    quint64        shown_events = 0;
    quint64        first_ts     = 0;
    QHash<QString, quint64> kind_counts;
    QVector<TraceEvent>     backing;
    QSet<int>               bookmarks;   // indices into backing
    void render_details(const TraceEvent& e);
    void refresh_stats();
    void rebuild_from_backing();
    bool filter_match(const TraceEvent& e) const;
    void insert_row(int row, const TraceEvent& e, int back_idx);
};

class ExplorerPage : public QWidget {
    Q_OBJECT
public:
    ExplorerPage(DriverClient* d, QWidget* p = nullptr);
    void setToolkitId(int id);
signals:
    void toast(const QString& msg, int kind);
private slots:
    void refresh_procs();
    void read_memory();
    void read_msrs();
    void read_cpuid();
private:
    DriverClient* drv;
    int           toolkit_id = -1;
    QLabel*       toolkitLbl;
    QPushButton*  loadToolkitBtn;
    // procs
    QTableWidget* procTable;
    // memory
    QLineEdit*    memAddr;
    QSpinBox*     memLen;
    QPlainTextEdit* memHex;
    // msr
    QTableWidget* msrTable;
    QLineEdit*    msrCustom;
    // cpuid
    QTableWidget* cpuidTable;
    QSpinBox*     cpuidMax;
};

class ProfilerPage : public QWidget {
    Q_OBJECT
public:
    ProfilerPage(QWidget* p = nullptr);
    void observe(const QVector<TraceEvent>& events);
    void clear();
private:
    struct Stat { quint64 count = 0; quint64 total_ns = 0; quint64 max_ns = 0; quint64 last_call_ts = 0; };
    QHash<QString, Stat> stats;
    QTableWidget* table;
    QLabel*       total;
    void refresh();
};

class DeployPage : public QWidget {
    Q_OBJECT
public:
    DeployPage(DriverClient* d, QWidget* p = nullptr);
    void refresh(bool devUp, bool present, bool running);
signals:
    void toast(const QString& msg, int kind);
    void stateChanged();
private:
    DriverClient* drv;
    QLabel*       svcState;
    QLabel*       runState;
    QLabel*       devState;
};
