#pragma once
#include <QMainWindow>
#include <QListWidget>
#include <QStackedWidget>
#include <QTabWidget>
#include <QLabel>
#include <QTimer>
#include <QMenu>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSettings>
#include "driver.h"
#include "pages.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
private slots:
    void tick_fast();
    void tick_slow();
    void on_module_selected(quint32 id);
    void on_module_loaded(quint32 id);
    void on_state_changed();
    void on_nav_changed(int row);
    void show_toast(const QString& msg, int kind);
private:
    DriverClient*    drv;
    QTabWidget*      tabs;
    WorkbenchPage*   workbench;
    MemoryPage*      memory;
    EventsPage*      events;
    LogPage*         logp;
    TracePage*       tracep;
    ExplorerPage*    explorerp;
    ProfilerPage*    profilerp;
    DeployPage*      deploy;
    QTimer           fast_timer;
    QTimer           slow_timer;

    // status
    QLabel*  statDevice;
    QLabel*  statSvc;
    QLabel*  statCount;

    // log state
    quint64  log_seq   = 0;
    quint64  trace_seq = 0;

    void set_stat(QLabel* lbl, const QString& txt, const QString& color, const QString& dot_color);

    // shortcuts + palette
    void open_command_palette();
    void open_about();
    void load_wasm_from_dialog();
    void reload_current_selection();
    void go_to_page(int i);

    // hot reload
    QFileSystemWatcher* hot_watcher = nullptr;
    QHash<QString, quint32> path_to_module;
    void on_file_changed(const QString& path);

    // recent files
    QMenu* recentMenu = nullptr;
    QStringList recent_files;
    void add_recent(const QString& path);
    void rebuild_recent_menu();

    // session persistence
    void save_settings();
    void load_settings();

protected:
    void closeEvent(QCloseEvent* e) override;
};
